// Drawing text outlined as faces (drawing_text.hpp, UI-92).
#include "drawing_text.hpp"

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRep_Builder.hxx>
#include <Geom_BezierCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <gp_XY.hxx>
#ifdef OPAD_HAVE_FONT
#include <Font_FontMgr.hxx>
#include <Font_SystemFont.hxx>
#ifndef OPAD_HAVE_SHAPING
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Font_TextFormatter.hxx>
#include <StdPrs_BRepFont.hxx>
#include <StdPrs_BRepTextBuilder.hxx>
#endif
#endif
#ifdef OPAD_HAVE_SHAPING
#include <hb-ot.h>
#include <hb.h>
#endif

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>

#include "opad/util.hpp"
#include "shx_font.hpp"

namespace opad::detail {
namespace {

enum Bidi : uint8_t { L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON };

bool in(uint32_t c, uint32_t a, uint32_t b) { return c >= a && c <= b; }

bool mark(uint32_t c) {
#ifdef OPAD_HAVE_SHAPING
  const auto gc = hb_unicode_general_category(hb_unicode_funcs_get_default(), c);
  return gc == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK || gc == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
#else
  return in(c, 0x300, 0x36F) || in(c, 0x591, 0x5BD) || in(c, 0x610, 0x61A) || in(c, 0x64B, 0x65F) || c == 0x670 || in(c, 0x6D6, 0x6ED) ||
         in(c, 0x8D3, 0x8FF) || in(c, 0x20D0, 0x20FF) || in(c, 0xFE20, 0xFE2F);
#endif
}

// The Unicode bidi class, from the ranges that matter in drawings (Latin, Arabic, Hebrew and their numbers and signs);
// other letters are left to right, other symbols neutral.
Bidi bidi_class(uint32_t c) {
  if (c == 0x0A || c == 0x0D || in(c, 0x1C, 0x1E) || c == 0x85 || c == 0x2029) return B;
  if (c == 0x09 || c == 0x0B || c == 0x1F) return S;
  if (c == 0x0C || c == 0x20 || c == 0x1680 || in(c, 0x2000, 0x200A) || c == 0x2028 || c == 0x205F || c == 0x3000) return WS;
  if (in(c, '0', '9') || c == 0xB2 || c == 0xB3 || c == 0xB9 || in(c, 0x6F0, 0x6F9) || c == 0x2070 || in(c, 0x2074, 0x2079) ||
      in(c, 0x2080, 0x2089) || in(c, 0xFF10, 0xFF19))
    return EN;
  if (in(c, 0x660, 0x669) || c == 0x66B || c == 0x66C || in(c, 0x600, 0x605) || c == 0x6DD || c == 0x8E2) return AN;
  if (c == '+' || c == '-' || c == 0x207A || c == 0x207B || c == 0x208A || c == 0x208B || c == 0x2212 || c == 0xFE62 || c == 0xFE63 ||
      c == 0xFF0B || c == 0xFF0D)
    return ES;
  if (in(c, '#', '%') || in(c, 0xA2, 0xA5) || c == 0xB0 || c == 0xB1 || c == 0x609 || c == 0x60A || c == 0x66A || in(c, 0x2030, 0x2034) ||
      in(c, 0x20A0, 0x20CF) || c == 0x212E || c == 0x2213 || c == 0xFE5F || c == 0xFE69 || c == 0xFE6A || in(c, 0xFF03, 0xFF05) ||
      c == 0xFFE0 || c == 0xFFE1 || c == 0xFFE5 || c == 0xFFE6)
    return ET;
  if (c == ',' || c == '.' || c == '/' || c == ':' || c == 0xA0 || c == 0x60C || c == 0x202F || c == 0x2044 || c == 0xFE50 || c == 0xFE52 ||
      c == 0xFE55 || c == 0xFF0C || c == 0xFF0E || c == 0xFF0F || c == 0xFF1A)
    return CS;
  if (c == 0x200E) return L;
  if (c == 0x200F) return R;
  if (c == 0x61C) return AL;
  if (c < 0x20 || in(c, 0x7F, 0x9F) || c == 0xAD || in(c, 0x200B, 0x200D) || in(c, 0x202A, 0x202E) || in(c, 0x2060, 0x2069) || c == 0xFEFF) return BN;
  if (mark(c)) return NSM;
  if (in(c, 0x590, 0x5FF) || in(c, 0x7C0, 0x85F) || in(c, 0xFB1D, 0xFB4F) || in(c, 0x10800, 0x10FFF) || in(c, 0x1E800, 0x1EDFF) ||
      in(c, 0x1EF00, 0x1EFFF))
    return R;
  if (in(c, 0x600, 0x7BF) || in(c, 0x860, 0x8FF) || (in(c, 0xFB50, 0xFDFF) && c != 0xFD3E && c != 0xFD3F) || in(c, 0xFE70, 0xFEFE) ||
      in(c, 0x1EE00, 0x1EEFF))
    return AL;
#ifdef OPAD_HAVE_SHAPING
  switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), c)) {
    case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER: case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER: case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER: case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER: case HB_UNICODE_GENERAL_CATEGORY_LETTER_NUMBER:
      return L;
    case HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR: return WS;
    default: return ON;
  }
#else
  return (in(c, 'A', 'Z') || in(c, 'a', 'z') || in(c, 0xC0, 0x24F) || in(c, 0x370, 0x58F) || c >= 0x900) && !in(c, 0x2000, 0x2BFF) ? L : ON;
#endif
}

bool space(uint32_t c) { return c == ' ' || c == '\t' || c == 0x3000 || in(c, 0x2000, 0x200A); }
bool ideograph(uint32_t c) { return in(c, 0x2E80, 0x9FFF) || in(c, 0xAC00, 0xD7AF) || in(c, 0xF900, 0xFAFF) || in(c, 0x20000, 0x3FFFF); }

}  // namespace

