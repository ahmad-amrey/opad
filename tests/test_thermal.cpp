// Thermal studies (sim/fea.cpp, kind thermal; sim/airflow.cpp) against heat-transfer theory: conduction along a bar (q L / k
// A), a slab with heat generated in it (q''' L^2 / 8 k), a fin with a convective tip (Incropera's fin equation), a small
// block warming up (lumped capacitance), radiation alone (Stefan-Boltzmann), and the air: a vertical plate's natural
// convection and a flat plate's forced convection (textbook examples), the heatsink channel model's two limits
// (fully developed and boundary layer), the channels' Poiseuille pressure drop, a fan's operating point, a heatsink's fins
// found in its geometry, and a fan-cooled heatsink against the one-dimensional fin-array model. Studies are skipped when
// ccx is not installed.
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepPrimAPI_MakeBox.hxx>

#include <cmath>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/sim/airflow.hpp"
#include "opad/sim/fea.hpp"

using namespace opad;
namespace air = opad::sim::air;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool engines_ok() { return sim::engines().value("thermal", false); }

std::string box(Document& doc, Vec3 at, double l, double w, double h, const std::string& name, const std::string& join = {}) {
  json inputs = {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"length", l}, {"width", w}, {"height", h}, {"centered", false}};
  if (!join.empty()) inputs["operation"] = "join", inputs["targets"] = {join};
  const json r = commands::run("feature", {{"kind", "box"}, {"name", name}, {"inputs", inputs}}, &doc);
  return join.empty() ? r["body_ids"][0].get<std::string>() : join;
}

json face_at(const std::string& body, const char* axis, double value) {
  return {{"body", body}, {"kind", "face"}, {"select", {{"surface", "plane"}, {"at_plane", {{"axis", axis}, {"value", value}}}}}, {"expect", 1}};
}

sim::StudyRun thermal(Document& doc, json settings) {
  return sim::run_study(doc, resolve(doc), {{"kind", "thermal"}, {"settings", settings}});
}

}  // namespace

TEST(conduction_along_a_bar) {
  if (!engines_ok()) return;
  // 100 x 10 x 10 mm, held at 100 degC at one end, 10 W into the other: the far end is q L / k A hotter.
  Document doc = Document::create();
  const std::string bar = box(doc, {0, 0, 0}, 100, 10, 10, "Bar");
  commands::run("load", {{"kind", "temperature"}, {"on", {face_at(bar, "x", 0)}}, {"value", 100}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"on", {face_at(bar, "x", 100)}}, {"value", 10}}, &doc);
  const double k = 167;
  const sim::StudyRun run = thermal(doc, {{"mesh_size", 4}, {"materials", {{"all", {{"k", k}}}}}});
  const double dT = 10 * 0.1 / (k * 1e-4);
  CHECK_NEAR(sim::probe(*run.fea, {100, 5, 5}, "temperature"), 100 + dT, 1e-3 * dT);
  CHECK_NEAR(sim::probe(*run.fea, {50, 5, 5}, "temperature"), 100 + dT / 2, 1e-3 * dT);
  CHECK_NEAR(run.summary["to_fixed_temperatures_W"].get<double>(), 10, 1e-9);
}

// A study material by library id ("material": the heatsink of the thermal setup, one metal for all its bodies): copper's
// conductivity, not the body's own (none here, steel assumed); a name that is no library material is refused.
TEST(study_material_from_the_library) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  const std::string bar = box(doc, {0, 0, 0}, 100, 10, 10, "Bar");
  commands::run("load", {{"kind", "temperature"}, {"on", {face_at(bar, "x", 0)}}, {"value", 100}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"on", {face_at(bar, "x", 100)}}, {"value", 10}}, &doc);
  const sim::StudyRun run = thermal(doc, {{"mesh_size", 4}, {"materials", {{bar, {{"material", "copper"}}}}}});
  const double dT = 10 * 0.1 / (390 * 1e-4);
  CHECK_NEAR(sim::probe(*run.fea, {100, 5, 5}, "temperature"), 100 + dT, 1e-3 * dT);
  CHECK_THROWS(thermal(doc, {{"mesh_size", 4}, {"materials", {{bar, {{"material", "unobtainium"}}}}}}));
}

