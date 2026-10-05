#include "Tracking.hpp"

#include <cmath>
#include <numbers>

namespace tracking {

bool meet(const Line& a, const Line& b, gp_Pnt& at, double tolerance) {
  const double dot = a.direction.Dot(b.direction), det = 1 - dot * dot;
  if (det < 1e-10) return false;
  const gp_Vec delta(a.anchor, b.anchor);
  const double t = (delta.Dot(a.direction) - dot * delta.Dot(b.direction)) / det;
  const double u = (dot * delta.Dot(a.direction) - delta.Dot(b.direction)) / det;
  const gp_Pnt p = a.anchor.Translated(a.direction * t), q = b.anchor.Translated(b.direction * u);
  if (p.Distance(q) > tolerance) return false;
  at = p;
  return true;
}

std::vector<Crossing> crossings(const Line& locked, const std::vector<gp_Pnt>& anchors, const std::vector<Line>& lines) {
  std::vector<Crossing> out;
  auto add = [&](const gp_Pnt& p, const gp_Pnt& from) {
    if (p.Distance(locked.anchor) < 1e-7) return;
    for (const auto& c : out)
      if (c.point.Distance(p) < 1e-7) return;
    out.push_back({p, from});
  };
  for (const auto& b : anchors) {
    if (b.Distance(locked.anchor) < 1e-7) continue;
    for (int k = 1; k <= 3; ++k)
      if (const double d = locked.direction.Coord(k); std::abs(d) > 0.1) add(locked.anchor.Translated(locked.direction * ((b.Coord(k) - locked.anchor.Coord(k)) / d)), b);
  }
  for (const auto& line : lines) {
    gp_Pnt p;
    if (line.anchor.Distance(locked.anchor) >= 1e-7 && meet(locked, line, p)) add(p, line.anchor);
  }
  return out;
}

double reach(const gp_XY& cursor, const gp_XY& locked, const gp_XY& along, const gp_XY& from, const gp_XY& at) {
  const gp_XY guide = at - from;
  const double length = guide.Modulus(), run = along.Modulus();
  if (length < 2 || run < 1e-9 || std::abs(guide.Crossed(along)) < std::sin(std::numbers::pi / 12) * length * run) return (at - locked).Modulus();
  return std::abs(guide.Crossed(cursor - from)) / length;
}

ShiftLock::Result ShiftLock::press(std::int64_t now, int guides) {
  if (m_held) return Result::Ignored;
  if (m_sticky) {
    m_held = true, m_pressed = now;
    return Result::Kept;
  }
  if (guides <= 0) return Result::Ignored;
  m_held = m_locked = true, m_pressed = now;
  return Result::Locked;
}

ShiftLock::Result ShiftLock::release(std::int64_t now, int guides, int crossings) {
  if (!m_held) return Result::Ignored;
  m_held = false;
  const bool tap = now - m_pressed < kTapMs, second = tap && m_tapped >= 0 && now - m_tapped < kDoubleTapMs;
  m_tapped = -1;
  if (m_sticky) {
    if (!tap || second) return Result::Kept;  // second: the double tap's (or the quick repeat after a single-guide tap)
    if (crossings > 1) return Result::NextCrossing;
    m_locked = m_sticky = false;
    return Result::Unlocked;
  }
  if (tap && (guides == 1 || second)) {
    m_sticky = true;
    if (!second) m_tapped = now;  // a double tap out of habit keeps it
    return second ? Result::StuckPrevious : Result::Stuck;
  }
  m_locked = false;
  if (tap) m_tapped = now;
  return tap && guides > 1 ? Result::Cycled : Result::Unlocked;
}

bool ShiftLock::escape() {
  if (!m_locked) return false;
  m_locked = m_sticky = m_held = false, m_tapped = -1;
  return true;
}

void ShiftLock::clicked() {
  if (m_sticky) m_locked = m_sticky = false;
  m_tapped = -1;
}

void ShiftLock::deactivate() {
  m_held = false, m_locked = m_sticky, m_tapped = -1;
}

}  // namespace tracking
