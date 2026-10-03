// A shape's typed sizes (app/ShapeInput.hpp, TODO 11 UI-17): where the click goes for a typed length and angle, a
// rectangle's width and height, a slot's width, an arc's radius; what a typed line angle holds once the line is made.
#include "check.hpp"
#include "ShapeInput.hpp"
using namespace shapeinput;

namespace {
const double pi = std::acos(-1.0);
bool near(P p, double u, double v) { return std::fabs(p.u - u) < 1e-9 && std::fabs(p.v - v) < 1e-9; }
}  // namespace

TEST(a_length_and_an_angle_place_the_point_the_pointer_gives_what_is_not_typed) {
  const double length = 50, angle = pi / 6;
  CHECK(near(polar({0, 0}, {3, -7}, &length, &angle), 50 * std::cos(pi / 6), 25));  // both typed: the pointer does not matter
  CHECK(near(polar({10, 10}, {10, 30}, &length, nullptr), 10, 60));               // a length alone: towards the pointer
  CHECK(near(polar({10, 10}, {10, 10}, &length, nullptr), 60, 10));               // the pointer on the base: along +X
  const double up = pi / 2;
  CHECK(near(polar({0, 0}, {4, 12}, nullptr, &up), 0, 12));   // an angle alone: as far along it as the pointer goes
  CHECK(near(polar({0, 0}, {3, -4}, nullptr, &up), 0, 5));    // the pointer behind: its distance
  const double back = -20;
  CHECK(near(polar({0, 0}, {5, 0}, &back, nullptr), -20, 0));  // a negative length goes the other way
}

TEST(a_rectangles_sides_go_to_the_pointers_side_and_a_negative_size_the_negative_way) {
  const double w = 40, h = 25;
  CHECK(near(corner({10, 10}, {60, 50}, &w, &h, true, 1), 50, 35));
  CHECK(near(corner({10, 10}, {0, 0}, &w, &h, true, 1), -30, -15));  // the pointer down left of the corner
  CHECK(near(corner({10, 10}, {0, 0}, &w, &h, false, 1), 50, 35));   // no pointer: up and right
  CHECK(near(corner({10, 10}, {0, 70}, &w, nullptr, true, 1), -30, 70));  // the height from the pointer
  const double left = -20;
  CHECK(near(corner({100, 0}, {150, 30}, &left, &h, true, 1), 80, 25));
  CHECK(near(corner({0, 0}, {-1, -1}, &w, &h, true, 0.5), -20, -12.5));  // a centre rectangle: half its size from the centre
}

TEST(a_width_off_a_line_keeps_the_pointers_place_along_it) {
  CHECK(near(across({0, -40}, {30, -40}, {0, 0}, 4), 0, -36));
  CHECK(near(across({0, -40}, {30, -40}, {12, -90}, 4), 12, -44));  // the pointer below: below
  CHECK(near(across({0, 0}, {0, 10}, {0, 5}, 3), -3, 5));           // on the line: its left
  CHECK(near(across({0, 0}, {0, 0}, {7, 8}, 3), 7, 8));             // no line: the pointer
}

TEST(an_arcs_radius_puts_its_middle_on_the_pointers_side) {
  P out;
  CHECK(bulge({0, 0}, {40, 0}, {20, 3}, 25, out));  // the shorter arc above: its middle 25 - 15 above the chord
  CHECK(near(out, 20, 10));
  CHECK(bulge({0, 0}, {40, 0}, {20, 38}, 25, out));  // the pointer out by the longer arc: 25 + 15
  CHECK(near(out, 20, 40));
  CHECK(bulge({0, 0}, {40, 0}, {20, -2}, 25, out));  // below
  CHECK(near(out, 20, -10));
  CHECK(bulge({0, 0}, {40, 0}, {20, 1}, 20, out));  // a half circle
  CHECK(near(out, 20, 20));
  CHECK(!bulge({0, 0}, {40, 0}, {20, 1}, 19, out));  // under half the chord
  // The circle through the ends and that point has the radius.
  CHECK(bulge({3, 4}, {-9, 17}, {50, 50}, 12, out));
  const double ax = 3, ay = 4, bx = -9, by = 17, cx = out.u, cy = out.v, d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  const double ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
  const double uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
  CHECK(std::fabs(std::hypot(ax - ux, ay - uy) - 12) < 1e-9);
}

