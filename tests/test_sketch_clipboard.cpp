// The sketch clipboard (core design/sketch_edit, TODO 11 UI-129): copied curves keep their points and the constraints among
// them, never what ties them to geometry left behind (a coincidence with the origin, a fix, a coordinate); a paste puts the
// base point where it is asked to with new ids, dimension labels moved along and expressions renamed to the copies; a
// clip survives JSON, pastes into another sketch and solves there; anything else is refused.
#include "check.hpp"
#include "opad/design/sketch_edit.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
using namespace opad::design;
using opad::json;
using T = SkConstraint::Type;

namespace {
// A 40 x 20 rectangle from the origin point (coincident with it through its corner), its width a dimension, its height an
// expression of the width, a fixed circle beside it.
struct Drawing {
  Sketch sk;
  int origin, a, b, c, d, bottom, right, top, left, width, height, circle;
  Drawing() {
    origin = sk.add_point(0, 0, true);
    a = sk.add_point(0, 0), b = sk.add_point(40, 0), c = sk.add_point(40, 20), d = sk.add_point(0, 20);
    bottom = sk.add_line(a, b), right = sk.add_line(b, c), top = sk.add_line(c, d), left = sk.add_line(d, a);
    sk.add_constraint(T::Horizontal, {bottom});
    sk.add_constraint(T::Horizontal, {top});
    sk.add_constraint(T::Vertical, {left});
    sk.add_constraint(T::Vertical, {right});
    sk.add_constraint(T::Coincident, {a, origin});
    width = sk.add_constraint(T::Distance, {bottom}, 40);
    sk.constraint(width)->pos[0] = 20, sk.constraint(width)->pos[1] = -5;
    height = sk.add_constraint(T::Distance, {right}, 20, "d" + std::to_string(width) + " / 2");
    circle = sk.add_circle(sk.add_point(60, 10), 5);
    sk.add_constraint(T::Fix, {circle});
    sk.add_constraint(T::HDistance, {sk.entity(circle)->p[0]}, 60);
    CHECK(solve(sk).converged);
  }
};
size_t count(const Sketch& sk, T type) { return size_t(std::count_if(sk.constraints.begin(), sk.constraints.end(), [type](const SkConstraint& c) { return c.type == type; })); }
}  // namespace

TEST(a_copy_keeps_the_constraints_among_the_curves_only) {
  Drawing d;
  const json clip = copy_entities(d.sk, {d.bottom, d.right, d.top, d.left, d.circle}, 0, 0);
  CHECK(clip.at("format") == "opad.sketch.clipboard" && clip.at("version") == 1 && clip.at("base") == json::array({0.0, 0.0}));
  const Sketch copied = Sketch::from_json(clip.at("sketch"));
  CHECK_EQ(copied.entities.size(), size_t(5));
  CHECK_EQ(copied.points.size(), size_t(5));  // four corners and the centre, not the origin
  CHECK_EQ(count(copied, T::Horizontal) + count(copied, T::Vertical), size_t(4));
  CHECK_EQ(count(copied, T::Distance), size_t(2));
  CHECK_EQ(count(copied, T::Coincident) + count(copied, T::Fix) + count(copied, T::HDistance), size_t(0));  // tied to what stays
  CHECK(std::none_of(copied.points.begin(), copied.points.end(), [](const SkPoint& p) { return p.fixed; }));
  CHECK_THROWS(copy_entities(d.sk, {}, 0, 0));
  CHECK_THROWS(copy_entities(d.sk, {d.width}, 0, 0));  // a constraint alone names no curve
}

