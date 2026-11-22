// Offset relationships (SkConstraint::Type::Offset, offset_relationship / refresh_offsets): an offset copy held to its chain
// like a rigid group, whose distance can be changed later (the offset is made again there).
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "check.hpp"
#include "opad/design/sketch_modify.hpp"

using namespace opad::design;
using CT = SkConstraint::Type;
using opad::json;

namespace {

struct Plate {
  Sketch sk;
  std::vector<int> sides;  // the 40 x 20 rectangle's lines
};

Plate make_plate() {
  Plate p;
  auto& sk = p.sk;
  const int a = sk.add_point(0, 0), b = sk.add_point(40, 0), c = sk.add_point(40, 20), d = sk.add_point(0, 20);
  p.sides = {sk.add_line(a, b), sk.add_line(b, c), sk.add_line(c, d), sk.add_line(d, a)};
  return p;
}

const SkConstraint* offset_of(const Sketch& sk) {
  for (const auto& c : sk.constraints)
    if (c.type == CT::Offset) return &c;
  return nullptr;
}

std::vector<int> made_by(const SkConstraint& c) { return {c.refs.begin() + c.sources, c.refs.end()}; }
std::vector<int> sources_of(const SkConstraint& c) { return {c.refs.begin(), c.refs.begin() + c.sources}; }

// Half the width the curves' points span more than the source's: the offset's distance (negative: inside).
double grown(const Sketch& sk, const std::vector<int>& source, const std::vector<int>& made) {
  auto width = [&](const std::vector<int>& ids) {
    double lo = 1e300, hi = -1e300;
    for (int id : ids)
      for (int p : sk.entity(id)->p) lo = std::min(lo, sk.point(p)->x), hi = std::max(hi, sk.point(p)->x);
    return hi - lo;
  };
  return (width(made) - width(source)) / 2;
}

std::vector<int> group_points(const Sketch& sk, const SkConstraint& c) {
  std::set<int> pts;
  for (int id : c.refs)
    for (int p : sk.entity(id)->p) pts.insert(p);
  return {pts.begin(), pts.end()};
}

double shape_change(const Sketch& before, const Sketch& after, const std::vector<int>& ids) {
  double worst = 0;
  for (size_t i = 0; i < ids.size(); ++i)
    for (size_t j = i + 1; j < ids.size(); ++j) {
      const SkPoint *a = before.point(ids[i]), *b = before.point(ids[j]), *c = after.point(ids[i]), *d = after.point(ids[j]);
      worst = std::max(worst, std::fabs(std::hypot(a->x - b->x, a->y - b->y) - std::hypot(c->x - d->x, c->y - d->y)));
    }
  return worst;
}

// An outward offset of the plate by 3 (whichever sign the kernel takes for outward here), sharp corners unless `round`.
int outward(Plate& p, bool round, double d = 3) {
  Sketch trial = p.sk;
  const int id = offset_relationship(trial, p.sides, d, round);
  if (grown(trial, p.sides, made_by(*trial.constraint(id))) < 0) {
    trial = p.sk;
    return offset_relationship(p.sk, p.sides, -d, round);
  }
  p.sk = trial;
  return id;
}

}  // namespace

TEST(offset_relationship_holds_the_copy_as_one_piece) {
  Plate p = make_plate();
  const int id = outward(p, false);
  const SkConstraint& c = *p.sk.constraint(id);
  CHECK(c.type == CT::Offset && c.is_dimension());
  CHECK_EQ(c.sources, 4);
  CHECK_EQ(int(c.refs.size()), 8);
  CHECK_NEAR(std::fabs(c.value), 3, 1e-12);
  CHECK_NEAR(c.built, c.value, 1e-12);
  CHECK_NEAR(grown(p.sk, p.sides, made_by(c)), 3, 1e-6);
  const SolveResult r = solve(p.sk);
  CHECK(r.converged);
  CHECK_EQ(r.dof, 3);  // the chain and its offset only move and turn together
}

TEST(offset_new_value_makes_it_again_in_place) {
  Plate p = make_plate();
  const int id = outward(p, false);
  const auto made = made_by(*p.sk.constraint(id));
  const auto points = group_points(p.sk, *p.sk.constraint(id));
  SkConstraint* c = p.sk.constraint(id);
  c->value = c->value > 0 ? 1 : -1;  // 1 mm, the same side
  evaluate_dimensions(p.sk);
  CHECK(solve(p.sk).converged);
  c = p.sk.constraint(id);
  CHECK(made_by(*c) == made);                       // the same curves, ids kept
  CHECK(group_points(p.sk, *c) == points);          // and the same points
  CHECK_NEAR(grown(p.sk, p.sides, made), 1, 1e-6);  // now 1 mm out
  CHECK_NEAR(c->built, c->value, 1e-12);
  // Nothing to do while the value stays: the curves are left as they are.
  const Sketch before = p.sk;
  evaluate_dimensions(p.sk);
  CHECK(p.sk.to_json() == before.to_json());
}

