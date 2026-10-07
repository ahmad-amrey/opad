// Thermal studies with the air solved (sim/cfd.cpp, settings.air cfd) against the correlations of the engineering model
// (sim/airflow.cpp) and conservation: a fan-cooled heatsink's flow and temperatures, the heat the air carries off against the
// heat put in, and the streamlines through the fins; a vented box with a fan inside it: the box found, the fan on its curve,
// as much air out of the box as into it, the heat leaving with it. Skipped when OpenFOAM (or CalculiX, for the comparison)
// is missing.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/sim/airflow.hpp"
#include "opad/sim/cfd.hpp"
#include "opad/sim/fea.hpp"

using namespace opad;
namespace air = opad::sim::air;

namespace {

std::string box(Document& doc, Vec3 at, double l, double w, double h, const std::string& name, const std::string& join = {}) {
  json inputs = {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"length", l}, {"width", w}, {"height", h}, {"centered", false}};
  if (!join.empty()) inputs["operation"] = "join", inputs["targets"] = {join};
  const json r = commands::run("feature", {{"kind", "box"}, {"name", name}, {"inputs", inputs}}, &doc);
  return join.empty() ? r["body_ids"][0].get<std::string>() : join;
}

// A 60 x 60 x 5 mm aluminium heatsink with six 3 mm fins 25 mm tall along X, 20 W in it, an 80 mm fan blowing along X:
// coarse fins, so that the air's mesh stays small.
Document heatsink(std::string& hs) {
  Document doc = Document::create();
  hs = box(doc, {0, 0, 0}, 60, 60, 5, "Heatsink");
  const double t = 3, gap = (60 - 6 * t) / 5;
  for (int i = 0; i < 6; ++i) box(doc, {0, i * (t + gap), 5}, 60, t, 25, "Fin", hs);
  commands::run("load", {{"kind", "heat"}, {"on", {hs}}, {"value", 20}}, &doc);
  commands::run("load", {{"kind", "fan"}, {"on", {hs}}, {"fan", "80x25"}, {"vector", {1, 0, 0}}}, &doc);
  return doc;
}

std::string cut(Document& doc, const std::string& target, Vec3 at, double l, double w, double h) {
  commands::run("feature", {{"kind", "box"},
                            {"inputs", {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"length", l}, {"width", w}, {"height", h}, {"centered", false},
                                        {"operation", "cut"}, {"targets", {target}}}}},
                &doc);
  return target;
}

// A 90 x 60 x 40 mm box with 3 mm walls, a 30 mm opening in the -X wall with a fan (a 10 x 34 x 34 mm block standing for
// it) blowing in, three 4 mm slots in the +X wall, and a 30 x 30 x 10 mm aluminium block on the floor making 5 W.
Document vented_box(std::string& enclosure, std::string& fan, std::string& block) {
  Document doc = Document::create();
  enclosure = box(doc, {0, 0, 0}, 90, 60, 40, "Enclosure");
  cut(doc, enclosure, {3, 3, 3}, 84, 54, 34);
  cut(doc, enclosure, {-1, 15, 5}, 5, 30, 30);
  for (double z : {10.0, 18.0, 26.0}) cut(doc, enclosure, {86, 10, z}, 5, 40, 4);
  fan = box(doc, {3, 13, 3}, 10, 34, 34, "Fan");
  block = box(doc, {40, 15, 3}, 30, 30, 10, "Block");
  commands::run("part_properties", {{"target", block}, {"set", {{"material", "aluminium-6061"}}}}, &doc);
  commands::run("part_properties", {{"target", enclosure}, {"set", {{"material", "abs"}}}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"on", {block}}, {"value", 5}}, &doc);
  commands::run("load", {{"kind", "fan"}, {"on", {fan}}, {"fan", {{"flow", 15}, {"pressure", 30}}}, {"vector", {1, 0, 0}}}, &doc);
  return doc;
}

sim::StudyRun thermal(Document& doc, json settings) {
  return sim::run_study(doc, resolve(doc), {{"kind", "thermal"}, {"settings", settings}});
}

}  // namespace

