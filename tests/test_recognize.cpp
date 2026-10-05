// Feature recognition on dumb solids and the remove_faces feature (TODO 11 UI-97).
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax2.hxx>

#include <cmath>
#include <cstdio>

#include "check.hpp"
#include "opad/agent.hpp"
#include "opad/core.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/recognize.hpp"
#include "opad/step_io.hpp"

using namespace opad;

namespace {

TopoDS_Shape cut(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  BRepAlgoAPI_Cut c(a, b);
  c.SimplifyResult();
  return c.Shape();
}
TopoDS_Shape fuse(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  BRepAlgoAPI_Fuse f(a, b);
  f.SimplifyResult();
  return f.Shape();
}
TopoDS_Shape box(double x0, double y0, double z0, double x1, double y1, double z1) { return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), gp_Pnt(x1, y1, z1)).Shape(); }
TopoDS_Shape rod(double x, double y, double z0, double d, double h) { return BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, y, z0), gp::DZ()), d / 2, h).Shape(); }

double volume(const TopoDS_Shape& s) {
  GProp_GProps g;
  BRepGProp::VolumeProperties(s, g);
  return g.Mass();
}

int count(const TopoDS_Shape& s, TopAbs_ShapeEnum t) {
  TopTools_IndexedMapOfShape m;
  TopExp::MapShapes(s, t, m);
  return m.Extent();
}

// The face whose centre of mass is nearest `at` (optionally of one surface type).
int face_at(const TopoDS_Shape& s, const gp_Pnt& at, int type = -1) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  int best = -1;
  double d = 1e300;
  for (int i = 1; i <= faces.Extent(); ++i) {
    if (type >= 0 && BRepAdaptor_Surface(TopoDS::Face(faces(i))).GetType() != type) continue;
    GProp_GProps g;
    BRepGProp::SurfaceProperties(faces(i), g);
    if (g.CentreOfMass().Distance(at) < d) d = g.CentreOfMass().Distance(at), best = i - 1;
  }
  return best;
}

int edge_at(const TopoDS_Shape& s, const gp_Pnt& at) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(s, TopAbs_EDGE, edges);
  int best = -1;
  double d = 1e300;
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    const double dist = c.Value((c.FirstParameter() + c.LastParameter()) / 2).Distance(at);
    if (dist < d) d = dist, best = i - 1;
  }
  return best;
}

TopoDS_Edge edge_of(const TopoDS_Shape& s, int i) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(s, TopAbs_EDGE, edges);
  return TopoDS::Edge(edges(i + 1));
}

void show(const char* what, const std::vector<Recognized>& groups) {
  std::printf("  %s:\n", what);
  for (const auto& g : groups) std::printf("    %s [%s] faces %zu edges %zu %s\n", g.kind.c_str(), g.label.c_str(), g.faces.size(), g.edges.size(), g.params.dump().c_str());
}

// An 80x50x10 plate: four through holes Ø6, two counterbored Ø4 (Ø8 x 3), a blind Ø5 x 6 with a 118° drill point, a
// countersunk Ø4 (Ø8, 90°) and a flat blind Ø6 x 4.
TopoDS_Shape holes_plate() {
  TopoDS_Shape s = box(0, 0, 0, 80, 50, 10);
  for (auto [x, y] : {std::pair{10.0, 10.0}, {70.0, 10.0}, {10.0, 40.0}, {70.0, 40.0}}) s = cut(s, rod(x, y, -1, 6, 12));
  for (double x : {30.0, 50.0}) s = cut(s, fuse(rod(x, 25, -1, 4, 12), rod(x, 25, 7, 8, 4)));
  const double tip = 2.5 / std::tan(59 * M_PI / 180);
  s = cut(s, fuse(rod(40, 10, 4, 5, 7), BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(40, 10, 4), -gp::DZ()), 2.5, 0, tip).Shape()));
  s = cut(s, fuse(rod(40, 40, -1, 4, 12), BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(40, 40, 8), gp::DZ()), 2, 5, 3).Shape()));
  s = cut(s, rod(60, 25, 6, 6, 5));
  return s;
}

