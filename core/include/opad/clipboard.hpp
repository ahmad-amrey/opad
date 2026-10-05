#pragma once
// Bodies and components on the clipboard (TODO 11 UI-129). Copy writes the copied nodes as JSON (format
// "opad.nodes.clipboard"): each with its name, colour, representation and body key, the top ones with their parent and
// where they are in the world, and, while they stay under a size limit, the body entries themselves (BREP and meta) so
// that another document can take them. A paste is import ops (one per parent, source "clipboard") of new nodes on the
// same body keys: in the document they came from they are the same parts again (linked instances, as a part imported
// twice); in another one the entries come along from the clip. Nothing here touches the clipboard itself (the app does).
#include <string>
#include <vector>

#include "document.hpp"
#include "scene.hpp"

namespace opad {

constexpr const char* kNodesClipFormat = "opad.nodes.clipboard";

// `ids`: bodies and components (a component brings everything under it; a node under another copied one comes with it,
// once); anything else is passed over. Throws when nothing is left.
json copy_nodes(const Document& doc, const Scene& scene, const std::vector<std::string>& ids, size_t brep_limit = size_t(32) << 20);

struct PastePlan {
  std::vector<json> ops;             // import ops (no id yet), one per parent
  std::vector<std::string> nodes;    // the top nodes they make, in the clip's order
  std::vector<std::string> missing;  // body keys the document lacks: the clip's entries are added first
};
// In the clip's own document a top node goes back under its parent (when that is still a component), placed where it was
// moved by `offset` (world mm); in another document, or when its parent is gone, at the root where it was in the world.
// Throws when a key is in neither the document nor the clip.
PastePlan plan_paste(const Document& doc, const Scene& scene, const json& clip, const Vec3& offset = {0, 0, 0});
// plan_paste, then the missing entries added and the ops appended: {operation_ids, ids, bodies_added}.
json paste_nodes(Document& doc, const json& clip, const Vec3& offset = {0, 0, 0}, const std::string& author = {});

}  // namespace opad
