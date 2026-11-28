// model_batch (opad/model_batch.hpp): each step is checked as a single command is, and a batch's defaults are not the
// step's own choices.
#include "opad/model_batch.hpp"
#include "opad/core.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "check.hpp"
#include <algorithm>
#include <set>
#include <string>

using namespace opad;

namespace {
// A 4 mm square swept along an S-shaped fit spline: the kernel builds it, but the solid fails BRepCheck (a live
// agent's batch met the same). A box intersected with its start then leaves a small valid body.
json path_step() {
  return {{"id", "path"}, {"command", "sketch"}, {"arguments", {{"plane", {{"base", "xy"}}}, {"geometry", {
    {"points", json::array({{{"id", 1}, {"x", 0}, {"y", 0}}, {{"id", 2}, {"x", 10}, {"y", 5}}, {{"id", 3}, {"x", 20}, {"y", -5}},
                            {{"id", 4}, {"x", 30}, {"y", 5}}, {{"id", 5}, {"x", 40}, {"y", -5}}})},
    {"entities", json::array({{{"id", 100}, {"type", "spline"}, {"p", {1, 2, 3, 4, 5}}, {"start_tangent", {1, 0}}}})}}}}}};
}
json profile_step() {
  json points = json::array(), lines = json::array();
  const double corners[4][2] = {{-2, -2}, {2, -2}, {2, 2}, {-2, 2}};
  for (int i = 0; i < 4; ++i) {
    points.push_back({{"id", i + 1}, {"x", corners[i][0]}, {"y", corners[i][1]}});
    lines.push_back({{"id", 100 + i}, {"type", "line"}, {"p", {i + 1, (i + 1) % 4 + 1}}});
  }
  return {{"id", "profile"}, {"command", "sketch"}, {"arguments", {{"plane", {{"base", "yz"}}}, {"geometry", {{"points", points}, {"entities", lines}}}}}};
}
json sweep_inputs(const std::string& profile, const std::string& path) {
  return {{"profiles", json::array({{{"sketch", profile}, {"all", true}}})}, {"path", {{"sketch", path}}}};
}
const json kIntersect = {{"x", "-1 mm"}, {"y", "-3 mm"}, {"length", "4 mm"}, {"width", "6 mm"}, {"height", "30 mm"}, {"operation", "intersect"}};
bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}  // namespace

// The premise, step by step: the sweep alone is invalid, and after the intersect every body is valid again, so a check of
// the batch's end state alone (what the live bridge did) passes.
TEST(invalid_sweep_is_hidden_by_a_later_step) {
  Document doc = Document::create();
  const Scene empty = resolve(doc);
  const std::string path = commands::run("sketch", path_step()["arguments"], &doc)["ids"][0];
  const std::string profile = commands::run("sketch", profile_step()["arguments"], &doc)["ids"][0];
  commands::run("feature", {{"kind", "sweep"}, {"inputs", sweep_inputs(profile, path)}}, &doc);
  CHECK_EQ(agent::invalid_changed_solids(doc, empty, resolve(doc)).size(), size_t(1));
  commands::run("feature", {{"kind", "box"}, {"inputs", kIntersect}}, &doc);
  const Scene end = resolve(doc);
  CHECK_EQ(end.all_bodies().size(), size_t(1));
  CHECK(agent::invalid_changed_solids(doc, empty, end).empty());
}

