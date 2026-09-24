#pragma once
#include <BRepAdaptor_Curve.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopoDS_Edge.hxx>
#include <vector>
#include <algorithm>

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
