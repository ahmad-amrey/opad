// Smart selection's and Del's decisions (SmartRules.hpp).
#include "SmartRules.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "opad/design/feature.hpp"
#include "opad/design/provenance.hpp"

namespace smart {

bool Candidate::group() const {
  static const std::set<std::string> groups = {"hole", "fillet", "chamfer", "boss", "pocket", "wall"};
  return groups.count(kind) > 0;
}

std::vector<std::string> Candidate::bodies() const {
  std::vector<std::string> out;
  for (const auto& r : refs)
    if (std::find(out.begin(), out.end(), r.body) == out.end()) out.push_back(r.body);
  return out;
}

std::vector<Candidate> candidates(const opad::json& related) {
  std::vector<Candidate> out;
  for (const auto& j : related.value("candidates", opad::json::array())) {
    Candidate c;
    c.kind = j.value("kind", "");
    c.op = j.value("op", "");
    c.name = j.contains("name") ? j.value("name", "") : j.value("label", "");
    c.category = j.value("category", "");
    c.featureKind = j.value("feature_kind", "");
    c.icon = j.value("icon", "");
    c.rule = j.value("rule", "");
    c.count = j.value("count", size_t(0));
    c.containsSelection = j.value("contains_selection", false);
    c.truncated = j.value("truncated", false);
    if (j.contains("params")) c.params = j["params"];
    for (const auto& r : j.value("refs", opad::json::array())) c.refs.push_back(opad::Ref::from_json(r));
    out.push_back(std::move(c));
  }
  return out;
}

bool sameRefs(std::vector<opad::Ref> a, std::vector<opad::Ref> b) {
  auto key = [](const opad::Ref& r) { return std::tie(r.body, r.kind, r.index); };
  auto order = [&](const opad::Ref& x, const opad::Ref& y) { return key(x) < key(y); };
  std::sort(a.begin(), a.end(), order);
  std::sort(b.begin(), b.end(), order);
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [&](const opad::Ref& x, const opad::Ref& y) { return key(x) == key(y); });
}

int matching(const std::vector<Candidate>& c, const std::vector<opad::Ref>& picks) {
  for (size_t i = 0; i < c.size(); ++i)
    if (c[i].kind != "body" && c[i].kind != "similar" && !c[i].truncated && sameRefs(c[i].refs, picks)) return static_cast<int>(i);
  return -1;
}

int headline(const std::vector<Candidate>& c, const std::vector<opad::Ref>& picks, size_t bodyFaces) {
  for (size_t i = 0; i < c.size(); ++i) {
    const Candidate& x = c[i];
    if (!x.containsSelection || x.kind == "body" || x.kind == "similar" || x.kind == "import" || x.refs.empty()) continue;
    if (!x.feature() && bodyFaces > 0 && x.count * 2 > bodyFaces) continue;  // most of the body: no detail of it
    if (x.count <= picks.size() || (!x.truncated && sameRefs(x.refs, picks))) continue;  // nothing to add
    return static_cast<int>(i);
  }
  return -1;
}

Deletion routeDelete(const opad::Scene& scene, const std::vector<std::string>& ids) {
  Deletion d;
  auto add = [](std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
  };
  std::vector<std::string> items;  // the picked bodies and components, none inside another
  for (const auto& id : ids) {
    if (scene.sketch(id)) add(d.tombstone, id);  // a sketch is its op
    else if (scene.node(id)) add(items, id);
  }
  auto inside = [&](const std::string& id) {
    for (const opad::Node* n = scene.node(id); n && !n->parent.empty(); n = scene.node(n->parent))
      if (std::find(items.begin(), items.end(), n->parent) != items.end()) return true;
    return false;
  };
  items.erase(std::remove_if(items.begin(), items.end(), inside), items.end());
  if (items.empty()) return d;
  if (!scene.features.empty() || !scene.sketches.empty()) {  // a component goes whole, an empty one too
    d.remove = items;
    return d;
  }
  // Without a history: by the import that made them, whole or in part. An import is whole when every body it made is
  // picked, or, when it made none (a New component), every component it made.
  std::set<std::string> covered;  // the items and all under them
  std::function<void(const std::string&)> cover = [&](const std::string& id) {
    if (!covered.insert(id).second) return;
    for (const auto& c : scene.node(id)->children) cover(c);
  };
  for (const auto& id : items) cover(id);
  std::map<std::string, bool> whole;  // source op -> all it made is picked
  std::set<std::string> madeBodies;
  for (const auto& id : covered) whole[scene.node(id)->source_op] = true;
  for (const auto& [id, n] : scene.nodes)
    if (n.kind == opad::Node::Kind::Body && whole.count(n.source_op)) madeBodies.insert(n.source_op);
  for (const auto& [id, n] : scene.nodes)
    if (const auto it = whole.find(n.source_op); it != whole.end() && !covered.count(id) && (n.kind == opad::Node::Kind::Body || !madeBodies.count(n.source_op)))
      it->second = false;
  for (const auto& id : items) {
    std::vector<std::string> made;  // the ops behind it: its bodies', and those of the components in it that made no body
    std::function<void(const std::string&)> walk = [&](const std::string& n) {
      const opad::Node* x = scene.node(n);
      if (x->kind == opad::Node::Kind::Body || !madeBodies.count(x->source_op)) add(made, x->source_op);
      for (const auto& c : x->children) walk(c);
    };
    walk(id);
    if (std::all_of(made.begin(), made.end(), [&](const std::string& op) { return !op.empty() && whole[op]; }))
      for (const auto& op : made) add(d.tombstone, op);
    else
      d.remove.push_back(id);
  }
  return d;
}

