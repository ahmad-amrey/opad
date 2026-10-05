// Face provenance (TODO 11 UI-94): which feature made each face, from the body key chain the history keeps.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

#include "check.hpp"
#include "opad/agent.hpp"
#include "opad/core.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/provenance.hpp"
#include "opad/design/sketch.hpp"
#include "opad/geometry.hpp"
#include "opad/step_io.hpp"

using namespace opad;
using namespace opad::design;

namespace {

json feature_cmd(Document& doc, const std::string& kind, const json& inputs, const std::string& name = {}) {
  json args = {{"kind", kind}, {"inputs", inputs}};
  if (!name.empty()) args["name"] = name;
  return commands::run("feature", args, &doc);
}

json body_ref(const std::string& node) { return {{"body", node}, {"kind", "body"}}; }

json on_plane(double x, double y, double z) { return {{"origin", {x, y, z}}, {"normal", {0, 0, 1}}}; }

// How many faces each op owns.
std::map<std::string, int> tally(const std::vector<FaceOwner>& owners) {
  std::map<std::string, int> out;
  for (const auto& o : owners) ++out[o.op];
  return out;
}

void show(const char* what, const std::vector<FaceOwner>& owners, const std::map<std::string, std::string>& names) {
  std::printf("  %s:", what);
  for (const auto& [op, n] : tally(owners)) {
    auto it = names.find(op);
    std::printf(" %s=%d", it == names.end() ? op.substr(0, 8).c_str() : it->second.c_str(), n);
  }
  std::printf("\n");
}

// The ordinal of the edge of a body whose middle is nearest `at` (world coordinates).
int edge_near(const Document& doc, const std::string& body, const gp_Pnt& at) {
  const TopoDS_Shape s = node_world_shape(doc, resolve(doc), body);
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(s, TopAbs_EDGE, edges);
  int best = -1;
  double d = 1e300;
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    const double dist = c.Value((c.FirstParameter() + c.LastParameter()) / 2).Distance(at);
    if (dist < d) {
      d = dist;
      best = i - 1;
    }
  }
  return best;
}

// The ordinal of the face of a body whose centre is nearest `at`.
int face_near(const Document& doc, const std::string& body, const gp_Pnt& at) {
  const TopoDS_Shape s = node_world_shape(doc, resolve(doc), body);
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  int best = -1;
  double d = 1e300;
  for (int i = 1; i <= faces.Extent(); ++i) {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(faces(i), g);
    if (g.CentreOfMass().Distance(at) < d) {
      d = g.CentreOfMass().Distance(at);
      best = i - 1;
    }
  }
  return best;
}

int surfaces_of(const Document& doc, const std::string& body, const std::vector<FaceOwner>& owners, const std::string& op, GeomAbs_SurfaceType type) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(body_shape(doc, resolve(doc).node(body)->body_key), TopAbs_FACE, faces);
  int n = 0;
  for (int i = 1; i <= faces.Extent(); ++i)
    if (owners[size_t(i - 1)].op == op && BRepAdaptor_Surface(TopoDS::Face(faces(i))).GetType() == type) ++n;
  return n;
}

struct Part {
  Document doc = Document::create();
  std::string body, base, boss, round;
};

// The report's example: a 60x40x10 box, a 20x20x15 boss joined on its top face (placed at the face's corner, so part
// of it hangs over), then a 3 mm round on two of the boss's vertical edges.
Part boss_part() {
  Part p;
  const json base = feature_cmd(p.doc, "box", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}, "Base");
  p.base = base["feature_id"];
  p.body = base["body_ids"][0];
  p.boss = feature_cmd(p.doc, "box", {{"plane", {{"face", p.body + "/face/5"}}}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "15 mm"}, {"operation", "join"}, {"targets", {body_ref(p.body)}}},
                       "Boss")["feature_id"];
  const int a = edge_near(p.doc, p.body, gp_Pnt(-40, -30, 17)), b = edge_near(p.doc, p.body, gp_Pnt(-20, -30, 17));
  p.round = feature_cmd(p.doc, "fillet", {{"edges", {p.body + "/edge/" + std::to_string(a), p.body + "/edge/" + std::to_string(b)}}, {"radius", "3 mm"}}, "Round")["feature_id"];
  return p;
}

}  // namespace