TEST(slab_with_heat_generated_in_it) {
  if (!engines_ok()) return;
  // 1 W spread through a 100 x 10 x 10 mm bar held at 20 degC at both ends: T_max - T_s = q''' L^2 / 8 k half way.
  Document doc = Document::create();
  const std::string bar = box(doc, {0, 0, 0}, 100, 10, 10, "Bar");
  commands::run("load", {{"kind", "temperature"}, {"on", {face_at(bar, "x", 0), face_at(bar, "x", 100)}}, {"value", 20}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"on", {bar}}, {"value", 1}}, &doc);
  const double k = 50, q = 1 / 1e-5;  // W/m3: 1 W in 1e4 mm3
  const sim::StudyRun run = thermal(doc, {{"mesh_size", 4}, {"materials", {{"all", {{"k", k}}}}}});
  const double rise = q * 0.1 * 0.1 / (8 * k);
  CHECK_NEAR(sim::probe(*run.fea, {50, 5, 5}, "temperature") - 20, rise, 0.005 * rise);
  CHECK_NEAR(sim::probe(*run.fea, {25, 5, 5}, "temperature") - 20, 0.75 * rise, 0.005 * rise);  // parabolic: 4 x (1 - x) at x = 1/4
}

TEST(fin_with_a_convective_tip) {
  if (!engines_ok()) return;
  // An aluminium pin 100 x 5 x 5 mm, its base held at 100 degC in air at 25 degC, h = 25 W/m2K on its sides and tip:
  // theta_tip / theta_b = 1 / (cosh mL + (h / m k) sinh mL), m = sqrt(h P / k A); the heat it sheds, the fin equation.
  Document doc = Document::create();
  const double L = 0.1, a = 0.005, k = 167, h = 25;
  const std::string pin = box(doc, {0, 0, 0}, 100, 5, 5, "Pin");
  commands::run("load", {{"kind", "temperature"}, {"on", {face_at(pin, "x", 0)}}, {"value", 100}}, &doc);
  commands::run("load", {{"kind", "convection"},
                         {"on", {face_at(pin, "x", 100), face_at(pin, "y", 0), face_at(pin, "y", 5), face_at(pin, "z", 0), face_at(pin, "z", 5)}},
                         {"h", h},
                         {"ambient", 25}},
                &doc);
  const sim::StudyRun run = thermal(doc, {{"mesh_size", 2.5}, {"ambient", 25}, {"materials", {{"all", {{"k", k}}}}}});
  const double P = 4 * a, A = a * a, m = std::sqrt(h * P / (k * A)), r = h / (m * k), tb = 75;
  const double tip = tb / (std::cosh(m * L) + r * std::sinh(m * L));
  CHECK_NEAR(sim::probe(*run.fea, {100, 2.5, 2.5}, "temperature") - 25, tip, 0.01 * tb);
  const double q = std::sqrt(h * P * k * A) * tb * (std::sinh(m * L) + r * std::cosh(m * L)) / (std::cosh(m * L) + r * std::sinh(m * L));
  CHECK_NEAR(run.summary["to_air_W"].get<double>(), q, 0.01 * q);
}

TEST(small_block_warming_up) {
  if (!engines_ok()) return;
  // A 10 mm copper cube, 2 W in it, h = 20 W/m2K all round (Biot 1e-4: one temperature): T - T_inf = (P / h A)(1 - e^(-t/tau)),
  // tau = rho c V / h A.
  Document doc = Document::create();
  const std::string cube = box(doc, {0, 0, 0}, 10, 10, 10, "Cube");
  commands::run("part_properties", {{"target", cube}, {"set", {{"material", "copper"}}}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"on", {cube}}, {"value", 2}}, &doc);
  commands::run("load", {{"kind", "convection"}, {"on", {cube}}, {"h", 20}}, &doc);
  const sim::StudyRun run = thermal(doc, {{"mesh_size", 5}, {"ambient", 20}, {"duration", 600}, {"frames", 61}});
  const double A = 6e-4, V = 1e-6, tau = 8960 * 385 * V / (20 * A), steady = 2 / (20 * A);
  CHECK(run.t.size() >= 60);
  const auto& series = run.series.front().v;  // the cube's hottest point over time
  for (size_t i = 10; i < run.t.size(); i += 15) {
    const double want = steady * (1 - std::exp(-run.t[i] / tau));
    CHECK_NEAR(series[i] - 20, want, 0.01 * steady);
  }
}

