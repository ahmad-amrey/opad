// Hidden-line projection (core/src/drawing, TODO 11 UI-77): the exact, draft and hybrid tiers on small parts, typed
// curves and their sources, instances, the cache and cancelling.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "check.hpp"
#include "opad/drawing/projection.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/step_io.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {

TopoDS_Shape block_with_hole() {  // 40 x 30 x 20, a 8 mm hole through it along Z at (20, 15)
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(40, 30, 20).Shape();
  const TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 15, -1), gp_Dir(0, 0, 1)), 4, 22).Shape();
  return BRepAlgoAPI_Cut(box, hole).Shape();
}

Document doc_of(const std::vector<TopoDS_Shape>& shapes) {
  Document d = Document::create();
  for (size_t i = 0; i < shapes.size(); ++i) import_brep(d, brep_from_shape(shapes[i]), "part " + std::to_string(i));
  return d;
}

ViewSpec spec_of(const std::string& view, Quality q) {
  ViewSpec s = ViewSpec::preset(view);
  s.quality = q;
  return s;
}

double total(const ViewGeometry& g, bool hidden, int kind = -1) {
  double l = 0;
  for (const auto& c : g.curves)
    if (c.hidden == hidden && (kind < 0 || static_cast<int>(c.kind) == kind)) l += c.length();
  return l;
}

int count(const ViewGeometry& g, Curve::Type type, bool hidden) {
  int n = 0;
  for (const auto& c : g.curves) n += c.type == type && c.hidden == hidden;
  return n;
}

// Every point of the curve lies on the projection of the body edge it names.
void check_sources(const Document& doc, const Scene& scene, const ViewGeometry& g) {
  int checked = 0;
  for (const auto& c : g.curves) {
    if (c.edge < 0) continue;
    CHECK(c.body >= 0 && c.body < static_cast<int>(g.bodies.size()));
    const TopoDS_Shape shape = node_world_shape(doc, scene, g.bodies[static_cast<size_t>(c.body)].node);
    BRepAdaptor_Curve edge(TopoDS::Edge(subshape(shape, Ref::Kind::Edge, c.edge)));
    std::vector<Vec2> projected;
    for (int i = 0; i <= 400; ++i) {
      const gp_Pnt p = edge.Value(edge.FirstParameter() + (edge.LastParameter() - edge.FirstParameter()) * i / 400);
      projected.push_back({p.X() * g.x[0] + p.Y() * g.x[1] + p.Z() * g.x[2], p.X() * g.y[0] + p.Y() * g.y[1] + p.Z() * g.y[2]});
    }
    for (const auto& q : c.sample(1e-3)) {
      double best = 1e300;
      for (size_t i = 1; i < projected.size(); ++i) {
        const double ex = projected[i][0] - projected[i - 1][0], ey = projected[i][1] - projected[i - 1][1];
        const double t = std::clamp(((q[0] - projected[i - 1][0]) * ex + (q[1] - projected[i - 1][1]) * ey) / std::max(ex * ex + ey * ey, 1e-18), 0.0, 1.0);
        best = std::min(best, std::hypot(q[0] - projected[i - 1][0] - t * ex, q[1] - projected[i - 1][1] - t * ey));
      }
      CHECK(best < 0.02);
    }
    ++checked;
  }
  CHECK(checked > 0);
}

}  // namespace

