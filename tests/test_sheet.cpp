// Drawing sheets (TODO 11 UI-76): the sheet, sheet_view, sheet_item and properties ops, replay, layout with first and
// third angle projection, dimension values that follow the model, deletes, gc, forward compatibility and the sheet drawn
// for the writers.
#include <BRepPrimAPI_MakeBox.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {

json run(Document& doc, const std::string& command, const json& args) { return commands::run(command, args, &doc); }

// A 60 x 40 x 10 plate centred on the origin (z 0..10) with a 10 mm hole through its middle.
struct Plate {
  Document doc = Document::create();
  std::string box, body, sheet, front;
  Plate() {
    const json made = run(doc, "feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}});
    box = made["feature_id"];
    body = made["body_ids"][0];
    run(doc, "feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"base", "xy"}}}, {"x", 0}, {"y", 0}, {"diameter", 10}, {"height", 10}, {"operation", "cut"}, {"targets", {body}}}}});
    sheet = run(doc, "sheet", {{"size", "A4"}})["id"];
    front = run(doc, "sheet_view", {{"sheet", sheet}, {"orient", "front"}, {"at", {80, 170}}})["id"];
  }
  // The body's edge through `at` running along `dir` (ordinals change when the topology does).
  std::string edge(const Vec3& at, const Vec3& dir) {
    const Scene s = resolve(doc);
    const TopoDS_Shape shape = node_world_shape(doc, s, body);
    for (int i = 0; i < subshape_count(shape, Ref::Kind::Edge); ++i) {
      Ref r{body, Ref::Kind::Edge, i};
      const json e = inspect_ref(doc, s, r);
      if (!e.contains("direction")) continue;
      const auto c = e["bbox"]["center"], d = e["direction"];
      if (std::hypot(std::hypot(c[0].get<double>() - at[0], c[1].get<double>() - at[1]), c[2].get<double>() - at[2]) < 1e-6 &&
          std::fabs(std::fabs(d[0].get<double>() * dir[0] + d[1].get<double>() * dir[1] + d[2].get<double>() * dir[2]) - 1) < 1e-9)
        return r.str();
    }
    throw Error("no such edge");
  }
  std::string circle(double z) {
    const Scene s = resolve(doc);
    const TopoDS_Shape shape = node_world_shape(doc, s, body);
    for (int i = 0; i < subshape_count(shape, Ref::Kind::Edge); ++i) {
      Ref r{body, Ref::Kind::Edge, i};
      const json e = inspect_ref(doc, s, r);
      if (e.value("curve", "") == "circle" && std::fabs(e["center"][2].get<double>() - z) < 1e-6) return r.str();
    }
    throw Error("no circle");
  }
};

const ViewFrame& frame(const std::vector<ViewFrame>& frames, const std::string& id) {
  for (const auto& f : frames)
    if (f.id == id) return f;
  throw Error("no frame");
}

bool near3(const Vec3& a, const Vec3& b) { return std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]) < 1e-9; }

bool has_unresolved(const Scene& s, const std::string& id, const std::string& words) {
  for (const auto& u : s.unresolved)
    if (u.op_id == id && u.reason.find(words) != std::string::npos) return true;
  return false;
}

}  // namespace

TEST(sheet_paper_and_scales) {
  const json a3 = paper_size("a3");
  CHECK_EQ(a3["preset"], "A3");
  CHECK_EQ(a3["w"].get<double>(), 420);
  CHECK_EQ(a3["h"].get<double>(), 297);
  CHECK_EQ(paper_size("ANSI B", false)["h"].get<double>(), 431.8);
  CHECK_EQ(paper_size("C")["preset"], "ANSI-C");
  CHECK_THROWS(paper_size("A7"));
  CHECK_NEAR(parse_scale("1:2"), 0.5, 1e-12);
  CHECK_NEAR(parse_scale("5:1"), 5, 1e-12);
  CHECK_NEAR(parse_scale("1:2.5"), 0.4, 1e-12);
  CHECK_THROWS(parse_scale("2"));
  CHECK_THROWS(parse_scale("1:0"));
  CHECK_THROWS(parse_scale("a:b"));
  CHECK_EQ(scale_text(0.5), "1:2");
  CHECK_EQ(scale_text(2), "2:1");
  CHECK_EQ(scale_text(1), "1:1");
  CHECK_EQ(fit_scale(600, 300, 160, 120), 0.2);
  CHECK_EQ(fit_scale(6, 3, 160, 120), 20);
  CHECK_EQ(fit_scale(0, 0, 160, 120), 1);
}

