// DXF reader: drawings as AutoCAD, LibreDWG's dwg2dxf (OPAD's DWG path) and the ODA converter write them. Everything
// model space shows: curves become edges, fills (hatches, solids) and text become faces, grouped by layer and colour.
// A block is built once; a rigid placement shares it by location, a scaled or mirrored one gets a transformed copy.
// What cannot be shown (ACIS solids, images, proxies) is listed in the warnings, and a broken entity is skipped rather
// than failing the drawing.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // before OCCT, which includes it with parts left out (MultiByteToWideChar for old code pages)
#endif

#include "drawing_common.hpp"
#include "drawing_text.hpp"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <OSD_Parallel.hxx>
#include <OSD_ThreadPool.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "opad/util.hpp"

namespace opad::detail {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kNoColor = Drawing::kNoColor, kByLayer = 0x01000000u, kByBlock = 0x02000000u;

struct Pair {
  int code;
  std::string_view value;
};

std::string_view trimmed(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// Locale-independent: a Qt app on a German Linux desktop has a decimal comma in the C locale.
double parse_number(std::string_view text) {
  auto s = trimmed(text);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  double v = 0;
  const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (error != std::errc() || end != s.data() + s.size() || !std::isfinite(v) || std::abs(v) > 1e15)
    throw Error("invalid DXF number: " + std::string(s.substr(0, 40)));
  return v;
}

std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

void append_utf8(std::string& out, uint32_t c) {
  if (c < 0x80) out += char(c);
  else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
  else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
  else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}

// What is not valid UTF-8 becomes U+FFFD, so names and text never break the JSON they end up in.
std::string valid_utf8(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    bool ok = n > 0 && i + n <= s.size();
    for (int k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    if (ok) { out.append(s.substr(i, n)); i += n; }
    else { out += "\xEF\xBF\xBD"; ++i; }
  }
  return out;
}

std::string from_codepage(std::string_view s, int codepage) {
#ifdef _WIN32
  if (s.empty()) return {};
  const int wide = MultiByteToWideChar(codepage, 0, s.data(), int(s.size()), nullptr, 0);
  if (wide > 0) {
    std::wstring w(size_t(wide), L'\0');
    MultiByteToWideChar(codepage, 0, s.data(), int(s.size()), w.data(), wide);
    const int bytes = WideCharToMultiByte(65001 /* CP_UTF8 */, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
    std::string out(size_t(bytes), '\0');
    WideCharToMultiByte(65001, 0, w.data(), wide, out.data(), bytes, nullptr, nullptr);
    return out;
  }
#else
  (void)codepage;
#endif
  std::string out;  // Latin-1
  for (unsigned char c : s) append_utf8(out, c);
  return out;
}

int hex_digit(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// \U+XXXX (any character) and \M+nXXXX (a double-byte character of an East Asian code page) as DXF escapes them.
std::string unicode_escapes(std::string s) {
  if (s.find("\\U+") == std::string::npos && s.find("\\u+") == std::string::npos && s.find("\\M+") == std::string::npos) return s;
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 7 <= s.size() && (s[i + 1] == 'U' || s[i + 1] == 'u') && s[i + 2] == '+') {
      uint32_t code = 0;
      bool ok = true;
      for (int k = 0; ok && k < 4; ++k) { const int d = hex_digit(s[i + 3 + k]); ok = d >= 0; code = code * 16 + uint32_t(std::max(d, 0)); }
      if (ok) { append_utf8(out, code); i += 6; continue; }
    }
    if (s[i] == '\\' && i + 8 <= s.size() && (s[i + 1] == 'M' || s[i + 1] == 'm') && s[i + 2] == '+') {
      static const int pages[] = {0, 932, 950, 949, 1361, 936};
      const int page = s[i + 3] - '0';
      int hi = hex_digit(s[i + 4]) * 16 + hex_digit(s[i + 5]), lo = hex_digit(s[i + 6]) * 16 + hex_digit(s[i + 7]);
      if (page >= 1 && page <= 5 && hi >= 0 && lo >= 0) {
        const char bytes[2] = {char(hi), char(lo)};
        out += from_codepage(std::string_view(bytes, 2), pages[page]);
        i += 7;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// TEXT control codes: %%d degree, %%p plus/minus, %%c diameter, %%nnn a character, %%u/%%o/%%k underline toggles, ^J;
// ^I a space, or with `tabs` a tab (MTEXT's go to its tab stops).
std::string text_codes(std::string_view s, bool tabs = false) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && s[i + 1] == '%') {
      const char c = char(std::tolower(static_cast<unsigned char>(s[i + 2])));
      if (c == 'd') { out += "\xC2\xB0"; i += 2; continue; }
      if (c == 'p') { out += "\xC2\xB1"; i += 2; continue; }
      if (c == 'c') { out += "\xC3\x98"; i += 2; continue; }  // Ø: fonts without the diameter sign have it
      if (c == '%') { out += '%'; i += 2; continue; }
      if (c == 'u' || c == 'o' || c == 'k') { i += 2; continue; }
      if (std::isdigit(static_cast<unsigned char>(c))) {
        size_t j = i + 2;
        uint32_t code = 0;
        while (j < s.size() && j < i + 5 && std::isdigit(static_cast<unsigned char>(s[j]))) code = code * 10 + uint32_t(s[j++] - '0');
        if (code >= 32) append_utf8(out, code);
        i = j - 1;
        continue;
      }
    }
    if (s[i] == '^' && i + 1 < s.size()) {
      const char c = s[i + 1];
      if (c == ' ') { out += '^'; ++i; continue; }
      if (c == 'J') { out += '\n'; ++i; continue; }
      if (c == 'I') { out += tabs ? '\t' : ' '; ++i; continue; }
      if (c >= '@' && c <= '_') { ++i; continue; }
    }
    out += s[i];
  }
  return out;
}

}  // namespace

// AutoCAD Color Index -> RGB: 1-9 fixed, 10-249 24 hues x 5 shades x full/half saturation, 250-255 greys.
uint32_t aci_rgb(int i) {
  static const uint32_t fixed[10] = {0x000000, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF, 0x0000FF, 0xFF00FF, 0xFFFFFF, 0x414141, 0x808080};
  static const uint32_t greys[6] = {0x333333, 0x505050, 0x696969, 0x828282, 0xBEBEBE, 0xFFFFFF};
  if (i >= 1 && i <= 9) return fixed[i];
  if (i >= 250 && i <= 255) return greys[i - 250];
  if (i < 10 || i > 249) return kNoColor;
  static const double values[5] = {1.0, 0.8, 0.6, 0.5, 0.3};
  const int step = i % 10;
  const double value = values[step / 2], saturation = step % 2 ? 0.5 : 1.0, h = (i / 10 - 1) * 15 / 60.0;
  const double chroma = value * saturation, x = chroma * (1 - std::abs(std::fmod(h, 2.0) - 1)), m = value - chroma;
  double r = 0, g = 0, b = 0;
  switch (int(h)) {
    case 0: r = chroma; g = x; break;
    case 1: r = x; g = chroma; break;
    case 2: g = chroma; b = x; break;
    case 3: g = x; b = chroma; break;
    case 4: r = x; b = chroma; break;
    default: r = chroma; b = x; break;
  }
  auto byte = [](double t) { return uint32_t(std::floor(t * 255 + 1e-9)); };
  return byte(r + m) << 16 | byte(g + m) << 8 | byte(b + m);
}

namespace {

// White and black are the drawing's foreground (ACI 7 swaps with the background): the viewer's own colour.
uint32_t visible_color(uint32_t rgb) { return rgb == 0xFFFFFF || rgb == 0 ? kNoColor : rgb; }

bool blank(const std::string& s) { return s.find_first_not_of(" \t\n") == std::string::npos; }

// TEXT's %%u, %%o and %%k turn a line under, over and through it on and off: its parts in `base` with them; none
// without them.
std::vector<TextSpan> text_spans(std::string_view s, const TextFormat& base) {
  std::vector<TextSpan> out;
  TextSpan span;
  static_cast<TextFormat&>(span) = base;
  std::string run;
  bool toggled = false;
  auto flush = [&] {
    if (!run.empty()) span.text = text_codes(run), out.push_back(span);
    run.clear();
  };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && s[i + 1] == '%') {
      const char c = char(std::tolower(static_cast<unsigned char>(s[i + 2])));
      if (c == 'u' || c == 'o' || c == 'k') {
        flush();
        bool& on = c == 'u' ? span.underline : c == 'o' ? span.overline : span.strike;
        on = !on;
        toggled = true;
      } else {
        run += s.substr(i, 3);  // another code, for text_codes
      }
      i += 2;
      continue;
    }
    run += s[i];
  }
  flush();
  return toggled ? out : std::vector<TextSpan>{};
}

// MTEXT in parts of their own formats, from `base` (the entity's): \P starts a paragraph (\X and \N too), \~ is a hard
// space, a {group}'s formatting ends with it; \f a TrueType family (|b1 bold, |i1 italic) and \F a shape font, \H a height
// (drawing units, or times the current one with x), \W a width factor, \Q an obliquing angle, \T tracking, \A an
// alignment (0 baseline, 1 middle, 2 top), \C a colour index and \c a true colour (0 and 256: the entity's), \L \O \K
// a line under, over and through on (lower case: off), \S a stacked pair (a/b, a#b, a^b), \p a paragraph's layout (x, then
// i first-line indent, l left and r right indent, t tab stops, in text heights; q its alignment, c r l j or d), kept by
// the paragraphs after it (`paragraphs`). Each part's text takes the TEXT codes (%%d ..., ^I a tab).
std::vector<TextSpan> mtext_spans(std::string_view s, const TextFormat& base, double unit, std::vector<TextParagraph>& paragraphs) {
  std::vector<TextSpan> out;
  std::vector<TextFormat> groups;
  TextFormat f = base;
  std::string text;
  paragraphs.assign(1, TextParagraph());
  auto flush = [&] {
    if (text.empty()) return;
    TextSpan span;
    static_cast<TextFormat&>(span) = f;
    span.text = text_codes(text, true);
    out.push_back(std::move(span));
    text.clear();
  };
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '{' || c == '}') {
      flush();
      if (c == '{') groups.push_back(f);
      else if (!groups.empty()) f = groups.back(), groups.pop_back();
      continue;
    }
    if (c != '\\' || i + 1 >= s.size()) { text += c; continue; }
    const char k = s[++i];
    if (k == 'P' || k == 'X' || k == 'N') { text += '\n'; paragraphs.push_back(paragraphs.back()); continue; }
    if (k == '~') { text += "\xC2\xA0"; continue; }
    if (std::string_view("LlOoKk").find(k) != std::string_view::npos) {
      flush();
      (k == 'L' || k == 'l' ? f.underline : k == 'O' || k == 'o' ? f.overline : f.strike) = std::isupper(static_cast<unsigned char>(k)) != 0;
      continue;
    }
    if (std::string_view("ACcFfHQTWpS").find(k) == std::string_view::npos) { text += k; continue; }  // \\ \{ \} and the rest as written
    size_t end = i + 1;  // the value, up to its ';' (in a stack, one not escaped)
    while (end < s.size() && s[end] != ';') end += k == 'S' && s[end] == '\\' ? 2 : 1;
    end = std::min(end, s.size());
    std::string_view value = s.substr(i + 1, end - i - 1);
    i = end;
    flush();
    try {
      if (k == 'f' || k == 'F') {
        size_t at = value.find('|');
        f.font = std::string(trimmed(value.substr(0, at)));
        f.family.clear();
        f.bold = f.italic = false;
        while (at != std::string_view::npos) {
          const size_t next = value.find('|', at + 1);
          const auto part = value.substr(at + 1, next == std::string_view::npos ? std::string_view::npos : next - at - 1);
          f.bold = f.bold || part == "b1";
          f.italic = f.italic || part == "i1";
          at = next;
        }
      } else if (k == 'S') {
        std::string parts[2];
        char kind = 0;
        for (size_t j = 0; j < value.size(); ++j) {
          if (value[j] == '\\' && j + 1 < value.size()) parts[kind != 0] += value[++j];
          else if (!kind && (value[j] == '^' || value[j] == '/' || value[j] == '#')) kind = value[j];
          else parts[kind != 0] += value[j];
        }
        if (!kind) { text += parts[0]; continue; }
        TextSpan span;
        static_cast<TextFormat&>(span) = f;
        span.stack = kind;
        span.text = text_codes(parts[0], true);
        span.bottom = text_codes(parts[1], true);
        if (!blank(span.text + span.bottom)) out.push_back(std::move(span));
      } else if (k == 'p') {
        TextParagraph& p = paragraphs.back();
        bool tabs = false;  // after t: the stops, each a number (c or r before it: a centred or right one, taken as a stop)
        for (size_t at = 0; at <= value.size();) {
          const size_t comma = std::min(value.find(',', at), value.size());
          std::string_view item = value.substr(at, comma - at);
          at = comma + 1;
          if (!item.empty() && item[0] == 'x' && !tabs) item.remove_prefix(1);
          if (item.empty()) continue;
          const bool stop = tabs && (std::isdigit(static_cast<unsigned char>(item[0])) || item[0] == '.' || item[0] == 'c' || item[0] == 'r');
          const char key = stop ? 't' : item[0];
          if (!stop) item.remove_prefix(1);
          tabs = key == 't';
          try {
            if (key == 'q') {
              const char a = item.empty() ? 0 : item[0];
              if (a == 'c' || a == 'r' || a == 'l' || a == 'j' || a == 'd') p.justify = a == 'c' ? TextRequest::Center : a == 'r' ? TextRequest::Right : TextRequest::Left;
            } else if (key == 't') {
              if (!stop) p.tabs.clear();
              if (!item.empty() && (item[0] == 'c' || item[0] == 'r')) item.remove_prefix(1);
              if (!item.empty()) p.tabs.push_back(parse_number(item) * base.size);
            } else if (key == 'i' || key == 'l' || key == 'r') {
              (key == 'i' ? p.first : key == 'l' ? p.left : p.right) = parse_number(item) * base.size;
            }
          } catch (const Error&) {
          }
        }
      } else {
        const bool times = !value.empty() && (value.back() == 'x' || value.back() == 'X');
        if (times) value.remove_suffix(1);
        const double v = parse_number(value);
        if (k == 'H' && v > 0) f.size = times ? f.size * v : v * unit;
        else if (k == 'W' && v > 0) f.width = times ? f.width * v : v;
        else if (k == 'T' && v > 0) f.tracking = times ? f.tracking * v : v;
        else if (k == 'Q') f.oblique = v * kPi / 180;
        else if (k == 'A') f.align = v == 1 ? TextFormat::Center : v == 2 ? TextFormat::Top : TextFormat::Base;
        else if (k == 'C') f.color = v >= 1 && v <= 255 ? visible_color(aci_rgb(int(v))) : TextFormat::kInherit;
        else if (k == 'c') {  // 0xBBGGRR
          const auto bgr = uint32_t(int64_t(v)) & 0xFFFFFFu;
          f.color = visible_color((bgr & 0xFF) << 16 | (bgr & 0xFF00) | bgr >> 16);
        }
      }
    } catch (const Error&) {
    }
  }
  flush();
  while (!out.empty() && !out.back().stack) {  // what ends it: new paragraphs and spaces
    auto& t = out.back().text;
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
    if (!t.empty()) break;
    out.pop_back();
  }
  return out;
}

