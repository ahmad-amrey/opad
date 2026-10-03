// 2D sketch model and constraint solver (core/src/design): pure maths, no kernel needed.
#include <algorithm>
#include <chrono>
#include <cmath>

#include "check.hpp"
#include "opad/design/sketch.hpp"

using namespace opad::design;
using CT = SkConstraint::Type;

namespace {

constexpr double kPi = 3.14159265358979323846;

double dist(const Sketch& sk, int a, int b) { return std::hypot(sk.point(a)->x - sk.point(b)->x, sk.point(a)->y - sk.point(b)->y); }
double len(const Sketch& sk, int line) { return dist(sk, sk.entity(line)->p[0], sk.entity(line)->p[1]); }
bool has(const std::vector<int>& v, int id) { return std::find(v.begin(), v.end(), id) != v.end(); }

// Signed angle from line a's direction to line b's.
double angle(const Sketch& sk, int a, int b) {
  auto dir = [&](int l, double& x, double& y) {
    const SkPoint *p = sk.point(sk.entity(l)->p[0]), *q = sk.point(sk.entity(l)->p[1]);
    x = q->x - p->x;
    y = q->y - p->y;
  };
  double ax, ay, bx, by;
  dir(a, ax, ay);
  dir(b, bx, by);
  return std::atan2(ax * by - ay * bx, ax * bx + ay * by);
}

// Distance from a point to the infinite line.
double off_line(const Sketch& sk, int p, int line) {
  const SkPoint *a = sk.point(sk.entity(line)->p[0]), *b = sk.point(sk.entity(line)->p[1]), *q = sk.point(p);
  const double dx = b->x - a->x, dy = b->y - a->y;
  return std::fabs(dx * (q->y - a->y) - dy * (q->x - a->x)) / std::hypot(dx, dy);
}

// Four lines with their own end points, tied by coincidences; corner 0 (bottom-left) is fixed.
struct Rect {
  Sketch sk;
  int p[8];  // line i runs p[2i] -> p[2i+1]: bottom, right, top, left
  int l[4];
  int width = 0, height = 0;
};

Rect make_rect(double w, double h) {
  Rect r;
  const double c[8][2] = {{0, 0}, {9.5, 0.4}, {10.2, -0.3}, {10.4, 6.1}, {9.8, 5.7}, {0.3, 6.2}, {-0.4, 5.9}, {0.2, 0.3}};
  for (int i = 0; i < 8; ++i) r.p[i] = r.sk.add_point(c[i][0], c[i][1], i == 0);
  for (int i = 0; i < 4; ++i) r.l[i] = r.sk.add_line(r.p[2 * i], r.p[2 * i + 1]);
  for (int i = 0; i < 4; ++i) r.sk.add_constraint(CT::Coincident, {r.p[2 * i + 1], r.p[(2 * i + 2) % 8]});
  r.sk.add_constraint(CT::Horizontal, {r.l[0]});
  r.sk.add_constraint(CT::Horizontal, {r.l[2]});
  r.sk.add_constraint(CT::Vertical, {r.l[1]});
  r.sk.add_constraint(CT::Vertical, {r.l[3]});
  r.width = r.sk.add_constraint(CT::Distance, {r.l[0]}, w, "width");
  r.height = r.sk.add_constraint(CT::Distance, {r.l[3]}, h);
  return r;
}

}  // namespace

// ---------------------------------------------------------------- model
TEST(sketch_json_round_trip) {
  const char* text = R"({"points":[{"id":1,"x":0.0,"y":0.0,"fixed":true},{"id":2,"x":20.0,"y":1.5},{"id":4,"x":5.0,"y":5.0},
    {"id":6,"x":1.0,"y":0.0},{"id":7,"x":0.0,"y":1.0}],
    "entities":[{"id":3,"type":"line","p":[1,2],"construction":true,"fixed":true},{"id":5,"type":"circle","p":[4],"r":5.0},
    {"id":8,"type":"arc","p":[1,6,7]}],
    "constraints":[{"id":9,"type":"distance","refs":[1,2],"value":20.0,"expr":"width","pos":[10.0,-5.0]},
    {"id":10,"type":"tangent","refs":[3,5]},{"id":11,"type":"radius","refs":[8],"value":1.0,"pos":[0.0,0.0]}]})";
  const opad::json j = opad::json::parse(text);
  const Sketch sk = Sketch::from_json(j);
  CHECK_EQ(sk.points.size(), size_t(5));
  CHECK(sk.point(1)->fixed && !sk.point(2)->fixed);
  CHECK(sk.entity(3)->construction && sk.entity(3)->fixed);
  CHECK(sk.entity(5)->type == SkEntity::Type::Circle);
  CHECK_NEAR(sk.entity(5)->r, 5.0, 0);
  CHECK(sk.constraints[0].expr == "width");
  CHECK_NEAR(sk.constraints[0].pos[1], -5.0, 0);
  CHECK_EQ(sk.next_id(), 12);
  CHECK(sk.to_json() == j);
  CHECK_EQ(sk.to_json().dump(), j.dump());  // key order and omitted members too
  CHECK(!sk.to_json()["constraints"][1].contains("value"));
  CHECK(!sk.to_json()["entities"][0].contains("r"));
}

TEST(sketch_type_names) {
  CHECK(std::string(SkEntity::type_name(SkEntity::Type::Spline)) == "spline");
  CHECK(SkEntity::type_from_name("ellipse") == SkEntity::Type::Ellipse);
  CHECK(std::string(SkConstraint::type_name(CT::HDistance)) == "hdistance");
  for (int t = 0; t <= int(CT::Angle); ++t) CHECK(SkConstraint::type_from_name(SkConstraint::type_name(CT(t))) == CT(t));
  CHECK_THROWS(SkConstraint::type_from_name("Coincident"));
  CHECK_THROWS(SkEntity::type_from_name("polygon"));
  SkConstraint c;
  c.type = CT::Radius;
  CHECK(c.is_dimension());
  c.type = CT::Fix;
  CHECK(!c.is_dimension());
}