TEST(box_boss_fillet) {
  Part p = boss_part();
  Provenance prov(p.doc);
  const auto owners = prov.face_owners(p.body);
  show("boss part", owners, {{p.base, "Base"}, {p.boss, "Boss"}, {p.round, "Round"}});
  auto t = tally(owners);
  CHECK_EQ(t[p.boss], 6);   // top, four walls, the underside where it hangs over
  CHECK_EQ(t[p.round], 2);  // the two rounds
  CHECK_EQ(t[p.base], 6);
  CHECK_EQ(owners.size(), size_t(14));
  CHECK_EQ(surfaces_of(p.doc, p.body, owners, p.round, GeomAbs_Cylinder), 2);
  for (const auto& o : owners) CHECK(!o.merged && o.via.empty());
  // Edges: a round's edges are the round's; where the boss stands on the base, the boss's (the later of the two).
  const auto edges = prov.edge_owners(p.body);
  CHECK_EQ(edges.size(), size_t(subshape_count(body_shape(p.doc, resolve(p.doc).node(p.body)->body_key), Ref::Kind::Edge)));
  CHECK_EQ(edges[size_t(edge_near(p.doc, p.body, gp_Pnt(-25, -10, 10)))].op, p.boss);  // boss wall y=-10 on the base top
  CHECK_EQ(edges[size_t(edge_near(p.doc, p.body, gp_Pnt(30, 0, 10)))].op, p.base);     // the base's own top edge
  const auto info = prov.op_info(p.boss);
  CHECK_EQ(info["name"], "Boss");
  CHECK_EQ(info["category"], "boss");
  CHECK_EQ(prov.op_info(p.round)["category"], "fillet");
  CHECK_EQ(prov.op_info(p.base)["category"], "body");
  // Again from the cache: the same answer.
  Provenance again(p.doc);
  CHECK(tally(again.face_owners(p.body)) == t);
}

TEST(two_coplanar_bosses_keep_their_own_faces) {
  Document doc = Document::create();
  const json base = feature_cmd(doc, "box", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}, "Base");
  const std::string body = base["body_ids"][0];
  const json joined = {{"operation", "join"}, {"targets", {body_ref(body)}}};
  json a = {{"plane", on_plane(-15, 0, 10)}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "5 mm"}};
  a.update(joined);
  json b = a;
  b["plane"] = on_plane(15, 0, 10);
  const std::string left = feature_cmd(doc, "box", a, "Left")["feature_id"], right = feature_cmd(doc, "box", b, "Right")["feature_id"];
  Provenance prov(doc);
  auto t = tally(prov.face_owners(body));
  show("two bosses", prov.face_owners(body), {{base["feature_id"], "Base"}, {left, "Left"}, {right, "Right"}});
  // Their tops are on one plane; each keeps its own (the samples of one are not inside the other).
  CHECK_EQ(t[left], 5);
  CHECK_EQ(t[right], 5);
  CHECK_EQ(t[base["feature_id"].get<std::string>()], 6);
}

