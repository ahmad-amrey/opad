#include "CurveSamples.hpp"
#include <Prs3d_PointAspect.hxx>
#include "BodyShape.hpp"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include "opad/geometry.hpp"
#include <Select3D_SensitiveTriangle.hxx>
#include <Select3D_SensitiveSegment.hxx>
#include <Select3D_SensitivePoint.hxx>

#include <AIS_DisplayMode.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <cmath>
#include <algorithm>
#include <tuple>
#include <BRepAdaptor_Curve.hxx>
#include <Select3D_SensitiveCurve.hxx>
#include <Select3D_SensitivePrimitiveArray.hxx>
#include <Select3D_SensitiveSet.hxx>
#include <SelectBasics_SelectingVolumeManager.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
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
namespace {
class CurvePresentation : public StdSelect_Shape {
 public:
  CurvePresentation(const TopoDS_Shape& shape,std::shared_ptr<const std::vector<gp_Pnt>> points)
    : StdSelect_Shape(shape),m_points(std::move(points)) {}
  void Compute(const Handle(PrsMgr_PresentationManager)&,const Handle(Prs3d_Presentation)& prs,Standard_Integer) override {
    auto line=new Graphic3d_ArrayOfSegments(int(m_points->size()-1)*2);
    for(size_t i=1;i<m_points->size();++i) {line->AddVertex((*m_points)[i-1]);line->AddVertex((*m_points)[i]);}
    auto group=prs->NewGroup();group->SetGroupPrimitivesAspect(myDrawer->WireAspect()->Aspect());group->AddPrimitiveArray(line);
  }
 private:
  std::shared_ptr<const std::vector<gp_Pnt>> m_points;
};

// The free edges of a big body as one picking entity: their sampled segments under one BVH, built on the mesh worker.
class SegmentSet : public Select3D_SensitiveSet {
  DEFINE_STANDARD_RTTI_INLINE(SegmentSet, Select3D_SensitiveSet)
 public:
  explicit SegmentSet(std::vector<gp_Pnt> pairs) : Select3D_SensitiveSet(nullptr), m_points(std::move(pairs)) {
    m_order.resize(m_points.size() / 2);
    for (size_t i = 0; i < m_order.size(); ++i) m_order[i] = int(i);
    gp_XYZ sum(0, 0, 0);
    for (const auto& p : m_points) {
      m_box.Add(SelectMgr_Vec3(p.X(), p.Y(), p.Z()));
      sum += p.XYZ();
    }
    if (!m_points.empty()) m_center = gp_Pnt(sum / double(m_points.size()));
  }
  Standard_Integer Size() const override { return int(m_order.size()); }
  Select3D_BndBox3d Box(const Standard_Integer i) const override {
    const gp_Pnt &a = first(i), &b = second(i);
    return Select3D_BndBox3d(SelectMgr_Vec3(std::min(a.X(), b.X()), std::min(a.Y(), b.Y()), std::min(a.Z(), b.Z())),
                             SelectMgr_Vec3(std::max(a.X(), b.X()), std::max(a.Y(), b.Y()), std::max(a.Z(), b.Z())));
  }
  Standard_Real Center(const Standard_Integer i, const Standard_Integer axis) const override {
    return (first(i).Coord(axis + 1) + second(i).Coord(axis + 1)) / 2;
  }
  void Swap(const Standard_Integer i, const Standard_Integer j) override { std::swap(m_order[size_t(i)], m_order[size_t(j)]); }
  Select3D_BndBox3d BoundingBox() override { return m_box; }
  gp_Pnt CenterOfGeometry() const override { return m_center; }
  Standard_Integer NbSubElements() const override { return Size(); }

