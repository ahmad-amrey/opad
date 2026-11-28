// Point clouds (opad/point_cloud.hpp): the readers on generated files (PLY ASCII / binary little and big endian, with and
// without colour, normals and a scalar; XYZ, PTS, CSV; E57 written here and E57 with bit-packed scaled integers; LAS), the
// body they become (a nodes-only triangulation through BREP text and viewer mode), regions, the neighbour grid, slices and
// their profiles, outliers, voxel downsampling, normals and the writers; editing by hand (opad/cloud_edit.hpp: brushes,
// deleting, adding on the surface, the cloud_edit feature replayed).
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/commands.hpp"
#include "opad/cloud_edit.hpp"
#include "opad/design/cloud.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/point_cloud.hpp"
#include "opad/render.hpp"
#include "opad/step_io.hpp"

using namespace opad;
namespace pc = opad::cloud;

namespace {
const double kPi = 3.14159265358979323846;

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-cloud-" + new_uuid());
  Files() { std::filesystem::create_directories(dir); }
  ~Files() { std::error_code e; std::filesystem::remove_all(dir, e); }
};
void write_binary(const std::filesystem::path& p, const std::string& data) {
  std::ofstream f(p, std::ios::binary);
  f.write(data.data(), std::streamsize(data.size()));
}
template <class T>
void put(std::string& s, T v, bool big = false) {
  char b[sizeof(T)];
  std::memcpy(b, &v, sizeof(T));
  if (big) std::reverse(b, b + sizeof(T));
  s.append(b, sizeof(T));
}

// A generated part: points on a cylinder of radius 10 and height 20 about Z, with colours by height and outward normals.
pc::Points cylinder(size_t around = 90, size_t up = 21) {
  pc::Points p;
  for (size_t j = 0; j < up; ++j)
    for (size_t i = 0; i < around; ++i) {
      const double a = 2 * kPi * double(i) / double(around), z = 20.0 * double(j) / double(up - 1);
      p.xyz.insert(p.xyz.end(), {float(10 * std::cos(a)), float(10 * std::sin(a)), float(z)});
      p.rgb.insert(p.rgb.end(), {uint8_t(j * 10), uint8_t(100), uint8_t(255 - j * 10)});
      p.normals.insert(p.normals.end(), {float(std::cos(a)), float(std::sin(a)), 0.f});
      p.scalar.push_back(float(j));
    }
  p.scalar_name = "intensity";
  return p;
}

Document open(const std::filesystem::path& file, bool viewer, const ImportOptions& base = {}) {
  Document d = Document::create();
  ImportOptions o = base;
  o.viewer = viewer;
  import_file(d, file, o);
  return d;
}

// The one cloud of a document, in world coordinates (the body's placement applied).
pc::Points cloud_of(const Document& d) {
  const Scene s = resolve(d);
  CHECK_EQ(s.all_bodies().size(), 1u);
  const std::string id = s.all_bodies().front();
  CHECK_EQ(s.node(id)->representation, std::string("points"));
  const TopoDS_Shape shape = node_world_shape(d, s, id);
  CHECK(pc::is_cloud(shape));
  CHECK(is_mesh_shape(shape));
  return pc::from_shape(shape);
}

bool same_point(const pc::Points& a, size_t i, double x, double y, double z, double eps = 1e-3) {
  return std::abs(a.xyz[3 * i] - x) < eps && std::abs(a.xyz[3 * i + 1] - y) < eps && std::abs(a.xyz[3 * i + 2] - z) < eps;
}
}  // namespace

TEST(cloud_shape_round_trips_through_brep_text) {
  const pc::Points p = cylinder();
  const TopoDS_Shape s = pc::to_shape(p);
  CHECK(pc::is_cloud(s));
  CHECK_EQ(pc::point_count(s), p.size());
  const pc::Points back = pc::from_shape(shape_from_brep(pc::to_brep(s)));
  CHECK_EQ(back.size(), p.size());
  CHECK(back.colored());
  CHECK(back.has_normals());
  CHECK(back.has_scalar());
  for (size_t i : {size_t(0), size_t(500), p.size() - 1}) {
    CHECK(same_point(back, i, p.xyz[3 * i], p.xyz[3 * i + 1], p.xyz[3 * i + 2], 1e-5));
    CHECK_EQ(int(back.rgb[3 * i]), int(p.rgb[3 * i]));
    CHECK_EQ(int(back.rgb[3 * i + 2]), int(p.rgb[3 * i + 2]));
    CHECK_NEAR(back.normals[3 * i], p.normals[3 * i], 1e-5);
    CHECK_NEAR(back.scalar[i], p.scalar[i], 1e-5);
  }
  // A cloud without colour has none after the round trip; the scalar alone still rides the UV nodes.
  pc::Points plain = p;
  plain.rgb.clear();
  const pc::Points b2 = pc::from_shape(shape_from_brep(pc::to_brep(pc::to_shape(plain))));
  CHECK(!b2.colored());
  CHECK(b2.has_scalar());
  // A mesh is no cloud.
  CHECK(!pc::is_cloud(TopoDS_Shape()));
}

