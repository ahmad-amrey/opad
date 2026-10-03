// Drawing text (UI-92, core/src/drawing_text.cpp): the bidi order of mixed Arabic, Hebrew, Latin and numbers; Arabic
// shaped (joining forms, lam-alef), right-to-left runs in visual order, characters the font lacks from a fallback font;
// outlines filled with their holes, aligned, fitted and wrapped. The shaping cases need a font with Arabic (Windows' Arial).
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepGProp.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <TopoDS_Face.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "check.hpp"
#include "drawing_text.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"
#include "opad/util.hpp"
#include "shx_font.hpp"

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

// Glyph faces mesh as the view meshes a drawing (its deflection for a 100 mm body) to their own area, letter by letter: a
// hole drawn across its outline would leave slivers and spikes.
TEST(glyph_faces_mesh_to_their_area) {
  if (!text_shaping()) return;
  TextOutliner t;
  for (const char* text : {"\xD8\xBA\xD8\xB1\xD9\x81\xD8\xA9 \xD8\xA7\xD9\x84\xD9\x86\xD9\x88\xD9\x85", "HELLO @&%8 \xC3\x98", "\xD8\xA9\xD8\xA9 \xD9\x87\xD9\x87"}) {
    for (double deflection : {0.005, 0.02, 0.08, 0.3}) {
    const auto shape = BRepBuilderAPI_Copy(t.outline(request(text), gp_Ax3()), true, false).Shape();  // glyphs are shared
    opad::mesh_shape(shape, deflection);
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face& face = TopoDS::Face(e.Current());
      GProp_GProps props;
      BRepGProp::SurfaceProperties(face, props);
      TopLoc_Location location;
      const auto mesh = BRep_Tool::Triangulation(face, location);
      double area = 0;
      for (int i = 1; mesh && i <= mesh->NbTriangles(); ++i) {
        int a, b, c;
        mesh->Triangle(i).Get(a, b, c);
        area += gp_Vec(mesh->Node(a), mesh->Node(b)).Crossed(gp_Vec(mesh->Node(a), mesh->Node(c))).Magnitude() / 2;
      }
      if (!(mesh && std::abs(area - std::abs(props.Mass())) < 0.03 * std::abs(props.Mass()) + 0.05)) std::printf("%s at %g: %g against %g\n", text, deflection, area, props.Mass());
      CHECK(mesh && std::abs(area - std::abs(props.Mass())) < 0.03 * std::abs(props.Mass()) + 0.05);
    }
    }
  }
}

namespace {
TextSpan span(const std::string& text, double size = 10) {
  TextSpan s;
  s.text = text;
  s.size = size;
  return s;
}
TextRequest spans(std::vector<TextSpan> parts) {
  TextRequest r = request("");
  r.spans = std::move(parts);
  return r;
}
// The edges that are not glyph outlines (rules, bars), as segments {x0, y0, x1, y1}.
std::vector<std::array<double, 4>> lines(const TopoDS_Shape& shape) {
  std::vector<std::array<double, 4>> out;
  for (TopExp_Explorer e(shape, TopAbs_EDGE, TopAbs_FACE); e.More(); e.Next()) {
    const auto b = extent(e.Current());
    out.push_back(b);
  }
  return out;
}
}  // namespace

