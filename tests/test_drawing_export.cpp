// 2D writers and model-to-2D export (TODO 11 UI-86, UI-87): the DXF R2000 and SVG writers read back by OPAD's own
// readers, the R2000 structure (handles, owners, tables, objects), text for converters, dimensions as geometry,
// hidden-line views of solids written with exact arcs, and DWG through LibreDWG when a converter is at hand.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <Bnd_Box.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/drawing/display.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/step_io.hpp"

using namespace opad;
using namespace opad::drawing;

namespace {

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / new_uuid();
  Files() { std::filesystem::create_directory(dir); }
  ~Files() {
    std::error_code e;
    std::filesystem::remove_all(dir, e);
  }
};

struct Pair {
  int code;
  std::string value;
};

std::vector<Pair> pairs_of(const std::string& text) {
  std::vector<Pair> out;
  std::istringstream in(text);
  std::string code, value;
  while (std::getline(in, code) && std::getline(in, value)) out.push_back({std::stoi(code), value});
  return out;
}

// ENTITIES as (type, layer) with their groups.
struct Entity {
  std::string type, layer, owner;
  std::vector<Pair> groups;
  double num(int code, double fallback = 0) const {
    for (const auto& g : groups)
      if (g.code == code) return std::stod(g.value);
    return fallback;
  }
};
std::vector<Entity> entities_of(const std::vector<Pair>& p) {
  std::vector<Entity> out;
  bool in = false;
  for (size_t i = 0; i < p.size(); ++i) {
    if (p[i].code == 2 && i && p[i - 1].value == "SECTION") in = p[i].value == "ENTITIES";
    if (!in || p[i].code != 0) continue;
    if (p[i].value == "ENDSEC") break;
    Entity e{p[i].value, "", "", {}};
    for (size_t j = i + 1; j < p.size() && p[j].code != 0; ++j) {
      if (p[j].code == 8) e.layer = p[j].value;
      if (p[j].code == 330) e.owner = p[j].value;
      e.groups.push_back(p[j]);
    }
    out.push_back(std::move(e));
  }
  return out;
}

int count(const std::vector<Entity>& es, const std::string& type, const std::string& layer = {}) {
  int n = 0;
  for (const auto& e : es) n += e.type == type && (layer.empty() || e.layer == layer);
  return n;
}

// A drawing with a bit of everything the writers know.
Display sample() {
  Display d;
  d.title = "Sample";
  const int visible = d.layer({"Visible", kInk, LineType::Continuous, 0.5});
  const int hidden = d.layer({"Hidden", kInk, LineType::Hidden, 0.25});
  const int red = d.layer({"Red centre", 0xFF0000, LineType::Center, 0.35});
  const int arabic = d.layer({"\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A", 0x0000FF, LineType::Continuous, 0.25});  // "عربي"
  d.line(visible, {0, 0}, {100, 0});
  d.line(visible, {100, 0}, {100.0 / 3, 50});
  d.polyline(visible, {{0, 0}, {10, 60}, {0, 60}}, true);
  d.circle(red, {50, 25}, 10);
  d.arc(red, {50, 25}, 15, 0, M_PI / 2);
  d.line(hidden, {0, 10}, {100, 10});
  Curve e;
  e.type = Curve::Type::Ellipse;
  e.c = {150, 25};
  e.r1 = 20;
  e.r2 = 10;
  e.rot = M_PI / 6;
  e.a0 = 0;
  e.a1 = 2 * M_PI;
  d.curve(visible, e);
  e.c = {150, 80};
  e.a1 = M_PI;
  d.curve(visible, e);
  Curve s;  // a quarter circle as a rational quadratic spline, radius 30 about (0, 100)
  s.type = Curve::Type::Spline;
  s.degree = 2;
  s.pts = {{30, 100}, {30, 130}, {0, 130}};
  s.weights = {1, std::sqrt(0.5), 1};
  s.knots = {0, 0, 0, 1, 1, 1};
  d.curve(visible, s);
  d.fill(arabic, {{{200, 0}, {210, 0}, {210, 10}, {200, 10}}, {{203, 3}, {207, 3}, {207, 7}, {203, 7}}});  // a square with a hole
  d.fill(arabic, {{{220, 0}, {230, 0}, {225, 8}}});                                                          // a triangle
  d.text(visible, "OPAD 1/3", {0, -10}, 3.5);
  d.text(visible, "Line one\n\xD8\xB3\xD8\xB7\xD8\xB1 {2}", {50, -10}, 2.5, 0, 1, 3);  // "سطر"
  linear_dimension(d, red, {0, 0}, {100, 0}, {1, 0}, {50, -20}, "100");
  radial_dimension(d, red, {50, 25}, 10, {75, 45}, "\xC3\x98" "20", true);
  return d;
}

}  // namespace

