#pragma once
// Drawing sheets (TODO 11 UI-76). Three op types hold a technical drawing's definitions, none with a `target`, so the
// merge driver never takes two people's sheets, views or dimensions for one change:
//   sheet       name, drawing, size {preset, w, h} (paper mm), standard iso|asme, projection first|third, scale "1:2",
//               units, values (title block fields), template (embedded, its geometry a body key)
//   sheet_view  sheet, name, kind base|projected; base: source {nodes|"all", hide}, orient {preset} | {dir, up} |
//               {view: bookmark op}, at [x, y] (paper mm of the view's centre), scale "sheet"|"1:5", style {hidden,
//               tangent show|thin|hide, silhouettes, quality}; projected: parent, side (left right top bottom or a
//               corner), gap (paper mm between the frames), align false + at to break the alignment
//   sheet_item  sheet, view, kind dimension|note; dimension: type horizontal|vertical|aligned|radius|diameter|angle,
//               refs (references with hint and aspect start|end|mid|center), place {text: [x, y]} (paper mm from the
//               view's centre), precision, tol {sym|dev, plus, minus}, text {prefix, suffix}, result {value, shown}
//               (the value when it was made); note: text, at, height
// Changes are `edit` ops, removal `delete` (a deleted sheet takes its views and items with it, a deleted view its
// projected views and items). Replay records definitions only; a view is projected from the final scene when a sheet is
// shown or exported (projection.hpp caches it by fingerprint), never stored in the log, so opening a document with
// sheets costs nothing and a model edit adds no drawing lines. The loader checks structure only: a kind, side or
// orientation this build does not know is kept and reported as needing a newer OPAD, never refused.
#include <array>
#include <string>
#include <vector>

#include "opad/document.hpp"
#include "opad/drawing/projection.hpp"
#include "opad/scene.hpp"

namespace opad::drawing {

struct PaperSize {
  const char* name;
  double w, h;  // portrait: w <= h
};
const std::vector<PaperSize>& paper_sizes();  // ISO A0-A4, ANSI A-E
json paper_size(const std::string& preset, bool landscape = true);  // {"preset","w","h"}; throws Error

double parse_scale(const std::string& text);  // "1:2" -> 0.5, "2:1" -> 2; throws Error
std::string scale_text(double scale);
const std::vector<double>& standard_scales();  // ISO 5455, largest first
// The largest standard scale at which a model extent (mm) fits into a room on paper (mm).
double fit_scale(double model_w, double model_h, double room_w, double room_h);

bool is_sheet_record(const std::string& op_type);  // sheet | sheet_view | sheet_item
// A sheet record or part properties: nothing the design history reads, so deleting one walks no features, and the
// design timeline does not show them (the browser's Drawings folder and the Properties panel do).
bool is_drawing_op(const std::string& op_type);
void validate_record(const json& op);               // Document::validate_op's check of those; throws Error
void record_body_keys(const json& op, std::vector<std::string>& keys);  // template geometry, frozen linework

// What a view draws and from where: the orientation follows its parents (first or third angle), the source and the style
// are its own or its base view's. Throws Error when it cannot be drawn.
ViewSpec view_spec(const Scene& scene, const SheetView& view);
// The standard view it shows, as Camera::preset names it (front, top, left, iso, ...): empty when it looks askew or cannot
// be drawn. In first angle the view right of a front view is "left".
std::string view_orientation(const Scene& scene, const SheetView& view);

// The browser's Drawings folder: drawings (the sheets sharing `drawing`, in the order of their first sheet; a sheet with
// none stands alone) > sheets > their views in log order, each with its items > the items on the sheet itself. Names are
// the records' own; a dimension's is the value it was made with, a note's its first line; a view without one gets its
// orientation (above) for the app to word. error: why it is not drawn (a kind of a newer OPAD, a missing parent, ...).
struct OutlineRow {
  std::string id;    // the op; a drawing: "drawing:" + its name
  std::string kind;  // drawing | sheet | view | item
  std::string name, orient, error;
  std::vector<OutlineRow> children;
};
std::vector<OutlineRow> outline(const Scene& scene);

// Where a view lands on its sheet: model point p -> at + scale * (view(p) - centre). Boxes come from the bodies' tight
// boxes (walks the geometry the first time: workers only).
struct ViewFrame {
  std::string id;
  double scale = 1;
  Vec2 at{0, 0}, centre{0, 0};
  Vec3 x{1, 0, 0}, y{0, 0, 1}, dir{0, -1, 0};
  std::array<double, 4> box{0, 0, 0, 0};  // paper: xmin, ymin, xmax, ymax
  std::string error;
  Vec2 view(const Vec3& world) const;   // view coordinates (model mm)
  Vec2 paper(const Vec3& world) const;
  json to_json() const;
};
std::vector<ViewFrame> layout(const Document& doc, const Scene& scene, const Sheet& sheet);
// The bodies' extent in a view: xmin, ymin, xmax, ymax in view coordinates (model mm); zeros when it draws nothing.
std::array<double, 4> view_extent(const Document& doc, const Scene& scene, const ViewSpec& spec);

// A dimension's value from its references now (hint-aware, as features resolve theirs): {"value", "shown", "anchor"}
// in the sheet's units, angles in degrees; anchor is where it measures, paper mm from the view's centre. Throws Error
// when a reference is gone or the view shows it foreshortened.
json evaluate_item(const Document& doc, const Scene& scene, const Sheet& sheet, const SheetItem& item, const ViewFrame& frame);
std::string format_value(double value, const json& item);  // precision, prefix, tolerance and suffix of an item

}  // namespace opad::drawing
