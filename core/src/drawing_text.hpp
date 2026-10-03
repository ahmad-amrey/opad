#pragma once
// Text of the 2D drawing readers (DXF TEXT, ATTRIB and MTEXT, SVG text) outlined as faces (UI-92). With HarfBuzz
// (OPAD_HAVE_SHAPING) it is shaped as a text renderer shapes it: Arabic letters join and carry their marks, lam-alef and
// other ligatures, Indic clusters, kerning; right-to-left runs and the numbers in them in visual order (the Unicode bidi
// algorithm without explicit embeddings, mirrored brackets); each character from the style's font, else from the first
// fallback font that has it (as Qt merges fonts). Without it, OCCT's text (no shaping). A style's AutoCAD shape font
// (.shx) that is found (beside the drawing, in DWG TrueView's or AutoCAD's Fonts folder) draws text in its strokes, edges
// rather than faces, as long as it has every character (shx_font.hpp). Not a public header.
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace opad::detail {

struct TextRequest {
  std::string text;    // UTF-8; '\n' starts a line
  std::string font;    // a font file (.ttf, .ttc, .otf: a path, or a name looked up in the outliner's folders), a shape
                       // font (.shx, or a name without extension), else families separated by commas; one not found
  std::string family;  // (or a shape font lacking a character): the families of `family`, else the default sans-serif
  bool bold = false, italic = false;  // the family's bold and italic faces (a font file is taken as it is)
  double size = 2.5;   // mm: the em, or with `cap` the height of the capitals (a DXF text height)
  bool cap = false;
  double width = 1;    // width factor
  double spacing = 0;  // mm from baseline to baseline; 0: the font's line spacing
  double wrap = 0;     // mm: lines longer than this break at spaces (or between ideographs)
  double fit = 0;      // mm: one line stretched to this length; wider only or, `aligned`, larger
  bool aligned = false;
  enum H { Left, Center, Right } h = Left;  // each line within the block
  // What lies on the origin: the first line's baseline, the block's top (the first line's capitals), its middle, its
  // bottom (the last baseline) or the last line's descent.
  enum V { Baseline, Top, Middle, Bottom, Descent } v = Baseline;
};

// One shaped line: glyphs left to right, positions in ems from the line's left end on its baseline.
struct ShapedGlyph {
  std::string font;  // the font file it comes from
  const void* face = nullptr;
  unsigned glyph = 0;
  uint32_t cluster = 0;  // the first character it shows (index into the line's code points)
  double x = 0, y = 0, advance = 0;
};
struct ShapedLine {
  std::u32string text;  // the line's characters in logical order
  std::vector<ShapedGlyph> glyphs;
  double width = 0;  // ems
  int direction = 0;  // the paragraph's: 0 left to right, 1 right to left
};

class TextOutliner {
 public:
  explicit TextOutliner(std::vector<std::filesystem::path> folders = {});
  ~TextOutliner();
  TextOutliner(const TextOutliner&) = delete;
  TextOutliner& operator=(const TextOutliner&) = delete;
  // Faces in the plane of `at` (x along its X direction, y along its Y), the block's anchor on its origin. Null when no
  // font loads; an empty compound when nothing is drawn.
  TopoDS_Shape outline(const TextRequest& request, const gp_Ax3& at);
  // The lines as shaped (request.size, width, fit and alignment aside); empty without shaping or a font.
  std::vector<ShapedLine> shape(const TextRequest& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

bool text_shaping();  // built with HarfBuzz
// The Unicode bidi algorithm's embedding levels of one paragraph (no explicit embeddings): `base` is its direction (the
// first strong character's), each character's level; and the paragraph's characters in visual order, left to right.
std::vector<uint8_t> bidi_levels(const std::u32string& text, int& base);
std::u32string bidi_visual(const std::u32string& text);
std::u32string utf32(const std::string& utf8);

}  // namespace opad::detail
