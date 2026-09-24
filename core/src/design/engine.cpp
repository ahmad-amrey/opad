#include "engine.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <set>

#include "opad/geometry.hpp"
#include "opad/design/sketch_pattern.hpp"

namespace opad::design {

namespace {

TopAbs_ShapeEnum abs_of(Ref::Kind k) { return k == Ref::Kind::Face ? TopAbs_FACE : k == Ref::Kind::Edge ? TopAbs_EDGE : TopAbs_VERTEX; }

// Centre and size of a sub-shape: what is left to recognise it by when ordinals have moved.
void fingerprint(const TopoDS_Shape& sub, gp_Pnt& centre, double& size) {
  GProp_GProps g;
  if (sub.ShapeType() == TopAbs_FACE) BRepGProp::SurfaceProperties(sub, g);
  else if (sub.ShapeType() == TopAbs_EDGE) BRepGProp::LinearProperties(sub, g);
  else {
    centre = BRep_Tool::Pnt(TopoDS::Vertex(sub));
    size = 0;
    return;
  }
  centre = g.CentreOfMass();
  size = std::fabs(g.Mass());
}

void collect_refs(const json& j, std::set<std::string>& nodes, std::set<std::string>& sketches, std::set<std::string>& features) {
  if (j.is_object()) {
    if (j.contains("body") && j["body"].is_string()) nodes.insert(j["body"].get<std::string>());
    if (j.contains("sketch") && j["sketch"].is_string()) sketches.insert(j["sketch"].get<std::string>());
    if (j.contains("feature") && j["feature"].is_string()) features.insert(j["feature"].get<std::string>());
    for (const auto& [k, v] : j.items())
      if (k != "hint") collect_refs(v, nodes, sketches, features);
  } else if (j.is_array()) {
    for (const auto& v : j) collect_refs(v, nodes, sketches, features);
  }
}

bool input_shown(const InputSpec& in, const json& inputs) {
  if (in.show_if.empty()) return true;
  const size_t eq = in.show_if.find('=');
  const std::string key = in.show_if.substr(0, eq);
  std::string have;
  if (inputs.contains(key)) have = inputs[key].is_string() ? inputs[key].get<std::string>() : inputs[key].dump();
  std::string values = in.show_if.substr(eq + 1);
  size_t start = 0;
  while (start <= values.size()) {
    const size_t bar = values.find('|', start);
    if (values.substr(start, bar == std::string::npos ? std::string::npos : bar - start) == have) return true;
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return false;
}

Dim dim_of(const std::string& type) { return type == "length" ? Dim::Length : type == "angle" ? Dim::Angle : Dim::None; }

}  // namespace

// ---------------------------------------------------------------- Ctx
void Ctx::check_cancel() const {
  if (cancel && cancel()) throw Error("cancelled");
}

TopoDS_Shape Ctx::key_shape(const std::string& key) const {
  auto it = fresh.find(key);
  return it != fresh.end() ? it->second : body_shape(doc, key);
}

gp_Trsf Ctx::node_trsf(const std::string& node) const {
  if (node.empty()) return gp_Trsf();
  const Mat4 w = scene.world(node);
  return w.is_identity() ? gp_Trsf() : trsf_from_mat(w);
}

TopoDS_Shape Ctx::node_shape(const std::string& node) const {
  const Node* n = scene.node(node);
  if (!n || n->kind != Node::Kind::Body) throw Error("a referenced body no longer exists");
  if (n->representation == "mesh") throw Error("Mesh objects are view-only; solid modelling requires a CAD body");
  TopoDS_Shape proto = key_shape(n->body_key);
  const gp_Trsf t = node_trsf(node);
  return t.Form() == gp_Identity ? proto : proto.Moved(TopLoc_Location(t));
}

json ref_hint(const TopoDS_Shape& body_world, const TopoDS_Shape& sub) {
  gp_Pnt c;
  double size = 0;
  fingerprint(sub, c, size);
  json counts = json::array();
  for (TopAbs_ShapeEnum t : {TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX}) {
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(body_world, t, map);
    counts.push_back(map.Extent());
  }
  return {{"c", {c.X(), c.Y(), c.Z()}}, {"s", size}, {"n", counts}};
}

ResolvedRef Ctx::resolve(const json& j) const {
  const Ref r = Ref::from_json(j);
  if (r.kind == Ref::Kind::Point) throw Error("a point is not a valid reference here");
  ResolvedRef out;
  out.node = r.body;
  const TopoDS_Shape body = node_shape(r.body);
  if (r.kind == Ref::Kind::Body) {
    out.sub = body;
    return out;
  }
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(body, abs_of(r.kind), map);
  const json hint = j.is_object() ? j.value("hint", json()) : json();
  // Same entity counts as when the reference was made: the topology did not change, the ordinal is good.
  bool same_topology = !hint.is_object();
  if (hint.is_object() && hint.contains("n") && hint["n"].is_array() && hint["n"].size() == 3) {
    same_topology = true;
    int i = 0;
    for (TopAbs_ShapeEnum t : {TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX}) {
      TopTools_IndexedMapOfShape m;
      TopExp::MapShapes(body, t, m);
      if (m.Extent() != hint["n"][static_cast<size_t>(i++)].get<int>()) same_topology = false;
    }
  }
  if (same_topology) {
    if (r.index < 0 || r.index >= map.Extent()) throw Error(std::string("a referenced ") + Ref::kind_name(r.kind) + " no longer exists");
    out.sub = map(r.index + 1);
    out.index = r.index;
    return out;
  }
  // The body was rebuilt differently: take the entity that is nearest to where the picked one was, of about
  // its size. A loose match is better than none, and a wrong one shows up in the result.
  const gp_Pnt want(hint["c"][0].get<double>(), hint["c"][1].get<double>(), hint["c"][2].get<double>());
  const double wantSize = hint.value("s", 0.0);
  double best = 1e300;
  for (int i = 1; i <= map.Extent(); ++i) {
    gp_Pnt c;
    double size = 0;
    fingerprint(map(i), c, size);
    const double scale = r.kind == Ref::Kind::Face ? std::sqrt(std::max(wantSize, 1e-12)) : std::max(wantSize, 1e-6);
    const double score = c.Distance(want) + std::fabs((r.kind == Ref::Kind::Face ? std::sqrt(size) : size) - scale);
    if (score < best) {
      best = score;
      out.sub = map(i);
      out.index = i - 1;
    }
  }
  if (out.sub.IsNull()) throw Error(std::string("a referenced ") + Ref::kind_name(r.kind) + " no longer exists");
  return out;
}

std::vector<ResolvedRef> Ctx::resolve_all(const json& refs) const {
  std::vector<ResolvedRef> out;
  if (refs.is_array())
    for (const auto& r : refs) out.push_back(resolve(r));
  else if (!refs.is_null())
    out.push_back(resolve(refs));
  return out;
}

double Ctx::length(const json& inputs, const char* name) const {
  if (!inputs.contains(name)) throw Error(std::string("missing input \"") + name + "\"");
  return eval_input(params, inputs[name], Dim::Length);
}
double Ctx::angle(const json& inputs, const char* name) const {
  if (!inputs.contains(name)) throw Error(std::string("missing input \"") + name + "\"");
  return eval_input(params, inputs[name], Dim::Angle);
}
double Ctx::number(const json& inputs, const char* name) const {
  if (!inputs.contains(name)) throw Error(std::string("missing input \"") + name + "\"");
  return eval_input(params, inputs[name], Dim::None);
}
int Ctx::count(const json& inputs, const char* name) const {
  const double v = number(inputs, name);
  if (std::fabs(v - std::round(v)) > 1e-9) throw Error(std::string("\"") + name + "\" must be a whole number");
  return static_cast<int>(std::lround(v));
}

Sketch Ctx::sketch(const std::string& id, Frame* frame) const {
  const SketchItem* s = scene.sketch(id);
  if (!s) throw Error("a referenced sketch no longer exists");
  if (frame) *frame = s->frame;
  return Sketch::from_json(s->geometry);
}

Frame Ctx::plane(const json& in) const {
  if (!in.is_object()) throw Error("no plane was chosen");
  if (in.contains("base")) return base_frame(in["base"].get<std::string>());
  if (in.contains("feature")) {
    const Feature* f = scene.feature(in["feature"].get<std::string>());
    if (!f || !f->result.contains("plane")) throw Error("the construction plane no longer exists");
    return Frame::from_json(f->result["plane"]);
  }
  if (in.contains("sketch")) {
    const SketchItem* s = scene.sketch(in["sketch"].get<std::string>());
    if (!s) throw Error("a referenced sketch no longer exists");
    return s->frame;
  }
  if (in.contains("face")) {
    const ResolvedRef r = resolve(in["face"]);
    if (r.sub.IsNull() || r.sub.ShapeType() != TopAbs_FACE) throw Error("the plane must be a planar face");
    BRepAdaptor_Surface surf(TopoDS::Face(r.sub));
    if (surf.GetType() != GeomAbs_Plane) throw Error("that face is not planar");
    gp_Ax3 ax = surf.Plane().Position();
    gp_Dir n = ax.Direction();
    if (!ax.Direct()) n.Reverse();
    if (r.sub.Orientation() == TopAbs_REVERSED) n.Reverse();  // outward from the body
    // Origin at the face's centre, so a sketch on it starts where the user is looking.
    GProp_GProps g;
    BRepGProp::SurfaceProperties(r.sub, g);
    gp_Dir xd = ax.XDirection();
    // Prefer a world-aligned x so sketches on box faces are not rotated arbitrarily.
    for (const gp_Dir& cand : {gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1)})
      if (std::fabs(cand.Dot(n)) < 1e-6) { xd = cand; break; }
    Frame f = frame_from_ax3(gp_Ax3(g.CentreOfMass(), n, xd));
    if (in.contains("frame")) {  // keep the sketch where it was on the face: project the old origin and x
      const Frame old = Frame::from_json(in["frame"]);
      const gp_Pln pl(g.CentreOfMass(), n);
      const gp_Pnt o(old.origin[0], old.origin[1], old.origin[2]);
      const double d = gp_Vec(pl.Location(), o).Dot(gp_Vec(n));
      const gp_Pnt po = o.Translated(gp_Vec(n) * -d);
      gp_Vec ox(old.x[0], old.x[1], old.x[2]);
      ox -= gp_Vec(n) * ox.Dot(gp_Vec(n));
      if (ox.Magnitude() > 1e-6) f = frame_from_ax3(gp_Ax3(po, n, gp_Dir(ox)));
    }
    return f;
  }
  throw Error("no plane was chosen");
}

gp_Ax1 Ctx::axis(const json& in) const {
  if (!in.is_object()) throw Error("no axis was chosen");
  if (in.contains("base")) {
    const std::string b = in["base"].get<std::string>();
    return gp_Ax1(gp_Pnt(0, 0, 0), b == "x" ? gp_Dir(1, 0, 0) : b == "y" ? gp_Dir(0, 1, 0) : gp_Dir(0, 0, 1));
  }
  if (in.contains("feature")) {
    const Feature* f = scene.feature(in["feature"].get<std::string>());
    if (!f || !f->result.contains("axis")) throw Error("the construction axis no longer exists");
    const json& a = f->result["axis"];
    return gp_Ax1(gp_Pnt(a["origin"][0], a["origin"][1], a["origin"][2]), gp_Dir(a["dir"][0], a["dir"][1], a["dir"][2]));
  }
  if (in.contains("sketch")) {
    Frame frame;
    const Sketch sk = sketch(in["sketch"].get<std::string>(), &frame);
    const SkEntity* e = sk.entity(in.value("entity", 0));
    if (!e || e->type != SkEntity::Type::Line) throw Error("the axis must be a sketch line");
    const SkPoint *a = sk.point(e->p[0]), *b = sk.point(e->p[1]);
    const Vec3 pa = frame.to_world(a->x, a->y), pb = frame.to_world(b->x, b->y);
    const gp_Vec d(pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]);
    if (d.Magnitude() < 1e-9) throw Error("the axis line has no length");
    return gp_Ax1(gp_Pnt(pa[0], pa[1], pa[2]), gp_Dir(d));
  }
  const json* ref = in.contains("edge") ? &in["edge"] : in.contains("face") ? &in["face"] : nullptr;
  if (!ref) throw Error("no axis was chosen");
  const ResolvedRef r = resolve(*ref);
  if (r.sub.ShapeType() == TopAbs_EDGE) {
    BRepAdaptor_Curve c(TopoDS::Edge(r.sub));
    if (c.GetType() == GeomAbs_Line) return c.Line().Position();
    if (c.GetType() == GeomAbs_Circle) return c.Circle().Axis();
    throw Error("the axis must be a straight or circular edge");
  }
  if (r.sub.ShapeType() == TopAbs_FACE) {
    BRepAdaptor_Surface s(TopoDS::Face(r.sub));
    if (s.GetType() == GeomAbs_Cylinder) return s.Cylinder().Axis();
    if (s.GetType() == GeomAbs_Cone) return s.Cone().Axis();
    if (s.GetType() == GeomAbs_Torus) return s.Torus().Axis();
    if (s.GetType() == GeomAbs_Plane) return s.Plane().Axis();
    throw Error("that face has no axis");
  }
  throw Error("no axis was chosen");
}

