#pragma once
// Joints, relations, loads and studies as the document stores them (joint / pose / load / study ops): the kinds this build
// knows, what each one's coordinates are, and the checks an op of these types must pass. No kernel, no solver.
//
//   joint  {"name", "kind", "base": node|null, "part": node, "frames": [frame in the base's coordinates, frame in the
//          part's], "values": [its coordinates when made], "limits": {coordinate: [min, max]}, "locked", "pitch",
//          "drive", "spring", "friction"}; a relation {"name", "kind", "joints": [j1, j2], "ratio" | "radius" | "lead",
//          "values": [j1's and j2's coordinates when made]}.
//   pose   {"name", "values": {joint: [coordinates]}, "placements": [{"target": node, "matrix": [16]}]}: where the kinematic
//          solver put the parts for those joint values.
//   load   {"name", "kind", "case", "refs": [...], ...} (sim/fea.hpp).
//   study  {"name", "kind", "settings": {...}, "result": {...}} (sim/study.hpp).
//
// Coordinates are degrees and millimetres in every op and command; the solvers work in radians.
#include <string>
#include <vector>

#include "../json.hpp"

namespace opad::sim {

struct Coord {
  std::string name;  // rotation | translation | x | y
  bool angle = false;
};

struct JointKind {
  std::string kind;
  std::vector<Coord> coords;  // the joint's own coordinates (none for rigid, ground and ball)
  int constraints = 0;        // how many of the 6 relative freedoms it takes away (relations: 1)
  bool relation = false;      // couples two joints' coordinates instead of joining two parts
};

const std::vector<JointKind>& joint_kinds();
const JointKind* joint_kind(const std::string& kind);
bool is_relation(const std::string& kind);
// The index of a coordinate of a joint kind, -1 when it has none of that name.
int coord_index(const JointKind& kind, const std::string& coord);
// A relation's coupled coordinates: what of each of its two joints it reads (gear: rotation and rotation; rack_pinion and
// lead_screw: rotation and translation).
std::pair<std::string, std::string> relation_coords(const std::string& kind);

const std::vector<std::string>& load_kinds();
const std::vector<std::string>& study_kinds();

// Document::validate_op's checks for these op types (throw Error).
void validate_joint_op(const json& op);
void validate_pose_op(const json& op);
void validate_load_op(const json& op);
void validate_study_op(const json& op);

}  // namespace opad::sim
