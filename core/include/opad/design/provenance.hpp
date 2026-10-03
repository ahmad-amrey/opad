#pragma once
// Face provenance (TODO 11 UI-94): which feature made each face of a body, read from what the history already keeps.
// Every feature stores the body it produced under a content-addressed key, so the chain of keys a body went through is
// in the document: each step's result is compared with the body before it (and with the bodies it took in: combine
// tools, joined targets). A face of the result lying on a face it had before (the same surface, facing the same way,
// interior samples inside that face) keeps that face's owner; anything else is this step's. Copies (pattern, mirror,
// move with copy) and transforms (move, scale) keep their source's faces by ordinal. Nothing is recomputed and the
// format is unchanged. Walks geometry: workers only (the app goes through JobRunner::async on a document snapshot).
#include <memory>
#include <string>
#include <vector>

#include "feature.hpp"

namespace opad::design {

struct FaceOwner {
  std::string op;       // the feature or import op whose result first held this patch of surface
  std::string via;      // copies and cut tools: the owner the face had in the body it came from
  bool merged = false;  // the face also holds surface of another op (coplanar faces merged by a Boolean)
};

class Provenance {
 public:
  // Nothing is computed until a body is asked for; `doc` must outlive this.
  explicit Provenance(const Document& doc, Cancel cancel = {});
  ~Provenance();
  Provenance(const Provenance&) = delete;
  Provenance& operator=(const Provenance&) = delete;

  // Per face ordinal (TopExp::MapShapes order, as references count them) of the node's current body. Empty for a node
  // that is not a solid body (meshes, drawings, missing geometry).
  std::vector<FaceOwner> face_owners(const std::string& node);
  // Per edge ordinal: the owner of its faces when they agree, else the later of them (the step that made the edge).
  std::vector<FaceOwner> edge_owners(const std::string& node);
  // An owner op as the history shows it: {"op","type":"feature"|"import","kind","name","category","icon"} (null if unknown).
  json op_info(const std::string& op);
  // op_info of an owner, with `via` (op_info of the source owner) and `merged` when set.
  json describe(const FaceOwner& owner);
  // Position of an op in the history (effective order), -1 when it is not there.
  int order(const std::string& op);
  // The state replay reached (the document's current scene).
  const Scene& scene();

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

// What a feature does to a body, for selection and agents: body, boss, pocket, hole, groove, intersect, fillet, chamfer,
// shell, draft, press_pull, transform, combine, split, mirror, pattern, remove, imported, or the kind itself.
std::string feature_category(const std::string& op_type, const json& data);

// The read-only `related` command: the owning feature of each face/edge reference, and per owner the faces it made on
// the picked bodies. args: refs (or ref), kinds (["feature","import","body"], default all), limit (refs per candidate).
json related(const Document& doc, const json& args, const Cancel& cancel = {});

}  // namespace opad::design
