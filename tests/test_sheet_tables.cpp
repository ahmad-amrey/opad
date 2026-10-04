// Parts lists, balloons, revision tables and issued revisions (TODO 11 UI-84): rows from the bill of materials with item
// numbers that stay with their parts while parts come and go, renumbering, balloons numbered from the list (one placed by
// hand settles a new row's number; auto-balloon places one per row around the view), the drawing's issues in a revision
// table and the title block, what an issue keeps (values, fingerprints, frozen linework in the body store) and reports as
// changed afterwards, and the records' checks.
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>

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