 protected:
  Standard_Boolean overlapsElement(SelectBasics_PickResult& result, SelectBasics_SelectingVolumeManager& volume, Standard_Integer i,
                                   Standard_Boolean inside) override {
    return inside || volume.OverlapsSegment(first(i), second(i), result);
  }
  Standard_Boolean elementIsInside(SelectBasics_SelectingVolumeManager& volume, Standard_Integer i, Standard_Boolean inside) override {
    if (inside) return true;
    if (volume.GetActiveSelectionType() == SelectMgr_SelectionType_Polyline) {
      SelectBasics_PickResult unused;
      return volume.OverlapsSegment(first(i), second(i), unused);
    }
    return volume.OverlapsPoint(first(i)) && volume.OverlapsPoint(second(i));
  }
  Standard_Real distanceToCOG(SelectBasics_SelectingVolumeManager& volume) override { return volume.DistToGeometryCenter(m_center); }

 private:
  const gp_Pnt& first(int i) const { return m_points[size_t(m_order[size_t(i)]) * 2]; }
  const gp_Pnt& second(int i) const { return m_points[size_t(m_order[size_t(i)]) * 2 + 1]; }
  std::vector<gp_Pnt> m_points;
  std::vector<int> m_order;
  Select3D_BndBox3d m_box;
  gp_Pnt m_center;
};

// Faces + edges above which a body's picking is built on the worker (BodyPrs::whole / edgeSensitives).
constexpr int kBigBody = 3000;
}

void SubShapeOwner::HilightWithColor(const Handle(PrsMgr_PresentationManager)& pm, const Handle(Prs3d_Drawer)& style, const Standard_Integer mode) {
  if(pm->IsImmediateModeOn()) {
    if(curve && curve->size()>1 && myPrsSh.IsNull()) myPrsSh=new CurvePresentation(myShape,curve);
    StdSelect_BRepOwner::HilightWithColor(pm,style,mode);
  }
}

void SubShapeOwner::Unhilight(const Handle(PrsMgr_PresentationManager)& pm, const Standard_Integer mode) {
  if (!myPrsSh.IsNull()) StdSelect_BRepOwner::Unhilight(pm, mode);  // with no presentation the base un-highlights the whole body
}

void SubHighlight::Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) {
  if (!m_triangles.empty()) {
    Handle(Graphic3d_AspectFillArea3d) fill = new Graphic3d_AspectFillArea3d();
    fill->SetInteriorStyle(Aspect_IS_SOLID);
    fill->SetInteriorColor(Quantity_ColorRGBA(m_color, 0.18f));
    fill->SetAlphaMode(Graphic3d_AlphaMode_Blend);
    fill->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);  // a flat tint: the arrays carry no normals
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(fill);
    for (const auto& a : m_triangles) g->AddPrimitiveArray(a);
    Handle(Graphic3d_AspectFillArea3d) glow = new Graphic3d_AspectFillArea3d(*fill);
    glow->SetInteriorColor(Quantity_ColorRGBA(Quantity_NOC_WHITE,0.12f));
    auto white=prs->NewGroup();white->SetGroupPrimitivesAspect(glow);
    for(const auto& a:m_triangles) white->AddPrimitiveArray(a);
  }
  if (!m_segments.empty()) {
    auto halo=prs->NewGroup();Handle(Graphic3d_AspectLine3d) glow=new Graphic3d_AspectLine3d(Quantity_NOC_WHITE,Aspect_TOL_SOLID,6);
    glow->SetInteriorColor(Quantity_ColorRGBA(Quantity_NOC_WHITE,0.35f));glow->SetAlphaMode(Graphic3d_AlphaMode_Blend);halo->SetGroupPrimitivesAspect(glow);
    for(const auto& a:m_segments) halo->AddPrimitiveArray(a);
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(m_color, Aspect_TOL_SOLID, 3.0));
    for (const auto& a : m_segments) g->AddPrimitiveArray(a);
    auto white=prs->NewGroup();white->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(Quantity_NOC_WHITE,Aspect_TOL_SOLID,1.5));
    for(const auto& a:m_segments) white->AddPrimitiveArray(a);
  }
  if (!m_points.empty()) {
    auto halo=prs->NewGroup();Handle(Graphic3d_AspectMarker3d) glow=new Graphic3d_AspectMarker3d(Aspect_TOM_BALL,Quantity_NOC_WHITE,6.0);
    glow->SetInteriorColor(Quantity_ColorRGBA(Quantity_NOC_WHITE,0.55f));glow->SetAlphaMode(Graphic3d_AlphaMode_Blend);halo->SetGroupPrimitivesAspect(glow);
    for(const auto& a:m_points) halo->AddPrimitiveArray(a);
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, m_color, 2.5));
    for (const auto& a : m_points) g->AddPrimitiveArray(a);
  }
}

