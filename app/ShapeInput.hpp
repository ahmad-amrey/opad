#pragma once
// A shape's typed sizes (TODO 11 UI-17), decided without widgets: where the click a step waits for goes when some of its
// values are typed (the pointer gives the others), and what a typed line angle holds once the line is made. The sketch
// clicks exactly there, so the rubber band, a click and Enter make the same shape. No Qt and no sketch here:
// tests/test_shape_input.cpp.
#include <algorithm>
#include <cmath>

namespace shapeinput {
struct P { double u = 0, v = 0; };

// From `base`: a length alone keeps the pointer's direction (+X while the pointer is on the base), an angle alone (radians
// from X) goes as far along it as the pointer does, both give one point. A negative length goes the other way.
inline P polar(P base, P pointer, const double* length, const double* angle) {
  const double du = pointer.u - base.u, dv = pointer.v - base.v, distance = std::hypot(du, dv);
  const double a = angle ? *angle : distance > 1e-12 ? std::atan2(dv, du) : 0;
  const double along = du * std::cos(a) + dv * std::sin(a);
  const double l = length ? *length : along > 1e-9 ? along : distance;
  return {base.u + l * std::cos(a), base.v + l * std::sin(a)};
}

// One side of a rectangle typed: `size` towards the pointer's side of `from` (the positive side while the pointer is not
// known or level with it), a negative size the negative way; times `scale` (a centre rectangle's corner: half the size).
inline double side(double from, double pointer, const double* size, bool known, double scale) {
  if (!size) return pointer;
  const double sign = *size < 0 || !known || pointer >= from - 1e-12 ? 1 : -1;
  return from + sign * *size * scale;
}
inline P corner(P first, P pointer, const double* width, const double* height, bool known, double scale) {
  return {side(first.u, pointer.u, width, known, scale), side(first.v, pointer.v, height, known, scale)};
}

// Off the line a -> b by `offset` on the pointer's side (its left while the pointer is on the line), where the pointer is
// along it: a slot's half width, an ellipse's minor radius, a three-point rectangle's height.
inline P across(P a, P b, P pointer, double offset) {
  const double dx = b.u - a.u, dy = b.v - a.v, length = std::hypot(dx, dy);
  if (length < 1e-12) return pointer;
  const double nx = -dy / length, ny = dx / length, now = (pointer.u - a.u) * nx + (pointer.v - a.v) * ny;
  const double move = (now < 0 ? -1 : 1) * offset - now;
  return {pointer.u + move * nx, pointer.v + move * ny};
}

// The third point of a three-point arc (or circle) from a to b with `radius`: the middle of the arc on the pointer's side
// of the chord, of the two circles the one the pointer is nearer (the shorter arc unless it is out by the longer one).
// False: the radius is less than half the chord.
inline bool bulge(P a, P b, P pointer, double radius, P& out) {
  const double dx = b.u - a.u, dy = b.v - a.v, chord = std::hypot(dx, dy), r = std::fabs(radius);
  if (chord < 1e-12 || r < chord / 2 - 1e-12) return false;
  const double nx = -dy / chord, ny = dx / chord, mu = (a.u + b.u) / 2, mv = (a.v + b.v) / 2;
  const double side = (pointer.u - mu) * nx + (pointer.v - mv) * ny < 0 ? -1 : 1;
  const double h = std::sqrt(std::max(0.0, r * r - chord * chord / 4));
  auto off = [&](double centre) { return std::fabs(std::hypot(pointer.u - (mu + nx * centre), pointer.v - (mv + ny * centre)) - r); };
  const double sagitta = off(side * h) < off(-side * h) ? r + h : r - h;  // the centre on the pointer's side: the longer arc
  out = {mu + nx * side * sagitta, mv + ny * side * sagitta};
  return true;
}

// The end of an arc that leaves `a` along the unit direction `t` (tangent to it there) and turns to the pointer's side:
// with `radius` and `sweep` (radians) when typed, else the circle through the pointer and the pointer's place on it.
inline P tangentArc(P a, P t, P pointer, const double* radius, const double* sweep) {
  const double lx = -t.v, ly = t.u, du = pointer.u - a.u, dv = pointer.v - a.v, off = du * lx + dv * ly, side = off < 0 ? -1 : 1;
  const double r = radius ? std::fabs(*radius) : std::fabs(off) > 1e-12 ? (du * du + dv * dv) / (2 * std::fabs(off)) : 0;
  if (r < 1e-12) return pointer;
  const P c{a.u + side * lx * r, a.v + side * ly * r};
  const double from = std::atan2(a.v - c.v, a.u - c.u);
  const double turn = sweep ? side * *sweep : std::remainder(std::atan2(pointer.v - c.v, pointer.u - c.u) - from, 2 * std::acos(-1.0));
  return {c.u + r * std::cos(from + turn), c.v + r * std::sin(from + turn)};
}

// What a typed line angle holds once the line is made. Measured from the X axis (`relative` false, or no line before
// it): along an axis it is horizontal or vertical. Otherwise (and when no axis holds it) against the line before it,
// `between` (radians, its angle from that line): parallel, perpendicular, or that angle between them. Nothing to hold it
// against: None.
enum class Hold { None, Horizontal, Vertical, Parallel, Perpendicular, Angle };
inline Hold angleHold(double absolute, bool previous, double between, bool relative) {
  const double pi = std::acos(-1.0);
  auto multiple = [](double a, double of) { return std::fabs(std::remainder(a, of)) < 1e-9; };
  if (!relative || !previous) {
    if (multiple(absolute, pi)) return Hold::Horizontal;
    if (multiple(absolute - pi / 2, pi)) return Hold::Vertical;
  }
  if (!previous) return Hold::None;
  if (multiple(between, pi)) return Hold::Parallel;
  if (multiple(between - pi / 2, pi)) return Hold::Perpendicular;
  return Hold::Angle;
}
// The size of the angle between two directions, 0..pi (what an angle dimension between two lines holds).
inline double between(double angle) { return std::fabs(std::remainder(angle, 2 * std::acos(-1.0))); }

// A chamfer given by its distance along the corner's first line and its angle to that line (radians): its distance along
// the second line, the corner's lines `corner` apart (0..pi). Negative: the cut never meets the second line.
inline double chamferSecond(double first, double angle, double corner) {
  if (angle <= 1e-12 || angle + corner >= std::acos(-1.0) - 1e-9) return -1;
  return first * std::sin(angle) / std::sin(angle + corner);
}
}  // namespace shapeinput
