// Annotations of drawing sheets (TODO 11 UI-79, UI-80, UI-81): vector glyphs, holes recognised from faces and from hole
// features with their callouts and a hole table, centre marks and lines, leaders, datum symbols, feature control frames,
// surface texture, ordinate/baseline/chain sets (from datums too), smart dimension readings from picks, re-attaching, and
// forward compatibility of the new kinds.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/design/sketch.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/holes.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/drawing/symbols.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {

json run(Document& doc, const std::string& command, const json& args) { return commands::run(command, args, &doc); }

int count_kind(const Display& d, Prim::Kind k) {
  return static_cast<int>(std::count_if(d.prims.begin(), d.prims.end(), [&](const Prim& p) { return p.kind == k; }));
}

const Prim* text_of(const Display& d, const std::string& s) {
  for (const auto& p : d.prims)
    if (p.kind == Prim::Kind::Text && p.text == s) return &p;
  return nullptr;
}

// The 60 x 40 x 10 plate centred on the origin (z 0..10) with a 10 mm hole at the origin and a 6 mm one at x 20.
struct Plate {
  Document doc = Document::create();
  std::string box, body, sheet, front, top;
  Plate() {
    const json made = run(doc, "feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}});
    box = made["feature_id"];
    body = made["body_ids"][0];
    run(doc, "feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"base", "xy"}}}, {"x", 0}, {"y", 0}, {"diameter", 10}, {"height", 10}, {"operation", "cut"}, {"targets", {body}}}}});
    run(doc, "feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"base", "xy"}}}, {"x", 20}, {"y", 0}, {"diameter", 6}, {"height", 10}, {"operation", "cut"}, {"targets", {body}}}}});
    sheet = run(doc, "sheet", {{"size", "A3"}})["id"];
    front = run(doc, "sheet_view", {{"sheet", sheet}, {"orient", "front"}, {"at", {120, 220}}})["id"];
    top = run(doc, "sheet_view", {{"sheet", sheet}, {"parent", front}, {"side", "bottom"}})["id"];
  }
  // An entity of the body whose inspection passes `ok`.
  std::string find(Ref::Kind kind, const std::function<bool(const json&)>& ok) {
    const Scene s = resolve(doc);
    const TopoDS_Shape shape = node_world_shape(doc, s, body);
    for (int i = 0; i < subshape_count(shape, kind); ++i) {
      Ref r{body, kind, i};
      if (ok(inspect_ref(doc, s, r))) return r.str();
    }
    throw Error("no such entity");
  }
  std::string edge(const Vec3& at, const Vec3& dir) {
    return find(Ref::Kind::Edge, [&](const json& e) {
      if (!e.contains("direction")) return false;
      const auto c = e["bbox"]["center"], d = e["direction"];
      return std::hypot(std::hypot(c[0].get<double>() - at[0], c[1].get<double>() - at[1]), c[2].get<double>() - at[2]) < 1e-6 &&
             std::fabs(std::fabs(d[0].get<double>() * dir[0] + d[1].get<double>() * dir[1] + d[2].get<double>() * dir[2]) - 1) < 1e-9;
    });
  }
  std::string circle(double x, double z) {
    return find(Ref::Kind::Edge, [&](const json& e) {
      return e.value("curve", "") == "circle" && std::fabs(e["center"][0].get<double>() - x) < 1e-6 && std::fabs(e["center"][2].get<double>() - z) < 1e-6;
    });
  }
  std::string cylinder(double r) {
    return find(Ref::Kind::Face, [&](const json& e) { return e.value("surface", "") == "cylinder" && std::fabs(e.value("radius", 0.0) - r) < 1e-6; });
  }
};

std::map<std::string, json> items_of(const json& info) {
  std::map<std::string, json> out;
  for (const auto& i : info["items"]) out[i["id"]] = i;
  return out;
}

const Hole* hole_near(const std::vector<Hole>& holes, double x, double y) {
  for (const auto& h : holes)
    if (std::fabs(h.entry[0] - x) < 1e-6 && std::fabs(h.entry[1] - y) < 1e-6) return &h;
  return nullptr;
}

}  // namespace

// Glyphs are vectors, the text around them text runs on the same baseline; text without glyphs stays one primitive.
TEST(glyphs_are_drawn_as_vectors) {
  Display d;
  const int l = d.layer({"D"});
  rich_text(d, l, "⌀10", {0, 0}, 3.5);
  CHECK_EQ(count_kind(d, Prim::Kind::Text), 1);
  const Prim* ten = text_of(d, "10");
  CHECK(ten && std::fabs(ten->at[0] - 3.5) < 1e-9 && std::fabs(ten->at[1]) < 1e-9 && ten->halign == 0 && ten->valign == 0);
  bool circle = false;
  for (const auto& p : d.prims) circle = circle || (p.kind == Prim::Kind::Curve && p.curve.type == Curve::Type::Arc && std::fabs(p.curve.r1 - 1.4) < 1e-9);
  CHECK(circle);
  CHECK_NEAR(text_width("10", 3.5), 2 * 0.556 * 3.5 / 0.716, 1e-9);
  CHECK_NEAR(text_width("⌀10", 3.5), 3.5 + 2 * 0.556 * 3.5 / 0.716, 1e-9);
  Display c;  // centred: the run starts half the width left of the anchor
  rich_text(c, c.layer({"D"}), "⌀10", {0, 0}, 3.5, 0, 1, 0);
  CHECK_NEAR(text_of(c, "10")->at[0], -text_width("⌀10", 3.5) / 2 + 3.5, 1e-9);
  Display p;
  rich_text(p, p.layer({"D"}), "60", {1, 2}, 3.5, 0.5, 1, 2);
  CHECK(p.prims.size() == 1 && p.prims[0].halign == 1 && p.prims[0].valign == 2 && p.prims[0].angle == 0.5);
  for (const auto& name : characteristics()) {
    Display g;
    rich_text(g, g.layer({"D"}), characteristic_glyph(name), {0, 0}, 3.5);
    CHECK(!g.prims.empty() && count_kind(g, Prim::Kind::Text) == 0);
  }
  CHECK(characteristic_glyph("roundness").empty());
  Display m;
  rich_text(m, m.layer({"D"}), "0.1" + modifier_glyph("M"), {0, 0}, 3.5);
  CHECK(text_of(m, "0.1") && text_of(m, "M"));
  // Ordinate values too close together spread apart along their level.
  Display o;
  ordinate_dimensions(o, o.layer({"D"}), {{0, 0}, {1, 0}, {30, 0}}, {"0", "1", "30"}, {1, 0}, -10);
  CHECK(text_of(o, "0") && text_of(o, "1") && std::fabs(text_of(o, "1")->at[0] - text_of(o, "0")->at[0]) >= 1.6 * 3.5 - 1e-9);
  CHECK_NEAR(text_of(o, "30")->at[0], 30, 1e-9);
}

// A block cut by every kind of hole: through (twice), blind with a flat bottom, blind with a drill point, counterbored
// and countersunk; a fillet-like quarter cylinder is no hole.
TEST(holes_recognised_from_faces) {
  TopoDS_Shape s = BRepPrimAPI_MakeBox(gp_Pnt(-40, -20, -20), 80, 40, 20).Shape();
  const auto cut = [&](const TopoDS_Shape& tool) { s = BRepAlgoAPI_Cut(s, tool).Shape(); };
  const gp_Dir up(0, 0, 1), down(0, 0, -1);
  cut(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(-30, 10, -25), up), 2.5, 30).Shape());  // through
  cut(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(-30, -10, -25), up), 2.5, 30).Shape());  // through, the same
  cut(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(-15, 10, -8), up), 2, 9).Shape());   // blind, flat
  const double tip = 2.5 / std::tan(59 * M_PI / 180);
  cut(BRepAlgoAPI_Fuse(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 10, -10), up), 2.5, 11).Shape(),
                       BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 10, -10), down), 2.5, 0, tip).Shape()).Shape());  // drilled 118°
  cut(BRepAlgoAPI_Fuse(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(15, 10, -25), up), 3, 30).Shape(),
                       BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(15, 10, -4), up), 5, 5).Shape()).Shape());  // counterbored
  cut(BRepAlgoAPI_Fuse(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(30, 10, -12), up), 2.5, 13).Shape(),
                       BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(30, 10, 1), down), 6, 0, 6).Shape()).Shape());  // countersunk
  cut(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(40, 20, -25), up), 4, 30).Shape());  // a quarter round notch in a corner
  const auto holes = find_holes(s);
  CHECK_EQ(holes.size(), 6u);
  const Hole *thru = hole_near(holes, -30, 10), *flat = hole_near(holes, -15, 10), *drilled = hole_near(holes, 0, 10), *cb = hole_near(holes, 15, 10),
             *cs = hole_near(holes, 30, 10);
  CHECK(thru && flat && drilled && cb && cs);
  if (!thru || !flat || !drilled || !cb || !cs) return;
  CHECK(thru->through && thru->type == "simple" && std::fabs(thru->diameter - 5) < 1e-9 && std::fabs(thru->entry[2]) < 1e-9 && thru->dir[2] < -0.999);
  CHECK(!flat->through && std::fabs(flat->depth - 8) < 1e-9 && flat->tip_angle == 0 && std::fabs(flat->diameter - 4) < 1e-9);
  CHECK(!drilled->through && std::fabs(drilled->depth - 10) < 1e-9 && std::fabs(drilled->tip_angle - 118) < 1e-6);
  CHECK(cb->through && cb->type == "counterbore" && std::fabs(cb->cb_diameter - 10) < 1e-9 && std::fabs(cb->cb_depth - 4) < 1e-9 && std::fabs(cb->diameter - 6) < 1e-9);
  CHECK(!cs->through && cs->type == "countersink" && std::fabs(cs->cs_diameter - 10) < 1e-6 && std::fabs(cs->cs_angle - 90) < 1e-6 && std::fabs(cs->depth - 12) < 1e-9);
  int same = 0;
  for (const auto& h : holes) same += h.same_size(*thru);
  CHECK_EQ(same, 2);
  const auto num = [](double v) { return format_number(v, 2); };
  CHECK_EQ(hole_callout(*thru, 2, "iso", 1, num), "2× ⌀5 THRU");
  CHECK_EQ(hole_callout(*thru, 2, "asme", 1, num), "2X ⌀5 THRU");
  CHECK_EQ(hole_callout(*flat, 1, "iso", 1, num), "⌀4 ↧8");
  CHECK_EQ(hole_callout(*cb, 1, "iso", 1, num), "⌀6 THRU\n⌴ ⌀10 ↧4");
  CHECK_EQ(hole_callout(*cs, 1, "iso", 1, num), "⌀5 ↧12\n⌵ ⌀10 × 90°");
  CHECK_EQ(hole_callout(*thru, 2, "asme", 1 / 25.4, [](double v) { return format_number(v, 3); }), "2X ⌀0.197 THRU");
  // Any face or circle of a hole names it.
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  for (int f : cb->faces) CHECK_EQ(hole_of(holes, s, faces(f + 1)), static_cast<int>(cb - holes.data()));
}

