#include "check.hpp"
#include "opad/design/sketch_create.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch_shapes.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <algorithm>
#include <cmath>
using namespace opad::design;
using E=SkEntity::Type;

TEST(advanced_primitives_form_exact_profiles) {
  for(const std::string kind:{"rect3","circle2","polygon_outer","cslot","arcslot"}) {
    Sketch sk;
    std::vector<std::pair<double,double>> picks={{0,0},{20,0},{0,10}};
    if(kind=="cslot")picks={{0,0},{20,0},{20,3}};
    if(kind=="arcslot")picks={{0,0},{20,0},{0,20}};
    auto ids=create_primitive(sk,kind,picks,{{"sides",5},{"width",4.0}});
    CHECK(!ids.empty());CHECK(solve(sk).converged);
    auto profiles=sketch_regions(sk,opad::Frame{});
    CHECK_EQ(profiles.size(),size_t(1));
    if(kind=="rect3")CHECK_NEAR(profiles[0].area,200,1e-5);
    if(kind=="circle2")CHECK_NEAR(profiles[0].area,100*M_PI,1e-5);
    if(kind=="arcslot")CHECK_NEAR(profiles[0].area,40*M_PI+4*M_PI,1e-5);
  }
}

TEST(control_spline_and_rational_conic_are_native_curves) {
  Sketch sk;
  auto ids=create_primitive(sk,"control_spline",{{0,0},{5,10},{15,10},{20,0}});
  const auto* e=sk.entity(ids[0]);CHECK_EQ(e->degree,3);
  BRepAdaptor_Curve curve(entity_edge(sk,*e,opad::Frame{}));CHECK(curve.GetType()==GeomAbs_BSplineCurve);
  CHECK_NEAR(curve.Value(0.5).Y(),7.5,1e-8);
  ids=create_primitive(sk,"conic",{{30,0},{40,20},{50,0}},{{"rho",0.5}});
  e=sk.entity(ids[0]);CHECK_EQ(e->degree,2);CHECK_EQ(e->weights.size(),size_t(3));
  BRepAdaptor_Curve conic(entity_edge(sk,*e,opad::Frame{}));CHECK_NEAR(conic.Value(0.5).Y(),10,1e-8);
  const auto before=sk.to_json();CHECK_THROWS(create_primitive(sk,"conic",{{0,0},{1,1},{2,0}},{{"rho",1}}));CHECK(sk.to_json()==before);
}

