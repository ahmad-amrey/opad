// Versions compared as a person reads them (UI-57): index-mode reading, the semantic diff of bodies, design, notes and
// histories, its text and summary line, and the textconv outline. Fake BREP text except where geometry is measured.
#include <BRepPrimAPI_MakeBox.hxx>

#include <algorithm>
#include <set>
#include <sstream>

#include "check.hpp"
#include "opad/design/sketch.hpp"
#include "opad/diff.hpp"
#include "opad/document.hpp"
#include "opad/geometry.hpp"
#include "opad/scene.hpp"
#include "opad/util.hpp"

using namespace opad;

namespace {
std::string brep(int i) { return "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations " + std::to_string(i) + "\n"; }
json body_node(const std::string& id, const std::string& name, const std::string& key) {
  return {{"type", "body"}, {"id", id}, {"name", name}, {"key", key}};
}
// An import of one body; returns the import op id.
std::string import_one(Document& d, const std::string& id, const std::string& name, const std::string& key) {
  return d.append(json{{"op", "import"}, {"source", name + ".step"}, {"nodes", json::array({body_node(id, name, key)})}}).id;
}
// The first change of that kind, change and name (name empty: any).
json find(const json& diff, const std::string& kind, const std::string& change, const std::string& name = {}) {
  for (const auto& c : diff["changes"])
    if (c["kind"] == kind && c["change"] == change && (name.empty() || c.value("name", "") == name)) return c;
  return json();
}
size_t count(const json& diff, const std::string& kind, const std::string& change) {
  size_t n = 0;
  for (const auto& c : diff["changes"]) n += c["kind"] == kind && c["change"] == change;
  return n;
}
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
std::set<std::string> lines_of(const std::string& text) {
  std::set<std::string> out;
  std::istringstream in(text);
  for (std::string l; std::getline(in, l);) out.insert(l);
  return out;
}

struct Tree {
  Document doc = Document::create();
  std::string asm_id = new_uuid(), a = new_uuid(), b = new_uuid(), c = new_uuid(), d = new_uuid(), import_c, import_d;
  Tree() {
    const std::string ka = doc.add_body(brep(1), json{{"name", "A"}}), kb = doc.add_body(brep(2), json{{"name", "B"}});
    doc.append(json{{"op", "import"}, {"source", "asm.step"}, {"nodes", json::array({json{{"type", "component"}, {"id", asm_id}, {"name", "Asm"},
                                                                                       {"children", json::array({body_node(a, "A", ka), body_node(b, "B", kb)})}}})}});
    import_c = import_one(doc, c, "C", doc.add_body(brep(3), json{{"name", "C"}}));
    import_d = import_one(doc, d, "D", doc.add_body(brep(4), json{{"name", "D"}}));
  }
};
}  // namespace

TEST(index_mode_reads_bodies_in_place) {
  Tree t;
  const std::string text = t.doc.serialize();
  const Document x = Document::parse_index(text);
  CHECK(x.indexed());
  CHECK_EQ(x.ops.size(), t.doc.ops.size());
  CHECK_EQ(x.body_keys(), t.doc.body_keys());
  for (const auto& b : x.bodies()) {
    CHECK(b.brep.empty());
    CHECK_EQ(std::string(b.text()), t.doc.body(b.key)->brep);
  }
  CHECK(!x.has_live_bodies());
  CHECK_EQ(x.serialize(), text);  // saves byte-identically
  // Nothing is hashed: an edited entry reads in index mode and fails the verifying read.
  std::string edited = text;
  edited.replace(edited.find("Locations 3"), 11, "Locations 9");
  CHECK_THROWS(Document::parse(edited));
  CHECK(has(std::string(Document::parse_index(edited).bodies()[2].text()), "Locations 9"));
  // CRLF (a checkout without eol=lf) and a missing last newline read as the LF text in both modes.
  std::string crlf;
  for (char ch : text) crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
  crlf.resize(crlf.size() - 2);
  CHECK_EQ(Document::parse_index(crlf).serialize(), text);
  CHECK_EQ(Document::parse(crlf).serialize(), text);
  // A body cut short is an error either way; skipped bodies are not listed.
  const std::string cut = text.substr(0, text.rfind("Locations 4"));
  CHECK_THROWS(Document::parse_index(cut));
  CHECK_THROWS(Document::parse(cut));
  const std::string first = t.doc.body_keys()[0];
  CHECK_EQ(Document::parse_index(text, {}, [&](const std::string& k) { return k == first; }).body_count(), 3u);
  // Entries moved into another document take their text along.
  Document other = Document::create();
  {
    Document source = Document::parse_index(text);
    other.arrange_bodies({first}, source, false);
  }
  CHECK_EQ(other.body(first)->brep, brep(1));
  CHECK(other.body(first)->indexed.empty());
}

