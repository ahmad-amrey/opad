// Forward compatibility of the .opad format (UI-65): a file written by a newer build, with op types this build does not
// know, opens, resolves without them, saves byte for byte and keeps the bodies they use.
#include "check.hpp"
#include "opad/document.hpp"
#include "opad/scene.hpp"
#include "opad/util.hpp"

using namespace opad;

static const std::string kBrep = "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations 0\n";
static const std::string kFrozen = "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations 0\nfrozen view\n";

// A document as a newer build would write it: one import, a sheet (one line), a sheet item spread over several lines
// with a body key in a nested token, and an edit of the sheet; both bodies in the store.
static std::string newer_file(std::string& key, std::string& frozen, std::string& sheet) {
  Document d = Document::create();
  key = d.add_body(kBrep, json{{"name", "Part"}});
  frozen = d.add_body(kFrozen, json{{"name", "View linework"}});
  d.append(json{{"op", "import"}, {"nodes", json::array({json{{"type", "body"}, {"id", new_uuid()}, {"name", "Part"}, {"key", key}}})}});
  std::string text = d.serialize();
  sheet = new_uuid();
  const std::string item = new_uuid(), edit = new_uuid();
  const std::string records =
      R"({"op":"sheet","id":")" + sheet + R"(","ts":"2026-10-03T10:00:00Z","by":"newer","name":"Sheet 1","size":"A3"})" "\n"
      R"({"op":"sheet_item","id":")" + item + R"(","ts":"2026-10-03T10:00:01Z","by":"newer","sheet":")" + sheet + R"(","kind":"issue",)" "\n"
      R"(  "frozen": {"V": [")" + frozen + R"(/edge/3"]},)" "\n"
      R"(  "points": [)" "\n"
      R"(    [0,0],)" "\n"
      R"(    [1,2]]})" "\n"
      R"({"op":"edit","id":")" + edit + R"(","ts":"2026-10-03T10:00:02Z","by":"newer","target":")" + sheet + R"(","set":{"size":"A2"}})" "\n";
  text.insert(text.find("#bodies\n"), records);
  return text;
}

TEST(unknown_op_types_load_save_identically_and_resolve) {
  std::string key, frozen, sheet;
  const std::string text = newer_file(key, frozen, sheet);
  Document d = Document::parse(text);
  CHECK_EQ(d.ops.size(), 4u);
  CHECK(!Document::known_type("sheet") && Document::known_type("import"));
  CHECK_EQ(d.ops[1].type, "sheet");
  CHECK_EQ(d.ops[2].type, "sheet_item");
  CHECK_EQ(d.serialize(), text);  // opaque records are written back as they were read, multi-line one included
  const Scene s = resolve(d);
  CHECK_EQ(s.all_bodies().size(), 1u);
  CHECK_EQ(s.unresolved.size(), 2u);  // the two records this build cannot apply, reported, not refused
  for (const auto& u : s.unresolved) CHECK(u.reason.find("needs a newer OPAD") != std::string::npos);
  CHECK(s.unresolved[0].op_id == sheet && s.unresolved[0].op_type == "sheet");
  // gc keeps the body only the newer record mentions (in an "<key>/edge/3" token).
  CHECK(d.gc().empty());
  CHECK(d.has_body(frozen));
  CHECK_EQ(d.serialize(), text);
}

TEST(unknown_op_types_are_not_editable_and_still_refused_on_append) {
  std::string key, frozen, sheet;
  Document d = Document::parse(newer_file(key, frozen, sheet));
  CHECK_THROWS(d.append(json{{"op", "sheet"}, {"name", "mine"}}));  // this build never writes a type it does not know
  CHECK_THROWS(d.append(json{{"op", "edit"}, {"target", sheet}, {"set", {{"size", "A4"}}}}));
  CHECK_EQ(d.ops.size(), 4u);
  // A tombstone is type-agnostic: the record goes from the effective log, and gc lets its body go with it.
  const std::string text = d.serialize();
  d.append(json{{"op", "delete"}, {"target", d.ops[2].id}});
  CHECK_EQ(resolve(d).unresolved.size(), 1u);
  CHECK_EQ(d.gc().size(), 1u);
  CHECK(!d.has_body(frozen) && d.has_body(key));
  CHECK_EQ(d.serialize().substr(0, text.find("#bodies\n")), text.substr(0, text.find("#bodies\n")));  // earlier lines intact
}

TEST(unknown_op_records_still_need_a_valid_envelope) {
  const std::string head = Document::create().serialize();
  auto with = [&](const std::string& line) { std::string t = head; t.insert(t.find("#bodies\n"), line + "\n"); return t; };
  CHECK_THROWS(Document::parse(with(R"({"op":"sheet","name":"no id"})")));
  CHECK_THROWS(Document::parse(with(R"({"op":"sheet","id":"not-a-uuid"})")));
  CHECK_THROWS(Document::parse(with(R"({"op":"sheet","id":")" + new_uuid() + R"(")")));  // truncated record
  CHECK_THROWS(Document::parse(with(R"({"op":"import","id":")" + new_uuid() + R"(","nodes":"x"})")));  // known types keep their checks
  CHECK_EQ(Document::parse(with(R"({"op":"sheet","id":")" + new_uuid() + R"("})")).ops.size(), 1u);
}

CHECK_MAIN()