// The fields of one entity (or table record): the group codes after its "0 TYPE" pair.
struct Fields {
  const std::vector<Pair>* pairs = nullptr;
  size_t begin = 0, end = 0;
  const Pair* find(int code) const {
    for (size_t i = begin; i < end; ++i)
      if ((*pairs)[i].code == code) return &(*pairs)[i];
    return nullptr;
  }
  bool has(int code) const { return find(code) != nullptr; }
  std::string_view str(int code, std::string_view fallback = {}) const {
    const auto* p = find(code);
    return p ? p->value : fallback;
  }
  double num(int code, double fallback = 0) const {
    const auto* p = find(code);
    return p ? parse_number(p->value) : fallback;
  }
  int integer(int code, int fallback = 0) const { return int(std::lround(num(code, fallback))); }
  gp_XYZ xyz(int code, const gp_XYZ& fallback = gp_XYZ(0, 0, 0)) const {
    return gp_XYZ(num(code, fallback.X()), num(code + 10, fallback.Y()), num(code + 20, fallback.Z()));
  }
};

struct Entity {
  std::string_view type;
  Fields f;
};

// Reads repeated groups in order (HATCH boundaries, pattern lines), where a code means different things by position.
struct Cursor {
  const std::vector<Pair>& p;
  size_t i, end;
  bool at(int code) const { return i < end && p[i].code == code; }
  double number(int code, double fallback = 0) { return at(code) ? parse_number(p[i++].value) : fallback; }
  int integer(int code, int fallback = 0) { return int(std::lround(number(code, fallback))); }
  void skip() { if (i < end) ++i; }
};

// Object coordinate system of a planar entity (the DXF "arbitrary axis algorithm").
struct Ocs {
  gp_XYZ ax{1, 0, 0}, ay{0, 1, 0}, az{0, 0, 1};
  explicit Ocs(gp_XYZ n) {
    if (n.Modulus() < 1e-12) return;
    n.Normalize();
    if (std::abs(n.X() - 0) < 1e-12 && std::abs(n.Y()) < 1e-12 && n.Z() > 0) return;
    az = n;
    ax = (std::abs(n.X()) < 1.0 / 64 && std::abs(n.Y()) < 1.0 / 64 ? gp_XYZ(0, 1, 0) : gp_XYZ(0, 0, 1)).Crossed(n);
    ax.Normalize();
    ay = n.Crossed(ax);
    ay.Normalize();
  }
  bool planar() const { return std::abs(std::abs(az.Z()) - 1) < 1e-9; }  // parallel to XY: drawn exactly, not projected
  gp_XYZ to_wcs(double x, double y, double z) const { return ax * x + ay * y + az * z; }
  gp_XYZ to_wcs(const gp_XYZ& p) const { return to_wcs(p.X(), p.Y(), p.Z()); }
};

// Anything in it but empty compounds (a text's placeholder stays empty when no font loads or a part in a colour of its
// own shows nothing).
bool drawn(const TopoDS_Shape& s) {
  if (s.IsNull()) return false;
  if (s.ShapeType() != TopAbs_COMPOUND) return true;
  for (TopoDS_Iterator it(s, false, false); it.More(); it.Next())
    if (drawn(it.Value())) return true;
  return false;
}

// Geometry by (layer as written, colour key, linetype, lineweight, linetype scale): model space, or a block's content in
// its own coordinates. Linetype "" and lineweight -1 are by layer, "BYBLOCK" and -2 by block.
struct Space {
  std::map<std::tuple<std::string, uint32_t, std::string, int, double>, TopoDS_Compound> groups;
  int depth = 0;  // 1 + the level of the deepest copy its groups show that is still to be made (Reader::finish), 0 for none
};

// Where an entity's geometry goes and how its points map: drawing units -> mm, minus the model-space shift, z dropped.
struct Place {
  Space* space;
  double sx = 0, sy = 0;
  bool model = false;
};

struct PatternLine {
  double angle = 0;
  gp_XY base, offset;
  std::vector<double> dashes;
};

// How a block lands: rigid (shared by location), similar (a uniform scale, then rigid: the block's copy at that scale is
// made once and shared by location) or general (a transformed copy each time).
struct Placement {
  enum Kind { None, Location, Similar, General } kind = None;
  gp_Trsf trsf;
  gp_GTrsf gtrsf;
  double scale = 1;
};

Placement placement(double a11, double a12, double a21, double a22, double tx, double ty) {
  Placement p;
  const double s1 = std::hypot(a11, a21), s2 = std::hypot(a12, a22), dot = a11 * a12 + a21 * a22, det = a11 * a22 - a12 * a21;
  if (s1 < 1e-12 || s2 < 1e-12 || std::abs(det) < 1e-12 * s1 * s2) return p;
  if (std::abs(s1 - s2) <= 1e-9 * std::max(s1, s2) && std::abs(dot) <= 1e-9 * s1 * s2) {
    // A mirror in the plane is a half turn about an in-plane axis in 3D, so z flips with it: never a negative transform.
    const double k = 1 / s1, z = det > 0 ? 1 : -1;
    p.trsf.SetValues(a11 * k, a12 * k, 0, tx, a21 * k, a22 * k, 0, ty, 0, 0, z, 0);
    p.kind = std::abs(s1 - 1) < 1e-9 ? Placement::Location : Placement::Similar;
    p.scale = s1;
  } else {
    p.gtrsf.SetValue(1, 1, a11); p.gtrsf.SetValue(1, 2, a12); p.gtrsf.SetValue(1, 4, tx);
    p.gtrsf.SetValue(2, 1, a21); p.gtrsf.SetValue(2, 2, a22); p.gtrsf.SetValue(2, 4, ty);
    p.gtrsf.SetValue(3, 3, 1);
    p.kind = Placement::General;
  }
  return p;
}

bool inside(const std::vector<gp_XY>& poly, const gp_XY& q) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    if ((poly[i].Y() > q.Y()) != (poly[j].Y() > q.Y()) &&
        q.X() < (poly[j].X() - poly[i].X()) * (q.Y() - poly[i].Y()) / (poly[j].Y() - poly[i].Y()) + poly[i].X())
      in = !in;
  return in;
}

double signed_area(const std::vector<gp_XY>& poly) {
  double a = 0;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) a += poly[j].X() * poly[i].Y() - poly[i].X() * poly[j].Y();
  return a / 2;
}

class Reader {
 public:
  Reader(const std::filesystem::path& file, const ImportOptions& options) : m_options(options), m_fontFolders(font_folders(file)) { load(file); }
  Drawing read();

 private:
  struct Layer {
    std::string name, linetype;
    std::vector<double> pattern;  // its linetype's dashes from the LTYPE table, sized as acad.lin's (mm; < 0 gap, 0 dot)
    uint32_t color = kNoColor;
    bool visible = true, off = false, frozen = false, locked = false, plot = true;
    int lineweight = -3;  // 1/100 mm; negative: by layer, by block or the default
    json info() const;    // what an import node keeps of it (Node::layer)
  };
  struct Style {
    std::string font, family;  // its font file (a shape font, .shx, or a TrueType one) and its TrueType family (XDATA)
    double height = 0, width = 1, oblique = 0;  // oblique: degrees
    bool bold = false, italic = false;  // the family's faces (XDATA flags)
  };
  struct Block {
    size_t first = 0, last = 0;  // entity range in m_blockEntities
    gp_XYZ base;
    bool xref = false;
    int state = 0;  // 0 not built, 1 building, 2 built
    Space space;
    std::map<double, Space> scaled;  // its content at each scale it is inserted at (a plan: 1306 scaled inserts, 71 copies)
  };
  struct Out {
    Space* space;
    std::string layer;
    uint32_t color;
    std::string linetype;  // "" by layer, "BYBLOCK", else its own (6)
    int weight = -1;       // 1/100 mm; -1 by layer, -2 by block, -3 the default (370)
    double scale = 1;      // its linetype's scale (48, CELTSCALE)
  };