TEST(sketch_json_validation) {
  auto bad = [](const char* text) { return Sketch::from_json(opad::json::parse(text)); };
  // duplicate id across points and entities
  CHECK_THROWS(bad(R"({"points":[{"id":1,"x":0,"y":0},{"id":2,"x":1,"y":0}],"entities":[{"id":2,"type":"line","p":[1,2]}]})"));
  // ... and the message names both items and the shared id space (agents read it)
  try {
    bad(R"({"points":[{"id":1,"x":0,"y":0},{"id":2,"x":1,"y":0}],"entities":[{"id":1,"type":"line","p":[1,2]}]})");
    CHECK(false);
  } catch (const opad::Error& e) {
    const std::string why = e.what();
    CHECK(why.find("point 1 and entity 1") != std::string::npos);
    CHECK(why.find("share one id space") != std::string::npos);
  }
  // dangling point
  CHECK_THROWS(bad(R"({"points":[{"id":1,"x":0,"y":0}],"entities":[{"id":3,"type":"line","p":[1,2]}]})"));
  // arc needs three points
  CHECK_THROWS(bad(R"({"points":[{"id":1,"x":0,"y":0},{"id":2,"x":1,"y":0}],"entities":[{"id":3,"type":"arc","p":[1,2]}]})"));
  // circle radius
  CHECK_THROWS(bad(R"({"points":[{"id":1,"x":0,"y":0}],"entities":[{"id":3,"type":"circle","p":[1],"r":0.0}]})"));
  CHECK_THROWS(bad(R"({"points":[{"id":1,"x":0,"y":0}],"entities":[{"id":3,"type":"circle","p":[1]}]})"));
  // unknown types
  CHECK_THROWS(bad(R"({"points":[{"id":1,"x":0,"y":0}],"entities":[{"id":3,"type":"blob","p":[1]}]})"));
  CHECK_THROWS(bad(R"({"points":[{"id":1,"x":0,"y":0}],"constraints":[{"id":3,"type":"glue","refs":[1]}]})"));
  // wrong kind / arity / dangling reference
  const std::string base =
      R"({"points":[{"id":1,"x":0,"y":0},{"id":2,"x":1,"y":0}],"entities":[{"id":3,"type":"line","p":[1,2]}],"constraints":[)";
  CHECK_THROWS(bad((base + R"({"id":4,"type":"parallel","refs":[3,1]}]})").c_str()));
  CHECK_THROWS(bad((base + R"({"id":4,"type":"radius","refs":[3],"value":2.0}]})").c_str()));
  CHECK_THROWS(bad((base + R"({"id":4,"type":"horizontal","refs":[3,3,3]}]})").c_str()));
  CHECK_THROWS(bad((base + R"({"id":4,"type":"fix","refs":[99]}]})").c_str()));
  CHECK_THROWS(bad((base + R"({"id":4,"type":"distance","refs":[1,2]}]})").c_str()));  // dimension without a value
  CHECK_THROWS(bad((base + R"({"id":4,"type":"fix","refs":[4]}]})").c_str()));          // a constraint is no geometry
  CHECK_THROWS(bad("[1,2,3]"));
  CHECK(bad((base + R"({"id":4,"type":"horizontal","refs":[3]}]})").c_str()).constraints.size() == 1);
  CHECK(bad("{}").points.empty());
}

TEST(sketch_builders_and_remove) {
  Sketch sk;
  const int a = sk.add_point(0, 0), b = sk.add_point(10, 0), c = sk.add_point(10, 10);
  const int l1 = sk.add_line(a, b), l2 = sk.add_line(b, c);
  const int h = sk.add_constraint(CT::Horizontal, {l1});
  const int perp = sk.add_constraint(CT::Perpendicular, {l1, l2});
  const int d = sk.add_constraint(CT::Distance, {a, c}, 12, "diag");
  CHECK_EQ(a, 1);
  CHECK_EQ(d, 8);
  CHECK(sk.constraint(d)->expr == "diag");
  CHECK_THROWS(sk.add_line(a, 77));
  CHECK_THROWS(sk.add_circle(a, -1));
  CHECK_THROWS(sk.add_constraint(CT::Parallel, {l1, a}));
  CHECK_EQ(sk.next_id(), 9);  // failed builders leave nothing behind
  sk.validate();

  sk.remove(b);  // still used by both lines
  CHECK(sk.point(b) != nullptr);
  sk.remove(l2);  // takes c (now unused), the perpendicular and the dimension on c; b stays for l1
  CHECK(sk.entity(l2) == nullptr && sk.point(c) == nullptr && sk.point(b) != nullptr);
  CHECK(sk.constraint(perp) == nullptr && sk.constraint(d) == nullptr && sk.constraint(h) != nullptr);
  sk.remove(h);
  CHECK(sk.constraints.empty());
  sk.remove(l1);
  CHECK(sk.points.empty() && sk.entities.empty());
  sk.remove(12345);  // unknown: no-op
}

// UI-29: next_id reads only what was appended since its last call, yet always answers as a scan of every list would, whatever
// the callers did in between (builders, direct pushes, removals, lists cut, reordered or replaced, a renumbered last item).
// Point and curve lookups (a binary search first) find what a scan finds, in sorted lists and in lists out of order.
TEST(next_id_running_maximum_matches_a_full_scan) {
  Sketch sk;
  auto scan = [&] {
    int top = sk.id_watermark;
    for (const auto& p : sk.points) top = std::max(top, p.id);
    for (const auto& e : sk.entities) top = std::max(top, e.id);
    for (const auto& c : sk.constraints) top = std::max(top, c.id);
    for (const auto& i : sk.images) top = std::max(top, i.at("id").get<int>());
    for (const auto& p : sk.patterns) top = std::max(top, p.at("id").get<int>());
    return top + 1;
  };
  unsigned seed = 7;
  auto next = [&](unsigned n) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % n; };
  for (int step = 0; step < 4000; ++step) {
    switch (next(10)) {
      case 0: case 1: case 2: sk.add_point(next(100), next(100)); break;
      case 3: if (sk.points.size() >= 2) sk.add_line(sk.points[next(unsigned(sk.points.size()))].id, sk.points.back().id); break;
      case 4: if (!sk.entities.empty() && sk.entities.back().type == SkEntity::Type::Line) sk.add_constraint(CT::Horizontal, {sk.entities.back().id}); break;
      case 5: if (!sk.points.empty()) sk.remove(sk.points[next(unsigned(sk.points.size()))].id); break;
      case 6: if (!sk.entities.empty()) sk.remove(sk.entities[next(unsigned(sk.entities.size()))].id); break;
      case 7: {  // a caller's own push of a point with an id it chose, erased again now and then (no watermark)
        sk.points.push_back({sk.next_id() + int(next(3)), 1, 1, false});
        if (next(2)) std::erase_if(sk.points, [&](const SkPoint& p) { return p.id == sk.points.back().id; });
        break;
      }
      case 8:  // reordered, or the last point renumbered below the others
        if (next(2)) std::reverse(sk.points.begin(), sk.points.end());
        else if (!sk.points.empty() && std::none_of(sk.entities.begin(), sk.entities.end(), [&](const SkEntity& e) { return std::count(e.p.begin(), e.p.end(), sk.points.back().id); })) sk.points.back().id = -int(step);
        break;
      default: sk.images.push_back({{"id", sk.next_id()}}); if (next(3) == 0) sk.images.erase(sk.images.begin()); break;
    }
    CHECK_EQ(sk.next_id(), scan());
    for (int k = 0; k < 4; ++k) {
      const int id = int(next(unsigned(sk.next_id() + 2))) - 1;
      const SkPoint* point = nullptr;
      for (const auto& p : sk.points) if (!point && p.id == id) point = &p;
      const SkEntity* entity = nullptr;
      for (const auto& e : sk.entities) if (!entity && e.id == id) entity = &e;
      CHECK(std::as_const(sk).point(id) == point && std::as_const(sk).entity(id) == entity);
    }
  }
}