TEST(ply_vertices_only_is_a_cloud_ascii_and_binary) {
  Files f;
  // ASCII, positions only.
  write_text_file(f.dir / "a.ply", "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nend_header\n"
                                   "0 0 0\n1.5 2 3\n-4 5 6.25\n");
  CHECK(pc::ply_is_cloud(f.dir / "a.ply"));
  for (bool viewer : {true, false}) {
    const pc::Points a = cloud_of(open(f.dir / "a.ply", viewer));
    CHECK_EQ(a.size(), 3u);
    CHECK(!a.colored());
    CHECK(same_point(a, 2, -4, 5, 6.25));
  }
  // Binary little endian with colours, normals and an intensity; big endian the same.
  for (bool big : {false, true}) {
    std::string d = std::string("ply\nformat ") + (big ? "binary_big_endian" : "binary_little_endian") +
                    " 1.0\nelement vertex 2\nproperty double x\nproperty double y\nproperty double z\n"
                    "property uchar red\nproperty uchar green\nproperty uchar blue\nproperty float nx\nproperty float ny\nproperty float nz\n"
                    "property float scalar_Intensity\nend_header\n";
    const double pts[2][3] = {{1, 2, 3}, {4, 5, 6}};
    for (int i = 0; i < 2; ++i) {
      for (double v : pts[i]) put(d, v, big);
      d += char(10 * (i + 1)), d += char(20), d += char(30);
      for (float v : {0.f, 0.f, 2.f}) put(d, v, big);  // normalised on reading
      put(d, float(7 + i), big);
    }
    write_binary(f.dir / "b.ply", d);
    const pc::Points b = cloud_of(open(f.dir / "b.ply", true));
    CHECK_EQ(b.size(), 2u);
    CHECK(same_point(b, 1, 4, 5, 6));
    CHECK(b.colored());
    CHECK_EQ(int(b.rgb[3]), 20);
    CHECK(b.has_normals());
    CHECK_NEAR(b.normals[5], 1.0, 1e-6);
    CHECK(b.has_scalar());
    CHECK_NEAR(b.scalar[1], 8.0, 1e-6);
  }
  // A PLY with faces is still a mesh.
  write_text_file(f.dir / "m.ply", "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                                   "element face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n");
  CHECK(!pc::ply_is_cloud(f.dir / "m.ply"));
  const Document m = open(f.dir / "m.ply", true);
  const Scene s = resolve(m);
  CHECK_EQ(s.node(s.all_bodies().front())->representation, std::string("mesh"));
}

TEST(text_clouds_xyz_pts_csv) {
  Files f;
  write_text_file(f.dir / "p.xyz", "# a comment\n1 2 3\n4 5 6\n\n7 8 9\n");
  pc::Points a = cloud_of(open(f.dir / "p.xyz", true));
  CHECK_EQ(a.size(), 3u);
  CHECK(same_point(a, 2, 7, 8, 9));
  // Six columns: colours when they are bytes, normals when they are unit.
  write_text_file(f.dir / "c.xyz", "1 2 3 255 0 128\n4 5 6 10 20 30\n");
  a = cloud_of(open(f.dir / "c.xyz", true));
  CHECK(a.colored());
  CHECK(!a.has_normals());
  CHECK_EQ(int(a.rgb[2]), 128);
  write_text_file(f.dir / "n.xyz", "1 2 3 0 0 1\n4 5 6 0.6 0.8 0\n");
  a = cloud_of(open(f.dir / "n.xyz", true));
  CHECK(!a.colored());
  CHECK(a.has_normals());
  CHECK_NEAR(a.normals[4], 0.8, 1e-6);
  // PTS: a count line, then x y z intensity r g b; several scans one after the other.
  write_text_file(f.dir / "s.pts", "2\n1 1 1 -200 10 20 30\n2 2 2 100 40 50 60\n1\n3 3 3 0 70 80 90\n");
  a = cloud_of(open(f.dir / "s.pts", true));
  CHECK_EQ(a.size(), 3u);
  CHECK(a.colored());
  CHECK(a.has_scalar());
  CHECK_EQ(int(a.rgb[8]), 90);
  CHECK_NEAR(a.scalar[0], -200, 1e-6);
  // CSV with a header row in any order, semicolons, fractions as colours.
  write_text_file(f.dir / "h.csv", "Red;X;Y;Z;Green;Blue;Intensity\n1.0;1;2;3;0.5;0;9\n0;4;5;6;0.25;1;8\n");
  a = cloud_of(open(f.dir / "h.csv", true));
  CHECK_EQ(a.size(), 2u);
  CHECK(same_point(a, 0, 1, 2, 3));
  CHECK_EQ(int(a.rgb[0]), 255);
  CHECK_EQ(int(a.rgb[1]), 128);
  CHECK(a.has_scalar());
  CHECK_NEAR(a.scalar[1], 8, 1e-6);
  // ASC (RealityScan, CloudCompare): the same columns as XYZ, here x y z r g b nx ny nz and x y z nx ny nz r g b.
  write_text_file(f.dir / "r.asc", "0.5 -1.25 2 200 100 50 0 0 1\n1.5 -1.25 2 10 20 30 0 1 0\n");
  a = cloud_of(open(f.dir / "r.asc", true));
  CHECK_EQ(a.size(), 2u);
  CHECK(a.colored());
  CHECK(a.has_normals());
  CHECK_EQ(int(a.rgb[0]), 200);
  CHECK_NEAR(a.normals[4], 1, 1e-6);
  write_text_file(f.dir / "n.asc", "1 2 3 0 0 1 255 0 128\n4 5 6 0.6 0.8 0 1 2 3\n");
  a = cloud_of(open(f.dir / "n.asc", true));
  CHECK(a.colored());
  CHECK(a.has_normals());
  CHECK_EQ(int(a.rgb[2]), 128);
  CHECK_NEAR(a.normals[4], 0.8, 1e-6);
  // A bad line is reported.
  write_text_file(f.dir / "bad.xyz", "1 2 3\n4 5\n");
  CHECK_THROWS(open(f.dir / "bad.xyz", true));
  // Units and up axis: metres, Y up.
  ImportOptions o;
  o.cloud_scale = 1000;
  o.up_axis = "y";
  write_text_file(f.dir / "m.xyz", "1 2 3\n");
  a = cloud_of(open(f.dir / "m.xyz", true, o));
  CHECK(same_point(a, 0, 1000, -3000, 2000));
}

TEST(far_points_keep_their_precision_through_an_offset) {
  Files f;
  // Survey coordinates in mm: 500 km out, 0.1 mm apart.
  write_text_file(f.dir / "far.xyz", "500000000.0 4000000000.0 100.0\n500000000.1 4000000000.0 100.0\n500000000.0 4000000000.2 100.3\n");
  for (bool viewer : {true, false}) {
    const Document d = open(f.dir / "far.xyz", viewer);
    const pc::Points a = cloud_of(d);  // world coordinates through the body's placement
    CHECK_NEAR(a.xyz[0], 5e8, 64);  // floats in world coordinates: far, so coarse
    // The stored points themselves are near zero and exact to 1e-3 mm.
    const pc::Points local = pc::from_shape(body_shape(d, d.body_keys().front()));
    CHECK(std::abs(local.xyz[0]) < 1e4);
    CHECK_NEAR(double(local.xyz[3]) - local.xyz[0], 0.1, 1e-3);
    CHECK_NEAR(double(local.xyz[7]) - local.xyz[1], 0.2, 1e-3);
  }
}

TEST(e57_written_and_read_back) {
  Files f;
  pc::Points p = cylinder(60, 11);
  p.offset = {0, 0, 0};
  write_binary(f.dir / "c.e57", pc::write_e57_data(p));
  // Pages of 1024 bytes, each ending in its CRC-32C.
  CHECK_EQ(std::filesystem::file_size(f.dir / "c.e57") % 1024, 0u);
  const pc::Points a = cloud_of(open(f.dir / "c.e57", true));
  CHECK_EQ(a.size(), p.size());
  for (size_t i : {size_t(0), size_t(333), p.size() - 1}) {
    CHECK(same_point(a, i, p.xyz[3 * i], p.xyz[3 * i + 1], p.xyz[3 * i + 2], 1e-4));
    CHECK_EQ(int(a.rgb[3 * i + 2]), int(p.rgb[3 * i + 2]));
    CHECK_NEAR(a.normals[3 * i], p.normals[3 * i], 1e-5);
  }
  CHECK(a.has_scalar());
  // Big enough for several data packets.
  pc::Points big;
  for (int i = 0; i < 9000; ++i) big.xyz.insert(big.xyz.end(), {float(i), float(i % 7), float(-i)});
  write_binary(f.dir / "big.e57", pc::write_e57_data(big));
  const pc::Points b = cloud_of(open(f.dir / "big.e57", true));
  CHECK_EQ(b.size(), 9000u);
  CHECK(same_point(b, 8999, 8999, 8999 % 7, -8999, 1e-3));
}

namespace {
// An E57 as a scanner's software writes it: coordinates as scaled integers bit-packed at 20 bits, colours at 8, intensity at
// 11 bits and the invalid-state at 2, split over two data packets (values straddle them), a pose that turns and moves the
// scan, and a point marked invalid. Pages carry no CRC (it is not checked on reading).
std::string scanner_e57(int n, std::vector<std::array<double, 3>>& expected) {
  struct Bits {
    std::string bytes;
    uint64_t acc = 0;
    int used = 0;
    void add(uint64_t v, int bits) {
      for (int b = 0; b < bits; ++b) {
        acc |= ((v >> b) & 1) << used;
        if (++used == 8) bytes += char(acc), acc = 0, used = 0;
      }
    }
    void flush() {
      if (used) bytes += char(acc), acc = 0, used = 0;
    }
  };
  Bits s[8];
  const int64_t lo = -500000, hi = 500000;  // scale 1e-5: +-5 m in steps of 10 um
  expected.clear();
  for (int i = 0; i < n; ++i) {
    const double x = 0.001 * i, y = 0.5 - 0.002 * i, z = 0.25;
    const int invalid = i == 3 ? 2 : 0;
    for (int k = 0; k < 3; ++k) s[k].add(uint64_t(std::llround((k == 0 ? x : k == 1 ? y : z) / 1e-5) - lo), 20);
    for (int k = 0; k < 3; ++k) s[3 + k].add(uint64_t((i * 13 + k * 50) % 256), 8);
    s[6].add(uint64_t(i % 2048), 11);
    s[7].add(uint64_t(invalid), 2);
    // Pose: a quarter turn about Z, then 1 m along X. mm.
    if (!invalid) expected.push_back({(-y + 1) * 1000, x * 1000, z * 1000});
  }
  for (auto& b : s) b.flush();
  std::string L(48, '\0');
  const size_t section = L.size();
  L.append(32, '\0');
  const size_t data = L.size();
  // Two packets: the first takes about half of each stream's bytes.
  for (int part = 0; part < 2; ++part) {
    std::string packet;
    packet += char(1), packet += char(0);
    put<uint16_t>(packet, 0);
    put<uint16_t>(packet, 8);
    std::vector<std::string> pieces;
    for (auto& b : s) {
      const size_t half = b.bytes.size() / 2 + 1;
      pieces.push_back(part == 0 ? b.bytes.substr(0, half) : b.bytes.substr(half));
    }
    for (auto& pc2 : pieces) put<uint16_t>(packet, uint16_t(pc2.size()));
    for (auto& pc2 : pieces) packet += pc2;
    while (packet.size() % 4) packet += '\0';
    const uint16_t len = uint16_t(packet.size() - 1);
    std::memcpy(packet.data() + 2, &len, 2);
    L += packet;
  }
  auto phys = [](uint64_t l) { return l / 1020 * 1024 + l % 1020; };
  {
    std::string h;
    h += char(1);
    h.append(7, '\0');
    put<uint64_t>(h, L.size() - section);
    put<uint64_t>(h, phys(data));
    put<uint64_t>(h, 0);
    std::memcpy(L.data() + section, h.data(), 32);
  }
  const size_t xmlAt = L.size();
  std::string x = "<?xml version=\"1.0\"?><e57Root type=\"Structure\" xmlns=\"http://www.astm.org/COMMIT/E57/2010-e57-v1.0\">"
                  "<formatName type=\"String\"><![CDATA[ASTM E57 3D Imaging Data File]]></formatName>"
                  "<data3D type=\"Vector\"><vectorChild type=\"Structure\"><name type=\"String\">scan &amp; one</name>"
                  "<pose type=\"Structure\"><rotation type=\"Structure\"><w type=\"Float\">0.70710678118654757</w><x type=\"Float\">0</x>"
                  "<y type=\"Float\">0</y><z type=\"Float\">0.70710678118654757</z></rotation><translation type=\"Structure\">"
                  "<x type=\"Float\">1</x><y type=\"Float\">0</y><z type=\"Float\">0</z></translation></pose>"
                  "<intensityLimits type=\"Structure\"><intensityMinimum type=\"Integer\">0</intensityMinimum>"
                  "<intensityMaximum type=\"Integer\">2047</intensityMaximum></intensityLimits>"
                  "<points type=\"CompressedVector\" fileOffset=\"" + std::to_string(phys(section)) + "\" recordCount=\"" + std::to_string(n) +
                  "\"><prototype type=\"Structure\">";
  for (const char* c : {"cartesianX", "cartesianY", "cartesianZ"})
    x += std::string("<") + c + " type=\"ScaledInteger\" minimum=\"-500000\" maximum=\"500000\" scale=\"1.0000000000000001e-05\" offset=\"0\"/>";
  for (const char* c : {"colorRed", "colorGreen", "colorBlue"}) x += std::string("<") + c + " type=\"Integer\" minimum=\"0\" maximum=\"255\"/>";
  x += "<intensity type=\"Integer\" minimum=\"0\" maximum=\"2047\"/><cartesianInvalidState type=\"Integer\" minimum=\"0\" maximum=\"2\"/>"
       "</prototype><codecs type=\"Vector\"/></points></vectorChild></data3D></e57Root>";
  L += x;
  const uint64_t pages = (L.size() + 1019) / 1020;
  std::string h = "ASTM-E57";
  put<uint32_t>(h, 1), put<uint32_t>(h, 0), put<uint64_t>(h, pages * 1024), put<uint64_t>(h, phys(xmlAt)), put<uint64_t>(h, x.size()), put<uint64_t>(h, 1024);
  std::memcpy(L.data(), h.data(), 48);
  L.resize(pages * 1020, '\0');
  std::string out(pages * 1024, '\0');
  for (uint64_t k = 0; k < pages; ++k) std::memcpy(out.data() + k * 1024, L.data() + k * 1020, 1020);
  (void)hi;
  return out;
}
}  // namespace

TEST(e57_bit_packed_scaled_integers_pose_and_invalid_points) {
  Files f;
  std::vector<std::array<double, 3>> expected;
  write_binary(f.dir / "scan.e57", scanner_e57(1500, expected));
  const pc::Points a = cloud_of(open(f.dir / "scan.e57", true));
  CHECK_EQ(a.size(), expected.size());  // the invalid point left out
  CHECK_EQ(a.size(), 1499u);
  for (size_t i : {size_t(0), size_t(5), size_t(700), a.size() - 1}) CHECK(same_point(a, i, expected[i][0], expected[i][1], expected[i][2], 0.02));
  CHECK(a.colored());
  // Point 4 (the fifth record, the fourth was invalid) has red (4 * 13) % 256.
  CHECK_EQ(int(a.rgb[3 * 3]), (4 * 13) % 256);
  CHECK(a.has_scalar());
  CHECK_NEAR(a.scalar[3], 4.0 / 2047.0, 1e-6);
}

TEST(las_point_format_2) {
  Files f;
  std::string d(227, '\0');
  std::memcpy(d.data(), "LASF", 4);
  d[24] = 1, d[25] = 2;
  auto set16 = [&](size_t at, uint16_t v) { std::memcpy(d.data() + at, &v, 2); };
  auto set32 = [&](size_t at, uint32_t v) { std::memcpy(d.data() + at, &v, 4); };
  auto setd = [&](size_t at, double v) { std::memcpy(d.data() + at, &v, 8); };
  set16(94, 227);
  set32(96, 227);
  d[104] = 2;
  set16(105, 26);
  set32(107, 2);
  setd(131, 0.001), setd(139, 0.001), setd(147, 0.001);
  setd(155, 0), setd(163, 0), setd(171, 0);
  for (int i = 0; i < 2; ++i) {
    std::string r(26, '\0');
    const int32_t xyz[3] = {1000 * (i + 1), 2000, 3000};  // metres x 1000
    std::memcpy(r.data(), xyz, 12);
    const uint16_t intensity = uint16_t(100 + i);
    std::memcpy(r.data() + 12, &intensity, 2);
    const uint16_t rgb[3] = {65535, 32768, 0};
    std::memcpy(r.data() + 20, rgb, 6);
    d += r;
  }
  write_binary(f.dir / "a.las", d);
  const pc::Points a = cloud_of(open(f.dir / "a.las", true));
  CHECK_EQ(a.size(), 2u);
  CHECK(same_point(a, 1, 2000, 2000, 3000));  // metres to mm
  CHECK_EQ(int(a.rgb[0]), 255);
  CHECK_EQ(int(a.rgb[1]), 128);
  CHECK_NEAR(a.scalar[1], 101, 1e-6);
  d[104] = char(2 | 0x80);
  write_binary(f.dir / "z.las", d);
  CHECK_THROWS(open(f.dir / "z.las", true));
  write_binary(f.dir / "z.laz", d);
  CHECK_THROWS(open(f.dir / "z.laz", true));
}

TEST(writers_round_trip) {
  Files f;
  const pc::Points p = cylinder(40, 5);
  for (const char* ext : {".ply", ".xyz", ".pts", ".csv", ".e57"}) {
    const auto file = f.dir / (std::string("out") + ext);
    pc::write_file(file, p);
    const pc::Points a = pc::read_file(file);
    CHECK_EQ(a.size(), p.size());
    CHECK(same_point(a, 77, p.xyz[231], p.xyz[232], p.xyz[233], 1e-3));
    CHECK(a.colored());
    CHECK_EQ(int(a.rgb[3 * 77 + 2]), int(p.rgb[3 * 77 + 2]));
    if (std::string(ext) != ".pts") CHECK(a.has_normals());
    if (std::string(ext) != ".xyz") CHECK(a.has_scalar());
  }
  // ASCII PLY too.
  write_text_file(f.dir / "t.ply", pc::write_ply_data(p, false));
  CHECK_EQ(pc::read_file(f.dir / "t.ply").size(), p.size());
  CHECK_THROWS(pc::write_file(f.dir / "x.stl", p));
}

TEST(viewer_cloud_becomes_editable) {
  Files f;
  write_binary(f.dir / "c.ply", pc::write_ply_data(cylinder(), true));
  Document viewer = open(f.dir / "c.ply", true);
  CHECK(viewer.has_live_bodies());
  Document editable = make_editable(viewer);
  CHECK(!editable.has_live_bodies());
  editable.save_as(f.dir / "c.opad");
  const Document loaded = Document::load(f.dir / "c.opad");
  const pc::Points a = cloud_of(loaded);
  CHECK_EQ(a.size(), cylinder().size());
  CHECK(a.colored() && a.has_normals() && a.has_scalar());
  CHECK_EQ(loaded.bodies().front().meta.value("representation", ""), std::string("points"));
}

TEST(regions) {
  pc::Region r = pc::Region::of({5, 3, 4, 10, 4, 0});
  CHECK_EQ(r.ranges.size(), 3u);  // [0,0] [3,5] [10,10]
  CHECK_EQ(r.count(), 5u);
  CHECK(r.contains(4));
  CHECK(!r.contains(6));
  const pc::Region s = pc::Region::from_json(json::parse("[[4, 12], 20]"));
  CHECK_EQ(r.unite(s).count(), 1u + 10u + 1u);  // 0, 3..12, 20
  CHECK_EQ(r.subtract(s).indices(), (std::vector<uint32_t>{0, 3}));
  CHECK_EQ(pc::Region::from_json(r.to_json()).indices(), r.indices());
  CHECK_THROWS(pc::Region::from_json(json::parse("[[5, 2]]")));
  const json ref = pc::region_ref("node", r);
  CHECK_EQ(ref["kind"].get<std::string>(), std::string("region"));
  CHECK_EQ(pc::region_of_ref(ref, 11).count(), 5u);
  CHECK_THROWS(pc::region_of_ref(ref, 8));
  // Selections on the cylinder.
  const pc::Points p = cylinder();
  const auto top = pc::in_box(p, {-20, -20, 19.5}, {20, 20, 21});
  CHECK_EQ(top.count(), 90u);
  CHECK_EQ(pc::in_sphere(p, {10, 0, 0}, 0.01).count(), 1u);
  CHECK_EQ(pc::in_band(p, {0, 0, 10}, {0, 0, 2}, 0.6).count(), 90u);
  // A lasso through a view looking down Z (x, y as they are, scaled to NDC): the half with x > 0.
  const std::array<double, 16> view = {0.05, 0, 0, 0, 0, 0.05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  const auto half = pc::in_polygon(p, view, {{0.001, -1}, {1, -1}, {1, 1}, {0.001, 1}});
  CHECK(half.count() > 0.45 * double(p.size()) && half.count() < 0.55 * double(p.size()));
  const pc::Points kept = pc::subset(p, top);
  CHECK_EQ(kept.size(), 90u);
  CHECK(kept.colored());
  CHECK_EQ(pc::without(p, top).size(), p.size() - 90);
  const json j = pc::points_json(p, &top, 10, 5);
  CHECK_EQ(j["returned"].get<int>(), 5);
  CHECK_EQ(j["next_offset"].get<int>(), 15);
  CHECK_EQ(j["indices"][0].get<int>(), int(top.ranges.front()[0]) + 10);
}

TEST(grid_nearest_matches_brute_force) {
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(-50, 50);
  pc::Points p;
  for (int i = 0; i < 5000; ++i) p.xyz.insert(p.xyz.end(), {u(rng), u(rng), u(rng) * 0.1f});
  const pc::Grid g(p);
  std::vector<uint32_t> nb;
  std::vector<float> d2;
  for (int q = 0; q < 50; ++q) {
    const size_t i = size_t(q) * 97;
    g.nearest(&p.xyz[3 * i], 8, nb, &d2, int64_t(i));
    std::vector<std::pair<double, uint32_t>> all;
    for (size_t j = 0; j < p.size(); ++j) {
      if (j == i) continue;
      double s = 0;
      for (int k = 0; k < 3; ++k) s += (double(p.xyz[3 * j + size_t(k)]) - p.xyz[3 * i + size_t(k)]) * (double(p.xyz[3 * j + size_t(k)]) - p.xyz[3 * i + size_t(k)]);
      all.push_back({s, uint32_t(j)});
    }
    std::sort(all.begin(), all.end());
    CHECK_EQ(nb.size(), 8u);
    for (size_t k = 0; k < 8; ++k) CHECK_NEAR(d2[k], all[k].first, 1e-2);
    std::vector<uint32_t> in;
    g.within(&p.xyz[3 * i], std::sqrt(all[7].first) + 1e-4, in);
    CHECK(in.size() >= 9u);  // the 8 and the point itself
  }
  CHECK(pc::spacing(p) > 0);
}

TEST(slice_of_a_cylinder_is_a_closed_circle) {
  const pc::Points p = cylinder(360, 41);  // rings every 0.5 mm in height
  const pc::Slice s = pc::slice(p, {0, 0, 10}, {0, 0, 1}, 0.25);
  CHECK_EQ(s.indices.size(), 360u);
  const auto lines = pc::profile(s.uv, 0, 0.05);
  CHECK(!lines.empty());
  const auto& loop = lines.front();
  CHECK(loop.size() > 8);
  CHECK_NEAR(loop.front()[0], loop.back()[0], 1e-9);  // closed
  double worst = 0;
  for (const auto& q : loop) worst = std::max(worst, std::abs(std::hypot(q[0], q[1]) - 10));
  CHECK(worst < 0.3);
  // An open profile: a slice across the cylinder's side gives two straight walls.
  const pc::Slice side = pc::slice(p, {0, 0, 0}, {1, 0, 0}, 0.2);
  const auto walls = pc::profile(side.uv, 2.0, 0.05);
  CHECK(walls.size() >= 2u);
  CHECK(walls[0].size() >= 2u && walls[0].size() <= 4u);  // a straight line thins to its ends
  CHECK_THROWS(pc::slice(p, {0, 0, 0}, {0, 0, 0}, 1));
}

TEST(outliers_downsampling_and_normals) {
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(0, 100);
  pc::Points p;
  // A dense plane with a few points far off it.
  for (int i = 0; i < 100; ++i)
    for (int j = 0; j < 100; ++j) p.xyz.insert(p.xyz.end(), {float(i), float(j), 0.f});
  for (int i = 0; i < 20; ++i) p.xyz.insert(p.xyz.end(), {u(rng), u(rng), 30 + u(rng)});
  const pc::Region out = pc::outliers(p, 8, 2.0);
  CHECK(out.count() >= 18u && out.count() <= 30u);
  size_t far = 0;
  for (uint32_t i : out.indices()) far += i >= 10000;
  CHECK(far >= 18u);
  const pc::Points clean = pc::without(p, out);
  CHECK(clean.size() < p.size());
  // Downsampling to 4 mm voxels: 25 x 25 voxels on the plane.
  const pc::Points coarse = pc::voxel_downsample(clean, 4.0);
  CHECK(coarse.size() >= 625u && coarse.size() <= 640u);
  CHECK_THROWS(pc::voxel_downsample(clean, 0));
  // Normals on a sphere: unit, square to the surface and facing out.
  pc::Points s;
  for (int i = 0; i < 4000; ++i) {
    const double z = 1 - 2 * (i + 0.5) / 4000.0, r = std::sqrt(1 - z * z), a = i * 2.399963229728653;
    s.xyz.insert(s.xyz.end(), {float(20 * r * std::cos(a)), float(20 * r * std::sin(a)), float(20 * z)});
  }
  pc::estimate_normals(s, 12);
  CHECK(s.has_normals());
  int outward = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const double d = (s.normals[3 * i] * s.xyz[3 * i] + s.normals[3 * i + 1] * s.xyz[3 * i + 1] + s.normals[3 * i + 2] * s.xyz[3 * i + 2]) / 20.0;
    if (d > 0.95) ++outward;
  }
  CHECK(outward > int(0.98 * double(s.size())));
  const json d = pc::describe(s, true);
  CHECK_EQ(d["points"].get<int>(), 4000);
  CHECK(d["spacing"].get<double>() > 0.1);
}


TEST(cloud_features_and_slice_to_sketch) {
  Files f;
  // A cylinder scan with noise points, placed far out (its placement carries the offset).
  pc::Points p = cylinder(180, 41);
  for (int i = 0; i < 12; ++i) p.xyz.insert(p.xyz.end(), {40.f + float(i), 40.f, 40.f}), p.rgb.insert(p.rgb.end(), {0, 0, 0}),
                                 p.normals.insert(p.normals.end(), {0, 0, 1}), p.scalar.push_back(0);
  for (auto& v : p.xyz) v += 0;  // as made
  pc::write_file(f.dir / "scan.ply", p);
  Document d = open(f.dir / "scan.ply", false);
  const std::string body = resolve(d).all_bodies().front();
  const json bodies = json::array({{{"body", body}}});
  auto points_now = [&] { return pc::point_count(body_shape(d, resolve(d).node(body)->body_key)); };
  const size_t all = points_now();
  CHECK_EQ(all, p.size());
  design::apply_ops(d, {design::make_feature_op("cloud_outliers", "Clean", {{"bodies", bodies}, {"neighbours", "8"}, {"ratio", "1.5"}})});
  CHECK(points_now() <= all - 10);
  CHECK(points_now() >= all - 400);
  CHECK_EQ(resolve(d).node(body)->representation, std::string("points"));
  design::apply_ops(d, {design::make_feature_op("cloud_downsample", "Thin", {{"bodies", bodies}, {"voxel", "1 mm"}})});
  const size_t thin = points_now();
  CHECK(thin < all && thin > 1000);
  design::apply_ops(d, {design::make_feature_op("cloud_normals", "Normals", {{"bodies", bodies}, {"neighbours", "10"}})});
  const pc::Points withNormals = pc::from_shape(body_shape(d, resolve(d).node(body)->body_key));
  CHECK(withNormals.has_normals());
  CHECK(withNormals.colored());  // colours kept through it all
  // Crop to the top half by a region reference.
  const pc::Region top = pc::in_box(withNormals, {-50, -50, 10}, {50, 50, 50});
  design::apply_ops(d, {design::make_feature_op("cloud_crop", "Top", {{"region", pc::region_ref(body, top)}, {"keep", "inside"}})});
  CHECK_EQ(points_now(), top.count());
  // A modelling feature refuses a cloud.
  CHECK_THROWS(design::apply_ops(d, {design::make_feature_op("shell", "S", {{"bodies", bodies}})}));
  // Two slices across the cylinder's axis: new clouds lying on their planes, with their frames.
  json r = design::apply_ops(d, {design::make_feature_op("cloud_slice", "Cut", {{"bodies", bodies}, {"plane", {{"base", "xy"}}}, {"offset", "12 mm"},
                                                                                  {"band", "0.3 mm"}, {"count", "2"}, {"spacing", "4 mm"}})});
  const Scene s = resolve(d);
  std::vector<std::string> slices;
  for (const auto& id : s.all_bodies())
    if (id != body) slices.push_back(id);
  CHECK_EQ(slices.size(), 2u);
  const pc::Points cut = pc::from_shape(node_world_shape(d, s, slices[0]));
  CHECK(cut.size() > 50);
  for (size_t i = 0; i < cut.size(); ++i) CHECK_NEAR(cut.xyz[3 * i + 2], 12.0, 1e-3);  // on the plane
  CHECK_EQ(s.node(slices[0])->representation, std::string("points"));
  // The slice as sketch geometry: a closed loop of lines about radius 10, on the slice's plane.
  json report;
  const json sketch = design::slice_sketch_op(d, s, slices[0], json(), 0, 0.05, 0, "", &report);
  CHECK(report["lines"].get<int>() >= 8);
  design::apply_ops(d, {sketch});
  const Scene after = resolve(d);
  CHECK_EQ(after.sketches.size(), 1u);
  CHECK_NEAR(after.sketches.front().frame.origin[2], 12.0, 1e-6);
  // Without a slice, a plane must be given, and points must lie near it (the crop left none at z = 0).
  CHECK_THROWS(design::slice_sketch_op(d, after, body, json(), 0.3, 0, 0, ""));
  CHECK_THROWS(design::slice_sketch_op(d, after, body, {{"base", "xy"}}, 0.3, 0, 0, ""));
  const json other = design::slice_sketch_op(d, after, body, {{"base", "yz"}}, 0.3, 0, 0, "", &report);
  CHECK_EQ(other["op"].get<std::string>(), std::string("sketch"));
  CHECK(report["polylines"].get<int>() >= 1);
  // Saved and opened again: the slices are clouds still.
  d.save_as(f.dir / "scan.opad");
  const Document again = Document::load(f.dir / "scan.opad");
  CHECK_EQ(pc::point_count(body_shape(again, resolve(again).node(slices[1])->body_key)), pc::from_shape(node_world_shape(d, s, slices[1])).size());
}

TEST(cloud_commands) {
  Files f;
  pc::write_file(f.dir / "scan.ply", cylinder(180, 41));
  Document d = Document::create();
  commands::run("import", {{"file", path_to_utf8(f.dir / "scan.ply")}}, &d);
  const std::string body = resolve(d).all_bodies().front();
  json info = commands::run("cloud", {{"body", body}, {"action", "info"}}, &d);
  CHECK_EQ(info["points"].get<int>(), 180 * 41);
  CHECK_NEAR(info["bounds"]["max"][2].get<double>(), 20.0, 1e-4);
  CHECK(info["spacing"].get<double>() > 0.1);
  json page = commands::run("cloud", {{"body", body}, {"action", "points"}, {"limit", 10}, {"offset", 5}}, &d);
  CHECK_EQ(page["points"].size(), 10u);
  CHECK_EQ(page["next_offset"].get<int>(), 15);
  json region = commands::run("cloud", {{"body", body}, {"action", "region"}, {"box", {{-20, -20, 19.75}, {20, 20, 21}}}}, &d);
  CHECK_EQ(region["count"].get<int>(), 180);
  CHECK_NEAR(region["centre"][2].get<double>(), 20.0, 1e-4);
  json sliced = commands::run("cloud", {{"body", body}, {"action", "slice"}, {"plane", {{"origin", {0, 0, 10}}, {"normal", {0, 0, 1}}}}, {"band", 0.2}, {"tolerance", 0.05}}, &d);
  CHECK_EQ(sliced["count"].get<int>(), 180);
  CHECK(!sliced["polylines"].empty());
  CHECK(sliced["polylines"][0]["closed"].get<bool>());
  // The region reference goes straight into Crop points, through the feature command.
  commands::run("feature", {{"kind", "cloud_crop"}, {"inputs", {{"region", region["reference"]}, {"keep", "outside"}}}}, &d);
  info = commands::run("cloud", {{"body", body}}, &d);
  CHECK_EQ(info["points"].get<int>(), 180 * 40);
  commands::run("feature", {{"kind", "cloud_slice"}, {"inputs", {{"bodies", json::array({{{"body", body}}})}, {"offset", "5 mm"}}}}, &d);
  std::string cut;
  for (const auto& id : resolve(d).all_bodies())
    if (id != body) cut = id;
  CHECK(!cut.empty());
  json sk = commands::run("cloud_sketch", {{"body", cut}, {"tolerance", 0.05}}, &d);
  CHECK(sk.contains("sketch_id"));
  CHECK(sk["profile"]["lines"].get<int>() >= 8);
  // Export of the clouds: one file in world mm.
  json ex = commands::run("export", {{"format", "e57"}, {"out", path_to_utf8(f.dir / "out.e57")}, {"select", json::array({body})}}, &d);
  CHECK_EQ(ex["points"].get<int>(), 180 * 40);
  CHECK_EQ(pc::read_file(f.dir / "out.e57").size(), size_t(180 * 40));
  CHECK_THROWS(commands::run("cloud", {{"body", body}, {"action", "nope"}}, &d));
  CHECK_THROWS(commands::run("cloud", {{"body", body}, {"action", "region"}}, &d));
}
TEST(clouds_render_as_points_and_by_height) {
  // A 60 x 40 sheet with a step: z = 0 for x < 36, z = 5 beyond, points 0.5 mm apart, normals up. Headless renders used to
  // draw a cloud as nothing (it has no triangles); now as splats, and by height against a plane.
  Files f;
  pc::Points p;
  for (double x = 0; x <= 60; x += 0.5)
    for (double y = 0; y <= 40; y += 0.5) {
      p.xyz.insert(p.xyz.end(), {float(x), float(y), float(x < 36 ? 0 : 5)});
      p.normals.insert(p.normals.end(), {0.f, 0.f, 1.f});
    }
  pc::write_file(f.dir / "sheet.ply", p);
  Document d = Document::create();
  commands::run("import", {{"file", path_to_utf8(f.dir / "sheet.ply")}}, &d);
  const Scene s = resolve(d);
  const std::string body = s.all_bodies().front();
  CHECK(pc::from_shape(node_world_shape(d, s, body), true, 1000).size() <= 1000u);  // a sample of a big cloud
  RenderOptions o;
  o.width = 320, o.height = 200, o.supersample = 1;
  o.camera = Camera::preset("top");
  json receipt;
  const Image plain = render_scene(d, s, o, &receipt);
  size_t drawn = 0;
  for (size_t i = 0; i < plain.rgb.size(); i += 3) drawn += plain.rgb[i] < 250 || plain.rgb[i + 1] < 250 || plain.rgb[i + 2] < 250;
  std::printf("    cloud render: %zu of %d pixels drawn\n", drawn, o.width * o.height);
  CHECK(drawn > size_t(0.5 * o.width * o.height));
  CHECK_EQ(receipt["visible_ids"].size(), 1u);
  // By height: the cloud's own base (the larger flat, z = 0) is blue, the step red; the range and plane reported.
  o.cloud_color = "height";
  json heights;
  const Image h = render_scene(d, s, o, &heights);
  CHECK(heights.contains("cloud_height"));
  const json& ch = heights["cloud_height"];
  std::printf("    by height: %.3f .. %.3f mm, %s\n", ch["range_mm"][0].get<double>(), ch["range_mm"][1].get<double>(),
              ch.contains("planes") ? ch["planes"].dump().c_str() : "no planes");
  CHECK_NEAR(ch["range_mm"][0].get<double>(), 0, 0.01);
  CHECK_NEAR(ch["range_mm"][1].get<double>(), 5, 0.01);
  CHECK_EQ(ch["planes"].size(), 1u);
  const uint8_t* low = h.px(96, 100);
  const uint8_t* high = h.px(272, 100);
  CHECK(low[2] > low[0] + 80);
  CHECK(high[0] > high[2] + 80);
  // Against a plane given (XZ: the heights run along Y, 0 to 40, the range cut at the 2nd and 98th percentiles).
  o.height_plane = {{"base", "xz"}};
  json given;
  render_scene(d, s, o, &given);
  CHECK(given["cloud_height"].contains("plane"));
  const double y0 = given["cloud_height"]["range_mm"][0].get<double>(), y1 = given["cloud_height"]["range_mm"][1].get<double>();
  std::printf("    against XZ: %.3f .. %.3f mm\n", y0, y1);
  CHECK(std::fabs(std::fabs(y1 - y0) - 38.4) < 1.0);
  // Through the render command.
  const json r = commands::run("render", {{"out", path_to_utf8(f.dir / "h.png")}, {"view", "top"}, {"cloud_color", "height"}}, &d);
  CHECK(!r.contains("error"));
  CHECK(std::filesystem::exists(f.dir / "h.png"));
  CHECK_THROWS(commands::run("render", {{"out", path_to_utf8(f.dir / "x.png")}, {"cloud_color", "rainbow"}}, &d));
}

namespace {
// A flat patch of points 1 mm apart on z = 0 (x, y in 0..40), normals +Z, colours by x.
std::shared_ptr<const pc::Points> patch(bool normals = true, bool colours = true) {
  auto p = std::make_shared<pc::Points>();
  for (int j = 0; j <= 40; ++j)
    for (int i = 0; i <= 40; ++i) {
      p->xyz.insert(p->xyz.end(), {float(i), float(j), 0.f});
      if (normals) p->normals.insert(p->normals.end(), {0.f, 0.f, 1.f});
      if (colours) p->rgb.insert(p->rgb.end(), {uint8_t(i * 6), uint8_t(50), uint8_t(200)});
    }
  return p;
}
uint32_t at_xy(int i, int j) { return uint32_t(j * 41 + i); }
}  // namespace

TEST(cloud_edit_push_and_pull_follow_the_falloff) {
  auto base = patch();
  pc::Editor e(base);
  pc::Brush b;
  b.centre = {20, 20, 0};
  b.toward = {0, 0, 1};
  b.radius = 5;
  const auto moved = e.push(b, 2.0);  // pull: out of the surface, toward the eye
  CHECK_NEAR(e.at(at_xy(20, 20))[2], 2.0, 1e-6);
  CHECK_NEAR(e.at(at_xy(22, 20))[2], 2.0 * pc::falloff(0.4), 1e-6);  // (1 - 0.16)^2 of it
  CHECK_NEAR(e.at(at_xy(20, 17))[2], 2.0 * pc::falloff(0.6), 1e-6);
  CHECK_NEAR(e.at(at_xy(22, 20))[0], 22.0, 1e-6);  // along the normal only
  CHECK_EQ(e.at(at_xy(25, 20))[2], 0.0);  // on the rim: weight 0
  CHECK_EQ(e.at(at_xy(30, 30))[2], 0.0);
  size_t inside = 0;
  for (int j = 0; j <= 40; ++j)
    for (int i = 0; i <= 40; ++i) inside += (i - 20) * (i - 20) + (j - 20) * (j - 20) < 25;
  CHECK_EQ(moved.size(), inside);
  CHECK_EQ(e.edit().moved.size(), inside);
  // Push takes it back in, by the same falloff about the same centre.
  e.push(b, -2.0);
  CHECK_NEAR(e.at(at_xy(20, 20))[2], 0.0, 1e-6);
  CHECK_NEAR(e.at(at_xy(22, 20))[2], 0.0, 1e-6);
  // Normals facing away from the eye are turned to it: a pull still comes toward the eye.
  auto flipped = std::make_shared<pc::Points>(*base);
  for (size_t i = 0; i < flipped->size(); ++i) flipped->normals[3 * i + 2] = -1;
  pc::Editor f(flipped);
  f.push(b, 1.5);
  CHECK_NEAR(f.at(at_xy(20, 20))[2], 1.5, 1e-6);
  // Without normals, along the view.
  pc::Editor g(patch(false));
  pc::Brush slanted = b;
  const double s = std::sqrt(0.5);
  slanted.toward = {s, 0, s};
  g.push(slanted, 1.0);
  CHECK_NEAR(g.at(at_xy(20, 20))[0], 20 + s, 1e-6);
  CHECK_NEAR(g.at(at_xy(20, 20))[2], s, 1e-6);
  // The far side of a part is out of reach: a point 10 mm behind the surface stays.
  auto two = std::make_shared<pc::Points>(*base);
  two->xyz.insert(two->xyz.end(), {20.f, 20.f, -10.f});
  two->normals.insert(two->normals.end(), {0.f, 0.f, -1.f});
  two->rgb.insert(two->rgb.end(), {0, 0, 0});
  pc::Editor h(two);
  h.push(b, 2.0);
  CHECK_EQ(h.at(uint32_t(two->size() - 1))[2], -10.0);
  // Smooth: a spike pulled out sinks back toward the plane of its neighbours.
  pc::Brush narrow = b;
  narrow.radius = 1.2;
  e.push(narrow, 3.0);
  const double before = e.at(at_xy(20, 20))[2];
  CHECK(before > 2.9);
  b.radius = 3;
  e.smooth(b, 1.0, 8);
  CHECK(e.at(at_xy(20, 20))[2] < 0.5 * before);
}

TEST(cloud_edit_deletes_by_brush_region_and_keep) {
  auto base = patch();
  pc::Editor e(base);
  pc::Brush b;
  b.centre = {10, 10, 0};
  b.radius = 3;
  const auto gone = e.erase(b);
  CHECK(!gone.empty());
  CHECK(e.deleted(at_xy(10, 10)) && !e.deleted(at_xy(14, 10)));
  // A deleted point is under no brush any more and moves no more.
  std::vector<uint32_t> ids;
  std::vector<float> w;
  e.under(b, ids, w);
  CHECK(ids.empty());
  CHECK(e.push(b, 1).empty());
  CHECK_EQ(e.erase(pc::Region::of({at_xy(10, 10), at_xy(30, 30)})), size_t(1));  // one was gone already
  pc::Points out = pc::apply(*base, e.edit());
  CHECK_EQ(out.size(), base->size() - gone.size() - 1);
  CHECK(out.colored() && out.has_normals());
  // A lasso (a square in a top view: NDC = (p - 20) / 20) finds only live points, where they are now.
  std::array<double, 16> top{};
  top[0] = 1.0 / 20, top[3] = -1, top[5] = 1.0 / 20, top[7] = -1, top[15] = 1;
  CHECK(e.inPolygon(top, {{-0.6, -0.6}, {-0.4, -0.6}, {-0.4, -0.4}, {-0.6, -0.4}}).empty());  // x, y in 8..12: all deleted
  const pc::Region r = e.inPolygon(top, {{-0.1, -0.1}, {0.1, -0.1}, {0.1, 0.1}, {-0.1, 0.1}});  // x, y in 18..22
  CHECK(!r.empty());
  for (uint32_t i : r.indices()) CHECK(!e.deleted(i));
  CHECK(r.count() < 25u);
  // Keep: every other point goes.
  const size_t live = r.count();
  e.keep(r);
  CHECK_EQ(pc::apply(*base, e.edit()).size(), live);
}

TEST(cloud_edit_adds_points_on_the_local_surface) {
  // A tilted plane z = x / 2 with a hole about (10, 10), colours by x.
  auto p = std::make_shared<pc::Points>();
  const double l = std::sqrt(1.25);
  for (int j = 0; j <= 20; ++j)
    for (int i = 0; i <= 20; ++i) {
      if ((i - 10) * (i - 10) + (j - 10) * (j - 10) < 5) continue;
      p->xyz.insert(p->xyz.end(), {float(i), float(j), float(i * 0.5)});
      p->normals.insert(p->normals.end(), {float(-0.5 / l), 0.f, float(1 / l)});
      p->rgb.insert(p->rgb.end(), {uint8_t(i * 10), 0, 0});
    }
  pc::Editor e(p);
  // The pointer's ray straight down through (10, 10), seeded above the hole's middle.
  const auto added = e.add({10, 10, 9}, {0, 0, 1});
  CHECK(added.has_value());
  CHECK_EQ(*added, uint32_t(p->size()));
  const Vec3 q = e.at(*added);
  CHECK_NEAR(q[0], 10, 1e-4);
  CHECK_NEAR(q[1], 10, 1e-4);
  CHECK_NEAR(q[2], 5, 1e-3);  // on the plane
  const pc::Edit edit = e.edit();
  CHECK_EQ(edit.added.size(), size_t(1));
  CHECK_NEAR(edit.added.normals[0], -0.5 / l, 1e-3);  // the plane's normal, turned as its neighbours'
  CHECK_NEAR(edit.added.normals[2], 1 / l, 1e-3);
  CHECK(std::abs(int(edit.added.rgb[0]) - 100) <= 12);  // its neighbours' colour (x about 10)
  // It is a point like the others: found, moved and deleted.
  std::vector<uint32_t> nb;
  e.nearest({10, 10, 5}, 1, nb);
  CHECK_EQ(nb.front(), *added);
  pc::Brush b;
  b.centre = q;
  b.toward = {0, 0, 1};
  b.radius = 0.5;
  e.push(b, 1.0);
  CHECK(e.at(*added)[2] > 5.5);
  CHECK_EQ(pc::apply(*p, e.edit()).size(), p->size() + 1);
  b.centre = e.at(*added);
  e.erase(b);
  CHECK(e.deleted(*added));
  CHECK_EQ(pc::apply(*p, e.edit()).size(), p->size());
  // Too few points to fit a plane to: nothing added.
  auto two = std::make_shared<pc::Points>();
  two->xyz = {0, 0, 0, 1, 0, 0};
  pc::Editor lone(two);
  CHECK(!lone.add({0, 0, 1}, {0, 0, 1}).has_value());
}

TEST(cloud_edit_feature_replays_the_same_cloud) {
  Files f;
  pc::Points scan = cylinder(120, 31);
  pc::write_file(f.dir / "scan.ply", scan);
  Document d = open(f.dir / "scan.ply", false);
  const std::string body = resolve(d).all_bodies().front();
  auto shape = [&] { return body_shape(d, resolve(d).node(body)->body_key); };
  auto base = std::make_shared<const pc::Points>(pc::from_shape(shape()));
  // A session: pulled out on +X, deleted round the top, a point added.
  pc::Editor e(base);
  pc::Brush b;
  b.centre = {10, 0, 10};
  b.toward = {1, 0, 0};
  b.radius = 3;
  e.push(b, 0.8);
  e.countStroke();
  b.centre = {0, 10, 19};
  b.toward = {0, 1, 0};
  e.erase(b);
  e.countStroke();
  CHECK(e.add({-10, 0, 7.3}, {-1, 0, 0}).has_value());
  e.countStroke();
  const pc::Edit edit = e.edit();
  CHECK(!edit.moved.empty() && !edit.deleted.empty() && edit.added.size() == 1);
  // Through JSON and back, bit for bit.
  const json stored = edit.to_json();
  const pc::Edit back = pc::Edit::from_json(json::parse(stored.dump()));
  CHECK(back.moved == edit.moved && back.offsets == edit.offsets && back.deleted.ranges == edit.deleted.ranges && back.added.xyz == edit.added.xyz &&
        back.added.rgb == edit.added.rgb && back.added.normals == edit.added.normals && back.added.scalar == edit.added.scalar && back.strokes == 3);
  const pc::Points expected = pc::apply(*base, edit);
  CHECK_EQ(expected.size(), base->size() - edit.deleted.count() + 1);
  // The feature: one step, the cloud changed in place.
  const json op = design::make_feature_op("cloud_edit", "Edit1", {{"bodies", json::array({{{"body", body}}})}, {"edit", stored}});
  design::apply_ops(d, {op});
  CHECK_EQ(resolve(d).all_bodies().size(), size_t(1));
  CHECK_EQ(resolve(d).node(body)->representation, std::string("points"));
  const pc::Points now = pc::from_shape(shape());
  CHECK(now.xyz == expected.xyz && now.rgb == expected.rgb && now.normals == expected.normals && now.scalar == expected.scalar);
  // Regenerated, saved and opened again: the same points.
  design::commit(d, design::plan_regenerate(d, true));
  CHECK(pc::from_shape(shape()).xyz == expected.xyz);
  d.save_as(f.dir / "edited.opad");
  const Document again = Document::load(f.dir / "edited.opad");
  CHECK(pc::from_shape(body_shape(again, resolve(again).node(body)->body_key)).xyz == expected.xyz);
  // Edited to no change: the cloud as scanned.
  const std::string id = resolve(d).features.back().id;
  design::apply_ops(d, {design::make_edit_op(id, {{"inputs", {{"bodies", json::array({{{"body", body}}})}, {"edit", json::object()}}}})});
  CHECK(pc::from_shape(shape()).xyz == base->xyz);
  // An edit made on another cloud (another number of points) is refused, and so is a malformed one.
  pc::Edit wrong = edit;
  wrong.base += 1;
  CHECK_THROWS(design::apply_ops(d, {design::make_feature_op("cloud_edit", "Bad", {{"bodies", json::array({{{"body", body}}})}, {"edit", wrong.to_json()}})}));
  json cut = stored;
  cut["offsets"] = "AAAA";
  CHECK_THROWS(pc::Edit::from_json(cut));
  CHECK_THROWS(pc::apply(*base, wrong));
}

TEST(cloud_edit_falloff_presets_and_the_front_surface) {
  // Smooth (1 - t^2)^2, Sharp (1 - t)^3, Flat: all of it to 0.6 of the radius.
  CHECK_NEAR(pc::falloff(0.5, pc::Falloff::Smooth), 0.5625, 1e-12);
  CHECK_NEAR(pc::falloff(0.5, pc::Falloff::Sharp), 0.125, 1e-12);
  CHECK_NEAR(pc::falloff(0.5, pc::Falloff::Flat), 1.0, 1e-12);
  CHECK_NEAR(pc::falloff(0.8, pc::Falloff::Flat), 0.5625, 1e-12);
  for (auto kind : {pc::Falloff::Smooth, pc::Falloff::Sharp, pc::Falloff::Flat}) {
    CHECK_NEAR(pc::falloff(0, kind), 1.0, 1e-12);
    CHECK_EQ(pc::falloff(1, kind), 0.0);
  }
  auto base = patch();
  pc::Editor e(base);
  pc::Brush b;
  b.centre = {20, 20, 0};
  b.radius = 5;
  b.falloff = pc::Falloff::Sharp;
  e.push(b, 2.0);
  CHECK_NEAR(e.at(at_xy(22, 20))[2], 2.0 * 0.216, 1e-6);  // (1 - 0.4)^3
  // A wall 4 mm behind the surface: out of reach from the front, reached when the brush goes through.
  auto two = std::make_shared<pc::Points>(*base);
  two->xyz.insert(two->xyz.end(), {20.f, 20.f, -4.f});
  two->normals.insert(two->normals.end(), {0.f, 0.f, -1.f});
  two->rgb.insert(two->rgb.end(), {0, 0, 0});
  pc::Editor h(two);
  const uint32_t back = uint32_t(two->size() - 1);
  pc::Brush front;
  front.centre = {20, 20, 0};
  front.radius = 5;
  std::vector<uint32_t> ids;
  std::vector<float> w;
  h.under(front, ids, w);
  CHECK(!std::binary_search(ids.begin(), ids.end(), back));
  front.front = false;
  h.under(front, ids, w);
  CHECK(std::binary_search(ids.begin(), ids.end(), back));
}

TEST(cloud_edit_grow_region_keeps_to_its_surface) {
  // A floor (z = 0, 30 x 30) and a wall standing on its far edge (x = 30, z up to 15), 0.5 mm apart, no normals.
  auto p = std::make_shared<pc::Points>();
  for (int j = 0; j <= 60; ++j)
    for (int i = 0; i <= 60; ++i) p->xyz.insert(p->xyz.end(), {float(i * 0.5), float(j * 0.5), 0.f});
  const size_t floor = p->size();
  for (int j = 0; j <= 60; ++j)
    for (int k = 1; k <= 30; ++k) p->xyz.insert(p->xyz.end(), {30.f, float(j * 0.5), float(k * 0.5)});
  // A step: a raised patch 1 mm up at the floor's near corner, its own surface.
  for (int j = 0; j <= 10; ++j)
    for (int i = 0; i <= 10; ++i) p->xyz.insert(p->xyz.end(), {float(-5.5 + i * 0.5), float(j * 0.5), 1.f});
  pc::Editor e(p);
  bool stopped = true;
  const pc::Region r = e.grow(uint32_t(15 * 61 + 15), 10, 0.05, 4000000, &stopped);  // a floor point
  CHECK(!stopped);
  size_t onFloor = 0, onWall = 0, onStep = 0;
  for (uint32_t i : r.indices()) {
    if (i < floor) ++onFloor;
    else if (i < floor + 61 * 30) ++onWall;
    else ++onStep;
  }
  CHECK(onFloor > floor * 9 / 10);  // the floor, up to its corner with the wall
  CHECK(onWall < 61 * 2);           // the wall's first row at most (its corner points bend both ways)
  CHECK_EQ(onStep, size_t(0));
  // Bends allowed up to a right angle: the wall comes too. A limit stops it early.
  const pc::Region all = e.grow(uint32_t(15 * 61 + 15), 95, 0.6);
  CHECK(all.count() > floor + 61 * 25);
  const pc::Region few = e.grow(uint32_t(15 * 61 + 15), 10, 0.05, 100, &stopped);
  CHECK(stopped && few.count() == 100u);
  // Deleted points are not grown over.
  e.erase(r);
  CHECK(e.grow(uint32_t(15 * 61 + 15), 10, 0.05).empty());
}

CHECK_MAIN()
