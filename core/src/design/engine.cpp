#include "opad/design/sketch_reference.hpp"
#include "engine.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
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

// Bodies an input names as plain strings ("uuid", "uuid/edge/3"), which collect_refs (objects only) misses: the items
// of a pick input, and the values of face/edge/body keys inside plane, axis and path inputs. Choices and bases such
// as {"base":"xy"} are not references.
void string_refs(const json& v, bool reference, std::set<std::string>& out) {
  if (v.is_string()) {
    if (reference) {
      const auto& s = v.get_ref<const std::string&>();
      out.insert(s.substr(0, s.find('/')));
    }
  } else if (v.is_array()) {
    for (const auto& c : v) string_refs(c, reference, out);
  } else if (v.is_object()) {
    for (const auto& [k, c] : v.items())
      if (k != "hint") string_refs(c, k == "body" || k == "face" || k == "edge" || k == "edges", out);
  }
}

bool reference_input(const std::string& type) {
  return type == "bodies" || type == "faces" || type == "edges" || type == "points" || type == "profiles" || type == "plane" || type == "axis" || type == "path";
}

// Two results of one feature from equal inputs: the same topology, vertices (to about 1e-6 mm), volume, area and centre.
// A regeneration that produced such a body keeps the stored entry: the kernel's output can differ in bits that are not
// geometry (a Boolean on bodies read back from the store versus the same bodies fresh from the kernel), and a new key
// for an unchanged body is noise in the file and in git.
bool same_geometry(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  for (TopAbs_ShapeEnum type : {TopAbs_SOLID, TopAbs_SHELL, TopAbs_FACE, TopAbs_EDGE, TopAbs_VERTEX}) {
    TopTools_IndexedMapOfShape ma, mb;
    TopExp::MapShapes(a, type, ma);
    TopExp::MapShapes(b, type, mb);
    if (ma.Extent() != mb.Extent()) return false;
  }
  auto vertices = [](const TopoDS_Shape& s) {
    TopTools_IndexedMapOfShape map;
    TopExp::MapShapes(s, TopAbs_VERTEX, map);
    std::vector<std::array<long long, 3>> out;
    for (int i = 1; i <= map.Extent(); ++i) {
      const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(map(i)));
      out.push_back({std::llround(p.X() * 1e6), std::llround(p.Y() * 1e6), std::llround(p.Z() * 1e6)});
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  const auto va = vertices(a), vb = vertices(b);
  for (size_t i = 0; i < va.size(); ++i)
    for (int k = 0; k < 3; ++k)
      if (std::llabs(va[i][k] - vb[i][k]) > 2) return false;
  auto close = [](double x, double y) { return std::abs(x - y) <= 1e-7 * std::max({1.0, std::abs(x), std::abs(y)}); };
  GProp_GProps volume_a, volume_b, area_a, area_b;
  BRepGProp::VolumeProperties(a, volume_a);
  BRepGProp::VolumeProperties(b, volume_b);
  BRepGProp::SurfaceProperties(a, area_a);
  BRepGProp::SurfaceProperties(b, area_b);
  if (!close(volume_a.Mass(), volume_b.Mass()) || !close(area_a.Mass(), area_b.Mass())) return false;
  const gp_Pnt ca = area_a.CentreOfMass(), cb = area_b.CentreOfMass();
  return close(ca.X(), cb.X()) && close(ca.Y(), cb.Y()) && close(ca.Z(), cb.Z());
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
  if (in.contains("support")) {
    Frame f=plane(in.at("support"));
    const auto& origin=in.at("origin");
    double u=0,v=0;
    if(origin.contains("uv")) {
      u=origin.at("uv").at(0).get<double>();v=origin.at("uv").at(1).get<double>();
    } else {
      Vec3 world={0,0,0};
      if(origin.contains("world"))world=origin.at("world").get<Vec3>();
      else if(origin.contains("ref")) {
        const auto r=resolve(origin.at("ref"));gp_Pnt p;
        if(r.sub.ShapeType()==TopAbs_VERTEX)p=BRep_Tool::Pnt(TopoDS::Vertex(r.sub));
        else if(r.sub.ShapeType()==TopAbs_EDGE){BRepAdaptor_Curve curve(TopoDS::Edge(r.sub));if(curve.GetType()!=GeomAbs_Circle)throw Error("choose a circular edge for the origin center");p=curve.Circle().Location();}
        else throw Error("choose a vertex or circular edge for the origin");
        world={p.X(),p.Y(),p.Z()};
      } else throw Error("no sketch origin was chosen");
      f.to_local(world,u,v);
    }
    if(!std::isfinite(u)||!std::isfinite(v))throw Error("invalid sketch origin coordinates");
    f.origin=f.to_world(u,v);return f;
  }
  if(in.size()==1 && in.contains("frame"))return Frame::from_json(in.at("frame"));
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
    // Use the face centre to establish its axes before choosing the lower-left corner.
    GProp_GProps g;
    BRepGProp::SurfaceProperties(r.sub, g);
    gp_Dir xd = ax.XDirection();
    // Prefer a world-aligned x so sketches on box faces are not rotated arbitrarily.
    for (const gp_Dir& cand : {gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1)})
      if (std::fabs(cand.Dot(n)) < 1e-6) { xd = cand; break; }
    Frame f = frame_from_ax3(gp_Ax3(g.CentreOfMass(), n, xd));
    // New face sketches start at the lower-left real vertex in the plane axes.
    // Existing sketches retain their persisted frame below during regeneration.
    if(!in.contains("frame")) {
      bool have=false;double bestU=0,bestV=0;Vec3 corner=f.origin;
      for(TopExp_Explorer vertices(r.sub,TopAbs_VERTEX);vertices.More();vertices.Next()) {
        const auto p=BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current()));double u,v;f.to_local({p.X(),p.Y(),p.Z()},u,v);
        if(!have || v<bestV-1e-7 || (std::abs(v-bestV)<=1e-7 && u<bestU)){have=true;bestU=u;bestV=v;corner={p.X(),p.Y(),p.Z()};}
      }
      if(have)f.origin=corner;
    }
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
  const std::vector<EffectiveOp>* timeline = nullptr;  // the whole history being walked
  std::optional<std::set<std::string>> history_names;

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
    // Plain-string references count too (agents and the CLI write "uuid" and "uuid/edge/3"): without them a fillet
    // or a pattern did not regenerate when its body changed. A component stands for the bodies under it, as
    // body_ids resolves it. Features that name no such strings keep exactly the fingerprint they always had.
    bool automatic_targets = false;
    if (const FeatureSpec* spec = feature_spec(kind))
      for (const auto& in : spec->inputs) {
        automatic_targets |= in.name == "targets";
        if (inputs.contains(in.name) && reference_input(in.type))
          string_refs(inputs[in.name], in.type != "plane" && in.type != "axis" && in.type != "path", nodes);
      }
    for (const auto& id : std::set<std::string>(nodes))
      if (const Node* n = scene.node(id); n && n->kind == Node::Kind::Component)
        for (const auto& b : scene.bodies_under(id)) nodes.insert(b);
    // Automatic targets (join/cut/intersect with none named) depend on every body. Only features with a targets input
    // have them: combine's own operation names its target and tools, so it depends on those alone.
    const std::string op = inputs.value("operation", "new");
    const bool automatic = automatic_targets && inputs.contains("operation") && op != "new" && (!inputs.contains("targets") || inputs["targets"].empty());
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

  // A copy or a piece is named after its source with the next number no body has: "Screw" -> "Screw 2",
  // "Screw 7" -> "Screw 8".
  std::string copy_name(const Scene& scene, const json& pending, const std::string& source) {
    const Node* s = scene.node(source);
    std::string base = s ? s->name : "Body";
    int from = 1;
    const size_t space = base.find_last_of(' ');
    if (space != std::string::npos && space + 1 < base.size() && base.size() - space <= 7 &&
        std::all_of(base.begin() + space + 1, base.end(), [](unsigned char c) { return std::isdigit(c); })) {
      from = std::stoi(base.substr(space + 1));
      base.erase(space);
    }
    // Names given anywhere in the history count too: a copy a regenerated pattern adds must not take the name of a
    // copy made later in the timeline.
    if (!history_names && timeline) {
      history_names.emplace();
      for (const auto& e : *timeline) {
        const json& d = e.data();
        if (e.op->type == "rename" && d.contains("name") && d["name"].is_string()) history_names->insert(d["name"].get<std::string>());
        if (e.op->type == "feature" && d.contains("result") && d["result"].contains("bodies"))
          for (const auto& b : d["result"]["bodies"])
            if (b.contains("name") && b["name"].is_string()) history_names->insert(b["name"].get<std::string>());
      }
    }
    std::set<std::string> used = history_names ? *history_names : std::set<std::string>();
    for (const auto& [id, n] : scene.nodes) used.insert(n.name);
    for (const auto& b : pending) used.insert(b.value("name", ""));
    for (int i = from + 1;; ++i) {
      std::string name = base + " " + std::to_string(i);
      if (!used.count(name)) return name;
    }
  }

  // New bodies are named, placed and coloured when they are first made, and the entry keeps it: a regeneration never
  // renames or moves them, and replay only reads what is stored (TODO 10 B14, C2). A body made from scratch takes
  // the feature's name (numbered when the feature makes several); a copy or a piece takes its source's name, the
  // component its source is in and its source's colour.
  json materialize(const Ctx& ctx, const Out& out, const json& previous, const std::string& op_id, const std::string& feature_name) {
    json result = json::object();
    json bodies = json::array();
    std::vector<json> prev_new;
    for (const auto& b : previous.value("bodies", json::array()))
      if (b.value("new", false)) prev_new.push_back(b);
    size_t next_new = 0, made = 0, made_count = 0;
    for (const auto& b : out.bodies) made_count += b.node.empty() && b.source.empty();
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
        if (prev && prev->contains("name")) entry["name"] = (*prev)["name"];
        else if (!b.source.empty()) entry["name"] = copy_name(ctx.scene, bodies, b.source);
        else if (!feature_name.empty()) entry["name"] = numbered_name(feature_name, made + 1, made_count);
        else entry["name"] = next_body_name(ctx.scene, bodies, "Body");
        made += b.source.empty();
        if (prev) {
          for (const char* k : {"parent", "color"})
            if (prev->contains(k)) entry[k] = (*prev)[k];
        } else if (const Node* s = b.source.empty() ? nullptr : ctx.scene.node(b.source)) {
          if (!s->parent.empty()) entry["parent"] = s->parent;
          if (s->has_color) entry["color"] = s->color;
        }
        // The body is kept in its component's frame, as replay places it there.
        const std::string parent = entry.value("parent", "");
        if (!parent.empty() && ctx.scene.node(parent)) to_local = ctx.node_trsf(parent).Inverted();
        else entry.erase("parent");
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
      std::string key = body_key_for(local, &brep);
      // What later features of this walk (and the shape cache) get is the body as read back from its entry, the same
      // shape every later regeneration and a reopened file start from; the kernel's in-memory result made the first
      // computation of a feature differ from its recomputations (new keys for unchanged bodies).
      TopoDS_Shape stored;
      std::string was;
      for (const auto& p : previous.value("bodies", json::array()))
        if (p.value("id", "") == entry["id"].get<std::string>()) was = p.value("key", "");
      if (!was.empty() && was != key && doc.has_body(was)) {
        const TopoDS_Shape old = body_shape(doc, was);
        if (same_geometry(local, old)) {
          key = was;
          stored = old;
        }
      }
      entry["key"] = key;
      if (stored.IsNull() && !doc.has_body(key) && !fresh.count(key)) {
        stored = shape_from_brep(brep);
        NewBody nb;
        nb.key = key;
        nb.brep = std::move(brep);
        nb.meta = {{"name", entry.value("name", std::string("Body"))}, {"units", "mm"}, {"source", "design"}};
        nb.shape = std::make_shared<TopoDS_Shape>(stored);
        plan.bodies.push_back(std::move(nb));
      }
      if (stored.IsNull()) stored = fresh.count(key) ? fresh[key] : body_shape(doc, key);
      fresh[key] = stored;
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
      refresh_references(sk,[&](const json& ref,const std::string& mode){return derive_sketch(ctx.doc,ctx.scene,ctx.plane(plane),ref,mode,ctx.fresh);});
      s+=sk.to_json().dump();
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
    if (plane.contains("face") || plane.contains("feature") || plane.contains("support")) {
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
    if (plane.contains("face") || plane.contains("feature") || plane.contains("support")) {
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
    timeline = &ops;

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
            result = materialize(ctx, out, stored, id, data.value("name", ""));
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

std::string numbered_name(const std::string& name, size_t n, size_t count) {
  const std::string number = std::to_string(n);
  if (name.find("{n}") == std::string::npos) return count > 1 ? name + " " + number : name;
  std::string out = name;
  for (size_t at = 0; (at = out.find("{n}", at)) != std::string::npos; at += number.size()) out.replace(at, 3, number);
  return out;
}

size_t style_new_bodies(Plan& plan, const std::string& feature_op, const json& style) {
  std::vector<std::string> ids;
  for (const auto& op : plan.ops)
    if (op.value("id", "") == feature_op)
      for (const auto& b : op.value("result", json::object()).value("bodies", json::array()))
        if (b.value("new", false)) ids.push_back(b.value("id", ""));
  const std::string name = style.contains("body_name") && style["body_name"].is_string() ? style["body_name"].get<std::string>() : "";
  const bool colour = style.contains("color") && !style["color"].is_null();
  const bool parent = style.contains("parent");
  if (colour && !(style["color"].is_array() && style["color"].size() == 3)) throw Error("color must be [r,g,b] in 0..1");
  for (size_t i = 0; i < ids.size(); ++i) {
    if (!name.empty()) plan.ops.push_back({{"op", "rename"}, {"target", ids[i]}, {"name", numbered_name(name, i + 1, ids.size())}});
    if (colour) plan.ops.push_back({{"op", "appearance"}, {"target", ids[i]}, {"color", style["color"]}});
    if (parent) {
      const json& p = style["parent"];
      plan.ops.push_back({{"op", "reparent"}, {"target", ids[i]}, {"parent", p.is_string() && !p.get<std::string>().empty() ? p : json(nullptr)}});
    }
  }
  return ids.size();
}

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
