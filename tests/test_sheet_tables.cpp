// Parts lists, balloons, revision tables and issued revisions (TODO 11 UI-84): rows from the bill of materials with item
// numbers that stay with their parts while parts come and go, renumbering, balloons numbered from the list (one placed by
// hand settles a new row's number; auto-balloon places one per row around the view), the drawing's issues in a revision
// table and the title block, what an issue keeps (values, fingerprints, frozen linework in the body store) and reports as
// changed afterwards, and the records' checks.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/drawing/tables.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/util.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {

json run(Document& doc, const std::string& command, const json& args) { return commands::run(command, args, &doc); }

json body(const std::string& name, const std::string& key, double x = 0, double y = 0, double z = 0) {
  json n = {{"type", "body"}, {"id", new_uuid()}, {"name", name}, {"key", key}};
  if (x || y || z) n["transform"] = Mat4{{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1}}.to_json();
  return n;
}

std::string id_of(const Scene& s, const std::string& name) {
  for (const auto& [id, n] : s.nodes)
    if (n.name == name) return id;
  throw Error("no node " + name);
}

bool has_text(const Display& d, const std::string& s) {
  return std::any_of(d.prims.begin(), d.prims.end(), [&](const Prim& p) { return p.kind == Prim::Kind::Text && p.text == s; });
}

// An assembly: a 60 x 40 x 5 plate, four 6 mm pins standing on it (one shape, four places) and a bracket beside it; an A3
// sheet with its front view.
struct Assembly {
  Document doc = Document::create();
  std::string assembly, sheet, front, pin;
  Assembly() {
    const std::string plate = doc.add_body(brep_from_shape(BRepPrimAPI_MakeBox(60, 40, 5).Shape()), {{"name", "Plate"}, {"units", "mm"}});
    pin = doc.add_body(brep_from_shape(BRepPrimAPI_MakeCylinder(3, 20).Shape()), {{"name", "Pin"}, {"units", "mm"}});
    const std::string bracket = doc.add_body(brep_from_shape(BRepPrimAPI_MakeBox(10, 10, 30).Shape()), {{"name", "Bracket"}, {"units", "mm"}});
    assembly = new_uuid();
    doc.append({{"op", "import"},
                {"source", "design"},
                {"nodes", {{{"type", "component"}, {"id", assembly}, {"name", "Assembly"},
                            {"children", {body("Plate", plate), body("Pin 1", pin, 10, 10, 5), body("Pin 2", pin, 50, 10, 5), body("Pin 3", pin, 10, 30, 5),
                                          body("Pin 4", pin, 50, 30, 5), body("Bracket", bracket, -20, 0, 0)}}}}}});
    const json made = run(doc, "sheet", {{"size", "A3"}, {"views", {"front"}}});
    sheet = made["id"];
    front = made["views"][0]["id"];
  }
  Scene scene() const { return resolve(doc); }
  json rows(const std::string& list) {
    const Scene s = scene();
    return parts_rows(doc, s, *s.sheet(sheet), s.sheet_item(list)->def)["rows"];
  }
  // name -> number in the list now
  std::map<std::string, int> numbers(const std::string& list) {
    std::map<std::string, int> out;
    for (const auto& r : rows(list)) out[r["name"]] = r["number"];
    return out;
  }
};

}  // namespace

// A parts list lists the assembly's first level with quantities, numbered 1, 2, 3 in its order and settled in the record;
// it stands on the title block as wide as it, and draws its header and rows.
TEST(parts_list_from_the_bom) {
  Assembly a;
  const json made = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "parts_list"}});
  const Scene s = a.scene();
  const SheetItem* list = s.sheet_item(made["id"]);
  CHECK(list && list->kind == "parts_list" && list->error.empty());
  CHECK_EQ(list->def["numbers"].size(), 3u);
  const auto n = a.numbers(made["id"]);
  CHECK_EQ(n.at("Plate"), 1);
  CHECK_EQ(n.at("Pin"), 2);
  CHECK_EQ(n.at("Bracket"), 3);
  for (const auto& r : a.rows(made["id"])) CHECK(r["settled"].get<bool>());
  CHECK_EQ(a.rows(made["id"])[1]["qty"], 4);
  const auto at = list->def["at"];  // the ISO block: 180 wide, 10 mm in from the right, standing on it
  CHECK_NEAR(at[0].get<double>(), 410, 1e-9);
  CHECK(at[1].get<double>() > 10);
  CHECK_NEAR(list->def["width"].get<double>(), 180, 1e-9);
  const Display d = sheet_display(a.doc, s, *s.sheet(a.sheet));
  for (const char* t : {"ITEM", "QTY", "NAME", "PART NUMBER", "MATERIAL", "Plate", "Pin", "Bracket", "4"}) CHECK(has_text(d, t));
  // Header at the bottom, item 1 right above it, the table as wide as the block.
  std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
  double item1 = 0, header = 0;
  for (const auto& p : d.prims) {
    if (p.source != list->id) continue;
    if (p.kind == Prim::Kind::Text && p.text == "Plate") item1 = p.at[1];
    if (p.kind == Prim::Kind::Text && p.text == "NAME") header = p.at[1];
    if (p.kind == Prim::Kind::Curve)
      for (const auto& q : p.curve.pts) box = {std::min(box[0], q[0]), std::min(box[1], q[1]), std::max(box[2], q[0]), std::max(box[3], q[1])};
  }
  CHECK(item1 > header);
  CHECK_NEAR(box[2] - box[0], 180, 1e-6);
  CHECK_NEAR(box[1], at[1].get<double>(), 1e-6);
  // Columns, headers and the order of the rows: settable; a part's own part number shows.
  run(a.doc, "part_properties", {{"target", id_of(a.scene(), "Bracket")}, {"set", {{"part_number", "BR-7"}, {"material", "steel"}}}});
  const Scene t = a.scene();
  const Display e = sheet_display(a.doc, t, *t.sheet(a.sheet));
  CHECK(has_text(e, "BR-7"));
  CHECK_EQ(a.numbers(made["id"]).at("Bracket"), 3);  // its identity changed with the part number: found by its node
}

// Numbers stay with their parts: a new part is numbered after the highest (not in the BoM's order), a part left out keeps its
// number (never given to another) and gets it back; renumbering counts 1, 2, ... again.
TEST(item_numbers_are_stable) {
  Assembly a;
  const std::string list = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "parts_list"}})["id"];
  // A washer inserted first in the tree: number 4, not 1, and not settled yet.
  const std::string washer = a.doc.add_body(brep_from_shape(BRepPrimAPI_MakeCylinder(5, 1).Shape()), {{"name", "Washer"}, {"units", "mm"}});
  a.doc.append({{"op", "import"}, {"source", "design"}, {"parent", a.assembly}, {"nodes", {body("Washer", washer, 30, 20, 5)}}});
  auto n = a.numbers(list);
  CHECK_EQ(n.at("Plate"), 1);
  CHECK_EQ(n.at("Pin"), 2);
  CHECK_EQ(n.at("Bracket"), 3);
  CHECK_EQ(n.at("Washer"), 4);
  bool settled = true;
  for (const auto& r : a.rows(list))
    if (r["name"] == "Washer") settled = r["settled"].get<bool>();
  CHECK(!settled);
  // The pins out of the bill of materials: their number stays theirs.
  for (const char* pin : {"Pin 1", "Pin 2", "Pin 3", "Pin 4"}) run(a.doc, "part_properties", {{"target", id_of(a.scene(), pin)}, {"set", {{"bom", "exclude"}}}});
  n = a.numbers(list);
  CHECK(!n.count("Pin"));
  CHECK_EQ(n.at("Bracket"), 3);
  CHECK_EQ(n.at("Washer"), 4);
  for (const char* pin : {"Pin 1", "Pin 2", "Pin 3", "Pin 4"}) run(a.doc, "part_properties", {{"target", id_of(a.scene(), pin)}, {"set", {{"bom", nullptr}}}});
  CHECK_EQ(a.numbers(list).at("Pin"), 2);
  // Renumbered: in the BoM's order (the washer is last in the tree, so 4 still), all settled.
  run(a.doc, "sheet_edit", {{"target", list}, {"set", {{"renumber", true}}}});
  n = a.numbers(list);
  CHECK_EQ(n.at("Plate"), 1);
  CHECK_EQ(n.at("Washer"), 4);
  for (const auto& r : a.rows(list)) CHECK(r["settled"].get<bool>());
  CHECK_EQ(a.scene().sheet_item(list)->def["numbers"].size(), 4u);
}

