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
#include <Quantity_Color.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <Select3D_SensitiveEntity.hxx>
#include <memory>
#include <map>
#include <vector>

inline Quantity_Color selectionTint() { return Quantity_Color(0.70,0.70,0.70,Quantity_TOC_sRGB); }

// Per body-store key; shared by every instance of that body. Built off the UI thread.
struct BodyPrs {
  Handle(Graphic3d_ArrayOfTriangles) triangles;
  Handle(Graphic3d_ArrayOfSegments) boundaries;  // face boundaries, for the shaded-with-edges style
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
  bool closed = false;                           // closed solid: back faces can be culled
  std::vector<gp_Pnt> drawingSegments; // sampled pairs for drawing-only orbit fallback
  Bnd_Box box;                                   // of the prototype; spares Display() a pass over every vertex
  double deflection = 0;                         // chordal deflection the triangles were meshed with (mm)
  // Worker thread; needs triangulation. `drawingOnly` skips what only picking uses (circles, navigation BVH, curves):
  // the zoom refinement's finer arrays are drawn, never picked.
  static std::shared_ptr<BodyPrs> build(const TopoDS_Shape& meshedProto, const Bnd_Box& box, bool drawingOnly = false);
  // Worker thread: how every view path meshes a body before build() (whole-model display, zoom refinement, previews):
  // BRepMesh at `deflection`, then cylinders and extrusions of any curve as upright strips (test_body_prs pins it).
  static opad::MeshingReport meshForDisplay(const TopoDS_Shape& shape, double deflection);
  size_t triangleCount() const;
};

class BodyShape : public AIS_Shape {
  DEFINE_STANDARD_RTTI_INLINE(BodyShape, AIS_Shape)
 public:
  BodyShape(const TopoDS_Shape& proto, std::shared_ptr<const BodyPrs> prs) : AIS_Shape(proto), m_prs(std::move(prs)) {}

 public:
  bool setRayBias(double offset) { if (offset==m_rayBias) return false; m_rayBias=offset; m_rayTriangles.Nullify(); SetToUpdate(); return true; }
  // A finer mesh of the same body for the current zoom (Viewport::refineVisible), or nullptr for the base one. Only
  // the drawn arrays change: picking, sub-shape ordinals and highlights keep using the prototype's own mesh.
  bool setDisplayPrs(std::shared_ptr<const BodyPrs> prs) { if (prs==m_display) return false; m_display=std::move(prs); m_rayTriangles.Nullify(); SetToUpdate(); return true; }
  const std::shared_ptr<const BodyPrs>& displayPrs() const { return m_display; }
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Presentation)& prs, const Standard_Integer mode) override;
  // Sub-shape modes: the stock owners are swapped for SubShapeOwner.
  void ComputeSelection(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode) override;

 private:
  std::shared_ptr<const BodyPrs> m_prs, m_display;
  double m_rayBias=0;
  Handle(Graphic3d_ArrayOfTriangles) m_rayTriangles;
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
};

// Circular rims discover and select a stable center reference in vertex mode.
class CircleOwner : public SubShapeOwner {
  DEFINE_STANDARD_RTTI_INLINE(CircleOwner, SubShapeOwner)
 public:
  CircleOwner(const BodyPrs::Circle& circle, const Handle(SelectMgr_SelectableObject)& body, int index)
      : SubShapeOwner(circle.edge, body, 12, index), center(circle.center) {}
  gp_Pnt center;
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
  explicit SubHighlight(const Quantity_Color& color) : m_color(color) {}
  std::vector<Handle(Graphic3d_ArrayOfTriangles)> m_triangles;
  std::vector<Handle(Graphic3d_ArrayOfSegments)> m_segments;
  std::vector<Handle(Graphic3d_ArrayOfPoints)> m_points;

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Presentation)& prs, const Standard_Integer mode) override;
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  Quantity_Color m_color;
};