// Callouts take a hole feature's values where it drilled the hole, count the equal ones, follow the model and go into a
// hole table; the table tags the holes in the view.
TEST(hole_callouts_and_the_hole_table) {
  Document doc = Document::create();
  run(doc, "feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "-10 mm"}}}});
  const std::string body = resolve(doc).all_bodies().front();
  design::Sketch pts;
  for (const auto& [x, y] : std::initializer_list<std::pair<double, double>>{{-20, 0}, {0, 0}, {20, 0}, {0, 12}}) {
    design::SkEntity e;
    e.type = design::SkEntity::Type::Point;
    e.p = {pts.add_point(x, y)};
    e.id = pts.next_id();
    pts.entities.push_back(e);
  }
  const std::string sk = run(doc, "sketch", {{"plane", {{"base", "xy"}}}, {"geometry", pts.to_json()}})["ids"][0];
  json at = json::array();
  for (const auto& p : pts.points) at.push_back({{"sketch", sk}, {"point", p.id}});
  const std::string simple = run(doc, "feature", {{"kind", "hole"}, {"inputs", {{"points", {at[0], at[1], at[2]}}, {"diameter", "6 mm"}, {"extent", "all"}}}})["ids"][0];
  const std::string bored = run(doc, "feature", {{"kind", "hole"}, {"inputs", {{"points", {at[3]}}, {"type", "counterbore"}, {"diameter", "5 mm"}, {"depth", "6 mm"},
                                                                              {"cb_diameter", "9 mm"}, {"cb_depth", "3 mm"}}}})["ids"][0];
  const std::string sheet = run(doc, "sheet", {{"size", "A3"}})["id"];
  const std::string top = run(doc, "sheet_view", {{"sheet", sheet}, {"orient", "top"}, {"at", {150, 180}}, {"scale", "2:1"}})["id"];
  const std::string front = run(doc, "sheet_view", {{"sheet", sheet}, {"orient", "front"}, {"at", {150, 90}}, {"scale", "2:1"}})["id"];
  Scene s = resolve(doc);
  const auto circle = [&](double x, double y, double r) {
    const TopoDS_Shape shape = node_world_shape(doc, s, body);
    for (int i = 0; i < subshape_count(shape, Ref::Kind::Edge); ++i) {
      const json e = inspect_ref(doc, s, Ref{body, Ref::Kind::Edge, i});
      if (e.value("curve", "") == "circle" && std::fabs(e["center"][0].get<double>() - x) < 1e-6 && std::fabs(e["center"][1].get<double>() - y) < 1e-6 &&
          std::fabs(e.value("radius", 0.0) - r) < 1e-6 && std::fabs(e["center"][2].get<double>()) < 1e-6)
        return Ref{body, Ref::Kind::Edge, i}.str();
    }
    throw Error("no circle");
  };
  const json c1 = run(doc, "sheet_item", {{"sheet", sheet}, {"view", top}, {"kind", "hole_callout"}, {"refs", {circle(-20, 0, 3)}}});
  CHECK_EQ(c1["result"]["shown"], "3× ⌀6 THRU");
  const json c2 = run(doc, "sheet_item", {{"sheet", sheet}, {"view", top}, {"kind", "hole_callout"}, {"refs", {circle(0, 12, 4.5)}}});
  CHECK_EQ(c2["result"]["shown"], "⌀5 ↧6\n⌴ ⌀9 ↧3");
  const json side = run(doc, "sheet_item", {{"sheet", sheet}, {"view", front}, {"kind", "hole_callout"}, {"refs", {circle(20, 0, 3)}}, {"place", {40, 20}}});
  CHECK_EQ(side["result"]["shown"], "3× ⌀6 THRU");
  CHECK_THROWS(run(doc, "sheet_item", {{"sheet", sheet}, {"view", top}, {"kind", "hole_callout"}, {"refs", {Ref{body, Ref::Kind::Face, 0}.str()}}}));
  s = resolve(doc);
  const Sheet& sh = *s.sheet(sheet);
  const auto frames = layout(doc, s, sh);
  const auto frameOf = [&](const std::string& id) -> const ViewFrame& { return *std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& f) { return f.id == id; }); };
  const json m1 = measure_item(doc, s, sh, *s.sheet_item(c1["id"]), &frameOf(top));
  CHECK_EQ(m1["hole"].value("feature", ""), simple);
  CHECK_NEAR(m1["r"].get<double>(), 6, 1e-9);  // 3 mm at 2:1, seen along its axis
  CHECK_EQ(measure_item(doc, s, sh, *s.sheet_item(c2["id"]), &frameOf(top))["hole"].value("feature", ""), bored);
  // The hole table: the 6 mm holes A1-A3 left to right, the counterbored one B1; from the view's lower left (-30, -20).
  const json table = run(doc, "sheet_item", {{"sheet", sheet}, {"view", top}, {"kind", "hole_table"}, {"at", {300, 280}}});
  const json rows = table["result"]["rows"];
  CHECK_EQ(rows.size(), 4u);
  CHECK(rows[0]["tag"] == "A1" && std::fabs(rows[0]["x"].get<double>() - 10) < 1e-6 && std::fabs(rows[0]["y"].get<double>() - 20) < 1e-6 && rows[0]["size"] == "⌀6 THRU");
  CHECK(rows[2]["tag"] == "A3" && std::fabs(rows[2]["x"].get<double>() - 50) < 1e-6);
  CHECK(rows[3]["tag"] == "B1" && std::fabs(rows[3]["x"].get<double>() - 30) < 1e-6 && std::fabs(rows[3]["y"].get<double>() - 32) < 1e-6 && rows[3]["size"] == "⌀5 ↧6 ⌴ ⌀9 ↧3");
  json report;
  const Scene withTable = resolve(doc);
  const Display d = sheet_display(doc, withTable, *withTable.sheet(sheet), {}, &report);
  CHECK_EQ(report["skipped"].size(), 0u);
  CHECK(text_of(d, "TAG") && text_of(d, "A1") && text_of(d, "B1") && text_of(d, "6 THRU") && text_of(d, "3× "));
  // The model changes: the callouts and the table follow, and say so.
  run(doc, "feature_edit", {{"target", simple}, {"inputs", {{"diameter", "8 mm"}}}});
  const auto items = items_of(run(doc, "sheet_info", {{"sheet", sheet}}));
  CHECK_EQ(items.at(c1["id"])["current"]["shown"], "3× ⌀8 THRU");
  CHECK(items.at(c1["id"])["changed"].get<bool>() && !items.at(c2["id"])["changed"].get<bool>());
  CHECK_EQ(items.at(table["id"])["current"]["rows"][0]["size"], "⌀8 THRU");
  CHECK_EQ(run(doc, "holes", {{"target", body}})["holes"].size(), 4u);
}

