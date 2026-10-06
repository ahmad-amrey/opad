// Problems an AI agent met modelling through MCP (mcp-eval-smartknob, 2026-10-06), each fixed with a case here.
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <regex>
#include <string>

#include <Bnd_Box.hxx>

#include "check.hpp"
#include "opad/agent.hpp"
#include "opad/core.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/geometry.hpp"
#include "opad/live.hpp"
#include "opad/mass.hpp"

using namespace opad;

namespace {

json feature(Document& doc, const std::string& kind, const json& inputs, const std::string& name = {}) {
  json args = {{"kind", kind}, {"inputs", inputs}};
  if (!name.empty()) args["name"] = name;
  return commands::run("feature", args, &doc);
}

std::string error_of(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// A 112 x 86 x 14 mm slab and a triangular web standing on it (sketched in the side plane, extruded 10 mm symmetric and
// joined), as the eval's stand was built. Returns the joined body.
std::string slab_and_web(Document& doc) {
  const json slab = feature(doc, "box", {{"length", "112 mm"}, {"width", "86 mm"}, {"height", "14 mm"}});
  const std::string body = slab["body_ids"][0];
  json geometry = {{"points", json::array({{{"id", 1}, {"x", -30}, {"y", 14}}, {{"id", 2}, {"x", 10}, {"y", 14}}, {{"id", 3}, {"x", 10}, {"y", 60}}})},
                   {"entities", json::array({{{"id", 10}, {"type", "line"}, {"p", {1, 2}}}, {{"id", 11}, {"type", "line"}, {"p", {2, 3}}}, {{"id", 12}, {"type", "line"}, {"p", {3, 1}}}})}};
  const json sketch = commands::run("sketch", {{"plane", {{"base", "yz"}}}, {"geometry", geometry}}, &doc);
  feature(doc, "extrude", {{"profiles", json::array({{{"sketch", sketch["sketch_id"]}}})}, {"direction", "symmetric"}, {"distance", "10 mm"}, {"operation", "join"},
                           {"targets", json::array({body})}});
  return body;
}

}  // namespace

// 1. Fillet/chamfer failures reached agents as the kernel's text ("the modelling kernel failed: TopOpeBRepDS_DataStructure::Point")
// or as "that radius does not fit these edges". Now the error names the feature, the edges that do not take the size, and the
// largest size that does (which then works); a kernel exception of any feature is worded per kind.
TEST(fillet_failure_is_worded) {
  Document doc = Document::create();
  const std::string body = slab_and_web(doc);
  const Scene scene = resolve(doc);
  // The web's own edges and where it meets the slab top.
  const json found = agent::query_entities(doc, scene, {{"body", body}, {"kind", "edge"}, {"filters", {{"bounds", {{"min", {-5.5, -30.1, 13.9}}, {"max", {5.5, 10.1, 100}}}}}}, {"limit", 100}});
  json edges = json::array();
  for (const auto& item : found["items"]) edges.push_back(item["reference"]["ref"]);
  CHECK_EQ(edges.size(), 9u);
  const size_t ops = doc.ops.size();
  const std::string why = error_of([&] { feature(doc, "fillet", {{"edges", edges}, {"radius", "12 mm"}}, "Web edges"); });
  std::printf("%s\n", why.c_str());
  CHECK_EQ(doc.ops.size(), ops);
  CHECK(has(why, "Fillet \"Web edges\": R12 is too large"));
  CHECK(has(why, "of body " + body));
  CHECK(!has(why, "kernel failed") && !has(why, "TopOpeBRep") && !has(why, "Standard_"));
  std::smatch m;
  CHECK(std::regex_search(why, m, std::regex("try up to R([0-9.]+)")));
  const double largest = std::stod(m[1].str());
  CHECK(largest > 2 && largest < 12);
  // The radius it suggests works.
  Document copy = doc;
  feature(copy, "fillet", {{"edges", edges}, {"radius", std::to_string(largest) + " mm"}}, "Web edges");
  CHECK_EQ(copy.ops.size(), ops + 1);
  // A chamfer says it in its own words.
  const std::string chamfer = error_of([&] { feature(doc, "chamfer", {{"edges", edges}, {"distance", "12 mm"}}, "Web bevel"); });
  std::printf("%s\n", chamfer.c_str());
  CHECK(has(chamfer, "Chamfer \"Web bevel\": 12 mm is too large"));
  // Whatever kernel exception a feature meets: the feature, and what to change, never the kernel's text.
  const std::string thrown = design::kernel_failure_text("fillet", "Web edges");
  CHECK(has(thrown, "Fillet \"Web edges\"") && has(thrown, "smaller") && !has(thrown, "TopOpeBRep"));
  CHECK(has(design::kernel_failure_text("combine", ""), "Combine: "));
}

// 2. hole's schema offered points in space ({point:[x,y,z]}, [x,y,z], "point/x,y,z") and the feature refused them ("holes are
// placed at sketch points"). Now such a point (or a vertex) drills into the nearest body's face, or along direction.
TEST(hole_at_points_in_space) {
  Document doc = Document::create();
  const std::string block = feature(doc, "box", {{"length", "40 mm"}, {"width", "30 mm"}, {"height", "20 mm"}})["body_ids"][0];
  const double full = volume_properties(node_world_shape(doc, resolve(doc), block)).mass;
  // On the top face, no direction: straight down into it, 6 mm deep, at two points written two ways.
  const json made = feature(doc, "hole", {{"points", json::array({{{"point", {-10, 0, 20}}}, json::array({10, 0, 20})})}, {"diameter", "4 mm"}, {"depth", "6 mm"}});
  CHECK_EQ(made["body_ids"][0], block);
  Scene scene = resolve(doc);
  const double drilled = volume_properties(node_world_shape(doc, scene, block)).mass;
  CHECK_NEAR(full - drilled, 2 * M_PI * 4 * 6, 0.5);
  json walls = agent::query_entities(doc, scene, {{"body", block}, {"kind", "face"}, {"filters", {{"surface", "cylinder"}}}});
  CHECK_EQ(walls["total"], 2);
  for (const auto& w : walls["items"]) CHECK_NEAR(w["bbox"]["min"][2].get<double>(), 14, 1e-6);
  // A point beside the body, off its surface: into the nearest face (the +X side), here 5 mm deep.
  feature(doc, "hole", {{"points", json::array({"point/25,0,10"})}, {"diameter", "4 mm"}, {"depth", "10 mm"}});
  scene = resolve(doc);
  walls = agent::query_entities(doc, scene, {{"body", block}, {"kind", "face"}, {"filters", {{"surface", "cylinder"}, {"bounds", {{"min", {14, -3, 7}}, {"max", {20.1, 3, 13}}}}}}});
  CHECK_EQ(walls["total"], 1);
  // Along a given direction: through the block along -Y from its front.
  const double before = volume_properties(node_world_shape(doc, scene, block)).mass;
  feature(doc, "hole", {{"points", json::array({json::array({0, -15, 10})})}, {"direction", {{"direction", {0, 1, 0}}}}, {"diameter", "2 mm"}, {"extent", "all"}});
  scene = resolve(doc);
  CHECK_NEAR(before - volume_properties(node_world_shape(doc, scene, block)).mass, M_PI * 1 * 30, 0.5);
  // A vertex: into the face it lies on that faces it most (any of three here; a hole is made).
  const size_t ops = doc.ops.size();
  feature(doc, "hole", {{"points", json::array({block + "/vertex/0"})}, {"diameter", "2 mm"}, {"depth", "3 mm"}});
  CHECK_EQ(doc.ops.size(), ops + 1);
  // Nowhere to drill: said so, with what to give.
  Document empty = Document::create();
  const std::string why = error_of([&] { feature(empty, "hole", {{"points", json::array({json::array({0, 0, 0})})}}); });
  CHECK(has(why, "no body to drill") && has(why, "direction"));
  // The schema says what the feature does.
  const json schema = agent::feature_schema("hole");
  CHECK(schema["properties"].contains("direction"));
  agent::validate_input(schema, {{"points", json::array({json::array({0, 0, 0})})}, {"direction", {{"direction", {0, 0, -1}}}}});
}

// 3. A sketch's points, entities, constraints, images and patterns share one id space; only the refusal said so. Now the
// sketch tool's description, every id's schema and the brief geometry of model_batch steps say it.
TEST(sketch_id_space_is_described) {
  const auto described = [](const std::string& text) { return has(text, "one id space") && has(text, "point 1 and entity 1 collide"); };
  for (const auto& c : commands::list())
    if (c.name == "sketch") CHECK(described(c.description));
  json sketchTool, batch;
  for (const auto& tool : agent::live_tools()) {
    if (tool["name"] == "sketch") sketchTool = tool;
    if (tool["name"] == "model_batch") batch = tool;
  }
  CHECK(described(sketchTool["description"].get<std::string>()));
  const json geometry = agent::live_schema("sketch")["properties"]["geometry"];
  for (const char* list : {"points", "entities", "constraints"})
    CHECK(has(geometry["properties"][list]["items"]["properties"]["id"].value("description", ""), "one id space"));
  bool brief = false;  // the sketch step of a batch names the geometry briefly
  for (const auto& step : batch["inputSchema"]["properties"]["steps"]["items"]["anyOf"])
    if (step["properties"]["command"]["enum"][0] == "sketch") brief = described(step["properties"]["arguments"]["properties"]["geometry"].value("description", ""));
  CHECK(brief);
}

// 4. Headless reads of a document with linked files failed ("body entry not found" for a linked body, which by design is not
// stored). A linked file found where its relative path puts it is read; one that is not there is reported as not loaded and
// the rest is validated.
TEST(linked_files_in_headless_reads) {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("opad-eval-linked-" + new_uuid().substr(0, 8));
  fs::create_directories(root / "stand");
  fs::create_directories(root / "boards");
  Document board = Document::create();
  feature(board, "box", {{"length", "30 mm"}, {"width", "20 mm"}, {"height", "1.6 mm"}});
  commands::run("export", {{"format", "step"}, {"out", (root / "boards" / "board.step").string()}}, &board);
  const fs::path file = root / "stand" / "stand.opad";
  {
    Document doc = Document::create();
    doc.save_as(file);
    commands::run("import", {{"file", (root / "boards" / "board.step").string()}, {"link", true}}, &doc);
    feature(doc, "box", {{"length", "40 mm"}, {"width", "40 mm"}, {"height", "5 mm"}, {"plane", {{"origin", {0, 0, 10}}, {"normal", {0, 0, 1}}}}});
    doc.save();
  }
  const json args = {{"doc", file.string()}};
  // Beside the document, one folder up: read, so everything validates.
  json v = commands::run("validate", args);
  CHECK_EQ(v["total"], 2);
  CHECK(v["valid_page"].get<bool>());
  CHECK(!v.contains("not_loaded"));
  // The board file gone: reported, the rest validated, and the other reads work.
  fs::rename(root / "boards" / "board.step", root / "boards" / "moved.step");
  v = commands::run("validate", args);
  std::printf("%s\n", v.dump().substr(0, 600).c_str());
  CHECK_EQ(v["total"], 2);
  CHECK_EQ(v["not_loaded"]["bodies"], 1);
  CHECK_EQ(v["not_loaded"]["files"].size(), 1u);
  CHECK_EQ(v["not_loaded"]["files"][0]["state"], "missing");
  int validated = 0, unloaded = 0;
  for (const auto& item : v["items"]) {
    if (item.value("loaded", true)) validated += item["valid"].get<bool>();
    else ++unloaded;
  }
  CHECK_EQ(validated, 1);
  CHECK_EQ(unloaded, 1);
  CHECK(v["valid_page"].get<bool>());  // what could be checked is valid
  CHECK(commands::run("info", args)["bodies"].get<int>() == 2);
  CHECK(!commands::run("features", args).empty());
  CHECK(commands::run("context", {{"doc", file.string()}, {"section", "nodes"}})["total"].get<int>() >= 2);
  std::error_code ec;
  fs::remove_all(root, ec);
}

namespace {
// A component "Board" of two 10 x 10 x 2 boxes centred at x = 0 and x = 20, and an enclosure box beside them, `gap` mm past
// the second one's +X face (x = 25).
struct Assembly {
  Document doc = Document::create();
  std::string board, enclosure;
  explicit Assembly(double gap) {
    board = commands::run("component", {{"name", "Board"}}, &doc)["component_id"];
    for (const double x : {0.0, 20.0})
      commands::run("feature", {{"kind", "box"}, {"inputs", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "2 mm"}, {"x", x}}}, {"parent", board}}, &doc);
    enclosure = feature(doc, "box", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "2 mm"}, {"x", 30 + gap}})["body_ids"][0];
  }
};
}  // namespace