// ---------------------------------------------------------------- the walk
namespace {

struct Walk {
  const Document& doc;
  std::vector<json>& new_ops;
  bool force;
  Cancel cancel;
  Plan plan;
  std::map<std::string, TopoDS_Shape> fresh;
  std::map<std::string, size_t> new_index;  // op id -> index in new_ops
  json regen = json::object();
  std::vector<std::string> regenerated;

  bool is_new(const std::string& id) const { return new_index.count(id) > 0; }

  // The new op that carries this op's inputs: the op itself when new, or a new edit of it that sets them.
  json* patchable_inputs(const std::string& id) {
    if (auto it = new_index.find(id); it != new_index.end()) return &new_ops[it->second]["inputs"];
    for (auto it = new_ops.rbegin(); it != new_ops.rend(); ++it)
      if (it->value("op", "") == "edit" && it->value("target", "") == id && (*it)["set"].contains("inputs")) return &(*it)["set"]["inputs"];
    return nullptr;
  }

  std::string node_state(const Scene& scene, const std::string& id) const {
    const Node* n = scene.node(id);
    if (!n) return id + ":gone;";
    return id + ":" + n->body_key + ":" + scene.world(id).to_json().dump() + ";";
  }

  std::string feature_fingerprint(const Scene& scene, const ParamTable& params, const std::string& kind, const json& inputs) const {
    std::string s = "feature|" + kind + "|" + inputs.dump() + "|";
    if (const FeatureSpec* spec = feature_spec(kind))
      for (const auto& in : spec->inputs) {
        if (!inputs.contains(in.name) || !input_shown(in, inputs)) continue;
        if (in.type != "length" && in.type != "angle" && in.type != "number" && in.type != "count") continue;
        try {
          s += in.name + "=" + json(eval_input(params, inputs[in.name], dim_of(in.type))).dump() + ";";
        } catch (const std::exception& e) {
          s += in.name + "!" + e.what() + ";";
        }
      }
    std::set<std::string> nodes, sketches, features;
    collect_refs(inputs, nodes, sketches, features);
    const std::string op = inputs.value("operation", "new");
    const bool automatic = inputs.contains("operation") && op != "new" && (!inputs.contains("targets") || inputs["targets"].empty());
    if (automatic)
      for (const auto& b : scene.all_bodies()) nodes.insert(b);
    for (const auto& n : nodes) s += node_state(scene, n);
    for (const auto& id : sketches)
      if (const SketchItem* sk = scene.sketch(id)) s += id + ":" + sk->geometry.dump() + sk->frame.to_json().dump() + ";";
      else s += id + ":gone;";
    for (const auto& id : features)
      if (const Feature* f = scene.feature(id)) s += id + ":" + f->result.value("plane", json()).dump() + f->result.value("axis", json()).dump() + ";";
      else s += id + ":gone;";
    return sha256_hex(s).substr(0, 24);
  }

