#pragma once
// Drafting symbols drawn as vectors (TODO 11 UI-79, UI-80, UI-81), so no font has to have them and every writer (DXF, SVG,
// PDF, PNG) shows the same: the dimensioning glyphs (⌀ diameter, ⌴ counterbore, ⌵ countersink, ↧ depth, □ square), the
// 14 geometric characteristics (⏤ ⏥ ○ ⌭ ⌒ ⌓ ∠ ⟂ ∥ ⌖ ◎ ⌯ ↗ ⌰) and the circled modifiers (Ⓜ Ⓛ Ⓢ Ⓟ Ⓕ Ⓣ Ⓤ Ⓔ), and
// the marks built of them: centre marks and centre lines (long dash, short dash, laid out symmetrically so they start and
// end with a long dash and cross at the centre), leaders, datum feature symbols, feature control frames, surface texture
// symbols and ordinate (running) dimensions. Sizes are paper mm times DimStyle::scale. Drawn from general drafting
// practice, not from any standard's figures.
#include <array>
#include <string>
#include <vector>

#include "opad/drawing/display.hpp"

namespace opad::drawing {

// The width of a line of text in the drawing font (Arial's advance widths: the painter's font, capitals `height` tall);
// glyphs count their own advance.
double text_width(const std::string& line, double height);
bool is_glyph(char32_t c);
double glyph_advance(char32_t c, double height);
// One glyph, its baseline's left end at `at`, turned by `angle` (radians); returns its advance.
double draw_glyph(Display& d, int layer, char32_t c, Vec2 at, double height, double angle = 0, uint32_t rgb = kByLayer);
// Text as Display::text lays it out (lines, alignment), the glyphs among it drawn as vectors and the text between them as
// text runs left-aligned on the baseline. Text without glyphs is one text primitive, as Display::text makes it.
void rich_text(Display& d, int layer, const std::string& s, Vec2 at, double height, double angle = 0, int halign = 0, int valign = 0,
               uint32_t rgb = kByLayer);
// The widest line of a text (glyphs included).
double rich_width(const std::string& s, double height);

// The geometric characteristics by name (straightness flatness circularity cylindricity line_profile surface_profile
// angularity perpendicularity parallelism position concentricity symmetry circular_runout total_runout) as their glyph
// (UTF-8); empty for a name it does not know. Material and other modifiers (M L S P F T U E) as their circled glyph.
std::string characteristic_glyph(const std::string& name);
const std::vector<std::string>& characteristics();
std::string modifier_glyph(const std::string& letter);

// A line drawn as a centre line: dashes of `dash` and dots between, symmetric about its middle (long dashes at both ends);
// short lines solid.
void centre_line(Display& d, int layer, Vec2 a, Vec2 b, const DimStyle& s = {});
// A circle's centre mark: a small cross at the centre and arms to `extend` past the circle along x and y of `axis` (a
// unit direction; {1, 0} upright); a small circle gets plain lines.
void centre_mark(Display& d, int layer, Vec2 centre, double r, double extend, Vec2 axis = {1, 0}, const DimStyle& s = {});
// A leader from `text_at` to a filled arrowhead at `tip` (dot: a dot instead, on a surface), with a horizontal shoulder at
// the text: the text goes left-aligned after it when the tip is left of it, else right-aligned. Returns the text's anchor
// (its first line's baseline end at the shoulder).
Vec2 leader(Display& d, int layer, Vec2 tip, Vec2 text_at, const std::string& text, const DimStyle& s = {}, bool dot = false);
// A datum feature symbol: a filled triangle standing on the feature at `foot` (its base along the feature, `out` the unit
// direction away from the material towards the frame), a line to a square frame centred at `frame` with the letter in it.
void datum_symbol(Display& d, int layer, Vec2 foot, Vec2 out, Vec2 frame, const std::string& letter, const DimStyle& s = {});
// A feature control frame: cells side by side from `at` (the left end of its middle line): the characteristic glyph, the
// tolerance (with ⌀ for a cylindrical zone and a modifier), then the datums with theirs. Returns its width.
struct FrameCells {
  std::string characteristic;  // glyph (UTF-8)
  std::string tolerance;       // "⌀0.1Ⓜ"
  std::vector<std::string> datums;  // "A", "BⓂ"
};
double feature_frame(Display& d, int layer, Vec2 at, const FrameCells& cells, const DimStyle& s = {});
double feature_frame_width(const FrameCells& cells, const DimStyle& s = {});
// A surface texture symbol standing on a surface at `foot` (`out` away from the material): the basic check mark, with the
// bar of material removal required (process "removal") or the circle of removal not allowed ("no_removal"), and its text
// (the parameter and value, e.g. "Ra 1.6") over the long leg's extension.
void surface_symbol(Display& d, int layer, Vec2 foot, Vec2 out, const std::string& process, const std::string& text, const DimStyle& s = {});
// Ordinate dimensions: each point's extension line runs to the `level` (a coordinate along `normal`, the unit direction
// square to the measured axis) where its value stands along the line; values too close together are spread apart and
// their lines jogged. texts[i] is points[i]'s value; the origin's is "0".
void ordinate_dimensions(Display& d, int layer, const std::vector<Vec2>& points, const std::vector<std::string>& texts, Vec2 axis, double level,
                         const DimStyle& s = {});

}  // namespace opad::drawing
