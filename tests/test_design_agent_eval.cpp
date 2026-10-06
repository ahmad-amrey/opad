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

CHECK_MAIN()
