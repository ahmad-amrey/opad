#pragma once
// Navigation staples (UI-47) that need no view: the previous / next view history, the zoom window's rectangle and the
// view twist.
//
// Previous / next view: the cameras the view came to rest at, kept like a browser's history. A camera is recorded
// once the view has been still for a moment (an orbit is one entry, not every frame of it); going back and then to a new
// view drops what was ahead. Only the camera's place and zoom: the projection and the display stay as they are.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>
#include <gp_Vec.hxx>

struct ViewState {
  std::array<double, 3> eye{0, 0, 1}, target{0, 0, 0}, up{0, 1, 0};
  double scale = 1;  // the orthographic view height (also what a perspective view keeps)
  // The same view: positions within a hundred-thousandth of the view's height, the same up and zoom.
  bool same(const ViewState& o) const {
    const double tolerance = 1e-5 * std::max({scale, o.scale, 1e-9});
    auto close = [](const std::array<double, 3>& a, const std::array<double, 3>& b, double tol) {
      return std::abs(a[0] - b[0]) <= tol && std::abs(a[1] - b[1]) <= tol && std::abs(a[2] - b[2]) <= tol;
    };
    return close(eye, o.eye, tolerance) && close(target, o.target, tolerance) && close(up, o.up, 1e-6) &&
           std::abs(scale - o.scale) <= 1e-6 * std::max(scale, o.scale);
  }
};

class ViewHistory {
 public:
  explicit ViewHistory(std::size_t limit = 50) : m_limit(std::max<std::size_t>(limit, 2)) {}
  // The view rests here: a new entry after the current one, unless it is the current one; what was ahead goes. True: added.
  bool record(const ViewState& s) {
    if (!m_entries.empty() && m_entries[m_pos].same(s)) return false;
    if (!m_entries.empty()) m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(m_pos) + 1, m_entries.end());
    m_entries.push_back(s);
    if (m_entries.size() > m_limit) m_entries.erase(m_entries.begin());
    m_pos = m_entries.size() - 1;
    return true;
  }
  const ViewState* back() { return canBack() ? &m_entries[--m_pos] : nullptr; }
  const ViewState* forward() { return canForward() ? &m_entries[++m_pos] : nullptr; }
  bool canBack() const { return !m_entries.empty() && m_pos > 0; }
  bool canForward() const { return m_pos + 1 < m_entries.size(); }
  void clear() {
    m_entries.clear();
    m_pos = 0;
  }
  std::size_t size() const { return m_entries.size(); }
  std::size_t position() const { return m_pos; }

 private:
  std::vector<ViewState> m_entries;
  std::size_t m_pos = 0, m_limit;
};

namespace viewnav {
// The zoom window's rectangle (x0, y0, x1, y1, ordered) from a drag in view pixels; a click (under `click` pixels either
// way) zooms in twice about where it was: a rectangle of half the view's size around it.
inline std::array<int, 4> zoomRect(int x0, int y0, int x1, int y1, int width, int height, int click = 4) {
  if (std::abs(x1 - x0) < click || std::abs(y1 - y0) < click) return {x0 - width / 4, y0 - height / 4, x0 + width / 4, y0 + height / 4};
  return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
}

// The up a view along `direction` has untwisted: world Y for a view along Z (top, bottom: a plan reads north up), world Z
// for any other (elevations, an iso view), made square to the direction.
inline gp_Dir naturalUp(const gp_Dir& direction) {
  const bool plan = std::abs(direction.Z()) >= std::max(std::abs(direction.X()), std::abs(direction.Y()));
  gp_Vec up = plan ? gp_Vec(0, 1, 0) : gp_Vec(0, 0, 1);
  up -= gp_Vec(direction) * up.Dot(gp_Vec(direction));
  if (up.Magnitude() < 1e-9) up = gp_Vec(direction).Crossed(gp_Vec(1, 0, 0));
  return gp_Dir(up);
}
// How far `up` is turned from `natural` about the direction looked along, degrees in (-180, 180]; positive: the view
// turned counter-clockwise on screen (Viewport::rollView's sense).
inline double twist(const gp_Dir& direction, const gp_Dir& up, const gp_Dir& natural) {
  double a = natural.AngleWithRef(up, direction) * 180.0 / M_PI;
  if (a <= -180 + 1e-9) a += 360;
  return std::abs(a) < 1e-9 ? 0.0 : a;
}
// The up turned `degrees` from natural about the direction looked along.
inline gp_Dir twisted(const gp_Dir& direction, const gp_Dir& natural, double degrees) {
  return natural.Rotated(gp_Ax1(gp_Pnt(0, 0, 0), direction), degrees * M_PI / 180.0);
}
}  // namespace viewnav
