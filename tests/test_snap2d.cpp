// Object snaps in a plane (UI-90, core/src/snap2d.cpp): the order (points, then midpoints, quadrants and intersections, then
// the nearest point on a curve), the kinds switched off, exact circles, sources placed in a common plane with intersections
// between them, and indexes built from OCCT edges (arcs either way round).
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>

#include <cmath>

#include "check.hpp"
#include "opad/snap2d.hpp"

using namespace opad::snap2d;

namespace {
Index room() {
  Index i;
  i.add_line({0, 0}, {100, 0});
  i.add_line({50, -50}, {50, 50});
  i.add_arc({200, 0}, 20, 0, 2 * M_PI, 1e-3);   // a circle
  i.add_arc({300, 0}, 10, 0, M_PI / 2, 1e-3);  // a quarter, counter-clockwise from +x
  i.add_line({150, 10}, {250, 10});              // crosses the circle
  i.add_point({0, 200});
  i.finish();
  return i;
}
Snap at(const Index& i, P p, double aperture = 3, const Kinds& kinds = {}) { return snap({Placed{&i}}, p, aperture, kinds); }
bool same(P a, P b, double eps = 1e-9) { return std::hypot(a.x - b.x, a.y - b.y) <= eps; }
}  // namespace

TEST(points_win_then_midpoints_and_crossings_then_curves) {
  const Index i = room();
  Snap s = at(i, {1, 1});
  CHECK(s.kind == Kind::Endpoint && same(s.at, {0, 0}));
  s = at(i, {49, 1});  // the midpoint of one line is where the other crosses it
  CHECK((s.kind == Kind::Midpoint || s.kind == Kind::Intersection) && same(s.at, {50, 0}));
  s = at(i, {50.5, 30});
  CHECK(s.kind == Kind::Nearest && same(s.at, {50, 30}));
  s = at(i, {25, 2});
  CHECK(s.kind == Kind::Nearest && same(s.at, {25, 0}));
  CHECK(!at(i, {25, 4}));  // outside the aperture
  CHECK(at(i, {1, 199}).kind == Kind::Endpoint);  // a point of its own
}

TEST(circles_centres_quadrants_and_exact_points) {
  const Index i = room();
  Snap s = at(i, {219, 1});
  CHECK(s.kind == Kind::Quadrant && same(s.at, {220, 0}));
  s = at(i, {201, 1});
  CHECK(s.kind == Kind::Center && same(s.at, {200, 0}));
  const P onCircle{200 + 20 * std::cos(-M_PI / 3), 20 * std::sin(-M_PI / 3)};
  s = at(i, {onCircle.x + 0.5, onCircle.y - 0.3}, 1);
  CHECK(s.kind == Kind::Nearest);
  CHECK_NEAR(std::hypot(s.at.x - 200, s.at.y), 20, 1e-9);  // on the circle, not on a chord
  s = at(i, {217.5, 10.3}, 2);
  CHECK(s.kind == Kind::Intersection && same(s.at, {200 + std::sqrt(300.0), 10}));  // line and circle meet exactly
  s = at(i, {300 + 10 / std::sqrt(2.0) + 0.5, 10 / std::sqrt(2.0)}, 1);  // the quarter's midpoint
  CHECK(s.kind == Kind::Midpoint && same(s.at, {300 + 10 / std::sqrt(2.0), 10 / std::sqrt(2.0)}));
  CHECK(at(i, {300.5, 10.5}).kind == Kind::Endpoint);  // its end, a quadrant too: points first
  CHECK(at(i, {289, 1}).kind != Kind::Quadrant);       // (290, 0) is not on the quarter
}

TEST(kinds_switched_off) {
  const Index i = room();
  Kinds k;
  k.endpoint = false;
  Snap s = at(i, {1, 1}, 3, k);
  CHECK(s.kind == Kind::Nearest && same(s.at, {1, 0}));
  k.nearest = false;
  CHECK(!at(i, {1, 1}, 3, k));
  Kinds none;
  none.endpoint = none.midpoint = none.center = none.quadrant = none.intersection = none.nearest = false;
  CHECK(!at(i, {49, 1}, 3, none));
}