// A balloon shows its part's row number (a pin: the pins' row), with its quantity when asked; placed on a part whose number
// was not settled, it settles the list's numbers in the same step; a part outside the list dangles.
TEST(balloons_number_from_the_list) {
  Assembly a;
  const std::string list = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "parts_list"}})["id"];
  const Scene s = a.scene();
  const std::string pin3 = id_of(s, "Pin 3");
  const TopoDS_Shape shape = node_world_shape(a.doc, s, pin3);
  int top = -1;  // the pin's top circle (z 25)
  for (int i = 0; i < subshape_count(shape, Ref::Kind::Edge) && top < 0; ++i) {
    const json e = inspect_ref(a.doc, s, Ref{pin3, Ref::Kind::Edge, i});
    if (e.value("curve", "") == "circle" && std::fabs(e["center"][2].get<double>() - 25) < 1e-6) top = i;
  }
  CHECK(top >= 0);
  const json b = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"view", a.front}, {"kind", "balloon"}, {"refs", {Ref{pin3, Ref::Kind::Edge, top}.str()}}, {"qty", true}});
  CHECK_EQ(b["result"]["shown"], "2");
  const Scene t = a.scene();
  const Display d = sheet_display(a.doc, t, *t.sheet(a.sheet));
  CHECK(has_text(d, "4×"));
  bool circle = false;
  for (const auto& p : d.prims)
    if (p.source == b["id"].get<std::string>() && p.kind == Prim::Kind::Curve && p.curve.type == Curve::Type::Arc && std::fabs(p.curve.r1 - 5) < 1e-9) circle = true;
  CHECK(circle);
  // A new part ballooned: the list's numbers settled first, in the same command.
  const std::string washer = a.doc.add_body(brep_from_shape(BRepPrimAPI_MakeBox(8, 8, 2).Shape()), {{"name", "Shim"}, {"units", "mm"}});
  a.doc.append({{"op", "import"}, {"source", "design"}, {"parent", a.assembly}, {"nodes", {body("Shim", washer, 70, 0, 0)}}});
  const size_t ops = a.doc.ops.size();
  const json shim = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"view", a.front}, {"kind", "balloon"}, {"refs", {Ref{id_of(a.scene(), "Shim"), Ref::Kind::Edge, 0}.str()}}});
  CHECK_EQ(shim["result"]["shown"], "4");
  CHECK_EQ(a.doc.ops.size(), ops + 2);
  CHECK_EQ(a.doc.ops[ops].type, "edit");
  for (const auto& r : a.rows(list)) CHECK(r["settled"].get<bool>());
  // Out of the bill of materials: dangling, drawn with the number it showed.
  run(a.doc, "part_properties", {{"target", id_of(a.scene(), "Shim")}, {"set", {{"bom", "exclude"}}}});
  const json info = run(a.doc, "sheet_info", {{"sheet", a.sheet}});
  bool dangling = false;
  for (const auto& i : info["items"])
    if (i["id"] == shim["id"]) dangling = i.value("dangling", false);
  CHECK(dangling);
  CHECK_THROWS(run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"view", a.front}, {"kind", "balloon"}, {"refs", {Ref{pin3, Ref::Kind::Edge, top}.str()}}, {"list", a.front}}));
}

// Auto-balloon: one balloon per row the view shows (a parts list is added when there is none), numbered as the list, around
// the view's frame without overlapping; again, it adds none for rows that have one; all adds them again.
TEST(auto_balloon) {
  Assembly a;
  const json first = run(a.doc, "sheet_balloons", {{"sheet", a.sheet}, {"view", a.front}});
  CHECK(first["created"].get<bool>());
  CHECK_EQ(first["ids"].size(), 3u);
  const Scene s = a.scene();
  const auto frames = layout(a.doc, s, *s.sheet(a.sheet));
  const ViewFrame& f = frames[0];
  std::set<std::string> shown;
  std::vector<Vec2> at;
  for (const auto& id : first["ids"]) {
    const SheetItem* t = s.sheet_item(id);
    CHECK(t && t->kind == "balloon" && t->def["list"] == first["list"]);
    shown.insert(t->def["result"]["shown"].get<std::string>());
    const Vec2 p{f.at[0] + t->def["place"]["text"][0].get<double>(), f.at[1] + t->def["place"]["text"][1].get<double>()};
    CHECK(p[0] < f.box[0] || p[0] > f.box[2] || p[1] < f.box[1] || p[1] > f.box[3]);  // outside the view
    at.push_back(p);
  }
  CHECK(shown == std::set<std::string>({"1", "2", "3"}));
  for (size_t i = 0; i < at.size(); ++i)
    for (size_t j = i + 1; j < at.size(); ++j) CHECK(std::hypot(at[i][0] - at[j][0], at[i][1] - at[j][1]) >= 10);
  const json info = run(a.doc, "sheet_info", {{"sheet", a.sheet}});
  for (const auto& i : info["items"]) CHECK(!i.contains("error"));
  CHECK_EQ(run(a.doc, "sheet_balloons", {{"sheet", a.sheet}, {"view", a.front}})["ids"].size(), 0u);
  CHECK_EQ(run(a.doc, "sheet_balloons", {{"sheet", a.sheet}, {"view", a.front}, {"all", true}})["ids"].size(), 3u);
}

// Auto-balloon keeps its balloons inside the frame and off what is on the sheet: a parts list placed right under the
// view (where the plate's balloon would go, below its long bottom edge), the title block, the other views.
TEST(auto_balloon_keeps_off_the_sheets_tables) {
  Assembly a;
  run(a.doc, "sheet_view", {{"sheet", a.sheet}, {"parent", a.front}, {"side", "top"}});
  Scene s = a.scene();
  auto frames = layout(a.doc, s, *s.sheet(a.sheet));
  const ViewFrame front = *std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& x) { return x.id == a.front; });
  const std::string list = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "parts_list"}, {"width", 120}, {"grow", "down"}, {"at", {front.box[2] + 20, front.box[1] - 4}}})["id"];
  const json made = run(a.doc, "sheet_balloons", {{"sheet", a.sheet}, {"view", a.front}});
  CHECK_EQ(made["ids"].size(), 3u);
  s = a.scene();
  frames = layout(a.doc, s, *s.sheet(a.sheet));
  const Sheet& sheet = *s.sheet(a.sheet);
  std::vector<std::array<double, 4>> taken;
  for (const auto& f : frames)
    if (f.id != a.front) taken.push_back(f.box);
  Display d;
  draw_table_item(d, s.sheet_item(list)->def, measure_item(a.doc, s, sheet, *s.sheet_item(list), nullptr));
  const auto table = d.bounds();
  CHECK(table[3] > front.box[1] - 13 + 5);  // a balloon below the view would sit on it
  taken.push_back(table);
  const json block = sheet.def["template"]["title_block"];
  taken.push_back({sheet.width - 10 - block["w"].get<double>(), 10, sheet.width - 10, 10 + block["h"].get<double>()});
  for (const auto& id : made["ids"]) {
    const SheetItem& b = *s.sheet_item(id.get<std::string>());
    const Vec2 at{front.at[0] + b.def["place"]["text"][0].get<double>(), front.at[1] + b.def["place"]["text"][1].get<double>()};
    CHECK(at[0] - 5 >= 20 && at[0] + 5 <= sheet.width - 10 && at[1] - 5 >= 10 && at[1] + 5 <= sheet.height - 10);  // inside the frame
    for (const auto& t : taken) CHECK(!(at[0] + 5 > t[0] && at[0] - 5 < t[2] && at[1] + 5 > t[1] && at[1] - 5 < t[3]));
  }
}

