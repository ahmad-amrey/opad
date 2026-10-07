// Dynamic studies (Project Chrono, sim/dynamics.cpp) against textbook results: a compound pendulum's period at small and
// large swings, free fall, a flywheel spun by a torque, inertia seen through a gear pair, a spring-mass oscillator, the
// torque a screw jack needs to lift a load, energy conservation, a knob held by its joint while a loose one falls, and a
// box landing on a plate. Masses and inertias come from the solids (sim/inertia.hpp), checked against formulas first.
#include <cmath>
#include <functional>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/sim/inertia.hpp"
#include "opad/sim/study.hpp"

using namespace opad;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double g = 9.80665;  // m/s2

std::string box(Document& doc, Vec3 at, double l, double w, double h, const std::string& name, Vec3 normal = {0, 0, 1}) {
  const json r = commands::run("feature", {{"kind", "box"}, {"name", name},
                                           {"inputs", {{"plane", {{"origin", at}, {"normal", normal}}}, {"length", l}, {"width", w}, {"height", h}}}},
                               &doc);
  return r["body_ids"][0];
}

std::string disc(Document& doc, Vec3 at, double d, double h, const std::string& name) {
  const json r = commands::run("feature", {{"kind", "cylinder"}, {"name", name}, {"inputs", {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"diameter", d}, {"height", h}}}}, &doc);
  return r["body_ids"][0];
}

json joint(Document& doc, json args) { return commands::run("joint", args, &doc); }
json axis(Vec3 o, Vec3 z = {0, 0, 1}) { return {{"origin", o}, {"z", z}}; }

bool engines_ok() { return sim::engines().value("dynamic", false); }

const std::vector<double>* series(const sim::StudyRun& run, const std::string& name) {
  for (const auto& s : run.series)
    if (s.name == name) return &s.v;
  return nullptr;
}

sim::StudyRun dynamic(Document& doc, json settings) {
  return sim::run_study(doc, resolve(doc), {{"kind", "dynamic"}, {"settings", settings}});
}

// Times the angle series crosses zero going up: the period is the spacing.
double period(const std::vector<double>& t, const std::vector<double>& v) {
  std::vector<double> ups;
  for (size_t i = 1; i < v.size(); ++i)
    if (v[i - 1] < 0 && v[i] >= 0) ups.push_back(t[i - 1] + (t[i] - t[i - 1]) * (-v[i - 1]) / (v[i] - v[i - 1]));
  if (ups.size() < 2) return 0;
  return (ups.back() - ups.front()) / double(ups.size() - 1);
}

// Complete elliptic integral of the first kind (arithmetic-geometric mean).
double ellipk(double k) {
  double a = 1, b = std::sqrt(1 - k * k);
  for (int i = 0; i < 30; ++i) {
    const double an = (a + b) / 2, bn = std::sqrt(a * b);
    a = an, b = bn;
  }
  return kPi / (2 * a);
}

}  // namespace

TEST(inertia_of_a_box_is_the_formula) {
  Document doc = Document::create();
  const std::string b = box(doc, {0, 0, 0}, 200, 20, 10, "Bar");
  const sim::PartMass m = sim::part_mass(doc, resolve(doc), b);
  const double mass = 200 * 20 * 10 * 7.85e-6;  // kg (steel assumed, said so)
  CHECK_NEAR(m.mass, mass, 1e-9);
  CHECK(!m.notes.empty());
  CHECK_NEAR(m.centre[2], 5, 1e-9);
  CHECK_NEAR(m.inertia[0], mass * (20 * 20 + 10 * 10) / 12, 1e-6);   // Ixx kg.mm2
  CHECK_NEAR(m.inertia[4], mass * (200 * 200 + 10 * 10) / 12, 1e-6);  // Iyy
  CHECK_NEAR(m.inertia[8], mass * (200 * 200 + 20 * 20) / 12, 1e-6);  // Izz
  CHECK_NEAR(m.inertia[1], 0, 1e-9);
  // Aluminium: the material property wins.
  commands::run("part_properties", {{"target", b}, {"set", {{"material", "aluminium-6061"}}}}, &doc);
  CHECK_NEAR(sim::part_mass(doc, resolve(doc), b).mass, 200 * 20 * 10 * 2.70e-6, 1e-9);
}