// TODO 11 UI-92: a text in parts of their own formats (MTEXT inline formatting): sizes, colours, slants, tracking, lines
// under, over and through, stacked fractions and tolerances, paragraphs aligned on their own; letters join across parts.
TEST(parts_of_a_text_keep_their_own_formats) {
  if (!text_shaping()) return;
  TextOutliner t;
  const gp_Ax3 at;
  // A part twice the size: the line as high as its capitals, the other part's as high as its own.
  auto big = span("HH", 20);
  auto both = extent(t.outline(spans({span("HH"), big}), at));
  CHECK(std::abs(both[3] - 20) < 0.05 && std::abs(both[1]) < 0.05);
  // A coloured part goes into its colour's compound, the rest stays.
  auto red = span("EE");
  red.color = 0xFF0000;
  std::map<uint32_t, TopoDS_Compound> colored;
  const auto rest = t.outline(spans({span("HH"), red}), at, &colored);
  CHECK(colored.size() == 1 && colored.count(0xFF0000) && count(colored[0xFF0000], TopAbs_FACE) == 2 && count(rest, TopAbs_FACE) == 2);
  CHECK(extent(colored[0xFF0000])[0] > extent(rest)[2]);  // after the plain part
  // Underline below the baseline as long as its part, one rule; overline above the capitals; strike-through in their middle.
  auto under = span("HHH");
  under.underline = true;
  auto ruled = lines(t.outline(spans({span("A"), under, span("A")}), at));
  CHECK(ruled.size() == 1 && ruled[0][1] < 0 && ruled[0][1] > -3 && ruled[0][2] - ruled[0][0] > 15);
  under.underline = false, under.overline = true, under.strike = true;
  ruled = lines(t.outline(spans({under}), at));
  CHECK(ruled.size() == 2 && std::max(ruled[0][1], ruled[1][1]) > 10 && std::abs(std::min(ruled[0][1], ruled[1][1]) - 5) < 0.05);
  // A fraction: 1 over 2 at 70 %, a bar between them; a tolerance has none; a diagonal one has its slash.
  auto fraction = span("1");
  fraction.stack = '/';
  fraction.bottom = "2";
  const auto f = t.outline(spans({span("H"), fraction}), at);
  const auto bars = lines(f);
  const auto fb = extent(f);
  CHECK(bars.size() == 1 && std::abs(bars[0][1] - 5) < 0.05 && std::abs(bars[0][3] - 5) < 0.05);
  CHECK(fb[3] > 12 && fb[1] < -2 && count(f, TopAbs_FACE) == 3);  // above the capitals and below the baseline
  fraction.stack = '^';
  CHECK(lines(t.outline(spans({fraction}), at)).empty());
  fraction.stack = '#';
  const auto slash = lines(t.outline(spans({fraction}), at));
  CHECK(slash.size() == 1 && slash[0][3] - slash[0][1] > 9);
  // Oblique: an upright stroke leans right at its top; tracking spreads the letters.
  auto leaning = span("I");
  leaning.oblique = 15 * 3.14159265358979 / 180;
  const auto upright = extent(t.outline(spans({span("I")}), at)), leant = extent(t.outline(spans({leaning}), at));
  CHECK(leant[2] - upright[2] > 2.4 && leant[2] - upright[2] < 2.9);  // 10 * tan 15 = 2.68 further at the top
  auto spread = span("HHHH");
  spread.tracking = 2;
  CHECK(extent(t.outline(spans({spread}), at))[2] > 1.6 * extent(t.outline(spans({span("HHHH")}), at))[2]);
  // Paragraphs: the second one centred on its own, under a line twice as far down for its larger text.
  auto second = span("\nHH", 20);
  auto r = spans({span("HHHHHH"), second});
  r.spacing = 50.0 / 3;
  r.justify = {-1, TextRequest::Center};
  const auto two = t.shape(r);
  CHECK(two.size() == 2);
  const auto p = extent(t.outline(r, at));
  CHECK(std::abs(p[1] + 2 * 50.0 / 3) < 0.05);  // the second baseline 2 x 50/3 down
  r.spans[1].text = "\n";  // an empty second paragraph keeps its place
  r.spans.push_back(span("\nH"));
  CHECK(t.shape(r).size() == 3);
  // Arabic letters join across parts: beh beh in one part and beh in another (another colour) are the word's forms.
  if (arabic(t)) {
    const std::string beh = "\xD8\xA8";
    auto tail = span(beh);
    tail.color = 0x00FF00;
    const auto word = t.shape(request(beh + beh + beh))[0], parted = t.shape(spans({span(beh + beh), tail}))[0];
    CHECK(word.glyphs.size() == 3 && parted.glyphs.size() == 3);
    for (size_t i = 0; i < 3 && parted.glyphs.size() == 3; ++i) CHECK(word.glyphs[i].glyph == parted.glyphs[i].glyph);
  }
}

namespace {
// A shape font made here (UI-92): 'A' a stroke 10 up, then 6 on; 'B' a full octant circle of radius 5 beside it; 'C' two
// A's as subshapes; 'D' a half circle by bulge, below its chord; 'E' a fractional arc of radius 3 from 55 to 96 degrees.
// Above 10, below 2. As a shapes 1.0 file, or a unifont (numbered by code point, subshapes in two bytes).
std::vector<uint8_t> shapeFont(bool unifont) {
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> shapes = {
      {0, {'T', 0, 10, 2, 0, 0}},
      {'A', {0, 0x01, 0xA4, 0x02, 0xAC, 0x60, 0}},  // pen down first: a subshape goes on in its caller's pen
      {'B', {0, 0x02, 0x08, 10, 5, 0x01, 0x0A, 5, 0x00, 0x02, 0x08, 2, uint8_t(-5), 0}},
      {'C', unifont ? std::vector<uint8_t>{0, 7, 0, 'A', 7, 0, 'A', 0} : std::vector<uint8_t>{0, 7, 'A', 7, 'A', 0}},
      {'D', {0, 0x0C, 10, 0, 127, 0}},
      {'E', {0, 0x0B, 56, 34, 0, 3, 0x12, 0}},
  };
  if (unifont) shapes[0].second.insert(shapes[0].second.end(), {0, 0});  // encoding, type
  shapes[0].second.push_back(0);
  const std::string signature = unifont ? "AutoCAD-86 unifont 1.0\r\n\x1a" : "AutoCAD-86 shapes 1.0\r\n\x1a";
  std::vector<uint8_t> out(signature.begin(), signature.end());
  auto u16 = [&out](size_t v) { out.push_back(uint8_t(v & 255)), out.push_back(uint8_t(v >> 8)); };
  if (unifont) {
    u16(shapes.size()), u16(0), u16(shapes[0].second.size());
    out.insert(out.end(), shapes[0].second.begin(), shapes[0].second.end());
    for (size_t k = 1; k < shapes.size(); ++k) {
      u16(shapes[k].first), u16(shapes[k].second.size());
      out.insert(out.end(), shapes[k].second.begin(), shapes[k].second.end());
    }
  } else {
    u16(0), u16('E'), u16(shapes.size());
    for (const auto& [number, bytes] : shapes) u16(number), u16(bytes.size());
    for (const auto& [number, bytes] : shapes) out.insert(out.end(), bytes.begin(), bytes.end());
  }
  return out;
}
std::array<double, 4> strokeBox(const ShxFont::Glyph& g) {
  std::array<double, 4> b{1e9, 1e9, -1e9, -1e9};
  for (const auto& s : g.strokes)
    for (const auto& p : s) b = {std::min(b[0], p.X()), std::min(b[1], p.Y()), std::max(b[2], p.X()), std::max(b[3], p.Y())};
  return b;
}
bool about(double a, double b, double tol = 1e-6) { return std::abs(a - b) < tol; }
}  // namespace

