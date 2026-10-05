// Reconciling versions of one document (UI-56): manifests, how a version relates to a base, and the in-memory
// three-way merge (the git driver's rules, ours' unsaved ops after theirs); and the git driver itself over whole files
// (UI-60). No OCCT geometry involved.
#include <filesystem>
#include <set>
#include <sstream>

#include "check.hpp"
#include "opad/document.hpp"
#include "opad/merge.hpp"
#include "opad/scene.hpp"
#include "opad/util.hpp"

using namespace opad;

namespace {
std::string brep(int i) { return "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations " + std::to_string(i) + "\n"; }

// An import of one fake body; returns the body node id.
std::string import_body(Document& d, int i, const std::string& name) {
  const std::string key = d.add_body(brep(i), json{{"name", name}});
  const std::string id = new_uuid();
  d.append(json{{"op", "import"}, {"source", name + ".step"}, {"nodes", json::array({json{{"type", "body"}, {"id", id}, {"name", name}, {"key", key}}})}});
  return id;
}
void rename(Document& d, const std::string& target, const std::string& name) { d.append(json{{"op", "rename"}, {"target", target}, {"name", name}}); }
std::string ops_part(const std::string& text) { return text.substr(0, text.find("#bodies\n")); }
std::string bodies_part(const std::string& text) { return text.substr(text.find("#bodies\n")); }

struct Versions {
  std::string base_text;
  Manifest base;
  std::string a, b;  // body node ids
};
Versions make_base() {
  Document d = Document::create();
  Versions v;
  v.a = import_body(d, 1, "A");
  v.b = import_body(d, 2, "B");
  v.base_text = d.serialize();
  v.base = Manifest::of(Document::parse(v.base_text));
  return v;
}
// The file as an open session would read it: bodies the base lists are skipped and every key is listed in order.
Document read_skipping(const std::string& text, const Manifest& base, std::vector<std::string>& keys) {
  return Document::parse(text, {}, [&](const std::string& key) {
    keys.push_back(key);
    return base.bodies.count(key) > 0;
  });
}
}  // namespace

TEST(relation_of_versions) {
  Versions v = make_base();
  CHECK(relation(v.base, Document::parse(v.base_text)) == Relation::same);
  Document more = Document::parse(v.base_text);
  rename(more, v.a, "A2");
  const std::string moreText = more.serialize();
  CHECK(relation(v.base, more) == Relation::extends);
  Document fewer = Document::parse(v.base_text);
  fewer.truncate_ops(1);
  CHECK(relation(v.base, fewer) == Relation::rewritten);  // a reset
  std::string edited = v.base_text;
  edited.replace(edited.find("\"A.step\""), 8, "\"X.step\"");
  CHECK(relation(v.base, Document::parse(edited)) == Relation::rewritten);  // a saved op changed in place
  // The new op between the two base ops (as a git merge of two branches writes it): still a continuation.
  std::vector<std::string> lines;
  for (size_t at = 0, nl; (nl = moreText.find('\n', at)) != std::string::npos; at = nl + 1) lines.push_back(moreText.substr(at, nl - at + 1));
  std::swap(lines[4], lines[5]);  // header, uuid, #ops, import A, import B, rename -> rename before import B
  std::string between;
  for (const auto& l : lines) between += l;
  CHECK(relation(v.base, Document::parse(between)) == Relation::extends);
  std::swap(lines[3], lines[5]);  // the base ops swapped: reordered history
  std::string swapped;
  for (const auto& l : lines) swapped += l;
  CHECK(relation(v.base, Document::parse(swapped)) == Relation::rewritten);
  CHECK(relation(v.base, Document::create()) == Relation::other);
  Document units = Document::parse(v.base_text);
  units.header.units = "in";  // the header itself changed: the driver refuses that too
  CHECK(relation(v.base, units) == Relation::rewritten);
  CHECK_EQ(std::string(relation_name(Relation::extends)), "extends");
}