TEST(a_boss_flush_with_a_wall_merges_into_it) {
  Document doc = Document::create();
  const json base = feature_cmd(doc, "box", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "10 mm"}}, "Base");
  const std::string body = base["body_ids"][0];
  // x from 10 to 30: its +x wall is the base's +x side carried up, and the Boolean makes them one face.
  const std::string boss = feature_cmd(doc, "box", {{"plane", on_plane(20, 0, 10)}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "15 mm"}, {"operation", "join"}, {"targets", {body_ref(body)}}},
                                       "Boss")["feature_id"];
  Provenance prov(doc);
  const auto owners = prov.face_owners(body);
  auto t = tally(owners);
  CHECK_EQ(t[boss], 4);  // top and three walls
  int merged = 0;
  for (const auto& o : owners)
    if (o.merged) {
      ++merged;
      CHECK_EQ(o.op, base["feature_id"].get<std::string>());  // the older owner, marked as holding the boss's wall too
    }
  CHECK_EQ(merged, 1);
}

TEST(a_chain_of_features) {
  Document doc = Document::create();
  const json base = feature_cmd(doc, "box", {{"length", "80 mm"}, {"width", "50 mm"}, {"height", "12 mm"}}, "Base");
  const std::string body = base["body_ids"][0];
  auto on = [&](json in) {
    in["operation"] = in.value("operation", "join");
    in["targets"] = {body_ref(body)};
    return in;
  };
  const std::string boss = feature_cmd(doc, "cylinder", on({{"plane", on_plane(-20, 0, 12)}, {"diameter", "16 mm"}, {"height", "10 mm"}}), "Boss")["feature_id"];
  const std::string pocket = feature_cmd(doc, "box", on({{"plane", on_plane(20, 0, 12)}, {"length", "20 mm"}, {"width", "14 mm"}, {"height", "-5 mm"}, {"operation", "cut"}}), "Pocket")["feature_id"];
  const std::string bore = feature_cmd(doc, "cylinder", on({{"plane", on_plane(-20, 0, 22)}, {"diameter", "6 mm"}, {"height", "-40 mm"}, {"operation", "cut"}}), "Bore")["feature_id"];
  // A drilled hole at a sketch point on the top face.
  Sketch pt;
  SkEntity e;
  e.type = SkEntity::Type::Point;
  e.p = {pt.add_point(30, 18)};
  e.id = pt.next_id();
  pt.entities.push_back(e);
  const std::string sk = commands::run("sketch", {{"plane", on_plane(0, 0, 12)}, {"geometry", pt.to_json()}}, &doc)["ids"][0];
  const std::string hole = feature_cmd(doc, "hole", {{"points", {{{"sketch", sk}, {"point", pt.points.front().id}}}}, {"diameter", "4 mm"}, {"extent", "all"}}, "Drill")["feature_id"];
  const std::string chamfer = feature_cmd(doc, "chamfer", {{"edges", {body + "/edge/" + std::to_string(edge_near(doc, body, gp_Pnt(0, -25, 12)))}}, {"distance", "1 mm"}}, "Bevel")["feature_id"];
  const std::string round = feature_cmd(doc, "fillet", {{"edges", {body + "/edge/" + std::to_string(edge_near(doc, body, gp_Pnt(-40, 25, 6)))}}, {"radius", "2 mm"}}, "Round")["feature_id"];
  const std::string press = feature_cmd(doc, "offset_face", {{"faces", {body + "/face/" + std::to_string(face_near(doc, body, gp_Pnt(40, 0, 6)))}}, {"distance", "2 mm"}}, "Push")["feature_id"];
  const auto start = std::chrono::steady_clock::now();
  Provenance prov(doc);
  const auto owners = prov.face_owners(body);
  std::printf("  chain of 8: %lld ms\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()));
  const std::map<std::string, std::string> names = {{base["feature_id"], "Base"}, {boss, "Boss"}, {pocket, "Pocket"}, {bore, "Bore"}, {hole, "Drill"}, {chamfer, "Bevel"}, {round, "Round"}, {press, "Push"}};
  show("chain", owners, names);
  auto t = tally(owners);
  CHECK_EQ(t[boss], 2);    // the boss's wall and top (the bore goes through its top)
  CHECK_EQ(t[pocket], 5);  // four walls and the floor
  CHECK_EQ(t[bore], 1);    // the bore's wall, through the boss and the base
  CHECK_EQ(t[hole], 1);
  CHECK_EQ(t[chamfer], 1);
  CHECK_EQ(t[round], 1);
  CHECK_EQ(surfaces_of(doc, body, owners, round, GeomAbs_Cylinder), 1);
  CHECK_EQ(surfaces_of(doc, body, owners, bore, GeomAbs_Cylinder), 1);
  CHECK(t[press] >= 1);  // the pushed face moved: it is the press pull's now
  for (const auto& [op, n] : t) CHECK(names.count(op));
  // Categories, as related reports them: a round cut is a hole.
  const json r = commands::run("related", {{"refs", {body}}}, &doc);
  std::map<std::string, std::string> category;
  for (const auto& c : r["candidates"])
    if (c["kind"] == "feature") category[c["op"]] = c["category"];
  CHECK_EQ(category[boss], "boss");
  CHECK_EQ(category[pocket], "pocket");
  CHECK_EQ(category[bore], "hole");
  CHECK_EQ(category[hole], "hole");
  CHECK_EQ(category[chamfer], "chamfer");
  CHECK_EQ(category[round], "fillet");
  CHECK_EQ(category[press], "press_pull");
}

TEST(a_shell_makes_its_inside_and_keeps_the_rim) {
  Document doc = Document::create();
  const json base = feature_cmd(doc, "box", {{"length", "40 mm"}, {"width", "30 mm"}, {"height", "20 mm"}}, "Tub");
  const std::string body = base["body_ids"][0];
  const std::string shell = feature_cmd(doc, "shell", {{"faces", {body + "/face/5"}}, {"thickness", "2 mm"}}, "Hollow")["feature_id"];
  Provenance prov(doc);
  auto t = tally(prov.face_owners(body));
  show("shell", prov.face_owners(body), {{base["feature_id"], "Tub"}, {shell, "Hollow"}});
  CHECK_EQ(t[shell], 5);                                   // the inner walls and floor
  CHECK_EQ(t[base["feature_id"].get<std::string>()], 6);  // the outside, and the rim that is left of the top
}

TEST(copies_keep_their_source_as_via) {
  Document doc = Document::create();
  const json plate = feature_cmd(doc, "box", {{"length", "100 mm"}, {"width", "40 mm"}, {"height", "5 mm"}}, "Plate");
  const std::string body = plate["body_ids"][0];
  // A peg as a body of its own, rounded, then patterned as new bodies.
  const json peg = feature_cmd(doc, "cylinder", {{"plane", on_plane(-40, 0, 5)}, {"diameter", "6 mm"}, {"height", "8 mm"}}, "Peg");
  const std::string pegBody = peg["body_ids"][0];
  const std::string round = feature_cmd(doc, "fillet", {{"edges", {pegBody + "/edge/" + std::to_string(edge_near(doc, pegBody, gp_Pnt(-40, 0, 13.5)))}}, {"radius", "1 mm"}}, "Tip")["feature_id"];
  const json row = feature_cmd(doc, "pattern_rect", {{"bodies", {body_ref(pegBody)}}, {"count", "3"}, {"spacing", "20 mm"}}, "Row");
  const std::string pattern = row["feature_id"];
  CHECK_EQ(row["body_ids"].size(), size_t(2));
  Provenance prov(doc);
  const auto seed = prov.face_owners(pegBody);
  for (const auto& copy : row["body_ids"]) {
    const auto owners = prov.face_owners(copy.get<std::string>());
    CHECK_EQ(owners.size(), seed.size());
    for (size_t i = 0; i < owners.size(); ++i) {
      CHECK_EQ(owners[i].op, pattern);
      CHECK_EQ(owners[i].via, seed[i].op);  // the copy of the peg's tip is the Tip's
    }
  }
  CHECK_EQ(tally(seed)[round], 1);
  // A mirror copy too.
  const json mirrored = feature_cmd(doc, "mirror", {{"bodies", {body_ref(pegBody)}}, {"plane", {{"base", "yz"}}}}, "Flip");
  Provenance after(doc);
  const auto flipped = after.face_owners(mirrored["body_ids"][0].get<std::string>());
  CHECK_EQ(flipped.size(), seed.size());
  for (size_t i = 0; i < flipped.size(); ++i) CHECK(flipped[i].op == mirrored["feature_id"].get<std::string>() && flipped[i].via == seed[i].op);
  // Joined into the plate, copies are the pattern's faces; related names the pattern and its source.
  const std::string joined = feature_cmd(doc, "pattern_rect", {{"bodies", {body_ref(pegBody)}}, {"count", "2"}, {"spacing", "50 mm"}, {"operation", "join"}, {"targets", {body_ref(body)}}}, "Pins")["feature_id"];
  Provenance last(doc);
  auto t = tally(last.face_owners(body));
  CHECK_EQ(t[joined], 3);  // the pin's wall, round and top: its foot is on the plate
  CHECK_EQ(t[plate["feature_id"].get<std::string>()], 6);
  const json r = commands::run("related", {{"refs", {row["body_ids"][0].get<std::string>() + "/face/0"}}}, &doc);
  CHECK_EQ(r["candidates"][0]["op"], pattern);
  CHECK_EQ(r["candidates"][0]["category"], "pattern");
  CHECK(!r["candidates"][0]["via"].empty());
}

TEST(moves_combines_and_splits) {
  Document doc = Document::create();
  const json a = feature_cmd(doc, "box", {{"length", "30 mm"}, {"width", "30 mm"}, {"height", "10 mm"}}, "Block");
  const std::string body = a["body_ids"][0];
  const std::string round = feature_cmd(doc, "fillet", {{"edges", {body + "/edge/" + std::to_string(edge_near(doc, body, gp_Pnt(15, 15, 5)))}}, {"radius", "3 mm"}}, "Round")["feature_id"];
  // Moved: the same faces, the same owners.
  const std::string move = feature_cmd(doc, "move", {{"bodies", {body_ref(body)}}, {"dx", "5 mm"}, {"rotate", true}, {"angle", "30 deg"}}, "Shift")["feature_id"];
  {
    Provenance prov(doc);
    auto t = tally(prov.face_owners(body));
    CHECK_EQ(t[round], 1);
    CHECK_EQ(t[a["feature_id"].get<std::string>()], 6);
    CHECK(!t.count(move));
  }
  // A cap of its own joined on top: its faces stay the cap's. A cutter cut away: its walls are the combine's, via the cutter.
  const json cap = feature_cmd(doc, "box", {{"plane", on_plane(5, 0, 10)}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "4 mm"}}, "Cap");
  const std::string join = feature_cmd(doc, "combine", {{"target", {body_ref(body)}}, {"tools", {body_ref(cap["body_ids"][0])}}, {"operation", "join"}}, "Fuse")["feature_id"];
  const json cutter = feature_cmd(doc, "box", {{"plane", on_plane(5, 0, -1)}, {"length", "4 mm"}, {"width", "4 mm"}, {"height", "30 mm"}}, "Cutter");
  const std::string cut = feature_cmd(doc, "combine", {{"target", {body_ref(body)}}, {"tools", {body_ref(cutter["body_ids"][0])}}, {"operation", "cut"}}, "Cut")["feature_id"];
  Provenance prov(doc);
  const auto owners = prov.face_owners(body);
  show("combined", owners, {{a["feature_id"], "Block"}, {round, "Round"}, {cap["feature_id"], "Cap"}, {join, "Fuse"}, {cutter["feature_id"], "Cutter"}, {cut, "Cut"}});
  auto t = tally(owners);
  CHECK_EQ(t[cap["feature_id"].get<std::string>()], 5);
  CHECK_EQ(t[cut], 4);
  CHECK(!t.count(join));
  for (const auto& o : owners)
    if (o.op == cut) CHECK_EQ(o.via, cutter["feature_id"].get<std::string>());
  // Split: the cut faces are the split's, the rest keep their owners on both pieces.
  const json split = feature_cmd(doc, "split", {{"bodies", {body_ref(body)}}, {"plane", {{"origin", {0, 0, 5}}, {"normal", {0, 0, 1}}}}}, "Halve");
  Provenance halves(doc);
  const std::string piece = split["body_ids"][1];
  auto top = tally(halves.face_owners(body)), bottom = tally(halves.face_owners(piece));
  CHECK_EQ(top[split["feature_id"].get<std::string>()] + bottom[split["feature_id"].get<std::string>()], 2);
  CHECK_EQ(top[round] + bottom[round], 2);
  CHECK_EQ(top[cut] + bottom[cut], 8);  // the cutter's walls, above and below the split
}

