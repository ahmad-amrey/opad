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
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <zlib.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
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
  viewer_cache_store(first, f.dir / "plate.step", viewer);
  Document again = Document::create();
  CHECK(viewer_cache_load(again, f.dir / "plate.step", viewer));
  CHECK(again.has_live_bodies());
  const Scene s = resolve(again);
  CHECK_EQ(s.all_bodies().size(), resolve(first).all_bodies().size());
  CHECK_EQ(s.node(s.all_bodies().front())->name, resolve(first).node(resolve(first).all_bodies().front())->name);
  const Bnd_Box box = scene_box(again);
  CHECK_NEAR(box.CornerMax().X() - box.CornerMin().X(), 25.0, 0.05);
  CHECK_EQ(make_editable(again).body_keys(), open(f.dir / "plate.step", false).body_keys());  // saves like a fresh read
  // A changed file is read again, not taken from the cache.
  std::filesystem::last_write_time(f.dir / "plate.step", std::filesystem::last_write_time(f.dir / "plate.step") + std::chrono::seconds(5));
  Document stale = Document::create();
  CHECK(!viewer_cache_load(stale, f.dir / "plate.step", viewer));
  std::error_code e;
  std::filesystem::remove_all(kCacheDir, e);
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
