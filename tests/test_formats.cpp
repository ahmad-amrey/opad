// The file formats beyond STEP and the drawings, and viewer mode: a read that prepares nothing for saving, then the
// conversion to an editable document (make_editable) that keeps what was changed while viewing.
#include <BRepBndLib.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Writer.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <zlib.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../core/src/import_common.hpp"  // the DWG conversion's keep rule
#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/mesh.hpp"
#include "opad/render.hpp"
#include "opad/step_io.hpp"

using namespace opad;

namespace {
// The viewer cache lives under cache_dir(), which reads OPAD_CACHE_DIR once: aim it at a scratch folder before any test.
const std::filesystem::path kCacheDir = [] {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-formats-cache-" + new_uuid());
#ifdef _WIN32
  _putenv_s("OPAD_CACHE_DIR", dir.string().c_str());
#else
  setenv("OPAD_CACHE_DIR", dir.string().c_str(), 1);
#endif
  return dir;
}();
struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-formats-" + new_uuid());
  Files() { std::filesystem::create_directory(dir); }
  ~Files() { std::error_code e; std::filesystem::remove_all(dir, e); }
};

void write_binary(const std::filesystem::path& p, const std::string& data) {
  std::ofstream f(p, std::ios::binary);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

Document open(const std::filesystem::path& file, bool viewer) {
  Document d = Document::create();
  ImportOptions o;
  o.viewer = viewer;
  import_file(d, file, o);
  return d;
}

Bnd_Box scene_box(const Document& d) {
  const Scene s = resolve(d);
  Bnd_Box box;
  for (const auto& id : s.all_bodies()) BRepBndLib::Add(node_world_shape(d, s, id), box);
  return box;
}

int triangles(const Document& d) {
  int n = 0;
  for (const auto& key : d.body_keys())
    for (TopExp_Explorer f(body_shape(d, key), TopAbs_FACE); f.More(); f.Next()) {
      TopLoc_Location loc;
      if (const auto t = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc); !t.IsNull()) n += t->NbTriangles();
    }
  return n;
}

// A zip with the given entries; `deflate` compresses them as 3MF writers do.
std::string zip(const std::vector<std::pair<std::string, std::string>>& entries, bool packed_entries) {
  std::string out, directory;
  auto u16 = [](std::string& s, uint32_t v) { s += char(v & 0xFF); s += char((v >> 8) & 0xFF); };
  auto u32 = [&](std::string& s, uint32_t v) { u16(s, v & 0xFFFF); u16(s, v >> 16); };
  for (const auto& [name, raw] : entries) {
    std::string data = raw;
    if (packed_entries) {
      z_stream z{};
      deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
      std::string packed(compressBound(static_cast<uLong>(raw.size())) + 64, '\0');
      z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(raw.data()));
      z.avail_in = static_cast<uInt>(raw.size());
      z.next_out = reinterpret_cast<Bytef*>(packed.data());
      z.avail_out = static_cast<uInt>(packed.size());
      ::deflate(&z, Z_FINISH);
      packed.resize(z.total_out);
      deflateEnd(&z);
      data = packed;
    }
    const uint32_t crc = static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uInt>(raw.size())));
    const uint32_t offset = static_cast<uint32_t>(out.size());
    u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, packed_entries ? 8 : 0); u16(out, 0); u16(out, 0);
    u32(out, crc); u32(out, static_cast<uint32_t>(data.size())); u32(out, static_cast<uint32_t>(raw.size()));
    u16(out, static_cast<uint32_t>(name.size())); u16(out, 0);
    out += name + data;
    u32(directory, 0x02014b50); u16(directory, 20); u16(directory, 20); u16(directory, 0); u16(directory, packed_entries ? 8 : 0);
    u16(directory, 0); u16(directory, 0); u32(directory, crc); u32(directory, static_cast<uint32_t>(data.size()));
    u32(directory, static_cast<uint32_t>(raw.size())); u16(directory, static_cast<uint32_t>(name.size()));
    u16(directory, 0); u16(directory, 0); u16(directory, 0); u16(directory, 0); u32(directory, 0); u32(directory, offset);
    directory += name;
  }
  const uint32_t at = static_cast<uint32_t>(out.size());
  out += directory;
  u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, static_cast<uint32_t>(entries.size()));
  u16(out, static_cast<uint32_t>(entries.size())); u32(out, static_cast<uint32_t>(directory.size())); u32(out, at); u16(out, 0);
  return out;
}

const char* kTetra =
    "solid t\n"
    "facet normal 0 0 -1\nouter loop\nvertex 0 0 0\nvertex 0 10 0\nvertex 10 0 0\nendloop\nendfacet\n"
    "facet normal 0 -1 0\nouter loop\nvertex 0 0 0\nvertex 10 0 0\nvertex 0 0 10\nendloop\nendfacet\n"
    "facet normal -1 0 0\nouter loop\nvertex 0 0 0\nvertex 0 0 10\nvertex 0 10 0\nendloop\nendfacet\n"
    "facet normal 1 1 1\nouter loop\nvertex 10 0 0\nvertex 0 10 0\nvertex 0 0 10\nendloop\nendfacet\n"
    "endsolid t\n";
}  // namespace

TEST(viewer_step_then_editable_keeps_view_changes) {
  Files f;
  Document source = Document::create();
  import_brep(source, brep_from_shape(BRepPrimAPI_MakeBox(30, 20, 10).Shape()), "Block");
  ExportOptions eo;
  export_selection(source, resolve(source), f.dir / "block.step", eo);

  Document full = open(f.dir / "block.step", false);
  Document viewer = open(f.dir / "block.step", true);
  CHECK(!full.has_live_bodies());
  CHECK(viewer.has_live_bodies());
  CHECK_THROWS(viewer.save_as(f.dir / "nope.opad"));
  const Scene before = resolve(viewer);
  CHECK_EQ(before.all_bodies().size(), 1u);
  const std::string body = before.all_bodies().front();
  // Changes made while viewing: a hidden body and a colour.
  viewer.append({{"op", "appearance"}, {"target", body}, {"visible", false}});
  viewer.append({{"op", "appearance"}, {"target", body}, {"color", {1.0, 0.0, 0.0}}});

  EditableKeys keys;
  Document editable = make_editable(viewer, &keys);
  CHECK(!editable.has_live_bodies());
  CHECK_EQ(keys.renamed.size() + keys.healed.size(), 1u);
  CHECK_EQ(editable.body_keys(), full.body_keys());  // the same content key as a full import of the file
  editable.save_as(f.dir / "block.opad");
  const Document loaded = Document::load(f.dir / "block.opad");
  const Scene after = resolve(loaded);
  CHECK_EQ(after.all_bodies().size(), 1u);
  CHECK(!after.node(body)->visible);  // same node id, view changes kept
  CHECK_NEAR(after.node(body)->color[0], 1.0, 1e-9);
  Bnd_Box box = scene_box(loaded);
  CHECK_NEAR(box.CornerMax().X() - box.CornerMin().X(), 30.0, 0.05);
}