// Issues: the revision table lists them, the title block shows the latest's revision, date and approver; an issue keeps
// every value, the views' fingerprints and their linework (gc keeps it); a model change shows as changed views and values
// since; a revision cannot be issued twice; the next one follows the letters (I, O, Q, S, X, Z skipped).
TEST(issued_revisions) {
  Assembly a;
  const std::string table = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "revision_table"}})["id"];
  const std::string plate = id_of(a.scene(), "Plate"), bracket = id_of(a.scene(), "Bracket");
  run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"view", a.front}, {"kind", "dimension"}, {"type", "horizontal"}, {"refs", {plate + "/vertex/0", bracket + "/vertex/0"}}});
  CHECK_EQ(next_revision(a.scene(), *a.scene().sheet(a.sheet)), "A");
  Scene s = a.scene();
  const size_t grows = frozen_bytes(a.doc, s, *s.sheet(a.sheet));
  CHECK(grows > 1000);
  const json issued = run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"description", "First release"}, {"approved", "R. Engineer"}, {"date", "2026-10-04"}});
  CHECK_EQ(issued["rev"], "A");
  CHECK_EQ(issued["frozen"], 1);
  s = a.scene();
  const SheetItem* issue = s.sheet_item(issued["id"]);
  CHECK(issue && issue->kind == "issue" && issue->error.empty());
  CHECK_EQ(issue->def["values"].size(), 1u);
  CHECK(issue->def["fingerprints"].contains(a.front));
  const std::string key = issue->def["frozen"][a.front];
  CHECK(a.doc.has_body(key));
  const TopoDS_Shape lines = shape_from_brep(a.doc.body(key)->brep);
  CHECK(subshape_count(lines, Ref::Kind::Edge) > 4);
  CHECK_EQ(a.doc.body(key)->brep.size(), grows);
  CHECK_EQ(frozen_bytes(a.doc, s, *s.sheet(a.sheet)), 0u);  // unchanged since A: nothing more to keep
  a.doc.gc();
  CHECK(a.doc.has_body(key));
  const json values = title_values(a.doc, s, *s.sheet(a.sheet));
  CHECK_EQ(values["revision"], "A");
  CHECK_EQ(values["date"], "2026-10-04");
  CHECK_EQ(values["approved"], "R. Engineer");
  const Display d = sheet_display(a.doc, s, *s.sheet(a.sheet));
  for (const char* t : {"REV", "DESCRIPTION", "First release", "2026-10-04", "R. Engineer"}) CHECK(has_text(d, t));
  json changes = issue_changes(a.doc, s, *issue);
  CHECK(changes["views"].empty() && changes["values"].empty() && changes["gone"].empty());
  CHECK_THROWS(run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"rev", "A"}}));
  // The bracket moved: the view and the dimension changed since A.
  a.doc.append({{"op", "transform"}, {"target", bracket}, {"matrix", Mat4{{1, 0, 0, -25, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}.to_json()}});
  s = a.scene();
  changes = issue_changes(a.doc, s, *s.sheet_item(issued["id"]));
  CHECK_EQ(changes["views"].size(), 1u);
  CHECK_EQ(changes["values"].size(), 1u);
  CHECK_EQ(run(a.doc, "sheet_info", {{"sheet", a.sheet}})["issues"][0]["changed"]["views"].size(), 1u);
  CHECK(frozen_bytes(a.doc, s, *s.sheet(a.sheet)) > 1000);
  const json b = run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"freeze", false}});
  CHECK_EQ(b["rev"], "B");
  CHECK_EQ(b["frozen"], 0);
  run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"rev", "H"}});
  CHECK_EQ(next_revision(a.scene(), *a.scene().sheet(a.sheet)), "J");
  run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"rev", "Y"}});
  CHECK_EQ(next_revision(a.scene(), *a.scene().sheet(a.sheet)), "AA");
  run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"rev", "9"}});
  CHECK_EQ(next_revision(a.scene(), *a.scene().sheet(a.sheet)), "10");
  s = a.scene();
  json m;
  SheetItem t = *s.sheet_item(table);
  m = measure_item(a.doc, s, *s.sheet(a.sheet), t, nullptr);
  CHECK_EQ(m["rows"].size(), 5u);
  CHECK_EQ(m["rows"][0]["rev"], "A");
  // An issue drawn with the scene as it will be (the PDF made before the op): the title block shows it.
  const json planned = plan_issue(a.doc, s, {{"sheet", a.sheet}})["op"];
  CHECK_EQ(planned["rev"], "10");
  const Scene ahead = with_issue(s, planned);
  CHECK_EQ(title_values(a.doc, ahead, *ahead.sheet(a.sheet))["revision"], "10");
  CHECK_EQ(title_values(a.doc, s, *s.sheet(a.sheet))["revision"], "9");
  // The app's path: planned and hashed on a worker, committed as it is (no parse, no hash on the UI thread).
  a.doc.append({{"op", "transform"}, {"target", bracket}, {"matrix", Mat4{{1, 0, 0, 7, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}.to_json()}});
  s = a.scene();
  std::map<std::string, std::string> frozen;
  const json p10 = plan_issue(a.doc, s, {{"sheet", a.sheet}, {"rev", "10"}}, &frozen);
  CHECK_EQ(frozen.size(), 1u);
  const std::string text = frozen.begin()->second;
  opad::design::Plan plan = issue_commit_plan(s, p10["op"], p10["edits"], std::move(frozen));
  CHECK_EQ(plan.bodies.size(), 1u);
  CHECK_EQ(plan.bodies[0].key, sha256_hex(text));
  CHECK_EQ(plan.ops.back()["frozen"][a.front], plan.bodies[0].key);
  CHECK(!a.doc.has_body(plan.bodies[0].key));
  const json report = opad::design::commit(a.doc, std::move(plan));
  CHECK_EQ(report["rev"], "10");
  CHECK_EQ(report["frozen"], 1);
  s = a.scene();
  const SheetItem* ten = s.sheet_item(a.doc.ops.back().id);
  CHECK(ten && ten->kind == "issue" && a.doc.has_body(ten->def["frozen"][a.front]) && a.doc.body(ten->def["frozen"][a.front])->brep == text);
  // A caller's planned op: its linework is read back before it is stored.
  json op11 = plan_issue(a.doc, a.scene(), {{"sheet", a.sheet}, {"rev", "11"}})["op"];
  CHECK_THROWS(run(a.doc, "sheet_issue", {{"op", op11}, {"frozen", {{a.front, "not a shape\n"}}}}));
  CHECK_EQ(run(a.doc, "sheet_issue", {{"op", op11}, {"frozen", {{a.front, text}}}})["frozen"], 1);
}

