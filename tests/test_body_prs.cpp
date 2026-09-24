#include "BodyShape.hpp"
#include "DepthBias.hpp"
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
#include <SelectMgr_Selection.hxx>
#include <SelectMgr_SensitiveEntity.hxx>
#include <BRep_Tool.hxx>

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

TEST(mesh_selection_has_independent_facet_edge_and_vertex_owners) {
  struct TestBody : BodyShape { using BodyShape::BodyShape; using BodyShape::ComputeSelection; };
  Handle(Poly_Triangulation) mesh=new Poly_Triangulation(4,2,false);
  mesh->SetNode(1,gp_Pnt(0,0,0)); mesh->SetNode(2,gp_Pnt(10,0,0));
  mesh->SetNode(3,gp_Pnt(0,10,0)); mesh->SetNode(4,gp_Pnt(10,10,0));
  mesh->SetTriangle(1,Poly_Triangle(1,2,3)); mesh->SetTriangle(2,Poly_Triangle(2,4,3));
  TopoDS_Face face; BRep_Builder().MakeFace(face,mesh);
  Handle(TestBody) body=new TestBody(face,BodyPrs::build(face,Bnd_Box()));
  Handle(Graphic3d_Camera) camera=new Graphic3d_Camera();
  camera->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
  camera->SetEyeAndCenter(gp_Pnt(5,5,100),gp_Pnt(5,5,0)); camera->SetUp(gp::DY()); camera->SetScale(20);
  for(const auto [type,count]:{std::pair{TopAbs_FACE,2},std::pair{TopAbs_EDGE,6},std::pair{TopAbs_VERTEX,4}}) {
    Handle(SelectMgr_Selection) selection=new SelectMgr_Selection(AIS_Shape::SelectionMode(type));
    body->ComputeSelection(selection,AIS_Shape::SelectionMode(type));
    CHECK_EQ(selection->Entities().Size(),count);
    SelectMgr_SelectingVolumeManager volume;
    volume.InitBoxSelectingVolume(gp_Pnt2d(0,0),gp_Pnt2d(1000,1000));
    volume.SetCamera(camera); volume.SetWindowSize(1000,1000); volume.AllowOverlapDetection(true); volume.BuildSelectingVolume();
    std::set<int> owners;
    for(const auto& entity:selection->Entities()) {
      SelectBasics_PickResult result;
      CHECK(entity->BaseSensitive()->Matches(volume,result));
      auto owner=Handle(SubShapeOwner)::DownCast(entity->BaseSensitive()->OwnerId());
      CHECK(!owner.IsNull() && owner->HasShape());
      CHECK_EQ(owner->Shape().ShapeType(),type); owners.insert(owner->index());
    }
    CHECK_EQ(owners.size(),size_t(count));
  }
}

TEST(mesh_circle_rim_retains_vertex_and_center_targets) {
  struct TestBody : BodyShape { using BodyShape::BodyShape; using BodyShape::ComputeSelection; };
  constexpr int n=32;
  Handle(Poly_Triangulation) mesh=new Poly_Triangulation(n+1,n,false);
  mesh->SetNode(1,gp_Pnt(0,0,0));
  for(int i=0;i<n;++i) {
    const double angle=2*M_PI*i/n;
    mesh->SetNode(i+2,gp_Pnt(10*std::cos(angle),10*std::sin(angle),0));
    mesh->SetTriangle(i+1,Poly_Triangle(1,i+2,(i+1)%n+2));
  }
  TopoDS_Face face;BRep_Builder().MakeFace(face,mesh);
  auto prs=BodyPrs::build(face,Bnd_Box());CHECK_EQ(prs->circles.size(),1u);
  Handle(TestBody) body=new TestBody(face,prs);
  Handle(SelectMgr_Selection) selection=new SelectMgr_Selection(AIS_Shape::SelectionMode(TopAbs_VERTEX));
  body->ComputeSelection(selection,AIS_Shape::SelectionMode(TopAbs_VERTEX));
  Handle(Graphic3d_Camera) camera=new Graphic3d_Camera();
  camera->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
  camera->SetEyeAndCenter(gp_Pnt(0,0,100),gp_Pnt(0,0,0));camera->SetUp(gp::DY());camera->SetScale(40);
  SelectMgr_SelectingVolumeManager volume;volume.InitPointSelectingVolume(gp_Pnt2d(750,500));
  volume.SetCamera(camera);volume.SetWindowSize(1000,1000);volume.SetPixelTolerance(2);volume.BuildSelectingVolume();
  int centers=0,vertices=0;
  for(const auto& entity:selection->Entities()) {
    SelectBasics_PickResult result;if(!entity->BaseSensitive()->Matches(volume,result)) continue;
    auto owner=Handle(CircleOwner)::DownCast(entity->BaseSensitive()->OwnerId());
    if(!owner.IsNull()) {CHECK_NEAR(owner->center.Distance(gp::Origin()),0,1e-7);++centers;}
    else {auto vertex=Handle(SubShapeOwner)::DownCast(entity->BaseSensitive()->OwnerId());CHECK(!vertex.IsNull());CHECK_EQ(vertex->Shape().ShapeType(),TopAbs_VERTEX);++vertices;}
  }
  CHECK(centers>0);CHECK(vertices>0);
}

TEST(coincident_parts_have_distinct_depth_slots) {
  Bnd_Box a(gp_Pnt(0,0,0), gp_Pnt(10,10,10));
  Bnd_Box touching(gp_Pnt(10,0,0), gp_Pnt(20,10,10));
  Bnd_Box separate(gp_Pnt(30,0,0), gp_Pnt(40,10,10));
  std::vector<Bnd_Box> boxes(100, a);
  boxes.push_back(touching); boxes.push_back(separate);
  const auto ranks = depthSlots(boxes);
  std::set<int> distinct(ranks.begin(), ranks.begin()+101);
  CHECK_EQ(distinct.size(), 101u);
  CHECK_EQ(ranks.back(), 0);
  CHECK(ranks == depthSlots(boxes));
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