TEST(viewer_drawing_has_no_brep_text_until_editable) {
  Files f;
  write_text_file(f.dir / "plate.dxf", "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nCut\n10\n0\n20\n0\n11\n40\n21\n0\n0\nCIRCLE\n8\nHoles\n10\n5\n20\n5\n40\n2\n0\nENDSEC\n0\nEOF\n");
  Document viewer = open(f.dir / "plate.dxf", true);
  CHECK(viewer.has_live_bodies());
  CHECK_EQ(resolve(viewer).all_bodies().size(), 2u);
  Document editable = make_editable(viewer);
  CHECK_EQ(editable.body_keys(), open(f.dir / "plate.dxf", false).body_keys());
}

TEST(stl_ascii_and_binary_weld_and_keep_edges_sharp) {
  Files f;
  write_text_file(f.dir / "tetra.stl", kTetra);
  Document ascii = open(f.dir / "tetra.stl", true);
  CHECK_EQ(triangles(ascii), 4);
  const Scene s = resolve(ascii);
  CHECK_EQ(s.node(s.all_bodies().front())->representation, "mesh");
  // Every corner of a tetrahedron is sharp: 4 vertices x 3 directions, no smoothing across its edges.
  TopExp_Explorer face(body_shape(ascii, ascii.body_keys().front()), TopAbs_FACE);
  TopLoc_Location loc;
  const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(face.Current()), loc);
  CHECK_EQ(mesh->NbNodes(), 12);
  CHECK(mesh->HasNormals());
  // The same facets in binary; a "solid" header must not make it look like text.
  std::string binary(80, '\0');
  std::memcpy(binary.data(), "solid binary header", 19);
  const uint32_t count = 4;
  binary.append(reinterpret_cast<const char*>(&count), 4);
  const float facets[4][9] = {{0, 0, 0, 0, 10, 0, 10, 0, 0}, {0, 0, 0, 10, 0, 0, 0, 0, 10}, {0, 0, 0, 0, 0, 10, 0, 10, 0}, {10, 0, 0, 0, 10, 0, 0, 0, 10}};
  for (const auto& facet : facets) {
    binary.append(12, '\0');
    binary.append(reinterpret_cast<const char*>(facet), 36);
    binary.append(2, '\0');
  }
  write_binary(f.dir / "tetra_bin.stl", binary);
  Document bin = open(f.dir / "tetra_bin.stl", false);
  CHECK_EQ(triangles(bin), 4);
  // Saved and loaded again: the triangulation is the geometry.
  bin.save_as(f.dir / "tetra.opad");
  CHECK_EQ(triangles(Document::load(f.dir / "tetra.opad")), 4);
  write_text_file(f.dir / "broken.stl", "solid x\nfacet normal 0 0 1\nouter loop\nvertex 0 0\n");
  CHECK_THROWS(open(f.dir / "broken.stl", true));
}

TEST(ply_ascii_and_binary_polygons) {
  Files f;
  write_text_file(f.dir / "quad.ply",
                  "ply\nformat ascii 1.0\ncomment a square\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
                  "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n"
                  "0 0 0 255 0 0\n10 0 0 255 0 0\n10 10 0 255 0 0\n0 10 0 255 0 0\n4 0 1 2 3\n");
  Document a = open(f.dir / "quad.ply", true);
  CHECK_EQ(triangles(a), 2);
  const Scene s = resolve(a);
  CHECK_NEAR(s.node(s.all_bodies().front())->color[0], 1.0, 1e-9);
  std::string bin = "ply\nformat binary_little_endian 1.0\nelement vertex 3\nproperty double x\nproperty double y\nproperty double z\n"
                    "element face 1\nproperty list uchar uint vertex_index\nend_header\n";
  const double v[9] = {0, 0, 0, 5, 0, 0, 0, 5, 0};
  bin.append(reinterpret_cast<const char*>(v), sizeof v);
  bin += char(3);
  const uint32_t idx[3] = {0, 1, 2};
  bin.append(reinterpret_cast<const char*>(idx), sizeof idx);
  write_binary(f.dir / "tri.ply", bin);
  CHECK_EQ(triangles(open(f.dir / "tri.ply", false)), 1);
}

TEST(three_mf_instances_colours_units_and_components) {
  Files f;
  const std::string model =
      "<?xml version=\"1.0\"?>\n<model unit=\"centimeter\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" "
      "xmlns:p=\"http://schemas.microsoft.com/3dmanufacturing/production/2015/06\"><resources>"
      "<basematerials id=\"1\"><base name=\"Red\" displaycolor=\"#FF0000FF\"/><base name=\"Blue\" displaycolor=\"#0000FF80\"/></basematerials>"
      "<object id=\"2\" type=\"model\" name=\"Square &amp; Co\" pid=\"1\" pindex=\"1\"><mesh><vertices>"
      "<vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"1\" y=\"0\" z=\"0\"/><vertex x=\"1\" y=\"1\" z=\"0\"/><vertex x=\"0\" y=\"1\" z=\"0\"/>"
      "</vertices><triangles><triangle v1=\"0\" v2=\"1\" v3=\"2\"/><triangle v1=\"0\" v2=\"2\" v3=\"3\"/></triangles></mesh></object>"
      "<object id=\"3\" type=\"model\"><components><component objectid=\"7\" p:path=\"/3D/Objects/part.model\"/></components></object>"
      "</resources><build><item objectid=\"2\"/><item objectid=\"2\" transform=\"1 0 0 0 1 0 0 0 1 5 0 0\"/><item objectid=\"3\"/></build></model>";
  const std::string part =
      "<model unit=\"millimeter\"><resources><object id=\"7\" name=\"Shard\"><mesh><vertices><vertex x=\"0\" y=\"0\" z=\"0\"/>"
      "<vertex x=\"3\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"3\" z=\"0\"/></vertices><triangles><triangle v1=\"0\" v2=\"1\" v3=\"2\"/>"
      "</triangles></mesh></object></resources></model>";
  const std::string rels =
      "<Relationships><Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
      "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>";
  for (bool deflate : {false, true}) {
    write_binary(f.dir / "set.3mf", zip({{"_rels/.rels", rels}, {"3D/3dmodel.model", model}, {"3D/Objects/part.model", part}}, deflate));
    Document d = open(f.dir / "set.3mf", deflate);
    const Scene s = resolve(d);
    CHECK_EQ(s.all_bodies().size(), 3u);
    CHECK_EQ(d.body_count(), 2u);  // the square twice (one entry, two instances) and the shard
    const Node* first = s.node(s.all_bodies()[0]);
    CHECK_EQ(first->name, "Square & Co");
    CHECK_NEAR(first->color[2], 1.0, 1e-9);  // pindex 1: blue, half transparent
    CHECK_NEAR(first->opacity, 128.0 / 255.0, 1e-3);
    const Bnd_Box box = scene_box(d);
    CHECK_NEAR(box.CornerMax().X(), 60.0, 0.05);  // centimetres: 10 mm square, the copy 50 mm over
    CHECK_NEAR(box.CornerMax().Y(), 10.0, 0.05);
  }
  write_binary(f.dir / "bad.3mf", "PK not really");
  CHECK_THROWS(open(f.dir / "bad.3mf", true));
}