// The loader checks structure; a kind, side or orientation this build does not know is kept, reported and written back
// byte for byte, so a later build's drawings still open here.
TEST(sheet_records_checked_and_forward_compatible) {
  const std::string s = new_uuid(), v = new_uuid();
  CHECK_THROWS(Document::validate_op({{"op", "sheet"}, {"name", "S"}}));                                   // no size
  CHECK_THROWS(Document::validate_op({{"op", "sheet"}, {"name", "S"}, {"size", {{"w", -1}, {"h", 2}}}}));
  CHECK_THROWS(Document::validate_op({{"op", "sheet_view"}, {"sheet", "nope"}, {"kind", "base"}}));
  CHECK_THROWS(Document::validate_op({{"op", "sheet_view"}, {"sheet", s}, {"kind", "base"}, {"at", {1}}}));
  CHECK_THROWS(Document::validate_op({{"op", "sheet_item"}, {"sheet", s}}));                                 // no kind
  CHECK_THROWS(Document::validate_op({{"op", "sheet_item"}, {"sheet", s}, {"kind", "dimension"}, {"refs", {{{"kind", "edge"}}}}}));
  CHECK_THROWS(Document::validate_op({{"op", "properties"}, {"target", s}, {"set", json::object()}}));
  CHECK_THROWS(Document::validate_op({{"op", "properties"}, {"target", s}, {"set", {{"mass", {1, 2}}}}}));
  CHECK_THROWS(Document::validate_op({{"op", "sheet"}, {"name", "S"}, {"size", {{"w", 1}, {"h", 1}}}, {"template", {{"geometry", "abc"}}}}));

  const std::string text = "#opad 2\n" + json{{"uuid", new_uuid()}, {"units", "mm"}, {"created", ""}, {"generator", ""}}.dump() + "\n#ops\n"
      "{\"op\":\"sheet\",\"id\":\"" + s + "\",\"ts\":\"\",\"by\":\"\",\"name\":\"S\",\"size\":{\"w\":420,\"h\":297},\"standard\":\"jis\",\"scale\":\"1:1\"}\n"
      "{\"op\":\"sheet_view\",\"id\":\"" + v + "\",\"ts\":\"\",\"by\":\"\",\"sheet\":\"" + s + "\",\"kind\":\"section\",\"cut\":[[0,0],[1,1]]}\n"
      "{\"op\":\"sheet_item\",\"id\":\"" + new_uuid() + "\",\"ts\":\"\",\"by\":\"\",\"sheet\":\"" + s + "\",\"view\":\"" + v + "\",\"kind\":\"hole_callout\"}\n"
      "{\"op\":\"sheet_item\",\"id\":\"" + new_uuid() + "\",\"ts\":\"\",\"by\":\"\",\"sheet\":\"" + s + "\",\"kind\":\"dimension\",\"type\":\"ordinate\"}\n"
      "#bodies\n";
  const Document doc = Document::parse(text);
  CHECK_EQ(doc.serialize(), text);
  const Scene scene = resolve(doc);
  CHECK_EQ(scene.sheets.size(), 1u);
  CHECK_EQ(scene.sheets[0].views.size(), 1u);
  CHECK_EQ(scene.sheets[0].items.size(), 2u);
  CHECK(has_unresolved(scene, s, "needs a newer OPAD (standard 'jis')"));
  CHECK(has_unresolved(scene, v, "needs a newer OPAD (sheet_view kind 'section')"));
  CHECK_EQ(scene.unresolved.size(), 4u);
  for (const auto& u : scene.unresolved) CHECK(u.reason.find("needs a newer OPAD") != std::string::npos);
}

