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

CHECK_MAIN()
