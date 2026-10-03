// DXF as real CAD drawings carry it (blocks, hatches, text, colours, survey coordinates, old code pages) and DWG
// through LibreDWG's dwg2dxf, which the build puts beside the test programs.
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopAbs.hxx>
#include <TopExp_Explorer.hxx>

#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/step_io.hpp"

using namespace opad;

namespace {
struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-dxf-" + new_uuid());
  Files() { std::filesystem::create_directory(dir); }
  ~Files() { std::error_code e; std::filesystem::remove_all(dir, e); }
};

using Groups = std::vector<std::pair<int, std::string>>;
std::string dxf(const Groups& groups) {
  std::ostringstream out;
  for (const auto& [code, value] : groups) out << code << '\n' << value << '\n';
  return out.str();
}
std::string section(const std::string& name, const Groups& body) {
  Groups g{{0, "SECTION"}, {2, name}};
  g.insert(g.end(), body.begin(), body.end());
  g.push_back({0, "ENDSEC"});
  return dxf(g);
}
const std::string kEof = "0\nEOF\n";

struct Body {
  std::string layer;
  bool has_color = false;
  std::array<double, 3> color{};
  Bnd_Box box;
  int faces = 0, edges = 0;
  double area = 0;
};
std::vector<Body> bodies(const Document& d) {
  std::vector<Body> out;
  const Scene s = resolve(d);
  for (const auto& id : s.all_bodies()) {
    const auto* n = s.node(id);
    Body b;
    b.layer = n->name;
    b.has_color = n->has_color;
    b.color = n->color;
    const auto shape = node_world_shape(d, s, id);
    BRepBndLib::Add(shape, b.box);
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) ++b.faces;
    for (TopExp_Explorer e(shape, TopAbs_EDGE); e.More(); e.Next()) ++b.edges;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(shape, props);
    b.area = props.Mass();
    out.push_back(b);
  }
  return out;
}
const Body* find(const std::vector<Body>& all, const std::string& layer, double r, double g, double b) {
  for (const auto& body : all)
    if (body.layer == layer && body.has_color && std::abs(body.color[0] - r) < 1e-6 && std::abs(body.color[1] - g) < 1e-6 &&
        std::abs(body.color[2] - b) < 1e-6)
      return &body;
  return nullptr;
}
const Body* find(const std::vector<Body>& all, const std::string& layer) {
  for (const auto& body : all)
    if (body.layer == layer) return &body;
  return nullptr;
}
bool near_box(const Bnd_Box& box, double x0, double y0, double x1, double y1, double tol = 1e-3) {
  if (box.IsVoid()) return false;
  double a, b, c, d, e, f;
  box.Get(a, b, c, d, e, f);
  return std::abs(a - x0) < tol && std::abs(b - y0) < tol && std::abs(d - x1) < tol && std::abs(e - y1) < tol;
}
std::array<double, 4> extent(const Bnd_Box& box) {
  double a, b, c, d, e, f;
  box.Get(a, b, c, d, e, f);
  return {a, b, d, e};
}
Document import(const std::filesystem::path& file, bool viewer = true, bool center = false, ImportResult* result = nullptr) {
  Document d = Document::create();
  ImportOptions o;
  o.viewer = viewer;
  o.center_drawing = center;
  const auto r = import_file(d, file, o);
  if (result) *result = r;
  return d;
}
}  // namespace

TEST(blocks_land_rotated_mirrored_scaled_and_flipped_in_their_colours) {
  Files f;
  const std::string text =
      section("TABLES", {{0, "TABLE"}, {2, "LAYER"}, {0, "LAYER"}, {2, "A"}, {62, "3"}, {70, "0"}, {0, "ENDTAB"}}) +
      section("BLOCKS", {{0, "BLOCK"}, {2, "B"}, {70, "0"}, {10, "0"}, {20, "0"}, {30, "0"},
                         {0, "LINE"}, {8, "0"}, {62, "0"}, {10, "0"}, {20, "0"}, {11, "10"}, {21, "0"},
                         {0, "ENDBLK"}}) +
      section("ENTITIES", {
          {0, "INSERT"}, {8, "A"}, {62, "1"}, {2, "B"}, {10, "100"}, {20, "0"}, {50, "90"},           // red, turned
          {0, "INSERT"}, {8, "A"}, {2, "B"}, {10, "200"}, {20, "0"}, {41, "-1"}, {42, "1"},            // layer green, mirrored
          {0, "INSERT"}, {8, "C"}, {62, "5"}, {2, "B"}, {10, "300"}, {20, "0"}, {41, "2"}, {42, "2"},  // blue, doubled
          {0, "INSERT"}, {8, "D"}, {62, "2"}, {2, "B"}, {10, "400"}, {20, "0"}, {210, "0"}, {220, "0"}, {230, "-1"},
          {0, "MINSERT"}, {8, "E"}, {2, "B"}, {10, "500"}, {20, "0"}, {70, "3"}, {71, "1"}, {44, "20"}, {45, "0"},
      }) +
      kEof;
  write_text_file(f.dir / "blocks.dxf", text);
  for (bool viewer : {true, false}) {
    const auto all = bodies(import(f.dir / "blocks.dxf", viewer));
    const auto* turned = find(all, "A", 1, 0, 0);
    const auto* mirrored = find(all, "A", 0, 1, 0);
    const auto* doubled = find(all, "C", 0, 0, 1);
    const auto* flipped = find(all, "D", 1, 1, 0);
    const auto* array = find(all, "E");
    CHECK(turned && near_box(turned->box, 100, 0, 100, 10));
    CHECK(mirrored && near_box(mirrored->box, 190, 0, 200, 0));
    CHECK(doubled && near_box(doubled->box, 300, 0, 320, 0));
    CHECK(flipped && near_box(flipped->box, -410, 0, -400, 0));  // extrusion -Z: the plane seen from below
    CHECK(array && near_box(array->box, 500, 0, 550, 0) && array->edges == 3);
  }
}

