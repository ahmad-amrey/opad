// Thermal studies with the air solved (sim/cfd.cpp, settings.air cfd) against the correlations of the engineering model
// (sim/airflow.cpp) and conservation: a fan-cooled heatsink's flow and temperatures, the heat the air carries off against the
// heat put in, and the streamlines through the fins; a vented box with a fan inside it: the box found, the fan on its curve,
// as much air out of the box as into it, the heat leaving with it. Those solves (minutes each) run as ctest cfd-solve (label
// slow); skipped when OpenFOAM (or CalculiX, for the comparison) is missing. The rest (vents, profiles, enclosures) is quick.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/design/sketch.hpp"
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

// The tests that solve the air with OpenFOAM (minutes each) run as ctest cfd-solve (label slow, OPAD_TEST_CFD set), not in
// the plain cfd test: a machine with OpenFOAM sat on it for many minutes with nothing said.
bool solves() { return sim::openfoam().found() && std::getenv("OPAD_TEST_CFD"); }

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
  if (!solves()) return;
  std::string hs;
  Document doc = heatsink(hs);
  // No radiation: the engineering model it is held against has none.
  const sim::StudyRun cfd = thermal(doc, {{"air", "cfd"}, {"ambient", 25}, {"cfd", {{"cell_size", 1.5}, {"radiation", false}}}});
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
  // Against the engineering model (sim/airflow.hpp: all the air through the fins, developing laminar channel correlations):
  // an estimate, not a reference. The CFD lets some air go round the fins, so the fan moves more (13 % here), and it resolves
  // the fins' boundary layers with the cell layers that put Pohlhausen's flat plate within 2-10 % (tools/validation.py): it
  // comes out 19 % cooler. This checks that the two agree in kind (flow within 30 %, rise within a quarter), not which is right.
  if (!sim::engines().value("thermal", false)) return;
  const sim::StudyRun model = thermal(doc, {{"mesh_size", 3}, {"ambient", 25}});
  const json& mf = model.summary["fans"][0];
  const double rise_cfd = s["max_temperature_C"].get<double>() - 25, rise_model = model.summary["max_temperature_C"].get<double>() - 25;
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("model: %g m3/h, rise %g; cfd: %g m3/h, rise %g\n", mf["flow_m3h"].get<double>(), rise_model, flow, rise_cfd);
  CHECK(flow > mf["flow_m3h"].get<double>() && flow < 1.3 * mf["flow_m3h"].get<double>());
  CHECK_NEAR(rise_cfd, rise_model, 0.25 * rise_model);
}

