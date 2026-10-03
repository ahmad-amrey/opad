#pragma once
// Exploded views (TODO 11 UI-35): which parts move together (units, by level) and where they go. Pure and deterministic
// over the resolved scene. How deep components split (levels) and how far they have moved (t) are separate inputs, so a
// UI shows them as separate controls, never as one slider cut into a segment per level.
#include <Bnd_Box.hxx>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "document.hpp"
#include "scene.hpp"

namespace opad {

// The optional `explode` object of a `view` op (older builds ignore the field and see a camera bookmark).
struct ExplodeSpec {
  std::string root;             // component to explode; "" = the document's roots. A lone component on the way down is skipped
  int levels = 1;               // how deep components split: 1 = the root's children move as wholes; 0 = every level
  // radial: away from the parent's centre | axis: along +-axis | stack: piled up along axis. In radial and axis a part
  // inside a larger sibling's box leaves it opposite to where that one goes, and no two siblings end overlapping.
  std::string mode = "radial";
  Vec3 axis{0, 0, 1};
  double spacing = 1;           // times the automatic distance (stack: the gap)
  bool attach_small = true;     // a small part moves with the larger part it touches (PCB passives, solder joints)
  double small_ratio = 0.05;    // small: its box diagonal under this share of its parent's
  double small_size = 0;        // ... or under this many mm when > 0
  double touch = 0.05;          // boxes this close (mm) touch
  std::set<std::string> keep;   // components that move as one unit at any level (the PCB)
  std::set<std::string> split;  // components whose children move apart beyond `levels` (the screws)
  std::vector<std::vector<std::string>> groups;  // nodes that move as one unit; the unit's id is the first member's
  std::map<std::string, Vec3> offsets;           // manual moves (mm) per unit id, on top of the automatic ones
  std::string stages = "levels";  // levels: level k moves while t runs through [(k-1)/L, k/L] | together | units: one by one
  double duration = 1.2;        // seconds a full play takes
  double t = 1;                 // the saved distance: 0 = assembled, 1 = exploded
  json to_json() const;
  static ExplodeSpec from_json(const json& j);  // Error for a malformed spec; unknown keys are ignored
};

struct ExplodeUnit {
  std::string id, name;               // the node (component or body; a group's first member)
  std::vector<std::string> bodies;    // body nodes that move with it, not those of its child units
  int parent = -1;                    // index of the enclosing unit; -1 = the explode root
  int level = 1;                      // 1 = moves away from the root's other children
  Vec3 lo{0, 0, 0}, hi{0, 0, 0};      // assembled box of everything under it
  Vec3 centre{0, 0, 0}, dir{0, 0, 1};
  double distance = 0;                // its own automatic move at t = 1 (mm), along dir
  double t0 = 0, t1 = 1;              // the stretch of t over which it moves
};

using ExplodeBoxFn = std::function<Bnd_Box(const std::string& body)>;

// The component the explode starts from: spec.root (or the roots), past components that are the only visible child.
std::string explode_root(const Scene& scene, const ExplodeSpec& spec);
// The levels the root offers (a level control's range): the deepest nesting under it, a body counting one.
int explode_depth(const Scene& scene, const ExplodeSpec& spec);
// The units, parents before children. box_of: a body node's world box; by default its tight box (node_tight_bbox from
// the cached corners), which walks each shape once: workers only.
std::vector<ExplodeUnit> explode_units(const Document& doc, const Scene& scene, const ExplodeSpec& spec, const ExplodeBoxFn& box_of = {});
// Each unit's move at t: its parent's plus its own dir * distance + offsets[id], eased over [t0, t1].
std::vector<Vec3> explode_unit_offsets(const std::vector<ExplodeUnit>& units, const ExplodeSpec& spec, double t);
// The same per body node (world translation). Microseconds per unit: a UI calls it every frame.
std::unordered_map<std::string, Vec3> explode_offsets(const std::vector<ExplodeUnit>& units, const ExplodeSpec& spec, double t);
// The scene with those bodies moved, so measuring, rendering and drawings see the parts where the view shows them.
Scene exploded_scene(const Scene& scene, const std::unordered_map<std::string, Vec3>& offsets);
// The view op's explode (Error when there is no such view; the defaults when it has none).
ExplodeSpec view_explode(const Scene& scene, const std::string& view_id);

// ---- what an explode editor needs (UI-36); pure, microseconds per unit.
// The trail lines at t: each moving unit's centre from where its parent units alone take it to where it is drawn.
struct ExplodeTrail {
  size_t unit = 0;
  Vec3 from{0, 0, 0}, to{0, 0, 0};
};
std::vector<ExplodeTrail> explode_trails(const std::vector<ExplodeUnit>& units, const ExplodeSpec& spec, double t);
// The unit that moves a node: the unit with that id (a group's is its first member's), the unit holding a body, or for
// a component the one unit holding all of its shown bodies. -1 when none (hidden, outside the root, split among units).
int explode_unit_of(const Scene& scene, const std::vector<ExplodeUnit>& units, const std::string& id);
// A unit's own move at t = 1 (automatic plus manual) along a unit axis, and the manual offset that makes it `travel`
// there, keeping the move across the axis. A manual offset that comes out zero is dropped.
double explode_travel(const ExplodeUnit& unit, const ExplodeSpec& spec, const Vec3& axis);
void set_explode_travel(ExplodeSpec& spec, const ExplodeUnit& unit, const Vec3& axis, double travel);
// Per component: follow the level setting, keep together (one unit at any level) or explode its parts (split).
enum class ExplodeRule { Level, Keep, Split };
ExplodeRule explode_rule(const ExplodeSpec& spec, const std::string& component);
void set_explode_rule(ExplodeSpec& spec, const std::string& component, ExplodeRule rule);
// Groups of nodes that move as one: grouping takes the ids out of the groups they were in (a group left with one
// member goes) and drops their manual offsets but the first's; ungrouping removes the group holding id and its offset.
int explode_group_of(const ExplodeSpec& spec, const std::string& id);  // index into spec.groups, -1 none
void explode_group(ExplodeSpec& spec, const std::vector<std::string>& ids);
bool explode_ungroup(ExplodeSpec& spec, const std::string& id);
// An `explode` argument (a view op id or a spec object) applied at the spec's t.
Scene exploded_scene(const Document& doc, const Scene& scene, const json& explode);

}  // namespace opad
