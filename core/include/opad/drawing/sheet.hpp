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
#include "opad/drawing/display.hpp"
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

// A dimension's value from its references now (hint-aware, as features resolve theirs): {"value", "shown", "anchor",
// "geometry"} in the sheet's units, angles in degrees; anchor is where it measures, paper mm from the view's centre, and
// geometry what it measures there, in the same paper mm: {"from", "to"} for lengths (and a cylinder seen from the side),
// {"centre", "r"} for a circle seen along its axis, {"lines": [[a, b], [a, b]]} for an angle. Throws Error when a
// reference is gone or the view shows it foreshortened.
json evaluate_item(const Document& doc, const Scene& scene, const Sheet& sheet, const SheetItem& item, const ViewFrame& frame);

// A sheet as a drawing (UI-86), in paper mm with the sheet as its paper: a frame 20 mm in from the left edge and 10 mm
// from the others (ISO 5457) with centring marks, each view projected and placed as layout() places it (visible lines
// 0.5 mm, tangent edges 0.25 mm or with the visible ones as the view's style says, hidden lines 0.25 mm dashed when it
// shows them), dimensions as geometry with their values now, notes. A dimension that cannot be measured any more shows
// the value it was made with in magenta; a view or item of a newer OPAD is left out. report (optional): {"views",
// "items", "bodies", "skipped": [{"id", "error"}]}. Projects every view: workers only; progress as project() takes it.
Display sheet_display(const Document& doc, const Scene& scene, const Sheet& sheet, const ProjectionProgress& progress = {}, json* report = nullptr);
std::string format_value(double value, const json& item);  // precision, prefix, tolerance and suffix of an item
// The parts of sheet_display, for an editor that shows each as soon as it is ready (UI-78); each primitive's source is the
// view or item that drew it. draw_paper: the paper's own drawing, its template (frame, zones, title block with its values,
// a template file's geometry) or for a sheet without one the plain ISO 5457 frame (title values may measure the part's
// mass: workers only). draw_view: a projected view placed by its frame, in the view's style. draw_items: the items of one
// view ("" those on the sheet itself), dimensions measured now (a dangling one in magenta with the value it was made with,
// listed in skipped as {id, error}); returns how many it drew.
void draw_paper(Display& d, const Document& doc, const Scene& scene, const Sheet& sheet);
void draw_view(Display& d, const ViewFrame& frame, const SheetView& view, const ViewGeometry& g);
int draw_items(Display& d, const Document& doc, const Scene& scene, const Sheet& sheet, const std::vector<ViewFrame>& frames, const std::string& view,
               json& skipped);

// ---- templates (UI-78): frames and title blocks drawn from scratch. A sheet record embeds its template (`template`), so
// a document draws the same everywhere and a later build's templates never change an existing sheet:
//   {"id": "iso" | "ansi" | "file", "name", "standard", "frame": {left, right, top, bottom, width}, "marks": centring marks,
//    "zones": {x, y, from: top-left (ISO: numbers from the left, letters from the top) | bottom-right (ASME)},
//    "title_block": {w, h, label_height, lines: [[x1, y1, x2, y2, width]], fields: [{key, label, rect: [x, y, w, h],
//    height, align left|center, valign bottom|middle|top}]} (in the block: mm from its bottom-left corner, which sits in
//    the frame's bottom-right corner), "geometry": a body key of a template file's drawing, "at": [x, y] where it goes,
//    "fields": the template's own fields in paper mm: cells {key, rect, height, align, valign, label?} placed in the editor
//    or text anchors {key, at, height, align left|center|right, valign baseline|bottom|middle|top, angle?, w?, tag?} that a
//    template file's attributes (ATTDEF, ATTRIB) and placeholder texts ({title}, <DWG_NO>) became}
// make_template: the ISO 5457 border (20 mm filing margin, 10 mm elsewhere, centring marks, zones of about 50 mm) with an
// ISO 7200 style block 180 mm wide, or the ASME style border with a block holding a general tolerance note; throws for
// another standard or a paper too small.
json make_template(const std::string& standard, double w, double h);
// Inside the frame and above the title block (paper mm: xmin, ymin, xmax, ymax), where views go; of a sheet record.
std::array<double, 4> drawing_room(const json& sheet);
// The title block's text by field key: the sheet's `values` (a value "=what" looks up what, as an empty one does its own
// key), else filled in: title (the drawn part's name, else the file's), number (its part number), author (who made the
// sheet), date (when), scale, size, units, sheet ("2 / 3" of its drawing), doctype (part or assembly drawing), material,
// mass (measured: workers only), tolerance (a general note), description, drawing, name, file, prop:<key> (a part
// property, else the document's), doc:<key> (a document property). The drawn part: the one node of the first base view,
// or the document's one root. The document's own properties (Scene::properties) come first for author, and for title,
// number and description when the sheet draws the whole document; any other key (owner or company, project, checked,
// approved, status, revision, ...) is the document property of that name.
json title_values(const Document& doc, const Scene& scene, const Sheet& sheet, bool measure = true);  // measure=false: no mass (the UI thread)
// A company's frame and title block from a DXF or DWG file: its 2D geometry goes into the body store (gc keeps it for the
// sheet) and the returned template draws it, on the smallest standard paper that holds it ("size"), moved onto it when it
// was drawn elsewhere ("at"); its attributes and placeholder texts become fields filled in like the built-in blocks' (a tag
// such as DWG_NO, DRAWN_BY or COMPANY names the field it stands for, others keep their own name). Throws Error when the
// file has no 2D geometry.
json template_from_file(Document& doc, const std::filesystem::path& file);
// The same in two steps, for an app that reads the file on a worker: the template without its geometry and the geometry
// as BREP text; then the commands take both (sheet / sheet_edit: template + template_brep) and store the geometry.
json read_template_file(const std::filesystem::path& file, std::string& brep);
std::string store_template_geometry(Document& doc, const json& tmpl, const std::string& brep);  // its body key; throws for bad BREP

// A new drawing's views (UI-78): `views` names standard views (front, back, top, bottom, left, right, iso, iso-back; "side"
// the one the projection puts right of the first). The first is the base view; the next ones are projected from it where
// the sheet's projection (first or third angle) puts them, the others (iso) are pictorial base views in a free corner.
// They are laid out on a grid around the base view, centred in the sheet's drawing room, `gap` paper mm apart; scale
// "auto" takes the largest standard scale (ISO 5455) at which they fit. sheet: the sheet record (size, template,
// projection); source: {nodes, hide}; style: the views' {hidden, tangent}. Returns {"scale", "views": [{"view", "record"}]}
// with sheet_view records without their sheet (a projected one's parent is "base"). Measures the bodies' boxes the first
// time: workers only. Throws for an unknown view or scale.
json plan_views(const Document& doc, const Scene& scene, const json& sheet, const std::vector<std::string>& views, const json& source,
                const std::string& scale, const json& style = json::object(), double gap = 20);

}  // namespace opad::drawing
