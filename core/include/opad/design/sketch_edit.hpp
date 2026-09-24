#pragma once
#include "sketch_geom.hpp"
namespace opad::design {
// Cubic Bezier spans with independent incoming/outgoing control poles and weights.
int add_cubic_spline(Sketch& sketch,const std::vector<int>& nodes,bool construction=false);
// Insert a curve node at the nearest parameter without changing the curve's shape.
int insert_spline_node(Sketch& sketch,int entity,double u,double v);
// Non-construction curve endpoints that meet neither another endpoint nor a curve.
std::vector<int> dangling_vertices(const Sketch& sketch,double tolerance=1e-6);
}