// ---------------------------------------------------------------- solver
TEST(solve_rectangle_and_resize) {
  Rect r = make_rect(20, 8);
  SolveResult res = solve(r.sk);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 0);
  CHECK(res.free_points.empty() && res.redundant.empty() && res.failed.empty());
  CHECK(res.residual <= 1e-8);
  CHECK_NEAR(len(r.sk, r.l[0]), 20, 1e-7);
  CHECK_NEAR(len(r.sk, r.l[2]), 20, 1e-7);
  CHECK_NEAR(len(r.sk, r.l[1]), 8, 1e-7);
  CHECK_NEAR(r.sk.point(r.p[3])->x, 20, 1e-7);  // top-right corner, up and to the right of the fixed one
  CHECK_NEAR(r.sk.point(r.p[3])->y, 8, 1e-7);

  r.sk.constraint(r.width)->value = 35;
  res = solve(r.sk);
  CHECK(res.converged);
  CHECK_NEAR(r.sk.point(r.p[0])->x, 0, 0);
  CHECK_NEAR(r.sk.point(r.p[0])->y, 0, 0);
  CHECK_NEAR(r.sk.point(r.p[7])->x, 0, 1e-7);
  CHECK_NEAR(r.sk.point(r.p[2])->x, 35, 1e-7);
  CHECK_NEAR(r.sk.point(r.p[4])->x, 35, 1e-7);
  CHECK_NEAR(r.sk.point(r.p[4])->y, 8, 1e-7);
}

// A constraint that a circle can meet by moving or by growing moves it: a tangent to a line 15 mm away from a 2 mm
// circle used to split the gap evenly between the centre, the radius and the line (the radius tripled).
TEST(solve_tangent_moves_circle_rather_than_resizing_it) {
  Sketch sk;
  const int a = sk.add_point(0, 0, true), b = sk.add_point(0, 10, true);
  const int l = sk.add_line(a, b);
  const int c = sk.add_circle(sk.add_point(17, 5), 2);
  sk.add_constraint(CT::Tangent, {l, c});
  const SolveResult res = solve(sk);
  CHECK(res.converged);
  const SkEntity* circle = sk.entity(c);
  CHECK(circle->r < 4);  // was 7 (2 + 15 / 3)
  CHECK_NEAR(std::fabs(sk.point(circle->p[0])->x), circle->r, 1e-7);
}

TEST(solve_under_constrained_line) {
  Sketch sk;
  const int a = sk.add_point(1, 2), b = sk.add_point(7, 10);  // length 10
  const int l = sk.add_line(a, b);
  sk.add_constraint(CT::Distance, {l}, 20);
  const SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 3);
  CHECK_EQ(res.free_points.size(), size_t(2));
  CHECK_NEAR(len(sk, l), 20, 1e-7);
  // nearest solution: both ends move out along the line by the same amount
  CHECK_NEAR((sk.point(a)->x + sk.point(b)->x) / 2, 4, 1e-6);
  CHECK_NEAR((sk.point(a)->y + sk.point(b)->y) / 2, 6, 1e-6);
  CHECK_NEAR(sk.point(a)->x, 4 - 6, 1e-6);
  CHECK_NEAR(sk.point(b)->y, 6 + 8, 1e-6);
}

TEST(solve_triangle_345_and_impossible) {
  Sketch sk;
  const int a = sk.add_point(0, 0, true), b = sk.add_point(3.6, 0.3), c = sk.add_point(0.4, 3.1);
  const int ab = sk.add_line(a, b), bc = sk.add_line(b, c), ca = sk.add_line(c, a);
  const int d1 = sk.add_constraint(CT::Distance, {ab}, 3);
  const int d2 = sk.add_constraint(CT::Distance, {ca}, 4);
  const int d3 = sk.add_constraint(CT::Distance, {bc}, 5);
  SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 1);  // it can still spin about the fixed corner
  CHECK_NEAR(std::fabs(angle(sk, ab, ca)), kPi / 2, 1e-7);
  CHECK(sk.point(c)->y > 0);  // did not mirror

  sk.constraint(d1)->value = 1;
  sk.constraint(d2)->value = 1;
  const opad::json before = sk.to_json();
  res = solve(sk);
  CHECK(!res.converged);
  CHECK(!res.failed.empty());
  CHECK(res.residual > 1e-3);
  for (int id : res.failed) CHECK(id == d1 || id == d2 || id == d3);
  CHECK(sk.to_json() == before);  // restored bit for bit

  Sketch best = sk;
  res = solve(best, {}, true);
  CHECK(!res.converged);
  CHECK(!(best.to_json() == before));  // keep_best leaves the least-bad attempt
  CHECK(std::isfinite(best.point(b)->x) && std::isfinite(best.point(c)->y));
}

TEST(solve_circle_tangent_two_lines) {
  Sketch sk;
  const int o = sk.add_point(0, 0, true), ex = sk.add_point(30, 0, true), ey = sk.add_point(0, 30, true);
  const int lx = sk.add_line(o, ex), ly = sk.add_line(o, ey);
  const int c = sk.add_point(4, 6);
  const int circ = sk.add_circle(c, 5);
  sk.add_constraint(CT::Tangent, {lx, circ});
  sk.add_constraint(CT::Tangent, {circ, ly});  // either order
  sk.add_constraint(CT::Radius, {circ}, 3);
  const SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 0);
  CHECK_NEAR(sk.entity(circ)->r, 3, 1e-7);
  CHECK_NEAR(sk.point(c)->x, 3, 1e-7);  // stayed in its quadrant
  CHECK_NEAR(sk.point(c)->y, 3, 1e-7);
}

TEST(solve_circle_circle_tangent) {
  Sketch sk;
  const int c1 = sk.add_point(0, 0, true), c2 = sk.add_point(11, 1), c3 = sk.add_point(2, 0.5);
  const int big = sk.add_circle(c1, 8), out = sk.add_circle(c2, 4), in = sk.add_circle(c3, 5);
  sk.add_constraint(CT::Fix, {big});
  sk.add_constraint(CT::Radius, {out}, 4);
  sk.add_constraint(CT::Radius, {in}, 5);
  sk.add_constraint(CT::Tangent, {big, out});  // starts outside: stays outside
  sk.add_constraint(CT::Tangent, {in, big});   // starts inside: stays inside
  const SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_NEAR(dist(sk, c1, c2), 12, 1e-7);
  CHECK_NEAR(dist(sk, c1, c3), 3, 1e-7);
  CHECK_NEAR(sk.entity(big)->r, 8, 1e-9);
}