TEST(hatches_fill_with_holes_draw_patterns_and_read_clockwise_arcs) {
  Files f;
  const Groups square_with_hole = {
      {0, "HATCH"}, {8, "S"}, {10, "0"}, {20, "0"}, {30, "0"}, {210, "0"}, {220, "0"}, {230, "1"}, {2, "SOLID"}, {70, "1"}, {71, "0"},
      {91, "2"},
      {92, "2"}, {72, "0"}, {73, "1"}, {93, "4"}, {10, "0"}, {20, "0"}, {10, "10"}, {20, "0"}, {10, "10"}, {20, "10"}, {10, "0"}, {20, "10"}, {97, "0"},
      {92, "16"}, {93, "4"},
      {72, "1"}, {10, "2"}, {20, "2"}, {11, "8"}, {21, "2"}, {72, "1"}, {10, "8"}, {20, "2"}, {11, "8"}, {21, "8"},
      {72, "1"}, {10, "8"}, {20, "8"}, {11, "2"}, {21, "8"}, {72, "1"}, {10, "2"}, {20, "8"}, {11, "2"}, {21, "2"}, {97, "0"},
      {75, "0"}, {76, "1"}, {98, "0"}};
  const Groups pattern = {
      {0, "HATCH"}, {8, "P"}, {10, "0"}, {20, "0"}, {30, "0"}, {2, "ANSI31"}, {70, "0"}, {71, "0"}, {91, "1"},
      {92, "2"}, {72, "0"}, {73, "1"}, {93, "4"}, {10, "20"}, {20, "0"}, {10, "30"}, {20, "0"}, {10, "30"}, {20, "10"}, {10, "20"}, {20, "10"}, {97, "0"},
      {75, "0"}, {76, "1"}, {52, "45"}, {41, "1"}, {77, "0"}, {78, "1"}, {53, "45"}, {43, "0"}, {44, "0"}, {45, "-1"}, {46, "1"}, {79, "0"}, {98, "0"}};
  // The upper half disc, its arc stored clockwise: AutoCAD writes such angles mirrored (180 -> 360 means 180 -> 0 over the top).
  const Groups half_disc = {
      {0, "HATCH"}, {8, "U"}, {10, "0"}, {20, "0"}, {30, "0"}, {2, "SOLID"}, {70, "1"}, {71, "0"}, {91, "1"},
      {92, "1"}, {93, "2"},
      {72, "2"}, {10, "50"}, {20, "0"}, {40, "10"}, {50, "180"}, {51, "360"}, {73, "0"},
      {72, "1"}, {10, "60"}, {20, "0"}, {11, "40"}, {21, "0"}, {97, "0"},
      {75, "0"}, {76, "1"}, {98, "0"}};
  Groups all = square_with_hole;
  all.insert(all.end(), pattern.begin(), pattern.end());
  all.insert(all.end(), half_disc.begin(), half_disc.end());
  write_text_file(f.dir / "hatch.dxf", section("ENTITIES", all) + kEof);
  const auto found = bodies(import(f.dir / "hatch.dxf"));
  const auto* solid = find(found, "S");
  CHECK(solid && solid->faces == 1 && std::abs(solid->area - 64) < 1e-6);
  const auto* lines = find(found, "P");
  CHECK(lines && lines->faces == 0 && lines->edges == 9);  // 45 degrees, 1.414 apart, across a 10 x 10 square
  const auto box = extent(lines->box);
  CHECK(box[0] > 20 - 1e-3 && box[1] > -1e-3 && box[2] < 30 + 1e-3 && box[3] < 10 + 1e-3);  // clipped to the boundary
  const auto* disc = find(found, "U");
  CHECK(disc && disc->faces == 1);
  const auto d = extent(disc->box);
  CHECK(d[1] > -1e-3 && d[3] > 9.9 && std::abs(disc->area - 3.14159265 * 50) < 0.5);
}

