// Least movement (SolveOptions::moves): a constraint added or a dimension changed moves what it names, and what has to
// follow through shared points, while the rest of the sketch stays exactly where it was. The plain least-squares step
// spread the correction over every free point: putting a short line's end onto the bottom edge of a profile lowered the
// top edge, moved an arc and tilted a construction line too.
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "check.hpp"
#include "opad/design/sketch.hpp"

using namespace opad::design;
using CT = SkConstraint::Type;

namespace {

// The profile of the report: a 100 x 60 rectangle held square (its corners shared), a circle centred on its top edge, a
// construction line across it whose ends lie on the sides (nothing holds it level), an arc centred on that line, and a
// short upright line 4 mm long standing free inside, 20 mm above the bottom edge.
struct Profile {
  Sketch sk;
  int a = 0, b = 0, c = 0, d = 0;                 // corners: bottom left, bottom right, top right, top left
  int bottom = 0, right = 0, top = 0, left = 0;   // sides
  int guide = 0, guideL = 0, guideR = 0;          // the construction line and its ends
  int s0 = 0, s1 = 0, stub = 0;                   // the short line: its bottom end, its top end
  int width = 0;                                  // the bottom edge's length dimension (when made)
};

Profile make_profile(bool widthDimension = false) {
  Profile p;
  Sketch& sk = p.sk;
  p.a = sk.add_point(0, 0), p.b = sk.add_point(100, 0), p.c = sk.add_point(100, 60), p.d = sk.add_point(0, 60);
  p.bottom = sk.add_line(p.a, p.b), p.right = sk.add_line(p.b, p.c), p.top = sk.add_line(p.c, p.d), p.left = sk.add_line(p.d, p.a);
  sk.add_constraint(CT::Horizontal, {p.bottom});
  sk.add_constraint(CT::Horizontal, {p.top});
  sk.add_constraint(CT::Vertical, {p.left});
  sk.add_constraint(CT::Vertical, {p.right});
  const int centre = sk.add_point(50, 60);
  sk.add_circle(centre, 6);
  sk.add_constraint(CT::Coincident, {centre, p.top});
  p.guideL = sk.add_point(0, 40), p.guideR = sk.add_point(100, 40);
  p.guide = sk.add_line(p.guideL, p.guideR, true);
  sk.add_constraint(CT::Coincident, {p.guideL, p.left});
  sk.add_constraint(CT::Coincident, {p.guideR, p.right});
  const int ac = sk.add_point(30, 40), as = sk.add_point(42, 40), ae = sk.add_point(30, 52);
  sk.add_arc(ac, as, ae);
  sk.add_constraint(CT::Coincident, {ac, p.guide});
  p.s0 = sk.add_point(80, 20), p.s1 = sk.add_point(80, 24);
  p.stub = sk.add_line(p.s0, p.s1);
  sk.add_constraint(CT::Vertical, {p.stub});
  sk.add_constraint(CT::Distance, {p.stub}, 4);
  if (widthDimension) p.width = sk.add_constraint(CT::Distance, {p.bottom}, 100);
  CHECK(solve(sk).converged);
  return p;
}

std::map<int, std::pair<double, double>> places(const Sketch& sk) {
  std::map<int, std::pair<double, double>> out;
  for (const auto& q : sk.points) out[q.id] = {q.x, q.y};
  return out;
}

double angle(const Sketch& sk, int line) {
  const SkEntity* e = sk.entity(line);
  const SkPoint *p = sk.point(e->p[0]), *q = sk.point(e->p[1]);
  return std::atan2(q->y - p->y, q->x - p->x);
}

// Every point not in `moved` is where it was, to 1e-9.
void unmoved(const std::map<int, std::pair<double, double>>& before, const Sketch& after, const std::set<int>& moved) {
  for (const auto& q : after.points) {
    if (moved.count(q.id)) continue;
    const auto [x, y] = before.at(q.id);
    if (std::fabs(q.x - x) > 1e-9 || std::fabs(q.y - y) > 1e-9)
      throw check::Failure("point " + std::to_string(q.id) + " moved from (" + std::to_string(x) + ", " + std::to_string(y) + ") to (" +
                           std::to_string(q.x) + ", " + std::to_string(q.y) + ")");
  }
}

}  // namespace

