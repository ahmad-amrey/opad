#pragma once
// Holes of a solid for hole callouts and hole tables (TODO 11 UI-80), recognised from its faces: a cylindrical face whose
// material lies outside it is a drilled diameter; the faces on its axis next to it tell the rest: a wider coaxial cylinder
// (directly or past a flat shoulder) at one end is a counterbore, a cone opening outwards a countersink, a cone closing
// onto the axis a drill point and a disc a flat bottom (a blind hole), anything else an opening. A hole open at both ends
// is through. A hole feature that drilled the body gives its own values instead where it made a hole of that size
// (drawing/annotate). Qt-free; walks geometry: workers only.
#include <TopoDS_Shape.hxx>

#include <functional>
#include <string>
#include <vector>

#include "opad/util.hpp"

namespace opad::drawing {

struct Hole {
  Vec3 entry{0, 0, 0}, dir{0, 0, 1};  // the centre of its opening and the direction it goes in (into the material)
  double diameter = 0;
  double depth = 0;                   // blind: from the opening to the end of the drilled diameter (the point not counted)
  bool through = false;
  std::string type = "simple";        // simple | counterbore | countersink
  double cb_diameter = 0, cb_depth = 0, cs_diameter = 0, cs_angle = 0;  // angles in degrees
  double tip_angle = 0;               // a blind hole's drill point (degrees); 0: a flat bottom
  std::vector<int> faces;             // the body's faces (ordinals) it is made of
  std::string feature;                // the hole feature that drilled it, when its values were taken
  double outer() const;               // the widest diameter it shows at its opening
  bool same_size(const Hole& o) const;  // the same callout, drilled the same way (parallel)
  json to_json() const;
};

// Every hole of a body (in the coordinates the shape is in), in the order of their faces.
std::vector<Hole> find_holes(const TopoDS_Shape& body);
// The hole that a face or edge of the body (by ordinal) belongs to: one of its faces, or a circle on its axis. -1: none.
int hole_of(const std::vector<Hole>& holes, const TopoDS_Shape& body, const TopoDS_Shape& sub);

// The callout of a hole: "4× ⌀5 THRU", "⌀5 ↧10", a second line "⌴ ⌀9 ↧3" or "⌵ ⌀10 × 90°". Values in the drawing's
// units (per_mm: 1 for mm, 1 / 25.4 for inches); standard asme writes "4X". number: how a value is written.
std::string hole_callout(const Hole& h, int count, const std::string& standard, double per_mm, const std::function<std::string(double)>& number);

}  // namespace opad::drawing