TEST(a_paste_puts_the_base_point_where_asked_with_new_ids) {
  Drawing d;
  const json clip = copy_entities(d.sk, {d.bottom, d.right, d.top, d.left}, 0, 0);
  const size_t points = d.sk.points.size(), constraints = d.sk.constraints.size();
  const auto made = paste_entities(d.sk, json::parse(clip.dump()), 100, 50);  // through text, as the clipboard carries it
  CHECK_EQ(made.size(), size_t(4));
  CHECK_EQ(d.sk.points.size(), points + 4);
  CHECK_EQ(d.sk.constraints.size(), constraints + 6);
  for (int id : made) CHECK(id > d.circle && d.sk.entity(id) && d.sk.entity(id)->type == SkEntity::Type::Line);
  double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (int id : made)
    for (int p : d.sk.entity(id)->p) x0 = std::min(x0, d.sk.point(p)->x), y0 = std::min(y0, d.sk.point(p)->y), x1 = std::max(x1, d.sk.point(p)->x), y1 = std::max(y1, d.sk.point(p)->y);
  CHECK_NEAR(x0, 100, 1e-12);
  CHECK_NEAR(y0, 50, 1e-12);
  CHECK_NEAR(x1, 140, 1e-12);
  CHECK_NEAR(y1, 70, 1e-12);
  // The width's label moved with it; the height's expression names the copied width.
  const SkConstraint& width = d.sk.constraints[constraints + 4];
  const SkConstraint& height = d.sk.constraints[constraints + 5];
  CHECK(width.type == T::Distance && width.pos[0] == 120 && width.pos[1] == 45 && width.expr.empty());
  CHECK(height.expr == "d" + std::to_string(width.id) + " / 2");
  CHECK(height.pos[0] == 0 && height.pos[1] == 0);  // never placed: still beside what it measures
  const auto result = solve(d.sk);
  CHECK(result.converged);
  d.sk.constraint(width.id)->value = 50;  // the copy's own dimensions drive it, the original stays
  evaluate_dimensions(d.sk);
  CHECK(solve(d.sk).converged);
  CHECK_NEAR(d.sk.constraint(height.id)->value, 25, 1e-9);
  CHECK_NEAR(d.sk.constraint(d.height)->value, 20, 1e-9);
  d.sk.validate();
}

TEST(a_clip_pastes_into_another_sketch) {
  Drawing d;
  json clip = copy_entities(d.sk, {d.right, d.top}, 40, 0);  // the height without the width it names
  Sketch other;
  other.add_point(0, 0, true);
  const auto made = paste_entities(other, clip, -10, -10);
  CHECK_EQ(made.size(), size_t(2));
  CHECK(std::all_of(other.constraints.begin(), other.constraints.end(), [](const SkConstraint& c) { return c.expr.empty(); }));  // "dN / 2" names nothing here
  CHECK(solve(other).converged);
  other.validate();
  CHECK_NEAR(other.point(other.entity(made[0])->p[0])->x, -10, 1e-12);
  // Twice: other ids again.
  const auto again = paste_entities(other, clip, 0, 0);
  CHECK(again[0] != made[0] && again[1] != made[1]);
  other.validate();
}

// A box selection of a converted drawing names every point as well as every curve: a point's point curve is found once,
// not by scanning the curves per point (30,000 segments: seconds before).
TEST(a_box_selection_of_a_large_sketch_copies_in_linear_time) {
  Sketch sk;
  std::vector<int> ids;
  for (int i = 0; i < 30000; ++i) {
    const int a = sk.add_point(i, 0), b = sk.add_point(i + 0.5, 1);
    ids.push_back(sk.add_line(a, b)), ids.push_back(a), ids.push_back(b);
  }
  const int lone = sk.add_point(-5, -5);
  SkEntity mark;
  mark.type = SkEntity::Type::Point, mark.id = sk.next_id(), mark.p = {lone};
  sk.entities.push_back(mark);
  ids.push_back(lone);
  const auto start = std::chrono::steady_clock::now();
  const json clip = copy_entities(sk, ids, -5, -5);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("copy of 30,000 segments and 60,001 points: %.3f s\n", seconds);
  CHECK(seconds < 1.5);
  CHECK_EQ(clip.at("sketch").at("entities").size(), size_t(30001));  // the lone point's curve came through its point
  CHECK_EQ(clip.at("sketch").at("points").size(), size_t(60001));
}

TEST(only_a_sketch_clip_pastes) {
  Sketch sk;
  CHECK_THROWS(paste_entities(sk, json::object(), 0, 0));
  CHECK_THROWS(paste_entities(sk, json{{"format", "opad.bodies"}, {"base", {0, 0}}, {"sketch", json::object()}}, 0, 0));
  CHECK_THROWS(paste_entities(sk, json{{"format", "opad.sketch.clipboard"}, {"base", {0, 0}}, {"sketch", {{"points", json::array()}, {"entities", json::array({{{"id", 3}, {"type", "line"}, {"p", {1, 2}}}})}}}}, 0, 0));  // dangling
  CHECK(sk.entities.empty() && sk.points.empty());
}

CHECK_MAIN()