// Centre marks on circles seen along their axis, centre lines through two centres or along a cylinder seen from the side,
// a note's leader onto a circle, and the views' own centre marks.
TEST(centre_marks_lines_and_leaders) {
  Plate p;
  const json mark = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "centermark"}, {"refs", {p.circle(0, 10)}}});
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"kind", "centermark"}, {"refs", {p.circle(0, 10)}}}));
  const json both = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "centerline"}, {"refs", {p.circle(0, 10), p.circle(20, 10)}}});
  const json axis = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"kind", "centerline"}, {"refs", {p.cylinder(5)}}});
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "centerline"}, {"refs", {p.cylinder(5)}}}));
  const json note = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "note"}, {"text", "REAM ⌀10"}, {"refs", {p.circle(0, 10)}}, {"at", {25, 25}}});
  Scene s = resolve(p.doc);
  const Sheet& sh = *s.sheet(p.sheet);
  const auto frames = layout(p.doc, s, sh);
  const auto frameOf = [&](const std::string& id) -> const ViewFrame* {
    for (const auto& f : frames)
      if (f.id == id) return &f;
    return nullptr;
  };
  const json mm = measure_item(p.doc, s, sh, *s.sheet_item(mark["id"]), frameOf(p.top));
  CHECK_NEAR(mm["r"].get<double>(), 5, 1e-9);
  const json ml = measure_item(p.doc, s, sh, *s.sheet_item(both["id"]), frameOf(p.top));
  const auto v = [](const json& j) { return Vec2{j[0].get<double>(), j[1].get<double>()}; };
  CHECK_NEAR(std::hypot(v(ml["to"])[0] - v(ml["from"])[0], v(ml["to"])[1] - v(ml["from"])[1]), 20, 1e-9);
  CHECK(std::fabs(ml["r"][0].get<double>() - 5) < 1e-9 && std::fabs(ml["r"][1].get<double>() - 3) < 1e-9);
  const json ma = measure_item(p.doc, s, sh, *s.sheet_item(axis["id"]), frameOf(p.front));
  CHECK_NEAR(std::fabs(v(ma["to"])[1] - v(ma["from"])[1]), 10, 1e-9);
  CHECK_NEAR(v(ma["to"])[0], v(ma["from"])[0], 1e-9);
  const json mn = measure_item(p.doc, s, sh, *s.sheet_item(note["id"]), frameOf(p.top));
  CHECK_NEAR(std::hypot(v(mn["tip"])[0], v(mn["tip"])[1]), 5, 1e-9);  // on the circle, towards the text
  CHECK(v(mn["tip"])[0] > 0 && v(mn["tip"])[1] > 0);
  json report;
  Display d = sheet_display(p.doc, s, sh, {}, &report);
  CHECK_EQ(report["skipped"].size(), 0u);
  int center = 0;
  for (const auto& prim : d.prims) center += d.layers[size_t(prim.layer)].name == "Center";
  CHECK(center >= 6 + 3);  // the mark's cross and arms, the two centre lines in dashes
  CHECK(text_of(d, "REAM ") && text_of(d, "10"));
  // A view's own centre marks: both holes from the top, the hidden holes' axes from the front.
  run(p.doc, "sheet_edit", {{"target", p.top}, {"set", {{"style", {{"centermarks", true}}}}}});
  run(p.doc, "sheet_edit", {{"target", p.front}, {"set", {{"style", {{"centermarks", true}, {"hidden", true}}}}}});
  s = resolve(p.doc);
  d = sheet_display(p.doc, s, *s.sheet(p.sheet));
  std::map<std::string, int> own;
  for (const auto& prim : d.prims)
    if (d.layers[size_t(prim.layer)].name == "Center") ++own[prim.source];
  CHECK(own[p.top] >= 12);   // two marks of a cross and four arms
  CHECK(own[p.front] >= 2);  // two centre lines
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "note"}, {"text", "x"}, {"refs", {p.circle(0, 10), p.circle(20, 10)}}}));
}