TEST(perpendicular_and_tangent_from_the_point_before) {
  const Index i = room();
  const P from{30, 40};
  Snap s = snap({Placed{&i}}, {29, 1}, 3, {}, &from);  // square onto the line below: (30, 0), before the nearest point
  CHECK(s.kind == Kind::Perpendicular && same(s.at, {30, 0}));
  CHECK(snap({Placed{&i}}, {29, 1}, 3).kind == Kind::Nearest);  // no point before: the nearest point
  const P slant{230, 40};  // square onto the circle: on the line through the centre, either side
  s = snap({Placed{&i}}, {212.5, 15}, 2, {}, &slant);
  CHECK(s.kind == Kind::Perpendicular && same(s.at, {212, 16}));
  s = snap({Placed{&i}}, {187.5, -15}, 2, {}, &slant);
  CHECK(s.kind == Kind::Perpendicular && same(s.at, {188, -16}));
  const P away{200, 60};
  // Touching the circle from (200, 60): 20 from the centre's 60, at acos(1/3) either side of the way to the point.
  const double turn = std::acos(20.0 / 60), base = M_PI / 2;
  const P touch{200 + 20 * std::cos(base - turn), 20 * std::sin(base - turn)};
  s = snap({Placed{&i}}, {touch.x + 0.4, touch.y + 0.3}, 1.5, {}, &away);
  CHECK(s.kind == Kind::Tangent && same(s.at, touch));
  CHECK_NEAR((s.at.x - 200) * (away.x - s.at.x) + s.at.y * (away.y - s.at.y), 0, 1e-9);  // the radius square to the line from the point
  Kinds k;
  k.tangent = false;
  CHECK(snap({Placed{&i}}, {touch.x + 0.4, touch.y + 0.3}, 1.5, k, &away).kind == Kind::Nearest);
  // The quarter (300, 0) r 10, 0 to 90 degrees: square from (300, -30) lands on (300, -10), off the arc: nothing there.
  const P below{300, -30};
  CHECK(snap({Placed{&i}}, {300.5, -9.5}, 1.5, {}, &below).kind != Kind::Perpendicular);
  CHECK(std::string(kind_name(Kind::Tangent)) == "tangent");
}

TEST(sources_in_a_common_plane) {
  Index a, b;
  a.add_line({0, 0}, {100, 0});
  a.finish();
  b.add_line({0, 0}, {0, 120});
  b.finish();
  Placed moved{&b, 1, 0, 75, 0, 1, -50};  // b's line from (75, -50) to (75, 70)
  Snap s = snap({Placed{&a}, moved}, {76, 1}, 3);
  CHECK(s.kind == Kind::Intersection && same(s.at, {75, 0}) && s.source != s.otherSource);
  Placed turned{&b, 0, -1, 0, 1, 0, 200};  // turned a quarter: (0, 120) lands on (-120, 200)
  s = snap({Placed{&a}, turned}, {-119, 201}, 3);
  CHECK(s.kind == Kind::Endpoint && s.source == 1 && same(s.at, {-120, 200}));
}

TEST(indexes_from_edges) {
  TopoDS_Compound shape;
  BRep_Builder builder;
  builder.MakeCompound(shape);
  builder.Add(shape, BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(40, 0, 0)).Edge());
  builder.Add(shape, BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(100, 0, 0), gp::DZ()), 10)).Edge());
  // A quarter about -z from +x: clockwise, ends at (200, 0) - (190, -10).
  builder.Add(shape, BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(190, 0, 0), -gp::DZ(), gp::DX()), 10), 0, M_PI / 2).Edge());
  builder.Add(shape, BRepBuilderAPI_MakeVertex(gp_Pnt(0, 50, 0)).Vertex());
  const auto i = index_shape(shape);
  CHECK(!i->empty());
  CHECK_NEAR(i->z, 0, 1e-12);
  CHECK(at(*i, {20.5, 0.5}).kind == Kind::Midpoint);
  CHECK(at(*i, {110.5, 0.5}).kind == Kind::Quadrant);
  CHECK(at(*i, {100.5, 0.5}).kind == Kind::Center);
  Snap s = at(*i, {190.5, -10.5});
  CHECK(s.kind == Kind::Endpoint && same(s.at, {190, -10}, 1e-7));
  s = at(*i, {190 + 10 / std::sqrt(2.0), -10 / std::sqrt(2.0) - 0.5}, 1);
  CHECK(s.kind == Kind::Midpoint && same(s.at, {190 + 10 / std::sqrt(2.0), -10 / std::sqrt(2.0)}, 1e-7));
  CHECK(at(*i, {0.5, 50.5}).kind == Kind::Endpoint);  // the loose vertex
  CHECK(std::string(kind_name(Kind::Quadrant)) == "quadrant");
}

CHECK_MAIN()
