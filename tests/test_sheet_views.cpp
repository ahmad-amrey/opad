// Section, detail, auxiliary, cropped and broken views (TODO 11 UI-82): the records, the cut and its hatched faces, the
// clipped and closed-up linework, alignment of auxiliary and section views, inherited breaks, dimensions across a break,
// the marks a parent view draws for its section and detail views, labels, frozen linework with its section faces.
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cmath>
#include <set>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/drawing/tables.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {

json run(Document& doc, const std::string& command, const json& args) { return commands::run(command, args, &doc); }

// A 60 x 40 x 10 plate centred on the origin (z 0..10) with a 10 mm hole through its middle, an A3 sheet, its front view.
struct Plate {
  Document doc = Document::create();
  std::string body, sheet, front;
  Plate() {
    const json made = run(doc, "feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}}});
    body = made["body_ids"][0];
    run(doc, "feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"base", "xy"}}}, {"x", 0}, {"y", 0}, {"diameter", 10}, {"height", 10}, {"operation", "cut"}, {"targets", {body}}}}});
    sheet = run(doc, "sheet", {{"size", "A3"}})["id"];
    front = run(doc, "sheet_view", {{"sheet", sheet}, {"orient", "front"}, {"at", {200, 180}}})["id"];
  }
};

const ViewFrame& frame(const std::vector<ViewFrame>& frames, const std::string& id) {
  for (const auto& f : frames)
    if (f.id == id) return f;
  throw Error("no frame");
}

double area(const std::vector<Vec2>& l) {
  double a = 0;
  for (size_t i = 0; i < l.size(); ++i) a += l[i][0] * l[(i + 1) % l.size()][1] - l[(i + 1) % l.size()][0] * l[i][1];
  return std::fabs(a / 2);
}

int on_layer(const Display& d, const std::string& layer, const std::string& source = {}) {
  int n = 0;
  for (const auto& p : d.prims)
    if (d.layers[static_cast<size_t>(p.layer)].name == layer && (source.empty() || p.source == source)) ++n;
  return n;
}

std::set<std::string> texts(const Display& d, const std::string& source = {}) {
  std::set<std::string> out;
  for (const auto& p : d.prims)
    if (p.kind == Prim::Kind::Text && (source.empty() || p.source == source)) out.insert(p.text);
  return out;
}

}  // namespace

TEST(views_records_checked) {
  const std::string s = new_uuid(), p = new_uuid();
  const auto view = [&](json extra) {
    json op = {{"op", "sheet_view"}, {"sheet", s}, {"kind", "section"}, {"parent", p}};
    op.update(extra);
    return op;
  };
  Document::validate_op(view({{"cut", {{0, 0}, {0, 10}}}}));
  CHECK_THROWS(Document::validate_op(view({{"cut", {{0, 0}}}})));
  CHECK_THROWS(Document::validate_op(view({{"cut", {0, 1}}})));
  CHECK_THROWS(Document::validate_op(view({{"radius", -2}})));
  CHECK_THROWS(Document::validate_op(view({{"flip", "yes"}})));
  CHECK_THROWS(Document::validate_op(view({{"crop", {0, 0, -1, 5}}})));
  CHECK_THROWS(Document::validate_op(view({{"breaks", {{{"axis", "z"}, {"from", 0}, {"to", 1}}}}})));
  CHECK_THROWS(Document::validate_op(view({{"breaks", {{{"axis", "x"}, {"from", 3}, {"to", 1}}}}})));
  Document::validate_op(view({{"breaks", {{{"axis", "y"}, {"from", 1}, {"to", 3}, {"gap", 5}}}}}));
  Plate pl;
  CHECK_THROWS(run(pl.doc, "sheet_view", {{"sheet", pl.sheet}, {"kind", "section"}, {"parent", pl.front}, {"cut", {{0, 0}, {0, 0}}}}));
  CHECK_THROWS(run(pl.doc, "sheet_view", {{"sheet", pl.sheet}, {"kind", "detail"}, {"parent", pl.front}, {"center", {0, 0}}}));
  CHECK_THROWS(run(pl.doc, "sheet_view", {{"sheet", pl.sheet}, {"kind", "auxiliary"}, {"parent", pl.front}}));
  CHECK_THROWS(run(pl.doc, "sheet_edit", {{"target", pl.front}, {"set", {{"breaks", {{{"axis", "x"}, {"from", 0}, {"to", 5}}, {{"axis", "x"}, {"from", 4}, {"to", 8}}}}}}}));
  // Letters: A, B, ... skipping I, O and Q, over the drawing's views.
  const Scene scene = resolve(pl.doc);
  CHECK_EQ(next_view_letter(scene, *scene.sheet(pl.sheet)), "A");
  for (int i = 0; i < 8; ++i) run(pl.doc, "sheet_view", {{"sheet", pl.sheet}, {"kind", "detail"}, {"parent", pl.front}, {"center", {0, 0}}, {"radius", 2}});
  const Scene more = resolve(pl.doc);
  CHECK_EQ(next_view_letter(more, *more.sheet(pl.sheet)), "J");
}

// A full section through the hole: the half towards the viewer taken away, the two cut faces hatched, the hole's far half
// still drawn, edges of the body named after the body's own; the parent shows the cutting line, its ends, arrows, letters.
TEST(views_full_section) {
  Plate p;
  const json made = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"kind", "section"}, {"parent", p.front}, {"cut", {{0, -10}, {0, 20}}}});
  const std::string sec = made["id"];
  Scene s = resolve(p.doc);
  CHECK(s.unresolved.empty());
  const SheetView& v = *s.sheet_view(sec);
  CHECK_EQ(v.def["letter"], "A");
  const ViewSpec spec = view_spec(s, v);
  CHECK_EQ(spec.cut.size(), 2u);
  CHECK(std::fabs(spec.dir[0] - 1) < 1e-12);  // first angle, placed left: seen from the right
  CHECK(!spec.hidden);
  const auto frames = layout(p.doc, s, *s.sheet(p.sheet));
  const ViewFrame& f = frame(frames, sec);
  const ViewFrame& fr = frame(frames, p.front);
  CHECK(f.error.empty());
  CHECK(f.box[2] < fr.box[0]);                 // left of the front view
  CHECK(std::fabs(f.at[1] - fr.at[1]) < 1e-9);  // lined up with it
  CHECK(std::fabs(fr.box[0] - f.box[2] - 20) < 1e-6);
  const auto g = project(p.doc, s, spec, {}, false);
  CHECK_EQ(g->sections.size(), 1u);
  CHECK_EQ(g->sections[0].loops.size(), 2u);
  for (const auto& l : g->sections[0].loops) CHECK(std::fabs(area(l) - 150) < 0.5);  // 15 x 10 either side of the hole
  bool named = false, made_by_cut = false;
  for (const auto& c : g->curves) {
    if (c.edge >= 0) {
      const json e = inspect_ref(p.doc, s, Ref{p.body, Ref::Kind::Edge, c.edge});
      named = named || e.value("curve", "") == "circle";  // the hole's rims, seen edge on
    }
    made_by_cut = made_by_cut || (c.edge < 0 && c.face < 0);
  }
  CHECK(named && made_by_cut);
  // Nothing of the removed half: every curve within the plate's width and height in the view.
  for (const auto& c : g->curves)
    for (const auto& q : c.sample(0.01)) CHECK(std::fabs(q[0]) <= 20 + 1e-6 && q[1] >= -1e-6 && q[1] <= 10 + 1e-6);
  CHECK(g->fingerprint != projection_fingerprint(p.doc, s, view_spec(s, *s.sheet_view(p.front)), Quality::Auto));
  // Cached with its faces, in memory and in the user cache's blob.
  CHECK(project(p.doc, s, spec).get() == project(p.doc, s, spec).get());
  const ViewGeometry back = ViewGeometry::deserialize(g->serialize());
  CHECK_EQ(back.sections.size(), 1u);
  CHECK_EQ(back.sections[0].loops.size(), 2u);
  CHECK_EQ(back.sections[0].loops[0].size(), g->sections[0].loops[0].size());
  // Drawn: hatching on the section, its label, the cutting line on the front view with its letters.
  json report;
  const Display d = sheet_display(p.doc, s, *s.sheet(p.sheet), {}, &report);
  CHECK_EQ(report["skipped"].size(), 0u);
  CHECK(on_layer(d, "Hatch", sec) > 10);
  CHECK(texts(d, sec).count("A-A"));
  CHECK_EQ(on_layer(d, "Section line", p.front), 1);
  CHECK_EQ(on_layer(d, "Section ends", p.front), 2);
  CHECK(texts(d, p.front).count("A"));
  for (const auto& prim : d.prims)  // hatch lines stay inside the cut faces (paper)
    if (d.layers[static_cast<size_t>(prim.layer)].name == "Hatch")
      for (const auto& q : prim.curve.pts) CHECK(q[0] >= f.box[0] - 1e-6 && q[0] <= f.box[2] + 1e-6 && q[1] >= f.box[1] - 1e-6 && q[1] <= f.box[3] + 1e-6);
  CHECK(dxf_text(d).find("Hatch") != std::string::npos);
  // Frozen linework keeps the faces.
  const ViewGeometry frozen = frozen_geometry(shape_from_brep(linework_brep(*g)));
  CHECK_EQ(frozen.sections.size(), 1u);
  CHECK_EQ(frozen.sections[0].loops.size(), 2u);
  // ASME: SECTION A-A.
  run(p.doc, "sheet_edit", {{"target", p.sheet}, {"set", {{"standard", "asme"}, {"projection", "third"}}}});
  s = resolve(p.doc);
  CHECK(std::fabs(view_spec(s, *s.sheet_view(sec)).dir[0] + 1) < 1e-12);  // third angle: seen from the left, as placed
  CHECK(texts(sheet_display(p.doc, s, *s.sheet(p.sheet)), sec).count("SECTION A-A"));
}

