#pragma once
// The drawing the 2D writer tests share (test_drawing_export, test_drawing_paint): a bit of everything the writers know.
#include <cmath>

#include "opad/drawing/display.hpp"

inline opad::drawing::Display sample() {
  using namespace opad::drawing;
  opad::drawing::Display d;
  d.title = "Sample";
  const int visible = d.layer({"Visible", kInk, LineType::Continuous, 0.5});
  const int hidden = d.layer({"Hidden", kInk, LineType::Hidden, 0.25});
  const int red = d.layer({"Red centre", 0xFF0000, LineType::Center, 0.35});
  const int arabic = d.layer({"\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A", 0x0000FF, LineType::Continuous, 0.25});  // "عربي"
  d.line(visible, {0, 0}, {100, 0});
  d.line(visible, {100, 0}, {100.0 / 3, 50});
  d.polyline(visible, {{0, 0}, {10, 60}, {0, 60}}, true);
  d.circle(red, {50, 25}, 10);
  d.arc(red, {50, 25}, 15, 0, M_PI / 2);
  d.line(hidden, {0, 10}, {100, 10});
  Curve e;
  e.type = Curve::Type::Ellipse;
  e.c = {150, 25};
  e.r1 = 20;
  e.r2 = 10;
  e.rot = M_PI / 6;
  e.a0 = 0;
  e.a1 = 2 * M_PI;
  d.curve(visible, e);
  e.c = {150, 80};
  e.a1 = M_PI;
  d.curve(visible, e);
  Curve s;  // a quarter circle as a rational quadratic spline, radius 30 about (0, 100)
  s.type = Curve::Type::Spline;
  s.degree = 2;
  s.pts = {{30, 100}, {30, 130}, {0, 130}};
  s.weights = {1, std::sqrt(0.5), 1};
  s.knots = {0, 0, 0, 1, 1, 1};
  d.curve(visible, s);
  d.fill(arabic, {{{200, 0}, {210, 0}, {210, 10}, {200, 10}}, {{203, 3}, {207, 3}, {207, 7}, {203, 7}}});  // a square with a hole
  d.fill(arabic, {{{220, 0}, {230, 0}, {225, 8}}});                                                          // a triangle
  d.text(visible, "OPAD 1/3", {0, -10}, 3.5);
  d.text(visible, "Line one\n\xD8\xB3\xD8\xB7\xD8\xB1 {2}", {50, -10}, 2.5, 0, 1, 3);  // "سطر"
  linear_dimension(d, red, {0, 0}, {100, 0}, {1, 0}, {50, -20}, "100");
  radial_dimension(d, red, {50, 25}, 10, {75, 45}, "\xC3\x98" "20", true);
  return d;
}