  void load(const std::filesystem::path& file);
  void tables(const std::vector<Entity>& section);
  void blocks(const std::vector<Entity>& section);
  std::string decode(std::string_view raw) const;
  const std::string& layer_name(std::string_view raw);
  uint32_t layer_color(const std::string& name) const;
  Out out(const Fields& f, Space* space);
  std::string layer_linetype(const std::string& name) const;
  int layer_weight(const std::string& name) const;
  uint32_t color_of(const Fields& f) const;
  void run(const std::vector<Entity>& list, size_t first, size_t last, Place& at);
  void entity(const Entity& e, Place& at);
  void polyline(const Entity& e, const std::vector<const Entity*>& vertices, Place& at);
  void insert(const Fields& f, const Out& o, const Place& at, std::string_view name, bool dimension, bool array);
  const Space& build(Block& b);
  void hatch(const Fields& f, Out& o, const Place& at);
  void text(const Fields& f, Out& o, const Place& at, bool attrib);
  void mtext(const Fields& f, Out& o, const Place& at);
  TextRequest text_request(std::string_view style, std::string text, double height) const;
  void text_shape(const Out& o, const TextRequest& request, const gp_Ax3& at);
  static std::vector<std::filesystem::path> font_folders(const std::filesystem::path& file);

  gp_Pnt pnt(const Place& at, const gp_XYZ& wcs) const { return gp_Pnt(wcs.X() * m_unit - at.sx, wcs.Y() * m_unit - at.sy, 0); }
  void add(const Out& o, const TopoDS_Shape& s) {
    auto& c = o.space->groups[{o.layer, o.color, o.linetype, o.weight, o.scale}];
    if (c.IsNull()) m_builder.MakeCompound(c);
    m_builder.Add(c, s);
  }
  void segment(const Out& o, const gp_Pnt& a, const gp_Pnt& b) {
    if (a.Distance(b) > 1e-9) add(o, BRepBuilderAPI_MakeEdge(a, b).Edge());
  }
  void chain(const Out& o, const std::vector<gp_Pnt>& points, bool closed) {
    for (size_t i = 0; i + 1 < points.size(); ++i) segment(o, points[i], points[i + 1]);
    if (closed && points.size() > 2) segment(o, points.back(), points.front());
  }
  void arc(const Out& o, const Place& at, const Ocs& ocs, const gp_XYZ& center, double r, double a0, double a1, bool full);
  void bulge(const Out& o, const Place& at, const Ocs& ocs, double elevation, const gp_XY& p, const gp_XY& q, double b);
  void fill(const Out& o, std::vector<std::vector<gp_XY>> loops);
  void outline(const Out& o, const std::vector<std::vector<gp_XY>>& loops) {
    for (const auto& loop : loops) {
      std::vector<gp_Pnt> points;
      for (const auto& p : loop) points.emplace_back(p.X(), p.Y(), 0);
      chain(o, points, true);
    }
  }
  bool pattern(const Out& o, const Place& at, const Ocs& ocs, double elevation, const std::vector<std::vector<gp_XY>>& loops,
               const std::vector<PatternLine>& lines);
  TopoDS_Compound later(const TopoDS_Shape& source, const Placement& p, int level);
  void finish();
  void tick();

  const ImportOptions& m_options;
  std::vector<std::filesystem::path> m_fontFolders;  // where a style's font file is looked for (font_folders)
  std::string m_buffer;
  std::vector<Pair> m_pairs;
  std::vector<Entity> m_model, m_blockEntities;
  double m_unit = 1;
  bool m_utf8 = true;
  int m_codepage = 1252;
  gp_XYZ m_extMin{0, 0, 0}, m_extMax{0, 0, 0};
  bool m_hasExtents = false;
  std::map<std::string, Layer> m_layers;  // by upper-case name
  std::map<std::string, std::string> m_layerNames;  // raw name -> display name
  std::map<std::string, Style> m_styles;  // by upper-case name
  std::map<std::string, std::vector<double>> m_linetypes;  // LTYPE: dashes by upper-case raw name (< 0 gap, 0 dot)
  std::map<std::string, Block> m_blocks;  // by upper-case raw name
  int m_depth = 0;
  BRep_Builder m_builder;
  size_t m_done = 0, m_total = 1;
  std::map<std::string, int> m_unsupported;
  int m_broken = 0, m_paper = 0, m_missing = 0, m_xrefs = 0, m_noFont = 0, m_patternOutlines = 0;
  size_t m_patternBudget = 600000;
  std::vector<std::string> m_warnings;
  // Made once every entity is read, side by side (finish): the copies of blocks placed scaled or skewed, level by level (a
  // copy of a block that shows another copy waits for it). Until then each is an empty compound, already placed where it
  // shows (a 3 MB DWG's plan: 1.5 s of copies before).
  struct CopyJob {
    TopoDS_Shape source;
    Placement p;
    int level;
    TopoDS_Compound into;
  };
  std::vector<CopyJob> m_copyJobs;
  // A text laid out once per request (text_key) at the origin and placed by location: half the texts of a plan repeat one.
  // Outlined in finish() side by side, before the copies (a copied block shows its texts); until then its placeholders
  // are empty: the part in the text's colour, if it has one, and each part in a colour of its own (MTEXT \C).
  struct TextJob {
    TextRequest request;
    TopoDS_Compound into;
    std::map<uint32_t, TopoDS_Compound> colored;
    int uses = 0;
  };
  std::vector<TextJob> m_textJobs;
  std::unordered_map<std::string, size_t> m_texts;  // text_key -> its job
};

void Reader::load(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw Error("cannot open DXF");
  std::ostringstream text;
  text << in.rdbuf();
  m_buffer = text.str();
  if (m_buffer.rfind("AutoCAD Binary DXF", 0) == 0) throw Error("binary DXF is not supported: save the drawing as ASCII DXF");
  size_t at = m_buffer.rfind("\xEF\xBB\xBF", 0) == 0 ? 3 : 0;
  auto line = [&](std::string_view& out) {
    if (at >= m_buffer.size()) return false;
    size_t end = m_buffer.find('\n', at);
    if (end == std::string::npos) end = m_buffer.size();
    out = std::string_view(m_buffer).substr(at, end - at);
    if (!out.empty() && out.back() == '\r') out.remove_suffix(1);
    at = end + 1;
    return true;
  };
  m_pairs.reserve(m_buffer.size() / 12);
  std::string_view code, value;
  while (line(code)) {
    const auto c = trimmed(code);
    if (c.empty() && at >= m_buffer.size()) break;  // trailing blank line
    if (!line(value)) throw Error("truncated DXF group");
    int number = -1;
    const auto [end, error] = std::from_chars(c.data(), c.data() + c.size(), number);
    if (error != std::errc() || end != c.data() + c.size() || number < 0 || number > 1071) throw Error("invalid DXF group code");
    m_pairs.push_back({number, value});
  }
  if (m_pairs.empty()) throw Error("DXF file is empty");
}

std::string Reader::decode(std::string_view raw) const {
  return unicode_escapes(m_utf8 ? valid_utf8(raw) : from_codepage(raw, m_codepage));
}

const std::string& Reader::layer_name(std::string_view raw) {
  auto it = m_layerNames.find(std::string(raw));
  if (it != m_layerNames.end()) return it->second;
  std::string name = decode(trimmed(raw));
  if (name.empty()) name = "0";
  if (const auto layer = m_layers.find(upper(name)); layer != m_layers.end()) name = layer->second.name;
  return m_layerNames.emplace(std::string(raw), name).first->second;
}

json Reader::Layer::info() const {
  json j = {{"name", name}};
  if (off) j["off"] = true;
  if (frozen) j["frozen"] = true;
  if (locked) j["locked"] = true;
  if (!plot) j["plot"] = false;
  if (!linetype.empty() && upper(linetype) != "CONTINUOUS") j["linetype"] = linetype;
  if (!pattern.empty() && j.contains("linetype")) j["pattern"] = pattern;
  if (lineweight >= 0) j["lineweight"] = lineweight / 100.0;  // mm
  return j;
}

uint32_t Reader::layer_color(const std::string& name) const {
  const auto it = m_layers.find(upper(name));
  return it == m_layers.end() ? kNoColor : it->second.color;
}

std::string Reader::layer_linetype(const std::string& name) const {
  const auto it = m_layers.find(upper(name));
  return it == m_layers.end() || it->second.linetype.empty() ? "Continuous" : it->second.linetype;
}

int Reader::layer_weight(const std::string& name) const {
  const auto it = m_layers.find(upper(name));
  return it == m_layers.end() || it->second.lineweight < 0 ? -3 : it->second.lineweight;
}

// An entity's layer, colour, linetype and lineweight: what goes into one body with it.
Reader::Out Reader::out(const Fields& f, Space* space) {
  Out o{space, layer_name(f.str(8, "0")), color_of(f)};
  const std::string type = decode(trimmed(f.str(6)));
  if (const auto u = upper(type); !u.empty() && u != "BYLAYER") o.linetype = u == "BYBLOCK" ? "BYBLOCK" : type;
  const int weight = f.integer(370, -1);
  o.weight = weight >= -3 && weight <= 211 ? weight : -1;
  if (const double scale = f.num(48, 1); scale > 0 && std::abs(scale - 1) > 1e-9) o.scale = std::round(scale * 1e6) / 1e6;
  return o;
}

uint32_t Reader::color_of(const Fields& f) const {
  if (const auto* t = f.find(420)) return visible_color(uint32_t(int64_t(parse_number(t->value))) & 0xFFFFFFu);
  const int aci = f.integer(62, 256);
  if (aci == 0) return kByBlock;
  if (aci == 256 || aci < 0) return kByLayer;
  return visible_color(aci_rgb(aci));
}

void Reader::tables(const std::vector<Entity>& section) {
  for (const auto& e : section) {
    const auto& f = e.f;
    if (e.type == "LAYER") {
      Layer layer;
      // Names are decoded once the code page is known, see read().
      layer.name = std::string(trimmed(f.str(2, "0")));
      const int aci = f.integer(62, 7), flags = f.integer(70);
      layer.color = f.has(420) ? visible_color(uint32_t(int64_t(f.num(420))) & 0xFFFFFFu) : visible_color(aci_rgb(std::abs(aci)));
      layer.off = aci < 0;  // negative colour = off; flag 1 = frozen, 4 = locked
      layer.frozen = flags & 1;
      layer.locked = flags & 4;
      layer.visible = !layer.off && !layer.frozen;
      layer.plot = f.integer(290, 1) != 0;
      layer.linetype = std::string(trimmed(f.str(6)));
      layer.lineweight = f.integer(370, -3);
      m_layers[upper(layer.name)] = layer;
    } else if (e.type == "STYLE") {
      Style style;
      style.font = decode(trimmed(f.str(3)));
      if (f.has(1001)) {  // ACAD's XDATA: the TrueType family and its italic (0x1000000) and bold (0x2000000) flags
        style.family = decode(trimmed(f.str(1000)));
        const long flags = long(f.num(1071, 0));
        style.italic = flags & 0x1000000;
        style.bold = flags & 0x2000000;
      }
      style.height = f.num(40);
      style.width = f.num(41, 1);
      if (!(style.width > 0)) style.width = 1;
      style.oblique = f.num(50);
      m_styles[upper(trimmed(f.str(2)))] = style;
    } else if (e.type == "LTYPE") {  // the dashes (49, repeated); the shapes and text of complex linetypes are left out
      std::vector<double> dashes;
      for (size_t i = f.begin; i < f.end; ++i)
        if ((*f.pairs)[i].code == 49) try {
            dashes.push_back(parse_number((*f.pairs)[i].value));
          } catch (const Error&) {
          }
      if (!dashes.empty()) m_linetypes[upper(trimmed(f.str(2)))] = dashes;
    }
  }
}

