// Bill of materials (TODO 11 UI-83): parts and assemblies with quantities, part properties and masses, and its CSV.
#include "opad/drawing/bom.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_XYZ.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

#include "opad/geometry.hpp"
#include "opad/materials.hpp"

namespace opad::drawing {
namespace {

// Properties with a column of their own; any other is a custom column.
bool own_field(const std::string& k) {
  static const std::set<std::string> f = {"part_number", "description", "material", "density", "mass", "vendor", "notes", "bom"};
  return f.count(k) > 0;
}

std::string text_of(const json& props, const char* key) {
  const auto it = props.find(key);
  return it != props.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// An instance number at the end of a name: in brackets ("Bolt[2]", "Bolt<2>", "Bolt (2)") or after a separator ("Bolt 2",
// "Bolt-2", "Bolt:2"). Digits glued to a word ("M5", "Bolt12") are part of the name.
std::string strip_number(const std::string& name) {
  const auto digits_from = [&](size_t end) {
    size_t s = end;
    while (s > 0 && std::isdigit(static_cast<unsigned char>(name[s - 1]))) --s;
    return s;
  };
  size_t cut = name.size();
  const char close = name.empty() ? 0 : name.back();
  if (const char open = close == ']' ? '[' : close == '>' ? '<' : close == ')' ? '(' : 0) {
    const size_t s = digits_from(name.size() - 1);
    if (s + 1 == name.size() || s == 0 || name[s - 1] != open) return name;
    cut = s - 1;
  } else {
    const size_t s = digits_from(name.size());
    if (s == name.size() || s == 0 || std::string(" _:#-").find(name[s - 1]) == std::string::npos) return name;
    cut = s;
  }
  while (cut > 0 && std::string(" _:#-").find(name[cut - 1]) != std::string::npos) --cut;
  return cut ? name.substr(0, cut) : name;
}

// CAD exports number the occurrences of a part: "Bracket:1", "V8-XT-01:2", "Bracket<1>" (before the "[2]" OPAD gives the
// solids of one part). A number standing alone before the ':' makes a ratio, the name's own ("Gear 3:1").
std::string without_occurrence(const std::string& name) {
  const auto digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
  const auto digits = [&](const std::string& s, size_t from, size_t to) { return from < to && std::all_of(s.begin() + long(from), s.begin() + long(to), digit); };
  size_t end = name.size();
  if (end && name.back() == ']')
    if (const size_t open = name.rfind('['); open != std::string::npos && open > 0 && digits(name, open + 1, end - 1)) end = open;
  std::string head = name.substr(0, end);
  const std::string tail = name.substr(end);
  if (!head.empty() && head.back() == '>') {
    if (const size_t open = head.rfind('<'); open != std::string::npos && open > 0 && digits(head, open + 1, head.size() - 1)) {
      head.erase(open);
      while (!head.empty() && head.back() == ' ') head.pop_back();
    }
  } else if (const size_t colon = head.rfind(':'); colon != std::string::npos && colon > 0 && digits(head, colon + 1, head.size()) && head[colon - 1] != ' ') {
    size_t run = colon;
    while (run > 0 && digit(head[run - 1])) --run;
    if (run == colon || (run > 0 && head[run - 1] != ' ')) head.erase(colon);
  }
  return head.empty() ? name : head + tail;
}

// The name a group of nodes shares once instance numbers are taken off, else the first one's.
std::string common_name(std::vector<std::string> names) {
  const std::string first = names.front();
  for (int round = 0; round < 4; ++round) {
    if (std::all_of(names.begin(), names.end(), [&](const std::string& n) { return n == names.front(); })) return names.front();
    bool changed = false;
    for (auto& n : names) {
      const std::string s = strip_number(n);
      changed = changed || s != n;
      n = s;
    }
    if (!changed) break;
  }
  return first;
}

// ---------------------------------------------------------------- same solid, moved and turned
// What pins a shape down: its vertices and the middle of each edge by parameter (copies keep their parameters).
std::vector<gp_XYZ> feature_points(const TopoDS_Shape& s) {
  std::vector<gp_XYZ> out;
  TopTools_IndexedMapOfShape vertices, edges;
  TopExp::MapShapes(s, TopAbs_VERTEX, vertices);
  TopExp::MapShapes(s, TopAbs_EDGE, edges);
  for (int i = 1; i <= vertices.Extent(); ++i) out.push_back(BRep_Tool::Pnt(TopoDS::Vertex(vertices(i))).XYZ());
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge& e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e) || !BRep_Tool::IsGeometric(e)) continue;
    const BRepAdaptor_Curve c(e);
    out.push_back(c.Value((c.FirstParameter() + c.LastParameter()) / 2).XYZ());
  }
  return out;
}

