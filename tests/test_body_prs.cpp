#include "BodyShape.hpp"
#include "check.hpp"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Circ.hxx>
#include <Poly_Triangulation.hxx>
#include <TopoDS_Face.hxx>
#include <SelectMgr_SelectingVolumeManager.hxx>
#include <Select3D_SensitivePrimitiveArray.hxx>
#include <Graphic3d_Camera.hxx>

namespace {
class CountingVolume : public SelectMgr_SelectingVolumeManager {
 public:
  mutable int triangles = 0;
  Standard_Boolean OverlapsTriangle(const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c,
                                    Standard_Integer sensitivity, SelectBasics_PickResult& result) const override {
    ++triangles;
    return SelectMgr_SelectingVolumeManager::OverlapsTriangle(a, b, c, sensitivity, result);
  }
};
}

TEST(navigation_area_query_stops_after_first_triangle_hit) {
  constexpr int side = 64;
  Handle(Poly_Triangulation) mesh = new Poly_Triangulation((side+1)*(side+1), side*side*2, false);
  for (int y=0; y<=side; ++y) for (int x=0; x<=side; ++x)
    mesh->SetNode(y*(side+1)+x+1, gp_Pnt(x, y, 0));
  int triangle = 1;
  for (int y=0; y<side; ++y) for (int x=0; x<side; ++x) {
    const int a = y*(side+1)+x+1, b = a+1, c = a+side+1, d = c+1;
    mesh->SetTriangle(triangle++, Poly_Triangle(a,b,c));
    mesh->SetTriangle(triangle++, Poly_Triangle(b,d,c));
  }
  TopoDS_Face face; BRep_Builder().MakeFace(face, mesh);
  auto prs = BodyPrs::build(face, Bnd_Box());
  Handle(Graphic3d_Camera) camera = new Graphic3d_Camera();
  camera->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
  camera->SetEyeAndCenter(gp_Pnt(32,32,100), gp_Pnt(32,32,0));
  camera->SetUp(gp::DY()); camera->SetScale(100);
  CountingVolume volume;
  volume.InitBoxSelectingVolume(gp_Pnt2d(350,350), gp_Pnt2d(650,650));
  volume.SetCamera(camera); volume.SetWindowSize(1000,1000);
  volume.AllowOverlapDetection(true); volume.BuildSelectingVolume();
  SelectBasics_PickResult result;
  CHECK(prs->navigation->Matches(volume, result));
  CHECK(volume.triangles <= 4);
  // Compare with ordinary area selection over the exact same mesh and rectangle.
  Handle(Select3D_SensitivePrimitiveArray) ordinary = new Select3D_SensitivePrimitiveArray(nullptr);
  CHECK(ordinary->InitTriangulation(prs->triangles->Attributes(), prs->triangles->Indices(), TopLoc_Location()));
  ordinary->BVH(); volume.triangles = 0;
  SelectBasics_PickResult all;
  CHECK(ordinary->Matches(volume, all));
  CHECK(volume.triangles > 64);
}

TEST(split_circle_highlights_all_arcs) {
  gp_Circ circle(gp_Ax2(gp_Pnt(0,0,0),gp::DZ()),10);
  BRep_Builder b; TopoDS_Compound c; b.MakeCompound(c);
  b.Add(c,BRepBuilderAPI_MakeEdge(circle,0,M_PI).Edge());
  b.Add(c,BRepBuilderAPI_MakeEdge(circle,M_PI,2*M_PI).Edge());
  auto p=BodyPrs::build(c,Bnd_Box());
  CHECK_EQ(p->circles.size(),2u);
  for(const auto& [id,arc]:p->circles) { CHECK_EQ(arc.canonical,0); CHECK_EQ(arc.edge.ShapeType(),TopAbs_COMPOUND); }
}
TEST(overlapping_arcs_do_not_make_a_circle) {
  gp_Circ circle(gp_Ax2(gp_Pnt(0,0,0),gp::DZ()),10);
  BRep_Builder b; TopoDS_Compound c; b.MakeCompound(c);
  b.Add(c,BRepBuilderAPI_MakeEdge(circle,0,M_PI).Edge());
  b.Add(c,BRepBuilderAPI_MakeEdge(circle,M_PI/2,1.5*M_PI).Edge());
  auto p=BodyPrs::build(c,Bnd_Box());
  for(const auto& [id,arc]:p->circles) CHECK_EQ(arc.canonical,-1);
}
CHECK_MAIN()