TEST(parse_skips_known_bodies_unverified) {
  Versions v = make_base();
  Document theirs = Document::parse(v.base_text);
  const std::string key3 = theirs.add_body(brep(3), json::object());
  theirs.append(json{{"op", "import"}, {"nodes", json::array({json{{"type", "body"}, {"id", new_uuid()}, {"name", "C"}, {"key", key3}}})}});
  std::string text = theirs.serialize();
  std::vector<std::string> keys;
  Document read = read_skipping(text, v.base, keys);
  CHECK_EQ(keys.size(), 3u);
  CHECK_EQ(read.body_count(), 1u);
  CHECK(read.has_body(key3));
  CHECK_EQ(read.ops.size(), 3u);
  std::string tampered = text;  // a skipped body is not even read: a damaged copy does not matter
  tampered.replace(tampered.find("Locations 1"), 11, "Locations 9");
  keys.clear();
  CHECK_EQ(read_skipping(tampered, v.base, keys).body_count(), 1u);
  CHECK_THROWS(Document::parse(tampered));
}

TEST(clean_session_fast_forwards_to_the_file) {
  Versions v = make_base();
  Document ours = Document::parse(v.base_text);
  Document disk = Document::parse(v.base_text);
  import_body(disk, 3, "C");
  rename(disk, v.b, "B2");
  const std::string diskText = disk.serialize();
  std::vector<std::string> keys;
  Document theirs = read_skipping(diskText, v.base, keys);
  MergePlan plan = plan_merge(v.base, ours, theirs);
  CHECK(plan.error.empty());
  CHECK(plan.theirs == Relation::extends && plan.appended && !plan.shared && !plan.design);
  CHECK_EQ(plan.incoming, 2u);
  CHECK(plan.mine.empty() && plan.conflicts.empty());
  apply_merge(ours, theirs, plan, keys);
  CHECK_EQ(ours.serialize(), diskText);  // byte for byte the file
}

TEST(unsaved_ops_go_after_the_files_new_ones) {
  Versions v = make_base();
  Document ours = Document::parse(v.base_text);
  rename(ours, v.a, "Mine");
  import_body(ours, 4, "D");  // an unsaved body of ours
  Document disk = Document::parse(v.base_text);
  disk.append(json{{"op", "appearance"}, {"target", v.a}, {"color", {1, 0, 0}}});
  import_body(disk, 3, "C");
  const std::string diskText = disk.serialize();
  std::vector<std::string> keys;
  Document theirs = read_skipping(diskText, v.base, keys);
  MergePlan plan = plan_merge(v.base, ours, theirs);
  CHECK(plan.error.empty() && plan.conflicts.empty());
  CHECK_EQ(plan.incoming, 2u);
  CHECK_EQ(plan.mine.size(), 2u);
  apply_merge(ours, theirs, plan, keys);
  CHECK_EQ(ours.ops.size(), 6u);
  const std::string merged = ours.serialize();
  // The file is only appended to, in both sections.
  CHECK_EQ(ops_part(merged).substr(0, ops_part(diskText).size()), ops_part(diskText));
  CHECK_EQ(bodies_part(merged).substr(0, bodies_part(diskText).size()), bodies_part(diskText));
  CHECK_EQ(ours.body_count(), 4u);
  const Scene s = resolve(Document::parse(merged));
  CHECK_EQ(s.node(v.a)->name, "Mine");
  CHECK(s.node(v.a)->has_color && s.node(v.a)->color[0] == 1.0);
  CHECK_EQ(s.all_bodies().size(), 4u);
  CHECK(relation(Manifest::of(Document::parse(diskText)), ours) == Relation::extends);
}

