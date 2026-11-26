// Fitted references (opad/scan_fit.hpp made into plane and axis inputs): a plane, line or axis estimated from a mesh body
// (a region of triangles grown from a seed, a box of triangles, picked points), where a scan has no exact face or edge.
//
//   {"fit": {"body": <node>, "of": <kind>, "seed": <triangle>, "tolerance": "10 deg", "triangles": [[first, last], ...],
//            "points": [...], "planes": [<plane>, <plane>], "base": 0, "turn": 0, "origin": "base"},
//    "result": {...}}
//
// The result is what the fit gave (origin, normal / dir, x, radius, quality) in the body's own coordinates (world ones for a
// fit with no body), with the body key it was fitted on and a digest of the "fit" object: while both match, the result is
// used as it is (moving the body moves it along), so replay and unrelated regenerations never refit; a new mesh or a
// changed request fits again. complete_fits writes results into a new op's inputs (Walk), as automatic targets are.
#include <BRep_Tool.hxx>
#include <TopoDS.hxx>

#include <cmath>

#include "engine.hpp"
#include "opad/geometry.hpp"
#include "opad/scan_fit.hpp"
#include "opad/util.hpp"

namespace opad::design {

namespace {

constexpr double kPi = 3.14159265358979323846;

gp_Pnt p3(const json& v) { return gp_Pnt(v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>()); }
gp_Vec v3(const json& v) { return gp_Vec(v.at(0).get<double>(), v.at(1).get<double>(), v.at(2).get<double>()); }
json j3(const gp_XYZ& p) { return json::array({p.X(), p.Y(), p.Z()}); }

struct MeshBody {
  std::string node, key;
  std::shared_ptr<const scanfit::FitMesh> mesh;
  gp_Trsf trsf;  // body -> world
};

const Node& body_node(const Ctx& ctx, const std::string& id) {
  const Node* n = ctx.scene.node(id);
  if (!n || n->kind != Node::Kind::Body) throw Error("a fitted reference's body no longer exists");
  return *n;
}

bool mesh_node(const Ctx& ctx, const Node& n) { return n.representation == "mesh" || is_mesh_shape(ctx.key_shape(n.body_key)); }

MeshBody mesh_body(const Ctx& ctx, const std::string& id) {
  const Node& n = body_node(ctx, id);
  const TopoDS_Shape shape = ctx.key_shape(n.body_key);
  if (n.representation != "mesh" && !is_mesh_shape(shape)) throw Error(n.name + " is not a mesh: pick its faces and edges themselves");
  return {id, n.body_key, scanfit::cached_fit_mesh(n.body_key, shape, ctx.cancel), ctx.node_trsf(id)};
}

std::string digest(const json& fit) { return sha256_hex(fit.dump()).substr(0, 16); }

double tolerance_of(const Ctx& ctx, const json& fit, double default_deg) {
  if (!fit.contains("tolerance")) return default_deg * kPi / 180;
  const json& t = fit["tolerance"];
  const double a = t.is_number() ? t.get<double>() * kPi / 180 : ctx.angle(fit, "tolerance");  // a bare number is degrees
  if (!(a > 0) || a >= kPi / 2) throw Error("a fit's tolerance is an angle between 0 and 90 degrees");
  return a;
}

// The region a fit names: triangles given (ordinals, or [first, last] ranges), else grown from the seed.
std::vector<int> region_of(const Ctx& ctx, const MeshBody& b, const json& fit, bool follow) {
  const int count = b.mesh->triangles();
  if (fit.contains("triangles")) {
    std::vector<int> out;
    for (const auto& t : fit["triangles"]) {
      int lo, hi;
      if (t.is_array() && t.size() == 2) lo = t[0].get<int>(), hi = t[1].get<int>();
      else lo = hi = t.get<int>();
      if (lo < 0 || hi >= count || lo > hi) throw Error("a fitted region names triangles the mesh does not have (it has " + std::to_string(count) + ")");
      for (int i = lo; i <= hi; ++i) out.push_back(i);
    }
    if (out.empty()) throw Error("the fitted region has no triangles");
    return out;
  }
  if (!fit.contains("seed")) throw Error("a fitted region needs a seed triangle or a list of triangles");
  const int seed = fit["seed"].get<int>();
  if (seed < 0 || seed >= count) throw Error("the seed triangle is not on the mesh (it has " + std::to_string(count) + " triangles)");
  const double angle = tolerance_of(ctx, fit, follow ? 20 : 10);
  return follow ? scanfit::grow_round(*b.mesh, seed, angle) : scanfit::grow_region(*b.mesh, seed, angle, false);
}

// A point a points fit takes, in world coordinates: [x, y, z], {"point": [...]}, "point/x,y,z", a sketch point, or a vertex
// (of a mesh: its node).
gp_Pnt fit_point(const Ctx& ctx, const json& r) {
  if (r.is_array() && r.size() == 3 && r[0].is_number()) return p3(r);
  if (r.is_object() && r.contains("point") && !r.contains("body") && !r.contains("sketch")) return p3(r["point"]);
  if (r.is_string() && r.get<std::string>().rfind("point/", 0) == 0) {
    const Ref p = Ref::parse(r.get<std::string>());
    return gp_Pnt(p.point[0], p.point[1], p.point[2]);
  }
  if (r.is_object() && r.contains("sketch")) {
    Frame frame;
    const Sketch sk = ctx.sketch(r["sketch"].get<std::string>(), &frame);
    const SkPoint* p = sk.point(r.value("point", 0));
    if (!p) throw Error("a picked sketch point no longer exists");
    const Vec3 w = frame.to_world(p->x, p->y);
    return gp_Pnt(w[0], w[1], w[2]);
  }
  if (const auto m = mesh_point(ctx, r)) return *m;
  const ResolvedRef v = ctx.resolve(r);
  if (v.sub.IsNull() || v.sub.ShapeType() != TopAbs_VERTEX) throw Error("a fit's points are points, sketch points or vertices");
  return BRep_Tool::Pnt(TopoDS::Vertex(v.sub));
}

json quality_json(const scanfit::Quality& q) { return q.to_json(); }

// The fit computed now: a result in the body's coordinates when there is a body, else in world coordinates.
json compute_fit(const Ctx& ctx, const json& fit, bool want_axis) {
  ctx.check_cancel();
  std::string of = fit.value("of", std::string());
  const bool body = fit.contains("body") && fit["body"].is_string();
  json r = {{"for", digest(fit)}};
  auto plane_result = [&](const scanfit::Plane& p) {
    r["of"] = "plane";
    r["origin"] = j3(p.origin.XYZ());
    r["normal"] = j3(p.normal.XYZ());
    r["x"] = j3(p.x.XYZ());
    r["quality"] = quality_json(p.q);
  };
  auto line_result = [&](const scanfit::Line& l, const std::string& kind) {
    r["of"] = kind;
    r["origin"] = j3(l.origin.XYZ());
    r["dir"] = j3(l.dir.XYZ());
    if (l.radius > 0) r["radius"] = l.radius;
    r["quality"] = quality_json(l.q);
  };
  if (of == "intersection") {  // two planes of any kind, each resolved as they are now: world coordinates
    const json& planes = fit.value("planes", json());
    if (!planes.is_array() || planes.size() != 2) throw Error("an intersection fit takes \"planes\": [plane, plane]");
    scanfit::Plane a, b;
    for (int k = 0; k < 2; ++k) {
      const Frame f = ctx.plane(planes[size_t(k)]);
      scanfit::Plane& p = k ? b : a;
      p.origin = gp_Pnt(f.origin[0], f.origin[1], f.origin[2]);
      const Vec3 n = f.normal();
      p.normal = gp_Dir(n[0], n[1], n[2]);
      p.x = gp_Dir(f.x[0], f.x[1], f.x[2]);
      if (planes[size_t(k)].is_object() && planes[size_t(k)].contains("fit")) {
        const json& res = planes[size_t(k)].value("result", json());
        if (res.contains("quality")) {
          const json& q = res["quality"];
          p.q.rms = q.value("rms_mm", 0.0), p.q.max = q.value("max_mm", 0.0), p.q.used = q.value("used", 0), p.q.total = q.value("total", 0);
          p.q.extent = q.value("extent_mm", 0.0), p.q.grade = q.value("grade", std::string());
        }
      }
      if (p.q.grade.empty()) p.q.grade = "good", p.q.extent = 1;  // an exact plane
    }
    line_result(scanfit::plane_intersection(a, b), "intersection");
    r["frame"] = "world";
    return r;
  }
  if (fit.contains("points")) {
    std::vector<gp_Pnt> pts;
    for (const auto& p : fit["points"]) pts.push_back(fit_point(ctx, p));
    if (of.empty()) of = want_axis ? "line" : "plane";
    gp_Trsf to_body;
    if (body) to_body = ctx.node_trsf(fit["body"].get<std::string>()).Inverted();  // kept on the body: it moves with it
    for (auto& p : pts) p.Transform(to_body);
    if (of == "line") line_result(scanfit::fit_line(pts), "line");
    else if (of == "plane" || of == "normal") {
      plane_result(scanfit::fit_plane(pts));
      if (of == "normal") r["of"] = "normal";
    } else throw Error("a points fit is a plane or a line, not \"" + of + "\"");
    r["frame"] = body ? "body" : "world";
    if (body) r["key"] = body_node(ctx, fit["body"].get<std::string>()).body_key;
    return r;
  }
  if (!body) throw Error("a fit needs a mesh body (\"body\") with a seed or triangles, or \"points\", or two \"planes\"");
  const MeshBody b = mesh_body(ctx, fit["body"].get<std::string>());
  r["frame"] = "body";
  r["key"] = b.key;
  if (of.empty()) of = want_axis ? "auto" : "plane";
  if (of == "scan_axes") {
    const scanfit::ScanAxes axes = scanfit::scan_axes(*b.mesh, ctx.cancel);
    const scanfit::ScanFrame f = scanfit::scan_frame(*b.mesh, axes, fit.value("base", 0), fit.value("turn", 0), fit.value("origin", std::string("auto")));
    r["of"] = "scan_axes";
    r["origin"] = j3(f.origin.XYZ());
    r["normal"] = j3(f.normal.XYZ());
    r["x"] = j3(f.x.XYZ());
    r["method"] = f.method;
    r["bases"] = f.bases;
    r["turns"] = f.turns;
    r["share"] = f.share;
    r["quality"] = quality_json(f.q);
    return r;
  }
  if (fit.contains("vertices")) {  // nodes by ordinal ([first, last] ranges): a point cloud's points, or a mesh's
    std::vector<gp_Pnt> pts;
    const int count = int(b.mesh->nodes.size());
    for (const auto& v : fit["vertices"]) {
      int lo, hi;
      if (v.is_array() && v.size() == 2) lo = v[0].get<int>(), hi = v[1].get<int>();
      else lo = hi = v.get<int>();
      if (lo < 0 || hi >= count || lo > hi) throw Error("a fitted region names points the body does not have (it has " + std::to_string(count) + ")");
      for (int i = lo; i <= hi; ++i) pts.push_back(b.mesh->nodes[size_t(i)]);
    }
    if (of == "auto") of = "normal";
    if (of == "line") line_result(scanfit::fit_line(pts), "line");
    else if (of == "plane" || of == "normal") {
      plane_result(scanfit::fit_plane(pts));
      if (of == "normal") r["of"] = "normal";
    } else throw Error("points given by \"vertices\" fit a plane, a normal or a line, not \"" + of + "\"");
    return r;
  }
  if (b.mesh->tri.empty() && of != "scan_axes") throw Error("a point cloud has no triangles to grow a region on: name its points with \"vertices\"");
  if (of == "plane" || of == "normal") {
    plane_result(scanfit::fit_plane_region(*b.mesh, region_of(ctx, b, fit, false)));
    r["region"] = r["quality"]["total"];
    if (of == "normal") r["of"] = "normal";
    return r;
  }
  if (of == "wall") {
    line_result(scanfit::wall_direction(*b.mesh, region_of(ctx, b, fit, false)), "wall");
    return r;
  }
  if (of == "cylinder") {
    line_result(scanfit::fit_cylinder_region(*b.mesh, region_of(ctx, b, fit, true)), "cylinder");
    return r;
  }
  if (of == "auto") {
    // An axis picked on a scan: the round face's axis when the region curves (a hole, a boss), else the flat's normal.
    const scanfit::Plane flat = scanfit::fit_plane_region(*b.mesh, region_of(ctx, b, fit, false));
    if (flat.q.grade != "good") {
      try {
        const scanfit::Line c = scanfit::fit_cylinder_region(*b.mesh, region_of(ctx, b, fit, true));
        if (c.q.grade != "poor") {
          line_result(c, "cylinder");
          return r;
        }
      } catch (const Error&) {
      }
    }
    plane_result(flat);
    r["of"] = "normal";
    return r;
  }
  throw Error("a fit is of plane, normal, wall, cylinder, line, intersection or scan_axes, not \"" + of + "\"");
}

bool result_current(const Ctx& ctx, const json& fit, const json& result) {
  if (!result.is_object() || result.value("for", std::string()) != digest(fit) || !result.contains("origin")) return false;
  if (result.value("frame", std::string()) == "world") return !fit.contains("points") && fit.value("of", std::string()) != "intersection";  // those are cheap: always now
  if (!fit.contains("body") || !fit["body"].is_string()) return false;
  const Node* n = ctx.scene.node(fit["body"].get<std::string>());
  return n && n->kind == Node::Kind::Body && n->body_key == result.value("key", std::string());
}

// The result as it stands now, in world coordinates.
json world_result(const Ctx& ctx, const json& in, bool want_axis) {
  const json& fit = in.at("fit");
  if (!fit.is_object()) throw Error("\"fit\" is an object: {body, seed | triangles | points, ...}");
  json r = result_current(ctx, fit, in.value("result", json())) ? in["result"] : compute_fit(ctx, fit, want_axis);
  if (r.value("frame", std::string()) == "body") {
    const gp_Trsf t = ctx.node_trsf(fit.at("body").get<std::string>());
    if (t.Form() != gp_Identity) {
      r["origin"] = j3(p3(r["origin"]).Transformed(t).XYZ());
      for (const char* k : {"normal", "x", "dir"})
        if (r.contains(k)) r[k] = j3(v3(r[k]).Transformed(t).XYZ());
    }
  }
  return r;
}

}  // namespace

std::optional<gp_Pnt> mesh_point(const Ctx& ctx, const json& r) {
  if (!(r.is_object() && r.contains("body")) && !r.is_string()) return std::nullopt;
  Ref ref;
  try {
    ref = Ref::from_json(r);
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (ref.kind != Ref::Kind::Vertex) return std::nullopt;
  const Node* n = ctx.scene.node(ref.body);
  if (!n || n->kind != Node::Kind::Body || !mesh_node(ctx, *n)) return std::nullopt;
  const MeshBody b = mesh_body(ctx, ref.body);
  if (ref.index < 0 || ref.index >= int(b.mesh->nodes.size())) throw Error("a picked vertex is not on the mesh");
  return b.mesh->nodes[size_t(ref.index)].Transformed(b.trsf);
}

Frame fitted_plane(const Ctx& ctx, const json& in) {
  const json r = world_result(ctx, in, false);
  const std::string of = r.value("of", std::string());
  if (!r.contains("normal")) throw Error("a fitted " + of + " is not a plane: use it where an axis is asked for");
  const gp_Vec x = v3(r.value("x", json::array({1, 0, 0})));
  return plane_through(p3(r["origin"]), v3(r["normal"]), r.contains("x") ? &x : nullptr);
}

gp_Ax1 fitted_axis(const Ctx& ctx, const json& in) {
  const json r = world_result(ctx, in, true);
  if (r.contains("dir")) return gp_Ax1(p3(r["origin"]), gp_Dir(v3(r["dir"])));
  return gp_Ax1(p3(r["origin"]), gp_Dir(v3(r["normal"])));  // a plane's normal through its centre
}

bool complete_fits(const Ctx& ctx, json& v, bool want_axis) {
  bool changed = false;
  if (v.is_object()) {
    if (v.contains("fit") && v["fit"].is_object()) {
      if (!result_current(ctx, v["fit"], v.value("result", json()))) {
        json r = compute_fit(ctx, v["fit"], want_axis);
        if (v.value("result", json()) != r) {
          v["result"] = std::move(r);
          changed = true;
        }
      }
      if (v["fit"].contains("planes"))
        for (auto& p : v["fit"]["planes"]) changed = complete_fits(ctx, p, false) || changed;
      return changed;
    }
    for (auto& [k, c] : v.items())
      if (k != "hint" && k != "result") changed = complete_fits(ctx, c, want_axis) || changed;
  } else if (v.is_array()) {
    for (auto& c : v) changed = complete_fits(ctx, c, want_axis) || changed;
  }
  return changed;
}

bool complete_feature_fits(const Ctx& ctx, const std::string& kind, json& inputs) {
  const FeatureSpec* spec = feature_spec(kind);
  bool changed = false;
  if (!spec || !inputs.is_object()) return false;
  for (const auto& in : spec->inputs)
    if (inputs.contains(in.name)) changed = complete_fits(ctx, inputs[in.name], in.type == "axis") || changed;
  return changed;
}

namespace {
Ctx plain_ctx(const Document& doc, const Scene& scene, const ParamTable& params, const std::map<std::string, TopoDS_Shape>& fresh, const Cancel& cancel) {
  return Ctx{doc, params, scene, fresh, cancel};
}
ParamTable params_of(const Scene& scene) {
  std::vector<ParamDef> defs;
  for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return ParamTable(defs, scene.units);
}
}  // namespace

json fit_reference(const Document& doc, const Scene& scene, json ref, const std::string& as, const Cancel& cancel) {
  const ParamTable params = params_of(scene);
  const std::map<std::string, TopoDS_Shape> fresh;
  const Ctx ctx = plain_ctx(doc, scene, params, fresh, cancel);
  if (!ref.is_object() || !ref.contains("fit")) throw Error("a fitted reference is {\"fit\": {...}}");
  complete_fits(ctx, ref, as == "axis");
  json out = ref;
  json world = world_result(ctx, ref, as == "axis");
  world.erase("for");
  out["world"] = world;  // for showing only: never stored (the app and agents read it, ops keep "result")
  return out;
}

json scan_axes_report(const Document& doc, const Scene& scene, const std::string& body, const Cancel& cancel) {
  const ParamTable params = params_of(scene);
  const std::map<std::string, TopoDS_Shape> fresh;
  const Ctx ctx = plain_ctx(doc, scene, params, fresh, cancel);
  const MeshBody b = mesh_body(ctx, body);
  const scanfit::ScanAxes axes = scanfit::scan_axes(*b.mesh, cancel);
  json bases = json::array();
  for (size_t i = 0; i < axes.bases.size(); ++i) {
    ctx.check_cancel();
    const auto& s = axes.bases[i];
    const auto turns = scanfit::scan_turns(*b.mesh, axes, int(i));
    json t = json::array();
    for (const auto& d : turns) t.push_back(j3(d.Transformed(b.trsf).XYZ()));
    bases.push_back({{"base", i},
                     {"normal", j3(s.normal.Transformed(b.trsf).XYZ())},
                     {"origin", j3(s.origin.Transformed(b.trsf).XYZ())},
                     {"share", s.share},
                     {"quality", s.q.to_json()},
                     {"turns", t}});
  }
  const json from = {{"fit", {{"body", body}, {"of", "scan_axes"}, {"base", 0}, {"turn", 0}, {"origin", "auto"}}}};
  return {{"body", body},
          {"method", axes.method},
          {"bases", bases},
          {"align_inputs",
           {{"bodies", json::array({{{"body", body}, {"kind", "body"}}})},
            {"from", "plane"},
            {"from_plane", from},
            {"to", "plane"},
            {"to_plane", {{"base", "xy"}}},
            {"frame", true}}},
          {"note", "Align with these inputs stands the scan on XY with base 0 and turn 0; change fit.base / fit.turn to try the others "
                   "(\"bases\" lists them; each base's \"turns\" are its X directions), flip to stand it the other way up."}};
}

}  // namespace opad::design
