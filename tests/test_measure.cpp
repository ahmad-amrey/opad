// Measuring (inspect.hpp): radii of free-form cylinders and circles (UI-50); centre-to-centre and largest distances, lengths,
// loop perimeters and areas (UI-144).
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_NurbsConvert.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Tool.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>

#include <cmath>
#include <functional>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/geometry.hpp"

using namespace opad;

namespace {
// One body node holding `shape` under `key`, placed by `local`.
std::string add_body(Document& d, Scene& scene, const std::string& key, const TopoDS_Shape& shape, const Mat4& local = Mat4()) {
  cache_shape(d, key, shape);
  Node n;
  n.id = new_uuid();
  n.kind = Node::Kind::Body;
  n.name = key;
  n.body_key = key;
  n.local = local;
  scene.nodes[n.id] = n;
  scene.roots.push_back(n.id);
  return n.id;
}

Ref ref(const std::string& body, Ref::Kind kind, int index) {
  Ref r;
  r.body = body;
  r.kind = kind;
  r.index = index;
  return r;
}

// The first face / edge of `shape` whose adaptor type passes `test`.
int face_where(const TopoDS_Shape& shape, const std::function<bool(const BRepAdaptor_Surface&)>& test) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i)
    if (test(BRepAdaptor_Surface(TopoDS::Face(faces(i))))) return i - 1;
  return -1;
}
int edge_where(const TopoDS_Shape& shape, const std::function<bool(const BRepAdaptor_Curve&)>& test) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i)
    if (test(BRepAdaptor_Curve(TopoDS::Edge(edges(i))))) return i - 1;
  return -1;
}
}  // namespace

// A rod written as B-splines (as STEP exporters do): its side has a radius, its rims are circles, its axis has a direction.
TEST(bspline_cylinder_radius) {
  Document d = Document::create();
  Scene scene;
  const TopoDS_Shape rod = BRepBuilderAPI_NurbsConvert(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 6.5, 40).Shape(), Standard_True).Shape();
  const std::string id = add_body(d, scene, "rod", rod, Mat4::translation(10, 20, 30));
  const int side = face_where(rod, [](const BRepAdaptor_Surface& s) {  // the side: its middle is half way up
    const gp_Pnt mid = s.Value((s.FirstUParameter() + s.LastUParameter()) / 2, (s.FirstVParameter() + s.LastVParameter()) / 2);
    return s.GetType() == GeomAbs_BSplineSurface && mid.Z() > 1 && mid.Z() < 39;
  });
  const int rim = edge_where(rod, [](const BRepAdaptor_Curve& c) { return c.GetType() == GeomAbs_BSplineCurve && c.IsClosed(); });
  CHECK(side >= 0);
  CHECK(rim >= 0);
  const json face = inspect_ref(d, scene, ref(id, Ref::Kind::Face, side));
  CHECK_EQ(face["surface"].get<std::string>(), "bspline");
  CHECK_EQ(face["recognized"].get<std::string>(), "cylinder");
  CHECK_NEAR(face["radius"].get<double>(), 6.5, 1e-6);
  CHECK(face["deviation"].get<double>() < 1e-4);
  const json r = measure_radius(d, scene, ref(id, Ref::Kind::Face, side));
  CHECK_NEAR(r["value"].get<double>(), 6.5, 1e-6);
  CHECK_NEAR(r["diameter"].get<double>(), 13, 1e-6);
  CHECK_EQ(r["recognized"].get<std::string>(), "cylinder");
  // The centre end lies on the placed axis (x = 10, y = 20), the rim end on the surface.
  CHECK_NEAR(r["point_a"][0].get<double>(), 10, 1e-6);
  CHECK_NEAR(r["point_a"][1].get<double>(), 20, 1e-6);
  CHECK_NEAR(std::hypot(r["point_b"][0].get<double>() - 10, r["point_b"][1].get<double>() - 20), 6.5, 1e-5);
  const json e = measure_radius(d, scene, ref(id, Ref::Kind::Edge, rim));
  CHECK_NEAR(e["value"].get<double>(), 6.5, 1e-6);
  CHECK_EQ(e["recognized"].get<std::string>(), "circle");
  CHECK_NEAR(e["point_a"][0].get<double>(), 10, 1e-6);
  CHECK_NEAR(e["point_a"][1].get<double>(), 20, 1e-6);
  // The angle tool takes the recognised axis: parallel to a line along Z, as the rod stands.
  const TopoDS_Shape line = BRepBuilderAPI_MakeEdge(gp_Pnt(50, 0, 0), gp_Pnt(50, 0, 10)).Shape();
  const std::string other = add_body(d, scene, "line", line);
  const json angle = measure_angle(d, scene, ref(id, Ref::Kind::Face, side), ref(other, Ref::Kind::Edge, 0));
  CHECK_NEAR(angle["value"].get<double>(), 0, 1e-6);
  CHECK_EQ(angle["meaning_a"].get<std::string>(), "axis");
}

