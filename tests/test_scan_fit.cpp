// Fitted references on meshes (opad/scan_fit.hpp, design/fit_refs.cpp): meshes made here of known solids, tilted, noisy
// like a scan and with a corner broken off, or clean as CAD writes STL, or organic with no flat at all; the fitted planes,
// axes and the automatic alignment checked against the answers the solids were made with.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Face.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopTools_ListOfShape.hxx>
#include <gp_Pln.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_GTrsf.hxx>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/design/feature.hpp"
#include "opad/document.hpp"
#include "opad/geometry.hpp"
#include "opad/scan_fit.hpp"
#include "opad/scene.hpp"
#include "opad/drawing_io.hpp"

using namespace opad;

namespace {

constexpr double kPi = 3.14159265358979323846;
double deg(double r) { return r * 180 / kPi; }

struct Tri {
  gp_Pnt a, b, c;
};

// The solid's triangles (BRepMesh), every one split `levels` times (in 4, at the sides' middles: two faces split a shared
// side alike, so the mesh stays closed), so a flat has thousands of facets as a scan's has, not two.
std::vector<Tri> triangles_of(const TopoDS_Shape& solid, double deflection, int levels) {
  BRepMesh_IncrementalMesh(solid, deflection, false, 0.25, true);
  std::vector<Tri> out;
  for (TopExp_Explorer f(solid, TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    TopLoc_Location loc;
    const auto tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int a, b, c;
      tri->Triangle(i).Get(a, b, c);
      if (face.Orientation() == TopAbs_REVERSED) std::swap(b, c);
      out.push_back({tri->Node(a).Transformed(loc.Transformation()), tri->Node(b).Transformed(loc.Transformation()), tri->Node(c).Transformed(loc.Transformation())});
    }
  }
  for (int k = 0; k < levels; ++k) {
    std::vector<Tri> next;
    for (const auto& t : out) {
      const gp_Pnt ab((t.a.XYZ() + t.b.XYZ()) / 2), bc((t.b.XYZ() + t.c.XYZ()) / 2), ca((t.c.XYZ() + t.a.XYZ()) / 2);
      next.push_back({t.a, ab, ca}), next.push_back({ab, t.b, bc}), next.push_back({ca, bc, t.c}), next.push_back({ab, bc, ca});
    }
    out.swap(next);
  }
  return out;
}

// A deterministic noise per position (the same vertex of two triangles moves the same way): a scan's.
gp_Vec noise_at(const gp_Pnt& p, double sigma) {
  auto h = [](long long x) {
    uint64_t z = uint64_t(x) * 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return double((z ^ (z >> 31)) >> 11) / double(1ull << 53);
  };
  const long long k = std::llround(p.X() * 1000) * 73856093LL ^ std::llround(p.Y() * 1000) * 19349663LL ^ std::llround(p.Z() * 1000) * 83492791LL;
  auto gauss = [&](int i) {  // Box-Muller
    const double u = std::max(1e-12, h(k + i * 7919)), v = h(k + i * 104729 + 1);
    return std::sqrt(-2 * std::log(u)) * std::cos(2 * kPi * v);
  };
  return gp_Vec(gauss(1), gauss(2), gauss(3)) * sigma;
}

gp_Trsf tilt() {
  gp_Trsf r, t;
  r.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), 27 * kPi / 180);
  t.SetTranslation(gp_Vec(40, -20, 15));
  return t * r;
}

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-scan-fit-" + new_uuid());
  Files() { std::filesystem::create_directory(dir); }
  ~Files() {
    std::error_code e;
    std::filesystem::remove_all(dir, e);
  }
};

