#include "check.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_ConicalSurface.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <cstdio>
#include <map>
#include <vector>
#include <iostream>

namespace {
std::string geometry_snapshot(const TopoDS_Shape& shape) {
  // OCCT's stock mesher changes Checked flags as meshes are built/reused. These
  // cache flags are not geometry and must not obscure a real B-rep difference.
  TopTools_IndexedMapOfShape shapes;TopExp::MapShapes(shape,shapes);
  std::vector<bool> checked;
  for(int i=1;i<=shapes.Extent();++i) {auto s=shapes(i);checked.push_back(s.Checked());s.Checked(false);}
  auto result=opad::brep_from_shape(shape);
  for(int i=1;i<=shapes.Extent();++i) {auto s=shapes(i);s.Checked(checked[i-1]);}
  return result;
}
double area(const Handle(Poly_Triangulation)& mesh) {
  if (mesh.IsNull()) return 0;
  double result = 0;
  for (int i = 1; i <= mesh->NbTriangles(); ++i) {
    int a,b,c; mesh->Triangle(i).Get(a,b,c);
    result += gp_Vec(mesh->Node(a),mesh->Node(b)).Crossed(gp_Vec(mesh->Node(a),mesh->Node(c))).Magnitude()/2;
  }
  return result;
}
void validate(const TopoDS_Face& face) {
  TopLoc_Location loc;
  const auto tri = BRep_Tool::Triangulation(face,loc);
  CHECK(!tri.IsNull()); CHECK(tri->HasUVNodes());
  GProp_GProps props; BRepGProp::SurfaceProperties(face,props);
  const double scale = loc.Transformation().ScaleFactor();
  CHECK(std::abs(area(tri)*scale*scale/std::abs(props.Mass())-1)<.01);
  auto surface = BRep_Tool::Surface(face,loc);
  double maxError=0;
  for (int i=1;i<=tri->NbNodes();++i) {
    auto uv=tri->UVNode(i);
    maxError=std::max(maxError,surface->Value(uv.X(),uv.Y()).Distance(tri->Node(i)));
  }
  double boundaryTolerance=BRep_Tool::Tolerance(face);
  TopTools_IndexedMapOfShape edges;TopExp::MapShapes(face,TopAbs_EDGE,edges);
  for (int i=1;i<=edges.Extent();++i) {
    const auto edge=TopoDS::Edge(edges(i));
    boundaryTolerance=std::max(boundaryTolerance,BRep_Tool::Tolerance(edge));
    if(BRep_Tool::Degenerated(edge)) continue;
    for(auto orientation:{TopAbs_FORWARD,TopAbs_REVERSED}) {
      auto poly=BRep_Tool::PolygonOnTriangulation(TopoDS::Edge(edge.Oriented(orientation)),tri,loc);
      CHECK(!poly.IsNull());
      for(int j=1;j<=poly->NbNodes();++j) {CHECK(poly->Node(j)>0); CHECK(poly->Node(j)<=tri->NbNodes());}
    }
  }
  std::cout<<"mesh area "<<area(tri)<<" exact "<<props.Mass()<<" UV error "<<maxError<<" boundary tolerance "<<boundaryTolerance<<'\n';
  CHECK(maxError<=boundaryTolerance*1.01+1e-6);
}
TopoDS_Face cone() {
  Handle(Geom_ConicalSurface) surface=new Geom_ConicalSurface(gp_Ax3(gp::Origin(),gp::DZ()),.3,20);
  return BRepBuilderAPI_MakeFace(surface,-M_PI,M_PI,-10,10,1e-7).Face();
}
void damaged_cone(bool placed) {
  auto face=cone();
  if(placed) {gp_Trsf tr;tr.SetRotation(gp_Ax1(gp::Origin(),gp::DX()),.7);tr.SetTranslationPart(gp_Vec(100,-30,17));face.Move(TopLoc_Location(tr));face.Reverse();}
  BRepMesh_IncrementalMesh initial(face,.05,false,.35,true);
  TopLoc_Location loc;auto tri=BRep_Tool::Triangulation(face,loc);CHECK(!tri.IsNull());
  tri->ResizeTriangles(1,true); // Simulate a successful mesher returning only a fragment.
  const auto before=geometry_snapshot(face);
  const auto identity=face.TShape();
  auto report=opad::mesh_shape(face,.05);
  CHECK_EQ(report.recovered_faces,1);CHECK_EQ(report.incomplete_cones,0);
  CHECK(face.TShape()==identity);CHECK_EQ(geometry_snapshot(face),before);
  validate(face);
  auto repeated=opad::mesh_shape(face,.05);
  CHECK_EQ(repeated.recovered_faces,0);validate(face);
}
}
TEST(incomplete_cone_recovers_without_changing_geometry) { damaged_cone(false); }
TEST(reversed_located_cone_preserves_mesh_and_boundaries) { damaged_cone(true); }
TEST(healthy_cone_keeps_existing_triangulation) {
  auto face=cone();BRepMesh_IncrementalMesh initial(face,.05,false,.35,true);
  TopLoc_Location loc;auto original=BRep_Tool::Triangulation(face,loc);
  auto report=opad::mesh_shape(face,.05);CHECK_EQ(report.recovered_faces,0);
  CHECK(BRep_Tool::Triangulation(face,loc)==original);validate(face);
}
TEST(recovery_preserves_adjacent_face_meshes) {
  auto solid=BRepPrimAPI_MakeCone(20,15,30).Shape();
  BRepMesh_IncrementalMesh initial(solid,.05,false,.35,true);
  TopTools_IndexedMapOfShape faces;TopExp::MapShapes(solid,TopAbs_FACE,faces);
  std::vector<Handle(Poly_Triangulation)> original;
  for(int i=1;i<=faces.Extent();++i) {
    auto face=TopoDS::Face(faces(i));TopLoc_Location loc;auto mesh=BRep_Tool::Triangulation(face,loc);
    original.push_back(mesh);
    if(BRepAdaptor_Surface(face).GetType()==GeomAbs_Cone) mesh->ResizeTriangles(1,true);
  }
  auto report=opad::mesh_shape(solid,.05);CHECK_EQ(report.recovered_faces,1);
  for(int i=1;i<=faces.Extent();++i) {
    auto face=TopoDS::Face(faces(i));TopLoc_Location loc;
    if(BRepAdaptor_Surface(face).GetType()!=GeomAbs_Cone) CHECK(BRep_Tool::Triangulation(face,loc)==original[i-1]);
    validate(face);
  }
}
// Extruded walls (TODO 10 A1): BRepMesh fills a curved wall with triangles that also span along its rims, so seen along
// the extrusion they cover slivers past the rims; straighten_ruled_faces makes them upright strips.
namespace {
// Area the non-planar faces cover when projected along `axis`: zero for upright walls.
double projected_wall_area(const TopoDS_Shape& shape, const gp_Dir& axis) {
  double total = 0;
  TopTools_IndexedMapOfShape faces; TopExp::MapShapes(shape, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    const TopoDS_Face face = TopoDS::Face(faces(i));
    if (BRepAdaptor_Surface(face).GetType() == GeomAbs_Plane) continue;
    TopLoc_Location loc; const auto tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    for (int k = 1; k <= tri->NbTriangles(); ++k) {
      int a, b, c; tri->Triangle(k).Get(a, b, c);
      const gp_Pnt p = tri->Node(a).Transformed(loc.Transformation()), q = tri->Node(b).Transformed(loc.Transformation()), r = tri->Node(c).Transformed(loc.Transformation());
      total += std::abs(gp_Vec(p, q).Crossed(gp_Vec(p, r)).Dot(gp_Vec(axis))) / 2;
    }
  }
  return total;
}
// Volume enclosed by the triangles, wound as the shaded presentation draws them (reversed faces flipped): checks that
// every face is covered and every triangle faces outwards.
double mesh_volume(const TopoDS_Shape& shape) {
  double volume = 0;
  for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
    const TopoDS_Face face = TopoDS::Face(faces.Current());
    TopLoc_Location loc; const auto tri = BRep_Tool::Triangulation(face, loc);
    CHECK(!tri.IsNull());
    for (int k = 1; k <= tri->NbTriangles(); ++k) {
      int a, b, c; tri->Triangle(k).Get(a, b, c);
      if (face.Orientation() == TopAbs_REVERSED) std::swap(b, c);
      const gp_XYZ p = tri->Node(a).Transformed(loc.Transformation()).XYZ(), q = tri->Node(b).Transformed(loc.Transformation()).XYZ(), r = tri->Node(c).Transformed(loc.Transformation()).XYZ();
      volume += p.Dot(q.Crossed(r)) / 6;
    }
  }
  return volume;
}
// Every edge keeps a polygon on each of its faces' triangulations, on that triangulation's boundary, and the polygons of
// one edge on its two faces are the same points: the shaded fill and the drawn edges meet without gaps.
void check_boundaries(const TopoDS_Shape& shape) {
  TopTools_IndexedDataMapOfShapeListOfShape map; TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    const TopoDS_Edge edge = TopoDS::Edge(map.FindKey(i));
    if (BRep_Tool::Degenerated(edge)) continue;
    std::vector<std::vector<gp_Pnt>> seen;
    for (const auto& f : map(i)) {
      const TopoDS_Face face = TopoDS::Face(f);
      TopLoc_Location loc; const auto tri = BRep_Tool::Triangulation(face, loc);
      const auto polygon = BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
      CHECK(!polygon.IsNull());
      std::map<std::pair<int, int>, int> uses;
      for (int k = 1; k <= tri->NbTriangles(); ++k) {
        int a, b, c; tri->Triangle(k).Get(a, b, c);
        for (auto [x, y] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) uses[{std::min(x, y), std::max(x, y)}]++;
      }
      std::vector<gp_Pnt> points;
      for (int k = 1; k <= polygon->NbNodes(); ++k) {
        points.push_back(tri->Node(polygon->Node(k)).Transformed(loc.Transformation()));
        if (k > 1) {
          const int a = polygon->Node(k - 1), b = polygon->Node(k);
          const int count = uses[{std::min(a, b), std::max(a, b)}];
          CHECK(count >= 1);
        }
      }
      seen.push_back(points);
    }
    for (size_t s = 1; s < seen.size(); ++s) {
      CHECK_EQ(seen[s].size(), seen[0].size());
      const bool same = seen[s].front().Distance(seen[0].front()) < 1e-9;
      for (size_t k = 0; k < seen[0].size(); ++k) CHECK(seen[s][same ? k : seen[0].size() - 1 - k].Distance(seen[0][k]) < 1e-9);
    }
  }
}
// A slot-like profile: two spline ends (a closed periodic spline is added separately) joined by straight sides.
TopoDS_Shape spline_slot(double height) {
  auto spline = [](std::vector<gp_Pnt> points) {
    Handle(TColgp_HArray1OfPnt) array = new TColgp_HArray1OfPnt(1, int(points.size()));
    for (size_t i = 0; i < points.size(); ++i) array->SetValue(int(i) + 1, points[i]);
    GeomAPI_Interpolate fit(array, Standard_False, 1e-9);
    fit.Perform();
    return BRepBuilderAPI_MakeEdge(fit.Curve()).Edge();
  };
  BRepBuilderAPI_MakeWire wire;
  wire.Add(spline({{0, 0, 0}, {-3, 2, 0}, {-3.5, 5, 0}, {-2, 8, 0}, {0, 10, 0}}));
  wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 10, 0), gp_Pnt(20, 10, 0)).Edge());
  wire.Add(spline({{20, 10, 0}, {23, 7, 0}, {22.5, 4, 0}, {21, 1, 0}, {20, 0, 0}}));
  wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(20, 0, 0), gp_Pnt(0, 0, 0)).Edge());
  return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire(), Standard_True).Face(), gp_Vec(0, 0, height)).Shape();
}
TopoDS_Shape periodic_spline_prism() {
  Handle(TColgp_HArray1OfPnt) points = new TColgp_HArray1OfPnt(1, 7);
  for (int i = 0; i < 7; ++i) {
    const double t = 2 * M_PI * i / 7, r = 10 + 3 * std::cos(3 * t);
    points->SetValue(i + 1, gp_Pnt(r * std::cos(t), r * std::sin(t), 0));
  }
  GeomAPI_Interpolate fit(points, Standard_True, 1e-9);
  fit.Perform();
  const TopoDS_Wire wire = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(fit.Curve()).Edge()).Wire();
  return BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire, Standard_True).Face(), gp_Vec(0, 0, 6)).Shape();
}
double exact_volume(const TopoDS_Shape& shape) {
  GProp_GProps props;
  BRepGProp::VolumeProperties(shape, props);
  return props.Mass();
}
}  // namespace

