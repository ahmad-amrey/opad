// The view tracker's pure parts (UI-32 cross lock): lines met in space, the crossings of a locked line with the other
// anchors, the pointer's reach on screen and the Shift key's lock states. The viewport side is the crosslock bench.
#include "Tracking.hpp"
#include "check.hpp"

#include <algorithm>

using tracking::Line;
using R = tracking::ShiftLock::Result;

namespace {
bool has(const std::vector<tracking::Crossing>& all, const gp_Pnt& p, const gp_Pnt& from) {
  return std::any_of(all.begin(), all.end(), [&](const auto& c) { return c.point.Distance(p) < 1e-9 && c.from.Distance(from) < 1e-9; });
}
}  // namespace

TEST(lines_meet_in_space_only) {
  gp_Pnt at;
  CHECK(tracking::meet({gp_Pnt(0, 0, 5), gp_Vec(1, 0, 0)}, {gp_Pnt(7, 3, 5), gp_Vec(0, 1, 0)}, at));
  CHECK(at.Distance(gp_Pnt(7, 0, 5)) < 1e-9);
  CHECK(!tracking::meet({gp_Pnt(0, 0, 5), gp_Vec(1, 0, 0)}, {gp_Pnt(7, 3, 6), gp_Vec(0, 1, 0)}, at));  // skew: crosses on screen only
  CHECK(!tracking::meet({gp_Pnt(0, 0, 0), gp_Vec(1, 0, 0)}, {gp_Pnt(0, 1, 0), gp_Vec(1, 0, 0)}, at));  // parallel
}

TEST(locked_line_lines_up_with_other_anchors) {
  const Line x{gp_Pnt(0, 0, 0), gp_Vec(1, 0, 0)};
  // Another anchor anywhere: the point of the line with its x (the others' y and z would need the line to run along them).
  auto c = tracking::crossings(x, {gp_Pnt(0, 0, 0), gp_Pnt(30, 20, 10)}, {});
  CHECK(c.size() == 1 && has(c, gp_Pnt(30, 0, 0), gp_Pnt(30, 20, 10)));
  // A diagonal line: the other anchor's x and its y; an axis it barely runs along (0.1 per unit or less) gives nothing.
  const double r = 1 / std::sqrt(2.0);
  c = tracking::crossings({gp_Pnt(0, 0, 0), gp_Vec(r, r, 0)}, {gp_Pnt(10, 4, 3)}, {});
  CHECK(c.size() == 2 && has(c, gp_Pnt(10, 10, 0), gp_Pnt(10, 4, 3)) && has(c, gp_Pnt(4, 4, 0), gp_Pnt(10, 4, 3)));
  const gp_Vec shallow = gp_Vec(1, 0.05, 0).Normalized();
  c = tracking::crossings({gp_Pnt(0, 0, 0), shallow}, {gp_Pnt(20, 9, 0)}, {});
  CHECK(c.size() == 1 && std::abs(c[0].point.X() - 20) < 1e-9);
  // Another anchor's own line (an edge's direction) where it meets the locked line in space, after the coordinate planes.
  c = tracking::crossings(x, {gp_Pnt(20, 10, 0)}, {{gp_Pnt(20, 10, 0), gp_Vec(r, r, 0)}, {gp_Pnt(20, 10, 1), gp_Vec(r, r, 0)}});
  CHECK(c.size() == 2 && has(c, gp_Pnt(20, 0, 0), gp_Pnt(20, 10, 0)) && has(c, gp_Pnt(10, 0, 0), gp_Pnt(20, 10, 0)));
  // The locked line's own anchor and its own lines give nothing; the same point twice counts once (the first source).
  c = tracking::crossings(x, {gp_Pnt(0, 0, 0)}, {{gp_Pnt(0, 0, 0), gp_Vec(0, 1, 0)}});
  CHECK(c.empty());
  c = tracking::crossings(x, {gp_Pnt(30, 20, 0)}, {{gp_Pnt(30, 20, 0), gp_Vec(0, 1, 0)}, {gp_Pnt(30, 20, 0), gp_Vec(0, 0, 1)}});
  CHECK(c.size() == 1 && has(c, gp_Pnt(30, 0, 0), gp_Pnt(30, 20, 0)));
  // An anchor on the locked line is a crossing at itself (its guide has no length).
  c = tracking::crossings(x, {gp_Pnt(12, 0, 0)}, {});
  CHECK(c.size() == 1 && has(c, gp_Pnt(12, 0, 0), gp_Pnt(12, 0, 0)));
}