// First angle puts the view of the left side right of the front view and the top view below it; third angle the other
// way round. Projected views stay aligned with their parent when it moves.
TEST(sheet_views_first_and_third_angle) {
  Plate p;
  const std::string right = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "right"}})["id"];
  const std::string below = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "bottom"}, {"gap", 15}})["id"];
  const std::string iso = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "top-right"}})["id"];
  Scene s = resolve(p.doc);
  CHECK(s.unresolved.empty());
  CHECK_EQ(s.sheet(p.sheet)->views.size(), 4u);
  CHECK_EQ(s.sheet_view(p.front)->children.size(), 3u);
  auto frames = layout(p.doc, s, *s.sheet(p.sheet));
  const ViewFrame& f = frame(frames, p.front);
  CHECK(near3(f.dir, {0, -1, 0}) && near3(f.x, {1, 0, 0}) && near3(f.y, {0, 0, 1}));
  CHECK_NEAR(f.box[0], 50, 1e-6);
  CHECK_NEAR(f.box[2], 110, 1e-6);
  CHECK_NEAR(f.box[1], 165, 1e-6);
  CHECK_NEAR(f.box[3], 175, 1e-6);
  const ViewFrame& r = frame(frames, right);
  CHECK(near3(r.dir, {-1, 0, 0}) && near3(r.y, {0, 0, 1}));  // the left side, its back towards the front view
  CHECK(near3(r.x, {0, -1, 0}));
  CHECK_NEAR(r.at[1], f.at[1], 1e-9);
  CHECK_NEAR(r.box[0] - f.box[2], 20, 1e-6);
  const ViewFrame& b = frame(frames, below);
  CHECK(near3(b.dir, {0, 0, 1}) && near3(b.x, {1, 0, 0}) && near3(b.y, {0, 1, 0}));  // the top, below
  CHECK_NEAR(f.box[1] - b.box[3], 15, 1e-6);
  CHECK_NEAR(b.at[0], f.at[0], 1e-9);
  const ViewFrame& i = frame(frames, iso);
  const double k = 1 / std::sqrt(3.0);
  CHECK(near3(i.dir, {k, -k, k}));
  // Moving the base view is one edit; the others follow.
  run(p.doc, "sheet_edit", {{"target", p.front}, {"set", {{"at", {100, 160}}}}});
  s = resolve(p.doc);
  frames = layout(p.doc, s, *s.sheet(p.sheet));
  CHECK_NEAR(frame(frames, right).at[1], 160, 1e-9);
  CHECK_NEAR(frame(frames, right).box[0] - frame(frames, p.front).box[2], 20, 1e-6);
  CHECK_NEAR(frame(frames, below).at[0], 100, 1e-9);
  // Third angle: the right side right of the front view, the top above it.
  run(p.doc, "sheet_edit", {{"target", p.sheet}, {"set", {{"projection", "third"}}}});
  s = resolve(p.doc);
  frames = layout(p.doc, s, *s.sheet(p.sheet));
  CHECK(near3(frame(frames, right).dir, {1, 0, 0}) && near3(frame(frames, right).x, {0, 1, 0}));
  CHECK(near3(frame(frames, below).dir, {0, 0, -1}) && near3(frame(frames, below).y, {0, -1, 0}));
  // A projected view takes its parent's scale; a base view its own or the sheet's.
  run(p.doc, "sheet_edit", {{"target", p.front}, {"set", {{"scale", "1:2"}}}});
  s = resolve(p.doc);
  frames = layout(p.doc, s, *s.sheet(p.sheet));
  CHECK_NEAR(frame(frames, right).scale, 0.5, 1e-12);
  CHECK_NEAR(frame(frames, p.front).box[2] - frame(frames, p.front).box[0], 30, 1e-6);
  // The view's projection is the projection engine's, from the view's definition.
  const auto g = project(p.doc, s, view_spec(s, *s.sheet_view(below)), {}, false);
  CHECK_EQ(g->counts()["arc"].get<int>(), 1);
  CHECK_THROWS(run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "behind"}}));
  CHECK_THROWS(run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"orient", "sideways"}}));
  CHECK_THROWS(run(p.doc, "sheet_edit", {{"target", p.front}, {"set", {{"parent", right}, {"kind", "projected"}, {"side", "left"}}}}));
  CHECK_THROWS(run(p.doc, "sheet_edit", {{"target", p.front}, {"set", {{"id", new_uuid()}}}}));
  CHECK_THROWS(run(p.doc, "sheet_edit", {{"target", p.sheet}, {"set", {{"scale", "big"}}}}));
}

