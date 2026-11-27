// The point cloud features (build_specs in features.cpp): Remove outliers, Downsample, Estimate normals and Crop points
// change a cloud in place; Slice points makes new clouds lying on their planes. A cloud's points stay in its own frame (a
// far scan keeps its placement), so each result is handed back placed by its node (Out::Body shape located), which the
// engine keeps as it is (materialize).
#include <TopLoc_Location.hxx>

#include "engine.hpp"
#include "opad/cloud_edit.hpp"
#include "opad/geometry.hpp"
#include "opad/point_cloud.hpp"

namespace opad::design {
namespace {

struct CloudBody {
  std::string node, name;
  cloud::Points points;
  gp_Trsf trsf;  // its frame -> world
};

CloudBody cloud_body(const Ctx& ctx, const std::string& node) {
  const Node* n = ctx.scene.node(node);
  if (!n || n->kind != Node::Kind::Body) throw Error("a referenced point cloud no longer exists");
  const TopoDS_Shape s = ctx.key_shape(n->body_key);
  if (!cloud::is_cloud(s)) throw Error(n->name + " is not a point cloud");
  return {node, n->name, cloud::from_shape(s), ctx.node_trsf(node)};
}

TopoDS_Shape placed(const cloud::Points& p, const gp_Trsf& t) {
  const TopoDS_Shape s = cloud::to_shape(p);
  return t.Form() == gp_Identity ? s : s.Moved(TopLoc_Location(t));
}

std::vector<std::string> body_nodes(const json& refs) {
  if (!refs.is_array() || refs.empty()) throw Error("pick at least one point cloud");
  std::vector<std::string> out;
  for (const auto& r : refs) out.push_back(r.is_object() && r.contains("body") ? r["body"].get<std::string>() : Ref::from_json(r).body);
  return out;
}

Vec3 vec(const gp_XYZ& v) { return {v.X(), v.Y(), v.Z()}; }

}  // namespace

Out compute_cloud_feature(const Ctx& ctx, const std::string& kind, const json& in) {
  Out out;
  const cloud::Cancel cancel = [&] { return ctx.cancel && ctx.cancel(); };
  json counts = json::array();
  auto keep = [&](CloudBody& b, cloud::Points p, const char* empty) {
    if (p.empty()) throw Error(std::string(empty) + " (" + b.name + ")");
    counts.push_back({{"body", b.node}, {"before", b.points.size()}, {"after", p.size()}});
    out.bodies.push_back({b.node, placed(p, b.trsf), ""});
  };
  if (kind == "cloud_outliers") {
    const int k = std::max(1, ctx.count(in, "neighbours"));
    const double ratio = ctx.number(in, "ratio");
    for (const auto& id : body_nodes(in.value("bodies", json()))) {
      ctx.check_cancel();
      CloudBody b = cloud_body(ctx, id);
      const cloud::Region drop = cloud::outliers(b.points, k, ratio, cancel);
      keep(b, cloud::without(b.points, drop), "removing the outliers would leave no points");
    }
  } else if (kind == "cloud_downsample") {
    const double voxel = ctx.length(in, "voxel");
    if (voxel <= 0) throw Error("the voxel size must be above zero");
    for (const auto& id : body_nodes(in.value("bodies", json()))) {
      ctx.check_cancel();
      CloudBody b = cloud_body(ctx, id);
      keep(b, cloud::voxel_downsample(b.points, voxel, cancel), "the cloud has no points");
    }
  } else if (kind == "cloud_normals") {
    const int k = std::clamp(ctx.count(in, "neighbours"), 3, 64);
    for (const auto& id : body_nodes(in.value("bodies", json()))) {
      ctx.check_cancel();
      CloudBody b = cloud_body(ctx, id);
      cloud::Points p = b.points;
      cloud::estimate_normals(p, k, cancel);
      keep(b, std::move(p), "the cloud has no points");
    }
  } else if (kind == "cloud_crop") {
    const json ref = in.value("region", json());
    if (!ref.is_object() || !ref.contains("body")) throw Error("pick a region of points to crop to");
    CloudBody b = cloud_body(ctx, ref["body"].get<std::string>());
    const cloud::Region r = cloud::region_of_ref(ref, b.points.size());
    if (r.empty()) throw Error("the region has no points");
    const bool inside = in.value("keep", std::string("inside")) != "outside";
    keep(b, inside ? cloud::subset(b.points, r) : cloud::without(b.points, r), inside ? "the region has no points" : "the region holds every point");
  } else if (kind == "cloud_edit") {
    const auto ids = body_nodes(in.value("bodies", json()));
    CloudBody b = cloud_body(ctx, ids.front());
    const cloud::Edit edit = cloud::Edit::from_json(in.value("edit", json::object()));
    if (edit.empty()) {
      keep(b, b.points, "the cloud has no points");
    } else {
      cloud::Points edited = cloud::apply(b.points, edit);
      keep(b, std::move(edited), "the edit deletes every point of the cloud");
    }
    out.extra["edit"] = edit.summary();
  } else if (kind == "cloud_slice") {
    const auto ids = body_nodes(in.value("bodies", json()));
    CloudBody b = cloud_body(ctx, ids.front());
    const Frame world = ctx.plane(in.value("plane", json{{"base", "xy"}}));
    const double offset = ctx.length(in, "offset"), band = ctx.length(in, "band"), spacing = ctx.length(in, "spacing");
    const int count = std::clamp(ctx.count(in, "count"), 1, 500);
    if (band <= 0) throw Error("the band must be wider than zero");
    if (count > 1 && spacing == 0) throw Error("several slices need a spacing");
    // The plane in the cloud's own frame.
    const Frame local = world.transformed(mat_from_trsf(b.trsf.Inverted()));
    const Vec3 n = local.normal();
    json frames = json::array();
    for (int i = 0; i < count; ++i) {
      ctx.check_cancel();
      const double at = offset + spacing * i;
      const Vec3 o = {local.origin[0] + n[0] * at, local.origin[1] + n[1] * at, local.origin[2] + n[2] * at};
      cloud::Slice s = cloud::slice(b.points, o, n, band, local.x);
      if (s.indices.empty()) throw Error("no points lie within the band of slice " + std::to_string(i + 1) + " (" + b.name + "): move the plane or widen the band");
      // The slice's points laid onto its plane, with their colours.
      cloud::Points p;
      p.offset = b.points.offset;
      const size_t m = s.indices.size();
      p.xyz.resize(m * 3);
      const Vec3 lo = {o[0] - b.points.offset[0], o[1] - b.points.offset[1], o[2] - b.points.offset[2]};
      for (size_t k = 0; k < m; ++k)
        for (size_t d = 0; d < 3; ++d) p.xyz[3 * k + d] = float(lo[d] + s.uv[2 * k] * s.frame.x[d] + s.uv[2 * k + 1] * s.frame.y[d]);
      if (b.points.colored())
        for (uint32_t idx : s.indices) p.rgb.insert(p.rgb.end(), b.points.rgb.begin() + std::ptrdiff_t(3 * size_t(idx)), b.points.rgb.begin() + std::ptrdiff_t(3 * size_t(idx) + 3));
      p.offset = {0, 0, 0};
      out.bodies.push_back({"", placed(p, b.trsf), ""});
      Frame f = s.frame;
      f = f.transformed(mat_from_trsf(b.trsf));
      json fr = frame_result(f);
      fr["points"] = m;
      frames.push_back(fr);
    }
    out.extra["slices"] = frames;
    return out;
  } else {
    throw Error("unknown point cloud feature: " + kind);
  }
  out.extra["points"] = counts;
  return out;
}

}  // namespace opad::design