TEST(conflicts_follow_the_driver_rules) {
  Versions v = make_base();
  Document ours = Document::parse(v.base_text);
  Document disk = Document::parse(v.base_text);
  rename(ours, v.a, "Mine");
  rename(disk, v.a, "Theirs");
  rename(ours, v.b, "B mine");                                                      // different targets: no conflict
  disk.append(json{{"op", "appearance"}, {"target", v.b}, {"visible", false}});    // a different field: no conflict
  ours.append(json{{"op", "param"}, {"name", "wall"}, {"expr", "2 mm"}});
  disk.append(json{{"op", "param"}, {"name", "wall"}, {"expr", "3 mm"}});
  const std::string importA = Document::parse(v.base_text).ops[0].id;
  disk.append(json{{"op", "delete"}, {"target", importA}});
  ours.append(json{{"op", "edit"}, {"target", importA}, {"set", {{"source", "y.step"}}}});
  std::vector<std::string> keys;
  Document theirs = read_skipping(disk.serialize(), v.base, keys);
  MergePlan plan = plan_merge(v.base, ours, theirs);
  CHECK(plan.error.empty());
  std::set<std::string> found;
  for (const auto& c : plan.conflicts) found.insert(c.target + "/" + c.field);
  CHECK_EQ(found.size(), 3u);
  CHECK(found.count(v.a + "/name") && found.count("parameter:wall/*") && found.count(importA + "/source"));
  CHECK(plan.conflicts[0].to_json().contains("theirs"));
  apply_merge(ours, theirs, plan, keys);
  const Scene s = resolve(ours);
  CHECK_EQ(s.node(v.b)->name, "B mine");
  CHECK(!s.node(v.b)->visible);
  CHECK_EQ(op_effects(json{{"op", "regen"}, {"results", {{"x", 1}}}}).front().second, "geometry");
  CHECK_EQ(op_effects(json{{"op", "edit"}, {"target", "t"}, {"set", {{"inputs", 1}}}}).front().second, "geometry");
}

TEST(merge_refuses_what_it_cannot_order) {
  Versions v = make_base();
  Document fewer = Document::parse(v.base_text);
  fewer.truncate_ops(1);
  CHECK(!plan_merge(v.base, Document::parse(v.base_text), fewer).error.empty());  // the file was reset
  CHECK(!plan_merge(v.base, Document::parse(v.base_text), Document::create()).error.empty());  // another document
  Document undone = Document::parse(v.base_text);
  undone.truncate_ops(1);
  rename(undone, v.a, "after undo");
  Document disk = Document::parse(v.base_text);
  rename(disk, v.b, "B2");
  CHECK(!plan_merge(v.base, undone, disk).error.empty());  // ours undid saved ops
  MergePlan bad = plan_merge(v.base, undone, disk);
  CHECK_THROWS(apply_merge(undone, disk, bad, {}));
  // The same op id with different content on both sides.
  Document ours = Document::parse(v.base_text), theirs = Document::parse(v.base_text);
  const std::string id = new_uuid();
  ours.append(json{{"op", "rename"}, {"id", id}, {"target", v.a}, {"name", "one"}});
  theirs.append(json{{"op", "rename"}, {"id", id}, {"target", v.a}, {"name", "two"}});
  CHECK(!plan_merge(v.base, ours, theirs).error.empty());
}

TEST(new_ops_between_saved_ones_and_shared_ops) {
  Versions v = make_base();
  Document ours = Document::parse(v.base_text);
  const std::string id = new_uuid();
  const json same{{"op", "rename"}, {"id", id}, {"ts", "2026-10-03T00:00:00Z"}, {"by", "t"}, {"target", v.a}, {"name", "same"}};
  ours.append(same);
  rename(ours, v.b, "Mine");
  Document disk = Document::parse(Document::parse(v.base_text).serialize());
  disk.append(same);  // both have it (an earlier save)
  disk.append(json{{"op", "appearance"}, {"target", v.a}, {"opacity", 0.5}});
  // Theirs puts its ops between the two saved imports, as a merged branch does.
  Document between = Document::parse(v.base_text);
  between.ops.insert(between.ops.begin() + 1, disk.ops.begin() + 2, disk.ops.end());
  std::vector<std::string> keys;
  Document theirs = read_skipping(between.serialize(), v.base, keys);
  MergePlan plan = plan_merge(v.base, ours, theirs);
  CHECK(plan.error.empty() && !plan.appended && plan.shared);
  CHECK_EQ(plan.incoming, 1u);
  CHECK_EQ(plan.mine.size(), 1u);
  apply_merge(ours, theirs, plan, keys);
  CHECK_EQ(ours.ops.size(), 5u);
  CHECK_EQ(ours.ops[1].id, id);
  CHECK_EQ(resolve(ours).node(v.b)->name, "Mine");
}