  std::string next_body_name(const Scene& scene, const json& pending, const std::string& prefix) const {
    std::set<std::string> used;
    for (const auto& [id, n] : scene.nodes) used.insert(n.name);
    for (const auto& b : pending) used.insert(b.value("name", ""));
    for (int i = 1;; ++i) {
      std::string name = prefix + " " + std::to_string(i);
      if (!used.count(name)) return name;
    }
  }

  json materialize(const Ctx& ctx, const Out& out, const json& previous, const std::string& op_id) {
    json result = json::object();
    json bodies = json::array();
    std::vector<json> prev_new;
    for (const auto& b : previous.value("bodies", json::array()))
      if (b.value("new", false)) prev_new.push_back(b);
    size_t next_new = 0;
    for (const auto& b : out.bodies) {
      if (b.shape.IsNull()) throw Error("the operation produced no geometry");
      json entry;
      gp_Trsf to_local;
      if (!b.node.empty()) {
        to_local = ctx.node_trsf(b.node).Inverted();
        entry["id"] = b.node;
      } else {
        const json* prev = next_new < prev_new.size() ? &prev_new[next_new] : nullptr;
        ++next_new;
        entry["id"] = prev ? (*prev)["id"] : json(new_uuid());
        entry["name"] = prev && prev->contains("name") ? (*prev)["name"] : json(next_body_name(ctx.scene, bodies, b.name.empty() ? "Body" : b.name));
        entry["new"] = true;
      }
      TopoDS_Shape local = b.shape;
      if (to_local.Form() != gp_Identity) local = BRepBuilderAPI_Transform(b.shape, to_local, Standard_True).Shape();
      // Locations are baked in so the entry is the same text whichever way the body was moved about.
      if (!local.Location().IsIdentity()) {
        const gp_Trsf where = local.Location().Transformation();
        local = BRepBuilderAPI_Transform(local.Located(TopLoc_Location()), where, Standard_True).Shape();
      }
      std::string brep;
      const std::string key = body_key_for(local, &brep);
      entry["key"] = key;
      if (!doc.has_body(key) && !fresh.count(key)) {
        NewBody nb;
        nb.key = key;
        nb.brep = std::move(brep);
        nb.meta = {{"name", entry.value("name", std::string("Body"))}, {"units", "mm"}, {"source", "design"}};
        nb.shape = std::make_shared<TopoDS_Shape>(local);
        plan.bodies.push_back(std::move(nb));
      }
      fresh[key] = local;
      bodies.push_back(entry);
      plan.changed.push_back({op_id, entry["id"].get<std::string>(), std::make_shared<TopoDS_Shape>(b.shape), false});
    }
    if (!bodies.empty()) result["bodies"] = bodies;
    if (!out.removed.empty()) {
      result["removed"] = out.removed;
      for (const auto& r : out.removed) plan.changed.push_back({op_id, r, nullptr, true});
    }
    for (const auto& [k, v] : out.extra.items()) result[k] = v;
    return result;
  }

