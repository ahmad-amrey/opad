#pragma once
// Versions of a document compared as a person reads them (UI-57). Both sides are usually read in index mode
// (Document::load_index): only the bodies a caller asks about are ever parsed.
#include <filesystem>
#include <string>

#include "document.hpp"
#include "render.hpp"

namespace opad {

struct DiffOptions {
  bool metrics = false;  // volume, area and size of each body whose geometry changed, before and after (parses those)
};

// What changed from `a` to `b`:
//   relation    same | descendant (b continues a) | ancestor (b is an earlier a) | diverged | unrelated (another document)
//   common_ops  length of the op logs' common prefix
//   changes     [{kind, change, id, name, ...}] in a fixed order: units, param, sketch, feature, asset, component and
//               body (tree order), annotation, measurement, section, view. Bodies and components: added (a component
//               with the count of its bodies; what is under it is not listed again), removed, renamed (before/after),
//               moved (local placement only: translation, rotation_deg, axis), geometry (key_before/key_after, metrics
//               on request), appearance (fields: color/opacity/visible/locked before/after), reparented. Features:
//               added, removed, renamed, edited (details [{key, label, before, after}] as text), regenerated (same
//               inputs, other results), suppressed, unsuppressed, failed, fixed. Params: added/removed/renamed/edited.
//               Sketches: added/removed/renamed/edited (points/entities/constraints {added, removed, changed},
//               dimensions [{id, type, before, after}], plane, dof). Notes: added, removed, resolved (tombstoned),
//               reopened, edited, restyled, reanchored, commented. Assets: synced, storage.
//   counts      {kind: {change: n}}
//   summary     one line naming the main changes ("Edit Extrude 1 distance; add Fillet 2"), a commit message draft
//   ops, bodies, geometry   the op-level lists, body-store keys and geometry counts of the first diff (kept for callers)
json semantic_diff(const Document& a, const Document& b, const DiffOptions& opt = {});
// The same with both versions resolved already (a viewer needs the scenes as well).
json semantic_diff(const Document& a, const Scene& sa, const Document& b, const Scene& sb, const DiffOptions& opt = {});
inline json diff_documents(const Document& a, const Document& b) { return semantic_diff(a, b); }

// The bodies of two versions as a viewer draws one over the other (Compare, UI-58), matched by node id in world placement:
// added (only in b), removed (only in a), modified (another body key), moved (the same key placed elsewhere in the world,
// so also a body whose component moved), unchanged. shown_a / shown_b: effectively visible on that side; a body shown on
// neither side is left out, and so is one that exists only on a side where it is hidden. In b's tree order, then a's
// removed bodies in a's.
struct BodyChange {
  enum class Kind { Added, Removed, Modified, Moved, Unchanged };
  Kind kind = Kind::Unchanged;
  std::string id, name, key_a, key_b;
  Mat4 world_a, world_b;
  bool shown_a = false, shown_b = false;
};
std::vector<BodyChange> body_changes(const Scene& a, const Scene& b);
const char* body_change_name(BodyChange::Kind kind);  // "added", "removed", "modified", "moved", "unchanged"
bool same_placement(const Mat4& x, const Mat4& y);     // equal within 1e-9 per entry
// A semantic diff as text, one change per line under a heading per kind (opad-cli diff --text).
std::string diff_text(const json& diff);
// One document as line-oriented text for git's textconv (`diff=opad`): history, parameters, sketches, features, the
// tree, notes and the body store, every body's BREP one line, so a git diff of two versions reads as what changed.
std::string document_outline(const Document& doc);
// The same for text that does not read as a document (conflict markers, a broken file): the error, then the text with
// each body's BREP lines left out.
std::string text_outline(const std::string& text, const std::string& error);
// The text of the version a diff side names: a file, or `git:REV` / `git:REV:path` (`git cat-file blob`): without a
// path, the file `other` names in its own repository; a relative path is the repository's, as git reads it. `file`
// receives the path the version stands for.
std::string version_text(const std::string& spec, const std::filesystem::path& other, std::filesystem::path* file = nullptr);
// Geometric diff image: unchanged grey, only-in-a red, only-in-b green.
Image render_diff(const Document& a, const Document& b, const RenderOptions& opt);
}  // namespace opad