TEST(imported_bodies_have_no_history_but_their_features_do) {
  Document doc = Document::create();
  import_brep(doc, brep_from_shape(BRepPrimAPI_MakeBox(30, 20, 10).Shape()), "Block");
  const Scene s = resolve(doc);
  const std::string body = s.all_bodies().front();
  const std::string import = s.node(body)->source_op;
  Provenance prov(doc);
  const auto owners = prov.face_owners(body);
  CHECK_EQ(owners.size(), size_t(6));
  for (const auto& o : owners) CHECK_EQ(o.op, import);
  CHECK_EQ(prov.op_info(import)["category"], "imported");
  json r = commands::run("related", {{"refs", {body + "/face/2"}}}, &doc);
  CHECK_EQ(r["candidates"][0]["kind"], "import");
  CHECK_EQ(r["candidates"][0]["provenance"], false);
  CHECK_EQ(r["candidates"][0]["count"], 6);
  // A fillet on the imported block is the fillet's.
  const std::string round = feature_cmd(doc, "fillet", {{"edges", {body + "/edge/" + std::to_string(edge_near(doc, body, gp_Pnt(30, 20, 5)))}}, {"radius", "2 mm"}}, "Round")["feature_id"];
  Provenance after(doc);
  auto t = tally(after.face_owners(body));
  CHECK_EQ(t[round], 1);
  CHECK_EQ(t[import], 6);
}