// A 40x30x20 block with its four top and four upright edges rounded R3 (one tangent chain, corners included).
TopoDS_Shape blended_block() {
  const TopoDS_Shape b = box(0, 0, 0, 40, 30, 20);
  BRepFilletAPI_MakeFillet mk(b);
  for (const gp_Pnt& p : {gp_Pnt(20, 0, 20), gp_Pnt(20, 30, 20), gp_Pnt(0, 15, 20), gp_Pnt(40, 15, 20), gp_Pnt(0, 0, 10), gp_Pnt(40, 0, 10), gp_Pnt(0, 30, 10), gp_Pnt(40, 30, 10)})
    mk.Add(3, edge_of(b, edge_at(b, p)));
  mk.Build();
  return mk.Shape();
}

// A 60x40x10 plate with a 20x10x5 boss, a 15x10x4 pocket, a 10x7 window through it and a 3 mm wide slot across it.
TopoDS_Shape features_plate() {
  TopoDS_Shape s = fuse(box(0, 0, 0, 60, 40, 10), box(5, 5, 10, 25, 15, 15));
  s = cut(s, box(35, 20, 6, 50, 30, 11));
  s = cut(s, box(35, 5, -1, 45, 12, 11));
  return cut(s, box(-1, 33, 7, 61, 36, 11));
}

}  // namespace

TEST(edges_meet_convex_concave_or_smooth) {
  const TopoDS_Shape b = box(0, 0, 0, 40, 30, 20);
  Recognizer r(b);
  for (int e = 0; e < r.edge_count(); ++e) CHECK(r.join(e) == Recognizer::Join::Convex);
  const TopoDS_Shape p = features_plate();
  Recognizer q(p);
  CHECK(q.join(edge_at(p, gp_Pnt(15, 5, 10))) == Recognizer::Join::Concave);   // the boss's foot
  CHECK(q.join(edge_at(p, gp_Pnt(15, 5, 15))) == Recognizer::Join::Convex);    // its top
  CHECK(q.join(edge_at(p, gp_Pnt(42.5, 20, 6))) == Recognizer::Join::Concave);  // the pocket's floor
  const TopoDS_Shape k = blended_block();
  Recognizer s(k);
  CHECK(s.join(edge_at(k, gp_Pnt(20, 3, 20))) == Recognizer::Join::Smooth);  // a round's edge on the top
}

TEST(holes_through_blind_counterbored_countersunk) {
  const TopoDS_Shape s = holes_plate();
  Recognizer r(s);
  const auto holes = r.all("hole");
  show("holes", holes);
  CHECK_EQ(holes.size(), size_t(9));
  int through6 = 0, bores = 0, sinks = 0, blind = 0;
  for (const auto& h : holes) {
    const double d = h.params["diameter"];
    if (h.params["type"] == "simple" && h.params["through"] == true && std::fabs(d - 6) < 1e-6) {
      ++through6;
      CHECK_NEAR(h.params["depth"].get<double>(), 10, 1e-6);
      CHECK_EQ(h.label, "Hole \xC3\x98" "6 through");
    }
    if (h.params["type"] == "counterbore") {
      ++bores;
      CHECK_NEAR(d, 4, 1e-6);
      CHECK_NEAR(h.params["cb_diameter"].get<double>(), 8, 1e-6);
      CHECK_NEAR(h.params["cb_depth"].get<double>(), 3, 1e-6);
      CHECK(h.params["through"] == true);
    }
    if (h.params["type"] == "countersink") {
      ++sinks;
      CHECK_NEAR(d, 4, 1e-6);
      CHECK_NEAR(h.params["cs_diameter"].get<double>(), 8, 1e-3);
      CHECK_NEAR(h.params["cs_angle"].get<double>(), 90, 1e-3);
    }
    if (h.params["through"] == false) {
      ++blind;
      if (std::fabs(d - 5) < 1e-6) {
        CHECK_NEAR(h.params["depth"].get<double>(), 6, 1e-6);
        CHECK_NEAR(h.params["tip_angle"].get<double>(), 118, 1e-3);
      } else {
        CHECK_NEAR(d, 6, 1e-6);
        CHECK_NEAR(h.params["depth"].get<double>(), 4, 1e-6);
      }
    }
  }
  CHECK_EQ(through6, 4);
  CHECK_EQ(bores, 2);
  CHECK_EQ(sinks, 1);
  CHECK_EQ(blind, 2);
  // From one wall: the hole first, then the holes like it (the flat blind Ø6 is not like it).
  const int wall = face_at(s, gp_Pnt(10, 10, 5), GeomAbs_Cylinder);
  const auto around = r.around({wall}, {}, {"hole", "fillet", "chamfer", "boss", "pocket", "wall", "tangent", "loop", "similar"});
  show("around a hole wall", around);
  CHECK(!around.empty() && around.front().kind == "hole");
  bool similar = false;
  for (const auto& g : around)
    if (g.kind == "similar" && g.rule == "hole") {
      similar = true;
      CHECK_EQ(g.params["count"], 4);
      CHECK_EQ(g.label, "All holes \xC3\x98" "6 through \xC2\xB7 4");
    }
  CHECK(similar);
  for (const auto& g : around) CHECK(g.kind != "pocket");  // the pocket a hole also is: said once, as the hole
  // A rim edge: the hole holds it, its loop is the one circle.
  const int rim = edge_at(s, gp_Pnt(7, 10, 10));
  const auto byEdge = r.around({}, {rim}, {"hole", "loop"});
  CHECK(!byEdge.empty() && byEdge.front().kind == "hole");
  const auto loops = r.loops({rim});
  CHECK_EQ(loops.size(), size_t(1));
  CHECK_EQ(loops.front().edges.size(), size_t(1));
  // Not holes: the plate's faces, a boss, a concave round.
  CHECK(!r.hole(face_at(s, gp_Pnt(40, 25, 10))));
}