TEST(old_polylines_ellipses_and_planes_seen_from_below) {
  Files f;
  write_text_file(f.dir / "curves.dxf", section("ENTITIES", {
      // a closed square whose top edge bulges into a half circle (bulge 1)
      {0, "POLYLINE"}, {8, "L"}, {66, "1"}, {10, "0"}, {20, "0"}, {30, "0"}, {70, "1"},
      {0, "VERTEX"}, {8, "L"}, {10, "0"}, {20, "0"},
      {0, "VERTEX"}, {8, "L"}, {10, "10"}, {20, "0"},
      {0, "VERTEX"}, {8, "L"}, {10, "10"}, {20, "10"}, {42, "1"},
      {0, "VERTEX"}, {8, "L"}, {10, "0"}, {20, "10"},
      {0, "SEQEND"}, {8, "L"},
      {0, "ELLIPSE"}, {8, "E"}, {10, "100"}, {20, "0"}, {30, "0"}, {11, "20"}, {21, "0"}, {31, "0"}, {40, "0.5"}, {41, "0"}, {42, "6.283185307179586"},
      // a circle in a plane seen from below: x mirrored
      {0, "CIRCLE"}, {8, "M"}, {10, "50"}, {20, "0"}, {30, "0"}, {40, "5"}, {210, "0"}, {220, "0"}, {230, "-1"},
      {0, "LWPOLYLINE"}, {8, "W"}, {90, "2"}, {70, "0"}, {10, "0"}, {20, "20"}, {10, "10"}, {20, "20"}, {210, "0"}, {220, "0"}, {230, "-1"},
  }) + kEof);
  const auto all = bodies(import(f.dir / "curves.dxf"));
  const auto l = extent(find(all, "L")->box);
  CHECK(std::abs(l[0]) < 0.01 && std::abs(l[2] - 10) < 0.01 && std::abs(l[3] - 15) < 0.05);  // the arc rises 5 above y = 10
  const auto e = extent(find(all, "E")->box);
  CHECK(std::abs(e[0] - 80) < 0.05 && std::abs(e[2] - 120) < 0.05 && std::abs(e[3] - 10) < 0.05);
  const auto m = extent(find(all, "M")->box);
  CHECK(std::abs((m[0] + m[2]) / 2 + 50) < 0.05);
  CHECK(near_box(find(all, "W")->box, -10, 20, 0, 20));
}

TEST(text_sits_on_its_baseline_and_mtext_hangs_from_its_top) {
  Files f;
  write_text_file(f.dir / "text.dxf", section("ENTITIES", {
      {0, "TEXT"}, {8, "T"}, {10, "0"}, {20, "0"}, {30, "0"}, {40, "10"}, {1, "HELLO"},
      {0, "TEXT"}, {8, "C"}, {10, "0"}, {20, "0"}, {40, "10"}, {1, "HI"}, {72, "1"}, {11, "100"}, {21, "0"},
      {0, "MTEXT"}, {8, "M"}, {10, "0"}, {20, "50"}, {30, "0"}, {40, "5"}, {71, "1"}, {1, "A\\PB{\\fArial|b1;C}"},
      {0, "TEXT"}, {8, "D"}, {10, "0"}, {20, "-50"}, {40, "10"}, {1, "%%c20"},
  }) + kEof);
  ImportResult result;
  const auto all = bodies(import(f.dir / "text.dxf", true, false, &result));
  const auto* t = find(all, "T");
  if (!t) return;  // no font on this machine: the warning says so
  const auto box = extent(t->box);
  CHECK(t->faces >= 5 && box[1] > -0.5 && box[1] < 0.5 && box[3] > 9.5 && box[3] < 10.5);  // capitals exactly the text height
  const auto c = extent(find(all, "C")->box);
  CHECK(std::abs((c[0] + c[2]) / 2 - 100) < 1);
  const auto m = extent(find(all, "M")->box);
  CHECK(m[3] > 49.4 && m[3] < 50.6 && m[1] < 44);  // two lines below the insertion point
  CHECK(find(all, "D") != nullptr);
}