// Front view of the block: four visible outline lines and the hole's two hidden silhouettes; both tiers agree and every
// curve names the edge it comes from.
TEST(projection_block_front) {
  const Document doc = doc_of({block_with_hole()});
  const Scene scene = resolve(doc);
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    const auto g = project(doc, scene, spec_of("front", q), {}, false);
    CHECK(g->tier == q);
    CHECK_EQ(count(*g, Curve::Type::Line, false), 4);
    CHECK_NEAR(total(*g, false), 2 * 40 + 2 * 20, 0.01);
    CHECK_NEAR(total(*g, true, static_cast<int>(Curve::Kind::Silhouette)), 2 * 20, 0.01);
    int at16 = 0, at24 = 0;
    for (const auto& c : g->curves)
      if (c.kind == Curve::Kind::Silhouette) {
        CHECK(c.hidden && c.type == Curve::Type::Line && (c.edge >= 0 || c.face >= 0));
        at16 += std::fabs(c.pts[0][0] - 16) < 1e-6 && std::fabs(c.pts[1][0] - 16) < 1e-6;
        at24 += std::fabs(c.pts[0][0] - 24) < 1e-6 && std::fabs(c.pts[1][0] - 24) < 1e-6;
      }
    CHECK(at16 == 1 && at24 == 1);
    CHECK_NEAR(g->bounds[0], 0, 1e-6);
    CHECK_NEAR(g->bounds[2], 40, 1e-6);
    CHECK_NEAR(g->bounds[3], 20, 1e-6);
    check_sources(doc, scene, *g);
  }
}

// Seen from the top the hole is a full circle, an analytic arc in both tiers.
TEST(projection_top_view_circle_stays_an_arc) {
  const Document doc = doc_of({block_with_hole()});
  const Scene scene = resolve(doc);
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    const auto g = project(doc, scene, spec_of("top", q), {}, false);
    double span = 0;
    for (const auto& c : g->curves)
      if (c.type == Curve::Type::Arc && !c.hidden) {
        CHECK_NEAR(c.r1, 4, 1e-9);
        CHECK_NEAR(c.c[0], 20, 1e-9);
        CHECK_NEAR(c.c[1], 15, 1e-9);
        span += c.a1 - c.a0;
      }
    CHECK_NEAR(span, 2 * M_PI, 1e-6);
  }
}

// A picture (an image canvas, UI-70) is a reference, not a part: views of the block with one lying on its top face, larger
// than it, are the block's views.
TEST(projection_leaves_image_canvases_out) {
  const Document plain = doc_of({block_with_hole()});
  Document doc = plain;
  const auto dir = std::filesystem::temp_directory_path() / ("opad-projection-" + new_uuid());
  std::filesystem::create_directories(dir);
  const auto be32 = [](uint32_t v) { return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; };
  const auto chunk = [&](const std::string& type, const std::string& data) { return be32(uint32_t(data.size())) + type + data + be32(0); };
  const auto pic = dir / "underlay.png";
  std::ofstream(pic, std::ios::binary) << std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", be32(400) + be32(200) + std::string("\x08\x06\0\0\0", 5)) +
                                              chunk("IDAT", "x") + chunk("IEND", "");
  ImportOptions o;
  o.placement = Mat4::translation(-10, -10, 20);
  import_file(doc, pic, o);
  std::filesystem::remove_all(dir);
  CHECK_EQ(resolve(doc).all_bodies().size(), 2u);
  for (const char* view : {"front", "top"}) {
    const auto with = project(doc, resolve(doc), spec_of(view, Quality::Exact), {}, false);
    const auto without = project(plain, resolve(plain), spec_of(view, Quality::Exact), {}, false);
    CHECK_EQ(with->curves.size(), without->curves.size());
    for (size_t i = 0; i < 4; ++i) CHECK_NEAR(with->bounds[i], without->bounds[i], 1e-9);
  }
}

// An isometric view: the rims become ellipses (minor axis = r cos 54.7 deg) and the tiers agree on what is visible, to
// 0.01 mm of line.
TEST(projection_iso_tiers_agree) {
  const Document doc = doc_of({block_with_hole()});
  const Scene scene = resolve(doc);
  const auto exact = project(doc, scene, spec_of("iso", Quality::Exact), {}, false);
  const auto hybrid = project(doc, scene, spec_of("iso", Quality::Hybrid), {}, false);
  CHECK(total(*exact, false) > 200);
  CHECK_NEAR(total(*hybrid, false), total(*exact, false), 0.01);  // the design's 0.01 mm
  CHECK_NEAR(total(*hybrid, true), total(*exact, true), 0.01);
  for (const auto* g : {exact.get(), hybrid.get()}) {
    CHECK(count(*g, Curve::Type::Ellipse, false) >= 1);
    for (const auto& c : g->curves)
      if (c.type == Curve::Type::Ellipse) {
        CHECK_NEAR(c.r1, 4, 1e-9);
        CHECK_NEAR(c.r2, 4 / std::sqrt(3.0), 1e-9);
      }
    check_sources(doc, scene, *g);
  }
}

