#pragma once
// 2D drawings in the view (area drawing2d): what is shown of a drawing's layers, independent of Qt and OCCT's view so
// tests/test_drawing2d covers it. The viewport, the Layers panel and the 2D vocabulary build on these.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "opad/document.hpp"
#include "opad/scene.hpp"

class TopoDS_Shape;

namespace drawing2d {
using Rgb = std::array<double, 3>;  // 0..1, sRGB

// WCAG 2 relative luminance and contrast ratio (1 .. 21).
double luminance(const Rgb& c);
double contrast(const Rgb& a, const Rgb& b);
// The drawing's own foreground (DXF colour 7 and every entity without a colour): #e6e6e6 on a dark background, #141414
// on a light one, whichever stands out more.
Rgb ink(const Rgb& background);
constexpr Rgb kInkOnDark{0xe6 / 255.0, 0xe6 / 255.0, 0xe6 / 255.0}, kInkOnLight{0x14 / 255.0, 0x14 / 255.0, 0x14 / 255.0};

// ---- layers (UI-89). A drawing's layer is a component holding its drawing bodies (one per colour); a DXF layer also
// carries its table entry (Node::layer: off, frozen, locked, plot, linetype, lineweight). Its state lives in the
// document: visible and locked as any node's, the rest as `layer` fields of appearance ops, which an earlier build reads
// for visible (always written along) and ignores otherwise.
struct Layer {
  std::string id, name, drawing;  // its component, its name, the drawing (its import's root) it belongs to
  std::vector<std::string> bodies;
  // The bodies a colour given to the layer applies to: those drawn in the layer's colour (DXF BYLAYER, Node::by_layer);
  // all of them when the file does not say (SVG, an earlier import, a layer whose entities all have colours of their own).
  std::vector<std::string> byLayer;
  bool on = true, frozen = false, locked = false, plot = true;
  bool colored = false, mixed = false;  // the layer has a colour (else the ink); the bodies it applies to differ
  int own = 0;  // bodies in colours of their own, which keep them
  Rgb color{0, 0, 0};
  std::string linetype;  // "" = continuous
  std::vector<double> pattern;  // its dashes as the file's LTYPE table has them (mm; < 0 gap, 0 dot); empty: by its name
  double lineweight = -1;  // mm; < 0 = the default
};
bool isLayer(const opad::Scene& scene, const std::string& id);
// A layer's (or a saved state's) true/false field, `fallback` when it is missing or not a boolean (a file edited by hand).
bool flag(const opad::json& fields, const char* key, bool fallback);
std::vector<Layer> layers(const opad::Scene& scene);  // in tree order
const Layer* find(const std::vector<Layer>& all, const std::string& id);
std::string layerOf(const opad::Scene& scene, const std::string& node);  // the layer a node (a drawing body) lies on, "" none
std::optional<Layer> layerAt(const opad::Scene& scene, const std::string& node);  // that layer alone, without a walk over the scene
// A layer frozen by its file or here is hidden like one turned off, and stays so while it is turned on: On and Freeze are
// told apart by the `off` and `frozen` fields; visible is what both leave.

// The appearance command's arguments for one change of a layer. Each keeps visible as the change leaves it.
opad::json setOn(const Layer& layer, bool on);
opad::json setFrozen(const Layer& layer, bool frozen);
opad::json setLocked(const Layer& layer, bool locked);
opad::json setColor(const Layer& layer, const Rgb& color);  // the layer and its byLayer bodies
opad::json setDefaultColor(const Layer& layer);  // back to the colour they were imported in (none: the ink)
// "" or Continuous: back to continuous. `pattern`: the file's dashes for that name (another layer's), else none.
opad::json setLinetype(const Layer& layer, const std::string& linetype, const std::vector<double>& pattern = {});
opad::json setLineweight(const Layer& layer, double mm);  // < 0: the default
opad::json setPlot(const Layer& layer, bool plot);

// The linetypes offered (with those the drawing's layers name) and the DXF lineweights (mm).
const std::vector<std::string>& linetypes();
const std::vector<double>& lineweights();
// A linetype's dashes (mm; < 0 gap, 0 dot): the file's own pattern when it has one, else acad.lin's and acadiso.lin's
// (DASHED, HIDDEN, CENTER, PHANTOM, DOT, DASHDOT, BORDER, DIVIDE with their 2 and X2 sizes, ACAD_ISO02W100..15), else
// one guessed from the name (dot, dash-dot or dashed); empty for a continuous line.
std::vector<double> dashes(const std::string& linetype, const std::vector<double>& pattern = {});
// How the view draws dashes: a 16-bit line stipple and its factor (pixels per bit), the dashes `pixelsPerMm` pixels per
// millimetre in screen space (drawn at the same size whatever the zoom, as long as the pattern fits 16 x 256 pixels).
// Each dash and gap gets at least one bit, dots exactly one. {0xFFFF, 1}: solid.
struct LinePattern {
  uint16_t bits = 0xFFFF, factor = 1;
  bool operator==(const LinePattern&) const = default;
};
LinePattern linePattern(const std::vector<double>& dashes, double pixelsPerMm);
constexpr double kPatternPixelsPerMm = 1.25;  // logical pixels per pattern millimetre: DASHED repeats every 24 px
// A lineweight as wide as on paper at 96 dpi, in screen points, at least one: 0.25 mm and less are hairlines.
double linePoints(double lineweight);

// How a drawing body's lines are drawn (UI-92): its own linetype and lineweight (DXF entities that set them, Node::line)
// over its layer's (the parent's Node::layer, as the Layers panel leaves it), its dashes in its own scale.
struct LineStyle {
  std::string linetype;  // "" continuous
  std::vector<double> pattern;  // the file's dashes for it; empty: by its name
  double lineweight = -1;  // mm; < 0 the default
  double scale = 1;  // its dashes times this (DXF CELTSCALE), whichever linetype it takes
  bool ownType = false, ownWeight = false;  // the body's own, not its layer's
};
LineStyle lineStyle(const opad::Scene& scene, const opad::Node& body);

// Layer states (LAYERSTATE): every layer's on, frozen, locked, plot, colour, linetype and lineweight, saved as
// `display.layers` of a view op; restoring writes the appearance changes that bring the layers back to it (by layer id,
// else by name), nothing for a layer already so.
opad::json captureState(const opad::Scene& scene);
std::vector<opad::json> restoreState(const opad::Scene& scene, const opad::json& display);

// ---- the 2D vocabulary (UI-118). In a drawing-only scene and in 2D mode, a drawing's bodies, edges and vertices are
// objects and points on layers, never "body 3 > edge 12"; faces are its fills (hatches, solids, text).
// What a picked sub-shape of a drawing is: {"type": line|arc|circle|ellipse|spline|curve|fill|point, "length" (mm, curves),
// "radius" (arcs and circles), "area" (fills)}; one curve or face, no walk over its body.
opad::json entityInfo(const TopoDS_Shape& sub);
// The same type from what opad::inspect_ref says of a picked sub-shape (Properties), "" when it is none of them.
std::string entityType(const opad::json& inspected);
// What a pick is called (source text for i18n::t): a body is a "group" (a layer's objects of one colour, picked
// together), an edge an "object", a vertex a "point", a face a "fill", a centre a "center".
const char* kindWord(opad::Ref::Kind kind);
// A browser row: "layer", "drawing" (an import's root holding layers), "group" (a drawing body), else "object".
const char* nodeWord(const opad::Scene& scene, const std::string& id);
// Properties of a drawing's body or entity in 2D words: its faces, edges and vertices counted as fills, objects and
// points; what only a solid has (volume, solid, a flat drawing's axis, normal and plane, neighbours, vertex lists) goes.
opad::json properties(opad::json props);
bool drawingOnly(const opad::Scene& scene);  // bodies, all of them drawings
bool hasDrawings(const opad::Scene& scene);  // some body is a drawing

// ---- drawing coordinates (UI-90): where a world point lies in a drawing as its file has it. A drawing's root (its import's
// component) is placed by its world matrix; a drawing read far from (0,0) keeps the offset as the root's drawing_origin.
struct DrawingFrame {
  std::string root;
  opad::Mat4 world;              // the root's placement now
  opad::Vec3 origin{0, 0, 0};    // drawing_origin: the drawing coordinates of the root's local (0, 0, 0)
  double x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // its extents in drawing coordinates (x0 > x1: none)
};
// Every drawing of the scene (roots holding drawing layers), in tree order; extents from the cached boxes.
std::vector<DrawingFrame> drawingFrames(const opad::Document& doc, const opad::Scene& scene);
opad::Vec3 toDrawing(const DrawingFrame& frame, const opad::Vec3& world);
opad::Vec3 fromDrawing(const DrawingFrame& frame, const opad::Vec3& drawing);
// The frame's plane in the world: origin at the root's local (0, 0, 0), x and y its axes.
opad::Frame planeOf(const DrawingFrame& frame);
}  // namespace drawing2d
