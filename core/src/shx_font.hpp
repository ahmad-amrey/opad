#pragma once
// AutoCAD shape fonts (.shx), read from the user's own files (DWG TrueView's or AutoCAD's Fonts folder, or beside the
// drawing) so text in txt, romans, isocp ... is drawn in their strokes as AutoCAD draws it (UI-92). Shapes 1.0/1.1 and
// unifont 1.0 files are read; big fonts (Asian code pages) are not. Not a public header.
#include <gp_XY.hxx>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace opad::detail {

struct ShxPen;

class ShxFont {
 public:
  struct Glyph {
    std::vector<std::vector<gp_XY>> strokes;  // polylines in vector units, the pen starting at (0, 0); arcs sampled
    gp_XY advance{0, 0};                       // where the pen ends: the next character's start
  };
  // The font in this file, read once per process (shared between threads); null when it is not a shape font read here.
  static std::shared_ptr<const ShxFont> load(const std::filesystem::path& file);
  // Read from the file's bytes; null when they are not a shape font read here.
  static std::shared_ptr<const ShxFont> parse(const std::vector<uint8_t>& bytes);
  double above = 0, below = 0;  // capital height and descent, vector units
  bool unicode = false;          // unifont: shapes numbered by code point
  // A character (Unicode; a shapes 1.0 font by its own numbers, which are ASCII and Latin-1, °, ± and the diameter sign
  // also as AutoCAD's 127, 128 and 129); null when the font has none.
  const Glyph* glyph(uint32_t c) const;

 private:
  std::map<uint32_t, std::vector<uint8_t>> m_defs;  // shape number -> its bytes (the name dropped)
  std::map<uint32_t, Glyph> m_glyphs;
  void draw(const std::vector<uint8_t>& bytes, ShxPen& pen, Glyph& glyph, int depth) const;
};

// Folders that hold AutoCAD's shape fonts on this machine (DWG TrueView's and AutoCAD's Fonts folders).
const std::vector<std::filesystem::path>& shx_folders();

}  // namespace opad::detail
