// Reconciling versions of one document (UI-56): manifests, how a version relates to a base, and the in-memory
// three-way merge (the git driver's rules, ours' unsaved ops after theirs). No OCCT geometry involved.
#include <set>

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

CHECK_MAIN()