// A sheet drawn as it was issued, in the scene as it stood then: the views from their frozen linework where they stood with
// their centre lines (not as the model is now), the annotations as they were (a dimension's extension lines on the model as
// issued, a note deleted later there, one made later not), the revision table without later issues, a dimension writing its
// issued value; gc keeps the model it showed (a washer deleted since); the export command takes the revision.
TEST(issued_revision_drawn_as_issued) {
  Assembly a;
  run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "revision_table"}});
  run(a.doc, "sheet_edit", {{"target", a.front}, {"set", {{"style", {{"centermarks", true}}}}}});
  const std::string plate = id_of(a.scene(), "Plate"), bracket = id_of(a.scene(), "Bracket");
  const std::string dim = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"view", a.front}, {"kind", "dimension"}, {"type", "horizontal"}, {"refs", {plate + "/vertex/0", bracket + "/vertex/0"}}})["id"];
  const std::string deburr = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "note"}, {"text", "DEBURR"}, {"at", {60, 60}}})["id"];
  const std::string washer = a.doc.add_body(brep_from_shape(BRepPrimAPI_MakeCylinder(5, 1).Shape()), {{"name", "Washer"}, {"units", "mm"}});
  const std::string placed = a.doc.append({{"op", "import"}, {"source", "design"}, {"parent", a.assembly}, {"nodes", {body("Washer", washer, 30, 20, 5)}}}).id;
  const Scene before = a.scene();
  const Display then = sheet_display(a.doc, before, *before.sheet(a.sheet));
  run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"description", "First"}});
  a.doc.append({{"op", "transform"}, {"target", bracket}, {"matrix", Mat4{{1, 0, 0, -45, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}.to_json()}});
  a.doc.append({{"op", "delete"}, {"target", deburr}});
  a.doc.append({{"op", "delete"}, {"target", placed}});
  run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "note"}, {"text", "LATER"}, {"at", {60, 80}}});
  run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"description", "Second"}});
  const Scene s = a.scene();
  const SheetItem* issue = find_issue(s, *s.sheet(a.sheet), "A");
  CHECK(issue && find_issue(s, *s.sheet(a.sheet), issue->id) == issue && !find_issue(s, *s.sheet(a.sheet), "Z"));
  const Display now = sheet_display(a.doc, s, *s.sheet(a.sheet));
  const Scene was = issued_scene(a.doc, *issue);
  CHECK(was.sheet_item(deburr) && was.node(id_of(before, "Washer")) && !s.sheet_item(deburr));
  json report;
  const Display issued = issued_display(a.doc, was, *s.sheet(a.sheet), *issue, {}, &report);
  CHECK_EQ(report["views"], 1);
  CHECK_EQ(report["items"], 4);  // the revision table, the dimension, the note and A (not drawn); not B
  CHECK(report["skipped"].empty());
  const auto bounds = [&](const Display& d, const std::string& source, const char* layer) {
    Display v;
    const int at = static_cast<int>(std::find_if(d.layers.begin(), d.layers.end(), [&](const Layer& l) { return l.name == layer; }) - d.layers.begin());
    for (const auto& p : d.prims)
      if (p.source == source && p.layer == at && p.kind == Prim::Kind::Curve) v.prims.push_back(p);
    return std::make_pair(v.prims.size(), v.bounds());
  };
  const auto b0 = bounds(then, a.front, "Visible"), b1 = bounds(issued, a.front, "Visible"), b2 = bounds(now, a.front, "Visible");
  for (int i = 0; i < 4; ++i) CHECK_NEAR(b1.second[i], b0.second[i], 1e-6);
  CHECK(std::fabs(b2.second[0] - b0.second[0]) > 1);  // the bracket is further left now
  const auto c0 = bounds(then, a.front, "Center"), c1 = bounds(issued, a.front, "Center");  // the pins' and the washer's centre lines
  CHECK(c0.first >= 5 && c1.first == c0.first);
  for (int i = 0; i < 4; ++i) CHECK_NEAR(c1.second[i], c0.second[i], 1e-6);
  const auto d0 = bounds(then, dim, "Dimensions"), d1 = bounds(issued, dim, "Dimensions"), d2 = bounds(now, dim, "Dimensions");
  CHECK(d0.first > 2 && d1.first == d0.first);  // the extension lines reach the bracket where it stood
  for (int i = 0; i < 4; ++i) CHECK_NEAR(d1.second[i], d0.second[i], 1e-6);
  CHECK(std::fabs(d2.second[0] - d0.second[0]) > 1);
  CHECK(has_text(issued, "First") && !has_text(issued, "Second") && has_text(now, "Second"));
  CHECK(has_text(issued, "DEBURR") && !has_text(now, "DEBURR") && !has_text(issued, "LATER") && has_text(now, "LATER"));
  const std::string value = issue->def["values"][dim];
  CHECK(has_text(issued, value) && !has_text(now, value));
  // gc keeps the washer for revision A; without A it goes.
  Document copy = Document::parse(a.doc.serialize());
  a.doc.gc();
  CHECK(a.doc.has_body(washer));
  copy.append({{"op", "delete"}, {"target", issue->id}});
  copy.gc();
  CHECK(!copy.has_body(washer));
  const auto out = std::filesystem::temp_directory_path() / "opad-issued.svg";
  const json e = run(a.doc, "export", {{"format", "svg"}, {"out", out.string()}, {"sheet", a.sheet}, {"issue", "A"}});
  CHECK_EQ(e["issue"], "A");
  CHECK_EQ(e["sheet"]["items"], 4);
  CHECK_THROWS(run(a.doc, "export", {{"format", "svg"}, {"out", out.string()}, {"sheet", a.sheet}, {"issue", "Q"}}));
  std::filesystem::remove(out);
}

// A parts list measured in the scene as issued, rolled back and now, in either order, from one document: each its own BoM
// (the cache keys on the state the scene was replayed from, not on the document's).
TEST(parts_lists_as_issued_and_now) {
  Assembly a;
  const std::string washer = a.doc.add_body(brep_from_shape(BRepPrimAPI_MakeCylinder(5, 1).Shape()), {{"name", "Washer"}, {"units", "mm"}});
  const std::string placed = a.doc.append({{"op", "import"}, {"source", "design"}, {"parent", a.assembly}, {"nodes", {body("Washer", washer, 30, 20, 5)}}}).id;
  const std::string list = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "parts_list"}})["id"];
  const json issued = run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"freeze", false}});
  const std::string issue = issued["id"];
  a.doc.append({{"op", "delete"}, {"target", placed}});  // the washer gone since, a spacer added
  const std::string spacer = a.doc.add_body(brep_from_shape(BRepPrimAPI_MakeCylinder(4, 8).Shape()), {{"name", "Spacer"}, {"units", "mm"}});
  const std::string later = a.doc.append({{"op", "import"}, {"source", "design"}, {"parent", a.assembly}, {"nodes", {body("Spacer", spacer, 30, 30, 5)}}}).id;
  const auto names = [&](const Scene& s) {
    std::set<std::string> out;
    const json rows = parts_rows(a.doc, s, *s.sheet(a.sheet), s.sheet_item(list)->def)["rows"];
    for (const auto& r : rows) out.insert(r["name"].get<std::string>());
    return out;
  };
  const Scene now = a.scene();
  CHECK(!names(now).count("Washer") && names(now).count("Spacer"));  // the current state first
  const Scene then = issued_scene(a.doc, *now.sheet_item(issue));
  CHECK_EQ(names(then).size(), 4u);
  CHECK(names(then).count("Washer") && !names(then).count("Spacer"));
  CHECK_EQ(names(a.scene()).size(), 4u);  // and the current one again after it
  CHECK(names(a.scene()).count("Spacer"));
  const Scene back = resolve(a.doc, later);  // rolled back to before the spacer
  CHECK_EQ(names(back).size(), 3u);
  CHECK(!names(back).count("Spacer"));
  CHECK(names(resolve(a.doc)).count("Spacer"));
  CHECK(issued_scene(a.doc, *now.sheet_item(issue)).state != now.state && back.state != now.state && Scene().state.empty());
}

