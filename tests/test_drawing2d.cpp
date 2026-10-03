// 2D drawings in the view (app/Drawing2D.cpp): the ink of a drawing without a colour on every background (UI-10), the
// layer model of the Layers manager, its changes as appearance ops and layer states in view ops (UI-89), the 2D
// vocabulary's words for what is picked (UI-118).
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>

#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include "Drawing2D.hpp"
#include "Plot2D.hpp"
#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/util.hpp"

using namespace drawing2d;

namespace {
Rgb hex(unsigned v) { return {((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0}; }

// Walls: locked, dashed, 0.5 mm. Notes: off, not plotted. Old: frozen. Plain: blue lines and a colour-7 one.
std::filesystem::path layersDxf() {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-2d-" + opad::new_uuid());
  std::filesystem::create_directory(dir);
  std::ostringstream out;
  auto g = [&](int code, const std::string& value) { out << code << '\n' << value << '\n'; };
  g(0, "SECTION"), g(2, "TABLES"), g(0, "TABLE"), g(2, "LAYER");
  using Extra = std::vector<std::pair<int, std::string>>;
  for (const auto& [name, aci, flags, extra] : std::vector<std::tuple<std::string, std::string, std::string, Extra>>{
           {"Walls", "1", "4", {{6, "DASHED"}, {370, "50"}}}, {"Notes", "-3", "0", {{290, "0"}}}, {"Old", "2", "1", {}}, {"Plain", "5", "0", {}}}) {
    g(0, "LAYER"), g(2, name), g(62, aci), g(70, flags);
    for (const auto& [code, value] : extra) g(code, value);
  }
  g(0, "ENDTAB"), g(0, "ENDSEC"), g(0, "SECTION"), g(2, "ENTITIES");
  for (const char* layer : {"Walls", "Notes", "Old", "Plain"}) g(0, "LINE"), g(8, layer), g(10, "0"), g(20, "0"), g(11, "10"), g(21, "0");
  g(0, "LINE"), g(8, "Plain"), g(62, "7"), g(10, "0"), g(20, "5"), g(11, "10"), g(21, "5");
  g(0, "ENDSEC"), g(0, "EOF");
  const auto file = dir / "layers.dxf";
  opad::write_text_file(file, out.str());
  return file;
}

std::map<std::string, Layer> byName(const opad::Scene& scene) {
  std::map<std::string, Layer> out;
  for (const auto& l : layers(scene)) out[l.name] = l;
  return out;
}

// An earlier build reads an appearance op only with one of the four fields it knows.
bool readableEarlier(const opad::json& args) {
  return args.contains("color") || args.contains("opacity") || args.contains("visible") || args.contains("locked");
}
}  // namespace

TEST(contrast_is_the_wcag_ratio) {
  CHECK(std::abs(contrast(hex(0x000000), hex(0xffffff)) - 21) < 1e-9);
  CHECK(std::abs(contrast(hex(0x777777), hex(0x777777)) - 1) < 1e-9);
  CHECK(std::abs(contrast(hex(0x777777), hex(0xffffff)) - contrast(hex(0xffffff), hex(0x777777))) < 1e-12);
  CHECK(std::abs(contrast(hex(0x767676), hex(0xffffff)) - 4.54) < 0.01);  // the classic AA grey
}

// The evaluation measured 191 on 228 (1.3:1) and 191 on 255 (1.6:1): the old grey on the light theme and on white.
TEST(colour_7_takes_the_ink_of_its_background) {
  CHECK(contrast(hex(0xbfbfc7), hex(0xe4e5e8)) < 1.5);
  const unsigned backgrounds[] = {0x232529 /* dark theme */, 0xe4e5e8 /* light theme */, 0xffffff, 0x171c24, 0x979899 /* gradient middle */,
                                  0xc7c8c9, 0x66696b, 0x000000, 0x808080};
  for (unsigned b : backgrounds) {
    const Rgb ink = drawing2d::ink(hex(b));
    CHECK(ink == kInkOnDark || ink == kInkOnLight);
    CHECK(contrast(ink, hex(b)) >= contrast(ink == kInkOnDark ? kInkOnLight : kInkOnDark, hex(b)));
  }
  CHECK(drawing2d::ink(hex(0x232529)) == kInkOnDark && drawing2d::ink(hex(0x171c24)) == kInkOnDark && drawing2d::ink(hex(0x000000)) == kInkOnDark);
  CHECK(drawing2d::ink(hex(0xe4e5e8)) == kInkOnLight && drawing2d::ink(hex(0xffffff)) == kInkOnLight && drawing2d::ink(hex(0x979899)) == kInkOnLight);
  for (unsigned b : {0x232529u, 0xe4e5e8u, 0xffffffu, 0x171c24u, 0x979899u}) CHECK(contrast(drawing2d::ink(hex(b)), hex(b)) >= 4.5);
  CHECK(kInkOnDark == hex(0xe6e6e6) && kInkOnLight == hex(0x141414));
}

TEST(a_drawings_layers_as_its_file_has_them) {
  opad::Document doc = opad::Document::create();
  opad::import_file(doc, layersDxf());
  const opad::Scene scene = opad::resolve(doc);
  auto all = byName(scene);
  CHECK_EQ(all.size(), 4u);
  CHECK(all["Walls"].on && !all["Walls"].frozen && all["Walls"].locked && all["Walls"].linetype == "DASHED" && std::abs(all["Walls"].lineweight - 0.5) < 1e-9);
  CHECK(!all["Notes"].on && !all["Notes"].frozen && !all["Notes"].plot);
  CHECK(all["Old"].on && all["Old"].frozen);  // frozen, its On kept
  CHECK(all["Plain"].on && all["Plain"].plot && all["Plain"].linetype.empty() && all["Plain"].lineweight < 0);
  CHECK(all["Walls"].colored && !all["Walls"].mixed && all["Walls"].color == hex(0xff0000) && all["Walls"].own == 0);
  // Blue lines by layer and a colour-7 one: the layer's colour is blue, the colour-7 line keeps its own.
  CHECK(all["Plain"].bodies.size() == 2 && all["Plain"].byLayer.size() == 1 && all["Plain"].own == 1);
  CHECK(all["Plain"].colored && !all["Plain"].mixed && all["Plain"].color == hex(0x0000ff) && scene.node(all["Plain"].byLayer.at(0))->has_color);
  CHECK(all["Walls"].drawing == "layers");
  CHECK(layerOf(scene, all["Walls"].bodies.at(0)) == all["Walls"].id && layerOf(scene, all["Walls"].id) == all["Walls"].id);
  CHECK(layerOf(scene, scene.roots.at(0)).empty());
  CHECK(isLayer(scene, all["Old"].id) && !isLayer(scene, scene.roots.at(0)));
}

TEST(layer_changes_are_appearance_ops_an_earlier_build_reads) {
  opad::Document doc = opad::Document::create();
  opad::import_file(doc, layersDxf());
  auto run = [&](const opad::json& args) {
    const size_t before = doc.ops.size();
    opad::commands::run("appearance", args, &doc);
    for (size_t i = before; i < doc.ops.size(); ++i) CHECK(readableEarlier(doc.ops[i].data));
    return byName(opad::resolve(doc));
  };
  auto visible = [&](const std::string& id) { return opad::resolve(doc).node(id)->visible; };
  auto all = byName(opad::resolve(doc));
  all = run(setOn(all["Notes"], true));
  CHECK(all["Notes"].on && !all["Notes"].frozen && visible(all["Notes"].id));
  all = run(setFrozen(all["Notes"], true));
  CHECK(all["Notes"].on && all["Notes"].frozen && !visible(all["Notes"].id));
  all = run(setOn(all["Notes"], false));
  CHECK(!all["Notes"].on && all["Notes"].frozen);
  all = run(setFrozen(all["Notes"], false));
  CHECK(!all["Notes"].on && !all["Notes"].frozen && !visible(all["Notes"].id));
  all = run(setFrozen(all["Old"], false));  // thawed: on as its file had it
  CHECK(all["Old"].on && !all["Old"].frozen && visible(all["Old"].id));
  all = run(setLocked(all["Walls"], false));
  CHECK(!all["Walls"].locked);
  all = run(setLinetype(all["Walls"], "Continuous"));
  CHECK(all["Walls"].linetype.empty() && !opad::resolve(doc).node(all["Walls"].id)->layer.contains("linetype"));
  all = run(setLinetype(all["Plain"], "Center"));
  CHECK(all["Plain"].linetype == "Center");
  all = run(setLineweight(all["Plain"], 0.35));
  CHECK(std::abs(all["Plain"].lineweight - 0.35) < 1e-9);
  all = run(setLineweight(all["Plain"], -1));
  CHECK(all["Plain"].lineweight < 0);
  all = run(setPlot(all["Notes"], true));
  CHECK(all["Notes"].plot);
  all = run(setColor(all["Plain"], {0, 1, 0}));
  CHECK(all["Plain"].colored && !all["Plain"].mixed && all["Plain"].color == Rgb({0, 1, 0}));
  auto ownColour = [&](const Layer& l) {  // the colour-7 line's body: never given the layer's colour
    for (const auto& b : l.bodies)
      if (std::find(l.byLayer.begin(), l.byLayer.end(), b) == l.byLayer.end()) return opad::resolve(doc).node(b)->has_color;
    return true;
  };
  CHECK(!ownColour(all["Plain"]));
  all = run(setDefaultColor(all["Plain"]));  // back to blue
  CHECK(all["Plain"].colored && all["Plain"].color == hex(0x0000ff) && !ownColour(all["Plain"]));
  // The command keeps visible along with a layer field given alone.
  const auto id = all["Plain"].id;
  opad::commands::run("appearance", {{"target", id}, {"layer", {{"plot", false}}}}, &doc);
  CHECK(doc.ops.back().data.value("visible", false) && doc.ops.back().data["layer"]["plot"] == false);
  // Lines drawn as their layer says.
  using Dashes = std::vector<double>;
  CHECK(dashes("").empty() && dashes("Continuous").empty() && dashes("BYLAYER").empty());
  CHECK(dashes("DASHED") == Dashes({12.7, -6.35}) && dashes("Dashed2") == Dashes({6.35, -3.175}) && dashes("DASHEDX2") == Dashes({25.4, -12.7}) &&
        dashes("HIDDEN") == Dashes({6.35, -3.175}) && dashes("ACAD_ISO02W100") == Dashes({12, -3}) && dashes("Center") == Dashes({31.75, -6.35, 6.35, -6.35}));
  CHECK(dashes("CENTERLINE") == dashes("CENTER") && dashes("Dash_Dot") == dashes("DASHDOT") && dashes("DOTTED") == dashes("DOT") && dashes("FENCE") == dashes("DASHED"));
  CHECK(dashes("CUSTOM", {5, -1, 0, -1}) == Dashes({5, -1, 0, -1}) && dashes("CUSTOM", {5}).empty());  // the file's own; no gap: continuous
  all = run(setLinetype(all["Plain"], "FENCE", {5, -1, 0, -1}));
  CHECK(all["Plain"].linetype == "FENCE" && all["Plain"].pattern == Dashes({5, -1, 0, -1}));
  all = run(setLinetype(all["Plain"], "Dashed"));  // the pattern goes with the name
  CHECK(all["Plain"].linetype == "Dashed" && all["Plain"].pattern.empty());
  CHECK(linePoints(-1) == 1 && linePoints(0) == 1 && linePoints(0.25) == 1 && linePoints(0.3) == 2 && linePoints(0.5) == 2 && linePoints(1.0) == 4 &&
        linePoints(2.11) == 8);
}

TEST(layer_states_come_back_in_one_go) {
  opad::Document doc = opad::Document::create();
  opad::import_file(doc, layersDxf());
  const opad::json saved = captureState(opad::resolve(doc));
  opad::commands::run("view", {{"name", "As drawn"}, {"camera", {{"eye", {0, 0, 1}}, {"target", {0, 0, 0}}, {"up", {0, 1, 0}}}}, {"display", saved}}, &doc);
  auto all = byName(opad::resolve(doc));
  CHECK(saved["layers"][all["Plain"].id]["color"] == opad::json({0.0, 0.0, 1.0}));  // the layer's colour, its colour-7 line aside
  for (const auto& args : {setOn(all["Notes"], true), setFrozen(all["Walls"], true), setLocked(all["Walls"], false), setLinetype(all["Plain"], "Dot"),
                           setLineweight(all["Walls"], 1.0), setPlot(all["Notes"], true), setColor(all["Walls"], {0, 0, 1}), setColor(all["Plain"], {0, 1, 0})})
    opad::commands::run("appearance", args, &doc);
  // Saved, loaded back: the view keeps its layers.
  const auto file = std::filesystem::temp_directory_path() / ("opad-2d-" + opad::new_uuid() + ".opad");
  doc.save_as(file);
  opad::Document loaded = opad::Document::load(file);
  std::filesystem::remove(file);
  opad::Scene scene = opad::resolve(loaded);
  CHECK_EQ(scene.views.size(), 1u);
  CHECK(scene.views.at(0).display == saved);
  const auto ops = restoreState(scene, scene.views.at(0).display);
  CHECK(!ops.empty());
  for (const auto& args : ops) {
    const size_t before = loaded.ops.size();
    opad::commands::run("appearance", args, &loaded);
    for (size_t i = before; i < loaded.ops.size(); ++i) CHECK(readableEarlier(loaded.ops[i].data));
  }
  scene = opad::resolve(loaded);
  CHECK(captureState(scene) == saved);
  CHECK(restoreState(scene, saved).empty());  // nothing left to change
  // By name when the ids differ (the drawing imported again).
  opad::Document again = opad::Document::create();
  opad::import_file(again, layersDxf());
  all = byName(opad::resolve(again));
  opad::commands::run("appearance", setOn(all["Notes"], true), &again);
  const auto byNames = restoreState(opad::resolve(again), saved);
  CHECK_EQ(byNames.size(), 1u);
  CHECK(byNames.at(0)["target"] == all["Notes"].id && byNames.at(0)["visible"] == false);
}

// A linetype as the view draws it: a 16-bit stipple of its dashes, each dash and gap close to its length on screen.
TEST(linetypes_draw_as_their_dashes) {
  using Dashes = std::vector<double>;
  auto runs = [](LinePattern p) {  // on/off runs of bits, around the end
    std::vector<std::pair<bool, int>> r;
    for (int b = 15; b >= 0; --b) {
      const bool on = (p.bits >> b) & 1;
      if (!r.empty() && r.back().first == on) ++r.back().second;
      else r.push_back({on, 1});
    }
    if (r.size() > 1 && r.front().first == r.back().first) r.front().second += r.back().second, r.pop_back();
    return r;
  };
  const double px = kPatternPixelsPerMm;
  CHECK(linePattern({}, px) == LinePattern{} && linePattern({5}, px) == LinePattern{} && linePattern(dashes("Continuous"), px) == LinePattern{});
  std::set<std::pair<int, int>> distinct;
  for (const char* name : {"DASHED", "HIDDEN", "CENTER", "PHANTOM", "DOT", "DASHDOT", "BORDER", "DIVIDE", "ACAD_ISO02W100", "DASHEDX2"}) {
    const auto d = dashes(name);
    const LinePattern p = linePattern(d, px);
    distinct.insert({p.bits, p.factor});
    const auto r = runs(p);
    int ons = 0, elements = 0;
    double period = 0;
    for (double v : d) period += std::abs(v) * px, ons += v >= 0;
    for (const auto& run : r) elements += run.first;
    CHECK(elements % ons == 0);  // whole periods in 16 bits
    const double shown = 16.0 * p.factor / (elements / ons);
    CHECK(std::abs(shown - period) <= 0.25 * period + p.factor);  // its size on screen
    for (const auto& run : r)
      if (run.first && std::string(name) == "DOT") CHECK(run.second == 1);  // dots one bit
  }
  CHECK_EQ(distinct.size(), 10u);  // each draws differently: HIDDEN is a smaller DASHED, CENTER and PHANTOM differ by a dot
  const auto center = runs(linePattern(dashes("CENTER"), px));
  CHECK(center.size() == 4 && center[0].first && center[0].second >= 3 * center[2].second);
  // Larger on a denser screen, the same shape.
  const LinePattern one = linePattern(dashes("CENTER"), px), two = linePattern(dashes("CENTER"), 2 * px);
  CHECK(two.factor >= 2 * one.factor - 1 && runs(two).size() == 4);
  // Dashes more than 16 bits hold: a plain dash.
  CHECK(linePattern(Dashes(20, 1.0) /* all on */, px) == LinePattern{});
  std::vector<double> many;
  for (int i = 0; i < 10; ++i) many.insert(many.end(), {1.0, -1.0});
  CHECK(linePattern(many, px).bits != 0xFFFF);
}

// UI-92: a body's own linetype and lineweight (DXF entities that set them) win over its layer's, also after the layer
// changes; the rest follow the layer; a plot draws each in its own. A linetype scale of its own (CELTSCALE) sizes the
// dashes of whichever linetype it takes.
TEST(own_linetypes_and_lineweights_win_over_the_layers) {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-2d-" + opad::new_uuid());
  std::filesystem::create_directory(dir);
  std::ostringstream out;
  auto g = [&](int code, const std::string& value) { out << code << '\n' << value << '\n'; };
  g(0, "SECTION"), g(2, "TABLES"), g(0, "TABLE"), g(2, "LAYER"), g(0, "LAYER"), g(2, "Walls"), g(62, "1"), g(70, "0"), g(6, "DASHED"), g(370, "50");
  g(0, "ENDTAB"), g(0, "ENDSEC"), g(0, "SECTION"), g(2, "ENTITIES");
  g(0, "LINE"), g(8, "Walls"), g(10, "0"), g(20, "0"), g(11, "10"), g(21, "0");
  g(0, "LINE"), g(8, "Walls"), g(6, "CENTER"), g(10, "0"), g(20, "1"), g(11, "10"), g(21, "1");
  g(0, "LINE"), g(8, "Walls"), g(370, "100"), g(10, "0"), g(20, "2"), g(11, "10"), g(21, "2");
  g(0, "LINE"), g(8, "Walls"), g(48, "0.5"), g(10, "0"), g(20, "3"), g(11, "10"), g(21, "3");
  g(0, "ENDSEC"), g(0, "EOF");
  opad::write_text_file(dir / "own.dxf", out.str());
  opad::Document doc = opad::Document::create();
  opad::import_file(doc, dir / "own.dxf");
  auto styles = [&doc] {
    const opad::Scene scene = opad::resolve(doc);
    std::multiset<std::tuple<std::string, double, bool, bool, double>> out;
    for (const auto& id : scene.all_bodies()) {
      const LineStyle s = lineStyle(scene, *scene.node(id));
      out.insert({s.linetype, s.lineweight, s.ownType, s.ownWeight, s.scale});
    }
    return out;
  };
  using Styles = std::multiset<std::tuple<std::string, double, bool, bool, double>>;
  CHECK(styles() == (Styles{{"DASHED", 0.5, false, false, 1}, {"CENTER", 0.5, true, false, 1}, {"DASHED", 1.0, false, true, 1}, {"DASHED", 0.5, false, false, 0.5}}));
  const opad::Scene scene = opad::resolve(doc);
  opad::commands::run("appearance", setLinetype(byName(scene)["Walls"], "HIDDEN"), &doc);
  opad::commands::run("appearance", setLineweight(byName(opad::resolve(doc))["Walls"], 0.35), &doc);
  CHECK(styles() == (Styles{{"HIDDEN", 0.35, false, false, 1}, {"CENTER", 0.35, true, false, 1}, {"HIDDEN", 1.0, false, true, 1}, {"HIDDEN", 0.35, false, false, 0.5}}));
  const opad::Scene now = opad::resolve(doc);
  const plot::Sheet sheet = plot::collect(doc, now, plot::plane(doc, now, opad::Frame{}));
  std::set<std::pair<std::vector<double>, double>> drawn;
  for (const auto& s : sheet.styles) drawn.insert({s.dashes, s.weight});
  std::vector<double> half = dashes("HIDDEN");
  for (double& d : half) d *= 0.5;
  CHECK(drawn == (std::set<std::pair<std::vector<double>, double>>{{dashes("HIDDEN"), 0.35}, {dashes("CENTER"), 0.35}, {dashes("HIDDEN"), 1.0}, {half, 0.35}}));
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}

// An import that does not say which bodies are in their layer's colour (an earlier build's, an SVG): a layer colour is
// given to all of its bodies, and a layer state keeps a colour only when they share one.
TEST(a_layer_colour_without_by_layer_marks_colours_every_body) {
  opad::Document doc = opad::Document::create();
  opad::import_file(doc, layersDxf());
  std::function<void(opad::json&)> unmark = [&](opad::json& nodes) {
    for (auto& n : nodes) {
      n.erase("by_layer");
      if (n.contains("children")) unmark(n["children"]);
    }
  };
  for (auto& op : doc.ops)
    if (op.type == "import") unmark(op.data["nodes"]);
  auto all = byName(opad::resolve(doc));
  CHECK(all["Plain"].byLayer == all["Plain"].bodies && all["Plain"].own == 0 && all["Plain"].mixed);
  CHECK(!captureState(opad::resolve(doc))["layers"][all["Plain"].id].contains("color"));
  opad::commands::run("appearance", setColor(all["Plain"], {0, 1, 0}), &doc);
  const opad::Scene scene = opad::resolve(doc);
  for (const auto& b : all["Plain"].bodies) CHECK(scene.node(b)->has_color && scene.node(b)->color == Rgb({0, 1, 0}));
  CHECK(captureState(scene)["layers"][all["Plain"].id]["color"] == opad::json({0.0, 1.0, 0.0}));
}

TEST(what_a_drawing_entity_is_called) {
  const auto line = entityInfo(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 0, 0), gp_Pnt(30, 40, 0)).Edge());
  CHECK(line["type"] == "line" && std::abs(line["length"].get<double>() - 50) < 1e-9 && !line.contains("radius"));
  const gp_Circ circle(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5);
  const auto full = entityInfo(BRepBuilderAPI_MakeEdge(circle).Edge());
  CHECK(full["type"] == "circle" && std::abs(full["radius"].get<double>() - 5) < 1e-9 && std::abs(full["length"].get<double>() - 10 * M_PI) < 1e-6);
  const auto arc = entityInfo(BRepBuilderAPI_MakeEdge(circle, 0, M_PI / 2).Edge());
  CHECK(arc["type"] == "arc" && std::abs(arc["length"].get<double>() - 2.5 * M_PI) < 1e-6);
  const auto square = entityInfo(BRepBuilderAPI_MakeFace(BRepBuilderAPI_MakePolygon(gp_Pnt(0, 0, 0), gp_Pnt(2, 0, 0), gp_Pnt(2, 3, 0), gp_Pnt(0, 3, 0), true).Wire()).Face());
  CHECK(square["type"] == "fill" && std::abs(square["area"].get<double>() - 6) < 1e-9);
  CHECK(entityInfo(BRepBuilderAPI_MakeVertex(gp_Pnt(1, 2, 0)).Vertex())["type"] == "point");
  CHECK(entityInfo(TopoDS_Shape()).is_null());
  CHECK(std::string(kindWord(opad::Ref::Kind::Body)) == "group" && std::string(kindWord(opad::Ref::Kind::Edge)) == "object" &&
        std::string(kindWord(opad::Ref::Kind::Vertex)) == "point" && std::string(kindWord(opad::Ref::Kind::Face)) == "fill");
  // A drawing-only scene: bodies, all drawings.
  opad::Document doc = opad::Document::create();
  CHECK(!drawingOnly(opad::resolve(doc)) && !hasDrawings(opad::resolve(doc)));
  opad::import_file(doc, layersDxf());
  CHECK(drawingOnly(opad::resolve(doc)) && hasDrawings(opad::resolve(doc)));
  opad::commands::run("feature", {{"kind", "box"}, {"inputs", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}}}}, &doc);
  const opad::Scene scene = opad::resolve(doc);
  CHECK(!drawingOnly(scene) && hasDrawings(scene));
  // Browser rows: the drawing, its layers, their bodies; a solid is an object.
  auto all = byName(scene);
  const std::string root = scene.node(all["Walls"].id)->parent;
  CHECK(std::string(nodeWord(scene, root)) == "drawing" && std::string(nodeWord(scene, all["Walls"].id)) == "layer" &&
        std::string(nodeWord(scene, all["Walls"].bodies.at(0))) == "group");
  for (const auto& id : scene.all_bodies())
    if (scene.node(id)->representation != "drawing2d") CHECK(std::string(nodeWord(scene, id)) == "object");
  // One layer without a walk: the same as in the list, from a body or the layer itself.
  for (const auto& [name, l] : all) {
    const auto one = layerAt(scene, l.bodies.at(0)), self = layerAt(scene, l.id);
    CHECK(one && self && one->id == l.id && one->drawing == l.drawing && one->byLayer == l.byLayer && one->color == l.color && one->on == l.on &&
          one->frozen == l.frozen && one->locked == l.locked && one->linetype == l.linetype && self->id == l.id);
  }
  CHECK(!layerAt(scene, root) && !layerAt(scene, "nothing"));
}

