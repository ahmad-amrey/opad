// Rigid groups (SkConstraint::Type::Rigid) and the sketch's Align (align_entities): curves that keep their shape and move
// and turn as one piece.
#include <cmath>
#include <vector>

#include "check.hpp"
#include "opad/design/sketch_modify.hpp"

using namespace opad::design;
using CT = SkConstraint::Type;

namespace {

// The shape of the question that started this: a leaning open line, a small arc over its top into a big arc, and a line
// down from it. No constraints of its own.
struct Hook {
  Sketch sk;
  int foot = 0, top = 0, small = 0, join = 0, big = 0, shoulder = 0, end = 0;  // points
  int left = 0, round = 0, arc = 0, right = 0;                                 // curves
  std::vector<int> curves() const { return {left, round, arc, right}; }
  std::vector<int> points() const { return {foot, top, small, join, big, shoulder, end}; }
};

Hook make_hook() {
  Hook h;
  auto& sk = h.sk;
  h.foot = sk.add_point(4, 0);
  h.top = sk.add_point(0.2, 17.5);
  // The small arc: centre right of the top, from the top round to the join (counter-clockwise runs from start to end,
  // so it goes from the join back over to the top).
  const double scx = 2.5, scy = 18.4, sr = std::hypot(0.2 - scx, 17.5 - scy);
  h.small = sk.add_point(scx, scy);
  h.join = sk.add_point(scx + sr * std::cos(1.4), scy + sr * std::sin(1.4));
  h.round = sk.add_arc(h.small, h.join, h.top);
  // The big arc: centre below, from the shoulder up to the join.
  const double bcx = h.sk.point(h.join)->x, bcy = 2.0, br = h.sk.point(h.join)->y - bcy;
  h.big = sk.add_point(bcx, bcy);
  h.shoulder = sk.add_point(bcx + br * std::cos(0.35), bcy + br * std::sin(0.35));
  // Make the join the big arc's top: same x as its centre, so both arcs meet there.
  h.arc = sk.add_arc(h.big, h.shoulder, h.join);
  h.end = sk.add_point(sk.point(h.shoulder)->x + 0.3, -1);
  h.left = sk.add_line(h.foot, h.top);
  h.right = sk.add_line(h.shoulder, h.end);
  return h;
}

// Largest change of any distance between two of the points: 0 = the same shape (moved and turned at most).
double shape_change(const Sketch& before, const Sketch& after, const std::vector<int>& ids) {
  double worst = 0;
  for (size_t i = 0; i < ids.size(); ++i)
    for (size_t j = i + 1; j < ids.size(); ++j) {
      const SkPoint *a = before.point(ids[i]), *b = before.point(ids[j]), *c = after.point(ids[i]), *d = after.point(ids[j]);
      worst = std::max(worst, std::fabs(std::hypot(a->x - b->x, a->y - b->y) - std::hypot(c->x - d->x, c->y - d->y)));
    }
  return worst;
}

double cross_z(const Sketch& sk, int o, int a, int b) {  // handedness of three points: a mirror image flips its sign
  const SkPoint *p = sk.point(o), *q = sk.point(a), *r = sk.point(b);
  return (q->x - p->x) * (r->y - p->y) - (q->y - p->y) * (r->x - p->x);
}

}  // namespace

TEST(rigid_group_leaves_three_freedoms) {
  Hook h = make_hook();
  CHECK(solve(h.sk).converged);
  const int before = solve(h.sk).dof;
  CHECK(before > 3);
  h.sk.add_constraint(CT::Rigid, h.curves());
  const auto r = solve(h.sk);
  CHECK(r.converged);
  CHECK_EQ(r.dof, 3);  // it moves and turns, nothing else
  CHECK(r.failed.empty());
}

TEST(rigid_group_turns_whole_when_one_line_is_made_vertical) {
  Hook h = make_hook();
  h.sk.add_constraint(CT::Rigid, h.curves());
  const Sketch before = h.sk;
  h.sk.add_constraint(CT::Vertical, {h.left});
  const auto r = solve(h.sk);
  CHECK(r.converged);
  CHECK_NEAR(h.sk.point(h.foot)->x, h.sk.point(h.top)->x, 1e-8);
  CHECK_NEAR(shape_change(before, h.sk, h.points()), 0, 1e-7);
  CHECK(cross_z(before, h.foot, h.top, h.end) * cross_z(h.sk, h.foot, h.top, h.end) > 0);  // not mirrored
}

