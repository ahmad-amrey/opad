#pragma once
// AIS_Shape whose shaded presentation comes from primitive arrays built on the mesh worker. Displaying a
// plain AIS_Shape walks the triangulation (and its face boundaries) on the UI thread, which takes hundreds
// of milliseconds for a heavy body; here Display() only hands the ready arrays to the graphic driver.
#include <AIS_Shape.hxx>
#include "opad/util.hpp"
#include "opad/mesh.hpp"
#include <Bnd_Box.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_ArrayOfPoints.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Quantity_Color.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <Select3D_SensitiveEntity.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <memory>
#include <map>
#include <vector>

// How SubHighlight draws a selection (UI-38, app/Highlight.hpp in OCCT terms): a tint over the faces, the edges' core
// line over a wider halo, vertex markers over a halo marker. Widths in device pixels.
struct GlowStyle {
  Quantity_Color fill{0.30, 0.61, 1.0, Quantity_TOC_sRGB}, edge{0.30, 0.61, 1.0, Quantity_TOC_sRGB}, halo{0.19, 0.38, 0.62, Quantity_TOC_sRGB};
  float fillAlpha = 0.4f, edgeWidth = 3, haloAlpha = 1, haloWidth = 7, point = 3, pointHalo = 7;
};

// The hover on lines (UI-38, set by Viewport::applyTokens): a curve body (sketch wire, drawing layer) hovered as a whole
// gets a white core as wide as a hovered edge, and on a light background a hovered edge or curve body gets a darker rim
// under its white line, which reads there where white alone hardly does. The owners read it while hovering (UI thread).
struct HoverLines {
  Handle(Prs3d_Drawer) rim;  // null: no rim (dark theme); else its colour and its wide WireAspect
  float coreWidth = 3;       // device pixels (highlight::kHoverEdgeWidth)
  static HoverLines& current();
};