TEST(cfd_fan_cooled_enclosure) {
  if (!solves()) return;
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
  if (!solves() || !std::getenv("OPAD_TEST_SLOW")) return;
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

namespace {
bool has(const std::vector<std::string>& v, const std::string& id) { return std::find(v.begin(), v.end(), id) != v.end(); }
}  // namespace

// Enclosures of several bodies (no OpenFOAM needed): a tray with a cover on it, its board, chip and a heatsink standing up past
// the tray's rim, on a desk: the tray holds the chip, the cover closes it (a wall too), the heatsink is inside, the desk stays out.
TEST(enclosure_of_a_tray_and_its_cover) {
  Document doc = Document::create();
  const std::string tray = cut(doc, box(doc, {0, 0, 0}, 100, 80, 25, "Tray"), {3, 3, 3}, 94, 74, 25);
  const std::string cover = cut(doc, box(doc, {0, 0, 25}, 100, 80, 15, "Cover"), {3, 3, 24}, 94, 74, 13);
  const std::string board = box(doc, {10, 10, 8}, 60, 50, 1.6, "Board");
  const std::string chip = box(doc, {30, 30, 9.6}, 10, 10, 2, "Chip");
  const std::string sink = box(doc, {28, 28, 11.6}, 14, 14, 20, "Heatsink");  // up to z 31.6: past the tray's rim, under the cover
  const std::string desk = box(doc, {-50, -50, -10}, 200, 180, 10, "Desk");
  box(doc, {300, 0, 0}, 10, 10, 10, "Elsewhere");
  const Scene s = resolve(doc);
  const sim::Enclosure e = sim::find_enclosure(doc, s, {chip});
  CHECK_EQ(e.enclosure, tray);
  CHECK_EQ(e.walls.size(), 2u);
  CHECK(has(e.walls, tray) && has(e.walls, cover));
  CHECK(has(e.inside, board) && has(e.inside, sink) && !has(e.inside, desk) && !has(e.walls, desk) && !has(e.walls, sink));
  CHECK_EQ(sim::enclosure_name(s, e.walls), "Tray + Cover");
}

// A fan load as Thermal setup writes it: on a component (the fan's own model, its bodies one part), through a heatsink
// component, with the face its way was taken across; kept as written and changed by id.
TEST(fan_load_on_components) {
  Document doc = Document::create();
  const std::string block = box(doc, {0, 0, 0}, 10, 30, 30, "Fan");
  const std::string hub = box(doc, {2, 13, 13}, 4, 4, 4, "Hub");
  const std::string sink = box(doc, {20, 0, 0}, 20, 20, 2, "Heatsink");
  const std::string fan = commands::run("component", {{"name", "Fan unit"}}, &doc)["id"].get<std::string>();
  commands::run("reparent", {{"targets", {block, hub}}, {"parent", fan}}, &doc);
  const json across = {{"body", block}, {"kind", "face"}, {"index", 0}};
  const json out = commands::run("load", {{"kind", "fan"}, {"on", {fan}}, {"fan", "40x10"}, {"vector", {1, 0, 0}}, {"heatsink", {sink}}, {"across", across}}, &doc);
  const Scene s = resolve(doc);
  const Load* l = s.load(out["id"].get<std::string>());
  CHECK(l && l->error.empty() && l->refs.size() == 1 && l->refs[0].body == fan);
  CHECK(l && l->def.value("heatsink", json()) == json::array({sink}) && l->def.value("across", json()) == across);
  commands::run("load", {{"id", out["id"]}, {"heatsink", json::array()}}, &doc);
  CHECK(resolve(doc).load(out["id"].get<std::string>())->def.value("heatsink", json()) == json::array());
}

// A box split down the middle into two halves: neither holds the chip that straddles them, the two together do. The board
// under the chip and the heatsink on it hold it too, but the halves stick through their box: not an enclosure.
TEST(enclosure_of_two_halves) {
  Document doc = Document::create();
  const std::string left = cut(doc, box(doc, {0, 0, 0}, 50, 80, 40, "Left"), {3, 3, 3}, 50, 74, 34);
  const std::string right = cut(doc, box(doc, {50, 0, 0}, 50, 80, 40, "Right"), {47, 3, 3}, 50, 74, 34);
  const std::string board = box(doc, {35, 20, 3}, 30, 40, 2, "Board");
  const std::string chip = box(doc, {45, 30, 5}, 10, 10, 2, "Chip");
  const std::string sink = box(doc, {43, 28, 7}, 14, 14, 15, "Heatsink");
  const Scene s = resolve(doc);
  const sim::Enclosure e = sim::find_enclosure(doc, s, {chip});
  CHECK_EQ(e.walls.size(), 2u);
  CHECK(has(e.walls, left) && has(e.walls, right));
  CHECK(has(e.inside, board) && has(e.inside, sink));
  // Named as a list (cfd.enclosure: [ids]): the same walls, what they hold inside.
  const sim::Enclosure named = sim::find_enclosure(doc, s, {}, std::vector<std::string>{left, right});
  CHECK_EQ(named.walls.size(), 2u);
  CHECK(has(named.inside, chip) && has(named.inside, board) && has(named.inside, sink));
  CHECK_THROWS(sim::find_enclosure(doc, s, {}, std::vector<std::string>{left, "not-a-body"}));
  // One body named: as before, alone.
  CHECK_EQ(sim::find_enclosure(doc, s, {}, left).walls.size(), 1u);
}

CHECK_MAIN()

// The vents before a run (sim::vent_estimate): the vented box's 30 mm intake behind its fan, its three 40 x 4 mm slots ahead
// of it, each a sharp-edged orifice with the laminar friction along it, in series, met by the fan's straight curve; with the
// fan's opening drawn in the intake's hole, the fan draws straight through it (its cut-out) and only the slots hold it back.
TEST(vent_estimate_of_a_vented_box) {
  std::string enclosure, fan, block;
  Document doc = vented_box(enclosure, fan, block);
  const Scene scene = resolve(doc);
  const air::Air a = air::properties(25);
  // A side of n alike holes (area, width, length through, mm): the pressure that drives Q (m3/s) through them.
  struct Holes {
    int n;
    double area, width, length;
  };
  auto drop = [&](const Holes& h, double Q) {
    if (h.n == 0) return 0.0;
    const double A = h.area * 1e-6, q = Q / h.n;
    return a.rho / (2 * std::pow(0.6 * A, 2)) * q * q + 32 * a.mu * h.length * 1e-3 / (A * std::pow(h.width * 1e-3, 2)) * q;
  };
  auto expected = [&](const std::vector<Holes>& sides) {  // 30 Pa (1 - Q / 15 m3/h) = the sides' drops at Q
    double lo = 0, hi = 15.0 / 3600;
    for (int i = 0; i < 100; ++i) {
      const double m = 0.5 * (lo + hi);
      double need = 0;
      for (const auto& h : sides) need += drop(h, m);
      (30 * (1 - m * 3600 / 15) > need ? lo : hi) = m;
    }
    return 0.5 * (lo + hi) * 3600;
  };
  auto measured = [](const json& e, double area) {  // the hole of that area as the estimate measured it
    for (const auto& o : e["openings"])
      if (std::fabs(o["area_mm2"].get<double>() - area) < 1) return Holes{1, area, o["width_mm"].get<double>(), o["thickness_mm"].get<double>()};
    return Holes{0, 0, 0, 0};
  };
  json load = {{"fan", {{"flow", 15}, {"pressure", 30}}}, {"vector", {1, 0, 0}}, {"refs", {fan}}};
  json e = sim::vent_estimate(doc, scene, {enclosure}, {load}, 5);
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("%s\n", e.dump(1).c_str());
  CHECK_NEAR(e["intake"]["area_mm2"].get<double>(), 900, 1);
  CHECK_EQ(e["intake"]["openings"].get<int>(), 1);
  CHECK_NEAR(e["exhaust"]["area_mm2"].get<double>(), 480, 1);
  CHECK_EQ(e["exhaust"]["openings"].get<int>(), 3);
  Holes intake = measured(e, 900), slot = measured(e, 160);
  slot.n = 3;
  CHECK_NEAR(intake.width, 30, 1e-6);           // a square: its own side
  CHECK_NEAR(slot.width, 4 * 160.0 / 88, 1e-6);  // 4 area / perimeter
  CHECK_NEAR(slot.length, 3, 0.3);               // through the wall
  CHECK_NEAR(e["flow_m3h"].get<double>(), expected({intake, slot}), 0.01);
  CHECK_EQ(e["limited_by"].get<std::string>(), "exhaust");
  CHECK_NEAR(e["needed_m3h"].get<double>(), 5 / (a.rho * a.cp * 10) * 3600, 1e-6);
  // The fan's opening drawn: its block's face towards the intake (the face whose normal is -X).
  for (int i = 0; i < 6; ++i) {
    Ref r;
    r.body = fan, r.kind = Ref::Kind::Face, r.index = i;
    const sim::FanProfile p = sim::fan_profile(doc, scene, r.to_json());
    if (p.normal[0] < -0.9) load["outlet"] = r.to_json();
  }
  CHECK(load.contains("outlet"));
  load.erase("refs");
  e = sim::vent_estimate(doc, scene, {enclosure}, {load}, 5);
  CHECK(e["intake"].value("fan", false));
  CHECK_EQ(e["intake"]["openings"].get<int>(), 0);
  CHECK_EQ(e["openings"][0]["side"].get<std::string>(), "fan");  // the cut-out it is mounted in
  CHECK_NEAR(e["flow_m3h"].get<double>(), expected({slot}), 0.01);
  // A boss on the lid (an inner loop of the top face, no hole), a connector in a cut-out of the -Y wall: neither a vent.
  box(doc, {40, 20, 40}, 10, 10, 2, "Boss", enclosure);
  cut(doc, enclosure, {60, -1, 10}, 10, 5, 8);
  box(doc, {60, -1, 10}, 10, 6, 8, "Connector");
  const json more = sim::vent_estimate(doc, resolve(doc), {enclosure}, {load}, 5);
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("%s\n", more.dump(1).c_str());
  CHECK_EQ(more["exhaust"]["openings"].get<int>() + more["intake"]["openings"].get<int>(), 3);
  CHECK_EQ(more["left_out"]["blind"].get<int>(), 1);
  CHECK_EQ(more["left_out"]["filled"].get<int>(), 1);
  CHECK_EQ(more["openings"].size(), size_t(4));  // the three slots and the fan's own
}

// A fan's opening drawn as Draw… draws it: the face's outline projected, the opening inside it; the sketch alone names its
// smallest region (the opening), a point picks another.
TEST(fan_profile_of_a_sketch_takes_its_smallest_region) {
  Document doc = Document::create();
  design::Sketch sk;
  const int a = sk.add_point(-20, -20), b = sk.add_point(20, -20), c = sk.add_point(20, 20), d = sk.add_point(-20, 20);
  sk.add_line(a, b), sk.add_line(b, c), sk.add_line(c, d), sk.add_line(d, a);
  sk.add_circle(sk.add_point(0, 0), 10);
  const std::string id = commands::run("sketch", {{"plane", {{"origin", {0, 0, 5}}, {"normal", {0, 0, 1}}, {"x", {1, 0, 0}}}}, {"geometry", sk.to_json()}}, &doc)["sketch_id"];
  const Scene scene = resolve(doc);
  const sim::FanProfile p = sim::fan_profile(doc, scene, {{"sketch", id}});
  CHECK_NEAR(p.area, M_PI * 100, 0.5);
  CHECK_NEAR(p.centre[2], 5, 1e-6);
  CHECK_NEAR(std::fabs(p.normal[2]), 1, 1e-9);
  CHECK_EQ(p.loops.size(), size_t(1));
  const sim::FanProfile ring = sim::fan_profile(doc, scene, {{"sketch", id}, {"at", {15, 15}}});
  CHECK_NEAR(ring.area, 1600 - M_PI * 100, 0.5);
}

// A sealed box: no air goes through it, whatever the fans give (it once came out at their free flow). And a lid with a
// grille of 100 holes is estimated in seconds, each hole counted (the classifiers were made per probe: minutes).
TEST(vent_estimate_of_a_sealed_box_and_a_grille) {
  Document doc = Document::create();
  const std::string enclosure = box(doc, {0, 0, 0}, 90, 60, 40, "Enclosure");
  cut(doc, enclosure, {3, 3, 3}, 84, 54, 34);
  const std::string fan = box(doc, {10, 13, 3}, 10, 34, 34, "Fan");
  const json load = {{"fan", {{"flow", 15}, {"pressure", 30}}}, {"vector", {1, 0, 0}}, {"refs", {fan}}};
  json e = sim::vent_estimate(doc, resolve(doc), {enclosure}, {load}, 5);
  CHECK_EQ(e["limited_by"].get<std::string>(), "vents");
  CHECK_NEAR(e["flow_m3h"].get<double>(), 0, 1e-12);
  for (int i = 0; i < 10; ++i)
    for (int j = 0; j < 10; ++j) cut(doc, enclosure, {20 + 5 * i, 8 + 4.5 * j, 36}, 2, 2, 5);
  const auto t0 = std::chrono::steady_clock::now();
  int calls = 0;
  e = sim::vent_estimate(doc, resolve(doc), {enclosure}, {load}, 5, 25, [&](int, int) { return ++calls > 0; });
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("grille: %.2f s, %d progress calls\n", seconds, calls);
  CHECK_EQ(e["openings"].size(), size_t(100));  // all on one side of the fan: in and out through them, half each
  CHECK(calls >= 100);
  CHECK(seconds < 30);
  // Cancelled: stops with an error.
  bool threw = false;
  try {
    sim::vent_estimate(doc, resolve(doc), {enclosure}, {load}, 5, 25, [](int done, int) { return done < 3; });
  } catch (const Error&) {
    threw = true;
  }
  CHECK(threw);
}

namespace {
// The faces of a block whose normal is along an axis (+X: {1, 0, 0}): as a fan opening's reference.
json face_along(Document& doc, const std::string& body, const Vec3& n) {
  const Scene s = resolve(doc);
  for (int i = 0; i < 6; ++i) {
    Ref r;
    r.body = body, r.kind = Ref::Kind::Face, r.index = i;
    const sim::FanProfile p = sim::fan_profile(doc, s, r.to_json());
    if (p.normal[0] * n[0] + p.normal[1] * n[1] + p.normal[2] * n[2] > 0.9) return r.to_json();
  }
  return json();
}
void round_hole(Document& doc, const std::string& target, Vec3 at, Vec3 normal, double diameter, double length) {
  commands::run("feature", {{"kind", "cylinder"},
                            {"inputs", {{"plane", {{"origin", at}, {"normal", normal}}}, {"diameter", diameter}, {"height", length}, {"operation", "cut"}, {"targets", {target}}}}},
                &doc);
}
}  // namespace

// A blower whose intake sits against a perforated panel of the box (36 holes of 1.5 mm in a 3 mm wall): those holes are its
// own intake, in series with it (they once counted as the cut-out it is mounted in, and held nothing back); it blows into
// the box, which the slots let out. 2 mm off the panel, the box's air also comes round through the gap (outline x gap), in
// parallel: more flow (a run of a blower 2 mm under a perforated lid drew most of its air that way). Then the same blower behind a grille inside the box (a plate with 9 holes in front
// of its intake, a 30 mm opening in the wall beside): the grille's holes are its intake path, from the box's air.
TEST(vent_estimate_of_a_blower_behind_a_perforated_panel_and_a_grille) {
  auto panel = [&](double x, std::string& e, json& load) {  // the box, its perforated panel and the blower at x
    Document doc = Document::create();
    e = box(doc, {0, 0, 0}, 90, 60, 40, "Enclosure");
    cut(doc, e, {3, 3, 3}, 84, 54, 34);
    for (double z : {10.0, 18.0, 26.0}) cut(doc, e, {86, 10, z}, 5, 40, 4);
    for (int i = 0; i < 6; ++i)
      for (int j = 0; j < 6; ++j) round_hole(doc, e, {-1, 22.5 + 3 * i, 12.5 + 3 * j}, {1, 0, 0}, 1.5, 5);
    const std::string blower = box(doc, {x, 15, 5}, 20, 30, 30, "Blower");
    load = {{"name", "Blower 1"}, {"fan", "cbm-3507cf-085-295"}, {"type", "blower"}, {"inlet", face_along(doc, blower, {-1, 0, 0})},
            {"outlet", face_along(doc, blower, {0, 0, 1})}};
    return doc;
  };
  std::string e;
  json load;
  Document doc = panel(3, e, load);
  json r = sim::vent_estimate(doc, resolve(doc), {e}, {load}, 3);
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("%s\n", r.dump().substr(0, 1200).c_str());
  const json& f = r["fans"][0];
  CHECK_EQ(f["intake"]["through"].get<std::string>(), "holes");
  CHECK_EQ(f["intake"]["openings"].get<int>(), 36);
  CHECK_NEAR(f["intake"]["area_mm2"].get<double>(), 36 * M_PI * 0.75 * 0.75, 0.5);
  CHECK_EQ(f["outlet"]["through"].get<std::string>(), "box");
  CHECK_EQ(r["exhaust"]["openings"].get<int>(), 3);
  CHECK_EQ(r["limited_by"].get<std::string>(), "fan_intake");
  CHECK(r["fraction"].get<double>() < 0.9);
  CHECK(f["intake"]["pressure_Pa"].get<double>() > 10 * r["exhaust"]["pressure_Pa"].get<double>());
  int own = 0;
  for (const auto& o : r["openings"]) own += o["side"] == "fan_intake";
  CHECK_EQ(own, 36);
  CHECK(!f["intake"].contains("round_mm2"));  // against the panel: sealed
  const double sealed = r["flow_m3h"].get<double>();
  // 2 mm off it.
  Document apart = panel(5, e, load);
  r = sim::vent_estimate(apart, resolve(apart), {e}, {load}, 3);
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("%s\n", r.dump().substr(0, 1200).c_str());
  const json& off = r["fans"][0]["intake"];
  CHECK_NEAR(off.value("gap_mm", 0.0), 2, 0.3);
  CHECK_NEAR(off.value("round_mm2", 0.0), 120 * off.value("gap_mm", 0.0), 1);  // the opening's outline (30 x 30) x the gap
  CHECK(off.value("round_m3h", 0.0) > 0 && r["flow_m3h"].get<double>() > sealed);
  // Behind a grille inside the box instead: a 2 mm plate 2 mm in front of the intake with 9 holes of 3 mm, the perforated
  // panel's holes no longer in front of it (the blower moved 6 mm in); a 30 mm opening in the -X wall lets the air in.
  Document g = Document::create();
  const std::string e2 = box(g, {0, 0, 0}, 90, 60, 40, "Enclosure");
  cut(g, e2, {3, 3, 3}, 84, 54, 34);
  for (double z : {10.0, 18.0, 26.0}) cut(g, e2, {86, 10, z}, 5, 40, 4);
  cut(g, e2, {-1, 5, 5}, 5, 8, 30);  // the intake opening, beside where the blower sits
  const std::string plate = box(g, {6, 14, 4}, 2, 32, 32, "Grille");
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) round_hole(g, plate, {5, 22 + 8 * i, 12 + 8 * j}, {1, 0, 0}, 3, 4);
  const std::string b2 = box(g, {10, 15, 5}, 20, 30, 30, "Blower");
  load["inlet"] = face_along(g, b2, {-1, 0, 0});
  load["outlet"] = face_along(g, b2, {0, 0, 1});
  r = sim::vent_estimate(g, resolve(g), {e2}, {load}, 3);
  if (std::getenv("OPAD_TEST_VERBOSE")) std::printf("%s\n", r.dump().substr(0, 1200).c_str());
  const json& f2 = r["fans"][0];
  CHECK_EQ(f2["intake"]["through"].get<std::string>(), "holes");
  CHECK_EQ(f2["intake"]["part"].get<std::string>(), "Grille");
  CHECK_EQ(f2["intake"]["openings"].get<int>(), 9);
  CHECK_EQ(r["intake"]["openings"].get<int>(), 1);  // the box's own intake, feeding the box's air the grille draws from
}
