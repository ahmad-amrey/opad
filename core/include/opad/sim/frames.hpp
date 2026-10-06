#pragma once
// Where a joint sits: a frame (origin, x, y; z = x cross y is the joint's axis) chosen from the geometry as it is now.
#include <string>

#include "../document.hpp"
#include "../scene.hpp"

namespace opad::sim {

// A frame from any of:
//   a circular edge (its centre, its axis), a straight edge (its middle, along it), a cylindrical, conical or toroidal
//   face (its axis, at the face's middle), a planar face (its centre, its outward normal), a spherical face (its
//   centre), a vertex or {"point": [x, y, z]} (z up): as a reference string ("<body>/face/3"), {"face": ref},
//   {"edge": ref}, {"vertex": ref} or a rule selector matching one entity;
//   {"base": "x"|"y"|"z"}, {"direction": [x, y, z], "origin": [x, y, z]}, {"feature": construction axis or plane id};
//   {"origin": [x, y, z], "z": [x, y, z], "x": [x, y, z]} written out.
// Any form also takes "x" (a direction for the frame's x, laid onto its plane), "flip" (z the other way) and "offset"
// (mm along z). World coordinates, millimetres.
Frame joint_frame(const Document& doc, const Scene& scene, const json& at);
// The body or component a frame input refers to (empty: none, a written-out frame or an axis of the world).
std::string frame_node(const json& at);

}  // namespace opad::sim