TEST(tangent_primitives_preserve_tangency) {
  Sketch sk;
  int a=sk.add_point(-20,0,true),b=sk.add_point(20,0,true),c=sk.add_point(0,-20,true),d=sk.add_point(0,20,true);
  int l1=sk.add_line(a,b,true),l2=sk.add_line(c,d,true);
  auto ids=create_primitive(sk,"tangent_circle",{{4,4}},{{"lines",{l1,l2}},{"radius",3.0}});
  CHECK(solve(sk).converged);auto* center=sk.point(sk.entity(ids[0])->p[0]);CHECK_NEAR(center->x,3,1e-7);CHECK_NEAR(center->y,3,1e-7);
  ids=create_primitive(sk,"tangent_arc",{{20,0},{30,10}},{{"line",l1}});
  CHECK(solve(sk).converged);auto* e=sk.entity(ids[0]);CHECK(e->type==E::Arc);
  CHECK_NEAR(sk.point(e->p[0])->x,20,1e-8);CHECK_NEAR(sk.point(e->p[0])->y,10,1e-8);
  // Smooth: the arc goes on from the line's end the way the line went, so an end behind it is reached past half a turn
  // (counter-clockwise from (20, 0) round the centre (20, 10) to (10, 10): three quarters); without it, the shorter arc.
  Sketch smooth=sk,shorter=sk;
  ids=create_primitive(smooth,"tangent_arc",{{20,0},{10,10}},{{"line",l1},{"smooth",true}});
  e=smooth.entity(ids[0]);CHECK(e->type==E::Arc && solve(smooth).converged);
  CHECK_NEAR(smooth.point(e->p[1])->x,20,1e-8);CHECK_NEAR(smooth.point(e->p[1])->y,0,1e-8);CHECK_NEAR(smooth.point(e->p[2])->x,10,1e-8);
  ids=create_primitive(shorter,"tangent_arc",{{20,0},{10,10}},{{"line",l1}});
  e=shorter.entity(ids[0]);CHECK_NEAR(shorter.point(e->p[1])->x,10,1e-8);CHECK_NEAR(shorter.point(e->p[2])->x,20,1e-8);
}
// TODO 10 B4: the basic shapes form exact, solvable profiles.
TEST(basic_shapes_form_exact_profiles) {
  auto area_of = [](const Sketch& sk) {
    double total = 0;
    for (const auto& r : sketch_regions(sk, opad::Frame{})) total += r.area;
    return total;
  };
  auto made = [](const std::string& kind, std::vector<std::pair<double, double>> picks, opad::json options) {
    Sketch sk;
    create_primitive(sk, kind, picks, options);
    CHECK(solve(sk).converged);
    return sk;
  };
  CHECK_NEAR(area_of(made("rect2", {{10, 5}, {0, 0}}, {})), 50, 1e-6);
  CHECK_NEAR(area_of(made("rect_center", {{0, 0}, {10, 5}}, {})), 200, 1e-6);
  CHECK_NEAR(area_of(made("rounded_rect", {{0, 0}, {40, 20}}, {{"radius", 3.0}})), 800 - (4 - M_PI) * 9, 1e-6);
  CHECK_NEAR(area_of(made("rounded_rect", {{20, 10}, {40, 20}}, {{"radius", 3.0}, {"center", true}})), 800 - (4 - M_PI) * 9, 1e-6);
  CHECK_NEAR(area_of(made("slot", {{0, 0}, {30, 0}}, {{"width", 6.0}})), 30 * 6 + M_PI * 9, 1e-6);
  CHECK_NEAR(area_of(made("circle", {{5, 5}}, {{"diameter", 10.0}})), M_PI * 25, 1e-6);
  // A three-point arc passes through its middle point; closed by a line it is a circular segment.
  {
    Sketch sk;
    create_primitive(sk, "arc3", {{-10, 0}, {0, 10}, {10, 0}});
    const SkEntity& arc = sk.entities.back();
    CHECK(arc.type == SkEntity::Type::Arc);
    const SkPoint* o = sk.point(arc.p[0]);
    CHECK_NEAR(std::hypot(o->x, o->y), 0, 1e-9);
    sk.add_line(arc.p[2], arc.p[1]);
    CHECK_NEAR(area_of(sk), M_PI * 50, 1e-6);  // a half disc of radius 10
  }
  // Arc by radius: the shorter way counter-clockwise by default; large and clockwise pick the other arcs.
  for (const auto& [options, area] : std::vector<std::pair<opad::json, double>>{
           {{{"radius", 10.0}}, M_PI * 100 / 2},
           {{{"radius", 10.0}, {"direction", "cw"}}, M_PI * 100 / 2},
           {{{"radius", 20.0}}, 400 * std::asin(0.5) - 10 * std::sqrt(300.0)},
           {{{"radius", 20.0}, {"large", true}}, M_PI * 400 - (400 * std::asin(0.5) - 10 * std::sqrt(300.0))}}) {
    Sketch sk;
    create_primitive(sk, "arc_radius", {{-10, 0}, {10, 0}}, options);
    const SkEntity& arc = sk.entities.back();
    sk.add_line(arc.p[2], arc.p[1]);
    CHECK_NEAR(area_of(sk), area, 1e-6);
  }
}

// TODO 10 B4 acceptance: the Benchy hull section (straight sides, rounded stern corners, a pointed bow of two arcs
// tangent to the sides) without an arc centre worked out by hand.
TEST(a_hull_section_is_one_path) {
  Sketch sk;
  // Stern at x = 0, sides at y = +-12, the bow tip at x = 60; each bow arc leaves its side tangentially at x = 40.
  create_primitive(sk, "path", {{0, -12}, {40, -12}, {60, 0}, {40, 12}, {0, 12}},
                   {{"segments", opad::json::array({"line", "tangent", "tangent_next", "line", "line"})}, {"fillets", opad::json::array({4.0, 0, 0, 0, 4.0})}});
  CHECK(solve(sk).converged);
  const auto regions = sketch_regions(sk, opad::Frame{});
  CHECK_EQ(regions.size(), size_t(1));
  size_t arcs = 0, tangents = 0;
  for (const auto& e : sk.entities) arcs += e.type == SkEntity::Type::Arc;
  for (const auto& c : sk.constraints) tangents += c.type == SkConstraint::Type::Tangent;
  CHECK_EQ(arcs, size_t(4));      // two bow arcs, two stern corners
  CHECK_EQ(tangents, size_t(6));  // each bow arc to its side, each fillet to its two lines
  // The bow is pointed: the two arcs meet at the tip at an angle, and the section is symmetric about y = 0.
  double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
  for (const auto& p : sk.points) {
    x0 = std::min(x0, p.x), x1 = std::max(x1, p.x);
    y0 = std::min(y0, p.y), y1 = std::max(y1, p.y);
  }
  CHECK_NEAR(x1, 60, 1e-9);
  CHECK_NEAR(y0, -y1, 1e-9);
  CHECK(regions[0].area > 60 * 24 * 0.8 && regions[0].area < 60 * 24);
}