// Tilted, noisy (sigma, per vertex), without the triangles `drop` says to leave out: a binary STL, imported as a mesh.
Document scan_document(const std::vector<Tri>& tris, double sigma, const std::function<bool(const gp_Pnt&)>& drop = {}) {
  static Files files;
  const auto path = files.dir / ("scan-" + new_uuid() + ".stl");
  const gp_Trsf t = tilt();
  std::vector<Tri> kept;
  for (const auto& tri : tris) {
    if (drop && drop(gp_Pnt((tri.a.XYZ() + tri.b.XYZ() + tri.c.XYZ()) / 3))) continue;
    Tri m = tri;
    for (gp_Pnt* p : {&m.a, &m.b, &m.c}) {
      const gp_Vec n = sigma > 0 ? noise_at(*p, sigma) : gp_Vec(0, 0, 0);
      p->Transform(t);
      p->Translate(n);
    }
    kept.push_back(m);
  }
  std::ofstream out(path, std::ios::binary);
  char header[80] = {};
  out.write(header, 80);
  const uint32_t n = uint32_t(kept.size());
  out.write(reinterpret_cast<const char*>(&n), 4);
  for (const auto& tri : kept) {
    float v[12] = {0, 0, 0};
    int k = 3;
    for (const gp_Pnt* p : {&tri.a, &tri.b, &tri.c}) v[k++] = float(p->X()), v[k++] = float(p->Y()), v[k++] = float(p->Z());
    out.write(reinterpret_cast<const char*>(v), 48);
    const uint16_t attr = 0;
    out.write(reinterpret_cast<const char*>(&attr), 2);
  }
  out.close();
  Document doc = Document::create();
  import_file(doc, path);
  return doc;
}

std::string mesh_node(const Document& doc) {
  const Scene s = resolve(doc);
  for (const auto& id : s.all_bodies())
    if (s.node(id)->representation == "mesh") return id;
  throw Error("no mesh body");
}

std::shared_ptr<const scanfit::FitMesh> mesh_of(const Document& doc, const std::string& node) {
  const Scene s = resolve(doc);
  return scanfit::fit_mesh(node_world_shape(doc, s, node));
}

// The triangle nearest a point of the untilted solid whose normal is near the untilted direction.
int seed_at(const scanfit::FitMesh& m, const gp_Pnt& local, const gp_Dir& normal) {
  const gp_Trsf t = tilt();
  const gp_Pnt p = local.Transformed(t);
  const gp_Dir n = normal.Transformed(t);
  int best = -1;
  double d = 1e300;
  for (int i = 0; i < m.triangles(); ++i)
    if (m.smooth[size_t(i)].Dot(gp_Vec(n)) > 0.9)
      if (const double e = m.centre[size_t(i)].Distance(p); e < d) d = e, best = i;
  if (best < 0) throw Error("no seed triangle there");
  return best;
}

double angle_to(const gp_Dir& a, const gp_Dir& b) { return deg(std::acos(std::min(1.0, std::fabs(a.Dot(b))))); }  // either way round
gp_Dir tilted(const gp_Dir& d) { return d.Transformed(tilt()); }
gp_Pnt tilted(const gp_Pnt& p) { return p.Transformed(tilt()); }

double line_distance(const gp_Pnt& p, const gp_Pnt& o, const gp_Dir& d) { return gp_Vec(o, p).Crossed(gp_Vec(d)).Magnitude(); }

// A cover plate: 100 x 60 x 4 with a lip 2 wide and 6 tall round its edge, a 12 mm hole at (30, 30), a 10 mm boss 8 tall at
// (70, 30) with a 4 mm hole.
TopoDS_Shape cover_plate() {
  TopoDS_Shape s = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 100, 60, 4).Shape();
  const TopoDS_Shape rim = BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 4), 100, 60, 6).Shape(), BRepPrimAPI_MakeBox(gp_Pnt(2, 2, 3), 96, 56, 8).Shape()).Shape();
  s = BRepAlgoAPI_Fuse(s, rim).Shape();
  s = BRepAlgoAPI_Fuse(s, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(70, 30, 3), gp_Dir(0, 0, 1)), 5, 9).Shape()).Shape();
  s = BRepAlgoAPI_Cut(s, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(30, 30, -1), gp_Dir(0, 0, 1)), 6, 12).Shape()).Shape();
  s = BRepAlgoAPI_Cut(s, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(70, 30, -1), gp_Dir(0, 0, 1)), 2, 14).Shape()).Shape();
  return s;
}