TEST(compound_pendulum_period) {
  if (!engines_ok()) return;
  // A 200 x 10 x 10 bar hanging from a pin through one end (axis Y), released at 5 and at 90 degrees.
  for (double amplitude : {5.0, 90.0}) {
    Document doc = Document::create();
    const double L = 200, w = 10;
    const std::string bar = box(doc, {L / 2 - 5, 0, 0}, L, w, w, "Bar");  // spans x in [-5, 195]: pin at x = 0
    const json j = joint(doc, {{"kind", "revolute"}, {"part", bar}, {"at", axis({0, 0, w / 2}, {0, 1, 0})}});
    // Released horizontal-ish: set the start angle kinematically (the bar along +X is 90 degrees from hanging).
    commands::run("joint_set", {{"values", {{j["id"], 90 - amplitude}}}}, &doc);
    const sim::StudyRun run = dynamic(doc, {{"duration", amplitude > 45 ? 3.0 : 2.0}, {"frames", 2001}, {"step", 2.5e-4}});
    const auto* theta = series(run, "Revolute 1 rotation");
    CHECK(theta);
    // About the hanging position (90 deg on the joint): swing angle.
    std::vector<double> swing;
    for (double v : *theta) swing.push_back(v - 90);
    const double T = period(run.t, swing);
    // I about the pin: m (L^2 + w^2) / 12 + m d^2 with d the centre's distance, the bar spanning [-5, 195].
    const double d = (L / 2 - 5) * 1e-3, Lm = L * 1e-3, wm = w * 1e-3;
    const double I_over_m = (Lm * Lm + wm * wm) / 12 + d * d;
    const double T0 = 2 * kPi * std::sqrt(I_over_m / (g * d));
    const double expected = amplitude < 10 ? T0 * (1 + amplitude * amplitude * kPi * kPi / 180 / 180 / 16) : 4 * std::sqrt(I_over_m / (g * d)) * ellipk(std::sin(amplitude * kPi / 360));
    CHECK_NEAR(T, expected, 2e-3 * expected);
    // Energy is kept (HHT with little numerical damping): drift under 0.5% of the swing's energy.
    const auto* E = series(run, "Total energy");
    const auto* K = series(run, "Kinetic energy");
    double kmax = 0;
    for (double k : *K) kmax = std::max(kmax, k);
    CHECK(run.summary["energy_drift_J"].get<double>() < 5e-3 * kmax);
    (void)E;
  }
}

TEST(free_fall_and_a_knob_that_does_not_fall) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  const std::string panel = box(doc, {0, 0, 0}, 100, 60, 5, "Panel");
  const std::string held = disc(doc, {-20, 0, 5}, 20, 15, "Held knob");
  const std::string loose = disc(doc, {20, 0, 5}, 20, 15, "Loose knob");
  joint(doc, {{"kind", "ground"}, {"part", panel}, {"at", axis({0, 0, 0})}});
  joint(doc, {{"kind", "revolute"}, {"base", panel}, {"part", held}, {"at", axis({-20, 0, 5})}});
  const sim::StudyRun run = dynamic(doc, {{"duration", 1.0}, {"frames", 11}, {"free", {loose}}});
  const size_t held_at = size_t(std::find(run.parts.begin(), run.parts.end(), held) - run.parts.begin());
  const size_t loose_at = size_t(std::find(run.parts.begin(), run.parts.end(), loose) - run.parts.begin());
  CHECK_NEAR(run.poses.back()[held_at].at(2, 3), 0, 1e-6);                         // the joint holds it up
  CHECK_NEAR(run.poses.back()[loose_at].at(2, 3), -0.5 * g * 1e3, 1e-3 * 0.5 * g * 1e3);  // 1/2 g t^2 in mm
  CHECK_NEAR(run.t.back(), 1.0, 1e-9);
}

TEST(flywheel_spun_by_a_torque) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  const std::string wheel = disc(doc, {0, 0, 0}, 100, 20, "Flywheel");
  joint(doc, {{"kind", "revolute"}, {"part", wheel}, {"at", axis({0, 0, 0})}, {"drive", {{"mode", "torque"}, {"value", 1000}}}});  // 1 N.m
  const sim::StudyRun run = dynamic(doc, {{"duration", 1.0}, {"frames", 11}, {"gravity", false}});
  const sim::PartMass m = sim::part_mass(doc, resolve(doc), wheel);
  const double I = m.inertia[8] * 1e-6;  // kg.m2 about the axis
  CHECK_NEAR(I, 0.5 * m.mass * 0.05 * 0.05, 1e-3 * I);  // a disc: m r^2 / 2 (the mesh-free solid is exact)
  const double alpha = 1.0 / I;                          // rad/s2
  const auto* theta = series(run, "Revolute 1 rotation");
  CHECK_NEAR(theta->back(), 0.5 * alpha * 180 / kPi, 2e-3 * 0.5 * alpha * 180 / kPi);
  const auto* power = series(run, "Revolute 1 motor power");
  CHECK(power);
  CHECK_NEAR(power->at(5), 1.0 * alpha * 0.5, 3e-3 * alpha);  // P = T w at t = 0.5 s
}

