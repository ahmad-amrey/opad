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
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <array>
#include <cmath>
#include <set>

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
      CHECK(!owner.IsNull());
      CHECK(!owner->HasShape()); // A box candidate must not construct analytic geometry.
      owner->prepare();
      CHECK(owner->HasShape());
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
    else {auto vertex=Handle(SubShapeOwner)::DownCast(entity->BaseSensitive()->OwnerId());CHECK(!vertex.IsNull());vertex->prepare();CHECK_EQ(vertex->Shape().ShapeType(),TopAbs_VERTEX);++vertices;}
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
TEST(wire_display_and_selection_share_smooth_samples) {
  const auto edge=BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp::Origin(),gp::DZ()),100)).Edge();
  Bnd_Box bounds;bounds.Add(gp_Pnt(-100,-100,0));bounds.Add(gp_Pnt(100,100,0));
  const auto prs=BodyPrs::build(edge,bounds);
  CHECK_EQ(prs->curves.size(),1u);
  struct TestBody:BodyShape {using BodyShape::BodyShape;using BodyShape::ComputeSelection;};
  Handle(TestBody) body=new TestBody(edge,prs);
  Handle(SelectMgr_Selection) selection=new SelectMgr_Selection(AIS_Shape::SelectionMode(TopAbs_EDGE));
  body->ComputeSelection(selection,AIS_Shape::SelectionMode(TopAbs_EDGE));
  CHECK_EQ(selection->Entities().Size(),1);
  const auto owner=Handle(SubShapeOwner)::DownCast(selection->Entities().First()->BaseSensitive()->OwnerId());
  CHECK(!owner.IsNull());CHECK(owner->curve==prs->curves.at(0));
  const auto& points=*prs->curves.at(0);
  CHECK(points.size()>128);
  CHECK_EQ(prs->boundaries->VertexNumber(),int(points.size()-1)*2);
  for(size_t i=1;i<points.size();++i) {
    const gp_Pnt midpoint((points[i-1].XYZ()+points[i].XYZ())*.5);
    CHECK(100-midpoint.Distance(gp::Origin())<.003);
    CHECK(prs->boundaries->Vertice(int(i)*2).Distance(points[i])<1e-4);
  }
}

CHECK_MAIN()

// TODO 10 A1/A13, the display arrays themselves: an extruded spline profile, meshed the way every view path meshes it.
// Its walls are exactly upright (seen along the extrusion they cover nothing past the profile) and every edge line
// drawn over the fill is an edge of the fill's triangles (the outline and the fill meet, no slivers between them).
TEST(extruded_profile_display_arrays_follow_the_profile) {
  Handle(TColgp_HArray1OfPnt) points = new TColgp_HArray1OfPnt(1, 5);
  const gp_Pnt at[] = {{0, 0, 0}, {-3, 2, 0}, {-3.5, 5, 0}, {-2, 8, 0}, {0, 10, 0}};
  for (int i = 0; i < 5; ++i) points->SetValue(i + 1, at[i]);
  GeomAPI_Interpolate fit(points, Standard_False, 1e-9);
  fit.Perform();
  BRepBuilderAPI_MakeWire wire;
  wire.Add(BRepBuilderAPI_MakeEdge(fit.Curve()).Edge());
  wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(0, 10, 0), gp_Pnt(20, 10, 0)).Edge());
  wire.Add(BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(20, 5, 0), gp::DZ()), 5), gp_Pnt(20, 10, 0), gp_Pnt(20, 0, 0)).Edge());
  wire.Add(BRepBuilderAPI_MakeEdge(gp_Pnt(20, 0, 0), gp_Pnt(0, 0, 0)).Edge());
  const TopoDS_Shape body = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire.Wire(), Standard_True).Face(), gp_Vec(0, 0, 10)).Shape();
  BodyPrs::meshForDisplay(body, 0.05);
  Bnd_Box box;
  BRepBndLib::Add(body, box);
  const auto prs = BodyPrs::build(body, box);
  CHECK(!prs->triangles.IsNull() && !prs->boundaries.IsNull());
  const auto& t = prs->triangles;
  auto vertex = [&](int i) { return t->Vertice(t->EdgeNumber() > 0 ? t->Edge(i) : i); };
  const int count = t->EdgeNumber() > 0 ? t->EdgeNumber() : t->VertexNumber();
  auto key = [](const gp_Pnt& p) { return std::array<long long, 3>{std::llround(p.X() * 1e6), std::llround(p.Y() * 1e6), std::llround(p.Z() * 1e6)}; };
  std::set<std::array<long long, 6>> edges;
  auto edgeKey = [&](const gp_Pnt& a, const gp_Pnt& b) {
    auto ka = key(a), kb = key(b);
    if (kb < ka) std::swap(ka, kb);
    return std::array<long long, 6>{ka[0], ka[1], ka[2], kb[0], kb[1], kb[2]};
  };
  int walls = 0;
  for (int i = 1; i + 2 <= count; i += 3) {
    const gp_Pnt a = vertex(i), b = vertex(i + 1), c = vertex(i + 2);
    edges.insert(edgeKey(a, b));
    edges.insert(edgeKey(b, c));
    edges.insert(edgeKey(c, a));
    const gp_Vec n = gp_Vec(a, b).Crossed(gp_Vec(a, c));
    if (n.Magnitude() < 1e-12 || std::abs(n.Z()) > 0.999 * n.Magnitude()) continue;  // the top and bottom
    ++walls;
    CHECK(std::abs(n.Z()) / 2 < 1e-9);  // area seen from +Z
  }
  CHECK(walls > 20);
  const auto& lines = prs->boundaries;
  int missing = 0;
  auto line = [&](int i) { return lines->Vertice(lines->EdgeNumber() > 0 ? lines->Edge(i) : i); };  // indexed polylines
  const int ends = lines->EdgeNumber() > 0 ? lines->EdgeNumber() : lines->VertexNumber();
  for (int i = 1; i + 1 <= ends; i += 2)
    if (!edges.count(edgeKey(line(i), line(i + 1)))) {
      if (++missing <= 3) {
        const gp_Pnt a = line(i), b = line(i + 1);
        double best = 1e300;
        for (int k = 1; k <= count; ++k) best = std::min(best, vertex(k).Distance(a));
        std::printf("segment (%.9f %.9f %.9f)-(%.9f %.9f %.9f) not a triangle edge; nearest triangle vertex %.3g away\n", a.X(), a.Y(), a.Z(), b.X(), b.Y(), b.Z(), best);
      }
    }
  std::printf("%d of %d edge segments are not triangle edges\n", missing, lines->VertexNumber() / 2);
  CHECK_EQ(missing, 0);
}
