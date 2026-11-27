// Align (bodies, components, sketches): a point, line or face put onto another point, line or plane; moved, turned or
// both; placed as they are (body keys kept); sketches re-planed and still on the face they were aligned to. And Move
// without a copy places its bodies the same way. Point pairs: the least-squares rigid move from picked pairs (a scan's
// points onto its CAD body's, a cloud's onto a sketch's), a stray pair left out, and the refusals.
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/document.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/point_pairs.hpp"
#include "opad/scan_fit.hpp"
#include "opad/scene.hpp"

using namespace opad;
using namespace opad::design;

namespace {
json feature_cmd(Document& doc, const std::string& kind, const json& inputs) { return commands::run("feature", {{"kind", kind}, {"inputs", inputs}}, &doc); }
json body_ref(const std::string& node) { return {{"body", node}, {"kind", "body"}}; }

// A box from (x, y, z) up, its sides along the axes.
std::string box(Document& doc, double x, double y, double z, double l, double w, double h) {
  return feature_cmd(doc, "box", {{"plane", {{"origin", {x, y, z}}, {"normal", {0, 0, 1}}}}, {"length", std::to_string(l) + " mm"}, {"width", std::to_string(w) + " mm"},
                                  {"height", std::to_string(h) + " mm"}, {"centered", false}})["body_ids"][0];
}

// The planar face of a body facing `n` (in the world, as it is now): "uuid/face/N".
std::string face(const Document& doc, const std::string& body, const gp_Dir& n) {
  const Scene s = resolve(doc);
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(node_world_shape(doc, s, body), TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    const TopoDS_Face f = TopoDS::Face(faces(i));
    const BRepAdaptor_Surface a(f);
    if (a.GetType() != GeomAbs_Plane) continue;
    gp_Dir d = a.Plane().Axis().Direction();
    if (!a.Plane().Position().Direct()) d.Reverse();
    if (f.Orientation() == TopAbs_REVERSED) d.Reverse();
    if (d.Dot(n) > 1 - 1e-9) return body + "/face/" + std::to_string(i - 1);
  }
  return {};
}
// The vertex of a body nearest p (world): "uuid/vertex/N".
std::string vertex(const Document& doc, const std::string& body, const gp_Pnt& p) {
  const Scene s = resolve(doc);
  TopTools_IndexedMapOfShape vs;
  TopExp::MapShapes(node_world_shape(doc, s, body), TopAbs_VERTEX, vs);
  int best = 0;
  double d = 1e300;
  for (int i = 1; i <= vs.Extent(); ++i)
    if (const double e = BRep_Tool::Pnt(TopoDS::Vertex(vs(i))).Distance(p); e < d) d = e, best = i;
  return body + "/vertex/" + std::to_string(best - 1);
}
// The straight edge of a body through a and b (world): "uuid/edge/N".
std::string edge(const Document& doc, const std::string& body, const gp_Pnt& a, const gp_Pnt& b) {
  const Scene s = resolve(doc);
  TopTools_IndexedMapOfShape es;
  TopExp::MapShapes(node_world_shape(doc, s, body), TopAbs_EDGE, es);
  for (int i = 1; i <= es.Extent(); ++i) {
    TopoDS_Vertex v0, v1;
    TopExp::Vertices(TopoDS::Edge(es(i)), v0, v1);
    const gp_Pnt p = BRep_Tool::Pnt(v0), q = BRep_Tool::Pnt(v1);
    if ((p.Distance(a) < 1e-6 && q.Distance(b) < 1e-6) || (p.Distance(b) < 1e-6 && q.Distance(a) < 1e-6)) return body + "/edge/" + std::to_string(i - 1);
  }
  return {};
}
struct Box {
  double x0, y0, z0, x1, y1, z1;
};
Box bounds(const Document& doc, const std::string& node) {
  Box b;
  node_tight_bbox(doc, resolve(doc), node).Get(b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
  return b;
}
std::string run_sketch(Document& doc, const Sketch& sk, const json& plane) {
  return commands::run("sketch", {{"plane", plane}, {"geometry", sk.to_json()}}, &doc)["sketch_id"];
}
json align(Document& doc, const json& bodies, json inputs) {
  inputs["bodies"] = bodies.is_array() ? bodies : json::array({bodies});  // {one ref} braces into the ref itself
  return feature_cmd(doc, "align", inputs);
}

// ---- point pairs
// A known move: 37 degrees about a tilted axis through (5, -3, 8), then a shift.
gp_Trsf known_move() {
  gp_Trsf r, t;
  r.SetRotation(gp_Ax1(gp_Pnt(5, -3, 8), gp_Dir(1, 2, 3)), 37 * M_PI / 180);
  t.SetTranslation(gp_Vec(120, 45, -30));
  return t * r;
}
// Noise hashed from an index (Gaussian, sigma per axis), the same on every run.
gp_Vec noise(int i, double sigma) {
  auto h = [](uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return double((z ^ (z >> 31)) >> 11) / double(1ull << 53);
  };
  auto gauss = [&](int k) {
    const uint64_t seed = uint64_t(i * 3 + k + 1) * 0x9E3779B97F4A7C15ull;
    return std::sqrt(-2 * std::log(std::max(1e-12, h(seed)))) * std::cos(2 * M_PI * h(seed + 7));
  };
  return gp_Vec(gauss(0), gauss(1), gauss(2)) * sigma;
}
// Points spread over a 100 x 60 x 40 mm part (not on one line or plane), and their copies moved away by the known move's
// inverse with noise: the fit must find the known move.
void pair_points(int n, double sigma, std::vector<gp_Pnt>& from, std::vector<gp_Pnt>& to) {
  from.clear(), to.clear();
  const gp_Trsf away = known_move().Inverted();
  for (int i = 0; i < n; ++i) {
    const gp_Pnt b(100 * std::fmod(i * 0.618034, 1.0), 60 * std::fmod(i * 0.414214 + 0.3, 1.0), 40 * std::fmod(i * 0.732051 + 0.6, 1.0));
    to.push_back(b);
    from.push_back(b.Transformed(away).Translated(noise(i, sigma)));
  }
}
// How far the fitted move is from the known one over the part's corners (mm).
double move_error(const gp_Trsf& fitted) {
  double worst = 0;
  for (double x : {0.0, 100.0})
    for (double y : {0.0, 60.0})
      for (double z : {0.0, 40.0}) {
        const gp_Pnt p(x, y, z);
        worst = std::max(worst, p.Transformed(fitted).Distance(p.Transformed(known_move())));
      }
  return worst;
}
std::string error_of(const std::function<void()>& f) {
  try {
    f();
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}
std::filesystem::path scratch(const std::string& name) {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-align-" + new_uuid());
  std::filesystem::create_directories(dir);
  return dir / name;
}
// The node ordinal of a mesh or cloud body nearest p (world): "uuid/vertex/N", as a pick gives it.
std::string mesh_vertex(const Document& doc, const std::string& body, const gp_Pnt& p) {
  const auto m = scanfit::fit_mesh(node_world_shape(doc, resolve(doc), body));
  size_t best = 0;
  for (size_t i = 1; i < m->nodes.size(); ++i)
    if (m->nodes[i].Distance(p) < m->nodes[best].Distance(p)) best = i;
  return body + "/vertex/" + std::to_string(best);
}
std::string only_body(const Document& doc, const std::string& representation) {
  const Scene s = resolve(doc);
  for (const auto& id : s.all_bodies())
    if (s.node(id)->representation == representation) return id;
  return {};
}
}  // namespace

TEST(face_onto_face_facing_flush_and_offset) {
  Document doc = Document::create();
  const std::string base = box(doc, 0, 0, 0, 10, 10, 10), block = box(doc, 50, 0, 0, 10, 10, 10);
  const std::string key = resolve(doc).node(block)->body_key;
  // The block's bottom onto the base's top: they face each other, so the block sits on the base.
  align(doc, {body_ref(block)}, {{"from", "plane"}, {"from_plane", {{"face", face(doc, block, gp_Dir(0, 0, -1))}}}, {"to", "plane"}, {"to_plane", {{"face", face(doc, base, gp_Dir(0, 0, 1))}}}});
  Box b = bounds(doc, block);
  CHECK_NEAR(b.z0, 10, 1e-6);
  CHECK_NEAR(b.z1, 20, 1e-6);
  CHECK_NEAR(b.x0, 50, 1e-6);  // only along the normal: no turn was needed and nothing else moves
  CHECK_EQ(resolve(doc).node(block)->body_key, key);  // placed, not rebuilt
  // With an offset: a gap above the face.
  const std::string block2 = box(doc, 80, 0, 0, 10, 10, 10);
  align(doc, {body_ref(block2)}, {{"from", "plane"}, {"from_plane", {{"face", face(doc, block2, gp_Dir(0, 0, -1))}}}, {"to", "plane"}, {"to_plane", {{"face", face(doc, base, gp_Dir(0, 0, 1))}}},
                                  {"offset", "5 mm"}});
  CHECK_NEAR(bounds(doc, block2).z0, 15, 1e-6);
  // Flip: flush with the face instead, so it is turned over (its bottom now faces up) and stands below the plane.
  const std::string block3 = box(doc, 110, 0, 0, 10, 10, 10);
  const std::string bottom = face(doc, block3, gp_Dir(0, 0, -1));
  align(doc, {body_ref(block3)}, {{"from", "plane"}, {"from_plane", {{"face", bottom}}}, {"to", "plane"}, {"to_plane", {{"face", face(doc, base, gp_Dir(0, 0, 1))}}}, {"flip", true}});
  b = bounds(doc, block3);
  CHECK_NEAR(b.z0, 0, 1e-6);
  CHECK_NEAR(b.z1, 10, 1e-6);
  CHECK(face(doc, block3, gp_Dir(0, 0, 1)) == bottom);  // the same face, now facing up
  // A side onto a side: turned a quarter so the block's -x side faces the base's +y side.
  const std::string side = box(doc, 0, 50, 0, 4, 6, 8);
  align(doc, {body_ref(side)}, {{"from", "plane"}, {"from_plane", {{"face", face(doc, side, gp_Dir(-1, 0, 0))}}}, {"to", "plane"}, {"to_plane", {{"face", face(doc, base, gp_Dir(0, 1, 0))}}}});
  b = bounds(doc, side);
  CHECK_NEAR(b.y0, 10, 1e-6);         // against the base's side
  CHECK_NEAR(b.y1 - b.y0, 4, 1e-6);  // its length now runs along y
  CHECK_NEAR(b.z1 - b.z0, 8, 1e-6);
}

TEST(points_lines_and_planes_in_every_pairing) {
  Document doc = Document::create();
  const std::string base = box(doc, 0, 0, 0, 10, 10, 10);
  // Point onto point: a corner onto the base's top corner.
  std::string b = box(doc, 30, 30, 0, 2, 2, 2);
  align(doc, {body_ref(b)}, {{"from", "point"}, {"from_point", {vertex(doc, b, gp_Pnt(30, 30, 0))}}, {"to", "point"}, {"to_point", {vertex(doc, base, gp_Pnt(10, 10, 10))}}});
  Box r = bounds(doc, b);
  CHECK_NEAR(r.x0, 10, 1e-6);
  CHECK_NEAR(r.y0, 10, 1e-6);
  CHECK_NEAR(r.z0, 10, 1e-6);
  // Point onto a line: square to it, onto the base's vertical edge at x=10, y=0.
  b = box(doc, 30, 30, 4, 2, 2, 2);
  align(doc, {body_ref(b)}, {{"from", "point"}, {"from_point", {vertex(doc, b, gp_Pnt(30, 30, 4))}}, {"to", "line"}, {"to_line", {{"edge", edge(doc, base, gp_Pnt(10, 0, 0), gp_Pnt(10, 0, 10))}}}});
  r = bounds(doc, b);
  CHECK_NEAR(r.x0, 10, 1e-6);
  CHECK_NEAR(r.y0, 0, 1e-6);
  CHECK_NEAR(r.z0, 4, 1e-6);  // along the line it stays
  // Point onto a plane: only along the plane's normal.
  b = box(doc, 30, 30, 4, 2, 2, 2);
  align(doc, {body_ref(b)}, {{"from", "point"}, {"from_point", {vertex(doc, b, gp_Pnt(30, 30, 4))}}, {"to", "plane"}, {"to_plane", {{"base", "xy"}}}});
  r = bounds(doc, b);
  CHECK_NEAR(r.z0, 0, 1e-6);
  CHECK_NEAR(r.x0, 30, 1e-6);
  // Line onto line: a bar along x stood up along the base's vertical edge.
  std::string bar = box(doc, 30, 0, 0, 20, 2, 2);
  align(doc, {body_ref(bar)}, {{"from", "line"}, {"from_line", {{"edge", edge(doc, bar, gp_Pnt(30, 0, 0), gp_Pnt(50, 0, 0))}}}, {"to", "line"},
                               {"to_line", {{"edge", edge(doc, base, gp_Pnt(10, 0, 0), gp_Pnt(10, 0, 10))}}}});
  r = bounds(doc, bar);
  CHECK_NEAR(r.z1 - r.z0, 20, 1e-6);
  CHECK_NEAR(r.x1 - r.x0, 2, 1e-6);
  // Line into a plane: a standing bar laid down into the xy plane (its edge, square to the plane, turned into it).
  bar = box(doc, 60, 0, 0, 2, 2, 20);
  align(doc, {body_ref(bar)}, {{"from", "line"}, {"from_line", {{"edge", edge(doc, bar, gp_Pnt(60, 0, 0), gp_Pnt(60, 0, 20))}}}, {"to", "plane"}, {"to_plane", {{"base", "xy"}}}});
  r = bounds(doc, bar);
  CHECK_NEAR(r.z1 - r.z0, 2, 1e-6);
  CHECK_NEAR(std::max(r.x1 - r.x0, r.y1 - r.y0), 20, 1e-6);  // lying down
  // Already in the plane: only moved onto it.
  bar = box(doc, 60, 40, 0, 2, 2, 20);
  align(doc, {body_ref(bar)}, {{"from", "line"}, {"from_line", {{"edge", edge(doc, bar, gp_Pnt(60, 40, 0), gp_Pnt(60, 40, 20))}}}, {"to", "plane"}, {"to_plane", {{"base", "yz"}}}});
  r = bounds(doc, bar);
  CHECK_NEAR(r.z1 - r.z0, 20, 1e-6);
  CHECK(std::fabs(r.x0) < 1e-6 || std::fabs(r.x1) < 1e-6);
  // Plane through a line: a slab's top made to contain the base's top front edge (it was parallel already: a move).
  std::string slab = box(doc, 30, 60, 0, 6, 6, 1);
  align(doc, {body_ref(slab)}, {{"from", "plane"}, {"from_plane", {{"face", face(doc, slab, gp_Dir(0, 0, 1))}}}, {"to", "line"},
                                {"to_line", {{"edge", edge(doc, base, gp_Pnt(0, 0, 10), gp_Pnt(10, 0, 10))}}}});
  r = bounds(doc, slab);
  CHECK_NEAR(r.z1, 10, 1e-6);
  // Line through a point, plane through a point: moved, not turned.
  bar = box(doc, 100, 0, 0, 20, 2, 2);
  align(doc, {body_ref(bar)}, {{"from", "plane"}, {"from_plane", {{"face", face(doc, bar, gp_Dir(0, 0, 1))}}}, {"to", "point"}, {"to_point", {vertex(doc, base, gp_Pnt(10, 10, 10))}}});
  r = bounds(doc, bar);
  CHECK_NEAR(r.z1, 10, 1e-6);
  CHECK_NEAR(r.x0, 100, 1e-6);
}

TEST(motion_choices_turn_and_refusals) {
  Document doc = Document::create();
  const std::string base = box(doc, 0, 0, 0, 10, 10, 10);
  // Move only: the bottom face goes onto the side's plane without turning (it lands on that plane's height... along x).
  std::string b = box(doc, 30, 0, 0, 2, 4, 6);
  const std::string side = face(doc, b, gp_Dir(-1, 0, 0));
  const double anchor = resolve_plane(doc, resolve(doc), {{"face", side}}).origin[2];  // the side's frame starts at one of its corners
  align(doc, {body_ref(b)}, {{"from", "plane"}, {"from_plane", {{"face", side}}}, {"to", "plane"}, {"to_plane", {{"face", face(doc, base, gp_Dir(0, 0, 1))}}}, {"motion", "move"}});
  Box r = bounds(doc, b);
  CHECK_NEAR(r.x1 - r.x0, 2, 1e-6);       // not turned
  CHECK_NEAR(r.z0, 10 - anchor, 1e-6);    // its From point (that corner) moved onto the plane
  // Rotate only: turned to face, about its own face's corner, without moving onto the plane.
  b = box(doc, 30, 30, 0, 2, 4, 6);
  align(doc, {body_ref(b)}, {{"from", "plane"}, {"from_plane", {{"face", face(doc, b, gp_Dir(-1, 0, 0))}}}, {"to", "plane"}, {"to_plane", {{"face", face(doc, base, gp_Dir(0, 0, 1))}}},
                             {"motion", "rotate"}});
  r = bounds(doc, b);
  CHECK_NEAR(r.z1 - r.z0, 2, 1e-6);  // its x side now faces down
  CHECK(r.z1 < 10.5);                // not lifted onto the base
  // Turn about it: face on face, then a quarter turn about the base's normal.
  b = box(doc, 30, 60, 0, 2, 8, 1);
  align(doc, {body_ref(b)}, {{"from", "plane"}, {"from_plane", {{"face", face(doc, b, gp_Dir(0, 0, -1))}}}, {"to", "plane"}, {"to_plane", {{"face", face(doc, base, gp_Dir(0, 0, 1))}}},
                             {"angle", "90 deg"}});
  r = bounds(doc, b);
  CHECK_NEAR(r.x1 - r.x0, 8, 1e-6);
  CHECK_NEAR(r.z0, 10, 1e-6);
  // A point has no direction to turn by.
  CHECK_THROWS(align(doc, {body_ref(b)}, {{"from", "point"}, {"from_point", {vertex(doc, b, gp_Pnt(0, 0, 0))}}, {"to", "point"}, {"to_point", {vertex(doc, base, gp_Pnt(10, 10, 10))}},
                                          {"motion", "rotate"}}));
  CHECK_THROWS(align(doc, json::array(), {{"from", "plane"}, {"from_plane", {{"base", "xy"}}}, {"to", "plane"}, {"to_plane", {{"base", "yz"}}}}));
}

TEST(components_move_by_their_placement_and_regenerate) {
  Document doc = Document::create();
  commands::run("param", {{"name", "h"}, {"expr", "10 mm"}}, &doc);
  const std::string base = feature_cmd(doc, "box", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "h"}, {"centered", false}})["body_ids"][0];
  const std::string part = commands::run("component", {{"name", "Part"}}, &doc)["component_id"];
  const std::string a = commands::run("feature", {{"kind", "box"}, {"inputs", {{"plane", {{"origin", {40, 0, 0}}, {"normal", {0, 0, 1}}}}, {"length", "4 mm"}, {"width", "4 mm"},
                                                                         {"height", "4 mm"}, {"centered", false}}}, {"parent", part}}, &doc)["body_ids"][0];
  const std::string b = commands::run("feature", {{"kind", "box"}, {"inputs", {{"plane", {{"origin", {50, 0, 0}}, {"normal", {0, 0, 1}}}}, {"length", "4 mm"}, {"width", "4 mm"},
                                                                         {"height", "4 mm"}, {"centered", false}}}, {"parent", part}}, &doc)["body_ids"][0];
  const Scene before = resolve(doc);
  const std::string ka = before.node(a)->body_key, kb = before.node(b)->body_key;
  // The component (and a body inside it, picked as well: it goes with the component, once) onto the base's top.
  const json made = align(doc, {part, body_ref(a)}, {{"from", "plane"}, {"from_plane", {{"face", face(doc, a, gp_Dir(0, 0, -1))}}}, {"to", "plane"},
                                                     {"to_plane", {{"face", face(doc, base, gp_Dir(0, 0, 1))}}}});
  const Scene s = resolve(doc);
  CHECK_EQ(s.node(a)->body_key, ka);
  CHECK_EQ(s.node(b)->body_key, kb);
  CHECK(s.node(a)->local.is_identity());  // the component carries the move
  CHECK_NEAR(s.world(part).at(2, 3), 10, 1e-9);
  CHECK_NEAR(bounds(doc, b).z0, 10, 1e-6);
  // The base grows: the align follows its top face.
  commands::run("param", {{"name", "h"}, {"expr", "25 mm"}}, &doc);
  CHECK_NEAR(bounds(doc, a).z0, 25, 1e-6);
  CHECK_NEAR(bounds(doc, b).z0, 25, 1e-6);
  (void)made;
}

