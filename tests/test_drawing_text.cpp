// Drawing text (UI-92, core/src/drawing_text.cpp): the bidi order of mixed Arabic, Hebrew, Latin and numbers; Arabic
// shaped (joining forms, lam-alef), right-to-left runs in visual order, characters the font lacks from a fallback font;
// outlines filled with their holes, aligned, fitted and wrapped. The shaping cases need a font with Arabic (Windows' Arial).
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <filesystem>
#include <map>
#include <array>
#include <string>
#include <vector>

#include "check.hpp"
#include "drawing_text.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/util.hpp"

using namespace opad::detail;

namespace {
std::u32string u(const std::string& s) { return utf32(s); }
std::array<double, 4> extent(const TopoDS_Shape& shape) {
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  if (box.IsVoid()) return {0, 0, -1, -1};
  double a, b, c, d, e, f;
  box.Get(a, b, c, d, e, f);
  return {a, b, d, e};
}
int count(const TopoDS_Shape& shape, TopAbs_ShapeEnum kind) {
  int n = 0;
  for (TopExp_Explorer e(shape, kind); e.More(); e.Next()) ++n;
  return n;
}
TextRequest request(const std::string& text, double size = 10) {
  TextRequest r;
  r.text = text;
  r.size = size;
  r.cap = true;
  return r;
}
// A font with Arabic is there (the default sans-serif): the shaping cases run.
bool arabic(TextOutliner& t) {
  if (!text_shaping()) return false;
  const auto lines = t.shape(request("\xD8\xA8"));  // beh
  return lines.size() == 1 && lines[0].glyphs.size() == 1 && lines[0].glyphs[0].glyph != 0;
}
}  // namespace

TEST(bidi_puts_right_to_left_runs_and_their_numbers_in_visual_order) {
  CHECK(bidi_visual(u("abc")) == u("abc"));
  CHECK(bidi_visual(u("\xD8\xA7\xD8\xA8\xD8\xAA")) == u("\xD8\xAA\xD8\xA8\xD8\xA7"));  // alef beh teh -> teh beh alef
  CHECK(bidi_visual(u("abc \xD8\xA7\xD8\xA8")) == u("abc \xD8\xA8\xD8\xA7"));  // Arabic inside left-to-right text
  // An Arabic paragraph: Latin and the numbers keep their own order and land on the left of what follows them.
  CHECK(bidi_visual(u("\xD8\xA7\xD8\xA8 123")) == u("123 \xD8\xA8\xD8\xA7"));
  CHECK(bidi_visual(u("\xD8\xA7\xD8\xA8 abc")) == u("abc \xD8\xA8\xD8\xA7"));
  CHECK(bidi_visual(u("\xD8\xA7 3.5 \xD8\xA8")) == u("\xD8\xA8 3.5 \xD8\xA7"));  // one separator between digits joins them
  CHECK(bidi_visual(u("\xD7\xA9\xD7\x9C")) == u("\xD7\x9C\xD7\xA9"));  // Hebrew
  CHECK(bidi_visual(u("\xD8\xA7\xD8\xA8 ")) == u(" \xD8\xA8\xD8\xA7"));  // trailing space at the paragraph's (right-to-left) level
  int base = -1;
  CHECK(bidi_levels(u("12 \xD8\xA7"), base) == std::vector<uint8_t>({2, 2, 1, 1}) && base == 1);  // the first strong letter decides
  CHECK(bidi_levels(u("ab \xD8\xA7\xD9\x8E"), base) == std::vector<uint8_t>({0, 0, 0, 1, 1}) && base == 0);  // a mark keeps its letter's level
}

TEST(arabic_is_shaped_joined_and_in_visual_order) {
  TextOutliner t;
  if (!arabic(t)) { std::puts("no font with Arabic: shaping cases skipped"); return; }
  const std::string beh = "\xD8\xA8", yeh = "\xD9\x8A", teh = "\xD8\xAA", lam = "\xD9\x84", alef = "\xD8\xA7";
  // Joined forms: the word's first beh is not the isolated one, and four in a row are far narrower than four alone.
  const auto alone = t.shape(request(beh)), word = t.shape(request(beh + yeh + teh));
  CHECK(word.size() == 1 && word[0].glyphs.size() == 3 && word[0].direction == 1);
  CHECK(word[0].glyphs.back().cluster == 0 && word[0].glyphs.front().cluster == 2);  // right to left: the first letter on the right
  CHECK(word[0].glyphs.back().glyph != alone[0].glyphs[0].glyph);
  CHECK(t.shape(request(beh + beh + beh + beh))[0].width < 0.75 * 4 * alone[0].width);
  CHECK(t.shape(request(lam + alef))[0].glyphs.size() == 1);  // lam-alef ligature
  // Mixed: Latin first (left to right), the Arabic word after it on the right, its numbers left to right.
  const auto mixed = t.shape(request("abc " + beh + yeh + teh))[0];
  double latin = 0, arabicLeft = 1e9;
  for (const auto& g : mixed.glyphs) {
    if (g.cluster <= 2) latin = std::max(latin, g.x);
    if (g.cluster >= 4) arabicLeft = std::min(arabicLeft, g.x);
  }
  CHECK(mixed.direction == 0 && latin < arabicLeft);
  const auto numbers = t.shape(request(beh + yeh + teh + " 12"))[0];
  CHECK(numbers.direction == 1 && numbers.glyphs[0].cluster == 4 && numbers.glyphs[1].cluster == 5 && numbers.glyphs.back().cluster == 0);
  // A character the font lacks comes from another font (when the system has one with it).
  const auto han = t.shape(request("A\xE4\xB8\xAD"))[0];
  CHECK(han.glyphs.size() == 2);
  if (han.glyphs[1].glyph != 0) CHECK(han.glyphs[1].font != han.glyphs[0].font);
}

