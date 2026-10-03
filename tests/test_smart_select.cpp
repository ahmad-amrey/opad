// Smart selection's and Del's decisions (app/SmartRules.cpp, TODO 11 UI-95 / UI-04): the chip's candidate for picked
// faces, the routing of Del on objects, and the later features deleting a feature breaks.
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>

#include "SmartRules.hpp"
#include "check.hpp"
#include "opad/core.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/provenance.hpp"
#include "opad/geometry.hpp"

using namespace opad;

namespace {

json feature_cmd(Document& doc, const std::string& kind, const json& inputs, const std::string& name) {
  return commands::run("feature", {{"kind", kind}, {"inputs", inputs}, {"name", name}}, &doc);
}

struct Part {
  Document doc = Document::create();
  std::string body, base, boss, round;
};

// A 40x40x10 base, a 10x10x10 boss joined on its top, a 2 mm round on the boss's four top edges.
Part boss_part() {
  Part p;
  const json base = feature_cmd(p.doc, "box", {{"length", "40 mm"}, {"width", "40 mm"}, {"height", "10 mm"}, {"centered", false}}, "Base");
  p.base = base["feature_id"];
  p.body = base["body_ids"][0];
  p.boss = feature_cmd(p.doc, "box", {{"plane", {{"origin", {15, 15, 10}}, {"normal", {0, 0, 1}}}}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"},
                                      {"centered", false}, {"operation", "join"}}, "Boss")["feature_id"];
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(node_world_shape(p.doc, resolve(p.doc), p.body), TopAbs_EDGE, edges);
  json top = json::array();
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    if (std::abs(c.Value(c.FirstParameter()).Z() - 20) < 1e-6 && std::abs(c.Value(c.LastParameter()).Z() - 20) < 1e-6) top.push_back(p.body + "/edge/" + std::to_string(i - 1));
  }
  CHECK_EQ(top.size(), size_t(4));
  p.round = feature_cmd(p.doc, "fillet", {{"edges", top}, {"radius", "2 mm"}}, "Round")["feature_id"];
  return p;
}

std::vector<Ref> owned(Part& p, const std::string& op) {
  design::Provenance prov(p.doc);
  const auto owners = prov.face_owners(p.body);
  std::vector<Ref> out;
  for (size_t i = 0; i < owners.size(); ++i)
    if (owners[i].op == op) out.push_back(Ref::parse(p.body + "/face/" + std::to_string(i)));
  return out;
}

json refs_json(const std::vector<Ref>& refs) {
  json out = json::array();
  for (const auto& r : refs) out.push_back(r.str());
  return out;
}

}  // namespace

TEST(the_chip_offers_the_feature_that_made_the_picks) {
  Part p = boss_part();
  const auto boss = owned(p, p.boss), round = owned(p, p.round);
  CHECK_EQ(boss.size(), size_t(5));  // top and four walls; its underside merged into the base's top
  CHECK_EQ(round.size(), size_t(4));
  // Two walls of the boss: the boss with its five faces.
  const std::vector<Ref> two = {boss[1], boss[3]};
  auto c = smart::candidates(commands::run("related", {{"refs", refs_json(two)}}, &p.doc));
  int best = smart::headline(c, two);
  CHECK(best >= 0);
  CHECK_EQ(c[size_t(best)].kind, "feature");
  CHECK_EQ(c[size_t(best)].op, p.boss);
  CHECK_EQ(c[size_t(best)].name, "Boss");
  CHECK_EQ(c[size_t(best)].count, size_t(5));
  CHECK(smart::sameRefs(c[size_t(best)].refs, boss));
  CHECK_EQ(smart::matching(c, two), -1);
  CHECK(c.back().kind == "body" && c.back().refs.size() == 1 && c.back().refs[0].kind == Ref::Kind::Body);
  CHECK(c.back().bodies() == std::vector<std::string>{p.body});
  // All five: that is the boss (the chip turns into its actions); what holds them and adds more is the body's maker.
  c = smart::candidates(commands::run("related", {{"refs", refs_json(boss)}}, &p.doc));
  const int match = smart::matching(c, boss);
  CHECK(match >= 0 && c[size_t(match)].op == p.boss);
  best = smart::headline(c, boss);
  CHECK(best < 0 || (c[size_t(best)].count > boss.size() && c[size_t(best)].kind != "body" && c[size_t(best)].kind != "similar"));
  // One face of the round: the round chain, four faces, whatever the order of the candidates.
  c = smart::candidates(commands::run("related", {{"refs", {round[0].str()}}}, &p.doc));
  best = smart::headline(c, {round[0]});
  CHECK(best >= 0 && c[size_t(best)].count == 4 && smart::sameRefs(c[size_t(best)].refs, round));
  // A similar set is never offered, only asked for.
  smart::Candidate similar;
  similar.kind = "similar";
  similar.containsSelection = true;
  similar.refs = boss;
  similar.count = boss.size();
  CHECK_EQ(smart::headline({similar}, two), -1);
  // Nor the import that made an imported body (that is the body's faces), nor a group holding most of the body.
  smart::Candidate import = similar, group = similar;
  import.kind = "import";
  group.kind = "pocket";
  group.count = 9;
  CHECK_EQ(smart::headline({import}, two), -1);
  CHECK_EQ(smart::headline({group}, two, 18), 0);
  CHECK_EQ(smart::headline({group}, two, 17), -1);
  CHECK_EQ(smart::headline({group}, two), 0);  // the bodies' face count unknown
}