// An offset section: two parallel cutting planes joined by a step; only the faces facing the viewer are hatched. A body
// left whole is not cut.
TEST(views_offset_section_and_whole) {
  Plate p;
  const std::string sec = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"kind", "section"}, {"parent", p.front}, {"cut", {{-15, -10}, {-15, 5}, {15, 5}, {15, 20}}}})["id"];
  Scene s = resolve(p.doc);
  auto g = project(p.doc, s, view_spec(s, *s.sheet_view(sec)), {}, false);
  CHECK_EQ(g->sections.size(), 1u);
  double total = 0;
  for (const auto& l : g->sections[0].loops) total += area(l);
  CHECK_EQ(g->sections[0].loops.size(), 2u);
  CHECK(std::fabs(total - 400) < 1);  // 40 x 5 at x = -15 and 40 x 5 at x = 15
  run(p.doc, "sheet_edit", {{"target", sec}, {"set", {{"whole", {p.body}}}}});
  s = resolve(p.doc);
  g = project(p.doc, s, view_spec(s, *s.sheet_view(sec)), {}, false);
  CHECK(g->sections.empty());
  CHECK(!g->curves.empty());
}

// A detail view: the parent's projection within its circle at its own scale, the circle and letter on the parent.
TEST(views_detail) {
  Plate p;
  const std::string det = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"kind", "detail"}, {"parent", p.front}, {"center", {25, 5}}, {"radius", 8},
                                                    {"scale", "4:1"}, {"at", {330, 120}}})["id"];
  const Scene s = resolve(p.doc);
  const auto frames = layout(p.doc, s, *s.sheet(p.sheet));
  const ViewFrame& f = frame(frames, det);
  CHECK(f.error.empty());
  CHECK_EQ(f.scale, 4);
  CHECK(std::fabs(f.box[2] - f.box[0] - 64) < 1e-6);
  CHECK_EQ(projection_fingerprint(p.doc, s, view_spec(s, *s.sheet_view(det)), Quality::Auto),
           projection_fingerprint(p.doc, s, view_spec(s, *s.sheet_view(p.front)), Quality::Auto));  // one projection for both
  const auto g = shape_linework(project(p.doc, s, view_spec(s, *s.sheet_view(det))), f);
  CHECK(!g->curves.empty());
  for (const auto& c : g->curves)
    for (const auto& q : c.sample(0.01)) CHECK(std::hypot(q[0] - 25, q[1] - 5) <= 8 + 1e-6);
  const Display d = sheet_display(p.doc, s, *s.sheet(p.sheet));
  CHECK(texts(d, det).count("A (4:1)"));
  CHECK_EQ(on_layer(d, "Detail", det), 1);      // its boundary
  CHECK_EQ(on_layer(d, "Detail", p.front), 1);  // the circle on the parent
  CHECK(texts(d, p.front).count("A"));
}