// A body's colour and the colours of its faces (in face order, the body's own as -1), with every face's triangle count.
struct Look {
  std::array<double, 3> color{};
  bool has_color = false;
  std::vector<std::array<double, 3>> faces;
  std::vector<int> triangles;
};
std::vector<Look> looks(const Document& d) {
  std::vector<Look> out;
  const Scene s = resolve(d);
  for (const auto& id : s.all_bodies()) {
    const Node* n = s.node(id);
    Look l{n->color, n->has_color, {}, {}};
    const FaceColors fc = face_colors(d, n->body_key);
    const TopoDS_Shape shape = body_shape(d, n->body_key);
    int i = 0;
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next(), ++i) {
      TopLoc_Location loc;
      const auto t = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc);
      l.triangles.push_back(t.IsNull() ? 0 : t->NbTriangles());
      l.faces.push_back(fc.at(i) < 0 ? std::array<double, 3>{-1, -1, -1} : fc.colors[size_t(fc.at(i))]);
    }
    out.push_back(l);
  }
  return out;
}
bool same(const std::array<double, 3>& a, const std::array<double, 3>& b) {
  return std::abs(a[0] - b[0]) < 0.003 && std::abs(a[1] - b[1]) < 0.003 && std::abs(a[2] - b[2]) < 0.003;
}
const std::array<double, 3> kOwn{-1, -1, -1}, kRed{1, 0, 0}, kGreen{0, 1, 0}, kBlue{0, 0, 1}, kWhite{1, 1, 1};

// Triangles coloured one by one keep their colours as faces of the body: 3MF materials per triangle, a slicer's painting and
// its parts' and volumes' filaments (Bambu Studio, PrusaSlicer).
TEST(three_mf_colours_per_triangle_painting_and_filaments) {
  Files f;
  const std::string square4 =
      "<mesh><vertices><vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"10\" y=\"0\" z=\"0\"/><vertex x=\"10\" y=\"10\" z=\"0\"/>"
      "<vertex x=\"0\" y=\"10\" z=\"0\"/><vertex x=\"5\" y=\"5\" z=\"0\"/></vertices><triangles>";
  auto tri = [](int a, int b, const std::string& extra) {
    return "<triangle v1=\"" + std::to_string(a) + "\" v2=\"" + std::to_string(b) + "\" v3=\"4\" " + extra + "/>";
  };
  const std::string rels =
      "<Relationships><Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/></Relationships>";
  // The core spec: a colour group, three triangles red and one green; no colour of the object's own.
  const std::string plain =
      "<model unit=\"millimeter\"><resources><colorgroup id=\"5\"><color color=\"#FF0000\"/><color color=\"#00FF00\"/></colorgroup>"
      "<object id=\"1\" name=\"Tile\">" + square4 + tri(0, 1, "pid=\"5\" p1=\"0\"") + tri(1, 2, "pid=\"5\" p1=\"1\"") +
      tri(2, 3, "pid=\"5\" p1=\"0\"") + tri(3, 0, "pid=\"5\" p1=\"0\"") + "</triangles></mesh></object></resources>"
      "<build><item objectid=\"1\"/></build></model>";
  write_binary(f.dir / "plain.3mf", zip({{"_rels/.rels", rels}, {"3D/3dmodel.model", plain}}, true));
  for (bool viewer : {true, false}) {
    const auto all = looks(open(f.dir / "plain.3mf", viewer));
    CHECK_EQ(all.size(), 1u);
    CHECK(all[0].has_color && same(all[0].color, kRed));
    CHECK_EQ(all[0].faces.size(), 2u);
    CHECK(same(all[0].faces[0], kOwn) && all[0].triangles[0] == 3 && same(all[0].faces[1], kGreen) && all[0].triangles[1] == 1);
  }
  // Bambu Studio: an object of two parts, one printed in filament 2, the other in the object's filament 1 with one triangle
  // painted mostly in filament 2 (split in four, three of them) and one in filament 4 (an extended state).
  const std::string bambuRoot =
      "<model unit=\"millimeter\" xmlns:p=\"http://schemas.microsoft.com/3dmanufacturing/production/2015/06\"><resources>"
      "<object id=\"2\" type=\"model\"><components><component p:path=\"/3D/Objects/object_1.model\" objectid=\"1\"/>"
      "<component p:path=\"/3D/Objects/object_1.model\" objectid=\"3\"/></components></object></resources>"
      "<build><item objectid=\"2\"/></build></model>";
  const std::string bambuParts =
      "<model unit=\"millimeter\"><resources><object id=\"1\" type=\"model\">" + square4 + tri(0, 1, "") + tri(1, 2, "") + tri(2, 3, "") +
      tri(3, 0, "") + "</triangles></mesh></object><object id=\"3\" type=\"model\">" + square4 + tri(0, 1, "") + tri(1, 2, "paint_color=\"08883\"") +
      tri(2, 3, "paint_color=\"1C\"") + tri(3, 0, "paint_color=\"0\"") + "</triangles></mesh></object></resources><build/></model>";
  const std::string bambuSettings =
      "<config><object id=\"2\"><metadata key=\"name\" value=\"Painted\"/><metadata key=\"extruder\" value=\"1\"/>"
      "<part id=\"1\" subtype=\"normal_part\"><metadata key=\"name\" value=\"Red part\"/><metadata key=\"extruder\" value=\"2\"/></part>"
      "<part id=\"3\" subtype=\"normal_part\"><metadata key=\"name\" value=\"White part\"/></part></object></config>";
  write_binary(f.dir / "bambu.3mf", zip({{"_rels/.rels", rels}, {"3D/3dmodel.model", bambuRoot}, {"3D/Objects/object_1.model", bambuParts},
                                         {"Metadata/model_settings.config", bambuSettings},
                                         {"Metadata/project_settings.config", "{\"filament_colour\": [\"#FFFFFF\", \"#FF0000\", \"#00FF00\", \"#0000FF\"]}"}},
                                        true));
  for (bool viewer : {true, false}) {
    const Document d = open(f.dir / "bambu.3mf", viewer);
    const auto all = looks(d);
    CHECK_EQ(all.size(), 2u);
    CHECK(same(all[0].color, kRed) && all[0].faces.size() == 1 && same(all[0].faces[0], kOwn));
    CHECK(same(all[1].color, kWhite) && all[1].faces.size() == 3);
    CHECK(same(all[1].faces[0], kOwn) && all[1].triangles[0] == 2);
    CHECK(same(all[1].faces[1], kRed) && all[1].triangles[1] == 1);
    CHECK(same(all[1].faces[2], kBlue) && all[1].triangles[2] == 1);
    CHECK_EQ(resolve(d).node(resolve(d).roots.front())->children.size(), 1u);
  }
  // PrusaSlicer: one mesh, a volume in extruder 2 and a painted triangle; the first extruder's own colour, the second the
  // filament's (its extruder colour is empty).
  const std::string prusa =
      "<model unit=\"millimeter\" xmlns:slic3rpe=\"http://schemas.slic3r.org/3mf/2017/06\"><resources><object id=\"1\" type=\"model\">" + square4 +
      tri(0, 1, "slic3rpe:mmu_segmentation=\"8\"") + tri(1, 2, "") + tri(2, 3, "") + tri(3, 0, "") +
      "</triangles></mesh></object></resources><build><item objectid=\"1\"/></build></model>";
  const std::string prusaModel =
      "<config><object id=\"1\" instances_count=\"1\"><metadata type=\"object\" key=\"name\" value=\"Box\"/>"
      "<volume firstid=\"0\" lastid=\"1\"><metadata type=\"volume\" key=\"extruder\" value=\"0\"/></volume>"
      "<volume firstid=\"2\" lastid=\"3\"><metadata type=\"volume\" key=\"extruder\" value=\"2\"/></volume></object></config>";
  write_binary(f.dir / "prusa.3mf", zip({{"_rels/.rels", rels}, {"3D/3dmodel.model", prusa}, {"Metadata/Slic3r_PE_model.config", prusaModel},
                                         {"Metadata/Slic3r_PE.config", "; extruder_colour = \"#FF0000\";\"\"\n; filament_colour = #00FF00;#0000FF\n"}},
                                        false));
  const auto all = looks(open(f.dir / "prusa.3mf", true));
  CHECK_EQ(all.size(), 1u);
  CHECK(same(all[0].color, kRed) && all[0].faces.size() == 2);
  CHECK(same(all[0].faces[0], kOwn) && all[0].triangles[0] == 1 && same(all[0].faces[1], kBlue) && all[0].triangles[1] == 3);
}

