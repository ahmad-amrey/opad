// The order sketch inferences are taken in and how grid snapping quantises them (app/SketchSnap.hpp, TODO 11 UI-18).
#include "check.hpp"
#include "SketchSnap.hpp"
using namespace sketchsnap;
static const double kPi = std::acos(-1.0);

TEST(grid_step_follows_the_zoom_or_the_set_spacing) {
  CHECK_NEAR(gridStep(105, 0), 10, 1e-12);
  CHECK_NEAR(gridStep(99, 0), 1, 1e-12);
  CHECK_NEAR(gridStep(1050, 0), 100, 1e-12);
  CHECK_NEAR(gridStep(100, 5), 5, 1e-12);
  CHECK_NEAR(gridStep(100, 0.1), 10, 1e-12);  // 1000 lines: automatic again
  CHECK_NEAR(onGrid(-16.3, 10), -20, 1e-12);
  CHECK_NEAR(onGrid(24.9, 10), 20, 1e-12);
}

TEST(a_node_near_the_pointer_beats_guides_and_the_angle_ray) {
  // From (10, 10) the pointer near node (50, 20): the 15 degree ray holds it, the grid node wins.
  Guide ray;
  CHECK(angleRay(10, 10, 50.2, 20.3, 15 * kPi / 180, 0.8, ray));
  const Pick p = resolve(50.2, 20.3, 0.8, 10, {}, &ray, {});
  CHECK(p.by == Pick::By::Node);
  CHECK_NEAR(p.u, 50, 1e-12);
  CHECK_NEAR(p.v, 20, 1e-12);
  // A guide through the node is kept (its constraint and its mark); one beside it is not.
  const std::vector<Guide> guides = {{0, 7, 1, 0, 1}, {50, 0, 0, 1, 2}};
  const Pick q = resolve(50.3, 19.8, 0.8, 10, guides, nullptr, {});
  CHECK(q.by == Pick::By::Node && q.guide == 1);
  CHECK(resolve(50.3, 19.8, 0.8, 10, {{0, 7, 1, 0, 1}}, nullptr, {}).guide == -1);
}

TEST(away_from_the_nodes_a_guide_is_quantised_along_itself) {
  // Horizontal from (3, 0): the pointer at u = 24.4 lands on the grid line u = 20, on the guide.
  const Pick h = resolve(24.4, 0.5, 1, 10, {{3, 0, 1, 0, 1}}, nullptr, {});
  CHECK(h.by == Pick::By::Guide && h.guide == 0);
  CHECK_NEAR(h.u, 20, 1e-12);
  CHECK_NEAR(h.v, 0, 1e-12);
  // A slanted guide (an extension) takes whole steps from its anchor.
  const double c = std::cos(kPi / 6), s = std::sin(kPi / 6);
  const Pick e = resolve(14 * c - 0.3 * s, 14 * s + 0.3 * c, 1, 10, {{0, 0, c, s, 1}}, nullptr, {});
  CHECK(e.by == Pick::By::Guide);
  CHECK_NEAR(e.u, 10 * c, 1e-12);
  CHECK_NEAR(e.v, 10 * s, 1e-12);
  // Without a grid: the foot of the perpendicular.
  const Pick f = resolve(24.4, 0.5, 1, 0, {{3, 0, 1, 0, 1}}, nullptr, {});
  CHECK_NEAR(f.u, 24.4, 1e-12);
  CHECK_NEAR(f.v, 0, 1e-12);
}

TEST(the_angle_ray_takes_whole_steps_polar_snap) {
  Guide ray;
  const double c = std::cos(kPi / 6), s = std::sin(kPi / 6);
  CHECK(angleRay(0, 0, 14 * c, 14 * s + 0.2, 15 * kPi / 180, 1, ray));
  const Pick p = resolve(14 * c, 14 * s + 0.2, 1, 10, {}, &ray, {});
  CHECK(p.by == Pick::By::Ray);
  CHECK_NEAR(p.u, 10 * c, 1e-9);
  CHECK_NEAR(p.v, 10 * s, 1e-9);
  // A vertical ray is an axis: its end lands on a grid line however far the anchor is from one.
  CHECK(angleRay(3.3, 0, 3.4, 27, 15 * kPi / 180, 1, ray));
  CHECK(ray.dx == 0 && axis(ray));
  const Pick v = resolve(3.4, 27, 1, 10, {}, &ray, {});
  CHECK_NEAR(v.u, 3.3, 1e-12);
  CHECK_NEAR(v.v, 30, 1e-12);
}