// B-spline edges keep their degree and poles (projected), so the drawing carries the curve itself.
TEST(projection_splines_keep_their_poles) {
  TColgp_Array1OfPnt poles(1, 4);
  poles(1) = gp_Pnt(0, 0, 0), poles(2) = gp_Pnt(10, 15, 0), poles(3) = gp_Pnt(25, -5, 0), poles(4) = gp_Pnt(35, 10, 0);
  TColStd_Array1OfReal knots(1, 2);
  knots(1) = 0, knots(2) = 1;
  TColStd_Array1OfInteger mults(1, 2);
  mults(1) = 4, mults(2) = 4;
  const Handle(Geom_BSplineCurve) curve = new Geom_BSplineCurve(poles, knots, mults, 3);
  BRepBuilderAPI_MakeWire wire(BRepBuilderAPI_MakeEdge(curve).Edge(), BRepBuilderAPI_MakeEdge(gp_Pnt(35, 10, 0), gp_Pnt(0, 0, 0)).Edge());
  const TopoDS_Shape prism = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire()).Face(), gp_Vec(0, 0, 5)).Shape();
  const Document doc = doc_of({prism});
  const Scene scene = resolve(doc);
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    const auto g = project(doc, scene, spec_of("top", q), {}, false);
    int found = 0;
    for (const auto& c : g->curves) {
      if (c.type != Curve::Type::Spline || c.hidden) continue;
      CHECK_EQ(c.degree, 3);
      CHECK_EQ(c.pts.size(), 4u);
      // The whole top curve; exact HLR may also show part of the bottom one, which lies under it.
      if (std::hypot(c.pts[0][0] - c.pts[3][0], c.pts[0][1] - c.pts[3][1]) < std::hypot(35, 10) - 1e-6) continue;
      const bool reversed = c.pts[0][0] > 1;
      for (int i = 0; i < 4; ++i) {
        const gp_Pnt& p = poles(reversed ? 4 - i : i + 1);
        CHECK_NEAR(c.pts[static_cast<size_t>(i)][0], p.X(), 1e-9);
        CHECK_NEAR(c.pts[static_cast<size_t>(i)][1], p.Y(), 1e-9);
      }
      ++found;
    }
    CHECK(found >= 1);
    check_sources(doc, scene, *g);
  }
}