TEST(related_lists_the_owning_feature_first) {
  Part p = boss_part();
  // Two faces of the boss: Boss holds both, with its six faces.
  Provenance prov(p.doc);
  const auto owners = prov.face_owners(p.body);
  std::vector<std::string> bossFaces;
  for (size_t i = 0; i < owners.size(); ++i)
    if (owners[i].op == p.boss) bossFaces.push_back(p.body + "/face/" + std::to_string(i));
  json r = commands::run("related", {{"refs", {bossFaces[0], bossFaces[3]}}}, &p.doc);
  const json& first = r["candidates"][0];
  CHECK_EQ(first["kind"], "feature");
  CHECK_EQ(first["op"], p.boss);
  CHECK_EQ(first["name"], "Boss");
  CHECK_EQ(first["category"], "boss");
  CHECK_EQ(first["feature_kind"], "box");
  CHECK_EQ(first["count"], 6);
  CHECK_EQ(first["refs"].size(), size_t(6));
  CHECK_EQ(first["contains_selection"], true);
  CHECK_EQ(r["refs"][0]["owner"]["name"], "Boss");
  CHECK_EQ(r["candidates"].back()["kind"], "body");
  // A boss face and a round: both owners listed, neither holds the whole selection.
  std::string roundFace;
  for (size_t i = 0; i < owners.size(); ++i)
    if (owners[i].op == p.round) roundFace = p.body + "/face/" + std::to_string(i);
  r = commands::run("related", {{"refs", {bossFaces[0], roundFace}}, {"kinds", {"feature"}}}, &p.doc);
  CHECK_EQ(r["candidates"].size(), size_t(2));
  CHECK_EQ(r["candidates"][0]["op"], p.round);  // the smaller first
  CHECK_EQ(r["candidates"][0]["contains_selection"], false);
  // An edge: the round's edge is the round's.
  const int edge = edge_near(p.doc, p.body, gp_Pnt(-40, -30, 17));
  r = commands::run("related", {{"refs", {p.body + "/edge/" + std::to_string(edge)}}}, &p.doc);
  CHECK_EQ(r["refs"][0]["owner"]["category"], "fillet");
  CHECK_EQ(r["candidates"][0]["op"], p.round);
  // Wrong input fails clearly.
  CHECK_THROWS(commands::run("related", {{"refs", {p.body + "/face/99"}}}, &p.doc));
  CHECK_THROWS(commands::run("related", {{"refs", {p.body + "/vertex/0"}}}, &p.doc));
  CHECK_THROWS(commands::run("related", json::object(), &p.doc));
  // The schema the MCP server checks takes refs; the agent's entity_details names the feature too.
  for (const auto& c : commands::list())
    if (c.name == "related") agent::validate_input(agent::command_schema(c), {{"doc", "x.opad"}, {"refs", {bossFaces[0]}}, {"kinds", {"feature", "body"}}});
  const json details = commands::run("entity_details", {{"ref", bossFaces[1]}}, &p.doc);
  CHECK_EQ(details["created_by"]["name"], "Boss");
  CHECK_EQ(details["created_by"]["category"], "boss");
}