TEST(extruded_walls_become_upright_strips) {
  const std::vector<std::pair<const char*, TopoDS_Shape>> shapes = {
      {"spline slot", spline_slot(10)},
      {"periodic spline", periodic_spline_prism()},
      {"cylinder", BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(1, 2, 3), gp::DZ()), 7, 12).Shape()},
      {"tilted cylinder", BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(1, 1, 1)), 3, 20).Shape()}};
  for (const auto& [name, shape] : shapes) {
    const gp_Dir axis = std::string(name) == "tilted cylinder" ? gp_Dir(1, 1, 1) : gp::DZ();
    const std::string before = geometry_snapshot(shape);
    opad::mesh_shape(shape, 0.05);
    const double leaning = projected_wall_area(shape, axis), volume = mesh_volume(shape);
    const int changed = opad::straighten_ruled_faces(shape);
    const double upright = projected_wall_area(shape, axis), after = mesh_volume(shape), exact = exact_volume(shape);
    std::printf("%s: %d faces straightened, wall area along the axis %.6f -> %.9f mm2, mesh volume %.4f -> %.4f (exact %.4f)\n",
                name, changed, leaning, upright, volume, after, exact);
    CHECK(changed >= 1);
    CHECK(upright < 1e-9);
    CHECK(std::abs(after - exact) < exact * 0.01);  // complete and wound outwards
    CHECK(after * volume > 0);                        // same winding as BRepMesh chose
    check_boundaries(shape);
    CHECK(geometry_snapshot(shape) == before);        // the B-rep itself is untouched
  }
  // The case from the report leans before: without straightening the test above would pass vacuously.
  const auto slot = spline_slot(10);
  opad::mesh_shape(slot, 0.05);
  CHECK(projected_wall_area(slot, gp::DZ()) > 1e-6);
}

