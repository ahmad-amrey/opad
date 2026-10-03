#pragma once
// The order the sketch takes its inferences in once no object snap (point, midpoint, quadrant, intersection) holds
// the pointer: two guides crossing > a grid node > one guide > the angle ray > a curve > the grid. With grid snapping
// on, a guide is quantised along itself: an axis guide to the grid lines it crosses, any other (an extension, the
// angle ray, the Shift lock) by whole steps from its anchor. No Qt and no sketch here: tests/test_sketch_snap.cpp.
#include <cmath>
#include <vector>

namespace sketchsnap {
struct Guide { double x = 0, y = 0, dx = 1, dy = 0; int anchor = 0; };  // through (x, y) along the unit (dx, dy)
struct Pick {
  enum class By { Pointer, Cross, Node, Guide, Ray, Curve, Grid } by = By::Pointer;
  double u = 0, v = 0;
  int guide = -1, other = -1;  // Cross: both guides; Node: a guide through it (-1: none); Guide: the guide
};

// The grid spacing for a view `span` wide: a tenth of it rounded down to a power of ten, or the set spacing while
// that draws at most 400 lines.
inline double gridStep(double span, double spacing) {
  return spacing > 0 && span / spacing <= 400 ? spacing : std::pow(10.0, std::floor(std::log10(span / 10.0)));
}
inline double onGrid(double x, double step) { return std::round(x / step) * step; }
inline bool axis(const Guide& g) { return std::abs(g.dx) < 1e-12 || std::abs(g.dy) < 1e-12; }
inline double distance(const Guide& g, double u, double v) { return std::abs((u - g.x) * g.dy - (v - g.y) * g.dx); }

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
// used to take every pointer within 60 px of the last point).
inline bool angleRay(double x, double y, double u, double v, double increment, double t, Guide& ray) {
  const double dx = u - x, dy = v - y, len = std::hypot(dx, dy);
  if (!(len > t) || !(increment > 0)) return false;
  const double angle = std::round(std::atan2(dy, dx) / increment) * increment;
  ray = {x, y, std::cos(angle), std::sin(angle)};
  if (std::abs(ray.dx) < 1e-12) ray.dx = 0;
  if (std::abs(ray.dy) < 1e-12) ray.dy = 0;
  return distance(ray, u, v) < std::min(t, len * std::sin(increment / 4));
}

// u, v: the pointer; t: the capture distance; step: the grid spacing, 0 = grid snapping off; guides: the alignments
// that apply; ray: the angle ray if it holds the pointer; curve: the foot on the nearest curve within t.
inline Pick resolve(double u, double v, double t, double step, const std::vector<Guide>& guides, const Guide* ray, const double* curve) {
  Pick p;
  p.u = u;
  p.v = v;
  std::vector<int> reach;  // the guides within t
  for (int i = 0; i < int(guides.size()); ++i)
    if (distance(guides[i], u, v) < t) reach.push_back(i);
  double best = 2 * t;
  for (size_t i = 0; i < reach.size(); ++i)
    for (size_t j = i + 1; j < reach.size(); ++j) {
      const Guide &a = guides[reach[i]], &b = guides[reach[j]];
      const double den = a.dx * b.dy - a.dy * b.dx;
      if (a.anchor == b.anchor || std::abs(den) < 0.1) continue;  // one point's own guides cross at it; nearly parallel ones far away
      const double k = ((b.x - a.x) * b.dy - (b.y - a.y) * b.dx) / den, x = a.x + k * a.dx, y = a.y + k * a.dy;
      if (const double d = std::hypot(x - u, y - v); d < best) {
        best = d;
        p = {Pick::By::Cross, x, y, reach[i], reach[j]};
      }
    }
  if (p.by == Pick::By::Cross) return p;
  if (step > 0) {
    const double gu = onGrid(u, step), gv = onGrid(v, step);
    if (std::hypot(gu - u, gv - v) < t) {
      p = {Pick::By::Node, gu, gv};
      for (int i : reach)
        if (distance(guides[i], gu, gv) < step * 1e-6) { p.guide = i; break; }  // the inferences that agree with it stay
      return p;
    }
  }
  if (!reach.empty()) {
    p.by = Pick::By::Guide;
    p.guide = reach.front();
    for (int i : reach)
      if (distance(guides[i], u, v) < distance(guides[p.guide], u, v)) p.guide = i;
    project(guides[p.guide], u, v, step, p.u, p.v);
    return p;
  }
  if (ray) {
    p.by = Pick::By::Ray;
    project(*ray, u, v, step, p.u, p.v);
    return p;
  }
  if (curve) {
    p.by = Pick::By::Curve;
    p.u = curve[0];
    p.v = curve[1];
    return p;
  }
  if (step > 0) {
    p.by = Pick::By::Grid;
    p.u = onGrid(u, step);
    p.v = onGrid(v, step);
  }
  return p;
}
}  // namespace sketchsnap
