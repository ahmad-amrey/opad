// Features that take mesh bodies (build_specs in features.cpp; opad/mesh_edit.hpp): Scale (solids too, per axis or to a
// size), Drop to bed, Split into parts, and, when a mesh is among the bodies, Combine and Split. Mesh to mesh, with no
// solid in between: results are mesh bodies (materialize stores their triangles). Mirror, the patterns and Move's copies
// map meshes in features.cpp itself.
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <gp_GTrsf.hxx>

#include <cmath>
#include <optional>

#include "engine.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh_edit.hpp"

namespace opad::design {

std::vector<std::string> feature_body_nodes(const Ctx& ctx, const json& refs);    // features.cpp (body_ids)
std::vector<std::string> feature_placed_nodes(const Ctx& ctx, const json& refs);  // features.cpp (placed_nodes)

namespace {

using meshops::Mesh;

// Inputs a caller left out take the spec's default (an agent or an older op names only what it changes).
double number_or(const Ctx& ctx, const json& in, const char* name, double def) { return in.contains(name) ? ctx.number(in, name) : def; }
double length_or(const Ctx& ctx, const json& in, const char* name, double def) { return in.contains(name) ? ctx.length(in, name) : def; }
int count_or(const Ctx& ctx, const json& in, const char* name, int def) { return in.contains(name) ? ctx.count(in, name) : def; }

meshops::Cancel cancel_of(const Ctx& ctx) {
  return [&ctx] { return ctx.cancel && ctx.cancel(); };
}

// A mesh body's triangles in world coordinates; a solid's meshed on a copy at `tolerance` (0: automatic).
Mesh world_mesh(const Ctx& ctx, const std::string& node, double tolerance = 0) {
  const TopoDS_Shape s = ctx.any_shape(node);
  return is_mesh_shape(s) ? meshops::from_shape(s) : meshops::mesh_solid_copy(s, tolerance);
}

std::string node_name(const Ctx& ctx, const std::string& id) {
  const Node* n = ctx.scene.node(id);
  return n ? n->name : id;
}

std::vector<std::string> picked(const Ctx& ctx, const json& refs) {
  std::vector<std::string> ids = feature_body_nodes(ctx, refs);
  if (ids.empty()) throw Error("pick at least one body");
  return ids;
}

bool any_mesh(const Ctx& ctx, const std::vector<std::string>& ids) {
  for (const auto& id : ids)
    if (ctx.is_mesh(id)) return true;
  return false;
}

std::vector<std::string> meshes_only(const Ctx& ctx, const json& refs, const char* what) {
  std::vector<std::string> ids = picked(ctx, refs);
  for (const auto& id : ids)
    if (!ctx.is_mesh(id)) throw Error(std::string(what) + " takes mesh bodies; " + node_name(ctx, id) + " is a solid");
  return ids;
}

std::array<double, 3> box_lo(const Bnd_Box& b) {
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  return {x0, y0, z0};
}
std::array<double, 3> box_hi(const Bnd_Box& b) {
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  return {x1, y1, z1};
}

// ---------------------------------------------------------------- Scale
Out scale(const Ctx& ctx, const json& in) {
  Out out;
  const std::vector<std::string> ids = picked(ctx, in.value("bodies", json()));
  const std::string mode = in.value("mode", "factor");
  std::vector<TopoDS_Shape> shapes;
  for (const auto& id : ids) shapes.push_back(ctx.any_shape(id));
  double k[3] = {1, 1, 1};
  if (mode == "factor" || mode.empty()) {
    k[0] = k[1] = k[2] = number_or(ctx, in, "factor", 2);
  } else if (mode == "percent") {
    k[0] = k[1] = k[2] = number_or(ctx, in, "percent", 100) / 100;
  } else if (mode == "axes") {
    k[0] = number_or(ctx, in, "factor_x", 1), k[1] = number_or(ctx, in, "factor_y", 1), k[2] = number_or(ctx, in, "factor_z", 1);
  } else if (mode == "size") {
    // Of the picked bodies' box together, as a slicer scales an object to a size.
    Bnd_Box all;
    for (const auto& s : shapes) all.Add(meshops::tight_box(s));
    if (all.IsVoid()) throw Error("the bodies have no size to scale");
    const auto lo = box_lo(all), hi = box_hi(all);
    const double want[3] = {length_or(ctx, in, "size_x", 0), length_or(ctx, in, "size_y", 0), length_or(ctx, in, "size_z", 0)};
    const char* axis = "XYZ";
    std::optional<double> first;
    for (int i = 0; i < 3; ++i) {
      if (want[i] < 0) throw Error(std::string("the ") + axis[i] + " size must not be negative");
      if (want[i] == 0) continue;
      const double now = hi[size_t(i)] - lo[size_t(i)];
      if (now <= 1e-9) throw Error(std::string("the bodies are flat in ") + axis[i] + ": give a size along another axis");
      k[i] = want[i] / now;
      if (!first) first = k[i];
    }
    if (!first) throw Error("give at least one size (X, Y or Z)");
    if (in.value("lock", true)) k[0] = k[1] = k[2] = *first;
  } else {
    throw Error("scale: mode must be factor, percent, axes or size");
  }
  for (double f : k)
    if (!(f > 0)) throw Error("the scale factors must be positive");
  if (k[0] == 1 && k[1] == 1 && k[2] == 1) throw Error("the scale does nothing (every factor is 1)");
  const std::string about = in.value("about", "origin");
  const bool uniform = k[0] == k[1] && k[1] == k[2];
  for (size_t i = 0; i < ids.size(); ++i) {
    ctx.check_cancel();
    const bool mesh = is_mesh_shape(shapes[i]);
    std::optional<Mesh> m;
    if (mesh) m = meshops::from_shape(shapes[i]);
    gp_XYZ pivot(0, 0, 0);
    if (about == "centre") {
      if (mesh) {
        const auto c = meshops::centroid(*m);
        pivot.SetCoord(c[0], c[1], c[2]);
      } else {
        GProp_GProps g;
        BRepGProp::VolumeProperties(shapes[i], g);
        pivot = g.CentreOfMass().XYZ();
      }
    } else if (about == "bed") {  // the middle of its bottom: it stays standing where it stood
      const Bnd_Box b = meshops::tight_box(shapes[i]);
      const auto lo = box_lo(b), hi = box_hi(b);
      pivot.SetCoord((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2]);
    } else if (about != "origin") {
      throw Error("scale: about must be origin, centre or bed");
    }
    gp_GTrsf g;
    g.SetValue(1, 1, k[0]);
    g.SetValue(2, 2, k[1]);
    g.SetValue(3, 3, k[2]);
    g.SetTranslationPart(gp_XYZ(pivot.X() * (1 - k[0]), pivot.Y() * (1 - k[1]), pivot.Z() * (1 - k[2])));
    if (mesh) {
      meshops::transform(*m, g);
      out.bodies.push_back({ids[i], meshops::to_shape(*m)});
    } else if (uniform) {
      gp_Trsf t;
      t.SetScale(gp_Pnt(pivot), k[0]);
      out.bodies.push_back({ids[i], BRepBuilderAPI_Transform(shapes[i], t, Standard_True).Shape()});
    } else {
      BRepBuilderAPI_GTransform gt(shapes[i], g, Standard_True);
      if (!gt.IsDone()) throw Error("scaling " + node_name(ctx, ids[i]) + " per axis failed");
      out.bodies.push_back({ids[i], gt.Shape()});
    }
  }
  out.extra["factors"] = {k[0], k[1], k[2]};
  return out;
}

// ---------------------------------------------------------------- Drop to bed
Out drop_to_bed(const Ctx& ctx, const json& in) {
  Out out;
  const bool centre = in.value("center", false);
  for (const auto& node : feature_placed_nodes(ctx, in.value("bodies", json()))) {
    ctx.check_cancel();
    Bnd_Box box;
    const Node* n = ctx.scene.node(node);
    if (!n) continue;
    const std::vector<std::string> bodies = n->kind == Node::Kind::Body ? std::vector<std::string>{node} : ctx.scene.bodies_under(node);
    for (const auto& b : bodies) {
      const Node* bn = ctx.scene.node(b);
      if (!bn || bn->body_missing) continue;
      TopoDS_Shape shape = ctx.key_shape(bn->body_key);  // a point cloud stands on the bed too (its nodes are its box)
      if (const gp_Trsf t = ctx.node_trsf(b); t.Form() != gp_Identity) shape = shape.Moved(TopLoc_Location(t));
      box.Add(meshops::tight_box(shape));
    }
    if (box.IsVoid()) continue;
    const auto lo = box_lo(box), hi = box_hi(box);
    const gp_Vec by(centre ? -(lo[0] + hi[0]) / 2 : 0.0, centre ? -(lo[1] + hi[1]) / 2 : 0.0, -lo[2]);
    if (by.Magnitude() < 1e-12) continue;
    gp_Trsf t;
    t.SetTranslation(by);
    out.placed.push_back({node, t});
  }
  if (out.placed.empty()) {
    if (feature_placed_nodes(ctx, in.value("bodies", json())).empty()) throw Error("pick at least one body");
    out.extra["already"] = true;  // every body already stands on the bed: nothing moves
  }
  return out;
}

// ---------------------------------------------------------------- Split into parts
std::vector<TopoDS_Shape> solids_in(const TopoDS_Shape& s) {
  std::vector<TopoDS_Shape> out;
  for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next()) out.push_back(e.Current());
  return out;
}

Out split_parts(const Ctx& ctx, const json& in) {
  Out out;
  bool any = false;
  for (const auto& id : picked(ctx, in.value("bodies", json()))) {
    ctx.check_cancel();
    const TopoDS_Shape s = ctx.any_shape(id);
    std::vector<TopoDS_Shape> parts;
    if (is_mesh_shape(s)) {
      for (const auto& p : meshops::components(meshops::from_shape(s))) parts.push_back(meshops::to_shape(p));
    } else {
      parts = solids_in(s);
    }
    if (parts.size() < 2) continue;
    if (parts.size() > 2000) throw Error(node_name(ctx, id) + " has " + std::to_string(parts.size()) + " separate pieces: more than 2000 bodies");
    any = true;
    out.bodies.push_back({id, parts.front()});
    for (size_t k = 1; k < parts.size(); ++k) out.bodies.push_back({"", parts[k], id});
  }
  if (!any) throw Error("each picked body is one piece already");
  return out;
}

// ---------------------------------------------------------------- Combine with a mesh
Out combine(const Ctx& ctx, const json& in, const std::vector<std::string>& targets, std::vector<std::string> tools) {
  Out out;
  const std::string op = in.value("operation", "join");
  if (op != "join" && op != "cut" && op != "intersect") throw Error("combine: operation must be join, cut or intersect");
  const double tol = length_or(ctx, in, "tolerance", 0);
  const auto cancel = cancel_of(ctx);
  json report = json::array();
  if (op == "join") {
    std::vector<Mesh> all;
    for (const auto& t : targets) all.push_back(world_mesh(ctx, t, tol));
    for (const auto& t : tools) all.push_back(world_mesh(ctx, t, tol));
    const Mesh r = meshops::union_all(all, cancel);
    if (r.empty()) throw Error("the join left nothing");
    out.bodies.push_back({targets.front(), meshops::to_shape(r)});
    for (size_t i = 1; i < targets.size(); ++i) out.removed.push_back(targets[i]);
    report.push_back({{"body", targets.front()}, {"triangles", r.triangles()}});
  } else {
    // The tools as one (BatchBoolean), then each target cut by it or intersected with it.
    std::vector<Mesh> parts;
    for (const auto& t : tools) parts.push_back(world_mesh(ctx, t, tol));
    const Mesh tool = parts.size() == 1 ? parts.front() : meshops::union_all(parts, cancel);
    bool any = false;
    for (const auto& target : targets) {
      ctx.check_cancel();
      const Mesh a = world_mesh(ctx, target, tol);
      const Mesh r = meshops::boolean(a, tool, op == "cut" ? meshops::Op::Subtract : meshops::Op::Intersect, cancel);
      if (r.empty()) {
        if (op == "intersect") continue;
        out.removed.push_back(target);
        any = true;
        continue;
      }
      if (op == "cut" && r.triangles() == a.triangles() && std::fabs(meshops::volume(r) - meshops::volume(a)) <= 1e-9 * std::max(1.0, std::fabs(meshops::volume(a))))
        continue;  // the tools miss it
      any = true;
      out.bodies.push_back({target, meshops::to_shape(r)});
      report.push_back({{"body", target}, {"triangles", r.triangles()}});
    }
    if (!any) throw Error(op == "cut" ? "the cut does not touch any body" : "the bodies do not intersect");
  }
  if (!in.value("keep_tools", false))
    for (const auto& t : tools) out.removed.push_back(t);
  out.extra["meshes"] = report;
  return out;
}

// ---------------------------------------------------------------- Split a mesh by a plane
Out split(const Ctx& ctx, const json& in, const std::vector<std::string>& ids) {
  Out out;
  const Frame frame = ctx.plane(in.value("plane", json()));
  const Vec3 n = frame.normal();
  const std::string keep = in.value("keep", "both");
  if (keep != "both" && keep != "above" && keep != "below") throw Error("split: keep must be both, above or below");
  const auto cancel = cancel_of(ctx);
  bool any = false, open = false;
  for (const auto& id : ids) {
    ctx.check_cancel();
    const TopoDS_Shape s = ctx.any_shape(id);
    if (!is_mesh_shape(s)) throw Error("Split cuts meshes and solids in separate steps: " + node_name(ctx, id) + " is a solid among meshes");
    const meshops::Cut cut = meshops::split_by_plane(meshops::from_shape(s), {frame.origin[0], frame.origin[1], frame.origin[2]}, {n[0], n[1], n[2]}, cancel);
    if (cut.above.empty() || cut.below.empty()) continue;  // the plane misses it
    any = true;
    open = open || !cut.capped;
    if (keep == "both") {
      out.bodies.push_back({id, meshops::to_shape(cut.above)});
      out.bodies.push_back({"", meshops::to_shape(cut.below), id});
    } else {
      out.bodies.push_back({id, meshops::to_shape(keep == "above" ? cut.above : cut.below)});
    }
  }
  if (!any) throw Error("the plane does not cut through the picked bodies");
  if (open) out.extra["uncapped"] = true;  // an open mesh (a scan) is cut without caps
  return out;
}

// ---------------------------------------------------------------- Repair and Simplify
Out repair(const Ctx& ctx, const json& in) {
  Out out;
  meshops::RepairOptions opt;
  opt.weld = length_or(ctx, in, "weld", 0);
  opt.fill_holes = in.value("fill_holes", true);
  opt.max_hole_edges = opt.fill_holes ? static_cast<size_t>(std::max(0, count_or(ctx, in, "max_hole_edges", 0))) : 0;
  opt.orient = in.value("orient", true);
  opt.remove_degenerate = opt.remove_duplicates = in.value("clean", true);
  if (opt.weld < 0) throw Error("the weld distance must not be negative");
  json report = json::array();
  for (const auto& id : meshes_only(ctx, in.value("bodies", json()), "Repair mesh")) {
    ctx.check_cancel();
    meshops::RepairResult r = meshops::repair(meshops::from_shape(ctx.any_shape(id)), opt, cancel_of(ctx));
    json j = r.to_json();
    j["body"] = id;
    report.push_back(std::move(j));
    out.bodies.push_back({id, meshops::to_shape(r.mesh)});
  }
  out.extra["repair"] = report;
  return out;
}

Out simplify(const Ctx& ctx, const json& in) {
  Out out;
  const std::string mode = in.value("mode", "ratio");
  json report = json::array();
  for (const auto& id : meshes_only(ctx, in.value("bodies", json()), "Simplify mesh")) {
    ctx.check_cancel();
    const Mesh m = meshops::from_shape(ctx.any_shape(id));
    meshops::SimplifyOptions opt;
    opt.keep_borders = in.value("keep_borders", true);
    if (mode == "ratio") {
      const double pc = number_or(ctx, in, "ratio", 50);
      if (!(pc > 0 && pc < 100)) throw Error("the percent of triangles to keep must be between 0 and 100");
      opt.target_triangles = std::max<size_t>(4, static_cast<size_t>(std::llround(m.triangles() * pc / 100)));
    } else if (mode == "count") {
      const int c = count_or(ctx, in, "count", 100000);
      if (c < 4) throw Error("keep at least 4 triangles");
      opt.target_triangles = static_cast<size_t>(c);
    } else if (mode == "tolerance") {
      opt.tolerance = length_or(ctx, in, "tolerance", 0.05);
      if (!(opt.tolerance > 0)) throw Error("the tolerance must be positive");
    } else {
      throw Error("simplify: mode must be ratio, count or tolerance");
    }
    double err = 0;
    const Mesh r = meshops::simplify(m, opt, &err, cancel_of(ctx));
    report.push_back({{"body", id}, {"before", m.triangles()}, {"after", r.triangles()}, {"error_mm", err}});
    out.bodies.push_back({id, meshops::to_shape(r)});
  }
  out.extra["simplify"] = report;
  return out;
}

}  // namespace

