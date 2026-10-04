// Design checks (TODO 10 B13, B17).
#include "opad/checks.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
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
#include <array>
#include <functional>
#include <map>
#include <set>

#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/mass.hpp"

namespace opad {
namespace {

// The solid bodies a check looks at: the selected nodes (components stand for their bodies), or every visible one.
// The print check also takes meshes (STL, 3MF...), which it checks per triangle.
std::vector<std::string> solid_bodies(const Scene& scene, const json& args, bool meshes = false) {
  std::vector<std::string> out;
  auto add = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (n && n->kind == Node::Kind::Body && !n->body_missing && (n->representation == "solid" || (meshes && n->representation == "mesh")) &&
        std::find(out.begin(), out.end(), id) == out.end())
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

double volume_of(const TopoDS_Shape& s) { return volume_properties(s).mass; }

size_t arg_size(const json& args, const char* key, size_t fallback, size_t most) {
  if (!args.contains(key)) return fallback;
  const long long v = args[key].get<long long>();
  return static_cast<size_t>(std::clamp<long long>(v, 0, static_cast<long long>(most)));
}

// The print check of a mesh body: the questions of the B-rep check asked per triangle. A mesh has no faces to name,
// so neighbouring triangles with the same finding make one region, reported once with its triangles (their ordinals
// are the body's face references) so the panel can highlight it. Wall thickness: from each triangle's centre along
// the inward normal, looking only min_wall ahead (a grid of the triangles keeps that local), counting only a
// triangle that faces back (the wall's other side), not the next wall of a concave corner.
json print_mesh(const TopoDS_Shape& shape, const gp_Vec& up, double steep, double min_wall, const std::function<bool()>& cancelled, size_t& findings) {
  struct Tri {
    gp_Pnt p[3];
    gp_Vec n;
    double area = 0;
    int v[3] = {0, 0, 0};
  };
  std::vector<Tri> tris;
  Bnd_Box box;
  BRepBndLib::Add(shape, box, Standard_True);
  if (box.IsVoid()) return {{"contact_area_mm2", 0}, {"overhangs", json::array()}, {"thin_walls", json::array()}, {"thin_features", json::array()}};
  const double weld = std::max(1e-7, box.CornerMin().Distance(box.CornerMax()) * 1e-7);
  std::map<std::array<long long, 3>, int> welded;
  auto vertex = [&](const gp_Pnt& p) {
    const std::array<long long, 3> key{std::llround(p.X() / weld), std::llround(p.Y() / weld), std::llround(p.Z() / weld)};
    return welded.emplace(key, int(welded.size())).first->second;
  };
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
    TopLoc_Location loc;
    const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc);
    if (mesh.IsNull()) continue;
    const bool reversed = f.Current().Orientation() == TopAbs_REVERSED;
    for (int i = 1; i <= mesh->NbTriangles(); ++i) {
      int a, b, c;
      mesh->Triangle(i).Get(a, b, c);
      if (reversed) std::swap(b, c);
      Tri t;
      const int nodes[3] = {a, b, c};
      for (int k = 0; k < 3; ++k) {
        t.p[k] = mesh->Node(nodes[k]).Transformed(loc.Transformation());
        t.v[k] = vertex(t.p[k]);
      }
      t.n = gp_Vec(t.p[0], t.p[1]).Crossed(gp_Vec(t.p[0], t.p[2]));
      t.area = t.n.Magnitude() / 2;
      if (t.area > 1e-18) t.n /= 2 * t.area;
      tris.push_back(t);  // degenerate ones too: ordinals are positions
    }
  }
  double plate = 1e300;
  for (const auto& t : tris)
    for (const auto& p : t.p) plate = std::min(plate, gp_Vec(p.XYZ()).Dot(up));
  const double tol = 1e-4 * std::max(1.0, std::fabs(plate));
  // Triangles sharing an edge are neighbours.
  std::map<std::pair<int, int>, std::vector<int>> edges;
  for (int i = 0; i < int(tris.size()); ++i)
    for (int k = 0; k < 3; ++k) edges[std::minmax(tris[size_t(i)].v[k], tris[size_t(i)].v[(k + 1) % 3])].push_back(i);
  auto regions = [&](const std::vector<char>& marked) {
    std::vector<int> parent(tris.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = int(i);
    std::function<int(int)> root = [&](int i) { return parent[size_t(i)] == i ? i : parent[size_t(i)] = root(parent[size_t(i)]); };
    for (const auto& [edge, list] : edges)
      for (size_t k = 1; k < list.size(); ++k)
        if (marked[size_t(list[0])] && marked[size_t(list[k])]) parent[size_t(root(list[k]))] = root(list[0]);
    std::map<int, std::vector<int>> out;
    for (int i = 0; i < int(tris.size()); ++i)
      if (marked[size_t(i)]) out[root(i)].push_back(i);
    std::vector<std::vector<int>> list;
    for (auto& [r, members] : out) list.push_back(std::move(members));
    return list;
  };
  auto ordinals = [](const std::vector<int>& members) {
    json faces = json::array();
    for (size_t k = 0; k < members.size() && k < 2000; ++k) faces.push_back(members[k]);
    return faces;
  };

  double contact = 0;
  std::vector<char> overhang(tris.size(), 0);
  for (size_t i = 0; i < tris.size(); ++i) {
    const Tri& t = tris[i];
    if (t.area <= 1e-18) continue;
    const double facing_down = -t.n.Dot(up);
    bool onPlate = facing_down > 1 - 1e-6;
    for (const auto& p : t.p) onPlate = onPlate && gp_Vec(p.XYZ()).Dot(up) - plate < tol;
    if (onPlate) contact += t.area;
    else if (facing_down > steep) overhang[i] = 1;
  }
  std::vector<std::pair<double, json>> found;
  for (const auto& members : regions(overhang)) {
    double worst = 0, area = 0;
    for (int i : members) {
      worst = std::max(worst, -tris[size_t(i)].n.Dot(up));
      area += tris[size_t(i)].area;
    }
    if (area < 1e-6) continue;
    found.push_back({area, {{"face", members.front()}, {"faces", ordinals(members)}, {"triangles", members.size()},
                            {"overhang_deg", std::asin(std::min(1.0, worst)) * 180.0 / M_PI}, {"area_mm2", area}}});
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  json overhangs = json::array();
  for (auto& [area, item] : found) overhangs.push_back(std::move(item));

  json thin = json::array();
  if (min_wall > 0 && !tris.empty()) {
    // A uniform grid of the triangles, cells sized for a few dozen triangles each, laid out as offsets + indices.
    const gp_Pnt lo = box.CornerMin(), hi = box.CornerMax();
    const double ex = std::max(hi.X() - lo.X(), 1e-9), ey = std::max(hi.Y() - lo.Y(), 1e-9), ez = std::max(hi.Z() - lo.Z(), 1e-9);
    double cell = std::cbrt(ex * ey * ez / std::max(1.0, double(tris.size()) / 16));
    cell = std::max({cell, std::max({ex, ey, ez}) / 256, 1e-6});
    const int nx = std::max(1, int(std::ceil(ex / cell))), ny = std::max(1, int(std::ceil(ey / cell))), nz = std::max(1, int(std::ceil(ez / cell)));
    auto clampi = [](double v, int n) { return std::clamp(int(std::floor(v)), 0, n - 1); };
    auto range = [&](const gp_Pnt& a, const gp_Pnt& b, int r[6]) {
      r[0] = clampi((std::min(a.X(), b.X()) - lo.X()) / cell, nx); r[1] = clampi((std::max(a.X(), b.X()) - lo.X()) / cell, nx);
      r[2] = clampi((std::min(a.Y(), b.Y()) - lo.Y()) / cell, ny); r[3] = clampi((std::max(a.Y(), b.Y()) - lo.Y()) / cell, ny);
      r[4] = clampi((std::min(a.Z(), b.Z()) - lo.Z()) / cell, nz); r[5] = clampi((std::max(a.Z(), b.Z()) - lo.Z()) / cell, nz);
    };
    const size_t cells = size_t(nx) * size_t(ny) * size_t(nz);
    std::vector<uint32_t> start(cells + 1, 0), slots;
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<uint32_t> fill(pass ? start.begin() : start.end(), pass ? start.end() - 1 : start.end());
      for (size_t i = 0; i < tris.size(); ++i) {
        const Tri& t = tris[i];
        gp_Pnt a(std::min({t.p[0].X(), t.p[1].X(), t.p[2].X()}), std::min({t.p[0].Y(), t.p[1].Y(), t.p[2].Y()}), std::min({t.p[0].Z(), t.p[1].Z(), t.p[2].Z()}));
        gp_Pnt b(std::max({t.p[0].X(), t.p[1].X(), t.p[2].X()}), std::max({t.p[0].Y(), t.p[1].Y(), t.p[2].Y()}), std::max({t.p[0].Z(), t.p[1].Z(), t.p[2].Z()}));
        int r[6];
        range(a, b, r);
        for (int z = r[4]; z <= r[5]; ++z)
          for (int y = r[2]; y <= r[3]; ++y)
            for (int x = r[0]; x <= r[1]; ++x) {
              const size_t c = (size_t(z) * size_t(ny) + size_t(y)) * size_t(nx) + size_t(x);
              if (pass == 0) ++start[c + 1];
              else slots[fill[c]++] = uint32_t(i);
            }
      }
      if (pass == 0) {
        for (size_t c = 0; c < cells; ++c) start[c + 1] += start[c];
        slots.resize(start[cells]);
      }
    }
    std::vector<char> thinMark(tris.size(), 0);
    std::vector<double> thickness(tris.size(), 1e300);
    std::vector<uint32_t> seen(tris.size(), 0);
    uint32_t stamp = 0;
    const size_t stride = std::max<size_t>(1, tris.size() / 200000);  // a ray per triangle up to 200k of them
    for (size_t i = 0; i < tris.size(); i += stride) {
      if ((i / stride) % 4096 == 0 && cancelled && cancelled()) throw Error("cancelled");
      const Tri& t = tris[i];
      if (t.area <= 1e-18) continue;
      const gp_Pnt centre((t.p[0].XYZ() + t.p[1].XYZ() + t.p[2].XYZ()) / 3);
      const gp_Vec d = -t.n;
      const gp_Pnt s = centre.Translated(d * 1e-4), e = centre.Translated(d * min_wall);
      int r[6];
      range(s, e, r);
      ++stamp;
      double nearest = 1e300;
      for (int z = r[4]; z <= r[5]; ++z)
        for (int y = r[2]; y <= r[3]; ++y)
          for (int x = r[0]; x <= r[1]; ++x) {
            const size_t c = (size_t(z) * size_t(ny) + size_t(y)) * size_t(nx) + size_t(x);
            for (uint32_t k = start[c]; k < start[c + 1]; ++k) {
              const uint32_t j = slots[k];
              if (j == i || seen[j] == stamp) continue;
              seen[j] = stamp;
              const Tri& o = tris[j];
              if (o.area <= 1e-18 || o.n.Dot(t.n) > -0.5) continue;  // only the wall's back, facing the other way
              bool neighbour = false;  // a fold or knife edge right beside it is not a wall
              for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) neighbour = neighbour || o.v[a] == t.v[b];
              if (neighbour) continue;
              // Moller-Trumbore
              const gp_Vec e1(o.p[0], o.p[1]), e2(o.p[0], o.p[2]), pv = d.Crossed(e2);
              const double det = e1.Dot(pv);
              if (std::abs(det) < 1e-18) continue;
              const gp_Vec tv(o.p[0], s);
              const double u = tv.Dot(pv) / det;
              if (u < 0 || u > 1) continue;
              const gp_Vec qv = tv.Crossed(e1);
              const double v = d.Dot(qv) / det;
              if (v < 0 || u + v > 1) continue;
              const double h = e2.Dot(qv) / det + 1e-4;
              if (h > 1e-4 && h < nearest) nearest = h;
            }
          }
      if (nearest < min_wall) {
        thinMark[i] = 1;
        thickness[i] = nearest;
      }
    }
    std::vector<std::pair<double, json>> walls;
    for (const auto& members : regions(thinMark)) {
      int worst = members.front();
      for (int i : members)
        if (thickness[size_t(i)] < thickness[size_t(worst)]) worst = i;
      const Tri& t = tris[size_t(worst)];
      const gp_Pnt at((t.p[0].XYZ() + t.p[1].XYZ() + t.p[2].XYZ()) / 3);
      walls.push_back({thickness[size_t(worst)], {{"face", worst}, {"faces", ordinals(members)}, {"triangles", members.size()},
                                                  {"thickness_mm", thickness[size_t(worst)]}, {"point", {at.X(), at.Y(), at.Z()}}}});
    }
    std::sort(walls.begin(), walls.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [value, item] : walls) thin.push_back(std::move(item));
  }
  // Long lists help nobody: the worst regions first, the rest counted.
  json shownOverhangs = json::array(), shownThin = json::array();
  for (size_t k = 0; k < overhangs.size() && k < 50; ++k) shownOverhangs.push_back(overhangs[k]);
  for (size_t k = 0; k < thin.size() && k < 50; ++k) shownThin.push_back(thin[k]);
  findings += overhangs.size() + thin.size();
  json out = {{"contact_area_mm2", contact}, {"overhangs", shownOverhangs}, {"thin_walls", shownThin}, {"thin_features", json::array()},
              {"mesh", true}, {"triangles", tris.size()}};
  if (overhangs.size() > shownOverhangs.size()) out["more_overhangs"] = overhangs.size() - shownOverhangs.size();
  if (thin.size() > shownThin.size()) out["more_thin_walls"] = thin.size() - shownThin.size();
  return out;
}

}  // namespace