std::u32string utf32(const std::string& s) {
  std::u32string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    uint32_t code = n == 1 ? c : n == 2 ? c & 0x1F : n == 3 ? c & 0x0F : c & 0x07;
    bool ok = n > 0 && i + size_t(n) <= s.size();
    for (int k = 1; ok && k < n; ++k) {
      const auto d = static_cast<unsigned char>(s[i + size_t(k)]);
      ok = (d & 0xC0) == 0x80;
      code = code << 6 | (d & 0x3F);
    }
    out += ok ? code : 0xFFFD;
    i += ok ? size_t(n) : 1;
  }
  return out;
}

std::vector<uint8_t> bidi_levels(const std::u32string& text, int& base) {
  const size_t n = text.size();
  std::vector<Bidi> t(n);
  for (size_t i = 0; i < n; ++i) t[i] = bidi_class(text[i]);
  const std::vector<Bidi> original = t;
  base = 0;
  for (Bidi c : t)
    if (c == L || c == R || c == AL) { base = c == L ? 0 : 1; break; }
  const Bidi sos = base ? R : L;
  // W1: marks (and format characters) take the class before them. W2: numbers after Arabic letters are Arabic numbers.
  // W3: Arabic letters are right to left.
  Bidi strong = sos;
  for (size_t i = 0; i < n; ++i) {
    if (t[i] == NSM || t[i] == BN) t[i] = i ? t[i - 1] : sos;
    if (t[i] == L || t[i] == R || t[i] == AL) strong = t[i];
    else if (t[i] == EN && strong == AL) t[i] = AN;
  }
  for (auto& c : t)
    if (c == AL) c = R;
  // W4: one separator between two numbers of a kind joins them. W5: terminators beside European numbers are numbers.
  // W6: the separators and terminators left are neutral.
  for (size_t i = 1; i + 1 < n; ++i) {
    if (t[i] == ES && t[i - 1] == EN && t[i + 1] == EN) t[i] = EN;
    else if (t[i] == CS && (t[i - 1] == EN || t[i - 1] == AN) && t[i + 1] == t[i - 1]) t[i] = t[i - 1];
  }
  for (size_t i = 0; i < n;) {
    if (t[i] != ET) { ++i; continue; }
    size_t j = i;
    while (j < n && t[j] == ET) ++j;
    if ((i > 0 && t[i - 1] == EN) || (j < n && t[j] == EN))
      for (size_t k = i; k < j; ++k) t[k] = EN;
    i = j;
  }
  for (auto& c : t)
    if (c == ES || c == ET || c == CS) c = ON;
  // W7: European numbers after left-to-right text are left to right.
  strong = sos;
  for (auto& c : t) {
    if (c == L || c == R) strong = c;
    else if (c == EN && strong == L) c = L;
  }
  // N1, N2: neutrals between two of one direction take it (numbers count as right to left), others the paragraph's.
  auto direction = [](Bidi c) { return c == L ? 0 : (c == R || c == EN || c == AN) ? 1 : -1; };
  for (size_t i = 0; i < n;) {
    if (direction(t[i]) >= 0) { ++i; continue; }
    size_t j = i;
    while (j < n && direction(t[j]) < 0) ++j;
    const int before = i ? direction(t[i - 1]) : base, after = j < n ? direction(t[j]) : base;
    for (size_t k = i; k < j; ++k) t[k] = (before == after ? before : base) ? R : L;
    i = j;
  }
  // I1, I2; L1: tabs, and white space before them or at the end, at the paragraph's level.
  std::vector<uint8_t> levels(n, uint8_t(base));
  for (size_t i = 0; i < n; ++i) {
    if (base == 0) levels[i] = t[i] == R ? 1 : (t[i] == AN || t[i] == EN) ? 2 : 0;
    else levels[i] = (t[i] == L || t[i] == EN || t[i] == AN) ? 2 : 1;
  }
  bool trailing = true;
  for (size_t i = n; i-- > 0;) {
    const Bidi c = original[i];
    if (c == S || c == B) { levels[i] = uint8_t(base); trailing = true; }
    else if (trailing && (c == WS || c == BN)) levels[i] = uint8_t(base);
    else trailing = false;
  }
  return levels;
}

namespace {
// L2: the order in which runs (or characters) of these levels are shown, left to right.
template <class Level>
std::vector<size_t> visual_order(const std::vector<Level>& levels) {
  std::vector<size_t> order(levels.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  if (levels.empty()) return order;
  int top = 0, lowestOdd = 255;
  for (auto l : levels) {
    top = std::max(top, int(l));
    if (l & 1) lowestOdd = std::min(lowestOdd, int(l));
  }
  for (int level = top; level >= lowestOdd; --level)
    for (size_t i = 0; i < order.size();) {
      if (int(levels[order[i]]) < level) { ++i; continue; }
      size_t j = i;
      while (j < order.size() && int(levels[order[j]]) >= level) ++j;
      std::reverse(order.begin() + long(i), order.begin() + long(j));
      i = j;
    }
  return order;
}
}  // namespace

std::u32string bidi_visual(const std::u32string& text) {
  int base = 0;
  const auto levels = bidi_levels(text, base);
  std::u32string out;
  for (size_t i : visual_order(levels)) out += text[i];
  return out;
}

// Text in an AutoCAD shape font (.shx) the user has: its strokes, laid out by each character's pen advance, as AutoCAD
// does (no shaping: a shape font holds no joining forms). Text with a character the font lacks is left to the outliner.
namespace {
struct ShxText {
  std::vector<std::filesystem::path> folders;
  std::map<std::string, std::shared_ptr<const ShxFont>> fonts;  // by the name a style gives
  std::map<std::tuple<const ShxFont*, uint32_t, double, double>, TopoDS_Shape> glyphs;  // strokes per character and size

  std::shared_ptr<const ShxFont> font(const std::string& name) {
    if (const auto it = fonts.find(name); it != fonts.end()) return it->second;
    std::shared_ptr<const ShxFont> found;
    const auto first = name.find_first_not_of(' '), last = name.find_last_not_of(' ');
    std::string file = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    const auto slash = file.find_last_of("/\\"), dot = file.find_last_of('.');
    std::string ext = dot == std::string::npos || (slash != std::string::npos && dot < slash) ? "" : file.substr(dot);
    for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (ext.empty() && !file.empty()) file += ".shx";
    if (ext.empty() || ext == ".shx") {
      std::error_code error;
      const auto given = path_from_utf8(file);
      std::vector<std::filesystem::path> candidates{given};
      for (const auto& folder : folders) candidates.push_back(folder / given.filename());
      for (const auto& folder : shx_folders()) candidates.push_back(folder / given.filename());
      for (const auto& path : candidates)
        if (!found && !path.empty() && std::filesystem::is_regular_file(path, error)) found = ShxFont::load(path);
    }
    return fonts[name] = found;
  }

