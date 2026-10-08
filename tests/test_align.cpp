// Align (bodies, components, sketches): a point, line or face put onto another point, line or plane; moved, turned or
// both; placed as they are (body keys kept); sketches re-planed and still on the face they were aligned to. And Move
// without a copy places its bodies the same way.
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/document.hpp"
#include "opad/geometry.hpp"
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
  const SketchItem* item = resolve(doc).sketch(id);
  CHECK(item->plane.contains("support"));  // linked to the face
  // The box grows: the sketch follows its top.
  commands::run("param", {{"name", "h"}, {"expr", "16 mm"}}, &doc);
  CHECK_NEAR(resolve(doc).sketch(id)->frame.origin[2], 16, 1e-6);
  // A point of it onto a vertex of the top: moved within the face, still on it.
  r = commands::run("align_sketch", {{"target", id}, {"inputs", {{"from", "point"}, {"from_point", {{{"sketch", id}, {"point", p0}}}}, {"to", "point"},
                                                                  {"to_point", {vertex(doc, base, gp_Pnt(40, 40, 16))}}}}}, &doc);
  const SketchItem* moved = resolve(doc).sketch(id);
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

CHECK_MAIN()