  json compute_sketch(const Ctx& ctx, const json& data, std::string& fp) {
    const json& geometry = data.at("geometry");
    const json& plane = data.at("plane");
    Sketch sk = Sketch::from_json(geometry);
    std::string s = "sketch|" + geometry.dump() + "|" + plane.dump() + "|";
    bool moved = false;
    std::string error;
    try {
      evaluate_patterns(sk,ctx.params);
      s += sk.patterns.dump();
      evaluate_dimensions(sk, ctx.params);
      for (const auto& c : sk.constraints) if (c.is_dimension() && !c.reference) {
        s += std::to_string(c.id) + "=" + json(c.value).dump() + ";";
      }
      moved = sk.to_json() != Sketch::from_json(geometry).to_json();
    } catch (const std::exception& e) {
      error = e.what();
      s += "!" + error;
    }
    Frame frame = Frame::from_json(plane.value("frame", json()));
    bool frame_moved = false;
    if (plane.contains("face") || plane.contains("feature")) {
      std::set<std::string> nodes, sketches, features;
      collect_refs(plane, nodes, sketches, features);
      for (const auto& n : nodes) s += node_state(ctx.scene, n);
      for (const auto& id : features)
        if (const Feature* f = ctx.scene.feature(id)) s += id + ":" + f->result.value("plane", json()).dump() + ";";
    }
    fp = sha256_hex(s).substr(0, 24);
    const json stored = data.value("result", json::object());
    if (!force && stored.value("in", "") == fp) return stored;

    json result = json::object();
    result["in"] = fp;
    if (plane.contains("face") || plane.contains("feature")) {
      try {
        const Frame now = ctx.plane(plane);
        if (now.to_json() != frame.to_json()) { frame = now; frame_moved = true; }
      } catch (const std::exception& e) {
        error = e.what();
      }
    }
    SolveResult solved = solve(sk);
    if (!solved.converged && error.empty()) error = "the sketch constraints cannot all be satisfied";
    result["dof"] = solved.dof;
    if (solved.converged && (moved || sk.to_json() != Sketch::from_json(geometry).to_json())) result["geometry"] = sk.to_json();
    if (frame_moved) result["frame"] = frame.to_json();
    if (!error.empty()) result["error"] = error;
    return result;
  }

