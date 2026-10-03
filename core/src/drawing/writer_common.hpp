#pragma once
// Shared by the drawing writers (dxf_writer.cpp, svg_writer.cpp, display.cpp); not a public header.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <vector>

#include "opad/drawing/display.hpp"

namespace opad::drawing::detail {

// At most `decimals` decimals, trailing zeros dropped, never "-0" or an exponent; the locale plays no part.
inline std::string num(double v, int decimals) {
  if (!std::isfinite(v)) v = 0;
  char buf[64];
  const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, std::clamp(decimals, 0, 12));
  std::string s(buf, r.ptr);
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s == "-0" ? "0" : s;
}

inline bool full_turn(const Curve& c) { return c.a1 - c.a0 >= 2 * M_PI - 1e-9; }

using drawing::text_lines;

// The colour a primitive is drawn in: its own, else its layer's.
inline uint32_t color_of(const Display& d, const Prim& p) {
  return p.rgb != kByLayer ? p.rgb : p.layer >= 0 && p.layer < static_cast<int>(d.layers.size()) ? d.layers[static_cast<size_t>(p.layer)].rgb : kInk;
}

}  // namespace opad::drawing::detail
