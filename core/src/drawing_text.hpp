#pragma once
// Text of the 2D drawing readers (DXF TEXT, ATTRIB and MTEXT, SVG text) outlined as faces (UI-92), in parts of their
// own formats where a text has them (MTEXT's inline fonts, sizes, colours, slants, rules and stacks). With HarfBuzz
// (OPAD_HAVE_SHAPING) it is shaped as a text renderer shapes it: Arabic letters join and carry their marks, lam-alef and
// other ligatures, Indic clusters, kerning; right-to-left runs and the numbers in them in visual order (the Unicode bidi
// algorithm with its explicit embeddings, overrides and isolates; mirrored brackets); each character from the style's
// font, else from the first fallback font that has it (as Qt merges fonts). Without it, OCCT's text (no shaping). A style's AutoCAD shape font
// (.shx) that is found (beside the drawing, in DWG TrueView's or AutoCAD's Fonts folder) draws text in its strokes, edges
// rather than faces, as long as it has every character (shx_font.hpp). Not a public header.
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax3.hxx>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace opad::detail {

// How a text, or a part of it, is drawn.
struct TextFormat {
  std::string font;    // a font file (.ttf, .ttc, .otf: a path, or a name looked up in the outliner's folders), a shape
                       // font (.shx, or a name without extension), else families separated by commas; one not found
  std::string family;  // (or a shape font lacking a character): the families of `family`, else the default sans-serif
  bool bold = false, italic = false;  // the family's bold and italic faces (a font file is taken as it is)
  double size = 2.5;   // mm: the em, or with the request's `cap` the height of the capitals (a DXF text height)
  double width = 1;    // width factor
  double oblique = 0;  // radians: the letters lean right by it (DXF obliquing, MTEXT \Q)
  double tracking = 1;  // times the advance from one character to the next (MTEXT \T; not in right-to-left runs)
  static constexpr uint32_t kInherit = 0xFFFFFFFFu;
  uint32_t color = kInherit;  // the reader's colour for this part (outline's `colored`), kInherit: the text's own
  bool underline = false, overline = false, strike = false;  // a line under, over or through it
  enum Align { Base, Center, Top } align = Base;  // a smaller part on the line's baseline, its middle or its top (\A)
};

// A part of a text in a format of its own (MTEXT's inline formatting, TEXT's %%u): `stack` makes it a stacked pair,
// `text` above `bottom` at 70 % of its size: '/' a fraction over a bar, '#' a diagonal one, '^' a tolerance (no bar,
// left aligned).
struct TextSpan : TextFormat {
  std::string text;  // UTF-8; '\n' starts a paragraph
  char stack = 0;
  std::string bottom;
};

// A paragraph's own layout (MTEXT \p), mm: its alignment, how far its lines start from the block's left end (the first
// line `first` further, a hanging indent when negative) and end before its right end, its tab stops from the block's
// left end (after the last, one every 4 text heights).
struct TextParagraph {
  int justify = -1;  // its TextRequest::H, -1 the request's
  double left = 0, first = 0, right = 0;
  std::vector<double> tabs;
};

struct TextRequest : TextFormat {
  std::string text;    // UTF-8; '\n' starts a line
  bool cap = false;
  double spacing = 0;  // mm from baseline to baseline (a line with larger text further down); 0: the font's line spacing
  double wrap = 0;     // mm: lines longer than this break at spaces (or between ideographs)
  double fit = 0;      // mm: one line stretched to this length; wider only or, `aligned`, larger
  bool aligned = false;
  // The block (as wide as `wrap`, else its widest line) on the origin by its left end, middle or right end, each line
  // within it so too (or as its paragraph says). A tab goes on to the next stop (every 4 sizes, or its paragraph's).
  enum H { Left, Center, Right } h = Left;
  // What lies on the origin: the first line's baseline, the block's top (the first line's capitals), its middle, its
  // bottom (the last baseline) or the last line's descent.
  enum V { Baseline, Top, Middle, Bottom, Descent } v = Baseline;
  // Text in parts of their own formats instead of `text` in the request's (whose size stays the line spacing's measure).
  std::vector<TextSpan> spans;
  std::vector<TextParagraph> paragraphs;  // per paragraph (none: as the request lays it out)
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
  // Faces in the plane of `at` (x along its X direction, y along its Y), the block's anchor on its origin; shape fonts'
  // strokes and the lines of underlines, bars and the like as edges. Null when no font loads; an empty compound when
  // nothing is drawn. Parts in colours of their own go into `colored` by colour (without it, into the result).
  TopoDS_Shape outline(const TextRequest& request, const gp_Ax3& at, std::map<uint32_t, TopoDS_Compound>* colored = nullptr);
  // The lines as shaped (request.size, width, fit and alignment aside); empty without shaping or a font.
  std::vector<ShapedLine> shape(const TextRequest& request);

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

bool text_shaping();  // built with HarfBuzz
// The Unicode bidi algorithm's embedding levels of one paragraph (explicit embeddings, overrides and isolates too):
// `base` is its direction (the first strong character's, isolates left out), each character's level; and the
// paragraph's characters in visual order, left to right.
std::vector<uint8_t> bidi_levels(const std::u32string& text, int& base);
std::u32string bidi_visual(const std::u32string& text);
std::u32string utf32(const std::string& utf8);

}  // namespace opad::detail