void Reader::blocks(const std::vector<Entity>& section) {
  m_blockEntities = section;
  Block* open = nullptr;
  for (size_t i = 0; i < m_blockEntities.size(); ++i) {
    const auto& e = m_blockEntities[i];
    if (e.type == "BLOCK") {
      Block b;
      b.first = i + 1;
      b.base = e.f.xyz(10);
      b.xref = (e.f.integer(70) & 4) != 0;
      open = &(m_blocks[upper(trimmed(e.f.str(2)))] = b);
    } else if (e.type == "ENDBLK" && open) {
      open->last = i;
      open = nullptr;
    }
  }
  if (open) open->last = m_blockEntities.size();
}

void Reader::tick() {
  if (++m_done % 2000 == 0 && m_options.progress &&
      !m_options.progress(std::min(0.99, double(m_done) / double(m_total)), "reading drawing"))
    throw Error("cancelled");
}

void Reader::run(const std::vector<Entity>& list, size_t first, size_t last, Place& at) {
  for (size_t i = first; i < last; ++i) {
    const auto& e = list[i];
    if (at.model) tick();
    auto guarded = [&](const std::function<void()>& work) {
      try {
        work();
      } catch (const Standard_Failure&) {
        ++m_broken;
      } catch (const Error& error) {
        if (std::string_view(error.what()) == "cancelled") throw;
        ++m_broken;
      } catch (const std::exception&) {
        ++m_broken;
      }
    };
    if (e.type == "POLYLINE") {
      std::vector<const Entity*> vertices;
      size_t j = i + 1;
      while (j < last && list[j].type == "VERTEX") vertices.push_back(&list[j++]);
      guarded([&] { polyline(e, vertices, at); });
      i = j < last && list[j].type == "SEQEND" ? j : j - 1;
      continue;
    }
    if (e.type == "SEQEND" || e.type == "VERTEX") continue;
    guarded([&] { entity(e, at); });
  }
}

void Reader::arc(const Out& o, const Place& at, const Ocs& ocs, const gp_XYZ& center, double r, double a0, double a1, bool full) {
  if (!(r > 1e-12)) return;
  if (ocs.planar()) {
    const gp_Ax2 axes(pnt(at, ocs.to_wcs(center)), gp_Dir(0, 0, ocs.az.Z() > 0 ? 1 : -1), gp_Dir(ocs.ax.X(), ocs.ax.Y(), 0));
    const gp_Circ circle(axes, r * m_unit);
    add(o, full ? BRepBuilderAPI_MakeEdge(circle).Edge() : BRepBuilderAPI_MakeEdge(circle, a0, a1).Edge());
    return;
  }
  // Tilted: what XY shows of it, as a polyline.
  if (full) a1 = a0 + 2 * kPi;
  const int n = std::max(8, int(std::ceil(std::abs(a1 - a0) / (kPi / 32))));
  std::vector<gp_Pnt> points;
  for (int k = 0; k <= n; ++k) {
    const double t = a0 + (a1 - a0) * k / n;
    points.push_back(pnt(at, ocs.to_wcs(center.X() + r * std::cos(t), center.Y() + r * std::sin(t), center.Z())));
  }
  chain(o, points, false);
}

void Reader::bulge(const Out& o, const Place& at, const Ocs& ocs, double elevation, const gp_XY& p, const gp_XY& q, double b) {
  if (std::abs(b) < 1e-12) {
    segment(o, pnt(at, ocs.to_wcs(p.X(), p.Y(), elevation)), pnt(at, ocs.to_wcs(q.X(), q.Y(), elevation)));
    return;
  }
  const double dx = q.X() - p.X(), dy = q.Y() - p.Y(), chord = std::hypot(dx, dy);
  if (chord < 1e-12) return;
  const double cx = (p.X() + q.X()) / 2 - dy * (1 - b * b) / (4 * b), cy = (p.Y() + q.Y()) / 2 + dx * (1 - b * b) / (4 * b);
  double start = std::atan2(p.Y() - cy, p.X() - cx), stop = start + 4 * std::atan(b);
  if (b < 0) std::swap(start, stop);
  arc(o, at, ocs, gp_XYZ(cx, cy, elevation), chord * (1 + b * b) / (4 * std::abs(b)), start, stop, false);
}

TopoDS_Compound Reader::later(const TopoDS_Shape& source, const Placement& p, int level) {
  TopoDS_Compound into;
  m_builder.MakeCompound(into);
  m_copyJobs.push_back({source, p, level, into});
  return into;
}

void Reader::finish() {
  // A placeholder takes what was made for it in place, so every place that shows it shows that.
  auto deliver = [](TopoDS_Compound into, const TopoDS_Shape& made) {
    BRep_Builder builder;
    into.Free(true);
    for (TopoDS_Iterator it(made); it.More(); it.Next()) builder.Add(into, it.Value());
    into.Free(false);
  };
  std::atomic<int> broken{0}, noFont{0};
  if (!m_textJobs.empty()) {
    if (m_options.progress && !m_options.progress(0.99, "reading drawing")) throw Error("cancelled");
    // In runs of about equal length, each with an outliner of its own (glyph caches and HarfBuzz buffers are not shared),
    // texts of one font and size together so that their glyphs are outlined once.
    auto font = [](const TextRequest& r) { return std::tie(r.font, r.family, r.bold, r.italic, r.size, r.width, r.oblique); };
    std::vector<size_t> order(m_textJobs.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return font(m_textJobs[a].request) < font(m_textJobs[b].request); });
    auto weight = [this](size_t job) {
      const TextRequest& r = m_textJobs[job].request;
      size_t w = 8 + r.text.size();
      for (const auto& span : r.spans) w += span.text.size() + span.bottom.size();
      return w;
    };
    size_t total = 0;
    for (size_t job : order) total += weight(job);
    const size_t runs = std::min(order.size(), size_t(std::max(1, OSD_ThreadPool::DefaultPool()->NbDefaultThreadsToLaunch())));
    std::vector<size_t> starts{0};
    for (size_t i = 0, sum = 0; i < order.size() && starts.size() < runs; ++i)
      if ((sum += weight(order[i])) * runs >= total * starts.size() && i + 1 < order.size()) starts.push_back(i + 1);
    starts.push_back(order.size());
    OSD_Parallel::For(0, int(starts.size() - 1), [&](int run) {
      TextOutliner outliner(m_fontFolders);
      for (size_t i = starts[size_t(run)]; i < starts[size_t(run) + 1]; ++i) {
        TextJob& job = m_textJobs[order[i]];
        try {
          std::map<uint32_t, TopoDS_Compound> colored;
          const TopoDS_Shape made = outliner.outline(job.request, gp_Ax3(), &colored);
          if (made.IsNull()) { noFont += job.uses; continue; }
          if (!job.into.IsNull()) deliver(job.into, made);
          for (auto& [color, into] : job.colored)
            if (const auto part = colored.find(color); part != colored.end()) deliver(into, part->second);
        } catch (...) {
          broken += job.uses;
        }
      }
    });
    m_textJobs.clear();
    m_texts.clear();
  }
  int top = -1;
  for (const auto& job : m_copyJobs) top = std::max(top, job.level);
  for (int level = 0; level <= top; ++level) {
    if (m_options.progress && !m_options.progress(0.99, "reading drawing")) throw Error("cancelled");
    std::vector<const CopyJob*> now;
    for (const auto& job : m_copyJobs)
      if (job.level == level) now.push_back(&job);
    OSD_Parallel::For(0, int(now.size()), [&](int i) {
      const CopyJob& job = *now[size_t(i)];
      if (!drawn(job.source)) return;  // texts with no font, say
      try {
        deliver(job.into, job.p.kind == Placement::General ? BRepBuilderAPI_GTransform(job.source, job.p.gtrsf, true).Shape()
                                                           : BRepBuilderAPI_Transform(job.source, job.p.trsf, true).Shape());
      } catch (...) {
        ++broken;
      }
    });
  }
  m_copyJobs.clear();
  m_broken += broken;
  m_noFont += noFont;
}

const Space& Reader::build(Block& b) {
  if (b.state == 2) return b.space;
  if (b.state == 1 || m_depth > 24) throw Error("DXF block contains itself");
  struct Depth {
    int& d;
    explicit Depth(int& depth) : d(depth) { ++d; }
    ~Depth() { --d; }
  } depth(m_depth);
  b.state = 1;
  Place at{&b.space, 0, 0, false};
  try {
    run(m_blockEntities, b.first, b.last, at);
  } catch (...) {
    b.state = 2;
    throw;
  }
  b.state = 2;
  return b.space;
}

void Reader::insert(const Fields& f, const Out& o, const Place& at, std::string_view name, bool dimension, bool array) {
  const auto found = m_blocks.find(upper(trimmed(name)));
  if (found == m_blocks.end()) {
    if (!dimension) ++m_missing;
    return;
  }
  Block& block = found->second;
  if (block.xref) { ++m_xrefs; return; }
  const Space& content = build(block);
  if (content.groups.empty()) return;
  const Ocs ocs(f.xyz(210, gp_XYZ(0, 0, 1)));
  const gp_XYZ ins = dimension ? f.xyz(12) : f.xyz(10);
  const double sx = dimension ? 1 : f.num(41, 1), sy = dimension ? 1 : f.num(42, 1), rot = dimension ? 0 : f.num(50) * kPi / 180;
  const double c = std::cos(rot), s = std::sin(rot);
  // block (mm, z = 0) -> OCS plane: rotate(scale(q - base)) + insertion; OCS -> WCS, seen from above (x and y rows).
  const double m11 = ocs.ax.X(), m12 = ocs.ay.X(), m21 = ocs.ax.Y(), m22 = ocs.ay.Y();
  const double r11 = c * sx, r12 = -s * sy, r21 = s * sx, r22 = c * sy;
  const double a11 = m11 * r11 + m12 * r21, a12 = m11 * r12 + m12 * r22, a21 = m21 * r11 + m22 * r21, a22 = m21 * r12 + m22 * r22;
  const gp_XYZ insW = ocs.to_wcs(ins);
  const double bx = block.base.X() * m_unit, by = block.base.Y() * m_unit;
  const double tx = insW.X() * m_unit - at.sx - (a11 * bx + a12 * by), ty = insW.Y() * m_unit - at.sy - (a21 * bx + a22 * by);
  const int columns = array ? std::clamp(f.integer(70, 1), 1, 1000) : 1, rows = array ? std::clamp(f.integer(71, 1), 1, 1000) : 1;
  if (columns * rows > 10000) throw Error("DXF block array too large");
  const double columnSpacing = f.num(44) * m_unit, rowSpacing = f.num(45) * m_unit;
  for (int row = 0; row < rows; ++row)
    for (int column = 0; column < columns; ++column) {
      const double ox = c * column * columnSpacing - s * row * rowSpacing, oy = s * column * columnSpacing + c * row * rowSpacing;
      const auto p = placement(a11, a12, a21, a22, tx + m11 * ox + m12 * oy, ty + m21 * ox + m22 * oy);
      if (p.kind == Placement::None) return;
      const Space* source = &content;
      if (p.kind == Placement::Similar) {  // scaled once (a turned insert's scale may differ in the last digit), then placed rigidly
        auto it = block.scaled.lower_bound(p.scale * (1 - 1e-9));
        if (it == block.scaled.end() || it->first > p.scale * (1 + 1e-9)) {
          it = block.scaled.emplace_hint(it, p.scale, Space());
          Placement scale;
          scale.trsf.SetScale(gp::Origin(), p.scale);
          for (const auto& [key, shape] : content.groups) it->second.groups[key] = later(shape, scale, content.depth);
          it->second.depth = content.depth + 1;
        }
        source = &it->second;
      }
      for (const auto& [key, shape] : source->groups) {
        const auto& [layer, color, linetype, weight, scale] = key;
        Out target{o.space, layer == "0" ? o.layer : layer, color, linetype, weight, scale};
        if (color == kByBlock) target.color = o.color == kByLayer && o.layer != "0" ? layer_color(o.layer) : o.color;
        // By block: the insert's own, or by layer its layer's (named, when the content lies on a layer of its own); its
        // dashes in the insert's scale too.
        if (linetype == "BYBLOCK") {
          target.linetype = o.linetype.empty() && target.layer != o.layer ? layer_linetype(o.layer) : o.linetype;
          target.scale = std::round(scale * o.scale * 1e6) / 1e6;
        }
        if (weight == -2) target.weight = o.weight == -1 && target.layer != o.layer ? layer_weight(o.layer) : o.weight;
        add(target, p.kind == Placement::General ? later(shape, p, content.depth) : shape.Moved(TopLoc_Location(p.trsf)));
      }
      o.space->depth = std::max(o.space->depth, p.kind == Placement::General ? content.depth + 1 : source->depth);
    }
}