// An auxiliary view looks along a line at any angle and stays lined up with its parent across it.
TEST(views_auxiliary_lined_up) {
  Plate p;
  const std::string aux = run(p.doc, "sheet_view", {{"sheet", p.sheet}, {"kind", "auxiliary"}, {"parent", p.front}, {"angle", 30}, {"gap", 15}})["id"];
  const Scene s = resolve(p.doc);
  const auto frames = layout(p.doc, s, *s.sheet(p.sheet));
  const ViewFrame &f = frame(frames, aux), &fr = frame(frames, p.front);
  CHECK(f.error.empty());
  const double c = std::cos(M_PI / 6), k = std::sin(M_PI / 6);
  CHECK(std::fabs(f.dir[0] + c) < 1e-9 && std::fabs(f.dir[2] + k) < 1e-9);  // first angle: from the opposite side
  const Vec2 a{-k, c};
  for (const Vec3& q : {Vec3{30, -20, 10}, Vec3{-30, 20, 0}, Vec3{5, 0, 3}}) {
    const Vec2 p1 = fr.paper(q), p2 = f.paper(q);
    CHECK(std::fabs((p1[0] - p2[0]) * a[0] + (p1[1] - p2[1]) * a[1]) < 1e-6);  // the same place across the line of sight
  }
  CHECK((f.at[0] - fr.at[0]) * c + (f.at[1] - fr.at[1]) * k > 0);  // out along its angle
  CHECK(resolve(p.doc).sheet_view(aux)->def.value("letter", "").empty());
}

