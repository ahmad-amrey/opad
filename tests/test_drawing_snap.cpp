// Snaps on a 2D drawing (TODO 11 UI-78): ends, midpoints, centres, quadrants, crossings and the nearest point of curves.
#include <chrono>
#include <cmath>

#include "check.hpp"
#include "opad/drawing/snap.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {
bool near(Vec2 a, Vec2 b, double tol = 1e-6) { return std::hypot(a[0] - b[0], a[1] - b[1]) <= tol; }
}  // namespace

TEST(snaps_on_curves) {
  Display d;
  const int ink = d.layer({"Visible", kInk, LineType::Continuous, 0.5});
  d.polyline(ink, {{0, 0}, {10, 0}, {10, 5}, {0, 5}}, true);
  d.circle(ink, {20, 0}, 3);
  d.arc(ink, {40, 0}, 5, 0, M_PI / 2);
  d.line(ink, {30, -4}, {36, 8});
  d.line(ink, {30, 6}, {36, -6});
  Curve e;
  e.type = Curve::Type::Ellipse;
  e.c = {60, 0}, e.r1 = 4, e.r2 = 2, e.rot = M_PI / 2, e.a0 = 0, e.a1 = 2 * M_PI;
  d.curve(ink, e);
  d.text(ink, "TEXT", {0, 20}, 5);  // text gives no snaps
  for (auto& p : d.prims) p.source = "view-1";
  const SnapIndex index(d);
  CHECK(index.points() > 10 && index.segments() > 10);
  const auto at = [&](Vec2 p, double r = 0.6, unsigned kinds = kAllSnaps) { return index.find(p, r, kinds); };
  auto s = at({10.1, 0.05});
  CHECK(s && s->kind == SnapKind::End && near(s->at, {10, 0}) && s->source == "view-1");
  s = at({5, 0.1});
  CHECK(s && s->kind == SnapKind::Mid && near(s->at, {5, 0}));
  s = at({20.1, 0.1});
  CHECK(s && s->kind == SnapKind::Centre && near(s->at, {20, 0}));
  s = at({23.1, 0.2});
  CHECK(s && s->kind == SnapKind::Quadrant && near(s->at, {23, 0}));
  s = at({32.6, 1.1});
  CHECK(s && s->kind == SnapKind::Intersection && near(s->at, {32.5, 1}, 1e-9));
  s = at({2.5, 4.9});  // nothing of note close: the nearest point of the edge
  CHECK(s && s->kind == SnapKind::Nearest && near(s->at, {2.5, 5}));
  CHECK(!at({2.5, 4.9}, 0.6, kAllSnaps & ~snap_bit(SnapKind::Nearest)));
  s = at({10.1, 0.05}, 0.6, snap_bit(SnapKind::Mid) | snap_bit(SnapKind::Nearest));  // ends off: the edge
  CHECK(s && s->kind == SnapKind::Nearest);
  // An arc: its ends, middle and the quadrants it passes; an ellipse turned a quarter: its major axis upright.
  s = at({45.1, 0.1});
  CHECK(s && near(s->at, {45, 0}) && (s->kind == SnapKind::End || s->kind == SnapKind::Quadrant));
  s = at({40 + 5 / std::sqrt(2.0), 5 / std::sqrt(2.0) + 0.1});
  CHECK(s && s->kind == SnapKind::Mid && near(s->at, {40 + 5 / std::sqrt(2.0), 5 / std::sqrt(2.0)}));
  s = at({60.1, 4.1});
  CHECK(s && s->kind == SnapKind::Quadrant && near(s->at, {60, 4}));
  s = at({62.1, 0.1});
  CHECK(s && s->kind == SnapKind::Quadrant && near(s->at, {62, 0}));
  CHECK(!at({100, 100}));
  CHECK(!at({0, 20.5}));  // the text
  CHECK(!SnapIndex().find({0, 0}, 1));
  CHECK_EQ(std::string(snap_kind_name(SnapKind::Intersection)), "intersection");
}

TEST(snaps_stay_quick_on_big_drawings) {
  // 40,000 short lines (a dense view): a query looks at a few cells.
  Display d;
  const int ink = d.layer({"Visible", kInk, LineType::Continuous, 0.5});
  for (int i = 0; i < 200; ++i)
    for (int j = 0; j < 200; ++j) d.line(ink, {i * 2.0, j * 2.0}, {i * 2.0 + 1.5, j * 2.0});
  const SnapIndex index(d);
  const auto start = std::chrono::steady_clock::now();
  int found = 0;
  for (int k = 0; k < 1000; ++k) found += index.find({(k % 200) * 2.0 + 0.1, (k / 5 % 200) * 2.0 + 0.1}, 0.5).has_value();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("1000 snap queries on 40,000 lines: %.1f ms\n", ms);
  CHECK_EQ(found, 1000);
  CHECK(ms < 500);
}

CHECK_MAIN()