// Whether a rotation (never a mirror) and a shift put every point of a onto one of b. The frame comes from the point
// farthest from the centre and the one farthest off that line; b's candidates for those two are its points at the same
// distances, a few dozen tried at most (a symmetric shape has many). Points all on one line give no frame: not matched.
bool congruent(const std::vector<gp_XYZ>& a, const std::vector<gp_XYZ>& b) {
  const size_t n = a.size();
  if (n != b.size() || n < 3) return false;
  gp_XYZ ca(0, 0, 0), cb(0, 0, 0);
  for (size_t i = 0; i < n; ++i) ca += a[i], cb += b[i];
  ca /= double(n);
  cb /= double(n);
  size_t i1 = 0, i2 = 0;
  double reach = 0, spread = 0;
  for (size_t i = 0; i < n; ++i)
    if (const double d = (a[i] - ca).Modulus(); d > reach) reach = d, i1 = i;
  const gp_XYZ u1 = a[i1] - ca;
  for (size_t i = 0; i < n; ++i)
    if (const double c = u1.Crossed(a[i] - ca).Modulus(); c > spread) spread = c, i2 = i;
  const double tol = 1e-6 * std::max(1.0, reach);
  if (spread < tol * std::max(1.0, reach)) return false;
  const gp_XYZ u2 = a[i2] - ca;
  const double d1 = u1.Modulus(), d2 = u2.Modulus(), d12 = (a[i2] - a[i1]).Modulus();
  const auto frame = [](const gp_XYZ& x, const gp_XYZ& y, gp_XYZ f[3]) {
    f[0] = x / x.Modulus();
    f[2] = x.Crossed(y);
    f[2] /= f[2].Modulus();
    f[1] = f[2].Crossed(f[0]);
  };
  gp_XYZ fa[3];
  frame(u1, u2, fa);
  std::vector<gp_XYZ> sorted = b;
  std::sort(sorted.begin(), sorted.end(), [](const gp_XYZ& p, const gp_XYZ& q) { return p.X() < q.X(); });
  const auto found = [&](const gp_XYZ& q) {
    auto it = std::lower_bound(sorted.begin(), sorted.end(), q.X() - 2 * tol, [](const gp_XYZ& p, double x) { return p.X() < x; });
    for (; it != sorted.end() && it->X() <= q.X() + 2 * tol; ++it)
      if ((*it - q).SquareModulus() <= 4 * tol * tol) return true;
    return false;
  };
  std::vector<size_t> first, second;
  for (size_t i = 0; i < n; ++i) {
    const double d = (b[i] - cb).Modulus();
    if (std::fabs(d - d1) <= tol) first.push_back(i);
    if (std::fabs(d - d2) <= tol) second.push_back(i);
  }
  int tries = 0;
  for (size_t j1 : first)
    for (size_t j2 : second) {
      if (std::fabs((b[j2] - b[j1]).Modulus() - d12) > tol) continue;
      if (++tries > 48) return false;
      gp_XYZ fb[3];
      frame(b[j1] - cb, b[j2] - cb, fb);
      bool all = true;
      for (size_t i = 0; all && i < n; ++i) {
        const gp_XYZ r = a[i] - ca;
        all = found(cb + fb[0] * r.Dot(fa[0]) + fb[1] * r.Dot(fa[1]) + fb[2] * r.Dot(fa[2]));
      }
      if (all) return true;
    }
  return false;
}