TEST(dxf_writer_writes_a_well_formed_r2000_file) {
  const Display d = sample();
  const std::string text = dxf_text(d);
  const auto p = pairs_of(text);
  CHECK(text.size() > 1000 && text.rfind("  0\nSECTION\n  2\nHEADER\n", 0) == 0);
  CHECK(text.find("$ACADVER\n  1\nAC1015\n") != std::string::npos);
  CHECK(text.find("$INSUNITS\n 70\n4\n") != std::string::npos);
  CHECK(text.find("$MEASUREMENT\n 70\n1\n") != std::string::npos);
  CHECK_EQ(p.back().value, "EOF");
  // Handles unique, the seed above them all; every entity owned by the model space record.
  std::set<unsigned long> handles;
  unsigned long seed = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    if ((p[i].code == 5 || p[i].code == 105) && p[i - 1].value != "$HANDSEED") CHECK(handles.insert(std::stoul(p[i].value, nullptr, 16)).second);
    if (p[i].code == 9 && p[i].value == "$HANDSEED") seed = std::stoul(p[i + 1].value, nullptr, 16);
  }
  CHECK(seed > *handles.rbegin());
  std::string model;
  for (size_t i = 0; i + 1 < p.size(); ++i)
    if (p[i].code == 0 && p[i].value == "BLOCK_RECORD" && p[i + 5].value == "*Model_Space") model = p[i + 1].value;
  CHECK(!model.empty());
  const auto es = entities_of(p);
  for (const auto& e : es) CHECK_EQ(e.owner, model);
  // Tables: the line types, a layer per drawing layer with its line type and weight, the text style, the layouts.
  for (const char* s : {"\nHIDDEN\n", "\nCENTER\n", "\nPHANTOM\n", "AcDbLayerTableRecord", "\nRed centre\n", "\\U+0639\\U+0631\\U+0628\\U+064A",
                        "\nStandard\n", "*Paper_Space", "ACAD_LAYOUT", "\nLAYOUT\n", "ACDBPLACEHOLDER"})
    CHECK(text.find(s) != std::string::npos);
  for (size_t i = 0; i + 8 < p.size(); ++i)
    if (p[i].code == 0 && p[i].value == "LAYER" && p[i + 5].value == "Red centre") {
      CHECK_EQ(p[i + 7].value, "1");  // red
      CHECK_EQ(p[i + 8].value, "CENTER");
      CHECK_EQ(p[i + 9].value, "35");
    }
  // Entities: exact curves, fills (a convex one a SOLID), text; dimensions as lines, arrowheads and text.
  CHECK_EQ(count(es, "LINE", "Visible"), 2);
  CHECK_EQ(count(es, "LINE", "Hidden"), 1);
  CHECK_EQ(count(es, "CIRCLE", "Red centre"), 1);
  CHECK_EQ(count(es, "ARC", "Red centre"), 1);
  CHECK_EQ(count(es, "ELLIPSE"), 2);
  CHECK_EQ(count(es, "SPLINE"), 1);
  CHECK_EQ(count(es, "LWPOLYLINE"), 1);
  CHECK_EQ(count(es, "HATCH"), 1);
  CHECK_EQ(count(es, "TEXT"), 3);
  CHECK_EQ(count(es, "MTEXT"), 1);
  CHECK_EQ(count(es, "SOLID", "Red centre"), 4);  // the dimensions' arrowheads
  CHECK_EQ(count(es, "SOLID"), 5);
  CHECK(count(es, "LINE", "Red centre") >= 4);    // extension lines and dimension lines
  for (const auto& e : es)
    if (e.type == "ARC") {
      CHECK_NEAR(e.num(40), 15, 1e-9);
      CHECK_NEAR(e.num(50), 0, 1e-9);
      CHECK_NEAR(e.num(51), 90, 1e-9);
    } else if (e.type == "LINE" && e.layer == "Visible" && e.num(10) == 100) {
      CHECK_EQ(std::find_if(e.groups.begin(), e.groups.end(), [](const Pair& g) { return g.code == 11; })->value, "33.333333");  // 6 decimals
    } else if (e.type == "MTEXT") {
      bool found = false;
      for (const auto& g : e.groups) found = found || g.value.find("Line one\\P\\U+0633\\U+0637\\U+0631 \\{2\\}") != std::string::npos;
      CHECK(found);
    }
  CHECK(text.find("33.3333333") == std::string::npos);
  CHECK_EQ(dxf_text(d), text);  // the same drawing, the same bytes
}