  const TopoDS_Shape& glyph(const ShxFont& font, uint32_t c, const ShxFont::Glyph& g, double kx, double ky) {
    auto& shape = glyphs[{&font, c, kx, ky}];
    if (!shape.IsNull()) return shape;
    BRep_Builder builder;
    TopoDS_Compound strokes;
    builder.MakeCompound(strokes);
    for (const auto& stroke : g.strokes) {
      BRepBuilderAPI_MakePolygon polygon;
      for (const auto& p : stroke) polygon.Add(gp_Pnt(p.X() * kx, p.Y() * ky, 0));
      if (polygon.IsDone()) builder.Add(strokes, polygon.Wire());
    }
    return shape = strokes;
  }

  // Null when the text is not in a shape font found here or uses a character it lacks.
  std::optional<TopoDS_Shape> outline(const TextRequest& r, const gp_Ax3& at) {
    const auto f = font(r.font);
    if (!f || !(f->above > 0)) return std::nullopt;
    const std::u32string text = utf32(r.text);
    const ShxFont::Glyph* spaceGlyph = f->glyph(' ');
    const double spaceAdvance = spaceGlyph ? spaceGlyph->advance.X() : f->above * 2 / 3;
    for (char32_t c : text)
      if (c != U'\n' && !space(c) && !f->glyph(c)) return std::nullopt;
    const double k = r.cap ? r.size / f->above : r.size / (f->above + f->below), width = r.width > 0 ? r.width : 1;
    if (!(k > 0) || !std::isfinite(k)) return std::nullopt;
    auto advance = [&](char32_t c) {
      const ShxFont::Glyph* g = f->glyph(c);
      return g ? g->advance.X() : space(c) ? spaceAdvance : 0.0;
    };
    // Lines (broken at spaces when wrapped), each its characters and width in vector units.
    std::vector<std::pair<std::u32string, double>> lines;
    const double wrap = r.wrap > 0 ? r.wrap / (k * width) : 0;
    size_t at0 = 0;
    while (at0 <= text.size()) {
      const size_t end = std::min(text.find(U'\n', at0), text.size());
      std::u32string line;
      double x = 0;
      for (size_t i = at0; i < end; ++i) {
        const char32_t c = text[i];
        if (wrap > 0 && !space(c) && x + advance(c) > wrap) {
          const size_t cut = line.find_last_of(U" \t");
          if (cut != std::u32string::npos && cut > 0) {
            std::u32string rest = line.substr(cut + 1), head = line.substr(0, cut);
            while (!head.empty() && space(head.back())) head.pop_back();
            double w = 0, v = 0;
            for (char32_t h : head) w += advance(h);
            for (char32_t h : rest) v += advance(h);
            lines.push_back({head, w});
            line = rest;
            x = v;
          }
        }
        line += c;
        x += advance(c);
      }
      lines.push_back({line, x});
      at0 = end + 1;
    }
    double kx = k * width, ky = k;
    if (r.fit > 0 && lines.size() == 1 && lines[0].second > 0) {
      kx = r.fit / lines[0].second;
      if (r.aligned) ky = kx / width;
    }
    const double spacing = r.spacing > 0 ? r.spacing * ky / k : 5.0 / 3.0 * f->above * ky;
    const double cap = f->above * ky, last = double(lines.size() - 1) * spacing;
    const double dy = r.v == TextRequest::Top ? -cap : r.v == TextRequest::Middle ? (last + cap) / 2 - cap : r.v == TextRequest::Bottom ? last
                      : r.v == TextRequest::Descent ? last + f->below * ky : 0;
    gp_Trsf place;
    place.SetDisplacement(gp_Ax3(gp::XOY()), at);
    const double qx = std::round(kx * 1e9) / 1e9, qy = std::round(ky * 1e9) / 1e9;
    BRep_Builder builder;
    TopoDS_Compound out;
    builder.MakeCompound(out);
    for (size_t i = 0; i < lines.size(); ++i) {
      const double w = lines[i].second * kx;
      gp_XY pen(r.h == TextRequest::Center ? -w / 2 : r.h == TextRequest::Right ? -w : 0, dy - double(i) * spacing);
      for (char32_t c : lines[i].first) {
        if (const ShxFont::Glyph* g = f->glyph(c)) {
          const TopoDS_Shape& shape = glyph(*f, uint32_t(c), *g, qx, qy);
          if (shape.NbChildren() > 0) {
            gp_Trsf move;
            move.SetTranslation(gp_Vec(pen.X(), pen.Y(), 0));
            builder.Add(out, shape.Moved(TopLoc_Location(place * move)));
          }
          pen += gp_XY(g->advance.X() * kx, g->advance.Y() * ky);
        } else {
          pen += gp_XY(spaceAdvance * kx, 0);
        }
      }
    }
    return TopoDS_Shape(out);
  }
};
}  // namespace

#ifdef OPAD_HAVE_FONT
namespace {
std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string trim(std::string s) {
  const auto a = s.find_first_not_of(" \t\"'"), b = s.find_last_not_of(" \t\"'");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::vector<std::string> split_families(const std::string& list) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= list.size()) {
    const size_t comma = std::min(list.find(',', at), list.size());
    if (auto name = trim(list.substr(at, comma - at)); !name.empty()) out.push_back(name);
    at = comma + 1;
  }
  return out;
}

struct FontRef {
  std::string path;  // UTF-8
  int index = 0;
};

// Font_FontMgr is one per process and is not made for threads: every use goes through this lock.
std::mutex& manager_lock() {
  static std::mutex lock;
  return lock;
}
// A family (or one of OCCT's aliases: sans-serif, serif, monospace); `any`: else whatever font the system has.
std::optional<FontRef> family_font(const std::string& family, bool any) {
  std::lock_guard<std::mutex> guard(manager_lock());
  Font_FontAspect aspect = Font_FontAspect_Regular;
  const Handle(Font_SystemFont) font = Font_FontMgr::GetInstance()->FindFont(
      TCollection_AsciiString(family.c_str()), any ? Font_StrictLevel_Any : Font_StrictLevel_Aliases, aspect, Standard_False);
  if (font.IsNull()) return std::nullopt;
  bool synthetic = false;
  int index = 0;
  const TCollection_AsciiString& path = font->FontPathAny(aspect, synthetic, index);
  if (path.IsEmpty()) return std::nullopt;
  return FontRef{path.ToCString(), index};
}

// A font file named in a drawing: as given, else by its name in one of the folders.
std::optional<FontRef> file_font(const std::string& name, const std::vector<std::filesystem::path>& folders) {
  std::error_code error;
  const auto given = path_from_utf8(name);
  std::vector<std::filesystem::path> candidates{given};
  for (const auto& folder : folders) candidates.push_back(folder / given.filename());
  for (const auto& path : candidates)
    if (!path.empty() && std::filesystem::is_regular_file(path, error)) {
      const auto u8 = path.u8string();
      return FontRef{std::string(u8.begin(), u8.end()), 0};
    }
  return std::nullopt;
}
}  // namespace
#endif