TEST(reach_is_the_distance_from_the_second_guide) {
  // Screen: the locked line runs right along y = 100; the other anchor is at (300, 20), its guide straight down to (300, 100).
  const gp_XY along(100, 0), from(300, 20), at(300, 100);
  CHECK(std::abs(tracking::reach(gp_XY(306, 100), gp_XY(306, 100), along, from, at) - 6) < 1e-9);
  CHECK(std::abs(tracking::reach(gp_XY(297, 40), gp_XY(297, 100), along, from, at) - 3) < 1e-9);  // following the second guide
  CHECK(tracking::reach(gp_XY(250, 100), gp_XY(250, 100), along, from, at) > 10);
  // A guide within 15 degrees of the locked line, or seen end-on: how far the locked point is from the crossing.
  CHECK(std::abs(tracking::reach(gp_XY(0, 0), gp_XY(304, 100), along, gp_XY(100, 80), at) - 4) < 1e-9);
  CHECK(std::abs(tracking::reach(gp_XY(0, 0), gp_XY(300, 107), along, at, at) - 7) < 1e-9);
}

TEST(shift_hold_locks_while_held) {
  tracking::ShiftLock k;
  CHECK(k.press(0, 0) == R::Ignored && !k.held());  // nothing offered
  CHECK(k.release(50, 0, 0) == R::Ignored);
  CHECK(k.press(1000, 2) == R::Locked && k.locked() && k.held() && !k.sticky());
  CHECK(k.press(1100, 2) == R::Ignored);  // a repeat while held
  CHECK(k.release(1400, 2, 0) == R::Unlocked && !k.locked() && !k.held());
  // Esc while held unlocks; the release then does nothing.
  CHECK(k.press(3000, 1) == R::Locked && k.escape() && !k.locked());
  CHECK(k.release(3400, 1, 0) == R::Ignored && !k.escape());
  // The application loses the keyboard while held: the lock ends.
  CHECK(k.press(5000, 1) == R::Locked);
  k.deactivate();
  CHECK(!k.locked() && !k.held() && k.release(5100, 1, 0) == R::Ignored);
}

TEST(shift_tap_locks_one_guide_until_a_click_or_esc) {
  tracking::ShiftLock k;
  CHECK(k.press(0, 1) == R::Locked && k.release(100, 1, 0) == R::Stuck && k.locked() && k.sticky());
  CHECK(k.press(200, 0) == R::Kept && k.release(260, 0, 0) == R::Kept && k.sticky());  // a double tap out of habit keeps it
  CHECK(k.press(1000, 0) == R::Kept && k.release(1500, 0, 0) == R::Kept && k.sticky());  // a hold keeps it
  k.deactivate();
  CHECK(k.sticky() && k.locked());  // a sticky lock outlives switching windows
  CHECK(k.press(2000, 0) == R::Kept && k.release(2080, 0, 3) == R::NextCrossing && k.sticky());  // several crossings in reach
  CHECK(k.press(2200, 0) == R::Kept && k.release(2280, 0, 2) == R::NextCrossing);  // cycling taps are never a double tap
  CHECK(k.press(3000, 0) == R::Kept && k.release(3080, 0, 1) == R::Unlocked && !k.locked() && !k.sticky());
  // A click ends it, Esc ends it.
  CHECK(k.press(4000, 1) == R::Locked && k.release(4050, 1, 0) == R::Stuck);
  k.clicked();
  CHECK(!k.locked());
  CHECK(k.press(5000, 1) == R::Locked && k.release(5050, 1, 0) == R::Stuck && k.escape() && !k.locked() && !k.sticky());
  // A click while held keeps the hold.
  CHECK(k.press(6000, 2) == R::Locked);
  k.clicked();
  CHECK(k.locked() && k.release(6500, 2, 0) == R::Unlocked);
}

TEST(shift_taps_cycle_several_guides_and_a_double_tap_locks) {
  tracking::ShiftLock k;
  CHECK(k.press(0, 3) == R::Locked && k.release(80, 3, 0) == R::Cycled && !k.locked());
  CHECK(k.press(200, 3) == R::Locked && k.release(260, 3, 0) == R::StuckPrevious && k.sticky());  // within 400 ms of the first
  CHECK(k.escape());
  CHECK(k.press(1000, 3) == R::Locked && k.release(1080, 3, 0) == R::Cycled);
  CHECK(k.press(1600, 3) == R::Locked && k.release(1660, 3, 0) == R::Cycled && !k.sticky());  // too late: two single taps
  CHECK(k.press(1700, 3) == R::Locked && k.release(2100, 3, 0) == R::Unlocked);  // a hold after a tap is a hold
  CHECK(k.press(2200, 3) == R::Locked && k.release(2250, 3, 0) == R::Cycled);  // the hold ended any double tap
}

CHECK_MAIN()