// The records' checks: item numbers, sheets, a balloon's list; the kinds are known and the outline names an issue by its
// revision.
TEST(records_are_checked) {
  Assembly a;
  const std::string list = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "parts_list"}})["id"];
  CHECK_THROWS(run(a.doc, "sheet_edit", {{"target", list}, {"set", {{"numbers", {{{"n", 0}}}}}}}));
  CHECK_THROWS(run(a.doc, "sheet_edit", {{"target", list}, {"set", {{"columns", "item"}}}}));
  CHECK_THROWS(run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"kind", "issue"}}));
  for (const char* k : {"parts_list", "balloon", "revision_table", "issue"}) CHECK(known_item(k, ""));
  run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"freeze", false}});
  const auto rows = outline(a.scene());
  bool named = false;
  for (const auto& sheet : rows)
    for (const auto& r : sheet.children.empty() ? std::vector<OutlineRow>{} : sheet.children[0].children)
      named = named || r.name == "A";
  for (const auto& sheet : rows)
    for (const auto& r : sheet.children) named = named || r.name == "A";
  CHECK(named);
  // Saved and loaded again: the same records.
  const Document again = Document::parse(a.doc.serialize());
  const Scene s = resolve(again);
  CHECK(s.unresolved.empty());
  CHECK_EQ(s.sheet_items.size(), 2u);
}

int main(int argc, char** argv) { return check::run_all(argc, argv); }

// Exploded-view drawings (UI-85): a base view of a saved exploded view draws the parts where the explode puts them, seen
// from its camera, with thin phantom trail lines from where they sit in the assembly; projected views follow it; a balloon
// on it points at the part where it is drawn; the view op names the exploded view only (the offsets are worked out where
// it is drawn), and one whose exploded view is gone cannot be drawn.
TEST(exploded_view_drawings) {
  Assembly a;
  run(a.doc, "explode", {{"mode", "axis"}, {"name", "Exploded"}});
  const std::string exploded = a.scene().views.back().id;
  CHECK(a.scene().views.back().explode.is_object());
  const json made = run(a.doc, "sheet_view", {{"sheet", a.sheet}, {"explode", exploded}, {"at", {300, 200}}});
  const std::string view = made["id"];
  const Scene s = a.scene();
  const SheetView* v = s.sheet_view(view);
  CHECK(v && v->error.empty());
  CHECK_EQ(v->def["source"]["explode"]["view"], exploded);
  CHECK_EQ(v->def["orient"]["view"], exploded);
  CHECK(!v->def.contains("offsets") && !v->def["source"].contains("offsets"));
  // Laid out where it is drawn: every pin and the bracket away from the plate, trail lines for those that moved.
  ViewSpec spec = view_spec(s, *v);
  CHECK(!spec.explode.is_null() && spec.offsets.empty());
  resolve_explode(a.doc, s, spec);
  const std::string pin3 = id_of(s, "Pin 3");
  CHECK(spec.offsets.count(pin3) && std::fabs(spec.offsets.at(pin3)[2]) > 1);
  CHECK(!spec.trails.empty());
  const auto bodies = view_bodies(s, spec);
  const auto moved = std::find_if(bodies.begin(), bodies.end(), [&](const auto& b) { return b.first == pin3; });
  CHECK(moved != bodies.end() && std::fabs(moved->second.at(2, 3) - (5 + spec.offsets.at(pin3)[2])) < 1e-9);
  const auto g = project(a.doc, s, view_spec(s, *v));
  const auto trails = std::count_if(g->curves.begin(), g->curves.end(), [](const Curve& c) { return c.kind == Curve::Kind::Trail; });
  CHECK(trails > 0 && trails <= static_cast<long>(spec.trails.size()));
  CHECK_EQ(project(a.doc, s, view_spec(s, *v))->fingerprint, g->fingerprint);  // cached as it is
  // Drawn on the sheet: the trail lines on their own thin phantom layer.
  const Display d = sheet_display(a.doc, s, *s.sheet(a.sheet));
  const auto layer = std::find_if(d.layers.begin(), d.layers.end(), [](const Layer& l) { return l.name == "Trail"; });
  CHECK(layer != d.layers.end() && layer->line == LineType::Phantom && layer->width < 0.3);
  const int trail = static_cast<int>(layer - d.layers.begin());
  CHECK(std::count_if(d.prims.begin(), d.prims.end(), [&](const Prim& p) { return p.layer == trail && p.source == view; }) == trails);
  // Frozen as linework (an issue): the trail lines come back as trail lines, the thin edges stay thin, and the drawing
  // as issued puts them on the Trail layer as the sheet did (they were frozen with the tangent edges and drawn solid).
  {
    const ViewGeometry frozen = frozen_geometry(shape_from_brep(linework_brep(*g)));
    auto length = [](const ViewGeometry& v, Curve::Kind kind, bool hidden) {
      double sum = 0;
      for (const auto& c : v.curves)
        if (c.kind == kind && c.hidden == hidden) sum += c.length();
      return sum;
    };
    CHECK(length(*g, Curve::Kind::Trail, false) > 1);
    CHECK_NEAR(length(frozen, Curve::Kind::Trail, false), length(*g, Curve::Kind::Trail, false), 1e-6);
    CHECK_NEAR(length(frozen, Curve::Kind::Tangent, false), length(*g, Curve::Kind::Tangent, false) + length(*g, Curve::Kind::Seam, false), 1e-3);
    // Break lines too (a broken-out section's), after the trails; the thin edges stay tangent.
    ViewGeometry kinds;
    for (const auto& [kind, x] : std::vector<std::pair<Curve::Kind, double>>{{Curve::Kind::Tangent, 0}, {Curve::Kind::Break, 10}, {Curve::Kind::Trail, 20}}) {
      Curve c;
      c.kind = kind;
      c.pts = {{x, 0}, {x, 5}};
      kinds.curves.push_back(c);
    }
    const ViewGeometry back = frozen_geometry(shape_from_brep(linework_brep(kinds)));
    CHECK_EQ(back.curves.size(), 3u);
    for (const auto& c : back.curves) {
      const Curve::Kind want = c.pts[0][0] < 5 ? Curve::Kind::Tangent : c.pts[0][0] < 15 ? Curve::Kind::Break : Curve::Kind::Trail;
      CHECK(c.kind == want && !c.hidden);
    }
    run(a.doc, "sheet_issue", {{"sheet", a.sheet}, {"description", "Exploded"}});
    const Scene now = a.scene();
    const SheetItem* issue = find_issue(now, *now.sheet(a.sheet), "A");
    CHECK(issue && issue->def.value("frozen", json::object()).contains(view));
    const Display issued = issued_display(a.doc, issued_scene(a.doc, *issue), *now.sheet(a.sheet), *issue);
    const auto at = std::find_if(issued.layers.begin(), issued.layers.end(), [](const Layer& l) { return l.name == "Trail"; });
    CHECK(at != issued.layers.end() && at->line == LineType::Phantom);
    const int issuedTrail = static_cast<int>(at - issued.layers.begin());
    CHECK(std::count_if(issued.prims.begin(), issued.prims.end(), [&](const Prim& p) { return p.layer == issuedTrail && p.source == view; }) >= trails);
  }
  // A projected view of it is exploded too.
  const json side = run(a.doc, "sheet_view", {{"sheet", a.sheet}, {"parent", view}, {"side", "right"}});
  const Scene s2 = a.scene();
  ViewSpec projected = view_spec(s2, *s2.sheet_view(side["id"]));
  resolve_explode(a.doc, s2, projected);
  CHECK(projected.offsets == spec.offsets);
  // A balloon on Pin 3 in the exploded view points at it where it is drawn: against the same balloon in a view from the same
  // camera that is not exploded, its tip (back in view coordinates) is moved by the pin's offset.
  const json plain = run(a.doc, "sheet_view", {{"sheet", a.sheet}, {"orient", exploded}, {"at", {120, 200}}});
  const json ref = {Ref{pin3, Ref::Kind::Edge, 0}.str()};
  const std::string onExploded = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"view", view}, {"kind", "balloon"}, {"refs", ref}})["id"];
  const std::string onPlain = run(a.doc, "sheet_item", {{"sheet", a.sheet}, {"view", plain["id"]}, {"kind", "balloon"}, {"refs", ref}})["id"];
  const Scene s3 = a.scene();
  const auto frames = layout(a.doc, s3, *s3.sheet(a.sheet));
  const auto viewed = [&](const std::string& item, const std::string& id) {  // the tip in the view's coordinates (model mm)
    const auto f = std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& x) { return x.id == id; });
    const json tip = measure_item(a.doc, s3, *s3.sheet(a.sheet), *s3.sheet_item(item), &*f)["tip"];
    return Vec2{tip[0].get<double>() / f->scale + f->centre[0], tip[1].get<double>() / f->scale + f->centre[1]};
  };
  const Vec2 drawn = viewed(onExploded, view), still = viewed(onPlain, plain["id"]);
  const Vec3 o = spec.offsets.at(pin3);
  Vec3 x, y, z;
  view_axes(spec, x, y, z);
  const Vec2 want{o[0] * x[0] + o[1] * x[1] + o[2] * x[2], o[0] * y[0] + o[1] * y[1] + o[2] * y[2]};
  CHECK(std::hypot(want[0], want[1]) > 5);  // the explode moves it visibly
  CHECK(std::hypot(drawn[0] - still[0] - want[0], drawn[1] - still[1] - want[1]) < 1e-6);
  // The exploded view deleted: the drawing view says why it cannot be drawn.
  run(a.doc, "delete", {{"target", exploded}});
  const Scene s4 = a.scene();
  CHECK(!s4.sheet_view(view)->error.empty());
}