void Reader::polyline(const Entity& e, const std::vector<const Entity*>& vertices, Place& at) {
  const auto& f = e.f;
  if (f.integer(60) == 1 || (at.model && f.integer(67) == 1)) return;
  const Out o = out(f, at.space);
  const int flags = f.integer(70);
  if (flags & 64) {  // polyface mesh: positions, then faces naming them (negative index = invisible edge)
    std::vector<gp_Pnt> positions;
    for (const auto* v : vertices)
      if ((v->f.integer(70) & 192) == 192) positions.push_back(pnt(at, v->f.xyz(10)));
    for (const auto* v : vertices) {
      if ((v->f.integer(70) & 192) != 128) continue;
      std::vector<int> corners;
      for (int code : {71, 72, 73, 74})
        if (const int k = v->f.integer(code); k != 0) corners.push_back(k);
      for (size_t k = 0; k < corners.size(); ++k) {
        const int a = corners[k], b = corners[(k + 1) % corners.size()];
        if (a < 0 || std::abs(a) > int(positions.size()) || std::abs(b) > int(positions.size()) || b == 0) continue;
        segment(o, positions[size_t(a - 1)], positions[size_t(std::abs(b) - 1)]);
      }
    }
    return;
  }
  if (flags & 16) {  // polygon mesh: an m x n grid
    const int m = f.integer(71), n = f.integer(72);
    std::vector<gp_Pnt> grid;
    for (const auto* v : vertices) grid.push_back(pnt(at, v->f.xyz(10)));
    if (m < 1 || n < 1 || size_t(m) * size_t(n) > grid.size()) return;
    for (int i = 0; i < m; ++i)
      for (int j = 0; j < n; ++j) {
        if (j + 1 < n || (flags & 32)) segment(o, grid[size_t(i * n + j)], grid[size_t(i * n + (j + 1) % n)]);
        if (i + 1 < m || (flags & 1)) segment(o, grid[size_t(i * n + j)], grid[size_t(((i + 1) % m) * n + j)]);
      }
    return;
  }
  const bool closed = flags & 1;
  if (flags & 8) {  // 3D polyline, WCS
    std::vector<gp_Pnt> points;
    for (const auto* v : vertices)
      if (!(v->f.integer(70) & 16)) points.push_back(pnt(at, v->f.xyz(10)));
    chain(o, points, closed);
    return;
  }
  const Ocs ocs(f.xyz(210, gp_XYZ(0, 0, 1)));
  const double elevation = f.num(30);
  std::vector<std::array<double, 3>> points;
  for (const auto* v : vertices)
    if (!(v->f.integer(70) & 16)) points.push_back({v->f.num(10), v->f.num(20), v->f.num(42)});
  const size_t segments = points.size() < 2 ? 0 : points.size() - 1 + (closed ? 1 : 0);
  for (size_t k = 0; k < segments; ++k) {
    const auto& p = points[k];
    const auto& q = points[(k + 1) % points.size()];
    bulge(o, at, ocs, elevation, gp_XY(p[0], p[1]), gp_XY(q[0], q[1]), p[2]);
  }
}

void Reader::entity(const Entity& e, Place& at) {
  const auto& f = e.f;
  if (f.integer(60) == 1) return;  // invisible
  if (at.model && f.integer(67) == 1) { ++m_paper; return; }  // paper space (layouts)
  Out o = out(f, at.space);
  const auto& t = e.type;
  if (t == "LINE") {
    segment(o, pnt(at, f.xyz(10)), pnt(at, f.xyz(11)));
  } else if (t == "POINT") {
    if (upper(o.layer) != "DEFPOINTS") add(o, BRepBuilderAPI_MakeVertex(pnt(at, f.xyz(10))).Vertex());
  } else if (t == "CIRCLE") {
    arc(o, at, Ocs(f.xyz(210, gp_XYZ(0, 0, 1))), f.xyz(10), f.num(40), 0, 2 * kPi, true);
  } else if (t == "ARC") {
    const double a0 = f.num(50) * kPi / 180;
    double a1 = f.num(51) * kPi / 180;
    while (a1 <= a0) a1 += 2 * kPi;
    arc(o, at, Ocs(f.xyz(210, gp_XYZ(0, 0, 1))), f.xyz(10), f.num(40), a0, a1, false);
  } else if (t == "ELLIPSE") {
    const gp_XYZ center = f.xyz(10), major = f.xyz(11), n = f.xyz(210, gp_XYZ(0, 0, 1));
    const double ratio = f.num(40, 1), a0 = f.num(41, 0);
    double a1 = f.num(42, 2 * kPi);
    while (a1 <= a0) a1 += 2 * kPi;
    const bool full = std::abs(a1 - a0 - 2 * kPi) < 1e-9;
    const double length = major.Modulus();
    if (length < 1e-12 || !(ratio > 0)) return;
    if (std::abs(std::abs(n.Z()) - 1) < 1e-9 && std::abs(major.Z()) < 1e-9 * length && ratio <= 1) {
      const gp_Elips ellipse(gp_Ax2(pnt(at, center), gp_Dir(0, 0, n.Z() > 0 ? 1 : -1), gp_Dir(major.X(), major.Y(), 0)),
                             length * m_unit, length * ratio * m_unit);
      add(o, full ? BRepBuilderAPI_MakeEdge(ellipse).Edge() : BRepBuilderAPI_MakeEdge(ellipse, a0, a1).Edge());
    } else {
      gp_XYZ normal = n;
      normal.Normalize();
      const gp_XYZ minor = normal.Crossed(major) * ratio;
      std::vector<gp_Pnt> points;
      const int count = std::max(16, int(std::ceil((a1 - a0) / (kPi / 32))));
      for (int k = 0; k <= count; ++k) {
        const double u = a0 + (a1 - a0) * k / count;
        points.push_back(pnt(at, center + major * std::cos(u) + minor * std::sin(u)));
      }
      chain(o, points, false);
    }
  } else if (t == "LWPOLYLINE") {
    const Ocs ocs(f.xyz(210, gp_XYZ(0, 0, 1)));
    const double elevation = f.num(38);
    std::vector<std::array<double, 3>> points;
    for (size_t j = f.begin; j < f.end; ++j) {
      const auto& p = (*f.pairs)[j];
      if (p.code == 10) points.push_back({parse_number(p.value), 0, 0});
      else if (p.code == 20 && !points.empty()) points.back()[1] = parse_number(p.value);
      else if (p.code == 42 && !points.empty()) points.back()[2] = parse_number(p.value);
    }
    if (points.size() == 1) {
      add(o, BRepBuilderAPI_MakeVertex(pnt(at, ocs.to_wcs(points[0][0], points[0][1], elevation))).Vertex());
      return;
    }
    const size_t segments = points.size() < 2 ? 0 : points.size() - 1 + ((f.integer(70) & 1) ? 1 : 0);
    for (size_t k = 0; k < segments; ++k) {
      const auto& p = points[k];
      const auto& q = points[(k + 1) % points.size()];
      bulge(o, at, ocs, elevation, gp_XY(p[0], p[1]), gp_XY(q[0], q[1]), p[2]);
    }
  } else if (t == "SPLINE" || t == "HELIX") {
    std::vector<gp_Pnt> poles, fits;
    std::vector<double> knots, weights;
    for (size_t j = f.begin; j < f.end; ++j) {
      const auto& p = (*f.pairs)[j];
      switch (p.code) {
        case 10: poles.emplace_back(parse_number(p.value), 0, 0); break;
        case 20: if (!poles.empty()) poles.back().SetY(parse_number(p.value)); break;
        case 11: fits.emplace_back(parse_number(p.value), 0, 0); break;
        case 21: if (!fits.empty()) fits.back().SetY(parse_number(p.value)); break;
        case 40: knots.push_back(parse_number(p.value)); break;
        case 41: weights.push_back(parse_number(p.value)); break;
        default: break;
      }
    }
    for (auto& p : poles) p = pnt(at, p.XYZ());
    for (auto& p : fits) p = pnt(at, p.XYZ());
    const int degree = f.integer(71, 3);
    bool built = false;
    if (degree >= 1 && degree <= Geom_BSplineCurve::MaxDegree() && poles.size() >= 2 && knots.size() == poles.size() + size_t(degree) + 1 &&
        (weights.empty() || weights.size() == poles.size())) {
      std::vector<double> distinct;
      std::vector<int> multiplicities;
      for (double k : knots) {
        if (!distinct.empty() && k < distinct.back()) { distinct.clear(); break; }
        if (!distinct.empty() && k - distinct.back() <= 1e-12 * std::max(1.0, std::abs(k))) ++multiplicities.back();
        else { distinct.push_back(k); multiplicities.push_back(1); }
      }
      if (distinct.size() >= 2) {
        TColgp_Array1OfPnt p(1, int(poles.size()));
        TColStd_Array1OfReal w(1, int(poles.size())), k(1, int(distinct.size()));
        TColStd_Array1OfInteger m(1, int(distinct.size()));
        for (int n = 1; n <= p.Length(); ++n) { p(n) = poles[size_t(n - 1)]; w(n) = weights.empty() ? 1 : weights[size_t(n - 1)]; }
        for (int n = 1; n <= k.Length(); ++n) { k(n) = distinct[size_t(n - 1)]; m(n) = multiplicities[size_t(n - 1)]; }
        add(o, BRepBuilderAPI_MakeEdge(new Geom_BSplineCurve(p, w, k, m, degree)).Edge());
        built = true;
      }
    }
    if (!built && fits.size() >= 2) {
      Handle(TColgp_HArray1OfPnt) points = new TColgp_HArray1OfPnt(1, int(fits.size()));
      for (size_t n = 0; n < fits.size(); ++n) points->SetValue(int(n + 1), fits[n]);
      try {
        GeomAPI_Interpolate interpolate(points, Standard_False, 1e-7);
        interpolate.Perform();
        if (interpolate.IsDone()) { add(o, BRepBuilderAPI_MakeEdge(interpolate.Curve()).Edge()); built = true; }
      } catch (const Standard_Failure&) {
      }
      if (!built) { chain(o, fits, false); built = true; }
    }
    if (!built) throw Error("invalid DXF spline");  // skipped and counted: its control polygon would mislead
  } else if (t == "SOLID" || t == "TRACE") {
    const Ocs ocs(f.xyz(210, gp_XYZ(0, 0, 1)));
    const double z = f.num(30);
    auto corner = [&](int code) { return ocs.to_wcs(f.num(code), f.num(code + 10), z); };
    const gp_XYZ c3 = corner(12), c4 = f.has(13) ? corner(13) : c3;
    std::vector<gp_XY> loop;
    for (const auto& c : {corner(10), corner(11), c4, c3}) {
      const auto p = pnt(at, c);
      if (loop.empty() || (loop.back() - gp_XY(p.X(), p.Y())).Modulus() > 1e-9) loop.emplace_back(p.X(), p.Y());
    }
    fill(o, {loop});
  } else if (t == "3DFACE") {
    const int hidden = f.integer(70);
    std::array<gp_Pnt, 4> c{pnt(at, f.xyz(10)), pnt(at, f.xyz(11)), pnt(at, f.xyz(12)), pnt(at, f.xyz(13, f.xyz(12)))};
    for (int k = 0; k < 4; ++k)
      if (!(hidden & (1 << k))) segment(o, c[size_t(k)], c[size_t((k + 1) % 4)]);
  } else if (t == "LEADER") {
    std::vector<gp_Pnt> points;
    for (size_t j = f.begin; j < f.end; ++j) {
      const auto& p = (*f.pairs)[j];
      if (p.code == 10) points.emplace_back(parse_number(p.value), 0, 0);
      else if (p.code == 20 && !points.empty()) points.back().SetY(parse_number(p.value));
    }
    for (auto& p : points) p = pnt(at, p.XYZ());
    chain(o, points, false);
  } else if (t == "INSERT") {
    insert(f, o, at, f.str(2), false, false);
  } else if (t == "MINSERT") {
    insert(f, o, at, f.str(2), false, true);
  } else if (t == "DIMENSION" || t == "ARC_DIMENSION" || t == "LARGE_RADIAL_DIMENSION") {
    insert(f, o, at, f.str(2), true, false);
  } else if (t == "HATCH") {
    hatch(f, o, at);
  } else if (t == "TEXT" || t == "ATTRIB") {
    text(f, o, at, t == "ATTRIB");
  } else if (t == "MTEXT") {
    mtext(f, o, at);
  } else if (t == "ATTDEF" || t == "VIEWPORT" || t == "WIPEOUT" || t == "LIGHT" || t == "SUN" || t == "SEQEND") {
    // attribute templates, layout windows, masks and lights draw nothing in model space
  } else {
    ++m_unsupported[std::string(t)];
  }
}