std::vector<opad::json> deletionOps(const Deletion& d, const opad::Scene& scene) {
  std::vector<opad::json> ops;
  for (const auto& op : d.tombstone) ops.push_back({{"op", "delete"}, {"target", op}});
  if (!d.remove.empty()) ops.push_back(opad::design::make_feature_op("remove", opad::design::next_name(scene, "Remove"), {{"bodies", d.remove}}));
  return ops;
}

std::vector<std::pair<std::string, std::string>> dependents(const opad::json& report, const opad::Scene& scene, const std::vector<std::string>& deleted) {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& e : report.value("errors", opad::json::array())) {
    const std::string op = e.value("op", "");
    if (op.empty() || std::find(deleted.begin(), deleted.end(), op) != deleted.end()) continue;
    const opad::Feature* f = scene.feature(op);
    const opad::SketchItem* s = scene.sketch(op);
    if ((f && !f->error.empty()) || (s && !s->error.empty())) continue;  // failing already
    if (std::none_of(out.begin(), out.end(), [&](const auto& p) { return p.first == op; }))
      out.push_back({op, e.value("name", f ? f->name : s ? s->name : op)});
  }
  return out;
}

Users usersOf(const opad::Document& doc, const std::string& op, const std::function<bool()>& cancel) {
  Users out;
  const opad::design::Plan plan = opad::design::plan_ops(doc, {{{"op", "delete"}, {"target", op}}}, false, cancel);
  opad::design::Provenance provenance(doc, cancel);
  const opad::Scene& scene = provenance.scene();
  out.ops = dependents(plan.report, scene, {op});
  std::set<std::string> ops;
  std::vector<std::string> bodies;
  for (const auto& [id, name] : out.ops) {
    ops.insert(id);
    const opad::Feature* f = scene.feature(id);
    if (!f || !f->result.is_object()) continue;
    for (const auto& b : f->result.value("bodies", opad::json::array()))
      if (const std::string node = b.is_object() ? b.value("id", "") : ""; scene.node(node) && std::find(bodies.begin(), bodies.end(), node) == bodies.end()) bodies.push_back(node);
  }
  for (const auto& b : bodies) {
    const auto owners = provenance.face_owners(b);
    for (size_t i = 0; i < owners.size(); ++i)
      if (ops.count(owners[i].op)) out.faces.push_back(opad::Ref::parse(b + "/face/" + std::to_string(i)));
  }
  return out;
}

std::map<std::string, Made> madeBy(const opad::Document& doc, const std::function<bool()>& cancel) {
  std::map<std::string, Made> out;
  opad::design::Provenance provenance(doc, cancel);
  const opad::Scene& scene = provenance.scene();
  for (const auto& id : scene.all_bodies())
    if (const opad::Node* n = scene.node(id); !scene.feature(n->source_op)) out[n->source_op].bodies.push_back(id);  // imports
  std::vector<std::string> touched;
  for (const auto& f : scene.features) {
    if (f.suppressed || !f.result.is_object()) continue;
    Made& m = out[f.id];
    bool own = false;
    for (const auto& b : f.result.value("bodies", opad::json::array())) {
      const std::string id = b.is_object() ? b.value("id", "") : "";
      const opad::Node* n = scene.node(id);
      if (!n || n->kind != opad::Node::Kind::Body || std::count(m.bodies.begin(), m.bodies.end(), id)) continue;
      m.bodies.push_back(id);
      own = own || n->source_op == f.id;
      if (!std::count(touched.begin(), touched.end(), id)) touched.push_back(id);
    }
    m.changes = !m.bodies.empty() && !own;
  }
  for (const auto& b : touched) {
    if (cancel && cancel()) return {};
    const auto owners = provenance.face_owners(b);
    for (size_t i = 0; i < owners.size(); ++i) {
      if (!scene.feature(owners[i].op)) continue;
      Made& m = out[owners[i].op];  // also a feature whose faces a later one carried onto another body (a combine)
      m.faces.push_back(opad::Ref::parse(b + "/face/" + std::to_string(i)));
      if (!std::count(m.bodies.begin(), m.bodies.end(), b)) m.bodies.push_back(b);
    }
  }
  out.erase(std::string());
  return out;
}

}  // namespace smart