namespace {
// Orbit fallback asks whether a screen region contains any geometry, not which
// triangles it contains. OCCT's ordinary area selection enumerates all matching
// primitives, making the first large region expensive on detailed models.
class NavigationTriangles : public Select3D_SensitivePrimitiveArray {
 public:
  NavigationTriangles() : Select3D_SensitivePrimitiveArray(nullptr) {}
  Standard_Boolean Matches(SelectBasics_SelectingVolumeManager& volume, SelectBasics_PickResult& result) override {
    if (volume.GetActiveSelectionType() == SelectMgr_SelectionType_Point || !volume.IsOverlapAllowed())
      return Select3D_SensitivePrimitiveArray::Matches(volume, result);
    if (Size() == 0) return false;
    const auto& tree = myContent.GetBVH();  // already built by the mesh worker
    std::vector<int> pending{0};
    while (!pending.empty()) {
      const int node = pending.back(); pending.pop_back();
      if (!volume.OverlapsBox(tree->MinPoint(node), tree->MaxPoint(node))) continue;
      const auto& data = tree->NodeInfoBuffer()[node];
      if (data.x() == 0) {
        pending.push_back(data.y()); pending.push_back(data.z());
      } else {
        for (int element = data.y(); element <= data.z(); ++element) {
          if (overlapsElement(result, volume, element, false)) {
            myDetectedIdx = element;
            result.SetDistToGeomCenter(distanceToCOG(volume));
            return true;
          }
        }
      }
    }
    return false;
  }
};
}

opad::MeshingReport BodyPrs::meshForDisplay(const TopoDS_Shape& shape, double deflection) {
  const auto report = opad::mesh_shape(shape, deflection);
  opad::straighten_ruled_faces(shape);  // extruded walls stay upright seen along the extrusion (TODO 10 A1)
  return report;
}

size_t BodyPrs::triangleCount() const {
  if (triangles.IsNull()) return 0;
  return size_t(triangles->EdgeNumber() > 0 ? triangles->EdgeNumber() : triangles->VertexNumber()) / 3;
}