TEST(design_on_both_sides_asks_for_regeneration) {
  Versions v = make_base();
  Document ours = Document::parse(v.base_text), disk = Document::parse(v.base_text);
  ours.append(json{{"op", "param"}, {"name", "a"}, {"expr", "1 mm"}});
  disk.append(json{{"op", "param"}, {"name", "b"}, {"expr", "2 mm"}});
  std::vector<std::string> keys;
  Document theirs = read_skipping(disk.serialize(), v.base, keys);
  MergePlan plan = plan_merge(v.base, ours, theirs);
  CHECK(plan.error.empty() && plan.design && plan.conflicts.empty());
  Document plain = Document::parse(v.base_text);
  rename(plain, v.a, "x");
  CHECK(!plan_merge(v.base, plain, theirs).design);
}

// The git driver over whole files (UI-60); tests/test_git_merge.py compares it with tools/opad_merge.py.
TEST(file_merge_keeps_ours_and_appends_theirs) {
  Versions v = make_base();
  Document ours = Document::parse(v.base_text), theirs = Document::parse(v.base_text);
  rename(ours, v.a, "Mine");
  import_body(ours, 4, "D");
  rename(theirs, v.b, "Theirs");
  import_body(theirs, 3, "C");
  import_body(theirs, 4, "D again");  // the same body as ours' D: one entry
  const std::string o = ours.serialize(), t = theirs.serialize();
  const FileMerge m = merge_files(v.base_text, o, t);
  CHECK(m.error.empty() && m.conflicts.empty());
  const std::string text = m.text();
  std::ostringstream written;
  m.write(written);
  CHECK_EQ(written.str(), text);
  CHECK_EQ(ops_part(text).substr(0, ops_part(o).size()), ops_part(o));  // ours as written, then theirs' new ops
  CHECK_EQ(bodies_part(text).substr(0, bodies_part(o).size()), bodies_part(o));
  const Document d = Document::parse(text);
  CHECK_EQ(d.ops.size(), 7u);
  CHECK_EQ(d.body_count(), 4u);
  CHECK(d.ops[3].raw == ours.ops[3].raw && d.ops[4].raw == theirs.ops[2].raw && d.ops[6].raw == theirs.ops[4].raw);
  const Scene s = resolve(d);
  CHECK(s.node(v.a)->name == "Mine" && s.node(v.b)->name == "Theirs");
  // CRLF and CR read as LF; Python's == on JSON: key order, 1 == 1.0 == true.
  std::string crlf;
  for (char c : o) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
  CHECK_EQ(merge_files(v.base_text, crlf, t).text(), text);
  const std::string renamed = ours.ops[2].raw;
  json same = json::parse(renamed);
  json reordered = json::object();
  for (auto it = same.rbegin(); it != same.rend(); ++it) reordered[it.key()] = *it;
  std::string flipped = o;
  flipped.replace(flipped.find(renamed), renamed.size(), reordered.dump());
  CHECK(merge_files(v.base_text, flipped, t).error.empty());
  std::string base1 = v.base_text;
  base1.replace(0, 7, "#opad 1");
  CHECK_EQ(merge_files(base1, o, t).text().substr(0, 8), "#opad 2\n");
}

