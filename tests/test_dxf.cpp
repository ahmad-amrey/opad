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
// The viewer cache lives under cache_dir(), which reads OPAD_CACHE_DIR once: aim it at a scratch folder before any test.
const std::filesystem::path kCacheDir = [] {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-dxf-cache-" + new_uuid());
#ifdef _WIN32
  _putenv_s("OPAD_CACHE_DIR", dir.string().c_str());
#else
  setenv("OPAD_CACHE_DIR", dir.string().c_str(), 1);
#endif
  return dir;
}();
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

// A block at one scale is copied once and placed rigidly everywhere else, a block holding a scaled block waits for that
// copy, a skewed one holds the text laid out after the entities are read, and a repeated text is laid out once.
TEST(scaled_blocks_and_repeated_texts_are_made_once_and_land_in_place) {
  Files f;
  const std::string text =
      section("BLOCKS", {{0, "BLOCK"}, {2, "B"}, {70, "0"}, {10, "0"}, {20, "0"},
                         {0, "LINE"}, {8, "0"}, {10, "0"}, {20, "0"}, {11, "10"}, {21, "0"},
                         {0, "ENDBLK"},
                         {0, "BLOCK"}, {2, "T"}, {70, "0"}, {10, "0"}, {20, "0"},
                         {0, "LINE"}, {8, "0"}, {10, "0"}, {20, "0"}, {11, "10"}, {21, "0"},
                         {0, "TEXT"}, {8, "0"}, {10, "0"}, {20, "0"}, {40, "10"}, {1, "HI"},
                         {0, "ENDBLK"},
                         {0, "BLOCK"}, {2, "N"}, {70, "0"}, {10, "0"}, {20, "0"},
                         {0, "INSERT"}, {8, "NB"}, {2, "B"}, {10, "0"}, {20, "0"}, {41, "3"}, {42, "3"},
                         {0, "INSERT"}, {8, "NT"}, {2, "T"}, {10, "0"}, {20, "20"}, {41, "2"}, {42, "2"},
                         {0, "ENDBLK"}}) +
      section("ENTITIES", {
          {0, "INSERT"}, {8, "S1"}, {2, "B"}, {10, "0"}, {20, "100"}, {41, "2"}, {42, "2"},
          {0, "INSERT"}, {8, "S2"}, {2, "B"}, {10, "0"}, {20, "200"}, {41, "2"}, {42, "2"}, {50, "90"},
          {0, "INSERT"}, {8, "S3"}, {2, "B"}, {10, "0"}, {20, "300"}, {41, "-2"}, {42, "2"},
          {0, "INSERT"}, {8, "S4"}, {2, "N"}, {10, "0"}, {20, "400"}, {41, "0.5"}, {42, "0.5"},  // B at 1.5, T at 1
          {0, "INSERT"}, {8, "S5"}, {2, "T"}, {10, "0"}, {20, "600"}, {41, "2"}, {42, "1"},      // skewed: a copy of its own
          {0, "TEXT"}, {8, "S6"}, {10, "0"}, {20, "700"}, {40, "10"}, {1, "HI"},
          {0, "TEXT"}, {8, "S6"}, {10, "100"}, {20, "700"}, {40, "10"}, {50, "90"}, {1, "HI"},
          {0, "TEXT"}, {8, "S7"}, {10, "0"}, {20, "800"}, {40, "10"}, {1, "HI"},
      }) +
      kEof;
  write_text_file(f.dir / "scaled.dxf", text);
  for (bool viewer : {true, false}) {
    ImportResult result;
    const auto all = bodies(import(f.dir / "scaled.dxf", viewer, false, &result));
    const auto* s1 = find(all, "S1");
    const auto* s2 = find(all, "S2");
    const auto* s3 = find(all, "S3");
    const auto* nb = find(all, "NB");
    CHECK(s1 && near_box(s1->box, 0, 100, 20, 100));
    CHECK(s2 && near_box(s2->box, 0, 200, 0, 220));
    CHECK(s3 && near_box(s3->box, -20, 300, 0, 300));
    CHECK(nb && near_box(nb->box, 0, 400, 15, 400) && nb->edges == 1);
    const auto* s7 = find(all, "S7");
    if (!s7) continue;  // no font on this machine
    const auto one = extent(s7->box);
    CHECK(s7->faces >= 2 && one[3] > 809.5 && one[3] < 810.5);
    const auto* s6 = find(all, "S6");
    CHECK(s6 && s6->faces == 2 * s7->faces);
    const auto* nt = find(all, "NT");  // T at 2 inside N at 0.5: the text as written, 10 high on its line
    CHECK(nt && nt->faces == s7->faces);
    const auto t = extent(nt->box);
    CHECK(std::abs(t[0]) < 0.5 && std::abs(t[1] - 410) < 0.5 && t[3] > 419.5 && t[3] < 420.5);
    const auto* s5 = find(all, "S5");  // twice as wide, as high
    CHECK(s5 && s5->faces == s7->faces);
    const auto w = extent(s5->box);
    CHECK(std::abs(w[1] - 600) < 0.5 && w[3] > 609.5 && w[3] < 610.5 && w[2] - w[0] > 2 * (one[2] - one[0]) - 0.5);
    CHECK(result.warnings.empty());
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

#ifdef OPAD_FAKE_ODA
// The ODA File Converter is opt-in (its terms allow non-members non-commercial use only): installed but off, a DWG never
// goes through it and the error points to the setting; switched on (the setting or OPAD_USE_ODA), it converts.
TEST(oda_converter_only_when_switched_on) {
  Files f;
  const auto converter = f.dir / "ODA" / "ODAFileConverter 99.0" / "ODAFileConverter.exe";
  std::filesystem::create_directories(converter.parent_path());
  std::filesystem::copy_file(OPAD_FAKE_ODA, converter);
  write_text_file(f.dir / "plan.dwg", "not a drawing");
  struct Env {
    std::vector<std::pair<std::wstring, std::wstring>> saved;
    Env() {
      for (const wchar_t* name : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"OPAD_DWG2DXF", L"OPAD_USE_ODA"}) {
        const wchar_t* value = _wgetenv(name);
        saved.emplace_back(name, value ? value : L"");
        _wputenv_s(name, L"");
      }
    }
    ~Env() { for (const auto& [name, value] : saved) _wputenv_s(name.c_str(), value.c_str()); set_use_oda(false); }
  } env;
  _wputenv_s(L"ProgramFiles", f.dir.wstring().c_str());
  CHECK(std::filesystem::equivalent(oda_file_converter(), converter));
  set_use_oda(false);
  CHECK(!use_oda());
  std::string message;
  try { import(f.dir / "plan.dwg"); } catch (const Error& e) { message = e.what(); }
  CHECK(message.find("not switched on") != std::string::npos);
  set_use_oda(true);
  const auto all = bodies(import(f.dir / "plan.dwg"));
  CHECK(all.size() == 1 && all[0].layer == "ODA");
  set_use_oda(false);
  _wputenv_s(L"OPAD_USE_ODA", L"1");
  CHECK(use_oda() && !bodies(import(f.dir / "plan.dwg")).empty());
  // A remembered slow read names its converter: switched off, the drawing is read again (LibreDWG); on, it is found.
  _wputenv_s(L"OPAD_USE_ODA", L"");
  set_use_oda(true);
  ImportOptions viewer;
  viewer.viewer = true;
  Document read = Document::create();
  import_file(read, f.dir / "plan.dwg", viewer);
  viewer_cache_store(read, f.dir / "plan.dwg", viewer);
  Document again = Document::create();
  CHECK(viewer_cache_load(again, f.dir / "plan.dwg", viewer) && bodies(again).size() == 1 && bodies(again)[0].layer == "ODA");
  set_use_oda(false);
  CHECK_EQ(dwg_reader(), std::string("libredwg"));
  Document off = Document::create();
  CHECK(!viewer_cache_load(off, f.dir / "plan.dwg", viewer));
  std::error_code e;
  std::filesystem::remove_all(kCacheDir, e);
}
#endif

CHECK_MAIN()