TEST(a_tangent_arc_leaves_the_line_end_along_it_to_the_pointers_side) {
  const P t{1, 0};
  const double r = 10, quarter = pi / 2;
  CHECK(near(tangentArc({0, 0}, t, {5, 3}, &r, &quarter), 10, 10));    // left of the line: counter-clockwise, centre (0, 10)
  CHECK(near(tangentArc({0, 0}, t, {5, -3}, &r, &quarter), 10, -10));  // right of it: clockwise
  CHECK(near(tangentArc({0, 0}, t, {0, 0}, &r, &quarter), 10, 10));    // no pointer: to the left
  // Nothing typed: the circle through the pointer, tangent at the end (centre (0, 5) for a pointer at (5, 5)).
  CHECK(near(tangentArc({0, 0}, t, {5, 5}, nullptr, nullptr), 5, 5));
  CHECK(near(tangentArc({0, 0}, t, {3, 9}, &r, nullptr), 10 * 3 / std::hypot(3.0, -1.0), 10 - 10 / std::hypot(3.0, -1.0)));  // a radius alone: towards the pointer
}

TEST(a_typed_angle_holds_the_line_along_an_axis_or_against_the_line_before) {
  CHECK(angleHold(0, false, 0, false) == Hold::Horizontal);
  CHECK(angleHold(pi, true, 0.3, false) == Hold::Horizontal);
  CHECK(angleHold(-pi / 2, false, 0, false) == Hold::Vertical);
  CHECK(angleHold(3 * pi / 2, true, 1, false) == Hold::Vertical);
  CHECK(angleHold(pi / 6, false, 0, false) == Hold::None);  // nothing to measure it against
  CHECK(angleHold(2 * pi / 3, true, pi / 2, false) == Hold::Perpendicular);  // 120 after a line at 30
  CHECK(angleHold(pi / 4, true, pi / 9, false) == Hold::Angle);
  // Typed from the line before: against it, even where the line comes out along an axis.
  CHECK(angleHold(pi / 2, true, pi / 3, true) == Hold::Angle);
  CHECK(angleHold(0.5, true, 0, true) == Hold::Parallel);
  CHECK(angleHold(0.5, true, -pi, true) == Hold::Parallel);
  CHECK(angleHold(0.5, true, -pi / 2, true) == Hold::Perpendicular);
  CHECK(angleHold(0, false, 0, true) == Hold::Horizontal);  // relative without a line before: from the X axis
  CHECK(std::fabs(between(-pi / 4) - pi / 4) < 1e-12 && std::fabs(between(3 * pi / 2) - pi / 2) < 1e-12);
}

TEST(a_chamfer_by_a_distance_and_an_angle_cuts_the_second_line_where_the_triangle_closes) {
  CHECK(std::fabs(chamferSecond(4, pi / 4, pi / 2) - 4) < 1e-12);                           // 45 degrees on a square corner
  CHECK(std::fabs(chamferSecond(4, pi / 6, pi / 2) - 4 * 0.5 / std::sin(2 * pi / 3)) < 1e-12);  // 30: shorter on the second
  CHECK(std::fabs(chamferSecond(2, pi / 3, pi / 3) - 2) < 1e-12);                           // equilateral
  CHECK(chamferSecond(4, pi / 2, pi / 2) < 0);  // parallel to the second line: it never meets it
  CHECK(chamferSecond(4, 0, pi / 2) < 0);
}

CHECK_MAIN()