TEST(dxf_written_reads_back_through_the_dxf_reader) {
  Files f;
  const auto file = f.dir / "sample.dxf";
  write_drawing(sample(), file, "dxf");
  auto doc = Document::create();
  const auto r = import_file(doc, file);
  const auto scene = resolve(doc);
  std::set<std::string> layers;
  for (const auto& c : scene.node(scene.roots[0])->children) layers.insert(scene.node(c)->name);
  CHECK(layers.count("Visible") && layers.count("Hidden") && layers.count("Red centre"));
  CHECK(layers.count("\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A"));
  // The circle comes back a circle, the spline a spline on the quarter circle, the fills as faces of the right area.
  int circles = 0, splines = 0;
  double fillArea = 0;
  for (const auto& id : scene.all_bodies()) {
    const auto shape = node_world_shape(doc, scene, id);
    for (TopExp_Explorer e(shape, TopAbs_EDGE, TopAbs_FACE); e.More(); e.Next()) {
      BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
      if (c.GetType() == GeomAbs_Circle && std::abs(c.Circle().Radius() - 10) < 1e-9 && std::abs(c.LastParameter() - c.FirstParameter() - 2 * M_PI) < 1e-9) ++circles;
      if (c.GetType() == GeomAbs_BSplineCurve) {
        ++splines;
        const auto mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        CHECK_NEAR(std::hypot(mid.X(), mid.Y() - 100), 30, 1e-6);
      }
    }
    if (scene.node(id)->name == "\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A")
      for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(e.Current(), g);
        fillArea += g.Mass();
      }
  }
  CHECK_EQ(circles, 1);
  CHECK_EQ(splines, 1);
  CHECK_NEAR(fillArea, 100 - 16 + 40, 1e-6);  // the square less its hole, and the triangle
  CHECK(r.warnings.empty() || r.warnings[0].find("font") != std::string::npos);
}

TEST(svg_writer_reads_back_with_its_layers) {
  Files f;
  Display d = sample();
  d.prims.erase(std::remove_if(d.prims.begin(), d.prims.end(), [](const Prim& p) { return p.kind == Prim::Kind::Text; }), d.prims.end());
  const auto file = f.dir / "sample.svg";
  write_drawing(d, file, "svg");
  const std::string text = read_text_file(file);
  CHECK(text.find("inkscape:label=\"Red centre\"") != std::string::npos && text.find("stroke-dasharray=\"12 3 1.5 3\"") != std::string::npos);
  CHECK(text.find("fill-rule=\"evenodd\"") != std::string::npos && text.find("<ellipse") != std::string::npos);
  CHECK(text.find("33.3333333") == std::string::npos && text.find("33.333333") != std::string::npos);
  auto doc = Document::create();
  import_file(doc, file);
  const auto scene = resolve(doc);
  std::set<std::string> layers;
  for (const auto& c : scene.node(scene.roots[0])->children) layers.insert(scene.node(c)->name);
  CHECK(layers.count("Visible") && layers.count("Hidden") && layers.count("Red centre") && layers.count("\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A"));
  // The same extents as written: the ellipse's far end, the polyline's top, the dimension below.
  Bnd_Box box;
  for (const auto& id : scene.all_bodies()) box.Add(node_world_bbox(doc, scene, id));
  const auto b = d.bounds();
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(x0, b[0], 0.05);
  CHECK_NEAR(x1, b[2], 0.05);
  CHECK_NEAR(y1, b[3], 0.05);
  // The arc stays the quarter it was (counter-clockwise from 0 to 90 degrees): its mid point at 45 degrees.
  bool arc = false;
  for (const auto& id : scene.all_bodies())
    for (TopExp_Explorer e(node_world_shape(doc, scene, id), TopAbs_EDGE); e.More(); e.Next()) {
      BRepAdaptor_Curve c(TopoDS::Edge(e.Current()));
      const auto mid = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
      if (std::abs(mid.Distance(gp_Pnt(50 + 15 * std::sqrt(0.5), 25 + 15 * std::sqrt(0.5), 0))) < 1e-6) arc = true;
    }
  CHECK(arc);
}