// Properties of a drawing in 2D words (UI-118): what a picked entity is, its counts as fills, objects and points, no solid words.
TEST(drawing_properties_in_2d_words) {
  using opad::json;
  CHECK(entityType({{"type", "edge"}, {"curve", "line"}}) == "line" && entityType({{"type", "vertex"}}) == "point" && entityType({{"type", "face"}}) == "fill" &&
        entityType({{"type", "edge"}, {"curve", "bspline"}}) == "spline" && entityType({{"type", "edge"}, {"curve", "other"}}) == "curve" &&
        entityType({{"type", "center"}}).empty());
  CHECK(entityType({{"type", "edge"}, {"curve", "circle"}, {"radius", 5}, {"start", {5, 0, 0}}, {"end", {5, 0, 0}}}) == "circle");
  CHECK(entityType({{"type", "edge"}, {"curve", "circle"}, {"radius", 5}, {"start", {5, 0, 0}}, {"end", {0, 5, 0}}}) == "arc");
  const json body = properties({{"faces", 2}, {"edges", 9}, {"vertices", 8}, {"solid", false}, {"area", 12.5}, {"name", "Walls"}, {"bbox", json::object()}});
  CHECK(body == json({{"area", 12.5}, {"name", "Walls"}, {"bbox", json::object()}, {"fills", 2}, {"objects", 9}, {"points", 8}}));  // in order
  const json edge = properties({{"type", "edge"}, {"curve", "circle"}, {"length", 3.14}, {"axis", {0, 0, 1}}, {"radius", 1}, {"adjacent_faces", json::array()},
                                {"vertices", {0, 1}}, {"start", {1, 0, 0}}, {"end", {-1, 0, 0}}});
  CHECK(edge == json({{"type", "edge"}, {"curve", "circle"}, {"length", 3.14}, {"radius", 1}, {"start", {1, 0, 0}}, {"end", {-1, 0, 0}}}));
  const json fill = properties({{"type", "face"}, {"surface", "plane"}, {"normal", {0, 0, 1}}, {"origin", {0, 0, 0}}, {"area", 4}, {"edges", {1, 2}}, {"adjacent_faces", {3}}});
  CHECK(fill == json({{"type", "face"}, {"area", 4}}));
}