TEST(rigid_group_follows_a_drag_as_one) {
  Hook h = make_hook();
  h.sk.add_constraint(CT::Rigid, h.curves());
  const Sketch before = h.sk;
  SolveOptions o;
  o.drags = {{h.foot, 9, -3}};
  const auto r = solve(h.sk, o);
  CHECK(r.converged);
  CHECK(std::hypot(h.sk.point(h.foot)->x - 4, h.sk.point(h.foot)->y) > 1);  // it went somewhere
  CHECK_NEAR(shape_change(before, h.sk, h.points()), 0, 1e-7);
}

TEST(rigid_group_refuses_a_dimension_that_reshapes_it) {
  Hook h = make_hook();
  const int length = h.sk.add_constraint(CT::Distance, {h.left}, std::hypot(4 - 0.2, 17.5));
  h.sk.add_constraint(CT::Rigid, h.curves());
  CHECK(solve(h.sk).converged);
  h.sk.constraint(length)->value += 2;
  Sketch tried = h.sk;
  CHECK(!solve(tried).converged);
}

TEST(rigid_circle_keeps_its_radius) {
  Sketch sk;
  const int c = sk.add_circle(sk.add_point(0, 0), 5);
  const int l = sk.add_line(sk.add_point(10, 0), sk.add_point(10, 8));
  sk.add_constraint(CT::Rigid, {c, l});
  sk.add_constraint(CT::Distance, {l}, 8);
  SolveOptions o;
  o.drags = {{sk.entity(l)->p[1], 3, 20}};
  CHECK(solve(sk, o).converged);
  CHECK_NEAR(sk.entity(c)->r, 5, 1e-8);
  const SkPoint *o0 = sk.point(sk.entity(c)->p[0]), *a = sk.point(sk.entity(l)->p[0]);
  CHECK_NEAR(std::hypot(a->x - o0->x, a->y - o0->y), 10, 1e-7);
}

TEST(rigid_group_saves_apart_for_older_builds_and_keeps_members_on_delete) {
  Hook h = make_hook();
  const int g = h.sk.add_constraint(CT::Rigid, h.curves());
  const opad::json j = h.sk.to_json();
  CHECK(j.contains("more_constraints"));  // an older build does not know the type: it skips that key
  for (const auto& c : j["constraints"]) CHECK(c["type"] != "rigid");
  Sketch back = Sketch::from_json(j);
  CHECK(back.constraint(g) && back.constraint(g)->type == CT::Rigid && back.constraint(g)->refs.size() == 4);
  back.remove(h.right);
  CHECK(back.constraint(g) && back.constraint(g)->refs.size() == 3);  // the rest still holds together
  CHECK_THROWS(back.add_constraint(CT::Rigid, {h.left, h.left}));
}

TEST(transform_of_one_member_moves_the_whole_group) {
  Hook h = make_hook();
  h.sk.add_constraint(CT::Rigid, {h.left, h.round, h.arc});
  const double x = h.sk.point(h.big)->x;
  SketchTransform t;
  t.x = 5;
  transform_entities(h.sk, {h.left}, t, false);
  CHECK_NEAR(h.sk.point(h.big)->x, x + 5, 1e-12);
  CHECK(solve(h.sk).converged);
}

TEST(align_makes_a_line_exactly_vertical_and_keeps_the_shape) {
  Hook h = make_hook();
  const Sketch before = h.sk;
  SketchAlign a;
  a.reference = h.left;
  a.pivot = h.foot;
  align_entities(h.sk, h.curves(), a);
  CHECK(h.sk.point(h.foot)->x == h.sk.point(h.top)->x);  // exactly, not within round-off
  CHECK_NEAR(h.sk.point(h.foot)->x, 4, 1e-12);            // turned about its foot
  CHECK_NEAR(h.sk.point(h.foot)->y, 0, 1e-12);
  CHECK_NEAR(shape_change(before, h.sk, h.points()), 0, 1e-9);
  CHECK(h.sk.point(h.top)->y > 0);  // the smaller turn: still pointing up
  CHECK(solve(h.sk).converged);
}

