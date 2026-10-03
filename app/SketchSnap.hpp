#pragma once
// The order the sketch takes its inferences in once no object snap (point, midpoint, quadrant, intersection) holds
// the pointer: a crossing (two guides, a guide and the angle ray, or either of them and a curve) > a grid node > one
// guide > the angle ray > a curve > the grid. With grid snapping on, a guide is quantised along itself: an axis guide
// to the grid lines it crosses, any other (an extension, the angle ray, the Shift lock) by whole steps from its anchor.
// No Qt and no sketch here: tests/test_sketch_snap.cpp.
#include <cmath>
#include <vector>

namespace sketchsnap {
struct Guide { double x = 0, y = 0, dx = 1, dy = 0; int anchor = 0; };  // through (x, y) along the unit (dx, dy)
// A curve near the pointer: the segment (x, y) -> (ex, ey), or with r > 0 the arc about (x, y) counter-clockwise from
// the angle `start` through `sweep` (a circle: 2 pi).
struct Curve { double x = 0, y = 0, ex = 0, ey = 0, r = 0, start = 0, sweep = 0; };
struct Pick {
  enum class By { Pointer, Cross, Node, Guide, Ray, Curve, Grid } by = By::Pointer;
  double u = 0, v = 0;
  // Cross: the guides crossing (-1: the ray or a curve is the other line); Node: a guide through it (-1: none); Guide:
  // the guide. ray: the angle ray is one of the crossing lines (Cross) or runs through the node (Node). curve: the
  // curve crossed (Cross) or landed on (Curve), an index into the curves.
  int guide = -1, other = -1, curve = -1;
  bool ray = false;
};

// The grid spacing for a view `span` wide: a tenth of it rounded down to a power of ten, or the set spacing while
// that draws at most 400 lines.
inline double gridStep(double span, double spacing) {
  return spacing > 0 && span / spacing <= 400 ? spacing : std::pow(10.0, std::floor(std::log10(span / 10.0)));
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
// the curves within t of the pointer that it may land on.
inline Pick resolve(double u, double v, double t, double step, const std::vector<Guide>& guides, const Guide* ray, const std::vector<Curve>& curves) {
  Pick p;
  p.u = u;
  p.v = v;
  std::vector<int> lines;  // the guides within t, then the ray (-1)
  for (int i = 0; i < int(guides.size()); ++i)
    if (distance(guides[i], u, v) < t) lines.push_back(i);
  const size_t reach = lines.size();
  if (ray) lines.push_back(-1);
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
  if (step > 0) {
    const double gu = onGrid(u, step), gv = onGrid(v, step);
    if (std::hypot(gu - u, gv - v) < t) {
      p = {Pick::By::Node, gu, gv};
      for (size_t i = 0; i < reach; ++i)
        if (distance(guides[size_t(lines[i])], gu, gv) < step * 1e-6) { p.guide = lines[i]; break; }  // the inferences that agree with it stay
      p.ray = ray && distance(*ray, gu, gv) < step * 1e-6;
      return p;
    }
  }
  if (reach) {
    p.by = Pick::By::Guide;
    p.guide = lines.front();
    for (size_t i = 0; i < reach; ++i)
      if (distance(guides[size_t(lines[i])], u, v) < distance(guides[size_t(p.guide)], u, v)) p.guide = lines[i];
    project(guides[size_t(p.guide)], u, v, step, p.u, p.v);
    return p;
  }
  if (ray) {
    p.by = Pick::By::Ray;
    project(*ray, u, v, step, p.u, p.v);
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
  if (p.by == Pick::By::Curve) return p;
  if (step > 0) {
    p.by = Pick::By::Grid;
    p.u = onGrid(u, step);
    p.v = onGrid(v, step);
  }
  return p;
}
}  // namespace sketchsnap