#if defined(OPAD_HAVE_FONT) && defined(OPAD_HAVE_SHAPING)
namespace {
constexpr int kUnits = 1 << 16;  // font units per em of every hb_font: positions and outlines in ems / kUnits

// A font file, loaded once per process (faces and immutable fonts are shared between threads).
struct FontFile {
  std::string path;
  hb_face_t* face = nullptr;
  hb_font_t* font = nullptr;
  double cap = 0.716, ascent = 0.905, descent = 0.212, gap = 0.033;  // ems
  bool has(uint32_t c) const {
    hb_codepoint_t glyph = 0;
    return hb_font_get_nominal_glyph(font, c, &glyph) && glyph != 0;
  }
};

const FontFile* load_font(const FontRef& ref) {
  static std::mutex lock;
  static std::map<std::pair<std::string, int>, std::unique_ptr<FontFile>> files;
  std::lock_guard<std::mutex> guard(lock);
  auto& slot = files[{ref.path, ref.index}];
  if (slot) return slot->font ? slot.get() : nullptr;
  slot = std::make_unique<FontFile>();
  hb_blob_t* blob = hb_blob_create_from_file_or_fail(ref.path.c_str());  // UTF-8 names, mapped
  if (!blob) return nullptr;
  hb_face_t* face = hb_face_create(blob, unsigned(std::max(0, ref.index)));
  hb_blob_destroy(blob);
  if (hb_face_get_glyph_count(face) == 0) { hb_face_destroy(face); return nullptr; }
  hb_font_t* font = hb_font_create(face);
  hb_font_set_scale(font, kUnits, kUnits);
  hb_font_make_immutable(font);
  FontFile& f = *slot;
  f.path = ref.path;
  f.face = face;
  f.font = font;
  hb_font_extents_t extents{};
  if (hb_font_get_h_extents(font, &extents) && extents.ascender > 0) {
    f.ascent = double(extents.ascender) / kUnits;
    f.descent = -double(extents.descender) / kUnits;
    f.gap = std::max(0.0, double(extents.line_gap) / kUnits);
  }
  hb_codepoint_t h = 0;
  hb_glyph_extents_t box{};
  hb_position_t capHeight = 0;
  if (hb_font_get_nominal_glyph(font, 'H', &h) && hb_font_get_glyph_extents(font, h, &box) && box.y_bearing > 0) f.cap = double(box.y_bearing) / kUnits;
  else if (hb_ot_metrics_get_position(font, HB_OT_METRICS_TAG_CAP_HEIGHT, &capHeight) && capHeight > 0) f.cap = double(capHeight) / kUnits;
  return &f;
}

const FontFile* load_family(const std::string& family, bool any = false) {
  const auto ref = family_font(family, any);
  return ref ? load_font(*ref) : nullptr;
}

// Families tried for a character the font lacks, by its script (Windows' first, then what Linux and macOS have).
const std::vector<std::string>& fallback_families(hb_script_t script) {
  static const std::vector<std::string> arabic{"Segoe UI", "Arial", "Tahoma", "Times New Roman", "Noto Sans Arabic", "Noto Naskh Arabic", "DejaVu Sans", "Geeza Pro"};
  static const std::vector<std::string> hebrew{"Segoe UI", "Arial", "Noto Sans Hebrew", "DejaVu Sans", "Arial Hebrew"};
  static const std::vector<std::string> cjk{"Microsoft YaHei", "SimSun", "Yu Gothic", "MS Gothic", "Noto Sans CJK SC", "Noto Sans CJK JP", "PingFang SC", "Hiragino Sans"};
  static const std::vector<std::string> hangul{"Malgun Gothic", "Gulim", "Noto Sans CJK KR", "Apple SD Gothic Neo"};
  static const std::vector<std::string> indic{"Nirmala UI", "Mangal", "Noto Sans Devanagari", "Kohinoor Devanagari"};
  static const std::vector<std::string> thai{"Leelawadee UI", "Tahoma", "Noto Sans Thai", "Thonburi"};
  static const std::vector<std::string> other{"Segoe UI", "Arial", "Segoe UI Symbol", "Segoe UI Emoji", "Arial Unicode MS", "DejaVu Sans", "Noto Sans", "Noto Sans Symbols", "FreeSans"};
  switch (script) {
    case HB_SCRIPT_ARABIC: case HB_SCRIPT_SYRIAC: case HB_SCRIPT_THAANA: case HB_SCRIPT_NKO: return arabic;
    case HB_SCRIPT_HEBREW: return hebrew;
    case HB_SCRIPT_HAN: case HB_SCRIPT_HIRAGANA: case HB_SCRIPT_KATAKANA: case HB_SCRIPT_BOPOMOFO: return cjk;
    case HB_SCRIPT_HANGUL: return hangul;
    case HB_SCRIPT_DEVANAGARI: case HB_SCRIPT_BENGALI: case HB_SCRIPT_GURMUKHI: case HB_SCRIPT_GUJARATI: case HB_SCRIPT_TAMIL:
    case HB_SCRIPT_TELUGU: case HB_SCRIPT_KANNADA: case HB_SCRIPT_MALAYALAM: case HB_SCRIPT_ORIYA: return indic;
    case HB_SCRIPT_THAI: case HB_SCRIPT_LAO: return thai;
    default: return other;
  }
}
Font_UnicodeSubset subset(hb_script_t script) {
  switch (script) {
    case HB_SCRIPT_ARABIC: case HB_SCRIPT_SYRIAC: case HB_SCRIPT_THAANA: case HB_SCRIPT_NKO: return Font_UnicodeSubset_Arabic;
    case HB_SCRIPT_HAN: case HB_SCRIPT_HIRAGANA: case HB_SCRIPT_KATAKANA: case HB_SCRIPT_BOPOMOFO: return Font_UnicodeSubset_CJK;
    case HB_SCRIPT_HANGUL: return Font_UnicodeSubset_Korean;
    default: return Font_UnicodeSubset_Western;
  }
}

// A glyph's outline as HarfBuzz draws it: contours of lines and quadratic or cubic Béziers, in font units.
struct Segment {
  int degree = 1;
  gp_XY p[4];
};
struct Pen {
  std::vector<std::vector<Segment>> contours;
};
hb_draw_funcs_t* draw_funcs() {
  static hb_draw_funcs_t* funcs = [] {
    hb_draw_funcs_t* f = hb_draw_funcs_create();
    hb_draw_funcs_set_move_to_func(f, [](hb_draw_funcs_t*, void* pen, hb_draw_state_t*, float, float, void*) {
      static_cast<Pen*>(pen)->contours.emplace_back();
    }, nullptr, nullptr);
    hb_draw_funcs_set_line_to_func(f, [](hb_draw_funcs_t*, void* pen, hb_draw_state_t* st, float x, float y, void*) {
      auto& c = static_cast<Pen*>(pen)->contours;
      if (!c.empty()) c.back().push_back({1, {gp_XY(st->current_x, st->current_y), gp_XY(x, y)}});
    }, nullptr, nullptr);
    hb_draw_funcs_set_quadratic_to_func(f, [](hb_draw_funcs_t*, void* pen, hb_draw_state_t* st, float cx, float cy, float x, float y, void*) {
      auto& c = static_cast<Pen*>(pen)->contours;
      if (!c.empty()) c.back().push_back({2, {gp_XY(st->current_x, st->current_y), gp_XY(cx, cy), gp_XY(x, y)}});
    }, nullptr, nullptr);
    hb_draw_funcs_set_cubic_to_func(f, [](hb_draw_funcs_t*, void* pen, hb_draw_state_t* st, float c1x, float c1y, float c2x, float c2y, float x, float y, void*) {
      auto& c = static_cast<Pen*>(pen)->contours;
      if (!c.empty()) c.back().push_back({3, {gp_XY(st->current_x, st->current_y), gp_XY(c1x, c1y), gp_XY(c2x, c2y), gp_XY(x, y)}});
    }, nullptr, nullptr);
    hb_draw_funcs_make_immutable(f);
    return f;
  }();
  return funcs;
}

bool inside(const std::vector<gp_XY>& poly, const gp_XY& q) {
  bool result = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    if ((poly[i].Y() > q.Y()) != (poly[j].Y() > q.Y()) &&
        q.X() < (poly[j].X() - poly[i].X()) * (q.Y() - poly[i].Y()) / (poly[j].Y() - poly[i].Y()) + poly[i].X())
      result = !result;
  return result;
}
double signed_area(const std::vector<gp_XY>& poly) {
  double a = 0;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) a += poly[j].X() * poly[i].Y() - poly[i].X() * poly[j].Y();
  return a / 2;
}

