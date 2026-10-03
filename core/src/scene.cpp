#include "opad/scene.hpp"

#include <algorithm>
#include <functional>
#include <cmath>
#include <set>

#include "opad/design/expr.hpp"
#include "opad/design/sketch.hpp"
#include "opad/drawing/sheet.hpp"

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

bool Scene::effectively_locked(const std::string& id) const {
  for (const Node* n = node(id); n; n = n->parent.empty() ? nullptr : node(n->parent))
    if (n->locked) return true;
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

const Sheet* Scene::sheet(const std::string& id) const {
  for (const auto& s : sheets)
    if (s.id == id) return &s;
  return nullptr;
}

const SheetView* Scene::sheet_view(const std::string& id) const {
  for (const auto& v : sheet_views)
    if (v.id == id) return &v;
  return nullptr;
}

const SheetItem* Scene::sheet_item(const std::string& id) const {
  for (const auto& t : sheet_items)
    if (t.id == id) return &t;
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

Frame Frame::transformed(const Mat4& m) const {
  auto unit = [](Vec3 v) {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l < 1e-300 ? v : Vec3{v[0] / l, v[1] / l, v[2] / l};
  };
  Frame f;
  f.origin = m.apply(origin);
  f.x = unit(m.apply_dir(x));
  const Vec3 y1 = m.apply_dir(y);
  const double along = y1[0] * f.x[0] + y1[1] * f.x[1] + y1[2] * f.x[2];
  f.y = unit({y1[0] - along * f.x[0], y1[1] - along * f.x[1], y1[2] - along * f.x[2]});
  return f;
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
    if (n->linked) j["linked"] = true;
    if (!n->local.is_identity()) j["transform"] = n->local.to_json();
    if (n->has_color) j["color"] = {n->color[0], n->color[1], n->color[2]};
    if (n->opacity != 1.0) j["opacity"] = n->opacity;
    if (!n->visible) j["visible"] = false;
    if (n->locked) j["locked"] = true;
    if (!n->properties.empty()) j["properties"] = n->properties;
    if (n->layer.is_object()) j["layer"] = n->layer;
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
  // Sketches and construction planes / axes made in a component follow its later moves (TODO 11 UI-33 phase 2). The op
  // keeps where they were made; the scene has them where the component is now, so later features read them there.
  struct Follower {
    bool sketch;
    size_t index;  // into scene.sketches or scene.features
    std::string component;
    Mat4 placed;   // the component's world placement when made
    json made;     // the frame, or the feature's result, as made
  };
  std::vector<Follower> followers;

  explicit Impl(const Document& d) : doc(d) {scene.units=d.header.units;}

  void follow() {
    for (const auto& f : followers) {
      if (!scene.node(f.component)) continue;  // removed: stays where it was last
      Mat4 moved;
      try {
        moved = scene.world(f.component) * f.placed.inverse();
      } catch (const Error&) {  // made while the component was squashed flat: nothing to follow
        continue;
      }
      if (f.sketch) {
        SketchItem& s = scene.sketches[f.index];
        s.moved = moved;
        s.frame = Frame::from_json(f.made).transformed(moved);
        continue;
      }
      json& result = scene.features[f.index].result;
      if (f.made.contains("plane")) result["plane"] = Frame::from_json(f.made["plane"]).transformed(moved).to_json();
      if (f.made.contains("axis")) {  // as a frame: its origin and x moved the same way
        Frame axis;
        axis.origin = vec3_from(f.made["axis"].value("origin", json()), axis.origin);
        axis.x = vec3_from(f.made["axis"].value("dir", json()), {0, 0, 1});
        axis.y = std::fabs(axis.x[0]) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        axis = axis.transformed(moved);
        result["axis"] = {{"origin", axis.origin}, {"dir", axis.x}};
      }
    }
  }

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
    if (n.body_missing)
      unresolved(op_id, op_type, n.linked ? "linked file " + asset_file + " is not loaded: " + n.name
                                          : "body entry " + key.substr(0, 12) + "... is missing from the body store");
    scene.instance_count[key]++;
  }

  std::string asset_file;  // the linked asset whose nodes are being built (its path, for unresolved reasons)
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
      n.locked = jn.contains("locked") && jn["locked"].is_boolean() && jn["locked"].get<bool>();  // a drawing's locked layer
      if (jn.contains("layer") && jn["layer"].is_object()) n.layer = jn["layer"];
      n.source_op = op_id;
      n.linked = !asset_file.empty();
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
        // A linked picture: its bytes come with the body read from the file, never with the op.
        if (b && b->external && placed.raster.is_object() && !placed.raster.contains("href") && b->meta.contains("href"))
          placed.raster["href"] = b->meta["href"];
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
    if (!f.component.empty() && (f.result.contains("plane") || f.result.contains("axis"))) {
      json made = json::object();
      for (const char* k : {"plane", "axis"})
        if (f.result.contains(k)) made[k] = f.result[k];
      followers.push_back({false, scene.features.size(), f.component, scene.world(f.component), made});
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
      // A linked asset (assets.hpp): its bodies are registered when the file is read, so an unread file leaves them missing.
      const json asset = d.value("asset", json());
      asset_file.clear();
      if (asset.is_object() && asset.value("storage", "linked") != "embedded")
        asset_file = asset.value("path", asset.value("abs", d.value("source", std::string("?"))));
      build_nodes(d.value("nodes", json::array()), parent, id, type);
      asset_file.clear();
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
      follow();
    } else if (type == "transform") {
      if (Node* n = target_of(id, type, d)) n->local = Mat4::from_json(d["matrix"]);
      follow();
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
      // A regeneration (changed parameters, a moved face) leaves the solved state in the result.
      s.geometry = design::solved_geometry(d);
      s.frame = Frame::from_json(s.plane.value("frame", json()));
      const json res = d.value("result", json::object());
      if (res.contains("frame")) s.frame = Frame::from_json(res["frame"]);
      s.dof = res.value("dof", d.value("dof", -1));
      s.error = res.value("error", "");
      if (!s.error.empty()) unresolved(id, type, s.name + ": " + s.error);
      if (!s.component.empty()) followers.push_back({true, scene.sketches.size(), s.component, s.placed, s.frame.to_json()});
      scene.sketches.push_back(std::move(s));
    } else if (type == "feature") {
      apply_feature(id, d);
    } else if (type == "sheet") {
      drawing::validate_record(d);  // edits loaded from disk meet the same checks
      Sheet s;
      s.id = id;
      s.name = d.value("name", "Sheet");
      s.drawing = d.value("drawing", "");
      s.width = d["size"]["w"].get<double>();
      s.height = d["size"]["h"].get<double>();
      s.standard = d.value("standard", "iso");
      s.projection = d.value("projection", s.standard == "asme" ? "third" : "first");
      s.def = d;
      scene.sheets.push_back(std::move(s));
    } else if (type == "sheet_view") {
      drawing::validate_record(d);
      SheetView v;
      v.id = id;
      v.sheet = d["sheet"].get<std::string>();
      v.parent = d.value("parent", "");
      v.name = d.value("name", "");
      v.kind = d["kind"].get<std::string>();
      v.def = d;
      scene.sheet_views.push_back(std::move(v));
    } else if (type == "sheet_item") {
      drawing::validate_record(d);
      SheetItem t;
      t.id = id;
      t.sheet = d["sheet"].get<std::string>();
      t.view = d.value("view", "");
      t.kind = d["kind"].get<std::string>();
      t.type = d.value("type", "");
      for (const auto& r : d.value("refs", json::array())) t.refs.push_back(Ref::from_json(r));
      t.def = d;
      scene.sheet_items.push_back(std::move(t));
    } else if (type == "properties") {
      if (!doc.header.uuid.empty() && d.value("target", "") == doc.header.uuid) {  // the document's own
        for (const auto& [k, v] : d["set"].items()) {
          if (v.is_null()) scene.properties.erase(k);
          else scene.properties[k] = v;
        }
        return;
      }
      Node* n = target_of(id, type, d);
      if (!n) return;
      for (const auto& [k, v] : d["set"].items()) {
        if (v.is_null()) n->properties.erase(k);
        else n->properties[k] = v;
      }
    } else if (!Document::known_type(type)) {
      unresolved(id, type, "needs a newer OPAD (op '" + type + "')");
    }
  }

  // Sheets, their views and items, linked once the whole log is read (a view may name a parent written after it). A view
  // or item whose sheet, parent view or view was deleted goes with it; one naming something that never existed, or a
  // kind this build does not know, is kept and reported.
  void link_sheets() {
    if (scene.sheets.empty() && scene.sheet_views.empty() && scene.sheet_items.empty()) return;
    std::set<std::string> gone(scene.deleted_ops.begin(), scene.deleted_ops.end());
    std::unordered_map<std::string, size_t> sheet_at, view_at;
    for (size_t i = 0; i < scene.sheets.size(); ++i) sheet_at[scene.sheets[i].id] = i;
    for (bool dropped = true; dropped;) {
      dropped = false;
      std::set<std::string> present;
      for (const auto& v : scene.sheet_views) present.insert(v.id);
      std::erase_if(scene.sheet_views, [&](const SheetView& v) {
        const bool goes = (!sheet_at.count(v.sheet) && gone.count(v.sheet)) || (!v.parent.empty() && !present.count(v.parent) && gone.count(v.parent));
        if (goes) gone.insert(v.id), dropped = true;
        return goes;
      });
    }
    for (size_t i = 0; i < scene.sheet_views.size(); ++i) view_at[scene.sheet_views[i].id] = i;
    std::erase_if(scene.sheet_items, [&](const SheetItem& t) {
      return (!sheet_at.count(t.sheet) && gone.count(t.sheet)) || (!t.view.empty() && !view_at.count(t.view) && gone.count(t.view));
    });
    const auto missing_keys = [&](const json& def) {
      std::vector<std::string> keys;
      drawing::record_body_keys(def, keys);
      for (const auto& k : keys)
        if (!doc.has_body(k)) return "body entry " + k.substr(0, 12) + "... is missing from the body store";
      return std::string();
    };
    for (auto& s : scene.sheets) {
      std::string why;
      try {
        s.scale = drawing::parse_scale(s.def.value("scale", "1:1"));
      } catch (const std::exception& e) {
        why = e.what();
      }
      const std::string units = s.def.value("units", "mm");
      if (s.standard != "iso" && s.standard != "asme") why = "needs a newer OPAD (standard '" + s.standard + "')";
      else if (s.projection != "first" && s.projection != "third") why = "needs a newer OPAD (projection '" + s.projection + "')";
      else if (units != "mm" && units != "in") why = "needs a newer OPAD (units '" + units + "')";
      if (why.empty()) why = missing_keys(s.def);
      if (!why.empty()) unresolved(s.id, "sheet", s.name + ": " + why);
    }
    for (auto& v : scene.sheet_views) {
      const auto fail = [&](const std::string& why) {
        if (!v.error.empty()) return;
        v.error = why;
        unresolved(v.id, "sheet_view", (v.name.empty() ? std::string("view") : v.name) + ": " + why);
      };
      const auto parent = v.parent.empty() ? view_at.end() : view_at.find(v.parent);
      if (!sheet_at.count(v.sheet)) fail("sheet " + v.sheet + " does not exist");
      else if (!v.parent.empty() && parent == view_at.end()) fail("parent view " + v.parent + " does not exist");
      else if (!v.parent.empty() && scene.sheet_views[parent->second].sheet != v.sheet) fail("its parent view is on another sheet");
      if (sheet_at.count(v.sheet)) scene.sheets[sheet_at[v.sheet]].views.push_back(v.id);
      if (!v.parent.empty() && parent != view_at.end()) scene.sheet_views[parent->second].children.push_back(v.id);
    }
    for (auto& v : scene.sheet_views) {  // orientation through the parents, sides, sources: no geometry walked
      if (!v.error.empty()) continue;
      std::string why;
      try {
        drawing::view_spec(scene, v);
        why = missing_keys(v.def);
      } catch (const std::exception& e) {
        why = e.what();
      }
      if (!why.empty()) {
        v.error = why;
        unresolved(v.id, "sheet_view", (v.name.empty() ? std::string("view") : v.name) + ": " + why);
      }
    }
    static const std::set<std::string> kinds = {"dimension", "note"};
    static const std::set<std::string> dimensions = {"horizontal", "vertical", "aligned", "radius", "diameter", "angle"};
    for (auto& t : scene.sheet_items) {
      const auto fail = [&](const std::string& why) {
        if (t.error.empty()) t.error = why;
        unresolved(t.id, "sheet_item", t.kind + ": " + why);
      };
      const auto view = t.view.empty() ? view_at.end() : view_at.find(t.view);
      if (!sheet_at.count(t.sheet)) fail("sheet " + t.sheet + " does not exist");
      else if (!t.view.empty() && view == view_at.end()) fail("view " + t.view + " does not exist");
      else if (!t.view.empty() && scene.sheet_views[view->second].sheet != t.sheet) fail("its view is on another sheet");
      if (!kinds.count(t.kind)) fail("needs a newer OPAD (sheet_item kind '" + t.kind + "')");
      else if (t.kind == "dimension" && !dimensions.count(t.type)) fail("needs a newer OPAD (dimension type '" + t.type + "')");
      for (const auto& r : t.refs)
        if (!ref_ok(r)) {
          t.unresolved = true;
          fail("reference body " + r.body + " does not exist");
        }
      if (const std::string why = missing_keys(t.def); !why.empty()) fail(why);
      if (sheet_at.count(t.sheet)) scene.sheets[sheet_at[t.sheet]].items.push_back(t.id);
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
    link_sheets();
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
          data["geometry"] = design::apply_sketch_delta(design::solved_geometry(data), v);
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

std::set<std::string> ops_in_component(const Document& doc, const Scene& scene, const std::string& component) {
  std::set<std::string> out;
  auto under = [&](const json& id) {
    if (!id.is_string()) return false;
    for (const Node* n = scene.node(id.get<std::string>()); n; n = n->parent.empty() ? nullptr : scene.node(n->parent))
      if (n->id == component) return true;
    return false;
  };
  for (const auto& [id, n] : scene.nodes)  // what made the nodes in it (the component itself, imports, features)
    if (component.empty() || under(id)) out.insert(n.source_op);
  for (const auto& e : effective_ops(doc)) {
    const json& d = e.data();
    const std::string& type = e.op->type;
    bool in = component.empty() || out.count(e.op->id);
    if (!in && (type == "sketch" || type == "feature")) in = under(d.value("component", json()));
    if (!in && type == "feature")
      for (const auto& b : d.value("result", json::object()).value("bodies", json::array())) in = in || under(b.value("id", json()));
    if (!in && (type == "reparent" || type == "transform" || type == "appearance" || type == "rename")) in = under(d.value("target", json()));
    if (!in && type == "reparent") in = under(d.value("parent", json()));
    if (!in && type == "import") in = under(d.value("parent", json()));
    auto ref_under = [&](const json& r) {
      try {
        return under(json(Ref::from_json(r).body));
      } catch (const std::exception&) {
        return false;
      }
    };
    if (!in && type == "annotation" && d.contains("anchor")) in = ref_under(d["anchor"]);
    if (!in && type == "measurement")
      for (const auto& r : d.value("refs", json::array())) in = in || ref_under(r);
    if (in) out.insert(e.op->id);
  }
  return out;
}

}  // namespace opad