// Holes on an exploded view (UI-85): the hole table and a hole callout find a hole where the view draws its part, as balloons
// and dimensions do. A washer with a 5 mm hole on a base, exploded along X, seen from the top beside the same view not
// exploded: the hole's place in each, back in view coordinates, differs by the washer's offset.
TEST(exploded_view_holes) {
  Document doc = Document::create();
  const std::string base = doc.add_body(brep_from_shape(BRepPrimAPI_MakeBox(60, 40, 5).Shape()), {{"name", "Base"}, {"units", "mm"}});
  const TopoDS_Shape holed = BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(20, 20, 3).Shape(), BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(10, 10, -1), gp::DZ()), 2.5, 5).Shape()).Shape();
  const std::string washer = doc.add_body(brep_from_shape(holed), {{"name", "Washer"}, {"units", "mm"}});
  doc.append({{"op", "import"},
              {"source", "design"},
              {"nodes", {{{"type", "component"}, {"id", new_uuid()}, {"name", "Stack"}, {"children", {body("Base", base), body("Washer", washer, 20, 10, 5)}}}}}});
  run(doc, "explode", {{"mode", "axis"}, {"axis", {1, 0, 0}}, {"name", "Apart"}});
  const std::string exploded = resolve(doc).views.back().id;
  const json made = run(doc, "sheet", {{"size", "A3"}, {"views", {"top"}}});
  const std::string sheet = made["id"], plain = made["views"][0]["id"];
  const std::string view = run(doc, "sheet_view", {{"sheet", sheet}, {"explode", exploded}, {"orient", "top"}, {"at", {300, 200}}})["id"];
  const std::string tableOnExploded = run(doc, "sheet_item", {{"sheet", sheet}, {"view", view}, {"kind", "hole_table"}})["id"];
  const std::string tableOnPlain = run(doc, "sheet_item", {{"sheet", sheet}, {"view", plain}, {"kind", "hole_table"}})["id"];
  const Scene s = resolve(doc);
  const std::string node = id_of(s, "Washer");
  ViewSpec spec = view_spec(s, *s.sheet_view(view));
  resolve_explode(doc, s, spec);
  CHECK(spec.offsets.count(node) && std::fabs(spec.offsets.at(node)[0]) > 5);  // the washer moves along X
  // Its trail line runs from where it was to where it is drawn, seen from the top: the stretches inside the washer (at its
  // old place and at its new one) are hidden, the gap between them is drawn.
  {
    double full = 0, drawn = 0;
    for (const auto& [from, to] : spec.trails) full += std::hypot(to[0] - from[0], to[1] - from[1]);
    const auto g = project(doc, s, view_spec(s, *s.sheet_view(view)));
    for (const Curve& c : g->curves)
      if (c.kind == Curve::Kind::Trail) drawn += std::hypot(c.pts[1][0] - c.pts[0][0], c.pts[1][1] - c.pts[0][1]);
    CHECK(full > 5 && drawn > 1 && drawn < full - 5);
  }
  const auto frames = layout(doc, s, *s.sheet(sheet));
  const auto frame = [&](const std::string& id) { return &*std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& f) { return f.id == id; }); };
  const auto toView = [&](const json& local, const ViewFrame* f) {  // paper mm from the frame's place -> view coordinates
    return Vec2{local[0].get<double>() / f->scale + f->centre[0], local[1].get<double>() / f->scale + f->centre[1]};
  };
  const auto rows = [&](const std::string& item, const std::string& id) { return measure_item(doc, s, *s.sheet(sheet), *s.sheet_item(item), frame(id))["rows"]; };
  const json drawnRows = rows(tableOnExploded, view), stillRows = rows(tableOnPlain, plain);
  CHECK_EQ(drawnRows.size(), size_t(1));
  CHECK_EQ(stillRows.size(), size_t(1));
  Vec3 x, y, z;
  view_axes(spec, x, y, z);
  const Vec3 o = spec.offsets.at(node);
  const Vec2 want{o[0] * x[0] + o[1] * x[1] + o[2] * x[2], o[0] * y[0] + o[1] * y[1] + o[2] * y[2]};
  CHECK(std::hypot(want[0], want[1]) > 5);
  const Vec2 drawn = toView(drawnRows[0]["centre"], frame(view)), still = toView(stillRows[0]["centre"], frame(plain));
  CHECK(std::hypot(drawn[0] - still[0] - want[0], drawn[1] - still[1] - want[1]) < 1e-6);
  // A hole callout on its wall: the same (the washer is placed by a transform: the callout finds its hole too).
  int wall = -1;
  for (int i = 0; i < 12 && wall < 0; ++i) {
    const json ref = {Ref{node, Ref::Kind::Face, i}.str()};
    try {
      const std::string a = run(doc, "sheet_item", {{"sheet", sheet}, {"view", view}, {"kind", "hole_callout"}, {"refs", ref}})["id"];
      const std::string b = run(doc, "sheet_item", {{"sheet", sheet}, {"view", plain}, {"kind", "hole_callout"}, {"refs", ref}})["id"];
      const Scene t = resolve(doc);
      const auto framesNow = layout(doc, t, *t.sheet(sheet));
      auto frameNow = [&](const std::string& id) { return &*std::find_if(framesNow.begin(), framesNow.end(), [&](const ViewFrame& f) { return f.id == id; }); };
      const json ma = measure_item(doc, t, *t.sheet(sheet), *t.sheet_item(a), frameNow(view));
      const json mb = measure_item(doc, t, *t.sheet(sheet), *t.sheet_item(b), frameNow(plain));
      const Vec2 ca = toView(ma["centre"], frameNow(view)), cb = toView(mb["centre"], frameNow(plain));
      CHECK(std::hypot(ca[0] - cb[0] - want[0], ca[1] - cb[1] - want[1]) < 1e-6);
      wall = i;
    } catch (const Error&) {  // not the hole's wall
    }
  }
  CHECK(wall >= 0);
}