// What a plot draws (UI-88): visible bodies of plotted layers (not Notes, off and unplotted, nor Old, frozen), each with its
// colour (or the ink), lineweight and linetype; a layer turned on but left out of plots stays out.
TEST(a_plot_draws_the_visible_plotted_layers) {
  opad::Document doc = opad::Document::create();
  opad::import_file(doc, layersDxf());
  opad::Scene scene = opad::resolve(doc);
  const opad::Frame plane = plot::plane(doc, scene, opad::Frame{});
  plot::Sheet sheet = plot::collect(doc, scene, plane);
  CHECK_EQ(sheet.bodies, 3);  // Walls, Plain's blue lines, Plain's colour-7 line
  CHECK_EQ(sheet.items.size(), 3u);
  CHECK(std::abs(sheet.x0) < 1e-9 && std::abs(sheet.x1 - 10) < 1e-9 && std::abs(sheet.y0) < 1e-9 && std::abs(sheet.y1 - 5) < 1e-9);
  int dashed = 0, inked = 0, blue = 0;
  for (const auto& s : sheet.styles) {
    dashed += !s.dashes.empty() && std::abs(s.weight - 0.5) < 1e-9;
    inked += s.ink;
    blue += !s.ink && s.color == hex(0x0000ff);
  }
  CHECK(dashed == 1 && inked == 1 && blue == 1 && sheet.styles.size() == 3);
  auto all = byName(scene);
  opad::commands::run("appearance", setOn(all["Notes"], true), &doc);
  scene = opad::resolve(doc);
  CHECK_EQ(plot::collect(doc, scene, plane).bodies, 3);  // shown now, still not plotted
  opad::commands::run("appearance", setPlot(byName(scene)["Notes"], true), &doc);
  scene = opad::resolve(doc);
  CHECK_EQ(plot::collect(doc, scene, plane).bodies, 4);
  // Paper: black for the ink and in monochrome, the layer's lineweight (0.25 mm by default), the thinnest without lineweights.
  plot::Settings settings;
  const plot::Style& walls = *std::find_if(sheet.styles.begin(), sheet.styles.end(), [](const plot::Style& s) { return !s.dashes.empty(); });
  CHECK(plot::paperColor(walls, settings) == hex(0xff0000));
  settings.monochrome = true;
  CHECK(plot::paperColor(walls, settings) == hex(0x000000));
  CHECK(std::abs(plot::paperWeight(walls, settings) - 0.5) < 1e-12);
  plot::Style plain;
  CHECK(std::abs(plot::paperWeight(plain, settings) - plot::kDefaultWeight) < 1e-12);
  settings.lineweights = false;
  CHECK(std::abs(plot::paperWeight(walls, settings) - plot::kThinnest) < 1e-12);
}

