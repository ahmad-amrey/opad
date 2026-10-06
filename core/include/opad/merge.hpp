#pragma once
// Versions of one document side by side (UI-56): what a file held when it was read or written, how another version
// relates to that, and the three-way merge of two versions that both continue it. The rules are the git merge
// driver's (tools/opad_merge.py): the op log only grows, so a version either continues the base (its ops in the same
// order and unchanged, new ones anywhere after or between them) or rewrote it.
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "document.hpp"

namespace opad {

// A version's op log without the bodies' text: enough to tell later whether another version continues it.
struct Manifest {
  std::string uuid;
  std::string header;  // the header line but its format version (the git driver refuses other header changes)
  std::vector<std::pair<std::string, size_t>> ops;  // id, hash of the persisted record
  std::set<std::string> bodies;
  static Manifest of(const Document& d);
};

enum class Relation {
  same,       // the same op log
  extends,    // the base's ops in order and unchanged, plus new ones
  rewritten,  // a base op changed, went or moved (a reset, rebase, amend or another branch), or the header changed
  other       // another document (header uuid)
};
const char* relation_name(Relation r);
Relation relation(const Manifest& base, const Document& version);

struct MergeConflict {
  std::string target, field;  // what both change (the git driver's conflict keys)
  std::string ours, theirs;   // the op ids
  json to_json() const;
};
// A conflict that only follows from others: one of its two ops is a regen, the results recomputed after a change (one
// parameter changed on both sides is one conflict of its own plus one per result it recomputed on each side). It is no
// decision of its own: regenerating after the decisions settles it. Ours' op is looked up in `ours`, theirs' in `theirs`.
bool derived_conflict(const MergeConflict& c, const Document& ours, const Document& theirs);
// The conflicts to decide (the derived ones left out), in order; `derived` (when given) counts those left out. Resolving
// only these (resolve_merge) and regenerating afterwards is the whole decision: a derived one is never given its own.
std::vector<MergeConflict> source_conflicts(const std::vector<MergeConflict>& conflicts, const Document& ours, const Document& theirs, size_t* derived = nullptr);

// Ours (an open session: the base plus its unsaved ops) and theirs (the file now) both continue the base. Merged, the
// log is theirs and then ours' unsaved ops: the file is only appended to, and where both change the same thing ours,
// applied last, wins (listed in `conflicts`).
struct MergePlan {
  Relation theirs = Relation::same;
  size_t incoming = 0;        // theirs' ops ours does not have
  std::vector<size_t> mine;   // ours' unsaved ops (indices into ours.ops), in order: they go after theirs
  bool appended = false;      // theirs = base + its new ops, so the merge only inserts before ours' unsaved ops
  bool shared = false;        // an unsaved op of ours is in theirs too (identical): it takes theirs' place
  bool design = false;        // both sides change the design (params, sketches, features): regenerate after
  std::vector<MergeConflict> conflicts;
  std::string error;          // why they cannot be merged; empty when they can
};
MergePlan plan_merge(const Manifest& base, const Document& ours, const Document& theirs);
// Applies a plan without an error: ours' log becomes theirs' plus its unsaved ops, and its body store theirs' entries in
// their order (`theirs_bodies`: every key the file lists; those ours has stay, the others move out of `theirs`) followed
// by ours' others. Throws when a body is in neither.
void apply_merge(Document& ours, Document& theirs, const MergePlan& plan, const std::vector<std::string>& theirs_bodies);
// The keys an op changes (target, field), as the git driver computes them: "*" is the whole target.
std::vector<std::pair<std::string, std::string>> op_effects(const json& op);
// What an op makes of one of its keys (UI-63): its kind and the value it sets ([kind, value]). Two new ops of either side
// that change the same key alike (both renamed a part the same, both deleted one note, both set a parameter to 3 mm)
// are no conflict: same_effect.
json op_effect(const json& op, const std::string& target, const std::string& field);
bool same_effect(const json& a, const std::string& fieldA, const json& b, const std::string& fieldB, const std::string& target);

// Whether `version` changes the design after `base` (UI-62): an op base does not have that is a param, sketch, feature or
// regen, or an edit or delete of one. Two branches that both do were computed without each other's changes: regenerate
// after merging them.
bool changes_design(const Document& base, const Document& version);

// History (UI-62): an earlier version of `current` restored as new changes, the log only growing. Every op of `version`
// must be in current's log as written (current continues it, merges in between allowed); replaying it is then what
// tombstones of current's later ops give: one each, except a delete or an edit of one of those later ops (gone with its
// target). The version's bodies current no longer has (gc) come over from it.
struct RestorePlan {
  enum class Problem { none, current, other_document, not_ancestor };
  Problem problem = Problem::none;  // current: nothing came after it; not_ancestor: it has ops current lacks or holds otherwise
  std::string op;                   // not_ancestor: the first such op
  std::vector<std::string> tombstones;  // current's later ops to delete, in log order
  std::vector<std::string> bodies;      // the version's body keys current lacks
  size_t later = 0;                     // current's ops the version does not have
};
RestorePlan plan_restore(const Document& current, const Document& version);
// Appends the plan's tombstones (by `author`) to `current` and moves the bodies it lacks over from `version`. Throws on a
// plan with a problem, or when a body is missing from `version`.
void apply_restore(Document& current, Document& version, const RestorePlan& plan, const std::string& author = {});

// The git merge driver (UI-60) over whole files, rule for rule tools/opad_merge.py (the reference; tests/test_git_merge.py
// runs both). Each file is read as records: '#opad 1|2', a header line, '#ops', ops (JSON objects with a string id, one
// or more lines, no blank or '#' lines between them), '#bodies', '#body <key> <lines> <meta>' entries whose text must
// hash to the key. Lines end at LF (CRLF and CR read as LF). Refused, with the driver's message: a changed header; a base
// op missing, changed or moved in either version; a base body missing; one id with two contents; new ops of both sides
// that change the same (target, field) (op_effects; "*" meets every field); theirs' order not kept. Merged: ours as
// written (each id and key once), then theirs' new ops and bodies in their order; the format the highest of the three.
struct FileMerge {
  std::string error;                     // why a person has to merge it; empty when merged
  std::vector<MergeConflict> conflicts;  // the concurrent changes behind such an error (every pair, not only the first)
  std::string text() const;
  void write(std::ostream& out) const;
  // The merged file as views into the inputs, which the result keeps.
  std::vector<std::string_view> pieces;
  std::vector<std::shared_ptr<const std::string>> texts;
};
// keep_conflicts (UI-63, a merge git stopped on): concurrent changes do not stop it; the merge is made anyway (ours as
// written, then theirs' new ops: theirs win each of them, applied later) with every conflict listed, `error` empty unless
// something else refuses it.
FileMerge merge_files(std::string base, std::string ours, std::string theirs, bool keep_conflicts = false);
// That merge, decided (UI-63): `merged` is merge_files(..., true)'s text, theirs winning every conflict. A conflict decided
// for ours (mine[i]) appends a copy of ours' op (applied last, it wins), or, when theirs' op is a delete, a delete of that
// delete (the target lives again, with ours' change); one decided for theirs against a delete of ours appends a delete of
// ours' delete. Each op once, by `author`. Returns the decided file.
std::string resolve_merge(std::string merged, const std::vector<MergeConflict>& conflicts, const std::vector<bool>& mine,
                          const std::string& author = {});
// git's merge.<driver>.driver with `<base> <ours> <theirs> [<name>]` (%O %A %B %P): writes the merge over <ours> and
// returns 0, or leaves <ours> as it was, says why on stderr and returns 1. `opad-cli merge-driver` and
// `opad.exe --merge-driver` (no Qt, no window: portable and single-file installs need neither Python nor the CLI).
int merge_driver(const std::vector<std::filesystem::path>& args);

}  // namespace opad