// A base view's state (UI-85): sheet_edit's explode turns a view drawn assembled into one of a saved exploded view, from
// the same side, and back; the views projected from it follow and other views cannot take one. The view keeps only the
// exploded view's id, so an update of that exploded view moves the drawing's parts and trail lines with it. Balloons placed
// on it by sheet_balloons point at their parts where they are drawn, numbered as the parts list numbers them, and the
// sheet's DXF carries the trail lines on their phantom layer.
TEST(exploded_view_state_follows_its_exploded_view) {
  Assembly a;
  run(a.doc, "explode", {{"mode", "axis"}, {"name", "Exploded"}});
  const std::string exploded = a.scene().views.back().id;
  const std::string pin3 = id_of(a.scene(), "Pin 3");
  const auto spec_of_front = [&](Scene& s) {
    s = a.scene();
    ViewSpec spec = view_spec(s, *s.sheet_view(a.front));
    resolve_explode(a.doc, s, spec);
    return spec;
  };
  Scene s;
  const ViewSpec assembled = spec_of_front(s);
  const auto plain = view_extent(a.doc, s, assembled);
  CHECK(assembled.offsets.empty() && assembled.trails.empty());
  const json side = run(a.doc, "sheet_view", {{"sheet", a.sheet}, {"parent", a.front}, {"side", "right"}});
  // Exploded: the same front view, the parts apart.
  run(a.doc, "sheet_edit", {{"target", a.front}, {"set", {{"explode", exploded}}}});
  ViewSpec spec = spec_of_front(s);
  const SheetView* v = s.sheet_view(a.front);
  CHECK(v->error.empty() && v->def["source"]["explode"]["view"] == exploded && v->def["orient"]["preset"] == "front");
  CHECK(spec.offsets.count(pin3) && !spec.trails.empty());
  Vec3 x, y, z;
  view_axes(spec, x, y, z);
  CHECK(std::fabs(z[1] + 1) < 1e-9);  // still seen from the front
  const auto apart = view_extent(a.doc, s, spec);
  CHECK(apart[3] - apart[1] > plain[3] - plain[1] + 10);  // taller: the pins lifted off the plate
  ViewSpec projected = view_spec(s, *s.sheet_view(side["id"]));
  resolve_explode(a.doc, s, projected);
  CHECK(projected.offsets == spec.offsets);
  CHECK_THROWS(run(a.doc, "sheet_edit", {{"target", side["id"]}, {"set", {{"explode", exploded}}}}));  // a projected view follows its parent
  CHECK_THROWS(run(a.doc, "sheet_edit", {{"target", a.front}, {"set", {{"explode", a.front}}}}));     // not an exploded view
  // The exploded view updated (twice the spacing): the drawing follows, its fingerprint with it.
  const std::string before = projection_fingerprint(a.doc, s, view_spec(s, *v), Quality::Auto);
  run(a.doc, "explode", {{"view", exploded}, {"spacing", 2.5}, {"update", true}});
  const ViewSpec wider = spec_of_front(s);
  CHECK(std::fabs(wider.offsets.at(pin3)[2]) > std::fabs(spec.offsets.at(pin3)[2]) + 5);
  CHECK(projection_fingerprint(a.doc, s, view_spec(s, *s.sheet_view(a.front)), Quality::Auto) != before);
  CHECK(view_extent(a.doc, s, wider)[3] > apart[3] + 5);
  // Auto-balloons on it: on each part where it is drawn, numbered from the parts list it made.
  const json balloons = run(a.doc, "sheet_balloons", {{"sheet", a.sheet}, {"view", a.front}});
  CHECK(balloons["created"] == true && balloons["ids"].size() >= 3);
  s = a.scene();
  const auto frames = layout(a.doc, s, *s.sheet(a.sheet));
  const ViewFrame& f = *std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& fr) { return fr.id == a.front; });
  const auto g = project(a.doc, s, view_spec(s, *s.sheet_view(a.front)));
  CHECK(std::any_of(g->curves.begin(), g->curves.end(), [](const Curve& c) { return c.kind == Curve::Kind::Trail; }));
  const json rows = parts_rows(a.doc, s, *s.sheet(a.sheet), s.sheet_item(balloons["list"])->def)["rows"];
  for (const auto& id : balloons["ids"]) {
    const SheetItem& b = *s.sheet_item(id.get<std::string>());
    const json m = measure_item(a.doc, s, *s.sheet(a.sheet), b, &f);
    const Vec2 tip{m["tip"][0].get<double>() / f.scale + f.centre[0], m["tip"][1].get<double>() / f.scale + f.centre[1]};
    double nearest = 1e9;
    for (const Curve& c : g->curves) {
      if (c.body < 0 || g->bodies[size_t(c.body)].node != b.refs[0].body) continue;
      const auto pts = c.sample(0.01);
      for (size_t k = 1; k < pts.size(); ++k) {
        const Vec2 d{pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]};
        const double l2 = d[0] * d[0] + d[1] * d[1];
        const double t = l2 > 0 ? std::clamp(((tip[0] - pts[k - 1][0]) * d[0] + (tip[1] - pts[k - 1][1]) * d[1]) / l2, 0.0, 1.0) : 0;
        nearest = std::min(nearest, std::hypot(pts[k - 1][0] + t * d[0] - tip[0], pts[k - 1][1] + t * d[1] - tip[1]));
      }
    }
    CHECK(nearest < 0.05);  // on its part's linework as drawn apart
    const json* row = row_of(s, rows, b.refs[0].body);
    CHECK(row && m["number"] == std::to_string(row->value("number", 0)));
  }
  // The sheet as DXF: the trail lines on the Trail layer, drawn PHANTOM.
  const auto dxf = std::filesystem::temp_directory_path() / (new_uuid() + ".dxf");
  run(a.doc, "export", {{"format", "dxf"}, {"sheet", a.sheet}, {"out", dxf.string()}});
  const std::string text = read_text_file(dxf);
  std::filesystem::remove(dxf);
  const size_t layer = text.find("  2\nTrail\n");
  CHECK(layer != std::string::npos && text.find("  6\nPHANTOM\n", layer) < text.find("  0\n", layer + 1));
  size_t on = 0;
  for (size_t at = text.find("  8\nTrail\n"); at != std::string::npos; at = text.find("  8\nTrail\n", at + 1)) ++on;
  CHECK(on > 0);
  // Back to assembled: no explode left in the view, no trail lines.
  run(a.doc, "sheet_edit", {{"target", a.front}, {"set", {{"explode", nullptr}}}});
  const ViewSpec back = spec_of_front(s);
  CHECK(!s.sheet_view(a.front)->def.value("source", json::object()).contains("explode"));
  CHECK(back.offsets.empty() && back.trails.empty());
}