  Plan run(bool strict) {
    std::vector<Op> temp;
    temp.reserve(new_ops.size());
    for (size_t i = 0; i < new_ops.size(); ++i) {
      json& op = new_ops[i];
      if (!op.contains("id")) op["id"] = new_uuid();
      Document::validate_op(op);
      const std::string type = op["op"].get<std::string>();
      if ((type == "delete" || type == "edit") && !doc.find_op(op["target"].get<std::string>()) && !new_index.count(op["target"].get<std::string>()))
        throw Error(type + ": target op not found");
      new_index[op["id"].get<std::string>()] = i;
    }
    for (const auto& op : new_ops) temp.push_back(Op{op["id"].get<std::string>(), op["op"].get<std::string>(), op, {}});
    std::vector<const Op*> all;
    all.reserve(doc.ops.size() + temp.size());
    for (const auto& o : doc.ops) all.push_back(&o);
    for (const auto& o : temp) all.push_back(&o);
    const std::vector<EffectiveOp> ops = effective_ops(all);

    std::vector<ParamDef> defs;
    for (const auto& e : ops)
      if (e.op->type == "param") defs.push_back({e.op->id, e.data().value("name", ""), e.data().value("expr", ""), e.data().value("comment", "")});
    const ParamTable params(defs);
    if (strict) check_params(params, ops);

    json errors = json::array();
    SceneBuilder builder(doc);
    for (const auto& e : ops) {
      if (cancel && cancel()) throw Error("cancelled");
      const std::string& id = e.op->id;
      const std::string& type = e.op->type;
      if (type != "sketch" && type != "feature") {
        try {
          builder.apply(id, type, e.data());
        } catch (const std::exception&) {  // resolve() reports it; the walk only needs the state
        }
        continue;
      }
      json data = e.data();
      const json stored = data.value("result", json::object());
      json result;
      if (type == "feature" && data.value("suppressed", false)) {
        builder.apply(id, type, data);
        continue;
      }
      const Ctx ctx{doc, params, builder.scene(), fresh, cancel};
      if (type == "sketch") {
        std::string fp;
        try {
          result = compute_sketch(ctx, data, fp);
        } catch (const std::exception& ex) {
          result = {{"in", fp}, {"error", ex.what()}};
        }
      } else {
        const std::string kind = data.value("kind", "");
        json inputs = data.value("inputs", json::object());
        std::string fp = feature_fingerprint(builder.scene(), params, kind, inputs);
        if (!force && stored.value("in", "") == fp) {
          result = stored;
          note_fresh(result);
        } else {
          try {
            Out out = compute_feature(ctx, kind, inputs);
            if (!out.used_targets.empty()) {
              if (json* patch = patchable_inputs(id)) {
                json targets = json::array();
                for (const auto& t : out.used_targets) targets.push_back({{"body", t}, {"kind", "body"}});
                inputs["targets"] = targets;
                (*patch)["targets"] = targets;
                data["inputs"] = inputs;
                fp = feature_fingerprint(builder.scene(), params, kind, inputs);
              }
            }
            result = materialize(ctx, out, stored, id);
          } catch (const Standard_Failure& ex) {
            result = {{"error", std::string("the modelling kernel failed: ") + ex.GetMessageString()}};
          } catch (const std::exception& ex) {
            if (std::string(ex.what()) == "cancelled") throw;
            result = {{"error", ex.what()}};
          }
          result["in"] = fp;
        }
      }
      if (result.contains("error")) errors.push_back({{"op", id}, {"name", data.value("name", "")}, {"error", result["error"]}});
      if (is_new(id)) {
        new_ops[new_index[id]] = [&] {
          json op = new_ops[new_index[id]];
          op["result"] = result;
          return op;
        }();
      } else if (result != stored) {
        regen[id] = result;
        regenerated.push_back(id);
      }
      data["result"] = result;
      try {
        builder.apply(id, type, data);
      } catch (const std::exception&) {
      }
    }

    if (strict) {
      // The ops being added (and the ops they edit) must work; trouble further down the timeline is
      // reported and shown there instead, as it would be after any upstream change.
      std::set<std::string> direct;
      for (const auto& op : new_ops) {
        direct.insert(op["id"].get<std::string>());
        if (op.value("op", "") == "edit") direct.insert(op.value("target", ""));
      }
      for (const auto& err : errors)
        if (direct.count(err["op"].get<std::string>())) throw Error(err["error"].get<std::string>());
    }

    plan.ops = new_ops;
    if (!regen.empty()) plan.ops.push_back({{"op", "regen"}, {"results", regen}});
    json ids = json::array();
    for (const auto& op : new_ops) ids.push_back(op["id"]);
    plan.report = {{"ids", ids}, {"regenerated", regenerated}, {"errors", errors}};
    return std::move(plan);
  }

