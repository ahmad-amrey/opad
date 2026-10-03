// Smart selection's and Del's decisions (SmartRules.hpp).
#include "SmartRules.hpp"

#include <algorithm>
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
  std::vector<std::string> bodies;
  auto add = [](std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
  };
  for (const auto& id : ids) {
    if (scene.sketch(id)) {
      add(d.tombstone, id);  // a sketch is its op
      continue;
    }
    const opad::Node* n = scene.node(id);
    if (!n) continue;
    for (const auto& b : n->kind == opad::Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id)) add(bodies, b);
  }
  if (bodies.empty()) return d;
  if (!scene.features.empty() || !scene.sketches.empty()) {
    d.remove = bodies;
    return d;
  }
  // Without a history: by the import that made them, whole or in part.
  const std::set<std::string> picked(bodies.begin(), bodies.end());
  std::map<std::string, bool> whole;  // source op -> every body it made is picked
  for (const auto& b : bodies) whole[scene.node(b)->source_op] = true;
  for (const auto& b : scene.all_bodies())
    if (const auto it = whole.find(scene.node(b)->source_op); it != whole.end() && !picked.count(b)) it->second = false;
  for (const auto& b : bodies) {
    const std::string& op = scene.node(b)->source_op;
    if (whole[op] && !op.empty()) add(d.tombstone, op);
    else d.remove.push_back(b);
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

}  // namespace smart