TEST(solve_arc_stays_round_under_drag) {
  Sketch sk;
  const int c = sk.add_point(0, 0), s = sk.add_point(10, 0), e = sk.add_point(0, 10);
  const int arc = sk.add_arc(c, s, e);
  SolveOptions opt;
  opt.drags.push_back({s, 15, 2});
  SolveResult res = solve(sk, opt);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 5);
  CHECK_NEAR(dist(sk, c, s), dist(sk, c, e), 1e-7);
  CHECK_NEAR(sk.point(s)->x, 15, 1e-3);
  CHECK_NEAR(sk.point(s)->y, 2, 1e-3);

  // with a radius dimension and a fixed centre the start can only slide round the circle
  sk.point(c)->fixed = true;
  sk.add_constraint(CT::Radius, {arc}, 12);
  opt.drags = {{s, 0, -30}};
  res = solve(sk, opt);
  CHECK(res.converged);
  CHECK_NEAR(dist(sk, c, s), 12, 1e-7);
  CHECK_NEAR(dist(sk, c, e), 12, 1e-7);
  CHECK(sk.point(s)->y < -11.9);  // went to the point of the circle nearest the mouse
}

TEST(solve_redundant_constraints) {
  Sketch sk;
  const int a = sk.add_point(0, 0), b = sk.add_point(10, 1);
  const int l = sk.add_line(a, b);
  const int h1 = sk.add_constraint(CT::Horizontal, {l});
  const int h2 = sk.add_constraint(CT::Horizontal, {a, b});
  SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 3);
  CHECK_NEAR(sk.point(a)->y, sk.point(b)->y, 1e-8);
  CHECK(has(res.redundant, h2) && !has(res.redundant, h1));

  // closed quadrilateral with four right angles: the fourth follows from the other three
  Sketch q;
  const int p0 = q.add_point(0, 0, true), p1 = q.add_point(10, 0.5), p2 = q.add_point(10.5, 6), p3 = q.add_point(-0.5, 5.5);
  const int e0 = q.add_line(p0, p1), e1 = q.add_line(p1, p2), e2 = q.add_line(p2, p3), e3 = q.add_line(p3, p0);
  q.add_constraint(CT::Perpendicular, {e0, e1});
  q.add_constraint(CT::Perpendicular, {e1, e2});
  q.add_constraint(CT::Perpendicular, {e2, e3});
  const int last = q.add_constraint(CT::Perpendicular, {e3, e0});
  res = solve(q);
  CHECK(res.converged);
  CHECK_NEAR(std::fabs(angle(q, e0, e1)), kPi / 2, 1e-7);
  CHECK_NEAR(std::fabs(angle(q, e3, e0)), kPi / 2, 1e-7);
  CHECK_EQ(res.redundant.size(), size_t(1));
  CHECK(has(res.redundant, last));
  CHECK_EQ(res.dof, 3);  // rotation about the fixed corner, width, height
}

TEST(solve_symmetric_midpoint_collinear) {
  Sketch sk;
  const int a0 = sk.add_point(0, -10, true), a1 = sk.add_point(0, 10, true);
  const int axis = sk.add_line(a0, a1, true);
  const int p = sk.add_point(-4, 3), q = sk.add_point(5, 2);
  sk.add_constraint(CT::Symmetric, {p, q, axis});
  const int m = sk.add_point(3, 3);
  const int pq = sk.add_line(p, q);
  sk.add_constraint(CT::Midpoint, {m, pq});
  const int u = sk.add_point(1, 20), v = sk.add_point(-1, 31);
  const int far = sk.add_line(u, v);
  sk.add_constraint(CT::Collinear, {axis, far});
  const SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_NEAR(sk.point(p)->x, -sk.point(q)->x, 1e-7);
  CHECK_NEAR(sk.point(p)->y, sk.point(q)->y, 1e-7);
  CHECK_NEAR(sk.point(p)->x, -4.5, 1e-6);  // nearest mirror pair
  CHECK_NEAR(sk.point(m)->x, 0, 1e-7);
  CHECK_NEAR(sk.point(m)->y, sk.point(p)->y, 1e-7);
  CHECK_NEAR(sk.point(u)->x, 0, 1e-7);
  CHECK_NEAR(sk.point(v)->x, 0, 1e-7);
  CHECK_NEAR(sk.point(u)->y, 20, 1e-6);  // only moved sideways
  CHECK(has(res.free_points, p) && has(res.free_points, u) && !has(res.free_points, a0));
}

TEST(solve_equal_concentric_on_circle_diameter) {
  Sketch sk;
  const int c1 = sk.add_point(0, 0, true), c2 = sk.add_point(3, -2);
  const int k1 = sk.add_circle(c1, 7), k2 = sk.add_circle(c2, 4);
  sk.add_constraint(CT::Diameter, {k1}, 20);
  sk.add_constraint(CT::Concentric, {k1, k2});
  sk.add_constraint(CT::Equal, {k2, k1});
  const int p = sk.add_point(7, 9);
  sk.add_constraint(CT::Coincident, {p, k1});
  // an arc on the same centre with its start on the circle: equal radius by construction
  const int s = sk.add_point(6, 0), e = sk.add_point(0, 5);
  const int arc = sk.add_arc(c1, s, e);
  sk.add_constraint(CT::Equal, {arc, k2});
  // equal lines, perpendicular
  const int a = sk.add_point(20, 0, true), b = sk.add_point(29, 1), c = sk.add_point(21, 5);
  const int ab = sk.add_line(a, b), ac = sk.add_line(a, c);
  sk.add_constraint(CT::Equal, {ab, ac});
  sk.add_constraint(CT::Perpendicular, {ab, ac});
  sk.add_constraint(CT::Distance, {ab}, 10);
  const SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_NEAR(sk.entity(k1)->r, 10, 1e-7);
  CHECK_NEAR(sk.entity(k2)->r, 10, 1e-7);
  CHECK_NEAR(dist(sk, c1, c2), 0, 1e-7);
  CHECK_NEAR(dist(sk, c1, p), 10, 1e-7);
  CHECK_NEAR(sk.point(p)->x / sk.point(p)->y, 7.0 / 9.0, 1e-6);  // moved radially = the nearest point
  CHECK_NEAR(dist(sk, c1, s), 10, 1e-7);
  CHECK_NEAR(dist(sk, c1, e), 10, 1e-7);
  CHECK_NEAR(len(sk, ac), 10, 1e-7);
  CHECK_NEAR(angle(sk, ab, ac), kPi / 2, 1e-7);  // kept its counter-clockwise turn
}

