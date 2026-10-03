#pragma once
// A 2D drawing ready to be written (TODO 11 UI-86): layers with a pen each (colour, line type, line weight) and
// primitives in drawing millimetres, y up. One list feeds every writer (DXF R2000, SVG, and through Qt PDF and PNG), so a
// sheet, a quick view of the model and an exported sketch come out the same in each. Curves are the
// projection's (lines, arcs, ellipses, splines, polylines: exact, never chopped into segments), fills are closed outlines
// (holes inside outer ones, even-odd), text is UTF-8. Dimensions are written as their geometry (lines, filled arrowheads,
// text), which every reader shows.
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "opad/drawing/projection.hpp"

class TopoDS_Shape;

namespace opad::drawing {

constexpr uint32_t kInk = 0xFF000000u;      // the drawing's foreground: black on paper (DXF colour 7)
constexpr uint32_t kByLayer = 0xFE000000u;  // a primitive in its layer's colour

enum class LineType : uint8_t { Continuous, Hidden, Center, Phantom, Dotted };
const char* line_type_name(LineType t);           // as DXF names them: Continuous, HIDDEN, CENTER, PHANTOM, DOT
const std::vector<double>& line_type_dashes(LineType t);  // paper mm: dash > 0, gap < 0, a dot 0; empty for solid

struct Layer {
  std::string name;
  uint32_t rgb = kInk;  // 0xRRGGBB or kInk
  LineType line = LineType::Continuous;
  double width = 0.25;  // paper mm (ISO 128: 0.13 0.18 0.25 0.35 0.5 0.7 1)
};

struct Prim {
  enum class Kind : uint8_t { Curve, Fill, Text, Image };
  Kind kind = Kind::Curve;
  int layer = 0;
  uint32_t rgb = kByLayer;
  Curve curve;                            // Curve: a line, an arc (a full turn: a circle), an ellipse, a spline or a polyline
  std::vector<std::vector<Vec2>> loops;   // Fill: closed outlines, the last point not repeated
  std::string text;                       // Text: UTF-8, lines split by '\n'; Image: the href
  Vec2 at{0, 0};                          // Text: the anchor
  double height = 3.5, angle = 0;         // Text: cap height (drawing units) and direction (radians, counter-clockwise)
  int halign = 0, valign = 0;             // Text: 0 left, 1 centre, 2 right; 0 baseline, 1 bottom, 2 middle, 3 top
  std::array<Vec2, 3> corners{};          // Image: its top-left, top-right and bottom-left corners
  std::string fit;                        // Image: SVG preserveAspectRatio
};

struct Display {
  std::string title;
  // Drawing units per paper mm for the pens: line widths and dash lengths are paper sizes times this (a model drawn 1:1
  // that plots at 1:5 has 5), so dashes stay dashes at the scale it is meant for. DXF writes it as $LTSCALE.
  double pen_scale = 1;
  // The sheet it is drawn on (xmin, ymin, xmax, ymax in drawing units): SVG's view box, PDF's page, PNG's picture. Empty:
  // fitted around the drawing.
  std::array<double, 4> paper{0, 0, 0, 0};
  bool has_paper() const { return paper[2] > paper[0] && paper[3] > paper[1]; }
  std::vector<Layer> layers;
  std::vector<Prim> prims;
  int layer(const Layer& l);  // the index of the layer of that name, added as given when new
  void curve(int layer, const Curve& c, uint32_t rgb = kByLayer);
  void line(int layer, Vec2 a, Vec2 b, uint32_t rgb = kByLayer);
  void polyline(int layer, const std::vector<Vec2>& pts, bool closed = false, uint32_t rgb = kByLayer);
  void arc(int layer, Vec2 c, double r, double a0, double a1, uint32_t rgb = kByLayer);  // counter-clockwise, radians
  void circle(int layer, Vec2 c, double r, uint32_t rgb = kByLayer);
  void fill(int layer, std::vector<std::vector<Vec2>> loops, uint32_t rgb = kByLayer);
  void text(int layer, const std::string& s, Vec2 at, double height, double angle = 0, int halign = 0, int valign = 0, uint32_t rgb = kByLayer);
  std::array<double, 4> bounds() const;  // xmin, ymin, xmax, ymax of everything (text by its rough box); zeros when empty
  json counts() const;                   // primitives by DXF entity and by layer
};

// A text's lines as every writer lays them out, each with the point on its baseline that its horizontal alignment refers
// to: lines 1.6 heights apart, the block placed by the vertical alignment (top: the first line's capitals touch the
// anchor; middle: the block's middle; bottom: the last line's descenders, 0.3 heights below its baseline; baseline: the
// first line's).
struct TextLine {
  std::string text;
  Vec2 at;
};
std::vector<TextLine> text_lines(const Prim& p);

// Edges and faces of a shape as the drawing shows them seen from +Z (z dropped): analytic curves where they lie in a
// plane parallel to XY (lines, circles, ellipses, B-splines, Béziers as splines), polylines within `tol` elsewhere;
// each face a fill of its outline and holes, its own edges not drawn again. Free vertices are left out.
void add_shape(Display& d, int layer, const TopoDS_Shape& shape, double tol = 0.01, uint32_t rgb = kByLayer);

// A hidden-line view as a drawing: visible edges and silhouettes 0.5 mm on "Visible", edges between tangent faces
// 0.25 mm on "Tangent", hidden lines 0.25 mm dashed on "Hidden" (when the view has them); pens for full size, or for the
// largest standard scale at which the view fits an A3 sheet when it does not fit at full size.
Display view_display(const ViewGeometry& g, const std::string& title = {});

// Dimensions drawn as geometry (ISO 129 look: extension lines 1 mm off the feature and 2 mm past the dimension line,
// filled arrowheads, text above the dimension line and along it, read from below or from the right). Sizes are paper
// mm times `scale` (drawing units per paper mm).
struct DimStyle {
  double text = 3.5, arrow = 3, gap = 1, overshoot = 2, scale = 1;
};
// Measures a -> b along `axis` ({1, 0} horizontal, {0, 1} vertical, b - a aligned); the dimension line runs through `place`.
void linear_dimension(Display& d, int layer, Vec2 a, Vec2 b, Vec2 axis, Vec2 place, const std::string& text, const DimStyle& s = {});
// A radius (from the centre) or a diameter (across) of a circle, its leader towards `place`, where the text goes.
void radial_dimension(Display& d, int layer, Vec2 centre, double r, Vec2 place, const std::string& text, bool diameter, const DimStyle& s = {});
// The angle between two lines (each by two points), its arc through `place` in the corner between them that holds it;
// extension lines out to the arc where it lies beyond a line's ends. Parallel lines draw only the text.
void angular_dimension(Display& d, int layer, std::array<Vec2, 2> a, std::array<Vec2, 2> b, Vec2 place, const std::string& text, const DimStyle& s = {});

// The writers. Coordinates in mm with at most `decimals` decimals (trailing zeros dropped), independent of the locale.
// DXF: R2000 (AC1015) with handles; HEADER ($INSUNITS mm, $MEASUREMENT metric, $LTSCALE = pen_scale, extents), TABLES
// (the line types, a layer per Layer with its colour (nearest of the 255 indexed colours), line type and line weight, the
// Standard text style), the model and paper space blocks, ENTITIES (LINE, ARC, CIRCLE, ELLIPSE, SPLINE, LWPOLYLINE,
// TEXT/MTEXT, SOLID for small convex fills, solid HATCH) and the OBJECTS a reader expects (layouts, plot style names).
// Text beyond ASCII is written as \U+XXXX. Images throw (DXF raster references need external files). mtext=false writes
// a text of several lines as one TEXT a line (LibreDWG's DWG writer loses MTEXT heights).
std::string dxf_text(const Display& d, int decimals = 6, bool mtext = true);
// SVG 1.1 written by hand (Qt Svg is not part of the build): width and height in mm with a mm viewBox, y flipped once; a
// group per layer (an Inkscape layer, named) with its stroke colour, width and dash pattern; curves as one path per
// layer (arcs and ellipses as arc commands, splines as cubic Béziers), full circles and ellipses as their elements, fills
// as even-odd paths, text as <text>, images as <image>.
std::string svg_text(const Display& d, int decimals = 6);
// PDF and PNG are painted with Qt (paint/: opad_paint, the same list through one QPainter backend), which the app and
// opad-cli install; the core alone cannot write them. options: {"dpi"} for PNG; it returns what it wrote (page, pixels).
using PaintWriter = std::function<json(const Display&, const std::filesystem::path&, const std::string& format, const json& options)>;
void set_paint_writer(PaintWriter writer);
bool can_paint();
// dxf | svg | pdf | png (the last two when a painter is installed); returns the painter's report, else an empty object.
json write_drawing(const Display& d, const std::filesystem::path& file, const std::string& format, int decimals = 6, const json& options = {});

}  // namespace opad::drawing
