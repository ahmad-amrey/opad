#ifdef _WIN32  // first: OCCT's headers leave out its code-page API when they include it themselves
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "opad/diff.hpp"

#include <Bnd_Box.hxx>
#include <OSD_Parallel.hxx>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/mass.hpp"
#include "opad/merge.hpp"
#include "opad/scene.hpp"

namespace opad {
namespace {

// ---------------------------------------------------------------- text helpers
std::string num(double v) {
  if (std::abs(v) < 5e-10) v = 0;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6g", v);
  return buf;
}
std::string vec_text(const Vec3& v) { return "(" + num(v[0]) + ", " + num(v[1]) + ", " + num(v[2]) + ")"; }
std::string hex_color(const std::array<double, 3>& c) {
  auto byte = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
  char buf[8];
  std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(c[0]), byte(c[1]), byte(c[2]));
  return buf;
}
// At most n bytes on one line, cut between characters.
std::string clip(std::string s, size_t n = 60) {
  for (auto& c : s)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  if (s.size() <= n) return s;
  size_t cut = n - 3;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
  return s.substr(0, cut) + "...";
}
std::string quoted(const std::string& s, size_t n = 60) { return "\"" + clip(s, n) + "\""; }
std::string str(const json& j, const char* key) { return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string(); }
std::string utf8(const std::filesystem::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}
std::string join(const std::vector<std::string>& parts, const char* sep) {
  std::string out;
  for (const auto& p : parts) out += (out.empty() ? "" : sep) + p;
  return out;
}

bool same_op(const Op& x, const Op& y) { return x.id == y.id && (!x.raw.empty() && !y.raw.empty() ? x.raw == y.raw : x.data == y.data); }
bool same_matrix(const Mat4& x, const Mat4& y) {
  for (size_t i = 0; i < 16; ++i)
    if (std::abs(x.m[i] - y.m[i]) > 1e-9) return false;
  return true;
}
// The rotation taking x's frame to y's (Ry Rx^T): its angle in degrees and its axis.
double rotation_between(const Mat4& x, const Mat4& y, Vec3& axis) {
  double r[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      r[i][j] = 0;
      for (int k = 0; k < 3; ++k) r[i][j] += y.at(i, k) * x.at(j, k);
    }
  const double angle = std::acos(std::clamp((r[0][0] + r[1][1] + r[2][2] - 1) / 2, -1.0, 1.0));
  axis = {r[2][1] - r[1][2], r[0][2] - r[2][0], r[1][0] - r[0][1]};
  double len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  if (len < 1e-9 && angle > 1) {  // a half turn: the axis is the column of R + I that is not zero
    int k = 0;
    for (int i = 1; i < 3; ++i)
      if (r[i][i] > r[k][k]) k = i;
    for (int i = 0; i < 3; ++i) axis[size_t(i)] = r[i][k] + (i == k ? 1 : 0);
    len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
  }
  for (auto& v : axis) v = len > 1e-12 ? v / len : 0;
  return angle * 180 / M_PI;
}
Vec3 translation_of(const Mat4& m) { return {m.at(0, 3), m.at(1, 3), m.at(2, 3)}; }

std::string name_of(const Scene& s, const std::string& id) {
  if (const Node* n = s.node(id)) return n->name;
  if (const SketchItem* k = s.sketch(id)) return k->name;
  if (const Feature* f = s.feature(id)) return f->name;
  return id.substr(0, 8);
}

// A reference or a plane/axis input as a person reads it: "Housing face 12", "Sketch1 region", "XY plane".
std::string ref_text(const json& r, const Scene& s) {
  if (r.is_string()) return name_of(s, r.get<std::string>());
  if (!r.is_object()) return clip(r.dump());
  if (r.contains("support")) return ref_text(r["support"], s);
  if (r.contains("base") && r["base"].is_string()) {
    std::string b = r["base"].get<std::string>();
    for (auto& c : b) c = char(std::toupper(static_cast<unsigned char>(c)));
    return b + (b.size() == 1 ? " axis" : " plane");
  }
  for (const char* k : {"face", "edge", "feature"})
    if (r.contains(k)) return ref_text(r[k], s);
  if (r.contains("sketch") && r["sketch"].is_string()) {
    std::string t = name_of(s, r["sketch"].get<std::string>());
    if (r.contains("at")) return t + " region";
    if (r.contains("point")) return t + " point " + r["point"].dump();
    if (r.contains("entity")) return t + " entity " + r["entity"].dump();
    return t;
  }
  if (r.contains("body") && r["body"].is_string()) {
    std::string t = name_of(s, r["body"].get<std::string>());
    const std::string kind = str(r, "kind");
    if (!kind.empty() && kind != "body" && r.contains("index")) t += " " + kind + " " + r["index"].dump();
    return t;
  }
  if (str(r, "kind") == "point" && r.contains("point") && r["point"].is_array() && r["point"].size() == 3)
    return "point " + vec_text({r["point"][0].get<double>(), r["point"][1].get<double>(), r["point"][2].get<double>()});
  if (r.contains("select")) return "rule " + clip(r["select"].dump(), 40);
  return clip(r.dump());
}
std::string refs_text(const json& v, const Scene& s) {
  if (!v.is_array()) return ref_text(v, s);
  if (v.empty()) return "none";
  std::vector<std::pair<std::string, int>> runs;
  for (const auto& r : v) {
    std::string t = ref_text(r, s);
    if (!runs.empty() && runs.back().first == t) ++runs.back().second;
    else runs.emplace_back(std::move(t), 1);
  }
  std::vector<std::string> parts;
  for (const auto& [t, n] : runs) {
    if (parts.size() == 4) { parts.push_back("+" + std::to_string(runs.size() - 4) + " more"); break; }
    parts.push_back((n > 1 ? std::to_string(n) + " x " : "") + t);
  }
  return join(parts, ", ");
}
design::ParamTable param_table(const Scene& s) {
  std::vector<design::ParamDef> defs;
  for (const auto& p : s.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return design::ParamTable(std::move(defs), s.units);
}
// A feature input value: expressions as typed (and what they come to, when that reads differently), bare numbers with
// the unit they are read in, references by name.
std::string value_text(const std::string& type, const json& v, const Scene& s, const design::ParamTable* params = nullptr) {
  if (v.is_null()) return "none";
  if (v.is_string() && params && (type == "length" || type == "angle" || type == "number" || type == "count")) {
    const std::string expr = v.get<std::string>();
    try {
      const std::string shown = design::format_quantity(params->eval(expr));
      if (shown != expr) return clip(expr, 80) + " = " + shown;
    } catch (const std::exception&) {
    }
  }
  if (v.is_string()) return clip(v.get<std::string>(), 80);
  if (v.is_boolean()) return v.get<bool>() ? "yes" : "no";
  if (v.is_number()) return num(v.get<double>()) + (type == "length" ? " mm" : type == "angle" ? " deg" : "");
  return refs_text(v, s);
}
json without_hints(const json& v) {
  if (v.is_object()) {
    json o = json::object();
    for (const auto& [k, x] : v.items())
      if (k != "hint") o[k] = without_hints(x);
    return o;
  }
  if (v.is_array()) {
    json a = json::array();
    for (const auto& x : v) a.push_back(without_hints(x));
    return a;
  }
  return v;
}
std::string param_text(const Param& p) { return p.shown.empty() || p.shown == p.expr ? p.expr : p.expr + " = " + p.shown; }
std::string dim_text(const json& c) {
  const double v = c.value("value", 0.0);
  const std::string t = c.value("type", "") == "angle" ? num(v * 180 / M_PI) + " deg" : num(v) + " mm";
  const std::string expr = str(c, "expr");
  return expr.empty() || expr == t ? t : expr + " = " + t;
}

std::vector<std::string> tree_order(const Scene& s) {
  std::vector<std::string> out;
  out.reserve(s.nodes.size());
  std::vector<std::string> stack(s.roots.rbegin(), s.roots.rend());
  while (!stack.empty()) {
    std::string id = std::move(stack.back());
    stack.pop_back();
    const Node* n = s.node(id);
    if (!n) continue;
    out.push_back(id);
    stack.insert(stack.end(), n->children.rbegin(), n->children.rend());
  }
  return out;
}

json op_summary(const Op& o) {
  json j;
  j["id"] = o.id;
  j["op"] = o.type;
  j["ts"] = str(o.data, "ts");
  j["by"] = str(o.data, "by");
  for (const char* k : {"target", "name", "text", "kind", "source"})
    if (o.data.contains(k) && o.data[k].is_string()) j[k] = o.data[k];
  return j;
}

json body_metrics(const Document& d, const std::string& key) {
  try {
    const TopoDS_Shape shape = body_shape(d, key);
    json m{{"volume", volume_properties(shape).mass}, {"area", area_properties(shape).mass}};
    const Bnd_Box box = tight_bbox(shape);
    if (!box.IsVoid()) {
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      m["size"] = {x1 - x0, y1 - y0, z1 - z0};
    }
    return m;
  } catch (const std::exception& e) {
    return {{"error", e.what()}};
  } catch (...) {
    return {{"error", "the body cannot be read"}};
  }
}

// ---------------------------------------------------------------- the parts of a diff
struct Sides {
  const Document &a, &b;
  const Scene &sa, &sb;
  design::ParamTable pa = param_table(sa), pb = param_table(sb);
  std::unordered_set<std::string> ops_a{}, ops_b{};  // op ids of each log
  bool tombstoned(bool in_b, const std::string& id) const {
    const Scene& s = in_b ? sb : sa;
    return (in_b ? ops_b : ops_a).count(id) && std::binary_search(s.deleted_ops.begin(), s.deleted_ops.end(), id);
  }
};

json entry(const char* kind, const char* change, const std::string& id, const std::string& name) {
  return {{"kind", kind}, {"change", change}, {"id", id}, {"name", name}};
}

void diff_params(const Sides& d, json& out) {
  std::unordered_map<std::string, const Param*> in_a, in_b;
  for (const auto& p : d.sa.params) in_a[p.id] = &p;
  for (const auto& p : d.sb.params) in_b[p.id] = &p;
  for (const auto& y : d.sb.params) {
    const auto it = in_a.find(y.id);
    if (it == in_a.end()) {
      json c = entry("param", "added", y.id, y.name);
      c["after"] = param_text(y);
      out.push_back(std::move(c));
      continue;
    }
    const Param& x = *it->second;
    if (x.name != y.name) {
      json c = entry("param", "renamed", y.id, y.name);
      c["before"] = x.name;
      c["after"] = y.name;
      out.push_back(std::move(c));
    }
    if (param_text(x) != param_text(y)) {
      json c = entry("param", "edited", y.id, y.name);
      c["before"] = param_text(x);
      c["after"] = param_text(y);
      out.push_back(std::move(c));
    }
  }
  for (const auto& x : d.sa.params)
    if (!in_b.count(x.id)) {
      json c = entry("param", "removed", x.id, x.name);
      c["before"] = param_text(x);
      out.push_back(std::move(c));
    }
}

// Records of one sketch list matched by id: how many were added, removed or changed (`both`: the changed pairs).
json record_counts(const json& before, const json& after, const char* key, std::vector<std::pair<const json*, const json*>>* both = nullptr) {
  static const json none = json::array();
  auto records = [&](const json& g) -> const json& { return g.is_object() && g.contains(key) && g[key].is_array() ? g[key] : none; };
  std::map<int, const json*> x, y;
  for (const auto& r : records(before)) x[r.value("id", 0)] = &r;
  for (const auto& r : records(after)) y[r.value("id", 0)] = &r;
  int added = 0, removed = 0, changed = 0;
  for (const auto& [id, r] : y) {
    const auto it = x.find(id);
    if (it == x.end()) ++added;
    else if (*it->second != *r) {
      ++changed;
      if (both) both->emplace_back(it->second, r);
    }
  }
  for (const auto& [id, r] : x)
    if (!y.count(id)) ++removed;
  json o = json::object();
  if (added) o["added"] = added;
  if (removed) o["removed"] = removed;
  if (changed) o["changed"] = changed;
  return o;
}

bool same_frame(const Frame& x, const Frame& y) {
  for (int i = 0; i < 3; ++i)
    if (std::abs(x.origin[i] - y.origin[i]) > 1e-9 || std::abs(x.x[i] - y.x[i]) > 1e-12 || std::abs(x.y[i] - y.y[i]) > 1e-12) return false;
  return true;
}

void diff_sketches(const Sides& d, json& out) {
  std::unordered_map<std::string, const SketchItem*> in_a, in_b;
  for (const auto& s : d.sa.sketches) in_a[s.id] = &s;
  for (const auto& s : d.sb.sketches) in_b[s.id] = &s;
  auto size = [](const json& g, const char* key) { return g.value(key, json::array()).size(); };
  for (const auto& y : d.sb.sketches) {
    const auto it = in_a.find(y.id);
    if (it == in_a.end()) {
      json c = entry("sketch", "added", y.id, y.name);
      c["entities"] = size(y.geometry, "entities");
      c["constraints"] = size(y.geometry, "constraints");
      out.push_back(std::move(c));
      continue;
    }
    const SketchItem& x = *it->second;
    if (x.name != y.name) {
      json c = entry("sketch", "renamed", y.id, y.name);
      c["before"] = x.name;
      c["after"] = y.name;
      out.push_back(std::move(c));
    }
    json c = entry("sketch", "edited", y.id, y.name);
    std::vector<std::pair<const json*, const json*>> constraints;
    for (const char* key : {"points", "entities", "constraints", "images", "patterns"}) {
      json n = record_counts(x.geometry, y.geometry, key, std::string(key) == "constraints" ? &constraints : nullptr);
      if (!n.empty()) c[key] = std::move(n);
    }
    json dims = json::array();
    for (const auto& [p, q] : constraints)
      if (q->contains("value") && (p->value("value", 0.0) != q->value("value", 0.0) || str(*p, "expr") != str(*q, "expr")))
        dims.push_back({{"id", q->value("id", 0)}, {"type", q->value("type", "")}, {"before", dim_text(*p)}, {"after", dim_text(*q)}});
    if (!dims.empty()) c["dimensions"] = std::move(dims);
    if (without_hints(x.plane) != without_hints(y.plane) || !same_frame(x.frame, y.frame)) c["plane"] = true;
    if (x.dof != y.dof) c["dof"] = {{"before", x.dof}, {"after", y.dof}};
    if (x.error != y.error) c["error"] = y.error;
    if (c.size() > 4) out.push_back(std::move(c));
  }
  for (const auto& x : d.sa.sketches)
    if (!in_b.count(x.id)) out.push_back(entry("sketch", "removed", x.id, x.name));
}

json input_details(const Feature& x, const Feature& y, const Sides& d) {
  const Scene &sa = d.sa, &sb = d.sb;
  json out = json::array();
  std::vector<std::pair<std::string, const design::InputSpec*>> keys;
  std::set<std::string> named, rest;
  if (const design::FeatureSpec* spec = design::feature_spec(y.kind))
    for (const auto& in : spec->inputs) { keys.emplace_back(in.name, &in); named.insert(in.name); }
  for (const json* in : {&x.inputs, &y.inputs})
    if (in->is_object())
      for (const auto& [k, v] : in->items())
        if (!named.count(k)) rest.insert(k);
  for (const auto& k : rest) keys.emplace_back(k, nullptr);
  for (const auto& [key, in] : keys) {
    const json va = x.inputs.is_object() && x.inputs.contains(key) ? x.inputs[key] : json();
    const json vb = y.inputs.is_object() && y.inputs.contains(key) ? y.inputs[key] : json();
    if (without_hints(va) == without_hints(vb)) continue;
    const std::string type = in ? in->type : std::string();
    out.push_back({{"key", key}, {"label", in ? in->label : key}, {"before", value_text(type, va, sa, &d.pa)}, {"after", value_text(type, vb, sb, &d.pb)}});
  }
  if (x.suppress_if != y.suppress_if)
    out.push_back({{"key", "suppress_if"}, {"label", "Suppress if"}, {"before", x.suppress_if.empty() ? "none" : x.suppress_if},
                   {"after", y.suppress_if.empty() ? "none" : y.suppress_if}});
  return out;
}

json result_bodies(const Feature& f) {
  json ids = json::array();
  for (const auto& b : f.result.value("bodies", json::array())) ids.push_back(str(b, "id"));
  return ids;
}
json result_state(const Feature& f) {
  json o = json::object();
  for (const auto& b : f.result.value("bodies", json::array())) o[str(b, "id")] = str(b, "key");
  o["-"] = f.result.value("removed", json::array());
  return o;
}

void diff_features(const Sides& d, json& out) {
  std::unordered_map<std::string, const Feature*> in_a, in_b;
  for (const auto& f : d.sa.features) in_a[f.id] = &f;
  for (const auto& f : d.sb.features) in_b[f.id] = &f;
  auto feature = [](const Feature& f, const char* change) {
    json c = entry("feature", change, f.id, f.name);
    c["feature"] = f.kind;
    return c;
  };
  for (const auto& y : d.sb.features) {
    const auto it = in_a.find(y.id);
    if (it == in_a.end()) {
      json c = feature(y, "added");
      if (const auto* spec = design::feature_spec(y.kind)) c["label"] = spec->label;
      out.push_back(std::move(c));
      continue;
    }
    const Feature& x = *it->second;
    if (x.name != y.name) {
      json c = feature(y, "renamed");
      c["before"] = x.name;
      c["after"] = y.name;
      out.push_back(std::move(c));
    }
    json details = input_details(x, y, d);
    if (!details.empty() || result_state(x) != result_state(y)) {
      json c = feature(y, details.empty() ? "regenerated" : "edited");
      if (!details.empty()) c["details"] = std::move(details);
      c["bodies"] = result_bodies(y);
      out.push_back(std::move(c));
    }
    if (x.suppressed != y.suppressed) out.push_back(feature(y, y.suppressed ? "suppressed" : "unsuppressed"));
    if (x.error != y.error) {
      json c = feature(y, y.error.empty() ? "fixed" : "failed");
      if (!y.error.empty()) c["error"] = y.error;
      out.push_back(std::move(c));
    }
  }
  for (const auto& x : d.sa.features)
    if (!in_b.count(x.id)) out.push_back(feature(x, "removed"));
}

void diff_assets(const std::vector<EffectiveOp>& ea, const std::vector<EffectiveOp>& eb, json& out) {
  auto assets = [](const std::vector<EffectiveOp>& ops) {
    std::vector<std::pair<const Op*, const json*>> found;
    for (const auto& e : ops)
      if (e.op->type == "import" && e.data().contains("asset") && e.data()["asset"].is_object()) found.emplace_back(e.op, &e.data()["asset"]);
    return found;
  };
  std::unordered_map<std::string, const json*> before;
  for (const auto& [op, asset] : assets(ea)) before[op->id] = asset;
  for (const auto& [op, asset] : assets(eb)) {
    const auto it = before.find(op->id);
    if (it == before.end()) continue;  // a new import: its nodes are listed as added
    std::string name = str(*asset, "path");
    if (name.empty()) name = str(op->data, "source");
    name = name.substr(name.find_last_of("/\\") + 1);
    const std::string x = str(*it->second, "sha256"), y = str(*asset, "sha256");
    if (x != y) {
      json c = entry("asset", "synced", op->id, name);
      c["before"] = x.substr(0, 7);
      c["after"] = y.substr(0, 7);
      c["path"] = str(*asset, "path");
      out.push_back(std::move(c));
    }
    if (str(*it->second, "storage") != str(*asset, "storage")) {
      json c = entry("asset", "storage", op->id, name);
      c["before"] = str(*it->second, "storage");
      c["after"] = str(*asset, "storage");
      out.push_back(std::move(c));
    }
  }
}

// Components and bodies matched by node id. What a new (or removed) component holds is not listed again.
void diff_tree(const Sides& d, json& out, std::vector<size_t>& geometry) {
  auto kind = [](const Node& n) { return n.kind == Node::Kind::Body ? "body" : "component"; };
  auto parent_name = [](const Scene& s, const std::string& p) { return p.empty() ? std::string() : name_of(s, p); };
  auto added = [&](const Scene& s, const Node& n, const char* change) {
    json c = entry(kind(n), change, n.id, n.name);
    if (!n.parent.empty()) c["parent"] = parent_name(s, n.parent);
    if (n.kind == Node::Kind::Component) c["bodies"] = s.bodies_under(n.id).size();
    out.push_back(std::move(c));
  };
  for (const auto& id : tree_order(d.sb)) {
    const Node& y = *d.sb.node(id);
    const Node* x = d.sa.node(id);
    if (!x) {
      if (y.parent.empty() || d.sa.node(y.parent)) added(d.sb, y, "added");
      continue;
    }
    if (x->name != y.name) {
      json c = entry(kind(y), "renamed", id, y.name);
      c["before"] = x->name;
      c["after"] = y.name;
      out.push_back(std::move(c));
    }
    if (x->parent != y.parent) {
      json c = entry(kind(y), "reparented", id, y.name);
      c["before"] = parent_name(d.sa, x->parent);
      c["after"] = parent_name(d.sb, y.parent);
      c["parent_before"] = x->parent;
      c["parent_after"] = y.parent;
      out.push_back(std::move(c));
    }
    if (!same_matrix(x->local, y.local)) {
      json c = entry(kind(y), "moved", id, y.name);
      const Vec3 tx = translation_of(x->local), ty = translation_of(y.local);
      c["translation"] = {ty[0] - tx[0], ty[1] - tx[1], ty[2] - tx[2]};
      Vec3 axis;
      const double deg = rotation_between(x->local, y.local, axis);
      c["rotation_deg"] = deg;
      if (deg > 1e-7) c["axis"] = axis;
      out.push_back(std::move(c));
    }
    if (y.kind == Node::Kind::Body && x->body_key != y.body_key) {
      json c = entry("body", "geometry", id, y.name);
      c["key_before"] = x->body_key;
      c["key_after"] = y.body_key;
      geometry.push_back(out.size());
      out.push_back(std::move(c));
    }
    json fields = json::object();
    auto color = [](const Node& n) { return n.has_color ? json(hex_color(n.color)) : json(); };
    if (color(*x) != color(y)) fields["color"] = {{"before", color(*x)}, {"after", color(y)}};
    if (x->opacity != y.opacity) fields["opacity"] = {{"before", x->opacity}, {"after", y.opacity}};
    if (x->visible != y.visible) fields["visible"] = {{"before", x->visible}, {"after", y.visible}};
    if (x->locked != y.locked) fields["locked"] = {{"before", x->locked}, {"after", y.locked}};
    if (!fields.empty()) {
      json c = entry(kind(y), "appearance", id, y.name);
      c["fields"] = std::move(fields);
      out.push_back(std::move(c));
    }
  }
  for (const auto& id : tree_order(d.sa)) {
    const Node& x = *d.sa.node(id);
    if (!d.sb.node(id) && (x.parent.empty() || d.sb.node(x.parent))) added(d.sa, x, "removed");
  }
}

void diff_notes(const Sides& d, json& out) {
  std::unordered_map<std::string, const Annotation*> in_a, in_b;
  for (const auto& n : d.sa.annotations) in_a[n.id] = &n;
  for (const auto& n : d.sb.annotations) in_b[n.id] = &n;
  auto note = [](const Annotation& n, const char* change) { return json{{"kind", "annotation"}, {"change", change}, {"id", n.id}, {"text", n.text}}; };
  for (const auto& y : d.sb.annotations) {
    const auto it = in_a.find(y.id);
    if (it == in_a.end()) {
      json c = note(y, d.tombstoned(false, y.id) ? "reopened" : "added");
      c["style"] = y.style;
      c["by"] = y.by;
      out.push_back(std::move(c));
      continue;
    }
    const Annotation& x = *it->second;
    if (x.text != y.text) {
      json c = note(y, "edited");
      c["before"] = x.text;
      c["after"] = y.text;
      out.push_back(std::move(c));
    }
    if (x.style != y.style) {
      json c = note(y, "restyled");
      c["before"] = x.style;
      c["after"] = y.style;
      out.push_back(std::move(c));
    }
    if (x.anchor.str() != y.anchor.str()) {
      json c = note(y, "reanchored");
      c["before"] = ref_text(x.anchor.to_json(), d.sa);
      c["after"] = ref_text(y.anchor.to_json(), d.sb);
      out.push_back(std::move(c));
    }
    std::set<std::string> seen;
    for (const auto& r : x.comments) seen.insert(str(r, "id"));
    json replies = json::array();
    for (const auto& r : y.comments)
      if (!seen.count(str(r, "id"))) replies.push_back({{"by", str(r, "by")}, {"text", str(r, "text")}});
    if (!replies.empty()) {
      json c = note(y, "commented");
      c["comments"] = std::move(replies);
      out.push_back(std::move(c));
    }
  }
  for (const auto& x : d.sa.annotations)
    if (!in_b.count(x.id)) out.push_back(note(x, d.tombstoned(true, x.id) ? "resolved" : "removed"));

  auto label = [](const Measurement& m) { return m.text.empty() ? m.kind : m.kind + ": " + clip(m.text); };
  std::unordered_set<std::string> measured_a, measured_b;
  for (const auto& m : d.sa.measurements) measured_a.insert(m.id);
  for (const auto& m : d.sb.measurements) measured_b.insert(m.id);
  for (const auto& m : d.sb.measurements)
    if (!measured_a.count(m.id)) out.push_back(entry("measurement", d.tombstoned(false, m.id) ? "reopened" : "added", m.id, label(m)));
  for (const auto& m : d.sa.measurements)
    if (!measured_b.count(m.id)) out.push_back(entry("measurement", d.tombstoned(true, m.id) ? "resolved" : "removed", m.id, label(m)));
}

template <class T, class Same>
void diff_named(const char* kind, const std::vector<T>& xs, const std::vector<T>& ys, Same same, json& out) {
  std::unordered_map<std::string, const T*> in_a, in_b;
  for (const auto& v : xs) in_a[v.id] = &v;
  for (const auto& v : ys) in_b[v.id] = &v;
  for (const auto& y : ys) {
    const auto it = in_a.find(y.id);
    if (it == in_a.end()) { out.push_back(entry(kind, "added", y.id, y.name)); continue; }
    if (it->second->name != y.name) {
      json c = entry(kind, "renamed", y.id, y.name);
      c["before"] = it->second->name;
      c["after"] = y.name;
      out.push_back(std::move(c));
    }
    if (!same(*it->second, y)) out.push_back(entry(kind, "edited", y.id, y.name));
  }
  for (const auto& x : xs)
    if (!in_b.count(x.id)) out.push_back(entry(kind, "removed", x.id, x.name));
}

// ---------------------------------------------------------------- summary line
struct Phrase {
  std::string verb, object, noun;  // noun: what the object is, for "move 12 bodies"
};
void phrases_of(const json& c, bool design, std::vector<Phrase>& out) {
  const std::string kind = str(c, "kind"), change = str(c, "change"), name = str(c, "name");
  const std::string noun = kind == "body" ? "bodies" : kind == "component" ? "components" : kind == "annotation" ? "notes" : kind + "s";
  const std::string note = quoted(str(c, "text"), 40);
  if (kind == "annotation") {
    if (change == "added" || change == "reopened") out.push_back({change == "added" ? "note" : "reopen", note, noun});
    else if (change == "resolved" || change == "removed") out.push_back({change == "resolved" ? "resolve" : "remove", note, noun});
    else if (change == "commented") out.push_back({"reply to", note, noun});
    else out.push_back({"edit note", note, noun});
    return;
  }
  if (kind == "units") { out.push_back({"set units to", str(c, "after"), ""}); return; }
  if (kind == "asset") { out.push_back({"sync", name, "assets"}); return; }
  if (change == "added") out.push_back({"add", (kind == "param" ? "parameter " : "") + name, noun});
  else if (change == "removed") out.push_back({"remove", (kind == "param" ? "parameter " : "") + name, noun});
  else if (change == "renamed") out.push_back({"rename", str(c, "before") + " to " + str(c, "after"), noun});
  else if (change == "moved") out.push_back({"move", name, noun});
  else if (change == "reparented") out.push_back({"move", name + " into " + (str(c, "after").empty() ? "the root" : str(c, "after")), noun});
  else if (change == "geometry" && !design) out.push_back({"change", name, noun});
  else if (change == "suppressed" || change == "unsuppressed") out.push_back({change.substr(0, change.size() - 2), name, noun});
  else if (change == "appearance") {
    const json& f = c["fields"];
    if (f.contains("visible")) out.push_back({f["visible"]["after"].get<bool>() ? "show" : "hide", name, noun});
    else if (f.contains("locked")) out.push_back({f["locked"]["after"].get<bool>() ? "lock" : "unlock", name, noun});
    else out.push_back({"restyle", name, noun});
  } else if (change == "edited" && kind == "param") out.push_back({"set", name + " = " + str(c, "after"), noun});
  else if (change == "edited" && kind == "feature") {
    std::vector<std::string> labels;
    for (const auto& x : c["details"]) {
      std::string l = str(x, "label");
      for (auto& ch : l) ch = char(std::tolower(static_cast<unsigned char>(ch)));
      labels.push_back(l);
    }
    out.push_back({"edit", name + " " + join(labels, ", "), noun});
  } else if (change == "edited") out.push_back({"edit", name, noun});
}
std::string summary_line(const json& changes, const std::string& relation) {
  if (relation == "unrelated") return "Another document";
  bool design = false;
  for (const auto& c : changes) design = design || str(c, "kind") == "feature" || str(c, "kind") == "sketch" || str(c, "kind") == "param";
  std::vector<Phrase> phrases;
  for (const auto& c : changes) phrases_of(c, design, phrases);
  if (phrases.empty()) return changes.empty() ? "No changes" : "Regenerate";
  // One clause per verb, in order of first use: up to three objects by name, more by count.
  std::vector<std::string> verbs;
  std::map<std::string, std::vector<const Phrase*>> by_verb;
  for (const auto& p : phrases) {
    auto& list = by_verb[p.verb];
    if (list.empty()) verbs.push_back(p.verb);
    if (std::none_of(list.begin(), list.end(), [&](const Phrase* q) { return q->object == p.object; })) list.push_back(&p);
  }
  std::vector<std::string> clauses;
  for (const auto& v : verbs) {
    const auto& list = by_verb[v];
    std::vector<std::string> objects;
    for (const Phrase* p : list) objects.push_back(p->object);
    const bool one_noun = std::all_of(list.begin(), list.end(), [&](const Phrase* p) { return p->noun == list[0]->noun; });
    clauses.push_back(v + " " + (list.size() <= 3 ? join(objects, ", ") : std::to_string(list.size()) + " " + (one_noun ? list[0]->noun : "items")));
  }
  std::string line;
  for (size_t i = 0; i < clauses.size(); ++i) {
    if (i == 5) { line += "; and " + std::to_string(clauses.size() - 5) + " more"; break; }
    line += (i ? "; " : "") + clauses[i];
  }
  line[0] = char(std::toupper(static_cast<unsigned char>(line[0])));
  return line;
}

// ---------------------------------------------------------------- one change as text
std::string change_line(const json& c) {
  const std::string kind = str(c, "kind"), change = str(c, "change"), name = str(c, "name");
  const char sign = change == "added" || change == "reopened" ? '+' : change == "removed" || change == "resolved" ? '-' : '~';
  std::string t;
  if (kind == "annotation") {
    const std::string text = quoted(str(c, "text"), 80);
    if (change == "added") t = "[" + str(c, "style") + "] " + text + (str(c, "by").empty() ? "" : " by " + str(c, "by"));
    else if (change == "edited") t = quoted(str(c, "before"), 80) + " -> " + quoted(str(c, "after"), 80);
    else if (change == "restyled" || change == "reanchored") t = text + ": " + (change == "reanchored" ? "moved " : "") + str(c, "before") + " -> " + str(c, "after");
    else if (change == "commented") {
      std::vector<std::string> replies;
      for (const auto& r : c["comments"]) replies.push_back("reply by " + str(r, "by") + ": " + quoted(str(r, "text"), 80));
      t = text + ": " + join(replies, "; ");
    } else t = text + (change == "removed" ? "" : " " + change);
  } else if (kind == "units") {
    t = "units " + str(c, "before") + " -> " + str(c, "after");
  } else if (change == "added") {
    t = name;
    if (kind == "component") t += " (component, " + std::to_string(c.value("bodies", 0)) + " bodies)";
    else if (kind == "body") t += " (body)";
    else if (kind == "feature") t += " (" + (c.contains("label") ? str(c, "label") : str(c, "feature")) + ")";
    else if (kind == "param") t += " = " + str(c, "after");
    else if (kind == "sketch") t += ": " + std::to_string(c.value("entities", 0)) + " entities, " + std::to_string(c.value("constraints", 0)) + " constraints";
    if (c.contains("parent")) t += " under " + str(c, "parent");
  } else if (change == "removed") {
    t = name + (kind == "component" ? " (component, " + std::to_string(c.value("bodies", 0)) + " bodies)" : "");
  } else if (change == "renamed") {
    t = name + ": renamed from " + str(c, "before");
  } else if (change == "moved") {
    const json& tr = c["translation"];
    t = name + ": moved " + vec_text({tr[0].get<double>(), tr[1].get<double>(), tr[2].get<double>()}) + " mm";
    if (c.contains("axis")) t += ", rotated " + num(c.value("rotation_deg", 0.0)) + " deg about " + vec_text({c["axis"][0].get<double>(), c["axis"][1].get<double>(), c["axis"][2].get<double>()});
  } else if (change == "geometry") {
    t = name + ": geometry changed (" + str(c, "key_before").substr(0, 12) + " -> " + str(c, "key_after").substr(0, 12) + ")";
    if (c.contains("metrics") && c["metrics"]["before"].contains("volume") && c["metrics"]["after"].contains("volume")) {
      const json& m = c["metrics"];
      t += ", volume " + num(m["before"]["volume"].get<double>()) + " -> " + num(m["after"]["volume"].get<double>()) + " mm3, area " +
           num(m["before"]["area"].get<double>()) + " -> " + num(m["after"]["area"].get<double>()) + " mm2";
    }
  } else if (change == "appearance") {
    std::vector<std::string> parts;
    const json& f = c["fields"];
    auto colour = [](const json& v) { return v.is_string() ? v.get<std::string>() : std::string("default"); };
    if (f.contains("color")) parts.push_back("colour " + colour(f["color"]["before"]) + " -> " + colour(f["color"]["after"]));
    if (f.contains("opacity")) parts.push_back("opacity " + num(f["opacity"]["before"].get<double>()) + " -> " + num(f["opacity"]["after"].get<double>()));
    if (f.contains("visible")) parts.push_back(f["visible"]["after"].get<bool>() ? "shown" : "hidden");
    if (f.contains("locked")) parts.push_back(f["locked"]["after"].get<bool>() ? "locked" : "unlocked");
    t = name + ": " + join(parts, ", ");
  } else if (change == "reparented") {
    t = name + ": moved from " + (str(c, "before").empty() ? "the root" : str(c, "before")) + " to " + (str(c, "after").empty() ? "the root" : str(c, "after"));
  } else if (change == "edited" && kind == "feature") {
    std::vector<std::string> parts;
    for (const auto& x : c["details"]) parts.push_back(str(x, "label") + " " + str(x, "before") + " -> " + str(x, "after"));
    t = name + ": " + join(parts, "; ");
  } else if (change == "edited" && kind == "param") {
    t = name + ": " + str(c, "before") + " -> " + str(c, "after");
  } else if (change == "edited" && kind == "sketch") {
    std::vector<std::string> parts;
    for (const char* key : {"entities", "constraints", "images", "patterns"})
      if (c.contains(key)) {
        const json& n = c[key];
        std::string p = std::string(key) + " ";
        if (n.contains("added")) p += "+" + std::to_string(n["added"].get<int>()) + " ";
        if (n.contains("removed")) p += "-" + std::to_string(n["removed"].get<int>()) + " ";
        if (n.contains("changed")) p += "~" + std::to_string(n["changed"].get<int>()) + " ";
        p.pop_back();
        parts.push_back(p);
      }
    if (c.contains("points") && c["points"].contains("changed")) parts.push_back(std::to_string(c["points"]["changed"].get<int>()) + " points moved");
    for (const auto& x : c.value("dimensions", json::array()))
      parts.push_back("dimension " + std::to_string(x.value("id", 0)) + " (" + str(x, "type") + ") " + str(x, "before") + " -> " + str(x, "after"));
    if (c.value("plane", false)) parts.push_back("plane changed");
    if (c.contains("dof")) parts.push_back("dof " + std::to_string(c["dof"]["before"].get<int>()) + " -> " + std::to_string(c["dof"]["after"].get<int>()));
    if (c.contains("error")) parts.push_back(str(c, "error").empty() ? "no longer fails" : "fails: " + str(c, "error"));
    t = name + ": " + join(parts, "; ");
  } else if (change == "failed") {
    t = name + ": fails: " + str(c, "error");
  } else if (change == "fixed") {
    t = name + ": no longer fails";
  } else if (change == "synced" || change == "storage") {
    t = name + ": " + (change == "synced" ? "synced " : "") + str(c, "before") + " -> " + str(c, "after");
  } else {
    t = name + ": " + change;
  }
  return std::string("  ") + sign + " " + t + "\n";
}

}  // namespace

json semantic_diff(const Document& a, const Document& b, const DiffOptions& opt) {
  const Scene sa = resolve(a), sb = resolve(b);
  Sides d{a, b, sa, sb};
  for (const auto& o : a.ops) d.ops_a.insert(o.id);
  for (const auto& o : b.ops) d.ops_b.insert(o.id);

  json j;
  j["a"] = a.path.empty() ? std::string("a") : utf8(a.path);
  j["b"] = b.path.empty() ? std::string("b") : utf8(b.path);
  j["same_document"] = a.header.uuid == b.header.uuid;
  size_t common = 0;
  while (common < std::min(a.ops.size(), b.ops.size()) && same_op(a.ops[common], b.ops[common])) ++common;
  std::string relation = "unrelated";
  if (a.header.uuid == b.header.uuid) {
    const Relation r = opad::relation(Manifest::of(a), b);
    relation = r == Relation::same ? "same" : r == Relation::extends ? "descendant" : opad::relation(Manifest::of(b), a) == Relation::extends ? "ancestor" : "diverged";
  }
  j["relation"] = relation;
  j["common_ops"] = common;
  j["op_count"] = {{"a", a.ops.size()}, {"b", b.ops.size()}};

  json changes = json::array();
  if (sa.units != sb.units) changes.push_back({{"kind", "units"}, {"change", "edited"}, {"before", sa.units}, {"after", sb.units}});
  diff_params(d, changes);
  diff_sketches(d, changes);
  diff_features(d, changes);
  diff_assets(effective_ops(a), effective_ops(b), changes);
  std::vector<size_t> geometry;
  diff_tree(d, changes, geometry);
  diff_notes(d, changes);
  diff_named("section", sa.sections, sb.sections, [](const SectionPlane& x, const SectionPlane& y) { return x.origin == y.origin && x.normal == y.normal && x.enabled == y.enabled; }, changes);
  diff_named("view", sa.views, sb.views, [](const ViewBookmark& x, const ViewBookmark& y) { return x.camera == y.camera; }, changes);
  if (opt.metrics && !geometry.empty()) {
    std::vector<json> before(geometry.size()), after(geometry.size());
    OSD_Parallel::For(0, int(geometry.size()), [&](int i) {
      const json& c = changes[geometry[size_t(i)]];
      before[size_t(i)] = body_metrics(a, c["key_before"].get<std::string>());
      after[size_t(i)] = body_metrics(b, c["key_after"].get<std::string>());
    });
    for (size_t i = 0; i < geometry.size(); ++i) changes[geometry[i]]["metrics"] = {{"before", before[i]}, {"after", after[i]}};
  }
  json counts = json::object();
  for (const auto& c : changes) {
    json& n = counts[str(c, "kind")][str(c, "change")];
    n = n.is_number() ? n.get<int>() + 1 : 1;
  }
  j["summary"] = summary_line(changes, relation);
  j["changes"] = std::move(changes);
  j["counts"] = std::move(counts);

  // The op-level view of the first diff.
  std::unordered_map<std::string, const Op*> am, bm;
  for (const auto& o : a.ops) am[o.id] = &o;
  for (const auto& o : b.ops) bm[o.id] = &o;
  json added = json::array(), removed = json::array(), changed = json::array();
  for (const auto& o : b.ops)
    if (!am.count(o.id)) added.push_back(op_summary(o));
  for (const auto& o : a.ops) {
    const auto it = bm.find(o.id);
    if (it == bm.end()) removed.push_back(op_summary(o));
    else if (!same_op(o, *it->second)) {
      json c = op_summary(o);
      if (o.raw.size() + it->second->raw.size() < 65536) {
        c["before"] = o.data;
        c["after"] = it->second->data;
      } else c["large"] = true;
      changed.push_back(std::move(c));
    }
  }
  j["ops"] = {{"added", std::move(added)}, {"removed", std::move(removed)}, {"changed", std::move(changed)}};
  json badd = json::array(), brem = json::array();
  for (const auto& e : b.bodies())
    if (!a.has_body(e.key)) badd.push_back(e.key);
  for (const auto& e : a.bodies())
    if (!b.has_body(e.key)) brem.push_back(e.key);
  j["bodies"] = {{"added", std::move(badd)}, {"removed", std::move(brem)}};

  // Geometry counts: a body is moved when only its world placement differs, changed when its geometry does.
  auto signature = [](const Scene& s, const std::string& id) {
    std::string t = s.node(id)->body_key + "|";
    for (double v : s.world(id).m) t += num(v) + ",";
    return t;
  };
  std::multiset<std::string> sig_a, sig_b;
  for (const auto& id : sa.all_bodies()) sig_a.insert(signature(sa, id));
  for (const auto& id : sb.all_bodies()) sig_b.insert(signature(sb, id));
  int geom_added = 0, geom_removed = 0, geom_moved = 0, geom_changed = 0;
  for (const auto& s : sig_b) geom_added += !sig_a.count(s);
  for (const auto& s : sig_a) geom_removed += !sig_b.count(s);
  for (const auto& id : sb.all_bodies()) {
    const Node* x = sa.node(id);
    if (!x || x->kind != Node::Kind::Body) continue;
    if (x->body_key != sb.node(id)->body_key) ++geom_changed;
    else if (!same_matrix(sa.world(id), sb.world(id))) ++geom_moved;
  }
  j["geometry"] = {{"added_or_moved", geom_added}, {"removed_or_moved", geom_removed}, {"moved", geom_moved}, {"changed", geom_changed}};
  return j;
}

std::string diff_text(const json& d) {
  std::string out = "OPAD diff: " + str(d, "a") + " -> " + str(d, "b") + "\n";
  const std::string relation = str(d, "relation");
  const size_t common = d.value("common_ops", size_t(0));
  const json counts = d.value("op_count", json::object());
  const size_t na = counts.value("a", size_t(0)), nb = counts.value("b", size_t(0));
  if (relation == "unrelated") out += "Different documents.\n";
  else if (relation == "same") out += "Same history (" + std::to_string(na) + " ops).\n";
  else if (relation == "descendant") out += "b continues a: " + std::to_string(na) + " ops in common, " + std::to_string(nb - na) + " new.\n";
  else if (relation == "ancestor") out += "b is an earlier version of a: " + std::to_string(nb) + " ops in common, " + std::to_string(na - nb) + " fewer.\n";
  else out += "Diverged after " + std::to_string(common) + " common ops (a has " + std::to_string(na - std::min(na, common)) + " more, b " + std::to_string(nb - std::min(nb, common)) + ").\n";
  const json& changes = d.contains("changes") ? d["changes"] : json::array();
  if (changes.empty()) out += "No changes.\n";
  static const std::vector<std::pair<const char*, std::vector<std::string>>> groups = {
      {"Document", {"units"}}, {"Parameters", {"param"}}, {"Sketches", {"sketch"}}, {"Features", {"feature"}}, {"Assets", {"asset"}},
      {"Bodies", {"component", "body"}}, {"Notes", {"annotation", "measurement"}}, {"Sections", {"section"}}, {"Views", {"view"}}};
  for (const auto& [title, kinds] : groups) {
    std::string lines;
    for (const auto& c : changes)
      if (std::find(kinds.begin(), kinds.end(), str(c, "kind")) != kinds.end()) lines += change_line(c);
    if (!lines.empty()) out += std::string(title) + "\n" + lines;
  }
  if (d.contains("ops") && d.contains("bodies"))
    out += "Ops: " + std::to_string(d["ops"]["added"].size()) + " added, " + std::to_string(d["ops"]["removed"].size()) + " removed, " +
           std::to_string(d["ops"]["changed"].size()) + " changed; body store: " + std::to_string(d["bodies"]["added"].size()) + " added, " +
           std::to_string(d["bodies"]["removed"].size()) + " removed\n";
  return out;
}

// ---------------------------------------------------------------- textconv
std::string document_outline(const Document& doc) {
  const Scene s = resolve(doc);
  const design::ParamTable params = param_table(s);
  std::unordered_map<std::string, const Op*> by_id;
  for (const auto& o : doc.ops) by_id[o.id] = &o;
  auto op_label = [&](const std::string& id) {
    const auto it = by_id.find(id);
    if (it == by_id.end()) return id.substr(0, 8);
    const json& d = it->second->data;
    if (!str(d, "text").empty()) return it->second->type + " " + quoted(str(d, "text"));
    for (const char* k : {"name", "source"})
      if (!str(d, k).empty()) return it->second->type + " " + clip(str(d, k));
    return it->second->type + " " + id.substr(0, 8);
  };
  std::function<size_t(const json&)> count_bodies = [&](const json& nodes) {
    size_t n = 0;
    if (nodes.is_array())
      for (const auto& x : nodes) n += str(x, "type") == "body" ? 1 : count_bodies(x.value("children", json::array()));
    return n;
  };
  std::string out = "#opad " + std::to_string(doc.header.format) + " " + doc.header.uuid + " units " + doc.header.units + " created " + doc.header.created + " " + doc.header.generator + "\n";

  out += "\n## History\n";
  for (const auto& o : doc.ops) {
    const json& d = o.data;
    const std::string target = str(d, "target");
    std::string detail;
    if (o.type == "import") detail = clip(str(d, "source")) + " (" + std::to_string(count_bodies(d.value("nodes", json::array()))) + " bodies)" + (d.contains("asset") ? " linked" : "");
    else if (o.type == "reparent") detail = name_of(s, target) + " into " + (str(d, "parent").empty() ? std::string("the root") : name_of(s, str(d, "parent")));
    else if (o.type == "transform") detail = name_of(s, target) + " to " + vec_text(translation_of(Mat4::from_json(d["matrix"])));
    else if (o.type == "appearance") {
      std::vector<std::string> parts;
      if (d.contains("color") && d["color"].is_array() && d["color"].size() == 3) parts.push_back("colour " + hex_color({d["color"][0].get<double>(), d["color"][1].get<double>(), d["color"][2].get<double>()}));
      if (d.contains("opacity") && d["opacity"].is_number()) parts.push_back("opacity " + num(d["opacity"].get<double>()));
      if (d.contains("visible") && d["visible"].is_boolean()) parts.push_back(d["visible"].get<bool>() ? "shown" : "hidden");
      if (d.contains("locked") && d["locked"].is_boolean()) parts.push_back(d["locked"].get<bool>() ? "locked" : "unlocked");
      detail = name_of(s, target) + ": " + join(parts, ", ");
    } else if (o.type == "rename") detail = "to " + quoted(str(d, "name")) + " (" + target.substr(0, 8) + ")";
    else if (o.type == "annotation") detail = (d.contains("reply_to") ? "reply " : "") + quoted(str(d, "text")) + " on " + ref_text(d.value("anchor", json()), s);
    else if (o.type == "measurement") detail = str(d, "kind") + " on " + refs_text(d.value("refs", json::array()), s);
    else if (o.type == "section" || o.type == "view" || o.type == "sketch") detail = clip(str(d, "name"));
    else if (o.type == "delete") detail = op_label(target);
    else if (o.type == "param") detail = str(d, "name") + " = " + str(d, "expr");
    else if (o.type == "feature") detail = clip(str(d, "name")) + " (" + str(d, "kind") + ")";
    else if (o.type == "edit") {
      std::vector<std::string> keys;
      for (const auto& [k, v] : d["set"].items()) keys.push_back(k);
      detail = op_label(target) + ": " + join(keys, ", ");
    } else if (o.type == "regen") detail = std::to_string(d["results"].size()) + " results";
    else if (o.type == "units") detail = str(d, "length");
    out += str(d, "ts") + "  " + str(d, "by") + "  " + o.type + (detail.empty() ? "" : "  " + detail) + "\n";
  }

  if (!s.params.empty()) {
    out += "\n## Parameters\n";
    for (const auto& p : s.params) out += p.name + " = " + param_text(p) + (p.error.empty() ? "" : "  error: " + clip(p.error, 100)) + "\n";
  }
  if (!s.sketches.empty()) {
    out += "\n## Sketches\n";
    for (const auto& k : s.sketches) {
      std::map<std::string, int> types;
      for (const auto& e : k.geometry.value("entities", json::array())) ++types[str(e, "type")];
      std::vector<std::string> parts{std::to_string(k.geometry.value("points", json::array()).size()) + " points"};
      for (const auto& [t, n] : types) parts.push_back(std::to_string(n) + " " + t + (n > 1 ? "s" : ""));
      parts.push_back(std::to_string(k.geometry.value("constraints", json::array()).size()) + " constraints");
      if (k.dof >= 0) parts.push_back("dof " + std::to_string(k.dof));
      out += k.name + "  on " + ref_text(k.plane, s) + ": " + join(parts, ", ") + (k.visible ? "" : "  hidden") + (k.error.empty() ? "" : "  error: " + clip(k.error, 100)) + "\n";
      for (const auto& c : k.geometry.value("constraints", json::array()))
        if (c.contains("value")) out += "    " + str(c, "type") + " " + std::to_string(c.value("id", 0)) + " = " + dim_text(c) + (c.value("reference", false) ? " (reference)" : "") + "\n";
    }
  }
  if (!s.features.empty()) {
    out += "\n## Features\n";
    for (const auto& f : s.features) {
      out += f.name + "  (" + f.kind + ")" + (f.suppressed ? "  suppressed" : "") + (f.error.empty() ? "" : "  error: " + clip(f.error, 100)) + "\n";
      const design::FeatureSpec* spec = design::feature_spec(f.kind);
      std::set<std::string> named;
      json filled = f.inputs.is_object() ? f.inputs : json::object();  // what an input's show_if sees: defaults where unset
      if (spec)
        for (const auto& in : spec->inputs)
          if (!filled.contains(in.name) && !in.def.is_null()) filled[in.name] = in.def;
      if (spec)
        for (const auto& in : spec->inputs) {
          named.insert(in.name);
          if (f.inputs.is_object() && f.inputs.contains(in.name) && design::input_active(in, filled)) out += "    " + in.label + " = " + value_text(in.type, f.inputs[in.name], s, &params) + "\n";
        }
      if (f.inputs.is_object())
        for (const auto& [k, v] : f.inputs.items())
          if (!named.count(k)) out += "    " + k + " = " + value_text("", v, s) + "\n";
      if (!f.suppress_if.empty()) out += "    Suppress if = " + f.suppress_if + "\n";
      std::vector<std::string> bodies;
      for (const auto& b : f.result.value("bodies", json::array())) bodies.push_back(name_of(s, str(b, "id")) + " " + str(b, "key").substr(0, 12));
      if (!bodies.empty()) out += "    -> " + join(bodies, ", ") + "\n";
    }
  }

  out += "\n## Tree\n";
  std::vector<std::pair<std::string, int>> stack;
  for (auto it = s.roots.rbegin(); it != s.roots.rend(); ++it) stack.emplace_back(*it, 0);
  while (!stack.empty()) {
    const auto [id, depth] = stack.back();
    stack.pop_back();
    const Node* n = s.node(id);
    if (!n) continue;
    std::string line(size_t(depth) * 2, ' ');
    line += clip(n->name, 200) + "  [" + (n->kind == Node::Kind::Body ? "body " + n->body_key.substr(0, 12) : std::string("component")) + "]";
    if (!n->local.is_identity()) {
      Vec3 axis;
      const double deg = rotation_between(Mat4::identity(), n->local, axis);
      line += " at " + vec_text(translation_of(n->local));
      if (deg > 1e-7) line += " rot " + num(deg) + " deg about " + vec_text(axis);
    }
    if (n->kind == Node::Kind::Body && n->representation != "solid") line += " " + n->representation;
    if (n->has_color) line += " " + hex_color(n->color);
    if (n->opacity != 1.0) line += " opacity " + num(n->opacity);
    if (!n->visible) line += " hidden";
    if (n->locked) line += " locked";
    if (n->body_missing) line += " missing";
    out += line + "\n";
    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.emplace_back(*it, depth + 1);
  }

  if (!s.annotations.empty() || !s.measurements.empty()) {
    out += "\n## Notes\n";
    for (const auto& a : s.annotations) {
      out += "[" + a.style + "] " + quoted(a.text, 200) + " on " + ref_text(a.anchor.to_json(), s) + "  by " + a.by + " " + a.ts + (a.unresolved ? "  (anchor missing)" : "") + "\n";
      for (const auto& r : a.comments) out += "    reply by " + str(r, "by") + ": " + quoted(str(r, "text"), 200) + "\n";
    }
    for (const auto& m : s.measurements) {
      std::vector<std::string> refs;
      for (const auto& r : m.refs) refs.push_back(ref_text(r.to_json(), s));
      out += m.kind + " on " + join(refs, ", ") + (m.text.empty() ? "" : "  " + quoted(m.text, 200)) + "  by " + m.by + "\n";
    }
  }
  if (!s.sections.empty() || !s.views.empty()) {
    out += "\n## Sections and views\n";
    for (const auto& x : s.sections) out += "section " + x.name + " at " + vec_text(x.origin) + " normal " + vec_text(x.normal) + (x.enabled ? "" : " off") + "\n";
    for (const auto& v : s.views) out += "view " + v.name + "\n";
  }
  if (!s.unresolved.empty()) {
    out += "\n## Unresolved\n";
    for (const auto& u : s.unresolved) out += u.op_type + " " + u.op_id.substr(0, 8) + ": " + clip(u.reason, 200) + "\n";
  }
  out += "\n## Body store\n";
  for (const auto& b : doc.bodies()) out += b.key.substr(0, 12) + "  " + std::to_string(b.text().size()) + " bytes  " + clip(str(b.meta, "name"), 200) + "\n";
  return out;
}

std::string text_outline(const std::string& text, const std::string& error) {
  std::string out = "unreadable OPAD document: " + error + "\n";
  size_t pos = 0;
  bool bodies = false;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) nl = text.size();
    std::string_view line(text.data() + pos, nl - pos);
    pos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    out.append(line);
    out += '\n';
    if (line == "#bodies") bodies = true;
    if (!bodies || line.rfind("#body ", 0) != 0) continue;
    const size_t space = line.find(' ', 6);
    size_t lines = space == std::string_view::npos ? 0 : std::strtoull(std::string(line.substr(space + 1, 20)).c_str(), nullptr, 10);
    for (; lines > 0 && pos < text.size(); --lines) {  // the BREP: left out
      nl = text.find('\n', pos);
      pos = nl == std::string::npos ? text.size() : nl + 1;
    }
  }
  return out;
}