TEST(solve_angle_keeps_orientation) {
  for (double side : {1.0, -1.0}) {
    Sketch sk;
    const int a = sk.add_point(0, 0, true), b = sk.add_point(10, 0.5), c = sk.add_point(5, side * 7);
    const int ab = sk.add_line(a, b), ac = sk.add_line(a, c);
    sk.add_constraint(CT::Horizontal, {ab});
    sk.add_constraint(CT::Distance, {ab}, 10);
    sk.add_constraint(CT::Distance, {ac}, 10);
    const int ang = sk.add_constraint(CT::Angle, {ab, ac}, kPi / 3);
    SolveResult res = solve(sk);
    CHECK(res.converged);
    CHECK_EQ(res.dof, 0);
    CHECK_NEAR(angle(sk, ab, ac), side * kPi / 3, 1e-8);
    CHECK_NEAR(sk.point(c)->x, 5, 1e-7);
    CHECK_NEAR(sk.point(c)->y, side * 10 * std::sin(kPi / 3), 1e-7);
    // solving again, and opening the angle to 150 deg through pi/2, stays on the same side
    sk.constraint(ang)->value = 150 * kPi / 180;
    res = solve(sk);
    CHECK(res.converged);
    CHECK_NEAR(angle(sk, ab, ac), side * 150 * kPi / 180, 1e-8);
    res = solve(sk);
    CHECK(res.converged);
    CHECK(sk.point(c)->y * side > 0);
  }
}

TEST(solve_distances_keep_their_side) {
  Sketch sk;
  const int o = sk.add_point(0, 0, true), e = sk.add_point(50, 0, true);
  const int base = sk.add_line(o, e);
  const int below = sk.add_point(10, -2), left = sk.add_point(-3, 4);
  sk.add_constraint(CT::Distance, {below, base}, 12);
  sk.add_constraint(CT::HDistance, {o, left}, 8);
  sk.add_constraint(CT::VDistance, {o, left}, 6);
  const int u = sk.add_point(5, -20), v = sk.add_point(40, -21);
  const int par = sk.add_line(u, v);
  sk.add_constraint(CT::Parallel, {base, par});
  sk.add_constraint(CT::Distance, {base, par}, 25);
  const SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_NEAR(sk.point(below)->y, -12, 1e-7);
  CHECK_NEAR(sk.point(below)->x, 10, 1e-7);
  CHECK_NEAR(sk.point(left)->x, -8, 1e-7);
  CHECK_NEAR(sk.point(left)->y, 6, 1e-7);
  CHECK_NEAR(sk.point(u)->y, -25, 1e-7);
  CHECK_NEAR(sk.point(v)->y, -25, 1e-7);
}

TEST(solve_fix_constraint_and_fixed_entity) {
  Sketch sk;
  const int a = sk.add_point(1, 1), b = sk.add_point(9, 2);
  const int l = sk.add_line(a, b);
  sk.add_constraint(CT::Fix, {a});
  sk.add_constraint(CT::Distance, {l}, 20);
  SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 1);
  CHECK_NEAR(sk.point(a)->x, 1, 1e-9);
  CHECK_NEAR(sk.point(a)->y, 1, 1e-9);
  CHECK_NEAR(len(sk, l), 20, 1e-7);
  CHECK(has(res.free_points, b) && !has(res.free_points, a));

  sk.entity(l)->fixed = true;  // now nothing can move and the dimension cannot be met
  res = solve(sk);
  CHECK_EQ(res.dof, 0);
  CHECK(res.converged);  // it already holds
  sk.constraints.back().value = 5;
  res = solve(sk);
  CHECK(!res.converged);
  CHECK(has(res.failed, sk.constraints.back().id));
  CHECK_NEAR(len(sk, l), 20, 1e-7);
}

TEST(solve_drag_fully_constrained_does_not_move) {
  Rect r = make_rect(20, 8);
  CHECK(solve(r.sk).converged);
  const opad::json before = r.sk.to_json();
  SolveOptions opt;
  opt.drags.push_back({r.p[3], 80, -40});
  opt.drags.push_back({r.p[0], 5, 5});  // a fixed point ignores the mouse
  const SolveResult res = solve(r.sk, opt);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 0);
  for (int i = 0; i < 8; ++i) {
    CHECK_NEAR(r.sk.point(r.p[i])->x, before["points"][size_t(i)]["x"].get<double>(), 1e-6);
    CHECK_NEAR(r.sk.point(r.p[i])->y, before["points"][size_t(i)]["y"].get<double>(), 1e-6);
  }
}

TEST(solve_drag_free_rectangle_corner) {
  Sketch sk;
  const int p0 = sk.add_point(0, 0), p1 = sk.add_point(10, 0), p2 = sk.add_point(10, 6), p3 = sk.add_point(0, 6);
  const int b = sk.add_line(p0, p1), r = sk.add_line(p1, p2), t = sk.add_line(p2, p3), l = sk.add_line(p3, p0);
  sk.add_constraint(CT::Horizontal, {b});
  sk.add_constraint(CT::Horizontal, {t});
  sk.add_constraint(CT::Vertical, {r});
  sk.add_constraint(CT::Vertical, {l});
  SolveOptions opt;
  opt.drags.push_back({p2, 14, 9});
  const SolveResult res = solve(sk, opt);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 4);
  CHECK_NEAR(sk.point(p2)->x, 14, 1e-3);
  CHECK_NEAR(sk.point(p2)->y, 9, 1e-3);
  CHECK_NEAR(sk.point(p1)->x, sk.point(p2)->x, 1e-8);
  CHECK_NEAR(sk.point(p3)->y, sk.point(p2)->y, 1e-8);
  CHECK_NEAR(sk.point(p1)->y, sk.point(p0)->y, 1e-8);
  CHECK_NEAR(sk.point(p3)->x, sk.point(p0)->x, 1e-8);
  CHECK_NEAR(sk.point(p0)->x, 0, 1e-3);  // the far corner had no reason to move
  CHECK_NEAR(sk.point(p0)->y, 0, 1e-3);
}

TEST(solve_zigzag_chain_is_fast) {
  Sketch sk;
  const int n = 40;
  std::vector<int> pts, lines;
  for (int i = 0; i <= n; ++i) pts.push_back(sk.add_point(i * 6.0 + (i % 3) * 0.7, (i % 2) * 7.0 + (i % 5) * 0.4, i == 0));
  for (int i = 0; i < n; ++i) lines.push_back(sk.add_line(pts[size_t(i)], pts[size_t(i) + 1]));
  for (int i = 0; i < n; ++i) sk.add_constraint(CT::Distance, {lines[size_t(i)]}, 10);
  for (int i = 0; i + 1 < n; ++i) sk.add_constraint(CT::Angle, {lines[size_t(i)], lines[size_t(i) + 1]}, 2 * kPi / 3);
  sk.add_constraint(CT::Angle, {lines[0], lines[1]}, 2 * kPi / 3);  // a duplicate on top
  const int first = sk.add_constraint(CT::Distance, {pts[0], pts[1]}, 10);
  sk.add_constraint(CT::VDistance, {pts[0], pts[1]}, 10 * std::sin(kPi / 3));

  const auto t0 = std::chrono::steady_clock::now();
  const SolveResult res = solve(sk);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("       zig-zag of %d lines: %.1f ms, residual %.2e\n", n, ms, res.residual);
  CHECK(res.converged);
  CHECK(ms < 500);
  CHECK_EQ(res.dof, 0);
  CHECK_EQ(res.redundant.size(), size_t(2));
  CHECK(has(res.redundant, first));
  for (int i = 0; i < n; ++i) CHECK_NEAR(len(sk, lines[size_t(i)]), 10, 1e-7);
  for (int i = 0; i + 1 < n; ++i)
    CHECK_NEAR(angle(sk, lines[size_t(i)], lines[size_t(i) + 1]), (i % 2 ? 1 : -1) * 2 * kPi / 3, 1e-7);
  CHECK_NEAR(sk.point(pts[size_t(n)])->x, n * 5.0, 1e-5);  // every zig and zag advances 10 cos 60
}