TEST(text_of_several_lines_can_go_as_text_lines) {
  // For converters: LibreDWG's DWG writer loses an MTEXT's height. Aligned text keeps its alignment point apart from its
  // start (LibreDWG drops one equal to it).
  const auto es = entities_of(pairs_of(dxf_text(sample(), 6, false)));
  CHECK_EQ(count(es, "MTEXT"), 0);
  CHECK_EQ(count(es, "TEXT"), 5);
  for (const auto& e : es)
    if (e.type == "TEXT" && e.num(72) == 1) CHECK(std::hypot(e.num(10) - e.num(11), e.num(20) - e.num(21)) > 1);
}

TEST(dimensions_are_drawn_as_geometry) {
  Display d;
  const int l = d.layer({"Dimensions", kInk, LineType::Continuous, 0.18});
  linear_dimension(d, l, {0, 0}, {0, 40}, {0, 1}, {-15, 20}, "40");  // vertical, to the left
  int lines = 0, arrows = 0;
  const Prim* text = nullptr;
  for (const auto& p : d.prims) {
    if (p.kind == Prim::Kind::Curve) ++lines;
    if (p.kind == Prim::Kind::Fill) ++arrows;
    if (p.kind == Prim::Kind::Text) text = &p;
  }
  CHECK_EQ(lines, 3);
  CHECK_EQ(arrows, 2);
  CHECK(text && text->text == "40");
  CHECK_NEAR(text->angle, M_PI / 2, 1e-12);  // read from the right
  CHECK(text->at[0] < -15 && text->at[0] > -17);  // above the dimension line, which runs at x = -15
  CHECK_NEAR(text->at[1], 20, 1e-9);
  // Extension lines leave a 1 mm gap at the feature and run 2 mm past the dimension line.
  CHECK_NEAR(d.prims[0].curve.pts[0][0], -1, 1e-9);
  CHECK_NEAR(d.prims[0].curve.pts[1][0], -17, 1e-9);
  // Too short for the arrows between the lines: they point in from outside.
  Display s;
  linear_dimension(s, s.layer({"D"}), {0, 0}, {4, 0}, {1, 0}, {2, 10}, "4");
  CHECK(s.prims[2].curve.pts[0][0] < -5 && s.prims[2].curve.pts[1][0] > 9);
  CHECK(s.prims[3].loops[0][1][0] < 0);  // the left arrowhead's back lies outside, left of its tip at 0
}

TEST(view_export_draws_a_part_with_hidden_lines_and_exact_circles) {
  Files f;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(40, 30, 20).Shape();
  const TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 15, -1), gp_Dir(0, 0, 1)), 4, 22).Shape();
  auto doc = Document::create();
  import_brep(doc, brep_from_shape(BRepAlgoAPI_Cut(box, hole).Shape()), "Block");
  const auto front = f.dir / "front.dxf";
  auto r = commands::run("export", {{"format", "dxf"}, {"out", front.string()}, {"view", "front"}, {"hidden", true}}, &doc);
  CHECK_EQ(r["bodies"], 1);
  CHECK_EQ(r["layers"]["Visible"], 4);  // the outline
  CHECK_EQ(r["layers"]["Hidden"], 2);   // the hole's sides
  const auto es = entities_of(pairs_of(read_text_file(front)));
  CHECK_EQ(count(es, "LINE", "Visible"), 4);
  CHECK_EQ(count(es, "LINE", "Hidden"), 2);
  auto round = Document::create();
  import_file(round, front);
  auto scene = resolve(round);
  Bnd_Box all;
  for (const auto& id : scene.all_bodies()) all.Add(node_world_bbox(round, scene, id));
  double x0, y0, z0, x1, y1, z1;
  all.Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(x1 - x0, 40, 1e-3);
  CHECK_NEAR(y1 - y0, 20, 1e-3);
  // From the top the hole is one exact circle; no hidden lines unless asked for.
  const auto top = f.dir / "top.dxf";
  r = commands::run("export", {{"format", "dxf"}, {"out", top.string()}, {"view", "top"}}, &doc);
  CHECK(!r["layers"].contains("Hidden") && r["entities"]["CIRCLE"] == 1);
  for (const auto& e : entities_of(pairs_of(read_text_file(top))))
    if (e.type == "CIRCLE") {
      CHECK_NEAR(e.num(40), 4, 1e-6);
      CHECK_NEAR(e.num(10), 20, 1e-6);
      CHECK_NEAR(e.num(20), 15, 1e-6);
    }
  // Any direction (the app's current camera), and SVG.
  const auto iso = f.dir / "iso.svg";
  r = commands::run("export", {{"format", "svg"}, {"out", iso.string()}, {"dir", {1, -1, 1}}, {"up", {0, 0, 1}}}, &doc);
  CHECK(r["entities"].value("ELLIPSE", 0) >= 2);  // the hole's rims seen at an angle
  round = Document::create();
  import_file(round, iso);
  CHECK(!resolve(round).all_bodies().empty());
  // Without a view the solid is seen from the top (where drawings lie); a mesh needs a view.
  r = commands::run("export", {{"format", "dxf"}, {"out", (f.dir / "plain.dxf").string()}}, &doc);
  CHECK(r["view"]["dir"][2].get<double>() > 0.99 && r["entities"]["CIRCLE"] == 1);
  write_text_file(f.dir / "mesh.obj", "v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\nf 1 2 3\nf 1 2 4\nf 1 3 4\nf 2 3 4\n");
  auto mesh = Document::create();
  import_file(mesh, f.dir / "mesh.obj");
  CHECK_THROWS(commands::run("export", {{"format", "dxf"}, {"out", (f.dir / "mesh.dxf").string()}}, &mesh));
  r = commands::run("export", {{"format", "dxf"}, {"out", (f.dir / "mesh.dxf").string()}, {"view", "front"}}, &mesh);
  CHECK(r["layers"]["Visible"].get<int>() >= 3);
  // A cancelled view stops.
  ExportOptions o;
  o.format = "dxf";
  o.view = {{"view", "right"}, {"quality", "exact"}};
  o.progress = [](double, const std::string&) { return false; };
  drawing::clear_projection_memory();
  CHECK_THROWS(export_drawing(doc, resolve(doc), f.dir / "cancelled.dxf", o));
}

