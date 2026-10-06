#pragma once
// The kinematic solver: where the parts of a mechanism go when its joints are driven (sim/joints.hpp has what the
// document stores). The unknowns are the parts' world placements; each joint, relation, lock, limit and driven value is
// a set of equations on them, solved by damped Newton steps of least motion from where the parts are, so a free part
// stays where it is and a linkage stays on the branch it is on. Values move in sub-steps of at most 10 degrees (or a
// tenth of the mechanism's size) for the same reason. No kernel calls: microseconds per step for tens of parts.
//
// Parts are the nodes the joints name (bodies or components); everything under a part moves with it. A part joined
// to the world, or by `ground`, stays; a group of parts joined to nothing that stays keeps one part still (the base of the
// joint being driven, else of its first joint), so driving a joint never moves its base for no reason.
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../scene.hpp"

namespace opad::sim {

// Joint values: joint id -> its coordinates (deg, mm) in sim::joint_kind(kind).coords order.
using Values = std::map<std::string, std::vector<double>>;

class Mechanism {
 public:
  // The joints of the scene that resolve (broken ones are listed in problems()).
  explicit Mechanism(const Scene& scene);
  ~Mechanism();
  Mechanism(const Mechanism&);
  Mechanism& operator=(const Mechanism&);

  struct Result {
    bool ok = true;
    double residual = 0;     // the worst equation left, in mm (angles counted at the mechanism's size)
    int iterations = 0;
    double reached = 1;      // the fraction of the way the parts got when it failed
    std::string error;       // why it failed: which joints cannot all be met
    std::vector<std::string> notes;  // values held at a limit, joints that a lock or a limit stopped
  };
  // Drives these joints to these coordinates (NaN: left free) and moves the parts to meet every joint. `anchor`: the
  // part a group joined to nothing keeps still (default: the base of the first driven joint). On failure the parts stay
  // where the last sub-step that worked put them.
  Result drive(const Values& targets, const std::string& anchor = {});
  // Moves the parts until every joint is met again (after a joint was added, or parts were moved by hand).
  Result settle(const std::string& anchor = {});
  // The same with one part placed somewhere first (a drag): the others follow through the joints.
  Result place(const std::string& part, const Mat4& world, const std::string& anchor = {});

  Values values() const;                          // every joint's coordinates now
  std::map<std::string, Mat4> part_worlds() const;  // each part's world placement now
  Mat4 part_world(const std::string& part) const;
  const std::vector<std::string>& parts() const;
  bool has_part(const std::string& node) const;
  // The joint's frames in the world now: on its base and on its part.
  std::pair<Frame, Frame> joint_frames(const std::string& joint) const;
  // Local placements of the parts that moved, parents first: what a pose op stores.
  std::vector<std::pair<std::string, Mat4>> placements(const Scene& scene) const;
  // The scene with the parts where they are now (render, measure, drawings of a pose without writing it).
  Scene posed(const Scene& scene) const;
  // Degrees of freedom: {"dof", "constraints", "redundant", "parts", "joints": [{id, name, kind, values, free: [bool per
  // coordinate]}], "free_parts": [...], "problems": [...]} (free: that coordinate can move without breaking a joint).
  json analysis() const;
  const std::vector<std::string>& problems() const;
  double size() const;  // the length angles are weighed at (mm)

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
  friend std::vector<double> joint_coordinates(const std::string&, const Mat4&, const Mat4&, const std::vector<double>&);
};

// A joint's coordinates (radians, mm) where its base frame is `a` and its part frame `b` (world placements of the two
// frames; z of a is the axis), angles unwrapped about `ref` (radians; empty: none). The kinematic solver's definition,
// shared with the dynamic one.
std::vector<double> joint_coordinates(const std::string& kind, const Mat4& a, const Mat4& b, const std::vector<double>& ref = {});

// The pose op the mechanism's state is: placements of the parts that moved and every joint's values. Empty placements
// and unchanged values: nothing to write (null).
json pose_op(const Scene& before, const Mechanism& mech, const std::string& name);

}  // namespace opad::sim