TEST(offset_negative_value_goes_to_the_other_side) {
  Plate p = make_plate();
  const int id = outward(p, false);
  SkConstraint* c = p.sk.constraint(id);
  c->value = -c->value * 2 / 3;  // 2 mm, inside
  evaluate_dimensions(p.sk);
  CHECK(solve(p.sk).converged);
  CHECK_NEAR(grown(p.sk, p.sides, made_by(*p.sk.constraint(id))), -2, 1e-6);
}

TEST(offset_other_curves_replace_the_old_ones) {
  Plate p = make_plate();
  const int id = outward(p, true);  // round corners outside: four lines and four arcs
  CHECK_EQ(int(p.sk.constraint(id)->refs.size()), 12);
  SkConstraint* c = p.sk.constraint(id);
  c->value = -c->value;  // inside, where the corners stay sharp: four lines
  evaluate_dimensions(p.sk);
  CHECK(solve(p.sk).converged);
  c = p.sk.constraint(id);
  CHECK(c && c->type == CT::Offset);
  CHECK_EQ(int(c->refs.size()), 8);
  CHECK(sources_of(*c) == p.sides);
  CHECK_NEAR(grown(p.sk, p.sides, made_by(*c)), -3, 1e-6);
  CHECK_EQ(int(p.sk.entities.size()), 8);  // the arcs went
}

TEST(offset_follows_a_drag_with_its_distance) {
  Plate p = make_plate();
  const int id = outward(p, false);
  CHECK(solve(p.sk).converged);
  const Sketch before = p.sk;
  SolveOptions o;
  o.drags.push_back({p.sk.entity(p.sides[0])->p[0], 10, 7});
  CHECK(solve(p.sk, o).converged);
  const auto pts = group_points(p.sk, *p.sk.constraint(id));
  CHECK_NEAR(shape_change(before, p.sk, pts), 0, 1e-7);
  CHECK(std::hypot(p.sk.point(pts[0])->x - before.point(pts[0])->x, p.sk.point(pts[0])->y - before.point(pts[0])->y) > 1);
  // Moving one curve in place takes the whole relationship along.
  const auto group = rigid_closure(p.sk, {p.sides[1]});
  CHECK_EQ(int(group.size()), 8);
}

TEST(offset_refuses_a_dimension_that_reshapes_the_chain) {
  Plate p = make_plate();
  outward(p, false);
  CHECK(solve(p.sk).converged);
  Sketch tried = p.sk;
  tried.add_constraint(CT::Distance, {p.sides[0]}, 50);
  CHECK(!solve(tried).converged);
}

TEST(offset_follows_a_parameter) {
  Plate p = make_plate();
  const int id = outward(p, false);
  SkConstraint* c = p.sk.constraint(id);
  c->expr = c->value > 0 ? "gap" : "-gap";
  const std::vector<ParamDef> defs{{"gap", "gap", "2 mm", {}}};
  evaluate_dimensions(p.sk, ParamTable(defs));
  CHECK(solve(p.sk).converged);
  CHECK_NEAR(std::fabs(p.sk.constraint(id)->value), 2, 1e-12);
  CHECK_NEAR(grown(p.sk, p.sides, made_by(*p.sk.constraint(id))), 2, 1e-6);
}

TEST(offset_saves_apart_for_older_builds_and_goes_with_its_curves) {
  Plate p = make_plate();
  const int id = outward(p, true);
  const json j = p.sk.to_json();
  CHECK(j.contains("more_constraints"));
  for (const auto& c : j["constraints"]) CHECK(c["type"] != "offset");
  Sketch back = Sketch::from_json(j);
  const SkConstraint* c = back.constraint(id);
  CHECK(c && c->type == CT::Offset && c->sources == 4 && !c->sharp);
  CHECK_NEAR(c->built, p.sk.constraint(id)->built, 1e-12);
  CHECK(back.to_json() == j);
  // Deleting a curve of the offset leaves plain curves: the relationship goes.
  Sketch cut = p.sk;
  cut.remove(made_by(*cut.constraint(id)).front());
  CHECK(!offset_of(cut));
  // A malformed record is refused.
  Sketch bad = p.sk;
  bad.constraint(id)->sources = int(bad.constraint(id)->refs.size());  // no curves left for what it made
  CHECK_THROWS(bad.validate());
}