// A crop box keeps part of a view; breaks take bands out and close them up (the view projected from it follows), and a
// dimension across a break keeps its true value while drawn shortened.
TEST(views_crop_and_breaks) {
  Plate p;
  run(p.doc, "sheet_edit", {{"target", p.front}, {"set", {{"crop", {-30, -1, 0, 11}}}}});
  Scene s = resolve(p.doc);
  auto frames = layout(p.doc, s, *s.sheet(p.sheet));
  CHECK(std::fabs(frame(frames, p.front).box[2] - frame(frames, p.front).box[0] - 30) < 1e-6);
  const auto g = shape_linework(project(p.doc, s, view_spec(s, *s.sheet_view(p.front))), frame(frames, p.front));
  for (const auto& c : g->curves)
    for (const auto& q : c.sample(0.01)) CHECK(q[0] <= 1e-6);
  CHECK(std::fabs(g->bounds[2]) < 1e-6 && std::fabs(g->bounds[0] + 30) < 1e-6);

  Document doc = Document::create();
  const std::string bar = run(doc, "feature", {{"kind", "box"}, {"inputs", {{"length", "200 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}}})["body_ids"][0];
  const std::string sheet = run(doc, "sheet", {{"size", "A3"}})["id"];
  const std::string front = run(doc, "sheet_view", {{"sheet", sheet}, {"orient", "front"}, {"at", {200, 200}}, {"breaks", {{{"axis", "x"}, {"from", -60}, {"to", 60}}}}})["id"];
  const std::string top = run(doc, "sheet_view", {{"sheet", sheet}, {"parent", front}, {"side", "bottom"}})["id"];
  s = resolve(doc);
  frames = layout(doc, s, *s.sheet(sheet));
  const ViewFrame &f = frame(frames, front), &t = frame(frames, top);
  CHECK(std::fabs(f.box[2] - f.box[0] - 86) < 1e-6);  // 200 - 120 + a 6 mm gap
  CHECK(std::fabs(t.box[2] - t.box[0] - 86) < 1e-6);  // the top view broken with it
  CHECK_EQ(t.breaks.size(), 1u);
  for (double x : {-100.0, -70.0, 75.0, 100.0}) CHECK(std::fabs(f.unfold(f.fold({x, 3}))[0] - x) < 1e-9);
  const auto lines = shape_linework(project(doc, s, view_spec(s, *s.sheet_view(front))), f);
  for (const auto& c : lines->curves)
    for (const auto& q : c.pts) CHECK(q[0] <= -60 + 1e-6 || q[0] >= -54 - 1e-6);  // nothing in the gap's place but the far half moved in
  // The bar's top front edge dimensioned across the break: 200, drawn 86 long.
  std::string edge;
  const TopoDS_Shape shape = node_world_shape(doc, s, bar);
  for (int i = 0; i < subshape_count(shape, Ref::Kind::Edge) && edge.empty(); ++i) {
    const json e = inspect_ref(doc, s, Ref{bar, Ref::Kind::Edge, i});
    if (e.contains("length") && std::fabs(e["length"].get<double>() - 200) < 1e-6 && std::fabs(e["bbox"]["center"][2].get<double>() - 10) < 1e-6 &&
        e["bbox"]["center"][1].get<double>() < 0)
      edge = Ref{bar, Ref::Kind::Edge, i}.str();
  }
  CHECK(!edge.empty());
  const json dim = run(doc, "sheet_item", {{"sheet", sheet}, {"view", front}, {"type", "horizontal"}, {"refs", {edge}}, {"place", {0, 15}}});
  CHECK(std::fabs(dim["result"]["value"].get<double>() - 200) < 1e-9);
  const Display d = sheet_display(doc, resolve(doc), *resolve(doc).sheet(sheet));
  CHECK_EQ(on_layer(d, "Break", front), 2);
  CHECK_EQ(on_layer(d, "Break", top), 2);
  std::array<double, 4> b{1e300, 1e300, -1e300, -1e300};
  for (const auto& prim : d.prims)
    if (prim.source == dim["id"].get<std::string>() && prim.kind == Prim::Kind::Curve)
      for (const auto& q : prim.curve.pts) b = {std::min(b[0], q[0]), std::min(b[1], q[1]), std::max(b[2], q[0]), std::max(b[3], q[1])};
  CHECK(std::fabs(b[2] - b[0] - 86) < 2.5);  // its extension lines 86 apart (arrows within)
  CHECK(texts(d, dim["id"].get<std::string>()).count("200"));
}

CHECK_MAIN()
