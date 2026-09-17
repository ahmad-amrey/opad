#include "NavCube.hpp"

#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_Presentation.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <Standard_Version.hxx>
#include <V3d.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>

#include <cmath>

NavCube::NavCube() {
  SetDrawEdges(Standard_False);     // the bevel facets are what the base draws for edges and corners;
  SetDrawVertices(Standard_False);  // this cube has none, its edge/corner parts exist only for picking
}

// One quad on the face whose outward normal is `axis` with `sign`, spanning lo..hi on the other two axes.
void NavCube::addFaceQuad(const Handle(Graphic3d_ArrayOfTriangles)& tris, Standard_Integer& nbNodes, Standard_Integer& nbTris, int axis, double sign, const double lo[3], const double hi[3]) const {
  nbNodes += 4;
  nbTris += 2;
  if (tris.IsNull()) return;
  const int b = (axis + 1) % 3, c = (axis + 2) % 3;
  double p[4][3];
  for (int i = 0; i < 4; ++i) {
    p[i][axis] = sign * Size() * 0.5;
    p[i][b] = (i == 1 || i == 2) ? hi[b] : lo[b];
    p[i][c] = (i >= 2) ? hi[c] : lo[c];
  }
  double n[3] = {0, 0, 0};
  n[axis] = sign;
  const gp_Dir normal(n[0], n[1], n[2]);
  const Standard_Integer first = tris->VertexNumber();
  for (int i = 0; i < 4; ++i) tris->AddVertex(gp_Pnt(p[i][0], p[i][1], p[i][2]), normal);
  // Wind the triangles so their geometric normal agrees with the face normal.
  const gp_XYZ e1 = gp_XYZ(p[1][0], p[1][1], p[1][2]) - gp_XYZ(p[0][0], p[0][1], p[0][2]);
  const gp_XYZ e2 = gp_XYZ(p[2][0], p[2][1], p[2][2]) - gp_XYZ(p[0][0], p[0][1], p[0][2]);
  const bool flip = e1.Crossed(e2).Dot(normal.XYZ()) < 0.0;
  if (!flip) {
    tris->AddTriangleEdges(first + 1, first + 2, first + 3);
    tris->AddTriangleEdges(first + 1, first + 3, first + 4);
  } else {
    tris->AddTriangleEdges(first + 1, first + 3, first + 2);
    tris->AddTriangleEdges(first + 1, first + 4, first + 3);
  }
}

void NavCube::partTriangles(const Handle(Graphic3d_ArrayOfTriangles)& tris, Standard_Integer& nbNodes, Standard_Integer& nbTris, V3d_TypeOfOrientation dir) const {
  if (IsBoxSide(dir)) {
    createBoxPartTriangles(tris, nbNodes, nbTris, dir);
    return;
  }
  const gp_Dir d = V3d::GetProjAxis(dir);
  const double comp[3] = {d.X(), d.Y(), d.Z()};
  const double h = Size() * 0.5;
  const bool corner = IsBoxCorner(dir);
  const double w = Size() * (corner ? 0.22 : 0.16);  // band width on each face
  for (int a = 0; a < 3; ++a) {
    if (std::fabs(comp[a]) < 0.1) continue;  // not one of this part's faces
    const double sa = comp[a] > 0 ? 1.0 : -1.0;
    double lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
      if (k == a) { lo[k] = hi[k] = sa * h; continue; }
      if (std::fabs(comp[k]) < 0.1) { lo[k] = -h; hi[k] = h; continue; }  // the edge runs along this axis
      const double sk = comp[k] > 0 ? 1.0 : -1.0;  // the band hugs this axis's face
      lo[k] = std::min(sk * (h - w), sk * h);
      hi[k] = std::max(sk * (h - w), sk * h);
    }
    addFaceQuad(tris, nbNodes, nbTris, a, sa, lo, hi);
  }
}

// As AIS_ViewCube::ComputeSelection, with the band geometry and owner priorities.
void NavCube::ComputeSelection(const Handle(SelectMgr_Selection)& selection, const Standard_Integer mode) {
  if (mode != 0) return;
  for (Standard_Integer part = 0; part <= Standard_Integer(V3d_XnegYnegZneg); ++part) {
    const V3d_TypeOfOrientation ori = static_cast<V3d_TypeOfOrientation>(part);
    Standard_Integer nbNodes = 0, nbTris = 0;
    partTriangles(Handle(Graphic3d_ArrayOfTriangles)(), nbNodes, nbTris, ori);
    if (nbNodes <= 0) continue;
    Handle(Graphic3d_ArrayOfTriangles) tris = new Graphic3d_ArrayOfTriangles(nbNodes, nbTris * 3, Graphic3d_ArrayFlags_None);
    nbNodes = nbTris = 0;
    partTriangles(tris, nbNodes, nbTris, ori);
    Standard_Integer sensitivity = 2, priority = 5;
    if (IsBoxCorner(ori)) { sensitivity = 8; priority = 7; }
    else if (IsBoxEdge(ori)) { sensitivity = 4; priority = 6; }
    Handle(AIS_ViewCubeOwner) owner = new AIS_ViewCubeOwner(this, ori, priority);
    Handle(AIS_ViewCubeSensitive) sens = new AIS_ViewCubeSensitive(owner, tris);
    sens->SetSensitivityFactor(sensitivity);
    selection->Add(sens);
  }
}

// As AIS_ViewCube::HilightOwnerWithColor, drawing the band geometry for edges and corners. The fill lies in
// the face plane; the style's shading aspect carries a polygon offset (set in Viewport) so it wins the depth
// test against the face instead of fighting it.
void NavCube::HilightOwnerWithColor(const Handle(PrsMgr_PresentationManager)& mgr, const Handle(Prs3d_Drawer)& style, const Handle(SelectMgr_EntityOwner)& owner) {
  if (owner.IsNull() || !mgr->IsImmediateModeOn()) return;
  const AIS_ViewCubeOwner* cubeOwner = dynamic_cast<AIS_ViewCubeOwner*>(owner.get());
  if (!cubeOwner) return;
  const Graphic3d_ZLayerId layer = style->ZLayer() != Graphic3d_ZLayerId_UNKNOWN ? style->ZLayer() : myDrawer->ZLayer();
  Handle(Prs3d_Presentation) prs = GetHilightPresentation(mgr);
  prs->Clear();
#if OCC_VERSION_HEX >= 0x070700
  prs->CStructure()->ViewAffinity = ViewAffinity();
#else
  prs->CStructure()->ViewAffinity = mgr->StructureManager()->ObjectAffinity(Handle(Standard_Transient)(this));
#endif
  prs->SetTransformPersistence(TransformPersistence());
  prs->SetZLayer(layer);
  Handle(Graphic3d_Group) group = prs->NewGroup();
  group->SetGroupPrimitivesAspect(style->ShadingAspect()->Aspect());
  Standard_Integer nbNodes = 0, nbTris = 0;
  partTriangles(Handle(Graphic3d_ArrayOfTriangles)(), nbNodes, nbTris, cubeOwner->MainOrientation());
  if (nbNodes > 0) {
    Handle(Graphic3d_ArrayOfTriangles) tris = new Graphic3d_ArrayOfTriangles(nbNodes, nbTris * 3, Graphic3d_ArrayFlags_None);
    nbNodes = nbTris = 0;
    partTriangles(tris, nbNodes, nbTris, cubeOwner->MainOrientation());
    group->AddPrimitiveArray(tris);
  }
  mgr->AddToImmediateList(prs);
}