// A free-form face that is no cylinder still has no radius, and the error says what it is.
TEST(bspline_box_face_has_no_radius) {
  Document d = Document::create();
  Scene scene;
  const TopoDS_Shape box = BRepBuilderAPI_NurbsConvert(BRepPrimAPI_MakeBox(10, 20, 30).Shape(), Standard_True).Shape();
  const std::string id = add_body(d, scene, "box", box);
  const json face = inspect_ref(d, scene, ref(id, Ref::Kind::Face, 0));
  CHECK_EQ(face["recognized"].get<std::string>(), "plane");  // a plane: its normal is known (angles, sections)
  CHECK(face.contains("normal"));
  CHECK(!face.contains("radius"));
  bool threw = false;
  try {
    measure_radius(d, scene, ref(id, Ref::Kind::Face, 0));
  } catch (const std::exception& error) {
    threw = std::string(error.what()).find("not a bspline face") != std::string::npos;
  }
  CHECK(threw);
}

// Centre to centre: two holes' axes, a box's volume centroid, a circle's centre.
TEST(center_distance) {
  Document d = Document::create();
  Scene scene;
  const TopoDS_Shape small = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 3, 10).Shape();
  const TopoDS_Shape large = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(25, 0, 0), gp_Dir(0, 0, 1)), 5, 10).Shape();
  const std::string a = add_body(d, scene, "small", small), b = add_body(d, scene, "large", large);
  const int sa = face_where(small, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Cylinder; });
  const int sb = face_where(large, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Cylinder; });
  json r = measure_center_distance(d, scene, ref(a, Ref::Kind::Face, sa), ref(b, Ref::Kind::Face, sb));
  CHECK_EQ(r["mode"].get<std::string>(), "center");
  CHECK_NEAR(r["value"].get<double>(), 25, 1e-6);
  CHECK_NEAR(r["delta"][0].get<double>(), 25, 1e-6);
  CHECK_EQ(r["centre_a"].get<std::string>(), "axis");
  // The plain minimum between the same faces is the gap between the walls.
  CHECK_NEAR(measure_distance(d, scene, ref(a, Ref::Kind::Face, sa), ref(b, Ref::Kind::Face, sb))["value"].get<double>(), 17, 1e-6);
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(40, 0, 0), 10, 20, 30).Shape();
  const std::string c = add_body(d, scene, "box", box);
  const int rim = edge_where(large, [](const BRepAdaptor_Curve& e) { return e.GetType() == GeomAbs_Circle; });
  r = measure_center_distance(d, scene, ref(c, Ref::Kind::Body, 0), ref(b, Ref::Kind::Edge, rim));
  CHECK_EQ(r["centre_a"].get<std::string>(), "volume centroid");
  CHECK_EQ(r["centre_b"].get<std::string>(), "circle centre");
  CHECK_NEAR(r["point_a"][0].get<double>(), 45, 1e-6);
  CHECK_NEAR(r["point_a"][1].get<double>(), 10, 1e-6);
  CHECK_NEAR(r["point_a"][2].get<double>(), 15, 1e-6);
  CHECK_NEAR(r["point_b"][0].get<double>(), 25, 1e-6);
}