// PLY colours by face or by vertex: a few become the body's and faces of their own, a scan's many their average.
TEST(ply_face_and_vertex_colours) {
  Files f;
  const std::string head = "ply\nformat ascii 1.0\nelement vertex 5\nproperty float x\nproperty float y\nproperty float z\n";
  const std::string points = "0 0 0\n10 0 0\n10 10 0\n0 10 0\n5 5 0\n";
  write_text_file(f.dir / "faces.ply", head + "element face 3\nproperty list uchar int vertex_indices\nproperty uchar red\nproperty uchar green\n"
                                              "property uchar blue\nend_header\n" + points + "4 0 1 2 3 255 0 0\n3 0 1 4 255 0 0\n3 1 2 4 0 0 255\n");
  for (bool viewer : {true, false}) {
    const auto all = looks(open(f.dir / "faces.ply", viewer));
    CHECK(all.size() == 1 && same(all[0].color, kRed) && all[0].faces.size() == 2);
    CHECK(same(all[0].faces[0], kOwn) && all[0].triangles[0] == 3 && same(all[0].faces[1], kBlue) && all[0].triangles[1] == 1);
  }
  write_text_file(f.dir / "vertices.ply", head + "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face 4\n"
                                                 "property list uchar int vertex_indices\nend_header\n"
                                                 "0 0 0 0 255 0\n10 0 0 0 255 0\n10 10 0 0 0 255\n0 10 0 0 0 255\n5 5 0 0 255 0\n"
                                                 "3 0 1 4\n3 1 2 4\n3 2 3 4\n3 3 0 4\n");
  const auto byVertex = looks(open(f.dir / "vertices.ply", true));
  CHECK(byVertex.size() == 1 && byVertex[0].faces.size() == 2);  // green where two corners are, blue on the far side
  CHECK(same(byVertex[0].faces[1], kBlue) && byVertex[0].triangles[1] == 1);
  std::string scan = "ply\nformat ascii 1.0\nelement vertex 300\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\n"
                     "property uchar green\nproperty uchar blue\nelement face 100\nproperty list uchar int vertex_indices\nend_header\n";
  for (int i = 0; i < 100; ++i)
    for (int k = 0; k < 3; ++k) scan += std::to_string(i * 3 + (k == 1)) + " " + std::to_string(k == 2) + " 0 " + std::to_string(i + 50) + " 100 100\n";
  for (int i = 0; i < 100; ++i) scan += "3 " + std::to_string(i * 3) + " " + std::to_string(i * 3 + 1) + " " + std::to_string(i * 3 + 2) + "\n";
  write_text_file(f.dir / "scan.ply", scan);
  const auto many = looks(open(f.dir / "scan.ply", true));
  CHECK(many.size() == 1 && many[0].faces.size() == 1 && many[0].has_color);
  CHECK_NEAR(many[0].color[0], 99.5 / 255, 0.003);
}