// The solid cut into cells by planes `step` apart (each face in pieces), so that its mesh has facets of about that size
// everywhere, as a scan's has (splitting the mesher's triangles instead made needles by the holes).
TopoDS_Shape in_cells(const TopoDS_Shape& s, double step, const gp_Pnt& lo, const gp_Pnt& hi) {
  TopTools_ListOfShape args, tools;
  for (TopExp_Explorer f(s, TopAbs_FACE); f.More(); f.Next()) args.Append(f.Current());  // its skin: a solid would be split into cells, inner walls and all
  const double reach = lo.Distance(hi) + 10;
  for (int axis = 1; axis <= 3; ++axis)
    for (double c = lo.Coord(axis); c <= hi.Coord(axis); c += step) {
      gp_Pnt at(0, 0, 0);
      at.SetCoord(axis, c);
      const gp_Dir n(axis == 1, axis == 2, axis == 3);
      tools.Append(BRepBuilderAPI_MakeFace(gp_Pln(at, n), -reach, reach, -reach, reach).Face());
    }
  BRepAlgoAPI_Splitter split;
  split.SetArguments(args);
  split.SetTools(tools);
  split.Build();
  return split.Shape();
}

// The plate as a scan: 0.03 mm noise and a corner broken off (everything within 9 mm of (100, 60, 4)).
Document noisy_plate() {
  static const std::vector<Tri> tris = triangles_of(in_cells(cover_plate(), 1.25, gp_Pnt(0.6, 0.6, 0.6), gp_Pnt(100, 60, 12)), 0.03, 1);
  return scan_document(tris, 0.03, [](const gp_Pnt& p) { return p.Distance(gp_Pnt(100, 60, 4)) < 9; });
}

}  // namespace

TEST(region_plane_on_a_noisy_scan) {
  const Document doc = noisy_plate();
  const auto m = mesh_of(doc, mesh_node(doc));
  int open = 0;
  for (const auto& a : m->across)
    for (int o : a) open += o < 0;
  std::printf("    plate: %d triangles, %d open sides\n", m->triangles(), open);
  CHECK(m->triangles() > 20000);
  // The plate's top inside the lip, grown from one triangle: the lip's walls and the boss stop it.
  const int seed = seed_at(*m, gp_Pnt(50, 15, 4), gp_Dir(0, 0, 1));
  const auto region = scanfit::grow_region(*m, seed, 10 * kPi / 180, false);
  const scanfit::Plane p = scanfit::fit_plane_region(*m, region);
  std::printf("    top: %zu triangles, normal off by %.3f deg, rms %.4f, grade %s\n", region.size(), angle_to(p.normal, tilted(gp_Dir(0, 0, 1))), p.q.rms, p.q.grade.c_str());
  CHECK(angle_to(p.normal, tilted(gp_Dir(0, 0, 1))) < 0.2);
  CHECK(p.normal.Dot(tilted(gp_Dir(0, 0, 1))) > 0);  // facing out of the material
  CHECK(std::fabs(gp_Vec(p.origin, tilted(gp_Pnt(50, 15, 4))).Dot(gp_Vec(p.normal))) < 0.02);
  CHECK(p.q.rms < 0.06);
  CHECK(p.q.grade == "good");
  CHECK(p.q.used > 0.8 * p.q.total);
  // The bottom, broken corner and all.
  const auto low = scanfit::grow_region(*m, seed_at(*m, gp_Pnt(50, 30, 0), gp_Dir(0, 0, -1)), 10 * kPi / 180, false);
  const auto bottom = scanfit::fit_plane_region(*m, low);
  std::printf("    bottom: %zu triangles, normal off by %.3f deg, rms %.4f, grade %s\n", low.size(), angle_to(bottom.normal, tilted(gp_Dir(0, 0, 1))), bottom.q.rms, bottom.q.grade.c_str());
  CHECK(angle_to(bottom.normal, tilted(gp_Dir(0, 0, 1))) < 0.2);
  CHECK(std::fabs(gp_Vec(bottom.origin, tilted(gp_Pnt(10, 10, 0))).Dot(gp_Vec(bottom.normal))) < 0.02);
}

TEST(a_curved_region_reports_a_poor_plane) {
  // The boss's round side taken for a plane: its report says so, where the flats said good.
  const Document doc = noisy_plate();
  const auto m = mesh_of(doc, mesh_node(doc));
  const int seed = seed_at(*m, gp_Pnt(75, 30, 8), gp_Dir(1, 0, 0));
  const auto region = scanfit::grow_region(*m, seed, 40 * kPi / 180, true);
  const scanfit::Plane p = scanfit::fit_plane_region(*m, region);
  std::printf("    boss side as a plane: rms %.3f of %.1f, grade %s\n", p.q.rms, p.q.extent, p.q.grade.c_str());
  CHECK(p.q.grade != "good");
}

