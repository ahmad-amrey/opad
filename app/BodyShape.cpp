#include "BodyShape.hpp"

#include <AIS_DisplayMode.hxx>
#include <BRep_Tool.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_Presentation.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <StdPrs_ShadedShape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

std::shared_ptr<BodyPrs> BodyPrs::build(const TopoDS_Shape& meshedProto, const Bnd_Box& box) {
  auto p = std::make_shared<BodyPrs>();
  p->box = box;
  p->triangles = StdPrs_ShadedShape::FillTriangles(meshedProto);
  p->boundaries = StdPrs_ShadedShape::FillFaceBoundaries(meshedProto);
  p->closed = true;
  for (TopExp_Explorer e(meshedProto, TopAbs_SHELL); e.More(); e.Next())
    if (!BRep_Tool::IsClosed(e.Current())) { p->closed = false; break; }
  if (meshedProto.ShapeType() > TopAbs_SHELL) p->closed = false;  // a bare face or lower
  return p;
}

void BodyShape::Compute(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Presentation)& prs, const Standard_Integer mode) {
  if (mode != AIS_Shaded || !m_prs || m_prs->triangles.IsNull()) {
    AIS_Shape::Compute(mgr, prs, mode);  // wireframe/HLR, or nothing precomputed: the stock path
    return;
  }
  // Min/max are supplied from the worker's box; evaluating them here walks every vertex on the UI thread.
  const bool haveBox = !m_prs->box.IsVoid();
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  if (haveBox) m_prs->box.Get(x0, y0, z0, x1, y1, z1);
  Handle(Graphic3d_Group) g = prs->NewGroup();
  g->SetClosed(m_prs->closed);
  g->SetGroupPrimitivesAspect(myDrawer->ShadingAspect()->Aspect());
  g->AddPrimitiveArray(m_prs->triangles, !haveBox);
  if (haveBox) g->SetMinMaxValues(x0, y0, z0, x1, y1, z1);
  if (myDrawer->FaceBoundaryDraw() && !m_prs->boundaries.IsNull()) {
    Handle(Graphic3d_Group) e = prs->NewGroup();
    e->SetGroupPrimitivesAspect(myDrawer->FaceBoundaryAspect()->Aspect());
    e->AddPrimitiveArray(m_prs->boundaries, !haveBox);
    if (haveBox) e->SetMinMaxValues(x0, y0, z0, x1, y1, z1);
  }
}
