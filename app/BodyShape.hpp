#pragma once
// AIS_Shape whose shaded presentation comes from primitive arrays built on the mesh worker. Displaying a
// plain AIS_Shape walks the triangulation (and its face boundaries) on the UI thread, which takes hundreds
// of milliseconds for a heavy body; here Display() only hands the ready arrays to the graphic driver.
#include <AIS_Shape.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <memory>

// Per body-store key; shared by every instance of that body. Built off the UI thread.
struct BodyPrs {
  Handle(Graphic3d_ArrayOfTriangles) triangles;
  Handle(Graphic3d_ArrayOfSegments) boundaries;  // face boundaries, for the shaded-with-edges style
  bool closed = false;                           // closed solid: back faces can be culled
  Bnd_Box box;                                   // of the prototype; spares Display() a pass over every vertex
  static std::shared_ptr<BodyPrs> build(const TopoDS_Shape& meshedProto, const Bnd_Box& box);  // worker thread; needs triangulation
};

class BodyShape : public AIS_Shape {
  DEFINE_STANDARD_RTTI_INLINE(BodyShape, AIS_Shape)
 public:
  BodyShape(const TopoDS_Shape& proto, std::shared_ptr<const BodyPrs> prs) : AIS_Shape(proto), m_prs(std::move(prs)) {}

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Presentation)& prs, const Standard_Integer mode) override;

 private:
  std::shared_ptr<const BodyPrs> m_prs;
};