TEST(fillets_chains_and_radii) {
  const TopoDS_Shape s = blended_block();
  Recognizer r(s);
  const int top = face_at(s, gp_Pnt(20, 15, 20), GeomAbs_Plane);
  const int round = face_at(s, gp_Pnt(20, 1, 19), GeomAbs_Cylinder);
  const auto chain = r.fillet(round);
  CHECK(chain.has_value());
  std::printf("  chain: %s, %zu faces\n", chain->label.c_str(), chain->faces.size());
  CHECK_NEAR(chain->params["radius"].get<double>(), 3, 1e-6);
  CHECK(chain->params["convex"] == true);
  CHECK(chain->faces.size() >= 12);  // eight rounds and the corners between them
  CHECK_EQ(r.all("fillet").size(), size_t(1));
  CHECK(!r.fillet(top));
  // Tangent faces from the top: everything but the bottom.
  CHECK_EQ(r.tangent_faces({top}).faces.size(), size_t(r.face_count() - 1));
  // Tangent edges from one straight edge of the top: across the corners' arcs and down the upright rounds' edges.
  const auto ring = r.tangent_edges({edge_at(s, gp_Pnt(20, 3, 20))});
  CHECK_EQ(ring.edges.size(), size_t(5));
  // All fillets of the radius.
  bool all = false;
  for (const auto& g : r.similar_faces(round))
    if (g.rule == "fillet") all = g.faces.size() == chain->faces.size();
  CHECK(all);
  // A concave round in an inside corner is a fillet, not a hole.
  TopoDS_Shape l = fuse(box(0, 0, 0, 40, 30, 10), box(0, 0, 10, 10, 30, 30));
  BRepFilletAPI_MakeFillet mk(l);
  mk.Add(2, edge_of(l, edge_at(l, gp_Pnt(10, 15, 10))));
  mk.Build();
  l = mk.Shape();
  Recognizer q(l);
  const int inside = face_at(l, gp_Pnt(10.6, 15, 10.6), GeomAbs_Cylinder);
  const auto fillet = q.fillet(inside);
  CHECK(fillet.has_value());
  CHECK_NEAR(fillet->params["radius"].get<double>(), 2, 1e-6);
  CHECK(fillet->params["convex"] == false);
  CHECK_EQ(fillet->label, "Fillet R2");
  CHECK(!q.hole(inside));
}