TEST(inertia_through_a_gear_pair) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  const std::string pinion = disc(doc, {0, 0, 0}, 40, 10, "Pinion");
  const std::string wheel = disc(doc, {60, 0, 0}, 80, 10, "Wheel");
  const json a = joint(doc, {{"kind", "revolute"}, {"part", pinion}, {"at", axis({0, 0, 0})}, {"drive", {{"mode", "torque"}, {"value", 100}}}});
  const json b = joint(doc, {{"kind", "revolute"}, {"part", wheel}, {"at", axis({60, 0, 0})}});
  joint(doc, {{"kind", "gear"}, {"joints", {a["id"], b["id"]}}, {"teeth", {20, 40}}});
  const sim::StudyRun run = dynamic(doc, {{"duration", 0.5}, {"frames", 11}, {"gravity", false}});
  const Scene s = resolve(doc);
  const double I1 = sim::part_mass(doc, s, pinion).inertia[8] * 1e-6, I2 = sim::part_mass(doc, s, wheel).inertia[8] * 1e-6;
  const double alpha = 0.1 / (I1 + I2 * 0.25);  // the wheel's inertia seen through a 2:1 reduction
  const auto* t1 = series(run, "Revolute 1 rotation");
  const auto* t2 = series(run, "Revolute 2 rotation");
  const double want = 0.5 * alpha * 0.25 * 180 / kPi;
  CHECK_NEAR(t1->back(), want, 3e-3 * want);
  CHECK_NEAR(t2->back(), -0.5 * t1->back(), 1e-6 * want);
  // The tooth force's torque on the wheel: I2 x its angular acceleration (alpha / 2).
  const auto* tq = series(run, "Gear 1 torque");
  CHECK(tq);
  CHECK_NEAR(tq->back(), I2 * alpha / 2 * 1000, 5e-3 * I2 * alpha / 2 * 1000);  // N.mm
}

TEST(planetary_reduction_torque) {
  if (!engines_ok()) return;
  // Sun driven at 3.6 turns/s, carrier loaded with 3600 N.mm: at steady speed the sun's motor gives 3600 / 3.6 = 1000 N.mm.
  Document doc = Document::create();
  const std::string carrier = disc(doc, {0, 0, -10}, 50, 4, "Carrier");
  const std::string sun = disc(doc, {0, 0, 0}, 20, 4, "Sun");
  const std::string ring = disc(doc, {0, 0, 10}, 60, 4, "Ring");
  const json jc = joint(doc, {{"kind", "revolute"}, {"part", carrier}, {"at", axis({0, 0, 0})}, {"drive", {{"mode", "torque"}, {"value", -3600}}}});
  const json js = joint(doc, {{"kind", "revolute"}, {"part", sun}, {"at", axis({0, 0, 0})}, {"drive", {{"mode", "speed"}, {"value", 3.6 * 360}}}});
  const json jr = joint(doc, {{"kind", "revolute"}, {"part", ring}, {"at", axis({0, 0, 0})}, {"locked", true}});
  for (int i = 0; i < 3; ++i) {
    const double a = 2 * kPi * i / 3, x = 18 * std::cos(a), y = 18 * std::sin(a);
    const std::string planet = disc(doc, {x, y, 0}, 16, 4, "Planet");
    const json jp = joint(doc, {{"kind", "revolute"}, {"base", carrier}, {"part", planet}, {"at", axis({x, y, 0})}});
    joint(doc, {{"kind", "gear"}, {"joints", {js["id"], jp["id"]}}, {"teeth", {20, 16}}, {"carrier", carrier}});
    joint(doc, {{"kind", "gear"}, {"joints", {jr["id"], jp["id"]}}, {"teeth", {52, 16}}, {"internal", true}, {"carrier", carrier}});
  }
  const sim::StudyRun run = dynamic(doc, {{"duration", 0.5}, {"frames", 51}, {"gravity", false}, {"step", 2.5e-4}});
  const auto* tq = series(run, "Revolute 2 motor torque");
  CHECK(tq);
  CHECK_NEAR(tq->at(40), 1000, 10);  // 1% (the start-up transient long gone)
  const auto* c = series(run, "Revolute 1 rotation");
  CHECK_NEAR(c->back(), 0.5 * 360, 0.5);  // the carrier turns once per 3.6 sun turns
}