// Body key -> the first key (in `keys`' order) of the same solid moved and turned. Only shapes with the same numbers of
// faces, edges and vertices are compared, and a match must enclose the same volume.
std::unordered_map<std::string, std::string> shape_classes(const Document& doc, const std::vector<std::string>& keys, const std::function<bool()>& cancelled) {
  std::unordered_map<std::string, std::string> cls;
  // Counted side by side: a document read from disk parses each shape here first (the Engine's 250 took 5 s in a row).
  std::vector<std::array<int, 3>> counts(keys.size(), {-1, -1, -1});
  OSD_Parallel::For(0, static_cast<int>(keys.size()), [&](int i) {
    if (!doc.has_body(keys[size_t(i)]) || (cancelled && cancelled())) return;
    try {
      const TopoDS_Shape s = body_shape(doc, keys[size_t(i)]);
      TopTools_IndexedMapOfShape f, e, v;
      TopExp::MapShapes(s, TopAbs_FACE, f);
      TopExp::MapShapes(s, TopAbs_EDGE, e);
      TopExp::MapShapes(s, TopAbs_VERTEX, v);
      counts[size_t(i)] = {f.Extent(), e.Extent(), v.Extent()};
    } catch (const std::exception&) {
    } catch (const Standard_Failure&) {
    }
  });
  if (cancelled && cancelled()) throw Error("cancelled");
  std::map<std::array<int, 3>, std::vector<std::string>> buckets;
  for (size_t i = 0; i < keys.size(); ++i) {
    cls[keys[i]] = keys[i];
    if (counts[i][0] >= 0) buckets[counts[i]].push_back(keys[i]);
  }
  for (const auto& [counts, list] : buckets) {
    if (list.size() < 2) continue;
    std::vector<std::optional<std::vector<gp_XYZ>>> points(list.size());
    const auto points_of = [&](size_t i) -> const std::vector<gp_XYZ>& {
      if (!points[i]) points[i] = feature_points(body_shape(doc, list[i]));
      return *points[i];
    };
    for (size_t i = 0; i < list.size(); ++i) {
      if (cls[list[i]] != list[i]) continue;
      for (size_t j = i + 1; j < list.size(); ++j) {
        if (cancelled && cancelled()) throw Error("cancelled");
        if (cls[list[j]] != list[j] || !congruent(points_of(i), points_of(j))) continue;
        try {
          const double vi = body_volume(doc, list[i]), vj = body_volume(doc, list[j]);
          if (std::fabs(vi - vj) <= 1e-7 * std::max({1.0, vi, vj})) cls[list[j]] = list[i];
        } catch (const std::exception&) {
        }
      }
    }
  }
  return cls;
}

std::string number(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

std::string short_hash(const std::string& s) { return sha256_hex(s).substr(0, 16); }

// One node the BoM keeps.
struct Item {
  std::string id;
  const Node* node = nullptr;
  bool part = false;          // a body, or a component bought as one
  std::vector<size_t> kids;   // an assembly's items
  std::string identity;
  MaterialChoice material;    // parts
  std::optional<double> mass;  // grams
  std::string mass_error;
};

}  // namespace

