// Design checks (TODO 10 B13, B17).
#include "opad/checks.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>
#include <set>

#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

namespace opad {
namespace {

// The solid bodies a check looks at: the selected nodes (components stand for their bodies), or every visible one.
std::vector<std::string> solid_bodies(const Scene& scene, const json& args) {
  std::vector<std::string> out;
  auto add = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (n && n->kind == Node::Kind::Body && !n->body_missing && n->representation == "solid" && std::find(out.begin(), out.end(), id) == out.end())
      out.push_back(id);
  };
  if (args.contains("select") && args["select"].is_array() && !args["select"].empty()) {
    for (const auto& s : args["select"]) {
      const std::string id = s.get<std::string>();
      if (!scene.node(id)) throw Error("unknown node " + id);
      for (const auto& b : scene.bodies_under(id)) add(b);
    }
  } else {
    for (const auto& b : scene.all_bodies())
      if (scene.effectively_visible(b)) add(b);
  }
  return out;
}

double volume_of(const TopoDS_Shape& s) {
  GProp_GProps g;
  BRepGProp::VolumeProperties(s, g);
  return g.Mass();
}

size_t arg_size(const json& args, const char* key, size_t fallback, size_t most) {
  if (!args.contains(key)) return fallback;
  const long long v = args[key].get<long long>();
  return static_cast<size_t>(std::clamp<long long>(v, 0, static_cast<long long>(most)));
}

}  // namespace

json check_interference(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled) {
  const std::vector<std::string> bodies = solid_bodies(scene, args);
  const double clearance = args.value("clearance_mm", 0.0);
  if (!(clearance >= 0) || !std::isfinite(clearance)) throw Error("clearance_mm must be zero or more");
  const size_t max_pairs = arg_size(args, "max_pairs", 20000, 200000);
  // Pairs meant to overlap, either way round; a component stands for every body under it.
  std::set<std::pair<std::string, std::string>> ignored;
  for (const auto& pair : args.value("ignore", json::array())) {
    if (!pair.is_array() || pair.size() != 2) throw Error("ignore takes pairs: [[a, b], ...]");
    for (const auto& a : scene.bodies_under(pair[0].get<std::string>()))
      for (const auto& b : scene.bodies_under(pair[1].get<std::string>())) ignored.insert(std::minmax(a, b));
  }
  // Candidates: boxes (the fast view boxes, never smaller than the bodies) that meet, grown by the clearance.
  struct Box { std::string id; double lo[3], hi[3]; };
  std::vector<Box> boxes;
  for (const auto& id : bodies) {
    const Bnd_Box b = node_world_bbox(doc, scene, id);
    if (b.IsVoid()) continue;
    Box box{id, {}, {}};
    b.Get(box.lo[0], box.lo[1], box.lo[2], box.hi[0], box.hi[1], box.hi[2]);
    for (int k = 0; k < 3; ++k) box.lo[k] -= clearance / 2, box.hi[k] += clearance / 2;
    boxes.push_back(box);
  }
  std::sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.lo[0] < b.lo[0] || (a.lo[0] == b.lo[0] && a.id < b.id); });
  std::vector<std::pair<std::string, std::string>> candidates;
  size_t skipped = 0;
  bool truncated = false;
  for (size_t i = 0; i < boxes.size() && !truncated; ++i)
    for (size_t j = i + 1; j < boxes.size() && boxes[j].lo[0] <= boxes[i].hi[0]; ++j) {
      if (boxes[j].lo[1] > boxes[i].hi[1] || boxes[i].lo[1] > boxes[j].hi[1] || boxes[j].lo[2] > boxes[i].hi[2] || boxes[i].lo[2] > boxes[j].hi[2]) continue;
      const auto pair = std::minmax(boxes[i].id, boxes[j].id);
      if (ignored.count(pair)) {
        ++skipped;
        continue;
      }
      if (candidates.size() >= max_pairs) {
        truncated = true;
        break;
      }
      candidates.push_back(pair);
    }
  // Exact work only on the candidates: the common solid, else (with a clearance) the closest distance.
  json findings = json::array();
  size_t overlaps = 0, close = 0;
  for (const auto& [a, b] : candidates) {
    if (cancelled && cancelled()) throw Error("cancelled");
    const TopoDS_Shape sa = node_world_shape(doc, scene, a), sb = node_world_shape(doc, scene, b);
    json entry = {{"a", a}, {"b", b}, {"a_name", scene.node(a)->name}, {"b_name", scene.node(b)->name}};
    BRepAlgoAPI_Common common(sa, sb);
    double overlap = 0;
    TopoDS_Shape region;
    if (common.IsDone()) {
      region = common.Shape();
      bool solid = false;
      for (TopExp_Explorer ex(region, TopAbs_SOLID); ex.More() && !solid; ex.Next()) solid = true;
      if (solid) overlap = volume_of(region);
    }
    // Touching faces leave no volume; a sliver below a millionth of the smaller body is the kernel's rounding.
    const double smaller = std::min(volume_of(sa), volume_of(sb));
    if (overlap > std::max(1e-9, 1e-6 * smaller)) {
      entry["kind"] = "interference";
      entry["volume_mm3"] = overlap;
      entry["bbox"] = bbox_to_json(tight_bbox(region));
      findings.push_back(entry);
      ++overlaps;
      continue;
    }
    if (clearance > 0) {
      Ref ra, rb;
      ra.body = a;
      rb.body = b;
      const json d = measure_distance(doc, scene, ra, rb, cancelled);
      const double distance = d.value("value", 1e300);
      if (distance < clearance) {
        entry["kind"] = "clearance";
        entry["distance_mm"] = distance;
        for (const char* k : {"point_a", "point_b"})
          if (d.contains(k)) entry[k] = d[k];
        findings.push_back(entry);
        ++close;
      }
    }
  }
  // Worst first: the largest overlaps, then the smallest gaps.
  std::stable_sort(findings.begin(), findings.end(), [](const json& x, const json& y) {
    const bool xi = x["kind"] == "interference", yi = y["kind"] == "interference";
    if (xi != yi) return xi;
    return xi ? x["volume_mm3"].get<double>() > y["volume_mm3"].get<double>() : x["distance_mm"].get<double>() < y["distance_mm"].get<double>();
  });
  const size_t offset = arg_size(args, "offset", 0, 1u << 30), limit = arg_size(args, "limit", 50, 500);
  json page = json::array();
  for (size_t i = offset; i < findings.size() && page.size() < limit; ++i) page.push_back(findings[i]);
  json out = {{"check", "interference"}, {"bodies", bodies.size()}, {"candidate_pairs", candidates.size()}, {"ignored_pairs", skipped},
              {"interferences", overlaps}, {"clearance_mm", clearance}, {"too_close", close}, {"total", findings.size()},
              {"offset", offset}, {"items", page}, {"status", overlaps ? "interference" : close ? "too_close" : "clear"}};
  if (truncated) out["truncated"] = "more candidate pairs than max_pairs; check a smaller selection";
  if (offset + page.size() < findings.size()) out["next_offset"] = offset + page.size();
  return out;
}