TEST(a_limit_stops_a_door) {
  if (!engines_ok()) return;
  // A 500 x 20 x 300 door hinged on Z, pushed by 20 N.m against its 90 degree stop: it rests there (the stop's give is a
  // fraction of a degree) and the hinge's stop pushes back with the 20 N.m.
  Document doc = Document::create();
  const std::string door = box(doc, {250, 0, 0}, 500, 20, 300, "Door");
  joint(doc, {{"kind", "revolute"}, {"part", door}, {"at", axis({0, 0, 0})}, {"limits", {{"rotation", {0, 90}}}}, {"drive", {{"mode", "torque"}, {"value", 20000}}}});
  const sim::StudyRun run = dynamic(doc, {{"duration", 2.0}, {"frames", 201}, {"gravity", false}});
  const auto* th = series(run, "Revolute 1 rotation");
  double peak = 0;
  for (double v : *th) peak = std::max(peak, v);
  CHECK_NEAR(th->back(), 90, 0.5);
  CHECK(peak < 92);
}

TEST(spring_mass_oscillator) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  const std::string block = box(doc, {0, 0, 0}, 40, 40, 40, "Block");  // 0.5024 kg of steel
  joint(doc, {{"kind", "slider"}, {"part", block}, {"at", axis({0, 0, 0}, {1, 0, 0})}, {"spring", {{"stiffness", 2.0}, {"rest", -10}}}});  // 2 N/mm, starts 10 mm out
  const sim::StudyRun run = dynamic(doc, {{"duration", 2.0}, {"frames", 2001}, {"gravity", false}, {"step", 2.5e-4}});
  const double m = 40 * 40 * 40 * 7.85e-6, k = 2000;  // kg, N/m
  const auto* x = series(run, "Slider 1 translation");
  std::vector<double> off;
  for (double v : *x) off.push_back(v + 10);
  CHECK_NEAR(period(run.t, off), 2 * kPi * std::sqrt(m / k), 2e-3 * 2 * kPi * std::sqrt(m / k));
  double peak = 0;
  for (double v : off) peak = std::max(peak, std::fabs(v));
  CHECK_NEAR(peak, 10, 0.05);
}

TEST(screw_jack_torque) {
  if (!engines_ok()) return;
  // A screw turning at one turn per second lifts a 2 kg-ish block along Z through a 4 mm lead: torque = m g lead / 2 pi.
  Document doc = Document::create();
  const std::string screw = disc(doc, {0, 0, 0}, 16, 100, "Screw");
  const std::string load = box(doc, {40, 0, 20}, 60, 60, 60, "Load");
  const json r = joint(doc, {{"kind", "revolute"}, {"part", screw}, {"at", axis({0, 0, 0})}, {"drive", {{"mode", "speed"}, {"value", 360}}}});
  const json sl = joint(doc, {{"kind", "slider"}, {"part", load}, {"at", axis({40, 0, 20})}});
  joint(doc, {{"kind", "lead_screw"}, {"joints", {r["id"], sl["id"]}}, {"lead", 4}});
  const sim::StudyRun run = dynamic(doc, {{"duration", 1.0}, {"frames", 21}});
  const double m = sim::part_mass(doc, resolve(doc), load).mass;
  const auto* tq = series(run, "Revolute 1 motor torque");
  CHECK(tq);
  const double want = m * g * 0.004 / (2 * kPi) * 1000;  // N.mm
  CHECK_NEAR(tq->at(10), want, 1e-2 * want);
  const auto* z = series(run, "Slider 1 translation");
  CHECK_NEAR(z->back(), 4, 1e-3);
}

TEST(box_lands_on_a_plate) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  box(doc, {0, 0, -10}, 200, 200, 10, "Plate");  // not a part: the ground
  const std::string b = box(doc, {0, 0, 20}, 30, 30, 30, "Box");
  const sim::StudyRun run = dynamic(doc, {{"duration", 0.6}, {"frames", 61}, {"free", {b}}, {"contacts", true}, {"step", 5e-4}});
  const double z = run.poses.back()[0].at(2, 3);  // its placement moved down by the 20 mm gap
  CHECK_NEAR(z, -20, 0.6);
  const auto* contacts = series(run, "Contacts");
  CHECK(contacts && contacts->back() >= 1);
}

CHECK_MAIN()