// A dimension keeps the value it was made with; the value now follows parameter edits, also when the topology changed
// (the reference's hint finds the edge again).
TEST(sheet_dimensions_follow_the_model) {
  Plate p;
  const std::string top = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "bottom"}})["id"];
  const json width = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"type", "horizontal"}, {"refs", {p.edge({0, -20, 10}, {1, 0, 0})}}});
  CHECK_NEAR(width["result"]["value"].get<double>(), 60, 1e-9);
  CHECK_EQ(width["result"]["shown"], "60");
  const json height = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"type", "vertical"}, {"refs", {p.edge({-30, -20, 5}, {0, 0, 1})}}, {"precision", 1}});
  CHECK_NEAR(height["result"]["value"].get<double>(), 10, 1e-9);
  const json hole = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", top}, {"type", "diameter"}, {"refs", {p.circle(10)}}, {"tolerance", {{"type", "dev"}, {"plus", 0.1}, {"minus", 0}}}});
  CHECK_EQ(hole["result"]["shown"], "⌀10 +0.1/+0");
  const std::string center = p.circle(10);
  const json offset = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", top}, {"type", "horizontal"},
                                                {"refs", {p.edge({-30, 0, 10}, {0, 1, 0}), center.substr(0, center.find('/')) + "/center/" + center.substr(center.rfind('/') + 1)}}});
  CHECK_NEAR(offset["result"]["value"].get<double>(), 30, 1e-9);
  const json across = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", top}, {"type", "aligned"}, {"refs", {p.edge({-30, 0, 10}, {0, 1, 0}), p.edge({30, 0, 10}, {0, 1, 0})}}});
  CHECK_NEAR(across["result"]["value"].get<double>(), 60, 1e-9);
  const json corner = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", top}, {"type", "angle"}, {"refs", {p.edge({-30, 0, 10}, {0, 1, 0}), p.edge({0, 20, 10}, {1, 0, 0})}}});
  CHECK_NEAR(corner["result"]["value"].get<double>(), 90, 1e-9);
  const json start = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"type", "horizontal"}, {"refs", {p.edge({0, -20, 10}, {1, 0, 0}), p.circle(10)}},
                                               {"aspects", {"start", "center"}}, {"text", "<> TYP"}});
  CHECK_EQ(start["result"]["shown"], "30 TYP");
  run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"text", "BREAK SHARP EDGES"}});
  // Refused: the hole is foreshortened in the front view, a dimension needs its view, a radius takes a circle.
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"type", "diameter"}, {"refs", {p.circle(10)}}}));
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"type", "horizontal"}, {"refs", {p.circle(10)}}}));
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", top}, {"type", "radius"}, {"refs", {p.edge({0, 20, 10}, {1, 0, 0})}}}));
  const size_t ops = p.doc.ops.size();

  // Longer plate, and a second hole that renumbers the edges: the dimensions find their edges again.
  const std::string param_box = p.box;
  run(p.doc, "feature_edit", {{"target", param_box}, {"inputs", {{"length", "80 mm"}}}});
  run(p.doc, "feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"base", "xy"}}}, {"x", 25}, {"y", 0}, {"diameter", 6}, {"height", 10}, {"operation", "cut"}, {"targets", {p.body}}}}});
  CHECK(p.doc.ops.size() > ops);
  const json info = run(p.doc, "sheet_info", {{"sheet", p.sheet}});
  CHECK(info["unresolved"].empty());
  std::map<std::string, json> items;
  for (const auto& i : info["items"]) items[i["id"]] = i;
  CHECK_NEAR(items[width["id"]]["current"]["value"].get<double>(), 80, 1e-9);
  CHECK(items[width["id"]]["changed"].get<bool>());
  CHECK_NEAR(items[width["id"]]["result"]["value"].get<double>(), 60, 1e-9);
  CHECK(!items[height["id"]]["changed"].get<bool>());
  CHECK_EQ(items[height["id"]]["current"]["shown"], "10");
  CHECK_NEAR(items[offset["id"]]["current"]["value"].get<double>(), 40, 1e-9);
  CHECK_NEAR(items[across["id"]]["current"]["value"].get<double>(), 80, 1e-9);
  CHECK_NEAR(items[hole["id"]]["current"]["value"].get<double>(), 10, 1e-9);
  CHECK(items[hole["id"]]["current"].contains("rehinted"));  // the second hole renumbered the edges
  // Changing what a dimension measures gives it its value again.
  const json now = run(p.doc, "sheet_edit", {{"target", width["id"]}, {"set", {{"type", "aligned"}, {"precision", 0}}}});
  CHECK_NEAR(now["result"]["value"].get<double>(), 80, 1e-9);
  // The body gone: the dimension dangles, reported, never dropped.
  run(p.doc, "delete", {{"target", param_box}});
  const Scene gone = resolve(p.doc);
  CHECK(gone.sheet_item(width["id"])->unresolved);
  CHECK(has_unresolved(gone, width["id"], "does not exist"));
}

