#pragma once
// Annotations of drawing sheets (TODO 11 UI-79, UI-80, UI-81): sheet_item records that refer to the model through stable
// references (node + ordinal + hint, as features do), measured from the model whenever the sheet is drawn and keeping what
// they showed when they were made (`result`). Kinds, beside dimension and note (sheet.hpp):
//   note          + refs [one]: a leader from the text to that point (its aspect), or to: [x, y] a free leader end
//   centermark    refs [a circle seen along its axis, or its cylinder]; extend (paper mm past the circle, 2)
//   centerline    refs [two circles] (through their centres), [a cylinder seen from the side] (its axis) or [two
//                 parallel lines] (midway); extend (3)
//   hole_callout  refs [a face or circular edge of a hole]; place {text}; result {shown, hole} ("4× ⌀5 THRU")
//   hole_table    view; at [x, y] (sheet paper mm, its top left corner); refs [origin] (optional: else the view's lower
//                 left); result {rows: [{tag, x, y, size}]} of the holes seen along their axis, tagged in the view
//   datum         refs [an edge]; letter "A"; place {text} (the frame's centre)
//   fcf           characteristic (characteristics()), value (the tolerance), zone "diameter", material "M" | "L" | "S",
//                 datums ["A", "B(M)"]; place {text} (its left end); refs [optional: a leader to it]
//   surface       refs [an edge]; process any | removal | no_removal; value "Ra 1.6"; place {text} (which side, where)
//   dimension_set type ordinate | baseline | chain; axis horizontal | vertical; refs [origin, features...]; place {text}
//                 (ordinate: where the values stand; else the first dimension line); spacing (baseline, 7); precision;
//                 result {values, shown}
// Places are paper mm from the item's view's centre. A build that does not know a kind keeps the record and reports it.
#include <set>
#include <string>
#include <vector>

#include "opad/drawing/display.hpp"
#include "opad/drawing/sheet.hpp"

namespace opad::drawing {

bool known_item(const std::string& kind, const std::string& type);  // kinds and types this build draws
// An item's value written the sheet's way: precision decimals; inches under ASME with trailing zeros and no leading zero,
// otherwise without trailing zeros.
std::string format_number(double value, int precision, const Sheet* sheet = nullptr);

// A pick on a view: what the app's snap index found under the pointer, {"node", "edge" | "face" (ordinals of the curve's
// source), "snap" (end, mid, centre, quadrant, intersection, nearest), "at" [x, y] (sheet paper mm)}, made a reference
// with its hint: {"ref", "what" (vertex, line, circle, arc, edge, cylinder, plane, face), "at" (paper mm from the view's
// centre: the point it stands for)}. An end snap is the edge's start or end where that end shows (a hidden-line piece's
// end is not a vertex: the edge itself then). Throws when the curve is not the model's. Workers: resolves the reference.
json pick_reference(const Document& doc, const Scene& scene, const ViewFrame& frame, const json& pick);

// What an item draws from, measured from its references now, in paper mm from its view's centre (a hole table's rows in
// sheet paper mm): a dimension's evaluate_item, a centre mark's {centre, r}, a centre line's {from, to}, a leader's {tip}, a
// hole callout's {shown, hole, count, tip, centre, r}, a datum's or surface symbol's {foot, out}, a set's {origin, points,
// values, shown}, a hole table's {rows, origin}. Throws Error when a reference is gone or the view shows it foreshortened.
// Workers: resolves references (a hole table walks the view's bodies).
json measure_item(const Document& doc, const Scene& scene, const Sheet& sheet, const SheetItem& item, const ViewFrame* frame);
// Draws an item from its measure: pure 2D (the sheet canvas previews an item being placed with it on every mouse move).
// origin: its view's centre on paper. Layers: Dimensions, Text, Center.
void draw_item(Display& d, const Sheet& sheet, const json& def, const json& measured, Vec2 origin, const DimStyle& style = {});
// The result an item keeps (what it showed when it was made), from its measure; null for kinds without one.
json item_result(const json& def, const json& measured);

// References as a sheet item keeps them (with hints; a centre reference as its circle with the aspect "center"); aspects:
// per reference, start | end | mid | center. Throws for a body that does not exist.
json item_references(const Document& doc, const Scene& scene, const json& refs, const json& aspects = json());
// The sheet_item record the sheet_item command appends: checked, measured, its result kept and placed beside what it
// measures unless args place it. args as the command takes them (sheet, view, kind, type, refs or picks, aspects, place,
// ...). Workers: measures. The app plans on a worker and appends the record (sheet_item "op").
json plan_item(const Document& doc, const Scene& scene, const json& args, json* measured = nullptr);
// The readings a smart dimension of these picks or refs can have, each planned: {"picks": [pick_reference...],
// "choices": [{"type", "op", "measured"}]}: one circle -> diameter (radius for an arc); a cylinder from the side ->
// diameter; one line or two points -> horizontal, vertical and aligned; two lines that meet -> angle (and those three);
// a line and a point -> the three; the app takes the one the pointer's place says. Throws for picks that make none.
json plan_dimension(const Document& doc, const Scene& scene, const json& args);
// "Dimension from datums": the datum symbols of a view (letters, default the first two that are straight edges, one
// upright and one level) as origins, and its features (refs; default every circle seen along its axis, i.e. the holes):
// a dimension_set record (type ordinate | baseline | chain) for each direction a datum measures. Returns {"ops": [...]}.
json datum_dimensions(const Document& doc, const Scene& scene, const json& args);
// The centre marks and centre lines a view with style centermarks draws itself (draw_view): a mark on every whole circle
// seen along its axis (the widest of concentric ones), and a centre line along every cylinder seen from the side: `axes`
// (cylinder_axes), else midway between the outlines the projection traced on one face. Pure 2D.
void view_centre_marks(Display& d, const ViewFrame& frame, const ViewGeometry& g, const DimStyle& style = {},
                       const std::vector<std::array<Vec2, 2>>* axes = nullptr);
// The axes of the whole cylinders (holes, pins, shafts; split ones joined) a view sees from the side, in sheet paper mm,
// collinear pieces made one. Walks the view's bodies' faces: workers only.
std::vector<std::array<Vec2, 2>> cylinder_axes(const Document& doc, const Scene& scene, const ViewFrame& frame, const ViewSpec& spec);

}  // namespace opad::drawing