json bom(const Document& doc, const Scene& scene, const BomOptions& o) {
  if (o.mode != "top" && o.mode != "parts" && o.mode != "indented") throw Error("bom: mode is top, parts or indented, not '" + o.mode + "'");
  const double unit = mass_unit(o.mass_unit);
  const auto cancelled = [&] {
    if (o.cancelled && o.cancelled()) throw Error("cancelled");
  };
  const auto flag = [](const Node& n) { return text_of(n.properties, "bom"); };

  // What it lists: the root's children when it is an assembly (a document with one root component is that assembly).
  json assembly = nullptr;
  std::vector<std::string> top;
  const auto as_assembly = [&](const Node& n) {
    if (n.kind != Node::Kind::Component || flag(n) == "purchased" || flag(n) == "exclude") return false;
    assembly = {{"id", n.id}, {"name", n.name}};
    top = n.children;
    return true;
  };
  if (!o.root.empty()) {
    const Node* r = scene.node(o.root);
    if (!r) throw Error("bom: node " + o.root + " does not exist");
    if (!as_assembly(*r)) top = {o.root};
  } else if (scene.roots.size() != 1 || !as_assembly(*scene.node(scene.roots[0]))) {
    top = scene.roots;
  }

  std::vector<Item> items;
  std::function<int(const std::string&)> take = [&](const std::string& id) -> int {
    const Node* n = scene.node(id);
    if (!n || flag(*n) == "exclude") return -1;
    Item it;
    it.id = id;
    it.node = n;
    if (n->kind == Node::Kind::Body) {
      if (n->representation != "solid" && !o.references) return -1;
      it.part = true;
    } else if (flag(*n) == "purchased") {
      it.part = true;
    } else {
      for (const auto& c : n->children)
        if (const int k = take(c); k >= 0) it.kids.push_back(static_cast<size_t>(k));
      if (it.kids.empty()) return -1;
    }
    if (it.part) it.material = material_of(doc, scene, id);
    items.push_back(std::move(it));
    return static_cast<int>(items.size() - 1);
  };
  std::vector<size_t> roots;
  for (const auto& id : top)
    if (const int k = take(id); k >= 0) roots.push_back(static_cast<size_t>(k));
  cancelled();

  // The parts in tree order.
  std::vector<size_t> parts;
  std::function<void(size_t)> collect = [&](size_t i) {
    if (items[i].part) parts.push_back(i);
    for (size_t k : items[i].kids) collect(k);
  };
  for (size_t r : roots) collect(r);
  // A purchased component's mass is what is in it.
  const auto inside = [&](const Item& it) {
    std::vector<std::string> bodies;
    if (it.node->kind == Node::Kind::Body) bodies.push_back(it.id);
    else
      for (const auto& b : scene.bodies_under(it.id))
        if (scene.node(b)->representation == "solid") bodies.push_back(b);
    return bodies;
  };

  // Masses: the volumes of every key that needs one, side by side, then each part's.
  if (o.mass) {
    std::vector<std::string> keys;
    for (size_t i : parts)
      for (const auto& b : inside(items[i]))
        if (!scene.node(b)->properties.contains("mass") && material_of(doc, scene, b).density > 0) keys.push_back(scene.node(b)->body_key);
    warm_volumes(doc, keys, o.cancelled);
    for (size_t i : parts) {
      Item& it = items[i];
      if (const double given = property_number(it.node->properties.value("mass", json())); given > 0) {
        it.mass = given;
        continue;
      }
      double sum = 0;
      int counted = 0;
      std::string why;
      for (const auto& b : inside(it)) {
        std::string reason;
        if (const auto m = body_mass(doc, scene, b, &reason)) sum += *m, ++counted;
        else if (reason != "not a solid" || it.node->kind == Node::Kind::Body) why = why.empty() ? reason : why;
      }
      if (why.empty() && counted) it.mass = sum;
      else it.mass_error = why.empty() ? "not a solid" : why;
    }
  }

  // Identities, children first (items are stored children first).
  std::unordered_map<std::string, std::string> cls;
  if (o.match_shapes) {
    std::vector<std::string> keys;
    std::set<std::string> seen;
    for (size_t i : parts)
      if (items[i].node->kind == Node::Kind::Body && seen.insert(items[i].node->body_key).second) keys.push_back(items[i].node->body_key);
    cls = shape_classes(doc, keys, o.cancelled);
  }
  const auto placement = [](const Mat4& m) {
    std::string s;
    for (int k = 0; k < 12; ++k) s += std::to_string(std::llround(m.m[static_cast<size_t>(k)] * 1e6)) + ",";
    return s;
  };
  for (auto& it : items) {
    const Node& n = *it.node;
    if (const std::string pn = text_of(n.properties, "part_number"); !pn.empty()) {
      it.identity = "pn:" + pn;
    } else if (n.kind == Node::Kind::Body) {
      const auto c = cls.find(n.body_key);
      it.identity = "key:" + (c == cls.end() ? n.body_key : c->second);
      if (const std::string m = it.material.id.empty() ? it.material.text : it.material.id; !m.empty()) it.identity += "|" + m;
      for (const char* k : {"density", "mass"})
        if (const double v = property_number(n.properties.value(k, json())); v > 0) it.identity += std::string("|") + k + "=" + json(v).dump();
      if (flag(n) == "purchased") it.identity += "|purchased";
    } else if (it.part) {  // bought as one: its name without instance numbers and the shapes in it
      std::vector<std::string> keys;
      for (const auto& b : scene.bodies_under(it.id)) keys.push_back(scene.node(b)->body_key);
      std::sort(keys.begin(), keys.end());
      std::string s = strip_number(n.name);
      for (const auto& k : keys) s += "|" + k;
      it.identity = "purchased:" + short_hash(s);
    } else {
      std::vector<std::string> content;
      for (size_t k : it.kids) content.push_back(items[k].identity + "@" + placement(items[k].node->local));
      std::sort(content.begin(), content.end());
      std::string s;
      for (const auto& c : content) s += c + ";";
      it.identity = "assembly:" + short_hash(s);
    }
  }
  cancelled();

  // An assembly's mass is its items', unless a mass property gives it.
  std::function<std::optional<double>(size_t)> mass_of = [&](size_t i) -> std::optional<double> {
    Item& it = items[i];
    if (it.part || it.mass || !it.mass_error.empty()) return it.mass;
    if (const double given = property_number(it.node->properties.value("mass", json())); given > 0) return it.mass = given;
    double sum = 0;
    for (size_t k : it.kids) {
      const auto m = mass_of(k);
      if (!m) {
        it.mass_error = "a part in it has no mass";
        return std::nullopt;
      }
      sum += *m;
    }
    return it.mass = sum;
  };

  std::unordered_map<std::string, std::string> sources;
  const auto source_of = [&](const Node& n) -> std::string {
    if (const auto it = sources.find(n.source_op); it != sources.end()) return it->second;
    const Op* op = doc.find_op(n.source_op);
    std::string s = !op ? "" : op->type == "feature" ? "design" : op->type == "import" ? op->data.value("source", "") : "";
    return sources[n.source_op] = s;
  };

  // Rows: equal identities in one, in the order they first appear.
  const auto group = [&](const std::vector<size_t>& list) {
    std::vector<std::vector<size_t>> groups;
    std::unordered_map<std::string, size_t> at;
    for (size_t i : list) {
      const auto [it, added] = at.emplace(items[i].identity, groups.size());
      if (added) groups.emplace_back();
      groups[it->second].push_back(i);
    }
    return groups;
  };
  json rows = json::array();
  std::set<std::string> columns;
  const bool indented = o.mode == "indented";
  const auto row = [&](const std::vector<size_t>& g, const std::string& item, int level, long long qty, long long total) {
    const Item& first = items[g.front()];
    json r = {{"item", item}, {"qty", qty}};
    if (indented) r["level"] = level, r["total_qty"] = total;
    r["kind"] = first.part ? "part" : "assembly";
    std::vector<std::string> names;
    json merged = json::object(), nodes = json::array();
    bool purchased = false;
    for (size_t i : g) {
      const Node& n = *items[i].node;
      const std::string source = source_of(n);
      names.push_back(source.empty() || source == "design" ? n.name : without_occurrence(n.name));
      nodes.push_back(n.id);
      purchased = purchased || flag(n) == "purchased";
      for (const auto& [k, v] : n.properties.items())
        if (!merged.contains(k) && !v.is_null() && !(v.is_string() && v.get_ref<const std::string&>().empty())) merged[k] = v;
    }
    r["name"] = common_name(names);
    for (const char* k : {"part_number", "description"})
      if (const std::string v = text_of(merged, k); !v.empty()) r[k] = v;
    if (first.part) {
      if (!first.material.text.empty()) r["material"] = first.material.shown();
      if (!first.material.id.empty()) r["material_id"] = first.material.id;
      if (first.material.density > 0) r["density"] = first.material.density;
    }
    if (o.mass) {
      if (const auto m = mass_of(g.front())) {
        r["mass"] = *m / unit;
        r["total_mass"] = *m * double(indented ? total : qty) / unit;
      } else {
        r["mass_error"] = items[g.front()].mass_error;
      }
    }
    for (const char* k : {"vendor", "notes"})
      if (const std::string v = text_of(merged, k); !v.empty()) r[k] = v;
    if (purchased) r["purchased"] = true;
    if (const std::string s = source_of(*first.node); !s.empty()) r["source"] = s;
    json custom = json::object();
    for (const auto& [k, v] : merged.items())
      if (!own_field(k)) custom[k] = v, columns.insert(k);
    if (!custom.empty()) r["properties"] = custom;
    r["nodes"] = nodes;
    r["identity"] = first.identity;
    rows.push_back(std::move(r));
  };
  if (o.mode == "top") {
    int n = 0;
    for (const auto& g : group(roots)) row(g, std::to_string(++n), 1, static_cast<long long>(g.size()), static_cast<long long>(g.size()));
  } else if (o.mode == "parts") {
    int n = 0;
    for (const auto& g : group(parts)) row(g, std::to_string(++n), 1, static_cast<long long>(g.size()), static_cast<long long>(g.size()));
  } else {
    // The items of every instance of an assembly in one row each (their nodes all listed), counted per assembly.
    using Lists = std::vector<const std::vector<size_t>*>;
    std::function<void(const Lists&, int, const std::string&, long long)> emit = [&](const Lists& lists, int level, const std::string& prefix,
                                                                                    long long outer) {
      std::vector<size_t> all;
      for (const auto* l : lists) all.insert(all.end(), l->begin(), l->end());
      int n = 0;
      for (const auto& g : group(all)) {
        const std::string item = (prefix.empty() ? "" : prefix + ".") + std::to_string(++n);
        const long long qty = std::max<long long>(1, std::llround(double(g.size()) / double(lists.size())));
        row(g, item, level, qty, outer * qty);
        Lists next;
        for (size_t i : g)
          if (!items[i].part) next.push_back(&items[i].kids);
        if (!next.empty()) emit(next, level + 1, item, outer * qty);
      }
    };
    emit({&roots}, 1, "", 1);
  }

  json totals = {{"rows", rows.size()}, {"parts", parts.size()}};
  if (o.mass) {  // what is known of it, and whether that is all
    bool complete = true;
    std::function<double(size_t)> weigh = [&](size_t i) -> double {
      const Item& it = items[i];
      if (it.part) {
        complete = complete && it.mass.has_value();
        return it.mass.value_or(0);
      }
      if (const double given = property_number(it.node->properties.value("mass", json())); given > 0) return given;
      double sum = 0;
      for (size_t k : it.kids) sum += weigh(k);
      return sum;
    };
    double sum = 0;
    for (size_t r : roots) sum += weigh(r);
    totals["mass"] = sum / unit;
    totals["mass_complete"] = complete;
  }
  return {{"mode", o.mode}, {"assembly", assembly}, {"mass_unit", o.mass_unit}, {"columns", columns}, {"rows", rows}, {"totals", totals}};
}