// A deleted sheet takes its views and items with it (not reported); restoring it brings them back. A deleted base view
// takes its projected views and their items.
TEST(sheet_deletes_cascade) {
  Plate p;
  const std::string right = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "right"}})["id"];
  const std::string next = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", right}, {"side", "right"}})["id"];
  run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", next}, {"type", "vertical"}, {"refs", {p.edge({-30, -20, 5}, {0, 0, 1})}}});
  run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"text", "NOTE"}});
  const std::string other = run(p.doc, "sheet", {{"name", "Parts"}})["id"];
  Scene s = resolve(p.doc);
  CHECK_EQ(s.sheets.size(), 2u);
  CHECK_EQ(s.sheets[1].drawing, "Drawing 1");
  CHECK_EQ(s.sheet_views.size(), 3u);
  CHECK_EQ(s.sheet_items.size(), 2u);
  const std::string del = run(p.doc, "delete", {{"target", p.front}})["id"];
  s = resolve(p.doc);
  CHECK(s.unresolved.empty());
  CHECK(s.sheet_views.empty());
  CHECK_EQ(s.sheet_items.size(), 1u);
  run(p.doc, "delete", {{"target", del}});
  s = resolve(p.doc);
  CHECK_EQ(s.sheet_views.size(), 3u);
  run(p.doc, "delete", {{"target", p.sheet}});
  s = resolve(p.doc);
  CHECK(s.unresolved.empty());
  CHECK(s.sheet_views.empty() && s.sheet_items.empty());
  CHECK_EQ(s.sheets.size(), 1u);
  CHECK_EQ(s.sheets[0].id, other);
  CHECK_THROWS(run(p.doc, "sheet_edit", {{"target", p.front}, {"set", {{"at", {1, 1}}}}}));
  // Never there: reported.
  p.doc.append({{"op", "sheet_view"}, {"sheet", new_uuid()}, {"kind", "base"}, {"orient", {{"preset", "top"}}}});
  CHECK_EQ(resolve(p.doc).unresolved.size(), 1u);
}

TEST(sheet_part_properties) {
  Plate p;
  run(p.doc, "part_properties", {{"target", p.body}, {"set", {{"part_number", "OP-1002"}, {"material", "PA12"}, {"bom", "include"}}}});
  run(p.doc, "part_properties", {{"targets", {p.body}}, {"set", {{"material", nullptr}, {"description", "Plate"}}}});
  const Scene s = resolve(p.doc);
  const json part = s.node(p.body)->properties;
  CHECK_EQ(part, (json{{"part_number", "OP-1002"}, {"bom", "include"}, {"description", "Plate"}}));
  CHECK_EQ(node_properties(p.doc, s, p.body, false)["part"], part);
  CHECK_EQ(s.tree_json()[0]["properties"], part);
  CHECK_THROWS(run(p.doc, "part_properties", {{"target", p.body}, {"set", {{"bom", "maybe"}}}}));
  CHECK_THROWS(run(p.doc, "part_properties", {{"target", new_uuid()}, {"set", {{"bom", "exclude"}}}}));
  CHECK_THROWS(run(p.doc, "part_properties", {{"target", p.body}, {"set", json::object()}}));
}

// gc keeps body entries a sheet names (a template's geometry, frozen linework) while the sheet lives.
TEST(sheet_gc_keeps_referenced_keys) {
  Document doc = Document::create();
  const std::string key = doc.add_body(brep_from_shape(BRepPrimAPI_MakeBox(1, 2, 3).Shape()), {{"name", "frame"}});
  const std::string sheet = doc.append({{"op", "sheet"}, {"name", "S"}, {"size", {{"w", 420}, {"h", 297}}}, {"template", {{"id", "own"}, {"geometry", key}}}}).id;
  CHECK(doc.gc().empty());
  CHECK(doc.has_body(key));
  CHECK(resolve(doc).unresolved.empty());
  doc.append({{"op", "delete"}, {"target", sheet}});
  CHECK_EQ(doc.gc().size(), 1u);
  CHECK(!doc.has_body(key));
  const std::string missing = doc.append({{"op", "sheet_item"}, {"sheet", doc.append({{"op", "sheet"}, {"name", "T"}, {"size", {{"w", 1}, {"h", 1}}}}).id},
                                          {"kind", "note"}, {"text", "x"}, {"frozen", {{"v", key}}}}).id;
  CHECK(has_unresolved(resolve(doc), missing, "missing from the body store"));
}

