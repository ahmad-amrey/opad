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