// TODO 11 UI-92: text is shaped (an Arabic word right-aligned on its point, the letters joined), fitted and aligned
// between its two points, and MTEXT's bottom and middle attachments hold its last baseline and its middle.
TEST(arabic_text_aligns_and_text_fits_between_its_points) {
  Files f;
  const std::string word = "\xD8\xBA\xD8\xB1\xD9\x81\xD8\xA9";  // "غرفة"
  write_text_file(f.dir / "text2.dxf",
                  section("HEADER", {{9, "$ACADVER"}, {1, "AC1027"}}) +
                      section("ENTITIES", {
                          {0, "TEXT"}, {8, "R"}, {10, "0"}, {20, "0"}, {40, "10"}, {1, word}, {72, "2"}, {11, "100"}, {21, "0"},
                          {0, "TEXT"}, {8, "F"}, {10, "0"}, {20, "-50"}, {40, "10"}, {1, "HELL"}, {72, "5"}, {11, "100"}, {21, "-50"},
                          {0, "TEXT"}, {8, "A"}, {10, "0"}, {20, "-100"}, {40, "10"}, {1, "HELL"}, {72, "3"}, {11, "100"}, {21, "-100"},
                          {0, "MTEXT"}, {8, "B"}, {10, "200"}, {20, "0"}, {40, "6"}, {71, "7"}, {1, "HE\\PEH"},
                          {0, "MTEXT"}, {8, "M"}, {10, "300"}, {20, "0"}, {40, "6"}, {71, "5"}, {1, "HE\\PEH"},
                      }) + kEof);
  ImportResult result;
  const auto all = bodies(import(f.dir / "text2.dxf", true, false, &result));
  const auto* r = find(all, "R");
  if (!r) return;  // no font on this machine: the warning says so
  const auto right = extent(r->box);
  CHECK(right[2] <= 100.01 && right[2] > 98.5 && right[0] > 50 && right[0] < 90);
  const auto fit = extent(find(all, "F")->box), aligned = extent(find(all, "A")->box);
  CHECK(fit[0] < 5 && fit[2] > 95 && fit[2] <= 100.01 && std::abs(fit[3] + 40) < 0.05);  // as long as its points, as high
  CHECK(aligned[0] < 5 && aligned[2] > 95 && aligned[3] + 100 > 15);                     // and larger with them
  const auto bottom = extent(find(all, "B")->box), middle = extent(find(all, "M")->box);
  CHECK(std::abs(bottom[1]) < 0.05 && std::abs(bottom[3] - (10 + 6)) < 0.05);  // last baseline on the point, lines 10 apart
  CHECK(std::abs((middle[1] + middle[3]) / 2) < 0.05);
}

// TODO 11 UI-92: a style's shape font (.shx) found beside the drawing (or in DWG TrueView's and AutoCAD's Fonts folders)
// draws its text in strokes; text with a character it lacks, and a shape font that is nowhere, in the outline font.
TEST(shape_font_text_is_drawn_in_its_strokes) {
  Files f;
  // shapes 1.0: the font's line (above 10, below 2) and 'I', a stroke 10 up, then 6 on.
  const std::string font = std::string("AutoCAD-86 shapes 1.0\r\n\x1a") + std::string("\x00\x00\x49\x00\x02\x00\x00\x00\x06\x00\x49\x00\x07\x00", 14) +
                           std::string("T\x00\x0a\x02\x00\x00", 6) + std::string("\x00\x01\xa4\x02\xac\x60\x00", 7);
  write_text_file(f.dir / "mini.shx", font);
  write_text_file(f.dir / "shx.dxf",
                  section("TABLES", {{0, "TABLE"}, {2, "STYLE"}, {0, "STYLE"}, {2, "MINI"}, {70, "0"}, {40, "0"}, {41, "1"}, {3, "mini.shx"},
                                     {0, "STYLE"}, {2, "GONE"}, {70, "0"}, {40, "0"}, {41, "1"}, {3, "nowhere.shx"}, {0, "ENDTAB"}}) +
                      section("ENTITIES", {{0, "TEXT"}, {8, "S"}, {7, "MINI"}, {10, "0"}, {20, "0"}, {40, "20"}, {1, "II"},
                                           {0, "TEXT"}, {8, "Lacks"}, {7, "MINI"}, {10, "0"}, {20, "-50"}, {40, "20"}, {1, "IJ"},
                                           {0, "TEXT"}, {8, "Gone"}, {7, "GONE"}, {10, "0"}, {20, "-100"}, {40, "20"}, {1, "II"}}) +
                      kEof);
  const auto all = bodies(import(f.dir / "shx.dxf"));
  const auto* strokes = find(all, "S");
  CHECK(strokes && strokes->faces == 0 && strokes->edges == 2 && near_box(strokes->box, 0, 0, 12, 20));  // two strokes 12 apart, 20 high
  if (const auto* lacks = find(all, "Lacks")) CHECK(lacks->faces > 0);
  if (const auto* gone = find(all, "Gone")) CHECK(gone->faces > 0);
}