std::shared_ptr<BodyPrs> BodyPrs::build(const TopoDS_Shape& meshedProto, const Bnd_Box& box, bool drawingOnly,
                                        std::shared_ptr<const opad::FaceColors> faceColors) {
  auto p = std::make_shared<BodyPrs>();
  p->box = box;
  p->triangles = StdPrs_ShadedShape::FillTriangles(meshedProto);
  p->boundaries = StdPrs_ShadedShape::FillFaceBoundaries(meshedProto);
  if (faceColors && !faceColors->empty() && !p->triangles.IsNull()) {
    // The faces by colour (index 0: the body's own), each group's triangles filled as the whole body's are.
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(meshedProto, TopAbs_FACE, faces);
    BRep_Builder builder;
    std::vector<TopoDS_Compound> groups(faceColors->colors.size() + 1);
    std::vector<bool> used(groups.size(), false);
    for (auto& g : groups) builder.MakeCompound(g);
    for (int i = 1; i <= faces.Extent(); ++i) {
      const size_t group = size_t(faceColors->at(i - 1) + 1);
      builder.Add(groups[group], faces(i));
      used[group] = true;
    }
    p->faceColors = faceColors;
    if (used[0]) p->own = StdPrs_ShadedShape::FillTriangles(groups[0]);
    for (size_t g = 1; g < groups.size(); ++g) {
      if (!used[g]) continue;
      const auto& c = faceColors->colors[g - 1];
      if (auto t = StdPrs_ShadedShape::FillTriangles(groups[g]); !t.IsNull()) p->painted.push_back({Quantity_Color(c[0], c[1], c[2], Quantity_TOC_sRGB), t});
    }
  }
  if (drawingOnly) {
    p->closed = false;
    for (TopExp_Explorer e(meshedProto, TopAbs_SHELL); e.More(); e.Next()) {
      p->closed = true;
      if (!BRep_Tool::IsClosed(e.Current())) { p->closed = false; break; }
    }
    if (meshedProto.ShapeType() > TopAbs_SHELL) p->closed = false;
    return p;
  }
  if (!p->triangles.IsNull()) {
    Handle(Select3D_SensitivePrimitiveArray) triangles = new NavigationTriangles();
    if (triangles->InitTriangulation(p->triangles->Attributes(), p->triangles->Indices(), TopLoc_Location())) {
      triangles->BVH();
      p->navigation = triangles;
    }
  }
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(meshedProto, TopAbs_EDGE, edges);
  // A big body (a drawing layer of text and lines: thousands of faces and edges) gets its picking built here as well:
  // OCCT builds one sensitive per face and edge on the UI thread when a selection mode is activated (0.85 s for a
  // DWG's text layer). Its edges are sampled for that, as a line-only body's always are.
  int faceCount = 0;
  for (TopExp_Explorer f(meshedProto, TopAbs_FACE); f.More() && faceCount <= kBigBody; f.Next()) ++faceCount;
  const bool big = faceCount + edges.Extent() > kBigBody;
  for (int i = 1; i <= edges.Extent(); ++i) {
    if (BRep_Tool::Degenerated(TopoDS::Edge(edges(i)))) continue;
    BRepAdaptor_Curve curve(TopoDS::Edge(edges(i)));
    if (p->triangles.IsNull() || big) {
      const double span=box.IsVoid()?1.0:std::sqrt(box.SquareExtent());
      auto samples=std::make_shared<const std::vector<gp_Pnt>>(curveSamples(TopoDS::Edge(edges(i)),std::max(1e-6,span*1e-5)));
      p->curves[i-1]=samples;
      if (p->triangles.IsNull())
        for(size_t j=1;j<samples->size();++j) {p->drawingSegments.push_back((*samples)[j-1]);p->drawingSegments.push_back((*samples)[j]);}
    }
    if (curve.GetType() != GeomAbs_Circle) continue;
    TColgp_Array1OfPnt points(1, 257);
    for (int j = 1; j <= points.Length(); ++j)
      points(j) = curve.Value(curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * (j - 1) / 256.0);
    Handle(Select3D_SensitiveCurve) sensitive = new Select3D_SensitiveCurve(nullptr, points);
    sensitive->BVH();
    p->circles.emplace(i - 1, Circle{edges(i), curve.Circle().Location(), sensitive,-1,0,0,{}});
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
  for(auto& [id,c]:p->circles) {
    // Grouped circular arcs keep the radius of their canonical analytic edge.
    BRepAdaptor_Curve curve(TopoDS::Edge(edges(id+1))); c.radius=curve.Circle().Radius();
  }
  for(const auto& fit:opad::mesh_circles(meshedProto)) {
    TColgp_Array1OfPnt points(1,fit.segments+1);
    BRep_Builder builder; TopoDS_Compound rim; builder.MakeCompound(rim);
    for(int i=0;i<fit.segments;++i) {
      points(i+1)=fit.rim[i];
      builder.Add(rim,BRepBuilderAPI_MakeEdge(fit.rim[i],fit.rim[(i+1)%fit.segments]).Edge());
    }
    points(fit.segments+1)=fit.rim.front();
    Handle(Select3D_SensitiveCurve) sensitive=new Select3D_SensitiveCurve(nullptr,points); sensitive->BVH();
    p->circles.emplace(fit.index,Circle{rim,fit.circle.Location(),sensitive,fit.index,fit.circle.Radius(),fit.segments,fit.edges});
  }
  if (big) {
    // Each edge's sensitive (the Edge filter wraps them with owners) and the whole body's: its triangles (the
    // navigation set) and its free edges' segments in one set (the Body filter).
    TopTools_IndexedDataMapOfShapeListOfShape faces;
    TopExp::MapShapesAndAncestors(meshedProto, TopAbs_EDGE, TopAbs_FACE, faces);
    p->edgeSensitives.resize(size_t(edges.Extent()));
    std::vector<gp_Pnt> loose;
    for (const auto& [index, points] : p->curves) {
      if (points->size() < 2) continue;
      TColgp_Array1OfPnt array(1, int(points->size()));
      for (int j = 1; j <= array.Length(); ++j) array(j) = (*points)[size_t(j - 1)];
      Handle(Select3D_SensitiveCurve) sensitive = new Select3D_SensitiveCurve(nullptr, array);
      sensitive->BVH();
      sensitive->BoundingBox();  // computed once and kept: the selector asks for it on the UI thread otherwise
      p->edgeSensitives[size_t(index)] = sensitive;
      const TopoDS_Shape& edge = edges(index + 1);
      if (!faces.Contains(edge) || faces.FindFromKey(edge).IsEmpty())
        for (size_t j = 1; j < points->size(); ++j) { loose.push_back((*points)[j - 1]); loose.push_back((*points)[j]); }
    }
    if (!p->navigation.IsNull()) p->whole.push_back(p->navigation);
    if (!loose.empty()) {
      Handle(SegmentSet) set = new SegmentSet(std::move(loose));
      set->BVH();
      p->whole.push_back(set);
    }
  }
  p->closed = false;
  for (TopExp_Explorer e(meshedProto, TopAbs_SHELL); e.More(); e.Next()) {
    p->closed = true;
    if (!BRep_Tool::IsClosed(e.Current())) { p->closed = false; break; }
  }
  if (meshedProto.ShapeType() > TopAbs_SHELL) p->closed = false;  // a bare face or lower
  if(p->triangles.IsNull() && !p->drawingSegments.empty()) {
    p->boundaries=new Graphic3d_ArrayOfSegments(int(p->drawingSegments.size()));
    for(const auto& point:p->drawingSegments) p->boundaries->AddVertex(point);
  }
  std::vector<gp_Pnt> loose;
  for(TopExp_Explorer vertex(meshedProto,TopAbs_VERTEX,TopAbs_EDGE);vertex.More();vertex.Next())
    loose.push_back(BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current())));
  if(!loose.empty()) {
    p->loosePoints=new Graphic3d_ArrayOfPoints(int(loose.size()));
    for(const auto& point:loose) p->loosePoints->AddVertex(point);
  }
  return p;
}