// ---------------------------------------------------------------- adversarial
TEST(solve_far_from_the_solution) {
  Rect r = make_rect(20, 8);
  const double wild[8][2] = {{0, 0}, {300, -120}, {-80, 40}, {15, 900}, {-400, 35}, {7, -60}, {55, 55}, {-9, 250}};
  for (int i = 1; i < 8; ++i) {
    r.sk.point(r.p[i])->x = wild[i][0];
    r.sk.point(r.p[i])->y = wild[i][1];
  }
  const SolveResult res = solve(r.sk);
  CHECK(res.converged);
  CHECK_EQ(res.dof, 0);
  CHECK_NEAR(len(r.sk, r.l[0]), 20, 1e-7);
  CHECK_NEAR(len(r.sk, r.l[1]), 8, 1e-7);
  CHECK_NEAR(off_line(r.sk, r.p[4], r.l[1]), 0, 1e-7);
}

TEST(solve_degenerate_input_has_no_nan) {
  // every point on one spot: zero-length lines under direction and length constraints
  Sketch sk;
  const int a = sk.add_point(5, 5), b = sk.add_point(5, 5), c = sk.add_point(5, 5), d = sk.add_point(5, 5);
  const int ab = sk.add_line(a, b), cd = sk.add_line(c, d);
  sk.add_constraint(CT::Parallel, {ab, cd});
  sk.add_constraint(CT::Angle, {ab, cd}, 0.5);
  sk.add_constraint(CT::Perpendicular, {cd, ab});
  sk.add_constraint(CT::Coincident, {c, ab});
  sk.add_constraint(CT::Symmetric, {a, d, ab});
  sk.add_constraint(CT::Distance, {c, ab}, 0);
  SolveResult res = solve(sk);
  CHECK(std::isfinite(res.residual));
  for (const auto& p : sk.points) CHECK(std::isfinite(p.x) && std::isfinite(p.y));

  // a length on a zero-length line pulls it apart instead of dividing by zero
  Sketch z;
  const int p = z.add_point(1, 1, true), q = z.add_point(1, 1);
  const int pq = z.add_line(p, q);
  z.add_constraint(CT::Distance, {pq}, 10);
  res = solve(z);
  CHECK(res.converged);
  CHECK_NEAR(len(z, pq), 10, 1e-7);

  // circle squeezed onto a point by a zero radius dimension: fails cleanly
  Sketch k;
  const int kc = k.add_point(0, 0);
  const int circ = k.add_circle(kc, 5);
  k.add_constraint(CT::Radius, {circ}, 0);
  res = solve(k);
  CHECK(!res.converged);
  CHECK_NEAR(k.entity(circ)->r, 5, 0);
}

TEST(solve_nearly_parallel_lines) {
  // the intersection of two lines 1e-4 rad apart: ill-conditioned but legitimate
  Sketch sk;
  const int a = sk.add_point(0, 0, true), b = sk.add_point(100, 0, true);
  const int c = sk.add_point(0, 1, true), d = sk.add_point(100, 0.99, true);
  const int l1 = sk.add_line(a, b), l2 = sk.add_line(c, d);
  const int p = sk.add_point(50, 3);
  sk.add_constraint(CT::Coincident, {p, l1});
  sk.add_constraint(CT::Coincident, {p, l2});
  SolveResult res = solve(sk);
  CHECK(res.converged);
  CHECK_NEAR(sk.point(p)->x, 10000, 1e-3);
  CHECK_NEAR(sk.point(p)->y, 0, 1e-7);

  // truly parallel and apart: no intersection, no divergence
  sk.point(d)->y = 1;
  sk.point(p)->x = 50;
  sk.point(p)->y = 3;
  res = solve(sk);
  CHECK(!res.converged);
  CHECK(!res.failed.empty());
  CHECK_NEAR(sk.point(p)->x, 50, 0);
  CHECK_NEAR(sk.point(p)->y, 3, 0);
}

TEST(solve_is_deterministic) {
  Rect r1 = make_rect(33, 12), r2 = make_rect(33, 12);
  SolveOptions opt;
  opt.drags.push_back({r1.p[3], 3, 3});
  solve(r1.sk, opt);
  solve(r2.sk, opt);
  CHECK_EQ(r1.sk.to_json().dump(), r2.sk.to_json().dump());
}

TEST(solve_rejects_a_malformed_sketch) {
  Sketch sk;
  const int a = sk.add_point(0, 0), b = sk.add_point(1, 0);
  sk.add_line(a, b);
  SkConstraint c;
  c.id = sk.next_id();
  c.type = CT::Radius;
  c.refs = {a};
  sk.constraints.push_back(c);
  CHECK_THROWS(solve(sk));
}

TEST(solve_drag_rotates_a_crank) {
  // fixed pivot + length: the free end can only go round; the mouse is far away and on the other side
  Sketch sk;
  const int o = sk.add_point(0, 0, true), e = sk.add_point(10, 0);
  const int l = sk.add_line(o, e);
  sk.add_constraint(CT::Distance, {l}, 10);
  SolveOptions opt;
  opt.drags.push_back({e, -300, 400});
  SolveResult res;
  for (int i = 0; i < 4; ++i) res = solve(sk, opt);  // a few mouse events at the same spot
  CHECK(res.converged);
  CHECK_EQ(res.dof, 1);
  CHECK_NEAR(len(sk, l), 10, 1e-7);
  CHECK_NEAR(sk.point(e)->x, -6, 1e-3);
  CHECK_NEAR(sk.point(e)->y, 8, 1e-3);
}