TEST(file_merge_refuses_as_the_driver_does) {
  Versions v = make_base();
  const std::string importA = Document::parse(v.base_text).ops[0].id;
  Document ours = Document::parse(v.base_text), theirs = Document::parse(v.base_text);
  rename(ours, v.a, "Mine");
  ours.append(json{{"op", "delete"}, {"target", importA}});
  rename(theirs, v.a, "Theirs");
  theirs.append(json{{"op", "edit"}, {"target", importA}, {"set", {{"source", "x.step"}}}});
  const std::string o = ours.serialize(), t = theirs.serialize();
  const FileMerge m = merge_files(v.base_text, o, t);
  CHECK_EQ(m.error, "concurrent changes to " + v.a + "/name; manual review required");
  CHECK(m.pieces.empty() && m.text().empty());
  CHECK_EQ(m.conflicts.size(), 2u);
  CHECK(m.conflicts[0].target == v.a && m.conflicts[0].field == "name" && m.conflicts[0].ours == ours.ops[2].id);
  CHECK(m.conflicts[1].target == importA && m.conflicts[1].field == "source" && m.conflicts[1].theirs == theirs.ops[3].id);
  auto refused = [&](std::string base, std::string a, std::string b) { return merge_files(std::move(base), std::move(a), std::move(b)).error; };
  CHECK_EQ(refused(v.base_text, o, t.substr(0, t.find("#bodies\n"))), "missing body store");
  std::string corrupt = t;
  corrupt.replace(corrupt.find("Locations 1"), 11, "Locations 7");
  CHECK_EQ(refused(v.base_text, o, corrupt), "body hash mismatch");
  corrupt += "#body x 1\n";  // a malformed entry after the corrupted one: the hash is what the driver meets first
  CHECK_EQ(refused(v.base_text, o, corrupt), "body hash mismatch");
  CHECK_EQ(refused(v.base_text, o, t + "#body x 1\n"), "invalid body header");
  CHECK_EQ(refused(v.base_text, o, t + "#body x 9 {}\n"), "invalid body length");
  std::string blank = o;
  blank.insert(blank.find("#bodies\n"), "\n");
  CHECK_EQ(refused(v.base_text, blank, t), "unexpected metadata in operation log");
  CHECK_EQ(refused(v.base_text, o, "\xef\xbb\xbf" + t), "unsupported OPAD header");
  CHECK_EQ(refused(v.base_text, o, t + "\xff"), "not UTF-8 text");
  std::string units = t;
  units.replace(units.find("\"mm\""), 4, "\"in\"");
  CHECK_EQ(refused(v.base_text, o, units), "document header changed; manual review required");
  CHECK_EQ(refused(v.base_text, o, Document::create().serialize()), "document header changed; manual review required");
  Document fewer = Document::parse(v.base_text);
  fewer.truncate_ops(1);
  CHECK_EQ(refused(v.base_text, o, fewer.serialize()), "history was rewritten; manual review required");
  std::vector<std::string> keys;
  CHECK_EQ(refused(v.base_text, o, read_skipping(t, v.base, keys).serialize()), "body store was pruned; manual review required");
}