TEST(cylinder_axes_of_a_hole_and_a_boss) {
  const Document doc = noisy_plate();
  const auto m = mesh_of(doc, mesh_node(doc));
  struct Case {
    gp_Pnt at;
    gp_Dir n;
    gp_Pnt axis;
    double r;
  } cases[] = {{gp_Pnt(36, 30, 2), gp_Dir(-1, 0, 0), gp_Pnt(30, 30, 0), 6}, {gp_Pnt(75, 30, 9), gp_Dir(1, 0, 0), gp_Pnt(70, 30, 0), 5}};
  for (const auto& c : cases) {
    const auto region = scanfit::grow_round(*m, seed_at(*m, c.at, c.n), 20 * kPi / 180);
    std::printf("    round region: %zu triangles\n", region.size());
    const scanfit::Line l = scanfit::fit_cylinder_region(*m, region);
    std::printf("    axis: %zu triangles, off by %.3f deg, %.3f mm, r %.3f, rms %.4f %s\n", region.size(), angle_to(l.dir, tilted(gp_Dir(0, 0, 1))),
                line_distance(tilted(c.axis), l.origin, l.dir), l.radius, l.q.rms, l.q.grade.c_str());
    CHECK(angle_to(l.dir, tilted(gp_Dir(0, 0, 1))) < 1.0);
    CHECK(line_distance(tilted(c.axis), l.origin, l.dir) < 0.1);
    CHECK(std::fabs(l.radius - c.r) < 0.1);
    CHECK(l.q.grade != "poor");
  }
}

TEST(lines_planes_and_their_intersection) {
  const Document doc = noisy_plate();
  const auto m = mesh_of(doc, mesh_node(doc));
  // The outer wall at y = 0 and the bottom meet along the plate's edge y = 0, z = 0.
  const auto wall = scanfit::fit_plane_region(*m, scanfit::grow_region(*m, seed_at(*m, gp_Pnt(50, 0, 5), gp_Dir(0, -1, 0)), 10 * kPi / 180, false));
  const auto bottom = scanfit::fit_plane_region(*m, scanfit::grow_region(*m, seed_at(*m, gp_Pnt(50, 30, 0), gp_Dir(0, 0, -1)), 10 * kPi / 180, false));
  const scanfit::Line edge = scanfit::plane_intersection(wall, bottom);
  CHECK(angle_to(edge.dir, tilted(gp_Dir(1, 0, 0))) < 0.3);
  CHECK(line_distance(tilted(gp_Pnt(50, 0, 0)), edge.origin, edge.dir) < 0.05);
  // The wall's long direction is along it.
  const auto along = scanfit::wall_direction(*m, scanfit::grow_region(*m, seed_at(*m, gp_Pnt(50, 0, 5), gp_Dir(0, -1, 0)), 10 * kPi / 180, false));
  CHECK(angle_to(along.dir, tilted(gp_Dir(1, 0, 0))) < 0.5);
  CHECK_THROWS(scanfit::plane_intersection(bottom, bottom));
  // Points along a line, noisy, with two strays: the line through the rest.
  std::vector<gp_Pnt> pts;
  for (int i = 0; i <= 40; ++i) pts.push_back(gp_Pnt(i * 2.0, 5, 3).Translated(noise_at(gp_Pnt(i, 1, 2), 0.02)));
  pts[7].Translate(gp_Vec(0, 3, 0)), pts[30].Translate(gp_Vec(0, 0, -4));
  const scanfit::Line l = scanfit::fit_line(pts);
  CHECK(angle_to(l.dir, gp_Dir(1, 0, 0)) < 0.1);
  CHECK(l.dir.X() > 0);  // from the first point towards the last
  CHECK(line_distance(gp_Pnt(40, 5, 3), l.origin, l.dir) < 0.02);
  CHECK(l.q.used == 39);
  // Points on a plane with a stray: the plane through the rest, facing as the first three turn.
  std::vector<gp_Pnt> on;
  for (int j = 0; j < 5; ++j)  // along X first: the first points turn anticlockwise seen from +Z
    for (int i = 0; i < 6; ++i) on.push_back(gp_Pnt(i * 10.0, j * 7.0, 2).Translated(noise_at(gp_Pnt(i, j, 9), 0.01)));
  on.push_back(gp_Pnt(20, 10, 15));
  const scanfit::Plane q = scanfit::fit_plane(on);
  CHECK(angle_to(q.normal, gp_Dir(0, 0, 1)) < 0.1);
  CHECK(q.normal.Z() > 0);
  CHECK(q.q.used == 30);
}