TEST(angle_rays_no_longer_hold_every_direction_of_a_short_segment) {
  // 8 px capture, 15 degree rays: a 40 px segment was held in every direction; now only near the rays.
  Guide ray;
  int held = 0, total = 0;
  for (double a = 0; a < 15; a += 0.1, ++total)
    held += angleRay(0, 0, 40 * std::cos(a * kPi / 180), 40 * std::sin(a * kPi / 180), 15 * kPi / 180, 8, ray);
  CHECK(held < total * 6 / 10);
  CHECK(angleRay(0, 0, 40 * std::cos(0.5 * kPi / 180), 40 * std::sin(0.5 * kPi / 180), 15 * kPi / 180, 8, ray));
  CHECK(!angleRay(0, 0, 40 * std::cos(7 * kPi / 180), 40 * std::sin(7 * kPi / 180), 15 * kPi / 180, 8, ray));
  CHECK(angleRay(0, 0, 300 * std::cos(14 * kPi / 180), 300 * std::sin(14 * kPi / 180), 15 * kPi / 180, 8, ray));  // long: the full 8 px
  CHECK(!angleRay(0, 0, 5, 0.1, 15 * kPi / 180, 8, ray));  // within the capture of its own start
}

TEST(guides_crossing_beat_the_grid_and_one_points_own_guides_do_not_cross) {
  // Horizontal from (0, 3.3) and vertical from (7.7, 0): their crossing, although node (8, 3) is within reach.
  const std::vector<Guide> guides = {{0, 3.3, 1, 0, 1}, {7.7, 0, 0, 1, 2}};
  const Pick p = resolve(7.75, 3.2, 0.5, 1, guides, nullptr, {});
  CHECK(p.by == Pick::By::Cross && p.guide == 0 && p.other == 1);
  CHECK_NEAR(p.u, 7.7, 1e-12);
  CHECK_NEAR(p.v, 3.3, 1e-12);
  // The same point's horizontal and vertical meet at the point itself: never a crossing.
  const Pick q = resolve(7.75, 0.1, 0.5, 0, {{7.7, 0, 1, 0, 2}, {7.7, 0, 0, 1, 2}}, nullptr, {});
  CHECK(q.by == Pick::By::Guide);
}

TEST(a_curve_comes_after_the_inferences_and_the_grid_last) {
  const std::vector<Curve> line = {{4, 6.2, 24, 6.2}};
  const Pick c = resolve(14, 6, 1, 10, {}, nullptr, line);  // node (10, 10) is out of reach: the foot on the line
  CHECK(c.by == Pick::By::Curve && c.curve == 0);
  CHECK_NEAR(c.u, 14, 1e-12);
  CHECK_NEAR(c.v, 6.2, 1e-12);
  const Pick n = resolve(10.3, 9.8, 1, 10, {}, nullptr, line);  // a node in reach wins (the line is out of reach here)
  CHECK(n.by == Pick::By::Node);
  const Pick g = resolve(14, 6, 1, 10, {}, nullptr, {});  // nothing else: the nearest node anyway
  CHECK(g.by == Pick::By::Grid);
  CHECK_NEAR(g.u, 10, 1e-12);
  CHECK_NEAR(g.v, 10, 1e-12);
  const Pick off = resolve(14, 6, 1, 0, {}, nullptr, {});  // grid snapping off: where the pointer is
  CHECK(off.by == Pick::By::Pointer);
  CHECK_NEAR(off.u, 14, 1e-12);
}

TEST(curves_segments_and_arcs) {
  const Curve arc = {0, 0, 0, 0, 5, 0, kPi / 2};  // the first quadrant of a circle about the origin
  CHECK(onArc(arc, 3, 4) && !onArc(arc, 3, -4) && !onArc(arc, -3, 4));
  CHECK(onArc(arc, 5, -1e-12) && onArc(arc, 0, 5));
  const Curve wrap = {0, 0, 0, 0, 5, 1.5 * kPi, kPi};  // from straight down through the right to straight up
  CHECK(onArc(wrap, 5, 0) && onArc(wrap, 3, -4) && !onArc(wrap, -5, 0));
  double fu, fv, x[2], y[2];
  CHECK_NEAR(foot(arc, 3.3, 4.4, fu, fv), 0.5, 1e-12);
  CHECK_NEAR(fu, 3, 1e-12);
  CHECK_NEAR(fv, 4, 1e-12);
  CHECK_NEAR(foot(arc, 6, -1, fu, fv), std::hypot(1, 1), 1e-12);  // past its end: the distance to the end
  const Curve segment = {0, 0, 10, 0};
  CHECK_NEAR(foot(segment, 12, 1, fu, fv), std::hypot(2, 1), 1e-12);
  CHECK_NEAR(fu, 12, 1e-12);  // on the line through it (that is what the constraint holds)
  CHECK(meet({3, -9, 0, 1}, arc, x, y) == 1);  // x = 3 crosses the quarter once
  CHECK_NEAR(x[0], 3, 1e-12);
  CHECK_NEAR(y[0], 4, 1e-12);
  CHECK(meet({3, -9, 0, 1}, {0, 0, 0, 0, 5, 0, 2 * kPi}, x, y) == 2);
  CHECK(meet({0, 1, 1, 0}, segment, x, y) == 0);  // parallel
  CHECK(meet({12, 0, 0, 1}, segment, x, y) == 0);  // past its end
  CHECK(meet({4, 7, 0, 1}, segment, x, y) == 1);
  CHECK_NEAR(x[0], 4, 1e-12);
  CHECK_NEAR(y[0], 0, 1e-12);
}

