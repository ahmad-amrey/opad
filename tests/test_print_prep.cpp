// Preparing parts for 3D printing: the export's print placement (turned by build_direction or a face down on the bed,
// dropped onto it, centred) with its mesh formats (STL, 3MF, OBJ, PLY), and the print check's thin walls naming what
// makes them while leaving out the knife edges of sharp edges.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <Bnd_Box.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

#include "check.hpp"
#include "opad/checks.hpp"
#include "opad/core.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/step_io.hpp"

using namespace opad;

namespace {

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-print-prep-" + new_uuid());
  Files() { std::filesystem::create_directory(dir); }
  ~Files() {
    std::error_code e;
    std::filesystem::remove_all(dir, e);
  }
};

json feature_cmd(Document& doc, const std::string& kind, const json& inputs, const std::string& name = {}) {
  json args = {{"kind", kind}, {"inputs", inputs}};
  if (!name.empty()) args["name"] = name;
  return commands::run("feature", args, &doc);
}

// The face of a body whose normal points along `dir` (planar faces).
int face_along(const Document& doc, const Scene& s, const std::string& body, const Vec3& dir) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(node_world_shape(doc, s, body), TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i)
    if (const json d = describe_entity(faces(i)); d.contains("normal") &&
        d["normal"][0].get<double>() * dir[0] + d["normal"][1].get<double>() * dir[1] + d["normal"][2].get<double>() * dir[2] > 0.999)
      return i - 1;
  return -1;
}

// A binary STL's box: min x y z, max x y z.
std::array<float, 6> stl_box(const std::filesystem::path& file) {
  std::ifstream f(file, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  uint32_t n = 0;
  std::memcpy(&n, data.data() + 80, 4);
  std::array<float, 6> box{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
  for (uint32_t t = 0; t < n; ++t)
    for (int v = 0; v < 3; ++v)
      for (int k = 0; k < 3; ++k) {
        float x;
        std::memcpy(&x, data.data() + 84 + size_t(t) * 50 + 12 + size_t(v) * 12 + size_t(k) * 4, 4);
        box[size_t(k)] = std::min(box[size_t(k)], x), box[size_t(k) + 3] = std::max(box[size_t(k) + 3], x);
      }
  return box;
}

}  // namespace

TEST(build_direction_reads_as_the_print_check_takes_it) {
  CHECK(parse_build_direction(json()) == (Vec3{0, 0, 1}));
  CHECK(parse_build_direction("-y") == (Vec3{0, -1, 0}));
  const Vec3 d = parse_build_direction(json::array({0, 3, 4}));
  CHECK_NEAR(d[1], 0.6, 1e-12);
  CHECK_NEAR(d[2], 0.8, 1e-12);
  CHECK_THROWS(parse_build_direction("up"));
  CHECK_THROWS(parse_build_direction(json::array({0, 0, 0})));
  CHECK_THROWS(parse_build_direction(json::array({1, 2})));
}

TEST(export_turns_drops_and_centres_meshes_for_the_bed) {
  Files f;
  Document doc = Document::create();
  // 20 long (x), 10 wide (y), 5 high (z), away from the origin.
  const std::string block = feature_cmd(doc, "box", {{"x", "100 mm"}, {"y", "50 mm"}, {"length", "20 mm"}, {"width", "10 mm"}, {"height", "5 mm"}, {"centered", false}})["body_ids"][0];
  const Scene s = resolve(doc);
  // As modelled: the file holds the world coordinates.
  json r = commands::run("export", {{"format", "stl"}, {"out", (f.dir / "plain.stl").string()}}, &doc);
  CHECK(!r.contains("placement"));
  auto box = stl_box(f.dir / "plain.stl");
  CHECK_NEAR(box[0], 100, 1e-4);
  CHECK_NEAR(box[2], 0, 1e-4);
  // +x up (validate print's build_direction): the 20 mm length stands up, on the bed (drop is the default once turned).
  r = commands::run("export", {{"format", "stl"}, {"out", (f.dir / "up.stl").string()}, {"build_direction", "+x"}, {"center_xy", true}}, &doc);
  CHECK(r.contains("placement"));
  box = stl_box(f.dir / "up.stl");
  CHECK_EQ(box[2], 0.0f);  // exactly on the bed
  CHECK_NEAR(box[5], 20, 1e-4);
  CHECK_NEAR(box[0] + box[3], 0, 1e-4);  // centred
  CHECK_NEAR(box[1] + box[4], 0, 1e-4);
  CHECK_NEAR(r["placement"]["bbox"]["max"][2].get<double>(), 20, 1e-4);
  // A direction as [x, y, z], without the drop: turned about the origin only.
  r = commands::run("export", {{"format", "stl"}, {"out", (f.dir / "down.stl").string()}, {"build_direction", {0, 0, -1}}, {"drop_to_bed", false}}, &doc);
  box = stl_box(f.dir / "down.stl");
  CHECK_NEAR(box[2], -5, 1e-4);
  CHECK_NEAR(box[5], 0, 1e-4);
  // A face down on the bed: the +y side (20 x 5) lies on it, so the part is 10 high.
  const int side = face_along(doc, s, block, {0, 1, 0});
  CHECK(side >= 0);
  r = commands::run("export", {{"format", "stl"}, {"out", (f.dir / "face.stl").string()}, {"bed_face", {{"body", block}, {"kind", "face"}, {"index", side}}}}, &doc);
  box = stl_box(f.dir / "face.stl");
  CHECK_EQ(box[2], 0.0f);
  CHECK_NEAR(box[5], 10, 1e-4);
  CHECK_NEAR(r["placement"]["up"][1].get<double>(), -1, 1e-9);
  // One or the other, and only mesh formats.
  CHECK_THROWS(commands::run("export", {{"format", "stl"}, {"out", (f.dir / "x.stl").string()}, {"build_direction", "+z"}, {"bed_face", block + "/face/0"}}, &doc));
  CHECK_THROWS(commands::run("export", {{"format", "step"}, {"out", (f.dir / "x.step").string()}, {"build_direction", "+x"}}, &doc));
}

TEST(per_body_stl_files_each_land_on_the_bed) {
  Files f;
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}}, "Low");
  feature_cmd(doc, "box", {{"plane", {{"origin", {40, 0, 30}}, {"normal", {0, 0, 1}}}}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "4 mm"}}, "High");
  const json r = commands::run("export", {{"format", "stl"}, {"out", (f.dir / "parts.stl").string()}, {"per_body", true}, {"drop_to_bed", true}, {"center_xy", true}}, &doc);
  CHECK_EQ(r["files"].size(), size_t(2));
  CHECK_EQ(r["placement"]["files"].size(), size_t(2));
  for (const auto& file : r["files"]) {
    const auto box = stl_box(path_from_utf8(file.get<std::string>()));
    CHECK_EQ(box[2], 0.0f);
    CHECK_NEAR(box[0] + box[3], 0, 1e-4);
  }
}

