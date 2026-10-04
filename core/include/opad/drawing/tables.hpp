#pragma once
// Parts lists, balloons, revision tables and issued revisions of drawing sheets (TODO 11 UI-84). sheet_item kinds:
//   parts_list      at [x, y] (sheet paper mm: its bottom right corner, the header at the bottom and the rows above it, as
//                   it stands on a title block; grow "down": its top right corner, the header on top), bom {mode top |
//                   parts, root} (default: the first level of what the sheet's first base view draws), columns [item qty
//                   name part_number description material mass vendor], headers {column: text}, width (paper mm: the
//                   name column takes what the others leave), numbers [{n, identity, node}]: item numbers as they were
//                   settled, so a row keeps its number while parts come and go (found by its BoM identity, else by a node
//                   of the row; a number is never given to another row). A row without one is numbered after the highest,
//                   in the BoM's order, until something settles it (a balloon on it, auto-balloon, an issue, renumbering).
//   balloon         view, refs [a face, edge or vertex of a part], place {text} (the circle's centre, paper mm from the
//                   view's centre), list (the parts list it numbers from; default the sheet's first, else its drawing's),
//                   qty (true: "n×" beside it), diameter (paper mm, 10); result {shown: its number}. The number is looked
//                   up whenever it is drawn: the row of its part, or of the listed component the part is in.
//   revision_table  at [x, y] (its top right corner; grow "up": its bottom right corner), width; rows: the drawing's
//                   issues, oldest first (REV, DESCRIPTION, DATE, APPROVED)
//   issue           a released revision of the sheet's drawing (not drawn): rev, description, date, by, approved, sheets,
//                   values {item: what it showed}, fingerprints {view: the projection's fingerprint}, frames {view: {at,
//                   centre, scale}}, frozen {view: body key of its linework as a 2D BREP compound in view coordinates
//                   (visible, tangent and hidden edges, in that order)}, pdf (file name), pdf_sha256, tag (a git tag)
// Title blocks take revision, date (of issue) and approved from the drawing's latest issue.
#include <map>
#include <string>
#include <vector>

#include "opad/design/feature.hpp"
#include "opad/drawing/display.hpp"
#include "opad/drawing/sheet.hpp"