TEST(chamfers_and_what_is_not_one) {
  const TopoDS_Shape b = box(0, 0, 0, 40, 30, 20);
  BRepFilletAPI_MakeChamfer mk(b);
  for (const gp_Pnt& p : {gp_Pnt(20, 0, 20), gp_Pnt(20, 30, 20), gp_Pnt(0, 15, 20), gp_Pnt(40, 15, 20)}) mk.Add(2, edge_of(b, edge_at(b, p)));
  mk.Build();
  const TopoDS_Shape s = mk.Shape();
  Recognizer r(s);
  const int face = face_at(s, gp_Pnt(20, 1, 19), GeomAbs_Plane);
  const auto chain = r.chamfer(face);
  CHECK(chain.has_value());
  std::printf("  chamfer: %s\n", chain->label.c_str());
  CHECK_NEAR(chain->params["distance"].get<double>(), 2, 1e-6);
  CHECK_EQ(chain->faces.size(), size_t(4));
  CHECK_EQ(r.all("chamfer").size(), size_t(1));
  CHECK(!r.chamfer(face_at(s, gp_Pnt(20, 15, 20), GeomAbs_Plane)));
  // The sides of a thin plate meet its faces square: no bevel.
  Recognizer thin(box(0, 0, 0, 60, 40, 2));
  CHECK(thin.all("chamfer").empty());
}

TEST(bosses_pockets_windows_and_slots) {
  const TopoDS_Shape s = features_plate();
  Recognizer r(s);
  const auto boss = r.boss_or_pocket({face_at(s, gp_Pnt(15, 10, 15))});
  CHECK(boss.has_value());
  std::printf("  boss: %s\n", boss->label.c_str());
  CHECK_EQ(boss->kind, "boss");
  CHECK_EQ(boss->faces.size(), size_t(5));
  CHECK(std::abs(boss->params.value("height", 0.0) - 5) < 1e-6);  // out of the plate's top
  const auto pocket = r.boss_or_pocket({face_at(s, gp_Pnt(42.5, 25, 6))});
  CHECK(pocket.has_value());
  CHECK_EQ(pocket->kind, "pocket");
  CHECK_EQ(pocket->faces.size(), size_t(5));
  CHECK(std::abs(pocket->params.value("depth", 0.0) - 4) < 1e-6);
  const auto window = r.boss_or_pocket({face_at(s, gp_Pnt(35, 8.5, 5))});
  CHECK(window.has_value());
  CHECK_EQ(window->kind, "pocket");
  CHECK_EQ(window->faces.size(), size_t(4));
  const auto slot = r.boss_or_pocket({face_at(s, gp_Pnt(30, 34.5, 7))});
  CHECK(slot.has_value());
  CHECK_EQ(slot->kind, "pocket");
  CHECK_EQ(slot->faces.size(), size_t(3));
  CHECK(!r.boss_or_pocket({face_at(s, gp_Pnt(30, 20, 0))}));  // the bottom is the body, not a detail
  CHECK_EQ(r.all("boss").size(), size_t(1));
  CHECK_EQ(r.all("pocket").size(), size_t(1));  // the closed one; windows and slots are found from a pick
  // A pick's search (the loops nearest it) finds what the walk over every loop finds, on a fresh recogniser too.
  CHECK(r.all("boss").front().faces == boss->faces && r.all("pocket").front().faces == pocket->faces);
  Recognizer fresh(s);
  CHECK(fresh.boss_or_pocket({face_at(s, gp_Pnt(42.5, 25, 6))})->faces == pocket->faces);
}

TEST(walls_of_a_shell) {
  const TopoDS_Shape b = box(0, 0, 0, 40, 30, 20);
  TopTools_ListOfShape open;
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(b, TopAbs_FACE, faces);
  open.Append(faces(face_at(b, gp_Pnt(20, 15, 20)) + 1));
  BRepOffsetAPI_MakeThickSolid mk;
  mk.MakeThickSolidByJoin(b, open, -2, 1e-4);
  const TopoDS_Shape s = mk.Shape();
  Recognizer r(s);
  const auto wall = r.wall(face_at(s, gp_Pnt(0, 15, 10)));
  CHECK(wall.has_value());
  std::printf("  wall: %s, %zu faces\n", wall->label.c_str(), wall->faces.size());
  CHECK_NEAR(wall->params["thickness"].get<double>(), 2, 1e-6);
  CHECK(wall->faces.size() >= 3);  // outside, inside and the rim
  CHECK_EQ(wall->label, "Wall 2 mm");
  bool similar = false;
  for (const auto& g : r.similar_faces(face_at(s, gp_Pnt(0, 15, 10))))
    if (g.rule == "wall") similar = g.faces.size() >= 8;
  CHECK(similar);
  Recognizer solid(b);
  CHECK(!solid.wall(face_at(b, gp_Pnt(0, 15, 10))));
}

