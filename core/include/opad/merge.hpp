#pragma once
// Versions of one document side by side (UI-56): what a file held when it was read or written, how another version
// relates to that, and the three-way merge of two versions that both continue it. The rules are the git merge
// driver's (tools/opad_merge.py): the op log only grows, so a version either continues the base (its ops in the same
// order and unchanged, new ones anywhere after or between them) or rewrote it.
#include <set>
#include <string>
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

}  // namespace opad