TEST(move_places_instead_of_rebuilding) {
  Document doc = Document::create();
  const std::string a = box(doc, 0, 0, 0, 10, 10, 10);
  const std::string key = resolve(doc).node(a)->body_key;
  feature_cmd(doc, "move", {{"bodies", json::array({body_ref(a)})}, {"dx", "5 mm"}, {"rotate", true}, {"axis", {{"base", "z"}}}, {"angle", "90 deg"}});
  const Scene s = resolve(doc);
  CHECK_EQ(s.node(a)->body_key, key);
  const Box r = bounds(doc, a);
  CHECK_NEAR(r.x0, -5, 1e-6);  // turned about z (x -> y, y -> -x), then 5 along x
  CHECK_NEAR(r.x1, 5, 1e-6);
  CHECK_NEAR(r.y0, 0, 1e-6);
  // A copy is still a new body.
  const json copy = feature_cmd(doc, "move", {{"bodies", json::array({body_ref(a)})}, {"dz", "20 mm"}, {"copy", true}});
  CHECK_EQ(copy["body_ids"].size(), 1u);
  CHECK(copy["body_ids"][0] != a);
}

TEST(sketches_align_onto_faces_and_follow_them) {
  Document doc = Document::create();
  commands::run("param", {{"name", "h"}, {"expr", "10 mm"}}, &doc);
  const std::string base = feature_cmd(doc, "box", {{"length", "40 mm"}, {"width", "40 mm"}, {"height", "h"}, {"centered", false}})["body_ids"][0];
  Sketch sk;
  const int p0 = sk.add_point(0, 0), p1 = sk.add_point(6, 0), p2 = sk.add_point(6, 4), p3 = sk.add_point(0, 4);
  sk.add_line(p0, p1), sk.add_line(p1, p2), sk.add_line(p2, p3), sk.add_line(p3, p0);
  const std::string id = run_sketch(doc, sk, {{"origin", {0, 0, -30}}, {"normal", {0, 0, 1}}});
  const std::string top = face(doc, base, gp_Dir(0, 0, 1));
  // Its plane onto the box's top (lying flush, facing up), its corner onto the top's far corner.
  json r = commands::run("align_sketch", {{"target", id}, {"inputs", {{"from", "plane"}, {"from_plane", {{"sketch", id}}}, {"to", "plane"}, {"to_plane", {{"face", top}}}}}}, &doc);
  CHECK_NEAR(r["frame"]["origin"][2].get<double>(), 10, 1e-6);
  CHECK_NEAR(r["frame"]["normal"][2].get<double>(), 1, 1e-9);
  const Scene linked = resolve(doc);
  const SketchItem* item = linked.sketch(id);
  CHECK(item->plane.contains("support"));  // linked to the face
  // The box grows: the sketch follows its top.
  commands::run("param", {{"name", "h"}, {"expr", "16 mm"}}, &doc);
  CHECK_NEAR(resolve(doc).sketch(id)->frame.origin[2], 16, 1e-6);
  // A point of it onto a vertex of the top: moved within the face, still on it.
  r = commands::run("align_sketch", {{"target", id}, {"inputs", {{"from", "point"}, {"from_point", {{{"sketch", id}, {"point", p0}}}}, {"to", "point"},
                                                                  {"to_point", {vertex(doc, base, gp_Pnt(40, 40, 16))}}}}}, &doc);
  const Scene onVertex = resolve(doc);
  const SketchItem* moved = onVertex.sketch(id);
  CHECK_NEAR(moved->frame.origin[0], 40, 1e-6);
  CHECK_NEAR(moved->frame.origin[1], 40, 1e-6);
  CHECK(moved->plane.contains("support"));
  commands::run("param", {{"name", "h"}, {"expr", "12 mm"}}, &doc);
  CHECK_NEAR(resolve(doc).sketch(id)->frame.origin[2], 12, 1e-6);
  // An extrusion of it moves with it.
  const json ex = feature_cmd(doc, "extrude", {{"profiles", {{{"sketch", id}, {"at", {3, 2}}}}}, {"distance", "5 mm"}});
  commands::run("align_sketch", {{"target", id}, {"inputs", {{"from", "plane"}, {"from_plane", {{"sketch", id}}}, {"to", "plane"}, {"to_plane", {{"base", "xy"}}}, {"offset", "-20 mm"}}}}, &doc);
  CHECK_NEAR(bounds(doc, ex["body_ids"][0]).z0, -20, 1e-6);
  // Turned on its plane by an angle, and flipped to face down.
  commands::run("align_sketch", {{"target", id}, {"inputs", {{"from", "plane"}, {"from_plane", {{"sketch", id}}}, {"to", "plane"}, {"to_plane", {{"base", "xy"}}}, {"flip", true}}}}, &doc);
  CHECK_NEAR(resolve(doc).sketch(id)->frame.normal()[2], -1, 1e-9);
}