TEST(solve_drag_does_not_mirror_a_rigid_triangle) {
  Sketch sk;
  const int a = sk.add_point(0, 0, true), b = sk.add_point(3, 0), c = sk.add_point(0, 4);
  const int ab = sk.add_line(a, b), bc = sk.add_line(b, c), ca = sk.add_line(c, a);
  sk.add_constraint(CT::Distance, {ab}, 3);
  sk.add_constraint(CT::Distance, {bc}, 5);
  sk.add_constraint(CT::Distance, {ca}, 4);
  SolveOptions opt;
  opt.drags.push_back({c, 1, -50});  // straight through the opposite side
  const double handed = angle(sk, ab, ca);
  SolveResult res;
  for (int i = 0; i < 3; ++i) res = solve(sk, opt);
  CHECK(res.converged);
  CHECK_NEAR(len(sk, ab), 3, 1e-7);
  CHECK_NEAR(len(sk, bc), 5, 1e-7);
  CHECK_NEAR(len(sk, ca), 4, 1e-7);
  CHECK_NEAR(angle(sk, ab, ca), handed, 1e-7);  // same handedness: a rotation, not a reflection
  CHECK_NEAR(sk.point(c)->x, 4 * 1 / std::hypot(1, 50), 1e-3);  // and it did follow the mouse: nearest point of its circle
  CHECK(sk.point(c)->y < -3.99);
}

TEST(solve_drag_chain_with_many_freedoms) {
  Sketch sk;
  const int n = 60;
  std::vector<int> pts, lines;
  for (int i = 0; i <= n; ++i) pts.push_back(sk.add_point(i * 5.0, (i % 2) * 5.0, i == 0));
  for (int i = 0; i < n; ++i) lines.push_back(sk.add_line(pts[size_t(i)], pts[size_t(i) + 1]));
  for (int i = 0; i < n; ++i) sk.add_constraint(CT::Distance, {lines[size_t(i)]}, std::sqrt(50.0));
  SolveOptions opt;
  opt.drags.push_back({pts[size_t(n)], n * 5.0 - 20, 30});
  const auto t0 = std::chrono::steady_clock::now();
  SolveResult res = solve(sk, opt);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("       drag on a %d-link chain: %.1f ms\n", n, ms);
  CHECK(res.converged);
  CHECK(ms < 500);
  CHECK(std::hypot(sk.point(pts[size_t(n)])->x - (n * 5.0 - 20), sk.point(pts[size_t(n)])->y - 30) < 10);  // most of the way at once
  for (int i = 0; i < 3; ++i) res = solve(sk, opt);  // the mouse rests: the following events close the gap
  CHECK(res.converged);
  CHECK_EQ(res.dof, n);
  for (int i = 0; i < n; ++i) CHECK_NEAR(len(sk, lines[size_t(i)]), std::sqrt(50.0), 1e-7);
  CHECK_NEAR(sk.point(pts[size_t(n)])->x, n * 5.0 - 20, 1e-2);
  CHECK_NEAR(sk.point(pts[size_t(n)])->y, 30, 1e-2);
}

TEST(solve_is_scale_independent) {
  // the same figure in micrometres and in hundreds of metres
  for (double k : {1e-3, 1.0, 1e5}) {
    Sketch sk;
    const int a = sk.add_point(0, 0, true), b = sk.add_point(9 * k, 1 * k), c = sk.add_point(8 * k, 7 * k), d = sk.add_point(-1 * k, 5 * k);
    const int ab = sk.add_line(a, b), bc = sk.add_line(b, c), cd = sk.add_line(c, d), da = sk.add_line(d, a);
    sk.add_constraint(CT::Horizontal, {ab});
    sk.add_constraint(CT::Perpendicular, {ab, bc});
    sk.add_constraint(CT::Parallel, {ab, cd});
    sk.add_constraint(CT::Angle, {da, ab}, kPi / 3);
    sk.add_constraint(CT::Distance, {ab}, 10 * k);
    sk.add_constraint(CT::Distance, {bc}, 6 * k);
    SolveOptions opt;
    opt.tolerance = 1e-8 * std::max(1.0, k);
    const SolveResult res = solve(sk, opt);
    CHECK(res.converged);
    CHECK_EQ(res.dof, 0);
    CHECK_NEAR(sk.point(c)->x / k, 10, 1e-6);
    CHECK_NEAR(sk.point(c)->y / k, 6, 1e-6);
    CHECK_NEAR(sk.point(d)->y / k, 6, 1e-6);
    CHECK_NEAR(sk.point(d)->x / k, -6 / std::tan(kPi / 3), 1e-6);  // da turns +60 deg into ab: d is up and to the left
  }
}

TEST(solve_conflicting_constraints_are_named) {
  Sketch sk;
  const int a = sk.add_point(0, 0, true), b = sk.add_point(10, 3);
  const int l = sk.add_line(a, b);
  const int h = sk.add_constraint(CT::Horizontal, {l});
  const int v = sk.add_constraint(CT::VDistance, {a, b}, 5);
  const int d = sk.add_constraint(CT::Distance, {l}, 10);
  const SolveResult res = solve(sk);
  CHECK(!res.converged);
  CHECK(has(res.failed, h) || has(res.failed, v));
  CHECK(std::isfinite(res.residual));
  CHECK_NEAR(sk.point(b)->y, 3, 0);
  (void)d;
}

TEST(sketch_deleted_ids_are_not_recycled) {
  Sketch sk;
  const int a=sk.add_point(0,0), b=sk.add_point(2,0), line=sk.add_line(a,b);
  sk.remove(line);
  sk=Sketch::from_json(sk.to_json());
  CHECK(sk.add_point(1,1)>line);
}

TEST(sketch_deltas_are_id_based_and_atomic) {
  Sketch sk;
  const int a=sk.add_point(0,0), b=sk.add_point(2,0), line=sk.add_line(a,b);
  auto before=sk.to_json();
  sk.point(b)->x=8;
  const auto delta=sketch_delta(before,sk.to_json());
  CHECK_EQ(delta.size(),size_t(1));
  CHECK_EQ(delta["points"].size(),size_t(1));
  CHECK_EQ(delta["points"][0]["id"].get<int>(),b);
  CHECK(apply_sketch_delta(before,delta)==sk.to_json());
  CHECK_THROWS(apply_sketch_delta(before,opad::json{{"points",opad::json::array({{{"id",a},{"deleted",true}}})}}));
  sk.remove(line);
  CHECK(apply_sketch_delta(before,sketch_delta(before,sk.to_json()))==sk.to_json());
}

TEST(sketch_reference_and_arc_length_dimensions) {
  Sketch sk;
  int o=sk.add_point(0,0,true), a=sk.add_point(5,0,true), b=sk.add_point(0,5);
  int arc=sk.add_arc(o,a,b), length=sk.add_constraint(CT::ArcLength,{arc},5*kPi/3);
  int radial=sk.add_constraint(CT::Radius,{arc},99);
  sk.constraint(radial)->reference=true;
  auto r=solve(sk);
  CHECK(r.converged);
  CHECK_NEAR(dimension_value(sk,*sk.constraint(length)),5*kPi/3,1e-7);
  CHECK_NEAR(sk.constraint(radial)->value,5,1e-10);
  CHECK(Sketch::from_json(sk.to_json()).constraint(radial)->reference);
}