TEST(similar_faces_and_edges_by_rule) {
  const TopoDS_Shape s = holes_plate();
  Recognizer r(s);
  const int top = face_at(s, gp_Pnt(40, 25, 10), GeomAbs_Plane);
  bool normal = false;
  for (const auto& g : r.similar_faces(top))
    if (g.rule == "normal") normal = g.faces.size() == 4;  // the top, the counterbores' steps and the flat floor
  CHECK(normal);
  const TopoDS_Shape b = box(0, 0, 0, 40, 30, 20);
  Recognizer q(b);
  const auto rules = q.similar_edges(edge_at(b, gp_Pnt(20, 0, 0)));
  CHECK_EQ(rules.front().rule, "direction");
  CHECK_EQ(rules.front().edges.size(), size_t(4));
}

// A body picked whole (Select similar without a face or edge; what the modal Select by geometry offered).
TEST(body_rules_in_world_coordinates) {
  auto names = [](const std::vector<Recognized>& rules) {
    std::string s;
    for (const auto& g : rules) s += (s.empty() ? "" : " ") + g.rule + "=" + std::to_string(g.faces.empty() ? g.edges.size() : g.faces.size());
    return s;
  };
  Recognizer b(box(0, 0, 0, 10, 8, 3));
  CHECK_EQ(names(b.body_rules()), "top=4 bottom=4 x=4 y=4 z=4 up=1");
  CHECK_EQ(b.body_rules().front().label, "Top perimeter \xC2\xB7 4");
  const TopoDS_Shape s = holes_plate();
  Recognizer r(s);
  const auto rules = r.body_rules();
  show("plate", rules);
  // Top: the outline and the nine holes' rims at z 10 (seams left out); bottom: the outline and the seven holes through;
  // up: the top, the counterbores' steps and the flat floor; no chamfers (left out).
  const std::string all = names(rules);
  CHECK_EQ(all.substr(0, all.find(" circle=")), "top=13 bottom=11 x=4 y=4 z=4");
  CHECK_EQ(all.substr(all.find(" up=")), " up=4 holes=16");
  CHECK_EQ(rules.back().params["count"], 9);  // the holes, not their faces
  CHECK_EQ(rules.back().label, "All holes \xC2\xB7 9");
  // A drawing's lines (no faces, flat): no top or bottom, edges along x and y.
  BRep_Builder bb;
  TopoDS_Compound lines;
  bb.MakeCompound(lines);
  for (auto [a, c] : {std::pair{gp_Pnt(0, 0, 0), gp_Pnt(5, 0, 0)}, {gp_Pnt(0, 2, 0), gp_Pnt(5, 2, 0)}, {gp_Pnt(0, 0, 0), gp_Pnt(0, 2, 0)}, {gp_Pnt(0, 0, 0), gp_Pnt(3, 3, 0)}})
    bb.Add(lines, BRepBuilderAPI_MakeEdge(a, c).Edge());
  CHECK_EQ(count(lines, TopAbs_FACE), 0);
  CHECK_EQ(names(Recognizer(lines).body_rules()), "x=2 y=1");
}

TEST(fitted_surfaces_of_imported_splines) {
  // An export that turned everything into B-splines: holes and rounds are still read.
  const TopoDS_Shape s = BRepBuilderAPI_NurbsConvert(holes_plate()).Shape();
  Recognizer r(s);
  int through6 = 0;
  for (const auto& h : r.all("hole"))
    if (h.params["through"] == true && h.params["type"] == "simple" && std::fabs(h.params["diameter"].get<double>() - 6) < 1e-3) ++through6;
  CHECK_EQ(through6, 4);
  const TopoDS_Shape k = BRepBuilderAPI_NurbsConvert(blended_block()).Shape();
  Recognizer q(k);
  const auto chain = q.fillet(face_at(k, gp_Pnt(20, 1, 19)));
  CHECK(chain.has_value());
  CHECK_NEAR(chain->params["radius"].get<double>(), 3, 1e-3);
}