void Reader::fill(const Out& o, std::vector<std::vector<gp_XY>> loops) {
  for (auto& loop : loops) {
    std::vector<gp_XY> clean;
    for (const auto& p : loop)
      if (clean.empty() || (clean.back() - p).Modulus() > 1e-7) clean.push_back(p);
    while (clean.size() > 2 && (clean.back() - clean.front()).Modulus() <= 1e-7) clean.pop_back();
    loop = std::move(clean);
  }
  loops.erase(std::remove_if(loops.begin(), loops.end(), [](const auto& l) { return l.size() < 3 || std::abs(signed_area(l)) < 1e-12; }), loops.end());
  if (loops.empty()) return;
  // Nesting by containment: even depth is an outer boundary, odd depth a hole in the smallest outer one around it.
  const size_t n = loops.size();
  std::vector<double> area(n);
  std::vector<int> depth(n, 0);
  for (size_t i = 0; i < n; ++i) area[i] = std::abs(signed_area(loops[i]));
  for (size_t i = 0; i < n; ++i)
    for (size_t j = 0; j < n; ++j)
      if (i != j && area[j] > area[i] && inside(loops[j], loops[i].front())) ++depth[i];
  auto wire = [](std::vector<gp_XY> loop, bool ccw) {
    if ((signed_area(loop) > 0) != ccw) std::reverse(loop.begin(), loop.end());
    BRepBuilderAPI_MakePolygon polygon;
    for (const auto& p : loop) polygon.Add(gp_Pnt(p.X(), p.Y(), 0));
    polygon.Close();
    return polygon.Wire();
  };
  for (size_t i = 0; i < n; ++i) {
    if (depth[i] % 2) continue;
    try {
      BRepBuilderAPI_MakeFace face(gp_Pln(gp::XOY()), wire(loops[i], true), Standard_True);
      for (size_t j = 0; j < n; ++j) {
        if (depth[j] != depth[i] + 1 || !inside(loops[i], loops[j].front())) continue;
        bool nearest = true;  // not inside a smaller outer boundary that also holds it
        for (size_t k = 0; k < n && nearest; ++k)
          nearest = !(k != i && depth[k] == depth[i] && area[k] < area[i] && inside(loops[k], loops[j].front()));
        if (nearest) face.Add(wire(loops[j], false));
      }
      if (face.IsDone()) add(o, face.Face());
      else outline(o, {loops[i]});
    } catch (const Standard_Failure&) {
      outline(o, {loops[i]});
    }
  }
}

bool Reader::pattern(const Out& o, const Place& at, const Ocs& ocs, double elevation, const std::vector<std::vector<gp_XY>>& loops,
                     const std::vector<PatternLine>& lines) {
  struct Edge2 { gp_XY a, b; };
  std::vector<Edge2> edges;
  double xmin = 1e300, ymin = 1e300, xmax = -1e300, ymax = -1e300;
  for (const auto& loop : loops)
    for (size_t i = 0; i < loop.size(); ++i) {
      edges.push_back({loop[i], loop[(i + 1) % loop.size()]});
      xmin = std::min(xmin, loop[i].X()); xmax = std::max(xmax, loop[i].X());
      ymin = std::min(ymin, loop[i].Y()); ymax = std::max(ymax, loop[i].Y());
    }
  if (edges.empty()) return true;
  // Too dense to draw (a fine pattern over a large area): the caller draws the boundary instead.
  double count = 0;
  for (const auto& line : lines) {
    const gp_XY u(std::cos(line.angle), std::sin(line.angle)), n(-u.Y(), u.X());
    const double spacing = std::abs(line.offset.Dot(n));
    if (spacing < 1e-12) continue;
    count += (std::abs(n.X()) * (xmax - xmin) + std::abs(n.Y()) * (ymax - ymin)) / spacing;
  }
  if (count > 20000 || count * double(edges.size()) > 5e7) return false;
  std::vector<std::pair<gp_XY, gp_XY>> segments;
  for (const auto& line : lines) {
    const gp_XY u(std::cos(line.angle), std::sin(line.angle)), n(-u.Y(), u.X());
    const double spacing = line.offset.Dot(n);
    if (std::abs(spacing) < 1e-12) continue;
    double dmin = 1e300, dmax = -1e300;
    for (const gp_XY corner : {gp_XY(xmin, ymin), gp_XY(xmax, ymin), gp_XY(xmin, ymax), gp_XY(xmax, ymax)}) {
      const double d = (corner - line.base).Dot(n);
      dmin = std::min(dmin, d);
      dmax = std::max(dmax, d);
    }
    long k0 = long(std::ceil(std::min(dmin / spacing, dmax / spacing))), k1 = long(std::floor(std::max(dmin / spacing, dmax / spacing)));
    double period = 0;
    for (double d : line.dashes) period += std::abs(d);
    std::vector<double> ts;
    for (long k = k0; k <= k1; ++k) {
      const gp_XY origin = line.base + line.offset * double(k);
      ts.clear();
      for (const auto& e : edges) {
        const double dp = (e.a - origin).Dot(n), dq = (e.b - origin).Dot(n);
        if ((dp > 0) == (dq > 0)) continue;
        const double tp = (e.a - origin).Dot(u), tq = (e.b - origin).Dot(u);
        ts.push_back(tp + (tq - tp) * dp / (dp - dq));
      }
      std::sort(ts.begin(), ts.end());
      for (size_t i = 0; i + 1 < ts.size(); i += 2) {
        const double t0 = ts[i], t1 = ts[i + 1];
        if (period < 1e-12) { segments.push_back({origin + u * t0, origin + u * t1}); continue; }
        double pos = std::floor(t0 / period) * period;
        while (pos < t1) {
          for (double d : line.dashes) {
            const double a = std::max(pos, t0), b = std::min(pos + std::abs(d), t1);
            if (d > 0 && b > a) segments.push_back({origin + u * a, origin + u * b});
            pos += std::abs(d);
            if (pos >= t1) break;
          }
          if (segments.size() > m_patternBudget) return false;
        }
      }
      if (segments.size() > m_patternBudget) return false;
    }
  }
  m_patternBudget -= segments.size();
  for (const auto& [a, b] : segments)
    segment(o, pnt(at, ocs.to_wcs(a.X(), a.Y(), elevation)), pnt(at, ocs.to_wcs(b.X(), b.Y(), elevation)));
  return true;
}