TEST(radiation_alone) {
  if (!engines_ok()) return;
  // 0.5 W into a 10 mm copper cube that only radiates (emissivity 0.9, surroundings at 25 degC): P = e sigma A (T^4 - T_a^4).
  Document doc = Document::create();
  const std::string cube = box(doc, {0, 0, 0}, 10, 10, 10, "Cube");
  commands::run("part_properties", {{"target", cube}, {"set", {{"material", "copper"}}}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"on", {cube}}, {"value", 0.5}}, &doc);
  commands::run("load", {{"kind", "radiation"}, {"on", {cube}}, {"emissivity", 0.9}}, &doc);
  const sim::StudyRun run = thermal(doc, {{"mesh_size", 5}, {"ambient", 25}});
  const double Ta = 298.15, T = std::pow(Ta * Ta * Ta * Ta + 0.5 / (0.9 * 5.670e-8 * 6e-4), 0.25) - 273.15;
  CHECK_NEAR(run.summary["max_temperature_C"].get<double>(), T, 0.01 * (T - 25));
}

TEST(air_correlations) {
  // Still air (Incropera's air at 320 K): a 0.25 m vertical plate at 70 degC in 25 degC air, Churchill and Chu: h = 5.45.
  CHECK_NEAR(air::natural_h(0, 0.25, 70, 25), 5.45, 0.03 * 5.45);
  // A stream: 10 m/s of 25 degC air along a 0.5 m plate, laminar (Re 3.2e5): h = 0.664 Re^0.5 Pr^1/3 k / L = 17.6.
  CHECK_NEAR(air::forced_plate_h(10, 0.5, 25), 17.6, 0.03 * 17.6);
  // Fins far apart behave as lone plates: the channel correlation tends to the plate's.
  const double lone = air::natural_h(0, 0.05, 60, 25), wide = air::natural_h(0, 0.05, 60, 25, 0.05);
  CHECK(std::fabs(wide - lone) < 0.15 * lone);
  // Fins close together choke each other's flow.
  CHECK(air::natural_h(0, 0.05, 60, 25, 0.002) < 0.6 * lone);
  // A hot face looking up sheds more than one looking down.
  CHECK(air::natural_h(1, 0.05, 60, 25) > 1.5 * air::natural_h(-1, 0.05, 60, 25));
}

TEST(heatsink_channel_model) {
  air::FinArray f;
  f.fins = 31, f.t = 0.2e-3, f.gap = 2e-3, f.height = 40e-3, f.length = 0.1, f.width = 30 * 2.2e-3 + 0.2e-3;
  const air::Air a = air::properties(25);
  // Slow flow: the air leaves at the walls' temperature, fully developed: Nu_b -> Re* Pr / 2.
  {
    const double Q = 1e-6;
    const air::Channel c = air::channel(f, Q, 25);
    const double Res = c.V * f.gap / a.nu * f.gap / f.length;
    CHECK(Res < 0.05);
    CHECK_NEAR(c.h, Res * a.Pr / 2 * a.k / f.gap, 0.02 * c.h);
    // Long thin channels: Poiseuille, 12 mu V L / b^2, and the thin fins add almost no entrance and exit loss.
    CHECK_NEAR(c.dp, 12 * a.mu * c.V * f.length / (f.gap * f.gap), 0.05 * c.dp);
  }
  // Fast flow: a boundary layer on each wall, the flat plate's 0.664 Re_L^1/2 Pr^1/3.
  {
    air::FinArray w = f;
    w.gap = 20e-3, w.length = 0.02;
    const air::Channel c = air::channel(w, 0.24, 25);  // 10 m/s: Re* above 1e4, where the correction is under 2 %
    const double ReL = c.V * w.length / a.nu;
    CHECK_NEAR(c.h, 0.664 * std::sqrt(ReL) * std::cbrt(a.Pr) * a.k / w.length, 0.03 * c.h);
  }
}