void BodyShape::Compute(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Presentation)& prs, const Standard_Integer mode) {
  // The zoom refinement's arrays when there are some; everything else below only reads `shown`.
  const auto& shown = m_display && !m_display->triangles.IsNull() ? m_display : m_prs;
  if(m_prs && m_prs->triangles.IsNull() && (!m_prs->boundaries.IsNull() || !m_prs->loosePoints.IsNull())) {
    if(!m_prs->boundaries.IsNull()) {auto g=prs->NewGroup();g->SetGroupPrimitivesAspect(myDrawer->WireAspect()->Aspect());g->AddPrimitiveArray(m_prs->boundaries);}
    if(!m_prs->loosePoints.IsNull()) {auto g=prs->NewGroup();g->SetGroupPrimitivesAspect(myDrawer->PointAspect()->Aspect());g->AddPrimitiveArray(m_prs->loosePoints);}
    return;
  }
  if (mode == AIS_Shaded && !shown && m_faceColors && !m_faceColors->empty()) {
    // OCCT's shaded path, once per colour: the faces in the body's colour, then each face colour with the same look.
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(myshape, TopAbs_FACE, faces);
    std::map<int, TopoDS_Compound> groups;
    BRep_Builder builder;
    for (int i = 1; i <= faces.Extent(); ++i) {
      auto& group = groups[m_faceColors->at(i - 1)];
      if (group.IsNull()) builder.MakeCompound(group);
      builder.Add(group, faces(i));
    }
    for (const auto& [colour, group] : groups) {
      Handle(Prs3d_Drawer) look = myDrawer;
      if (colour >= 0) {
        look = new Prs3d_Drawer();
        look->SetLink(myDrawer);
        Handle(Prs3d_ShadingAspect) shading = new Prs3d_ShadingAspect(new Graphic3d_AspectFillArea3d(*myDrawer->ShadingAspect()->Aspect()));
        const auto& c = m_faceColors->colors[size_t(colour)];
        shading->SetColor(Quantity_Color(c[0], c[1], c[2], Quantity_TOC_sRGB));
        look->SetShadingAspect(shading);
      }
      StdPrs_ShadedShape::Add(prs, group, look);
    }
    return;
  }
  if (mode != AIS_Shaded || !shown || shown->triangles.IsNull()) {
    AIS_Shape::Compute(mgr, prs, mode);  // wireframe/HLR, or nothing precomputed: the stock path
    return;
  }
  // Min/max are supplied from the worker's box; evaluating them here walks every vertex on the UI thread.
  const bool haveBox = !shown->box.IsVoid();
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  if (haveBox) shown->box.Get(x0, y0, z0, x1, y1, z1);
  // Ray intersections do not use raster depth offsets. Separate only the render
  // skin along its normals; the analytic shape, selection and exports stay exact.
  auto biased=[this](const Handle(Graphic3d_ArrayOfTriangles)& src) {
    if (m_rayBias==0) return src;
    auto& copy=m_rayTriangles[src.get()];
    if (copy.IsNull()) {
      copy=new Graphic3d_ArrayOfTriangles(src->VertexNumber(),src->EdgeNumber(),true);
      for(int i=1;i<=src->VertexNumber();++i) {
        const gp_Dir n=src->VertexNormal(i);
        copy->AddVertex(src->Vertice(i).Translated(gp_Vec(n)*m_rayBias),n);
      }
      for(int i=1;i<=src->EdgeNumber();++i) copy->AddEdge(src->Edge(i));
    }
    return copy;
  };
  auto fill=[&](const Handle(Graphic3d_ArrayOfTriangles)& triangles, const Handle(Graphic3d_AspectFillArea3d)& aspect) {
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetClosed(shown->closed);
    g->SetGroupPrimitivesAspect(aspect);
    g->AddPrimitiveArray(biased(triangles), !haveBox);
    if (haveBox) g->SetMinMaxValues(x0, y0, z0, x1, y1, z1);
  };
  if (shown->painted.empty()) fill(shown->triangles, myDrawer->ShadingAspect()->Aspect());
  else {
    // The body's colour where the file gave a face none of its own, each face colour in its own group with the same look.
    if (!shown->own.IsNull()) fill(shown->own, myDrawer->ShadingAspect()->Aspect());
    for (const auto& part : shown->painted) {
      Handle(Prs3d_ShadingAspect) look = new Prs3d_ShadingAspect(new Graphic3d_AspectFillArea3d(*myDrawer->ShadingAspect()->Aspect()));
      look->SetColor(part.color);
      fill(part.triangles, look->Aspect());
    }
  }
  if (myDrawer->FaceBoundaryDraw() && !shown->boundaries.IsNull()) {
    Handle(Graphic3d_Group) e = prs->NewGroup();
    e->SetGroupPrimitivesAspect(myDrawer->FaceBoundaryAspect()->Aspect());
    e->AddPrimitiveArray(shown->boundaries, !haveBox);
    if (haveBox) e->SetMinMaxValues(x0, y0, z0, x1, y1, z1);
  }
}