void Reader::hatch(const Fields& f, Out& o, const Place& at) {
  const Ocs ocs(f.xyz(210, gp_XYZ(0, 0, 1)));
  const double elevation = f.num(30);
  const bool solid = f.integer(70) == 1 || f.integer(450) != 0;  // solid fill, or a gradient (drawn in its entity colour)
  const auto& pairs = *f.pairs;
  size_t start = f.begin;
  while (start < f.end && pairs[start].code != 91) ++start;
  if (start == f.end) return;
  Cursor c{pairs, start, f.end};
  const int paths = c.integer(91);
  std::vector<std::vector<gp_XY>> loops;  // OCS, drawing units
  auto sweep = [](std::vector<gp_XY>& out, const gp_XY& center, const gp_XY& major, double ratio, double a0, double a1) {
    // a0 -> a1 (radians, either direction) on the ellipse center + cos(t) major + sin(t) minor.
    const gp_XY minor(-major.Y() * ratio, major.X() * ratio);
    const int n = std::max(2, int(std::ceil(std::abs(a1 - a0) / (kPi / 32))));
    for (int k = 0; k <= n; ++k) {
      const double t = a0 + (a1 - a0) * k / n;
      out.push_back(center + major * std::cos(t) + minor * std::sin(t));
    }
  };
  for (int path = 0; path < paths && c.at(92); ++path) {
    const int flags = c.integer(92);
    std::vector<gp_XY> points;
    if (flags & 2) {  // polyline boundary
      const bool bulges = c.integer(72) != 0;
      c.integer(73);
      const int n = c.integer(93);
      std::vector<std::array<double, 3>> v;
      for (int k = 0; k < n && c.at(10); ++k) {
        const double x = c.number(10), y = c.number(20);
        v.push_back({x, y, bulges && c.at(42) ? c.number(42) : 0});
      }
      for (size_t k = 0; k < v.size(); ++k) {
        const auto& p = v[k];
        const auto& q = v[(k + 1) % v.size()];
        points.emplace_back(p[0], p[1]);
        if (std::abs(p[2]) > 1e-12 && v.size() > 1) {
          const double b = p[2], dx = q[0] - p[0], dy = q[1] - p[1], chord = std::hypot(dx, dy);
          if (chord < 1e-12) continue;
          const gp_XY center((p[0] + q[0]) / 2 - dy * (1 - b * b) / (4 * b), (p[1] + q[1]) / 2 + dx * (1 - b * b) / (4 * b));
          const double r = chord * (1 + b * b) / (4 * std::abs(b)), a0 = std::atan2(p[1] - center.Y(), p[0] - center.X());
          sweep(points, center, gp_XY(r, 0), 1, a0, a0 + 4 * std::atan(b));
        }
      }
    } else {
      const int n = c.integer(93);
      for (int k = 0; k < n && c.at(72); ++k) {
        const int type = c.integer(72);
        if (type == 1) {
          const double x0 = c.number(10), y0 = c.number(20), x1 = c.number(11), y1 = c.number(21);
          points.emplace_back(x0, y0);
          points.emplace_back(x1, y1);
        } else if (type == 2 || type == 3) {
          const double cx = c.number(10), cy = c.number(20);
          gp_XY major(0, 0);
          double ratio = 1;
          if (type == 3) {  // one read per statement: argument order is unspecified
            const double mx = c.number(11), my = c.number(21);
            major = gp_XY(mx, my);
            ratio = c.number(40, 1);
          }
          else major = gp_XY(c.number(40), 0);
          double a0 = c.number(50) * kPi / 180, a1 = c.number(51) * kPi / 180;
          const bool ccw = c.integer(73, 1) != 0;
          // Clockwise edges store their angles mirrored (checked against the neighbouring edges of real drawings).
          if (!ccw) { a0 = -a0; a1 = -a1; }
          if (ccw) { while (a1 <= a0) a1 += 2 * kPi; }
          else { while (a1 >= a0) a1 -= 2 * kPi; }
          if (std::abs(std::abs(a1 - a0) - 4 * kPi) < 1e-9) a1 = a0 + (ccw ? 2 : -2) * kPi;
          sweep(points, gp_XY(cx, cy), major, ratio, a0, a1);
        } else if (type == 4) {
          const int degree = c.integer(94, 3);
          const bool rational = c.integer(73) != 0;
          c.integer(74);
          const int knotCount = c.integer(95), poleCount = c.integer(96);
          std::vector<double> knots, weights;
          std::vector<gp_Pnt> poles;
          for (int i = 0; i < knotCount && c.at(40); ++i) knots.push_back(c.number(40));
          for (int i = 0; i < poleCount && c.at(10); ++i) {
            const double x = c.number(10), y = c.number(20);
            poles.emplace_back(x, y, 0);
            if (c.at(42)) weights.push_back(c.number(42));
          }
          while (c.at(42)) weights.push_back(c.number(42));
          if (c.at(97)) {
            const int fitCount = c.integer(97);
            for (int i = 0; i < fitCount && c.at(11); ++i) { c.number(11); c.number(21); }
            if (c.at(12)) { c.number(12); c.number(22); c.number(13); c.number(23); }
          }
          bool sampled = false;
          try {
            std::vector<double> distinct;
            std::vector<int> mult;
            for (double kv : knots) {
              if (!distinct.empty() && std::abs(kv - distinct.back()) <= 1e-12) ++mult.back();
              else { distinct.push_back(kv); mult.push_back(1); }
            }
            if (poles.size() >= 2 && knots.size() == poles.size() + size_t(degree) + 1 && distinct.size() >= 2) {
              TColgp_Array1OfPnt p(1, int(poles.size()));
              TColStd_Array1OfReal w(1, int(poles.size())), kk(1, int(distinct.size()));
              TColStd_Array1OfInteger m(1, int(distinct.size()));
              for (int i = 1; i <= p.Length(); ++i) { p(i) = poles[size_t(i - 1)]; w(i) = rational && weights.size() == poles.size() ? weights[size_t(i - 1)] : 1; }
              for (int i = 1; i <= kk.Length(); ++i) { kk(i) = distinct[size_t(i - 1)]; m(i) = mult[size_t(i - 1)]; }
              Handle(Geom_BSplineCurve) curve = new Geom_BSplineCurve(p, w, kk, m, degree);
              const int samples = int(std::max<size_t>(16, poles.size() * 8));
              for (int i = 0; i <= samples; ++i) {
                const gp_Pnt q = curve->Value(curve->FirstParameter() + (curve->LastParameter() - curve->FirstParameter()) * i / samples);
                points.emplace_back(q.X(), q.Y());
              }
              sampled = true;
            }
          } catch (const Standard_Failure&) {
          }
          if (!sampled)
            for (const auto& p : poles) points.emplace_back(p.X(), p.Y());
        } else {
          break;
        }
      }
    }
    if (c.at(97)) {
      const int sources = c.integer(97);
      for (int k = 0; k < sources && c.at(330); ++k) c.skip();
    }
    if (points.size() >= 3) loops.push_back(std::move(points));
  }
  if (loops.empty()) return;
  auto mapped = [&] {
    std::vector<std::vector<gp_XY>> out;
    for (const auto& loop : loops) {
      std::vector<gp_XY> m;
      for (const auto& p : loop) {
        const auto q = pnt(at, ocs.to_wcs(p.X(), p.Y(), elevation));
        m.emplace_back(q.X(), q.Y());
      }
      out.push_back(std::move(m));
    }
    return out;
  };
  if (solid) { fill(o, mapped()); return; }
  std::vector<PatternLine> lines;
  while (c.i < c.end && !c.at(78) && !c.at(98) && !c.at(450)) c.skip();
  if (c.at(78)) {
    const int count = c.integer(78);
    for (int k = 0; k < count && c.at(53); ++k) {
      PatternLine line;
      line.angle = c.number(53) * kPi / 180;
      const double bx = c.number(43), by = c.number(44), ox = c.number(45), oy = c.number(46);  // in order
      line.base = gp_XY(bx, by);
      line.offset = gp_XY(ox, oy);
      const int dashes = c.integer(79);
      for (int d = 0; d < dashes && c.at(49); ++d) line.dashes.push_back(c.number(49));
      lines.push_back(std::move(line));
    }
  }
  if (lines.empty() || !pattern(o, at, ocs, elevation, loops, lines)) {
    if (!lines.empty()) ++m_patternOutlines;
    outline(o, mapped());
  }
}

// Text is shaped and outlined by drawing_text.cpp: the TrueType font a style names (in the Windows fonts folders or
// beside the drawing), else its family; a shape font (.shx) or a font not found has a plain sans-serif stand in.
std::vector<std::filesystem::path> Reader::font_folders(const std::filesystem::path& file) {
  std::vector<std::filesystem::path> out;
#ifdef _WIN32
  if (const wchar_t* windows = _wgetenv(L"WINDIR"); windows && *windows) out.push_back(std::filesystem::path(windows) / "Fonts");
  if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && *local) out.push_back(std::filesystem::path(local) / "Microsoft" / "Windows" / "Fonts");
#endif
  if (file.has_parent_path()) out.push_back(file.parent_path());
  return out;
}

TextRequest Reader::text_request(std::string_view styleName, std::string text, double height) const {
  TextRequest r;
  r.text = std::move(text);
  r.cap = true;  // a DXF text height is its capitals'
  const auto style = m_styles.find(upper(trimmed(styleName)));
  if (style != m_styles.end()) {
    r.font = style->second.font;
    r.family = style->second.family;
    r.bold = style->second.bold;
    r.italic = style->second.italic;
    r.width = style->second.width;
    r.oblique = style->second.oblique * kPi / 180;
  }
  if (!(height > 0)) height = style != m_styles.end() && style->second.height > 0 ? style->second.height : 2.5;
  r.size = height * m_unit;
  return r;
}

// Parts in colours of their own (MTEXT \C) go into the bodies of those colours.
void Reader::text_shape(const Out& o, const TextRequest& request, const gp_Ax3& at) {
  auto [it, fresh] = m_texts.try_emplace(text_key(request), m_textJobs.size());
  if (fresh) {
    TextJob job;
    job.request = request;
    std::vector<const TextFormat*> parts;  // a part's own colour sends it to the placeholder of that colour
    if (request.spans.empty()) parts.push_back(&request);
    for (const auto& span : request.spans) parts.push_back(&span);
    for (const TextFormat* part : parts) {
      TopoDS_Compound& into = part->color == TextFormat::kInherit ? job.into : job.colored[part->color];
      if (into.IsNull()) m_builder.MakeCompound(into);
    }
    m_textJobs.push_back(std::move(job));
  }
  TextJob& job = m_textJobs[it->second];
  ++job.uses;
  gp_Trsf place;
  place.SetDisplacement(gp_Ax3(gp::XOY()), at);
  const TopLoc_Location location(place);
  if (!job.into.IsNull()) add(o, job.into.Moved(location));
  for (const auto& [color, part] : job.colored) {
    Out own = o;
    own.color = color;
    add(own, part.Moved(location));
  }
}

void Reader::text(const Fields& f, Out& o, const Place& at, bool attrib) {
  if (attrib && (f.integer(70) & 1)) return;  // invisible attribute
  const std::string raw = decode(f.str(1));
  std::string s = text_codes(raw);
  if (blank(s)) return;
  const Ocs ocs(f.xyz(210, gp_XYZ(0, 0, 1)));
  TextRequest r = text_request(f.str(7, "STANDARD"), std::move(s), f.num(40));
  r.width = f.num(41, r.width);
  if (!(r.width > 0)) r.width = 1;
  if (f.has(51)) r.oblique = f.num(51) * kPi / 180;
  r.spans = text_spans(raw, r);
  int ha = f.integer(72), va = f.integer(attrib ? 74 : 73);
  const gp_XYZ p1 = f.xyz(10), p2 = f.has(11) ? f.xyz(11) : p1;
  double rotation = f.num(50) * kPi / 180;
  gp_XYZ anchor = (ha == 0 && va == 0) ? p1 : p2;
  if (ha == 3 || ha == 5) {  // aligned / fit: from p1 along p1 -> p2 and as long, larger (aligned) or wider (fit)
    anchor = p1;
    if ((p2 - p1).Modulus() > 1e-12) {
      rotation = std::atan2(p2.Y() - p1.Y(), p2.X() - p1.X());
      r.fit = (p2 - p1).Modulus() * m_unit;
      r.aligned = ha == 3;
    }
    ha = 0;
    va = 0;
  } else if (ha == 4) {  // middle
    ha = 1;
    va = 2;
  }
  r.h = ha == 1 ? TextRequest::Center : ha == 2 ? TextRequest::Right : TextRequest::Left;
  r.v = va == 1 ? TextRequest::Descent : va == 2 ? TextRequest::Middle : va == 3 ? TextRequest::Top : TextRequest::Baseline;
  const gp_XYZ xo = ocs.ax * std::cos(rotation) + ocs.ay * std::sin(rotation), yo = ocs.ay * std::cos(rotation) - ocs.ax * std::sin(rotation);
  gp_XYZ x(xo.X(), xo.Y(), 0), y(yo.X(), yo.Y(), 0);
  if (x.Modulus() < 1e-9) return;
  x.Normalize();
  y = y.Modulus() > 1e-9 ? y.Normalized() : gp_XYZ(-x.Y(), x.X(), 0);
  gp_XYZ normal = x.Crossed(y);
  if (normal.Modulus() < 1e-9) return;
  normal.Normalize();
  const int mirror = f.integer(71);
  gp_Dir d(normal), xd(x);
  if ((mirror & 2) && (mirror & 4)) xd.Reverse();
  else if (mirror & 2) { d.Reverse(); xd.Reverse(); }  // backwards
  else if (mirror & 4) d.Reverse();                    // upside down
  text_shape(o, r, gp_Ax3(pnt(at, ocs.to_wcs(anchor)), d, xd));
}