// A glyph's faces in the XY plane, font units scaled by kx and ky (mm per unit): its outer contours (those turning as the
// largest one does) with the holes inside them, so either winding a font uses comes out filled the same.
TopoDS_Shape glyph_faces(const FontFile& font, unsigned glyph, double kx, double ky) {
  Pen pen;
  hb_font_draw_glyph(font.font, glyph, draw_funcs(), &pen);
  struct Loop {
    TopoDS_Wire wire;
    std::vector<gp_XY> poly;
    double area = 0;
  };
  std::vector<Loop> loops;
  const double tiny = 1e-6;
  for (auto& contour : pen.contours) {
    std::vector<Segment> segments;
    for (auto s : contour) {
      for (int k = 0; k <= s.degree; ++k) s.p[k] = gp_XY(s.p[k].X() * kx, s.p[k].Y() * ky);
      if ((s.p[s.degree] - s.p[0]).Modulus() > tiny) segments.push_back(s);
    }
    if (segments.size() < 2) continue;
    const size_t n = segments.size();
    std::vector<TopoDS_Vertex> vertices(n);
    for (size_t i = 0; i < n; ++i) vertices[i] = BRepBuilderAPI_MakeVertex(gp_Pnt(segments[i].p[0].X(), segments[i].p[0].Y(), 0)).Vertex();
    BRepBuilderAPI_MakeWire wire;
    Loop loop;
    for (size_t i = 0; i < n; ++i) {
      const auto& s = segments[i];
      const gp_XY end = segments[(i + 1) % n].p[0];  // exactly where the next one starts
      if (s.degree == 1) {
        wire.Add(BRepBuilderAPI_MakeEdge(vertices[i], vertices[(i + 1) % n]).Edge());
      } else {
        TColgp_Array1OfPnt poles(1, s.degree + 1);
        for (int k = 0; k < s.degree; ++k) poles(k + 1) = gp_Pnt(s.p[k].X(), s.p[k].Y(), 0);
        poles(s.degree + 1) = gp_Pnt(end.X(), end.Y(), 0);
        wire.Add(BRepBuilderAPI_MakeEdge(new Geom_BezierCurve(poles), vertices[i], vertices[(i + 1) % n]).Edge());
        for (int k = 1; k < 4; ++k) {  // samples for the area and the nesting
          const double t = k / 4.0, u = 1 - t;
          const gp_XY a = s.p[0], b = s.p[1], c = s.degree == 2 ? end : s.p[2];
          loop.poly.push_back(s.degree == 2 ? a * (u * u) + b * (2 * u * t) + c * (t * t)
                                            : a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + end * (t * t * t));
        }
      }
      loop.poly.push_back(end);
    }
    if (!wire.IsDone()) continue;
    loop.wire = wire.Wire();
    loop.area = signed_area(loop.poly);
    if (std::abs(loop.area) > tiny * tiny) loops.push_back(std::move(loop));
  }
  BRep_Builder builder;
  TopoDS_Compound out;
  builder.MakeCompound(out);
  if (loops.empty()) return out;
  const bool outerSign = std::max_element(loops.begin(), loops.end(), [](const Loop& a, const Loop& b) { return std::abs(a.area) < std::abs(b.area); })->area > 0;
  std::vector<std::vector<size_t>> holes(loops.size());
  for (size_t i = 0; i < loops.size(); ++i) {
    if ((loops[i].area > 0) == outerSign) continue;
    size_t best = loops.size();
    for (size_t j = 0; j < loops.size(); ++j)
      if ((loops[j].area > 0) == outerSign && std::abs(loops[j].area) > std::abs(loops[i].area) && inside(loops[j].poly, loops[i].poly.front()) &&
          (best == loops.size() || std::abs(loops[j].area) < std::abs(loops[best].area)))
        best = j;
    if (best < loops.size()) holes[best].push_back(i);
  }
  auto oriented = [&](const Loop& l, bool ccw) { return (l.area > 0) == ccw ? l.wire : TopoDS::Wire(l.wire.Reversed()); };
  for (size_t i = 0; i < loops.size(); ++i) {
    if ((loops[i].area > 0) != outerSign) continue;
    try {
      BRepBuilderAPI_MakeFace face(gp_Pln(gp::XOY()), oriented(loops[i], true), Standard_True);
      for (size_t h : holes[i]) face.Add(oriented(loops[h], false));
      if (face.IsDone()) builder.Add(out, face.Face());
    } catch (const Standard_Failure&) {
    }
  }
  return out;
}
}  // namespace