// Datum symbols, feature control frames and surface texture; ordinate, baseline and chain sets, also made from the datums.
TEST(datums_frames_surfaces_and_sets) {
  Plate p;
  const std::string left = p.edge({-30, 0, 10}, {0, 1, 0}), bottom = p.edge({0, -20, 10}, {1, 0, 0});
  const json a = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "datum"}, {"letter", "a"}, {"refs", {left}}});
  const json b = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "datum"}, {"letter", "B"}, {"refs", {bottom}}});
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "datum"}, {"letter", "a1"}, {"refs", {left}}}));
  const json fcf = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "fcf"}, {"characteristic", "position"}, {"value", 0.1}, {"zone", "diameter"},
                                             {"material", "m"}, {"datums", {"A", "B(M)"}}, {"refs", {p.circle(20, 10)}}});
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "fcf"}, {"characteristic", "roundness"}, {"value", 0.1}, {"place", {1, 1}}}));
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "fcf"}, {"characteristic", "flatness"}, {"value", 0.1}}));  // nowhere
  const json flat = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"kind", "fcf"}, {"characteristic", "flatness"}, {"value", "0.05"}, {"place", {-20, 15}}});
  const json texture = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.front}, {"kind", "surface"}, {"process", "removal"}, {"value", "Ra 1.6"},
                                                 {"refs", {p.edge({0, -20, 10}, {1, 0, 0})}}});
  Scene s = resolve(p.doc);
  const Sheet& sh = *s.sheet(p.sheet);
  CHECK_EQ(s.sheet_item(a["id"])->def["letter"], "A");
  const auto frames = layout(p.doc, s, sh);
  const ViewFrame& top = *std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& f) { return f.id == p.top; });
  const json ma = measure_item(p.doc, s, sh, *s.sheet_item(a["id"]), &top), mb = measure_item(p.doc, s, sh, *s.sheet_item(b["id"]), &top);
  CHECK(ma["out"][0].get<double>() < -0.999 && std::fabs(ma["foot"][0].get<double>() + 30) < 1e-9);  // left of the left edge (1:1)
  CHECK(mb["out"][1].get<double>() < -0.999);
  json report;
  const Display d = sheet_display(p.doc, s, sh, {}, &report);
  CHECK_EQ(report["skipped"].size(), 0u);
  CHECK(text_of(d, "A") && text_of(d, "B") && text_of(d, "M") && text_of(d, "0.1") && text_of(d, "Ra 1.6") && text_of(d, "0.05"));
  const auto rows = outline(s)[0].children[0].children;  // the sheet's views with their items
  std::map<std::string, std::string> names;
  for (const auto& v : rows)
    for (const auto& i : v.children) names[i.id] = i.name;
  CHECK_EQ(names[a["id"]], "A");
  CHECK_EQ(names[fcf["id"]], characteristic_glyph("position") + " ⌀0.1");
  CHECK_EQ(names[texture["id"]], "Ra 1.6");
  // Hole positions from the datums: ordinates across from A, up from B.
  const json sets = run(p.doc, "sheet_datum_dimensions", {{"sheet", p.sheet}, {"view", p.top}});
  CHECK_EQ(sets["ids"].size(), 2u);
  CHECK_EQ(sets["results"][0]["values"], json({30, 50}));
  CHECK_EQ(sets["results"][1]["values"], json({20, 20}));
  CHECK_EQ(sets["results"][0]["shown"], json({"30", "50"}));
  s = resolve(p.doc);
  CHECK_EQ(s.sheet_item(sets["ids"][0])->def["datum"], "A");
  CHECK_EQ(s.sheet_item(sets["ids"][1])->def["axis"], "vertical");
  // Baseline and chain sets by hand: from the left edge to both holes.
  const std::string c0 = p.circle(0, 10), c20 = p.circle(20, 10);
  const json base = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "dimension_set"}, {"type", "baseline"}, {"refs", {left, c20, c0}}});
  CHECK_EQ(base["result"]["values"], json({50, 30}));
  const json chain = run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "dimension_set"}, {"type", "chain"}, {"refs", {left, c20, c0}}, {"precision", 1}});
  CHECK_EQ(chain["result"]["values"], json({30, 20}));
  CHECK_THROWS(run(p.doc, "sheet_item", {{"sheet", p.sheet}, {"view", p.top}, {"kind", "dimension_set"}, {"type", "running"}, {"refs", {left, c0}}}));
  s = resolve(p.doc);
  json again;
  const Display e = sheet_display(p.doc, s, *s.sheet(p.sheet), {}, &again);
  CHECK_EQ(again["skipped"].size(), 0u);
  CHECK(text_of(e, "50") && text_of(e, "20"));
  // Re-attached: the baseline set measures from the bottom edge instead, upright; its values follow.
  const json moved = run(p.doc, "sheet_edit", {{"target", base["id"]}, {"set", {{"refs", {bottom, c20, c0}}, {"axis", "vertical"}}}});
  CHECK_EQ(moved["result"]["values"], json({20, 20}));
}