TEST(three_mf_and_ply_meshes_read_back) {
  Files f;
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"x", "5 mm"}, {"length", "30 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}, "Part & <one>");
  for (const char* fmt : {"3mf", "ply", "obj"}) {
    const auto out = f.dir / (std::string("part.") + fmt);
    const json r = commands::run("export", {{"format", fmt}, {"out", out.string()}, {"build_direction", "-z"}}, &doc);
    CHECK_EQ(r["bodies"], 1);
    Document back = Document::create();
    import_file(back, out);
    const Scene s = resolve(back);
    CHECK_EQ(s.all_bodies().size(), size_t(1));
    const std::string id = s.all_bodies().front();
    CHECK_EQ(s.node(id)->representation, "mesh");
    double x0, y0, z0, x1, y1, z1;
    node_tight_bbox(back, s, id).Get(x0, y0, z0, x1, y1, z1);
    CHECK_NEAR(z0, 0, 1e-3);  // turned over and dropped
    CHECK_NEAR(z1, 10, 1e-3);
    CHECK_NEAR(x1 - x0, 30, 1e-3);
    if (std::string(fmt) == "3mf") CHECK_EQ(s.node(id)->name, "Part & <one>");
    // Welded: a closed box of 8 corners and 12 triangles, which the print check reads as one closed mesh.
    const json p = check_print(back, s, json::object());
    CHECK_NEAR(p["items"][0]["contact_area_mm2"].get<double>(), 600, 1e-2);
  }
}