std::string bom_csv(const json& b, char separator, const json& labels) {
  const bool indented = b.value("mode", "") == "indented";
  const std::string unit = b.value("mass_unit", "g");
  const int decimals = unit == "g" ? 2 : 5;
  struct Column {
    std::string id, header;
    bool text;
  };
  std::vector<Column> columns;
  const auto add = [&](const std::string& id, const std::string& header, bool text) {
    std::string h = labels.is_object() && labels.contains(id) && labels[id].is_string() ? labels[id].get<std::string>() : header;
    if (id == "mass" || id == "total_mass") h += " (" + unit + ")";
    columns.push_back({id, h, text});
  };
  add("item", "Item", false);
  if (indented) add("level", "Level", false);
  add("qty", "Qty", false);
  if (indented) add("total_qty", "Total qty", false);
  for (const auto& [id, header] : std::vector<std::pair<std::string, std::string>>{
           {"part_number", "Part number"}, {"name", "Name"}, {"description", "Description"}, {"material", "Material"}})
    add(id, header, true);
  add("mass", "Mass", false);
  add("total_mass", "Total mass", false);
  add("vendor", "Vendor", true);
  add("purchased", "Purchased", false);
  add("source", "Source", true);
  add("notes", "Notes", true);
  const size_t own = columns.size();
  for (const auto& c : b.value("columns", json::array())) columns.push_back({c.get<std::string>(), c.get<std::string>(), true});

  const auto field = [&](std::string s, bool text) {
    // A spreadsheet runs text that starts like a formula: keep it text.
    if (text && !s.empty() && std::string("=+-@\t\r").find(s[0]) != std::string::npos) s = "'" + s;
    if (s.find_first_of(std::string("\"\r\n") + separator) != std::string::npos || (!s.empty() && (s.front() == ' ' || s.back() == ' '))) {
      std::string q = "\"";
      for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
      return q + "\"";
    }
    return s;
  };
  std::string out = "\xEF\xBB\xBF";
  const auto line = [&](const std::vector<std::string>& cells) {
    for (size_t i = 0; i < cells.size(); ++i) out += (i ? std::string(1, separator) : std::string()) + cells[i];
    out += "\r\n";
  };
  std::vector<std::string> cells;
  for (const auto& c : columns) cells.push_back(field(c.header, true));
  line(cells);
  for (const auto& r : b.value("rows", json::array())) {
    cells.clear();
    for (size_t i = 0; i < columns.size(); ++i) {
      const Column& c = columns[i];
      const json v = i < own ? r.value(c.id, json()) : r.value("properties", json::object()).value(c.id, json());
      std::string s;
      if (c.id == "purchased") s = v == true ? "yes" : "";
      else if (c.id == "mass" || c.id == "total_mass") s = v.is_number() ? number(v.get<double>(), decimals) : "";
      else if (v.is_string()) s = v.get<std::string>();
      else if (!v.is_null()) s = v.dump();
      cells.push_back(field(s, c.text));
    }
    line(cells);
  }
  return out;
}

}  // namespace opad::drawing
