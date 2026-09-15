#include "opad/scene.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace opad {

const Node* Scene::node(const std::string& id) const {
  auto it = nodes.find(id);
  return it == nodes.end() ? nullptr : &it->second;
}

Mat4 Scene::world(const std::string& id) const {
  const Node* n = node(id);
  if (!n) return Mat4::identity();
  if (n->parent.empty()) return n->local;
  return world(n->parent) * n->local;
}

bool Scene::effectively_visible(const std::string& id) const {
  const Node* n = node(id);
  while (n) {
    if (!n->visible) return false;
    if (n->parent.empty()) return true;
    n = node(n->parent);
  }
  return false;
}

std::vector<std::string> Scene::bodies_under(const std::string& id) const {
  std::vector<std::string> out;
  std::function<void(const std::string&)> rec = [&](const std::string& nid) {
    const Node* n = node(nid);
    if (!n) return;
    if (n->kind == Node::Kind::Body) out.push_back(nid);
    for (const auto& c : n->children) rec(c);
  };
  rec(id);
  return out;
}

std::vector<std::string> Scene::all_bodies() const {
  std::vector<std::string> out;
  for (const auto& r : roots) {
    auto b = bodies_under(r);
    out.insert(out.end(), b.begin(), b.end());
  }
  return out;
}

std::vector<std::string> Scene::path_to(const std::string& id) const {
  std::vector<std::string> p;
  const Node* n = node(id);
  while (n) {
    p.push_back(n->id);
    if (n->parent.empty()) break;
    n = node(n->parent);
  }
  std::reverse(p.begin(), p.end());
  return p;
}

json Scene::tree_json(int max_depth) const {
  std::function<json(const std::string&, int)> rec = [&](const std::string& id, int depth) {
    const Node* n = node(id);
    json j;
    if (!n) return j;
    j["id"] = n->id;
    j["type"] = n->kind == Node::Kind::Body ? "body" : "component";
    j["name"] = n->name;
    if (n->kind == Node::Kind::Body) {
      j["key"] = n->body_key;
      auto it = instance_count.find(n->body_key);
      j["instances"] = it == instance_count.end() ? 1 : it->second;
      if (n->body_missing) j["missing"] = true;
    }
    if (!n->local.is_identity()) j["transform"] = n->local.to_json();
    if (n->has_color) j["color"] = {n->color[0], n->color[1], n->color[2]};
    if (n->opacity != 1.0) j["opacity"] = n->opacity;
    if (!n->visible) j["visible"] = false;
    if (n->locked) j["locked"] = true;
    j["source_op"] = n->source_op;
    if (n->kind == Node::Kind::Component) {
      if (max_depth < 0 || depth < max_depth) {
        json ch = json::array();
        for (const auto& c : n->children) ch.push_back(rec(c, depth + 1));
        j["children"] = ch;
      } else {
        j["children_count"] = n->children.size();
      }
    }
    return j;
  };
  json out = json::array();
  for (const auto& r : roots) out.push_back(rec(r, 1));
  return out;
}

// ---------------------------------------------------------------- resolve
namespace {

struct Resolver {
  const Document& doc;
  Scene scene;

  void unresolved(const Op& op, const std::string& reason) { scene.unresolved.push_back({op.id, op.type, reason}); }

  void detach(const std::string& id) {
    Node& n = scene.nodes[id];
    auto& siblings = n.parent.empty() ? scene.roots : scene.nodes[n.parent].children;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
  }

  void attach(const std::string& id, const std::string& parent, int index) {
    Node& n = scene.nodes[id];
    n.parent = parent;
    auto& siblings = parent.empty() ? scene.roots : scene.nodes[parent].children;
    if (index < 0 || static_cast<size_t>(index) >= siblings.size()) siblings.push_back(id);
    else siblings.insert(siblings.begin() + index, id);
  }

  bool is_descendant(const std::string& id, const std::string& ancestor) {
    const Node* n = scene.node(id);
    while (n && !n->parent.empty()) {
      if (n->parent == ancestor) return true;
      n = scene.node(n->parent);
    }
    return false;
  }

  void build_nodes(const json& nodes, const std::string& parent, const Op& op) {
    for (const auto& jn : nodes) {
      Node n;
      n.id = jn["id"].get<std::string>();
      if (scene.nodes.count(n.id)) {
        unresolved(op, "duplicate node id " + n.id);
        continue;
      }
      n.kind = jn.value("type", "component") == "body" ? Node::Kind::Body : Node::Kind::Component;
      n.name = jn.value("name", n.kind == Node::Kind::Body ? "Body" : "Component");
      if (jn.contains("transform")) n.local = Mat4::from_json(jn["transform"]);
      if (jn.contains("color") && jn["color"].is_array() && jn["color"].size() == 3) {
        n.has_color = true;
        n.color = {jn["color"][0].get<double>(), jn["color"][1].get<double>(), jn["color"][2].get<double>()};
      }
      n.opacity = jn.value("opacity", 1.0);
      n.visible = jn.value("visible", true);
      n.source_op = op.id;
      if (n.kind == Node::Kind::Body) {
        n.body_key = jn.value("key", "");
        const BodyEntry* b = doc.body(n.body_key);
        if (!b) {
          n.body_missing = true;
          unresolved(op, "body entry " + n.body_key.substr(0, 12) + "... is missing from the body store");
        } else if (!n.has_color && b->meta.contains("color") && b->meta["color"].is_array()) {
          n.has_color = true;
          const auto& c = b->meta["color"];
          n.color = {c[0].get<double>(), c[1].get<double>(), c[2].get<double>()};
        }
        scene.instance_count[n.body_key]++;
      }
      scene.nodes[n.id] = n;
      attach(n.id, parent, -1);
      if (n.kind == Node::Kind::Component && jn.contains("children")) build_nodes(jn["children"], n.id, op);
    }
  }