// Bodies share no sub-shapes (the view meshes them on several threads at once): the same text, or a block with a fill,
// on two layers gives each layer's body its own edges, in a viewer (shapes kept as read) and in a document.
TEST(bodies_share_no_edges) {
  Files f;
  write_text_file(f.dir / "shared.dxf",
                  section("BLOCKS", {{0, "BLOCK"}, {2, "K"}, {70, "0"}, {10, "0"}, {20, "0"}, {0, "SOLID"}, {8, "0"}, {10, "0"}, {20, "0"},
                                     {11, "1"}, {21, "0"}, {12, "0"}, {22, "1"}, {13, "1"}, {23, "1"}, {0, "ENDBLK"}}) +
                      section("ENTITIES", {{0, "TEXT"}, {8, "A"}, {10, "0"}, {20, "0"}, {40, "10"}, {1, "HELLO"},
                                           {0, "TEXT"}, {8, "B"}, {10, "0"}, {20, "20"}, {40, "10"}, {1, "HELLO"},
                                           {0, "INSERT"}, {8, "A"}, {2, "K"}, {10, "50"}, {20, "0"},
                                           {0, "INSERT"}, {8, "B"}, {2, "K"}, {10, "50"}, {20, "20"}}) +
                      kEof);
  for (bool viewer : {true, false}) {
    const Document d = import(f.dir / "shared.dxf", viewer);
    std::map<const TopoDS_TShape*, std::string> owner;
    bool shared = false;
    for (const auto& key : d.body_keys())
      for (TopExp_Explorer e(body_shape(d, key), TopAbs_EDGE); e.More(); e.Next()) {
        const auto [it, added] = owner.emplace(e.Current().TShape().get(), key);
        shared = shared || (!added && it->second != key);
      }
    CHECK(d.body_keys().size() == 2 && !owner.empty() && !shared);
  }
}

TEST(survey_coordinates_and_metres) {
  Files f;
  write_text_file(f.dir / "site.dxf",
                  section("HEADER", {{9, "$INSUNITS"}, {70, "6"}}) +
                      section("ENTITIES", {{0, "LINE"}, {8, "0"}, {10, "500000"}, {20, "2400000"}, {11, "500010"}, {21, "2400000"}}) + kEof);
  // In place: millimetres, where the file says.
  CHECK(near_box(bodies(import(f.dir / "site.dxf"))[0].box, 500000000, 2400000000, 500010000, 2400000000, 1));
  // The body itself is stored near (0,0), where 32-bit display coordinates resolve; its component puts it back.
  const Document d = import(f.dir / "site.dxf");
  for (const auto& key : d.body_keys()) {
    Bnd_Box box;
    BRepBndLib::Add(body_shape(d, key), box);
    const auto b = extent(box);
    CHECK(std::abs(b[0]) < 1e5 && std::abs(b[1]) < 1e5);
  }
  // Opened on its own: centred.
  CHECK(near_box(bodies(import(f.dir / "site.dxf", true, true))[0].box, -5000, 0, 5000, 0, 1));
}

TEST(unknown_entities_are_listed_and_broken_ones_skipped) {
  Files f;
  write_text_file(f.dir / "mixed.dxf", section("ENTITIES", {
      {0, "REGION"}, {8, "0"},
      {0, "LINE"}, {8, "0"}, {10, "abc"}, {20, "0"}, {11, "1"}, {21, "0"},
      {0, "LINE"}, {8, "0"}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"},
      {0, "3DSOLID"}, {8, "0"},
      {0, "LINE"}, {8, "0"}, {67, "1"}, {10, "0"}, {20, "0"}, {11, "5"}, {21, "0"},  // paper space
  }) + kEof);
  ImportResult result;
  const auto all = bodies(import(f.dir / "mixed.dxf", true, false, &result));
  CHECK(all.size() == 1 && near_box(all[0].box, 0, 0, 1, 0));
  std::string warnings;
  for (const auto& w : result.warnings) warnings += w + "\n";
  CHECK(warnings.find("REGION") != std::string::npos && warnings.find("3DSOLID") != std::string::npos);
  CHECK(warnings.find("1 entities could not be read") != std::string::npos);
  CHECK(warnings.find("paper-space") != std::string::npos);
  write_text_file(f.dir / "nothing.dxf", section("ENTITIES", {{0, "REGION"}, {8, "0"}}) + kEof);
  CHECK_THROWS(import(f.dir / "nothing.dxf"));
}

