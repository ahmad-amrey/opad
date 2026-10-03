#pragma once
// Snap markers and constraint pictograms (TODO 11 UI-23, UI-21): what the pointer is pulled to, told by its shape, and
// the constraints a click there adds, as small pictograms beside the pointer. Shared by the sketch, 3D tracking and
// drawings: plain segments in pixels (x right, y up) about the snapped point (a marker) or the pictogram's centre, which
// the caller maps into its view (the sketch plane, the screen plane) and draws in the inference colour, 1.5 px wide.
//   Markers: endpoint □, midpoint △, centre ○, quadrant ◇, intersection ✕, apparent intersection ✕ in a square, nearest
//   (on a curve) ⧗, perpendicular ⟂, tangent ○ under a line, extension ⊢ (turned() along its dashed line, out of the
//   line's end), tracking ✛, locked: a padlock (by its line, which is drawn thick dashed), grid #.
//   Pictograms (constraints): coincident ●, on a curve (a dot on a line), midpoint, horizontal, vertical, perpendicular,
//   tangent, parallel, equal, concentric, collinear, symmetric, fix, smooth (G2), equal curvature; badge(): the frame they
//   sit in; layoutBadges(): where a sketch's constraint badges go so that none covers another (UI-24).
// No Qt and no OCCT: tests/test_snap_markers.cpp.
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace snapmarkers {
struct Seg { double x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
enum class Marker { Endpoint, Midpoint, Centre, Quadrant, Intersection, Apparent, Nearest, Perpendicular, Tangent, Extension, Tracking, Locked, Grid };
enum class Glyph { Coincident, OnCurve, Midpoint, Horizontal, Vertical, Perpendicular, Tangent, Parallel, Equal, Concentric, Collinear, Symmetric, Fix, Smooth, Curvature };

namespace detail {
inline void poly(std::vector<Seg>& out, std::initializer_list<std::pair<double, double>> pts, bool closed, double k, double dx = 0, double dy = 0) {
  const auto* p = pts.begin();
  for (size_t i = 1; i < pts.size(); ++i) out.push_back({dx + p[i - 1].first * k, dy + p[i - 1].second * k, dx + p[i].first * k, dy + p[i].second * k});
  if (closed && pts.size() > 2) out.push_back({dx + p[pts.size() - 1].first * k, dy + p[pts.size() - 1].second * k, dx + p[0].first * k, dy + p[0].second * k});
}
// An arc about (x, y) of radius r from angle a through sweep (radians), in n pieces.
inline void arc(std::vector<Seg>& out, double x, double y, double r, double a, double sweep, int n) {
  for (int i = 0; i < n; ++i)
    out.push_back({x + r * std::cos(a + sweep * i / n), y + r * std::sin(a + sweep * i / n), x + r * std::cos(a + sweep * (i + 1) / n), y + r * std::sin(a + sweep * (i + 1) / n)});
}
inline void ring(std::vector<Seg>& out, double x, double y, double r, int n = 16) { arc(out, x, y, r, 0, 2 * std::acos(-1.0), n); }
}  // namespace detail

// A marker `size` px across, centred on the snapped point.
inline std::vector<Seg> marker(Marker m, double size = 10) {
  using detail::poly;
  std::vector<Seg> out;
  const double h = size / 2, pi = std::acos(-1.0);
  switch (m) {
    case Marker::Endpoint: poly(out, {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, true, h); break;
    case Marker::Midpoint: poly(out, {{-1, -0.8}, {1, -0.8}, {0, 0.95}}, true, h); break;
    case Marker::Centre: detail::ring(out, 0, 0, h); break;
    case Marker::Quadrant: poly(out, {{0, 1}, {1, 0}, {0, -1}, {-1, 0}}, true, h); break;
    case Marker::Intersection:
      poly(out, {{-0.85, -0.85}, {0.85, 0.85}}, false, h);
      poly(out, {{-0.85, 0.85}, {0.85, -0.85}}, false, h);
      break;
    case Marker::Apparent:
      poly(out, {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}, true, h);
      poly(out, {{-0.6, -0.6}, {0.6, 0.6}}, false, h);
      poly(out, {{-0.6, 0.6}, {0.6, -0.6}}, false, h);
      break;
    case Marker::Nearest: poly(out, {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}}, true, h); break;  // an hourglass: top, a diagonal, bottom, back
    case Marker::Perpendicular:
      poly(out, {{-1, -1}, {1, -1}}, false, h);
      poly(out, {{0, -1}, {0, 1}}, false, h);
      poly(out, {{0, -0.45}, {0.55, -0.45}, {0.55, -1}}, false, h);  // the right angle's square
      break;
    case Marker::Tangent:
      detail::ring(out, 0, -0.25 * h, 0.75 * h);
      poly(out, {{-1, 0.5}, {1, 0.5}}, false, h);
      break;
    case Marker::Extension:
      poly(out, {{-0.6, -0.8}, {-0.6, 0.8}}, false, h);
      poly(out, {{-0.6, 0}, {1, 0}}, false, h);
      break;
    case Marker::Tracking:
      poly(out, {{-1, 0}, {1, 0}}, false, h);
      poly(out, {{0, -1}, {0, 1}}, false, h);
      break;
    case Marker::Locked:  // a padlock: its body and its shackle
      poly(out, {{-0.75, -1}, {0.75, -1}, {0.75, 0.1}, {-0.75, 0.1}}, true, h);
      poly(out, {{-0.45, 0.1}, {-0.45, 0.45}}, false, h);
      poly(out, {{0.45, 0.1}, {0.45, 0.45}}, false, h);
      detail::arc(out, 0, 0.45 * h, 0.45 * h, 0, pi, 8);
      break;
    case Marker::Grid:  // #
      poly(out, {{-0.35, -1}, {-0.35, 1}}, false, h);
      poly(out, {{0.35, -1}, {0.35, 1}}, false, h);
      poly(out, {{-1, -0.35}, {1, -0.35}}, false, h);
      poly(out, {{-1, 0.35}, {1, 0.35}}, false, h);
      break;
  }
  return out;
}

// The drawing cursor with grid snapping (the system pointer hidden): a crosshair `arm` px out each way from the snapped
// point, open `gap` px around it for the snap's marker or dot.
inline std::vector<Seg> crosshair(double arm = 11, double gap = 4) {
  return {{-arm, 0, -gap, 0}, {gap, 0, arm, 0}, {0, -arm, 0, -gap}, {0, gap, 0, arm}};
}

// A constraint's pictogram `size` px across, centred on (0, 0).
inline std::vector<Seg> glyph(Glyph g, double size = 10) {
  using detail::poly;
  std::vector<Seg> out;
  const double h = size / 2;
  switch (g) {
    case Glyph::Coincident:  // a dot: rings filling it
      for (const double r : {0.5, 0.33, 0.16}) detail::ring(out, 0, 0, r * h, 12);
      break;
    case Glyph::OnCurve:  // a dot on a line
      poly(out, {{-1, 0}, {1, 0}}, false, h);
      for (const double r : {0.4, 0.2}) detail::ring(out, 0, 0, r * h, 12);
      break;
    case Glyph::Midpoint:
      poly(out, {{-1, -0.5}, {1, -0.5}}, false, h);
      poly(out, {{-0.45, -0.5}, {0, 0.5}, {0.45, -0.5}}, false, h);
      break;
    case Glyph::Horizontal:
      poly(out, {{-1, 0}, {1, 0}}, false, h);
      poly(out, {{-1, -0.4}, {-1, 0.4}}, false, h);
      poly(out, {{1, -0.4}, {1, 0.4}}, false, h);
      break;
    case Glyph::Vertical:
      poly(out, {{0, -1}, {0, 1}}, false, h);
      poly(out, {{-0.4, -1}, {0.4, -1}}, false, h);
      poly(out, {{-0.4, 1}, {0.4, 1}}, false, h);
      break;
    case Glyph::Perpendicular:
      poly(out, {{-1, -0.8}, {1, -0.8}}, false, h);
      poly(out, {{0, -0.8}, {0, 1}}, false, h);
      break;
    case Glyph::Tangent:
      detail::ring(out, 0, -0.3 * h, 0.6 * h, 12);
      poly(out, {{-1, 0.3}, {1, 0.3}}, false, h);
      break;
    case Glyph::Parallel:
      poly(out, {{-0.7, -1}, {-0.1, 1}}, false, h);
      poly(out, {{0.1, -1}, {0.7, 1}}, false, h);
      break;
    case Glyph::Equal:
      poly(out, {{-0.9, 0.35}, {0.9, 0.35}}, false, h);
      poly(out, {{-0.9, -0.35}, {0.9, -0.35}}, false, h);
      break;
    case Glyph::Concentric:
      detail::ring(out, 0, 0, 0.9 * h, 16);
      detail::ring(out, 0, 0, 0.4 * h, 12);
      break;
    case Glyph::Collinear:
      poly(out, {{-1, -0.6}, {-0.15, 0}}, false, h);
      poly(out, {{0.15, 0.2}, {1, 0.8}}, false, h);
      break;
    case Glyph::Symmetric:
      poly(out, {{0, -1}, {0, -0.55}}, false, h);
      poly(out, {{0, -0.2}, {0, 0.2}}, false, h);
      poly(out, {{0, 0.55}, {0, 1}}, false, h);
      poly(out, {{-0.4, -0.7}, {-0.9, 0}, {-0.4, 0.7}}, false, h);
      poly(out, {{0.4, -0.7}, {0.9, 0}, {0.4, 0.7}}, false, h);
      break;
    case Glyph::Fix:  // a stake in the ground
      poly(out, {{0, 1}, {0, -0.3}}, false, h);
      poly(out, {{-0.9, -0.3}, {0.9, -0.3}}, false, h);
      for (const double x : {-0.6, 0.0, 0.6}) poly(out, {{x, -0.3}, {x - 0.35, -0.95}}, false, h);
      break;
    case Glyph::Smooth: {  // an S: two curves running on into each other (G2)
      const double pi = std::acos(-1.0);
      detail::arc(out, -0.45 * h, 0, 0.45 * h, 0, pi, 8);
      detail::arc(out, 0.45 * h, 0, 0.45 * h, pi, pi, 8);
      break;
    }
    case Glyph::Curvature: {  // an arc and its radius
      const double pi = std::acos(-1.0);
      detail::arc(out, 0, -0.9 * h, 1.6 * h, pi / 3, pi / 3, 8);
      poly(out, {{0, -0.9}, {0, 0.7}}, false, h);
      poly(out, {{-0.25, 0.4}, {0, 0.7}, {0.25, 0.4}}, false, h);
      break;
    }
  }
  return out;
}

// Segments turned by `a` radians about (0, 0), counter-clockwise (x right, y up): a marker that follows its line, as the
// extension's ⊢, whose stem (+x) points the way the line goes on past its end.
inline std::vector<Seg> turned(std::vector<Seg> segs, double a) {
  const double c = std::cos(a), s = std::sin(a);
  for (auto& g : segs) g = {c * g.x0 - s * g.y0, s * g.x0 + c * g.y0, c * g.x1 - s * g.y1, s * g.x1 + c * g.y1};
  return segs;
}

// Where a sketch's constraint badges go (UI-24), each `size` px square, given the place each belongs to (px, x right, y up):
// the first free place of a fixed list round it (above right first, where the letters were), then rows running on to the
// right and to the left, so that no badge covers another (they used to stack on one spot and overlap). The badges' centres,
// in order; a place found nowhere free keeps the first choice.
struct Place { double x = 0, y = 0; };
inline std::vector<Place> layoutBadges(const std::vector<Place>& anchors, double size = 16, double gap = 2) {
  const double step = size + gap, s = size;
  std::vector<Place> offsets = {{s, 0.9 * s}, {s, -0.9 * s}, {-s, 0.9 * s}, {-s, -0.9 * s}, {0, 1.4 * s}, {0, -1.4 * s}};
  for (int k = 1; k <= 16; ++k)
    for (const Place& o : {Place{s, 0.9 * s}, Place{s, -0.9 * s}, Place{-s, 0.9 * s}, Place{-s, -0.9 * s}}) offsets.push_back({o.x + (o.x > 0 ? 1 : -1) * k * step, o.y});
  std::unordered_map<std::uint64_t, std::vector<Place>> cells;  // placed centres by `step` cell: a badge meets only its neighbours'
  auto cell = [&](long long cx, long long cy) { return (std::uint64_t(std::uint32_t(cx)) << 32) | std::uint32_t(cy); };
  auto free = [&](const Place& c) {
    const long long cx = (long long)std::floor(c.x / step), cy = (long long)std::floor(c.y / step);
    for (long long dx = -1; dx <= 1; ++dx)
      for (long long dy = -1; dy <= 1; ++dy)
        if (const auto it = cells.find(cell(cx + dx, cy + dy)); it != cells.end())
          for (const Place& p : it->second)
            if (std::fabs(p.x - c.x) < step - 1e-9 && std::fabs(p.y - c.y) < step - 1e-9) return false;
    return true;
  };
  std::vector<Place> out;
  out.reserve(anchors.size());
  for (const Place& a : anchors) {
    Place at{a.x + offsets[0].x, a.y + offsets[0].y};
    for (const Place& o : offsets)
      if (free({a.x + o.x, a.y + o.y})) { at = {a.x + o.x, a.y + o.y}; break; }
    cells[cell((long long)std::floor(at.x / step), (long long)std::floor(at.y / step))].push_back(at);
    out.push_back(at);
  }
  return out;
}

// The frame a pictogram sits in, w x h px about (0, 0), its corners cut by r (a rounded badge drawn in segments).
inline std::vector<Seg> badge(double w, double h, double r = 3) {
  std::vector<Seg> out;
  const double x = w / 2, y = h / 2;
  detail::poly(out, {{-x + r, -y}, {x - r, -y}, {x, -y + r}, {x, y - r}, {x - r, y}, {-x + r, y}, {-x, y - r}, {-x, -y + r}}, true, 1);
  return out;
}
}  // namespace snapmarkers