// Per body-store key; shared by every instance of that body. Built off the UI thread.
struct BodyPrs {
  Handle(Graphic3d_ArrayOfTriangles) triangles;
  Handle(Graphic3d_ArrayOfSegments) boundaries;  // face boundaries, for the shaded-with-edges style
  // Edges of no face: drawn beside faces (a drawing layer's lines next to its fills) and, with the boundaries, the
  // wireframe of a meshed body (UI-48).
  Handle(Graphic3d_ArrayOfSegments) freeEdges;
  Handle(Graphic3d_ArrayOfPoints) loosePoints;
  struct Circle {
    TopoDS_Shape edge;
    gp_Pnt center;
    Handle(Select3D_SensitiveEntity) sensitive;
    int canonical = -1;
    double radius=0;
    int segments=0;
    std::vector<int> meshEdges;
  };
  std::map<int,std::shared_ptr<const std::vector<gp_Pnt>>> curves;
  std::map<int, Circle> circles;  // edge ordinals, including trimmed circular arcs
  Handle(Select3D_SensitiveEntity) navigation;  // triangles + BVH, shared by instances
  // Picking built here rather than by OCCT on the UI thread: the whole body (selection mode 0: its triangles and free
  // edges; any meshed body without unmeshed faces or lone vertices) and, for a big body, each edge by ordinal (the Edge
  // filter; empty otherwise).
  std::vector<Handle(Select3D_SensitiveEntity)> whole, edgeSensitives;
  // A big drawing layer's Edge and Vertex filters instead (UI-42): groups of up to a few hundred edges or vertices lying
  // near each other, one sensitive each (GroupSensitive), and the edges and vertices by ordinal for the owners made as
  // they are picked. OCCT builds an object's picking BVH over its sensitives on the UI thread when a mode is activated:
  // 0.5-1 s for 100,000 edges, and the stock Vertex filter of a DWG's text layer took 0.9 s a layer.
  std::vector<Handle(Select3D_SensitiveEntity)> edgeGroups, vertexGroups;
  std::vector<TopoDS_Shape> edgeShapes, vertexShapes;
  void buildEdgeGroups(const TopTools_IndexedMapOfShape& edges, const Bnd_Box& box);        // worker: edgeGroups and edgeShapes
  void buildVertexGroups(const TopTools_IndexedMapOfShape& vertices, const Bnd_Box& box);  // worker: vertexGroups and vertexShapes
  bool closed = false;                           // closed solid: back faces can be culled
  std::vector<gp_Pnt> drawingSegments; // sampled pairs for drawing-only orbit fallback
  // The segments in runs lying near each other (by the Morton order of their middles), 256 a run, each run's box: the orbit
  // pivot looks for the curve nearest the pointer run by run, nearest box first, instead of projecting every segment of a
  // big drawing on every press (UI-51). segmentOrder holds segment numbers (pair i is drawingSegments[2i], [2i+1]).
  struct SegmentRun { Bnd_Box box; size_t first = 0, count = 0; };
  std::vector<uint32_t> segmentOrder;
  std::vector<SegmentRun> segmentRuns;
  void buildSegmentRuns();  // worker
  Bnd_Box box;                                   // of the prototype; spares Display() a pass over every vertex
  double deflection = 0;                         // chordal deflection the triangles were meshed with (mm)
  // Faces the file coloured otherwise than the body (opad::FaceColors, UI-74) are drawn as one group per colour, `own`
  // holding the faces in the body's colour; `triangles` still has every face for picking, glows and highlights.
  struct Painted { Quantity_Color color; Handle(Graphic3d_ArrayOfTriangles) triangles; };
  std::vector<Painted> painted;
  Handle(Graphic3d_ArrayOfTriangles) own;
  std::shared_ptr<const opad::FaceColors> faceColors;  // what `painted` was made from (the zoom refinement uses it again)
  // Worker thread; needs triangulation. `drawingOnly` skips what only picking uses (circles, navigation BVH, curves):
  // the zoom refinement's finer arrays are drawn, never picked. `drawing`: a drawing layer, whose edges are picked in
  // groups when there are many (edgeGroups). `faceColors`: faces the file coloured otherwise (painted, own).
  static std::shared_ptr<BodyPrs> build(const TopoDS_Shape& meshedProto, const Bnd_Box& box, bool drawingOnly = false, bool drawing = false,
                                        std::shared_ptr<const opad::FaceColors> faceColors = {});
  static std::shared_ptr<BodyPrs> build(const TopoDS_Shape& meshedProto, const Bnd_Box& box, bool drawingOnly,
                                        std::shared_ptr<const opad::FaceColors> faceColors, bool drawing = false) {
    return build(meshedProto, box, drawingOnly, drawing, std::move(faceColors));
  }
  // Worker thread: how every view path meshes a body before build() (whole-model display, zoom refinement, previews):
  // BRepMesh at `deflection`, then cylinders and extrusions of any curve as upright strips (test_body_prs pins it).
  static opad::MeshingReport meshForDisplay(const TopoDS_Shape& shape, double deflection);
  size_t triangleCount() const;
};

// Owner of one face, edge or vertex of a BodyShape. It knows its ordinal within the body and leaves the
// selected highlight to the viewport (SubHighlight); only the hover highlight goes through OCCT.
class SubShapeOwner : public StdSelect_BRepOwner {
  DEFINE_STANDARD_RTTI_INLINE(SubShapeOwner, StdSelect_BRepOwner)
 public:
  SubShapeOwner(const TopoDS_Shape& sub, const Handle(SelectMgr_SelectableObject)& body, int priority, int index)
      : StdSelect_BRepOwner(sub, body, priority, Standard_True), m_index(index) {}
  virtual void prepare() {}
  virtual opad::Ref::Kind kind() const {
    return myShape.IsNull()?opad::Ref::Kind::Body:myShape.ShapeType()==TopAbs_FACE?opad::Ref::Kind::Face:myShape.ShapeType()==TopAbs_EDGE?opad::Ref::Kind::Edge:opad::Ref::Kind::Vertex;
  }
  std::shared_ptr<const std::vector<gp_Pnt>> curve;
  int index() const { return m_index; }  // as opad::subshape_index: 0-based, -1 when unknown