  // A reused result may point at bodies an earlier step of this plan has not put in the document yet; nothing
  // to do for stored ones (they are in the store), this only keeps the bookkeeping in one place.
  void note_fresh(const json&) {}

  void check_params(const ParamTable& params, const std::vector<EffectiveOp>& ops) const {
    std::set<std::string> touched;
    for (const auto& op : new_ops) {
      if (op.value("op", "") == "param") touched.insert(op["id"].get<std::string>());
      if (op.value("op", "") == "edit") touched.insert(op.value("target", ""));
    }
    std::map<std::string, int> names;
    for (const auto& e : ops)
      if (e.op->type == "param") names[e.data().value("name", "")]++;
    for (const auto& e : ops) {
      if (e.op->type != "param" || !touched.count(e.op->id)) continue;
      const std::string name = e.data().value("name", "");
      if (!valid_param_name(name)) throw Error("\"" + name + "\" is not a valid parameter name (letters, digits and _, not a unit or function name)");
      if (names[name] > 1) throw Error("a parameter called \"" + name + "\" already exists");
      params.value_of(name);  // throws with the reason
    }
  }
};

}  // namespace

Plan plan_ops(const Document& doc, std::vector<json> new_ops, bool strict, const Cancel& cancel) {
  Walk w{doc, new_ops, false, cancel, {}, {}, {}, json::object(), {}};
  return w.run(strict);
}

