#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "document.hpp"

namespace opad {

// The resolved state obtained by replaying the op log (skipping tombstoned ops).
struct Node {
  enum class Kind { Component, Body };
  std::string id;
  Kind kind = Kind::Component;
  std::string name;
  std::string parent;  // empty = document root
  std::vector<std::string> children;
  std::string body_key;  // Body only
  json raster;  // embedded SVG raster reference, independent of shared geometry
  std::string representation = "solid";  // solid | mesh | drawing2d
  bool body_missing = false;  // Body whose key is not in the store (F8)
  bool linked = false;        // part of a linked asset (assets.hpp): its geometry comes from the file, read-only
  Mat4 local;
  bool has_color = false;
  std::array<double, 3> color{0.75, 0.75, 0.78};
  double opacity = 1.0;
  bool visible = true;
  bool locked = false;  // not picked and not changed, moved or removed (Scene::effectively_locked: or under a locked component)
  json layer;  // a drawing layer as its file had it: {name, off, frozen, locked, plot, linetype, lineweight}; null otherwise
  bool by_layer = false;  // a drawing body in its layer's colour (DXF BYLAYER): a colour given to the layer applies to it
  std::string source_op;  // the import op that created it
  std::vector<std::string> modified_by;  // ops that touched this node after import
  json properties = json::object();  // part properties (properties ops): part_number, description, material, bom, ...
};

struct Annotation {
  std::string id;  // == op id
  Ref anchor;
  std::string text, by, ts;
  std::string style = "note";  // one of annotation_styles(); how the note is drawn, not whether it is open
  std::string reply_to;
  json comments = json::array();
  json drawing;  // camera-facing world frame and colored 2D polylines, never solid geometry
  bool unresolved = false;
};
// The tags a note can carry: ok, warning, issue, note (default), ai_agent.
const std::vector<std::string>& annotation_styles();

struct Measurement {
  std::string text, style = "note";
  json comments = json::array();
  std::string id;
  std::string kind;  // distance | angle | radius | diameter | bbox
  std::vector<Ref> refs;
  json result;
  std::string by, ts;
  bool unresolved = false;
};

struct SectionPlane {
  std::string id, name;
  Vec3 origin{0, 0, 0}, normal{0, 0, 1};
  bool enabled = true;
};

struct ViewBookmark {
  std::string id, name;
  json camera;
  json explode;  // optional exploded view (explode.hpp ExplodeSpec); null for a plain camera bookmark
  json display;  // optional: what is shown, {"layers": {layer id: state}} (a drawing's layer state, UI-89); null otherwise
};

struct Unresolved {
  std::string op_id, op_type, reason;
};

// ---- design (param / sketch / feature ops). Replay never runs the kernel or the sketch solver: a sketch
// carries its solved coordinates and a feature the keys of the bodies it produced (see docs/design.md).
struct Param {
  std::string id, name, expr, comment;
  double value = 0;    // mm, radians or a plain number
  int len = 0;         // power of length (1 = a length)
  bool angle = false;
  std::string shown;   // "12.5 mm"
  std::string error;   // bad expression, unknown name, cycle
};

// Right-handed sketch/construction plane frame in world coordinates (unit vectors).
struct Frame {
  Vec3 origin{0, 0, 0}, x{1, 0, 0}, y{0, 1, 0};
  Vec3 normal() const;
  Vec3 to_world(double u, double v) const;
  void to_local(const Vec3& p, double& u, double& v) const;
  Frame transformed(const Mat4& m) const;  // moved by m (its rigid part: axes stay unit and square)
  json to_json() const;
  static Frame from_json(const json& j);
};

struct SketchItem {
  std::string id, name;
  std::string component;  // the component it was made in (the op's optional "component"); empty = the document root
  Mat4 placed;            // that component's world placement when the sketch was made
  Mat4 moved;             // how far the component moved since (world now * inverse(placed)); frame includes it
  json plane;     // how the plane was chosen: {"base":"xy"} | {"face":ref} | {"feature":id}
  Frame frame;    // where it is now; the op keeps it as made (frame.transformed(moved.inverse()))
  json geometry;  // solved: {"points":[..],"entities":[..],"constraints":[..]} (design/sketch.hpp)
  bool visible = true;
  bool consumed = false;  // some feature uses it: hidden unless shown explicitly
  int dof = -1;
  std::string error;
};

struct Feature {
  std::string id, kind, name;
  std::string component;  // where its new bodies (or its plane / axis) went; empty = the document root
  json inputs, result;
  bool suppressed = false;
  std::string suppress_if;  // an expression that suppresses it while true (gap log #9)
  std::string error;
};

// ---- technical drawings (sheet / sheet_view / sheet_item ops, TODO 11 UI-76). Replay records definitions only: views
// are projected from the final scene when a sheet is shown or exported (drawing/projection.hpp caches them by
// fingerprint), and a dimension carries the value it had when it was made (`result`), like a pinned measurement.
struct Sheet {
  std::string id, name, drawing;  // drawing: the sheets of one drawing share it
  double width = 0, height = 0;   // paper mm
  std::string standard = "iso", projection = "first";  // iso | asme; first | third angle
  double scale = 1;               // paper / model, the views' default
  json def;                       // the effective record
  std::vector<std::string> views, items;  // ids, log order
};

struct SheetView {
  std::string id, sheet, parent, name;
  std::string kind;               // base | projected (later builds add more; unknown ones are kept, not drawn)
  json def;
  std::vector<std::string> children;  // views projected from this one
  std::string error;              // why it cannot be drawn
};

struct SheetItem {
  std::string id, sheet, view;    // view: empty for items placed on the sheet itself
  std::string kind, type;         // dimension (horizontal | vertical | aligned | radius | diameter | angle) | note
  std::vector<Ref> refs;
  json def;
  std::string error;
  bool unresolved = false;        // a reference names a body that is gone
};

struct Scene {
  std::string units="mm"; // document input/display unit; stored geometry remains millimetres
  std::vector<std::string> roots;
  std::unordered_map<std::string, Node> nodes;
  std::vector<Annotation> annotations;
  std::vector<Measurement> measurements;
  std::vector<SectionPlane> sections;
  std::vector<ViewBookmark> views;
  std::vector<Unresolved> unresolved;
  std::vector<Param> params;
  std::vector<SketchItem> sketches;
  std::vector<Feature> features;
  std::vector<Sheet> sheets;
  std::vector<SheetView> sheet_views;
  std::vector<SheetItem> sheet_items;
  std::vector<std::string> deleted_ops;  // ids of tombstoned ops
  std::unordered_map<std::string, int> instance_count;  // body key -> number of body nodes
  // The document's own properties (title, number, owner, project, ...): `properties` ops whose target is the header's uuid.
  json properties = json::object();

