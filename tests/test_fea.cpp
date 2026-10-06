// Structural studies (Netgen + CalculiX, sim/fea.cpp) against beam theory: a cantilever's tip deflection (P L^3 / 3 E I)
// and its bending stress (M c / I) half way along, a bar's axial stress and stretch (F / A, F L / A E), a cantilever's
// first natural frequency (1.8751^2 / 2 pi sqrt(E I / rho A L^4)), and a bolt's shank stress under its preload (F / A),
// the joint around it bonded where the parts touch. Skipped when ccx is not installed.
#include <cmath>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/sim/fea.hpp"

using namespace opad;

namespace {

constexpr double kPi = 3.14159265358979323846;
const double E = 210000, nu = 0.3, rho = 7.85e-9;  // steel: MPa, -, t/mm3

bool engines_ok() { return sim::engines().value("static", false); }

std::string box(Document& doc, Vec3 at, double l, double w, double h, const std::string& name) {
  const json r = commands::run("feature", {{"kind", "box"}, {"name", name}, {"inputs", {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"length", l}, {"width", w}, {"height", h}, {"centered", false}}}}, &doc);
  return r["body_ids"][0];
}

json face_at(const std::string& body, const char* axis, double value) {
  return {{"body", body}, {"kind", "face"}, {"select", {{"surface", "plane"}, {"at_plane", {{"axis", axis}, {"value", value}}}}}, {"expect", 1}};
}

}  // namespace

TEST(cantilever_deflection_and_stress) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  const double L = 400, b = 10, h = 10, P = 25;  // slender (L/h = 40): the clamped root's 3D effect stays under 0.5%
  const std::string beam = box(doc, {0, 0, 0}, L, b, h, "Beam");
  commands::run("part_properties", {{"target", beam}, {"set", {{"material", "steel"}}}}, &doc);
  commands::run("load", {{"kind", "fixed"}, {"refs", {face_at(beam, "x", 0)}}}, &doc);
  commands::run("load", {{"kind", "force"}, {"refs", {face_at(beam, "x", L)}}, {"vector", {0, 0, -P}}}, &doc);
  const sim::StudyRun run = sim::run_study(doc, resolve(doc), {{"kind", "static"}, {"settings", {{"mesh_size", 3}}}});
  const sim::FeaResult& r = *run.fea;
  const double I = b * h * h * h / 12;
  const double bending = P * L * L * L / (3 * E * I);
  const double G = E / (2 * (1 + nu)), shear = P * L / (5.0 / 6 * G * b * h);  // Timoshenko's shear part
  const double tip = -sim::probe(r, {L, b / 2, h / 2}, "dz");
  CHECK_NEAR(tip, bending + shear, 0.01 * (bending + shear));
  // Bending stress on the top fibre half way along: M c / I (tension on top).
  const double sigma = P * (L / 2) * (h / 2) / I;
  CHECK_NEAR(sim::probe(r, {L / 2, b / 2, h}, "sxx"), sigma, 0.02 * sigma);
  CHECK_NEAR(sim::probe(r, {L / 2, b / 2, 0}, "sxx"), -sigma, 0.02 * sigma);
  // The support carries the load.
  const json reaction = run.summary["reactions_N"]["Fixed 1"];
  CHECK_NEAR(reaction[2].get<double>(), P, 1e-3 * P);
  CHECK(run.summary["min_safety_factor"].get<double>() > 1);
}

TEST(bar_in_tension) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  const double L = 200, a = 10, F = 2000;
  const std::string bar = box(doc, {0, 0, 0}, L, a, a, "Bar");
  commands::run("load", {{"kind", "fixed"}, {"refs", {face_at(bar, "x", 0)}}}, &doc);
  commands::run("load", {{"kind", "pressure"}, {"refs", {face_at(bar, "x", L)}}, {"value", -F / (a * a)}}, &doc);  // a pull: negative pressure
  const sim::StudyRun run = sim::run_study(doc, resolve(doc), {{"kind", "static"}, {"settings", {{"mesh_size", 4}}}});
  const sim::FeaResult& r = *run.fea;
  CHECK_NEAR(sim::probe(r, {L / 2, a / 2, a / 2}, "sxx"), F / (a * a), 0.005 * F / (a * a));
  // Stretch measured between two sections away from the held end's Poisson restraint.
  const double stretch = sim::probe(r, {3 * L / 4, a / 2, a / 2}, "dx") - sim::probe(r, {L / 4, a / 2, a / 2}, "dx");
  CHECK_NEAR(stretch, F * (L / 2) / (a * a * E), 0.005 * F * (L / 2) / (a * a * E));
}

