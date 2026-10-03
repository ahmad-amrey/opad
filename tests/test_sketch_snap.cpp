// The order sketch inferences are taken in and how grid snapping quantises them (app/SketchSnap.hpp, TODO 11 UI-18).
#include "check.hpp"
#include "SketchSnap.hpp"
using namespace sketchsnap;
static const double kPi = std::acos(-1.0);

TEST(grid_step_follows_the_zoom_or_the_set_spacing) {
  // 1, 2 or 5 times a power of ten, at least 24 px: 24 to 60 px on the screen.
  CHECK_NEAR(gridStep(0.1, 0), 5, 1e-12);     // 2.4 mm wanted: 5 mm (50 px)
  CHECK_NEAR(gridStep(0.05, 0), 2, 1e-12);    // 1.2 mm: 2 mm (40 px)
  CHECK_NEAR(gridStep(1.0 / 24, 0), 1, 1e-12);  // exactly 24 px: 1 mm
  CHECK_NEAR(gridStep(0.042, 0), 2, 1e-12);   // a hair over: 2 mm
  CHECK_NEAR(gridStep(0.4, 0), 10, 1e-12);    // 9.6 mm: 10 mm (25 px)
  CHECK_NEAR(gridStep(4.5, 0), 200, 1e-12);   // 108 mm: 200 mm (44 px)
  CHECK(gridStep(0.0004, 0) == 0.01);         // 0.0096 mm: 0.01 mm, the decimal itself
  CHECK(gridStep(0.0007, 0) == 0.02);
  for (double pixel = 1e-4; pixel < 1e3; pixel *= 1.37) {  // every zoom: 24 to 60 px
    const double px = gridStep(pixel, 0) / pixel;
    CHECK(px >= 24 * (1 - 1e-9) && px < 60);
  }
  CHECK_NEAR(gridStep(0.1, 5), 5, 1e-12);     // a set 5 mm: 50 px, itself
  CHECK_NEAR(gridStep(5.0 / 60, 5), 5, 1e-12);  // 60 px: still itself
  CHECK_NEAR(gridStep(0.05, 5), 2.5, 1e-12);  // 100 px: halved (50 px)
  CHECK_NEAR(gridStep(0.05, 5, 1, 195), 5, 1e-12);  // in a view 780 px high (up to 195 px): itself
  CHECK_NEAR(gridStep(0.01, 5, 1, 195), 0.25, 1e-12);  // zoomed far in (500 px): a twentieth (25 px), a node always near
  CHECK_NEAR(gridStep(0.01, 5), 0.25, 1e-12);
  CHECK_NEAR(gridStep(0.5, 5), 25, 1e-12);    // zoomed out: 5 x 5 mm (50 px)
  CHECK_NEAR(gridStep(0.3, 2.5), 12.5, 1e-12);  // 7.2 mm wanted: 2.5 x 5 (2.5 x 2 is too fine)
  for (double pixel = 1e-4; pixel < 1e3; pixel *= 1.37) {  // a set 3 mm, every zoom: 24 to 60 px
    const double px = gridStep(pixel, 3) / pixel;
    CHECK(px >= 24 * (1 - 1e-9) && px <= 60 * (1 + 1e-9));
  }
  // Inches shown: tenths, halves and whole inches, never 0.19685 in.
  CHECK_NEAR(gridStep(0.1, 0, 25.4), 2.54, 1e-12);  // 2.4 mm wanted: 0.1 in (25.4 px)
  CHECK_NEAR(gridStep(0.5, 0, 25.4), 12.7, 1e-12);  // 12 mm: 0.5 in (25.4 px)
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

TEST(with_a_grid_a_guide_never_takes_the_point_off_the_nodes) {
  // Horizontal from (3, 0): the pointer at u = 24.4 lands on node (20, 0), which the guide runs through (kept).
  const Pick h = resolve(24.4, 0.5, 1, 10, {{3, 0, 1, 0, 1}}, nullptr, {});
  CHECK(h.by == Pick::By::Node && h.guide == 0);
  CHECK_NEAR(h.u, 20, 1e-12);
  CHECK_NEAR(h.v, 0, 1e-12);
  // On a slanted guide (an extension) away from the nodes: the nearest node, the guide not through it.
  const double c = std::cos(kPi / 6), s = std::sin(kPi / 6);
  const Pick e = resolve(14 * c - 0.3 * s, 14 * s + 0.3 * c, 1, 10, {{0, 0, c, s, 1}}, nullptr, {});
  CHECK(e.by == Pick::By::Node && e.guide == -1);
  CHECK_NEAR(e.u, 10, 1e-12);
  CHECK_NEAR(e.v, 10, 1e-12);
  // A guide 4 units from the pointer still names the node it runs through (the point is level with its anchor).
  const Pick level = resolve(31, 4, 1, 10, {{3, 0, 1, 0, 1}}, nullptr, {});
  CHECK(level.by == Pick::By::Node && level.guide == 0);
  CHECK_NEAR(level.u, 30, 1e-12);
  CHECK_NEAR(level.v, 0, 1e-12);
  // Without a grid: the foot of the perpendicular.
  const Pick f = resolve(24.4, 0.5, 1, 0, {{3, 0, 1, 0, 1}}, nullptr, {});
  CHECK(f.by == Pick::By::Guide);
  CHECK_NEAR(f.u, 24.4, 1e-12);
  CHECK_NEAR(f.v, 0, 1e-12);
}

TEST(with_a_grid_the_angle_ray_only_names_a_node_on_it) {
  // 75 degrees from (0, 0), the pointer on the ray between nodes: the nearest node, off the ray (a readout only).
  Guide ray;
  const double c = std::cos(5 * kPi / 12), s = std::sin(5 * kPi / 12);
  CHECK(angleRay(0, 0, 33 * c, 33 * s, 15 * kPi / 180, 1, ray));
  const Pick p = resolve(33 * c, 33 * s, 1, 10, {}, &ray, {});
  CHECK(p.by == Pick::By::Node && !p.ray);
  CHECK_NEAR(p.u, 10, 1e-12);
  CHECK_NEAR(p.v, 30, 1e-12);
  // A vertical ray from off the grid: the nearest node, not the ray's own line.
  CHECK(angleRay(3.3, 0, 3.4, 27, 15 * kPi / 180, 1, ray));
  CHECK(ray.dx == 0 && axis(ray));
  const Pick v = resolve(3.4, 27, 1, 10, {}, &ray, {});
  CHECK(v.by == Pick::By::Node && !v.ray);
  CHECK_NEAR(v.u, 0, 1e-12);
  CHECK_NEAR(v.v, 30, 1e-12);
  // Grid snapping off: the foot on the ray.
  CHECK(angleRay(0, 0, 33 * c, 33 * s + 0.2, 15 * kPi / 180, 1, ray));
  const Pick o = resolve(33 * c, 33 * s + 0.2, 1, 0, {}, &ray, {});
  CHECK(o.by == Pick::By::Ray);
  CHECK_NEAR(o.u * s - o.v * c, 0, 1e-9);
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

TEST(a_curve_never_beats_the_grid_and_holds_a_node_on_it) {
  const std::vector<Curve> line = {{4, 6.2, 24, 6.2}};
  const Pick c = resolve(14, 6, 1, 10, {}, nullptr, line);  // on the line, 4 units from node (10, 10): the node
  CHECK(c.by == Pick::By::Node && c.curve == -1);
  CHECK_NEAR(c.u, 10, 1e-12);
  CHECK_NEAR(c.v, 10, 1e-12);
  const Pick g = resolve(14, 6, 1, 10, {}, nullptr, {});  // nothing else: the nearest node
  CHECK(g.by == Pick::By::Node);
  CHECK_NEAR(g.u, 10, 1e-12);
  CHECK_NEAR(g.v, 10, 1e-12);
  const Pick on = resolve(13, 9.6, 1, 10, {}, nullptr, {{4, 10, 24, 10}});  // a line through the node: the point lies on it
  CHECK(on.by == Pick::By::Node && on.curve == 0);
  const Pick circle = resolve(9.4, 0.8, 1, 10, {}, nullptr, {{0, 0, 0, 0, 10, 0, 2 * kPi}});  // a circle through node (10, 0)
  CHECK(circle.by == Pick::By::Node && circle.curve == 0);
  const Pick free = resolve(14, 6, 1, 0, {}, nullptr, line);  // grid snapping off: the foot on the line
  CHECK(free.by == Pick::By::Curve && free.curve == 0);
  CHECK_NEAR(free.u, 14, 1e-12);
  CHECK_NEAR(free.v, 6.2, 1e-12);
  const Pick off = resolve(14, 6, 1, 0, {}, nullptr, {});  // grid snapping off, nothing near: where the pointer is
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
  // The quarter arc does not reach (3, -4): the node there, the guide through it.
  const Pick q = resolve(3.1, -3.6, 0.3, 1, {{3, -9, 0, 1, 2}}, nullptr, {{0, 0, 0, 0, 5, 0, kPi / 2}});
  CHECK(q.by == Pick::By::Node && q.guide == 0);
  CHECK_NEAR(q.u, 3, 1e-12);
  CHECK_NEAR(q.v, -4, 1e-12);
}

TEST(the_angle_ray_crosses_guides_and_curves_and_names_the_nodes_on_it) {
  // 45 degrees from (0, 0) meets the vertical through a tracked point (20.5, 7) (grid snapping off).
  Guide ray{0, 0, 1, 0, 1};
  CHECK(angleRay(0, 0, 20.6, 20.4, 15 * kPi / 180, 1, ray) && ray.anchor == 1);
  const Pick p = resolve(20.6, 20.4, 1, 0, {{20.5, 7, 0, 1, 2}}, &ray, {});
  CHECK(p.by == Pick::By::Cross && p.guide == 0 && p.other == -1 && p.ray && p.curve == -1);
  CHECK_NEAR(p.u, 20.5, 1e-9);
  CHECK_NEAR(p.v, 20.5, 1e-9);
  // ... and a line across it at u = 20.5: the ray alone with a curve.
  const Pick c = resolve(20.6, 20.4, 1, 0, {}, &ray, {{20.5, 0, 20.5, 40}});
  CHECK(c.by == Pick::By::Cross && c.guide == -1 && c.ray && c.curve == 0);
  CHECK_NEAR(c.v, 20.5, 1e-9);
  // With a grid the ray crosses nothing: node (20, 20), which it runs through.
  const Pick g = resolve(20.6, 20.4, 1, 10, {{20.5, 7, 0, 1, 2}}, &ray, {{20.5, 0, 20.5, 40}});
  CHECK(g.by == Pick::By::Node && g.ray && g.guide == -1);
  CHECK_NEAR(g.u, 20, 1e-12);
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

TEST(tracking_points_are_at_most_six_oldest_out_and_resting_again_lets_go) {
  std::vector<int> tracked;
  for (int id = 1; id <= 6; ++id) CHECK(track(tracked, id));
  CHECK((tracked == std::vector<int>{1, 2, 3, 4, 5, 6}));
  CHECK(track(tracked, 7));  // the seventh: the oldest goes
  CHECK((tracked == std::vector<int>{2, 3, 4, 5, 6, 7}));
  CHECK(!track(tracked, 4));  // resting on a tracked one lets it go
  CHECK((tracked == std::vector<int>{2, 3, 5, 6, 7}));
  CHECK(track(tracked, 4) && tracked.back() == 4);  // and again acquires it, as the newest
  CHECK(track(tracked, 9, 2) && (tracked == std::vector<int>{4, 9}));
}

TEST(two_tracked_points_cross_at_a_x_b_y) {
  // A = (3.3, 1.7) and B = (12.9, 8.1): A's vertical and B's horizontal meet at (3.3, 8.1), ahead of the grid and of each
  // guide alone; B's vertical and A's horizontal at (12.9, 1.7).
  const std::vector<Guide> guides = {{3.3, 1.7, 1, 0, 1}, {3.3, 1.7, 0, 1, 1}, {12.9, 8.1, 1, 0, 2}, {12.9, 8.1, 0, 1, 2}};
  const Pick p = resolve(3.5, 8.0, 0.5, 1, guides, nullptr, {});
  CHECK(p.by == Pick::By::Cross && guides[size_t(p.guide)].anchor != guides[size_t(p.other)].anchor);
  CHECK_NEAR(p.u, 3.3, 1e-12);
  CHECK_NEAR(p.v, 8.1, 1e-12);
  const Pick q = resolve(12.7, 1.9, 0.5, 1, guides, nullptr, {});
  CHECK(q.by == Pick::By::Cross);
  CHECK_NEAR(q.u, 12.9, 1e-12);
  CHECK_NEAR(q.v, 1.7, 1e-12);
}

TEST(a_locked_line_stops_where_other_guides_and_curves_cross_it) {
  // Locked on A's vertical (A = (3.3, 1.7)); B = (12.9, 8.1) is tracked. The pointer far to the side of the line, a
  // little below B's height: exactly (A.x, B.y), the crossing being B's horizontal.
  const Guide lock{3.3, 1.7, 0, 1, 1};
  const std::vector<Guide> guides = {{3.3, 1.7, 1, 0, 1}, {12.9, 8.1, 1, 0, 2}, {12.9, 8.1, 0, 1, 2}};
  const Pick p = along(lock, 9.0, 7.8, 0.5, 0, guides, {});
  CHECK(p.by == Pick::By::Cross && p.guide == -1 && p.other == 1 && p.curve == -1);
  CHECK_NEAR(p.u, 3.3, 1e-12);
  CHECK_NEAR(p.v, 8.1, 1e-12);
  // Further from B's height than the capture: the foot on the line (no grid), or whole grid lines (an axis lock).
  const Pick f = along(lock, 9.0, 6.4, 0.5, 0, guides, {});
  CHECK(f.by == Pick::By::Guide && f.other == -1);
  CHECK_NEAR(f.u, 3.3, 1e-12);
  CHECK_NEAR(f.v, 6.4, 1e-12);
  const Pick g = along(lock, 9.0, 6.4, 0.5, 1, guides, {});
  CHECK(g.by == Pick::By::Guide);
  CHECK_NEAR(g.u, 3.3, 1e-12);
  CHECK_NEAR(g.v, 6, 1e-12);
  // The lock's own point's horizontal crosses it at A: never a stop. A guide parallel to it never crosses.
  CHECK(along(lock, 9.0, 1.8, 0.5, 0, {{3.3, 1.7, 1, 0, 1}, {7, 0, 0, 1, 3}}, {}).by == Pick::By::Guide);
  // A circle of radius 5 about (0, 0) crosses x = 3.3 at y = +-sqrt(25 - 3.3^2); a segment from (0, 3) to (9, 3) at y = 3.
  const double y = std::sqrt(25 - 3.3 * 3.3);
  const Pick c = along(lock, -20, y + 0.3, 0.5, 1, {}, {{0, 0, 0, 0, 5, 0, 2 * kPi}});
  CHECK(c.by == Pick::By::Cross && c.curve == 0 && c.other == -1);
  CHECK_NEAR(c.u, 3.3, 1e-12);
  CHECK_NEAR(c.v, y, 1e-12);
  const Pick s = along(lock, 50, 2.8, 0.5, 0, {}, {{0, 3, 9, 3}});
  CHECK(s.by == Pick::By::Cross && s.curve == 0);
  CHECK_NEAR(s.v, 3, 1e-12);
  CHECK(along(lock, 50, 2.8, 0.5, 0, {}, {{4, 3, 9, 3}}).by == Pick::By::Guide);  // the segment ends before the line
  // A slanted lock (30 degrees from the origin) meets B's vertical at u = 12.9.
  const double c30 = std::cos(kPi / 6), s30 = std::sin(kPi / 6);
  const Pick o = along({0, 0, c30, s30, 4}, 12.8, 7.9, 0.5, 0, guides, {});
  CHECK(o.by == Pick::By::Cross && o.other == 2);
  CHECK_NEAR(o.u, 12.9, 1e-12);
  CHECK_NEAR(o.v, 12.9 * s30 / c30, 1e-12);
}

TEST(a_slanted_lock_stops_where_it_crosses_a_grid_line) {
  // Locked from (-20, -20) towards (0, 10), a 10 grid: the foot of (5.5, 18) is nearest the line's crossing with y = 20
  // (at x = -20 + 40 * 2 / 3), not with x = 0 or x = 10, nor whole steps from (-20, -20).
  const double len = std::hypot(20, 30);
  const Guide lock{-20, -20, 20 / len, 30 / len, 1};
  const Pick p = along(lock, 5.5, 18, 0.5, 10, {}, {});
  CHECK(p.by == Pick::By::Guide);
  CHECK_NEAR(p.u, -20 + 40.0 * 2 / 3, 1e-12);
  CHECK_NEAR(p.v, 20, 1e-12);
  // Further along, x = 10 comes first (at y = 25); a node the line runs through is both.
  const Pick q = along(lock, 12, 25.5, 0.5, 10, {}, {});
  CHECK_NEAR(q.u, 10, 1e-12);
  CHECK_NEAR(q.v, 25, 1e-12);
  const Pick n = along(lock, 1, 10.5, 0.5, 10, {}, {});
  CHECK_NEAR(n.u, 0, 1e-12);
  CHECK_NEAR(n.v, 10, 1e-12);
  // An axis lock: the grid lines across it, as project; a stop within the capture still comes first.
  double pu, pv;
  crossGrid({3.3, 1.7, 0, 1, 1}, 9, 6.4, 1, pu, pv);
  CHECK(pu == 3.3);
  CHECK_NEAR(pv, 6, 1e-12);
  crossGrid({3.3, 1.7, 1, 0, 1}, 8.6, 9, 1, pu, pv);
  CHECK(pv == 1.7);
  CHECK_NEAR(pu, 9, 1e-12);
  CHECK(along(lock, 5.5, 18, 0.5, 10, {{0, 18.2, 1, 0, 2}}, {}).by == Pick::By::Cross);
}

TEST(the_stops_along_a_locked_line_are_every_crossing_in_reach_nearest_first) {
  // Locked on A's vertical (A = (3.3, 1.7)), the pointer's foot at (3.3, 0): B's horizontal at y = 8.1, D's at y = -40,
  // A's own horizontal never, a segment ending on the line at (3.3, 8.1) (one stop with B's horizontal there) and a
  // circle about (0, 20) of radius 5 at y = 20 -+ sqrt(25 - 3.3^2).
  const Guide lock{3.3, 1.7, 0, 1, 1};
  const std::vector<Guide> guides = {{3.3, 1.7, 1, 0, 1}, {12.9, 8.1, 1, 0, 2}, {-7, -40, 1, 0, 3}};
  const std::vector<Curve> curves = {{12.9, 8.1, 3.3, 8.1}, {0, 20, 0, 0, 5, 0, 2 * kPi}};
  const double h = std::sqrt(25 - 3.3 * 3.3);
  const auto all = stops(lock, 9, 0, 50, guides, curves);
  CHECK(all.size() == 4);
  CHECK(all[0].by == Pick::By::Cross && all[0].other == 1 && all[0].curve == 0);
  CHECK_NEAR(all[0].u, 3.3, 1e-12);
  CHECK_NEAR(all[0].v, 8.1, 1e-12);
  CHECK(all[1].curve == 1 && all[1].other == -1);
  CHECK_NEAR(all[1].v, 20 - h, 1e-12);
  CHECK(all[2].curve == 1);
  CHECK_NEAR(all[2].v, 20 + h, 1e-12);
  CHECK(all[3].other == 2 && all[3].curve == -1);
  CHECK_NEAR(all[3].v, -40, 1e-12);
  // Only those within reach; counted from elsewhere, in another order.
  CHECK(stops(lock, 9, 0, 30, guides, curves).size() == 3);
  const auto far = stops(lock, -5, -35, 50, guides, curves);
  CHECK(far.size() == 2 && far[0].other == 2 && far[1].other == 1);
  // The nearest within the capture is what along() stops on, the same pick.
  const Pick p = along(lock, 9, 8.3, 0.5, 0, guides, curves);
  const auto held = stops(lock, 9, 8.3, 50, guides, curves);
  CHECK(p.by == Pick::By::Cross && p.other == held[0].other && p.curve == held[0].curve && p.u == held[0].u && p.v == held[0].v);
}

TEST(perpendicular_feet_and_tangent_points_from_the_last_point) {
  // UI-23. A segment: the foot of the perpendicular, only on the segment, none from a point on its line.
  const Curve seg{0, 0, 20, 0};
  double x[2], y[2];
  CHECK(normals(seg, 7, 9, x, y) == 1);
  CHECK_NEAR(x[0], 7, 1e-12);
  CHECK_NEAR(y[0], 0, 1e-12);
  CHECK(normals(seg, 25, 9, x, y) == 0);
  CHECK(normals(seg, 5, 0, x, y) == 0);
  // A circle: the near and the far point on the line through its centre; from the centre none.
  const Curve circle{10, 10, 0, 0, 5, 0, 2 * kPi};
  CHECK(normals(circle, 10, 30, x, y) == 2);
  CHECK_NEAR(x[0], 10, 1e-12);
  CHECK_NEAR(y[0], 15, 1e-12);
  CHECK_NEAR(y[1], 5, 1e-12);
  CHECK(normals(circle, 10, 10, x, y) == 0);
  // An arc (the upper half): only the point on it.
  const Curve upper{10, 10, 0, 0, 5, 0, kPi};
  CHECK(normals(upper, 10, 30, x, y) == 1);
  CHECK_NEAR(y[0], 15, 1e-12);
  // Tangent points: the radius to each is square to the line from the point; none from inside or on the circle.
  CHECK(tangents(circle, 10, 30, x, y) == 2);
  for (int i = 0; i < 2; ++i) {
    CHECK_NEAR(std::hypot(x[i] - 10, y[i] - 10), 5, 1e-12);
    CHECK_NEAR((x[i] - 10) * (x[i] - 10) + (y[i] - 10) * (y[i] - 30), 0, 1e-9);
  }
  CHECK(tangents(circle, 12, 11, x, y) == 0);
  CHECK(tangents(circle, 15, 10, x, y) == 0);
  CHECK(tangents(seg, 5, 5, x, y) == 0);
  CHECK(tangents(upper, 10, 30, x, y) == 2);  // both on the upper half
  CHECK(tangents(Curve{10, 10, 0, 0, 5, kPi, kPi}, 10, 30, x, y) == 0);  // the lower half: neither
}

TEST(apparent_intersections_are_past_the_ends) {
  // Two segments whose lines cross past the end of one: there; on both: no (a real intersection); parallel: no.
  double u, v;
  CHECK(apparent(Curve{0, 0, 10, 0}, Curve{20, 5, 20, 15}, u, v));
  CHECK_NEAR(u, 20, 1e-12);
  CHECK_NEAR(v, 0, 1e-12);
  CHECK(!apparent(Curve{0, 0, 10, 0}, Curve{5, -5, 5, 5}, u, v));
  CHECK(!apparent(Curve{0, 0, 10, 0}, Curve{0, 3, 10, 3.1}, u, v));
  // A segment's line past its end meets a circle: both crossings beyond it, none on the segment itself.
  double x[2], y[2];
  const Curve circle{30, 0, 0, 0, 5, 0, 2 * kPi};
  CHECK(apparent(Curve{0, 0, 10, 0}, circle, x, y) == 2);
  CHECK_NEAR(std::min(x[0], x[1]), 25, 1e-12);
  CHECK_NEAR(std::max(x[0], x[1]), 35, 1e-12);
  CHECK(apparent(Curve{0, 0, 30, 0}, circle, x, y) == 1);  // the near crossing is on the segment
  CHECK_NEAR(x[0], 35, 1e-12);
  CHECK(apparent(Curve{0, 10, 10, 10}, circle, x, y) == 0);
}

CHECK_MAIN()