  const Node* node(const std::string& id) const;
  Mat4 world(const std::string& id) const;
  bool effectively_visible(const std::string& id) const;
  bool effectively_locked(const std::string& id) const;  // it or a component above it is locked
  std::vector<std::string> bodies_under(const std::string& id) const;  // depth-first
  std::vector<std::string> all_bodies() const;
  std::vector<std::string> path_to(const std::string& id) const;  // root..id
  json tree_json(int max_depth = -1) const;
  const SketchItem* sketch(const std::string& id) const;
  const Feature* feature(const std::string& id) const;
  const Param* param(const std::string& name) const;
  const Sheet* sheet(const std::string& id) const;
  const SheetView* sheet_view(const std::string& id) const;
  const SheetItem* sheet_item(const std::string& id) const;
};

// The log as replay sees it: tombstoned ops dropped, `edit` ops merged into their targets (later edits win,
// key by key) and `regen` results put in place of the results the ops were written with. `edit`, `regen` and
// `delete` ops themselves are not part of it.
struct EffectiveOp {
  const Op* op = nullptr;
  std::shared_ptr<json> patched;  // only ops an edit or a regen touched carry their own copy
  const json& data() const { return patched ? *patched : op->data; }
  json& edit() {
    if (!patched) patched = std::make_shared<json>(op->data);
    return *patched;
  }
};
std::vector<EffectiveOp> effective_ops(const Document& doc, std::vector<std::string>* deleted = nullptr);
// The same over any op list (the design engine plans with ops that are not in the document yet).
std::vector<EffectiveOp> effective_ops(const std::vector<const Op*>& ops, std::vector<std::string>* deleted = nullptr);

// Replay, one op at a time. resolve() drives it from the stored log; the design engine drives it while it
// recomputes results, so both see the same state.
class SceneBuilder {
 public:
  explicit SceneBuilder(const Document& doc);
  ~SceneBuilder();
  void apply(const std::string& id, const std::string& type, const json& data);
  void finish();  // parameter values, sketch visibility
  Scene& scene();
  Scene take();

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

// `until`: stop before this op (the state an earlier feature was computed in; timeline roll-back).
Scene resolve(const Document& doc, const std::string& until = {});

// The ops (of the effective log) that touch a component and what is under it, for a timeline that dims the others
// while it is active (TODO 11 UI-33): what made its nodes, sketches and features made in it, features that change a
// body in it, reparent / transform / appearance / rename ops on something in it or putting something into it, and notes
// and measurements on it. An empty component is the document root: every op.
std::set<std::string> ops_in_component(const Document& doc, const Scene& scene, const std::string& component);

}  // namespace opad