// 5. measure distance refused components ("use its bodies") while bbox took them, and 0 mm could not tell touching from
// overlapping. Components now stand for their bodies, and bodies 0 mm apart say touching or intersecting, with the volume.
TEST(measure_distance_components_and_contact) {
  {
    Assembly a(4);
    const json d = commands::run("measure", {{"kind", "distance"}, {"refs", {a.board, a.enclosure}}}, &a.doc);
    CHECK_NEAR(d["value"].get<double>(), 4, 1e-6);
    CHECK(!d.contains("contact"));
  }
  {
    Assembly a(0);  // face to face
    const json d = commands::run("measure", {{"kind", "distance"}, {"refs", {a.enclosure, a.board}}}, &a.doc);
    CHECK_NEAR(d["value"].get<double>(), 0, 1e-6);
    CHECK_EQ(d["contact"], "touching");
  }
  {
    Assembly a(-3);  // 3 mm into the second box: 3 x 10 x 2
    const json d = commands::run("measure", {{"kind", "distance"}, {"refs", {a.board, a.enclosure}}}, &a.doc);
    CHECK_EQ(d["contact"], "intersecting");
    CHECK_NEAR(d["overlap_volume_mm3"].get<double>(), 60, 1e-3);
    CHECK_EQ(d["overlaps"].size(), 1u);
  }
}