// TODO 11 UI-37: the layer table's state reaches the layer's node. Off and frozen layers are hidden, a locked layer is
// locked (its bodies are not picked or changed), and plot, linetype and lineweight are kept for a layer manager.
TEST(layer_table_state_reaches_the_layer_nodes) {
  Files f;
  Groups lines;
  for (const char* layer : {"Walls", "Notes", "Old", "Plain"})
    lines.insert(lines.end(), {{0, "LINE"}, {8, layer}, {10, "0"}, {20, "0"}, {11, "10"}, {21, "0"}});
  write_text_file(f.dir / "layers.dxf",
                  section("TABLES", {{0, "TABLE"}, {2, "LAYER"},
                                     {0, "LAYER"}, {2, "Walls"}, {62, "1"}, {70, "4"}, {6, "DASHED"}, {370, "50"},
                                     {0, "LAYER"}, {2, "Notes"}, {62, "-3"}, {70, "0"}, {290, "0"},
                                     {0, "LAYER"}, {2, "Old"}, {62, "2"}, {70, "1"}, {6, "Continuous"},
                                     {0, "LAYER"}, {2, "Plain"}, {62, "5"}, {70, "0"}, {370, "-3"},
                                     {0, "ENDTAB"}}) +
                      section("ENTITIES", lines) + kEof);
  for (bool viewer : {true, false}) {
    const Document d = import(f.dir / "layers.dxf", viewer);
    const Scene s = resolve(d);
    std::map<std::string, const Node*> layers;
    for (const auto& [id, n] : s.nodes)
      if (n.layer.is_object()) layers[n.name] = &n;
    CHECK_EQ(layers.size(), 4u);
    CHECK(layers["Walls"]->locked && layers["Walls"]->visible);
    CHECK(layers["Walls"]->layer == json({{"name", "Walls"}, {"locked", true}, {"linetype", "DASHED"}, {"lineweight", 0.5}}));
    CHECK(!layers["Notes"]->visible && !layers["Notes"]->locked && layers["Notes"]->layer == json({{"name", "Notes"}, {"off", true}, {"plot", false}}));
    CHECK(!layers["Old"]->visible && layers["Old"]->layer == json({{"name", "Old"}, {"frozen", true}}));
    CHECK(layers["Plain"]->visible && !layers["Plain"]->locked && layers["Plain"]->layer == json({{"name", "Plain"}}));
    CHECK(s.effectively_locked(s.bodies_under(layers["Walls"]->id).at(0)));
    CHECK(!s.effectively_locked(s.bodies_under(layers["Plain"]->id).at(0)));
    CHECK(s.unresolved.empty());
  }
}

// TODO 11 UI-89: a layer's linetype brings its dashes from the LTYPE table (the shapes and text of a complex one left
// out), sized as acad.lin's: the file's DASHED and HIDDEN, in inches here, say how much larger its dashes are.
TEST(linetype_patterns_reach_the_layers) {
  Files f;
  Groups lines;
  for (const char* layer : {"Walls", "Hidden", "Fence", "Plain"}) lines.insert(lines.end(), {{0, "LINE"}, {8, layer}, {10, "0"}, {20, "0"}, {11, "10"}, {21, "0"}});
  write_text_file(f.dir / "linetypes.dxf",
                  section("TABLES", {{0, "TABLE"}, {2, "LTYPE"},
                                     {0, "LTYPE"}, {2, "CONTINUOUS"}, {73, "0"}, {40, "0"},
                                     {0, "LTYPE"}, {2, "DASHED"}, {73, "2"}, {40, "0.75"}, {49, "0.5"}, {74, "0"}, {49, "-0.25"}, {74, "0"},
                                     {0, "LTYPE"}, {2, "HIDDEN"}, {73, "2"}, {40, "0.375"}, {49, "0.25"}, {74, "0"}, {49, "-0.125"}, {74, "0"},
                                     {0, "LTYPE"}, {2, "FENCE"}, {73, "4"}, {40, "0.5"}, {49, "0.3"}, {74, "0"}, {49, "-0.1"}, {74, "2"}, {75, "0"},
                                     {46, "0.1"}, {50, "0"}, {44, "0"}, {45, "0"}, {9, "GAS"}, {49, "0"}, {74, "0"}, {49, "-0.1"}, {74, "0"},
                                     {0, "ENDTAB"}, {0, "TABLE"}, {2, "LAYER"},
                                     {0, "LAYER"}, {2, "Walls"}, {62, "1"}, {70, "0"}, {6, "DASHED"},
                                     {0, "LAYER"}, {2, "Hidden"}, {62, "2"}, {70, "0"}, {6, "HIDDEN"},
                                     {0, "LAYER"}, {2, "Fence"}, {62, "3"}, {70, "0"}, {6, "FENCE"},
                                     {0, "LAYER"}, {2, "Plain"}, {62, "5"}, {70, "0"}, {6, "CONTINUOUS"},
                                     {0, "ENDTAB"}}) +
                      section("ENTITIES", lines) + kEof);
  const Scene s = resolve(import(f.dir / "linetypes.dxf"));
  std::map<std::string, json> layers;
  for (const auto& [id, n] : s.nodes)
    if (n.layer.is_object()) layers[n.name] = n.layer;
  CHECK(layers["Walls"] == json({{"name", "Walls"}, {"linetype", "DASHED"}, {"pattern", {12.7, -6.35}}}));
  CHECK(layers["Hidden"] == json({{"name", "Hidden"}, {"linetype", "HIDDEN"}, {"pattern", {6.35, -3.175}}}));
  CHECK(layers["Fence"] == json({{"name", "Fence"}, {"linetype", "FENCE"}, {"pattern", {7.62, -2.54, 0.0, -2.54}}}));
  CHECK(layers["Plain"] == json({{"name", "Plain"}}));
}