TEST(bodies_and_components) {
  Tree t;
  const std::string base = t.doc.serialize();
  Document b = Document::parse(base);
  b.append(json{{"op", "rename"}, {"target", t.a}, {"name", "A2"}});
  b.append(json{{"op", "transform"}, {"target", t.b}, {"matrix", {0, -1, 0, 5, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}});
  b.append(json{{"op", "appearance"}, {"target", t.c}, {"color", {1, 0, 0}}, {"visible", false}});
  b.append(json{{"op", "reparent"}, {"target", t.c}, {"parent", t.asm_id}});
  const std::string kc = b.add_body(brep(9), json{{"name", "C"}});
  b.append(json{{"op", "edit"}, {"target", t.import_c}, {"set", {{"nodes", json::array({body_node(t.c, "C", kc)})}}}});
  b.append(json{{"op", "delete"}, {"target", t.import_d}});
  const std::string fresh = new_uuid(), k5 = b.add_body(brep(5), json::object()), k6 = b.add_body(brep(6), json::object());
  b.append(json{{"op", "import"}, {"source", "new.step"},
                {"nodes", json::array({json{{"type", "component"}, {"id", fresh}, {"name", "New"},
                                            {"children", json::array({body_node(new_uuid(), "N1", k5), body_node(new_uuid(), "N2", k6),
                                                                      json{{"type", "component"}, {"id", new_uuid()}, {"name", "Sub"},
                                                                           {"children", json::array({body_node(new_uuid(), "N3", k5)})}}})}}})}});
  const Document a = Document::parse_index(base), bb = Document::parse_index(b.serialize());
  const json d = semantic_diff(a, bb);
  CHECK_EQ(d["relation"], "descendant");
  CHECK_EQ(d["common_ops"].get<size_t>(), 3u);
  json c = find(d, "body", "renamed", "A2");
  CHECK_EQ(c["before"], "A");
  c = find(d, "body", "moved", "B");
  CHECK_EQ(c["translation"], json({5.0, 0.0, 0.0}));
  CHECK(std::abs(c["rotation_deg"].get<double>() - 90) < 1e-9);
  CHECK(std::abs(c["axis"][2].get<double>() - 1) < 1e-9);
  c = find(d, "body", "appearance", "C");
  CHECK_EQ(c["fields"]["visible"]["after"], false);
  CHECK_EQ(c["fields"]["color"]["after"], "#ff0000");
  CHECK_EQ(find(d, "body", "reparented", "C")["after"], "Asm");
  c = find(d, "body", "geometry", "C");
  CHECK_EQ(c["key_after"], kc);
  CHECK(!c.contains("metrics"));
  CHECK_EQ(find(d, "component", "added", "New")["bodies"].get<size_t>(), 3u);
  CHECK_EQ(count(d, "body", "added") + count(d, "component", "added"), 1u);  // what the new component holds is not listed again
  CHECK(!find(d, "body", "removed", "D").is_null());
  CHECK(find(d, "body", "moved", "A2").is_null() && find(d, "body", "geometry", "B").is_null());
  CHECK_EQ(d["counts"]["body"]["renamed"].get<int>(), 1);
  // The first diff's keys: moved means placement only now.
  CHECK_EQ(d["geometry"]["moved"].get<int>(), 1);
  CHECK_EQ(d["geometry"]["changed"].get<int>(), 1);
  CHECK_EQ(d["ops"]["added"].size(), 7u);
  CHECK_EQ(d["bodies"]["added"].size(), 3u);
  const std::string summary = d["summary"];
  CHECK(summary.rfind("Rename A to A2; move B, C into Asm; change C; hide C", 0) == 0);
  const std::string text = diff_text(d);
  CHECK(has(text, "b continues a: 3 ops in common, 7 new."));
  CHECK(has(text, "  ~ B: moved (5, 0, 0) mm, rotated 90 deg about (0, 0, 1)\n"));
  CHECK(has(text, "  + New (component, 3 bodies)\n"));
  CHECK(has(text, "  - D\n"));
  CHECK(has(text, "  ~ C: colour default -> #ff0000, hidden\n"));
}

TEST(bodies_as_a_viewer_overlays_them) {  // Compare (UI-58): world placement, both sides, hidden bodies
  Tree t;
  const std::string base = t.doc.serialize();
  Document b = Document::parse(base);
  b.append(json{{"op", "transform"}, {"target", t.asm_id}, {"matrix", {1, 0, 0, 0, 0, 1, 0, 7, 0, 0, 1, 0, 0, 0, 0, 1}}});  // A and B move with it
  const std::string kc = b.add_body(brep(9), json::object());
  b.append(json{{"op", "edit"}, {"target", t.import_c}, {"set", {{"nodes", json::array({body_node(t.c, "C", kc)})}}}});
  b.append(json{{"op", "delete"}, {"target", t.import_d}});
  const std::string e = new_uuid(), hidden = new_uuid();
  b.append(json{{"op", "import"}, {"source", "e.step"}, {"nodes", json::array({body_node(e, "E", b.add_body(brep(5), json::object())),
                                                                                 body_node(hidden, "H", b.add_body(brep(6), json::object()))})}});
  b.append(json{{"op", "appearance"}, {"target", hidden}, {"visible", false}});
  const Scene sa = resolve(t.doc), sb = resolve(b);
  const auto changes = body_changes(sa, sb);
  std::map<std::string, const BodyChange*> by;
  for (const auto& c : changes) by[c.id] = &c;
  CHECK_EQ(changes.size(), 5u);  // H: only on a side where it is hidden
  CHECK(!by.count(hidden));
  CHECK(by[t.a]->kind == BodyChange::Kind::Moved && by[t.b]->kind == BodyChange::Kind::Moved);  // the component's move
  CHECK_EQ(by[t.a]->world_b.at(1, 3), 7.0);
  CHECK_EQ(by[t.a]->world_a.at(1, 3), 0.0);
  CHECK(by[t.c]->kind == BodyChange::Kind::Modified && by[t.c]->key_b == kc && by[t.c]->key_a != kc);
  CHECK(by[t.d]->kind == BodyChange::Kind::Removed && by[t.d]->shown_a && !by[t.d]->shown_b);
  CHECK(by[e]->kind == BodyChange::Kind::Added && by[e]->key_a.empty());
  CHECK_EQ(std::string(body_change_name(changes.back().kind)), "removed");  // a's removed bodies come last
  // Hidden on one side: kept, drawn on the other.
  Document c = Document::parse(b.serialize());
  c.append(json{{"op", "appearance"}, {"target", e}, {"visible", false}});
  const auto again = body_changes(sb, resolve(c));
  const auto it = std::find_if(again.begin(), again.end(), [&](const BodyChange& x) { return x.id == e; });
  CHECK(it != again.end() && it->kind == BodyChange::Kind::Unchanged && it->shown_a && !it->shown_b);
  // The diff of scenes resolved once is the diff of the documents.
  CHECK_EQ(semantic_diff(t.doc, sa, b, sb)["changes"], semantic_diff(t.doc, b)["changes"]);
}

TEST(relations_between_histories) {
  Tree t;
  const std::string base = t.doc.serialize();
  Document x = Document::parse(base), y = Document::parse(base);
  x.append(json{{"op", "rename"}, {"target", t.a}, {"name", "X"}});
  y.append(json{{"op", "rename"}, {"target", t.b}, {"name", "Y"}});
  const Document a = Document::parse_index(base);
  const json same = semantic_diff(a, a);
  CHECK_EQ(same["relation"], "same");
  CHECK(same["changes"].empty());
  CHECK_EQ(same["summary"], "No changes");
  CHECK(has(diff_text(same), "No changes."));
  CHECK_EQ(semantic_diff(x, a)["relation"], "ancestor");
  const json diverged = semantic_diff(x, y);
  CHECK_EQ(diverged["relation"], "diverged");
  CHECK_EQ(diverged["common_ops"].get<size_t>(), 3u);
  CHECK(has(diff_text(diverged), "Diverged after 3 common ops (a has 1 more, b 1)."));
  const json other = semantic_diff(a, Document::create());
  CHECK_EQ(other["relation"], "unrelated");
  CHECK_EQ(other["same_document"], false);
}

TEST(recovery_snapshot_against_its_file) {  // UI-59: the file it was taken from, as it is now
  Tree t;
  const std::string base = t.doc.serialize();  // the file: 3 ops
  Document snap = Document::parse(base);
  snap.append(json{{"op", "rename"}, {"target", t.a}, {"name", "A2"}});
  snap.append(json{{"op", "appearance"}, {"target", t.c}, {"visible", false}});
  const Document prefix = ops_prefix(snap, 3);
  CHECK_EQ(prefix.ops.size(), 3u);
  CHECK_EQ(prefix.body_count(), 0u);
  CHECK_EQ(prefix.header.uuid, snap.header.uuid);
  CHECK_EQ(semantic_diff(prefix, snap)["summary"], "Rename A to A2; hide C");
  // Unchanged since: the snapshot's two changes, nothing incoming.
  const Document file = Document::parse_index(base);
  SnapshotPlan p = plan_snapshot(snap, 3, &file);
  CHECK(p.onto == SnapshotPlan::Onto::same && p.base == 3 && p.manifest->ops.size() == 3 && p.merge.error.empty());
  CHECK_EQ(p.merge.mine.size(), 2u);
  json d = snapshot_diff(snap, p, &file);
  CHECK_EQ(d["onto"], "same");
  CHECK_EQ(d["incoming"].get<int>(), 0);
  CHECK_EQ(d["unsaved"].get<int>(), 2);
  CHECK_EQ(d["summary"], "Rename A to A2; hide C");
  // Not known how far it was saved: what both have from the start.
  CHECK_EQ(plan_snapshot(snap, std::string::npos, &file).base, 3u);
  CHECK(plan_snapshot(snap, std::string::npos, &file).onto == SnapshotPlan::Onto::same);
  // Saved since with a change of its own: that one stays (not shown as undone), the snapshot's come after it.
  Document newer = Document::parse(base);
  newer.append(json{{"op", "rename"}, {"target", t.d}, {"name", "D2"}});
  const Document saved = Document::parse_index(newer.serialize());
  p = plan_snapshot(snap, 3, &saved);
  CHECK(p.onto == SnapshotPlan::Onto::extends && p.merge.error.empty() && p.merge.incoming == 1 && p.merge.conflicts.empty());
  d = snapshot_diff(snap, p, &saved);
  CHECK_EQ(d["onto"], "extends");
  CHECK_EQ(d["incoming"].get<int>(), 1);
  CHECK_EQ(d["relation"], "descendant");
  CHECK_EQ(d["summary"], "Rename A to A2; hide C");
  CHECK(find(d, "body", "renamed", "D2").is_null() && find(d, "body", "renamed", "D").is_null());
  // Both renamed A: a conflict, the snapshot's applied last.
  Document both = Document::parse(base);
  both.append(json{{"op", "rename"}, {"target", t.a}, {"name", "A3"}});
  p = plan_snapshot(snap, 3, &both);
  d = snapshot_diff(snap, p, &both);
  CHECK_EQ(d["conflicts"].size(), 1u);
  CHECK_EQ(d["conflicts"][0]["target"], t.a);
  CHECK_EQ(find(d, "body", "renamed")["after"], "A2");
  // Rewritten (a reset): the snapshot against it as it is, no merge.
  Document reset = Document::parse(base);
  reset.truncate_ops(2);
  p = plan_snapshot(snap, 3, &reset);
  CHECK(p.onto == SnapshotPlan::Onto::rewritten);
  d = snapshot_diff(snap, p, &reset);
  CHECK(!d.contains("incoming") && d["onto"] == "rewritten");
  CHECK(!find(d, "body", "added", "D").is_null());
  // Another document; no file at all (everything is new).
  const Document other = Document::create();
  CHECK(plan_snapshot(snap, 3, &other).onto == SnapshotPlan::Onto::other);
  CHECK_EQ(plan_snapshot(snap, std::string::npos, &other).base, 0u);
  p = plan_snapshot(snap, 3, nullptr);
  CHECK(p.onto == SnapshotPlan::Onto::missing);
  d = snapshot_diff(snap, p, nullptr);
  CHECK_EQ(d["onto"], "missing");
  CHECK_EQ(d["relation"], "descendant");
  CHECK(!find(d, "component", "added", "Asm").is_null() && !find(d, "body", "added", "D").is_null());
  // Onto the open session (Merge into current): it may have unsaved changes of its own; they stay first.
  Document session = Document::parse(base);
  session.append(json{{"op", "rename"}, {"target", t.b}, {"name", "B2"}});
  p = plan_snapshot(snap, 3, &session);
  CHECK(p.onto == SnapshotPlan::Onto::extends && p.merge.mine.size() == 2);
  std::vector<std::string> keys = session.body_keys();
  Document into = Document::parse(snap.serialize());
  apply_merge(into, session, p.merge, keys);
  CHECK_EQ(into.ops.size(), 6u);
  CHECK_EQ(resolve(into).node(t.b)->name, "B2");
  CHECK_EQ(resolve(into).node(t.a)->name, "A2");
}

TEST(parameters_sketches_and_features) {
  Document d = Document::create();
  const std::string param = d.append(json{{"op", "param"}, {"name", "width"}, {"expr", "30 mm"}}).id;
  const json geometry = {{"points", json::array({json{{"id", 1}, {"x", 0.0}, {"y", 0.0}}, json{{"id", 2}, {"x", 10.0}, {"y", 0.0}}})},
                         {"entities", json::array({json{{"id", 3}, {"type", "line"}, {"p", {1, 2}}}})},
                         {"constraints", json::array({json{{"id", 4}, {"type", "distance"}, {"refs", {3}}, {"value", 10.0}, {"expr", "width / 3"}, {"pos", {5.0, 2.0}}}})}};
  const std::string sketch = d.append(json{{"op", "sketch"}, {"name", "Sketch1"}, {"plane", {{"base", "xy"}}}, {"geometry", geometry}}).id;
  const std::string node = new_uuid(), k1 = d.add_body(brep(1), json::object());
  const std::string extrude = d.append(json{{"op", "feature"}, {"kind", "extrude"}, {"name", "Extrude1"},
                                            {"inputs", {{"profiles", json::array({json{{"sketch", sketch}, {"at", {5.0, 0.0}}}})}, {"distance", "width / 3"}}},
                                            {"result", {{"bodies", json::array({json{{"id", node}, {"key", k1}, {"name", "Extrude1"}}})}}}}).id;
  const std::string base = d.serialize();

  d.append(json{{"op", "edit"}, {"target", param}, {"set", {{"expr", "36 mm"}}}});
  json after = geometry;
  after["points"].push_back(json{{"id", 5}, {"x", 10.0}, {"y", 10.0}});
  after["entities"].push_back(json{{"id", 6}, {"type", "line"}, {"p", {2, 5}}});
  after["constraints"][0]["value"] = 12.0;
  d.append(json{{"op", "edit"}, {"target", sketch}, {"set", {{"geometry_delta", design::sketch_delta(geometry, after)}}}});
  const json profiles = json::array({json{{"sketch", sketch}, {"at", {5.0, 0.0}}, {"hint", {{"c", {1, 2, 3}}}}}});
  const json inputs = {{"profiles", profiles}, {"distance", "width / 2"}};
  d.append(json{{"op", "edit"}, {"target", extrude}, {"set", {{"inputs", inputs}}}});
  const std::string k2 = d.add_body(brep(2), json::object()), k3 = d.add_body(brep(3), json::object());
  d.append(json{{"op", "regen"}, {"results", {{extrude, {{"bodies", json::array({json{{"id", node}, {"key", k2}, {"name", "Extrude1"}}})}}}}}});
  d.append(json{{"op", "feature"}, {"kind", "fillet"}, {"name", "Fillet1"},
                {"inputs", {{"edges", json::array({json{{"body", node}, {"kind", "edge"}, {"index", 3}}})}, {"radius", "1 mm"}}},
                {"result", {{"bodies", json::array({json{{"id", node}, {"key", k3}}})}}}});
  const json diff = semantic_diff(Document::parse_index(base), Document::parse_index(d.serialize()));
  json c = find(diff, "param", "edited", "width");
  CHECK_EQ(c["before"], "30 mm");
  CHECK_EQ(c["after"], "36 mm");
  c = find(diff, "sketch", "edited", "Sketch1");
  CHECK_EQ(c["entities"]["added"].get<int>(), 1);
  CHECK_EQ(c["points"]["added"].get<int>(), 1);
  CHECK_EQ(c["constraints"]["changed"].get<int>(), 1);
  CHECK_EQ(c["dimensions"][0]["before"], "width / 3 = 10 mm");
  CHECK_EQ(c["dimensions"][0]["after"], "width / 3 = 12 mm");
  c = find(diff, "feature", "edited", "Extrude1");
  CHECK_EQ(c["details"].size(), 1u);  // the profile only got a hint: not a change
  CHECK_EQ(c["details"][0]["label"], "Distance");
  CHECK_EQ(c["details"][0]["before"], "width / 3 = 10 mm");  // each side's parameters
  CHECK_EQ(c["details"][0]["after"], "width / 2 = 18 mm");
  CHECK_EQ(c["bodies"], json::array({node}));
  CHECK_EQ(find(diff, "feature", "added", "Fillet1")["label"], "Fillet");
  CHECK_EQ(find(diff, "body", "geometry", "Extrude1")["key_after"], k3);
  CHECK_EQ(diff["summary"], "Set width = 36 mm; edit Sketch1, Extrude1 distance; add Fillet1");
  const std::string text = diff_text(diff);
  CHECK(has(text, "Parameters\n  ~ width: 30 mm -> 36 mm\n"));
  CHECK(has(text, "  ~ Extrude1: Distance width / 3 = 10 mm -> width / 2 = 18 mm\n"));
  CHECK(has(text, "dimension 4 (distance) width / 3 = 10 mm -> width / 3 = 12 mm"));
  CHECK(has(text, "  + Fillet1 (Fillet)\n"));

  // The outline git shows: what the document says, no BREP, the same text every time.
  const Document b = Document::parse_index(d.serialize());
  const std::string outline = document_outline(b);
  CHECK_EQ(outline, document_outline(b));
  CHECK(!has(outline, "CASCADE"));
  CHECK(has(outline, "\nwidth = 36 mm\n"));
  CHECK(has(outline, "Sketch1  on XY plane: 3 points, 2 lines, 1 constraints  hidden\n    distance 4 = width / 3 = 12 mm\n"));
  CHECK(has(outline, "Extrude1  (extrude)\n"));
  CHECK(has(outline, "    Distance = width / 2 = 18 mm\n"));
  CHECK(has(outline, "Extrude1  [body " + k3.substr(0, 12) + "]\n"));
  CHECK(has(outline, "  edit  feature Extrude1: inputs\n"));
  // One more op: the history line and what it changed, nothing else.
  Document renamed = Document::parse(d.serialize());
  renamed.append(json{{"op", "rename"}, {"target", node}, {"name", "Plate"}, {"ts", "2026-10-03T00:00:00Z"}, {"by", "carol"}});
  const auto x = lines_of(outline), y = lines_of(document_outline(Document::parse_index(renamed.serialize())));
  std::vector<std::string> gone, added;
  std::set_difference(x.begin(), x.end(), y.begin(), y.end(), std::back_inserter(gone));
  std::set_difference(y.begin(), y.end(), x.begin(), x.end(), std::back_inserter(added));
  const std::string s2 = k2.substr(0, 12), s3 = k3.substr(0, 12);
  CHECK_EQ(gone, std::vector<std::string>({"    -> Extrude1 " + s2, "    -> Extrude1 " + s3, "    Edges = Extrude1 edge 3", "Extrude1  [body " + s3 + "]"}));
  CHECK_EQ(added.size(), 5u);
  CHECK(std::find(added.begin(), added.end(), "Plate  [body " + s3 + "]") != added.end());
  CHECK(std::find(added.begin(), added.end(), "2026-10-03T00:00:00Z  carol  rename  to \"Plate\" (" + node.substr(0, 8) + ")") != added.end());
}

TEST(notes) {
  Tree t;
  const json anchor = {{"body", t.a}, {"kind", "face"}, {"index", 2}};
  const std::string n1 = t.doc.append(json{{"op", "annotation"}, {"anchor", anchor}, {"text", "deburr"}}).id;
  const std::string n3 = t.doc.append(json{{"op", "annotation"}, {"anchor", anchor}, {"text", "too thin"}}).id;
  const std::string base = t.doc.serialize();
  t.doc.append(json{{"op", "edit"}, {"target", n1}, {"set", {{"text", "deburr all edges"}}}});
  t.doc.append(json{{"op", "annotation"}, {"anchor", anchor}, {"reply_to", n1}, {"text", "done"}, {"by", "bob"}});
  t.doc.append(json{{"op", "annotation"}, {"anchor", t.b}, {"text", "hole undersized"}, {"style", "issue"}, {"by", "alice"}});
  t.doc.append(json{{"op", "delete"}, {"target", n3}});
  const json d = semantic_diff(Document::parse_index(base), t.doc);
  json c = find(d, "annotation", "edited");
  CHECK_EQ(c["before"], "deburr");
  CHECK_EQ(c["after"], "deburr all edges");
  c = find(d, "annotation", "commented");
  CHECK_EQ(c["comments"][0]["text"], "done");
  CHECK_EQ(c["comments"][0]["by"], "bob");
  c = find(d, "annotation", "added");
  CHECK_EQ(c["style"], "issue");
  CHECK_EQ(c["text"], "hole undersized");
  CHECK_EQ(find(d, "annotation", "resolved")["text"], "too thin");
  CHECK_EQ(count(d, "annotation", "added"), 1u);  // the reply is a comment, not a note
  // Back again: the resolved note reopens.
  CHECK_EQ(find(semantic_diff(t.doc, Document::parse_index(base)), "annotation", "reopened")["text"], "too thin");
  const std::string text = diff_text(d);
  CHECK(has(text, "Notes\n"));
  CHECK(has(text, "  + [issue] \"hole undersized\" by alice\n"));
  CHECK(has(text, "  - \"too thin\" resolved\n"));
  CHECK(has(text, "  ~ \"deburr all edges\": reply by bob: \"done\"\n"));
  const std::string outline = document_outline(t.doc);
  CHECK(has(outline, "[issue] \"hole undersized\" on B"));
  CHECK(has(outline, "    reply by bob: \"done\"\n"));
}

TEST(metrics_on_request) {
  auto box = [](double z) { return brep_from_shape(BRepPrimAPI_MakeBox(10, 20, z).Shape()); };
  Document d = Document::create();
  const std::string node = new_uuid();
  const std::string import = import_one(d, node, "Box", d.add_body(box(30), json::object()));
  const std::string base = d.serialize();
  const std::string k2 = d.add_body(box(40), json::object());
  d.append(json{{"op", "edit"}, {"target", import}, {"set", {{"nodes", json::array({body_node(node, "Box", k2)})}}}});
  DiffOptions opt;
  opt.metrics = true;
  const json diff = semantic_diff(Document::parse_index(base), Document::parse_index(d.serialize()), opt);  // parsed from the read text
  const json m = find(diff, "body", "geometry", "Box")["metrics"];
  CHECK(std::abs(m["before"]["volume"].get<double>() - 6000) < 1e-6);
  CHECK(std::abs(m["after"]["volume"].get<double>() - 8000) < 1e-6);
  CHECK(std::abs(m["after"]["area"].get<double>() - 2 * (200 + 400 + 800)) < 1e-6);
  CHECK(std::abs(m["after"]["size"][2].get<double>() - 40) < 1e-6);
  CHECK(has(diff_text(diff), "volume 6000 -> 8000 mm3"));
}

TEST(index_mode_checks_a_body_when_it_is_used) {
  auto box = [](double z) { return brep_from_shape(BRepPrimAPI_MakeBox(10, 20, z).Shape()); };
  Document d = Document::create();
  const std::string node = new_uuid(), k1 = d.add_body(box(30), json::object()), k0 = d.add_body(box(5), json::object());
  const std::string import = import_one(d, node, "Box", k1);
  import_one(d, new_uuid(), "Plate", k0);
  const std::string base = d.serialize();
  const std::string k2 = d.add_body(box(40), json::object());
  d.append(json{{"op", "edit"}, {"target", import}, {"set", {{"nodes", json::array({body_node(node, "Box", k2)})}}}});
  // The new body edited behind the store's back, same length and lines: a verifying read refuses the file, index mode
  // lists it unhashed and refuses it when it is first used, every time after.
  std::string text = d.serialize();
  text[text.find("Topology", text.find("#body " + k2)) + 1] = 'u';
  CHECK_THROWS(Document::parse(text));
  const Document v = Document::parse_index(text);
  CHECK_EQ(int(v.body(k2)->check.state), 0);
  CHECK_EQ(v.serialize(), text);  // saved as read
  CHECK(has(document_outline(v), "Box  [body " + k2.substr(0, 12) + "]"));
  for (int i = 0; i < 2; ++i) {
    try {
      body_shape(v, k2);
      CHECK(false);
    } catch (const Error& e) {
      CHECK(has(e.what(), "body entry " + k2.substr(0, 12) + ": content does not match its key"));
    }
  }
  CHECK_EQ(int(v.body(k2)->check.state), 2);
  CHECK(!body_shape(v, k0).IsNull());
  CHECK_EQ(int(v.body(k0)->check.state), 1);
  CHECK_EQ(int(v.body(k1)->check.state), 0);  // never used, never hashed
  // The diff names the change; measuring it says which side could not be read.
  const Document a = Document::parse_index(base);
  CHECK_EQ(find(semantic_diff(a, v), "body", "geometry", "Box")["key_after"], k2);
  DiffOptions opt;
  opt.metrics = true;
  const json diff = semantic_diff(a, v, opt);
  const json m = find(diff, "body", "geometry", "Box")["metrics"];
  CHECK(std::abs(m["before"]["volume"].get<double>() - 6000) < 1e-6);
  CHECK(has(m["after"].value("error", ""), "does not match its key"));
  CHECK(has(diff_text(diff), ", not measured after: body entry " + k2.substr(0, 12)));
  // Moving it into another store hashes it first and changes nothing when it does not match.
  Document other = Document::parse(base);
  Document source = Document::parse_index(text);
  CHECK_THROWS(other.arrange_bodies({k2, k1}, source, false));
  CHECK_EQ(other.body_keys(), (std::vector<std::string>{k1, k0}));
  CHECK_EQ(source.body_count(), 3u);
  other.arrange_bodies({k1}, source, false);  // already there: nothing moves, nothing is hashed
  CHECK_EQ(int(source.body(k2)->check.state), 2);
  CHECK_EQ(int(source.body(k1)->check.state), 0);
  CHECK_EQ(other.body_keys(), std::vector<std::string>{k1});
}

TEST(linked_assets) {  // the asset record the asset design writes on its import (an edit when it syncs)
  Document d = Document::create();
  const json asset = {{"v", 1}, {"kind", "step"}, {"path", "../hw/board.step"}, {"sha256", std::string(64, 'a')}, {"storage", "linked"}};
  const std::string import = d.append(json{{"op", "import"}, {"source", "board.step"}, {"asset", asset},
                                           {"nodes", json::array({body_node(new_uuid(), "Board", d.add_body(brep(1), json::object()))})}}).id;
  const std::string base = d.serialize();
  json synced = asset;
  synced["sha256"] = std::string(64, 'b');
  synced["storage"] = "embedded";
  d.append(json{{"op", "edit"}, {"target", import}, {"set", {{"asset", synced}}}});
  const json diff = semantic_diff(Document::parse_index(base), d);
  const json c = find(diff, "asset", "synced", "board.step");
  CHECK_EQ(c["before"], "aaaaaaa");
  CHECK_EQ(c["after"], "bbbbbbb");
  CHECK_EQ(find(diff, "asset", "storage")["after"], "embedded");
  CHECK_EQ(diff["summary"], "Sync board.step");
  CHECK(has(diff_text(diff), "Assets\n  ~ board.step: synced aaaaaaa -> bbbbbbb\n  ~ board.step: linked -> embedded\n"));
  CHECK(has(document_outline(d), "import  board.step (1 bodies) linked\n"));
}

TEST(unreadable_text_still_outlines) {
  Tree t;
  std::string text = t.doc.serialize();
  text.insert(text.find("#bodies"), "<<<<<<< ours\n=======\n>>>>>>> theirs\n");
  CHECK_THROWS(Document::parse_index(text));
  const std::string out = text_outline(text, "conflict");
  CHECK(out.rfind("unreadable OPAD document: conflict\n", 0) == 0);
  CHECK(has(out, "<<<<<<< ours\n"));
  CHECK(has(out, "\"source\":\"C.step\""));
  CHECK(has(out, "#body " + t.doc.body_keys()[0]));
  CHECK(!has(out, "CASCADE"));
}

TEST(versions_named_by_file) {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-diff-" + new_uuid());
  std::filesystem::create_directories(dir);
  const auto file = dir / "v.opad";
  Tree t;
  t.doc.save_as(file);
  std::filesystem::path named;
  CHECK_EQ(version_text(file.generic_string(), {}, &named), t.doc.serialize());
  CHECK(named == file);
  CHECK_THROWS(version_text("git:", file));       // no revision
  CHECK_THROWS(version_text("git:HEAD", {}));     // no document to name
  std::filesystem::remove_all(dir);
}

CHECK_MAIN()