namespace {
json feature_cmd(Document& doc, const std::string& kind, const json& inputs, const std::string& name = {}) {
  json args = {{"kind", kind}, {"inputs", inputs}};
  if (!name.empty()) args["name"] = name;
  return commands::run("feature", args, &doc);
}
}  // namespace

TEST(remove_faces_deletes_holes_bosses_and_rounds) {
  Document doc = Document::create();
  import_brep(doc, brep_from_shape(holes_plate()), "Plate");
  const std::string body = resolve(doc).all_bodies().front();
  auto shape = [&] { return body_shape(doc, resolve(doc).node(body)->body_key); };
  const double before = volume(shape());
  // The four through Ø6 holes by a rule: they go, the blind Ø6 stays.
  const json rule = {{"body", body}, {"kind", "face"}, {"select", {{"recognized", "hole"}, {"diameter", 6}, {"through", true}}}};
  const json made = feature_cmd(doc, "remove_faces", {{"faces", {rule}}}, "No mounting holes");
  CHECK_EQ(made["body_ids"][0], body);
  CHECK_NEAR(volume(shape()) - before, 4 * M_PI * 9 * 10, 1e-3);
  Recognizer r(shape());
  int six = 0;
  for (const auto& h : r.all("hole")) six += std::fabs(h.params["diameter"].get<double>() - 6) < 1e-6;
  CHECK_EQ(six, 1);
  const Scene resolved = resolve(doc);
  const opad::Feature* f = resolved.feature(made["feature_id"]);
  CHECK(f && f->result.contains("selected"));
  // A boss and a round on a block of their own.
  Document parts = Document::create();
  import_brep(parts, brep_from_shape(features_plate()), "Plate");
  const std::string plate = resolve(parts).all_bodies().front();
  auto plateShape = [&] { return body_shape(parts, resolve(parts).node(plate)->body_key); };
  const TopoDS_Shape p = plateShape();
  const auto boss = Recognizer(p).boss_or_pocket({face_at(p, gp_Pnt(15, 10, 15))});
  json refs = json::array();
  for (int i : boss->faces) refs.push_back(plate + "/face/" + std::to_string(i));
  const double plateVolume = volume(p);
  feature_cmd(parts, "remove_faces", {{"faces", refs}});
  CHECK_NEAR(plateVolume - volume(plateShape()), 20 * 10 * 5, 1e-3);
  CHECK(Recognizer(plateShape()).all("boss").empty());
  // A round on an upright edge: the sharp corner comes back.
  Document rounded = Document::create();
  const TopoDS_Shape b = box(0, 0, 0, 40, 30, 20);
  BRepFilletAPI_MakeFillet mk(b);
  mk.Add(4, edge_of(b, edge_at(b, gp_Pnt(40, 30, 10))));
  mk.Build();
  import_brep(rounded, brep_from_shape(mk.Shape()), "Block");
  const std::string block = resolve(rounded).all_bodies().front();
  const TopoDS_Shape withRound = body_shape(rounded, resolve(rounded).node(block)->body_key);
  feature_cmd(rounded, "remove_faces", {{"faces", {block + "/face/" + std::to_string(face_at(withRound, gp_Pnt(39, 29, 10), GeomAbs_Cylinder))}}});
  CHECK_NEAR(volume(body_shape(rounded, resolve(rounded).node(block)->body_key)), 40 * 30 * 20, 1e-3);
  // Faces whose neighbours cannot close the gap: refused with a reason, nothing appended.
  Document blended = Document::create();
  import_brep(blended, brep_from_shape(blended_block()), "Block");
  const std::string soft = resolve(blended).all_bodies().front();
  const TopoDS_Shape k = body_shape(blended, resolve(blended).node(soft)->body_key);
  const size_t ops = blended.ops.size();
  bool refused = false;
  try {
    feature_cmd(blended, "remove_faces", {{"faces", {soft + "/face/" + std::to_string(face_at(k, gp_Pnt(20, 1, 19), GeomAbs_Cylinder))}}});
  } catch (const std::exception& e) {
    refused = std::string(e.what()).find("cannot be removed") != std::string::npos;
    std::printf("  refused: %s\n", e.what());
  }
  CHECK(refused);
  CHECK_EQ(blended.ops.size(), ops);
  // The kind is in the table the panel, the CLI and MCP read.
  const auto* spec = design::feature_spec("remove_faces");
  CHECK(spec && spec->group == "modify" && spec->icon == "removeFaces");
}