TEST(obj_polygons_groups_and_material_colours) {
  Files f;
  write_text_file(f.dir / "parts.mtl", "newmtl green\nKd 0 1 0\n");
  write_text_file(f.dir / "parts.obj",
                  "mtllib parts.mtl\no Plate\nusemtl green\nv 0 0 0\nv 10 0 0\nv 10 0 10\nv 0 0 10\nf 1 2 3 4\n"
                  "o Wedge\nv 0 0 20\nv 5 0 20\nv 0 5 20\nf 5 6 7\n");
  Document d = open(f.dir / "parts.obj", true);
  const Scene s = resolve(d);
  CHECK(s.all_bodies().size() >= 2u);
  CHECK_EQ(triangles(d), 3);
  bool green = false, named = false;
  for (const auto& id : s.all_bodies()) {
    green = green || (s.node(id)->has_color && s.node(id)->color[1] > 0.9 && s.node(id)->color[0] < 0.1);
    const json props = node_properties(d, s, id, false);
    named = named || props.value("material", "") == "green";
    CHECK_EQ(props.value("representation", ""), "mesh");
  }
  CHECK(green);
  CHECK(named);  // the material's own name, shown in Properties
  // Y-up: the plate drawn in the file's XZ plane stands in OPAD's XY plane.
  const Bnd_Box box = scene_box(d);
  CHECK_NEAR(box.CornerMax().Z() - box.CornerMin().Z(), 5.0, 0.05);
}

TEST(face_colors_are_run_lengths_and_read_tolerantly) {
  FaceColors colors;
  colors.colors = {{1, 0, 0}, {0, 1, 0}};
  colors.face = {-1, 0, 0, 1, -1, -1};
  const json j = colors.to_json();
  CHECK(j["runs"] == json::array({1, -1, 2, 0, 1, 1}));  // the faces after the last run are the body's colour
  const FaceColors back = FaceColors::from_json(j);
  CHECK_EQ(back.face.size(), 4u);
  CHECK_EQ(back.at(2), 0);
  CHECK_EQ(back.at(3), 1);
  CHECK_EQ(back.at(5), -1);
  CHECK(FaceColors::from_json(json{{"colors", {{1, 0, 0}}}, {"runs", {2, 5}}}).empty());  // no colour 5
  CHECK(FaceColors::from_json(json{{"colors", {{1, 0, 0}}}, {"runs", {-2, 0}}}).empty());
  CHECK(FaceColors::from_json(json("nonsense")).empty());
  CHECK(FaceColors{}.to_json().is_null());
}

// OBJ materials (and glTF's) are kept as the file shows them: OCCT holds them linear, and taken as they came a Kd of 0.439
// was 0.162 (the model drew nearly black). Each material of an object is a colour group of its body, which the exports keep.
TEST(obj_materials_keep_their_shown_colour_as_face_groups) {
  Files f;
  write_text_file(f.dir / "two.mtl", "newmtl grey\nKd 0.439 0.439 0.439\nnewmtl red\nKd 1 0 0\n");
  write_text_file(f.dir / "two.obj", "mtllib two.mtl\no Block\nv 0 0 0\nv 10 0 0\nv 10 10 0\nv 0 10 0\nv 0 0 10\nv 10 0 10\n"
                                     "usemtl grey\nf 1 2 3\nf 1 3 4\nf 1 2 6\nusemtl red\nf 1 6 5\n");
  auto expect = [](const Document& d, const std::string& what) {
    const Scene s = resolve(d);
    CHECK_EQ(s.all_bodies().size(), 1u);
    const Node* n = s.node(s.all_bodies().front());
    if (!n->has_color || std::abs(n->color[0] - 0.439) > 0.003) throw check::Failure(what + ": body colour " + std::to_string(n->color[0]));
    const FaceColors faces = face_colors(d, n->body_key);
    CHECK_EQ(faces.colors.size(), 1u);
    CHECK_NEAR(faces.colors[0][0], 1.0, 0.003);
    CHECK_NEAR(faces.colors[0][1], 0.0, 0.003);
    CHECK_EQ(std::count_if(faces.face.begin(), faces.face.end(), [](int c) { return c == 0; }), 1);
  };
  for (bool viewer : {true, false}) expect(open(f.dir / "two.obj", viewer), viewer ? "viewer" : "full");
  Document d = open(f.dir / "two.obj", false);
  ExportOptions eo;
  eo.format = "obj";
  export_selection(d, resolve(d), f.dir / "out.obj", eo);
  const std::string mtl = read_text_file(f.dir / "out.mtl");
  CHECK(mtl.find("newmtl m0\nKd 0.4390 0.4390 0.4390") != std::string::npos);
  CHECK(mtl.find("newmtl m0_0\nKd 1.0000 0.0000 0.0000") != std::string::npos);
  expect(open(f.dir / "out.obj", false), "OBJ written and read again");
  // glTF stores colours linear and OPAD reads them back as shown; a body of several faces stays one node, its faces primitives.
  eo.format = "glb";
  export_selection(d, resolve(d), f.dir / "out.glb", eo);
  expect(open(f.dir / "out.glb", true), "glTF written and read again");
}