struct TextOutliner::Impl {
  std::vector<std::filesystem::path> folders;
  ShxText shx;
  std::map<std::string, const FontFile*> primaries;  // by request font + family
  std::map<std::pair<const FontFile*, uint32_t>, const FontFile*> fallbacks;  // a character the font lacks -> the font that has it
  std::map<std::tuple<const FontFile*, unsigned, double, double>, TopoDS_Shape> glyphs;  // faces per glyph and size
  hb_buffer_t* buffer = hb_buffer_create();
  ~Impl() { hb_buffer_destroy(buffer); }

  const FontFile* primary(const TextRequest& r) {
    const std::string key = r.font + '\x1f' + r.family;
    if (const auto it = primaries.find(key); it != primaries.end()) return it->second;
    const FontFile* found = nullptr;
    const std::string name = trim(r.font);
    const auto dot = name.find_last_of('.');
    const std::string ext = dot == std::string::npos || name.find_first_of("/\\", dot) != std::string::npos ? "" : lower(name.substr(dot));
    if (ext == ".ttf" || ext == ".ttc" || ext == ".otf") {
      if (const auto ref = file_font(name, folders)) found = load_font(*ref);
    } else if (ext != ".shx" && ext != ".shp") {
      for (const auto& family : split_families(name))
        if (!found) found = load_family(family);
    }
    for (const auto& family : split_families(r.family))
      if (!found) found = load_family(family);
    if (!found) found = load_family("Arial", true);  // shape fonts and fonts not found: a plain sans-serif stands in
    return primaries[key] = found;
  }

  const FontFile* font_for(const FontFile* font, uint32_t c, hb_script_t script) {
    if (font->has(c)) return font;
    const auto key = std::make_pair(font, c);
    if (const auto it = fallbacks.find(key); it != fallbacks.end()) return it->second;
    const FontFile* found = nullptr;
    {
      Handle(Font_SystemFont) system;
      {
        std::lock_guard<std::mutex> guard(manager_lock());
        system = Font_FontMgr::GetInstance()->FindFallbackFont(subset(script), Font_FontAspect_Regular);
      }
      bool synthetic = false;
      int index = 0;
      if (!system.IsNull())
        if (const auto& path = system->FontPathAny(Font_FontAspect_Regular, synthetic, index); !path.IsEmpty())
          if (const FontFile* f = load_font({path.ToCString(), index}); f && f->has(c)) found = f;
    }
    for (const auto& family : fallback_families(script)) {
      if (found) break;
      if (const FontFile* f = load_family(family); f && f->has(c)) found = f;
    }
    return fallbacks[key] = found ? found : font;  // none has it: the font's own missing-glyph box
  }

