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

  const gp_Pnt& first(int i) const { return m_points[size_t(m_order[size_t(i)]) * 2]; }
  const gp_Pnt& second(int i) const { return m_points[size_t(m_order[size_t(i)]) * 2 + 1]; }
  std::vector<gp_Pnt> m_points;  // segment s: 2s, 2s + 1
  std::vector<int> m_order;      // the BVH's order of the segments
  Select3D_BndBox3d m_box;
  gp_Pnt m_center;
};

// How close the segment ab comes to the pick ray (from its near to its far point).
double rayDistance(const gp_Pnt& eye, const gp_Pnt& away, const gp_Pnt& a, const gp_Pnt& b) {
  const gp_Vec u(eye, away), v(a, b), w(a, eye);
  const double uu = u.Dot(u), uv = u.Dot(v), vv = v.Dot(v), uw = u.Dot(w), vw = v.Dot(w), det = uu * vv - uv * uv;
  double t = det > 1e-18 * uu * vv ? (uu * vw - uv * uw) / det : (vv > 0 ? vw / vv : 0);  // along the segment
  t = std::clamp(t, 0.0, 1.0);
  const gp_Pnt on = a.Translated(v * t);
  const double along = uu > 0 ? gp_Vec(eye, on).Dot(u) / uu : 0;
  return on.Distance(eye.Translated(u * along));
}

// One group of a big drawing layer's edges (BodyPrs::edgeGroups, UI-42): their sampled segments under one BVH and the
// edge each one belongs to. Built on the mesh worker; EdgeGroupSensitive puts each body's owners on it.
class EdgeGroupSet : public SegmentSet {
  DEFINE_STANDARD_RTTI_INLINE(EdgeGroupSet, SegmentSet)
 public:
  struct Edge { int ordinal, first, count; Select3D_BndBox3d box; };
  EdgeGroupSet(std::vector<gp_Pnt> pairs, std::vector<int> edgeOf, std::vector<Edge> edges)
      : SegmentSet(std::move(pairs)), m_edgeOf(std::move(edgeOf)), m_edges(std::move(edges)) {}
  int firstOrdinal() const { return m_edges.front().ordinal; }
  // A point pick: the ordinal of the edge nearest the pointer's ray among those in reach, -1 for none.
  int pick(SelectBasics_SelectingVolumeManager& volume, SelectBasics_PickResult& result) {
    m_best = -1;
    m_bestDistance = RealLast();
    if (!Select3D_SensitiveSet::Matches(volume, result) || m_best < 0) return -1;
    return m_edges[size_t(m_edgeOf[size_t(m_best)])].ordinal;
  }
  // A box or polygon: the ordinals of the edges it takes, crossing (any part) or window (all of it).
  void take(SelectBasics_SelectingVolumeManager& volume, std::vector<int>& out) const {
    const bool crossing = volume.IsOverlapAllowed(), polyline = volume.GetActiveSelectionType() == SelectMgr_SelectionType_Polyline;
    SelectBasics_PickResult unused;
    for (const Edge& e : m_edges) {
      bool inside = false;
      if (!volume.OverlapsBox(e.box.CornerMin(), e.box.CornerMax(), &inside)) continue;
      bool hit = inside;
      for (int s = e.first; !hit && crossing && s < e.first + e.count; ++s) hit = volume.OverlapsSegment(m_points[size_t(s) * 2], m_points[size_t(s) * 2 + 1], unused);
      if (!hit && !crossing) {
        hit = true;
        for (int s = e.first; hit && s < e.first + e.count; ++s) {
          const gp_Pnt &a = m_points[size_t(s) * 2], &b = m_points[size_t(s) * 2 + 1];
          hit = polyline ? volume.OverlapsSegment(a, b, unused) : volume.OverlapsPoint(a) && volume.OverlapsPoint(b);
        }
      }
      if (hit) out.push_back(e.ordinal);
    }
  }

 protected:
  Standard_Boolean overlapsElement(SelectBasics_PickResult& result, SelectBasics_SelectingVolumeManager& volume, Standard_Integer i,
                                   Standard_Boolean inside) override {
    if (!SegmentSet::overlapsElement(result, volume, i, inside)) return false;
    if (volume.GetActiveSelectionType() == SelectMgr_SelectionType_Point) {
      const double d = rayDistance(volume.GetNearPickedPnt(), volume.GetFarPickedPnt(), first(i), second(i));
      if (d < m_bestDistance) {
        m_bestDistance = d;
        m_best = m_order[size_t(i)];
      }
    }
    return true;
  }