// A STEP that styles single faces (KiCad's models: a black body, gold pins) keeps them: the body takes the common colour,
// the others are face colours, through viewer mode and the conversion to editable, the STEP and OBJ exports and the renderer.
TEST(step_face_colours_follow_the_faces) {
  Files f;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 10).Shape();
  Handle(TDocStd_Document) xdoc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", xdoc);
  const auto st = XCAFDoc_DocumentTool::ShapeTool(xdoc->Main());
  const auto ct = XCAFDoc_DocumentTool::ColorTool(xdoc->Main());
  const TDF_Label part = st->AddShape(box, Standard_False);
  auto top = [](const TopoDS_Shape& face) {
    Bnd_Box b;
    BRepBndLib::Add(face, b);
    return b.CornerMin().Z() > 9.9;
  };
  for (TopExp_Explorer e(box, TopAbs_FACE); e.More(); e.Next())
    ct->SetColor(st->AddSubShape(part, e.Current()), top(e.Current()) ? Quantity_Color(0.9, 0.7, 0.2, Quantity_TOC_RGB) : Quantity_Color(0.1, 0.1, 0.1, Quantity_TOC_RGB), XCAFDoc_ColorSurf);
  STEPCAFControl_Writer writer;
  writer.SetColorMode(Standard_True);
  CHECK(writer.Transfer(xdoc, STEPControl_AsIs));
  CHECK(writer.Write((f.dir / "part.step").string().c_str()) == IFSelect_RetDone);

  auto expect = [&](const Document& d, const std::string& what) {
    const Scene s = resolve(d);
    CHECK_EQ(s.all_bodies().size(), 1u);
    const Node* n = s.node(s.all_bodies().front());
    const FaceColors faces = face_colors(d, n->body_key);
    if (!n->has_color || std::abs(n->color[0] - 0.1) > 0.003 || faces.colors.size() != 1) throw check::Failure(what + ": body or face colours");
    CHECK_NEAR(faces.colors[0][0], 0.9, 0.003);
    CHECK_NEAR(faces.colors[0][2], 0.2, 0.003);
    const TopoDS_Shape shape = body_shape(d, n->body_key);
    int gold = 0;
    for (int i = 0; i < subshape_count(shape, Ref::Kind::Face); ++i)
      if (faces.at(i) == 0) {
        ++gold;
        if (!top(subshape(shape, Ref::Kind::Face, i))) throw check::Failure(what + ": the gold face is not the top");
      }
    CHECK_EQ(gold, 1);
  };
  Document full = open(f.dir / "part.step", false), viewer = open(f.dir / "part.step", true);
  expect(full, "full read");
  expect(viewer, "viewer");
  expect(make_editable(viewer), "viewer made editable");
  ExportOptions eo;
  export_selection(full, resolve(full), f.dir / "again.step", eo);
  expect(open(f.dir / "again.step", false), "STEP written and read again");
  eo.format = "obj";
  export_selection(full, resolve(full), f.dir / "part.obj", eo);
  CHECK(read_text_file(f.dir / "part.mtl").find("newmtl m0_0\nKd 0.9000 0.7000 0.2000") != std::string::npos);

  // The renderer: from above the top is gold; the body recoloured red keeps it, and its sides turn red.
  const std::string body = resolve(full).all_bodies().front();
  full.append({{"op", "appearance"}, {"target", body}, {"color", {1.0, 0.0, 0.0}}});
  RenderOptions ro;
  ro.width = 120;
  ro.height = 90;
  ro.edges = false;
  auto centre = [&](const std::string& view) {
    ro.camera = Camera::preset(view);
    const Image image = render_scene(full, resolve(full), ro);
    const uint8_t* p = image.px(image.width / 2, image.height / 2);
    return std::array<int, 3>{p[0], p[1], p[2]};
  };
  const auto above = centre("top"), side = centre("front");
  CHECK(above[0] > above[2] + 60 && above[1] > above[2] + 30);  // gold
  CHECK(side[0] > side[1] + 60 && side[0] > side[2] + 60);      // red
}

// A mesh has no surfaces or curves: its triangles are what the Bounding box measure and Properties' size see (both
// came out empty for every STL/OBJ body), also turned (OBJ is Y-up) and far from the origin.
TEST(mesh_bodies_have_a_tight_box) {
  Files f;
  write_text_file(f.dir / "tetra.stl", kTetra);
  write_text_file(f.dir / "far.obj", "v 24000 0 -601000\nv 54000 0 -601000\nv 54000 0 -621000\nv 24000 12000 -601000\nf 1 2 3\nf 1 2 4\n");
  for (const auto& [file, size] : {std::pair<const char*, Vec3>{"tetra.stl", {10, 10, 10}}, {"far.obj", {30000, 20000, 12000}}}) {
    for (bool viewer : {true, false}) {
      const Document d = open(f.dir / file, viewer);
      const Scene s = resolve(d);
      const std::string body = s.all_bodies().front();
      const json box = node_properties(d, s, body, true).value("bbox", json::object());
      CHECK(box.contains("size"));
      Ref r;
      r.kind = Ref::Kind::Body;
      r.body = body;
      const json measured = measure_bbox(d, s, {r});
      for (int i = 0; i < 3; ++i) {
        CHECK_NEAR(box["size"][i].get<double>(), size[i], 1e-3);
        CHECK_NEAR(measured["size"][i].get<double>(), size[i], 1e-3);
      }
    }
  }
}

TEST(gltf_round_trip_keeps_millimetres) {
  Files f;
  Document source = Document::create();
  import_brep(source, brep_from_shape(BRepPrimAPI_MakeBox(40, 20, 10).Shape()), "Block");
  ExportOptions eo;
  eo.format = "glb";
  export_selection(source, resolve(source), f.dir / "block.glb", eo);
  Document d = open(f.dir / "block.glb", true);
  const Scene s = resolve(d);
  CHECK_EQ(s.node(s.all_bodies().front())->representation, "mesh");
  const Bnd_Box box = scene_box(d);
  CHECK_NEAR(box.CornerMax().X() - box.CornerMin().X(), 40.0, 0.05);
  CHECK_NEAR(box.CornerMax().Z() - box.CornerMin().Z(), 10.0, 0.05);  // Z up again
  Document editable = make_editable(d);
  editable.save_as(f.dir / "block.opad");
  CHECK(triangles(Document::load(f.dir / "block.opad")) >= 12);
}

TEST(iges_brep_and_vrml) {
  Files f;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(12, 8, 4).Shape();
  IGESControl_Controller::Init();
  IGESControl_Writer writer("MM", 1);
  writer.AddShape(box);
  writer.ComputeModel();
  CHECK(writer.Write((f.dir / "box.igs").string().c_str()));
  Document iges = open(f.dir / "box.igs", true);
  Bnd_Box b = scene_box(iges);
  CHECK_NEAR(b.CornerMax().X() - b.CornerMin().X(), 12.0, 0.05);
  CHECK(!make_editable(iges).has_live_bodies());

  BRepTools::Write(box, (f.dir / "box.brep").string().c_str());
  b = scene_box(open(f.dir / "box.brep", true));
  CHECK_NEAR(b.CornerMax().Y() - b.CornerMin().Y(), 8.0, 0.05);

  write_text_file(f.dir / "square.wrl",
                  "#VRML V2.0 utf8\nShape { geometry IndexedFaceSet { coord Coordinate { point [0 0 0, 1 0 0, 1 1 0, 0 1 0] } "
                  "coordIndex [0 1 2 3 -1] } }\n");
  b = scene_box(open(f.dir / "square.wrl", true));
  CHECK_NEAR(b.CornerMax().X() - b.CornerMin().X(), 1000.0, 0.5);  // metres, Y up
  CHECK_NEAR(b.CornerMax().Z() - b.CornerMin().Z(), 1000.0, 0.5);
  CHECK_NEAR(b.CornerMax().X(), 1000.0, 0.5);  // not mirrored (OCCT's reader scales by -1 unless told the file's unit)
  CHECK_NEAR(b.CornerMax().Z(), 1000.0, 0.5);
}

