// Headless tests for the document model and .opad format (no OCCT geometry involved).
#include <algorithm>
#include <set>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/document.hpp"
#include "opad/scene.hpp"
#include "opad/util.hpp"

using namespace opad;

static const std::string kFakeBrep = "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations 0\n";

static json body_node(const std::string& key, const std::string& name, const std::string& id = new_uuid()) {
  json n;
  n["type"] = "body";
  n["id"] = id;
  n["name"] = name;
  n["key"] = key;
  return n;
}

static json component(const std::string& name, json children, const std::string& id = new_uuid()) {
  json n;
  n["type"] = "component";
  n["id"] = id;
  n["name"] = name;
  n["children"] = std::move(children);
  return n;
}

TEST(sha256_known_vectors) {
  CHECK_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  std::string million(1000000, 'a');
  CHECK_EQ(sha256_hex(million), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// Third-party notices (TODO 11 UI-13): generated at build time from the link lines, compiled in.
TEST(third_party_notices_are_compiled_in) {
  const std::string text = third_party_notices();
  CHECK(text.rfind("OPAD " + version_string(), 0) == 0 && text.find("third-party notices") != std::string::npos);
  CHECK(text.find("MIT licence") != std::string::npos && text.find("Trademarks") != std::string::npos);
  if (text.find("packages.msys2.org") != std::string::npos)  // MSYS2 build: packages, versions and licence texts
    CHECK(text.find("* opencascade ") != std::string::npos && text.find("Licence texts") != std::string::npos);
}

TEST(uuid_format_and_uniqueness) {
  std::set<std::string> seen;
  for (int i = 0; i < 1000; ++i) {
    std::string u = new_uuid();
    CHECK(is_uuid(u));
    CHECK_EQ(u[14], '4');
    CHECK(seen.insert(u).second);
  }
  CHECK(!is_uuid("not-a-uuid"));
  CHECK(!is_uuid("123e4567-e89b-12d3-a456-42661417400"));
}

TEST(mat4_math_and_json) {
  Mat4 t = Mat4::translation(1, 2, 3);
  Mat4 t2 = Mat4::translation(10, 0, 0);
  Mat4 m = t * t2;
  Vec3 p = m.apply({0, 0, 0});
  CHECK_NEAR(p[0], 11, 1e-12);
  CHECK_NEAR(p[1], 2, 1e-12);
  CHECK_NEAR(p[2], 3, 1e-12);
  CHECK(!m.is_identity());
  CHECK(Mat4::identity().is_identity());
  Mat4 back = Mat4::from_json(m.to_json());
  CHECK(back.to_json() == m.to_json());
  json twelve = json::array();
  for (int i = 0; i < 12; ++i) twelve.push_back(m.m[i]);
  CHECK(Mat4::from_json(twelve).to_json() == m.to_json());
  CHECK_THROWS(Mat4::from_json(json::array({1, 2, 3})));
}

TEST(ref_parse_and_format) {
  std::string id = new_uuid();
  Ref r = Ref::parse(id);
  CHECK(r.kind == Ref::Kind::Body);
  CHECK_EQ(r.str(), id);
  Ref f = Ref::parse(id + "/face/3");
  CHECK(f.kind == Ref::Kind::Face);
  CHECK_EQ(f.index, 3);
  CHECK_EQ(f.str(), id + "/face/3");
  Ref p = Ref::parse("point/1,2.5,-3");
  CHECK(p.kind == Ref::Kind::Point);
  CHECK_NEAR(p.point[1], 2.5, 1e-12);
  CHECK(Ref::from_json(f.to_json()).str() == f.str());
  CHECK_THROWS(Ref::parse("nope/face/1"));
  CHECK_THROWS(Ref::parse(id + "/face"));
}

TEST(undo_truncate_and_restore_keep_persisted_text) {
  Document d = Document::create();
  std::string key = d.add_body(kFakeBrep, json{{"name", "Fake"}});
  std::string body_id = new_uuid();
  json imp;
  imp["op"] = "import";
  imp["source"] = "fake.step";
  imp["nodes"] = json::array({body_node(key, "Fake", body_id)});
  d.append(imp, "alice");
  const std::string saved = d.serialize();  // as if written to disk: raw lines are now pinned
  Document d2 = Document::parse(saved);
  json ren;
  ren["op"] = "rename";
  ren["target"] = body_id;
  ren["name"] = "Renamed";
  d2.append(ren);
  const std::string edited = d2.serialize();
  CHECK(edited != saved);
  std::vector<Op> popped = d2.truncate_ops(1);  // undo the rename
  CHECK_EQ(popped.size(), 1u);
  CHECK_EQ(d2.serialize(), saved);
  d2.restore_ops(popped);  // redo
  CHECK_EQ(d2.serialize(), edited);
  std::vector<Op> both = d2.truncate_ops(0);  // undo past the saved import as well
  CHECK_EQ(both.size(), 2u);
  CHECK_EQ(d2.ops.size(), 0u);
  d2.restore_ops(both);
  CHECK_EQ(d2.serialize(), edited);  // the persisted import line came back byte-identical
}

TEST(document_roundtrip_is_byte_stable) {
  Document d = Document::create();
  std::string key = d.add_body(kFakeBrep, json{{"name", "Fake"}});
  CHECK_EQ(key, sha256_hex(kFakeBrep));
  CHECK_EQ(d.add_body(kFakeBrep, json{{"name", "Again"}}), key);  // dedupe
  CHECK_EQ(d.body_count(), 1u);
  std::string body_id = new_uuid();
  json imp;
  imp["op"] = "import";
  imp["source"] = "fake.step";
  imp["nodes"] = json::array({body_node(key, "Fake", body_id)});
  const Op& o = d.append(imp, "alice");
  CHECK(is_uuid(o.id));
  CHECK_EQ(o.data["by"], "alice");
  CHECK(o.data.contains("ts"));
  std::string text1 = d.serialize();
  Document d2 = Document::parse(text1);
  CHECK_EQ(d2.header.uuid, d.header.uuid);
  CHECK_EQ(d2.ops.size(), 1u);
  CHECK_EQ(d2.body_count(), 1u);
  CHECK_EQ(d2.body(key)->brep, kFakeBrep);
  CHECK_EQ(d2.body(key)->meta["name"], "Fake");
  CHECK_EQ(d2.serialize(), text1);  // idempotent

  // Append-only (F7): a new op adds exactly one line before #bodies and changes nothing else.
  json ren;
  ren["op"] = "rename";
  ren["target"] = body_id;
  ren["name"] = "Renamed";
  d2.append(ren);
  std::string text2 = d2.serialize();
  size_t bodies1 = text1.find("#bodies\n"), bodies2 = text2.find("#bodies\n");
  CHECK_EQ(text1.substr(0, bodies1), text2.substr(0, text1.substr(0, bodies1).size()));
  CHECK_EQ(text1.substr(bodies1), text2.substr(bodies2));
  CHECK_EQ(std::count(text2.begin(), text2.end(), '\n'), std::count(text1.begin(), text1.end(), '\n') + 1);
}

// UI-40: a load reports how far through the file it is (read, then parsed, by bytes), only forwards, and stops when told.
TEST(document_load_reports_progress_by_bytes) {
  Document d = Document::create();
  json nodes = json::array();
  for (int i = 0; i < 400; ++i) {
    const std::string brep = kFakeBrep + "# body " + std::to_string(i) + "\n";
    nodes.push_back(body_node(d.add_body(brep, json{{"name", "Part"}}), "Part " + std::to_string(i)));
  }
  json imp;
  imp["op"] = "import";
  imp["source"] = "many.step";
  imp["nodes"] = nodes;
  d.append(imp);
  for (int i = 0; i < 300; ++i) d.append(json{{"op", "rename"}, {"target", nodes[i]["id"]}, {"name", "Renamed " + std::to_string(i)}});
  const auto path = std::filesystem::temp_directory_path() / ("opad-progress-" + new_uuid() + ".opad");
  d.save_as(path);
  std::vector<double> seen;
  Document loaded = Document::load(path, {}, [&](double f) { seen.push_back(f); return true; });
  CHECK_EQ(loaded.body_count(), 400u);
  CHECK_EQ(loaded.ops.size(), 301u);
  CHECK_EQ(loaded.serialize(), d.serialize());
  CHECK(seen.size() >= 20);
  CHECK(std::is_sorted(seen.begin(), seen.end()));
  CHECK(seen.front() > 0 && seen.front() <= 0.2 + 1e-9);  // the read: the first fifth
  CHECK(seen.back() > 0.95 && seen.back() <= 1.0);
  CHECK(std::any_of(seen.begin(), seen.end(), [](double f) { return f > 0.3 && f < 0.6; }));  // the parse in between
  size_t calls = 0;
  CHECK_THROWS(Document::load(path, {}, [&](double) { return ++calls < 5; }));  // cancelled
  std::filesystem::remove(path);
}

TEST(document_parse_rejects_corruption) {
  Document d = Document::create();
  std::string key = d.add_body(kFakeBrep, json::object());
  std::string text = d.serialize();
  std::string tampered = text;
  size_t pos = tampered.find("Locations 0");
  tampered.replace(pos, 11, "Locations 1");
  CHECK_THROWS(Document::parse(tampered));
  CHECK_THROWS(Document::parse("hello"));
  CHECK_THROWS(Document::parse("#opad 99\n{}\n#ops\n#bodies\n"));
  std::string conflict = text;
  conflict.insert(conflict.find("#bodies"), "<<<<<<< HEAD\n");
  CHECK_THROWS(Document::parse(conflict));
  // CRLF input is accepted and normalised.
  std::string crlf;
  for (char c : text) { if (c == '\n') crlf += "\r\n"; else crlf += c; }
  Document d3 = Document::parse(crlf);
  CHECK_EQ(d3.body(key)->brep, kFakeBrep);
}

TEST(op_validation) {
  Document d = Document::create();
  CHECK_THROWS(d.append(json{{"op", "explode"}}));
  CHECK_THROWS(d.append(json{{"op", "rename"}, {"target", "bad"}, {"name", "x"}}));
  CHECK_THROWS(d.append(json{{"op", "rename"}, {"target", new_uuid()}}));
  CHECK_THROWS(d.append(json{{"op", "transform"}, {"target", new_uuid()}, {"matrix", json::array({1, 2})}}));
  CHECK_THROWS(d.append(json{{"op", "appearance"}, {"target", new_uuid()}}));
  CHECK_THROWS(d.append(json{{"op", "delete"}, {"target", new_uuid()}}));  // unknown target op
  CHECK_THROWS(d.append(json{{"op", "import"}, {"nodes", json::array({json{{"type", "body"}, {"id", new_uuid()}, {"key", "short"}}})}}));
  const Op& o = d.append(json{{"op", "rename"}, {"target", new_uuid()}, {"name", "ok"}});
  CHECK_THROWS(d.append(json{{"op", "rename"}, {"id", o.id}, {"target", new_uuid()}, {"name", "dup"}}));
  CHECK_EQ(d.ops.size(), 1u);
}

TEST(resolve_hierarchy_and_edits) {
  Document d = Document::create();
  std::string key = d.add_body(kFakeBrep, json{{"name", "Fake"}, {"color", json::array({0.1, 0.2, 0.3})}});
  std::string asm_id = new_uuid(), sub_id = new_uuid(), b1 = new_uuid(), b2 = new_uuid(), b3 = new_uuid();
  json imp;
  imp["op"] = "import";
  imp["nodes"] = json::array({component("Asm", json::array({component("Sub", json::array({body_node(key, "B1", b1), body_node(key, "B2", b2)}), sub_id),
                                                             body_node(key, "B3", b3)}),
                                        asm_id)});
  d.append(imp);
  Scene s = resolve(d);
  CHECK_EQ(s.roots.size(), 1u);
  CHECK_EQ(s.nodes.size(), 5u);
  CHECK_EQ(s.instance_count[key], 3);
  CHECK_EQ(s.node(b1)->parent, sub_id);
  CHECK(s.node(b1)->has_color);
  CHECK_NEAR(s.node(b1)->color[2], 0.3, 1e-12);
  CHECK(s.unresolved.empty());

  d.append(json{{"op", "rename"}, {"target", b1}, {"name", "Bolt"}});
  d.append(json{{"op", "transform"}, {"target", sub_id}, {"matrix", Mat4::translation(5, 0, 0).to_json()}});
  d.append(json{{"op", "appearance"}, {"target", b2}, {"visible", false}, {"color", json::array({1, 0, 0})}});
  d.append(json{{"op", "reparent"}, {"target", b3}, {"parent", sub_id}, {"index", 0}});
  s = resolve(d);
  CHECK_EQ(s.node(b1)->name, "Bolt");
  CHECK_EQ(s.node(b3)->parent, sub_id);
  CHECK_EQ(s.node(sub_id)->children[0], b3);
  CHECK_EQ(s.node(asm_id)->children.size(), 1u);
  CHECK(!s.node(b2)->visible);
  CHECK(!s.effectively_visible(b2));
  CHECK(s.effectively_visible(b1));
  Vec3 p = s.world(b1).apply({0, 0, 0});
  CHECK_NEAR(p[0], 5, 1e-12);

  // Cycle: moving Asm under Sub must be rejected and flagged, not applied.
  d.append(json{{"op", "reparent"}, {"target", asm_id}, {"parent", sub_id}});
  s = resolve(d);
  CHECK_EQ(s.unresolved.size(), 1u);
  CHECK_EQ(s.node(asm_id)->parent, "");

  // Unknown target is kept and flagged (F8).
  d.append(json{{"op", "rename"}, {"target", new_uuid()}, {"name", "ghost"}});
  s = resolve(d);
  CHECK_EQ(s.unresolved.size(), 2u);

  // Annotation with an unknown anchor stays, flagged.
  d.append(json{{"op", "annotation"}, {"anchor", Ref::parse(new_uuid() + "/face/1").to_json()}, {"text", "hmm"}});
  d.append(json{{"op", "annotation"}, {"anchor", Ref::parse(b1 + "/face/1").to_json()}, {"text", "ok"}});
  s = resolve(d);
  CHECK_EQ(s.annotations.size(), 2u);
  CHECK(s.annotations[0].unresolved);
  CHECK(!s.annotations[1].unresolved);
  CHECK_EQ(s.tree_json().size(), 1u);

  // A note's tag: "note" unless given, checked on append, and changed later by an edit op.
  CHECK_EQ(s.annotations[1].style, "note");
  const std::string tagged = d.append(json{{"op", "annotation"}, {"anchor", Ref::parse(b1).to_json()}, {"text", "burr"}, {"style", "issue"}}).id;
  CHECK_THROWS(d.append(json{{"op", "annotation"}, {"anchor", Ref::parse(b1).to_json()}, {"text", "x"}, {"style", "loud"}}));
  CHECK_EQ(resolve(d).annotations[2].style, "issue");
  d.append(json{{"op", "edit"}, {"target", tagged}, {"set", {{"style", "ok"}}}});
  CHECK_EQ(resolve(d).annotations[2].style, "ok");
}

// Hide others (UI-02): the fewest nodes, each subtree without a kept body as high up as it goes; hidden or empty
// subtrees are left alone.
TEST(others_to_hide_is_the_fewest_nodes) {
  Document d = Document::create();
  std::string key = d.add_body(kFakeBrep, json{{"name", "Fake"}});
  std::string engine = new_uuid(), head = new_uuid(), block = new_uuid(), empty = new_uuid(), gone = new_uuid();
  std::string valve = new_uuid(), spring = new_uuid(), bolt = new_uuid(), crank = new_uuid(), pin = new_uuid(), loose = new_uuid();
  json imp;
  imp["op"] = "import";
  imp["nodes"] = json::array({component("Engine", json::array({component("Head", json::array({body_node(key, "Valve", valve), body_node(key, "Spring", spring),
                                                                                              body_node(key, "Bolt", bolt)}), head),
                                                                component("Block", json::array({body_node(key, "Crank", crank)}), block),
                                                                component("Empty", json::array(), empty),
                                                                component("Gone", json::array({body_node(key, "Pin", pin)}), gone)}), engine),
                              body_node(key, "Loose", loose)});
  d.append(imp);
  d.append(json{{"op", "appearance"}, {"target", gone}, {"visible", false}});
  Scene s = resolve(d);
  auto sorted = [](std::vector<std::string> v) { std::sort(v.begin(), v.end()); return v; };
  CHECK(sorted(s.others_to_hide({valve})) == sorted({spring, bolt, block, loose}));  // not Crank one by one, not Empty or Gone
  CHECK(sorted(s.others_to_hide({head})) == sorted({block, loose}));
  CHECK(sorted(s.others_to_hide({valve, crank})) == sorted({spring, bolt, loose}));
  CHECK(s.others_to_hide({engine}) == std::vector<std::string>{loose});
  CHECK(sorted(s.others_to_hide({})) == sorted({engine, loose}));
  d.append(json{{"op", "appearance"}, {"target", spring}, {"visible", false}});
  CHECK(sorted(resolve(d).others_to_hide({valve})) == sorted({bolt, block, loose}));  // already hidden: nothing to do
}

TEST(tombstones_and_gc) {
  Document d = Document::create();
  std::string key = d.add_body(kFakeBrep, json::object());
  std::string b1 = new_uuid();
  json imp;
  imp["op"] = "import";
  imp["nodes"] = json::array({body_node(key, "B1", b1)});
  const Op& io = d.append(imp);
  std::string import_id = io.id;
  const Op& ro = d.append(json{{"op", "rename"}, {"target", b1}, {"name", "X"}});
  std::string rename_id = ro.id;
  CHECK_EQ(resolve(d).node(b1)->name, "X");

  const Op& del = d.append(json{{"op", "delete"}, {"target", rename_id}});
  std::string del_id = del.id;
  Scene s = resolve(d);
  CHECK_EQ(s.node(b1)->name, "B1");
  CHECK_EQ(s.deleted_ops.size(), 1u);
  CHECK(d.is_deleted(rename_id));

  // Deleting the delete revives the rename.
  d.append(json{{"op", "delete"}, {"target", del_id}});
  s = resolve(d);
  CHECK_EQ(s.node(b1)->name, "X");

  // Tombstoning the import removes the body node; gc then drops the entry, and the op is still flagged.
  CHECK(d.gc().empty());
  d.append(json{{"op", "delete"}, {"target", import_id}});
  s = resolve(d);
  CHECK(s.nodes.empty());
  CHECK_EQ(d.gc().size(), 1u);
  CHECK_EQ(d.body_count(), 0u);
  CHECK_EQ(d.ops.size(), 5u);  // history untouched
}

// The document's Home (UI-47) is a view op with an optional "home": true; an older build reads it as a view named Home.
TEST(home_view_is_a_view_op_with_an_optional_key) {
  Document d = Document::create();
  const json camera{{"eye", {100, -100, 100}}, {"target", {0, 0, 0}}, {"up", {0, 0, 1}}, {"projection", "orthographic"}, {"scale", 80}, {"absolute", true}};
  commands::run("view", {{"name", "Front"}, {"camera", camera}}, &d);
  const std::string first = commands::run("view", {{"home", true}, {"camera", camera}}, &d)["id"];
  json later = camera;
  later["scale"] = 40;
  const std::string second = commands::run("view", {{"home", true}, {"camera", later}}, &d)["id"];
  const json op = d.find_op(first)->data;
  CHECK(op["name"] == "Home" && op["home"] == true && !d.find_op(d.ops.front().id)->data.contains("home"));
  Scene s = resolve(d);
  CHECK_EQ(s.views.size(), 3u);
  CHECK(!s.views[0].home && s.views[1].home && s.views[2].home && s.views[2].camera["scale"] == 40);
  const std::string text = d.serialize();
  CHECK_EQ(Document::parse(text).serialize(), text);
  CHECK(resolve(Document::parse(text)).views[1].home);
  // Reset: tombstones; the named view stays.
  d.append(json{{"op", "delete"}, {"target", first}});
  d.append(json{{"op", "delete"}, {"target", second}});
  s = resolve(d);
  CHECK(s.views.size() == 1u && !s.views[0].home && s.views[0].name == "Front");
  // A view op without the key (every file before it) is a plain bookmark; a non-boolean value is not a Home.
  json odd{{"op", "view"}, {"name", "Odd"}, {"camera", camera}, {"home", "yes"}};
  d.append(odd);
  CHECK(!resolve(d).views.back().home);
}

TEST(missing_body_entry_is_flagged_not_dropped) {
  Document d = Document::create();
  std::string b1 = new_uuid();
  json imp;
  imp["op"] = "import";
  imp["nodes"] = json::array({body_node(std::string(64, 'a'), "Ghost", b1)});
  d.append(imp);
  Scene s = resolve(d);
  CHECK_EQ(s.nodes.size(), 1u);
  CHECK(s.node(b1)->body_missing);
  CHECK_EQ(s.unresolved.size(), 1u);
  CHECK(s.tree_json()[0].value("missing", false));
}

TEST(corrupt_body_counts_are_rejected_before_allocation) {
  const std::string prefix = Document::create().serialize();
  for (const std::string count : {"18446744073709551615", "-1", "999999999", "2"}) {
    CHECK_THROWS(Document::parse(prefix + "#body " + std::string(64, 'a') + " " + count + " {}\nx\n"));
  }
}

TEST(annotation_edits_and_independent_comments_roundtrip) {
  Document d = Document::create();
  const auto id=d.append({{"op","annotation"},{"anchor","point/0,0,0"},{"text","Draft"}},"Alice").id;
  d.append({{"op","annotation"},{"anchor","point/0,0,0"},{"text","First reply"},{"reply_to",id}},"Bob");
  d.append({{"op","annotation"},{"anchor","point/0,0,0"},{"text","Second reply"},{"reply_to",id}},"Carol");
  d.append({{"op","edit"},{"target",id},{"set",{{"text","Updated"},{"style","warning"}}}},"Alice");
  auto scene=resolve(Document::parse(d.serialize()));
  CHECK_EQ(scene.annotations.size(),1u);
  CHECK_EQ(scene.annotations[0].text,"Updated");
  CHECK_EQ(scene.annotations[0].style,"warning");
  CHECK_EQ(scene.annotations[0].comments.size(),2u);
  CHECK_EQ(scene.annotations[0].comments[1]["by"],"Carol");
  d.append({{"op","delete"},{"target",id}});
  CHECK(resolve(d).annotations.empty());
}

TEST(pinned_measurement_comments_remove_restore_roundtrip) {
  Document d=Document::create();
  const auto id=d.append({{"op","measurement"},{"kind","distance"},{"refs",{"point/0,0,0","point/3,0,0"}},{"result",{{"value",3},{"unit","mm"}}}},"Alice").id;
  d.append({{"op","annotation"},{"anchor","point/0,0,0"},{"text","Check tolerance"},{"reply_to",id}},"Bob");
  d.append({{"op","edit"},{"target",id},{"set",{{"text","Clearance"}}}});
  auto scene=resolve(Document::parse(d.serialize()));
  CHECK_EQ(scene.measurements.size(),1u); CHECK(scene.annotations.empty());
  CHECK_EQ(scene.measurements[0].comments.size(),1u); CHECK_EQ(scene.measurements[0].comments[0]["by"],"Bob");
  CHECK_EQ(scene.measurements[0].text,"Clearance"); CHECK_EQ(scene.measurements[0].result["value"],3);
  const auto removed=d.append({{"op","delete"},{"target",id}}).id;
  CHECK(resolve(d).measurements.empty());
  d.append({{"op","delete"},{"target",removed}});
  scene=resolve(Document::parse(d.serialize()));
  CHECK_EQ(scene.measurements.size(),1u); CHECK_EQ(scene.measurements[0].comments.size(),1u);
}

// UI-12: the whole file at its size, byte for byte (CR and NUL kept), also under a name outside the ANSI code page; a
// missing file throws with its name in UTF-8.
TEST(read_text_file_reads_exactly) {
  const auto dir = std::filesystem::temp_directory_path() / ("opad-read-" + new_uuid());
  std::filesystem::create_directories(dir);
  std::string text;
  for (int i = 0; i < 300000; ++i) text += "line " + std::to_string(i) + (i % 7 ? "\n" : "\r\n");
  text += std::string("\0tail", 5);
  const auto file = dir / path_from_utf8("\xd9\x86\xd9\x85\xd9\x88\xd8\xb0\xd8\xac.opad");  // نموذج
  write_text_file(file, text);
  CHECK_EQ(read_text_file(file).size(), text.size());
  CHECK(read_text_file(file) == text);
  write_text_file(dir / "empty.txt", "");
  CHECK(read_text_file(dir / "empty.txt").empty());
  try {
    read_text_file(dir / path_from_utf8("\xd9\x84\xd8\xa7.opad"));  // لا
    CHECK(false);
  } catch (const Error& e) {
    CHECK(std::string(e.what()).find("\xd9\x84\xd8\xa7.opad") != std::string::npos);
  }
  CHECK_EQ(path_to_utf8(path_from_utf8("a/\xd9\x86.opad")), "a/\xd9\x86.opad");
  std::filesystem::remove_all(dir);
}

CHECK_MAIN()