TEST(clean_cad_stl_fits_exactly) {
  // A box with a boss, meshed as CAD writes STL (a flat is a couple of triangles), tilted, no noise.
  TopoDS_Shape s = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 80, 50, 20).Shape();
  s = BRepAlgoAPI_Fuse(s, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(40, 25, 19), gp_Dir(0, 0, 1)), 8, 16).Shape()).Shape();
  const Document doc = scan_document(triangles_of(s, 0.02, 0), 0);
  const auto m = mesh_of(doc, mesh_node(doc));
  std::printf("    CAD mesh: %d triangles\n", m->triangles());
  const auto top = scanfit::fit_plane_region(*m, scanfit::grow_region(*m, seed_at(*m, gp_Pnt(10, 10, 20), gp_Dir(0, 0, 1)), 2 * kPi / 180, false));
  CHECK(angle_to(top.normal, tilted(gp_Dir(0, 0, 1))) < 1e-3);
  CHECK(std::fabs(gp_Vec(top.origin, tilted(gp_Pnt(70, 45, 20))).Dot(gp_Vec(top.normal))) < 1e-3);
  CHECK(top.q.rms < 1e-3);
  CHECK(top.q.grade == "good");
  const auto side = scanfit::fit_plane_region(*m, scanfit::grow_region(*m, seed_at(*m, gp_Pnt(80, 25, 10), gp_Dir(1, 0, 0)), 2 * kPi / 180, false));
  CHECK(angle_to(side.normal, tilted(gp_Dir(1, 0, 0))) < 1e-3);
  const auto boss = scanfit::fit_cylinder_region(*m, scanfit::grow_round(*m, seed_at(*m, gp_Pnt(48, 25, 28), gp_Dir(1, 0, 0)), 25 * kPi / 180));
  std::printf("    boss: r %.4f, off %.4f deg, rms %.5f\n", boss.radius, angle_to(boss.dir, tilted(gp_Dir(0, 0, 1))), boss.q.rms);
  CHECK(angle_to(boss.dir, tilted(gp_Dir(0, 0, 1))) < 0.05);
  CHECK(std::fabs(boss.radius - 8) < 0.02);
  CHECK(line_distance(tilted(gp_Pnt(40, 25, 0)), boss.origin, boss.dir) < 0.02);
}

TEST(scan_axes_of_a_noisy_plate) {
  const Document doc = noisy_plate();
  const auto m = mesh_of(doc, mesh_node(doc));
  const scanfit::ScanAxes axes = scanfit::scan_axes(*m);
  std::printf("    method %s, %zu bases\n", axes.method.c_str(), axes.bases.size());
  CHECK(axes.method == "flats");
  CHECK(axes.bases.size() >= 2);
  // The bottom first (the largest flat the whole plate stands behind), its normal out of the material.
  CHECK(angle_to(axes.bases[0].normal, tilted(gp_Dir(0, 0, 1))) < 0.3);
  CHECK(axes.bases[0].normal.Dot(tilted(gp_Dir(0, 0, -1))) > 0);
  // X: the walls' direction, its quarter turns offered in turn.
  const auto turns = scanfit::scan_turns(*m, axes, 0);
  CHECK(turns.size() >= 4);
  CHECK(std::min(angle_to(turns[0], tilted(gp_Dir(1, 0, 0))), angle_to(turns[0], tilted(gp_Dir(0, 1, 0)))) < 0.3);
  CHECK(angle_to(turns[0], turns[1]) > 89.5);
  // Some base is the top of the lip, facing the other way.
  bool lip = false;
  for (const auto& b : axes.bases)
    if (angle_to(b.normal, tilted(gp_Dir(0, 0, 1))) < 0.5 && b.normal.Dot(tilted(gp_Dir(0, 0, 1))) > 0) lip = true;
  CHECK(lip);
}