// UI-63: a merge git stopped on, made anyway (theirs win, the conflicts listed) and decided per conflict: ours' op again
// after theirs', a delete of a delete to bring its target back; changes alike on both sides are no conflict.
TEST(stopped_merge_resolved_per_conflict) {
  Versions v = make_base();
  const std::string importA = Document::parse(v.base_text).ops[0].id;
  Document ours = Document::parse(v.base_text), theirs = Document::parse(v.base_text);
  rename(ours, v.a, "Mine");
  ours.append(json{{"op", "appearance"}, {"target", v.b}, {"color", {0, 0, 1}}, {"locked", true}});
  ours.append(json{{"op", "delete"}, {"target", importA}});
  rename(theirs, v.a, "Theirs");
  theirs.append(json{{"op", "appearance"}, {"target", v.b}, {"color", {1, 0, 0}}, {"locked", true}});
  theirs.append(json{{"op", "edit"}, {"target", importA}, {"set", {{"source", "x.step"}}}});
  const std::string o = ours.serialize(), t = theirs.serialize();
  CHECK(!merge_files(v.base_text, o, t).error.empty());
  const FileMerge m = merge_files(v.base_text, o, t, true);
  CHECK(m.error.empty() && !m.text().empty());
  CHECK_EQ(m.conflicts.size(), 3u);  // the name, the colour (both lock it alike: no conflict), the import deleted / edited
  CHECK(m.conflicts[0].field == "name" && m.conflicts[1].field == "color" && m.conflicts[2].target == importA);
  const Scene asIs = resolve(Document::parse(m.text()));
  CHECK(!asIs.node(v.a) && asIs.node(v.b)->color[0] == 1 && asIs.node(v.b)->locked);  // a delete wins as it is: its import stays deleted
  // Ours for the name and the deleted import, theirs for the colour.
  const Document mine = Document::parse(resolve_merge(m.text(), m.conflicts, {true, false, true}, "me"));
  const Scene s = resolve(mine);
  CHECK(!s.node(v.a) && s.node(v.b)->color[0] == 1 && s.node(v.b)->locked);
  CHECK_EQ(mine.ops.size(), Document::parse(m.text()).ops.size() + 1);  // one copy: ours' delete already wins
  CHECK(mine.ops.back().type == "rename" && mine.ops.back().data["by"] == "me" && mine.ops.back().id != ours.ops[2].id);
  // Theirs for the import: ours' delete deleted, the import back with their edit.
  const Document back = Document::parse(resolve_merge(m.text(), m.conflicts, {false, true, false}));
  const Scene b = resolve(back);
  CHECK(b.node(v.a) && b.node(v.a)->name == "Theirs" && b.node(v.b)->color[2] == 1);
  CHECK(back.ops.back().type == "delete" && back.ops.back().data["target"] == ours.ops[4].id);
  // Alike on both sides: no conflict at all.
  Document left = Document::parse(v.base_text), right = Document::parse(v.base_text);
  rename(left, v.a, "Same");
  left.append(json{{"op", "delete"}, {"target", importA}});
  right.append(json{{"op", "param"}, {"name", "w"}, {"expr", "1 mm"}});
  rename(right, v.a, "Same");
  right.append(json{{"op", "delete"}, {"target", importA}});
  const FileMerge alike = merge_files(v.base_text, left.serialize(), right.serialize());
  CHECK(alike.error.empty() && alike.conflicts.empty());
  CHECK(same_effect(left.ops[2].data, "name", right.ops[3].data, "name", v.a) && !same_effect(ours.ops[2].data, "name", theirs.ops[2].data, "name", v.a));
  std::vector<std::string> keys;
  CHECK(plan_merge(v.base, left, read_skipping(right.serialize(), v.base, keys)).conflicts.empty());
}

// UI-84 with UI-63: a parts list's numbers settled on both sides merge number by number; numbers that do not merge (one
// item changed on both sides, one part under two numbers) stop the driver, and the merge kept for review lists the two
// edits as an ordinary numbers conflict instead of failing as a whole.
TEST(parts_list_numbers_merge_or_stay_a_conflict) {
  Versions v = make_base();
  Document d = Document::parse(v.base_text);
  const json a = {{"n", 1}, {"identity", "a"}};
  const std::string list = d.append(json{{"op", "sheet_item"}, {"sheet", new_uuid()}, {"kind", "parts_list"}, {"numbers", json::array({a})}}).id;
  const std::string base = d.serialize();
  auto numbered = [&](json numbers, std::string* id = nullptr) {
    Document s = Document::parse(base);
    const std::string op = s.append(json{{"op", "edit"}, {"target", list}, {"set", {{"numbers", std::move(numbers)}}}}).id;
    if (id) *id = op;
    return s.serialize();
  };
  // Number 2 given to a different new part on each side: ours keeps it, the merge writes the settled list last.
  const std::string o = numbered(json::array({a, json{{"n", 2}, {"identity", "left"}}}));
  const std::string t = numbered(json::array({a, json{{"n", 2}, {"identity", "right"}}}));
  for (const bool keep : {false, true}) {
    const FileMerge m = merge_files(base, o, t, keep);
    CHECK(m.error.empty() && m.conflicts.empty());
    const Document merged = Document::parse(m.text());
    CHECK(merged.ops.back().data["by"] == "merge" && merged.ops.back().data["set"]["numbers"].size() == 2);
  }
  // Item 1 changed on both sides, and one part numbered 1 and 2: refused by the driver, a conflict in the kept merge.
  std::string ours, theirs;
  const std::string left = numbered(json::array({json{{"n", 1}, {"identity", "left"}}}), &ours);
  const std::string right = numbered(json::array({json{{"n", 1}, {"identity", "right"}}}), &theirs);
  std::string twiceOurs, twiceTheirs;
  const std::string both = numbered(json::array({a, json{{"n", 2}, {"identity", "b"}}}), &twiceOurs);
  const std::string moved = numbered(json::array({json{{"n", 1}, {"identity", "b"}}}), &twiceTheirs);
  CHECK(merge_files(base, left, right).error.find("item 1 of parts list " + list + " changed on both sides") == 0);
  CHECK(merge_files(base, both, moved).error.find("part b numbered 1 and 2 in parts list " + list) == 0);
  for (const auto& [o2, t2, idO, idT] : {std::tuple(left, right, ours, theirs), std::tuple(both, moved, twiceOurs, twiceTheirs)}) {
    const FileMerge kept = merge_files(base, o2, t2, true);
    CHECK(kept.error.empty() && !kept.text().empty());
    CHECK_EQ(kept.conflicts.size(), 1u);
    CHECK(kept.conflicts[0].target == list && kept.conflicts[0].field == "numbers" && kept.conflicts[0].ours == idO && kept.conflicts[0].theirs == idT);
    CHECK(Document::parse(kept.text()).ops.back().id == idT);  // no merge record: theirs' edit stands until decided
  }
  const FileMerge kept = merge_files(base, left, right, true);
  const Document mine = Document::parse(resolve_merge(kept.text(), kept.conflicts, {true}, "me"));
  CHECK(mine.ops.back().type == "edit" && mine.ops.back().data["set"]["numbers"][0]["identity"] == "left" && mine.ops.back().data["by"] == "me");
}