TEST(offset_mirrored_copy_keeps_its_side_when_changed) {
  Plate p = make_plate();
  const int id = outward(p, false);
  const SkConstraint c = *p.sk.constraint(id);
  SketchTransform t;
  t.mirror = true;
  t.y = -30;
  transform_entities(p.sk, c.refs, t, true);
  const SkConstraint* copy = nullptr;
  for (const auto& x : p.sk.constraints)
    if (x.type == CT::Offset && x.id != id) copy = &x;
  CHECK(copy);
  const int cid = copy->id;
  const auto src = sources_of(*copy);
  p.sk.constraint(cid)->value = c.value / 3;  // 1 mm, on the side it is on (outside)
  evaluate_dimensions(p.sk);
  CHECK(solve(p.sk).converged);
  CHECK_NEAR(grown(p.sk, src, made_by(*p.sk.constraint(cid))), 1, 1e-6);
  CHECK_NEAR(grown(p.sk, p.sides, made_by(*p.sk.constraint(id))), 3, 1e-6);  // the first one is left alone
}

TEST(offset_of_an_offset_follows_it) {
  Plate p = make_plate();
  const int first = outward(p, false);
  const auto ring = made_by(*p.sk.constraint(first));
  Sketch trial = p.sk;
  int second = offset_relationship(trial, ring, 2, false);
  if (grown(trial, ring, made_by(*trial.constraint(second))) < 0) {
    trial = p.sk;
    second = offset_relationship(trial, ring, -2, false);
  }
  p.sk = trial;
  CHECK(solve(p.sk).converged);
  p.sk.constraint(first)->value /= 3;  // the ring comes in to 1 mm; the outer ring stays 2 mm from it
  evaluate_dimensions(p.sk);
  CHECK(solve(p.sk).converged);
  CHECK_NEAR(grown(p.sk, p.sides, ring), 1, 1e-6);
  CHECK_NEAR(grown(p.sk, ring, made_by(*p.sk.constraint(second))), 2, 1e-6);
}

TEST(offset_too_far_inside_is_refused) {
  Plate p = make_plate();
  const int id = outward(p, false);
  Sketch tried = p.sk;
  SkConstraint* c = tried.constraint(id);
  c->value = -c->value / 3 * 15;  // 15 mm inside a 20 mm wide plate
  CHECK_THROWS(evaluate_dimensions(tried));
}

// An open chain offsets to ONE side, a parallel copy (it used to come back as a closed loop round both sides with end caps):
// the side of the point given (the pointer's), for a line, an arc and a chain; the relationship keeps that side.
TEST(open_chain_offsets_to_one_side) {
  {  // one line along X: the copy above it, then below it
    for (const double side : {5.0, -5.0}) {
      Sketch sk;
      const int line = sk.add_line(sk.add_point(0, 0), sk.add_point(20, 0));
      const double d = offset_side(sk, {line}, 3, false, 10, side);
      const auto made = offset_entities(sk, {line}, d, false);
      CHECK_EQ(made.size(), size_t(1));
      const SkEntity* copy = sk.entity(made[0]);
      CHECK(copy && copy->type == SkEntity::Type::Line);
      const SkPoint *a = sk.point(copy->p[0]), *b = sk.point(copy->p[1]);
      CHECK(std::fabs(a->y - (side > 0 ? 3 : -3)) < 1e-6 && std::fabs(b->y - a->y) < 1e-6);
      CHECK(std::fabs(std::fabs(b->x - a->x) - 20) < 1e-6);  // as long as the line: no caps
    }
  }
  {  // a quarter arc of radius 10: the copy outside (radius 12) where the point is outside
    Sketch sk;
    const int arc = sk.add_arc(sk.add_point(0, 0), sk.add_point(10, 0), sk.add_point(0, 10));
    const double d = offset_side(sk, {arc}, 2, false, 9, 9);
    const auto made = offset_entities(sk, {arc}, d, false);
    CHECK_EQ(made.size(), size_t(1));
    const SkEntity* copy = sk.entity(made[0]);
    CHECK(copy && copy->type == SkEntity::Type::Arc);
    const SkPoint *c = sk.point(copy->p[0]), *s = sk.point(copy->p[1]);
    CHECK(std::fabs(std::hypot(s->x - c->x, s->y - c->y) - 12) < 1e-6);
  }
  {  // an L of two lines: one open chain on the inside of the L
    Sketch sk;
    const int a = sk.add_point(0, 10), b = sk.add_point(0, 0), c = sk.add_point(10, 0);
    const std::vector<int> chain{sk.add_line(a, b), sk.add_line(b, c)};
    const double d = offset_side(sk, chain, 2, false, 5, 5);
    const auto made = offset_entities(sk, chain, d, false);
    CHECK(made.size() == 2);
    std::map<int, int> ends;  // an open chain: two ends used once
    for (int id : made) for (int p : sk.entity(id)->p) ++ends[p];
    CHECK(std::count_if(ends.begin(), ends.end(), [](const auto& e) { return e.second == 1; }) == 2);
    for (int id : made) for (int p : sk.entity(id)->p) CHECK(sk.point(p)->x > 1.9 - 1e-6 && sk.point(p)->y > 1.9 - 1e-6);
  }
  {  // the relationship on one line: the copy stays on its side when the distance changes
    Sketch sk;
    const int line = sk.add_line(sk.add_point(0, 0), sk.add_point(20, 0));
    const double d = offset_side(sk, {line}, 3, false, 10, -5);
    const int rel = offset_relationship(sk, {line}, d, false);
    CHECK(solve(sk).converged);
    sk.constraint(rel)->value = d / 3 * 7;
    evaluate_dimensions(sk);
    CHECK(solve(sk).converged);
    const SkConstraint* c = sk.constraint(rel);
    const SkEntity* copy = sk.entity(c->refs.back());
    CHECK(copy && std::fabs(sk.point(copy->p[0])->y + 7) < 1e-6);
  }
}