Plan plan_regenerate(const Document& doc, bool force, const Cancel& cancel) {
  std::vector<json> none;
  Walk w{doc, none, force, cancel, {}, {}, {}, json::object(), {}};
  return w.run(false);
}

json commit(Document& doc, Plan&& plan, const std::string& author) {
  for (auto& b : plan.bodies) {
    doc.add_body(b.brep, b.meta);
    if (b.shape) cache_shape(doc, b.key, *b.shape);
  }
  for (auto& op : plan.ops) doc.append(op, author);
  return plan.report;
}

json apply_ops(Document& doc, std::vector<json> new_ops, const std::string& author) { return commit(doc, plan_ops(doc, std::move(new_ops)), author); }

// ---------------------------------------------------------------- helpers
json make_param_op(const std::string& name, const std::string& expr, const std::string& comment) {
  json op = {{"op", "param"}, {"name", name}, {"expr", expr}};
  if (!comment.empty()) op["comment"] = comment;
  return op;
}

json make_sketch_op(const std::string& name, const json& plane, const json& geometry) {
  return {{"op", "sketch"}, {"name", name}, {"plane", plane}, {"geometry", geometry}};
}

json make_feature_op(const std::string& kind, const std::string& name, const json& inputs) {
  return {{"op", "feature"}, {"kind", kind}, {"name", name}, {"inputs", inputs}};
}

json make_edit_op(const std::string& target, const json& set) { return {{"op", "edit"}, {"target", target}, {"set", set}}; }

std::string next_name(const Scene& scene, const std::string& prefix) {
  std::set<std::string> used;
  for (const auto& f : scene.features) used.insert(f.name);
  for (const auto& s : scene.sketches) used.insert(s.name);
  for (const auto& [id, n] : scene.nodes) used.insert(n.name);
  for (int i = 1;; ++i) {
    std::string name = prefix + std::to_string(i);
    if (!used.count(name)) return name;
  }
}