TEST(point_pairs_recover_a_known_move) {
  std::vector<gp_Pnt> from, to;
  // Exact pairs: the move itself.
  pair_points(3, 0, from, to);
  pairs::PairFit fit = pairs::fit_pairs(from, to, true);
  CHECK(move_error(fit.motion) < 1e-9);
  CHECK(fit.rms < 1e-9);
  // Noisy picks (0.05 mm a coordinate): three pairs fix it, more pin it down better.
  double first = 0;
  for (int n : {3, 4, 10}) {
    pair_points(n, 0.05, from, to);
    fit = pairs::fit_pairs(from, to, true);
    const double e = move_error(fit.motion);
    std::printf("    %d noisy pairs: off by %.4f mm at the corners, rms %.4f, dropped %d\n", n, e, fit.rms, fit.dropped);
    CHECK_EQ(fit.dropped, -1);  // nothing stray among them
    CHECK_EQ(fit.residuals.size(), size_t(n));
    CHECK(e < (n == 3 ? 1.0 : n == 4 ? 0.6 : 0.3));  // the corners lie beyond most picks: errors grow out there
    if (n == 3) first = e;
    if (n == 10) CHECK(e < first);
    CHECK(fit.rms > 0.005 && fit.rms < 0.2);  // the noise shows in the residuals
  }
}

