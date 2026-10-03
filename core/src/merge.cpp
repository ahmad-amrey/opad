#include "opad/merge.hpp"

#include <algorithm>
#include <map>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace opad {
namespace {
size_t record_hash(const Op& o) {
  return o.raw.empty() ? std::hash<std::string>{}(o.data.dump()) : std::hash<std::string_view>{}(o.raw);
}
bool design_type(const std::string& type) { return type == "param" || type == "sketch" || type == "feature" || type == "regen"; }
}  // namespace

Manifest Manifest::of(const Document& d) {
  Manifest m;
  m.uuid = d.header.uuid;
  m.header = d.header.to_json().dump();
  m.ops.reserve(d.ops.size());
  for (const auto& o : d.ops) m.ops.emplace_back(o.id, record_hash(o));
  for (const auto& b : d.bodies()) m.bodies.insert(b.key);
  return m;
}

const char* relation_name(Relation r) {
  switch (r) {
    case Relation::same: return "same";
    case Relation::extends: return "extends";
    case Relation::rewritten: return "rewritten";
    default: return "other";
  }
}

Relation relation(const Manifest& base, const Document& version) {
  if (version.header.uuid != base.uuid) return Relation::other;
  if (version.header.to_json().dump() != base.header) return Relation::rewritten;
  std::unordered_set<std::string_view> ids;
  for (const auto& [id, hash] : base.ops) ids.insert(id);
  size_t next = 0;
  for (const auto& o : version.ops) {
    if (!ids.count(o.id)) continue;  // new
    if (next >= base.ops.size() || o.id != base.ops[next].first || record_hash(o) != base.ops[next].second) return Relation::rewritten;
    ++next;
  }
  if (next < base.ops.size()) return Relation::rewritten;
  return version.ops.size() == base.ops.size() ? Relation::same : Relation::extends;
}

json MergeConflict::to_json() const { return {{"target", target}, {"field", field}, {"ours", ours}, {"theirs", theirs}}; }

std::vector<std::pair<std::string, std::string>> op_effects(const json& op) {
  std::vector<std::pair<std::string, std::string>> out;
  const std::string kind = op.value("op", "");
  const std::string target = op.contains("target") && op["target"].is_string() ? op["target"].get<std::string>() : std::string();
  if (kind == "regen") {
    if (op.contains("results") && op["results"].is_object())
      for (const auto& [key, value] : op["results"].items()) out.emplace_back(key, "geometry");
  } else if (kind == "param") {
    out.emplace_back("parameter:" + op.value("name", ""), "*");
  } else if (kind == "edit") {
    if (op.contains("set") && op["set"].is_object())
      for (const auto& [key, value] : op["set"].items())
        out.emplace_back(target, key == "geometry_delta" || key == "geometry" || key == "result" || key == "inputs" || key == "plane" ? "geometry" : key);
  } else if (kind == "delete") {
    out.emplace_back(target, "*");
  } else if (!target.empty()) {
    for (const auto& [key, value] : op.items())
      if (key != "op" && key != "id" && key != "ts" && key != "by" && key != "target") out.emplace_back(target, key);
  }
  return out;
}

MergePlan plan_merge(const Manifest& base, const Document& ours, const Document& theirs) {
  MergePlan p;
  p.theirs = relation(base, theirs);
  if (p.theirs == Relation::other) { p.error = "the file is another document"; return p; }
  if (p.theirs == Relation::rewritten) { p.error = "the history in the file was rewritten"; return p; }
  // A session only appends to what it read or wrote, or undoes: undone past that, its log no longer continues the base.
  const size_t n = base.ops.size();
  bool continues = ours.header.uuid == base.uuid && ours.ops.size() >= n;
  for (size_t i = 0; continues && i < n; ++i) continues = ours.ops[i].id == base.ops[i].first;
  if (!continues) { p.error = "changes saved before were undone"; return p; }
  p.appended = theirs.ops.size() >= n;
  for (size_t i = 0; p.appended && i < n; ++i) p.appended = theirs.ops[i].id == base.ops[i].first;
  std::unordered_map<std::string_view, size_t> fresh;  // theirs' ops the base lacks
  std::unordered_set<std::string_view> baseIds;
  for (const auto& [id, hash] : base.ops) baseIds.insert(id);
  for (size_t i = 0; i < theirs.ops.size(); ++i)
    if (!baseIds.count(theirs.ops[i].id)) fresh.emplace(theirs.ops[i].id, i);
  std::unordered_set<size_t> both;
  for (size_t i = n; i < ours.ops.size(); ++i) {
    const auto it = fresh.find(ours.ops[i].id);
    if (it == fresh.end()) { p.mine.push_back(i); continue; }
    if (theirs.ops[it->second].raw != ours.ops[i].raw) { p.error = "both versions hold a different operation " + ours.ops[i].id; return p; }
    p.shared = true;
    both.insert(it->second);
  }
  p.incoming = fresh.size() - both.size();
  // Conflicts: what ours' unsaved ops and theirs' new ones both change.
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> changed;  // target -> (field, theirs op)
  std::unordered_map<std::string_view, const std::string*> types;
  for (const auto& o : theirs.ops) types.emplace(o.id, &o.type);
  for (size_t i : p.mine) types.emplace(ours.ops[i].id, &ours.ops[i].type);
  auto design = [&](const Op& o) {
    if (design_type(o.type)) return true;
    if (o.type != "edit" && o.type != "delete") return false;
    const auto it = types.find(o.data.value("target", ""));
    return it != types.end() && design_type(*it->second);
  };
  bool theirsDesign = false, oursDesign = false;
  for (size_t i = 0; i < theirs.ops.size(); ++i) {
    if (baseIds.count(theirs.ops[i].id) || both.count(i)) continue;
    theirsDesign = theirsDesign || design(theirs.ops[i]);
    for (auto& [target, field] : op_effects(theirs.ops[i].data)) changed[target].emplace_back(field, theirs.ops[i].id);
  }
  std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
  for (size_t i : p.mine) {
    oursDesign = oursDesign || design(ours.ops[i]);
    for (const auto& [target, field] : op_effects(ours.ops[i].data)) {
      const auto it = changed.find(target);
      if (it == changed.end()) continue;
      for (const auto& [other, op] : it->second)
        if (field == other || field == "*" || other == "*") {
          MergeConflict c{target, field == "*" ? other : field, ours.ops[i].id, op};
          if (seen.emplace(c.target, c.field, c.ours, c.theirs).second) p.conflicts.push_back(std::move(c));
        }
    }
  }
  p.design = theirsDesign && oursDesign;
  return p;
}

void apply_merge(Document& ours, Document& theirs, const MergePlan& plan, const std::vector<std::string>& theirs_bodies) {
  if (!plan.error.empty()) throw Error("cannot merge: " + plan.error);
  for (size_t i : plan.mine)
    if (i >= ours.ops.size()) throw Error("cannot merge: the plan is out of date");
  ours.arrange_bodies(theirs_bodies, theirs, true);
  std::vector<Op> log = std::move(theirs.ops);
  theirs.ops.clear();
  log.reserve(log.size() + plan.mine.size());
  for (size_t i : plan.mine) log.push_back(std::move(ours.ops[i]));
  ours.ops = std::move(log);
  ours.dirty = true;
}

}  // namespace opad