// Several chains selected (the chain click takes a branching set, or chains lie apart): each is offset on its own.
TEST(offset_groups_split_the_selection_into_chains) {
  auto sizes = [](const std::vector<OffsetGroup>& groups) {
    std::vector<std::pair<size_t, bool>> out;
    for (const auto& g : groups) out.push_back({g.ids.size(), g.closed});
    return out;
  };
  using Sizes = std::vector<std::pair<size_t, bool>>;
  {  // two rectangles apart: two closed chains
    Plate p = make_plate();
    const int a = p.sk.add_point(60, 0), b = p.sk.add_point(80, 0), c = p.sk.add_point(80, 10), d = p.sk.add_point(60, 10);
    std::vector<int> ids = p.sides;
    for (int id : {p.sk.add_line(a, b), p.sk.add_line(b, c), p.sk.add_line(c, d), p.sk.add_line(d, a)}) ids.push_back(id);
    CHECK((sizes(offset_groups(p.sk, ids)) == Sizes{{4, true}, {4, true}}));
  }
  {  // an L picked from its second line, a line apart and a point: the L as one open chain in its order, the line alone
    Sketch sk;
    const int a = sk.add_point(0, 10), b = sk.add_point(0, 0), c = sk.add_point(10, 0);
    const int l1 = sk.add_line(a, b), l2 = sk.add_line(b, c), apart = sk.add_line(sk.add_point(20, 0), sk.add_point(30, 0));
    const auto groups = offset_groups(sk, {l2, apart, a, l1});
    CHECK((sizes(groups) == Sizes{{2, false}, {1, false}}));
    CHECK((groups[0].ids == std::vector<int>{l1, l2}));
  }
  {  // three lines meeting at one point (a T): three chains
    Sketch sk;
    const int o = sk.add_point(0, 0);
    const std::vector<int> ids{sk.add_line(o, sk.add_point(10, 0)), sk.add_line(o, sk.add_point(-10, 0)), sk.add_line(o, sk.add_point(0, 10))};
    CHECK((sizes(offset_groups(sk, ids)) == Sizes{{1, false}, {1, false}, {1, false}}));
  }
  {  // a rectangle with a line off one corner and a circle: the loop, the tail, the circle
    Plate p = make_plate();
    std::vector<int> ids = p.sides;
    ids.push_back(p.sk.add_line(p.sk.entity(p.sides[1])->p[0], p.sk.add_point(50, -10)));
    ids.push_back(p.sk.add_circle(p.sk.add_point(20, 10), 3));
    CHECK((sizes(offset_groups(p.sk, ids)) == Sizes{{4, true}, {1, false}, {1, true}}));
  }
  {  // two lines whose ends are two points in one place: one chain
    Sketch sk;
    const int l1 = sk.add_line(sk.add_point(0, 0), sk.add_point(10, 0)), l2 = sk.add_line(sk.add_point(10, 0), sk.add_point(10, 10));
    CHECK((sizes(offset_groups(sk, {l1, l2})) == Sizes{{2, false}}));
  }
}