 private:
  std::vector<int> m_edgeOf;  // segment -> index into m_edges
  std::vector<Edge> m_edges;
  int m_best = -1;  // the segment nearest the ray in the last point pick (UI thread only)
  double m_bestDistance = 0;
};

// Faces + edges above which a body's picking is built on the worker (BodyPrs::whole / edgeSensitives / edgeGroups).
constexpr int kBigBody = 3000;
constexpr size_t kEdgeGroup = 256;  // edges per group
}

HoverLines& HoverLines::current() {
  static HoverLines lines;
  return lines;
}

namespace {
// A curve body's lines at one width, coloured by the highlight it is drawn with: the hover's core and rim (HoverLines).
class HoverLinesPrs : public AIS_InteractiveObject {
 public:
  HoverLinesPrs(Handle(Graphic3d_ArrayOfSegments) segments, float width) : m_segments(std::move(segments)), m_width(width) {}
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(Quantity_NOC_WHITE, Aspect_TOL_SOLID, m_width));
    g->AddPrimitiveArray(m_segments);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}
 private:
  Handle(Graphic3d_ArrayOfSegments) m_segments;
  float m_width;
};

// Draws `prs` as the owner's hover draws its own (same layer, placement and immediate list), coloured by `style`.
void hoverAlike(const Handle(PrsMgr_PresentationManager)& pm, const Handle(PrsMgr_PresentableObject)& prs, const Handle(Prs3d_Drawer)& style,
                const Handle(Prs3d_Drawer)& hover, const Handle(SelectMgr_SelectableObject)& selectable, const gp_Trsf& placement) {
  prs->SetZLayer(selectable->ZLayer());
  prs->SetTransformPersistence(selectable->TransformPersistence());
  prs->SetLocalTransformation(placement);
  pm->Color(prs, style, 0, selectable, hover->ZLayer() != Graphic3d_ZLayerId_UNKNOWN ? hover->ZLayer() : selectable->ZLayer());
}
}  // namespace

void SubShapeOwner::HilightWithColor(const Handle(PrsMgr_PresentationManager)& pm, const Handle(Prs3d_Drawer)& style, const Standard_Integer mode) {
  if(pm->IsImmediateModeOn()) {
    if(curve && curve->size()>1 && myPrsSh.IsNull()) myPrsSh=new CurvePresentation(myShape,curve);
    // On a light background an edge's white hover gets a darker rim under it, drawn first (HoverLines).
    if(const auto& rim=HoverLines::current().rim; !rim.IsNull() && HasSelectable() && !myShape.IsNull() && myShape.ShapeType()==TopAbs_EDGE) {
      if(m_rim.IsNull()) {
        Handle(StdSelect_Shape) wide=curve && curve->size()>1 ? Handle(StdSelect_Shape)(new CurvePresentation(myShape,curve)) : new StdSelect_Shape(myShape);
        wide->Attributes()->SetLink(rim);
        m_rim=wide;
      }
      hoverAlike(pm,m_rim,rim,style,Selectable(),Location());
    }
    StdSelect_BRepOwner::HilightWithColor(pm,style,mode);
  }
}

void SubShapeOwner::Unhilight(const Handle(PrsMgr_PresentationManager)& pm, const Standard_Integer mode) {
  if (!myPrsSh.IsNull()) StdSelect_BRepOwner::Unhilight(pm, mode);  // with no presentation the base un-highlights the whole body
  if (!m_rim.IsNull()) pm->Unhighlight(m_rim);
}

void SubHighlight::Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) {
  const GlowStyle& s = m_style;
  auto translucent = [](const Handle(Graphic3d_Aspects)& aspect, const Quantity_Color& color, float alpha) {
    aspect->SetInteriorColor(Quantity_ColorRGBA(color, alpha));
    if (alpha < 1) aspect->SetAlphaMode(Graphic3d_AlphaMode_Blend);  // else drawn in order with the opaque ones, under the core
    return aspect;
  };
  if (!m_triangles.empty()) {
    Handle(Graphic3d_AspectFillArea3d) fill = new Graphic3d_AspectFillArea3d();
    fill->SetInteriorStyle(Aspect_IS_SOLID);
    fill->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);  // a flat tint: the arrays carry no normals
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(translucent(fill, s.fill, s.fillAlpha));
    for (const auto& a : m_triangles) g->AddPrimitiveArray(a);
  }
  if (!m_segments.empty()) {  // the core over a wider halo
    Handle(Graphic3d_Group) halo = prs->NewGroup();
    halo->SetGroupPrimitivesAspect(translucent(new Graphic3d_AspectLine3d(s.halo, Aspect_TOL_SOLID, s.haloWidth), s.halo, s.haloAlpha));
    for (const auto& a : m_segments) halo->AddPrimitiveArray(a);
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(s.edge, Aspect_TOL_SOLID, s.edgeWidth));
    for (const auto& a : m_segments) g->AddPrimitiveArray(a);
  }
  if (!m_points.empty()) {
    Handle(Graphic3d_Group) halo = prs->NewGroup();
    halo->SetGroupPrimitivesAspect(translucent(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, s.halo, s.pointHalo), s.halo, s.haloAlpha));
    for (const auto& a : m_points) halo->AddPrimitiveArray(a);
    Handle(Graphic3d_Group) g = prs->NewGroup();
    g->SetGroupPrimitivesAspect(new Graphic3d_AspectMarker3d(Aspect_TOM_BALL, s.edge, s.point));
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

