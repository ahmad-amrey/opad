#include "Drawing2D.hpp"

#include <algorithm>
#include <cmath>

namespace drawing2d {
double luminance(const Rgb& c) {
  auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  return 0.2126 * linear(c[0]) + 0.7152 * linear(c[1]) + 0.0722 * linear(c[2]);
}

double contrast(const Rgb& a, const Rgb& b) {
  const double la = luminance(a), lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Rgb ink(const Rgb& background) { return contrast(kInkOnDark, background) >= contrast(kInkOnLight, background) ? kInkOnDark : kInkOnLight; }
}  // namespace drawing2d