// TODO 11 UI-92: an entity's own linetype and lineweight put it into a body of its own that carries them (line, over its
// layer's), with the file's dashes; by block they are the insert's (or its layer's), in model space continuous; one
// that only repeats its layer's (or Continuous on a continuous layer) stays with the layer's body.
TEST(entity_linetypes_and_lineweights_reach_their_bodies) {
  Files f;
  auto line = [](const char* layer, double y, Groups extra) {
    Groups g{{0, "LINE"}, {8, layer}, {10, "0"}, {20, std::to_string(y)}, {11, "10"}, {21, std::to_string(y)}};
    g.insert(g.end(), extra.begin(), extra.end());
    return g;
  };
  Groups entities;
  for (const auto& g : {line("Walls", 0, {}), line("Walls", 1, {{6, "CENTER"}}), line("Walls", 2, {{6, "DASHED"}, {370, "50"}}), line("Plain", 3, {{370, "70"}}),
                        line("Plain", 4, {{6, "CONTINUOUS"}}), line("Plain", 5, {{6, "BYBLOCK"}}),
                        Groups{{0, "INSERT"}, {8, "Plain"}, {6, "HIDDEN"}, {370, "35"}, {2, "K"}, {10, "0"}, {20, "10"}},
                        Groups{{0, "INSERT"}, {8, "Walls"}, {2, "K"}, {10, "0"}, {20, "20"}}})
    entities.insert(entities.end(), g.begin(), g.end());
  write_text_file(f.dir / "pens.dxf",
                  section("TABLES", {{0, "TABLE"}, {2, "LTYPE"},
                                     {0, "LTYPE"}, {2, "DASHED"}, {73, "2"}, {40, "19.05"}, {49, "12.7"}, {49, "-6.35"},
                                     {0, "LTYPE"}, {2, "CENTER"}, {73, "4"}, {40, "50.8"}, {49, "31.75"}, {49, "-6.35"}, {49, "6.35"}, {49, "-6.35"},
                                     {0, "ENDTAB"}, {0, "TABLE"}, {2, "LAYER"},
                                     {0, "LAYER"}, {2, "Walls"}, {62, "1"}, {70, "0"}, {6, "DASHED"}, {370, "50"},
                                     {0, "LAYER"}, {2, "Plain"}, {62, "5"}, {70, "0"}, {0, "ENDTAB"}}) +
                      section("BLOCKS", {{0, "BLOCK"}, {2, "K"}, {70, "0"}, {10, "0"}, {20, "0"}, {0, "LINE"}, {8, "0"}, {6, "BYBLOCK"}, {370, "-2"},
                                         {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"}, {0, "ENDBLK"}}) +
                      section("ENTITIES", entities) + kEof);
  for (bool viewer : {true, false}) {
    const Scene s = resolve(import(f.dir / "pens.dxf", viewer));
    std::map<std::string, std::vector<json>> lines;
    for (const auto& id : s.all_bodies()) lines[s.node(id)->name].push_back(s.node(id)->line);
    for (auto& [name, list] : lines) std::sort(list.begin(), list.end());
    CHECK(lines["Walls"] == (std::vector<json>{json(), json({{"linetype", "CENTER"}, {"pattern", {31.75, -6.35, 6.35, -6.35}}})}));
    CHECK(lines["Plain"] == (std::vector<json>{json(), json({{"linetype", "HIDDEN"}, {"lineweight", 0.35}}), json({{"lineweight", 0.7}})}));
    CHECK(s.tree_json(-1).dump().find("\"line\":{") != std::string::npos);
  }
}

// TODO 11 UI-89: the body of a layer's BYLAYER entities says so (by_layer), so a colour given to the layer reaches it and
// not the entities drawn in colours of their own; a block's layer-0 BYLAYER entities follow the insert's layer.
TEST(by_layer_bodies_are_marked) {
  Files f;
  write_text_file(f.dir / "bylayer.dxf",
                  section("TABLES", {{0, "TABLE"}, {2, "LAYER"}, {0, "LAYER"}, {2, "A"}, {62, "1"}, {70, "0"}, {0, "LAYER"}, {2, "B"}, {62, "7"}, {70, "0"},
                                     {0, "LAYER"}, {2, "C"}, {62, "3"}, {70, "0"}, {0, "ENDTAB"}}) +
                      section("BLOCKS", {{0, "BLOCK"}, {2, "K"}, {70, "0"}, {10, "0"}, {20, "0"}, {0, "LINE"}, {8, "0"}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"},
                                         {0, "ENDBLK"}}) +
                      section("ENTITIES", {{0, "LINE"}, {8, "A"}, {10, "0"}, {20, "0"}, {11, "10"}, {21, "0"},              // by layer: red
                                           {0, "LINE"}, {8, "A"}, {62, "5"}, {10, "0"}, {20, "5"}, {11, "10"}, {21, "5"},   // blue of its own
                                           {0, "LINE"}, {8, "B"}, {10, "0"}, {20, "10"}, {11, "10"}, {21, "10"},            // by layer: the ink
                                           {0, "LINE"}, {8, "B"}, {62, "1"}, {10, "0"}, {20, "15"}, {11, "10"}, {21, "15"}, // red of its own
                                           {0, "LINE"}, {8, "C"}, {62, "5"}, {10, "0"}, {20, "20"}, {11, "10"}, {21, "20"}, // only its own colour
                                           {0, "INSERT"}, {8, "B"}, {2, "K"}, {10, "0"}, {20, "30"}}) +
                      kEof);
  for (bool viewer : {true, false}) {
    const Scene s = resolve(import(f.dir / "bylayer.dxf", viewer));
    std::map<std::string, int> marked, own;
    for (const auto& id : s.all_bodies()) {
      const Node* n = s.node(id);
      (n->by_layer ? marked : own)[n->name]++;
      if (n->by_layer && n->name == "A") CHECK(n->has_color && n->color == (std::array<double, 3>{1, 0, 0}));
      if (n->by_layer && n->name == "B") CHECK(!n->has_color);  // the ink, the block's line with it
    }
    CHECK(marked == (std::map<std::string, int>{{"A", 1}, {"B", 1}}));
    CHECK(own == (std::map<std::string, int>{{"A", 1}, {"B", 1}, {"C", 1}}));
    CHECK(s.tree_json(-1).dump().find("\"by_layer\":true") != std::string::npos);
  }
}

#ifdef _WIN32
TEST(old_code_pages_become_utf8) {
  Files f;
  const std::string walls = "\xC7\xE1\xCC\xCF\xD1\xC7\xE4";  // "الجدران" in Windows-1256
  write_text_file(f.dir / "arabic.dxf",
                  section("HEADER", {{9, "$ACADVER"}, {1, "AC1015"}, {9, "$DWGCODEPAGE"}, {3, "ANSI_1256"}}) +
                      section("TABLES", {{0, "TABLE"}, {2, "LAYER"}, {0, "LAYER"}, {2, walls}, {62, "1"}, {0, "ENDTAB"}}) +
                      section("ENTITIES", {{0, "LINE"}, {8, walls}, {10, "0"}, {20, "0"}, {11, "1"}, {21, "0"}}) + kEof);
  const auto all = bodies(import(f.dir / "arabic.dxf", false));  // editable: the name goes into the saved JSON
  CHECK(all.size() == 1 && all[0].layer == "الجدران" && all[0].has_color && all[0].color[0] == 1);
}
#endif

TEST(dwg_opens_through_libredwg) {
  const std::filesystem::path data = OPAD_LIBREDWG_DATA;
  if (!std::filesystem::exists(data / "example_2000.dwg")) return;  // submodule not checked out
  Files f;
  for (const char* name : {"example_r14.dwg", "example_2000.dwg", "example_2007.dwg", "example_2018.dwg"}) {
    ImportResult result;
    Document d = Document::create();
    try {
      result = import_file(d, data / name, ImportOptions{});
    } catch (const Error& e) {
      if (std::string(e.what()).find("needs a converter") != std::string::npos) return;  // built with OPAD_DWG=OFF
      throw;
    }
    CHECK(result.bodies > 0);
  }
  // A drawing named in Arabic, in a folder named in Arabic.
  const auto folder = f.dir / std::filesystem::path(u8"مخططات");
  std::filesystem::create_directory(folder);
  std::filesystem::copy_file(data / "example_2000.dwg", folder / std::filesystem::path(u8"المخطط.dwg"));
  CHECK(!bodies(import(folder / std::filesystem::path(u8"المخطط.dwg"))).empty());
}

CHECK_MAIN()