TEST(viewer_cache_round_trip_and_invalidation) {
  Files f;
  Document source = Document::create();
  import_brep(source, brep_from_shape(BRepPrimAPI_MakeBox(25, 15, 5).Shape()), "Plate");
  ExportOptions eo;
  export_selection(source, resolve(source), f.dir / "plate.step", eo);
  ImportOptions viewer;
  viewer.viewer = true;
  Document miss = Document::create();
  CHECK(!viewer_cache_load(miss, f.dir / "plate.step", viewer));
  Document first = open(f.dir / "plate.step", true);
  CHECK(viewer_cache_store(first, f.dir / "plate.step", viewer, 60000).value("kept", false));  // a minute's read: any entry wins
  Document again = Document::create();
  CHECK(viewer_cache_load(again, f.dir / "plate.step", viewer));
  CHECK(again.has_live_bodies());
  const Scene s = resolve(again);
  CHECK_EQ(s.all_bodies().size(), resolve(first).all_bodies().size());
  CHECK_EQ(s.node(s.all_bodies().front())->name, resolve(first).node(resolve(first).all_bodies().front())->name);
  const Bnd_Box box = scene_box(again);
  CHECK_NEAR(box.CornerMax().X() - box.CornerMin().X(), 25.0, 0.05);
  CHECK_EQ(make_editable(again).body_keys(), open(f.dir / "plate.step", false).body_keys());  // saves like a fresh read
  // Kept by content (UI-75): a copy elsewhere and the file touched find it; the file changed is read again.
  std::filesystem::copy_file(f.dir / "plate.step", f.dir / "copy.step");
  std::filesystem::last_write_time(f.dir / "plate.step", std::filesystem::last_write_time(f.dir / "plate.step") + std::chrono::seconds(5));
  for (const char* name : {"copy.step", "plate.step"}) {
    Document found = Document::create();
    CHECK(viewer_cache_load(found, f.dir / name, viewer));
  }
  write_text_file(f.dir / "plate.step", read_text_file(f.dir / "plate.step") + "\n");
  Document stale = Document::create();
  CHECK(!viewer_cache_load(stale, f.dir / "plate.step", viewer));
  // An entry that does not read back twice as fast as the file is not kept, nor tried again until the file changes.
  Document bar = Document::create();
  import_brep(bar, brep_from_shape(BRepPrimAPI_MakeBox(40, 4, 4).Shape()), "Bar");
  export_selection(bar, resolve(bar), f.dir / "bar.step", eo);
  const Document read = open(f.dir / "bar.step", true);
  json report = viewer_cache_store(read, f.dir / "bar.step", viewer, 0.001);
  CHECK(!report.value("kept", true) && report.value("reason", "") == "slower");
  Document none = Document::create();
  CHECK(!viewer_cache_load(none, f.dir / "bar.step", viewer));
  CHECK(!viewer_cache_store(read, f.dir / "bar.step", viewer, 60000).value("kept", true));
  write_text_file(f.dir / "bar.step", read_text_file(f.dir / "bar.step") + "\n");
  CHECK(viewer_cache_store(read, f.dir / "bar.step", viewer, 60000).value("kept", false));
  // Drawings are never kept that way (their entries were bigger than the file and no faster).
  write_text_file(f.dir / "plate.dxf", "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nCut\n10\n0\n20\n0\n11\n40\n21\n0\n0\nENDSEC\n0\nEOF\n");
  CHECK(!viewer_cache_applies(f.dir / "plate.dxf") && !viewer_cache_applies(f.dir / "a.svg") && !viewer_cache_applies(f.dir / "a.DWG"));
  CHECK(viewer_cache_applies(f.dir / "a.step") && viewer_cache_applies(f.dir / "a.stl"));
  report = viewer_cache_store(open(f.dir / "plate.dxf", true), f.dir / "plate.dxf", viewer, 60000);
  CHECK(!report.value("kept", true) && report.value("reason", "") == "drawing");
  Document drawing = Document::create();
  CHECK(!viewer_cache_load(drawing, f.dir / "plate.dxf", viewer));
  // A big mesh (over 16 MB, read about as fast as hashed) is not hashed to be looked up: the hash its store remembered for
  // its path (size and time) finds it, a copy elsewhere is read.
  std::string stl(80, '\0');
  const uint32_t count = 340000;
  stl.append(reinterpret_cast<const char*>(&count), 4);
  for (uint32_t t = 0; t < count; ++t) {
    const float x = float(t % 1000), y = float(t / 1000);
    const float facet[12] = {0, 0, 1, x, y, 0, x + 1, y, 0, x, y + 1, 0};
    stl.append(reinterpret_cast<const char*>(facet), sizeof facet);
    stl.append(2, '\0');
  }
  write_binary(f.dir / "big.stl", stl);
  write_binary(f.dir / "copy.stl", stl);
  const auto past = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);  // not written moments ago
  std::filesystem::last_write_time(f.dir / "big.stl", past);
  std::filesystem::last_write_time(f.dir / "copy.stl", past);
  Document unseen = Document::create();
  CHECK(!viewer_cache_load(unseen, f.dir / "big.stl", viewer));
  CHECK(viewer_cache_store(open(f.dir / "big.stl", true), f.dir / "big.stl", viewer, 60000).value("kept", false));
  Document remembered = Document::create();
  CHECK(viewer_cache_load(remembered, f.dir / "big.stl", viewer));
  Document copy = Document::create();
  CHECK(!viewer_cache_load(copy, f.dir / "copy.stl", viewer));
  std::error_code e;
  std::filesystem::remove_all(kCacheDir, e);
}