// Cubic Béziers for writers: chained end to end from the curve's start to its end, every point within the tolerance of
// the curve, with as few pieces as that allows (a quarter turn of an arc; one per span of a cubic spline).
TEST(projection_beziers) {
  auto off = [](const std::vector<Vec2>& line, Vec2 q) {
    double best = 1e300;
    for (size_t i = 1; i < line.size(); ++i) {
      const double ex = line[i][0] - line[i - 1][0], ey = line[i][1] - line[i - 1][1];
      const double t = std::clamp(((q[0] - line[i - 1][0]) * ex + (q[1] - line[i - 1][1]) * ey) / std::max(ex * ex + ey * ey, 1e-30), 0.0, 1.0);
      best = std::min(best, std::hypot(q[0] - line[i - 1][0] - t * ex, q[1] - line[i - 1][1] - t * ey));
    }
    return best;
  };
  auto check = [&](const Curve& c, double tol, size_t pieces) {
    const auto b = c.beziers(tol);
    const auto line = c.sample(tol * 1e-3);
    CHECK_EQ(b.size(), pieces);
    CHECK(std::hypot(b.front()[0][0] - line.front()[0], b.front()[0][1] - line.front()[1]) < 1e-9);
    CHECK(std::hypot(b.back()[3][0] - line.back()[0], b.back()[3][1] - line.back()[1]) < 1e-9);
    double worst = 0;
    for (size_t i = 0; i < b.size(); ++i) {
      if (i > 0) CHECK(std::hypot(b[i][0][0] - b[i - 1][3][0], b[i][0][1] - b[i - 1][3][1]) < 1e-12);
      for (int k = 0; k <= 64; ++k) {
        const double t = k / 64.0, s = 1 - t;
        const Vec2 q{s * s * s * b[i][0][0] + 3 * s * s * t * b[i][1][0] + 3 * s * t * t * b[i][2][0] + t * t * t * b[i][3][0],
                     s * s * s * b[i][0][1] + 3 * s * s * t * b[i][1][1] + 3 * s * t * t * b[i][2][1] + t * t * t * b[i][3][1]};
        worst = std::max(worst, off(line, q));
      }
    }
    CHECK(worst <= tol * 1.01);
  };
  Curve line;
  line.pts = {{1, 2}, {7, -3}};
  check(line, 0.01, 1);
  Curve arc;
  arc.type = Curve::Type::Arc;
  arc.c = {3, 4};
  arc.r1 = arc.r2 = 50;
  arc.a0 = 0.3, arc.a1 = 0.3 + 2 * M_PI;
  check(arc, 0.01, 5);  // at r = 50 a quarter turn is 0.0136 off, a fifth 0.0035
  arc.a1 = 1.2;
  check(arc, 0.01, 1);
  Curve ellipse = arc;
  ellipse.type = Curve::Type::Ellipse;
  ellipse.r2 = 20, ellipse.rot = 0.7, ellipse.a0 = 5.5, ellipse.a1 = 5.5 + 3;
  check(ellipse, 0.01, 3);  // half a turn: two pieces would be 0.0103 off
  Curve cubic;  // two spans
  cubic.type = Curve::Type::Spline;
  cubic.degree = 3;
  cubic.pts = {{0, 0}, {10, 15}, {25, -5}, {35, 10}, {50, 0}};
  cubic.knots = {0, 0, 0, 0, 0.5, 1, 1, 1, 1};
  check(cubic, 0.01, 2);
  Curve quarter = cubic;  // a rational quadratic: a quarter of a circle of radius 10, approximated
  quarter.degree = 2;
  quarter.pts = {{10, 0}, {10, 10}, {0, 10}};
  quarter.weights = {1, std::sqrt(0.5), 1};
  quarter.knots = {0, 0, 0, 1, 1, 1};
  const auto q = quarter.beziers(1e-4);
  CHECK(!q.empty());
  for (const auto& b : q)
    for (int k = 0; k <= 16; ++k) {
      const double t = k / 16.0, s = 1 - t;
      const double x = s * s * s * b[0][0] + 3 * s * s * t * b[1][0] + 3 * s * t * t * b[2][0] + t * t * t * b[3][0];
      const double y = s * s * s * b[0][1] + 3 * s * s * t * b[1][1] + 3 * s * t * t * b[2][1] + t * t * t * b[3][1];
      CHECK(std::fabs(std::hypot(x, y) - 10) < 1e-4);
    }
}

