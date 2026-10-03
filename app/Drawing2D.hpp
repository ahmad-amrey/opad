#pragma once
// 2D drawings in the view (area drawing2d): what is shown of a drawing's layers, independent of Qt and OCCT's view so
// tests/test_drawing2d covers it. The viewport, the Layers panel and the 2D vocabulary build on these.
#include <array>
#include <string>
#include <utility>
#include <vector>

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
  bool on = true, frozen = false, locked = false, plot = true;
  bool colored = false, mixed = false;  // its bodies have a colour of their own (else the ink), several different ones
  Rgb color{0, 0, 0};
  std::string linetype;  // "" = continuous
  double lineweight = -1;  // mm; < 0 = the default
};
bool isLayer(const opad::Scene& scene, const std::string& id);
std::vector<Layer> layers(const opad::Scene& scene);  // in tree order
const Layer* find(const std::vector<Layer>& all, const std::string& id);
std::string layerOf(const opad::Scene& scene, const std::string& node);  // the layer a node (a drawing body) lies on, "" none
// A layer frozen by its file or here is hidden like one turned off, and stays so while it is turned on: On and Freeze are
// told apart by the `off` and `frozen` fields; visible is what both leave.

// The appearance command's arguments for one change of a layer. Each keeps visible as the change leaves it.
opad::json setOn(const Layer& layer, bool on);
opad::json setFrozen(const Layer& layer, bool frozen);
opad::json setLocked(const Layer& layer, bool locked);
opad::json setColor(const Layer& layer, const Rgb& color);  // the layer and its bodies
opad::json setDefaultColor(const Layer& layer);  // back to the colours they were imported in (none: the ink)
opad::json setLinetype(const Layer& layer, const std::string& linetype);  // "" or Continuous: back to continuous
opad::json setLineweight(const Layer& layer, double mm);  // < 0: the default
opad::json setPlot(const Layer& layer, bool plot);

// The linetypes offered (with those the drawing's layers name) and the DXF lineweights (mm).
const std::vector<std::string>& linetypes();
const std::vector<double>& lineweights();
// How the view draws them: Aspect_TypeOfLine (0 solid, 1 dash, 2 dot, 3 dot-dash) and a width in screen points (a
// lineweight as wide as on paper at 96 dpi, at least one point: 0.25 mm and less are hairlines).
int lineType(const std::string& linetype);
double linePoints(double lineweight);

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
const char* kindWord(opad::Ref::Kind kind);  // "object", "point", "fill", "center" (source text for i18n::t)
bool drawingOnly(const opad::Scene& scene);  // bodies, all of them drawings
}  // namespace drawing2d
