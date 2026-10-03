// 2D drawings in the view (app/Drawing2D.cpp): the ink of a drawing without a colour on every background (UI-10), the
// layer model of the Layers manager, its changes as appearance ops and layer states in view ops (UI-89), the 2D
// vocabulary's words for what is picked (UI-118).
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>

#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <tuple>

#include "Drawing2D.hpp"
#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/drawing_io.hpp"
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
  CHECK(lineType("") == 0 && lineType("Continuous") == 0 && lineType("DASHED") == 1 && lineType("HIDDEN2") == 1 && lineType("Dot") == 2 &&
        lineType("CENTER") == 3 && lineType("PHANTOM") == 3 && lineType("DashDot") == 3 && lineType("ACAD_ISO02W100") == 1);
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

CHECK_MAIN()