TEST(an_edit_upstream_is_followed) {
  Part p = boss_part();
  // A taller boss: regenerated keys, the same owners.
  commands::run("feature_edit", {{"target", p.boss}, {"inputs", {{"height", "25 mm"}}}}, &p.doc);
  Provenance prov(p.doc);
  auto t = tally(prov.face_owners(p.body));
  CHECK_EQ(t[p.boss], 6);
  CHECK_EQ(t[p.round], 2);
  // Tombstoned, the boss owns nothing and its round fails (or goes); the base is whole again.
  commands::run("delete", {{"target", p.boss}}, &p.doc);
  commands::run("regenerate", json::object(), &p.doc);
  Provenance gone(p.doc);
  t = tally(gone.face_owners(p.body));
  CHECK(!t.count(p.boss));
  CHECK_EQ(t[p.base], 6);
}

TEST(a_long_history_stays_quick) {
  Document doc = Document::create();
  const json base = feature_cmd(doc, "box", {{"length", "200 mm"}, {"width", "200 mm"}, {"height", "10 mm"}}, "Base");
  const std::string body = base["body_ids"][0];
  std::vector<std::string> bosses;
  const int N = 6;
  for (int i = 0; i < N; ++i)
    for (int k = 0; k < N; ++k)
      bosses.push_back(feature_cmd(doc, "cylinder", {{"plane", on_plane(-81 + 18 * i, -81 + 18 * k, 10)}, {"diameter", "8 mm"}, {"height", std::to_string(5 + i + k) + " mm"},
                                                     {"operation", "join"}, {"targets", {body_ref(body)}}})["feature_id"]);
  auto start = std::chrono::steady_clock::now();
  Provenance prov(doc);
  const auto owners = prov.face_owners(body);
  const auto first = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  start = std::chrono::steady_clock::now();
  Provenance again(doc);
  (void)again.face_owners(body);
  const auto cached = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  std::printf("  %zu bosses (%zu faces): %lld ms, again %lld ms\n", bosses.size(), owners.size(), static_cast<long long>(first), static_cast<long long>(cached));
  auto t = tally(owners);
  for (const auto& b : bosses) CHECK_EQ(t[b], 2);
  CHECK(cached <= first);
}

CHECK_MAIN()
