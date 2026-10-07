// Mesh to solid (design.mesh_solid): meshes of known solids, written as STL and imported, rebuilt and measured against.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/design/mesh_solid.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh_solid.hpp"

using namespace opad;

namespace {

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-mesh-solid-" + new_uuid());
  Files() { std::filesystem::create_directory(dir); }
  ~Files() {
    std::error_code e;
    std::filesystem::remove_all(dir, e);
  }
};

// The solid's triangles as an ASCII STL (what a CAD program exports), imported as a mesh body into a new document.
Document mesh_document(const TopoDS_Shape& solid, double deflection, double angle_deg = 20, const std::string& name = "part") {
  BRepMesh_IncrementalMesh(solid, deflection, false, angle_deg * M_PI / 180, true);
  std::ostringstream stl;
  stl.precision(10);
  stl << "solid " << name << "\n";
  for (TopExp_Explorer f(solid, TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    TopLoc_Location loc;
    const auto tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int a, b, c;
      tri->Triangle(i).Get(a, b, c);
      if (face.Orientation() == TopAbs_REVERSED) std::swap(b, c);
      stl << "facet normal 0 0 0\nouter loop\n";
      for (int v : {a, b, c}) {
        const gp_Pnt p = tri->Node(v).Transformed(loc.Transformation());
        stl << "vertex " << p.X() << " " << p.Y() << " " << p.Z() << "\n";
      }
      stl << "endloop\nendfacet\n";
    }
  }
  stl << "endsolid " << name << "\n";
  static Files files;
  const auto path = files.dir / (name + "-" + new_uuid() + ".stl");
  std::ofstream(path) << stl.str();
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

int faces_of(const json& report, const char* kind) { return report.at("faces").value(kind, 0); }

design::MeshConversion convert(const Document& doc, const std::string& mode = "auto", double tolerance = 0) {
  design::MeshConversionOptions o;
  o.mode = mode;
  o.solid.tolerance = tolerance;
  const Scene s = resolve(doc);
  return design::convert_mesh(doc, s, mesh_node(doc), o);
}

void check_close(const design::MeshConversion& c) {
  const auto& d = c.deviation;
  std::printf("    %s: %s\n", c.method.c_str(), c.report.dump().c_str());
  CHECK(BRepCheck_Analyzer(c.shape).IsValid());
  CHECK(d.max <= d.tolerance * 1.5);
  CHECK(d.within > 0.99);
  CHECK(d.score() > 95);
  CHECK(std::abs(d.shape_volume - d.mesh_volume) <= 0.05 * d.mesh_volume);
}

}  // namespace

TEST(profile_fitting_finds_lines_arcs_and_circles) {
  // A rounded rectangle 40 x 20, corner radius 5, its arcs in 10 degree steps: four lines and four arcs.
  std::vector<std::array<double, 2>> pts;
  const double cx[] = {15, -15, -15, 15}, cy[] = {5, 5, -5, -5};
  for (int k = 0; k < 4; ++k)
    for (int i = 0; i <= 9; ++i) {
      const double a = (k * 90 + i * 10) * M_PI / 180;
      pts.push_back({cx[k] + 5 * std::cos(a), cy[k] + 5 * std::sin(a)});
    }
  design::Sketch sk;
  const auto ids = design::fit_profile(sk, pts, true, 0.01, 35 * M_PI / 180, 0.02);
  int lines = 0, arcs = 0;
  for (int id : ids) {
    const auto* e = sk.entity(id);
    lines += e->type == design::SkEntity::Type::Line;
    arcs += e->type == design::SkEntity::Type::Arc;
    if (e->type == design::SkEntity::Type::Arc) {
      const auto* c = sk.point(e->p[0]);
      CHECK(std::abs(std::abs(c->x) - 15) < 1e-6 && std::abs(std::abs(c->y) - 5) < 1e-6);
    }
  }
  CHECK_EQ(lines, 4);
  CHECK_EQ(arcs, 4);
  // A full circle in 24 steps: one circle.
  std::vector<std::array<double, 2>> ring;
  for (int i = 0; i < 24; ++i) ring.push_back({3 + 7 * std::cos(i * M_PI / 12), -2 + 7 * std::sin(i * M_PI / 12)});
  design::Sketch one;
  const auto circle = design::fit_profile(one, ring, true, 0.001, 35 * M_PI / 180, 0.1);
  CHECK_EQ(circle.size(), size_t(1));
  CHECK(one.entity(circle[0])->type == design::SkEntity::Type::Circle);
  CHECK_NEAR(one.entity(circle[0])->r, 7, 1e-9);
  // A hexagon stays six lines (its corners turn by more than a facet does).
  std::vector<std::array<double, 2>> hex;
  for (int i = 0; i < 6; ++i) hex.push_back({10 * std::cos(i * M_PI / 3), 10 * std::sin(i * M_PI / 3)});
  design::Sketch six;
  CHECK_EQ(design::fit_profile(six, hex, true, 0.01, 35 * M_PI / 180).size(), size_t(6));
}