TEST(straightening_keeps_faces_it_cannot_match) {
  // A cone is ruled but not along one direction; a sphere is not ruled: both keep BRepMesh's triangles.
  TopoDS_Shape cone = BRepPrimAPI_MakeCone(4, 1, 6).Shape(), sphere = BRepPrimAPI_MakeSphere(5).Shape();
  for (auto& shape : {cone, sphere}) {
    opad::mesh_shape(shape, 0.05);
    CHECK_EQ(opad::straighten_ruled_faces(shape), 0);
  }
}

int main(int argc,char**argv) {
  // Optional real-model regression: extracted pump BREP stays outside the repo.
  if(argc==3 && std::string(argv[1])=="--water-pump") {
    try {
      TopoDS_Shape shape;BRep_Builder builder;CHECK(BRepTools::Read(shape,argv[2],builder));
      auto before=geometry_snapshot(shape);
      auto report=opad::mesh_shape(shape,.601212);
      CHECK_EQ(report.recovered_faces,2);CHECK_EQ(report.incomplete_cones,0);
      TopTools_IndexedMapOfShape faces;TopExp::MapShapes(shape,TopAbs_FACE,faces);
      CHECK_EQ(faces.Extent(),1779);
      for(int index:{829,1751}) {auto face=TopoDS::Face(faces(index));validate(face);std::cout<<"recovered face "<<index<<" PASS\n";}
      CHECK_EQ(geometry_snapshot(shape),before);
      // OCCT may remesh a face when a shared boundary is refreshed. A second
      // tessellation must still return complete surfaces.
      CHECK_EQ(opad::mesh_shape(shape,.601212).incomplete_cones,0);
      for(int index:{829,1751}) validate(TopoDS::Face(faces(index)));
      std::cout<<"water pump: topology, geometry, surface coverage, UVs and boundaries PASS\n";
      return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
  }
  return check::run_all(argc,argv);
}