  void HilightWithColor(const Handle(PrsMgr_PresentationManager)& pm, const Handle(Prs3d_Drawer)& style, const Standard_Integer mode) override;
  void Unhilight(const Handle(PrsMgr_PresentationManager)& pm, const Standard_Integer mode) override;

 private:
  int m_index;
  Handle(PrsMgr_PresentableObject) m_rim;  // the edge drawn wide in the rim colour under the hover (HoverLines)
};

class BodyShape : public AIS_Shape {
  DEFINE_STANDARD_RTTI_INLINE(BodyShape, AIS_Shape)
 public:
  BodyShape(const TopoDS_Shape& proto, std::shared_ptr<const BodyPrs> prs) : AIS_Shape(proto), m_prs(std::move(prs)) {}
  // The owner of edge or vertex `index` of a big drawing layer in the Edge or Vertex filter (BodyPrs::edgeGroups,
  // vertexGroups), made the first time it is picked and kept, so a pick of it again finds the same one (a second click
  // takes it out). Null for any other body.
  Handle(SubShapeOwner) edgeOwner(int index) { return groupOwner(TopAbs_EDGE, index); }
  Handle(SubShapeOwner) vertexOwner(int index) { return groupOwner(TopAbs_VERTEX, index); }
  Handle(SubShapeOwner) groupOwner(TopAbs_ShapeEnum type, int index);
  bool groupedEdges() const { return m_prs && !m_prs->edgeGroups.empty(); }
  bool groupedVertices() const { return m_prs && !m_prs->vertexGroups.empty(); }

 public:
  bool setRayBias(double offset) { if (offset==m_rayBias) return false; m_rayBias=offset; m_rayTriangles.clear(); SetToUpdate(); return true; }
  // A finer mesh of the same body for the current zoom (Viewport::refineVisible), or nullptr for the base one. Only
  // the drawn arrays change: picking, sub-shape ordinals and highlights keep using the prototype's own mesh.
  bool setDisplayPrs(std::shared_ptr<const BodyPrs> prs) { if (prs==m_display) return false; m_display=std::move(prs); m_rayTriangles.clear(); SetToUpdate(); return true; }
  const std::shared_ptr<const BodyPrs>& displayPrs() const { return m_display; }
  const std::shared_ptr<const BodyPrs>& prs() const { return m_prs; }
  bool curveOnly() const { return m_prs && m_prs->triangles.IsNull() && !m_prs->boundaries.IsNull(); }  // a wire or drawing layer
  // Hidden line (UI-48): the faces in `face` (the background), unlit, outlined in `edge` where they turn away from the eye
  // (OCCT's silhouette), under the edges. True when that changes the shaded presentation (the caller has it computed again).
  bool setHiddenLine(bool on, const Quantity_Color& face, const Quantity_Color& edge);
  bool hiddenLine() const { return m_hiddenLine; }
  static int stockWireframes();  // wireframes OCCT computed from the shape on the UI thread so far (benches: none for meshed bodies)
  // The face colours of a body drawn without worker arrays (placed through a non-rigid transform: OCCT's own shaded path).
  void setFaceColors(std::shared_ptr<const opad::FaceColors> colors) { m_faceColors = std::move(colors); SetToUpdate(); }
  // Face colours are drawn through copies of the fill aspect, which SetTransparency and SynchronizeAspects never reach: they
  // take the body's look again here (its opacity; a ghost's colour too, `uniform`), before SynchronizeAspects (applyLook).
  void syncPainted(bool uniform);
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Presentation)& prs, const Standard_Integer mode) override;
  // Sub-shape modes: the stock owners are swapped for SubShapeOwner. The Edge and Vertex modes also hold the body's
  // faces under an OccluderOwner (UI-31).
  void ComputeSelection(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode) override;

 private:
  void computeSubShapes(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode);
  std::shared_ptr<const BodyPrs> m_prs, m_display;
  std::shared_ptr<const opad::FaceColors> m_faceColors;
  std::vector<std::pair<Handle(Graphic3d_AspectFillArea3d), Quantity_Color>> m_painted;  // each face colour's aspect as drawn
  bool m_uniform = false;
  Handle(Graphic3d_AspectFillArea3d) paintedAspect(const Quantity_Color& color);
  double m_rayBias=0;
  bool m_hiddenLine=false;
  Quantity_Color m_hiddenFace, m_hiddenEdge;
  std::map<const Graphic3d_ArrayOfTriangles*, Handle(Graphic3d_ArrayOfTriangles)> m_rayTriangles;  // drawn array -> its biased copy
  std::vector<Handle(SubShapeOwner)> m_edgeOwners, m_vertexOwners;  // by ordinal, as picked (grouped edges, vertices only)
};