// Finding 1: the batch fails at the step that made the invalid solid, with a receipt naming it.
TEST(batch_fails_at_the_step_that_made_an_invalid_solid) {
  Document doc = Document::create();
  const json steps = json::array({path_step(), profile_step(),
    {{"id", "arm"}, {"command", "feature"}, {"arguments", {{"kind", "sweep"}, {"inputs", sweep_inputs("@{profile#/sketch_id}", "@{path#/sketch_id}")}}}},
    {{"id", "trim"}, {"command", "feature"}, {"arguments", {{"kind", "box"}, {"inputs", kIntersect}}}}});
  json output;
  std::string error;
  try {
    agent::model_batch(doc, {{"steps", steps}}, output);
  } catch (const Error& e) {
    error = e.what();
  }
  CHECK(contains(error, "'arm'"));
  CHECK(contains(error, "solid validation"));
  CHECK_EQ(output.value("failed_step", ""), std::string("arm"));
  CHECK_EQ(output["steps"].size(), size_t(3));  // the trim step never ran
  CHECK_EQ(output["steps"][1]["state"], json("computed"));
  const json& failed = output["steps"][2];
  CHECK_EQ(failed["id"], json("arm"));
  CHECK_EQ(failed["state"], json("failed"));
  CHECK(contains(failed.value("error", ""), "solid validation"));
  CHECK_EQ(failed["invalid_body_ids"].size(), size_t(1));

  // A batch of valid steps commits, and reports the keys it checked so the caller need not check them again.
  Document good = Document::create();
  std::set<std::string> valid;
  json done;
  agent::model_batch(good, {{"steps", json::array({{{"id", "block"}, {"command", "feature"}, {"arguments", {{"kind", "box"}, {"inputs", {{"length", 10}, {"width", 8}, {"height", 3}}}}}}})}}, done, {nullptr, nullptr, {}, &valid});
  CHECK(!done.contains("failed_step"));
  CHECK_EQ(done["steps"][0]["state"], json("computed"));
  CHECK_EQ(valid.size(), size_t(1));
}

// Finding 2: a batch-level parent is a default; the "not used" warning is a step's only when the step set those itself.
TEST(batch_parent_warns_only_steps_that_set_their_own_style) {
  Document doc = Document::create();
  const json box = {{"length", 10}, {"width", 10}, {"height", 10}};
  const json join = {{"x", "5 mm"}, {"length", 10}, {"width", 10}, {"height", 10}, {"operation", "join"}};
  json joinLeft = join;
  joinLeft["x"] = "-5 mm";
  const json steps = json::array({
    {{"id", "group"}, {"command", "component"}, {"arguments", {{"name", "Part"}}}},
    {{"id", "base"}, {"command", "feature"}, {"arguments", {{"kind", "box"}, {"inputs", box}}}},
    {{"id", "grow"}, {"command", "feature"}, {"arguments", {{"kind", "box"}, {"inputs", join}}}},
    {{"id", "tinted"}, {"command", "feature"}, {"arguments", {{"kind", "box"}, {"inputs", joinLeft}, {"color", {1, 0, 0}}}}}});
  json output;
  agent::model_batch(doc, {{"steps", steps}, {"parent", "@{group#/component_id}"}}, output);
  const Scene scene = resolve(doc);
  const std::string group = output["steps"][0]["result"]["component_id"], base = output["steps"][1]["result"]["body_ids"][0];
  const auto under = scene.bodies_under(group);
  CHECK(std::find(under.begin(), under.end(), base) != under.end());  // the default still places the bodies made
  auto warned = [&](size_t i) {
    for (const auto& w : output["steps"][i]["result"].value("warnings", json::array()))
      if (contains(w.get<std::string>(), "body_name, color and parent")) return true;
    return false;
  };
  CHECK(!warned(1));
  CHECK(!warned(2));  // a join made no body: the batch's parent was not the step's choice, so no warning
  CHECK(warned(3));   // this step chose a colour itself, and it was not used
}

