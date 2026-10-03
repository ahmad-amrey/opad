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

// A text's lines, each with the point on its baseline that its horizontal alignment refers to: lines 1.6 heights apart,
// the block placed by the vertical alignment (top: the first line's capitals touch the anchor; middle: the block's
// middle; bottom: the last line's descenders, 0.3 heights below its baseline; baseline: the first line's).
struct TextLine {
  std::string text;
  Vec2 at;
};
inline std::vector<TextLine> text_lines(const Prim& p) {
  std::vector<TextLine> lines(1);
  for (char ch : p.text) {
    if (ch == '\n') lines.emplace_back();
    else lines.back().text += ch;
  }
  const double pitch = 1.6 * p.height, block = p.height + pitch * static_cast<double>(lines.size() - 1);
  const double first = p.valign == 3 ? -p.height : p.valign == 2 ? block / 2 - p.height : p.valign == 1 ? block - p.height + 0.3 * p.height : 0;
  const Vec2 v{-std::sin(p.angle), std::cos(p.angle)};
  for (size_t i = 0; i < lines.size(); ++i) {
    const double up = first - pitch * static_cast<double>(i);
    lines[i].at = {p.at[0] + v[0] * up, p.at[1] + v[1] * up};
  }
  return lines;
}

// The colour a primitive is drawn in: its own, else its layer's.
inline uint32_t color_of(const Display& d, const Prim& p) {
  return p.rgb != kByLayer ? p.rgb : p.layer >= 0 && p.layer < static_cast<int>(d.layers.size()) ? d.layers[static_cast<size_t>(p.layer)].rgb : kInk;
}

}  // namespace opad::drawing::detail