// One group of a big drawing layer's edges or vertices in the Edge or Vertex filter of one body (UI-42): the worker's
// group (shared by the body's instances) under the owner of the edge or vertex it found, which it takes on in Matches
// (OCCT's selector reads the owner after Matches). A point pick finds the one nearest the pointer's ray (a drawing is
// flat: depths tie); a box or polygon keeps every one of the group it takes in hits() (an edge crossing: any part; window:
// all of it), for the box selection.
class GroupSensitive : public Select3D_SensitiveEntity {
  DEFINE_STANDARD_RTTI_INLINE(GroupSensitive, Select3D_SensitiveEntity)
 public:
  GroupSensitive(BodyShape* body, TopAbs_ShapeEnum type, const Handle(Select3D_SensitiveEntity)& group);
  Standard_Boolean Matches(SelectBasics_SelectingVolumeManager& mgr, SelectBasics_PickResult& result) override;
  Standard_Integer NbSubElements() const override { return m_group->NbSubElements(); }
  Select3D_BndBox3d BoundingBox() override { return m_group->BoundingBox(); }
  gp_Pnt CenterOfGeometry() const override { return m_group->CenterOfGeometry(); }
  Standard_Boolean ToBuildBVH() const override { return false; }
  BodyShape* body() const { return m_body; }
  TopAbs_ShapeEnum type() const { return m_type; }
  const std::vector<int>& hits() const { return m_hits; }  // ordinals the last box or polygon test took
  Handle(SubShapeOwner) owner(int index) const { return m_body->groupOwner(m_type, index); }
 private:
  BodyShape* m_body;  // the body whose selection holds this (as an owner's selectable)
  TopAbs_ShapeEnum m_type;
  Handle(Select3D_SensitiveEntity) m_group;
  std::vector<int> m_hits;
};

// Circular rims discover and select a stable center reference in vertex mode.
class CircleOwner : public SubShapeOwner {
  DEFINE_STANDARD_RTTI_INLINE(CircleOwner, SubShapeOwner)
 public:
  CircleOwner(const BodyPrs::Circle& circle, const Handle(SelectMgr_SelectableObject)& body, int index)
      : SubShapeOwner(circle.edge, body, 12, index), center(circle.center) {}
  gp_Pnt center;
};

// The faces of a body in the Edge and Vertex modes (UI-31): its navigation triangles under one owner that is never
// highlighted or kept selected. A face in front of an edge or vertex is picked before it, and the viewport takes that
// pick for nothing (Viewport::dropOccluded), so what is drawn behind a face cannot be hovered, clicked or tracked.
class OccluderOwner : public SelectMgr_EntityOwner {
  DEFINE_STANDARD_RTTI_INLINE(OccluderOwner, SelectMgr_EntityOwner)
 public:
  explicit OccluderOwner(const Handle(SelectMgr_SelectableObject)& body) : SelectMgr_EntityOwner(body, 0) {}
  void HilightWithColor(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Drawer)&, const Standard_Integer) override {}
  void Unhilight(const Handle(PrsMgr_PresentationManager)&, const Standard_Integer) override {}
  Standard_Boolean IsHilighted(const Handle(PrsMgr_PresentationManager)&, const Standard_Integer) const override { return Standard_False; }
};