// A sketch edit that leaves a downstream feature's body invalid is reported with that feature and the kernel's reason
// (the live bridge's refusal used to say only "Generated body failed solid validation").
TEST(an_invalid_body_names_the_feature_that_made_it_and_why) {
  Document doc = Document::create();
  json straight = path_step()["arguments"];
  for (auto& p : straight["geometry"]["points"]) p["y"] = 0;
  const std::string path = commands::run("sketch", straight, &doc)["ids"][0];
  const std::string profile = commands::run("sketch", profile_step()["arguments"], &doc)["ids"][0];
  const json made = commands::run("feature", {{"kind", "sweep"}, {"name", "Arm"}, {"inputs", sweep_inputs(profile, path)}}, &doc);
  const Scene before = resolve(doc);
  const std::string body = made["body_ids"][0], feature = made["feature_id"];
  CHECK(agent::invalid_changed_solids(doc, Scene{}, before).empty());
  CHECK(agent::invalid_solid_reason(node_world_shape(doc, before, body)).empty());
  commands::run("sketch_edit", {{"target", path}, {"geometry", path_step()["arguments"]["geometry"]}}, &doc);
  const Scene after = resolve(doc);
  const auto invalid = agent::invalid_changed_solids(doc, before, after);
  CHECK_EQ(invalid.size(), size_t(1));
  const Feature* maker = agent::feature_that_made(after, body);
  CHECK(maker && maker->id == feature);
  const std::string said = agent::describe_invalid_solid(doc, after, body);
  CHECK(contains(said, "made by sweep 'Arm' (" + feature + ")"));
  CHECK(contains(said, " on "));  // a BRepCheck status with the sub-shapes it is on
  Feature broken = *maker;
  broken.error = "the cut does not touch any body";
  CHECK_EQ(agent::describe_feature_error(broken), "sweep 'Arm' (" + feature + ") failed: the cut does not touch any body");
}

// Finding: a cut moved away and back in one batch failed at the first step; validate "final" checks only the end state.
TEST(validate_final_lets_steps_pass_through_a_broken_state) {
  Document base = Document::create();
  commands::run("feature", {{"kind", "box"}, {"name", "Block"}, {"inputs", {{"length", 20}, {"width", 20}, {"height", 10}}}}, &base);
  const std::string cut = commands::run("feature", {{"kind", "box"}, {"name", "Pocket"},
                                                    {"inputs", {{"length", 4}, {"width", 4}, {"height", 20}, {"operation", "cut"}}}}, &base)["feature_id"];
  auto move = [&](const std::string& id, const char* x) {
    return json{{"id", id}, {"command", "feature_edit"}, {"arguments", {{"target", cut}, {"inputs", {{"x", x}}}}}};
  };
  const json there_and_back = json::array({move("away", "100 mm"), move("back", "3 mm")});
  {  // each step checked: the first one fails
    Document doc = base;
    json output;
    std::string error;
    try {
      agent::model_batch(doc, {{"steps", there_and_back}}, output);
    } catch (const Error& e) {
      error = e.what();
    }
    CHECK(contains(error, "does not touch"));
    CHECK_EQ(output.value("failed_step", ""), std::string("away"));
  }
  {  // the end state checked: valid, the step that passed through the error says so
    Document doc = base;
    json output;
    std::set<std::string> valid;
    agent::model_batch(doc, {{"steps", there_and_back}, {"validate", "final"}}, output, {nullptr, nullptr, {}, &valid});
    CHECK(!output.contains("failed_step"));
    CHECK_EQ(output["final_check"]["state"], json("valid"));
    CHECK(contains(output["steps"][0]["result"].value("errors", json::array()).dump(), "does not touch"));
    const Scene end = resolve(doc);
    CHECK(end.feature(cut)->error.empty());
    CHECK_EQ(end.feature(cut)->inputs["x"], json("3 mm"));
    CHECK_EQ(valid.size(), size_t(1));
  }
  {  // an end state that is still broken fails as a whole, naming the feature
    Document doc = base;
    json output;
    std::string error;
    try {
      agent::model_batch(doc, {{"steps", json::array({move("away", "100 mm")})}, {"validate", "final"}}, output);
    } catch (const Error& e) {
      error = e.what();
    }
    CHECK(contains(error, "end state"));
    CHECK(contains(error, "box 'Pocket' (" + cut + ") failed: the cut does not touch any body"));
    CHECK_EQ(output.value("failed_step", ""), std::string("final"));
    CHECK_EQ(output["final_check"]["state"], json("failed"));
    CHECK_EQ(output["final_check"]["errors"].size(), size_t(1));
  }
  // The errors are deferred only inside the batch.
  CHECK(!design::DeferErrors::active());
  Document doc = base;
  json output;
  CHECK_THROWS(agent::model_batch(doc, {{"steps", there_and_back}, {"validate", "later"}}, output));
}

CHECK_MAIN()