// Edits of sheet records are checked as the record they make; a sheet scaled to fit picks a standard scale.
TEST(sheet_edits_and_auto_scale) {
  Plate p;
  CHECK_THROWS(run(p.doc, "sheet_edit", {{"target", p.sheet}, {"set", {{"size", {{"w", "wide"}}}}}}));
  run(p.doc, "sheet_edit", {{"target", p.sheet}, {"set", {{"size", "A3"}, {"values", {{"title", "Plate"}}}}}});
  Scene s = resolve(p.doc);
  CHECK_EQ(s.sheet(p.sheet)->width, 420);
  CHECK_EQ(s.sheet(p.sheet)->def["values"]["title"], "Plate");
  const json big = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"orient", "top"}, {"scale", "auto"}, {"at", {300, 200}}});
  CHECK_EQ(big["frame"]["scale"], "2:1");  // 60 x 40 in 40% of A3 (168 x 119)
  CHECK_EQ(Document::parse(p.doc.serialize()).ops.size(), p.doc.ops.size());
  const json info = run(p.doc, "sheet_info", json::object());
  CHECK_EQ(info["sheets"].size(), 1u);
  CHECK_EQ(info["sheets"][0]["views"], 2);
  CHECK_EQ(document_info(p.doc, s)["sheets"], 1);
  // Items stay on their sheet's views; a record of a kind a later build made is not edited here.
  const std::string other = run(p.doc, "sheet", {{"name", "Other"}})["id"];
  const std::string there = run(p.doc, "sheet_view", {{"sheet", other}, {"orient", "top"}})["id"];
  const std::string note = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"text", "A"}})["id"];
  CHECK_THROWS(run(p.doc, "sheet_edit", {{"target", note}, {"set", {{"view", there}}}}));
  run(p.doc, "sheet_edit", {{"target", note}, {"set", {{"view", nullptr}, {"at", {20, 20}}}}});
  CHECK(resolve(p.doc).sheet_item(note)->view.empty());
  const std::string later = p.doc.append({{"op", "sheet_item"}, {"sheet", p.sheet}, {"kind", "balloon"}}).id;
  CHECK_THROWS(run(p.doc, "sheet_edit", {{"target", later}, {"set", {{"at", {1, 1}}}}}));
}

