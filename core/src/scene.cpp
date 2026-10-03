#include "opad/scene.hpp"

#include <algorithm>
#include <functional>
#include <cmath>
#include <set>

#include "opad/design/expr.hpp"
#include "opad/design/sketch.hpp"

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

const SketchItem* Scene::sketch(const std::string& id) const {
  for (const auto& s : sketches)
    if (s.id == id) return &s;
  return nullptr;
}

const Feature* Scene::feature(const std::string& id) const {
  for (const auto& f : features)
    if (f.id == id) return &f;
  return nullptr;
}

const Param* Scene::param(const std::string& name) const {
  for (auto it = params.rbegin(); it != params.rend(); ++it)
    if (it->name == name) return &*it;
  return nullptr;
}

// ---------------------------------------------------------------- Frame
static Vec3 vec3_from(const json& j, const Vec3& fallback) {
  if (!j.is_array() || j.size() != 3) return fallback;
  return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

Vec3 Frame::normal() const { return {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]}; }

Vec3 Frame::to_world(double u, double v) const {
  return {origin[0] + u * x[0] + v * y[0], origin[1] + u * x[1] + v * y[1], origin[2] + u * x[2] + v * y[2]};
}

void Frame::to_local(const Vec3& p, double& u, double& v) const {
  const Vec3 d{p[0] - origin[0], p[1] - origin[1], p[2] - origin[2]};
  u = d[0] * x[0] + d[1] * x[1] + d[2] * x[2];
  v = d[0] * y[0] + d[1] * y[1] + d[2] * y[2];
}

json Frame::to_json() const { return {{"origin", origin}, {"x", x}, {"y", y}}; }

