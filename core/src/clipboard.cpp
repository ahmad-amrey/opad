// Bodies and components on the clipboard (TODO 11 UI-129): see opad/clipboard.hpp.
#include "opad/clipboard.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace opad {
namespace {
bool below(const Scene& scene, const std::string& id, const std::set<std::string>& chosen) {
  for (const Node* n = scene.node(id); n && !n->parent.empty(); n = scene.node(n->parent))
    if (chosen.count(n->parent)) return true;
  return false;
}

json node_json(const Scene& scene, const Node& n, std::set<std::string>& keys) {
  json j = {{"type", n.kind == Node::Kind::Body ? "body" : "component"}, {"id", n.id}, {"name", n.name}};
  if (!n.local.is_identity()) j["transform"] = n.local.to_json();
  if (n.has_color) j["color"] = {n.color[0], n.color[1], n.color[2]};
  if (n.opacity != 1.0) j["opacity"] = n.opacity;
  if (n.kind == Node::Kind::Body) {
    j["key"] = n.body_key;
    keys.insert(n.body_key);
    if (n.representation != "solid") j["representation"] = n.representation;
    if (!n.raster.is_null()) j["raster"] = n.raster;
    return j;
  }
  json children = json::array();
  for (const auto& c : n.children)
    if (const Node* child = scene.node(c)) children.push_back(node_json(scene, *child, keys));
  j["children"] = std::move(children);
  return j;
}

// An affine placement's inverse (rotation and scale by cofactors, then the translation back).
Mat4 inverse(const Mat4& m) {
  const double a = m.at(0, 0), b = m.at(0, 1), c = m.at(0, 2), d = m.at(1, 0), e = m.at(1, 1), f = m.at(1, 2), g = m.at(2, 0), h = m.at(2, 1), i = m.at(2, 2);
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::abs(det) < 1e-300) throw Error("a component's placement cannot be inverted");
  Mat4 r;
  const double inv[9] = {e * i - f * h, c * h - b * i, b * f - c * e, f * g - d * i, a * i - c * g, c * d - a * f, d * h - e * g, b * g - a * h, a * e - b * d};
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) r.at(row, col) = inv[row * 3 + col] / det;
  for (int row = 0; row < 3; ++row) r.at(row, 3) = -(r.at(row, 0) * m.at(0, 3) + r.at(row, 1) * m.at(1, 3) + r.at(row, 2) * m.at(2, 3));
  return r;
}
}  // namespace

json copy_nodes(const Document& doc, const Scene& scene, const std::vector<std::string>& ids, size_t brep_limit) {
  std::set<std::string> chosen, done, keys;
  for (const auto& id : ids)
    if (scene.node(id)) chosen.insert(id);
  json nodes = json::array();
  for (const auto& id : ids) {
    const Node* n = scene.node(id);
    if (!n || below(scene, id, chosen) || !done.insert(id).second) continue;
    json j = node_json(scene, *n, keys);
    j["parent"] = n->parent;
    j["world"] = scene.world(id).to_json();
    nodes.push_back(std::move(j));
  }
  if (nodes.empty()) throw Error("select bodies or components to copy");
  // The entries ride along for another document while they are small enough (else they paste into this one only); a viewed
  // file's shapes have no BREP to give.
  size_t total = 0;
  for (const auto& k : keys)
    if (const BodyEntry* b = doc.body(k)) total += b->brep.size();
  json bodies = json::array();
  if (total <= brep_limit)
    for (const auto& k : keys)
      if (const BodyEntry* b = doc.body(k); b && !b->brep.empty()) bodies.push_back({{"key", k}, {"meta", b->meta}, {"brep", b->brep}});
  return {{"format", kNodesClipFormat}, {"version", 1}, {"document", doc.header.uuid}, {"nodes", std::move(nodes)}, {"bodies", std::move(bodies)}};
}

PastePlan plan_paste(const Document& doc, const Scene& scene, const json& clip, const Vec3& offset) {
  if (!clip.is_object() || clip.value("format", std::string()) != kNodesClipFormat || !clip.contains("nodes") || !clip["nodes"].is_array())
    throw Error("the clipboard holds no bodies or components");
  if (clip.value("version", 1) > 1) throw Error("the clipboard was written by a newer OPAD");
  const bool home = clip.value("document", std::string()) == doc.header.uuid;
  std::set<std::string> carried, missing;
  if (clip.contains("bodies") && clip["bodies"].is_array())
    for (const auto& b : clip["bodies"]) carried.insert(b.value("key", std::string()));
  PastePlan plan;
  std::function<json(const json&)> fresh = [&](const json& n) {
    json j = {{"type", n.value("type", "component")}, {"id", new_uuid()}, {"name", n.value("name", std::string())}};
    for (const char* key : {"transform", "color", "opacity", "representation", "raster"})
      if (n.contains(key)) j[key] = n[key];
    if (j["type"] == "body") {
      std::string key = n.value("key", std::string());
      if (!doc.has_body(key) && home)  // copied from a viewed file made editable since: its entries renamed, its node ids kept
        if (const Node* was = scene.node(n.value("id", std::string())); was && was->kind == Node::Kind::Body) key = was->body_key;
      if (!doc.has_body(key)) {
        if (!carried.count(key))
          throw Error("the clipboard does not carry these shapes: they were copied from a file opened for viewing, or were too large; paste them in the window they came from");
        if (missing.insert(key).second) plan.missing.push_back(key);
      }
      j["key"] = key;
    } else {
      j["children"] = json::array();
      if (n.contains("children"))
        for (const auto& c : n["children"]) j["children"].push_back(fresh(c));
    }
    return j;
  };
  std::vector<std::pair<std::string, json>> groups;  // by parent, in the clip's order
  const Mat4 move = Mat4::translation(offset[0], offset[1], offset[2]);
  for (const auto& n : clip["nodes"]) {
    json j = fresh(n);
    std::string parent = home ? n.value("parent", std::string()) : std::string();
    if (const Node* p = scene.node(parent); !p || p->kind != Node::Kind::Component) parent.clear();
    const Mat4 world = move * (n.contains("world") ? Mat4::from_json(n["world"]) : Mat4());
    const Mat4 local = parent.empty() ? world : inverse(scene.world(parent)) * world;
    j.erase("transform");
    if (!local.is_identity(1e-12)) j["transform"] = local.to_json();
    plan.nodes.push_back(j["id"].get<std::string>());
    auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == parent; });
    if (it == groups.end()) {
      groups.emplace_back(parent, json::array());
      it = groups.end() - 1;
    }
    it->second.push_back(std::move(j));
  }
  for (auto& [parent, nodes] : groups) {
    json op = {{"op", "import"}, {"source", "clipboard"}, {"units", "mm"}, {"nodes", std::move(nodes)}};
    if (!parent.empty()) op["parent"] = parent;
    plan.ops.push_back(std::move(op));
  }
  return plan;
}

json paste_nodes(Document& doc, const json& clip, const Vec3& offset, const std::string& author) {
  const PastePlan plan = plan_paste(doc, resolve(doc), clip, offset);
  for (const auto& key : plan.missing)
    for (const auto& b : clip["bodies"])
      if (b.value("key", std::string()) == key) {
        if (doc.add_body(b.at("brep").get<std::string>(), b.value("meta", json::object())) != key) throw Error("a body on the clipboard is damaged");
        break;
      }
  json ids = json::array();
  for (const auto& op : plan.ops) ids.push_back(doc.append(op, author).id);
  return {{"operation_ids", ids}, {"ids", plan.nodes}, {"bodies_added", plan.missing.size()}};
}

}  // namespace opad
