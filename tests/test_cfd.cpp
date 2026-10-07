// Thermal studies with the air solved (sim/cfd.cpp, settings.air cfd) against the correlations of the engineering model
// (sim/airflow.cpp) and conservation: a fan-cooled heatsink's flow and temperatures, the heat the air carries off against the
// heat put in, and the streamlines through the fins. Skipped when OpenFOAM (or CalculiX, for the comparison) is missing.
#include <cmath>
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
  const sim::StudyRun cfd = thermal(doc, {{"air", "cfd"}, {"ambient", 25}, {"cfd", {{"cell_size", 2}}}});
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
  for (size_t l = 0; l < cfd.fea->streamlines.size(); ++l) {
    CHECK_EQ(cfd.fea->streamlines[l].size(), cfd.fea->streamline_temperature[l].size());
    for (double T : cfd.fea->streamline_temperature[l]) CHECK(T > 25 - 0.2);
  }
  // Against the engineering model: the fan's operating point and the heatsink's temperature rise. A bypass around the fins
  // (the duct is a cell wider than them) and the developing flow the correlations average keep these to within a fifth.
  if (!sim::engines().value("thermal", false)) return;
  const sim::StudyRun model = thermal(doc, {{"mesh_size", 3}, {"ambient", 25}});
  const json& mf = model.summary["fans"][0];
  CHECK_NEAR(flow, mf["flow_m3h"].get<double>(), 0.2 * mf["flow_m3h"].get<double>());
  const double rise_cfd = s["max_temperature_C"].get<double>() - 25, rise_model = model.summary["max_temperature_C"].get<double>() - 25;
  CHECK_NEAR(rise_cfd, rise_model, 0.2 * rise_model);
}

CHECK_MAIN()
