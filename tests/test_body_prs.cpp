#include "BodyShape.hpp"
#include "check.hpp"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Circ.hxx>

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