namespace {

// Whether a-b crosses c-d, also through an end of c-d (where the next piece of a sampled curve starts).
bool segments_cross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  const auto side = [](Vec2 o, Vec2 p, Vec2 q) { return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0]); };
  const double d1 = side(c, d, a), d2 = side(c, d, b), d3 = side(a, b, c), d4 = side(a, b, d);
  return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && (d3 != 0 || d4 != 0) && ((d3 >= 0 && d4 <= 0) || (d3 <= 0 && d4 >= 0));
}

// Each balloon of a view as drawn: where its circle is, where its leader ends, the part it points at (sheet paper mm).
struct Drawn {
  Vec2 at, tip;
  std::string node;
};
std::vector<Drawn> balloons_drawn(const Document& doc, const Scene& s, const Sheet& sheet, const ViewFrame& f, const json& ids) {
  std::vector<Drawn> out;
  for (const auto& id : ids) {
    const SheetItem& b = *s.sheet_item(id.get<std::string>());
    const json m = measure_item(doc, s, sheet, b, &f);
    out.push_back({{f.at[0] + b.def["place"]["text"][0].get<double>(), f.at[1] + b.def["place"]["text"][1].get<double>()},
                   {f.at[0] + m["tip"][0].get<double>(), f.at[1] + m["tip"][1].get<double>()},
                   b.refs[0].body});
  }
  return out;
}

// A view's visible lines on the sheet (paper mm) in short pieces, each with the part it draws ("" for a trail line).
struct Piece {
  Vec2 a, b;
  std::string node;
};
std::vector<Piece> pieces_shown(const ViewGeometry& g, const ViewFrame& f) {
  std::vector<Piece> out;
  const auto paper = [&](Vec2 p) { return Vec2{f.at[0] + f.scale * (p[0] - f.centre[0]), f.at[1] + f.scale * (p[1] - f.centre[1])}; };
  for (const Curve& c : g.curves) {
    if (c.hidden) continue;
    const auto pts = c.sample(0.01);
    for (size_t k = 1; k < pts.size(); ++k) out.push_back({paper(pts[k - 1]), paper(pts[k]), c.body < 0 ? "" : g.bodies[size_t(c.body)].node});
  }
  return out;
}

double distance_to(Vec2 p, const Piece& s) {
  const Vec2 d{s.b[0] - s.a[0], s.b[1] - s.a[1]};
  const double l2 = d[0] * d[0] + d[1] * d[1];
  const double t = l2 > 0 ? std::clamp(((p[0] - s.a[0]) * d[0] + (p[1] - s.a[1]) * d[1]) / l2, 0.0, 1.0) : 0;
  return std::hypot(s.a[0] + t * d[0] - p[0], s.a[1] + t * d[1] - p[1]);
}

// The parts' lines a balloon's leader crosses (from its circle to a millimetre short of its tip): of other parts, of its
// own (each line once).
std::pair<int, int> leader_crossings(const std::vector<Piece>& shown, const Drawn& b, double radius = 5) {
  const double l = std::hypot(b.tip[0] - b.at[0], b.tip[1] - b.at[1]);
  const Vec2 u{(b.tip[0] - b.at[0]) / l, (b.tip[1] - b.at[1]) / l};
  const Vec2 from{b.at[0] + u[0] * radius, b.at[1] + u[1] * radius}, to{b.tip[0] - u[0], b.tip[1] - u[1]};
  int others = 0, own = 0;
  for (const Piece& s : shown)
    if (!s.node.empty() && segments_cross(from, to, s.a, s.b)) ++(s.node == b.node ? own : others);
  return {others, own};
}

}  // namespace

// Auto-balloon on an exploded stack (UI-85): a plate, a post and a lid apart along Z, seen from the exploded view's
// camera. Every balloon's leader ends on its part's visible lines and reaches it over nothing drawn (the post's used to
// run up across the plate below it), and no two leaders cross; numbered as the parts list numbers them.
TEST(auto_balloon_leaders_reach_their_parts_over_nothing_else) {
  Document doc = Document::create();
  run(doc, "feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "8 mm"}}}});
  run(doc, "feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"origin", {0, 0, 8}}, {"normal", {0, 0, 1}}}}, {"diameter", 16}, {"height", 24}, {"operation", "new"}}}});
  run(doc, "feature", {{"kind", "box"}, {"inputs", {{"plane", {{"origin", {0, 0, 32}}, {"normal", {0, 0, 1}}}}, {"length", "60 mm"}, {"width", "40 mm"}, {"height", "4 mm"}, {"operation", "new"}}}});
  run(doc, "explode", {{"mode", "axis"}, {"name", "Exploded 1"}});
  const std::string exploded = resolve(doc).views.back().id;
  const std::string sheet = run(doc, "sheet", {{"size", "A3"}, {"orientation", "landscape"}, {"scale", "1:1"}, {"views", {"front"}}})["id"];
  for (const char* orient : {"", "front"}) {  // its own camera (iso), then from the front
    json args = {{"sheet", sheet}, {"explode", exploded}, {"at", orient[0] ? json::array({90, 150}) : json::array({310.8, 163.35})}};
    if (orient[0]) args["orient"] = orient;
    const std::string view = run(doc, "sheet_view", args)["id"];
    const json made = run(doc, "sheet_balloons", {{"sheet", sheet}, {"view", view}});
    CHECK_EQ(made["ids"].size(), 3u);
    const Scene s = resolve(doc);
    const auto frames = layout(doc, s, *s.sheet(sheet));
    const ViewFrame& f = *std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& x) { return x.id == view; });
    const auto shown = pieces_shown(*shape_linework(project(doc, s, view_spec(s, *s.sheet_view(view))), f), f);
    const auto drawn = balloons_drawn(doc, s, *s.sheet(sheet), f, made["ids"]);
    const json rows = parts_rows(doc, s, *s.sheet(sheet), s.sheet_item(made["list"])->def)["rows"];
    for (const Drawn& b : drawn) {
      double nearest = 1e9;  // the tip on its part's visible lines
      for (const Piece& p : shown)
        if (p.node == b.node) nearest = std::min(nearest, distance_to(b.tip, p));
      CHECK(nearest < 0.05);
      const auto [others, own] = leader_crossings(shown, b);
      CHECK_EQ(others, 0);
      CHECK_EQ(own, 0);
      CHECK(row_of(s, rows, b.node));
    }
    for (size_t i = 0; i < drawn.size(); ++i)
      for (size_t j = i + 1; j < drawn.size(); ++j) {
        CHECK(!segments_cross(drawn[i].at, drawn[i].tip, drawn[j].at, drawn[j].tip));
        CHECK(std::hypot(drawn[i].at[0] - drawn[j].at[0], drawn[i].at[1] - drawn[j].at[1]) >= 10);
      }
  }
}
