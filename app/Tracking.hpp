#pragma once
// The view tracker's parts that need no view (UI-32 cross lock): where tracking lines meet in space, where a locked line
// lines up with the other anchors, how far the pointer is from taking such a crossing on screen, and the Shift key's lock
// states. Viewport::updateTracking and inferenceKey use them (ViewportTracking.cpp, ViewportPicking.cpp); test_tracking.
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_XY.hxx>

#include <cstdint>
#include <vector>

namespace tracking {

struct Line { gp_Pnt anchor; gp_Vec direction; };  // direction: unit

// Where two lines meet in space (lines that only cross on screen never do); false when parallel or apart by more than
// `tolerance` at their closest.
bool meet(const Line& a, const Line& b, gp_Pnt& at, double tolerance = 1e-7);

// Cross lock: the points of the locked line that line up with the other anchors. For each anchor, the point with its x,
// y or z, for every axis along which the line runs by more than 0.1 per unit (point filters: the coordinate planes
// through it); then where `lines` meet the locked line in space. The locked line's own anchor and repeats (1e-7) are out.
struct Crossing { gp_Pnt point, from; };  // from: where the second guide starts (the other anchor)
std::vector<Crossing> crossings(const Line& locked, const std::vector<gp_Pnt>& anchors, const std::vector<Line>& lines);

// How far the pointer is from taking a crossing on screen (px): its distance from the second guide (the line through
// `from` and `at`); where that guide is seen end-on or runs within 15 degrees of the locked line (`along`: the locked
// line's screen direction), the distance from the locked point (the pointer's projection onto the line) to `at`.
double reach(const gp_XY& cursor, const gp_XY& locked, const gp_XY& along, const gp_XY& from, const gp_XY& at);

// The Shift key over the tracker's guides (`guides`: how many are offered). Pressed, it locks the guide shown; released
// after a hold, it unlocks. A tap locks it until a click or Esc (sticky) when it is the only guide, else shows the next
// one, and a second tap within kDoubleTapMs locks the one shown before the first (StuckPrevious). A tap on a sticky lock
// shows the next crossing when several are in reach, else unlocks; a hold keeps it.
class ShiftLock {
 public:
  static constexpr std::int64_t kTapMs = 250, kDoubleTapMs = 400;
  enum class Result { Ignored, Locked, Kept, Unlocked, Cycled, Stuck, StuckPrevious, NextCrossing };
  Result press(std::int64_t now, int guides);
  Result release(std::int64_t now, int guides, int crossings);
  bool escape();      // true when it unlocked (Esc goes no further)
  void clicked();     // a pick ends a sticky lock
  void deactivate();  // the application lost the keyboard: a held lock ends, a sticky one stays
  void reset() { *this = ShiftLock(); }
  bool held() const { return m_held; }
  bool locked() const { return m_locked; }
  bool sticky() const { return m_sticky; }

 private:
  bool m_held = false, m_locked = false, m_sticky = false;
  std::int64_t m_pressed = 0, m_tapped = -1;  // m_tapped: a tap that a second one would complete, -1 none
};

}  // namespace tracking
