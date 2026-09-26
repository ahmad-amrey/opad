#pragma once
// Volume and area integrated span by span (gap log #4). OCCT's BRepGProp spreads at most 61 Gauss points over a
// whole boundary edge, and its adaptive form knows no spans of swept surfaces, so a disc bounded by a 640-span
// spline read 3-4 % off. Here each B-spline span of the boundary curves and of the surface gets its own Gauss rule.
// Reported numbers use these; modelling decisions (reference hints, face frames) keep BRepGProp so that existing
// documents regenerate exactly as before.
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

namespace opad {

struct MassProperties {
  double mass = 0;  // volume (mm3) or area (mm2)
  gp_Pnt centre;    // centre of that volume or area
};

// The volume the faces of `shape` enclose (solids; like BRepGProp, every face counts) and its centre.
MassProperties volume_properties(const TopoDS_Shape& shape);
// The area of the faces of `shape` and its centre.
MassProperties area_properties(const TopoDS_Shape& shape);

}  // namespace opad