void Reader::mtext(const Fields& f, Out& o, const Place& at) {
  std::string raw;
  for (size_t j = f.begin; j < f.end; ++j)
    if ((*f.pairs)[j].code == 3) raw += decode((*f.pairs)[j].value);
  raw += decode(f.str(1));
  TextRequest r = text_request(f.str(7, "STANDARD"), "", f.num(40));
  r.spans = mtext_spans(raw, r, m_unit, r.paragraphs);
  std::string all;
  for (const auto& span : r.spans) all += span.text + span.bottom;
  if (blank(all)) return;
  const Ocs ocs(f.xyz(210, gp_XYZ(0, 0, 1)));
  const int attachment = std::clamp(f.integer(71, 1), 1, 9);
  gp_XYZ x;
  if (f.has(11)) x = f.xyz(11);
  else {
    const double rotation = f.num(50) * kPi / 180;
    x = ocs.ax * std::cos(rotation) + ocs.ay * std::sin(rotation);
  }
  x.SetZ(0);
  if (x.Modulus() < 1e-9) x = gp_XYZ(1, 0, 0);
  x.Normalize();
  r.wrap = f.num(41) * m_unit;
  const double factor = f.num(44, 1);
  // At 1.0, 5/3 of the text height from baseline to baseline (of a larger height in a line with larger text).
  r.spacing = 5.0 / 3.0 * r.size * (factor > 0 ? factor : 1);
  const int column = (attachment - 1) % 3, row = (attachment - 1) / 3;
  r.h = column == 1 ? TextRequest::Center : column == 2 ? TextRequest::Right : TextRequest::Left;
  // Top: the first line's capitals touch the insertion point; middle and bottom: the block's middle and last baseline.
  r.v = row == 0 ? TextRequest::Top : row == 1 ? TextRequest::Middle : TextRequest::Bottom;
  const gp_XYZ normal(0, 0, ocs.az.Z() < 0 && ocs.planar() ? -1 : 1);
  text_shape(o, r, gp_Ax3(pnt(at, f.xyz(10)), gp_Dir(normal), gp_Dir(x)));
}

Drawing Reader::read() {
  // Sections and their entities ("0 TYPE" and the groups up to the next 0).
  std::map<std::string, std::vector<Entity>> sections;
  std::vector<Entity>* current = nullptr;
  for (size_t i = 0; i < m_pairs.size();) {
    if (m_pairs[i].code != 0) { ++i; continue; }
    size_t end = i + 1;
    while (end < m_pairs.size() && m_pairs[end].code != 0) ++end;
    const auto type = trimmed(m_pairs[i].value);
    Fields f{&m_pairs, i + 1, end};
    if (type == "SECTION") current = &sections[std::string(trimmed(f.str(2)))];
    else if (type == "ENDSEC") current = nullptr;
    else if (current) current->push_back({type, f});
    i = end;
  }
  // Header variables: "9 $NAME" then its value groups.
  for (size_t i = 0; i + 1 < m_pairs.size(); ++i) {
    if (m_pairs[i].code != 9) continue;
    const auto name = trimmed(m_pairs[i].value);
    auto value = [&](int code) -> std::string_view {
      for (size_t j = i + 1; j < m_pairs.size() && m_pairs[j].code != 9 && m_pairs[j].code != 0; ++j)
        if (m_pairs[j].code == code) return m_pairs[j].value;
      return {};
    };
    try {
      if (name == "$INSUNITS") {
        static const std::map<int, double> units = {{0, 1}, {1, 25.4}, {2, 304.8}, {3, 1609344}, {4, 1}, {5, 10}, {6, 1000},
                                                    {7, 1e6}, {8, 25.4e-6}, {9, 0.0254}, {10, 914.4}, {11, 1e-7}, {12, 1e-6},
                                                    {13, 1e-3}, {14, 100}, {15, 1e4}, {16, 1e5}, {17, 1e12}, {21, 1200.0 / 3937 * 1000}};
        const int code = int(parse_number(value(70)));
        const auto it = units.find(code);
        if (it != units.end()) m_unit = it->second;
        else m_warnings.push_back("Unknown drawing units (" + std::to_string(code) + "): read as millimetres");
      } else if (name == "$ACADVER") {
        m_utf8 = std::string(trimmed(value(1))) >= "AC1021";  // AutoCAD 2007 and later write UTF-8
      } else if (name == "$DWGCODEPAGE") {
        const auto page = upper(trimmed(value(3)));
        const auto digits = page.find_first_of("0123456789");
        if (page.rfind("ANSI_", 0) == 0 || page.rfind("DOS", 0) == 0) {
          if (digits != std::string::npos) m_codepage = std::atoi(page.c_str() + digits);
        } else if (page == "MACINTOSH") m_codepage = 10000;
      } else if (name == "$EXTMIN" || name == "$EXTMAX") {
        const gp_XYZ p(parse_number(value(10)), parse_number(value(20)), 0);
        (name == "$EXTMIN" ? m_extMin : m_extMax) = p;
        if (name == "$EXTMAX") m_hasExtents = true;
      }
    } catch (const Error&) {
    }
  }
  if (sections.count("TABLES")) tables(sections["TABLES"]);
  // Linetypes are drawn at a screen size (app Drawing2D), whatever the drawing's units and LTSCALE: their dashes as
  // acad.lin and acadiso.lin size them. The standard names the file defines say how much larger its own are (their first
  // dash against acad.lin's, in mm); without one, its typical dash is taken as 12.7 mm.
  std::vector<double> sizes, dashes;
  for (const auto& [name, d] : m_linetypes) {
    for (double v : d)
      if (v > 0) dashes.push_back(v);
    if (d.empty() || !(d[0] > 0)) continue;
    std::string base = name;
    double factor = 1;
    if (base.size() > 2 && base.compare(base.size() - 2, 2, "X2") == 0) factor = 2, base.resize(base.size() - 2);
    else if (base.size() > 1 && base.back() == '2') factor = 0.5, base.pop_back();
    static const std::map<std::string, double> first = {{"DASHED", 12.7}, {"HIDDEN", 6.35}, {"CENTER", 31.75}, {"PHANTOM", 31.75},
                                                        {"DASHDOT", 12.7}, {"BORDER", 12.7}, {"DIVIDE", 12.7}};
    if (const auto it = first.find(base); it != first.end()) sizes.push_back(it->second * factor / d[0]);
  }
  auto median = [](std::vector<double> v) {
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
  };
  if (!dashes.empty()) {
    const double size = sizes.empty() ? 12.7 / median(dashes) : median(sizes);
    for (auto& [name, d] : m_linetypes)
      for (double& v : d) v = std::round(v * size * 1000) / 1000;
  }
  std::map<std::string, Layer> decoded;  // keyed by the decoded name, which is what entities are looked up by
  for (auto& [key, layer] : m_layers) {
    if (const auto it = m_linetypes.find(upper(layer.linetype)); it != m_linetypes.end()) layer.pattern = it->second;
    layer.name = decode(layer.name);
    layer.linetype = decode(layer.linetype);
    decoded[upper(layer.name)] = layer;
  }
  m_layers = std::move(decoded);
  if (sections.count("BLOCKS")) blocks(sections["BLOCKS"]);
  m_model = sections["ENTITIES"];
  m_total = std::max<size_t>(1, m_model.size());

  // A drawing far from its origin is read near (0,0): its extents' centre, or its first point, becomes the origin.
  Drawing out;
  gp_XYZ center(0, 0, 0);
  bool distant = false;
  if (m_hasExtents && std::abs(m_extMin.X()) < 1e19 && std::abs(m_extMax.X()) < 1e19 && m_extMax.X() >= m_extMin.X())
    center = (m_extMin + m_extMax) / 2;
  else
    for (const auto& e : m_model)
      if (e.f.has(10)) {
        try { center = gp_XYZ(e.f.num(10), e.f.num(20), 0); } catch (const Error&) {}
        break;
      }
  center *= m_unit;
  if (std::max(std::abs(center.X()), std::abs(center.Y())) > 1e5) {
    distant = true;
    out.origin = gp_XYZ(std::round(center.X() / 1000) * 1000, std::round(center.Y() / 1000) * 1000, 0);
  }
  Space model;
  Place at{&model, distant ? out.origin.X() : 0, distant ? out.origin.Y() : 0, true};
  run(m_model, 0, m_model.size(), at);
  finish();
  for (auto it = model.groups.begin(); it != model.groups.end();) it = drawn(it->second) ? std::next(it) : model.groups.erase(it);

  std::map<std::string, std::vector<double>> patterns;  // by the upper-case decoded name
  for (const auto& [name, d] : m_linetypes) patterns[upper(decode(name))] = d;
  for (const auto& [key, shape] : model.groups) {
    const auto& [layer, color, linetype, weight, scale] = key;
    const uint32_t rgb = color == kByLayer ? layer_color(layer) : color == kByBlock ? kNoColor : color;
    // Its own linetype, lineweight and linetype scale (by block in model space: continuous and the default); the layer's
    // own are by layer. The scale only where it has dashes.
    Drawing::Pen pen{rgb, linetype == "BYBLOCK" ? "Continuous" : linetype, weight == -2 ? -3 : weight, scale};
    if (!pen.linetype.empty() && upper(pen.linetype) == upper(layer_linetype(layer))) pen.linetype.clear();
    if (pen.lineweight != -1 && pen.lineweight == layer_weight(layer)) pen.lineweight = -1;
    if (upper(pen.linetype.empty() ? layer_linetype(layer) : pen.linetype) == "CONTINUOUS") pen.scale = 1;
    if (const auto p = patterns.find(upper(pen.linetype)); !pen.linetype.empty() && p != patterns.end()) out.patterns[pen.linetype] = p->second;
    out.add(layer, shape, pen);
    if (color == kByLayer) out.by_layer[layer] = rgb;
    const auto it = m_layers.find(upper(layer));
    out.visible[layer] = it == m_layers.end() || it->second.visible;
    if (it != m_layers.end()) out.layer_info[layer] = it->second.info();
  }
  out.warnings = m_warnings;
  if (!m_unsupported.empty()) {
    std::vector<std::pair<int, std::string>> counts;
    for (const auto& [type, count] : m_unsupported) counts.push_back({count, type});
    std::sort(counts.rbegin(), counts.rend());
    std::string list;
    for (const auto& [count, type] : counts) list += (list.empty() ? "" : ", ") + std::to_string(count) + " " + type;
    out.warnings.push_back("Not shown (no reader for these entities): " + list);
  }
  if (m_broken) out.warnings.push_back(std::to_string(m_broken) + " entities could not be read and were skipped");
  if (m_paper) out.warnings.push_back(std::to_string(m_paper) + " paper-space entities (layouts) are not shown; model space is");
  if (m_missing) out.warnings.push_back(std::to_string(m_missing) + " block references name blocks the file does not contain");
  if (m_xrefs) out.warnings.push_back(std::to_string(m_xrefs) + " external references (xrefs) are not loaded");
  if (m_noFont) out.warnings.push_back(std::to_string(m_noFont) + " texts are not shown: no font could be loaded");
  if (m_patternOutlines) out.warnings.push_back(std::to_string(m_patternOutlines) + " hatch patterns too dense to draw are shown as their boundaries");
  return out;
}

}  // namespace

Drawing read_dxf(const std::filesystem::path& file, const ImportOptions& options) {
  Reader reader(file, options);
  auto drawing = reader.read();
  if (drawing.empty()) throw Error("DXF contains no drawable geometry");
  return drawing;
}

}  // namespace opad::detail