TEST(cantilever_first_frequency) {
  if (!engines_ok()) return;
  Document doc = Document::create();
  const double L = 200, a = 10;
  const std::string beam = box(doc, {0, 0, 0}, L, a, a, "Beam");
  commands::run("load", {{"kind", "fixed"}, {"refs", {face_at(beam, "x", 0)}}}, &doc);
  const sim::StudyRun run = sim::run_study(doc, resolve(doc), {{"kind", "modal"}, {"settings", {{"mesh_size", 3}, {"modes", 4}}}});
  const auto& f = run.fea->frequencies;
  const double I = a * a * a * a / 12, A = a * a;
  const double f1 = 1.875104 * 1.875104 / (2 * kPi) * std::sqrt(E * I / (rho * A * L * L * L * L));
  CHECK(f.size() >= 2);
  CHECK_NEAR(f[0], f1, 0.015 * f1);  // the two bending directions of a square bar
  CHECK_NEAR(f[1], f1, 0.015 * f1);
  CHECK_EQ(run.fea->modes.size(), f.size());
}

TEST(bolt_preload_stresses_the_shank) {
  if (!engines_ok()) return;
  // A bolt (10 mm shank, 16 mm head and nut) clamping a 20 mm sleeve between head and nut: bonded where they touch.
  Document doc = Document::create();
  auto cyl = [&](Vec3 at, double d, double h, const std::string& name, const std::string& join = {}) {
    json in = {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"diameter", d}, {"height", h}};
    if (!join.empty()) in["operation"] = "join", in["targets"] = {join};
    return commands::run("feature", {{"kind", "cylinder"}, {"name", name}, {"inputs", in}}, &doc)["body_ids"][0].get<std::string>();
  };
  const std::string bolt = cyl({0, 0, -6}, 16, 6, "Bolt");
  cyl({0, 0, 0}, 10, 20, "Shank", bolt);
  cyl({0, 0, 20}, 16, 6, "Nut", bolt);
  const std::string sleeve = cyl({0, 0, 0}, 24, 20, "Sleeve");
  commands::run("feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"origin", {0, 0, -1}}, {"normal", {0, 0, 1}}}}, {"diameter", 10.4}, {"height", 22}, {"operation", "cut"}, {"targets", {sleeve}}}}}, &doc);
  const double F = 10000;
  commands::run("load", {{"kind", "bolt_preload"}, {"refs", {bolt}}, {"value", F}}, &doc);
  const sim::StudyRun run = sim::run_study(doc, resolve(doc), {{"kind", "static"}, {"settings", {{"bodies", {bolt, sleeve}}, {"mesh_size", 1.5}}}});
  const json b = run.summary["bolts"][0];
  const double A = kPi * 25;
  CHECK_NEAR(b["section_area_mm2"].get<double>(), A, 0.01 * A);
  CHECK_NEAR(b["axial_stress_MPa"].get<double>(), F / A, 0.03 * F / A);
  // The sleeve carries it back in compression. It is not uniform (the head bears on its inner part only: the pressure cone),
  // but equilibrium makes the area-weighted mean across its middle -F over its ring.
  const double ring = kPi * (12 * 12 - 5.2 * 5.2);
  double sum = 0, weight = 0;
  for (int i = 0; i < 24; ++i) {
    const double r = 5.2 + (12 - 5.2) * (i + 0.5) / 24;
    for (int j = 0; j < 36; ++j) {
      const double t = 2 * kPi * j / 36;
      sum += sim::probe(*run.fea, {r * std::cos(t), r * std::sin(t), 10}, "szz") * r;
      weight += r;
    }
  }
  CHECK_NEAR(sum / weight, -F / ring, 0.03 * F / ring);
}

CHECK_MAIN()
