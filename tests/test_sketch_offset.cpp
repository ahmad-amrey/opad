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

CHECK_MAIN()