TEST(merge_driver_writes_ours_only_when_merged) {
  Versions v = make_base();
  Document ours = Document::parse(v.base_text), theirs = Document::parse(v.base_text);
  rename(ours, v.a, "Mine");
  rename(theirs, v.b, "Theirs");
  const auto dir = std::filesystem::temp_directory_path() / ("opad-merge-driver-" + new_uuid());
  std::filesystem::create_directories(dir);
  const auto base = dir / "base", mine = dir / "ours", other = dir / "theirs";
  write_text_file(base, v.base_text);
  write_text_file(mine, ours.serialize());
  write_text_file(other, theirs.serialize());
  CHECK_EQ(merge_driver({base, mine}), 1);
  CHECK_EQ(merge_driver({base, mine, other, "part.opad"}), 0);
  const std::string merged = read_text_file(mine);
  CHECK_EQ(merged, merge_files(v.base_text, ours.serialize(), theirs.serialize()).text());
  CHECK(!std::filesystem::exists(dir / "ours.tmp"));
  rename(theirs, v.a, "Again");
  write_text_file(other, theirs.serialize());
  CHECK_EQ(merge_driver({base, mine, other}), 1);
  CHECK_EQ(read_text_file(mine), merged);
  CHECK_EQ(merge_driver({base, dir / "missing", other}), 1);
  std::filesystem::remove_all(dir);
}

TEST(arrange_bodies_checks_before_moving) {
  Versions v = make_base();
  Document d = Document::parse(v.base_text), other = Document::create();
  const std::string key3 = other.add_body(brep(3), json::object());
  const auto before = d.body_keys();
  CHECK_THROWS(d.arrange_bodies({before[1], std::string(64, 'f')}, other, true));
  CHECK(d.body_keys() == before);
  CHECK_EQ(other.body_count(), 1u);
  d.arrange_bodies({key3, before[1]}, other, false);
  CHECK(d.body_keys() == (std::vector<std::string>{key3, before[1]}));
  CHECK_EQ(d.body(key3)->brep, brep(3));
  CHECK_EQ(other.body_count(), 0u);
}