  // One paragraph's lines, each shaped in visual order; wrapEm > 0 breaks lines longer than that (ems).
  std::vector<ShapedLine> paragraph(const std::u32string& text, const FontFile* primary, double wrapEm) {
    const size_t n = text.size();
    int base = 0;
    const std::vector<uint8_t> levels = bidi_levels(text, base);
    hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();
    // Scripts (common and inherited characters take their neighbours') and fonts (marks and joiners stay with their base).
    std::vector<hb_script_t> scripts(n, HB_SCRIPT_COMMON);
    hb_script_t last = HB_SCRIPT_COMMON;
    for (size_t i = 0; i < n; ++i) {
      const hb_script_t s = hb_unicode_script(unicode, text[i]);
      if (s != HB_SCRIPT_COMMON && s != HB_SCRIPT_INHERITED && s != HB_SCRIPT_UNKNOWN) last = s;
      scripts[i] = last;
    }
    for (size_t i = n; i-- > 0;)
      if (scripts[i] == HB_SCRIPT_COMMON && i + 1 < n) scripts[i] = scripts[i + 1];
    std::vector<const FontFile*> fonts(n, primary);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t c = text[i];
      const auto gc = hb_unicode_general_category(unicode, c);
      const bool attached = gc == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK || gc == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK ||
                            gc == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK || gc == HB_UNICODE_GENERAL_CATEGORY_FORMAT;
      if (i > 0 && (attached || (space(c) && fonts[i - 1]->has(c)))) fonts[i] = fonts[i - 1];
      else fonts[i] = font_for(primary, c, scripts[i]);
    }
    std::vector<std::pair<size_t, size_t>> ranges;
    if (wrapEm > 0 && n > 0) {
      std::vector<double> advance(n, 0.0), sum(n + 1, 0.0);
      line(text, 0, n, levels, base, scripts, fonts, &advance);
      for (size_t i = 0; i < n; ++i) sum[i + 1] = sum[i] + advance[i];
      size_t start = 0, opening = 0;  // the line's first character; the last place a line may start after it (0: none)
      for (size_t i = 0; i < n; ++i) {
        if (i > start && ((space(text[i - 1]) && !space(text[i])) || ideograph(text[i - 1]) || ideograph(text[i]))) opening = i;
        if (!space(text[i]) && sum[i + 1] - sum[start] > wrapEm && opening > start) {
          ranges.push_back({start, opening});
          start = opening;
        }
      }
      ranges.push_back({start, n});
    } else {
      ranges.push_back({0, n});
    }
    std::vector<ShapedLine> out;
    for (size_t k = 0; k < ranges.size(); ++k) {
      auto [a, b] = ranges[k];
      if (k + 1 < ranges.size())
        while (b > a && space(text[b - 1])) --b;  // the spaces a line was broken at
      out.push_back(line(text, a, b, levels, base, scripts, fonts, nullptr));
    }
    return out;
  }

  // Characters [a, b) of a paragraph as one line: runs of one level, script and font, in visual order, each shaped with
  // the paragraph around it as context (so letters join across runs). `advance`: each character's share, for wrapping.
  ShapedLine line(const std::u32string& text, size_t a, size_t b, std::vector<uint8_t> levels, int base, const std::vector<hb_script_t>& scripts,
                  const std::vector<const FontFile*>& fonts, std::vector<double>* advance) {
    ShapedLine out;
    out.text = text.substr(a, b - a);
    out.direction = base;
    for (size_t i = b; i > a && (space(text[i - 1]) || bidi_class(text[i - 1]) == BN); --i) levels[i - 1] = uint8_t(base);  // L1
    struct Run {
      size_t a, b;
      uint8_t level;
    };
    std::vector<Run> runs;
    for (size_t i = a; i < b; ++i) {
      if (runs.empty() || levels[i] != runs.back().level || fonts[i] != fonts[runs.back().a] || scripts[i] != scripts[runs.back().a]) runs.push_back({i, i + 1, levels[i]});
      else runs.back().b = i + 1;
    }
    std::vector<uint8_t> runLevels;
    for (const auto& r : runs) runLevels.push_back(r.level);
    double x = 0;
    for (size_t index : visual_order(runLevels)) {
      const Run& run = runs[index];
      const FontFile* font = fonts[run.a];
      hb_buffer_clear_contents(buffer);
      hb_buffer_add_utf32(buffer, reinterpret_cast<const uint32_t*>(text.data()), int(text.size()), unsigned(run.a), int(run.b - run.a));
      hb_buffer_set_direction(buffer, run.level & 1 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
      hb_buffer_set_script(buffer, scripts[run.a]);
      hb_buffer_guess_segment_properties(buffer);
      hb_shape(font->font, buffer, nullptr, 0);
      unsigned count = 0;
      const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buffer, &count);
      const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buffer, &count);
      for (unsigned g = 0; g < count; ++g) {
        const double dx = double(pos[g].x_advance) / kUnits;
        out.glyphs.push_back({font->path, font, info[g].codepoint, uint32_t(info[g].cluster - a), x + double(pos[g].x_offset) / kUnits,
                              double(pos[g].y_offset) / kUnits, dx});
        if (advance && info[g].cluster < advance->size()) (*advance)[info[g].cluster] += dx;
        x += dx;
      }
    }
    out.width = x;
    return out;
  }

  std::vector<ShapedLine> lines(const TextRequest& r, const FontFile* primary, double wrapEm) {
    std::vector<ShapedLine> out;
    const std::u32string text = utf32(r.text);
    size_t at = 0;
    while (at <= text.size()) {
      const size_t end = std::min(text.find(U'\n', at), text.size());
      auto shaped = paragraph(text.substr(at, end - at), primary, wrapEm);
      out.insert(out.end(), shaped.begin(), shaped.end());
      at = end + 1;
    }
    return out;
  }

  const TopoDS_Shape& glyph(const FontFile* font, unsigned id, double kx, double ky) {
    auto& shape = glyphs[{font, id, kx, ky}];
    if (shape.IsNull()) try {
        shape = glyph_faces(*font, id, kx / kUnits, ky / kUnits);
      } catch (const Standard_Failure&) {  // a broken outline loses its glyph, not the text
        BRep_Builder builder;
        TopoDS_Compound none;
        builder.MakeCompound(none);
        shape = none;
      }
    return shape;
  }
};

TextOutliner::TextOutliner(std::vector<std::filesystem::path> folders) : m(std::make_unique<Impl>()) {
  m->shx.folders = folders;
  m->folders = std::move(folders);
}
TextOutliner::~TextOutliner() = default;

std::vector<ShapedLine> TextOutliner::shape(const TextRequest& request) {
  const FontFile* primary = m->primary(request);
  if (!primary) return {};
  const double em = request.cap ? request.size / primary->cap : request.size;
  return m->lines(request, primary, request.wrap > 0 && em > 0 ? request.wrap / (em * request.width) : 0);
}

