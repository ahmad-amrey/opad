#pragma once
// Internal to the design engine: the state a feature is computed in, and what it hands back.
#include <TopoDS_Shape.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <map>
#include <string>
#include <vector>

#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_geom.hpp"

namespace opad::design {

struct ResolvedRef {
  std::string node;
  TopoDS_Shape sub;  // in world coordinates
  int index = -1;    // where it is now (may differ from the stored ordinal after a topology change)
};

struct Ctx {
  const Document& doc;
  const ParamTable& params;
  const Scene& scene;  // the state just before this feature
  const std::map<std::string, TopoDS_Shape>& fresh;  // bodies made earlier in the same plan, by key
  Cancel cancel;
  // What the references resolved to that the result should record (TODO 10 B7): "selected" (rule selectors and
  // what they matched) and "rehinted" (references taken by the nearest-hint fallback). Null: not recorded.
  json* notes = nullptr;
  // The component the feature is made in (its op's "component", UI-33; empty: the document root). An automatic operation
  // and automatic targets look only at the bodies under it, as Fusion's do in the active component.
  std::string component;

  void check_cancel() const;
  TopoDS_Shape key_shape(const std::string& key) const;
  TopoDS_Shape node_shape(const std::string& node) const;  // body node, world coordinates
  gp_Trsf node_trsf(const std::string& node) const;        // node -> world (identity for "")
  ResolvedRef resolve(const json& ref) const;               // body / face / edge / vertex reference, hint-aware
  std::vector<ResolvedRef> resolve_all(const json& refs) const;  // a rule selector ({.., "select"}) gives every match

  double length(const json& inputs, const char* name) const;
  double angle(const json& inputs, const char* name) const;
  double number(const json& inputs, const char* name) const;
  int count(const json& inputs, const char* name) const;

  // Sketch by id (solved state of this moment) and its frame.
  Sketch sketch(const std::string& id, Frame* frame = nullptr) const;
  Frame plane(const json& plane_input) const;  // {"base"} | {"face":ref} | {"feature":id}
  gp_Ax1 axis(const json& axis_input) const;   // {"base"} | {"edge":ref} | {"face":ref} | {"sketch","entity"} | {"feature":id}
};

// What a feature produced, in world coordinates. The engine turns it into body-store entries and node ids.
struct Out {
  struct Body {
    std::string node;  // existing node it replaces, or empty for a new body
    TopoDS_Shape shape;
    std::string source;  // new bodies: the body it is a copy or a piece of (named, placed and coloured after it)
  };
  std::vector<Body> bodies;
  std::vector<std::string> removed;
  std::vector<std::string> used_targets;  // creation features with automatic targets: who took part
  // Creation features (apply_operation): the solid they add, remove or intersect with, in world coordinates, and the
  // operation it was used for ("new", "join", "cut", "intersect"; an "auto" input says here what it was taken as).
  TopoDS_Shape tool;
  std::string operation;
  json extra = json::object();            // construction geometry: {"plane":frame} / {"axis":{origin,dir}}
};

Out compute_feature(const Ctx& ctx, const std::string& kind, const json& inputs);

// Whether a reference names a linked part (or a component of them) that is not loaded (its file missing, not trusted yet):
// nothing is known of it, so a sketch keeps its projection as last computed instead of saving an error (compute_sketch).
bool unloaded_link(const Scene& scene, const json& ref);

// A plane through `origin` with that normal (TODO 10 B5). Its x is `x` laid onto the plane, or when none is given
// world X laid onto it (world Y when X is the normal), so the same inputs always give the same frame.
Frame plane_through(const gp_Pnt& origin, const gp_Vec& normal, const gp_Vec* x = nullptr);

// Geometric fingerprint of a sub-shape in world coordinates (centre + size + the body's entity counts).
json ref_hint(const TopoDS_Shape& body_world, const TopoDS_Shape& sub);

}  // namespace opad::design
