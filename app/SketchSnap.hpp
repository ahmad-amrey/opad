#pragma once
// The order the sketch takes its inferences in once no object snap (point, midpoint, quadrant, intersection) holds
// the pointer. Grid snapping off: a crossing (two guides, a guide and the angle ray, or either of them and a curve) > one
// guide > the angle ray > a curve > the pointer. Grid snapping on (AutoCAD's SNAP): the point is a grid node wherever the
// pointer is, unless tracking guides cross (each other, or a curve) within reach; a guide, the angle ray or a curve the
// node lies on only names it (and constrains it), and never takes the point off the grid.
// A Shift lock and Ortho keep their line with a grid too: along it the pointer stops on grid lines (crossGrid).
// Cross-locking (UI-19): points acquired by resting the pointer on them (track) each add their guides, so two of them
// cross at (A.x, B.y); Shift locks the pointer onto one guide, and along it (along) it stops where another guide, the
// angle ray or a curve crosses it (stops: every such place in reach, which Shift taps go through on a lock that stays),
// else where it crosses a grid line (crossGrid: also a slanted lock).
// UI-23: from the step's last point the feet of perpendiculars (normals) and the points of tangency (tangents) on a curve;
// apparent intersections, where segments' lines cross past their ends.
// No Qt and no sketch here: tests/test_sketch_snap.cpp.
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <vector>

