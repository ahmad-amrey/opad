#pragma once
#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <TopoDS_Edge.hxx>
#include <vector>
#include <algorithm>

// The polyline an edge is drawn with, in its shape's frame: its nodes on the faces' mesh, else its own polygon, else
// a coarse sampling of the curve (unmeshed).
inline std::vector<gp_Pnt> edgePolyline(const TopoDS_Edge& e) {
  std::vector<gp_Pnt> line;
  TopLoc_Location loc;
  Handle(Poly_PolygonOnTriangulation) poly;
  Handle(Poly_Triangulation) t;
  BRep_Tool::PolygonOnTriangulation(e, poly, t, loc);
  if (!poly.IsNull() && !t.IsNull()) {
    for (int n = 1; n <= poly->NbNodes(); ++n) line.push_back(t->Node(poly->Node(n)));
  } else if (Handle(Poly_Polygon3D) p3 = BRep_Tool::Polygon3D(e, loc); !p3.IsNull()) {
    for (int n = 1; n <= p3->NbNodes(); ++n) line.push_back(p3->Nodes().Value(n));
  } else if (!BRep_Tool::Degenerated(e)) {
    loc = TopLoc_Location();
    BRepAdaptor_Curve c(e);
    constexpr int kSamples = 24;
    for (int n = 0; n <= kSamples; ++n) line.push_back(c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * n / kSamples));
  }
  if (!loc.IsIdentity()) for (auto& p : line) p.Transform(loc.Transformation());
  return line;
}

// Shared tessellation for drawing, picking and highlighting exact wire curves.
inline std::vector<gp_Pnt> curveSamples(const TopoDS_Edge& edge,double deflection) {
  BRepAdaptor_Curve curve(edge);
  if(curve.GetType()==GeomAbs_Line) return {curve.Value(curve.FirstParameter()),curve.Value(curve.LastParameter())};
  GCPnts_QuasiUniformDeflection points(curve,std::max(deflection,1e-7));
  std::vector<gp_Pnt> out;
  if(points.IsDone()) {
    out.reserve(points.NbPoints());
    for(int i=1;i<=points.NbPoints();++i) out.push_back(points.Value(i));
  }
  return out;
}
