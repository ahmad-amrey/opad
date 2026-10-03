// AutoCAD shape fonts (shx_font.hpp, UI-92), from the shape specification bytes AutoCAD's customization guide describes:
// vectors of 16 directions, pen up and down, scale, a location stack, subshapes, displacements, octant, fractional and
// bulge arcs, and commands for vertical text only (skipped: text is horizontal here).
#include "shx_font.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

namespace opad::detail {

struct ShxPen {
  gp_XY at{0, 0};
  bool down = true;  // each shape starts drawing
  double scale = 1;
  std::vector<gp_XY> stack;
  bool open = false;  // the last stroke ends where the pen is
};

namespace {
constexpr double kPi = 3.14159265358979323846;

void move(ShxPen& pen, ShxFont::Glyph& g, const gp_XY& to) {
  if (pen.down && (to - pen.at).Modulus() > 1e-9) {
    if (!pen.open || g.strokes.empty()) g.strokes.push_back({pen.at});
    g.strokes.back().push_back(to);
    pen.open = true;
  } else if (!pen.down) {
    pen.open = false;
  }
  pen.at = to;
}

// An arc of radius r about the centre that puts the pen at angle a0, to a1 (degrees, either way round); `end`: exactly there.
void arc(ShxPen& pen, ShxFont::Glyph& g, double r, double a0, double a1, const gp_XY* end = nullptr) {
  if (!(r > 0)) return;
  const gp_XY centre = pen.at - gp_XY(std::cos(a0 * kPi / 180), std::sin(a0 * kPi / 180)) * r;
  const int n = std::max(2, int(std::ceil(std::abs(a1 - a0) / 11.25)));
  for (int k = 1; k <= n; ++k) {
    const double a = (a0 + (a1 - a0) * k / n) * kPi / 180;
    move(pen, g, k == n && end ? *end : centre + gp_XY(std::cos(a), std::sin(a)) * r);
  }
}

// From the pen by (dx, dy) along an arc of this bulge (-127..127: twice the arc's height over its chord, times 127).
void bulge(ShxPen& pen, ShxFont::Glyph& g, double dx, double dy, int value) {
  const gp_XY p = pen.at, q = p + gp_XY(dx, dy);
  const double b = value / 127.0, chord = std::hypot(dx, dy);
  if (std::abs(b) < 1e-9 || chord < 1e-12) return move(pen, g, q);
  const gp_XY c((p.X() + q.X()) / 2 - dy * (1 - b * b) / (4 * b), (p.Y() + q.Y()) / 2 + dx * (1 - b * b) / (4 * b));
  const double start = std::atan2(p.Y() - c.Y(), p.X() - c.X()) * 180 / kPi;
  arc(pen, g, (p - c).Modulus(), start, start + 4 * std::atan(b) * 180 / kPi, &q);
}

uint32_t u16(const std::vector<uint8_t>& d, size_t at) { return at + 1 < d.size() ? uint32_t(d[at] | d[at + 1] << 8) : 0; }
}  // namespace

void ShxFont::draw(const std::vector<uint8_t>& d, ShxPen& pen, Glyph& g, int depth) const {
  static const double directions[16][2] = {{1, 0}, {1, .5}, {1, 1}, {.5, 1}, {0, 1}, {-.5, 1}, {-1, 1}, {-1, .5},
                                           {-1, 0}, {-1, -.5}, {-1, -1}, {-.5, -1}, {0, -1}, {.5, -1}, {1, -1}, {1, -.5}};
  size_t i = 0;
  auto u8 = [&]() -> int { return i < d.size() ? int(d[i++]) : 0; };
  auto s8 = [&]() -> int { return i < d.size() ? int(int8_t(d[i++])) : 0; };
  bool skip = false;  // code 14: the next command is for vertical text
  while (i < d.size()) {
    const int code = d[i++];
    const bool act = !skip;
    skip = false;
    if (code >> 4) {  // a vector: length, direction
      if (act) move(pen, g, pen.at + gp_XY(directions[code & 15][0], directions[code & 15][1]) * ((code >> 4) * pen.scale));
      continue;
    }
    switch (code) {
      case 0: return;
      case 1: if (act) pen.down = true; break;
      case 2: if (act) pen.down = false, pen.open = false; break;
      case 3: if (const int k = u8(); act && k) pen.scale /= k; break;
      case 4: if (const int k = u8(); act && k) pen.scale *= k; break;
      case 5: if (act && pen.stack.size() < 16) pen.stack.push_back(pen.at); break;
      case 6:
        if (act && !pen.stack.empty()) pen.at = pen.stack.back(), pen.stack.pop_back(), pen.open = false;
        break;
      case 7: {
        uint32_t n = uint32_t(u8());
        if (unicode) n = n << 8 | uint32_t(u8());
        if (const auto it = m_defs.find(n); act && depth < 8 && it != m_defs.end()) draw(it->second, pen, g, depth + 1);
        break;
      }
      case 8: {
        const int dx = s8(), dy = s8();
        if (act) move(pen, g, pen.at + gp_XY(dx, dy) * pen.scale);
        break;
      }
      case 9:
        for (int guard = 0; i < d.size() && guard < 4096; ++guard) {
          const int dx = s8(), dy = s8();
          if (dx == 0 && dy == 0) break;
          if (act) move(pen, g, pen.at + gp_XY(dx, dy) * pen.scale);
        }
        break;
      case 10: case 11: {  // octant arc: radius, octants; fractional: start and end offsets, radius high and low, octants
        const int startOffset = code == 11 ? u8() : 0, endOffset = code == 11 ? u8() : 0;
        const int high = code == 11 ? u8() : 0;  // one read per statement: the order is the file's
        const int radius = high << 8 | u8();
        const int octants = u8();
        const bool ccw = !(octants & 0x80);
        const int first = (octants >> 4) & 7, count = (octants & 15) ? (octants & 15) : 8;
        const double sign = ccw ? 1 : -1, a0 = first * 45 + sign * startOffset * 45.0 / 256;
        const double a1 = first * 45 + sign * ((count - (endOffset ? 1 : 0)) * 45 + endOffset * 45.0 / 256);
        if (act) arc(pen, g, radius * pen.scale, a0, a1);
        break;
      }
      case 12: {
        const int dx = s8(), dy = s8(), b = s8();
        if (act) bulge(pen, g, dx * pen.scale, dy * pen.scale, b);
        break;
      }
      case 13:
        for (int guard = 0; i < d.size() && guard < 4096; ++guard) {
          const int dx = s8(), dy = s8();
          if (dx == 0 && dy == 0) break;
          const int b = s8();
          if (act) bulge(pen, g, dx * pen.scale, dy * pen.scale, b);
        }
        break;
      case 14: skip = true; break;
      default: break;
    }
  }
}

std::shared_ptr<const ShxFont> ShxFont::parse(const std::vector<uint8_t>& d) {
  const auto end = std::find(d.begin(), d.end(), uint8_t(0x1A));
  if (end == d.end()) return nullptr;
  const std::string signature(d.begin(), end);
  auto font = std::make_shared<ShxFont>();
  size_t at = size_t(end - d.begin()) + 1;
  // A definition: its name up to a zero, then its bytes.
  auto definition = [&d](size_t from, size_t length) {
    const size_t stop = std::min(d.size(), from + length);
    size_t name = from;
    while (name < stop && d[name] != 0) ++name;
    return std::vector<uint8_t>(d.begin() + long(std::min(stop, name + 1)), d.begin() + long(stop));
  };
  if (signature.rfind("AutoCAD-86 shapes 1.", 0) == 0) {
    const uint32_t count = u16(d, at + 4);
    size_t index = at + 6, body = index + size_t(count) * 4;
    for (uint32_t k = 0; k < count && body < d.size(); ++k, index += 4) {
      const uint32_t number = u16(d, index), length = u16(d, index + 2);
      font->m_defs[number] = definition(body, length);
      body += length;
    }
  } else if (signature.rfind("AutoCAD-86 unifont 1.", 0) == 0) {
    const uint32_t infoLength = u16(d, at + 4);
    font->m_defs[0] = definition(at + 6, infoLength);
    font->unicode = true;
    for (size_t p = at + 6 + infoLength; p + 4 <= d.size();) {
      const uint32_t number = u16(d, p), length = u16(d, p + 2);
      font->m_defs[number] = definition(p + 4, length);
      p += 4 + length;
    }
  } else {
    return nullptr;  // a big font, or not a shape font
  }
  const auto info = font->m_defs.find(0);
  if (info == font->m_defs.end() || info->second.size() < 2 || info->second[0] == 0) return nullptr;
  font->above = info->second[0];
  font->below = info->second[1];
  for (const auto& [number, bytes] : font->m_defs) {
    if (number == 0) continue;
    ShxPen pen;
    Glyph g;
    font->draw(bytes, pen, g, 0);
    g.advance = pen.at;
    font->m_glyphs[number] = std::move(g);
  }
  return font;
}

std::shared_ptr<const ShxFont> ShxFont::load(const std::filesystem::path& file) {
  static std::mutex lock;
  static std::map<std::filesystem::path, std::shared_ptr<const ShxFont>> fonts;  // null: tried, not read
  std::lock_guard<std::mutex> guard(lock);
  const auto found = fonts.find(file);
  if (found != fonts.end()) return found->second;
  std::ifstream in(file, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return fonts[file] = in || in.eof() ? parse(bytes) : nullptr;
}

const ShxFont::Glyph* ShxFont::glyph(uint32_t c) const {
  std::vector<uint32_t> tries{c};
  if (c == 0xB0) tries.push_back(127);  // AutoCAD's degree, plus-minus and diameter signs
  if (c == 0xB1) tries.push_back(128);
  if (c == 0xD8 || c == 0x2205 || c == 0x2300) tries.insert(tries.end(), {0x2205, 0x2300, 0xD8, 129});
  for (uint32_t t : tries)
    if (const auto it = m_glyphs.find(t); it != m_glyphs.end()) return &it->second;
  return nullptr;
}

const std::vector<std::filesystem::path>& shx_folders() {
  static const std::vector<std::filesystem::path> folders = [] {
    std::vector<std::filesystem::path> out;
#ifdef _WIN32
    std::error_code error;
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramW6432"})
      if (const wchar_t* root = _wgetenv(variable); root && *root)
        for (std::filesystem::directory_iterator it(std::filesystem::path(root) / "Autodesk", error), stop; !error && it != stop; it.increment(error))
          if (const auto fonts = it->path() / "Fonts"; std::filesystem::is_directory(fonts, error) &&
                                                        std::find(out.begin(), out.end(), fonts) == out.end())
            out.push_back(fonts);
#endif
    return out;
  }();
  return folders;
}

}  // namespace opad::detail