TEST(point_pairs_leave_out_a_stray_pair) {
  std::vector<gp_Pnt> from, to;
  pair_points(10, 0.05, from, to);
  from[6].Translate(gp_Vec(3, -4, 2));  // a pick on the wrong spot: 5.4 mm off
  const pairs::PairFit fit = pairs::fit_pairs(from, to, true);
  CHECK_EQ(fit.dropped, 6);
  CHECK(fit.residuals[6] > 4);
  CHECK(fit.rms < 0.2);
  CHECK(move_error(fit.motion) < 0.3);
  // Kept, it spoils the move.
  const pairs::PairFit kept = pairs::fit_pairs(from, to, false);
  CHECK_EQ(kept.dropped, -1);
  CHECK(move_error(kept.motion) > 0.5);
  CHECK(kept.rms > 1);
  // Four pairs with one bad: left out too (three good ones still fix the move).
  pair_points(4, 0.02, from, to);
  from[1].Translate(gp_Vec(0, 6, 0));
  CHECK_EQ(pairs::fit_pairs(from, to, true).dropped, 1);
}

TEST(point_pairs_refusals) {
  std::vector<gp_Pnt> from, to;
  pair_points(2, 0, from, to);
  CHECK(error_of([&] { pairs::fit_pairs(from, to, true); }).find("three pairs") != std::string::npos);
  // On one line: four points along a straight edge leave the turn about it free (on either side).
  from = {gp_Pnt(0, 0, 0), gp_Pnt(10, 0, 0), gp_Pnt(25, 0, 0), gp_Pnt(40, 0, 0)};
  to = {gp_Pnt(0, 0, 5), gp_Pnt(0, 10, 5), gp_Pnt(3, 0, 9), gp_Pnt(7, 7, 7)};
  CHECK(error_of([&] { pairs::fit_pairs(from, to, true); }).find("points on the bodies are all on one line") != std::string::npos);
  std::swap(from, to);
  CHECK(error_of([&] { pairs::fit_pairs(from, to, true); }).find("target points are all on one line") != std::string::npos);
  // Nearly on one line (picks along an edge, a little noise) is no better.
  from = {gp_Pnt(0, 0, 0), gp_Pnt(30, 0.01, 0), gp_Pnt(60, -0.01, 0.01), gp_Pnt(90, 0, 0)};
  CHECK(error_of([&] { pairs::fit_pairs(from, to, true); }).find("on one line") != std::string::npos);
  // Counts that differ, and everything at one place.
  pair_points(4, 0, from, to);
  to.pop_back();
  CHECK(error_of([&] { pairs::fit_pairs(from, to, true); }).find("one target point for each") != std::string::npos);
  from.assign(3, gp_Pnt(1, 2, 3));
  to = from;
  CHECK(error_of([&] { pairs::fit_pairs(from, to, true); }).find("at one place") != std::string::npos);
  // Through the feature: the reason reaches the command.
  Document doc = Document::create();
  const std::string base = box(doc, 0, 0, 0, 10, 10, 10), b = box(doc, 30, 0, 0, 10, 10, 10);
  const json two_pairs = {{"from", "point_pairs"},
                          {"from_points", {vertex(doc, b, gp_Pnt(30, 0, 0)), vertex(doc, b, gp_Pnt(40, 0, 0))}},
                          {"to_points", {vertex(doc, base, gp_Pnt(0, 0, 0)), vertex(doc, base, gp_Pnt(10, 0, 0))}}};
  CHECK(error_of([&] { align(doc, {body_ref(b)}, two_pairs); }).find("three pairs") != std::string::npos);
}