TEST(deleting_a_feature_names_what_it_breaks) {
  Part p = boss_part();
  const Scene scene = resolve(p.doc);
  design::Plan plan = design::plan_ops(p.doc, {{{"op", "delete"}, {"target", p.boss}}}, false);
  const auto deps = smart::dependents(plan.report, scene, {p.boss});
  CHECK_EQ(deps.size(), size_t(1));
  CHECK_EQ(deps[0].first, p.round);
  CHECK_EQ(deps[0].second, "Round");
  // Select dependents: Round, by its four faces; nothing depends on Round, the base has the boss and Round.
  smart::Users users = smart::usersOf(p.doc, p.boss);
  CHECK(users.ops.size() == 1 && users.ops[0].first == p.round);
  CHECK(smart::sameRefs(users.faces, owned(p, p.round)));
  users = smart::usersOf(p.doc, p.round);
  CHECK(users.ops.empty() && users.faces.empty());
  users = smart::usersOf(p.doc, p.base);
  CHECK_EQ(users.ops.size(), size_t(2));
  CHECK_EQ(users.faces.size(), owned(p, p.boss).size() + owned(p, p.round).size());
  // With the round deleted too nothing is left failing; deleting the round alone breaks nothing.
  plan = design::plan_ops(p.doc, {{{"op", "delete"}, {"target", p.boss}}, {{"op", "delete"}, {"target", p.round}}}, false);
  CHECK(smart::dependents(plan.report, scene, {p.boss, p.round}).empty());
  plan = design::plan_ops(p.doc, {{{"op", "delete"}, {"target", p.round}}}, false);
  CHECK(smart::dependents(plan.report, scene, {p.round}).empty());
  // Committed: the base is whole again.
  design::apply_ops(p.doc, {{{"op", "delete"}, {"target", p.boss}}, {{"op", "delete"}, {"target", p.round}}});
  CHECK_EQ(subshape_count(node_world_shape(p.doc, resolve(p.doc), p.body), Ref::Kind::Face), 6);
}

TEST(del_on_objects_takes_out_only_what_was_selected) {
  // A designed document: a body goes to one Remove feature, history kept.
  Part p = boss_part();
  Scene scene = resolve(p.doc);
  smart::Deletion d = smart::routeDelete(scene, {p.body});
  CHECK(d.tombstone.empty() && d.remove == std::vector<std::string>{p.body});
  const auto ops = smart::deletionOps(d, scene);
  CHECK_EQ(ops.size(), size_t(1));
  CHECK_EQ(ops[0]["kind"], "remove");
  CHECK_EQ(ops[0]["name"], "Remove1");
  const size_t before = p.doc.ops.size();
  design::apply_ops(p.doc, ops);
  scene = resolve(p.doc);
  CHECK(scene.all_bodies().empty());
  CHECK(scene.feature(p.boss) && scene.feature(p.round));  // nothing tombstoned
  CHECK_EQ(p.doc.ops.size(), before + 1);
  // A sketch is its op.
  const std::string sketch = commands::run("sketch", {{"plane", {{"base", "xy"}}}, {"name", "S"}}, &p.doc)["sketch_id"];
  d = smart::routeDelete(resolve(p.doc), {sketch});
  CHECK(d.tombstone == std::vector<std::string>{sketch} && d.remove.empty());

  // An import of two bodies, no history: one of them is removed, both tombstone the import.
  Document doc = Document::create();
  TopoDS_Compound two;
  BRep_Builder b;
  b.MakeCompound(two);
  b.Add(two, BRepPrimAPI_MakeBox(10, 10, 10).Shape());
  gp_Trsf away;
  away.SetTranslation(gp_Vec(20, 0, 0));
  b.Add(two, BRepBuilderAPI_Transform(BRepPrimAPI_MakeBox(10, 10, 10).Shape(), away).Shape());
  const std::string import = import_brep(doc, brep_from_shape(two), "Pair").op_id;
  scene = resolve(doc);
  const auto bodies = scene.all_bodies();
  CHECK_EQ(bodies.size(), size_t(2));
  d = smart::routeDelete(scene, {bodies[0]});
  CHECK(d.tombstone.empty() && d.remove == std::vector<std::string>{bodies[0]});
  d = smart::routeDelete(scene, {bodies[1], bodies[0]});
  CHECK(d.tombstone == std::vector<std::string>{import} && d.remove.empty());
  const std::string component = scene.node(bodies[0])->parent;
  CHECK(!component.empty());
  d = smart::routeDelete(scene, {component});  // the component: its bodies, the whole import
  CHECK(d.tombstone == std::vector<std::string>{import} && d.remove.empty());
  CHECK(smart::routeDelete(scene, {"no-such-node"}).empty());
  design::apply_ops(doc, smart::deletionOps(smart::routeDelete(scene, {bodies[0]}), scene));
  CHECK_EQ(resolve(doc).all_bodies(), std::vector<std::string>{bodies[1]});
}

CHECK_MAIN()