// ---------------------------------------------------------------- versions from git
namespace {
// Runs a program and returns its exit status (-1 when it did not start), with its standard output in `out` and its
// errors in `err` (through a scratch file, so nothing has to read two pipes at once).
int run_capture(const std::vector<std::string>& args, std::string& out, std::string& err) {
#ifdef _WIN32
  auto wide = [](const std::string& s) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
  };
  std::wstring command;
  for (const auto& a : args) {
    command += L"\"";
    unsigned slashes = 0;
    for (wchar_t c : wide(a)) {
      if (c == L'\\') { ++slashes; continue; }
      command.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
      slashes = 0;
      command += c;
    }
    command.append(slashes * 2, L'\\');
    command += L"\" ";
  }
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &inherit, 0)) return -1;
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  const auto scratch = std::filesystem::temp_directory_path() / ("opad-git-" + new_uuid() + ".txt");
  HANDLE errors = CreateFileW(scratch.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nul;
  startup.hStdOutput = write;
  startup.hStdError = errors;
  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(write);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  int status = -1;
  if (started) {
    char buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(read, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(process.hProcess, &code)) status = int(code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (errors != INVALID_HANDLE_VALUE && SetFilePointer(errors, 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER)
      while (err.size() < 65536 && ReadFile(errors, buf, sizeof buf, &got, nullptr) && got) err.append(buf, got);
  }
  CloseHandle(read);
  if (errors != INVALID_HANDLE_VALUE) CloseHandle(errors);
  return status;
#else
  int pipefd[2];
  if (pipe(pipefd) != 0) return -1;
  char scratch[] = "/tmp/opad-git-XXXXXX";
  const int errors = mkstemp(scratch);
  if (errors >= 0) unlink(scratch);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  if (errors >= 0) posix_spawn_file_actions_adddup2(&actions, errors, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  std::vector<std::string> text = args;
  std::vector<char*> ptrs;
  for (auto& a : text) ptrs.push_back(a.data());
  ptrs.push_back(nullptr);
  pid_t pid;
  const int error = posix_spawnp(&pid, ptrs[0], &actions, nullptr, ptrs.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);
  int status = -1;
  if (!error) {
    char buf[1 << 16];
    ssize_t got;
    while ((got = read(pipefd[0], buf, sizeof buf)) > 0 || (got < 0 && errno == EINTR))
      if (got > 0) out.append(buf, size_t(got));
    int code = 0;
    while (waitpid(pid, &code, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(code)) status = WEXITSTATUS(code);
    if (errors >= 0 && lseek(errors, 0, SEEK_SET) == 0)
      while (err.size() < 65536 && (got = read(errors, buf, sizeof buf)) > 0) err.append(buf, size_t(got));
  }
  close(pipefd[0]);
  if (errors >= 0) close(errors);
  return status;
#endif
}
}  // namespace

std::string version_text(const std::string& spec, const std::filesystem::path& other, std::filesystem::path* file) {
  if (spec.rfind("git:", 0) != 0) {
    const auto p = path_from_utf8(spec);
    if (file) *file = p;
    return read_text_file(p);
  }
  const std::string rest = spec.substr(4);
  const size_t colon = rest.find(':');
  const std::string rev = rest.substr(0, colon);
  if (rev.empty()) throw Error("git: needs a revision (git:HEAD, git:main~2, git:<hash>)");
  std::filesystem::path where = colon == std::string::npos ? other : path_from_utf8(rest.substr(colon + 1));
  if (where.empty()) throw Error(spec + ": name the document (git:REV:path, or the other side as a file)");
  std::string object, dir;
  if (colon == std::string::npos || where.is_absolute()) {  // that file, in its own repository
    const auto absolute = std::filesystem::absolute(where);
    dir = utf8(absolute.parent_path());
    object = rev + ":./" + utf8(absolute.filename());
    where = absolute;
  } else {  // a path in the repository of the working directory, as git reads REV:path
    dir = utf8(std::filesystem::current_path());
    object = rev + ":" + rest.substr(colon + 1);
  }
  if (file) *file = where;
  const char* git = std::getenv("OPAD_GIT");
  std::string out, err;
  const int status = run_capture({git && *git ? git : "git", "-C", dir, "cat-file", "blob", object}, out, err);
  if (status < 0) throw Error("git was not found: install git or set OPAD_GIT");
  if (status != 0) {
    while (!err.empty() && (err.back() == '\n' || err.back() == '\r')) err.pop_back();
    throw Error("git cat-file blob " + object + ": " + (err.empty() ? "exit " + std::to_string(status) : err));
  }
  return out;
}

Image render_diff(const Document& a, const Document& b, const RenderOptions& opt) {
  Scene sa = resolve(a), sb = resolve(b);
  auto sig_of = [](const Scene& s, const std::string& id) {
    Mat4 w = s.world(id);
    char buf[64];
    std::string t = s.node(id)->body_key + "|";
    for (double v : w.m) { std::snprintf(buf, sizeof buf, "%.6f,", v); t += buf; }
    return t;
  };
  std::multiset<std::string> in_a, in_b;
  for (const auto& id : sa.all_bodies()) in_a.insert(sig_of(sa, id));
  for (const auto& id : sb.all_bodies()) in_b.insert(sig_of(sb, id));

  std::vector<Mesh> meshes;
  meshes.reserve(sa.nodes.size() + sb.nodes.size());
  std::vector<RenderItem> items;
  int id = 1;
  auto add = [&](const Document& d, const Scene& s, const std::string& nid, std::array<float, 3> color, float opacity) {
    const Node* n = s.node(nid);
    if (!n || n->body_missing) return;
    meshes.push_back(tessellate_body(d, n->body_key, opt.tolerance));
    RenderItem it;
    it.mesh = &meshes.back();
    it.world = s.world(nid);
    it.color = color;
    it.opacity = opacity;
    it.id = id++;
    items.push_back(it);
  };
  std::multiset<std::string> drawn_common;
  for (const auto& nid : sa.all_bodies()) {
    std::string s = sig_of(sa, nid);
    if (in_b.count(s)) { add(a, sa, nid, {0.72f, 0.72f, 0.75f}, 1.0f); drawn_common.insert(s); }
    else add(a, sa, nid, {0.85f, 0.22f, 0.18f}, 0.85f);
  }
  for (const auto& nid : sb.all_bodies()) {
    std::string s = sig_of(sb, nid);
    if (!in_a.count(s)) add(b, sb, nid, {0.18f, 0.68f, 0.30f}, 0.85f);
  }
  return render_items(items, opt);
}

}  // namespace opad