// Each open chain takes its side from the pointer, as if picked alone, and a negative distance turns every chain round
// (not only the one nearest the pointer: one 10 mm away kept its side when the arrow's point was used for all).
TEST(negative_distance_turns_every_chain) {
  auto copies = [](Sketch sk, const std::vector<int>& ids, double distance, double px, double py) {
    std::vector<std::pair<double, double>> mids;  // each copy's middle, in the order of the chains
    for (const auto& g : offset_groups(sk, ids))
      for (int id : offset_entities(sk, g.ids, offset_toward(sk, g, distance, false, px, py), false)) {
        const SkEntity* e = sk.entity(id);
        const SkPoint *a = sk.point(e->p.front()), *b = sk.point(e->p.back());
        mids.push_back({(a->x + b->x) / 2, (a->y + b->y) / 2});
      }
    return mids;
  };
  auto near = [](std::pair<double, double> p, double x, double y) { return std::hypot(p.first - x, p.second - y) < 1e-6; };
  {  // two lines apart, y = 0 and y = 10, the pointer between them
    Sketch sk;
    const int low = sk.add_line(sk.add_point(0, 0), sk.add_point(20, 0)), high = sk.add_line(sk.add_point(0, 10), sk.add_point(20, 10));
    const auto plus = copies(sk, {low, high}, 2, 10, 5), minus = copies(sk, {low, high}, -2, 10, 5);
    CHECK(plus.size() == 2 && near(plus[0], 10, 2) && near(plus[1], 10, 8));
    CHECK(minus.size() == 2 && near(minus[0], 10, -2) && near(minus[1], 10, 12));
  }
  {  // a T (three chains at its junction), the pointer right of the stem and above the bar
    Sketch sk;
    const int o = sk.add_point(100, 0);
    const std::vector<int> ids{sk.add_line(o, sk.add_point(80, 0)), sk.add_line(o, sk.add_point(120, 0)), sk.add_line(o, sk.add_point(100, 20))};
    const auto plus = copies(sk, ids, 2, 110, 5), minus = copies(sk, ids, -2, 110, 5);
    CHECK(plus.size() == 3 && near(plus[0], 90, 2) && near(plus[1], 110, 2) && near(plus[2], 102, 10));
    CHECK(minus.size() == 3 && near(minus[0], 90, -2) && near(minus[1], 110, -2) && near(minus[2], 98, 10));
  }
  {  // a closed chain grows outward for a positive distance wherever the pointer is
    Plate p = make_plate();
    const auto groups = offset_groups(p.sk, p.sides);
    CHECK(groups.size() == 1 && offset_toward(p.sk, groups[0], 2, false, 20, 10) == 2 && offset_toward(p.sk, groups[0], -2, false, 100, 100) == -2);
  }
}

TEST(each_chain_offsets_as_if_alone) {
  // An L and a line apart: the L's copy is the one it makes alone (an open chain of two), the line's a line.
  Sketch sk;
  const int a = sk.add_point(0, 10), b = sk.add_point(0, 0), c = sk.add_point(10, 0);
  const int l1 = sk.add_line(a, b), l2 = sk.add_line(b, c), apart = sk.add_line(sk.add_point(20, 0), sk.add_point(30, 0));
  auto points = [](const Sketch& s, const std::vector<int>& made) {
    std::vector<std::pair<double, double>> out;
    for (int id : made)
      for (int p : s.entity(id)->p) out.push_back({std::round(s.point(p)->x * 1e6) / 1e6, std::round(s.point(p)->y * 1e6) / 1e6});
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  };
  Sketch together = sk;
  std::vector<std::vector<int>> made;
  for (const auto& g : offset_groups(together, {l1, l2, apart})) made.push_back(offset_entities(together, g.ids, 2, false));
  CHECK_EQ(made.size(), size_t(2));
  Sketch chain = sk, line = sk;
  CHECK((points(together, made[0]) == points(chain, offset_entities(chain, {l1, l2}, 2, false))));
  CHECK((points(together, made[1]) == points(line, offset_entities(line, {apart}, 2, false))));
  CHECK_EQ(made[0].size(), size_t(2));
  CHECK_EQ(made[1].size(), size_t(1));
  // Held: one relationship per chain, each refreshing on its own.
  Sketch held = sk;
  std::vector<int> rels;
  for (const auto& g : offset_groups(held, {l1, l2, apart})) rels.push_back(offset_relationship(held, g.ids, 2, false));
  CHECK_EQ(rels.size(), size_t(2));
  CHECK(solve(held).converged);
  held.constraint(rels[1])->value = 4;
  evaluate_dimensions(held);
  CHECK(solve(held).converged);
  CHECK_EQ(held.constraint(rels[0])->built, 2.0);
  CHECK_EQ(std::fabs(held.constraint(rels[1])->built), 4.0);
}

