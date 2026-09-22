#include "BodyShape.hpp"

#include <AIS_DisplayMode.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <cmath>
#include <algorithm>
#include <BRepAdaptor_Curve.hxx>
#include <Select3D_SensitiveCurve.hxx>
#include <Select3D_SensitivePrimitiveArray.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_AspectMarker3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_Presentation.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Select3D_SensitiveEntity.hxx>
#include <SelectMgr_Selection.hxx>
#include <SelectMgr_SensitiveEntity.hxx>
#include <StdPrs_ShadedShape.hxx>
#include <StdSelect_Shape.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

// Hover is OCCT's (one immediate structure). The selected state is drawn by the viewport instead: the stock
// owner builds one presentation per selected sub-shape, which takes seconds to minutes for a rubber band over
// thousands of them, and it draws in the parent's layer, so it never showed through what is in front.
void SubShapeOwner::HilightWithColor(const Handle(PrsMgr_PresentationManager)& pm, const Handle(Prs3d_Drawer)& style, const Standard_Integer mode) {
  if (pm->IsImmediateModeOn()) StdSelect_BRepOwner::HilightWithColor(pm, style, mode);
}

void SubShapeOwner::Unhilight(const Handle(PrsMgr_PresentationManager)& pm, const Standard_Integer mode) {
  if (!myPrsSh.IsNull()) StdSelect_BRepOwner::Unhilight(pm, mode);  // with no presentation the base un-highlights the whole body
}

void SubHighlight::Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) {
  if (!m_triangles.empty()) {
    Handle(Graphic3d_AspectFillArea3d) fill = new Graphic3d_AspectFillArea3d();
    fill->SetInteriorStyle(Aspect_IS_SOLID);
    fill->SetInteriorColor(Quantity_ColorRGBA(m_color, 0.65f));
    fill->SetAlphaMode(Graphic3d_AlphaMode_Blend);
    fill->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);  // a flat tint: the arrays carry no normals
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(fill);
    for (const auto& a : m_triangles) g->AddPrimitiveArray(a);
  }
  if (!m_segments.empty()) {
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(m_color, Aspect_TOL_SOLID, 3.0));
    for (const auto& a : m_segments) g->AddPrimitiveArray(a);
  }
  if (!m_points.empty()) {
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_O_POINT, m_color, 2.0));
    for (const auto& a : m_points) g->AddPrimitiveArray(a);
  }
}