// Where a plot lands (UI-88): fit to the printable rectangle and centred, 1:N at its own size (clipped when too big), the
// display's and a window's area; the scale as people say it.
TEST(a_plot_fits_or_takes_its_scale) {
  plot::Sheet sheet;
  sheet.x0 = 0, sheet.y0 = 0, sheet.x1 = 1000, sheet.y1 = 500;  // a 1 m by 0.5 m drawing
  sheet.items.push_back({});
  plot::Settings s;  // A4 landscape, 10 mm margins: 277 x 190 printable
  plot::Placement p = plot::place(sheet, s);
  CHECK(p.valid() && !p.clipped);
  CHECK_NEAR(p.scale, 0.277, 1e-12);
  CHECK_NEAR(p.x, 10, 1e-9);
  CHECK_NEAR(p.y, 10 + (190 - 500 * 0.277) / 2, 1e-9);
  s.fit = false;
  s.scale = 1.0 / 5;
  p = plot::place(sheet, s);
  CHECK(!p.clipped && std::abs(p.scale - 0.2) < 1e-12 && std::abs(p.x - (10 + (277 - 200) / 2.0)) < 1e-9);
  s.scale = 1;  // 1:1 does not fit on A4
  CHECK(plot::place(sheet, s).clipped);
  s.fit = true;
  s.region = plot::Region::Window;
  s.window = {600, 400, 200, 100};  // corners either way round
  p = plot::place(sheet, s);
  CHECK(std::abs(p.area.x0 - 200) < 1e-12 && std::abs(p.area.y1 - 400) < 1e-12 && std::abs(p.scale - std::min(277 / 400.0, 190 / 300.0)) < 1e-12);
  s.region = plot::Region::Display;
  s.display = {0, 0, 100, 50};
  CHECK_NEAR(plot::place(sheet, s).scale, 2.77, 1e-12);
  plot::Sheet line;  // one horizontal line: room for its width
  line.x0 = 0, line.x1 = 100, line.y0 = line.y1 = 0;
  line.items.push_back({});
  CHECK(plot::place(line, plot::Settings{}).valid());
  CHECK(!plot::place(plot::Sheet{}, plot::Settings{}).valid());
  CHECK(plot::scaleText(0.02) == "1:50" && plot::scaleText(2) == "2:1" && plot::scaleText(1 / 37.4249) == "1:37.42" && plot::scaleText(1) == "1:1");
}