TEST(outlines_are_filled_with_their_holes_aligned_fitted_and_wrapped) {
  TextOutliner t;
  if (!text_shaping()) return;
  const gp_Ax3 at;
  const auto o = t.outline(request("O"), at);
  CHECK(count(o, TopAbs_FACE) == 1 && count(o, TopAbs_WIRE) == 2);  // the counter is a hole
  CHECK(count(t.outline(request("B"), at), TopAbs_WIRE) == 3);
  CHECK(count(t.outline(request("i"), at), TopAbs_FACE) == 2);
  auto h = extent(t.outline(request("H"), at));
  CHECK(std::abs(h[1]) < 0.05 && std::abs(h[3] - 10) < 0.05);  // the capitals as high as asked, on the baseline
  auto r = request("HH");
  r.h = TextRequest::Right;
  r.v = TextRequest::Top;
  h = extent(t.outline(r, at));
  CHECK(h[2] < 0.01 && h[2] > -2 && std::abs(h[3]) < 0.05);
  r.h = TextRequest::Center;
  r.v = TextRequest::Middle;
  h = extent(t.outline(r, at));
  CHECK(std::abs((h[0] + h[2]) / 2) < 0.5 && std::abs((h[1] + h[3]) / 2) < 0.05);
  // Fit: as long as asked, as high; aligned: larger both ways.
  r = request("HELL");
  r.fit = 100;
  h = extent(t.outline(r, at));
  CHECK(h[2] - h[0] > 95 && h[2] - h[0] < 100.5 && std::abs(h[3] - 10) < 0.05);
  r.aligned = true;
  const auto natural = t.shape(request("HELL"))[0].width;
  const auto big = extent(t.outline(r, at));
  CHECK(big[3] > 10.5 && std::abs(big[3] / (h[3] - h[1]) - 1) > 0.05 && natural > 0);
  // Wrapped at spaces: three lines 5/3 of the height apart, the first one's capitals at the top.
  r = request("AAA BBB EEE");
  r.wrap = 30;
  r.spacing = 50.0 / 3;
  r.v = TextRequest::Top;
  CHECK(t.shape(r).size() == 3);
  h = extent(t.outline(r, at));
  CHECK(std::abs(h[3]) < 0.05 && std::abs(h[1] + 10 + 2 * 50.0 / 3) < 0.05);
  // Placed: in the plane of `at`, turned with it.
  const auto turned = extent(t.outline(request("H"), gp_Ax3(gp_Pnt(100, 50, 0), gp::DZ(), gp::DY())));
  CHECK(std::abs(turned[2] - 100) < 0.05 && turned[0] < 91 && turned[1] > 50);
}

// SVG text sits on its baseline (y), shaped as DXF text is; text-anchor end puts its right end on x.
TEST(svg_text_is_shaped_on_its_baseline) {
  if (!text_shaping()) return;
  const auto dir = std::filesystem::temp_directory_path() / ("opad-text-" + opad::new_uuid());
  std::filesystem::create_directory(dir);
  opad::write_text_file(dir / "text.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100mm\" viewBox=\"0 0 100 100\">"
                                          "<g id=\"Latin\"><text x=\"10\" y=\"40\" font-size=\"20\" font-family=\"Arial, sans-serif\">H</text></g>"
                                          "<g id=\"Arabic\"><text x=\"90\" y=\"80\" font-size=\"20\" text-anchor=\"end\">\xD8\xBA\xD8\xB1\xD9\x81\xD8\xA9</text></g></svg>");
  auto d = opad::Document::create();
  const auto result = opad::import_file(d, dir / "text.svg");
  const auto scene = opad::resolve(d);
  std::map<std::string, std::array<double, 4>> boxes;
  for (const auto& id : scene.all_bodies()) {
    Bnd_Box b = opad::node_world_bbox(d, scene, id);
    double a, c, z, e, f, g;
    b.Get(a, c, z, e, f, g);
    boxes[scene.node(id)->name] = {a, c, e, f};
  }
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  CHECK(result.warnings.empty() && boxes.size() == 2);
  CHECK(std::abs(boxes["Latin"][1] + 40) < 0.05 && boxes["Latin"][3] > -40 + 13);  // the baseline at y = 40 (SVG y down)
  CHECK(boxes["Arabic"][2] <= 90.01 && boxes["Arabic"][2] > 88 && boxes["Arabic"][0] < 80);
}

CHECK_MAIN()
