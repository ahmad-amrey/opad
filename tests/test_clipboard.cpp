// Bodies and components on the clipboard (core opad/clipboard.hpp, TODO 11 UI-129): a copy holds the copied nodes once (a
// body under a copied component comes with it), with names, colours, placements and the body entries; pasted into the
// same document they are the same parts again under their parent, moved by the offset, without a new entry; pasted into
// another document they come at the root where they were in the world, with their entries; a clip without the entries
// pastes into its own document only; anything else is refused. Fake BREP text: nothing here needs the kernel.
#include "check.hpp"
#include "opad/clipboard.hpp"

using namespace opad;

namespace {
const std::string kBrepA = "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations 0\n";
const std::string kBrepB = "DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations 0\n\n";

struct Assembly {
  Document doc = Document::create();
  std::string keyA, keyB, frame, housing, bolt, loose;
  Assembly() {
    keyA = doc.add_body(kBrepA, {{"name", "A"}});
    keyB = doc.add_body(kBrepB, {{"name", "B"}});
    frame = new_uuid(), housing = new_uuid(), bolt = new_uuid(), loose = new_uuid();
    const Mat4 up = Mat4::translation(0, 0, 100), right = Mat4::translation(10, 0, 0);
    doc.append({{"op", "import"}, {"source", "a.step"}, {"units", "mm"},
                {"nodes", {{{"type", "component"}, {"id", frame}, {"name", "Frame"}, {"transform", up.to_json()},
                            {"children", {{{"type", "body"}, {"id", housing}, {"name", "Housing"}, {"key", keyA}, {"color", {0.1, 0.2, 0.3}}},
                                          {{"type", "body"}, {"id", bolt}, {"name", "Bolt"}, {"key", keyB}, {"transform", right.to_json()}}}}},
                           {{"type", "body"}, {"id", loose}, {"name", "Loose"}, {"key", keyB}}}}});
  }
};

}  // namespace

TEST(a_copy_holds_each_node_once_with_its_entries) {
  Assembly a;
  const Scene scene = resolve(a.doc);
  const json clip = copy_nodes(a.doc, scene, {a.bolt, a.frame, "not-a-node", a.frame});
  CHECK_EQ(clip["format"], kNodesClipFormat);
  CHECK_EQ(clip["document"], a.doc.header.uuid);
  CHECK_EQ(clip["nodes"].size(), 1u);  // the bolt comes with its component, the frame once
  const json& frame = clip["nodes"][0];
  CHECK_EQ(frame["name"], "Frame");
  CHECK_EQ(frame["children"].size(), 2u);
  CHECK_EQ(frame["children"][0]["key"], a.keyA);
  CHECK_EQ(frame["children"][0]["color"][2], 0.3);
  CHECK_NEAR(Mat4::from_json(frame["world"]).apply({0, 0, 0})[2], 100, 1e-12);
  CHECK_EQ(clip["bodies"].size(), 2u);
  CHECK_EQ(copy_nodes(a.doc, scene, {a.frame}, 10)["bodies"].size(), 0u);  // too large to carry
  CHECK_THROWS(copy_nodes(a.doc, scene, {"nothing"}));
}

TEST(in_its_own_document_a_paste_is_the_same_parts_again_beside_them) {
  Assembly a;
  Scene scene = resolve(a.doc);
  const json clip = json::parse(copy_nodes(a.doc, scene, {a.bolt, a.loose}).dump());  // as the clipboard holds it
  const size_t entries = a.doc.body_count();
  const PastePlan plan = plan_paste(a.doc, scene, clip, {0, 50, 0});
  CHECK(plan.missing.empty());
  CHECK_EQ(plan.ops.size(), 2u);  // one under the frame, one at the root
  CHECK_EQ(plan.ops[0]["parent"], a.frame);
  CHECK(!plan.ops[1].contains("parent"));
  const json out = paste_nodes(a.doc, clip, {0, 50, 0}, "tester");
  CHECK_EQ(out["ids"].size(), 2u);
  CHECK_EQ(out["bodies_added"], 0);
  CHECK_EQ(a.doc.body_count(), entries);
  scene = resolve(a.doc);
  CHECK(scene.unresolved.empty());
  const Node* bolt = scene.node(out["ids"][0].get<std::string>());
  CHECK(bolt && bolt->name == "Bolt" && bolt->body_key == a.keyB && bolt->parent == a.frame && bolt->id != a.bolt);
  const Vec3 at = scene.world(bolt->id).apply({0, 0, 0});
  CHECK_NEAR(at[0], 10, 1e-9);
  CHECK_NEAR(at[1], 50, 1e-9);
  CHECK_NEAR(at[2], 100, 1e-9);
  CHECK_EQ(scene.instance_count.at(a.keyB), 4);  // the bolt and the loose body, twice each: linked instances
  const Node* loose = scene.node(out["ids"][1].get<std::string>());
  CHECK(loose && loose->parent.empty() && loose->name == "Loose");
  CHECK_EQ(a.doc.ops.back().data["source"], "clipboard");
}