// Two instances of one body: the one behind is hidden by the one in front, and its lines all lie under the front one's
// outline, so none is left; moved aside, both show. Curves name their node.
TEST(projection_instances_and_occlusion) {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 10, 10).Shape();
  Document doc = doc_of({box, box});
  Scene scene = resolve(doc);
  const auto bodies = scene.all_bodies();
  CHECK_EQ(bodies.size(), 2u);
  CHECK_EQ(scene.node(bodies[0])->body_key, scene.node(bodies[1])->body_key);
  doc.append({{"op", "transform"}, {"target", bodies[1]}, {"matrix", Mat4::translation(0, 30, 0).to_json()}});
  scene = resolve(doc);
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    const auto g = project(doc, scene, spec_of("front", q), {}, false);
    CHECK_EQ(g->bodies.size(), 2u);
    const int behind = g->bodies[0].node == bodies[1] ? 0 : 1;
    int behind_any = 0, front_visible = 0;
    for (const auto& c : g->curves) c.body == behind ? ++behind_any : front_visible += !c.hidden;
    CHECK_EQ(behind_any, 0);
    CHECK_EQ(front_visible, 4);
    CHECK_EQ(total(*g, true), 0.0);
  }
  doc.append({{"op", "transform"}, {"target", bodies[1]}, {"matrix", Mat4::translation(30, 30, 0).to_json()}});
  scene = resolve(doc);
  const auto apart = project(doc, scene, spec_of("front", Quality::Hybrid), {}, false);
  CHECK_NEAR(total(*apart, false), 2 * (2 * 20 + 2 * 10), 0.01);
  check_sources(doc, scene, *apart);
}

// Tangent edges are classed (and left out on request); a cylinder's outline is a pair of silhouette lines, a sphere's a circle.
TEST(projection_tangent_edges_and_silhouettes) {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(30, 20, 10).Shape();
  BRepFilletAPI_MakeFillet fillet(box);
  TopExp_Explorer edges(box, TopAbs_EDGE);
  fillet.Add(3, TopoDS::Edge(edges.Current()));
  const Document doc = doc_of({fillet.Shape()});
  const Scene scene = resolve(doc);
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    auto s = spec_of("iso", q);
    const auto with = project(doc, scene, s, {}, false);
    CHECK(total(*with, false, static_cast<int>(Curve::Kind::Tangent)) + total(*with, true, static_cast<int>(Curve::Kind::Tangent)) > 1);
    s.tangent = false;
    const auto without = project(doc, scene, s, {}, false);
    for (const auto& c : without->curves) CHECK(c.kind != Curve::Kind::Tangent);
  }
  const Document round = doc_of({BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5, 12).Shape(),
                                 BRepPrimAPI_MakeSphere(gp_Pnt(30, 0, 6), 4).Shape()});
  const Scene rs = resolve(round);
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    const auto g = project(round, rs, spec_of("front", q), {}, false);
    double lines = 0, circle = 0;
    for (const auto& c : g->curves) {
      if (c.kind != Curve::Kind::Silhouette || c.hidden) continue;
      if (c.type == Curve::Type::Line && std::fabs(std::fabs(c.pts[0][0]) - 5) < 1e-6) lines += c.length();
      if (c.type == Curve::Type::Arc) {
        CHECK_NEAR(c.r1, 4, 1e-6);
        circle += c.a1 - c.a0;
      }
    }
    CHECK_NEAR(lines, 24, 0.01);
    CHECK_NEAR(circle, 2 * M_PI, 1e-3);
  }
}