// The largest distance: exact between flat faces (opposite corners), within its tolerance on a sphere.
TEST(max_distance) {
  Document d = Document::create();
  Scene scene;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 20, 30).Shape();
  const std::string id = add_body(d, scene, "box", box);
  const int left = face_where(box, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Plane && std::abs(s.Value(s.FirstUParameter(), s.FirstVParameter()).X()) < 1e-9 && std::abs(s.Plane().Axis().Direction().X()) > 0.5; });
  const int right = face_where(box, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Plane && std::abs(s.Value(s.FirstUParameter(), s.FirstVParameter()).X() - 10) < 1e-9 && std::abs(s.Plane().Axis().Direction().X()) > 0.5; });
  CHECK(left >= 0 && right >= 0);
  json r = measure_max_distance(d, scene, ref(id, Ref::Kind::Face, left), ref(id, Ref::Kind::Face, right));
  CHECK_EQ(r["mode"].get<std::string>(), "max");
  CHECK_NEAR(r["value"].get<double>(), std::sqrt(100.0 + 400 + 900), 1e-9);
  CHECK(!r.contains("approximate"));
  const TopoDS_Shape ball = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 100), 5).Shape();
  const std::string sphere = add_body(d, scene, "ball", ball);
  Ref point;
  point.kind = Ref::Kind::Point;
  point.point = {20, 0, 100};
  r = measure_max_distance(d, scene, ref(sphere, Ref::Kind::Body, 0), point);
  CHECK(r.value("approximate", false));
  CHECK_NEAR(r["value"].get<double>(), 25, r["tolerance_mm"].get<double>() + 1e-3);
  CHECK(r["value"].get<double>() <= 25 + 1e-9);
  CHECK_NEAR(r["point_a"][0].get<double>(), -5, 0.05);
}

// An edge's length and the loops it closes; a face's area and perimeters (seams left out); a body's area and volume.
TEST(length_and_area) {
  Document d = Document::create();
  Scene scene;
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 20, 30).Shape();
  const std::string id = add_body(d, scene, "box", box);
  const int upright = edge_where(box, [](const BRepAdaptor_Curve& c) { return c.GetType() == GeomAbs_Line && std::abs(c.Line().Direction().Z()) > 0.5; });
  json r = measure_length(d, scene, ref(id, Ref::Kind::Edge, upright));
  CHECK_EQ(r["kind"].get<std::string>(), "length");
  CHECK_NEAR(r["value"].get<double>(), 30, 1e-9);
  CHECK_EQ(r["loops"].size(), 2u);
  double perimeters = 0;
  for (const auto& loop : r["loops"]) {
    perimeters += loop["perimeter"].get<double>();
    CHECK_EQ(loop["edges"].get<int>(), 4);
    CHECK(loop["outer"].get<bool>());
  }
  CHECK_NEAR(perimeters, 80 + 100, 1e-9);  // the 10 x 30 and the 20 x 30 side
  const int bottom = face_where(box, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Plane && std::abs(s.Plane().Axis().Direction().Z()) > 0.5; });
  r = measure_length(d, scene, ref(id, Ref::Kind::Face, bottom));
  CHECK_EQ(r["kind"].get<std::string>(), "area");
  CHECK_NEAR(r["value"].get<double>(), 200, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 60, 1e-9);
  CHECK_NEAR(r["outer_perimeter"].get<double>(), 60, 1e-9);
  CHECK_EQ(r["loops"].get<int>(), 1);
  r = measure_length(d, scene, ref(id, Ref::Kind::Body, 0));
  CHECK_NEAR(r["value"].get<double>(), 2 * (200 + 300 + 600), 1e-6);
  CHECK_NEAR(r["volume"].get<double>(), 6000, 1e-6);
  const TopoDS_Shape can = BRepPrimAPI_MakeCylinder(5, 10).Shape();
  const std::string cylinder = add_body(d, scene, "can", can);
  const int side = face_where(can, [](const BRepAdaptor_Surface& s) { return s.GetType() == GeomAbs_Cylinder; });
  r = measure_length(d, scene, ref(cylinder, Ref::Kind::Face, side));
  CHECK_NEAR(r["value"].get<double>(), 2 * M_PI * 5 * 10, 1e-6);
  CHECK_NEAR(r["perimeter"].get<double>(), 2 * 2 * M_PI * 5, 1e-6);  // both rims, not the seam
  const int rim = edge_where(can, [](const BRepAdaptor_Curve& c) { return c.GetType() == GeomAbs_Circle; });
  r = measure_length(d, scene, ref(cylinder, Ref::Kind::Edge, rim));
  CHECK_NEAR(r["value"].get<double>(), 2 * M_PI * 5, 1e-6);
  CHECK(r["closed"].get<bool>());
  CHECK_EQ(r["loops"].size(), 2u);  // the cap's (the rim alone) and the side's (both rims)
  CHECK_THROWS(measure_length(d, scene, ref(cylinder, Ref::Kind::Vertex, 0)));
}

int main(int argc, char** argv) { return check::run_all(argc, argv); }