TEST(align_moves_a_rigid_group_from_its_line_alone_and_can_hold_it) {
  Hook h = make_hook();
  h.sk.add_constraint(CT::Rigid, h.curves());
  const Sketch before = h.sk;
  SketchAlign a;
  a.reference = h.left;
  a.pivot = h.top;
  a.to = AlignTo::Horizontal;
  a.hold = true;
  align_entities(h.sk, {}, a);  // nothing picked but the line: its group goes with it
  CHECK(h.sk.point(h.foot)->y == h.sk.point(h.top)->y);
  CHECK_NEAR(shape_change(before, h.sk, h.points()), 0, 1e-9);
  bool held = false;
  for (const auto& c : h.sk.constraints) held = held || (c.type == CT::Horizontal && c.refs == std::vector<int>{h.left});
  CHECK(held);
  CHECK(solve(h.sk).converged);
}

// A DXF's straight-looking segment is often a very flat arc (a polyline bulge): it aligns by the line between its ends.
TEST(align_takes_a_flat_arc_by_its_ends_and_turns_its_rigid_group) {
  Sketch sk;
  const double r = 71.0, cx = -68.3, cy = -12.9;  // as the imported profile: about 9 mm of arc on a 71 mm radius
  const int centre = sk.add_point(cx, cy);
  const int a = sk.add_point(cx + r * std::cos(0.19), cy + r * std::sin(0.19));
  const int b = sk.add_point(cx + r * std::cos(0.31), cy + r * std::sin(0.31));
  const int flat = sk.add_arc(centre, a, b);
  const int c = sk.add_point(sk.point(b)->x + 6, sk.point(b)->y + 1);
  const int top = sk.add_line(b, c);
  sk.add_constraint(CT::Rigid, {flat, top});
  CHECK(solve(sk).converged);
  const Sketch before = sk;
  SketchAlign al;
  al.reference = flat;
  al.pivot = a;
  al.hold = true;
  align_entities(sk, {}, al);
  const auto r1 = solve(sk);
  CHECK(r1.converged);
  CHECK_NEAR(sk.point(a)->x, sk.point(b)->x, 1e-8);  // its ends one above the other
  CHECK_NEAR(shape_change(before, sk, {centre, a, b, c}), 0, 1e-7);
  bool held = false;
  for (const auto& k : sk.constraints) held = held || (k.type == CT::Vertical && k.refs == std::vector<int>{a, b});
  CHECK(held);
  int x = 0, y = 0;
  CHECK(!align_ends(sk, sk.add_circle(sk.add_point(50, 50), 2), x, y));  // a circle has no ends
}

TEST(align_lays_a_line_onto_another) {
  Hook h = make_hook();
  const int guide = h.sk.add_line(h.sk.add_point(-10, -5), h.sk.add_point(30, 5));
  const Sketch before = h.sk;
  SketchAlign a;
  a.reference = h.left;
  a.pivot = h.foot;
  a.to = AlignTo::Collinear;
  a.target = guide;
  align_entities(h.sk, h.curves(), a);
  const SkEntity& g = *h.sk.entity(guide);
  const SkPoint *g0 = h.sk.point(g.p[0]), *g1 = h.sk.point(g.p[1]);
  auto off = [&](int p) {
    const SkPoint* q = h.sk.point(p);
    return ((g1->x - g0->x) * (q->y - g0->y) - (g1->y - g0->y) * (q->x - g0->x)) / std::hypot(g1->x - g0->x, g1->y - g0->y);
  };
  CHECK_NEAR(off(h.foot), 0, 1e-9);
  CHECK_NEAR(off(h.top), 0, 1e-9);
  CHECK_NEAR(shape_change(before, h.sk, h.points()), 0, 1e-9);
  CHECK_THROWS([&] { SketchAlign self = a; self.target = h.right; Sketch copy = h.sk; align_entities(copy, h.curves(), self); }());
}

CHECK_MAIN()