// Coincident pieces: a cylinder seen from the side has its rims edge-on (exact HLR returns each as two coincident
// segments, its back halves lie under the front ones); an identical cylinder right behind it has the same outline, which
// the hybrid tier sees as visible. One line is left of each, named after the front body, and the tiers agree.
TEST(projection_coincident_pieces) {
  const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5, 12).Shape();
  Document doc = doc_of({cyl, cyl});
  Scene scene = resolve(doc);
  const auto bodies = scene.all_bodies();
  doc.append({{"op", "transform"}, {"target", bodies[1]}, {"matrix", Mat4::translation(0, 40, 0).to_json()}});
  scene = resolve(doc);
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    for (bool both : {false, true}) {
      auto s = spec_of("front", q);
      if (!both) s.nodes = {bodies[0]};
      const auto g = project(doc, scene, s, {}, false);
      const int front = g->bodies[0].node == bodies[0] ? 0 : 1;
      CHECK_NEAR(total(*g, false), 2 * 12 + 2 * 10, 1e-6);
      CHECK_EQ(total(*g, true), 0.0);
      for (const auto& c : g->curves) CHECK(c.body == front && c.type == Curve::Type::Line);
      CHECK_EQ(g->curves.size(), 4u);
      check_sources(doc, scene, *g);
    }
  }
  // Coaxial cylinders seen along their axis: four rims on one circle, one full circle left. The near one is turned a
  // quarter, so its circles start elsewhere (arcs are compared by angle, across 2 pi).
  const Document coax = doc_of({BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5, 10).Shape(),
                                BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 20), gp_Dir(1, 0, 0) ^ gp_Dir(0, 1, 0), gp_Dir(0, 1, 0)), 5, 10).Shape()});
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    const auto g = project(coax, resolve(coax), spec_of("top", q), {}, false);
    CHECK_NEAR(total(*g, false), 2 * M_PI * 5, 1e-6);
    CHECK_EQ(total(*g, true), 0.0);
    double span = 0;
    for (const auto& c : g->curves) span += c.type == Curve::Type::Arc ? c.a1 - c.a0 : 100;
    CHECK_NEAR(span, 2 * M_PI, 1e-9);
  }
  // Partly covered: a wider, lower box behind a box. Its bottom edges run along the front box's bottom edge, so only
  // their ends are left; its top edges show at the ends and are hidden (once) in the middle.
  const Document l = doc_of({BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 10, 10).Shape(), BRepPrimAPI_MakeBox(gp_Pnt(-5, 20, 0), 30, 10, 4).Shape()});
  for (Quality q : {Quality::Exact, Quality::Hybrid}) {
    const auto g = project(l, resolve(l), spec_of("front", q), {}, false);
    CHECK_NEAR(total(*g, false), 2 * 20 + 2 * 10 + 2 * 5 + 2 * 5 + 2 * 4, q == Quality::Exact ? 1e-6 : 0.005);  // hybrid: cuts within a tenth of a pixel
    CHECK_NEAR(total(*g, true), 20, q == Quality::Exact ? 1e-6 : 0.005);
  }
}

// A rim passes behind its own side face where that face turns away from the viewer: the hybrid tier cuts it exactly
// there (the depth buffer's margin let it run on ~0.1 mm), so the tiers agree on cylinders and cones from any side.
TEST(projection_rims_behind_their_side) {
  const Document doc = doc_of({BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5, 12).Shape(),
                               BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(30, 0, 0), gp_Dir(0, 0, 1)), 6, 2, 10).Shape()});
  const Scene scene = resolve(doc);
  for (const auto& view : {ViewSpec::preset("iso"), ViewSpec::from_json({{"dir", {-0.3, -1, 0.5}}, {"up", {0, 0, 1}}}),
                           ViewSpec::from_json({{"dir", {0.2, 0.4, -1}}, {"up", {0, 1, 0}}})}) {
    for (const auto& body : scene.all_bodies()) {
      auto s = view;
      s.nodes = {body};
      s.quality = Quality::Exact;
      const auto exact = project(doc, scene, s, {}, false);
      s.quality = Quality::Hybrid;
      const auto hybrid = project(doc, scene, s, {}, false);
      CHECK(total(*exact, true) > 1);
      CHECK_NEAR(total(*hybrid, false), total(*exact, false), 1e-4);
      CHECK_NEAR(total(*hybrid, true), total(*exact, true), 1e-4);
    }
  }
}