TEST(sketch_dimension_expression_dependencies) {
  Sketch sk;
  int o=sk.add_point(0,0,true), a=sk.add_point(5,0), b=sk.add_point(0,10);
  int d=sk.add_constraint(CT::Distance,{o,a},5,"wall");
  int e=sk.add_constraint(CT::Distance,{o,b},10,"d"+std::to_string(d)+" * 2 + 1 cm");
  ParamTable params({{"wall","wall","3 mm",""}});
  evaluate_dimensions(sk,params);
  CHECK_NEAR(sk.constraint(e)->value,16,1e-10);
  sk.constraint(d)->expr="d"+std::to_string(e);
  CHECK_THROWS(evaluate_dimensions(sk,params));
}

TEST(sketch_hundreds_of_independent_curves_drag_interactively) {
  Sketch sk;
  int point=0;
  for(int i=0;i<400;++i) {
    int a=sk.add_point(i*20,0), b=sk.add_point(i*20+10,0);
    int l=sk.add_line(a,b); sk.add_constraint(CT::Distance,{l},10); point=b;
  }
  SolveOptions options; options.drags.push_back({point,8000,10});
  const auto start=std::chrono::steady_clock::now();
  auto r=solve(sk,options);
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  std::printf("       400-curve partitioned drag: %.1f ms\n",ms);
  CHECK(r.converged); CHECK(ms<250);
  CHECK_NEAR(sk.point(1)->x,0,0);
}

TEST(sketch_spline_continuity_is_stable_and_serialized) {
  Sketch sk;
  auto spline=[&](std::initializer_list<std::pair<double,double>> points) {
    SkEntity e;e.type=SkEntity::Type::Spline;e.degree=3;e.knots={0,1};e.multiplicities={4,4};
    for(auto [x,y]:points){e.p.push_back(sk.add_point(x,y));e.weights.push_back(1);}
    e.id=sk.next_id();sk.entities.push_back(e);return e.id;
  };
  int a=spline({{0,0},{2,3},{5,0},{8,0}}),b=spline({{8.2,.1},{10,1},{13,2},{16,0}});
  const int join=sk.add_constraint(CT::Smooth,{a,b});
  CHECK_EQ(sk.constraint(join)->anchors.front(),sk.entity(a)->p.back());
  CHECK(solve(sk).converged);
  auto jet=[&](int id,bool reverse) {
    auto e=*sk.entity(id);if(reverse)std::reverse(e.p.begin(),e.p.end());
    const auto p=*sk.point(e.p[0]),q=*sk.point(e.p[1]),r=*sk.point(e.p[2]);
    double dx=3*(q.x-p.x),dy=3*(q.y-p.y),ddx=6*(r.x-2*q.x+p.x),ddy=6*(r.y-2*q.y+p.y);
    return std::array<double,5>{p.x,p.y,dx,dy,(dx*ddy-dy*ddx)/std::pow(std::hypot(dx,dy),3)};
  };
  auto j=jet(a,true),k=jet(b,false);
  CHECK_NEAR(j[0],k[0],1e-7);CHECK_NEAR(j[1],k[1],1e-7);
  CHECK_NEAR(j[2]*k[3]-j[3]*k[2],0,1e-6);CHECK(j[2]*k[2]+j[3]*k[3]<0);
  CHECK_NEAR(j[4]+k[4],0,1e-7);
  const auto saved=sk.to_json();CHECK(Sketch::from_json(saved).to_json()==saved);
  sk.remove(a);CHECK(!sk.constraint(join));
}

TEST(sketch_line_spline_tangent_preserves_endpoint_on_line) {
  Sketch sk;SkEntity e;e.type=SkEntity::Type::Spline;e.degree=2;e.knots={0,1};e.multiplicities={3,3};e.weights={1,.7,1};
  e.p={sk.add_point(0,.1),sk.add_point(4,1),sk.add_point(8,4)};e.id=sk.next_id();sk.entities.push_back(e);
  int a=sk.add_point(-5,0,true),b=sk.add_point(5,0,true),line=sk.add_line(a,b);
  sk.add_constraint(CT::Tangent,{line,e.id});CHECK(solve(sk).converged);
  CHECK_NEAR(sk.point(e.p[0])->y,0,1e-7);CHECK_NEAR(sk.point(e.p[1])->y,0,1e-7);
}

TEST(reference_dimensions_cannot_indirectly_drive_geometry) {
  Sketch sk;int a=sk.add_point(0,0),b=sk.add_point(10,0),c=sk.add_point(0,20);
  int r=sk.add_constraint(CT::Distance,{a,b},10),d=sk.add_constraint(CT::Distance,{a,c},20,"indirect");sk.constraint(r)->reference=true;
  ParamTable params({{"parameter","indirect","d"+std::to_string(r)+" * 2",""}});
  CHECK_THROWS(evaluate_dimensions(sk,params));(void)d;
}

// Gap log #11: hdistance/vdistance were sizes only (a negative expression flipped the point back). signed: true
// drives q - p with its sign; one point is its coordinate from the sketch origin, signed: a "fix at (x, y)".
TEST(signed_and_coordinate_dimensions) {
  Sketch sk;
  const int p = sk.add_point(0, 0), q = sk.add_point(3, 1), r = sk.add_point(5, 5);
  sk.add_constraint(CT::Fix, {p});
  const int h = sk.add_constraint(CT::HDistance, {p, q}, -5);
  sk.constraint(h)->is_signed = true;
  const int x = sk.add_constraint(CT::HDistance, {r}, -7), y = sk.add_constraint(CT::VDistance, {r}, 3);
  CHECK(solve(sk).converged);
  CHECK_NEAR(sk.point(q)->x, -5, 1e-9);
  CHECK_NEAR(sk.point(r)->x, -7, 1e-9);
  CHECK_NEAR(sk.point(r)->y, 3, 1e-9);
  CHECK_NEAR(dimension_value(sk, *sk.constraint(h)), -5, 1e-9);
  CHECK_NEAR(dimension_value(sk, *sk.constraint(x)), -7, 1e-9);
  CHECK_NEAR(dimension_value(sk, *sk.constraint(y)), 3, 1e-9);
  const opad::json saved = sk.to_json();
  CHECK(Sketch::from_json(saved).constraint(h)->is_signed);
  CHECK(Sketch::from_json(saved).to_json() == saved);
  // Unsigned as before: the size, the side kept.
  Sketch old;
  const int a = old.add_point(0, 0), b = old.add_point(-2, 0);
  old.add_constraint(CT::Fix, {a});
  old.add_constraint(CT::HDistance, {a, b}, 4);
  CHECK(solve(old).converged);
  CHECK_NEAR(old.point(b)->x, -4, 1e-9);
}

CHECK_MAIN()