namespace opad::drawing {

// The parts list a balloon or auto-balloon numbers from: `list` when it is one, else the sheet's first, else the first of
// the sheet's drawing; null when there is none.
const SheetItem* parts_list_of(const Scene& scene, const Sheet& sheet, const std::string& list = {});
// What a parts list shows now (def: its record; null: the default list of the sheet): {"rows": [{number, qty, name,
// part_number, description, material, mass, vendor, nodes, identity, settled}] by number, "columns", "headers",
// "numbers": every row's number settled, the earlier entries of parts that are gone kept, "changed": whether that differs
// from the record's}. renumber: 1, 2, ... in the BoM's order. Lists the BoM (cached for the document's state): workers.
json parts_rows(const Document& doc, const Scene& scene, const Sheet& sheet, const json& def, bool renumber = false);
// The row of a node (a body, or a part inside a listed component) in parts_rows' rows; null when it is not listed.
const json* row_of(const Scene& scene, const json& rows, const std::string& node);
// A new parts list's record (sheet_item, without its id): the sheet's default BoM, standing on the title block (or in the
// frame's bottom right corner), as wide as the block; numbers settled. args: sheet, bom, columns, at, grow. Workers.
json plan_parts_list(const Document& doc, const Scene& scene, const json& args);
// The columns a parts list without its own shows: item, qty, name, part_number, material.
const json& parts_list_columns();
// A new revision table's record: in the frame's top right corner, growing down. args: sheet, at, grow, width.
json plan_revision_table(const Scene& scene, const json& args);

// Auto-balloon (args: sheet, view, list, qty, all): a balloon for every row of the parts list that the view shows and that
// has none there yet (all: every row again), on the longest edge of the part that the view shows whole, placed in columns
// and rows around the view, spread so that none overlap and their leaders do not cross. {"ops": balloon records, "list":
// its id, "create": a parts list record when the drawing has none (the balloons then number from it), "numbers": the list's
// settled numbers when they changed}. Projects the view: workers.
json plan_balloons(const Document& doc, const Scene& scene, const json& args);

// The issues of the sheet's drawing (the sheet's own when it has no drawing), oldest first.
std::vector<const SheetItem*> drawing_issues(const Scene& scene, const Sheet& sheet);
// The revision after the drawing's latest: A, B, ... (I, O, Q, S, X and Z skipped), AA after Y; 2 after 1; A at first.
std::string next_revision(const Scene& scene, const Sheet& sheet);
// An issue of the sheet's drawing (args: sheet, rev, description, by, approved, date, freeze, tag): its record with every
// item's value now, the views' fingerprints and frames, and (freeze, default on) the views' linework as BREP text by view
// id in *frozen, for the command to store; {"op", "edits": the parts lists' settled numbers as edit ops}. Projects every
// view: workers. Throws when the revision was issued already.
json plan_issue(const Document& doc, const Scene& scene, const json& args, std::map<std::string, std::string>* frozen = nullptr);
// That plan ready for design::commit (the app's AppDocument::commitPlan): the frozen linework as body entries hashed here,
// named after their views, their keys in the op's frozen; ops: the edits, then the issue; report: {rev, frozen: how many, pdf,
// pdf_sha256}. Workers: the UI thread neither parses nor hashes tens of megabytes of linework.
design::Plan issue_commit_plan(const Scene& scene, json op, const json& edits, std::map<std::string, std::string>&& frozen);
// The scene as it will be once `op` (an issue planned above) is appended: title blocks and revision tables show it, so the
// PDF written before the op is what the drawing then shows.
Scene with_issue(const Scene& scene, const json& op);
// A view's linework as a 2D BREP compound in view coordinates (model mm): visible, tangent and hidden edges, each a compound.
std::string linework_brep(const ViewGeometry& g);
// That linework back as the view's curves (sharp, tangent, hidden; a polyline comes back as its segments).
ViewGeometry frozen_geometry(const TopoDS_Shape& lines);
// What issuing the sheet's drawing with frozen linework adds to the body store, in bytes: the linework of every view that
// the store does not hold yet (a view unchanged since the last issue adds nothing). Projects every view (cached): workers.
size_t frozen_bytes(const Document& doc, const Scene& scene, const Sheet& sheet, const ProjectionProgress& progress = {});
// What changed since an issue: {"views": ids whose projection differs, "values": items that show another value now,
// "gone": items or views that are gone}. Workers (fingerprints count faces, items are measured).
json issue_changes(const Document& doc, const Scene& scene, const SheetItem& issue);
// The issue of a sheet's drawing named by its revision or id; null when there is none.
const SheetItem* find_issue(const Scene& scene, const Sheet& sheet, const std::string& rev);
// The scene as it stood when the issue was made: the log up to its op, so later edits, deletes, records and issues are left
// out and the model is the one it showed (Document::gc keeps its bodies). Throws when the issue is not in the log.
Scene issued_scene(const Document& doc, const SheetItem& issue);
// A sheet as it was issued (then: issued_scene(issue); sheet and issue: any scene's, found in it by id): its paper with that
// revision as the drawing's latest, each view's frozen linework where the view stood then with its centre marks (a view that
// froze none is projected in `then`), every item of then measured in it, a dimension writing the value it was issued with;
// report as sheet_display's. Throws when the sheet was not part of it. Workers (projects what was not frozen).
Display issued_display(const Document& doc, const Scene& then, const Sheet& sheet, const SheetItem& issue, const ProjectionProgress& progress = {},
                       json* report = nullptr);

// Drawing them (draw_item's): a parts list or revision table from its rows; a balloon on paper.
void draw_table_item(Display& d, const json& def, const json& measured, const DimStyle& s = {});
void balloon(Display& d, int layer, Vec2 centre, double diameter, const std::string& number, Vec2 tip, bool dot, const std::string& qty = {},
             const DimStyle& s = {});

}  // namespace opad::drawing
