#pragma once
#include "sketch_geom.hpp"
namespace opad::design {
// Cubic Bezier spans with independent incoming/outgoing control poles and weights.
int add_cubic_spline(Sketch& sketch,const std::vector<int>& nodes,bool construction=false);
// Insert a curve node at the nearest parameter without changing the curve's shape.
int insert_spline_node(Sketch& sketch,int entity,double u,double v);
// Non-construction curve endpoints that meet neither another endpoint nor a curve.
std::vector<int> dangling_vertices(const Sketch& sketch,double tolerance=1e-6);
// The clipboard (TODO 11 UI-129). copy_entities: the curves named (a point names the point curve on it), the points they
// use and the constraints among them (a fix, a coordinate or anything that names geometry left behind stays behind), as one
// self-contained sketch with its base point (bx, by): {"format": "opad.sketch.clipboard", "version": 1, "base": [x, y],
// "sketch": {...}}. Projected curves come as plain ones, patterns and equations as their curves.
json copy_entities(const Sketch& sketch,const std::vector<int>& ids,double bx,double by);
// Adds a copy of a clip with its base point at (x, y): new ids, the constraints kept, dimension labels moved with it, a
// dimension's expression naming another copied dimension renamed to the copy (one that no longer evaluates here keeps its
// value). Throws on a clip that is not one. Returns the new curves' ids.
std::vector<int> paste_entities(Sketch& sketch,const json& clip,double x,double y,const ParamTable& params={});
}