namespace {
// Applies fn to every expression string of an op: parameter expressions, numeric feature inputs, sketch
// dimension expressions. fn returns the replacement (or the same text).
json map_expressions(const std::string& type, const json& data, const std::function<std::string(const std::string&)>& fn) {
  json set = json::object();
  if (type == "param") {
    const std::string e = data.value("expr", "");
    if (fn(e) != e) set["expr"] = fn(e);
  } else if (type == "feature") {
    json inputs = data.value("inputs", json::object());
    bool changed = false;
    if (const FeatureSpec* spec = feature_spec(data.value("kind", "")))
      for (const auto& in : spec->inputs)
        if ((in.type == "length" || in.type == "angle" || in.type == "number" || in.type == "count") && inputs.contains(in.name) && inputs[in.name].is_string()) {
          const std::string e = inputs[in.name].get<std::string>();
          if (fn(e) != e) { inputs[in.name] = fn(e); changed = true; }
        }
    if (changed) set["inputs"] = inputs;
  } else if (type == "sketch") {
    const json original = data.value("result",json::object()).value("geometry",data.value("geometry",json::object()));
    json geometry = original;
    bool changed = false;
    if (geometry.contains("constraints"))
      for (auto& c : geometry["constraints"])
        if (c.contains("expr") && c["expr"].is_string()) {
          const std::string e = c["expr"].get<std::string>();
          if (fn(e) != e) { c["expr"] = fn(e); changed = true; }
        }
    if(geometry.contains("patterns"))for(auto& pattern:geometry["patterns"])for(auto& value:pattern["inputs"])
      if(value.is_string()){const std::string e=value.get<std::string>();if(fn(e)!=e){value=fn(e);changed=true;}}
    if (changed) set["geometry_delta"] = sketch_delta(original,geometry);
  }
  return set;
}
}  // namespace

std::vector<json> rename_param_ops(const Document& doc, const std::string& from, const std::string& to) {
  std::vector<json> out;
  for (const auto& e : effective_ops(doc)) {
    json set = map_expressions(e.op->type, e.data(), [&](const std::string& x) { return expr_rename(x, from, to); });
    if (e.op->type == "param" && e.data().value("name", "") == from) set["name"] = to;
    if (!set.empty()) out.push_back(make_edit_op(e.op->id, set));
  }
  return out;
}

std::vector<std::string> param_users(const Document& doc, const std::string& name) {
  std::vector<std::string> out;
  for (const auto& e : effective_ops(doc)) {
    bool uses = false;
    map_expressions(e.op->type, e.data(), [&](const std::string& x) {
      const auto ids = expr_identifiers(x);
      if (std::find(ids.begin(), ids.end(), name) != ids.end()) uses = true;
      return x;
    });
    if (uses) out.push_back(e.data().value("name", e.op->type));
  }
  return out;
}

Frame resolve_plane(const Document& doc, const Scene& scene, const json& plane) {
  const ParamTable params;
  const std::map<std::string, TopoDS_Shape> fresh;
  const Ctx ctx{doc, params, scene, fresh, {}};
  return ctx.plane(plane);
}

json make_ref(const Document& doc, const Scene& scene, const Ref& ref) {
  json j = ref.to_json();
  if (ref.kind == Ref::Kind::Body || ref.kind == Ref::Kind::Point) return j;
  const TopoDS_Shape body = node_world_shape(doc, scene, ref.body);
  j["hint"] = ref_hint(body, subshape(body, ref.kind, ref.index));
  return j;
}

json hint_refs(const Document& doc, const Scene& scene, json inputs) {
  std::function<void(json&)> walk = [&](json& j) {
    if (j.is_array()) {
      for (auto& v : j) walk(v);
    } else if (j.is_object()) {
      if (j.contains("body") && j.contains("kind") && j["kind"].is_string() && !j.contains("hint")) {
        try {
          const Ref r = Ref::from_json(j);
          if (r.kind == Ref::Kind::Face || r.kind == Ref::Kind::Edge || r.kind == Ref::Kind::Vertex) j = make_ref(doc, scene, r);
        } catch (const std::exception&) {  // a stale reference: the feature reports it
        }
        return;
      }
      for (auto& [k, v] : j.items()) walk(v);
    }
  };
  walk(inputs);
  return inputs;
}

}  // namespace opad::design
