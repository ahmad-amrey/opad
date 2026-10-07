// Design sweeps (sim/sweep.cpp, study kind sweep): a thermal study run again over a parameter's values: every point
// reported, the best found (the coolest), a refined point between the best grid point's neighbours, a value that breaks
// the model recorded as failed without stopping the rest, the document itself unchanged. Skipped without CalculiX.
#include <cmath>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/sim/fea.hpp"

using namespace opad;

namespace {

bool engines_ok() { return sim::engines().value("thermal", false); }

// A 30 x 30 mm aluminium block making 3 W in still air, its height the parameter blk_h.
Document block(std::string& id) {
  Document doc = Document::create();
  commands::run("param", {{"name", "blk_h"}, {"expr", "10 mm"}}, &doc);
  const json r = commands::run("feature", {{"kind", "box"},
                                           {"name", "Block"},
                                           {"inputs", {{"plane", {{"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}}}, {"length", 30}, {"width", 30}, {"height", "blk_h"}, {"centered", false}}}},
                               &doc);
  id = r["body_ids"][0].get<std::string>();
  commands::run("part_properties", {{"target", id}, {"set", {{"material", "aluminium-6061"}}}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"on", {id}}, {"value", 3}}, &doc);
  commands::run("load", {{"kind", "convection"}, {"on", {id}}, {"h", 10}}, &doc);
  commands::run("study", {{"kind", "thermal"}, {"name", "Block heat"}, {"settings", {{"mesh_size", 6}}}, {"run", false}}, &doc);
  return doc;
}

}  // namespace

TEST(sweep_finds_the_coolest_height) {
  if (!engines_ok()) return;
  std::string id;
  Document doc = block(id);
  const size_t ops = doc.ops.size();
  // A taller block has more area for the same 3 W: cooler. T = 25 + P / (h A), A = 30 x 30 + 4 x 30 x H (the bottom too).
  const json r = commands::run("study", {{"kind", "sweep"},
                                         {"name", "Height sweep"},
                                         {"settings", {{"study", "Block heat"}, {"params", {{{"name", "blk_h"}, {"from", 5}, {"to", 25}, {"steps", 3}}}}, {"refine", 2}}}},
                               &doc);
  const json& pts = r["points"];
  CHECK_EQ(pts.size(), 5u);  // three on the grid, two refining
  for (const auto& p : pts) {
    CHECK(p.contains("objective"));
    const double H = p["params"]["blk_h"].get<double>();
    const double A = 2 * 30 * 30 * 1e-6 + 4 * 30 * H * 1e-6;
    CHECK_NEAR(p["objective"].get<double>(), 25 + 3 / (10 * A), 0.02 * (3 / (10 * A)));
  }
  // The best: the tallest of the grid, and the refined points lie between its neighbour and it.
  CHECK_NEAR(r["best"]["params"]["blk_h"].get<double>(), 25, 1e-9);
  for (size_t i = 3; i < pts.size(); ++i) CHECK(pts[i]["params"]["blk_h"].get<double>() >= 15 - 1e-9);
  // The parameter is unchanged in the document: the sweep works on copies (it adds only its own study op).
  CHECK_EQ(resolve(doc).param("blk_h")->shown, "10 mm");
  CHECK_EQ(doc.ops.size(), ops + 1);
}

TEST(sweep_records_a_point_that_breaks_the_model) {
  if (!engines_ok()) return;
  std::string id;
  Document doc = block(id);
  // A zero height makes no block: that point fails, the others run.
  const json r = commands::run("study", {{"kind", "sweep"}, {"settings", {{"study", "Block heat"}, {"params", {{{"name", "blk_h"}, {"values", {0, 10, 20}}}}}}}}, &doc);
  CHECK(r["points"][0].contains("error"));
  CHECK(r["points"][1].contains("objective") && r["points"][2].contains("objective"));
  CHECK_NEAR(r["best"]["params"]["blk_h"].get<double>(), 20, 1e-9);
  // Settings it cannot run are refused before anything runs.
  CHECK_THROWS(commands::run("study", {{"kind", "sweep"}, {"settings", {{"study", "Block heat"}, {"params", {{{"name", "nope"}, {"values", {1}}}}}}}}, &doc));
  CHECK_THROWS(commands::run("study", {{"kind", "sweep"}, {"settings", {{"study", "No such study"}, {"params", {{{"name", "blk_h"}, {"values", {1}}}}}}}}, &doc));
}

CHECK_MAIN()