// Handles the kinds above; for Combine and Split only when a mesh is among the bodies (else nullopt: the solid paths).
std::optional<Out> compute_mesh_feature(const Ctx& ctx, const std::string& kind, const json& in) {
  if (kind == "scale") return scale(ctx, in);
  if (kind == "drop_to_bed") return drop_to_bed(ctx, in);
  if (kind == "split_parts") return split_parts(ctx, in);
  if (kind == "mesh_repair") return repair(ctx, in);
  if (kind == "mesh_simplify") return simplify(ctx, in);
  if (kind == "combine") {
    const std::vector<std::string> targets = feature_body_nodes(ctx, in.value("target", json()));
    std::vector<std::string> tools = feature_body_nodes(ctx, in.value("tools", json()));
    std::vector<std::string> all = targets;
    all.insert(all.end(), tools.begin(), tools.end());
    if (targets.empty() || !any_mesh(ctx, all)) return std::nullopt;
    const bool pickedTools = !tools.empty();
    for (const auto& t : targets) tools.erase(std::remove(tools.begin(), tools.end(), t), tools.end());
    if (tools.empty()) throw Error(pickedTools ? "the tool bodies are the target bodies: pick other bodies as tools" : "pick at least one tool body");
    return combine(ctx, in, targets, tools);
  }
  if (kind == "split") {
    const std::vector<std::string> ids = feature_body_nodes(ctx, in.value("bodies", json()));
    if (ids.empty() || !any_mesh(ctx, ids)) return std::nullopt;
    return split(ctx, in, ids);
  }
  return std::nullopt;
}

}  // namespace opad::design