TEST(cylinder_becomes_one_circle_extruded) {
  const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(5, -3, 2), gp_Dir(0, 0, 1)), 10, 25).Shape();
  const Document doc = mesh_document(cyl, 0.05);
  const auto c = convert(doc);
  check_close(c);
  CHECK_EQ(c.method, std::string("extrude"));
  CHECK_EQ(c.report.value("curves", 0), 1);
  CHECK_EQ(c.deviation.shape_faces, 3);
  CHECK_EQ(faces_of(c.report, "cylinder"), 1);
  CHECK_EQ(faces_of(c.report, "plane"), 2);
  CHECK(c.deviation.mesh_triangles > 100);
  // The plan holds the sketch (one circle) and the extrusion, ready to commit; the committed body is the measured one.
  Document d = doc;
  design::commit(d, std::move(const_cast<design::MeshConversion&>(c).plan));
  const Scene s = resolve(d);
  CHECK_EQ(s.sketches.size(), size_t(1));
  CHECK_EQ(s.features.size(), size_t(1));
  CHECK_EQ(s.features[0].kind, std::string("extrude"));
  int solids = 0;
  for (const auto& id : s.all_bodies()) solids += s.node(id)->representation != "mesh";
  CHECK_EQ(solids, 1);
}

TEST(plate_with_holes_is_an_extrusion_along_its_thin_side) {
  TopoDS_Shape plate = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 60, 40, 6).Shape();
  for (const auto& at : {gp_Pnt(15, 20, -1), gp_Pnt(45, 20, -1)})
    plate = BRepAlgoAPI_Cut(plate, BRepPrimAPI_MakeCylinder(gp_Ax2(at, gp_Dir(0, 0, 1)), 4, 8).Shape()).Shape();
  const auto c = convert(mesh_document(plate, 0.02));
  check_close(c);
  CHECK_EQ(c.method, std::string("extrude"));
  CHECK_EQ(c.report.value("curves", 0), 6);  // four lines, two circles
  CHECK_EQ(c.deviation.shape_faces, 8);
}

TEST(rounded_bracket_profile_keeps_its_arcs) {
  // An L section with a filleted inside corner, extruded along Y: lines and one arc.
  TopoDS_Shape l = BRepAlgoAPI_Fuse(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 40, 30, 5).Shape(), BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 5, 30, 40).Shape()).Shape();
  BRepFilletAPI_MakeFillet fillet(l);
  for (TopExp_Explorer e(l, TopAbs_EDGE); e.More(); e.Next()) {
    BRepAdaptor_Curve curve(TopoDS::Edge(e.Current()));
    const gp_Pnt mid = curve.Value((curve.FirstParameter() + curve.LastParameter()) / 2);
    if (std::abs(mid.X() - 5) < 1e-6 && std::abs(mid.Z() - 5) < 1e-6) fillet.Add(4, TopoDS::Edge(e.Current()));
  }
  const TopoDS_Shape bracket = fillet.Shape();
  const auto c = convert(mesh_document(bracket, 0.01));
  check_close(c);
  CHECK_EQ(c.method, std::string("extrude"));
  CHECK_EQ(faces_of(c.report, "cylinder"), 1);
  CHECK_EQ(c.deviation.shape_faces, 9);
}

TEST(turned_parts_become_revolutions) {
  // A cone frustum on a cylinder: a revolution (the extrusion test fails on the cone).
  const TopoDS_Shape shaft = BRepAlgoAPI_Fuse(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 12, 10).Shape(),
                                              BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, 10), gp_Dir(0, 0, 1)), 8, 4, 15).Shape())
                                 .Shape();
  const auto c = convert(mesh_document(shaft, 0.02));
  check_close(c);
  CHECK_EQ(c.method, std::string("revolve"));
  CHECK_EQ(faces_of(c.report, "cone"), 1);
  CHECK_EQ(faces_of(c.report, "cylinder"), 1);
  // A torus: a ring profile (one circle) revolved.
  const auto t = convert(mesh_document(BRepPrimAPI_MakeTorus(gp_Ax2(gp_Pnt(1, 2, 3), gp_Dir(0, 1, 0)), 20, 4).Shape(), 0.02));
  check_close(t);
  CHECK_EQ(t.method, std::string("revolve"));
  CHECK_EQ(t.report.value("curves", 0), 1);
  CHECK_EQ(faces_of(t.report, "torus"), 1);
}