// Silhouettes of freeform faces and of a torus seen askew come from the mesh but are settled onto the surface: splines
// on the true contour (where the normal is across the view) to within the tolerance, which the mesh polyline is not.
TEST(projection_freeform_silhouettes) {
  const double R = 20, r = 5;
  const Document doc = doc_of({BRepPrimAPI_MakeTorus(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), R, r).Shape(),
                               BRepBuilderAPI_NurbsConvert(BRepPrimAPI_MakeSphere(gp_Pnt(60, 0, 0), 8).Shape()).Shape()});
  const Scene scene = resolve(doc);
  const auto g = project(doc, scene, spec_of("iso", Quality::Hybrid), {}, false);
  const gp_Vec x(g->x[0], g->x[1], g->x[2]), y(g->y[0], g->y[1], g->y[2]), d(g->dir[0], g->dir[1], g->dir[2]);
  // The torus' contour: for each u the two v where (cos v cos u, cos v sin u, sin v) . d = 0.
  std::vector<Vec2> contour;
  for (int i = 0; i < 40000; ++i) {
    const double u = 2 * M_PI * i / 40000, v0 = std::atan2(-(std::cos(u) * d.X() + std::sin(u) * d.Y()), d.Z());
    for (double v : {v0, v0 + M_PI}) {
      const gp_Vec p((R + r * std::cos(v)) * std::cos(u), (R + r * std::cos(v)) * std::sin(u), r * std::sin(v));
      contour.push_back({p.Dot(x), p.Dot(y)});
    }
  }
  const gp_Vec centre(60, 0, 0);
  double torus = 0, sphere = 0;
  for (const auto& c : g->curves) {
    if (c.kind != Curve::Kind::Silhouette) continue;
    CHECK(c.type == Curve::Type::Spline && c.face >= 0);
    const bool round = g->bodies[static_cast<size_t>(c.body)].node == scene.all_bodies()[1];
    for (const auto& q : c.sample(1e-4)) {
      if (round) {
        CHECK(std::fabs(std::hypot(q[0] - centre.Dot(x), q[1] - centre.Dot(y)) - 8) < 0.01);
        continue;
      }
      double best = 1e300;
      for (const auto& p : contour) best = std::min(best, std::hypot(p[0] - q[0], p[1] - q[1]));
      CHECK(best < 0.01);
    }
    (round ? sphere : torus) += c.length();
  }
  CHECK_NEAR(sphere, 2 * M_PI * 8, 0.01);
  CHECK(torus > 2 * M_PI * (R + r));
  const auto exact = project(doc, scene, spec_of("iso", Quality::Exact), {}, false);
  CHECK_NEAR(total(*g, false), total(*exact, false), 0.01);
}

// The draft tier: polylines from the polygonal algorithm, roughly where the exact lines are. The box's edges are sharp
// and name their edge; the hole's outline at x = 16 lies inside its cylindrical face and names that face, the one at
// x = 24 runs along the cylinder's seam edge.
TEST(projection_draft_polylines) {
  const Document doc = doc_of({block_with_hole()});
  const Scene scene = resolve(doc);
  const auto g = project(doc, scene, spec_of("front", Quality::Draft), {}, false);
  CHECK(g->tier == Quality::Draft);
  for (const auto& c : g->curves) CHECK(c.type == Curve::Type::Polyline && c.pts.size() >= 2);
  CHECK_NEAR(total(*g, false), 120, 0.5);
  CHECK_NEAR(total(*g, false, static_cast<int>(Curve::Kind::Sharp)), 120, 0.5);
  const TopoDS_Shape shape = node_world_shape(doc, scene, g->bodies[0].node);
  int faces = 0, seams = 0;
  for (const auto& c : g->curves) {
    CHECK(c.body == 0 && (c.edge >= 0) != (c.face >= 0));
    if (c.kind != Curve::Kind::Silhouette) continue;
    CHECK(c.hidden && std::fabs(c.pts.front()[0] - (c.face >= 0 ? 16 : 24)) < 1e-6);
    if (c.face >= 0) CHECK(BRepAdaptor_Surface(TopoDS::Face(subshape(shape, Ref::Kind::Face, c.face))).GetType() == GeomAbs_Cylinder);
    (c.face >= 0 ? faces : seams) += 1;
  }
  CHECK(faces == 1 && seams == 1);
  check_sources(doc, scene, *g);
}