TopoDS_Shape TextOutliner::outline(const TextRequest& r, const gp_Ax3& at) {
  if (auto strokes = m->shx.outline(r, at)) return *strokes;
  const FontFile* primary = m->primary(r);
  if (!primary) return {};
  const double em = r.cap ? r.size / primary->cap : r.size, width = r.width > 0 ? r.width : 1;
  BRep_Builder builder;
  TopoDS_Compound out;
  builder.MakeCompound(out);
  if (!(em > 0) || !std::isfinite(em)) return out;
  const auto lines = m->lines(r, primary, r.wrap > 0 ? r.wrap / (em * width) : 0);
  double kx = em * width, ky = em;  // mm per em
  if (r.fit > 0 && lines.size() == 1 && lines[0].width > 0) {
    kx = r.fit / lines[0].width;
    if (r.aligned) ky = kx / width;
  }
  const double spacing = r.spacing > 0 ? r.spacing * ky / em : (primary->ascent + primary->descent + primary->gap) * ky;
  const double cap = primary->cap * ky, last = double(lines.size() - 1) * spacing;
  const double dy = r.v == TextRequest::Top ? -cap : r.v == TextRequest::Middle ? (last + cap) / 2 - cap : r.v == TextRequest::Bottom ? last
                    : r.v == TextRequest::Descent ? last + primary->descent * ky : 0;
  gp_Trsf place;
  place.SetDisplacement(gp_Ax3(gp::XOY()), at);
  // Glyph faces are kept per size and font units: a size that differs by rounding only shares them.
  const double qx = std::round(kx * 1e9) / 1e9, qy = std::round(ky * 1e9) / 1e9;
  for (size_t i = 0; i < lines.size(); ++i) {
    const double x0 = r.h == TextRequest::Center ? -lines[i].width * kx / 2 : r.h == TextRequest::Right ? -lines[i].width * kx : 0;
    const double y = dy - double(i) * spacing;
    for (const auto& g : lines[i].glyphs) {
      const TopoDS_Shape& shape = m->glyph(static_cast<const FontFile*>(g.face), g.glyph, qx, qy);
      if (shape.IsNull() || shape.NbChildren() == 0) continue;
      gp_Trsf move;
      move.SetTranslation(gp_Vec(x0 + g.x * kx, y + g.y * ky, 0));
      builder.Add(out, shape.Moved(TopLoc_Location(place * move)));
    }
  }
  return out;
}

bool text_shaping() { return true; }

#else  // OCCT's text: no shaping, every character as its font has it on its own

struct TextOutliner::Impl {
  std::vector<std::filesystem::path> folders;
  ShxText shx;
#ifdef OPAD_HAVE_FONT
  struct Font {
    Handle(StdPrs_BRepFont) font;
    double cap = 0;  // per em
  };
  std::map<std::string, Font> fonts;  // by file (or "") and em size
  std::map<std::string, double> capRatio;
#endif
};

TextOutliner::TextOutliner(std::vector<std::filesystem::path> folders) : m(std::make_unique<Impl>()) {
  m->shx.folders = folders;
  m->folders = std::move(folders);
}
TextOutliner::~TextOutliner() = default;
std::vector<ShapedLine> TextOutliner::shape(const TextRequest&) { return {}; }

TopoDS_Shape TextOutliner::outline(const TextRequest& r, const gp_Ax3& at) {
  if (auto strokes = m->shx.outline(r, at)) return *strokes;
#ifdef OPAD_HAVE_FONT
  std::string file;
  const auto dot = r.font.find_last_of('.');
  if (const std::string ext = dot == std::string::npos ? "" : lower(r.font.substr(dot)); ext == ".ttf" || ext == ".ttc" || ext == ".otf")
    if (const auto ref = file_font(trim(r.font), m->folders)) file = ref->path;
  auto init = [&](StdPrs_BRepFont& font, double size) {
    return file.empty() ? font.FindAndInit("Arial", Font_FA_Regular, size) : font.Init(NCollection_String(file.c_str()), size, 0);
  };
  auto ratio = m->capRatio.find(file);
  if (ratio == m->capRatio.end()) {
    double cap = -1;
    StdPrs_BRepFont probe;
    if (init(probe, 100.0)) {
      cap = 0.716;
      const TopoDS_Shape h = probe.RenderGlyph('H');
      Bnd_Box box;
      if (!h.IsNull()) BRepBndLib::Add(h, box);
      if (!box.IsVoid()) {
        double x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        if (y1 - y0 > 1) cap = (y1 - y0) / 100.0;
      }
    }
    ratio = m->capRatio.emplace(file, cap).first;
  }
  if (ratio->second < 0) return {};
  const double em = r.cap ? r.size / ratio->second : r.size;
  if (!(em > 1e-9)) return {};
  auto& f = m->fonts[file + "|" + std::to_string(em) + "|" + std::to_string(r.width)];
  if (f.font.IsNull()) {
    f.font = new StdPrs_BRepFont();
    if (!init(*f.font, em)) return {};
    if (std::abs(r.width - 1) > 1e-6) f.font->SetWidthScaling(float(r.width));
  }
  const double cap = ratio->second * em, spacing = r.spacing > 0 ? r.spacing : double(f.font->LineSpacing());
  const double last = double(std::count(r.text.begin(), r.text.end(), '\n')) * spacing;
  const double descent = std::abs(double(f.font->FTFont()->Descender())) * f.font->Scale();
  const double dy = r.v == TextRequest::Top ? -cap : r.v == TextRequest::Middle ? (last + cap) / 2 - cap : r.v == TextRequest::Bottom ? last
                    : r.v == TextRequest::Descent ? last + descent : 0;
  Handle(Font_TextFormatter) formatter = new Font_TextFormatter();
  formatter->SetupAlignment(r.h == TextRequest::Center ? Graphic3d_HTA_CENTER : r.h == TextRequest::Right ? Graphic3d_HTA_RIGHT : Graphic3d_HTA_LEFT,
                            Graphic3d_VTA_TOPFIRSTLINE);
  if (r.wrap > 0) formatter->SetWrapping(float(r.wrap / f.font->Scale()));
  formatter->Append(NCollection_String(r.text.c_str()), *f.font->FTFont());
  formatter->Format();
  const gp_Pnt origin = at.Location().Translated(gp_Vec(at.YDirection()) * dy);
  return StdPrs_BRepTextBuilder().Perform(*f.font, formatter, gp_Ax3(origin, at.Direction(), at.XDirection()));
#else
  (void)r;
  (void)at;
  return {};
#endif
}

bool text_shaping() { return false; }
#endif

}  // namespace opad::detail
