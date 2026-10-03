#pragma once
// The navigation cube: a plain cube (no bevels) whose edges and corners are still hover/click targets.
// AIS_ViewCube gives edges and corners their own bevel facets, which are both what it draws and what it
// picks; with the bevels removed those parts vanish as targets. This subclass keeps the sharp cube and
// describes each edge as a band across its two faces and each corner as a square on its three faces, used
// only for picking and for the hover fill (nothing extra is drawn on the cube itself, so nothing fights
// the faces for depth).
#include <AIS_ViewCube.hxx>
#include <AIS_AnimationCamera.hxx>

#include <utility>

// Keep the picked surface fixed throughout cube orientation animation, including
// when it projects away from the viewport center. Other camera animations are unchanged.
class OrbitCameraAnimation : public AIS_AnimationCamera {
  DEFINE_STANDARD_RTTI_INLINE(OrbitCameraAnimation, AIS_AnimationCamera)
 public:
  OrbitCameraAnimation(const Handle(V3d_View)& view) : AIS_AnimationCamera("ViewCamera", view) {}
  void setOrbitPoint(const Handle(Graphic3d_Camera)& start, const gp_Pnt& point) { m_start = start; m_point = point; }
 protected:
  void update(const AIS_AnimationProgress& progress) override;
 private:
  Handle(Graphic3d_Camera) m_start;
  gp_Pnt m_point;
};

class NavCube : public AIS_ViewCube {
  DEFINE_STANDARD_RTTI_INLINE(NavCube, AIS_ViewCube)
 public:
  NavCube();
  void setOrbitPoint(const gp_Pnt& point) { m_orbitPoint = point; }
  const gp_Pnt& orbitPoint() const { return m_orbitPoint; }
  // The side the view looks straight at (a V3d side orientation, -1: none) is drawn in the selection's role (UI-38), so a
  // standard view is told from one a little off it. True when that changed: recompute the presentation.
  bool setCurrentSide(int side) { return std::exchange(m_side, side) != side; }
  int currentSide() const { return m_side; }
  void setCurrentColor(const Quantity_Color& color) { m_sideColor = color; }
  void Compute(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Presentation)& prs, const Standard_Integer mode) override;

 protected:
  // A cube click changes orientation about the visible focus, preserving the user's zoom.
  void viewFitAll(const Handle(V3d_View)&, const Handle(Graphic3d_Camera)&) override;
  // Base ComputeSelection/Compute path: sides only (edge and corner drawing is off).
  void ComputeSelection(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode) override;
  void HilightOwnerWithColor(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Drawer)& style, const Handle(SelectMgr_EntityOwner)& owner) override;

 private:
  gp_Pnt m_orbitPoint;
  int m_side = -1;
  Quantity_Color m_sideColor{0.30, 0.61, 1.0, Quantity_TOC_sRGB};
  // Sides from the base geometry; edges and corners as bands/squares in the face planes. Picking gives them
  // priority corner > edge > side, since coplanar entities tie on depth.
  void partTriangles(const Handle(Graphic3d_ArrayOfTriangles)& tris, Standard_Integer& nbNodes, Standard_Integer& nbTris, V3d_TypeOfOrientation dir) const;
  void addFaceQuad(const Handle(Graphic3d_ArrayOfTriangles)& tris, Standard_Integer& nbNodes, Standard_Integer& nbTris, int axis, double sign, const double lo[3], const double hi[3]) const;
};
