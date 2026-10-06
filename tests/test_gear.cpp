// The gear feature (design/gear.cpp): tip, root and pitch sizes of spur, internal and rack gears, and two spur gears placed
// at the sum of their pitch radii turning through a gear relation without their teeth cutting into each other.
#include <BRepAlgoAPI_Common.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/geometry.hpp"

using namespace opad;

namespace {

constexpr double kPi = 3.14159265358979323846;

json gear(Document& doc, json inputs, const std::string& name) {
  return commands::run("feature", {{"kind", "gear"}, {"name", name}, {"inputs", inputs}}, &doc);
}

double volume(const TopoDS_Shape& s) {
  GProp_GProps g;
  BRepGProp::VolumeProperties(s, g);
  return g.Mass();
}

}  // namespace

TEST(spur_internal_and_rack_sizes) {
  Document doc = Document::create();
  const json g = gear(doc, {{"module", 2}, {"teeth", 20}, {"width", 10}, {"bore", 8}}, "Pinion");
  const Scene s = resolve(doc);
  const std::string id = g["body_ids"][0];
  const Bnd_Box box = tight_bbox(node_world_shape(doc, s, id));
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(x1, 22, 1e-3);  // tooth 0 along +X: tip radius m (z + 2) / 2
  CHECK_NEAR(z1 - z0, 10, 1e-6);
  // Area between root and tip: the teeth fill about half the ring between root and tip circles.
  const double v = volume(node_world_shape(doc, s, id));
  const double root = 20 - 2.5, ring = kPi * (22 * 22 - root * root) * 10, core = kPi * (root * root - 16) * 10;
  CHECK(v > core + 0.4 * ring && v < core + 0.6 * ring);
  // Internal: tips at m (z - 2) / 2 inside.
  const json r = gear(doc, {{"type", "internal"}, {"module", 2}, {"teeth", 40}, {"width", 10}, {"rim", 100}}, "Ring");
  const Scene s2 = resolve(doc);
  const Bnd_Box rb = tight_bbox(node_world_shape(doc, s2, r["body_ids"][0]));
  rb.Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(x1 - x0, 100, 1e-3);
  // Rack: 10 teeth of pitch pi m.
  const json k = gear(doc, {{"type", "rack"}, {"module", 2}, {"teeth", 10}, {"width", 10}}, "Rack");
  const Bnd_Box kb = tight_bbox(node_world_shape(doc, resolve(doc), k["body_ids"][0]));
  kb.Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(x1 - x0, 10 * kPi * 2, 1e-3);
  CHECK_NEAR(y1, 2, 1e-6);  // tips one module above the pitch line
}

TEST(meshing_spur_gears_turn_without_interference) {
  Document doc = Document::create();
  // 18 and 30 teeth, module 2: centre distance (18 + 30) = 48 mm. The wheel's phase puts a gap on the line of centres.
  const std::string a = gear(doc, {{"module", 2}, {"teeth", 18}, {"width", 8}}, "Pinion")["body_ids"][0];
  const std::string b = gear(doc, {{"module", 2}, {"teeth", 30}, {"width", 8}, {"x", 48}, {"phase", 180.0 / 30 + 180}}, "Wheel")["body_ids"][0];
  const json ja = commands::run("joint", {{"kind", "revolute"}, {"part", a}, {"at", {{"origin", {0, 0, 0}}, {"z", {0, 0, 1}}}}}, &doc);
  const json jb = commands::run("joint", {{"kind", "revolute"}, {"part", b}, {"at", {{"origin", {48, 0, 0}}, {"z", {0, 0, 1}}}}}, &doc);
  commands::run("joint", {{"kind", "gear"}, {"joints", {ja["id"], jb["id"]}}, {"teeth", {18, 30}}}, &doc);
  double worst = 0;
  for (double turn = 0; turn <= 40; turn += 2.5) {
    commands::run("joint_set", {{"values", {{ja["id"], turn}}}}, &doc);
    const Scene s = resolve(doc);
    const TopoDS_Shape both = BRepAlgoAPI_Common(node_world_shape(doc, s, a), node_world_shape(doc, s, b)).Shape();
    worst = std::max(worst, volume(both));
  }
  CHECK(worst < 1e-3);  // they touch along the line of action, never overlap (mm3)
}

CHECK_MAIN()
