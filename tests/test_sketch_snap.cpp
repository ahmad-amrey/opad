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
  const Pick p = resolve(50.2, 20.3, 0.8, 10, {}, &ray, nullptr);
  CHECK(p.by == Pick::By::Node);
  CHECK_NEAR(p.u, 50, 1e-12);
  CHECK_NEAR(p.v, 20, 1e-12);
  // A guide through the node is kept (its constraint and its mark); one beside it is not.
  const std::vector<Guide> guides = {{0, 7, 1, 0, 1}, {50, 0, 0, 1, 2}};
  const Pick q = resolve(50.3, 19.8, 0.8, 10, guides, nullptr, nullptr);
  CHECK(q.by == Pick::By::Node && q.guide == 1);
  CHECK(resolve(50.3, 19.8, 0.8, 10, {{0, 7, 1, 0, 1}}, nullptr, nullptr).guide == -1);
}

TEST(away_from_the_nodes_a_guide_is_quantised_along_itself) {
  // Horizontal from (3, 0): the pointer at u = 24.4 lands on the grid line u = 20, on the guide.
  const Pick h = resolve(24.4, 0.5, 1, 10, {{3, 0, 1, 0, 1}}, nullptr, nullptr);
  CHECK(h.by == Pick::By::Guide && h.guide == 0);
  CHECK_NEAR(h.u, 20, 1e-12);
  CHECK_NEAR(h.v, 0, 1e-12);
  // A slanted guide (an extension) takes whole steps from its anchor.
  const double c = std::cos(kPi / 6), s = std::sin(kPi / 6);
  const Pick e = resolve(14 * c - 0.3 * s, 14 * s + 0.3 * c, 1, 10, {{0, 0, c, s, 1}}, nullptr, nullptr);
  CHECK(e.by == Pick::By::Guide);
  CHECK_NEAR(e.u, 10 * c, 1e-12);
  CHECK_NEAR(e.v, 10 * s, 1e-12);
  // Without a grid: the foot of the perpendicular.
  const Pick f = resolve(24.4, 0.5, 1, 0, {{3, 0, 1, 0, 1}}, nullptr, nullptr);
  CHECK_NEAR(f.u, 24.4, 1e-12);
  CHECK_NEAR(f.v, 0, 1e-12);
}

TEST(the_angle_ray_takes_whole_steps_polar_snap) {
  Guide ray;
  const double c = std::cos(kPi / 6), s = std::sin(kPi / 6);
  CHECK(angleRay(0, 0, 14 * c, 14 * s + 0.2, 15 * kPi / 180, 1, ray));
  const Pick p = resolve(14 * c, 14 * s + 0.2, 1, 10, {}, &ray, nullptr);
  CHECK(p.by == Pick::By::Ray);
  CHECK_NEAR(p.u, 10 * c, 1e-9);
  CHECK_NEAR(p.v, 10 * s, 1e-9);
  // A vertical ray is an axis: its end lands on a grid line however far the anchor is from one.
  CHECK(angleRay(3.3, 0, 3.4, 27, 15 * kPi / 180, 1, ray));
  CHECK(ray.dx == 0 && axis(ray));
  const Pick v = resolve(3.4, 27, 1, 10, {}, &ray, nullptr);
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
  const Pick p = resolve(7.75, 3.2, 0.5, 1, guides, nullptr, nullptr);
  CHECK(p.by == Pick::By::Cross && p.guide == 0 && p.other == 1);
  CHECK_NEAR(p.u, 7.7, 1e-12);
  CHECK_NEAR(p.v, 3.3, 1e-12);
  // The same point's horizontal and vertical meet at the point itself: never a crossing.
  const Pick q = resolve(7.75, 0.1, 0.5, 0, {{7.7, 0, 1, 0, 2}, {7.7, 0, 0, 1, 2}}, nullptr, nullptr);
  CHECK(q.by == Pick::By::Guide);
}

TEST(a_curve_comes_after_the_inferences_and_the_grid_last) {
  const double foot[2] = {14.1, 6.2};
  const Pick c = resolve(14, 6, 1, 10, {}, nullptr, foot);  // node (10, 10) is out of reach
  CHECK(c.by == Pick::By::Curve);
  CHECK_NEAR(c.u, 14.1, 1e-12);
  const Pick n = resolve(10.3, 9.8, 1, 10, {}, nullptr, foot);  // a node in reach wins
  CHECK(n.by == Pick::By::Node);
  const Pick g = resolve(14, 6, 1, 10, {}, nullptr, nullptr);  // nothing else: the nearest node anyway
  CHECK(g.by == Pick::By::Grid);
  CHECK_NEAR(g.u, 10, 1e-12);
  CHECK_NEAR(g.v, 10, 1e-12);
  const Pick off = resolve(14, 6, 1, 0, {}, nullptr, nullptr);  // grid snapping off: where the pointer is
  CHECK(off.by == Pick::By::Pointer);
  CHECK_NEAR(off.u, 14, 1e-12);
}

CHECK_MAIN()