json check_interference(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled) {
  std::vector<std::string> bodies = solid_bodies(scene, args);
  if (args.contains("against") && args["against"].is_array() && !args["against"].empty())  // hidden ones too: they are named
    for (const auto& b : solid_bodies(scene, {{"select", args["against"]}}))
      if (std::find(bodies.begin(), bodies.end(), b) == bodies.end()) bodies.push_back(b);
  return interference_of(scene, bodies, args, [&](const std::string& id) { return node_world_shape(doc, scene, id); },
                         [&](const std::string& id) { return node_world_bbox(doc, scene, id); }, cancelled);
}

json interference_of(const Scene& scene, const std::vector<std::string>& bodies, const json& args, const std::function<TopoDS_Shape(const std::string&)>& shape_of,
                     const std::function<Bnd_Box(const std::string&)>& box_of, const std::function<bool()>& cancelled) {
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
  // Only pairs across: one body of `against` (a board, components standing for their bodies) and one of the rest (its enclosure).
  std::set<std::string> against;
  if (args.contains("against")) {
    if (!args["against"].is_array()) throw Error("against takes node ids: [a, ...]");
    for (const auto& id : args["against"]) {
      if (!id.is_string() || !scene.node(id.get<std::string>())) throw Error("unknown node " + id.dump());
      for (const auto& b : scene.bodies_under(id.get<std::string>())) against.insert(b);
    }
  }
  // Candidates: boxes (the fast view boxes, never smaller than the bodies) that meet, grown by the clearance.
  struct Box { std::string id; double lo[3], hi[3]; };
  std::vector<Box> boxes;
  for (const auto& id : bodies) {
    const Bnd_Box b = box_of(id);
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
      if (args.contains("against") && against.count(boxes[i].id) == against.count(boxes[j].id)) continue;
      const auto pair = std::minmax(boxes[i].id, boxes[j].id);
      if (ignored.count(pair)) {
        ++skipped;
        continue;
      }
      if (candidates.size() >= max_pairs) {
        truncated = true;
        break;
      }
      candidates.push_back(against.count(pair.first) ? std::make_pair(pair.second, pair.first) : std::make_pair(pair.first, pair.second));  // against's body second
    }
  // Exact work only on the candidates: the common solid, else (with a clearance) the closest distance.
  json findings = json::array();
  size_t overlaps = 0, close = 0;
  for (const auto& [a, b] : candidates) {
    if (cancelled && cancelled()) throw Error("cancelled");
    const TopoDS_Shape sa = shape_of(a), sb = shape_of(b);
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
      const json d = shape_distance(sa, sb, cancelled);
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
  if (args.contains("against")) out["against"] = against.size();
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
      // Not SetCoord(index, value): OCCT writes (&x)[index - 1], which clang may assume only reaches x.
      const double s1 = s[0] == '-' ? -1.0 : 1.0;
      up = gp_Vec(s[1] == 'x' ? s1 : 0.0, s[1] == 'y' ? s1 : 0.0, s[1] == 'z' ? s1 : 0.0);
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
  const std::vector<std::string> bodies = solid_bodies(scene, args, true);
  const size_t offset = arg_size(args, "offset", 0, 1u << 30), limit = arg_size(args, "limit", 25, 100);
  json items = json::array();
  size_t findings = 0;
  for (size_t bi = offset; bi < bodies.size() && items.size() < limit; ++bi) {
    const std::string& id = bodies[bi];
    const TopoDS_Shape shape = node_world_shape(doc, scene, id);
    if (scene.node(id)->representation == "mesh") {
      json item = print_mesh(shape, up, steep, min_wall, cancelled, findings);
      item["id"] = id;
      item["name"] = scene.node(id)->name;
      items.push_back(std::move(item));
      continue;
    }
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
      const double area = area_properties(face).mass;
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