TEST(thin_walls_name_both_sides_and_the_features_that_made_them) {
  Document doc = Document::create();
  const std::string cup = feature_cmd(doc, "box", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}}, "Cup block")["body_ids"][0];
  Scene s = resolve(doc);
  const int top = face_along(doc, s, cup, {0, 0, 1});
  feature_cmd(doc, "shell", {{"faces", json::array({cup + "/face/" + std::to_string(top)})}, {"thickness", "0.5 mm"}}, "Cup shell");
  s = resolve(doc);
  const json r = check_print(doc, s, {{"select", json::array({cup})}, {"min_wall_mm", 0.8}});
  const json& walls = r["items"][0]["thin_walls"];
  CHECK(!walls.empty());
  bool across = false;  // a side wall: the block's outside against the shell's inside
  for (const auto& w : walls) {
    CHECK(w["opposite_face"].get<int>() >= 0);
    CHECK_EQ(w["between"].size(), size_t(2));
    CHECK_EQ(w["between"][0]["face"], w["face"]);
    CHECK_EQ(w["between"][1]["face"], w["opposite_face"]);
    const gp_Pnt a(w["point"][0].get<double>(), w["point"][1].get<double>(), w["point"][2].get<double>());
    const gp_Pnt b(w["opposite_point"][0].get<double>(), w["opposite_point"][1].get<double>(), w["opposite_point"][2].get<double>());
    CHECK_NEAR(a.Distance(b), w["thickness_mm"].get<double>(), 1e-3);
    std::set<std::string> names;
    for (const auto& n : w.value("features", json::array())) names.insert(n.get<std::string>());
    across = across || (names.count("Cup block") && names.count("Cup shell"));
    for (const auto& side : w["between"]) CHECK(side.contains("created_by"));
  }
  CHECK(across);
  // Through validate, as agents ask.
  const json v = commands::run("validate", {{"checks", json::array({"print"})}, {"select", json::array({cup})}}, &doc);
  CHECK(v["print"]["items"][0]["thin_walls"][0].contains("features"));
}

TEST(a_pocket_coming_out_of_a_slanted_face_is_not_a_thin_wall) {
  // A block whose top slopes down at 30 degrees along x, with a pocket cut down into the slope: the pocket's downhill wall
  // meets the slope at 60 degrees, a sharp edge whose wedge is thin only right at the edge. Nowhere is a wall thinner
  // than 2.6 mm.
  const double slope = std::tan(30 * M_PI / 180);
  BRepBuilderAPI_MakePolygon profile;
  for (const auto& [x, z] : std::initializer_list<std::pair<double, double>>{{0, 0}, {30, 0}, {30, 20 - 30 * slope}, {0, 20}}) profile.Add(gp_Pnt(x, 0, z));
  profile.Close();
  const TopoDS_Shape block = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(profile.Wire()).Face(), gp_Vec(0, 20, 0)).Shape();
  const TopoDS_Shape pocket = BRepPrimAPI_MakeBox(gp_Pnt(8, 5, 5), gp_Pnt(20, 15, 50)).Shape();
  Document doc = Document::create();
  import_brep(doc, brep_from_shape(BRepAlgoAPI_Cut(block, pocket).Shape()), "Sloped block");
  const Scene s = resolve(doc);
  const std::string body = s.all_bodies().front();
  const json r = check_print(doc, s, {{"select", json::array({body})}, {"min_wall_mm", 0.8}});
  const json& item = r["items"][0];
  CHECK(item["thin_walls"].empty());
  CHECK(item["knife_edges"].get<int>() > 0);  // the samples by the sharp edges, left out
  // The same block 0.5 mm thick under the pocket is a wall again, and named as one.
  const TopoDS_Shape deep = BRepPrimAPI_MakeBox(gp_Pnt(8, 5, 0.5), gp_Pnt(20, 15, 50)).Shape();
  Document thin = Document::create();
  import_brep(thin, brep_from_shape(BRepAlgoAPI_Cut(block, deep).Shape()), "Thin floor");
  const Scene t = resolve(thin);
  const json w = check_print(thin, t, {{"min_wall_mm", 0.8}});
  CHECK(!w["items"][0]["thin_walls"].empty());
  double thinnest = 1e9;
  for (const auto& wall : w["items"][0]["thin_walls"]) thinnest = std::min(thinnest, wall["thickness_mm"].get<double>());
  CHECK_NEAR(thinnest, 0.5, 1e-3);
  CHECK_EQ(w["items"][0]["thin_walls"][0]["between"][0]["created_by"]["type"], "import");
}

TEST(mesh_thin_walls_name_the_opposite_triangle) {
  Files f;
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "0.5 mm"}}, "Plate");
  commands::run("export", {{"format", "stl"}, {"out", (f.dir / "plate.stl").string()}}, &doc);
  Document mesh = Document::create();
  import_file(mesh, f.dir / "plate.stl");
  const Scene s = resolve(mesh);
  const json r = check_print(mesh, s, {{"min_wall_mm", 0.8}});
  const json& item = r["items"][0];
  CHECK(item["mesh"].get<bool>());
  CHECK(!item["thin_walls"].empty());
  CHECK(item.contains("knife_edges"));
  const json& wall = item["thin_walls"][0];
  CHECK(wall["opposite_face"].get<int>() >= 0);
  CHECK(wall["opposite_face"] != wall["face"]);
  CHECK_NEAR(wall["thickness_mm"].get<double>(), 0.5, 1e-3);
  CHECK_NEAR(std::fabs(wall["opposite_point"][2].get<double>() - wall["point"][2].get<double>()), 0.5, 1e-3);
}

CHECK_MAIN()