std::shared_ptr<BodyPrs> BodyPrs::build(const TopoDS_Shape& meshedProto, const Bnd_Box& box, bool drawingOnly, bool drawing) {
  auto p = std::make_shared<BodyPrs>();
  p->box = box;
  p->triangles = StdPrs_ShadedShape::FillTriangles(meshedProto);
  p->boundaries = StdPrs_ShadedShape::FillFaceBoundaries(meshedProto);
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
    // Each edge's sensitive (the Edge filter wraps them with owners; a drawing layer's in groups) and the whole body's:
    // its triangles (the navigation set) and its free edges' segments in one set (the Body filter).
    TopTools_IndexedDataMapOfShapeListOfShape faces;
    TopExp::MapShapesAndAncestors(meshedProto, TopAbs_EDGE, TopAbs_FACE, faces);
    if (drawing) p->buildEdgeGroups(edges, box);
    else p->edgeSensitives.resize(size_t(edges.Extent()));
    std::vector<gp_Pnt> loose;
    for (const auto& [index, points] : p->curves) {
      if (points->size() < 2) continue;
      if (!drawing) {
        TColgp_Array1OfPnt array(1, int(points->size()));
        for (int j = 1; j <= array.Length(); ++j) array(j) = (*points)[size_t(j - 1)];
        Handle(Select3D_SensitiveCurve) sensitive = new Select3D_SensitiveCurve(nullptr, array);
        sensitive->BVH();
        sensitive->BoundingBox();  // computed once and kept: the selector asks for it on the UI thread otherwise
        p->edgeSensitives[size_t(index)] = sensitive;
      }
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
  } else if (!p->navigation.IsNull()) {
    // Any other meshed body picks as a whole (the Body filter, which every display activates) through the same set: OCCT
    // builds a triangulation sensitive and its BVH per face on the UI thread, 50-90 ms for a heavy Engine part (UI-40).
    // A face without a mesh or a vertex of its own keeps the stock path.
    bool meshed = !TopExp_Explorer(meshedProto, TopAbs_VERTEX, TopAbs_EDGE).More();
    for (TopExp_Explorer f(meshedProto, TopAbs_FACE); f.More() && meshed; f.Next()) {
      TopLoc_Location at;
      meshed = !BRep_Tool::Triangulation(TopoDS::Face(f.Current()), at).IsNull();
    }
    if (meshed) {
      TopTools_IndexedDataMapOfShapeListOfShape faces;
      TopExp::MapShapesAndAncestors(meshedProto, TopAbs_EDGE, TopAbs_FACE, faces);
      const double span = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
      std::vector<gp_Pnt> loose;
      for (int i = 1; i <= edges.Extent(); ++i) {
        const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
        if (BRep_Tool::Degenerated(edge) || (faces.Contains(edge) && !faces.FindFromKey(edge).IsEmpty())) continue;
        const auto samples = curveSamples(edge, std::max(1e-6, span * 1e-5));
        for (size_t j = 1; j < samples.size(); ++j) { loose.push_back(samples[j - 1]); loose.push_back(samples[j]); }
      }
      p->whole.push_back(p->navigation);
      if (!loose.empty()) {
        Handle(SegmentSet) set = new SegmentSet(std::move(loose));
        set->BVH();
        p->whole.push_back(set);
      }
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
  if (mode != AIS_Shaded || !shown || shown->triangles.IsNull()) {
    AIS_Shape::Compute(mgr, prs, mode);  // wireframe/HLR, or nothing precomputed: the stock path
    return;
  }
  // Min/max are supplied from the worker's box; evaluating them here walks every vertex on the UI thread.
  const bool haveBox = !shown->box.IsVoid();
  double x0 = 0, y0 = 0, z0 = 0, x1 = 0, y1 = 0, z1 = 0;
  if (haveBox) shown->box.Get(x0, y0, z0, x1, y1, z1);
  Handle(Graphic3d_Group) g = prs->NewGroup();
  g->SetClosed(shown->closed);
  g->SetGroupPrimitivesAspect(myDrawer->ShadingAspect()->Aspect());
  // Ray intersections do not use raster depth offsets. Separate only the render
  // skin along its normals; the analytic shape, selection and exports stay exact.
  if (m_rayBias!=0 && m_rayTriangles.IsNull()) {
    const auto& src=shown->triangles;
    m_rayTriangles=new Graphic3d_ArrayOfTriangles(src->VertexNumber(),src->EdgeNumber(),true);
    for(int i=1;i<=src->VertexNumber();++i) {
      const gp_Dir n=src->VertexNormal(i);
      m_rayTriangles->AddVertex(src->Vertice(i).Translated(gp_Vec(n)*m_rayBias),n);
    }
    for(int i=1;i<=src->EdgeNumber();++i) m_rayTriangles->AddEdge(src->Edge(i));
  }
  g->AddPrimitiveArray(m_rayTriangles.IsNull()?shown->triangles:m_rayTriangles, !haveBox);
  if (haveBox) g->SetMinMaxValues(x0, y0, z0, x1, y1, z1);
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
// A curve body (sketch wire, drawing layer) hovered as a whole: its lines as wide as a hovered edge's, in white over the
// rim on a light background (HoverLines); the stock hover alone kept the body's thin lines.
class BodySelectionOwner : public StdSelect_BRepOwner {
 public:
  BodySelectionOwner(const TopoDS_Shape& shape,const Handle(SelectMgr_SelectableObject)& body,int priority)
      : StdSelect_BRepOwner(shape,body,priority,false) {}
  void HilightWithColor(const Handle(PrsMgr_PresentationManager)& pm,const Handle(Prs3d_Drawer)& style,Standard_Integer mode) override {
    if(!pm->IsImmediateModeOn()) return;
    const auto body=Handle(BodyShape)::DownCast(Selectable());
    if(body.IsNull() || !body->curveOnly()) return StdSelect_BRepOwner::HilightWithColor(pm,style,mode);
    const HoverLines& look=HoverLines::current();
    if(m_core.IsNull()) m_core=new HoverLinesPrs(body->prs()->boundaries,look.coreWidth);
    if(!look.rim.IsNull()) {
      const float width=float(look.rim->WireAspect()->Aspect()->Width());
      if(m_rim.IsNull() || m_rimWidth!=width) m_rim=new HoverLinesPrs(body->prs()->boundaries,m_rimWidth=width);
      hoverAlike(pm,m_rim,look.rim,style,body,body->Transformation());
    }
    StdSelect_BRepOwner::HilightWithColor(pm,style,mode);
    hoverAlike(pm,m_core,style,style,body,body->Transformation());
  }
  void Unhilight(const Handle(PrsMgr_PresentationManager)& pm,const Standard_Integer mode) override {
    StdSelect_BRepOwner::Unhilight(pm,mode);
    for(const auto& prs:{m_rim,m_core}) if(!prs.IsNull()) pm->Unhighlight(prs);
  }
 private:
  Handle(PrsMgr_PresentableObject) m_rim,m_core;
  float m_rimWidth=0;
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
  computeSubShapes(selection, mode);
  if (!m_prs || m_prs->navigation.IsNull() || (mode != AIS_Shape::SelectionMode(TopAbs_EDGE) && mode != AIS_Shape::SelectionMode(TopAbs_VERTEX))) return;
  // One pixel: the faces block only what lies behind them; an edge on a face keeps its own depth tolerance, so it wins.
  Handle(OccluderSensitive) faces = new OccluderSensitive(new OccluderOwner(this), m_prs->navigation);
  faces->SetSensitivityFactor(1);
  selection->Add(faces);
}

void BodyShape::computeSubShapes(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode) {
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
  if(mode==AIS_Shape::SelectionMode(TopAbs_EDGE) && groupedEdges()) {  // a big drawing layer: one sensitive per group (UI-42)
    for(const auto& group:m_prs->edgeGroups) selection->Add(new EdgeGroupSensitive(this,group));
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
  if (mode == 0 && m_prs && !m_prs->whole.empty()) {  // a meshed body: its picking was built on the worker
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

// The edges in groups of kEdgeGroup lying near each other (the Morton order of their centres in the body's box), so a
// group's box stays small and a pick tests few of them.
void BodyPrs::buildEdgeGroups(const TopTools_IndexedMapOfShape& edges, const Bnd_Box& bounds) {
  edgeShapes.reserve(size_t(edges.Extent()));
  for (int i = 1; i <= edges.Extent(); ++i) edgeShapes.push_back(edges(i));
  double x0 = 0, y0 = 0, z0 = 0, x1 = 1, y1 = 1, z1 = 1;
  if (!bounds.IsVoid()) bounds.Get(x0, y0, z0, x1, y1, z1);
  auto spread = [](uint64_t v) {  // 10 bits, two zeros after each
    v &= 0x3ff;
    v = (v | v << 16) & 0x30000ff;
    v = (v | v << 8) & 0x300f00f;
    v = (v | v << 4) & 0x30c30c3;
    return (v | v << 2) & 0x9249249;
  };
  auto cell = [](double v, double lo, double hi) { return uint64_t(std::clamp((v - lo) / std::max(hi - lo, 1e-12), 0.0, 1.0) * 1023); };
  std::vector<std::pair<uint64_t, int>> order;
  for (const auto& [index, points] : curves) {
    if (points->size() < 2) continue;
    const gp_Pnt& a = points->front();
    const gp_Pnt& b = (*points)[points->size() / 2];
    const gp_XYZ c = (a.XYZ() + b.XYZ()) / 2;
    order.emplace_back(spread(cell(c.X(), x0, x1)) | spread(cell(c.Y(), y0, y1)) << 1 | spread(cell(c.Z(), z0, z1)) << 2, index);
  }
  std::sort(order.begin(), order.end());
  for (size_t from = 0; from < order.size(); from += kEdgeGroup) {
    std::vector<gp_Pnt> pairs;
    std::vector<int> edgeOf;
    std::vector<EdgeGroupSet::Edge> group;
    for (size_t k = from; k < std::min(order.size(), from + kEdgeGroup); ++k) {
      const auto& points = *curves.at(order[k].second);
      EdgeGroupSet::Edge e{order[k].second, int(pairs.size() / 2), int(points.size() - 1), {}};
      for (size_t j = 1; j < points.size(); ++j) {
        pairs.push_back(points[j - 1]);
        pairs.push_back(points[j]);
        edgeOf.push_back(int(group.size()));
      }
      for (const auto& q : points) e.box.Add(SelectMgr_Vec3(q.X(), q.Y(), q.Z()));
      group.push_back(e);
    }
    Handle(EdgeGroupSet) set = new EdgeGroupSet(std::move(pairs), std::move(edgeOf), std::move(group));
    set->BVH();
    edgeGroups.push_back(set);
  }
}

EdgeGroupSensitive::EdgeGroupSensitive(BodyShape* body, const Handle(Select3D_SensitiveEntity)& group)
    : Select3D_SensitiveEntity(body->edgeOwner(Handle(EdgeGroupSet)::DownCast(group)->firstOrdinal())), m_body(body), m_group(group) {
  SetSensitivityFactor(group->SensitivityFactor());
}

Standard_Boolean EdgeGroupSensitive::Matches(SelectBasics_SelectingVolumeManager& mgr, SelectBasics_PickResult& result) {
  m_hits.clear();
  auto& group = static_cast<EdgeGroupSet&>(*m_group);
  if (mgr.GetActiveSelectionType() == SelectMgr_SelectionType_Point) {
    const int edge = group.pick(mgr, result);
    if (edge < 0) return false;
    Set(m_body->edgeOwner(edge));  // the selector takes the owner after this
    return true;
  }
  group.take(mgr, m_hits);
  if (m_hits.empty()) return false;
  Set(m_body->edgeOwner(m_hits.front()));
  return true;
}

Handle(SubShapeOwner) BodyShape::edgeOwner(int index) {
  if (!m_prs || index < 0 || size_t(index) >= m_prs->edgeShapes.size()) return nullptr;
  if (m_edgeOwners.empty()) m_edgeOwners.resize(m_prs->edgeShapes.size());
  Handle(SubShapeOwner)& owner = m_edgeOwners[size_t(index)];
  if (owner.IsNull()) {
    owner = new SubShapeOwner(m_prs->edgeShapes[size_t(index)], this, 7, index);
    if (const auto curve = m_prs->curves.find(index); curve != m_prs->curves.end()) owner->curve = curve->second;
  }
  return owner;
}

void NavigationShape::ComputeSelection(const Handle(SelectMgr_Selection)& selection, Standard_Integer) {
  if (!m_prototype.IsNull()) selection->Add(new SharedSensitive(new SelectMgr_EntityOwner(this), m_prototype));
}