// The smart dimension's readings: a line gives horizontal, vertical and aligned; a circle its diameter; two lines that
// meet an angle; picks made on the projection become references with their aspects.
TEST(smart_dimension_readings_from_picks) {
  Plate p;
  const json width = plan_dimension(p.doc, resolve(p.doc), {{"sheet", p.sheet}, {"view", p.front}, {"refs", {p.edge({0, -20, 10}, {1, 0, 0})}}});
  std::vector<std::string> types;
  for (const auto& c : width["choices"]) types.push_back(c["type"]);
  CHECK((types == std::vector<std::string>{"horizontal", "vertical", "aligned"}));
  CHECK_NEAR(width["choices"][0]["op"]["result"]["value"].get<double>(), 60, 1e-9);
  const json hole = plan_dimension(p.doc, resolve(p.doc), {{"sheet", p.sheet}, {"view", p.top}, {"refs", {p.circle(0, 10)}}});
  CHECK(hole["choices"].size() == 1 && hole["choices"][0]["type"] == "diameter" && hole["choices"][0]["op"]["result"]["shown"] == "⌀10");
  const json corner = plan_dimension(p.doc, resolve(p.doc), {{"sheet", p.sheet}, {"view", p.top}, {"refs", {p.edge({-30, 0, 10}, {0, 1, 0}), p.edge({0, 20, 10}, {1, 0, 0})}}});
  CHECK(corner["choices"][0]["type"] == "angle" && std::fabs(corner["choices"][0]["op"]["result"]["value"].get<double>() - 90) < 1e-9);
  CHECK_THROWS(plan_dimension(p.doc, resolve(p.doc), {{"sheet", p.sheet}, {"view", p.top}, {"refs", {p.find(Ref::Kind::Vertex, [](const json&) { return true; })}}}));
  // Picks on the front view's projection: an end snap on the top edge's end, the middle of the left edge.
  const Scene s = resolve(p.doc);
  const auto frames = layout(p.doc, s, *s.sheet(p.sheet));
  const ViewFrame& front = *std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& f) { return f.id == p.front; });
  const auto g = project(p.doc, s, view_spec(s, *s.sheet_view(p.front)));
  json ends = json::array();
  const auto paper = [&](Vec2 q) { return Vec2{front.at[0] + front.scale * (q[0] - front.centre[0]), front.at[1] + front.scale * (q[1] - front.centre[1])}; };
  for (int pass = 0; pass < 2; ++pass)
    for (const auto& c : g->curves) {
      if (c.type != Curve::Type::Line || c.edge < 0 || c.hidden) continue;
      if (pass == 0 && std::fabs(c.pts[0][1] - c.pts[1][1]) < 1e-9 && std::fabs(c.pts[0][1] - 10) < 1e-9 && ends.empty())  // the top edge
        ends.push_back({{"node", g->bodies[size_t(c.body)].node}, {"edge", c.edge}, {"snap", "end"}, {"at", {paper(c.pts[0])[0], paper(c.pts[0])[1]}}});
      if (pass == 1 && std::fabs(c.pts[0][0] - c.pts[1][0]) < 1e-9 && std::fabs(c.pts[0][0] + 30) < 1e-9 && ends.size() == 1) {  // the left edge
        const Vec2 m = paper({c.pts[0][0], (c.pts[0][1] + c.pts[1][1]) / 2});
        ends.push_back({{"node", g->bodies[size_t(c.body)].node}, {"edge", c.edge}, {"snap", "mid"}, {"at", {m[0], m[1]}}});
      }
    }
  CHECK_EQ(ends.size(), 2u);
  const json picked = plan_dimension(p.doc, s, {{"sheet", p.sheet}, {"view", p.front}, {"picks", ends}});
  CHECK(picked["picks"][0]["what"] == "vertex" && picked["picks"][1]["what"] == "midpoint" && picked["picks"][1]["ref"]["aspect"] == "mid");
  CHECK(picked["picks"][0]["ref"].contains("hint"));
  CHECK_EQ(picked["choices"][1]["type"], "vertical");
  CHECK_NEAR(picked["choices"][1]["op"]["result"]["value"].get<double>(), 5, 1e-9);  // from the top corner to the left edge's middle
  // A planned record goes in as it is.
  const json op = picked["choices"][0]["op"];
  const json added = run(p.doc, "sheet_item", {{"op", op}});
  CHECK_EQ(resolve(p.doc).sheet_item(added["id"])->def["refs"], op["refs"]);
  CHECK_THROWS(run(p.doc, "sheet_item", {{"op", {{"op", "sheet_item"}, {"sheet", p.sheet}, {"kind", "weld"}}}}));
}