Frame Frame::from_json(const json& j) {
  Frame f;
  if (!j.is_object()) return f;
  f.origin = vec3_from(j.value("origin", json()), f.origin);
  f.x = vec3_from(j.value("x", json()), f.x);
  f.y = vec3_from(j.value("y", json()), f.y);
  return f;
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
      j["representation"] = n->representation;
      if(!n->raster.is_null()) j["raster"] = n->raster;
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
struct SceneBuilder::Impl {
  const Document& doc;
  Scene scene;
  std::set<std::string> shown_sketches, hidden_sketches;  // explicit appearance ops on sketches

  explicit Impl(const Document& d) : doc(d) {scene.units=d.header.units;}

  void unresolved(const std::string& id, const std::string& type, const std::string& reason) { scene.unresolved.push_back({id, type, reason}); }

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

  void drop_instance(const std::string& key) {
    auto it = scene.instance_count.find(key);
    if (it != scene.instance_count.end() && --it->second <= 0) scene.instance_count.erase(it);
  }

  void set_key(Node& n, const std::string& key, const std::string& op_id, const std::string& op_type) {
    if (!n.body_key.empty()) drop_instance(n.body_key);
    n.body_key = key;
    n.body_missing = !doc.has_body(key);
    if (n.body_missing) unresolved(op_id, op_type, "body entry " + key.substr(0, 12) + "... is missing from the body store");
    scene.instance_count[key]++;
  }

  void build_nodes(const json& nodes, const std::string& parent, const std::string& op_id, const std::string& op_type) {
    for (const auto& jn : nodes) {
      Node n;
      n.id = jn["id"].get<std::string>();
      if (scene.nodes.count(n.id)) {
        unresolved(op_id, op_type, "duplicate node id " + n.id);
        continue;
      }
      n.kind = jn.value("type", "component") == "body" ? Node::Kind::Body : Node::Kind::Component;
      n.name = jn.value("name", n.kind == Node::Kind::Body ? "Body" : "Component");
      n.representation = jn.value("representation", "solid");
      n.raster = jn.value("raster", json());
      if (jn.contains("transform")) n.local = Mat4::from_json(jn["transform"]);
      if (jn.contains("color") && jn["color"].is_array() && jn["color"].size() == 3) {
        n.has_color = true;
        n.color = {jn["color"][0].get<double>(), jn["color"][1].get<double>(), jn["color"][2].get<double>()};
      }
      n.opacity = jn.value("opacity", 1.0);
      n.visible = jn.value("visible", true);
      n.source_op = op_id;
      const std::string nid = n.id;
      const bool body = n.kind == Node::Kind::Body;
      scene.nodes[nid] = std::move(n);
      if (body) {
        Node& placed = scene.nodes[nid];
        set_key(placed, jn.value("key", ""), op_id, op_type);
        const BodyEntry* b = doc.body(placed.body_key);
        if (b && !placed.has_color && b->meta.contains("color") && b->meta["color"].is_array()) {
          placed.has_color = true;
          const auto& c = b->meta["color"];
          placed.color = {c[0].get<double>(), c[1].get<double>(), c[2].get<double>()};
        }
      }
      attach(nid, parent, -1);
      if (!body && jn.contains("children")) build_nodes(jn["children"], nid, op_id, op_type);
    }
  }

  void remove_node(const std::string& id) {
    auto it = scene.nodes.find(id);
    if (it == scene.nodes.end()) return;
    const std::vector<std::string> children = it->second.children;
    for (const auto& c : children) remove_node(c);
    it = scene.nodes.find(id);
    if (it->second.kind == Node::Kind::Body) drop_instance(it->second.body_key);
    detach(id);
    scene.nodes.erase(id);
  }

  Node* target_of(const std::string& id, const std::string& type, const json& d) {
    std::string t = d.value("target", "");
    auto it = scene.nodes.find(t);
    if (it == scene.nodes.end()) {
      unresolved(id, type, "target node " + t + " does not exist");
      return nullptr;
    }
    it->second.modified_by.push_back(id);
    return &it->second;
  }

  SketchItem* sketch_of(const std::string& id) {
    for (auto& s : scene.sketches)
      if (s.id == id) return &s;
    return nullptr;
  }

  bool ref_ok(const Ref& r) { return r.kind == Ref::Kind::Point || scene.nodes.count(r.body) > 0; }

  // A sketch's or feature's optional "component" (TODO 11 UI-33), while that component exists; else the root, as a
  // feature's new body whose parent is gone.
  std::string component_of(const json& d) {
    if (!d.contains("component") || !d["component"].is_string()) return {};
    const Node* n = scene.node(d["component"].get<std::string>());
    return n && n->kind == Node::Kind::Component ? n->id : std::string();
  }

  // Marks every sketch a feature's inputs mention ({"sketch": id} anywhere in them) as consumed.
  void mark_consumed(const json& j) {
    if (j.is_object()) {
      if (j.contains("sketch") && j["sketch"].is_string())
        if (SketchItem* s = sketch_of(j["sketch"].get<std::string>())) s->consumed = true;
      for (const auto& [k, v] : j.items()) mark_consumed(v);
    } else if (j.is_array()) {
      for (const auto& v : j) mark_consumed(v);
    }
  }

  void apply_feature(const std::string& id, const json& d) {
    Feature f;
    f.id = id;
    f.kind = d.value("kind", "");
    f.name = d.value("name", f.kind);
    f.component = component_of(d);
    f.inputs = d.value("inputs", json::object());
    f.result = d.value("result", json::object());
    // suppress_if (gap log #9): the walk evaluated it and kept the answer in the result; replay only reads that.
    const bool conditional = d.contains("suppress_if") && d["suppress_if"].is_string();
    if (conditional) f.suppress_if = d["suppress_if"].get<std::string>();
    f.suppressed = d.value("suppressed", false) || (conditional && f.result.value("suppressed", false));
    f.error = f.result.value("error", "");
    if (!f.suppressed) {
      if (!f.error.empty()) unresolved(id, "feature", f.name + ": " + f.error);
      mark_consumed(f.inputs);
      for (const auto& rid : f.result.value("removed", json::array())) remove_node(rid.get<std::string>());
      for (const auto& b : f.result.value("bodies", json::array())) {
        const std::string nid = b.value("id", "");
        const std::string key = b.value("key", "");
        auto it = scene.nodes.find(nid);
        if (it != scene.nodes.end()) {  // the feature changed an existing body: same node, new geometry
          if (it->second.kind != Node::Kind::Body) {
            unresolved(id, "feature", "node " + nid + " is not a body");
            continue;
          }
          set_key(it->second, key, id, "feature");
          it->second.modified_by.push_back(id);
          continue;
        }
        std::string parent = b.value("parent", "");
        if (!parent.empty() && !scene.nodes.count(parent)) parent.clear();
        Node n;
        n.id = nid;
        n.kind = Node::Kind::Body;
        n.name = b.value("name", "Body");
        n.source_op = id;
        if (b.contains("transform")) n.local = Mat4::from_json(b["transform"]);
        if (b.contains("color") && b["color"].is_array() && b["color"].size() == 3) {
          n.has_color = true;
          n.color = {b["color"][0].get<double>(), b["color"][1].get<double>(), b["color"][2].get<double>()};
        }
        scene.nodes[nid] = std::move(n);
        set_key(scene.nodes[nid], key, id, "feature");
        attach(nid, parent, -1);
      }
    }
    scene.features.push_back(std::move(f));
  }

  void apply(const std::string& id, const std::string& type, const json& d) {
    if(type=="units") {scene.units=d.at("length").get<std::string>();
    } else if (type == "import") {
      std::string parent;
      if (d.contains("parent") && d["parent"].is_string()) {
        parent = d["parent"].get<std::string>();
        if (!scene.nodes.count(parent)) {
          unresolved(id, type, "import parent " + parent + " does not exist; attached to root");
          parent.clear();
        }
      }
      build_nodes(d.value("nodes", json::array()), parent, id, type);
    } else if (type == "reparent") {
      Node* n = target_of(id, type, d);
      if (!n) return;
      std::string parent = d["parent"].is_null() ? "" : d["parent"].get<std::string>();
      if (!parent.empty()) {
        const Node* p = scene.node(parent);
        if (!p) return unresolved(id, type, "parent node " + parent + " does not exist");
        if (p->kind != Node::Kind::Component) return unresolved(id, type, "parent " + parent + " is not a component");
        if (parent == n->id || is_descendant(parent, n->id)) return unresolved(id, type, "reparent would create a cycle");
      }
      std::string nid = n->id;
      detach(nid);
      attach(nid, parent, d.value("index", -1));
    } else if (type == "transform") {
      if (Node* n = target_of(id, type, d)) n->local = Mat4::from_json(d["matrix"]);
    } else if (type == "appearance") {
      if (SketchItem* s = sketch_of(d.value("target", ""))) {  // a sketch only has a visibility
        if (d.contains("visible")) {
          const bool on = d["visible"].get<bool>();
          (on ? shown_sketches : hidden_sketches).insert(s->id);
          (on ? hidden_sketches : shown_sketches).erase(s->id);
        }
        return;
      }
      Node* n = target_of(id, type, d);
      if (!n) return;
      if (d.contains("color") && d["color"].is_array()) {
        n->has_color = true;
        n->color = {d["color"][0].get<double>(), d["color"][1].get<double>(), d["color"][2].get<double>()};
      }
      if (d.contains("opacity")) n->opacity = d["opacity"].get<double>();
      if (d.contains("visible")) n->visible = d["visible"].get<bool>();
      if (d.contains("locked")) n->locked = d["locked"].get<bool>();
    } else if (type == "rename") {
      if (Node* n = target_of(id, type, d)) n->name = d["name"].get<std::string>();
    } else if (type == "annotation") {
      Document::validate_op(d);  // edits loaded from disk must meet the same drawing bounds
      Annotation a;
      a.reply_to = d.value("reply_to", "");
      a.id = id;
      a.anchor = Ref::from_json(d["anchor"]);
      a.text = d["text"].get<std::string>();
      a.drawing = d.value("drawing", json());
      a.by = d.value("by", "");
      a.ts = d.value("ts", "");
      if (d.contains("style") && d["style"].is_string()) {
        const auto& styles = annotation_styles();
        if (std::find(styles.begin(), styles.end(), d["style"].get<std::string>()) != styles.end()) a.style = d["style"].get<std::string>();
      }
      if (!ref_ok(a.anchor)) {
        a.unresolved = true;
        unresolved(id, type, "annotation anchor body " + a.anchor.body + " does not exist");
      }
      scene.annotations.push_back(a);
    } else if (type == "measurement") {
      Measurement m;
      m.text=d.value("text", ""); m.style=d.value("style", "note");
      m.id = id;
      m.kind = d["kind"].get<std::string>();
      for (const auto& r : d["refs"]) m.refs.push_back(Ref::from_json(r));
      m.result = d.value("result", json::object());
      m.by = d.value("by", "");
      m.ts = d.value("ts", "");
      for (const auto& r : m.refs)
        if (!ref_ok(r)) {
          m.unresolved = true;
          unresolved(id, type, "measurement reference body " + r.body + " does not exist");
        }
      scene.measurements.push_back(m);
    } else if (type == "section") {
      SectionPlane s;
      s.id = id;
      s.name = d["name"].get<std::string>();
      s.origin = {d["origin"][0].get<double>(), d["origin"][1].get<double>(), d["origin"][2].get<double>()};
      s.normal = {d["normal"][0].get<double>(), d["normal"][1].get<double>(), d["normal"][2].get<double>()};
      s.enabled = d.value("enabled", true);
      scene.sections.push_back(s);
    } else if (type == "view") {
      ViewBookmark v;
      v.id = id;
      v.name = d["name"].get<std::string>();
      v.camera = d["camera"];
      if (d.contains("explode") && d["explode"].is_object()) v.explode = d["explode"];
      scene.views.push_back(v);
    } else if (type == "param") {
      Param p;
      p.id = id;
      p.name = d.value("name", "");
      p.expr = d.value("expr", "");
      p.comment = d.value("comment", "");
      scene.params.push_back(std::move(p));
    } else if (type == "sketch") {
      SketchItem s;
      s.id = id;
      s.name = d.value("name", "Sketch");
      s.component = component_of(d);
      if (!s.component.empty()) s.placed = scene.world(s.component);
      s.plane = d.value("plane", json::object());
      s.geometry = d.value("geometry", json::object());
      s.frame = Frame::from_json(s.plane.value("frame", json()));
      // A regeneration (changed parameters, a moved face) leaves the solved state in the result.
      const json res = d.value("result", json::object());
      if (res.contains("geometry")) s.geometry = res["geometry"];
      if (res.contains("frame")) s.frame = Frame::from_json(res["frame"]);
      s.dof = res.value("dof", d.value("dof", -1));
      s.error = res.value("error", "");
      if (!s.error.empty()) unresolved(id, type, s.name + ": " + s.error);
      scene.sketches.push_back(std::move(s));
    } else if (type == "feature") {
      apply_feature(id, d);
    }
  }

  void finish() {
    // Replies are independent append-only operations, so concurrent comments merge cleanly.
    for (const auto& reply : scene.annotations) {
      if (reply.reply_to.empty()) continue;
      for (auto& parent : scene.annotations)
        if (parent.id == reply.reply_to)
          parent.comments.push_back(json{{"id", reply.id}, {"text", reply.text}, {"by", reply.by}, {"ts", reply.ts}});
      for(auto& parent:scene.measurements)
        if(parent.id==reply.reply_to)
          parent.comments.push_back(json{{"id",reply.id},{"text",reply.text},{"by",reply.by},{"ts",reply.ts}});
    }
    std::erase_if(scene.annotations, [](const Annotation& a) { return !a.reply_to.empty(); });
    std::vector<design::ParamDef> defs;
    for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    design::ParamTable table(defs);
    for (auto& p : scene.params) {
      try {
        if (table.find(p.name)->id != p.id) throw Error("another parameter is also called \"" + p.name + "\"");
        const design::Quantity q = table.value_of(p.name);
        p.value = q.value;
        p.len = q.len;
        p.angle = q.angle;
        p.shown = design::format_quantity(q);
      } catch (const std::exception& e) {
        p.error = e.what();
        unresolved(p.id, "param", p.name + ": " + p.error);
      }
    }
    for (auto& s : scene.sketches) s.visible = shown_sketches.count(s.id) ? true : hidden_sketches.count(s.id) ? false : !s.consumed;
  }
};

SceneBuilder::SceneBuilder(const Document& doc) : m(std::make_unique<Impl>(doc)) {}
SceneBuilder::~SceneBuilder() = default;
void SceneBuilder::apply(const std::string& id, const std::string& type, const json& data) { m->apply(id, type, data); }
void SceneBuilder::finish() { m->finish(); }
Scene& SceneBuilder::scene() { return m->scene; }
Scene SceneBuilder::take() { return std::move(m->scene); }

const std::vector<std::string>& annotation_styles() {
  static const std::vector<std::string> styles = {"ok", "warning", "issue", "note", "ai_agent"};
  return styles;
}

std::vector<EffectiveOp> effective_ops(const Document& doc, std::vector<std::string>* deleted_out) {
  std::vector<const Op*> ops;
  ops.reserve(doc.ops.size());
  for (const auto& op : doc.ops) ops.push_back(&op);
  return effective_ops(ops, deleted_out);
}

std::vector<EffectiveOp> effective_ops(const std::vector<const Op*>& ops, std::vector<std::string>* deleted_out) {
  // Tombstones: a delete op removes its target; a delete of a delete revives it. Iterate to a fixpoint.
  std::set<std::string> deleted;
  for (int iter = 0; iter < 16; ++iter) {
    std::set<std::string> next;
    for (const Op* op : ops)
      if (op->type == "delete" && !deleted.count(op->id)) next.insert(op->data["target"].get<std::string>());
    if (next == deleted) break;
    deleted = std::move(next);
  }
  if (deleted_out) deleted_out->assign(deleted.begin(), deleted.end());

  std::vector<EffectiveOp> out;
  std::unordered_map<std::string, size_t> at;
  for (const Op* p : ops) {
    const Op& op = *p;
    if (deleted.count(op.id) || op.type == "delete") continue;
    if (op.type == "edit") {
      auto it = at.find(op.data.value("target", ""));
      if (it == at.end()) continue;  // the target is tombstoned: nothing to edit
      json& data = out[it->second].edit();
      for (const auto& [k, v] : op.data["set"].items()) {
        if (k == "geometry_delta") {
          // Start from the last solved geometry, including parameter-driven changes.
          const json base = data.value("result", json::object()).value("geometry", data.value("geometry", json::object()));
          data["geometry"] = design::apply_sketch_delta(base, v);
          if (data.contains("result")) data["result"].erase("geometry");
        }
        else if (v.is_null()) data.erase(k);
        else data[k] = v;
      }
      continue;
    }
    if (op.type == "regen") {
      for (const auto& [target, result] : op.data["results"].items()) {
        auto it = at.find(target);
        if (it != at.end()) out[it->second].edit()["result"] = result;
      }
      continue;
    }
    at[op.id] = out.size();
    out.push_back({&op, nullptr});
  }
  return out;
}

Scene resolve(const Document& doc, const std::string& until) {
  SceneBuilder b(doc);
  std::vector<std::string> deleted;
  const std::vector<EffectiveOp> ops = effective_ops(doc, &deleted);
  b.scene().deleted_ops = deleted;
  for (const auto& op : doc.ops)
    if (op.type == "delete" && !std::binary_search(deleted.begin(), deleted.end(), op.id) && !doc.find_op(op.data["target"].get<std::string>()))
      b.scene().unresolved.push_back({op.id, op.type, "delete target op " + op.data["target"].get<std::string>() + " does not exist"});
  bool rolledBack = false;
  for (const auto& e : ops) {
    if (!until.empty() && e.op->id == until) rolledBack = true;
    // Parameters are global (the engine evaluates every feature against all of them), so a rolled-back scene keeps
    // the later ones: editing a feature can use a parameter defined after it.
    if (rolledBack && e.op->type != "param") continue;
    try {
      b.apply(e.op->id, e.op->type, e.data());
    } catch (const std::exception& ex) {
      b.scene().unresolved.push_back({e.op->id, e.op->type, std::string("failed to apply: ") + ex.what()});
    }
  }
  b.finish();
  return b.take();
}

}  // namespace opad
