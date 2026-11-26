// Point cloud commands (the CLI, Python, the headless and live MCP): `cloud` reads a cloud (info, paged points, regions,
// slices with their profile) and `cloud_sketch` turns a slice into sketch geometry; exports to PLY / XYZ / PTS / CSV / E57.
// The cloud features themselves go through `feature` (cloud_outliers, cloud_downsample, cloud_normals, cloud_crop,
// cloud_slice). Coordinates in and out are world mm.
#include <algorithm>
#include <cmath>

#include "opad/commands.hpp"
#include "opad/design/cloud.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/point_cloud.hpp"
#include "opad/scene.hpp"

namespace opad::cloud {

// A cloud body's points in world orientation, the placement's translation kept as their offset (exact far out).
Points world_points(const Document& doc, const Scene& scene, const std::string& node, bool attributes) {
  const Node* n = scene.node(node);
  if (!n || n->kind != Node::Kind::Body) throw Error("no such body: " + node);
  const TopoDS_Shape proto = body_shape(doc, n->body_key);
  if (!is_cloud(proto)) throw Error(n->name + " is not a point cloud");
  Points p = from_shape(proto, attributes);
  Mat4 w = scene.world(node);
  const Vec3 t = {w.at(0, 3), w.at(1, 3), w.at(2, 3)};
  w.at(0, 3) = w.at(1, 3) = w.at(2, 3) = 0;
  p.transform(w);
  p.offset = t;
  return p;
}

json export_points(const Document& doc, const Scene& scene, const std::filesystem::path& out, const std::vector<std::string>& select) {
  std::vector<std::string> ids;
  auto take = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (n && n->kind == Node::Kind::Body && n->representation == "points" && !n->body_missing && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  };
  if (select.empty())
    for (const auto& id : scene.all_bodies()) take(id);
  else
    for (const auto& s : select) {
      take(s);
      for (const auto& id : scene.bodies_under(s)) take(id);
    }
  if (ids.empty()) throw Error("PLY, XYZ, PTS, CSV and E57 export point clouds: the selection has none");
  Points all;
  bool colour = true, normals = true, scalar = true;
  std::vector<Points> parts;
  for (const auto& id : ids) {
    parts.push_back(world_points(doc, scene, id, true));
    colour = colour && parts.back().colored(), normals = normals && parts.back().has_normals(), scalar = scalar && parts.back().has_scalar();
  }
  all.offset = parts.front().offset;
  for (auto& p : parts) {
    for (size_t i = 0; i < p.xyz.size(); ++i) all.xyz.push_back(float(double(p.xyz[i]) + p.offset[i % 3] - all.offset[i % 3]));
    if (colour) all.rgb.insert(all.rgb.end(), p.rgb.begin(), p.rgb.end());
    if (normals) all.normals.insert(all.normals.end(), p.normals.begin(), p.normals.end());
    if (scalar) all.scalar.insert(all.scalar.end(), p.scalar.begin(), p.scalar.end()), all.scalar_name = p.scalar_name;
    p = Points();
  }
  if (out.has_parent_path()) std::filesystem::create_directories(out.parent_path());
  write_file(out, all);
  return {{"file", path_to_utf8(out)}, {"bodies", ids.size()}, {"points", all.size()}, {"attributes", meta_of(all)["attributes"]}};
}

namespace {

Document& need(Document* d) {
  if (!d) throw Error("this command needs a document: pass \"doc\"");
  return *d;
}

Vec3 vec3(const json& j, const char* what) {
  if (!j.is_array() || j.size() != 3) throw Error(std::string(what) + " is [x, y, z]");
  return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

json parse_if_text(const json& v) {
  if (!v.is_string()) return v;
  json j = json::parse(v.get<std::string>(), nullptr, false);
  if (j.is_discarded()) throw Error("expected JSON, got: " + v.get<std::string>());
  return j;
}

// A plane argument: {origin, normal[, x]} in world mm, or a plane input ({"base"} | {"face"} | {"feature"}); moved along its
// normal by offset_along.
Frame plane_arg(const Document& doc, const Scene& scene, const json& a) {
  const json plane = parse_if_text(a.value("plane", json{{"base", "xy"}}));
  Frame f;
  if (plane.contains("normal")) {
    const Vec3 o = plane.contains("origin") ? vec3(plane["origin"], "origin") : Vec3{0, 0, 0}, n = vec3(plane["normal"], "normal");
    const Vec3 x = plane.contains("x") ? vec3(plane["x"], "x") : Vec3{0, 0, 0};
    // The frame slice() makes: its x as given (laid onto the plane) or a world axis laid onto it.
    Points none;
    none.xyz = {0, 0, 0};
    f = slice(none, o, n, 1, x).frame;
  } else {
    f = design::resolve_plane(doc, scene, plane);
  }
  const double along = a.value("offset_along", 0.0);
  const Vec3 n = f.normal();
  for (size_t k = 0; k < 3; ++k) f.origin[k] += n[k] * along;
  return f;
}

// The region an argument names: a region reference, a box, a sphere or a band about a plane (world mm).
Region region_arg(const Document& doc, const Scene& scene, const Points& p, const json& a, std::string* how = nullptr) {
  auto local = [&](const Vec3& v) { return Vec3{v[0] - p.offset[0], v[1] - p.offset[1], v[2] - p.offset[2]}; };
  if (a.contains("region")) {
    if (how) *how = "region";
    return region_of_ref(parse_if_text(a["region"]), p.size());
  }
  if (a.contains("box")) {
    const json b = parse_if_text(a["box"]);
    if (!b.is_array() || b.size() != 2) throw Error("box is [[x, y, z], [x, y, z]]");
    Vec3 lo = vec3(b[0], "box corner"), hi = vec3(b[1], "box corner");
    for (size_t k = 0; k < 3; ++k)
      if (lo[k] > hi[k]) std::swap(lo[k], hi[k]);
    if (how) *how = "box";
    return in_box(p, local(lo), local(hi));
  }
  if (a.contains("sphere")) {
    const json s = parse_if_text(a["sphere"]);
    if (how) *how = "sphere";
    return in_sphere(p, local(vec3(s.value("centre", s.value("center", json())), "sphere centre")), s.value("radius", 0.0));
  }
  if (a.contains("plane")) {
    const Frame f = plane_arg(doc, scene, a);
    if (how) *how = "band";
    return in_band(p, local(f.origin), f.normal(), a.value("band", 0.25));
  }
  throw Error("name the points: region (a reference), box, sphere or plane with band");
}

json frame_json(const Frame& f) {
  json j = f.to_json();
  j["normal"] = f.normal();
  return j;
}

}  // namespace

void register_cloud_commands(const std::function<void(const commands::CommandInfo&, commands::Handler)>& add) {
  add({"cloud",
       "Point clouds (raw scans), a read: info (count, box, typical spacing, attributes), points (paged, optionally of a region), region (the points in a box, "
       "a sphere or a band about a plane: a region reference for Crop points and fitted references, with its centre and box) and slice (the points within a "
       "band either side of a plane, on it in the plane's frame, and the profile's polylines). World mm",
       {{"doc", "path"}, {"body", "uuid - a point cloud body"}, {"action", "info|points|region|slice"},
        {"region", "object - {\"body\", \"kind\": \"region\", \"vertices\": [[first, last], ...]}"}, {"box", "[[x,y,z],[x,y,z]] - mm"},
        {"sphere", "object - {centre: [x,y,z], radius}"}, {"plane", "object - {origin, normal[, x]} mm, or a plane input {base|face|feature}"},
        {"offset_along", "number - mm along the plane's normal"}, {"band", "number - mm either side (default 0.25)"},
        {"profile", "bool - slice: chain the points into polylines (default true)"}, {"tolerance", "number - profile thinning, mm (0: the spacing)"},
        {"gap", "number - profile: a jump that ends a chain, mm (0: automatic)"}, {"offset", "int - first point of the page"}, {"limit", "int - points a page (default 1000, at most 100000)"}},
       false},
      [](Document* d, const json& a) {
        Document& doc = need(d);
        const Scene scene = resolve(doc);
        const std::string body = a.value("body", "");
        if (body.empty()) throw Error("cloud: pass \"body\", a point cloud");
        const std::string action = a.value("action", "info");
        const size_t offset = size_t(std::max<int64_t>(0, a.value("offset", int64_t(0))));
        const size_t limit = size_t(std::clamp<int64_t>(a.value("limit", int64_t(1000)), 0, 100000));
        Points p = world_points(doc, scene, body, action != "slice");
        json out = {{"body", body}, {"action", action}};
        if (action == "info") {
          out.update(describe(p, true));
          const auto b = bounds(p);
          out["bounds"] = {{"min", {b[0][0] + p.offset[0], b[0][1] + p.offset[1], b[0][2] + p.offset[2]}},
                           {"max", {b[1][0] + p.offset[0], b[1][1] + p.offset[1], b[1][2] + p.offset[2]}}};
          if (const Node* n = scene.node(body)) out["name"] = n->name;
          return out;
        }
        if (action == "points") {
          std::string how;
          const bool some = a.contains("region") || a.contains("box") || a.contains("sphere") || a.contains("plane");
          const Region r = some ? region_arg(doc, scene, p, a, &how) : Region();
          out.update(points_json(p, some ? &r : nullptr, offset, limit));
          return out;
        }
        if (action == "region") {
          std::string how;
          const Region r = region_arg(doc, scene, p, a, &how);
          out["reference"] = region_ref(body, r);
          out["count"] = r.count();
          out["by"] = how;
          if (!r.empty()) {
            const Points in = subset(p, r);
            const auto b = bounds(in);
            double c[3] = {0, 0, 0};
            for (size_t i = 0; i < in.size(); ++i)
              for (size_t k = 0; k < 3; ++k) c[k] += in.xyz[3 * i + k];
            out["centre"] = {c[0] / double(in.size()) + p.offset[0], c[1] / double(in.size()) + p.offset[1], c[2] / double(in.size()) + p.offset[2]};
            out["bounds"] = {{"min", {b[0][0] + p.offset[0], b[0][1] + p.offset[1], b[0][2] + p.offset[2]}},
                             {"max", {b[1][0] + p.offset[0], b[1][1] + p.offset[1], b[1][2] + p.offset[2]}}};
            if (limit) out["sample"] = points_json(p, &r, offset, std::min<size_t>(limit, 200))["points"];
          }
          return out;
        }
        if (action == "slice") {
          const Frame f = plane_arg(doc, scene, a);
          const double band = a.value("band", 0.25);
          const Slice s = slice(p, f.origin, f.normal(), band, f.x);
          out["frame"] = frame_json(s.frame);
          out["band"] = band;
          out["count"] = s.indices.size();
          json uv = json::array(), xyz = json::array(), idx = json::array();
          auto r4 = [](double v) { return std::round(v * 1e4) / 1e4; };
          for (size_t k = offset; k < s.indices.size() && size_t(uv.size()) < limit; ++k) {
            const double u = s.uv[2 * k], v = s.uv[2 * k + 1];
            uv.push_back({r4(u), r4(v)});
            const Vec3 w = s.frame.to_world(u, v);
            xyz.push_back({r4(w[0]), r4(w[1]), r4(w[2])});
            idx.push_back(s.indices[k]);
          }
          out["offset"] = offset;
          out["indices"] = idx;
          out["uv"] = uv;
          out["points"] = xyz;
          if (offset + uv.size() < s.indices.size()) out["next_offset"] = offset + uv.size();
          if (a.value("profile", true) && s.indices.size() >= 3) {
            const auto lines = profile(s.uv, a.value("gap", 0.0), a.value("tolerance", 0.0));
            json pl = json::array();
            for (const auto& line : lines) {
              json one = json::array();
              for (const auto& q : line) one.push_back({r4(q[0]), r4(q[1])});
              pl.push_back({{"closed", line.size() > 3 && line.front() == line.back()}, {"uv", one}});
            }
            out["polylines"] = pl;
          }
          return out;
        }
        throw Error("cloud: action is info, points, region or slice");
      });

  add({"cloud_sketch",
       "Turn a cloud's slice into sketch geometry: its points chained into polylines, thinned to the tolerance and drawn as lines on the slice's plane "
       "(a body made by Slice points), or on the plane given with the points within band of it. Returns the sketch id",
       {{"doc", "path"}, {"body", "uuid - a slice (or any cloud with plane)"}, {"plane", "object - a plane input {base|face|feature}; default the slice's own"},
        {"band", "number - mm either side, with plane (default 0.25)"}, {"tolerance", "number - mm (0: the spacing)"}, {"gap", "number - mm (0: automatic)"},
        {"name", "string"}, {"by", "string"}},
       true},
      [](Document* d, const json& a) {
        Document& doc = need(d);
        const Scene scene = resolve(doc);
        json report;
        const json plane = a.contains("plane") ? parse_if_text(a["plane"]) : json();
        json op = design::slice_sketch_op(doc, scene, a.value("body", ""), plane, a.value("band", 0.25), a.value("tolerance", 0.0), a.value("gap", 0.0),
                                          a.value("name", ""), &report);
        json out = design::apply_ops(doc, {op}, a.value("by", ""));
        out["sketch_id"] = out["ids"][0];
        out["profile"] = report;
        return out;
      });
}

}  // namespace opad::cloud