// 6. The live server's feature kinds have interference like the core (a stale tool list in the eval; kept here as a check),
// and a read-only clearance tool on both servers: body/component against body/component.
TEST(clearance_tool) {
  bool live = false, headless = false;
  for (const auto& tool : agent::live_tools())
    if (tool["name"] == "clearance") live = tool["annotations"]["readOnlyHint"].get<bool>();
  for (const auto& c : commands::list())
    if (c.name == "clearance") headless = !c.mutates;
  CHECK(live && headless);
  const json kinds = agent::live_schema("feature")["properties"]["kind"]["enum"];
  CHECK(std::find(kinds.begin(), kinds.end(), "interference") != kinds.end());
  agent::validate_input(agent::live_schema("clearance"), {{"a", "x"}, {"b", json::array({"y", "z"})}, {"clearance_mm", 1}, {"limit", 1}});
  CHECK_THROWS(agent::validate_input(agent::live_schema("clearance"), {{"a", "x"}}));

  Assembly a(-3);
  json r = commands::run("clearance", {{"a", a.board}, {"b", a.enclosure}, {"clearance_mm", 20}}, &a.doc);
  std::printf("%s\n", r.dump().substr(0, 700).c_str());
  CHECK_EQ(r["status"], "intersecting");
  CHECK_EQ(r["bodies_a"], 2);
  CHECK_EQ(r["intersecting"], 1);
  CHECK_EQ(r["too_close"], 1);  // the far board box, 17 mm off
  CHECK_NEAR(r["overlap_volume_mm3"].get<double>(), 60, 1e-3);
  CHECK_EQ(r["min_distance_mm"], 0.0);
  CHECK_EQ(r["items"][0]["kind"], "intersecting");
  CHECK_NEAR(r["items"][1]["distance_mm"].get<double>(), 17, 1e-6);
  // Paged.
  r = commands::run("clearance", {{"a", a.board}, {"b", a.enclosure}, {"clearance_mm", 20}, {"limit", 1}}, &a.doc);
  CHECK_EQ(r["items"].size(), 1u);
  CHECK_EQ(r["next_offset"], 1);
  // Apart: the gap, clear.
  Assembly apart(4);
  r = commands::run("clearance", {{"a", apart.enclosure}, {"b", apart.board}}, &apart.doc);
  CHECK_EQ(r["status"], "clear");
  CHECK_NEAR(r["min_distance_mm"].get<double>(), 4, 1e-6);
  CHECK_EQ(r["total"], 0);
  // The interference feature through the command layer, as the live server runs it.
  const json check = feature(a.doc, "interference", {{"bodies", json::array({a.board, a.enclosure})}});
  CHECK(check.contains("feature_id"));
}

CHECK_MAIN()