TEST(a_guide_crossing_a_curve_beats_the_grid) {
  // Horizontal from (0, 0) meets the line u = 23.3: exactly there, on both, although node (20, 0) and the guide's
  // quantised point would take it.
  const std::vector<Guide> h = {{0, 0, 1, 0, 1}};
  const Pick p = resolve(23.5, 0.4, 1, 10, h, nullptr, {{23.3, -10, 23.3, 10}});
  CHECK(p.by == Pick::By::Cross && p.guide == 0 && p.other == -1 && p.curve == 0 && !p.ray);
  CHECK_NEAR(p.u, 23.3, 1e-12);
  CHECK_NEAR(p.v, 0, 1e-12);
  // Vertical through u = 3 meets a circle of radius 5 about the origin at (3, 4).
  const Pick c = resolve(3.2, 3.9, 0.5, 1, {{3, -9, 0, 1, 2}}, nullptr, {{0, 0, 0, 0, 5, 0, 2 * kPi}});
  CHECK(c.by == Pick::By::Cross && c.curve == 0);
  CHECK_NEAR(c.u, 3, 1e-12);
  CHECK_NEAR(c.v, 4, 1e-12);
  // The quarter arc does not reach (3, -4): the guide alone, quantised.
  const Pick q = resolve(3.1, -3.6, 0.3, 1, {{3, -9, 0, 1, 2}}, nullptr, {{0, 0, 0, 0, 5, 0, kPi / 2}});
  CHECK(q.by == Pick::By::Guide);
  CHECK_NEAR(q.u, 3, 1e-12);
  CHECK_NEAR(q.v, -4, 1e-12);
}

TEST(the_angle_ray_crosses_guides_and_curves_and_names_the_nodes_on_it) {
  // 45 degrees from (0, 0) meets the vertical through a tracked point (20.5, 7).
  Guide ray{0, 0, 1, 0, 1};
  CHECK(angleRay(0, 0, 20.6, 20.4, 15 * kPi / 180, 1, ray) && ray.anchor == 1);
  const Pick p = resolve(20.6, 20.4, 1, 10, {{20.5, 7, 0, 1, 2}}, &ray, {});
  CHECK(p.by == Pick::By::Cross && p.guide == 0 && p.other == -1 && p.ray && p.curve == -1);
  CHECK_NEAR(p.u, 20.5, 1e-9);
  CHECK_NEAR(p.v, 20.5, 1e-9);
  // ... and a line across it at u = 20.5: the ray alone with a curve.
  const Pick c = resolve(20.6, 20.4, 1, 10, {}, &ray, {{20.5, 0, 20.5, 40}});
  CHECK(c.by == Pick::By::Cross && c.guide == -1 && c.ray && c.curve == 0);
  CHECK_NEAR(c.v, 20.5, 1e-9);
  // The point's own horizontal never crosses its ray (they meet at the point).
  const Pick own = resolve(20.6, 20.4, 1, 0, {{0, 0, 1, 0, 1}, {0, 0, 0, 1, 1}}, &ray, {});
  CHECK(own.by == Pick::By::Ray);
  // A node on the ray is the node, and the ray stays named.
  CHECK(angleRay(0, 0, 30.3, 29.8, 15 * kPi / 180, 1, ray));
  const Pick n = resolve(30.3, 29.8, 1, 10, {}, &ray, {});
  CHECK(n.by == Pick::By::Node && n.ray && n.guide == -1);
  CHECK_NEAR(n.u, 30, 1e-12);
  CHECK_NEAR(n.v, 30, 1e-12);
  CHECK(angleRay(0, 0, 39.8, 10.3, 15 * kPi / 180, 1, ray));  // node (40, 10) is off the 15 degree ray that holds the pointer
  const Pick o = resolve(39.8, 10.3, 1, 10, {}, &ray, {});
  CHECK(o.by == Pick::By::Node && !o.ray);
}

CHECK_MAIN()