// The chain a click takes (chain_entities): every corner where two curves meet, and where several branch only the curve
// that carries on smoothly, as in the report: the arch's right leg carries on down, the horizontal line beside it stays out.
TEST(chain_follows_the_smooth_way_at_a_branch) {
  auto set = [](std::vector<int> v) { return std::set<int>(v.begin(), v.end()); };
  Sketch sk;
  const int a0 = sk.add_point(0, 0), a1 = sk.add_point(0, 10), top = sk.add_point(10, 20), b0 = sk.add_point(20, 10), b1 = sk.add_point(20, 0);
  const int c1 = sk.add_point(10, 10), c2 = sk.add_point(10, 10), side = sk.add_point(30, 10), guide = sk.add_point(-5, 15);
  const int left = sk.add_line(a0, a1), arc1 = sk.add_arc(c1, top, a1), arc2 = sk.add_arc(c2, b0, top), right = sk.add_line(b0, b1);
  const int branch = sk.add_line(b0, side), construction = sk.add_line(a1, guide, true);
  CHECK((set(chain_entities(sk, {left})) == std::set<int>{left, arc1, arc2, right}));
  CHECK((set(chain_entities(sk, {right})) == std::set<int>{left, arc1, arc2, right}));
  CHECK((chain_entities(sk, {left}).front() == left));
  // From the branch both ways on turn a right angle: no clear way on, it stays alone. The construction line too.
  CHECK((set(chain_entities(sk, {branch})) == std::set<int>{branch}));
  CHECK((set(chain_entities(sk, {construction})) == std::set<int>{construction}));
  // connected_entities still takes the whole shape (Move, Align, a rigid group).
  CHECK((set(connected_entities(sk, {left})).count(branch) == 1));
}

TEST(chain_stops_where_no_branch_carries_on) {
  auto set = [](std::vector<int> v) { return std::set<int>(v.begin(), v.end()); };
  Sketch sk;
  // A T: the bar runs straight through, the stem stops at it.
  const int o = sk.add_point(100, 0);
  const int barL = sk.add_line(o, sk.add_point(80, 0)), barR = sk.add_line(o, sk.add_point(120, 0)), stem = sk.add_line(o, sk.add_point(100, 20));
  CHECK((set(chain_entities(sk, {barR})) == std::set<int>{barL, barR}));
  CHECK((set(chain_entities(sk, {stem})) == std::set<int>{stem}));
  // A Y: both arms turn 30 degrees off the leg: neither is the way on.
  const int y = sk.add_point(0, 50);
  const int leg = sk.add_line(sk.add_point(0, 30), y);
  const int arm1 = sk.add_line(y, sk.add_point(-10, 50 + 10 * std::sqrt(3.0))), arm2 = sk.add_line(y, sk.add_point(10, 50 + 10 * std::sqrt(3.0)));
  CHECK((set(chain_entities(sk, {leg})) == std::set<int>{leg}));
  CHECK((set(chain_entities(sk, {arm1})) == std::set<int>{arm1}));
  (void)arm2;
  // A rectangle: every corner joins two curves, so the chain goes all round, and stops where it started.
  const int p = sk.add_point(200, 0), q = sk.add_point(240, 0), r = sk.add_point(240, 20), s = sk.add_point(200, 20);
  const std::vector<int> rect{sk.add_line(p, q), sk.add_line(q, r), sk.add_line(r, s), sk.add_line(s, p)};
  CHECK((set(chain_entities(sk, {rect[2]})) == set(rect)));
  // Ends that only touch (two points in one place, as a DXF brings them) still chain.
  const int t1 = sk.add_point(300, 0), t2 = sk.add_point(310, 0), t3 = sk.add_point(310, 0), t4 = sk.add_point(310, 10);
  const int u1 = sk.add_line(t1, t2), u2 = sk.add_line(t3, t4);
  CHECK((set(chain_entities(sk, {u1})) == std::set<int>{u1, u2}));
}