namespace {
// Whole-body selection uses a lightweight overlay of the prepared arrays, so
// the original material remains visible beneath its tint and white glow.
class BodySelectionOwner : public StdSelect_BRepOwner {
 public:
  BodySelectionOwner(const TopoDS_Shape& shape,const Handle(SelectMgr_SelectableObject)& body,int priority)
      : StdSelect_BRepOwner(shape,body,priority,false) {}
  void HilightWithColor(const Handle(PrsMgr_PresentationManager)& pm,const Handle(Prs3d_Drawer)& style,Standard_Integer mode) override {
    if(pm->IsImmediateModeOn()) StdSelect_BRepOwner::HilightWithColor(pm,style,mode);
  }
};
// Facets remain compact triangulations in the document. Construct an analytic
// triangle/segment/vertex only when that primitive is actually picked.
class MeshOwner : public SubShapeOwner {
 public:
  MeshOwner(const TopoDS_Shape& mesh, const Handle(SelectMgr_SelectableObject)& body, opad::Ref::Kind kind, int index)
      : SubShapeOwner({},body,kind==opad::Ref::Kind::Vertex?9:kind==opad::Ref::Kind::Edge?7:5,index), m_mesh(mesh), m_kind(kind) {
    SetHilightMode(kind==opad::Ref::Kind::Face?AIS_Shaded:AIS_WireFrame);
  }
  opad::Ref::Kind kind() const override { return m_kind; }
  void prepare() override { if(myShape.IsNull()) myShape=opad::subshape(m_mesh,m_kind,index()); }
  void HilightWithColor(const Handle(PrsMgr_PresentationManager)& pm,const Handle(Prs3d_Drawer)& style,Standard_Integer mode) override {
    if(pm->IsImmediateModeOn()) prepare();
    SubShapeOwner::HilightWithColor(pm,style,mode);
  }
 private:
  TopoDS_Shape m_mesh;
  opad::Ref::Kind m_kind;
};
template<class Sensitive> using MeshSensitive=Sensitive;
}