json check_print(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled) {
  // The build direction: which way layers stack.
  gp_Vec up(0, 0, 1);
  if (args.contains("build_direction")) {
    const json& d = args["build_direction"];
    if (d.is_string()) {
      const std::string s = d.get<std::string>();
      if (s.size() != 2 || (s[0] != '+' && s[0] != '-') || s[1] < 'x' || s[1] > 'z') throw Error("build_direction is +x, -x, +y, -y, +z, -z or [x, y, z]");
      up = gp_Vec(0, 0, 0);
      up.SetCoord(s[1] - 'x' + 1, s[0] == '-' ? -1.0 : 1.0);
    } else {
      up = gp_Vec(d.at(0).get<double>(), d.at(1).get<double>(), d.at(2).get<double>());
    }
  }
  if (up.Magnitude() < 1e-12) throw Error("build_direction must not be zero");
  up.Normalize();
  const double overhang = args.value("overhang_deg", 45.0), min_wall = args.value("min_wall_mm", 0.8);
  if (!(overhang >= 0 && overhang < 90)) throw Error("overhang_deg is from 0 up to 90");
  if (!(min_wall >= 0)) throw Error("min_wall_mm must be zero or more");
  const double steep = std::sin(overhang * M_PI / 180.0);  // a face facing down more than this is an overhang
  const std::vector<std::string> bodies = solid_bodies(scene, args);
  const size_t offset = arg_size(args, "offset", 0, 1u << 30), limit = arg_size(args, "limit", 25, 100);
  json items = json::array();
  size_t findings = 0;
  for (size_t bi = offset; bi < bodies.size() && items.size() < limit; ++bi) {
    const std::string& id = bodies[bi];
    const TopoDS_Shape shape = node_world_shape(doc, scene, id);
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    // The build plate: the lowest point of the body along the build direction.
    double plate = 1e300;
    for (TopExp_Explorer v(shape, TopAbs_VERTEX); v.More(); v.Next()) plate = std::min(plate, gp_Vec(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).XYZ()).Dot(up));
    const double tol = 1e-4 * std::max(1.0, std::fabs(plate));
    IntCurvesFace_ShapeIntersector rays;
    rays.Load(shape, 1e-7);
    json overhangs = json::array(), thin_walls = json::array(), thin_features = json::array();
    double contact = 0;
    size_t samples_left = 4000;
    for (int fi = 1; fi <= faces.Extent(); ++fi) {
      if (cancelled && cancelled()) throw Error("cancelled");
      const TopoDS_Face& face = TopoDS::Face(faces(fi));
      GProp_GProps g;
      BRepGProp::SurfaceProperties(face, g);
      const double area = g.Mass();
      if (area < 1e-12) continue;
      // Samples: a grid over the face's parameters, kept where they fall inside the face.
      double u0, u1, v0, v1;
      BRepTools::UVBounds(face, u0, u1, v0, v1);
      BRepGProp_Face props(face);
      BRepClass_FaceClassifier inside;
      double worst = 0, lowest = 1e300, thinnest = 1e300;
      gp_Pnt thin_at;
      int down = 0, total = 0;
      const int n = 6;
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
          const double u = u0 + (u1 - u0) * (i + 0.5) / n, v = v0 + (v1 - v0) * (j + 0.5) / n;
          inside.Perform(face, gp_Pnt2d(u, v), 1e-9);
          if (inside.State() != TopAbs_IN) continue;
          gp_Pnt p;
          gp_Vec normal;
          props.Normal(u, v, p, normal);  // outward, face orientation included
          if (normal.Magnitude() < 1e-12) continue;
          normal.Normalize();
          ++total;
          const double facing_down = -normal.Dot(up);
          lowest = std::min(lowest, gp_Vec(p.XYZ()).Dot(up));
          if (facing_down > steep) {
            ++down;
            worst = std::max(worst, facing_down);
          }
          // Wall thickness: along the inward normal to where the ray leaves the body again.
          if (min_wall > 0 && samples_left > 0) {
            --samples_left;
            const gp_Pnt start = p.Translated(-normal * 1e-4);
            rays.PerformNearest(gp_Lin(start, gp_Dir(-normal)), 0, 1e6);
            if (rays.IsDone() && rays.NbPnt() > 0) {
              const double t = rays.WParameter(1) + 1e-4;
              if (t < thinnest) thinnest = t, thin_at = p;
            }
          }
        }
      if (total == 0) continue;
      BRepAdaptor_Surface surf(face);
      const bool flat_down = surf.GetType() == GeomAbs_Plane && worst > 1 - 1e-9;
      if (flat_down && lowest - plate < tol) {
        contact += area;  // on the plate: carried by it, not an overhang
      } else if (down > 0) {
        overhangs.push_back({{"face", fi - 1}, {"overhang_deg", std::asin(std::min(1.0, worst)) * 180.0 / M_PI}, {"area_mm2", area * down / total}});
      }
      if (thinnest < min_wall) thin_walls.push_back({{"face", fi - 1}, {"thickness_mm", thinnest}, {"point", {thin_at.X(), thin_at.Y(), thin_at.Z()}}});
      // A narrow face (a rib's top, a fin): twice its area over its boundary length is about its width.
      GProp_GProps edges;
      BRepGProp::LinearProperties(face, edges);
      const double width = edges.Mass() > 0 ? 2 * area / edges.Mass() : 1e300;
      if (min_wall > 0 && width < min_wall) thin_features.push_back({{"face", fi - 1}, {"width_mm", width}});
    }
    findings += overhangs.size() + thin_walls.size() + thin_features.size();
    items.push_back({{"id", id}, {"name", scene.node(id)->name}, {"contact_area_mm2", contact}, {"overhangs", overhangs}, {"thin_walls", thin_walls},
                     {"thin_features", thin_features}});
  }
  json out = {{"check", "print"}, {"build_direction", {up.X(), up.Y(), up.Z()}}, {"overhang_deg", overhang}, {"min_wall_mm", min_wall},
              {"bodies", bodies.size()}, {"offset", offset}, {"items", items}, {"findings", findings}, {"status", findings ? "findings" : "clear"}};
  if (offset + items.size() < bodies.size()) out["next_offset"] = offset + items.size();
  return out;
}

}  // namespace opad