// The branch turned down at a point is never taken from its other end: the arch's frame comes round through plain corners to
// the leg's top, where the leg and the arch already meet (a third chain curve there would bring the report's horizontal back).
TEST(chain_never_puts_three_curves_on_a_point) {
  auto set = [](std::vector<int> v) { return std::set<int>(v.begin(), v.end()); };
  Sketch sk;
  const int a0 = sk.add_point(0, 0), a1 = sk.add_point(0, 10), top = sk.add_point(10, 20), b0 = sk.add_point(20, 10), b1 = sk.add_point(20, 0);
  const int c = sk.add_point(10, 10), p = sk.add_point(30, 10), q = sk.add_point(30, 0);
  const int left = sk.add_line(a0, a1), arc1 = sk.add_arc(c, top, a1), arc2 = sk.add_arc(c, b0, top), right = sk.add_line(b0, b1);
  const int branch = sk.add_line(b0, p), pq = sk.add_line(p, q), qb = sk.add_line(q, b1);
  const auto chain = set(chain_entities(sk, {arc2}));
  CHECK((chain == std::set<int>{left, arc1, arc2, right, qb, pq}));
  CHECK(!chain.count(branch));
  const auto groups = offset_groups(sk, chain_entities(sk, {arc2}));
  CHECK_EQ(groups.size(), size_t(1));
  CHECK(!groups.front().closed);
  // A window over many curves ending on one vertical line (a converted drawing's hatch) joins nothing it should not.
  Sketch hatch;
  std::vector<int> lines;
  for (int i = 0; i < 2000; ++i) lines.push_back(hatch.add_line(hatch.add_point(0, i * 0.5), hatch.add_point(5, i * 0.5)));
  CHECK((set(chain_entities(hatch, {lines[7]})) == std::set<int>{lines[7]}));
}

// The same chain whichever of its curves is clicked and however it was drawn: a rectangle with the bottom carried on straight
// past a corner takes the straight way (the side meeting it at a right angle goes), its mirror image the same; a click on that
// side keeps it, the rectangle closing instead.
TEST(chain_is_the_same_from_every_curve) {
  auto set = [](std::vector<int> v) { return std::set<int>(v.begin(), v.end()); };
  for (const bool mirrored : {false, true}) {
    Sketch sk;
    const int p0 = sk.add_point(0, 0), p1 = sk.add_point(10, 0), p2 = sk.add_point(10, 5), p3 = sk.add_point(0, 5);
    const int bottom = sk.add_line(p0, p1), right = sk.add_line(p1, p2), top = sk.add_line(p2, p3), left = sk.add_line(p3, p0);
    const int ext = mirrored ? sk.add_line(p1, sk.add_point(30, 0)) : sk.add_line(p0, sk.add_point(-20, 0));
    const int cornerSide = mirrored ? right : left;
    std::set<int> expect{bottom, right, top, left, ext};
    expect.erase(cornerSide);
    for (int seed : {bottom, top, ext, mirrored ? left : right}) CHECK((set(chain_entities(sk, {seed})) == expect));
    CHECK((set(chain_entities(sk, {cornerSide})) == std::set<int>{bottom, right, top, left}));
  }
  // Two seeds in one call (a window) each bring their own chain: an X of four lines, two of them boxed.
  Sketch x;
  const int c = x.add_point(0, 0);
  const int a1 = x.add_line(x.add_point(-10, 0), c), a2 = x.add_line(c, x.add_point(10, 0)), b1 = x.add_line(x.add_point(0, -10), c), b2 = x.add_line(c, x.add_point(0, 10));
  CHECK((set(chain_entities(x, {a1, b1})) == std::set<int>{a1, a2, b1, b2}));
  CHECK((set(chain_entities(x, {b1, a1})) == std::set<int>{a1, a2, b1, b2}));
  // Two rectangles touching at a corner: straight through it both ways, so one loop round both, which offset_groups cuts into
  // the two closed outlines (each offset outward), as before.
  Sketch two;
  const int q0 = two.add_point(0, 0), q1 = two.add_point(10, 0), xx = two.add_point(10, 10), q3 = two.add_point(0, 10);
  const int r1 = two.add_point(20, 10), r2 = two.add_point(20, 20), r3 = two.add_point(10, 20);
  const std::vector<int> all{two.add_line(q0, q1), two.add_line(q1, xx), two.add_line(xx, q3), two.add_line(q3, q0),
                             two.add_line(xx, r1), two.add_line(r1, r2), two.add_line(r2, r3), two.add_line(r3, xx)};
  CHECK((set(chain_entities(two, {all[0]})) == set(all)));
  const auto groups = offset_groups(two, chain_entities(two, {all[0]}));
  CHECK_EQ(groups.size(), size_t(2));
  CHECK(groups[0].closed && groups[1].closed);
}