TEST(fan_operating_point) {
  // A straight-line fan curve against a system dp = K Q^2: K Q^2 + (Pmax / Qmax) Q - Pmax = 0.
  air::Fan fan = air::fan_from(json{{"flow", 100.0}, {"pressure", 30.0}});
  const double Qmax = 100.0 / 3600, Pmax = 30, K = 2e5;
  const double Q = (-Pmax / Qmax + std::sqrt(Pmax * Pmax / (Qmax * Qmax) + 4 * K * Pmax)) / (2 * K);
  CHECK_NEAR(air::operating_point(fan, [&](double q) { return K * q * q; }), Q, 1e-9);
  // Two side by side: each at half the flow, the same pressure.
  const double Q2 = air::operating_point(fan, [&](double q) { return K * q * q; }, 2);
  CHECK_NEAR(fan.pressure(Q2 / 2), K * Q2 * Q2, 1e-6);
  // A curve from points; library fans; bad specs refused.
  const air::Fan pts = air::fan_from(json{{"curve", {{0, 40}, {50, 25}, {100, 0}}}});
  CHECK_NEAR(pts.pressure(25.0 / 3600), 32.5, 1e-9);
  CHECK(air::fan("120x25") && air::fan("120x25")->Pmax > 0);
  CHECK_THROWS(air::fan_from(json("no-such-fan")));
  CHECK_THROWS(air::fan_from(json{{"flow", 50}}));
}

TEST(heatsink_fins_found_in_the_geometry) {
  // A 60 x 60 x 5 mm base with ten 1.5 mm fins 30 mm tall along X.
  TopoDS_Shape s = BRepPrimAPI_MakeBox(60, 60, 5).Shape();
  const double t = 1.5, gap = (60 - 10 * t) / 9;
  for (int i = 0; i < 10; ++i) s = BRepAlgoAPI_Fuse(s, BRepPrimAPI_MakeBox(gp_Pnt(0, i * (t + gap), 5), 60, t, 30).Shape()).Shape();
  const auto f = air::fin_array(s, {1, 0, 0});
  CHECK(f.has_value());
  CHECK_EQ(f->fins, 10);
  CHECK_NEAR(f->t, 1.5e-3, 1e-7);
  CHECK_NEAR(f->gap, gap * 1e-3, 1e-7);
  CHECK_NEAR(f->height, 30e-3, 0.5e-3);  // to the scan's step
  CHECK_NEAR(f->base, 5e-3, 0.5e-3);
  CHECK_NEAR(f->length, 60e-3, 1e-7);
  CHECK_NEAR(f->up[2], 1, 1e-9);
  CHECK(!air::fin_array(BRepPrimAPI_MakeBox(60, 60, 5).Shape(), {1, 0, 0}).has_value());  // a plate has no fins
}

