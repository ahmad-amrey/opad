#pragma once
// The navigation cube: a plain cube (no bevels) whose edges and corners are still hover/click targets.
// AIS_ViewCube gives edges and corners their own bevel facets, which are both what it draws and what it
// picks; with the bevels removed those parts vanish as targets. This subclass keeps the sharp cube and
// describes each edge as a band across its two faces and each corner as a square on its three faces, used
// only for picking and for the hover fill (nothing extra is drawn on the cube itself, so nothing fights
// the faces for depth).
#include <AIS_ViewCube.hxx>

class NavCube : public AIS_ViewCube {
  DEFINE_STANDARD_RTTI_INLINE(NavCube, AIS_ViewCube)
 public:
  NavCube();

 protected:
  // Base ComputeSelection/Compute path: sides only (edge and corner drawing is off).
  void ComputeSelection(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode) override;
  void HilightOwnerWithColor(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Drawer)& style, const Handle(SelectMgr_EntityOwner)& owner) override;

 private:
  // Sides from the base geometry; edges and corners as bands/squares in the face planes. Picking gives them
  // priority corner > edge > side, since coplanar entities tie on depth.
  void partTriangles(const Handle(Graphic3d_ArrayOfTriangles)& tris, Standard_Integer& nbNodes, Standard_Integer& nbTris, V3d_TypeOfOrientation dir) const;
  void addFaceQuad(const Handle(Graphic3d_ArrayOfTriangles)& tris, Standard_Integer& nbNodes, Standard_Integer& nbTris, int axis, double sign, const double lo[3], const double hi[3]) const;
};