TEST(cfd_needs_openfoam_and_moving_air) {
  if (sim::openfoam().found()) {
    // Still air stays with the correlations.
    Document doc = Document::create();
    const std::string b = box(doc, {0, 0, 0}, 20, 20, 5, "Block");
    commands::run("load", {{"kind", "heat"}, {"on", {b}}, {"value", 1}}, &doc);
    commands::run("load", {{"kind", "convection"}, {"on", {b}}, {"h", "natural"}}, &doc);
    CHECK_THROWS(thermal(doc, {{"air", "cfd"}}));
  } else {
    std::string hs;
    Document doc = heatsink(hs);
    CHECK_THROWS(thermal(doc, {{"air", "cfd"}}));
  }
}

TEST(cfd_heatsink_against_the_correlations) {
  if (!sim::openfoam().found()) return;
  std::string hs;
  Document doc = heatsink(hs);
  const sim::StudyRun cfd = thermal(doc, {{"air", "cfd"}, {"ambient", 25}, {"cfd", {{"cell_size", 1.5}}}});
  const json& s = cfd.summary;
  CHECK_EQ(s["air"].get<std::string>(), "cfd");
  const json& fan = s["fans"][0];
  // Conservation: the heat put in leaves with the air (the duct's walls are adiabatic).
  CHECK_NEAR(fan["heat_to_air_W"].get<double>(), 20, 0.05 * 20);
  // The air's rise from its flow: Q = m cp dT.
  const air::Air a = air::properties(25);
  const double flow = fan["flow_m3h"].get<double>();
  CHECK_NEAR(fan["air_rise_C"].get<double>(), 20 / (a.rho * flow / 3600 * a.cp), 0.05 * fan["air_rise_C"].get<double>());
  // Streamlines: from the inlet, inside the duct, the air no colder than it came in.
  CHECK(!cfd.fea->streamlines.empty());
  if (std::getenv("OPAD_TEST_VERBOSE")) {
    double lo = 1e9, hi = -1e9;
    for (const auto& l : cfd.fea->streamline_temperature)
      for (double T : l) lo = std::min(lo, T), hi = std::max(hi, T);
    std::printf("%s\nstreamline T %g .. %g\n", s.dump(1).c_str(), lo, hi);
  }
  for (size_t l = 0; l < cfd.fea->streamlines.size(); ++l) {
    CHECK_EQ(cfd.fea->streamlines[l].size(), cfd.fea->streamline_temperature[l].size());
    for (double T : cfd.fea->streamline_temperature[l]) CHECK(T > 25 - 0.5);  // upwind: bounded to the solver's tolerance
  }
  // The fan's operating point is on its curve: the inlet's static plus dynamic pressure is the fan's rise at that flow.
  const air::Fan f80 = air::fan_from(json("80x25"));
  const double V = fan["inlet_velocity_m_s"].get<double>();
  CHECK_NEAR(fan["inlet_static_Pa"].get<double>() + 0.5 * a.rho * V * V, f80.pressure(flow / 3600), 0.02 * f80.pressure(flow / 3600));
  // Against the engineering model, which sends all the air through the fins: the duct here leaves a cell or two around
  // them, so some air goes round and the fan, seeing less resistance, moves more. At 1.5 mm cells (2 mm: the rise 29 %
  // over the model's; 1 mm: 11 % under) the flow is 20 % over and the rise 6 % over.
  if (!sim::engines().value("thermal", false)) return;
  const sim::StudyRun model = thermal(doc, {{"mesh_size", 3}, {"ambient", 25}});
  const json& mf = model.summary["fans"][0];
  const double rise_cfd = s["max_temperature_C"].get<double>() - 25, rise_model = model.summary["max_temperature_C"].get<double>() - 25;
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("model: %g m3/h, rise %g; cfd: %g m3/h, rise %g\n", mf["flow_m3h"].get<double>(), rise_model, flow, rise_cfd);
  CHECK(flow > mf["flow_m3h"].get<double>() && flow < 1.3 * mf["flow_m3h"].get<double>());
  CHECK_NEAR(rise_cfd, rise_model, 0.15 * rise_model);
}