// A drawing's raster image is plotted (UI-88): its data and corners where it is placed, inside the extents, left out with
// its layer.
TEST(a_plot_takes_a_drawings_images) {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-2d-" + opad::new_uuid());
  std::filesystem::create_directory(dir);
  const std::string png = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";
  opad::write_text_file(dir / "picture.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200mm\" height=\"100mm\" viewBox=\"0 0 200 100\">"
                                             "<path d=\"M 0 0 L 200 0 L 200 100 L 0 100 Z\" fill=\"none\" stroke=\"black\"/>"
                                             "<image x=\"20\" y=\"10\" width=\"60\" height=\"40\" href=\"" + png + "\"/></svg>");
  auto doc = opad::Document::create();
  opad::import_file(doc, dir / "picture.svg");
  auto scene = opad::resolve(doc);
  const plot::Sheet sheet = plot::collect(doc, scene, plot::plane(doc, scene, opad::Frame{}));
  CHECK_EQ(sheet.images.size(), 1u);
  CHECK_EQ(sheet.bodies, 2);
  if (!sheet.images.empty()) {
    const plot::Image& image = sheet.images[0];
    CHECK(image.href == png);
    CHECK_NEAR(std::hypot(image.right[0] - image.origin[0], image.right[1] - image.origin[1]), 60, 1e-6);  // its frame, 60 x 40 mm
    CHECK_NEAR(std::hypot(image.down[0] - image.origin[0], image.down[1] - image.origin[1]), 40, 1e-6);
    CHECK_NEAR(image.origin[1] - image.down[1], 40, 1e-6);  // the picture's top above its bottom, as the drawing's y goes up
    for (const auto& p : {image.origin, image.right, image.down})
      CHECK(p[0] >= sheet.x0 - 1e-9 && p[0] <= sheet.x1 + 1e-9 && p[1] >= sheet.y0 - 1e-9 && p[1] <= sheet.y1 + 1e-9);
  }
  for (const Layer& layer : layers(scene))  // the image's layer left out of plots: no image
    if (std::any_of(layer.bodies.begin(), layer.bodies.end(), [&](const std::string& id) { return !scene.node(id)->raster.is_null(); }))
      opad::commands::run("appearance", setPlot(layer, false), &doc);
  scene = opad::resolve(doc);
  CHECK(plot::collect(doc, scene, plot::plane(doc, scene, opad::Frame{})).images.empty());
  std::filesystem::remove_all(dir);
}

