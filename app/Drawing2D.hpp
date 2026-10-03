#pragma once
// 2D drawings in the view (area drawing2d): what is shown of a drawing's layers, independent of Qt and OCCT's view so
// tests/test_drawing2d covers it. The viewport, the Layers panel and the 2D vocabulary build on these.
#include <array>
#include <string>

namespace drawing2d {
using Rgb = std::array<double, 3>;  // 0..1, sRGB

// WCAG 2 relative luminance and contrast ratio (1 .. 21).
double luminance(const Rgb& c);
double contrast(const Rgb& a, const Rgb& b);
// The drawing's own foreground (DXF colour 7 and every entity without a colour): #e6e6e6 on a dark background, #141414
// on a light one, whichever stands out more.
Rgb ink(const Rgb& background);
constexpr Rgb kInkOnDark{0xe6 / 255.0, 0xe6 / 255.0, 0xe6 / 255.0}, kInkOnLight{0x14 / 255.0, 0x14 / 255.0, 0x14 / 255.0};
}  // namespace drawing2d