// A cheap instance of a worker-built sensitive. Matches runs only on the UI thread; the shared
// prototype's traversal scratch state is never accessed concurrently. No per-instance BVH rebuild.
class SharedSensitive : public Select3D_SensitiveEntity {
  DEFINE_STANDARD_RTTI_INLINE(SharedSensitive, Select3D_SensitiveEntity)
 public:
  SharedSensitive(const Handle(SelectMgr_EntityOwner)& owner, const Handle(Select3D_SensitiveEntity)& prototype)
      : Select3D_SensitiveEntity(owner), m_prototype(prototype) { SetSensitivityFactor(prototype->SensitivityFactor()); }
  Standard_Boolean Matches(SelectBasics_SelectingVolumeManager& mgr, SelectBasics_PickResult& result) override { return m_prototype->Matches(mgr, result); }
  Standard_Integer NbSubElements() const override { return m_prototype->NbSubElements(); }
  Select3D_BndBox3d BoundingBox() override { return m_prototype->BoundingBox(); }
  gp_Pnt CenterOfGeometry() const override { return m_prototype->CenterOfGeometry(); }
  Standard_Boolean ToBuildBVH() const override { return false; }
 private:
  Handle(Select3D_SensitiveEntity) m_prototype;
};

// An OccluderOwner's faces: they stand in front only of a point pick (hover, click). A box takes no faces in the Edge and
// Vertex modes, and testing a window box for whole triangles of every body in it took seconds on a big assembly.
class OccluderSensitive : public SharedSensitive {
  DEFINE_STANDARD_RTTI_INLINE(OccluderSensitive, SharedSensitive)
 public:
  using SharedSensitive::SharedSensitive;
  Standard_Boolean Matches(SelectBasics_SelectingVolumeManager& mgr, SelectBasics_PickResult& result) override {
    return mgr.GetActiveSelectionType() == SelectMgr_SelectionType_Point && SharedSensitive::Matches(mgr, result);
  }
};

// Only registered with the navigation selector, never displayed or activated in the UI selection.
class NavigationShape : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(NavigationShape, AIS_InteractiveObject)
 public:
  explicit NavigationShape(const Handle(Select3D_SensitiveEntity)& prototype) : m_prototype(prototype) {}
  void BoundingBox(Bnd_Box& box) override {
    const auto bounds = m_prototype->BoundingBox();
    Bnd_Box local;
    local.Update(bounds.CornerMin().x(), bounds.CornerMin().y(), bounds.CornerMin().z(),
                 bounds.CornerMax().x(), bounds.CornerMax().y(), bounds.CornerMax().z());
    box = local.Transformed(Transformation());
  }
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)&, Standard_Integer) override {}
  void ComputeSelection(const Handle(SelectMgr_Selection)& selection, Standard_Integer) override;
 private:
  Handle(Select3D_SensitiveEntity) m_prototype;
};

// Every selected sub-shape in one object: a few primitive arrays in world coordinates, filled by
// Viewport::refreshSubHighlight from the bodies' existing meshes. Never pickable.
class SubHighlight : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(SubHighlight, AIS_InteractiveObject)
 public:
  explicit SubHighlight(const GlowStyle& style) : m_style(style) {}
  std::vector<Handle(Graphic3d_ArrayOfTriangles)> m_triangles;
  std::vector<Handle(Graphic3d_ArrayOfSegments)> m_segments;
  std::vector<Handle(Graphic3d_ArrayOfPoints)> m_points;
  const GlowStyle& style() const { return m_style; }

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Presentation)& prs, const Standard_Integer mode) override;
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  GlowStyle m_style;
};