TEST(shape_fonts_draw_their_strokes) {
  for (bool unifont : {false, true}) {
    const auto font = ShxFont::parse(shapeFont(unifont));
    CHECK(font && font->unicode == unifont && font->above == 10 && font->below == 2);
    const auto* a = font->glyph('A');
    CHECK(a && a->strokes.size() == 1 && a->strokes[0].size() == 2 && about(a->strokes[0][1].Y(), 10) && about(a->advance.X(), 6) && about(a->advance.Y(), 0));
    const auto b = strokeBox(*font->glyph('B'));
    CHECK(about(b[0], 0, 0.1) && about(b[1], 0, 0.1) && about(b[2], 10) && about(b[3], 10, 0.1) && about(font->glyph('B')->advance.X(), 12));
    CHECK(font->glyph('C')->strokes.size() == 2 && about(font->glyph('C')->advance.X(), 12));  // two subshapes, one after the other
    const auto d = strokeBox(*font->glyph('D'));
    CHECK(about(d[1], -5, 0.1) && about(d[3], 0) && about(font->glyph('D')->advance.X(), 10));
    // The fractional arc ends 96 degrees round a centre that puts its start at 55 (offsets in 256ths of an octant).
    const auto& e = font->glyph('E')->strokes.at(0);
    const double a0 = 45 + 56 * 45.0 / 256, a1 = 90 + 34 * 45.0 / 256, rad = 3.14159265358979323846 / 180;
    const gp_XY centre = gp_XY(0, 0) - gp_XY(std::cos(a0 * rad), std::sin(a0 * rad)) * 3;
    CHECK(about((e.back() - centre - gp_XY(std::cos(a1 * rad), std::sin(a1 * rad)) * 3).Modulus(), 0, 1e-9));
    CHECK(font->glyph('Z') == nullptr);
  }
  CHECK(!ShxFont::parse(std::vector<uint8_t>{'n', 'o', 0x1a, 0, 0}));
}

TEST(text_in_a_shape_font_is_its_strokes) {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-shx-" + opad::new_uuid());
  std::filesystem::create_directory(dir);
  const auto bytes = shapeFont(true);
  std::ofstream(dir / "test.shx", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), long(bytes.size()));
  TextOutliner t({dir});
  TextRequest r = request("AB A");
  r.font = "test";  // as a style names it, without the extension
  const auto shape = t.outline(r, gp_Ax3());
  const auto box = extent(shape);
  // Height 10: one unit a millimetre; strokes only; 'A' 6 on, 'B' 12, a missing space two thirds of the height.
  CHECK(count(shape, TopAbs_FACE) == 0 && count(shape, TopAbs_EDGE) > 10 && about(box[0], 0) && about(box[3], 10, 0.1) && about(box[1], 0, 0.1));
  CHECK(about(box[2], 6 + 12 + 10.0 * 2 / 3, 1e-6));
  r.h = TextRequest::Right;
  r.v = TextRequest::Top;
  r.size = 20;
  const auto right = extent(t.outline(r, gp_Ax3()));
  CHECK(about(right[3], 0, 0.2) && about(right[1], -20, 0.2) && right[2] < 0 && right[2] > -40);
  r = request("AZ");  // a character the font lacks: the whole text in the outline font
  r.font = "test.shx";
  if (text_shaping()) CHECK(count(t.outline(r, gp_Ax3()), TopAbs_FACE) > 0);
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  // AutoCAD's own txt.shx, when DWG TrueView or AutoCAD is installed here.
  for (const auto& folder : shx_folders())
    if (const auto txt = ShxFont::load(folder / "txt.shx")) {
      CHECK(txt->above > 0 && txt->glyph('A') && txt->glyph(0xB0) && txt->glyph(0xB1) && txt->glyph(0xD8));
      break;
    }
}

CHECK_MAIN()