#include "opad/design/cloud.hpp"
#include "opad/design/sketch.hpp"

namespace opad::design {

json slice_sketch_op(const Document& doc, const Scene& scene, const std::string& body, const json& plane, double band, double tolerance,
                     double gap, const std::string& name, json* report) {
  const Node* n = scene.node(body);
  if (!n || n->kind != Node::Kind::Body) throw Error("no such body: " + body);
  const TopoDS_Shape proto = body_shape(doc, n->body_key);
  if (!cloud::is_cloud(proto)) throw Error(n->name + " is not a point cloud");
  const Mat4 world = scene.world(body);
  cloud::Points p = cloud::from_shape(proto, false);
  // The slice's frame: the feature that made it, else the plane given.
  Frame frame;
  bool found = false;
  if (plane.is_null() || (plane.is_object() && plane.empty())) {
    if (const Feature* f = scene.feature(n->source_op); f && f->kind == "cloud_slice") {
      const json bodies = f->result.value("bodies", json::array()), slices = f->result.value("slices", json::array());
      for (size_t i = 0; i < bodies.size() && i < slices.size(); ++i)
        if (bodies[i].value("id", "") == body) frame = Frame::from_json(slices[i]), found = true;
    }
    if (!found) throw Error(n->name + " is not a slice: give the plane to cut it with");
  } else {
    frame = resolve_plane(doc, scene, plane);
  }
  // The points in world coordinates (only those near the plane are needed: they are taken in the cloud's own frame first).
  const Frame local = frame.transformed(world.inverse());
  const double width = found ? std::max(band, 1e-3) + 1e-3 : band;
  if (width <= 0) throw Error("the band must be wider than zero");
  cloud::Slice s = cloud::slice(p, local.origin, local.normal(), found ? 1e6 : width, local.x);
  if (s.indices.size() < 3) throw Error("too few points of " + n->name + " lie on the plane to sketch over " + std::to_string(p.size()) + " " + local.to_json().dump() + (found ? " found" : ""));
  const auto lines = cloud::profile(s.uv, gap, tolerance);
  if (lines.empty()) throw Error("the points of " + n->name + " make no profile to sketch: widen the band or give a larger gap");
  Sketch sk;
  size_t count = 0, points = 0;
  for (const auto& line : lines) {
    const bool closed = line.size() > 3 && line.front() == line.back();
    std::vector<int> ids;
    for (size_t i = 0; i + (closed ? 1 : 0) < line.size(); ++i) ids.push_back(sk.add_point(line[i][0], line[i][1]));
    if (closed) ids.push_back(ids.front());
    for (size_t i = 1; i < ids.size(); ++i) sk.add_line(ids[i - 1], ids[i]), ++count;
    points += ids.size() - (closed ? 1 : 0);
  }
  if (report) *report = {{"polylines", lines.size()}, {"lines", count}, {"points", points}, {"slice_points", s.indices.size()}};
  return make_sketch_op(name.empty() ? next_name(scene, n->name + " profile ") : name, {{"frame", frame.to_json()}}, sk.to_json());
}

}  // namespace opad::design