TEST(fan_cooled_heatsink_against_the_fin_array_model) {
  if (!engines_ok()) return;
  // 40 W spread under the whole base of a 60 x 60 mm aluminium heatsink (ten 1.5 mm fins, 30 mm tall) with a 60 mm fan:
  // the base at T_air + Q / (h (N eta_f A_f + A_b)) + Q t_b / (k A), eta_f = tanh(m Hc) / (m Hc) with the corrected fin
  // height Hc = H + t/2, the air at its mean temperature along the fins.
  Document doc = Document::create();
  const std::string hs = box(doc, {0, 0, 0}, 60, 60, 5, "Heatsink");
  const double t = 1.5, gap = (60 - 10 * t) / 9, H = 30;
  for (int i = 0; i < 10; ++i) box(doc, {0, i * (t + gap), 5}, 60, t, H, "Fin", hs);
  const double k = 200;
  commands::run("load", {{"kind", "heat"}, {"on", {face_at(hs, "z", 0)}}, {"value", 40}}, &doc);
  commands::run("load", {{"kind", "fan"}, {"on", {hs}}, {"fan", "60x15"}, {"vector", {1, 0, 0}}}, &doc);
  const sim::StudyRun run = thermal(doc, {{"mesh_size", 3}, {"ambient", 25}, {"materials", {{"all", {{"k", k}}}}}});
  const json& fan = run.summary["fans"][0];
  CHECK_EQ(fan["fins"]["fins"].get<int>(), 10);
  const double h = fan["h_W_m2K"].get<double>(), rise = fan["air_rise_C"].get<double>();
  // Every face the air washes: fin sides and tips, the base between the fins and its ends, the fins' ends.
  const double Lm = 0.06, tm = t * 1e-3, Hm = H * 1e-3;
  const double m = std::sqrt(2 * h / (k * tm)), Hc = Hm + tm / 2, eta = std::tanh(m * Hc) / (m * Hc);
  const double Af = 2 * Hc * Lm;                     // one fin, tip counted by the corrected height
  const double Ab = (0.06 - 10 * tm) * Lm;           // the base between the fins
  const double ends = 2 * (0.06 * 0.005);            // the base's two ends along the flow (the fins' ends are in Hc)
  const double sides = 2 * (Lm * 0.005);             // the base's two sides
  const double R = 1 / (h * (10 * eta * Af + Ab + ends + sides)) + 0.005 / (k * 0.06 * 0.06);
  const double base = 25 + rise / 2 + 40 * R;
  CHECK_NEAR(sim::probe(*run.fea, {30, 30, 0}, "temperature"), base, 0.04 * (base - 25));
  CHECK_NEAR(run.summary["to_air_W"].get<double>(), 40, 0.01 * 40);
  // The air took the heat: Q = m cp dT.
  const air::Air a = air::properties(25);
  CHECK_NEAR(rise, 40 / (a.rho * fan["flow_m3h"].get<double>() / 3600 * a.cp), 0.02 * rise);
}

TEST(orthotropic_board_along_and_across) {
  if (!engines_ok()) return;
  // A 40 x 40 x 2 mm board, 20 W/m.K along it and 0.5 across. Across: 2 W into its top, its bottom held at 25 degC, the sides
  // insulated: the top at 25 + q t / k_across = 25 + 1250 W/m2 x 2 mm / 0.5 = 30 degC. Along: 1 W into one end, the other held:
  // 25 + P L / (k_along A) = 25 + 1 x 0.04 / (20 x 40 x 2e-6) = 50 degC.
  Document doc = Document::create();
  const std::string b = box(doc, {0, 0, 0}, 40, 40, 2, "Board");
  commands::run("load", {{"kind", "heat"}, {"case", "Across"}, {"on", {face_at(b, "z", 2)}}, {"value", 2}}, &doc);
  commands::run("load", {{"kind", "temperature"}, {"case", "Across"}, {"on", {face_at(b, "z", 0)}}, {"value", 25}}, &doc);
  commands::run("load", {{"kind", "heat"}, {"case", "Along"}, {"on", {face_at(b, "x", 40)}}, {"value", 1}}, &doc);
  commands::run("load", {{"kind", "temperature"}, {"case", "Along"}, {"on", {face_at(b, "x", 0)}}, {"value", 25}}, &doc);
  const json mats = {{"all", {{"k", 20}, {"k_through", 0.5}}}};
  const sim::StudyRun across = thermal(doc, {{"case", "Across"}, {"mesh_size", 4}, {"materials", mats}});
  CHECK_NEAR(sim::probe(*across.fea, {20, 20, 2}, "temperature"), 30, 0.1);
  const sim::StudyRun along = thermal(doc, {{"case", "Along"}, {"mesh_size", 4}, {"materials", mats}});
  CHECK_NEAR(sim::probe(*along.fea, {40, 20, 1}, "temperature"), 50, 0.3);
  CHECK_NEAR(along.summary["bodies"]["Board"]["conductivity_through_W_mK"].get<double>(), 0.5, 1e-9);
  // A 4-layer board of 1 oz copper covering 70 %: about 24 W/m.K along, FR-4's 0.3 across.
  const sim::StudyRun pcb = thermal(doc, {{"case", "Along"}, {"mesh_size", 4}, {"materials", {{"all", {{"pcb", {{"layers", 4}}}}}}}});
  CHECK_NEAR(pcb.summary["bodies"]["Board"]["conductivity_W_mK"].get<double>(), 24.2, 0.3);
  CHECK_NEAR(pcb.summary["bodies"]["Board"]["conductivity_through_W_mK"].get<double>(), 0.32, 0.01);
}

CHECK_MAIN()