TEST(other_shapes_get_fitted_faces) {
  // A block with a hole through one side and a boss on another: neither an extrusion nor a revolution.
  TopoDS_Shape part = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 50, 30, 20).Shape();
  part = BRepAlgoAPI_Cut(part, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(25, -1, 10), gp_Dir(0, 1, 0)), 5, 32).Shape()).Shape();
  part = BRepAlgoAPI_Fuse(part, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(12, 15, 20), gp_Dir(0, 0, 1)), 6, 8).Shape()).Shape();
  const auto c = convert(mesh_document(part, 0.02));
  check_close(c);
  CHECK_EQ(c.method, std::string("solid"));
  CHECK_EQ(faces_of(c.report, "cylinder"), 2);
  CHECK(c.deviation.shape_faces <= 12);
  CHECK(c.deviation.mesh_triangles > 10 * c.deviation.shape_faces);
}

TEST(spheres_and_solid_mode) {
  const TopoDS_Shape ball = BRepPrimAPI_MakeSphere(gp_Pnt(3, 4, 5), 15).Shape();
  const auto c = convert(mesh_document(ball, 0.02), "solid");
  check_close(c);
  CHECK_EQ(c.method, std::string("solid"));
  CHECK_EQ(faces_of(c.report, "sphere"), 1);
  // A cylinder asked as a solid: the general path also gives three faces.
  const auto s = convert(mesh_document(BRepPrimAPI_MakeCylinder(10, 20).Shape(), 0.05), "solid");
  check_close(s);
  CHECK_EQ(s.method, std::string("solid"));
  CHECK_EQ(s.deviation.shape_faces, 3);
}

TEST(deviation_sees_a_wrong_result) {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(20, 20, 20).Shape();
  const Document doc = mesh_document(box, 0.05);
  const TriMesh mesh = mesh_of_shape(node_world_shape(doc, resolve(doc), mesh_node(doc)));
  const MeshDeviation same = mesh_deviation(mesh, box, 0.01);
  CHECK(same.max < 1e-6 && same.within == 1 && same.score() > 99.9);
  const MeshDeviation bigger = mesh_deviation(mesh, BRepPrimAPI_MakeBox(gp_Pnt(-0.5, -0.5, -0.5), 21, 21, 21).Shape(), 0.01);
  CHECK_NEAR(bigger.max, 0.5, 1e-6);
  CHECK(bigger.within == 0);
  CHECK(bigger.score() < 1);
  CHECK(bigger.shape_volume > bigger.mesh_volume);
}

TEST(command_converts_and_removes_the_source) {
  Document doc = mesh_document(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 6, 30).Shape(), 0.05, 20, "rod");
  const std::string mesh = mesh_node(doc);
  const json out = commands::run("mesh_to_solid", {{"body", mesh}, {"name", "Rod"}, {"remove_source", true}}, &doc);
  CHECK_EQ(out.at("method").get<std::string>(), std::string("extrude"));
  CHECK(out.at("deviation").at("within").get<double>() > 99);
  const Scene s = resolve(doc);
  bool found = false;
  for (const auto& id : s.all_bodies()) {
    CHECK(s.node(id)->representation != "mesh");  // the mesh is removed
    found |= s.node(id)->name == "Rod";
  }
  CHECK(found);
  // A solid is refused.
  Document solid = Document::create();
  commands::run("feature", {{"kind", "box"}}, &solid);
  CHECK_THROWS(commands::run("mesh_to_solid", {{"body", resolve(solid).all_bodies().front()}}, &solid));
}

TEST(coarse_and_turned_meshes) {
  // A coarse export (30 degree facets, 12 sides) still reads as a circle; the part tilted off every axis too.
  gp_Trsf tilt;
  tilt.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 2, 3)), 0.7);
  TopoDS_Shape plate = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 30, 20, 4).Shape();
  plate = BRepAlgoAPI_Cut(plate, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(15, 10, -1), gp_Dir(0, 0, 1)), 5, 6).Shape()).Shape();
  const auto c = convert(mesh_document(BRepBuilderAPI_Transform(plate, tilt).Shape(), 0.5, 30));
  check_close(c);
  CHECK_EQ(c.method, std::string("extrude"));
  CHECK_EQ(c.report.value("curves", 0), 5);
  CHECK_EQ(faces_of(c.report, "cylinder"), 1);
  // A pointed cone: its apex on the axis.
  const auto cone = convert(mesh_document(BRepPrimAPI_MakeCone(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 10, 0, 18).Shape(), 0.02));
  check_close(cone);
  CHECK_EQ(cone.method, std::string("revolve"));
  CHECK_EQ(faces_of(cone.report, "cone"), 1);
}