// The cursor readout's drawing coordinates (UI-90): a drawing read far from (0,0) keeps the offset on its root, so a world
// point reads as the file has it whether the drawing was opened centred or placed where it is, and after a reload.
TEST(drawing_coordinates_of_a_far_drawing) {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-2d-" + opad::new_uuid());
  std::filesystem::create_directory(dir);
  opad::write_text_file(dir / "far.dxf", "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nSite\n10\n1000010\n20\n2000020\n11\n1000110\n21\n2000020\n0\nENDSEC\n0\nEOF\n");
  for (const bool centre : {false, true}) {
    auto doc = opad::Document::create();
    opad::ImportOptions options;
    options.center_drawing = centre;
    opad::import_file(doc, dir / "far.dxf", options);
    doc = opad::Document::parse(doc.serialize());  // what a reload reads
    const auto scene = opad::resolve(doc);
    const auto frames = drawingFrames(doc, scene);
    CHECK_EQ(frames.size(), 1u);
    CHECK_NEAR(frames[0].origin[0], 1000000, 1e-6);
    CHECK_NEAR(frames[0].origin[1], 2000000, 1e-6);
    CHECK(frames[0].x0 < 1000010 + 1 && frames[0].x1 > 1000110 - 1 && frames[0].y0 < 2000020 + 1 && frames[0].y1 > 2000020 - 1);
    std::set<long> xs;
    for (TopExp_Explorer v(opad::node_world_shape(doc, scene, scene.all_bodies().at(0)), TopAbs_VERTEX); v.More(); v.Next()) {
      const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
      const opad::Vec3 d = toDrawing(frames[0], {p.X(), p.Y(), p.Z()});
      CHECK_NEAR(d[1], 2000020, 1e-6);
      xs.insert(std::lround(d[0]));
      const opad::Vec3 back = fromDrawing(frames[0], d);
      CHECK_NEAR(back[0], p.X(), 1e-6);
      CHECK_NEAR(back[1], p.Y(), 1e-6);
    }
    CHECK(xs == std::set<long>({1000010, 1000110}));
    const opad::Frame plane = planeOf(frames[0]);
    CHECK_NEAR(plane.x[0], 1, 1e-12);
  }
  std::filesystem::remove_all(dir);
}

CHECK_MAIN()