std::shared_ptr<BodyPrs> BodyPrs::build(const TopoDS_Shape& meshedProto, const Bnd_Box& box) {
  auto p = std::make_shared<BodyPrs>();
  p->box = box;
  p->triangles = StdPrs_ShadedShape::FillTriangles(meshedProto);
  p->boundaries = StdPrs_ShadedShape::FillFaceBoundaries(meshedProto);
  if (!p->triangles.IsNull()) {
    Handle(Select3D_SensitivePrimitiveArray) triangles = new Select3D_SensitivePrimitiveArray(nullptr);
    if (triangles->InitTriangulation(p->triangles->Attributes(), p->triangles->Indices(), TopLoc_Location())) {
      triangles->BVH();
      p->navigation = triangles;
    }
  }
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(meshedProto, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    if (BRep_Tool::Degenerated(TopoDS::Edge(edges(i)))) continue;
    BRepAdaptor_Curve curve(TopoDS::Edge(edges(i)));
    if (curve.GetType() != GeomAbs_Circle) continue;
    TColgp_Array1OfPnt points(1, 257);
    for (int j = 1; j <= points.Length(); ++j)
      points(j) = curve.Value(curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * (j - 1) / 256.0);
    Handle(Select3D_SensitiveCurve) sensitive = new Select3D_SensitiveCurve(nullptr, points);
    sensitive->BVH();
    p->circles.emplace(i - 1, Circle{edges(i), curve.Circle().Location(), sensitive});
  }
  // STEP often splits a closed circle at seam vertices. Group co-circular arcs only
  // when their angular intervals cover a complete revolution (overlaps don't count twice).
  std::map<std::array<long long, 7>, std::vector<int>> groups;
  for (const auto& [index, entry] : p->circles) {
    const gp_Circ c = BRepAdaptor_Curve(TopoDS::Edge(entry.edge)).Circle();
    gp_Dir axis = c.Axis().Direction();
    if (axis.Z() < 0 || (axis.Z() == 0 && (axis.Y() < 0 || (axis.Y() == 0 && axis.X() < 0)))) axis.Reverse();
    auto q = [](double x) { return std::llround(x * 1e6); };
    groups[{q(c.Location().X()), q(c.Location().Y()), q(c.Location().Z()), q(c.Radius()), q(axis.X()), q(axis.Y()), q(axis.Z())}].push_back(index);
  }
  for (const auto& [key, indices] : groups) {
    if (indices.size() < 2) continue;
    const gp_Circ base = BRepAdaptor_Curve(TopoDS::Edge(p->circles.at(indices.front()).edge)).Circle();
    std::vector<std::pair<double, double>> spans;
    for (int index : indices) {
      BRepAdaptor_Curve curve(TopoDS::Edge(p->circles.at(index).edge));
      const bool same = curve.Circle().Axis().Direction().Dot(base.Axis().Direction()) > 0;
      gp_Vec v(base.Location(), curve.Value(same ? curve.FirstParameter() : curve.LastParameter()));
      double start = std::atan2(v.Dot(gp_Vec(base.Position().YDirection())), v.Dot(gp_Vec(base.Position().XDirection())));
      if (start < 0) start += 2 * M_PI;
      const double end = start + std::min(2 * M_PI, std::abs(curve.LastParameter() - curve.FirstParameter()));
      spans.emplace_back(start, std::min(end, 2 * M_PI));
      if (end > 2 * M_PI) spans.emplace_back(0, end - 2 * M_PI);
    }
    std::sort(spans.begin(), spans.end());
    double reach = 0; bool closed = true;
    for (auto [first, last] : spans) { if (first > reach + 1e-7) { closed = false; break; } reach = std::max(reach, last); }
    if (!closed || reach < 2 * M_PI - 1e-7) continue;
    TopoDS_Compound whole; BRep_Builder builder; builder.MakeCompound(whole);
    for (int index : indices) builder.Add(whole, p->circles.at(index).edge);
    for (int index : indices) { p->circles.at(index).edge = whole; p->circles.at(index).canonical = indices.front(); }
  }
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

void BodyShape::ComputeSelection(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode) {
  AIS_Shape::ComputeSelection(selection, mode);
  if (mode == 0) return;  // whole body: highlighted in place, see Viewport::applySelectionLayers
  // The ordinal of each sub-shape is worked out here, once per body and mode: looking it up per selected
  // sub-shape (opad::subshape_index walks the whole body) made a big rubber band quadratic.
  TopTools_IndexedMapOfShape ordinals;
  TopExp::MapShapes(myshape, AIS_Shape::SelectionType(mode), ordinals);
  NCollection_DataMap<Handle(SelectMgr_EntityOwner), Handle(SelectMgr_EntityOwner)> swapped;  // a face has several entities
  for (const Handle(SelectMgr_SensitiveEntity)& e : selection->Entities()) {
    const Handle(Select3D_SensitiveEntity)& sens = e->BaseSensitive();
    Handle(StdSelect_BRepOwner) old = Handle(StdSelect_BRepOwner)::DownCast(sens->OwnerId());
    if (old.IsNull() || !old->ComesFromDecomposition()) continue;
    Handle(SelectMgr_EntityOwner) mine;
    if (!swapped.Find(old, mine)) {
      Handle(SubShapeOwner) o = new SubShapeOwner(old->Shape(), this, old->Priority(), ordinals.FindIndex(old->Shape()) - 1);
      o->SetHilightMode(old->Shape().ShapeType() == TopAbs_FACE ? AIS_Shaded : old->HilightMode());
      mine = o;
      swapped.Bind(old, mine);
    }
    sens->Set(mine);
  }
  if (mode == AIS_Shape::SelectionMode(TopAbs_VERTEX) && m_prs)
    for (const auto& [index, circle] : m_prs->circles)
      selection->Add(new SharedSensitive(new CircleOwner(circle, this, circle.canonical < 0 ? index : circle.canonical), circle.sensitive));
}

void NavigationShape::ComputeSelection(const Handle(SelectMgr_Selection)& selection, Standard_Integer) {
  if (!m_prototype.IsNull()) selection->Add(new SharedSensitive(new SelectMgr_EntityOwner(this), m_prototype));
}