// A DWG keeps the DXF its conversion made, by the DWG's content and the converter (UI-75): the same drawing, here or copied
// elsewhere, viewed or imported, opens again without converting; another converter converts again.
TEST(dwg_keeps_its_converted_dxf_by_content) {
#ifdef OPAD_FAKE_DWG2DXF
  Files f;
  auto set = [](const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    value.empty() ? unsetenv(name) : setenv(name, value.c_str(), 1);
#endif
  };
  const auto log = f.dir / "converted.log";
  set("OPAD_DWG2DXF", OPAD_FAKE_DWG2DXF);
  set("OPAD_FAKE_DWG_LOG", log.string());
  set("OPAD_FAKE_DWG_SLEEP", "400");  // converting takes longer than reading the DXF, as it does for real drawings
  auto converted = [&] {
    std::error_code e;
    if (!std::filesystem::exists(log, e)) return 0;
    const std::string text = read_text_file(log);
    return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
  };
  write_text_file(f.dir / "plate.dwg", "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nCut\n10\n0\n20\n0\n11\n40\n21\n0\n0\nCIRCLE\n8\nHoles\n10\n5\n20\n5\n40\n2\n0\nENDSEC\n0\nEOF\n");
  auto named = [](const Document& d) {
    const Scene s = resolve(d);
    return s.node(s.roots.front())->name + "|" + d.ops.back().data.value("source", "");
  };
  const Document first = open(f.dir / "plate.dwg", true);
  CHECK_EQ(converted(), 1);
  const Document again = open(f.dir / "plate.dwg", true);
  CHECK_EQ(converted(), 1);
  CHECK_EQ(resolve(again).all_bodies().size(), 2u);
  CHECK_EQ(named(again), "plate|plate.dxf");  // named after the drawing, not the kept file
  std::filesystem::copy_file(f.dir / "plate.dwg", f.dir / "other.dwg");
  const Document copy = open(f.dir / "other.dwg", false);
  CHECK_EQ(converted(), 1);
  CHECK_EQ(named(copy), "other|other.dxf");
  CHECK_EQ(copy.body_keys(), make_editable(first).body_keys());
  set("OPAD_DWG2DXF", (std::filesystem::path(OPAD_FAKE_DWG2DXF).parent_path() / "." / std::filesystem::path(OPAD_FAKE_DWG2DXF).filename()).string());
  open(f.dir / "plate.dwg", true);
  CHECK_EQ(converted(), 2);
  for (const char* name : {"OPAD_DWG2DXF", "OPAD_FAKE_DWG_LOG", "OPAD_FAKE_DWG_SLEEP"}) set(name, "");
#endif
  // Kept only when it opens the drawing at least twice as fast: converting took at least as long as reading the DXF.
  Files g;
  write_text_file(g.dir / "rule.dwg", "a drawing of its own content");
  write_text_file(g.dir / "rule.dxf", "0\nEOF\n");
  CHECK(!detail::dwg_cache_keep(g.dir / "rule.dwg", "rule", g.dir / "rule.dxf", 1700, 2400));  // 1.7x: the drawing of UI-75
  CHECK(!detail::dwg_cache_keep(g.dir / "rule.dwg", "rule", g.dir / "rule.dxf", 200, 10));     // too quick to matter
  CHECK(detail::dwg_cache_find(g.dir / "rule.dwg", "rule").empty());
  CHECK(detail::dwg_cache_keep(g.dir / "rule.dwg", "rule", g.dir / "rule.dxf", 1700, 850));
  CHECK(!detail::dwg_cache_find(g.dir / "rule.dwg", "rule").empty());
}

// UI-07: an import op records its source absolute and inside its git work tree, found again after a clone of the
// repository (the absolute path gone), beside the document for an op that names only the file, never invented.
TEST(import_records_and_finds_its_source) {
  Files f;
  namespace fs = std::filesystem;
  fs::create_directories(f.dir / "repo" / ".git");
  fs::create_directories(f.dir / "repo" / "parts");
  fs::create_directories(f.dir / "repo" / "doc");
  const fs::path part = f.dir / "repo" / "parts" / path_from_utf8("\xd9\x82\xd8\xb7\xd8\xb9\xd8\xa9.stl");  // قطعة.stl
  write_text_file(part, kTetra);
  Document d = open(part, false);
  const json& op = d.ops.back().data;
  const auto generic = fs::absolute(part).lexically_normal().generic_u8string();
  CHECK_EQ(op.value("source_path", ""), std::string(reinterpret_cast<const char*>(generic.data()), generic.size()));
  CHECK_EQ(op.value("source_repo", ""), "parts/\xd9\x82\xd8\xb7\xd8\xb9\xd8\xa9.stl");
  CHECK(repo_top(part) == f.dir / "repo");
  bool exists = false;
  CHECK(import_source(op, f.dir / "repo" / "doc" / "model.opad", &exists) == part.lexically_normal().make_preferred() && exists);
  fs::copy(f.dir / "repo", f.dir / "clone", fs::copy_options::recursive);
  fs::remove(part);
  const fs::path cloned = f.dir / "clone" / "parts" / part.filename();
  CHECK(import_source(op, f.dir / "clone" / "doc" / "model.opad", &exists) == cloned.lexically_normal().make_preferred() && exists);
  fs::remove(cloned);
  CHECK(import_source(op, f.dir / "clone" / "doc" / "model.opad", &exists) == part.lexically_normal().make_preferred() && !exists);
  // An op of an older build names the file only: beside the document.
  write_text_file(f.dir / "clone" / "doc" / "old.stl", kTetra);
  CHECK(import_source(json{{"source", "old.stl"}}, f.dir / "clone" / "doc" / "model.opad", &exists) == (f.dir / "clone" / "doc" / "old.stl").lexically_normal().make_preferred() && exists);
  CHECK(import_source(json{{"source", ""}}, f.dir / "clone" / "doc" / "model.opad", &exists).empty() && !exists);
  // Outside a work tree: the absolute path only; saved and read back, the op keeps both.
  write_text_file(f.dir / "loose.stl", kTetra);
  Document loose = open(f.dir / "loose.stl", false);
  CHECK(loose.ops.back().data.contains("source_path") && !loose.ops.back().data.contains("source_repo"));
  d.save_as(f.dir / "model.opad");
  CHECK_EQ(Document::load(f.dir / "model.opad").ops.back().data.value("source_repo", ""), op.value("source_repo", ""));
}

TEST(unsupported_and_missing_files_fail_cleanly) {
  Files f;
  write_text_file(f.dir / "notes.txt", "hello");
  CHECK_THROWS(open(f.dir / "notes.txt", true));
  CHECK_THROWS(open(f.dir / "missing.stl", true));
  write_text_file(f.dir / "empty.obj", "# nothing\n");
  CHECK_THROWS(open(f.dir / "empty.obj", true));
  write_text_file(f.dir / "garbage.step", "ISO-10303-21;\nnot really\n");
  CHECK_THROWS(open(f.dir / "garbage.step", true));
}

CHECK_MAIN()
