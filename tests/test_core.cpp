// Headless tests for the document model and .opad format (no OCCT geometry involved).
#include <set>

#include "check.hpp"
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

CHECK_MAIN()
