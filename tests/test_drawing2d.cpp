// 2D drawings in the view (app/Drawing2D.cpp): the ink of a drawing without a colour on every background (UI-10).
#include "Drawing2D.hpp"
#include "check.hpp"

using namespace drawing2d;

namespace {
Rgb hex(unsigned v) { return {((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0}; }
}  // namespace

TEST(contrast_is_the_wcag_ratio) {
  CHECK(std::abs(contrast(hex(0x000000), hex(0xffffff)) - 21) < 1e-9);
  CHECK(std::abs(contrast(hex(0x777777), hex(0x777777)) - 1) < 1e-9);
  CHECK(std::abs(contrast(hex(0x777777), hex(0xffffff)) - contrast(hex(0xffffff), hex(0x777777))) < 1e-12);
  CHECK(std::abs(contrast(hex(0x767676), hex(0xffffff)) - 4.54) < 0.01);  // the classic AA grey
}

// The evaluation measured 191 on 228 (1.3:1) and 191 on 255 (1.6:1): the old grey on the light theme and on white.
TEST(colour_7_takes_the_ink_of_its_background) {
  CHECK(contrast(hex(0xbfbfc7), hex(0xe4e5e8)) < 1.5);
  const unsigned backgrounds[] = {0x232529 /* dark theme */, 0xe4e5e8 /* light theme */, 0xffffff, 0x171c24, 0x979899 /* gradient middle */,
                                  0xc7c8c9, 0x66696b, 0x000000, 0x808080};
  for (unsigned b : backgrounds) {
    const Rgb ink = drawing2d::ink(hex(b));
    CHECK(ink == kInkOnDark || ink == kInkOnLight);
    CHECK(contrast(ink, hex(b)) >= contrast(ink == kInkOnDark ? kInkOnLight : kInkOnDark, hex(b)));
  }
  CHECK(drawing2d::ink(hex(0x232529)) == kInkOnDark && drawing2d::ink(hex(0x171c24)) == kInkOnDark && drawing2d::ink(hex(0x000000)) == kInkOnDark);
  CHECK(drawing2d::ink(hex(0xe4e5e8)) == kInkOnLight && drawing2d::ink(hex(0xffffff)) == kInkOnLight && drawing2d::ink(hex(0x979899)) == kInkOnLight);
  for (unsigned b : {0x232529u, 0xe4e5e8u, 0xffffffu, 0x171c24u, 0x979899u}) CHECK(contrast(drawing2d::ink(hex(b)), hex(b)) >= 4.5);
  CHECK(kInkOnDark == hex(0xe6e6e6) && kInkOnLight == hex(0x141414));
}

CHECK_MAIN()