TEST(a_paste_into_another_document_brings_the_entries_where_they_were) {
  Assembly a;
  const json clip = copy_nodes(a.doc, resolve(a.doc), {a.frame});
  Document other = Document::create();
  CHECK(plan_paste(other, resolve(other), clip).missing.size() == 2u);
  const json out = paste_nodes(other, clip);
  CHECK_EQ(out["bodies_added"], 2);
  CHECK(other.has_body(a.keyA) && other.has_body(a.keyB));
  const Scene scene = resolve(other);
  CHECK(scene.unresolved.empty());
  const Node* frame = scene.node(out["ids"][0].get<std::string>());
  CHECK(frame && frame->parent.empty() && frame->children.size() == 2);
  const Node* housing = scene.node(frame->children[0]);
  CHECK(housing && housing->has_color && housing->color[1] == 0.2 && housing->body_key == a.keyA);
  CHECK_NEAR(scene.world(frame->children[1]).apply({0, 0, 0})[0], 10, 1e-9);
  CHECK_NEAR(scene.world(frame->children[1]).apply({0, 0, 0})[2], 100, 1e-9);
  // Without its entries a clip pastes into its own document only.
  json bare = clip;
  bare["bodies"] = json::array();
  Document third = Document::create();
  CHECK_THROWS(plan_paste(third, resolve(third), bare));
  CHECK(plan_paste(a.doc, resolve(a.doc), bare).missing.empty());
  json renamed = bare;  // copied while the file was viewed (live keys), pasted after it was made editable (content keys)
  renamed["nodes"][0]["children"][0]["key"] = "live-1";
  CHECK(plan_paste(a.doc, resolve(a.doc), renamed).missing.empty());
  CHECK_EQ(plan_paste(a.doc, resolve(a.doc), renamed).ops[0]["nodes"][0]["children"][0]["key"], a.keyA);
  json damaged = clip;
  damaged["bodies"][0]["brep"] = "damaged";
  CHECK_THROWS(paste_nodes(third, damaged));
}

TEST(a_paste_whose_parent_is_gone_lands_at_the_root_where_it_was) {
  Assembly a;
  const json clip = copy_nodes(a.doc, resolve(a.doc), {a.bolt});
  a.doc.append({{"op", "delete"}, {"target", a.doc.ops.front().id}});  // the import tombstoned: a cut
  const json out = paste_nodes(a.doc, clip);
  const Scene scene = resolve(a.doc);
  const Node* bolt = scene.node(out["ids"][0].get<std::string>());
  CHECK(bolt && bolt->parent.empty());
  CHECK_NEAR(scene.world(bolt->id).apply({0, 0, 0})[0], 10, 1e-9);
  CHECK_NEAR(scene.world(bolt->id).apply({0, 0, 0})[2], 100, 1e-9);
}

TEST(only_a_clip_of_nodes_pastes) {
  Document doc = Document::create();
  const Scene scene = resolve(doc);
  CHECK_THROWS(plan_paste(doc, scene, json::object()));
  CHECK_THROWS(plan_paste(doc, scene, {{"format", "opad.sketch.clipboard"}, {"nodes", json::array()}}));
  CHECK_THROWS(plan_paste(doc, scene, {{"format", kNodesClipFormat}, {"version", 2}, {"nodes", json::array()}}));
}

CHECK_MAIN()