TEST(cfd_fan_cooled_enclosure) {
  if (!sim::openfoam().found()) return;
  std::string enclosure, fan, block;
  Document doc = vented_box(enclosure, fan, block);
  const sim::StudyRun run = thermal(doc, {{"air", "cfd"}, {"ambient", 25}, {"cfd", {{"cell_size", 2.5}}}});
  const json& s = run.summary;
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("%s\n", s.dump(1).c_str());
  CHECK_EQ(s["enclosure"].get<std::string>(), "Enclosure");
  // The fan's block is air (its disk in the middle), the box and the block are solids.
  CHECK(s["bodies"].contains("Block") && s["bodies"].contains("Enclosure") && !s["bodies"].contains("Fan"));
  const json& f = s["fans"][0];
  const json& v = s["vents"];
  const double Q = f["flow_m3h"].get<double>();
  CHECK(Q > 1);
  // On the fan's curve: its pressure rise at its flow (a straight line from 30 Pa shut off to 15 m3/h free).
  CHECK_NEAR(f["pressure_Pa"].get<double>(), 30 * (1 - Q / 15), 0.03 * 30 * (1 - Q / 15));
  // Mass and energy: the box breathes out what it breathes in, and the air carries off the heat put in.
  CHECK_NEAR(v["air_out_m3h"].get<double>(), v["air_in_m3h"].get<double>(), 0.01 * v["air_in_m3h"].get<double>());
  // What the air carries off and what the parts radiate to the room (CalculiX's view factors) make the 5 W.
  CHECK_NEAR(v["heat_to_air_W"].get<double>() + s.value("radiated_W", 0.0), 5, 0.05 * 5);
  CHECK(s.value("radiated_W", 0.0) > 0);
  // The block is warmer than the air that leaves, and that air warmer than the room.
  CHECK(s["bodies"]["Block"]["max_temperature_C"].get<double>() > v["outlet_air_C"].get<double>());
  CHECK(v["outlet_air_C"].get<double>() > 25);
  CHECK(!run.fea->streamlines.empty());
}

// A box with vents low on one side and high on the other, a 5 W block on its floor, no fan: warm air rising draws the
// room's air in low and out high.
// About half an hour: run with OPAD_TEST_SLOW set.
TEST(cfd_passive_enclosure) {
  if (!sim::openfoam().found() || !std::getenv("OPAD_TEST_SLOW")) return;
  Document doc = Document::create();
  const std::string enclosure = box(doc, {0, 0, 0}, 90, 60, 40, "Enclosure");
  cut(doc, enclosure, {3, 3, 3}, 84, 54, 34);
  for (double y : {10.0, 26.0, 42.0}) cut(doc, enclosure, {-1, y, 5}, 5, 8, 5);   // low, in the -X wall
  for (double y : {10.0, 26.0, 42.0}) cut(doc, enclosure, {86, y, 30}, 5, 8, 5);  // high, in the +X wall
  const std::string block = box(doc, {30, 15, 3}, 30, 30, 10, "Block");
  commands::run("part_properties", {{"target", block}, {"set", {{"material", "aluminium-6061"}}}}, &doc);
  commands::run("part_properties", {{"target", enclosure}, {"set", {{"material", "abs"}}}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"on", {block}}, {"value", 5}}, &doc);
  const sim::StudyRun run =
      thermal(doc, {{"air", "cfd"}, {"ambient", 25}, {"cfd", {{"cell_size", 2.5}, {"buoyant_first", 300}, {"buoyant_pass", 100}, {"settle", 0.3}}}});
  const json& s = run.summary;
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("%s\n", s.dump(1).c_str());
  CHECK_EQ(s["enclosure"].get<std::string>(), "Enclosure");
  CHECK(s["fans"].empty());
  const json& v = s["vents"];
  CHECK(v["air_out_m3h"].get<double>() > 0.05);
  CHECK_NEAR(v["air_out_m3h"].get<double>(), v["air_in_m3h"].get<double>(), 0.02 * v["air_in_m3h"].get<double>());
  // Warm air rising does not settle fully: the air's balance closes to about 30 %, the whole to about 15 %.
  CHECK_NEAR(v["heat_to_air_W"].get<double>() + s.value("radiated_W", 0.0), 5, 0.15 * 5);
  CHECK(v["outlet_air_C"].get<double>() > 25);
  CHECK(s["bodies"]["Block"]["max_temperature_C"].get<double>() > v["outlet_air_C"].get<double>());
}

// Without a fan and without warm air rising, the air in a box stands still: refused.
TEST(cfd_enclosure_needs_moving_air) {
  if (!sim::openfoam().found()) return;
  std::string enclosure, fan, block;
  Document doc = vented_box(enclosure, fan, block);
  for (const auto& l : resolve(doc).loads)
    if (l.kind == "fan") commands::run("delete", {{"target", l.id}}, &doc);
  CHECK_THROWS(thermal(doc, {{"air", "cfd"}, {"cfd", {{"buoyancy", false}}}}));
}

CHECK_MAIN()