TEST(sketches_and_drawings_export_exactly_on_their_own_layers) {
  Files f;
  write_text_file(f.dir / "in.dxf",
                  "0\nSECTION\n2\nENTITIES\n0\nARC\n8\nArcs\n62\n1\n10\n5\n20\n5\n40\n2\n50\n0\n51\n90\n0\nELLIPSE\n8\nArcs\n10\n0\n20\n0\n11\n10\n21\n0\n40\n0.5\n"
                  "0\nLINE\n8\nOutline\n10\n0\n20\n0\n11\n20\n21\n10\n0\nENDSEC\n0\nEOF\n");
  auto doc = Document::create();
  import_file(doc, f.dir / "in.dxf");
  const auto out = f.dir / "out.dxf";
  const auto r = commands::run("export", {{"format", "dxf"}, {"out", out.string()}}, &doc);
  CHECK_EQ(r["entities"]["ARC"], 1);  // not 128 segments any more
  CHECK_EQ(r["entities"]["ELLIPSE"], 1);
  CHECK_EQ(r["entities"]["LINE"], 1);
  const auto es = entities_of(pairs_of(read_text_file(out)));
  for (const auto& e : es) {
    if (e.type == "ARC") CHECK(e.layer == "Arcs" && e.num(62, 256) == 256);  // red as its layer is
    if (e.type == "LINE") CHECK_EQ(e.layer, "Outline");
  }
  CHECK(read_text_file(out).find("\nArcs\n 70\n0\n 62\n1\n") != std::string::npos);
}

TEST(dwg_through_libredwg_keeps_a_view) {
  // A view as DWG and back, when a converter is at hand (LibreDWG beside the program, OPAD_DXF2DWG/OPAD_DWG2DXF, ODA).
  Files f;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(40, 30, 20).Shape();
  const TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 15, -1), gp_Dir(0, 0, 1)), 4, 22).Shape();
  auto doc = Document::create();
  import_brep(doc, brep_from_shape(BRepAlgoAPI_Cut(box, hole).Shape()), "Block");
  ExportOptions o;
  o.format = "dwg";
  o.view = {{"view", "front"}, {"hidden", true}};
  try {
    export_drawing(doc, resolve(doc), f.dir / "front.dwg", o);
  } catch (const Error& e) {
    if (std::string(e.what()).find("needs a converter") == std::string::npos) throw;
    std::printf("  (no DWG converter: skipped)\n");
    return;
  }
  auto round = Document::create();
  import_file(round, f.dir / "front.dwg");
  const auto scene = resolve(round);
  std::map<std::string, int> edges;
  for (const auto& id : scene.all_bodies())
    for (TopExp_Explorer e(node_world_shape(round, scene, id), TopAbs_EDGE); e.More(); e.Next()) ++edges[scene.node(id)->name];
  CHECK_EQ(edges["Visible"], 4);
  CHECK_EQ(edges["Hidden"], 2);
}

CHECK_MAIN()