// Options: no hidden lines; an exploded offset moves a node's curves; a triangulation-only body (OBJ) goes hybrid.
TEST(projection_options_offsets_and_meshes) {
  const Document doc = doc_of({block_with_hole()});
  const Scene scene = resolve(doc);
  auto s = spec_of("front", Quality::Auto);
  s.hidden = false;
  const auto plain = project(doc, scene, s, {}, false);
  CHECK(plain->tier == Quality::Exact);
  for (const auto& c : plain->curves) CHECK(!c.hidden);
  s.offsets[scene.all_bodies()[0]] = {10, 0, 5};
  const auto moved = project(doc, scene, s, {}, false);
  CHECK_NEAR(moved->bounds[0], 10, 1e-6);
  CHECK_NEAR(moved->bounds[3], 25, 1e-6);
  s.offsets.clear();
  s.exact_faces = 3;
  CHECK(choose_tier(doc, scene, s) == Quality::Hybrid);
  const auto dir = std::filesystem::temp_directory_path() / ("opad-proj-" + new_uuid());
  std::filesystem::create_directories(dir);
  write_text_file(dir / "cube.obj",
                  "v 0 0 0\nv 10 0 0\nv 10 10 0\nv 0 10 0\nv 0 0 10\nv 10 0 10\nv 10 10 10\nv 0 10 10\n"
                  "f 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\nf 2 3 7\nf 2 7 6\nf 3 4 8\nf 3 8 7\nf 4 1 5\nf 4 5 8\n");
  Document mesh = Document::create();
  import_file(mesh, dir / "cube.obj");
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  const Scene ms = resolve(mesh);
  const auto cube = project(mesh, ms, spec_of("iso", Quality::Auto), {}, false);
  CHECK(cube->tier == Quality::Hybrid);
  CHECK_NEAR(total(*cube, false), 9 * 10 * std::sqrt(2.0 / 3.0), 0.2);
  CHECK_NEAR(total(*cube, true), 3 * 10 * std::sqrt(2.0 / 3.0), 0.2);
}

// Fingerprint cache: the same request gives the same result object, a changed one another fingerprint, and a result
// read back from the user cache equals the computed one. A cancel from the progress callback throws.
TEST(projection_cache_and_cancel) {
  const Document doc = doc_of({block_with_hole()});
  const Scene scene = resolve(doc);
  clear_projection_memory();
  const auto a = project(doc, scene, spec_of("iso", Quality::Hybrid));
  const auto b = project(doc, scene, spec_of("iso", Quality::Hybrid));
  CHECK(a.get() == b.get());
  CHECK(projection_fingerprint(doc, scene, spec_of("front", Quality::Hybrid), Quality::Hybrid) != a->fingerprint);
  CHECK(projection_fingerprint(doc, scene, spec_of("iso", Quality::Exact), Quality::Exact) != a->fingerprint);
  clear_projection_memory();
  const auto c = project(doc, scene, spec_of("iso", Quality::Hybrid));
  CHECK(c.get() != a.get());
  CHECK_EQ(c->serialize(), a->serialize());
  const ViewGeometry back = ViewGeometry::deserialize(a->serialize());
  CHECK_EQ(back.curves.size(), a->curves.size());
  CHECK_EQ(back.to_json().dump(), a->to_json().dump());
  CHECK_THROWS(ViewGeometry::deserialize("OPADPRJ2"));
  for (Quality q : {Quality::Exact, Quality::Hybrid, Quality::Draft}) {
    bool threw = false;
    try {
      project(doc, scene, spec_of("right", q), [](double, const std::string&) { return false; }, false);
    } catch (const Error& e) {
      threw = std::string(e.what()) == "cancelled";
    }
    CHECK(threw);
  }
}

int main(int argc, char** argv) {
  // The cache test reads back from the user cache: give the run its own.
  const auto dir = std::filesystem::temp_directory_path() / ("opad-proj-cache-" + new_uuid());
#ifdef _WIN32
  _putenv_s("OPAD_CACHE_DIR", dir.string().c_str());
#else
  setenv("OPAD_CACHE_DIR", dir.string().c_str(), 1);
#endif
  const int r = check::run_all(argc, argv);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  return r;
}