TEST(rounded_box_gets_cylinders_and_spheres) {
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(-10, -8, -5), 20, 16, 10).Shape();
  BRepFilletAPI_MakeFillet fillet(box);
  for (TopExp_Explorer e(box, TopAbs_EDGE); e.More(); e.Next()) fillet.Add(2, TopoDS::Edge(e.Current()));
  const auto c = convert(mesh_document(fillet.Shape(), 0.01));
  check_close(c);
  CHECK_EQ(c.method, std::string("solid"));
  CHECK_EQ(faces_of(c.report, "plane"), 6);
  CHECK_EQ(faces_of(c.report, "cylinder"), 12);
  CHECK_EQ(faces_of(c.report, "sphere"), 8);
}

TEST(torus_as_a_fitted_solid) {
  const auto c = convert(mesh_document(BRepPrimAPI_MakeTorus(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 15, 5).Shape(), 0.02), "solid");
  check_close(c);
  CHECK_EQ(c.deviation.shape_faces, 1);
  CHECK_EQ(faces_of(c.report, "torus"), 1);
}

TEST(two_pieces_and_open_meshes) {
  // Two cylinders apart at different heights: neither one extrusion nor one revolution; two fitted solids.
  BRep_Builder b;
  TopoDS_Compound two;
  b.MakeCompound(two);
  b.Add(two, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 5, 10).Shape());
  b.Add(two, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(30, 0, 4), gp_Dir(0, 0, 1)), 4, 20).Shape());
  const auto c = convert(mesh_document(two, 0.02));
  check_close(c);
  CHECK_EQ(c.method, std::string("solid"));
  CHECK_EQ(c.deviation.shape_faces, 6);
  // A box without its lid: no solid to make, but the faces still come out as an open shell, measured the same way.
  TopoDS_Compound open;
  b.MakeCompound(open);
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
  for (TopExp_Explorer f(box, TopAbs_FACE); f.More(); f.Next()) {
    BRepAdaptor_Surface s(TopoDS::Face(f.Current()));
    if (s.Plane().Location().Z() < 9.9 || std::abs(s.Plane().Axis().Direction().Z()) < 0.5) b.Add(open, f.Current());
  }
  const Document lidless = mesh_document(open, 0.05);
  const TriMesh mesh = mesh_of_shape(node_world_shape(lidless, resolve(lidless), mesh_node(lidless)));
  CHECK(mesh.open_edges() > 0);
  const MeshBrep shell = mesh_to_brep(mesh, {});
  CHECK(BRepCheck_Analyzer(shell.shape).IsValid());
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shell.shape, TopAbs_FACE, faces);
  CHECK_EQ(faces.Extent(), 5);
}

TEST(dense_mesh_in_reasonable_time) {
  // About 60,000 triangles of curved surfaces.
  TopoDS_Shape part = BRepAlgoAPI_Fuse(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 20).Shape(), BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), 8, 35).Shape()).Shape();
  const Document doc = mesh_document(part, 0.004, 10);
  const auto start = std::chrono::steady_clock::now();
  const auto c = convert(doc);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("    %d triangles in %.2f s\n", c.deviation.mesh_triangles, seconds);
  check_close(c);
  CHECK(c.deviation.mesh_triangles > 20000);
  CHECK_EQ(faces_of(c.report, "sphere"), 1);
  CHECK_EQ(faces_of(c.report, "cylinder"), 1);
  CHECK(seconds < 60);
}

TEST(fitted_solid_is_a_feature_that_regenerates) {
  // A sphere asked as a solid with the mesh removed: a mesh_solid feature and a Remove; editing its tolerance regenerates
  // the body from the mesh it still references, and the saved document opens the same.
  Document doc = mesh_document(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 12).Shape(), 0.05, 20, "ball");
  const json out = commands::run("mesh_to_solid", {{"body", mesh_node(doc)}, {"mode", "solid"}, {"remove_source", true}}, &doc);
  CHECK_EQ(out.at("method").get<std::string>(), std::string("solid"));
  Scene s = resolve(doc);
  CHECK_EQ(s.features.size(), size_t(2));
  CHECK_EQ(s.features[0].kind, std::string("mesh_solid"));
  CHECK_EQ(s.features[1].kind, std::string("remove"));
  CHECK_EQ(s.all_bodies().size(), size_t(1));
  const std::string key = s.node(s.all_bodies().front())->body_key;
  commands::run("feature_edit", {{"target", s.features[0].id}, {"inputs", {{"tolerance", "0.05 mm"}}}}, &doc);
  s = resolve(doc);
  CHECK(s.unresolved.empty());
  CHECK_EQ(s.all_bodies().size(), size_t(1));
  CHECK(s.features[0].error.empty());
  const Document reopened = Document::parse(doc.serialize());
  CHECK(resolve(reopened).unresolved.empty());
  CHECK(!key.empty());
}

CHECK_MAIN()
