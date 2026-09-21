#pragma once
// Kernel side of sketches: curves in space, closed regions (profiles) and paths.
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

#include <vector>

#include "../scene.hpp"
#include "sketch.hpp"

namespace opad::design {

gp_Ax3 frame_ax3(const Frame& f);
gp_Pln frame_plane(const Frame& f);
Frame frame_from_ax3(const gp_Ax3& a);
Frame base_frame(const std::string& base);  // "xy" | "xz" | "yz"

// One entity as an edge in world coordinates (null for Point entities and degenerate curves).
TopoDS_Edge entity_edge(const Sketch& sk, const SkEntity& e, const Frame& frame);
// Every non-construction curve.
std::vector<TopoDS_Edge> sketch_edges(const Sketch& sk, const Frame& frame, bool with_construction = false);

// The closed regions the curves enclose, the way the sketcher shows them: curves are cut where they cross, so
// two overlapping circles give three regions, and a circle inside a rectangle gives the ring and the disc.
struct Region {
  TopoDS_Face face;
  double area = 0;
  double u = 0, v = 0;  // a point inside, in sketch coordinates
};
std::vector<Region> sketch_regions(const Sketch& sk, const Frame& frame);
// Index of the region containing (u, v), or -1.
int region_at(const std::vector<Region>& regions, const Frame& frame, double u, double v);
// The curves (all non-construction ones, or the listed entities) as one connected wire. Throws when they do
// not chain up.
TopoDS_Wire sketch_wire(const Sketch& sk, const Frame& frame, const std::vector<int>& entities = {});
// Bounding box of the points in sketch coordinates; false when empty.
bool sketch_bounds(const Sketch& sk, double& u0, double& v0, double& u1, double& v1);

}  // namespace opad::design