TEST(scan_axes_of_an_organic_blob) {
  // An ellipsoid 100 x 60 x 30, noisy: no flats, its principal axes. The shortest stands up, the longest is X.
  TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(1).Shape();
  gp_GTrsf g;
  g.SetValue(1, 1, 50), g.SetValue(2, 2, 30), g.SetValue(3, 3, 15);
  const TopoDS_Shape blob = BRepBuilderAPI_GTransform(sphere, g, true).Shape();
  const Document doc = scan_document(triangles_of(blob, 0.05, 1), 0.05);
  const auto m = mesh_of(doc, mesh_node(doc));
  const scanfit::ScanAxes axes = scanfit::scan_axes(*m);
  std::printf("    blob: %d triangles, method %s\n", m->triangles(), axes.method.c_str());
  CHECK(axes.method == "pca");
  CHECK(axes.bases.size() == 6);
  CHECK(angle_to(axes.bases[0].normal, tilted(gp_Dir(0, 0, 1))) < 1);
  const auto turns = scanfit::scan_turns(*m, axes, 0);
  CHECK(angle_to(turns[0], tilted(gp_Dir(1, 0, 0))) < 1);
  const scanfit::ScanFrame f = scanfit::scan_frame(*m, axes, 0, 0, "auto");
  CHECK(f.origin.Distance(tilted(gp_Pnt(0, 0, 0))) < 0.5);  // the box centre
}

TEST(a_point_cloud_has_nodes_and_principal_axes) {
  // A cloud body: one face whose triangulation holds points and no triangles. Its nodes are read, it stands by PCA.
  Handle(Poly_Triangulation) t = new Poly_Triangulation(4000, 0, false);
  for (int i = 0; i < 4000; ++i) {
    const double u = (i % 80) * 1.0, v = (i / 80) * 0.6;  // a flat 80 x 30 sheet, 0.2 thick by noise
    t->SetNode(i + 1, tilted(gp_Pnt(u, v, 0).Translated(noise_at(gp_Pnt(u, v, 1), 0.1))));
  }
  TopoDS_Face face;
  BRep_Builder().MakeFace(face, t);
  const auto m = scanfit::fit_mesh(face);
  CHECK(m->triangles() == 0 && m->nodes.size() == 4000u);
  const scanfit::Plane p = scanfit::fit_plane(m->nodes);
  CHECK(angle_to(p.normal, tilted(gp_Dir(0, 0, 1))) < 0.5);
  const scanfit::ScanAxes axes = scanfit::scan_axes(*m);
  CHECK(axes.method == "pca");
  CHECK(angle_to(axes.bases[0].normal, tilted(gp_Dir(0, 0, 1))) < 0.5);
  CHECK(angle_to(scanfit::scan_turns(*m, axes, 0)[0], tilted(gp_Dir(1, 0, 0))) < 0.5);
}