// TODO 10 B4 acceptance: the phone frame outline, a rounded rectangle and its concentric inner offset, is two shapes.
TEST(shapes_expand_into_ordinary_geometry) {
  opad::json id_map;
  const opad::json geometry = expand_sketch_shapes(
      {{"points", opad::json::array({{{"x", 100}, {"y", 100}}})},
       {"shapes", opad::json::array({{{"kind", "rounded_rect"}, {"picks", {{0, 0}, {70, 150}}}, {"options", {{"radius", 8.0}}}, {"first_id", 10}},
                                     {{"kind", "offset"}, {"options", {{"shape", 0}, {"distance", -2.0}}}}})}},
      &id_map);
  CHECK(!geometry.contains("shapes"));
  CHECK_EQ(geometry["points"][0]["id"], 1);  // a point without an id got the first free one
  CHECK_EQ(id_map.size(), size_t(2));
  CHECK_EQ(id_map[0]["points"][0], 10);  // first_id: the shape's ids run from 10
  CHECK_EQ(id_map[0]["entities"].size(), size_t(8));
  CHECK(!id_map[1]["entities"].empty());
  const Sketch sk = Sketch::from_json(geometry);
  const auto regions = sketch_regions(sk, opad::Frame{});
  CHECK_EQ(regions.size(), size_t(2));  // the frame between the outlines, and the inside
  double outer = 70 * 150 - (4 - M_PI) * 64, inner = 66 * 146 - (4 - M_PI) * 36, smallest = 1e9;
  for (const auto& r : regions) smallest = std::min(smallest, r.area);
  CHECK_NEAR(smallest, std::min(inner, outer - inner), 1e-4);
  // Mistakes name the shape and what to do.
  bool named = false;
  try { expand_sketch_shapes({{"shapes", opad::json::array({{{"kind", "hexagon"}}})}}); } catch (const opad::Error& e) { named = std::string(e.what()).find("shapes[0] (hexagon): unknown kind; the kinds are") != std::string::npos; }
  CHECK(named);
  named = false;
  try { expand_sketch_shapes({{"points", opad::json::array({{{"id", 5}, {"x", 0}, {"y", 0}}})}, {"shapes", opad::json::array({{{"kind", "line"}, {"picks", {{0, 0}, {1, 1}}}, {"first_id", 3}}})}}); } catch (const opad::Error& e) { named = std::string(e.what()).find("first_id 3 is already taken; the next free id is 6") != std::string::npos; }
  CHECK(named);
}

// TODO 10 B4: text from the built-in font, the same everywhere: closed block letters (profiles to extrude) or
// single strokes.
TEST(text_from_the_built_in_font) {
  Sketch outline;
  create_primitive(outline, "text", {{0, 0}}, {{"text", "Opad 7"}, {"height", 10.0}});
  const auto regions = sketch_regions(outline, opad::Frame{});
  // O, P, A and D each have a hole: the letters are 5 regions and their holes 4 more.
  CHECK_EQ(regions.size(), size_t(9));
  double x1 = -1e9;
  for (const auto& p : outline.points) x1 = std::max(x1, p.x);
  CHECK(x1 > 40 && x1 < 50);  // six advances of 0.9 cap heights, a space of 0.55, the last glyph 0.65 wide
  Sketch again;
  create_primitive(again, "text", {{0, 0}}, {{"text", "Opad 7"}, {"height", 10.0}});
  CHECK(outline.to_json() == again.to_json());
  Sketch strokes;
  create_primitive(strokes, "text", {{0, 0}}, {{"text", "L"}, {"height", 10.0}, {"style", "stroke"}});
  CHECK_EQ(strokes.entities.size(), size_t(2));
  CHECK_EQ(strokes.points.size(), size_t(3));
  Sketch centred;
  create_primitive(centred, "text", {{0, 0}}, {{"text", "H"}, {"align", "center"}, {"style", "stroke"}});
  double lo = 1e9, hi = -1e9;
  for (const auto& p : centred.points) lo = std::min(lo, p.x), hi = std::max(hi, p.x);
  CHECK_NEAR(lo + hi, 0, 1e-9);
  CHECK_THROWS(create_primitive(strokes, "text", {{0, 0}}, {{"text", "\xc3\xa9"}}));
}

CHECK_MAIN()