TEST(point_pairs_put_a_scan_on_its_cad_body) {
  // The CAD body, and its "scan": the same block as an STL mesh, moved away by the known move's inverse, its nodes noisy.
  Document doc = Document::create();
  const std::string cad = box(doc, 0, 0, 0, 100, 60, 40);
  const auto stl = scratch("scan.stl");
  {
    const gp_Trsf away = known_move().Inverted();
    std::vector<gp_Pnt> c;
    for (int i = 0; i < 8; ++i) c.push_back(gp_Pnt(i & 1 ? 100 : 0, i & 2 ? 60 : 0, i & 4 ? 40 : 0).Transformed(away).Translated(noise(i, 0.03)));
    const int faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4}, {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    std::ofstream out(stl, std::ios::binary);
    char header[80] = {};
    out.write(header, 80);
    const uint32_t n = 12;
    out.write(reinterpret_cast<const char*>(&n), 4);
    for (const auto& f : faces) {
      float v[12] = {0, 0, 0};
      int k = 3;
      for (int j = 0; j < 3; ++j) v[k++] = float(c[size_t(f[j])].X()), v[k++] = float(c[size_t(f[j])].Y()), v[k++] = float(c[size_t(f[j])].Z());
      out.write(reinterpret_cast<const char*>(v), 48);
      const uint16_t attr = 0;
      out.write(reinterpret_cast<const char*>(&attr), 2);
    }
  }
  import_file(doc, stl);
  const std::string scan = only_body(doc, "mesh");
  CHECK(!scan.empty());
  const std::string key = resolve(doc).node(scan)->body_key;
  // Four corners picked on the scan, then the same corners on the block, in turn.
  json from = json::array(), to = json::array();
  for (const gp_Pnt& corner : {gp_Pnt(0, 0, 0), gp_Pnt(100, 0, 0), gp_Pnt(100, 60, 40), gp_Pnt(0, 60, 0)}) {
    from.push_back(mesh_vertex(doc, scan, corner.Transformed(known_move().Inverted())));
    to.push_back(Ref::parse(vertex(doc, cad, corner)).to_json());  // as a pick gives it: an object (hinted when stored)
  }
  align(doc, {body_ref(scan)}, {{"from", "point_pairs"}, {"from_points", from}, {"to_points", to}});
  CHECK_EQ(resolve(doc).node(scan)->body_key, key);  // placed, not rebuilt
  const Box b = bounds(doc, scan);
  std::printf("    scan after the pairs: %.3f %.3f %.3f .. %.3f %.3f %.3f\n", b.x0, b.y0, b.z0, b.x1, b.y1, b.z1);
  for (const double off : {b.x0, b.y0, b.z0, b.x1 - 100, b.y1 - 60, b.z1 - 40}) CHECK(std::fabs(off) < 0.15);
  // The result says how well each pair met: per-pair residuals and the rms, every pair used.
  const json result = doc.ops.back().data.at("result");
  CHECK_EQ(result.at("pairs").size(), 4u);
  CHECK(result.at("rms").get<double>() < 0.1);
  for (const auto& p : result["pairs"]) {
    CHECK(p.at("used").get<bool>());
    CHECK(p.at("residual").get<double>() < 0.15);
  }
  CHECK(!result.contains("dropped_pair"));
  // Picked pairs are hinted as other references are (the app stores them so): the scan's nodes and the block's vertices.
  json picked = {{"from_points", json::array()}, {"to_points", to}};
  for (const auto& f : from) picked["from_points"].push_back(Ref::parse(f.get<std::string>()).to_json());
  const json hinted = design::hint_refs(doc, resolve(doc), picked);
  CHECK(hinted["from_points"][0].contains("hint") && hinted["to_points"][3].contains("hint"));
  // A regeneration recomputes nothing (replay reads the placements).
  CHECK(design::plan_regenerate(doc, true).bodies.empty());
  // A fifth pair picked wrong (another corner of the block): left out and named.
  from.push_back(mesh_vertex(doc, scan, gp_Pnt(100, 60, 0)));
  to.push_back(vertex(doc, cad, gp_Pnt(0, 0, 40)));
  align(doc, {body_ref(scan)}, {{"from", "point_pairs"}, {"from_points", from}, {"to_points", to}});
  const json again = doc.ops.back().data.at("result");
  CHECK_EQ(again.value("dropped_pair", 0), 5);
  CHECK(again.value("note", std::string()).find("pair 5 left out") != std::string::npos);
  CHECK(!again["pairs"][4]["used"].get<bool>());
  std::error_code e;
  std::filesystem::remove_all(stl.parent_path(), e);
}