// The browser's Drawings folder: drawings > sheets > views with their items > the sheet's own items; unnamed views by the
// standard view they show (first angle: the view right of the front is the left side). Drawing records are deleted and
// restored without walking the design history.
// A sheet as a drawing (UI-86): the frame, the views where layout() puts them, dimensions as geometry with their values
// now, one that lost its body in magenta with the value it was made with, a note; written as SVG on the sheet's paper and
// as DXF by the export command, the sheet named by id or by name.
TEST(sheet_draws_as_a_drawing) {
  Plate p;
  const std::string top = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "bottom"}})["id"];
  run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "right"}});
  run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"type", "horizontal"}, {"refs", {p.edge({0, -20, 10}, {1, 0, 0})}}, {"place", {0, 15}}});
  run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", top}, {"type", "diameter"}, {"refs", {p.circle(10)}}, {"place", {15, 12}}});
  run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", top}, {"type", "angle"}, {"refs", {p.edge({-30, 0, 10}, {0, 1, 0}), p.edge({0, 20, 10}, {1, 0, 0})}},
                            {"place", {-24, 14}}});
  run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"text", "BREAK SHARP EDGES"}, {"at", {30, 30}}});
  p.doc.append({{"op", "sheet_item"}, {"sheet", p.sheet}, {"view", p.front}, {"kind", "dimension"}, {"type", "vertical"},
                {"refs", {new_uuid() + "/edge/0"}}, {"place", {{"text", {-45, 0}}}}, {"result", {{"value", 7}, {"shown", "7"}}}});
  const Scene scene = resolve(p.doc);
  json report;
  const Display d = sheet_display(p.doc, scene, *scene.sheet(p.sheet), {}, &report);
  CHECK_EQ(report["views"], 3);
  CHECK_EQ(report["items"], 4);
  CHECK_EQ(report["skipped"].size(), 1u);
  CHECK(d.paper == (std::array<double, 4>{0, 0, 297, 210}));
  const json counts = d.counts();
  CHECK(counts["layers"]["Frame"].get<int>() > 5 && counts["layers"]["Title block"].get<int>() > 20);  // the frame, its centring marks and the ISO title block
  CHECK(counts["layers"]["Visible"].get<int>() >= 12 && counts["layers"].value("Hidden", 0) == 0);
  std::map<std::string, const Prim*> texts;
  for (const auto& prim : d.prims)
    if (prim.kind == Prim::Kind::Text) texts[prim.text] = &prim;
  CHECK(texts.count("60") && texts.count("⌀10") && texts.count("90°") && texts.count("BREAK SHARP EDGES") && texts.count("7"));
  CHECK_NEAR(texts["60"]->at[0], 80, 1e-6);  // over the middle of the front view's top edge (50..110 on paper)
  CHECK(texts["60"]->at[1] > 180);
  CHECK_EQ(texts["7"]->rgb, 0xFF00FFu);
  CHECK_NEAR(texts["BREAK SHARP EDGES"]->at[0], 30, 1e-9);
  const auto frames = layout(p.doc, scene, *scene.sheet(p.sheet));
  for (const auto& prim : d.prims) {  // every visible line inside a view's frame
    if (d.layers[size_t(prim.layer)].name != "Visible") continue;
    bool inside = false;
    for (const auto& f : frames)
      inside = inside || std::all_of(prim.curve.pts.begin(), prim.curve.pts.end(), [&](const Vec2& q) {
        return q[0] > f.box[0] - 1e-6 && q[0] < f.box[2] + 1e-6 && q[1] > f.box[1] - 1e-6 && q[1] < f.box[3] + 1e-6;
      });
    CHECK(inside);
  }
  const auto dir = std::filesystem::temp_directory_path() / new_uuid();
  const json svg = run(p.doc, "export", {{"format", "svg"}, {"sheet", p.sheet}, {"out", (dir / "sheet.svg").string()}});
  CHECK_EQ(svg["sheet"]["views"], 3);
  CHECK_EQ(svg["skipped"].size(), 1u);
  const std::string text = read_text_file(dir / "sheet.svg");
  CHECK(text.find("width=\"297mm\" height=\"210mm\" viewBox=\"0 -210 297 210\"") != std::string::npos);
  const json dxf = run(p.doc, "export", {{"format", "dxf"}, {"sheet", "Sheet 1"}, {"out", (dir / "sheet.dxf").string()}});
  CHECK_EQ(dxf["sheet"]["id"], p.sheet);
  Document back = Document::create();
  import_file(back, dir / "sheet.dxf");
  std::set<std::string> layers;
  const Scene round = resolve(back);
  for (const auto& id : round.all_bodies()) layers.insert(round.node(id)->name);
  CHECK(layers.count("Frame") && layers.count("Visible") && layers.count("Dimensions") && layers.count("Text"));
  CHECK_THROWS(run(p.doc, "export", {{"format", "dxf"}, {"sheet", "Sheet 9"}, {"out", (dir / "none.dxf").string()}}));
  // A drawing of one sheet goes in any format; of two, only as PDF pages (a painter's: not in this build).
  run(p.doc, "sheet_edit", {{"target", p.sheet}, {"set", {{"drawing", "Plate"}}}});
  CHECK_EQ(run(p.doc, "export", {{"format", "svg"}, {"sheet", "drawing:Plate"}, {"out", (dir / "plate.svg").string()}})["sheet"]["views"], 3);
  run(p.doc, "sheet", {{"drawing", "Plate"}});
  CHECK_THROWS(run(p.doc, "export", {{"format", "svg"}, {"sheet", "drawing:Plate"}, {"out", (dir / "plate.svg").string()}}));
  CHECK_THROWS(run(p.doc, "export", {{"format", "pdf"}, {"sheet", "drawing:Plate"}, {"out", (dir / "plate.pdf").string()}}));
  std::error_code e;
  std::filesystem::remove_all(dir, e);
}

