// Joints, relations and the kinematic solver against textbook mechanisms (sim/kinematics.hpp): the slider-crank's piston
// travel, Freudenstein's four-bar, gear, rack and lead-screw ratios, degrees of freedom, snapping, limits and locks, and
// what a pose leaves in the file.
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/diff.hpp"
#include "opad/sim/kinematics.hpp"

using namespace opad;

namespace {

constexpr double kPi = 3.14159265358979323846;
double rad(double deg) { return deg * kPi / 180; }

std::string box(Document& doc, Vec3 at, double l, double w, double h, const std::string& name) {
  const json r = commands::run("feature", {{"kind", "box"}, {"name", name},
                                           {"inputs", {{"plane", {{"origin", at}, {"normal", {0, 0, 1}}}}, {"length", l}, {"width", w}, {"height", h}}}},
                               &doc);
  return r["body_ids"][0];
}

json joint(Document& doc, json args) { return commands::run("joint", args, &doc); }
json set(Document& doc, const std::string& j, double v) { return commands::run("joint_set", {{"values", {{j, v}}}}, &doc); }

double value(Document& doc, const std::string& j, size_t c = 0) {
  const Scene s = resolve(doc);
  return s.joint(j)->values.at(c);
}

json axis_z(Vec3 o) { return {{"origin", o}, {"z", {0, 0, 1}}}; }

std::string error_of(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

// A slider-crank in the XY plane: crank r = 40 about the origin, rod l = 120, piston sliding along X.
struct SliderCrank {
  Document doc = Document::create();
  std::string crank, rod, piston, j1, j2, j3, j4;
  SliderCrank() {
    crank = box(doc, {20, 0, 0}, 50, 10, 5, "Crank");
    rod = box(doc, {100, 0, 5}, 130, 8, 5, "Rod");
    piston = box(doc, {160, 0, 10}, 20, 20, 20, "Piston");
    j1 = joint(doc, {{"kind", "revolute"}, {"name", "Crank pin"}, {"part", crank}, {"at", axis_z({0, 0, 0})}})["id"];
    j2 = joint(doc, {{"kind", "revolute"}, {"name", "Big end"}, {"base", crank}, {"part", rod}, {"at", axis_z({40, 0, 0})}})["id"];
    j3 = joint(doc, {{"kind", "revolute"}, {"name", "Small end"}, {"base", rod}, {"part", piston}, {"at", axis_z({160, 0, 0})}})["id"];
    j4 = joint(doc, {{"kind", "slider"}, {"name", "Cylinder"}, {"part", piston}, {"at", {{"origin", {160, 0, 0}}, {"z", {1, 0, 0}}}}})["id"];
  }
};

double piston_x(double theta_deg, double r = 40, double l = 120) {
  const double t = rad(theta_deg);
  return r * std::cos(t) + std::sqrt(l * l - r * r * std::sin(t) * std::sin(t));
}

}  // namespace

TEST(slider_crank_follows_the_textbook) {
  SliderCrank m;
  const json a = commands::run("mechanism", json::object(), &m.doc);
  CHECK_EQ(a["dof"].get<int>(), 1);
  CHECK(a["redundant"].get<int>() >= 1);  // a planar loop of spatial joints repeats its out-of-plane equations
  for (double theta : {30.0, 90.0, 135.0, 180.0, 270.0, 359.0, 720.0}) {
    set(m.doc, m.j1, theta);
    CHECK_NEAR(value(m.doc, m.j1), theta, 1e-6);
    // The slider's coordinate is the piston's travel from where it was made (at r + l = 160).
    CHECK_NEAR(160 + value(m.doc, m.j4), piston_x(theta), 1e-6);
  }
  // The crank went round twice: its value keeps counting turns.
  CHECK_NEAR(value(m.doc, m.j1), 720, 1e-6);
  // The piston body itself moved along X only.
  const Scene s = resolve(m.doc);
  const Mat4 w = s.world(m.piston);
  CHECK_NEAR(w.at(1, 3), 0, 1e-6);
  CHECK_NEAR(w.at(0, 3), 0, 1e-6);  // back at its start after two full turns
}

TEST(four_bar_matches_freudenstein) {
  // Ground pivots O2 = (0, 0) and O4 = (d, 0); crank a, coupler b, rocker c (a Grashof crank-rocker).
  const double a = 40, b = 120, c = 80, d = 100;
  auto rocker_angle = [&](double t2) {  // the open branch: B is the upper intersection of the coupler and rocker circles
    const double ax = a * std::cos(t2), ay = a * std::sin(t2);
    const double dx = d - ax, dy = -ay, L = std::hypot(dx, dy);
    const double along = (b * b - c * c + L * L) / (2 * L), h = std::sqrt(b * b - along * along);
    const double px = ax + along * dx / L, py = ay + along * dy / L;
    const double bx = px - h * dy / L, by = py + h * dx / L;
    return std::atan2(by, bx - d);
  };
  Document doc = Document::create();
  const double t0 = rad(60);
  const double ax = a * std::cos(t0), ay = a * std::sin(t0);
  const double t4 = rocker_angle(t0);
  const double bx = d + c * std::cos(t4), by = c * std::sin(t4);
  const std::string crank = box(doc, {ax / 2, ay / 2, 0}, 6, 6, 4, "Crank");
  const std::string coupler = box(doc, {(ax + bx) / 2, (ay + by) / 2, 4}, 6, 6, 4, "Coupler");
  const std::string rocker = box(doc, {(bx + d) / 2, by / 2, 8}, 6, 6, 4, "Rocker");
  const std::string j1 = joint(doc, {{"kind", "revolute"}, {"part", crank}, {"at", axis_z({0, 0, 0})}})["id"];
  joint(doc, {{"kind", "revolute"}, {"base", crank}, {"part", coupler}, {"at", axis_z({ax, ay, 0})}});
  joint(doc, {{"kind", "revolute"}, {"base", coupler}, {"part", rocker}, {"at", axis_z({bx, by, 0})}});
  const std::string j4 = joint(doc, {{"kind", "revolute"}, {"part", rocker}, {"at", axis_z({d, 0, 0})}})["id"];
  CHECK_EQ(commands::run("mechanism", json::object(), &doc)["dof"].get<int>(), 1);
  // Freudenstein: K1 cos t4 - K2 cos t2 + K3 = cos(t2 - t4) holds for every crank angle on the path.
  const double K1 = d / a, K2 = d / c, K3 = (a * a - b * b + c * c + d * d) / (2 * a * c);
  for (double deg = 60; deg <= 60 + 360; deg += 15) {
    set(doc, j1, deg - 60);
    const double t2 = rad(deg);
    const double rocker_now = t4 + rad(value(doc, j4));
    CHECK_NEAR(rocker_now, rocker_angle(t2) + 2 * kPi * std::round((rocker_now - rocker_angle(t2)) / (2 * kPi)), 1e-7);
    CHECK_NEAR(K1 * std::cos(rocker_now) - K2 * std::cos(t2) + K3, std::cos(t2 - rocker_now), 1e-7);
  }
}

TEST(gear_rack_and_lead_screw_ratios) {
  Document doc = Document::create();
  const std::string g1 = box(doc, {0, 0, 0}, 10, 10, 5, "Gear 20");
  const std::string g2 = box(doc, {30, 0, 0}, 10, 10, 5, "Gear 40");
  const std::string rack = box(doc, {0, -20, 0}, 60, 5, 5, "Rack");
  const std::string screw = box(doc, {0, 50, 0}, 5, 5, 40, "Screw");
  const std::string nut = box(doc, {0, 50, 10}, 12, 12, 6, "Nut");
  const std::string r1 = joint(doc, {{"kind", "revolute"}, {"part", g1}, {"at", axis_z({0, 0, 0})}})["id"];
  const std::string r2 = joint(doc, {{"kind", "revolute"}, {"part", g2}, {"at", axis_z({30, 0, 0})}})["id"];
  const std::string sl = joint(doc, {{"kind", "slider"}, {"part", rack}, {"at", {{"origin", {0, -20, 0}}, {"z", {1, 0, 0}}}}})["id"];
  const std::string rs = joint(doc, {{"kind", "revolute"}, {"part", screw}, {"at", axis_z({0, 50, 0})}})["id"];
  const std::string ns = joint(doc, {{"kind", "slider"}, {"part", nut}, {"at", axis_z({0, 50, 10})}})["id"];
  const json gear = joint(doc, {{"kind", "gear"}, {"joints", {r1, r2}}, {"teeth", {20, 40}}});
  CHECK_NEAR(resolve(doc).joint(gear["id"])->def["ratio"].get<double>(), -0.5, 1e-12);
  joint(doc, {{"kind", "rack_pinion"}, {"joints", {r1, sl}}, {"radius", 10}});
  joint(doc, {{"kind", "lead_screw"}, {"joints", {rs, ns}}, {"lead", 2}});
  CHECK_EQ(commands::run("mechanism", json::object(), &doc)["dof"].get<int>(), 2);  // the gear set and the screw
  set(doc, r1, 90);
  CHECK_NEAR(value(doc, r2), -45, 1e-6);
  CHECK_NEAR(value(doc, sl), 2 * kPi * 10 / 4, 1e-6);  // a quarter turn of a 10 mm pitch radius
  set(doc, r2, 180);  // driving the follower drives the leader back through the gear
  CHECK_NEAR(value(doc, r1), -360, 1e-6);
  CHECK_NEAR(value(doc, sl), -2 * kPi * 10, 1e-6);
  set(doc, rs, 720);
  CHECK_NEAR(value(doc, ns), 4, 1e-6);
  // The screw joint does the same on one joint: 1.5 mm per turn.
  Document d2 = Document::create();
  const std::string bolt = box(d2, {0, 0, 0}, 8, 8, 40, "Bolt");
  const std::string n2 = box(d2, {0, 0, 10}, 13, 13, 8, "Nut");
  const json sj = joint(d2, {{"kind", "screw"}, {"base", bolt}, {"part", n2}, {"at", axis_z({0, 0, 10})}, {"pitch", 1.5}});
  joint(d2, {{"kind", "ground"}, {"part", bolt}, {"at", axis_z({0, 0, 0})}});
  set(d2, sj["id"], 3600);
  const Scene s2 = resolve(d2);
  CHECK_NEAR(s2.world(n2).at(2, 3), 15, 1e-6);  // ten turns: 15 mm up
  CHECK_NEAR(s2.world(bolt).at(2, 3), 0, 1e-12);
}

TEST(snap_puts_a_knob_in_its_hole) {
  Document doc = Document::create();
  // A 60 x 40 x 5 panel with a 6 mm hole at (10, 5), and a knob (a 6 mm shaft under a 20 mm cap) made somewhere else.
  const std::string panel = box(doc, {0, 0, 0}, 60, 40, 5, "Panel");
  commands::run("feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"origin", {10, 5, -1}}, {"normal", {0, 0, 1}}}}, {"diameter", 6}, {"height", 7}, {"operation", "cut"}, {"targets", {panel}}}}}, &doc);
  const json shaft = commands::run("feature", {{"kind", "cylinder"}, {"name", "Knob"}, {"inputs", {{"plane", {{"origin", {100, 100, 30}}, {"normal", {0, 0, 1}}}}, {"diameter", 6}, {"height", 12}}}}, &doc);
  const std::string knob = shaft["body_ids"][0];
  commands::run("feature", {{"kind", "cylinder"}, {"inputs", {{"plane", {{"origin", {100, 100, 42}}, {"normal", {0, 0, 1}}}}, {"diameter", 20}, {"height", 8}, {"operation", "join"}, {"targets", {knob}}}}}, &doc);
  // The hole's rim on the panel's top face, and the bottom rim of the knob's shaft: query by rule.
  auto edge_at = [&](const std::string& body, double z, double r) {
    return json{{"body", body}, {"kind", "edge"}, {"select", {{"curve", "circle"}, {"at_plane", {{"axis", "z"}, {"value", z}}}, {"radius_min", r - 0.01}, {"radius_max", r + 0.01}}}};
  };
  joint(doc, {{"kind", "ground"}, {"part", panel}, {"at", axis_z({0, 0, 0})}});
  const json j = joint(doc, {{"kind", "revolute"}, {"name", "Knob turn"}, {"at", edge_at(panel, 5, 3)}, {"at_part", edge_at(knob, 30, 3)}, {"offset", -5}});
  CHECK_EQ(j["base"].get<std::string>(), panel);  // the base is what `at` is on
  CHECK_EQ(j["part"].get<std::string>(), knob);
  CHECK_EQ(j["moved"].size(), 1u);
  // The shaft's bottom rim went 5 mm into the hole: the knob's bottom now at z = 0, centred on (10, 5), still upright.
  const Scene s = resolve(doc);
  const Mat4 w = s.world(knob);
  const Vec3 bottom = w.apply({100, 100, 30});
  CHECK_NEAR(bottom[0], 10, 1e-6);
  CHECK_NEAR(bottom[1], 5, 1e-6);
  CHECK_NEAR(bottom[2], 0, 1e-6);
  CHECK_NEAR(w.at(2, 2), 1, 1e-9);
  // It turns about its axis and nothing else.
  set(doc, j["id"], 90);
  const Mat4 w2 = resolve(doc).world(knob);
  const Vec3 b2 = w2.apply({100, 100, 30});
  CHECK_NEAR(b2[0], 10, 1e-6);
  CHECK_NEAR(b2[1], 5, 1e-6);
  CHECK_NEAR(w2.at(2, 2), 1, 1e-9);
  CHECK_NEAR(std::fabs(w2.at(0, 0)), 0, 1e-9);  // a quarter turn
  // Locked: it cannot be turned; a limit holds a value at its end.
  commands::run("joint", {{"id", j["id"]}, {"locked", true}}, &doc);
  CHECK(error_of([&] { set(doc, j["id"], 10); }).find("locked") != std::string::npos);
  commands::run("joint", {{"id", j["id"]}, {"locked", false}, {"limits", {{"rotation", {0, 120}}}}}, &doc);
  const json held = set(doc, j["id"], 200);
  CHECK_NEAR(value(doc, j["id"]), 120, 1e-6);
  CHECK(held["notes"].dump().find("limit") != std::string::npos);
}

TEST(poses_are_saved_and_diffed) {
  SliderCrank m;
  set(m.doc, m.j1, 45);
  const auto path = std::filesystem::temp_directory_path() / "opad_test_motion.opad";
  m.doc.save_as(path);
  Document back = Document::load(path);
  const Scene s = resolve(back);
  CHECK_NEAR(s.joint(m.j1)->values[0], 45, 1e-9);
  CHECK_NEAR(160 + s.joint(m.j4)->values[0], piston_x(45), 1e-6);
  CHECK_EQ(s.joints.size(), 4u);
  const std::string outline = document_outline(back);
  CHECK(outline.find("## Motion and simulation") != std::string::npos);
  CHECK(outline.find("joint Crank pin (revolute)") != std::string::npos);
  // Undo of the pose (truncating it) brings the old placements back.
  back.truncate_ops(back.ops.size() - 1);
  CHECK_NEAR(resolve(back).joint(m.j1)->values[0], 0, 1e-9);
  std::filesystem::remove(path);
}

TEST(motion_study_traces_the_piston) {
  SliderCrank m;
  const json r = commands::run("study", {{"kind", "motion"}, {"settings", {{"duration", 2}, {"frames", 73}, {"drivers", {{{"joint", m.j1}, {"to", 360}}}},
                                                                     {"traces", {{{"part", m.piston}, {"point", {160, 0, 0}}, {"name", "Piston pin"}}}}}},
                                         {"series", {"Piston pin x", "Crank pin rotation"}}, {"samples", 73}},
                                 &m.doc);
  CHECK_EQ(r["frames"].get<int>(), 73);
  CHECK(r["completed"].get<bool>());
  const json& t = r["t"];
  const json* x = nullptr;
  const json* th = nullptr;
  for (const auto& s : r["series"]) {
    if (s["name"] == "Piston pin x") x = &s["v"];
    if (s["name"] == "Crank pin rotation") th = &s["v"];
  }
  CHECK(x && th);
  for (size_t i = 0; i < t.size(); ++i) CHECK_NEAR((*x)[i].get<double>(), piston_x((*th)[i].get<double>()), 1e-5);
  // Stroke: 2r = 80 mm, top and bottom dead centre.
  const json tr = r["traces"]["Piston pin"];
  CHECK_NEAR(tr["x"][1].get<double>() - tr["x"][0].get<double>(), 80, 1e-5);
  // The study is in the document with its summary.
  CHECK_EQ(resolve(m.doc).studies.size(), 1u);
  // An expression driver: one turn per second, written with t.
  const json e = commands::run("study", {{"kind", "motion"}, {"settings", {{"duration", 0.5}, {"frames", 11}, {"drivers", {{{"joint", m.j1}, {"expr", "360 deg * t"}}}}}}}, &m.doc);
  CHECK_NEAR(e["joints"]["Crank pin"]["rotation"]["max"].get<double>(), 180, 1e-6);
  CHECK_NEAR(e["joints"]["Crank pin"]["rotation"]["max_speed"].get<double>(), 360, 1e-6);
}

TEST(errors_say_what_to_do) {
  SliderCrank m;
  CHECK(error_of([&] { joint(m.doc, {{"kind", "hinge"}, {"part", m.crank}, {"at", axis_z({0, 0, 0})}}); }).find("unknown kind") != std::string::npos);
  CHECK(error_of([&] { joint(m.doc, {{"kind", "revolute"}, {"part", m.crank}}); }).find("give at") != std::string::npos);
  CHECK(error_of([&] { joint(m.doc, {{"kind", "gear"}, {"joints", {m.j1, m.j4}}, {"ratio", 2}}); }).find("rotation") != std::string::npos);
  CHECK(error_of([&] { commands::run("joint_set", {{"values", {{m.j4, 500}}}}, &m.doc); }).find("cannot all be met") != std::string::npos);
  // A refused move leaves the document as it was.
  CHECK_NEAR(value(m.doc, m.j4), 0, 1e-12);
}

CHECK_MAIN()