TEST(a_rule_keeps_removing_after_a_regeneration) {
  Document doc = Document::create();
  commands::run("param", {{"name", "L"}, {"expr", "80 mm"}}, &doc);
  const json plate = feature_cmd(doc, "box", {{"length", "L"}, {"width", "50 mm"}, {"height", "10 mm"}, {"centered", false}}, "Plate");
  const std::string body = plate["body_ids"][0];
  for (double x : {10.0, 30.0})
    feature_cmd(doc, "cylinder", {{"plane", {{"origin", {x, 25, -1}}, {"normal", {0, 0, 1}}}}, {"diameter", "6 mm"}, {"height", "12 mm"}, {"operation", "cut"}});
  feature_cmd(doc, "remove_faces", {{"faces", {{{"body", body}, {"kind", "face"}, {"select", {{"recognized", "hole"}, {"diameter", 6}}}}}}}, "Plugged");
  auto shape = [&] { return body_shape(doc, resolve(doc).node(body)->body_key); };
  CHECK_NEAR(volume(shape()), 80 * 50 * 10, 1e-3);
  commands::run("param", {{"name", "L"}, {"expr", "100 mm"}}, &doc);
  CHECK(resolve(doc).unresolved.empty());
  CHECK_NEAR(volume(shape()), 100 * 50 * 10, 1e-3);
  CHECK(Recognizer(shape()).all("hole").empty());
}

TEST(related_and_queries_offer_the_recognised_groups) {
  Document doc = Document::create();
  import_brep(doc, brep_from_shape(holes_plate()), "Plate");
  const std::string body = resolve(doc).all_bodies().front();
  const TopoDS_Shape s = body_shape(doc, resolve(doc).node(body)->body_key);
  const std::string wall = body + "/face/" + std::to_string(face_at(s, gp_Pnt(10, 10, 5), GeomAbs_Cylinder));
  const json r = commands::run("related", {{"refs", {wall}}}, &doc);
  const json& first = r["candidates"][0];
  CHECK_EQ(first["kind"], "hole");  // before the import that holds every face
  CHECK_EQ(first["contains_selection"], true);
  CHECK_EQ(first["params"]["type"], "simple");
  bool similar = false, imported = false;
  for (const auto& c : r["candidates"]) {
    if (c["kind"] == "similar" && c["rule"] == "hole") similar = c["params"]["count"] == 4;
    if (c["kind"] == "import") imported = true;
  }
  CHECK(similar && imported);
  CHECK_EQ(r["candidates"].back()["kind"], "body");
  // Only some kinds; a body picked whole lists every group of them.
  const json holes = commands::run("related", {{"refs", {body}}, {"kinds", {"hole"}}}, &doc);
  CHECK_EQ(holes["candidates"].size(), size_t(9));
  CHECK_THROWS(commands::run("related", {{"refs", {wall}}, {"kinds", {"holes"}}}, &doc));
  // query_entities narrows to a group's faces.
  const json q = commands::run("query_entities", {{"body", body}, {"kind", "face"}, {"filters", {{"recognized", "hole"}, {"diameter", 6}, {"through", true}, {"surface", "cylinder"}}}}, &doc);
  CHECK_EQ(q["total"], 4);
  CHECK_THROWS(commands::run("query_entities", {{"body", body}, {"kind", "edge"}, {"filters", {{"recognized", "hole"}}}}, &doc));
}

CHECK_MAIN()