// Drawn either way round, and with a window's seeds in any order, the chain comes out the same: a frame closing on a chord
// (both loose ends of the path come back to points it runs through), and the rectangle with its bottom carried on.
TEST(chain_ignores_drawing_direction_and_seed_order) {
  auto set = [](std::vector<int> v) { return std::set<int>(v.begin(), v.end()); };
  std::set<std::set<int>> results;
  for (const bool reversed : {false, true}) {
    Sketch sk;
    const int x1 = sk.add_point(0, 0), x2 = sk.add_point(10, 0), w = sk.add_point(-5, 0), nw = sk.add_point(-5, 10), ne = sk.add_point(10, 10);
    const int s = sk.add_point(10, -5), sw = sk.add_point(0, -5);
    auto line = [&](int a, int b) { return reversed ? sk.add_line(b, a) : sk.add_line(a, b); };
    const int a = line(x1, x2), b = line(x1, w), c1 = line(w, nw), c2 = line(nw, ne), c3 = line(ne, x2), d = line(x2, s), e1 = line(s, sw), e2 = line(sw, x1);
    std::set<int> got = set(chain_entities(sk, {b}));
    // In entity order (ids are the same either way round), so the two runs compare.
    std::set<int> rank;
    const std::vector<int> order{a, b, c1, c2, c3, d, e1, e2};
    for (size_t i = 0; i < order.size(); ++i)
      if (got.count(order[i])) rank.insert(int(i));
    results.insert(rank);
    CHECK(got.count(b) == 1);
  }
  CHECK_EQ(results.size(), size_t(1));
  Sketch sk;
  const int p0 = sk.add_point(0, 0), p1 = sk.add_point(10, 0), p2 = sk.add_point(10, 5), p3 = sk.add_point(0, 5);
  const int bottom = sk.add_line(p0, p1), right = sk.add_line(p1, p2), top = sk.add_line(p2, p3), left = sk.add_line(p3, p0);
  sk.add_line(p0, sk.add_point(-20, 0));
  CHECK((set(chain_entities(sk, {left, bottom})) == set(chain_entities(sk, {bottom, left}))));
  CHECK((set(chain_entities(sk, {top, left, right})) == set(chain_entities(sk, {right, top, left}))));
}

// The rc.35 report: a sketch saved a hair off its constraints (the editor never moves stored points on opening), or with
// freedom left, comes out of the offset's solve with every point shifted by about 1e-7, and the preview, comparing exactly,
// drew every curve as changed, as if the whole sketch were picked. Only what the offset makes is drawn.
TEST(the_preview_draws_only_what_an_offset_makes_in_a_sketch_a_hair_off_its_constraints) {
  auto p = make_plate();
  auto& sk = p.sk;
  sk.add_constraint(CT::Horizontal, {p.sides[0]});
  sk.add_constraint(CT::Vertical, {p.sides[1]});
  sk.add_constraint(CT::Horizontal, {p.sides[2]});
  sk.add_constraint(CT::Vertical, {p.sides[3]});
  const int line = sk.add_line(sk.add_point(60, 0), sk.add_point(100, 0));
  sk.points[1].y += 3e-7;  // the bottom no longer quite horizontal, the left no longer quite upright
  sk.points[3].x += 2e-7;
  const Sketch before = sk;
  for (int held = 0; held < 2; ++held) {
    Sketch after = before;
    if (held) offset_relationship(after, {line}, 5, true); else offset_entities(after, {line}, 5, true);
    CHECK(solve(after).converged);
    bool shifted = false;
    for (const auto& q : before.points) shifted = shifted || after.point(q.id)->x != q.x || after.point(q.id)->y != q.y;
    CHECK(shifted);  // the solve put the plate back: what drew every curve
    const auto drawn = changed_entities(before, after);
    CHECK(drawn.size() == 1 && !before.entity(drawn[0]));
  }
  // A real change is still drawn: the top raised by a tenth of a millimetre reshapes the top and both sides.
  Sketch raised = before;
  raised.points[2].y += 0.1;
  raised.points[3].y += 0.1;
  const auto drawn = changed_entities(before, raised);
  CHECK((std::set<int>(drawn.begin(), drawn.end()) == std::set<int>{p.sides[1], p.sides[2], p.sides[3]}));
  // What a tool's own work did is compared exactly (the preview's other half): in a plan 30 m across, a line straightened by
  // a hundredth of a millimetre is below the solve's tolerance there, and still the tool's change.
  Sketch plan = before;
  plan.add_point(30000, 0);
  Sketch straightened = plan;
  straightened.point(straightened.entity(line)->p[1])->y += 0.01;
  CHECK(changed_entities(plan, straightened).empty());
  CHECK((changed_entities(plan, straightened, true) == std::vector<int>{line}));
}

CHECK_MAIN()