  Node* target_of(const Op& op) {
    std::string t = op.data.value("target", "");
    auto it = scene.nodes.find(t);
    if (it == scene.nodes.end()) {
      unresolved(op, "target node " + t + " does not exist");
      return nullptr;
    }
    it->second.modified_by.push_back(op.id);
    return &it->second;
  }

  bool ref_ok(const Ref& r) { return r.kind == Ref::Kind::Point || scene.nodes.count(r.body) > 0; }

  void apply(const Op& op) {
    const json& d = op.data;
    if (op.type == "import") {
      std::string parent;
      if (d.contains("parent") && d["parent"].is_string()) {
        parent = d["parent"].get<std::string>();
        if (!scene.nodes.count(parent)) {
          unresolved(op, "import parent " + parent + " does not exist; attached to root");
          parent.clear();
        }
      }
      build_nodes(d.value("nodes", json::array()), parent, op);
    } else if (op.type == "reparent") {
      Node* n = target_of(op);
      if (!n) return;
      std::string parent = d["parent"].is_null() ? "" : d["parent"].get<std::string>();
      if (!parent.empty()) {
        const Node* p = scene.node(parent);
        if (!p) return unresolved(op, "parent node " + parent + " does not exist");
        if (p->kind != Node::Kind::Component) return unresolved(op, "parent " + parent + " is not a component");
        if (parent == n->id || is_descendant(parent, n->id)) return unresolved(op, "reparent would create a cycle");
      }
      std::string id = n->id;
      detach(id);
      attach(id, parent, d.value("index", -1));
    } else if (op.type == "transform") {
      if (Node* n = target_of(op)) n->local = Mat4::from_json(d["matrix"]);
    } else if (op.type == "appearance") {
      Node* n = target_of(op);
      if (!n) return;
      if (d.contains("color") && d["color"].is_array()) {
        n->has_color = true;
        n->color = {d["color"][0].get<double>(), d["color"][1].get<double>(), d["color"][2].get<double>()};
      }
      if (d.contains("opacity")) n->opacity = d["opacity"].get<double>();
      if (d.contains("visible")) n->visible = d["visible"].get<bool>();
      if (d.contains("locked")) n->locked = d["locked"].get<bool>();
    } else if (op.type == "rename") {
      if (Node* n = target_of(op)) n->name = d["name"].get<std::string>();
    } else if (op.type == "annotation") {
      Annotation a;
      a.id = op.id;
      a.anchor = Ref::from_json(d["anchor"]);
      a.text = d["text"].get<std::string>();
      a.by = d.value("by", "");
      a.ts = d.value("ts", "");
      if (!ref_ok(a.anchor)) {
        a.unresolved = true;
        unresolved(op, "annotation anchor body " + a.anchor.body + " does not exist");
      }
      scene.annotations.push_back(a);
    } else if (op.type == "measurement") {
      Measurement m;
      m.id = op.id;
      m.kind = d["kind"].get<std::string>();
      for (const auto& r : d["refs"]) m.refs.push_back(Ref::from_json(r));
      m.result = d.value("result", json::object());
      m.by = d.value("by", "");
      m.ts = d.value("ts", "");
      for (const auto& r : m.refs)
        if (!ref_ok(r)) {
          m.unresolved = true;
          unresolved(op, "measurement reference body " + r.body + " does not exist");
        }
      scene.measurements.push_back(m);
    } else if (op.type == "section") {
      SectionPlane s;
      s.id = op.id;
      s.name = d["name"].get<std::string>();
      s.origin = {d["origin"][0].get<double>(), d["origin"][1].get<double>(), d["origin"][2].get<double>()};
      s.normal = {d["normal"][0].get<double>(), d["normal"][1].get<double>(), d["normal"][2].get<double>()};
      s.enabled = d.value("enabled", true);
      scene.sections.push_back(s);
    } else if (op.type == "view") {
      ViewBookmark v;
      v.id = op.id;
      v.name = d["name"].get<std::string>();
      v.camera = d["camera"];
      scene.views.push_back(v);
    }
  }
};

}  // namespace

Scene resolve(const Document& doc) {
  Resolver r{doc, {}};

  // Tombstones: a delete op removes its target; a delete of a delete revives it. Iterate to a fixpoint.
  std::set<std::string> deleted;
  for (int iter = 0; iter < 16; ++iter) {
    std::set<std::string> next;
    for (const auto& op : doc.ops)
      if (op.type == "delete" && !deleted.count(op.id)) next.insert(op.data["target"].get<std::string>());
    if (next == deleted) break;
    deleted = std::move(next);
  }
  r.scene.deleted_ops.assign(deleted.begin(), deleted.end());

  for (const auto& op : doc.ops) {
    if (deleted.count(op.id)) continue;
    if (op.type == "delete") {
      if (!doc.find_op(op.data["target"].get<std::string>()))
        r.unresolved(op, "delete target op " + op.data["target"].get<std::string>() + " does not exist");
      continue;
    }
    try {
      r.apply(op);
    } catch (const std::exception& e) {
      r.unresolved(op, std::string("failed to apply: ") + e.what());
    }
  }
  return std::move(r.scene);
}

}  // namespace opad