TEST(sheet_outline_for_the_browser) {
  Plate p;
  const std::string right = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "right"}})["id"];
  const std::string below = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "bottom"}})["id"];
  const std::string corner = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"parent", p.front}, {"side", "top-right"}})["id"];
  const std::string named = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"name", "Detail"}, {"dir", {1, 2, 3}}, {"at", {40, 40}}})["id"];
  const std::string width = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"type", "horizontal"}, {"refs", {p.edge({0, -20, 10}, {1, 0, 0})}}})["id"];
  const std::string note = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"text", "BREAK SHARP EDGES\nALL OVER"}})["id"];
  const std::string later = p.doc.append({{"op", "sheet_item"}, {"sheet", p.sheet}, {"view", below}, {"kind", "balloon"}}).id;
  const std::string housing = run(p.doc, "sheet", {{"name", "Cover"}, {"drawing", "Housing"}})["id"];
  const std::string second = run(p.doc, "sheet", {{"name", "Parts"}, {"drawing", "Drawing 1"}})["id"];
  const std::string loose = p.doc.append({{"op", "sheet"}, {"name", "Loose"}, {"size", {{"w", 297}, {"h", 210}}}}).id;
  run(p.doc, "part_properties", {{"target", p.body}, {"set", {{"material", "PA12"}}}});
  Scene s = resolve(p.doc);
  auto rows = outline(s);
  CHECK_EQ(rows.size(), 3u);
  CHECK(rows[0].id == "drawing:Drawing 1" && rows[0].kind == "drawing" && rows[0].name == "Drawing 1");
  CHECK_EQ(rows[0].children.size(), 2u);
  CHECK(rows[1].name == "Housing" && rows[1].children.size() == 1 && rows[1].children[0].id == housing);
  CHECK(rows[2].id == loose && rows[2].kind == "sheet" && rows[2].children.empty());
  const OutlineRow& sheet = rows[0].children[0];
  CHECK(sheet.id == p.sheet && sheet.kind == "sheet" && sheet.name == "Sheet 1" && sheet.error.empty());
  CHECK_EQ(rows[0].children[1].id, second);
  CHECK_EQ(sheet.children.size(), 6u);  // five views, then the note
  CHECK(sheet.children[0].id == p.front && sheet.children[0].orient == "front" && sheet.children[0].name.empty());
  CHECK(sheet.children[1].id == right && sheet.children[1].orient == "left");
  CHECK(sheet.children[2].id == below && sheet.children[2].orient == "top");
  CHECK(sheet.children[3].id == corner && sheet.children[3].orient == "iso");
  CHECK(sheet.children[4].id == named && sheet.children[4].name == "Detail" && sheet.children[4].orient.empty());
  CHECK(sheet.children[5].id == note && sheet.children[5].kind == "item" && sheet.children[5].name == "BREAK SHARP EDGES");
  CHECK(sheet.children[0].children.size() == 1 && sheet.children[0].children[0].id == width && sheet.children[0].children[0].name == "60");
  CHECK(sheet.children[2].children.size() == 1 && sheet.children[2].children[0].id == later);
  CHECK(sheet.children[2].children[0].error.find("needs a newer OPAD") != std::string::npos);
  run(p.doc, "sheet_edit", {{"target", p.sheet}, {"set", {{"projection", "third"}}}});
  CHECK_EQ(outline(resolve(p.doc))[0].children[0].children[1].orient, "right");
  CHECK(is_drawing_op("sheet") && is_drawing_op("sheet_view") && is_drawing_op("sheet_item") && is_drawing_op("properties"));
  CHECK(!is_drawing_op("feature") && !is_drawing_op("delete") && !is_drawing_op("edit"));
  // Deleting a view or restoring it is one op, never a walk of the features (no regeneration report).
  const size_t ops = p.doc.ops.size();
  const json gone = run(p.doc, "delete", {{"target", p.front}});
  CHECK(!gone.contains("regenerated") && p.doc.ops.size() == ops + 1);
  CHECK(outline(resolve(p.doc))[0].children[0].children.size() == 2u);  // the named view and the note
  const json back = run(p.doc, "delete", {{"target", gone["id"]}});
  CHECK(!back.contains("regenerated") && outline(resolve(p.doc))[0].children[0].children.size() == 6u);
  const json renamed = run(p.doc, "sheet_edit", {{"target", p.sheet}, {"set", {{"name", "Plate"}}}});
  CHECK_EQ(resolve(p.doc).sheet(p.sheet)->name, "Plate");
  CHECK(!run(p.doc, "delete", {{"target", renamed["id"]}}).contains("regenerated"));
  CHECK_EQ(resolve(p.doc).sheet(p.sheet)->name, "Sheet 1");
  CHECK(run(p.doc, "delete", {{"target", p.box}}).contains("regenerated"));
}

CHECK_MAIN()