namespace sketchsnap {
struct Guide { double x = 0, y = 0, dx = 1, dy = 0; int anchor = 0; };  // through (x, y) along the unit (dx, dy)
// A curve near the pointer: the segment (x, y) -> (ex, ey), or with r > 0 the arc about (x, y) counter-clockwise from
// the angle `start` through `sweep` (a circle: 2 pi).
struct Curve { double x = 0, y = 0, ex = 0, ey = 0, r = 0, start = 0, sweep = 0; };
struct Pick {
  enum class By { Pointer, Cross, Node, Guide, Ray, Curve } by = By::Pointer;
  double u = 0, v = 0;
  // Cross: the guides crossing (-1: the ray or a curve is the other line); Node: a guide through it (-1: none); Guide:
  // the guide. ray: the angle ray is one of the crossing lines (Cross) or runs through the node (Node). curve: the
  // curve crossed (Cross), landed on (Curve) or running through the node (Node), an index into the curves.
  int guide = -1, other = -1, curve = -1;
  bool ray = false;
};

// The grid spacing at `pixel` world units per screen pixel (zoom-adaptive, the 1-2-5 sequence): the smallest of 1, 2 and
// 5 times a power of ten times `unit` (the shown length unit in mm: 25.4 puts the nodes on tenths of an inch) that is at
// least `least` pixels on the screen, so a step is 24 to 60 px. A set spacing (> 0) is the step from 24 px up to `most`
// (the view passes a quarter of its shorter side; at least 60): zoomed out its multiples by 2, 5, 10, 20, ..., zoomed in
// further its fractions by as much (24 to 60 px), so a node is never far off the pointer and some are always in the view.
inline double gridStep(double pixel, double spacing, double unit = 1, double most = 60, double least = 24) {
  const double base = spacing > 0 ? spacing : unit > 0 ? unit : 1, ratio = least * pixel / base;
  if (!(ratio > 0) || !std::isfinite(ratio)) return base;
  if (spacing > 0 && ratio <= 1 && ratio * std::max(most, 60.0) / least >= 1 - 1e-12) return spacing;  // 24 px to `most`: itself
  const int k = int(std::floor(std::log10(ratio)));
  for (const double m : {1.0, 2.0, 5.0, 10.0}) {
    // m * 10^k as the decimal it stands for (2 / 100, never 2 * 0.01)
    const double step = k < 0 ? m / std::pow(10.0, -k) : m * std::pow(10.0, k);
    if (step >= ratio * (1 - 1e-12)) return base * step;
  }
  return base * std::pow(10.0, k + 1);
}
inline double onGrid(double x, double step) { return std::round(x / step) * step; }
inline bool axis(const Guide& g) { return std::abs(g.dx) < 1e-12 || std::abs(g.dy) < 1e-12; }
inline double distance(const Guide& g, double u, double v) { return std::abs((u - g.x) * g.dy - (v - g.y) * g.dx); }

inline bool onArc(const Curve& c, double x, double y) {
  const double turn = 2 * std::acos(-1.0);
  return c.sweep >= turn - 1e-12 || std::abs(std::remainder(std::atan2(y - c.y, x - c.x) - c.start - c.sweep / 2, turn)) <= c.sweep / 2 + 1e-9;
}
// The point of c nearest (u, v) as the new point lands on it: on a segment's line (a point on a line is constrained to
// the whole line), radially on an arc's circle. Returns the distance to the curve itself.
inline double foot(const Curve& c, double u, double v, double& fu, double& fv) {
  if (c.r <= 0) {
    const double dx = c.ex - c.x, dy = c.ey - c.y, len2 = dx * dx + dy * dy;
    const double k = len2 < 1e-18 ? 0 : ((u - c.x) * dx + (v - c.y) * dy) / len2, inside = std::fmin(1.0, std::fmax(0.0, k));
    fu = c.x + k * dx;
    fv = c.y + k * dy;
    return std::hypot(c.x + inside * dx - u, c.y + inside * dy - v);
  }
  const double d = std::hypot(u - c.x, v - c.y);
  fu = d > 1e-12 ? c.x + (u - c.x) * c.r / d : c.x + c.r;
  fv = d > 1e-12 ? c.y + (v - c.y) * c.r / d : c.y;
  if (onArc(c, fu, fv)) return std::abs(d - c.r);
  const double e = c.start + c.sweep;
  return std::fmin(std::hypot(c.x + c.r * std::cos(c.start) - u, c.y + c.r * std::sin(c.start) - v), std::hypot(c.x + c.r * std::cos(e) - u, c.y + c.r * std::sin(e) - v));
}
// Where the guide's line crosses c: up to two points into x, y; none where it runs nearly along a segment.
inline int meet(const Guide& g, const Curve& c, double* x, double* y) {
  if (c.r <= 0) {
    const double dx = c.ex - c.x, dy = c.ey - c.y, den = dx * g.dy - dy * g.dx;
    if (!(std::abs(den) >= 0.1 * std::hypot(dx, dy))) return 0;
    const double k = ((c.y - g.y) * g.dx - (c.x - g.x) * g.dy) / den;
    if (k < -1e-9 || k > 1 + 1e-9) return 0;
    x[0] = c.x + k * dx;
    y[0] = c.y + k * dy;
    return 1;
  }
  const double along = (c.x - g.x) * g.dx + (c.y - g.y) * g.dy, h = distance(g, c.x, c.y);
  if (h > c.r) return 0;
  int n = 0;
  for (const double k : {along - std::sqrt(c.r * c.r - h * h), along + std::sqrt(c.r * c.r - h * h)})
    if (onArc(c, g.x + k * g.dx, g.y + k * g.dy)) {
      x[n] = g.x + k * g.dx;
      y[n++] = g.y + k * g.dy;
    }
  return n;
}

// From the point (fx, fy) to c (UI-23), up to two into x, y. normals: where a line from it meets c at a right angle (a
// segment's foot on it; on a circle or an arc the nearer and the farther point on the line through its centre).
// tangents: where a line from it touches a circle or an arc (none from inside). Only what lies on c itself.
inline int normals(const Curve& c, double fx, double fy, double* x, double* y) {
  if (c.r <= 0) {
    const double dx = c.ex - c.x, dy = c.ey - c.y, len2 = dx * dx + dy * dy;
    const double k = len2 < 1e-18 ? -1 : ((fx - c.x) * dx + (fy - c.y) * dy) / len2;
    if (k < -1e-9 || k > 1 + 1e-9) return 0;
    x[0] = c.x + k * dx;
    y[0] = c.y + k * dy;
    return std::hypot(x[0] - fx, y[0] - fy) > 1e-9 * (1 + std::abs(fx) + std::abs(fy));  // from a point on the line: none
  }
  const double d = std::hypot(fx - c.x, fy - c.y);
  if (d < 1e-12) return 0;  // from the centre every way is normal
  int n = 0;
  for (const double s : {1.0, -1.0}) {
    const double px = c.x + s * (fx - c.x) * c.r / d, py = c.y + s * (fy - c.y) * c.r / d;
    if (onArc(c, px, py)) {
      x[n] = px;
      y[n++] = py;
    }
  }
  return n;
}
inline int tangents(const Curve& c, double fx, double fy, double* x, double* y) {
  const double d = std::hypot(fx - c.x, fy - c.y);
  if (c.r <= 0 || !(d > c.r * (1 + 1e-9))) return 0;
  const double base = std::atan2(fy - c.y, fx - c.x), spread = std::acos(c.r / d);
  int n = 0;
  for (const double s : {1.0, -1.0}) {
    const double px = c.x + c.r * std::cos(base + s * spread), py = c.y + c.r * std::sin(base + s * spread);
    if (onArc(c, px, py)) {
      x[n] = px;
      y[n++] = py;
    }
  }
  return n;
}
// Apparent intersection (UI-23): where the lines of two segments cross past the end of either (false when they are
// nearly parallel, or cross on both: that is an intersection), and where a segment's line past its ends crosses a circle
// or an arc (up to two).
inline bool apparent(const Curve& a, const Curve& b, double& x, double& y) {
  const double ax = a.ex - a.x, ay = a.ey - a.y, bx = b.ex - b.x, by = b.ey - b.y, den = ax * by - ay * bx;
  if (!(std::abs(den) >= 0.1 * std::hypot(ax, ay) * std::hypot(bx, by)) || den == 0) return false;
  const double s = ((b.x - a.x) * by - (b.y - a.y) * bx) / den, t = ((b.x - a.x) * ay - (b.y - a.y) * ax) / den;
  x = a.x + s * ax;
  y = a.y + s * ay;
  return s < -1e-9 || s > 1 + 1e-9 || t < -1e-9 || t > 1 + 1e-9;
}
inline int apparent(const Curve& line, const Curve& round, double* x, double* y) {
  const double dx = line.ex - line.x, dy = line.ey - line.y, len = std::hypot(dx, dy);
  if (len < 1e-12 || round.r <= 0) return 0;
  const Guide g{line.x, line.y, dx / len, dy / len};
  double mx[2], my[2];
  int n = 0;
  for (int i = 0, m = meet(g, round, mx, my); i < m; ++i)
    if (const double k = ((mx[i] - line.x) * dx + (my[i] - line.y) * dy) / (len * len); k < -1e-9 || k > 1 + 1e-9) {
      x[n] = mx[i];
      y[n++] = my[i];
    }
  return n;
}

// The guide's point nearest (u, v); with a grid (step > 0) the nearest one on a grid line (axis guides) or a whole
// number of steps from the anchor (any other).
inline void project(const Guide& g, double u, double v, double step, double& pu, double& pv) {
  double along = (u - g.x) * g.dx + (v - g.y) * g.dy;
  if (step > 0 && !axis(g)) along = onGrid(along, step);
  pu = g.x + along * g.dx;
  pv = g.y + along * g.dy;
  if (step > 0 && axis(g)) {
    if (std::abs(g.dx) > std::abs(g.dy)) pu = onGrid(pu, step);
    else pv = onGrid(pv, step);
  }
}

// The ray from (x, y) at the multiple of `increment` (radians) nearest the pointer, if it holds the pointer: within t,
// narrowed for short segments so that neighbouring rays never cover every direction between them (15 degree rays
// used to take every pointer within 60 px of the last point). The ray's anchor is left as the caller set it.
inline bool angleRay(double x, double y, double u, double v, double increment, double t, Guide& ray) {
  const double dx = u - x, dy = v - y, len = std::hypot(dx, dy);
  if (!(len > t) || !(increment > 0)) return false;
  const double angle = std::round(std::atan2(dy, dx) / increment) * increment;
  ray.x = x;
  ray.y = y;
  ray.dx = std::abs(std::cos(angle)) < 1e-12 ? 0 : std::cos(angle);
  ray.dy = std::abs(std::sin(angle)) < 1e-12 ? 0 : std::sin(angle);
  return distance(ray, u, v) < std::min(t, len * std::sin(increment / 4));
}

// u, v: the pointer; t: the capture distance; step: the grid spacing, 0 = grid snapping off; guides: the alignments
// that apply; ray: the angle ray if it holds the pointer (its anchor that of the guides from the same point); curves:
// the curves within t of the pointer that it may land on (with a grid, also those through the pointer's node).
inline Pick resolve(double u, double v, double t, double step, const std::vector<Guide>& guides, const Guide* ray, const std::vector<Curve>& curves) {
  Pick p;
  p.u = u;
  p.v = v;
  std::vector<int> lines;  // the guides within t, then the ray (-1)
  for (int i = 0; i < int(guides.size()); ++i)
    if (distance(guides[i], u, v) < t) lines.push_back(i);
  const size_t reach = lines.size();
  // With a grid the angle ray never takes the point off it (it only names a node it runs through), so it crosses nothing.
  if (ray && !(step > 0)) lines.push_back(-1);
  auto line = [&](int i) -> const Guide& { return i < 0 ? *ray : guides[size_t(i)]; };
  double best = 2 * t;
  auto cross = [&](double x, double y, int a, int b, int curve) {
    if (const double d = std::hypot(x - u, y - v); d < best) {
      best = d;
      p = {Pick::By::Cross, x, y, a < 0 ? -1 : a, b, curve, a < 0 || (b < 0 && curve < 0)};
    }
  };
  for (size_t i = 0; i < lines.size(); ++i) {
    for (size_t j = i + 1; j < lines.size(); ++j) {
      const Guide &a = line(lines[i]), &b = line(lines[j]);
      const double den = a.dx * b.dy - a.dy * b.dx;
      if (a.anchor == b.anchor || std::abs(den) < 0.1) continue;  // one point's own guides cross at it; nearly parallel ones far away
      const double k = ((b.x - a.x) * b.dy - (b.y - a.y) * b.dx) / den;
      cross(a.x + k * a.dx, a.y + k * a.dy, lines[i], lines[j], -1);
    }
    for (int c = 0; c < int(curves.size()); ++c) {  // where the guide meets a curve the pointer is on
      double fu, fv, x[2], y[2];
      if (foot(curves[size_t(c)], u, v, fu, fv) >= t) continue;
      for (int n = meet(line(lines[i]), curves[size_t(c)], x, y), m = 0; m < n; ++m) cross(x[m], y[m], lines[i], -1, c);
    }
  }
  if (p.by == Pick::By::Cross) return p;
  if (step > 0) {  // the node nearest the pointer, however far; what runs through it stays (its constraint, its name)
    const double gu = onGrid(u, step), gv = onGrid(v, step), on = step * 1e-6;
    p = {Pick::By::Node, gu, gv};
    for (int i = 0; i < int(guides.size()) && p.guide < 0; ++i)
      if (distance(guides[size_t(i)], gu, gv) < on) p.guide = i;
    p.ray = ray && distance(*ray, gu, gv) < on && (gu - ray->x) * ray->dx + (gv - ray->y) * ray->dy > on;  // ahead of its point
    for (int c = 0; c < int(curves.size()) && p.curve < 0; ++c) {
      double fu, fv;
      if (foot(curves[size_t(c)], gu, gv, fu, fv) < on) p.curve = c;
    }
    return p;
  }
  if (reach) {
    p.by = Pick::By::Guide;
    p.guide = lines.front();
    for (size_t i = 0; i < reach; ++i)
      if (distance(guides[size_t(lines[i])], u, v) < distance(guides[size_t(p.guide)], u, v)) p.guide = lines[i];
    project(guides[size_t(p.guide)], u, v, 0, p.u, p.v);
    return p;
  }
  if (ray) {
    p.by = Pick::By::Ray;
    project(*ray, u, v, 0, p.u, p.v);
    return p;
  }
  double closest = t;
  for (int c = 0; c < int(curves.size()); ++c) {
    double fu, fv;
    if (const double d = foot(curves[size_t(c)], u, v, fu, fv); d < closest) {
      closest = d;
      p = {Pick::By::Curve, fu, fv, -1, -1, c};
    }
  }
  return p;
}

// The tracking points, oldest first: resting on a point acquires it, at most `most` of them (the oldest goes first);
// resting on a tracked one lets it go. Returns whether `id` is tracked now.
inline bool track(std::vector<int>& tracked, int id, std::size_t most = 6) {
  if (const auto it = std::find(tracked.begin(), tracked.end(), id); it != tracked.end()) {
    tracked.erase(it);
    return false;
  }
  tracked.push_back(id);
  if (tracked.size() > most) tracked.erase(tracked.begin(), tracked.end() - std::ptrdiff_t(most));
  return true;
}

// The point of g nearest (u, v) on a grid line (x or y a whole number of steps): for an axis guide the grid lines across
// it (as project), for a slanted one either set of lines, whichever it meets first.
inline void crossGrid(const Guide& g, double u, double v, double step, double& pu, double& pv) {
  const double along = (u - g.x) * g.dx + (v - g.y) * g.dy;
  double best = HUGE_VAL;
  pu = g.x + along * g.dx;
  pv = g.y + along * g.dy;
  const double fu = pu, fv = pv;
  if (std::abs(g.dx) > 1e-12) {
    const double x = onGrid(fu, step), k = (x - g.x) / g.dx;
    best = std::abs(k - along);
    pu = x;
    pv = std::abs(g.dy) < 1e-12 ? g.y : g.y + k * g.dy;
  }
  if (std::abs(g.dy) > 1e-12) {
    const double y = onGrid(fv, step), k = (y - g.y) / g.dy;
    if (std::abs(k - along) < best) {
      pu = std::abs(g.dx) < 1e-12 ? g.x : g.x + k * g.dx;
      pv = y;
    }
  }
}

// The places on the locked line within `reach` of the pointer's foot where another guide (not one of the lock's own
// point) or a curve crosses it, nearest first, one per place (`other` the guide crossing there, `curve` the curve; both
// when they meet there).
inline std::vector<Pick> stops(const Guide& lock, double u, double v, double reach, const std::vector<Guide>& guides, const std::vector<Curve>& curves) {
  double fu, fv;
  project(lock, u, v, 0, fu, fv);
  std::vector<std::pair<double, Pick>> found;
  auto cross = [&](double x, double y, int guide, int curve) {
    const double d = std::hypot(x - fu, y - fv);
    if (!(d < reach)) return;
    for (auto& f : found)
      if (Pick& p = f.second; std::hypot(p.u - x, p.v - y) < 1e-9 * (1 + std::abs(x) + std::abs(y))) {
        if (p.other < 0) p.other = guide;
        if (p.curve < 0) p.curve = curve;
        return;
      }
    found.push_back({d, {Pick::By::Cross, x, y, -1, guide, curve}});
  };
  for (int i = 0; i < int(guides.size()); ++i) {
    const Guide& g = guides[size_t(i)];
    const double den = lock.dx * g.dy - lock.dy * g.dx;
    if (g.anchor == lock.anchor || std::abs(den) < 0.1) continue;
    const double k = ((g.x - lock.x) * g.dy - (g.y - lock.y) * g.dx) / den;
    cross(lock.x + k * lock.dx, lock.y + k * lock.dy, i, -1);
  }
  for (int c = 0; c < int(curves.size()); ++c) {
    double x[2], y[2];
    for (int n = meet(lock, curves[size_t(c)], x, y), m = 0; m < n; ++m) cross(x[m], y[m], -1, c);
  }
  std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<Pick> out;
  for (const auto& f : found) out.push_back(f.second);
  return out;
}

// The pointer locked onto `lock`: its foot on the line, pulled within t along it to the nearest of its stops; else where
// it crosses a grid line (crossGrid), else the foot. Cross: `other` the guide crossing it and/or `curve` the curve;
// Guide: on the lock alone (guide stays -1).
inline Pick along(const Guide& lock, double u, double v, double t, double step, const std::vector<Guide>& guides, const std::vector<Curve>& curves) {
  if (const auto held = stops(lock, u, v, t, guides, curves); !held.empty()) return held.front();
  Pick p;
  p.by = Pick::By::Guide;
  if (step > 0) crossGrid(lock, u, v, step, p.u, p.v);
  else project(lock, u, v, 0, p.u, p.v);
  return p;
}
}  // namespace sketchsnap