// History (UI-62): an earlier version restored as new changes replays as that version did.
namespace {
std::string picture(const Scene& s) {  // what replay gives: the bodies (name, key, shown) and the parameters
  std::set<std::string> lines;
  for (const auto& id : s.all_bodies()) {
    const Node* n = s.node(id);
    lines.insert(id + " " + n->name + " " + n->body_key + (s.effectively_visible(id) ? "" : " hidden"));
  }
  for (const auto& p : s.params) lines.insert("param " + p.name + "=" + p.expr);
  std::string out;
  for (const auto& l : lines) out += l + "\n";
  return out;
}
}  // namespace

TEST(restore_an_earlier_version_as_new_changes) {
  Versions v = make_base();
  Document version = Document::parse(v.base_text);
  const std::string param = version.append(json{{"op", "param"}, {"name", "w"}, {"expr", "2 mm"}}).id;
  const std::string nodeD = import_body(version, 4, "D");
  const std::string importD = version.ops.back().id, keyD = version.body_keys().back();
  const std::string versionText = version.serialize();
  CHECK(changes_design(Document::parse(v.base_text), version) && !changes_design(version, Document::parse(v.base_text)));
  Document current = Document::parse(versionText);
  rename(current, v.a, "A2");
  current.append(json{{"op", "edit"}, {"target", param}, {"set", {{"expr", "3 mm"}}}});
  const std::string nodeC = import_body(current, 3, "C");
  const std::string importC = current.ops.back().id;
  const std::string deleteB = current.append(json{{"op", "delete"}, {"target", current.ops[1].id}}).id;  // B's import gone
  current.append(json{{"op", "delete"}, {"target", deleteB}});  // and back: a delete of a later op needs no tombstone
  current.append(json{{"op", "rename"}, {"target", nodeC}, {"name", "C2"}});
  current.append(json{{"op", "delete"}, {"target", importD}});
  CHECK(current.gc() == std::vector<std::string>{keyD});  // D's body no longer stored
  CHECK(!changes_design(Document::parse(versionText), Document::parse(versionText)));
  Document indexed = Document::parse_index(versionText);
  const std::string wanted = picture(resolve(indexed));
  CHECK(picture(resolve(current)) != wanted);
  const RestorePlan plan = plan_restore(current, indexed);
  CHECK(plan.problem == RestorePlan::Problem::none);
  CHECK_EQ(plan.later, 7u);
  CHECK_EQ(plan.tombstones.size(), 6u);  // all but the delete of the delete
  CHECK(std::find(plan.tombstones.begin(), plan.tombstones.end(), importC) != plan.tombstones.end());
  CHECK(std::find(plan.tombstones.begin(), plan.tombstones.end(), deleteB) != plan.tombstones.end());
  CHECK(plan.bodies == std::vector<std::string>{keyD});
  const size_t before = current.ops.size();
  apply_restore(current, indexed, plan, "bob");
  CHECK_EQ(current.ops.size(), before + 6);
  CHECK(current.ops.back().type == "delete" && current.ops.back().data.value("by", "") == "bob");
  CHECK(current.has_body(keyD) && current.body(keyD)->brep == brep(4));
  CHECK(resolve(current).node(nodeD) && resolve(current).param("w")->expr == "2 mm");
  CHECK_EQ(picture(resolve(current)), wanted);
  // Saved and read back: the same, the log only grew; restored again, nothing is left to do.
  const std::string text = current.serialize();
  CHECK_EQ(picture(resolve(Document::parse(text))), wanted);
  CHECK(text.compare(0, versionText.find("#bodies"), versionText, 0, versionText.find("#bodies")) == 0);
  CHECK(plan_restore(current, Document::parse(text)).problem == RestorePlan::Problem::current);
  // Not this way: another document, or a version with ops the current lacks (another branch).
  CHECK(plan_restore(current, Document::create()).problem == RestorePlan::Problem::other_document);
  Document branch = Document::parse(versionText);
  rename(branch, v.b, "B on a branch");
  const RestorePlan foreign = plan_restore(current, branch);
  CHECK(foreign.problem == RestorePlan::Problem::not_ancestor && foreign.op == branch.ops.back().id);
  CHECK_THROWS(apply_restore(current, branch, foreign));
}

CHECK_MAIN()
