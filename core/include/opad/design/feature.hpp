#pragma once
// The design engine: parameters, sketches and features as ops, and the regeneration that keeps their stored
// results true. See docs/design.md for the model. In short:
//
//   * a `feature` op records what the user asked for (`kind`, `inputs` with expressions and references) and
//     what came out (`result`: node id -> body-store key). Replay reads the result and never runs the kernel,
//     so a document opens the same everywhere and as fast as before.
//   * changing something is an `edit` op (key-by-key patch of an earlier param/sketch/feature op). Whatever
//     that changes downstream is recomputed once, here, and appended as one `regen` op holding the new results.
//   * work is split in two so the app can keep its UI-thread rule: plan_*() only reads the document (worker
//     thread, cancellable) and commit() applies a finished plan (owner thread, cheap).
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../document.hpp"
#include "../scene.hpp"
#include "expr.hpp"

class TopoDS_Shape;

namespace opad::design {

// ---------------------------------------------------------------- what a feature asks for (drives the UI form)
struct InputSpec {
  std::string name;   // key in the op's "inputs"
  std::string label;  // English source text; the app translates
  // length | angle | number | count: expression or number
  // bool | choice | text
  // bodies | faces | edges: lists of references ({"body":node,"kind":..,"index":N,"hint":{..}})
  // profiles: [{"sketch":id,"at":[u,v],...}] regions of sketches   path: {"sketch":id} or edges
  // plane: {"base":"xy|xz|yz"} | {"face":ref} | {"feature":id}     axis: {"base":"x|y|z"} | {"edge":ref} | {"sketch":id,"entity":n} | {"feature":id}
  // points: [{"sketch":id,"point":n}] or vertex references
  std::string type;
  json def;                          // default value
  std::vector<std::string> choices;  // for "choice"
  std::string show_if;               // "other=value[|value]": only shown (and used) then
  bool optional = false;
  int min_count = 1;                 // selections: how many picks at least (0 with optional)
  int max_count = 0;                 // 0 = any number
  bool advance = false;              // selections: the first pick moves on to the next input (more: its box again)
  bool planes = false;               // faces: origin and construction planes are offered beside them (To face)
};

struct FeatureSpec {
  std::string kind, label, icon, group;  // group: create | modify | combine | pattern | construct | body
  std::vector<InputSpec> inputs;
  std::string hint;  // one line for the panel
  // A primitive placed in the view (TODO 11 P1): a click on a plane writes its "plane", "x" and "y"; then the pointer draws
  // the footprint: "rect" (its "length" and "width" about the click, or from it as a corner with "centered" off), "round"
  // (its "diameter" from the click), "ring" ("diameter" to the tube's middle, then the tube's "section"). A "height" input
  // is pulled by the arrow after. Empty: not placed so.
  std::string footprint;
};

const std::vector<FeatureSpec>& feature_specs();
const FeatureSpec* feature_spec(const std::string& kind);
json feature_specs_json();

// ---------------------------------------------------------------- plans
struct NewBody {
  std::string key, brep;  // key = sha256_hex(brep) (commit trusts it); no brep: a linked asset's body, registered as external (assets.hpp)
  json meta;
  std::shared_ptr<TopoDS_Shape> shape;  // cached on commit so nothing is parsed back from text
};

struct Plan {
  std::vector<json> ops;        // to append, in order; feature/sketch ops carry their results
  std::vector<NewBody> bodies;  // entries the results refer to
  json report;                  // {"id":..., "regenerated":[...], "errors":[{"op","name","error"}]}
  // Preview: the bodies the change leaves different, in world coordinates (node id empty = a new body).
  struct Changed {
    std::string op;    // the sketch/feature that produced it (a preview shows the edited feature's own effect)
    std::string node;
    std::shared_ptr<TopoDS_Shape> shape;
    bool removed = false;
  };
  std::vector<Changed> changed;
  // Preview: the solid each creation feature computed here adds, removes or intersects with (world coordinates) and the
  // operation it was used for: "new", "join", "cut" or "intersect" (an "auto" operation as it was decided). The app draws
  // a cut's tool in red over the bodies it cuts.
  struct Tool {
    std::string op;         // the feature op
    std::string operation;  // new | join | cut | intersect
    std::shared_ptr<TopoDS_Shape> shape;
  };
  std::vector<Tool> tools;
};

using Cancel = std::function<bool()>;

// Appends new ops (param / sketch / feature / edit / delete ...), given without results: results of new and of
// every affected later op are computed. `strict`: an error in one of the *new* ops throws instead of being
// recorded, so a wrong input never reaches the document; so does a plan that changes a locked body (locked_change).
Plan plan_ops(const Document& doc, std::vector<json> new_ops, bool strict = true, const Cancel& cancel = {});
// Recomputes whatever is out of date (after a merge, a hand edit, a tombstone). An empty plan = up to date.
Plan plan_regenerate(const Document& doc, bool force = false, const Cancel& cancel = {});
json commit(Document& doc, Plan&& plan, const std::string& author = {});

// Whether two shapes are the same geometry as regeneration judges it (topology counts, vertices to 1e-6 mm, volume, area,
// centre): a recomputed body that is keeps its stored key (a linked asset's sync too).
bool same_shapes(const TopoDS_Shape& a, const TopoDS_Shape& b);

// Convenience for the command layer and tests: plan + commit.
json apply_ops(Document& doc, std::vector<json> new_ops, const std::string& author = {});

// Locks (TODO 11 UI-37): whether an op of the document locks anything (a scan of the log, no replay), and why going
// from `before` to `after` is refused: it removes, changes or moves a body or component that is locked in `before`
// (none: it does not; the outermost such node is named with what holds its lock, the rest counted). A locked one that
// goes with an unlocked component above it (a drawing deleted with a locked layer) may go; one inside a component that
// moves stays where it is in it.
bool has_locks(const Document& doc);
std::optional<LockedError> locked_change(const Scene& before, const Scene& after);

// ---------------------------------------------------------------- helpers shared with the app
// Op builders (no results; feed them to plan_ops).
json make_param_op(const std::string& name, const std::string& expr, const std::string& comment = {});
json make_sketch_op(const std::string& name, const json& plane, const json& geometry);
json make_feature_op(const std::string& kind, const std::string& name, const json& inputs);
json make_edit_op(const std::string& target, const json& set);
// "Extrude3": the first free name for that kind/prefix in the scene.
std::string next_name(const Scene& scene, const std::string& prefix);
// A new feature's name prefix: its label's first word ("Extrude"), the whole label run together when another kind's label
// starts with that word ("RemoveFaces" beside Remove's "Remove", "ConstructionPlane" beside "ConstructionAxis").
std::string name_prefix(const FeatureSpec& spec);
// Item n (from 1) of count named after one name: "{n}" marks where the number goes ("Board screw {n}" ->
// "Board screw 2"); without it several items are numbered "Board screw 1", "Board screw 2", ... and a single one
// keeps the name as it is.
std::string numbered_name(const std::string& name, size_t n, size_t count);
// The rename / appearance / reparent ops that give the new bodies a feature op made (its result's entries marked
// "new") a name, a colour and a component: `style` holds any of body_name (numbered as numbered_name does),
// color [r,g,b] in 0..1 and parent (component id; null or "" = the document root). They are the ops the long form
// writes, appended to plan.ops so they commit in the same step; the op format and replay are unchanged
// (TODO 10 B14). Returns how many new bodies there were.
size_t style_new_bodies(Plan& plan, const std::string& feature_op, const json& style);
// Ops renaming a parameter everywhere it is used (the param itself and every expression).
std::vector<json> rename_param_ops(const Document& doc, const std::string& from, const std::string& to);
// Names of features/sketches/params whose expressions use this parameter.
std::vector<std::string> param_users(const Document& doc, const std::string& name);
// Frame of a plane input ({"base":..} | {"face":ref} | {"feature":id}) in the given state.
Frame resolve_plane(const Document& doc, const Scene& scene, const json& plane);
// A plane chosen now for a sketch whose component has moved since it was made, as its op keeps it: where it was made
// (its frame and world points and directions moved back by sketch.moved; TODO 11 UI-33). Unchanged otherwise.
json plane_as_made(const SketchItem& sketch, json plane);
// Whether an input is in use for these inputs (its show_if holds).
bool input_active(const InputSpec& in, const json& inputs);
// A frame as results report it (TODO 10 B3): origin, x, y and the normal (x cross y).
json frame_result(const Frame& frame);
// A reference with its geometric fingerprint, which lets it survive a change of the body's topology.
json make_ref(const Document& doc, const Scene& scene, const Ref& ref);
// Feature inputs with a hint added to every face/edge/vertex reference that has none (the app picks plain
// references in its click handler and leaves this geometry walk to the worker).
json hint_refs(const Document& doc, const Scene& scene, json inputs);
// Drag handles for a feature's values (TODO 11 P2), for the app's preview: worked out on a worker in the state before the
// feature and never stored. Each is {input, origin, axis, value, scale}: the arrow sits at origin + axis * value * scale
// and a pull of d along the axis changes the input by d / scale. Fillet and chamfer: the radius or distance, half way
// along the first picked edge, pointing out between its two faces; thicken: the thickness off the first face (inwards with
// Other side); press pull: the distance off the first face's centre along its outward normal; an offset construction
// plane: the distance from its plane's origin; box, cylinder and cone: the height at the middle of the footprint; a move
// that turns: the angle about its axis (origin a point on the axis, axis its direction, "ring": true). The extrusion keeps
// the distance_handle its result stores. Empty when the kind has none or what it needs does not resolve.
json feature_handles(const Document& doc, const Scene& scene, const std::string& kind, const json& inputs);
// An extrusion started from a face with Start at: Sketch on face, made a real sketch (as committed from the panel): its
// profiles projected onto the start face's plane by a new sketch (named after their sketch, "Sketch1 (derived)"; it keeps the
// projection linked, so it follows the source sketch), on a construction plane offset from a planar face by the face offset
// (on the face itself when that is 0; a curved face: its tangent plane at its middle, fixed), then the extrusion of those
// regions from the sketch: the same solid. The ops in order, with ids, in `component` when given: one plan_ops step.
std::vector<json> derived_extrude_ops(const Document& doc, const Scene& scene, const json& inputs, const std::string& name,
                                      const std::string& component = {});

}  // namespace opad::design