// ---------------------------------------------------------------- as references (design)
namespace {
json body_ref(const std::string& node) { return {{"body", node}, {"kind", "body"}}; }

struct Extents {
  double lo[3], hi[3];
};
Extents world_extents(const Document& doc, const std::string& node) {
  const Scene s = resolve(doc);
  const auto m = scanfit::fit_mesh(node_world_shape(doc, s, node));
  Extents e{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
  for (const auto& p : m->nodes)
    for (int k = 0; k < 3; ++k) e.lo[k] = std::min(e.lo[k], p.Coord(k + 1)), e.hi[k] = std::max(e.hi[k], p.Coord(k + 1));
  return e;
}
}  // namespace

TEST(align_a_scan_by_a_fitted_plane) {
  Document doc = noisy_plate();
  const std::string scan = mesh_node(doc);
  const int seed = seed_at(*mesh_of(doc, scan), gp_Pnt(50, 30, 0), gp_Dir(0, 0, -1));
  // The fitted bottom face to face onto XY: the plate stands on it.
  const json r = commands::run("feature", {{"kind", "align"},
                                           {"inputs", {{"bodies", json::array({body_ref(scan)})}, {"from", "plane"},
                                                       {"from_plane", {{"fit", {{"body", scan}, {"seed", seed}, {"tolerance", "10 deg"}}}}},
                                                       {"to", "plane"}, {"to_plane", {{"base", "xy"}}}}}},
                               &doc);
  CHECK(!r.contains("error"));
  // The op keeps what it fitted: its result, with the body key it was fitted on.
  const json& op = doc.ops.back().data;
  const json& stored = op.at("inputs").at("from_plane");
  CHECK(stored.contains("result"));
  CHECK(stored["result"].value("of", "") == "plane");
  CHECK(stored["result"]["quality"].value("grade", "") == "good");
  // The bottom now lies in XY (the noise and the broken corner aside): every node within a few sigma above it.
  const Extents e = world_extents(doc, scan);
  std::printf("    after align: z %.3f .. %.3f\n", e.lo[2], e.hi[2]);
  CHECK(std::fabs(e.lo[2]) < 0.2);
  CHECK(std::fabs(e.hi[2] - 12) < 0.2);
  // Fitted again on the moved scan, through the same reference: normal -Z (out of the material, down).
  const Scene s = resolve(doc);
  const json again = design::fit_reference(doc, s, {{"fit", {{"body", scan}, {"seed", seed}, {"tolerance", "10 deg"}}}}, "plane");
  CHECK_NEAR(again["world"]["normal"][2].get<double>(), -1, 1e-4);
  // A regeneration of everything adds no body and fits nothing again (the result is current).
  const design::Plan plan = design::plan_regenerate(doc, true);
  CHECK(plan.bodies.empty());
}

TEST(align_a_scan_to_the_axes_automatically) {
  Document doc = noisy_plate();
  const std::string scan = mesh_node(doc);
  const json report = design::scan_axes_report(doc, resolve(doc), scan);
  CHECK(report.value("method", "") == "flats");
  CHECK(report["bases"].size() >= 2);
  CHECK(report["bases"][0]["turns"].size() >= 4);
  const json r = commands::run("feature", {{"kind", "align"}, {"inputs", report["align_inputs"]}}, &doc);
  CHECK(!r.contains("error"));
  // Standing on XY, its sides along X and Y, centred on the origin.
  const Extents e = world_extents(doc, scan);
  const double dx = e.hi[0] - e.lo[0], dy = e.hi[1] - e.lo[1];
  std::printf("    after: %.2f x %.2f x %.2f, centre %.2f %.2f, z from %.3f\n", dx, dy, e.hi[2] - e.lo[2], (e.lo[0] + e.hi[0]) / 2, (e.lo[1] + e.hi[1]) / 2, e.lo[2]);
  CHECK((std::fabs(dx - 100) < 0.3 && std::fabs(dy - 60) < 0.3) || (std::fabs(dx - 60) < 0.3 && std::fabs(dy - 100) < 0.3));
  CHECK(std::fabs(e.lo[2]) < 0.2);
  CHECK(std::fabs(e.hi[2] - 12) < 0.2);
  CHECK(std::fabs(e.lo[0] + e.hi[0]) < 0.3 && std::fabs(e.lo[1] + e.hi[1]) < 0.3);
  // Turn 1 is a quarter turn: the other side along X.
  Document other = noisy_plate();
  const std::string scan2 = mesh_node(other);
  json in = design::scan_axes_report(other, resolve(other), scan2)["align_inputs"];
  in["from_plane"]["fit"]["turn"] = 1;
  commands::run("feature", {{"kind", "align"}, {"inputs", in}}, &other);
  const Extents f = world_extents(other, scan2);
  CHECK(std::fabs((f.hi[0] - f.lo[0]) - dy) < 0.3 && std::fabs((f.hi[1] - f.lo[1]) - dx) < 0.3);
  // Flip: upside down, standing on the lip's top.
  Document flipped = noisy_plate();
  const std::string scan3 = mesh_node(flipped);
  json fin = design::scan_axes_report(flipped, resolve(flipped), scan3)["align_inputs"];
  fin["flip"] = true;
  commands::run("feature", {{"kind", "align"}, {"inputs", fin}}, &flipped);
  const Extents g = world_extents(flipped, scan3);
  CHECK(std::fabs(g.hi[2]) < 0.2);  // the bottom on XY, the plate below it
}

TEST(fitted_axes_and_points_as_references) {
  Document doc = noisy_plate();
  const std::string scan = mesh_node(doc);
  const auto m = mesh_of(doc, scan);
  const Scene s = resolve(doc);
  // A hole's axis by a seed ("auto" decides it is round).
  const json hole = design::fit_reference(doc, s, {{"fit", {{"body", scan}, {"seed", seed_at(*m, gp_Pnt(36, 30, 2), gp_Dir(-1, 0, 0))}}}}, "axis");
  CHECK(hole["result"].value("of", "") == "cylinder");
  CHECK_NEAR(hole["result"].value("radius", 0.0), 6, 0.1);
  // A construction axis on it, and a construction plane through the hole's axis and the plate's normal (two planes' line).
  const json axis = commands::run("feature", {{"kind", "axis"}, {"inputs", {{"mode", "edge"}, {"edge", {{"fit", hole["fit"]}}}}}}, &doc);
  CHECK(!axis.contains("error"));
  // Points: the corners of the bottom picked as mesh vertices (node ordinals), a best-fit construction plane through them.
  json points = json::array();
  for (const gp_Pnt& c : {gp_Pnt(0, 0, 0), gp_Pnt(100, 0, 0), gp_Pnt(0, 60, 0), gp_Pnt(50, 0, 0), gp_Pnt(0, 30, 0)}) {
    const gp_Pnt w = tilted(c);
    int best = 0;
    for (size_t i = 0; i < m->nodes.size(); ++i)
      if (m->nodes[i].Distance(w) < m->nodes[size_t(best)].Distance(w)) best = int(i);
    points.push_back({{"body", scan}, {"kind", "vertex"}, {"index", best}});
  }
  const json plane = commands::run("feature", {{"kind", "plane"}, {"inputs", {{"mode", "three_points"}, {"points", points}}}}, &doc);
  CHECK(!plane.contains("error"));
  const Scene after = resolve(doc);
  const Feature* f = nullptr;
  for (const auto& feat : after.features)
    if (feat.result.contains("plane")) f = &feat;
  CHECK(f != nullptr);
  const Frame frame = Frame::from_json(f->result["plane"]);
  const Vec3 n = frame.normal();
  CHECK(angle_to(gp_Dir(n[0], n[1], n[2]), tilted(gp_Dir(0, 0, 1))) < 0.2);
  // A plane fitted to the same points through a fit reference; a line through three of them.
  const json pf = design::fit_reference(doc, after, {{"fit", {{"points", points}}}}, "plane");
  CHECK(angle_to(gp_Dir(pf["world"]["normal"][0], pf["world"]["normal"][1], pf["world"]["normal"][2]), tilted(gp_Dir(0, 0, 1))) < 0.2);
  const json lf = design::fit_reference(doc, after, {{"fit", {{"points", json::array({points[0], points[3], points[1]})}, {"of", "line"}}}}, "axis");
  CHECK(angle_to(gp_Dir(lf["world"]["dir"][0], lf["world"]["dir"][1], lf["world"]["dir"][2]), tilted(gp_Dir(1, 0, 0))) < 0.3);
  // Two fitted planes' intersection as an axis.
  const json bottom = {{"fit", {{"body", scan}, {"seed", seed_at(*m, gp_Pnt(50, 30, 0), gp_Dir(0, 0, -1))}}}};
  const json wall = {{"fit", {{"body", scan}, {"seed", seed_at(*m, gp_Pnt(50, 0, 5), gp_Dir(0, -1, 0))}}}};
  const json edge = design::fit_reference(doc, after, {{"fit", {{"of", "intersection"}, {"planes", {bottom, wall}}}}}, "axis");
  CHECK(angle_to(gp_Dir(edge["world"]["dir"][0], edge["world"]["dir"][1], edge["world"]["dir"][2]), tilted(gp_Dir(1, 0, 0))) < 0.3);
  // A construction axis where the two fitted planes meet (mode two_planes).
  const json meet = commands::run("feature", {{"kind", "axis"}, {"inputs", {{"mode", "two_planes"}, {"plane", bottom}, {"plane2", wall}}}}, &doc);
  CHECK(!meet.contains("error"));
  const Scene later = resolve(doc);
  const json& made = later.features.back().result.at("axis");
  CHECK(angle_to(gp_Dir(made["dir"][0], made["dir"][1], made["dir"][2]), tilted(gp_Dir(1, 0, 0))) < 0.3);
  CHECK(line_distance(tilted(gp_Pnt(50, 0, 0)), gp_Pnt(made["origin"][0], made["origin"][1], made["origin"][2]), gp_Dir(made["dir"][0], made["dir"][1], made["dir"][2])) < 0.05);
  // Both fits were kept in the op (fitted once, when it was made).
  const json& kept = doc.ops.back().data.at("inputs");
  CHECK(kept["plane"].contains("result") && kept["plane2"].contains("result"));
  // A fit on a solid body is refused with a reason.
  CHECK_THROWS(design::fit_reference(doc, after, {{"fit", {{"body", "nope"}, {"seed", 0}}}}, "plane"));
}

CHECK_MAIN()