TEST(point_pairs_from_a_cloud_onto_a_sketch) {
  // A point cloud (an XYZ file: the block's bottom outline, moved away), its corners onto a sketch's corners.
  Document doc = Document::create();
  const auto xyz = scratch("scan.xyz");
  {
    std::ofstream out(xyz);
    out.precision(10);
    const gp_Trsf away = known_move().Inverted();
    for (int i = 0; i < 40; ++i) {
      const double t = i / 39.0;
      for (const gp_Pnt& p : {gp_Pnt(100 * t, 0, 0), gp_Pnt(100, 60 * t, 0), gp_Pnt(100 * (1 - t), 60, 0), gp_Pnt(0, 60 * (1 - t), 0)}) {
        const gp_Pnt q = p.Transformed(away);
        out << q.X() << " " << q.Y() << " " << q.Z() << "\n";
      }
    }
  }
  import_file(doc, xyz);
  const std::string cloud = only_body(doc, "points");
  CHECK(!cloud.empty());
  Sketch sk;
  const int p0 = sk.add_point(0, 0), p1 = sk.add_point(100, 0), p2 = sk.add_point(100, 60), p3 = sk.add_point(0, 60);
  sk.add_line(p0, p1), sk.add_line(p1, p2), sk.add_line(p2, p3), sk.add_line(p3, p0);
  const std::string id = run_sketch(doc, sk, {{"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}});
  json from = json::array(), to = json::array();
  const std::vector<gp_Pnt> corners = {gp_Pnt(0, 0, 0), gp_Pnt(100, 0, 0), gp_Pnt(100, 60, 0), gp_Pnt(0, 60, 0)};
  const std::vector<int> points = {p0, p1, p2, p3};
  for (size_t i = 0; i < 4; ++i) {
    from.push_back(mesh_vertex(doc, cloud, corners[i].Transformed(known_move().Inverted())));
    to.push_back({{"sketch", id}, {"point", points[i]}});
  }
  align(doc, {body_ref(cloud)}, {{"from", "point_pairs"}, {"from_points", from}, {"to_points", to}});
  const json result = doc.ops.back().data.at("result");
  std::printf("    cloud onto the sketch: rms %.6f mm\n", result.at("rms").get<double>());
  CHECK(result.at("rms").get<double>() < 1e-3);  // single-precision points
  const Box b = bounds(doc, cloud);
  CHECK(std::fabs(b.x0) < 0.01 && std::fabs(b.y1 - 60) < 0.01 && std::fabs(b.z1) < 0.01);
  std::error_code e;
  std::filesystem::remove_all(xyz.parent_path(), e);
}

CHECK_MAIN()

// ---- 3-2-1
namespace {
// The block 100 x 60 x 40 placed by the known move (its corner at the move's origin, its sides along the moved axes).
std::string moved_block(Document& doc) {
  const gp_Trsf m = known_move();
  const gp_Pnt o = gp_Pnt(0, 0, 0).Transformed(m);
  const gp_Dir z = gp_Dir(0, 0, 1).Transformed(m), x = gp_Dir(1, 0, 0).Transformed(m);
  return feature_cmd(doc, "box", {{"plane", {{"origin", {o.X(), o.Y(), o.Z()}}, {"normal", {z.X(), z.Y(), z.Z()}}, {"x", {x.X(), x.Y(), x.Z()}}}},
                                  {"length", "100 mm"}, {"width", "60 mm"}, {"height", "40 mm"}, {"centered", false}})["body_ids"][0];
}
gp_Dir moved(const gp_Dir& d) { return d.Transformed(known_move()); }
gp_Pnt moved(const gp_Pnt& p) { return p.Transformed(known_move()); }
}  // namespace

TEST(sequence_321_planes_line_and_point) {
  Document doc = Document::create();
  const std::string block = box(doc, 0, 0, 0, 100, 60, 40);
  // Primary only: the moved copy's bottom flush onto the block's bottom; three freedoms stay (two slides, one turn).
  std::string copy = moved_block(doc);
  json inputs = {{"from", "sequence"}, {"flip", true},
                 {"primary_from", {{"face", face(doc, copy, moved(gp_Dir(0, 0, -1)))}}}, {"primary_to", {{"face", face(doc, block, gp_Dir(0, 0, -1))}}},
                 {"secondary", "plane"}, {"tertiary", "plane"}};
  align(doc, {body_ref(copy)}, inputs);
  json result = doc.ops.back().data.at("result");
  CHECK_EQ(result.at("free").size(), 3u);
  CHECK(result["free"][2].contains("turn") && std::fabs(result["free"][2]["turn"][2].get<double>()) > 1 - 1e-9);
  Box b = bounds(doc, copy);
  CHECK_NEAR(b.z0, 0, 1e-6);  // flush on the bottom plane
  CHECK_NEAR(b.z1, 40, 1e-6);
  // Primary, secondary (front onto front), tertiary (left onto left), planes all: the copy lands on the block.
  copy = moved_block(doc);
  inputs["primary_from"] = {{"face", face(doc, copy, moved(gp_Dir(0, 0, -1)))}};
  inputs["secondary_from_plane"] = {{"face", face(doc, copy, moved(gp_Dir(0, -1, 0)))}};
  inputs["secondary_to_plane"] = {{"face", face(doc, block, gp_Dir(0, -1, 0))}};
  inputs["tertiary_from_plane"] = {{"face", face(doc, copy, moved(gp_Dir(-1, 0, 0)))}};
  inputs["tertiary_to_plane"] = {{"face", face(doc, block, gp_Dir(-1, 0, 0))}};
  align(doc, {body_ref(copy)}, inputs);
  result = doc.ops.back().data.at("result");
  CHECK(result.at("free").empty());
  CHECK_EQ(result.at("steps").size(), 3u);
  b = bounds(doc, copy);
  for (const double off : {b.x0, b.y0, b.z0, b.x1 - 100, b.y1 - 60, b.z1 - 40}) CHECK(std::fabs(off) < 1e-6);
  // Without the tertiary: one slide left, along the front plane's meeting with the bottom (X).
  copy = moved_block(doc);
  inputs["primary_from"] = {{"face", face(doc, copy, moved(gp_Dir(0, 0, -1)))}};
  inputs["secondary_from_plane"] = {{"face", face(doc, copy, moved(gp_Dir(0, -1, 0)))}};
  inputs["tertiary"] = "none";
  align(doc, {body_ref(copy)}, inputs);
  result = doc.ops.back().data.at("result");
  CHECK_EQ(result.at("free").size(), 1u);
  CHECK(std::fabs(result["free"][0]["slide"][0].get<double>()) > 1 - 1e-9);
  b = bounds(doc, copy);
  CHECK(std::fabs(b.y0) < 1e-6 && std::fabs(b.z0) < 1e-6 && std::fabs(b.x1 - b.x0 - 100) < 1e-6);
  // Secondary line (the bottom front edge along the block's) and tertiary point (the corner onto the corner).
  copy = moved_block(doc);
  json lines = {{"from", "sequence"}, {"flip", true},
                {"primary_from", {{"face", face(doc, copy, moved(gp_Dir(0, 0, -1)))}}}, {"primary_to", {{"face", face(doc, block, gp_Dir(0, 0, -1))}}},
                {"secondary", "line"}, {"secondary_from_line", {{"edge", edge(doc, copy, moved(gp_Pnt(0, 0, 0)), moved(gp_Pnt(100, 0, 0)))}}},
                {"secondary_to_line", {{"edge", edge(doc, block, gp_Pnt(0, 0, 0), gp_Pnt(100, 0, 0))}}},
                {"tertiary", "point"}, {"tertiary_from_point", {vertex(doc, copy, moved(gp_Pnt(0, 0, 0)))}}, {"tertiary_to_point", {vertex(doc, block, gp_Pnt(0, 0, 0))}}};
  align(doc, {body_ref(copy)}, lines);
  CHECK(doc.ops.back().data.at("result").at("free").empty());
  b = bounds(doc, copy);
  for (const double off : {b.x0, b.y0, b.z0, b.x1 - 100, b.y1 - 60, b.z1 - 40}) CHECK(std::fabs(off) < 1e-6);
  // A secondary line square to the primary plane cannot fix the turn; a tertiary plane along the slide cannot stop it.
  copy = moved_block(doc);
  lines["primary_from"] = {{"face", face(doc, copy, moved(gp_Dir(0, 0, -1)))}};
  lines["secondary_from_line"] = {{"edge", edge(doc, copy, moved(gp_Pnt(0, 0, 0)), moved(gp_Pnt(0, 0, 40)))}};
  CHECK(error_of([&] { align(doc, {body_ref(copy)}, lines); }).find("square to the primary plane") != std::string::npos);
  lines["secondary_from_line"] = {{"edge", edge(doc, copy, moved(gp_Pnt(0, 0, 0)), moved(gp_Pnt(100, 0, 0)))}};
  lines["tertiary"] = "plane";
  lines["tertiary_from_plane"] = {{"face", face(doc, copy, moved(gp_Dir(0, -1, 0)))}};
  lines["tertiary_to_plane"] = {{"face", face(doc, block, gp_Dir(0, -1, 0))}};
  CHECK(error_of([&] { align(doc, {body_ref(copy)}, lines); }).find("runs along the slide") != std::string::npos);
  // No primary: refused with what to pick.
  const std::string none = error_of([&] { align(doc, {body_ref(copy)}, json{{"from", "sequence"}}); });
  std::printf("    no primary: %s\n", none.c_str());
  CHECK(none.find("primary") != std::string::npos);  // the schema check names the field, the feature the planes
}