// The report: Coincident, first pick the short line's bottom end, second pick the bottom edge. The edge comes up to the
// point (its corners slide up the sides); the point, the top edge, the circle, the arc and the construction line stay.
TEST(point_onto_edge_moves_only_the_edge) {
  Profile p = make_profile();
  const auto before = places(p.sk);
  const double guideAngle = angle(p.sk, p.guide);
  p.sk.add_constraint(CT::Coincident, {p.s0, p.bottom});
  SolveOptions opt;
  opt.moves = {{p.bottom}, {p.s0, p.bottom}};  // the second pick gives way first
  const SolveResult r = solve(p.sk, opt);
  CHECK(r.converged);
  CHECK(std::fabs(p.sk.point(p.a)->y - 20) < 1e-7 && std::fabs(p.sk.point(p.b)->y - 20) < 1e-7);
  CHECK(std::fabs(p.sk.point(p.a)->x) < 1e-9 && std::fabs(p.sk.point(p.b)->x - 100) < 1e-9);
  unmoved(before, p.sk, {p.a, p.b});
  CHECK(std::fabs(angle(p.sk, p.guide) - guideAngle) < 1e-12);
}

// Without least movement the same solve spreads the step (the behaviour the report showed): kept as the reason for it.
TEST(plain_solve_spreads_the_step) {
  Profile p = make_profile();
  const auto before = places(p.sk);
  p.sk.add_constraint(CT::Coincident, {p.s0, p.bottom});
  CHECK(solve(p.sk).converged);
  const auto [x, y] = before.at(p.s0);
  CHECK(std::fabs(p.sk.point(p.s0)->y - y) > 1e-3);  // the point came down to meet the edge part of the way
}

// The other pick order: the edge first, the point second. The point gives way first: alone it cannot (its line is 4 mm
// long and upright), so the line it ends comes down whole; the edge stays.
TEST(second_pick_gives_way_first) {
  Profile p = make_profile();
  const auto before = places(p.sk);
  p.sk.add_constraint(CT::Coincident, {p.s0, p.bottom});
  SolveOptions opt;
  opt.moves = {{p.s0}, {p.bottom, p.s0}};
  CHECK(solve(p.sk, opt).converged);
  CHECK(std::fabs(p.sk.point(p.s0)->y) < 1e-7 && std::fabs(p.sk.point(p.s1)->y - 4) < 1e-7);  // the short line came down whole
  unmoved(before, p.sk, {p.s0, p.s1});
}

// A dimension changed: the bottom edge 100 -> 120. Its corners cannot move alone (the sides are upright), so the sides and
// what lies on them follow; the circle, the arc and the short line stay.
TEST(dimension_change_moves_what_it_must) {
  Profile p = make_profile(true);
  const auto before = places(p.sk);
  p.sk.constraint(p.width)->value = 120;
  SolveOptions opt;
  opt.moves = {{p.bottom}};
  CHECK(solve(p.sk, opt).converged);
  CHECK(std::fabs(p.sk.point(p.b)->x - p.sk.point(p.a)->x - 120) < 1e-7);
  std::set<int> moved{p.a, p.b, p.c, p.d, p.guideL, p.guideR};
  unmoved(before, p.sk, moved);
}

// Nothing can satisfy it: still refused (the sketch is left as it was).
TEST(least_movement_keeps_the_refusal) {
  Profile p = make_profile();
  const auto before = places(p.sk);
  p.sk.add_constraint(CT::Horizontal, {p.stub});  // upright and level at once: impossible
  SolveOptions opt;
  opt.moves = {{p.stub}};
  CHECK(!solve(p.sk, opt).converged);
  unmoved(before, p.sk, {});
}

CHECK_MAIN()