// Precision and units: ASME inches keep trailing zeros without the leading zero; limits stack; the new kinds stay
// readable by a build that does not know them (kept, reported) and kinds of later builds stay reported here.
TEST(values_and_compatibility) {
  Sheet asme;
  asme.standard = "asme";
  asme.def = {{"units", "in"}};
  CHECK_EQ(format_number(0.5, 3, &asme), ".500");
  CHECK_EQ(format_number(1.25, 3, &asme), "1.250");
  CHECK_EQ(format_number(0.5, 3), "0.5");
  CHECK_EQ(format_value(10, {{"precision", 1}, {"tol", {{"type", "limits"}, {"plus", 0.1}, {"minus", -0.05}}}}), "10.1\n9.95");
  CHECK_EQ(format_value(10, {{"type", "diameter"}, {"tol", {{"type", "sym"}, {"plus", 0.02}}}}), "⌀10 ±0.02");
  CHECK(known_item("hole_callout", "") && known_item("dimension_set", "chain") && !known_item("dimension_set", "running") && !known_item("weld", ""));
  Plate p;
  const std::string later = p.doc.append({{"op", "sheet_item"}, {"sheet", p.sheet}, {"view", p.top}, {"kind", "weld"}, {"refs", {p.circle(0, 10)}}}).id;
  const std::string typed = p.doc.append({{"op", "sheet_item"}, {"sheet", p.sheet}, {"view", p.top}, {"kind", "dimension_set"}, {"type", "running"}}).id;
  const Scene s = resolve(p.doc);
  bool laterReported = false, typedReported = false;
  for (const auto& u : s.unresolved) {
    laterReported = laterReported || (u.op_id == later && u.reason.find("needs a newer OPAD (sheet_item kind 'weld')") != std::string::npos);
    typedReported = typedReported || (u.op_id == typed && u.reason.find("needs a newer OPAD (dimension_set type 'running')") != std::string::npos);
  }
  CHECK(laterReported && typedReported);
  json report;
  sheet_display(p.doc, s, *s.sheet(p.sheet), {}, &report);
  CHECK_EQ(report["skipped"].size(), 2u);
}

CHECK_MAIN()