void BodyShape::ComputeSelection(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode) {
  if(mode!=0 && opad::is_mesh_shape(myshape)) {
    const auto type=AIS_Shape::SelectionType(mode);
    const auto kind=type==TopAbs_FACE?opad::Ref::Kind::Face:type==TopAbs_EDGE?opad::Ref::Kind::Edge:opad::Ref::Kind::Vertex;
    std::map<std::tuple<double,double,double>,Handle(CircleOwner)> rimVertices;
    std::map<int,Handle(CircleOwner)> circleOwners;
    if(type==TopAbs_VERTEX && m_prs) for(const auto& [id,circle]:m_prs->circles) {
      auto owner=Handle(CircleOwner)(new CircleOwner(circle,this,id));circleOwners[id]=owner;
      for(TopExp_Explorer vertex(circle.edge,TopAbs_VERTEX);vertex.More();vertex.Next()) {
        const auto p=BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current()));
        rimVertices.emplace(std::make_tuple(p.X(),p.Y(),p.Z()),owner);
      }
    }
    int index=0;
    for(TopExp_Explorer faces(myshape,TopAbs_FACE);faces.More();faces.Next()) {
      TopLoc_Location location;
      const auto mesh=BRep_Tool::Triangulation(TopoDS::Face(faces.Current()),location);
      if(mesh.IsNull()) continue;
      auto point=[&](int i) { return mesh->Node(i).Transformed(location.Transformation()); };
      auto owner=[&] { return Handle(MeshOwner)(new MeshOwner(myshape,this,kind,index++)); };
      if(type==TopAbs_VERTEX) {
        for(int i=1;i<=mesh->NbNodes();++i) {
          const auto p=point(i);auto circle=rimVertices.find(std::make_tuple(p.X(),p.Y(),p.Z()));
          // Keep both targets: the viewport admits circle owners only with Ctrl.
          if(circle!=rimVertices.end()) selection->Add(new Select3D_SensitivePoint(circle->second,p));
          selection->Add(new MeshSensitive<Select3D_SensitivePoint>(owner(),p));
        }
      } else for(int i=1;i<=mesh->NbTriangles();++i) {
        int a,b,c; mesh->Triangle(i).Get(a,b,c);
        if(type==TopAbs_FACE) selection->Add(new MeshSensitive<Select3D_SensitiveTriangle>(owner(),point(a),point(b),point(c)));
        else {
          selection->Add(new MeshSensitive<Select3D_SensitiveSegment>(owner(),point(a),point(b)));
          selection->Add(new MeshSensitive<Select3D_SensitiveSegment>(owner(),point(b),point(c)));
          selection->Add(new MeshSensitive<Select3D_SensitiveSegment>(owner(),point(c),point(a)));
        }
      }
    }
    if(type==TopAbs_VERTEX && m_prs) for(const auto& [index,circle]:m_prs->circles)
      selection->Add(new SharedSensitive(circleOwners.at(index),circle.sensitive));
    return;
  }
  if(m_prs && !m_prs->curves.empty() && mode==AIS_Shape::SelectionMode(TopAbs_EDGE)) {
    TopTools_IndexedMapOfShape edges;TopExp::MapShapes(myshape,TopAbs_EDGE,edges);
    for(const auto& [index,points]:m_prs->curves) {
      if(points->size()<2) continue;
      Handle(SubShapeOwner) owner=new SubShapeOwner(edges(index+1),this,7,index);owner->curve=points;
      if(size_t(index)<m_prs->edgeSensitives.size() && !m_prs->edgeSensitives[size_t(index)].IsNull()) {
        selection->Add(new SharedSensitive(owner,m_prs->edgeSensitives[size_t(index)]));  // built on the worker
        continue;
      }
      TColgp_Array1OfPnt array(1,int(points->size()));for(int i=1;i<=array.Length();++i) array(i)=(*points)[i-1];
      selection->Add(new Select3D_SensitiveCurve(owner,array));
    }
    return;
  }
  if (mode == 0 && m_prs && !m_prs->whole.empty()) {  // a big body: its picking was built on the worker
    Handle(SelectMgr_EntityOwner) owner=new BodySelectionOwner(myshape,this,5);
    for (const auto& sensitive : m_prs->whole) selection->Add(new SharedSensitive(owner,sensitive));
    return;
  }
  AIS_Shape::ComputeSelection(selection, mode);
  if (mode == 0) {
    Handle(SelectMgr_EntityOwner) owner=new BodySelectionOwner(myshape,this,5);
    for(const auto& entity:selection->Entities()) entity->BaseSensitive()->Set(owner);
    return;
  }
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
    for (const auto& [index, circle] : m_prs->circles) {
      Handle(CircleOwner) owner=new CircleOwner(circle,this,circle.canonical<0?index:circle.canonical);
      selection->Add(new SharedSensitive(owner,circle.sensitive));
      for(TopExp_Explorer vertex(circle.edge,TopAbs_VERTEX);vertex.More();vertex.Next())
        selection->Add(new Select3D_SensitivePoint(owner,BRep_Tool::Pnt(TopoDS::Vertex(vertex.Current()))));
    }
}

void NavigationShape::ComputeSelection(const Handle(SelectMgr_Selection)& selection, Standard_Integer) {
  if (!m_prototype.IsNull()) selection->Add(new SharedSensitive(new SelectMgr_EntityOwner(this), m_prototype));
}
