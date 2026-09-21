// Design engine: expressions, parameters, sketches -> profiles, features, regeneration, history edits.
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <filesystem>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"

using namespace opad;
using namespace opad::design;

namespace {

double volume_of_node(const Document& doc, const Scene& s, const std::string& node) {
  GProp_GProps g;
  BRepGProp::VolumeProperties(node_world_shape(doc, s, node), g);
  return g.Mass();
}

double total_volume(const Document& doc) {
  const Scene s = resolve(doc);
  double v = 0;
  for (const auto& b : s.all_bodies()) v += volume_of_node(doc, s, b);
  return v;
}

// w x h rectangle with its lower-left corner at (x, y), fully constrained, sizes as expressions.
Sketch rectangle(double x, double y, double w, double h, const std::string& wexpr = {}, const std::string& hexpr = {}) {
  Sketch sk;
  const int a = sk.add_point(x, y), b = sk.add_point(x + w, y), c = sk.add_point(x + w, y + h), d = sk.add_point(x, y + h);
  const int l0 = sk.add_line(a, b), l1 = sk.add_line(b, c), l2 = sk.add_line(c, d), l3 = sk.add_line(d, a);
  sk.add_constraint(SkConstraint::Type::Horizontal, {l0});
  sk.add_constraint(SkConstraint::Type::Horizontal, {l2});
  sk.add_constraint(SkConstraint::Type::Vertical, {l1});
  sk.add_constraint(SkConstraint::Type::Vertical, {l3});
  sk.add_constraint(SkConstraint::Type::Fix, {a});
  sk.add_constraint(SkConstraint::Type::Distance, {l0}, w, wexpr);
  sk.add_constraint(SkConstraint::Type::Distance, {l1}, h, hexpr);
  return sk;
}

std::string run_id(const json& report) { return report["ids"][0].get<std::string>(); }

json sketch_cmd(Document& doc, const Sketch& sk, const json& plane = {{"base", "xy"}}) {
  return commands::run("sketch", {{"plane", plane}, {"geometry", sk.to_json()}}, &doc);
}

json feature_cmd(Document& doc, const std::string& kind, const json& inputs) { return commands::run("feature", {{"kind", kind}, {"inputs", inputs}}, &doc); }

json body_ref(const std::string& node) { return {{"body", node}, {"kind", "body"}}; }

}  // namespace

TEST(expressions) {
  ParamTable t({{"1", "width", "20 mm", ""}, {"2", "half", "width / 2", ""}, {"3", "n", "4", ""}, {"4", "tilt", "30 deg", ""}, {"5", "area", "width * half", ""}});
  CHECK_NEAR(t.length("width"), 20, 1e-12);
  CHECK_NEAR(t.length("half + 1 cm"), 20, 1e-12);
  CHECK_NEAR(t.length("2 in"), 50.8, 1e-12);
  CHECK_NEAR(t.length("5"), 5, 1e-12);  // a plain number is mm
  CHECK_NEAR(t.length("width + 5"), 25, 1e-12);
  CHECK_NEAR(t.angle("tilt * 2"), M_PI / 3, 1e-12);
  CHECK_NEAR(t.angle("45"), M_PI / 4, 1e-12);  // a plain number is degrees
  CHECK_NEAR(t.number("n * 2 + 1"), 9, 1e-12);
  CHECK_EQ(t.count("n - 1"), 3);
  CHECK_NEAR(t.length("sqrt(area)"), std::sqrt(200.0), 1e-9);
  CHECK_NEAR(t.length("width * sin(tilt)"), 10, 1e-9);
  CHECK_NEAR(t.length("max(width, 3 cm, half)"), 30, 1e-12);
  CHECK_NEAR(t.number("width / half"), 2, 1e-12);
  CHECK_NEAR(t.length("-(width - 2 * half) + 1e1 mm"), 10, 1e-12);
  CHECK_NEAR(t.angle("atan2(1, 1)"), M_PI / 4, 1e-12);
  CHECK_NEAR(t.number("2 ^ 3 ^ 2"), 512, 1e-9);
  CHECK_THROWS(t.length("tilt"));          // an angle is not a length
  CHECK_THROWS(t.length("area"));          // nor is an area
  CHECK_THROWS(t.number("width"));
  CHECK_THROWS(t.length("width + tilt"));
  CHECK_THROWS(t.length("nope * 2"));
  CHECK_THROWS(t.length("width / 0"));
  CHECK_THROWS(t.length("(width"));
  CHECK_THROWS(t.length(""));
  CHECK_THROWS(t.count("2.5"));
  ParamTable cyc({{"1", "a", "b + 1", ""}, {"2", "b", "a * 2", ""}});
  CHECK_THROWS(cyc.value_of("a"));
  CHECK(valid_param_name("wall_2"));
  CHECK(!valid_param_name("2wall"));
  CHECK(!valid_param_name("mm"));
  CHECK(!valid_param_name("sin"));
  CHECK(!valid_param_name("a b"));
  CHECK_EQ(expr_identifiers("width / 2 + sin(tilt) * 3 mm + PI").size(), size_t(2));
  CHECK_EQ(expr_rename("width/2 + widthx + width", "width", "w"), std::string("w/2 + widthx + w"));
  CHECK_EQ(format_quantity(t.eval("half")), std::string("10 mm"));
  CHECK_EQ(format_quantity(t.eval("tilt")), std::string("30 deg"));
}

TEST(sketch_regions) {
  // A rectangle with a circle inside and a second circle crossing its right side.
  Sketch sk = rectangle(0, 0, 40, 20);
  sk.add_circle(sk.add_point(10, 10), 4);
  sk.add_circle(sk.add_point(40, 10), 5);
  const Frame f = base_frame("xy");
  const auto regions = sketch_regions(sk, f);
  // ring part of the rectangle, inner disc, the crossing circle's inside half and its outside half
  CHECK_EQ(regions.size(), size_t(4));
  double area = 0;
  for (const auto& r : regions) area += r.area;
  CHECK_NEAR(area, 40 * 20 + M_PI * 25 / 2, 1e-6);
  const int disc = region_at(regions, f, 10, 10);
  CHECK(disc >= 0);
  CHECK_NEAR(regions[static_cast<size_t>(disc)].area, M_PI * 16, 1e-6);
  CHECK_EQ(region_at(regions, f, -5, -5), -1);
  for (const auto& r : regions) CHECK_EQ(region_at(regions, f, r.u, r.v), static_cast<int>(&r - &regions[0]));
  // Construction geometry never bounds a profile.
  Sketch guide;
  guide.add_circle(guide.add_point(0, 0), 5, true);
  CHECK(sketch_regions(guide, f).empty());
}

TEST(primitives_and_operations) {
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"length", "40 mm"}, {"width", "30 mm"}, {"height", "10 mm"}});
  CHECK_NEAR(total_volume(doc), 12000, 1e-6);
  // A cylinder through it, cut: targets are found automatically and written into the op.
  json r = feature_cmd(doc, "cylinder", {{"diameter", "10 mm"}, {"height", "30 mm"}, {"operation", "cut"}});
  CHECK_NEAR(total_volume(doc), 12000 - M_PI * 25 * 10, 1e-6);
  Scene s = resolve(doc);
  CHECK_EQ(s.all_bodies().size(), size_t(1));
  CHECK_EQ(s.feature(run_id(r))->inputs["targets"].size(), size_t(1));
  // Joined boss, then a sphere that touches nothing as "join" still makes a body.
  feature_cmd(doc, "cylinder", {{"x", "12 mm"}, {"diameter", "6 mm"}, {"height", "15 mm"}, {"operation", "join"}});
  CHECK_NEAR(total_volume(doc), 12000 - M_PI * 25 * 10 + M_PI * 9 * 5, 1e-6);
  feature_cmd(doc, "sphere", {{"x", "200 mm"}, {"diameter", "10 mm"}, {"operation", "join"}});
  CHECK_EQ(resolve(doc).all_bodies().size(), size_t(2));
  CHECK_THROWS(feature_cmd(doc, "sphere", {{"x", "500 mm"}, {"diameter", "10 mm"}, {"operation", "cut"}}));  // cuts nothing: refused
  CHECK_THROWS(feature_cmd(doc, "box", {{"length", "-1 mm"}}));
  CHECK_THROWS(feature_cmd(doc, "nonsense", json::object()));
  feature_cmd(doc, "cone", {{"x", "-100 mm"}, {"diameter", "20 mm"}, {"top_diameter", "10 mm"}, {"height", "12 mm"}});
  feature_cmd(doc, "torus", {{"x", "-200 mm"}});
  CHECK_EQ(resolve(doc).all_bodies().size(), size_t(4));
  CHECK(resolve(doc).unresolved.empty());
}

TEST(parametric_extrude_and_regeneration) {
  Document doc = Document::create();
  commands::run("param", {{"name", "width"}, {"expr", "40 mm"}}, &doc);
  commands::run("param", {{"name", "depth"}, {"expr", "width / 2"}}, &doc);
  commands::run("param", {{"name", "thick"}, {"expr", "5 mm"}, {"comment", "plate thickness"}}, &doc);
  const std::string sketch = run_id(sketch_cmd(doc, rectangle(0, 0, 40, 20, "width", "depth")));
  const std::string extrude = run_id(feature_cmd(doc, "extrude", {{"profiles", json::array({json{{"sketch", sketch}, {"at", {5, 5}}}})}, {"distance", "thick"}}));
  CHECK_NEAR(total_volume(doc), 40 * 20 * 5, 1e-6);
  Scene s = resolve(doc);
  CHECK_EQ(s.sketches.size(), size_t(1));
  CHECK_EQ(s.sketches[0].dof, 0);
  CHECK(!s.sketches[0].visible);  // consumed by the extrude
  const std::string body = s.all_bodies().front();
  const std::string key_before = s.node(body)->body_key;

  // One parameter change regenerates the sketch and the extrude; the body node stays the same.
  const size_t ops_before = doc.ops.size();
  json rep = commands::run("param", {{"name", "width"}, {"expr", "60 mm"}}, &doc);
  CHECK_EQ(doc.ops.size(), ops_before + 2);  // the edit and one regen
  CHECK_EQ(rep["regenerated"].size(), size_t(2));
  CHECK_NEAR(total_volume(doc), 60 * 30 * 5, 1e-6);
  s = resolve(doc);
  CHECK_EQ(s.all_bodies().size(), size_t(1));
  CHECK_EQ(s.all_bodies().front(), body);
  CHECK(s.node(body)->body_key != key_before);
  CHECK_NEAR(s.param("depth")->value, 30, 1e-12);

  // Nothing out of date afterwards; regenerate is a no-op and appends nothing.
  const size_t ops_now = doc.ops.size();
  commands::run("regenerate", json::object(), &doc);
  CHECK_EQ(doc.ops.size(), ops_now);

  // Edit the feature: symmetric, other distance.
  commands::run("feature_edit", {{"target", extrude}, {"inputs", {{"direction", "symmetric"}, {"distance", "thick * 2"}}}}, &doc);
  CHECK_NEAR(total_volume(doc), 60 * 30 * 10, 1e-6);
  // A bad edit is refused and leaves the document alone.
  const size_t ops_ok = doc.ops.size();
  CHECK_THROWS(commands::run("feature_edit", {{"target", extrude}, {"inputs", {{"distance", "nope"}}}}, &doc));
  CHECK_THROWS(commands::run("param", {{"name", "thick"}, {"expr", "width +"}}, &doc));
  CHECK_THROWS(commands::run("param", {{"name", "sin"}, {"expr", "1"}}, &doc));
  CHECK_EQ(doc.ops.size(), ops_ok);

  // Rename follows into every expression; a used parameter cannot be deleted.
  commands::run("param", {{"name", "thick"}, {"rename", "t"}}, &doc);
  s = resolve(doc);
  CHECK(s.param("t") && !s.param("thick"));
  CHECK_EQ(s.feature(extrude)->inputs["distance"].get<std::string>(), std::string("t * 2"));
  CHECK_NEAR(total_volume(doc), 60 * 30 * 10, 1e-6);
  CHECK_THROWS(commands::run("param_delete", {{"name", "t"}}, &doc));
  commands::run("param", {{"name", "spare"}, {"expr", "1"}}, &doc);
  commands::run("param_delete", {{"name", "spare"}}, &doc);
  CHECK(!resolve(doc).param("spare"));

  // Survives the file format.
  const auto path = std::filesystem::temp_directory_path() / "opad_design_test.opad";
  doc.save_as(path);
  Document again = Document::load(path);
  CHECK_NEAR(total_volume(again), 60 * 30 * 10, 1e-6);
  CHECK(resolve(again).unresolved.empty());
  commands::run("regenerate", json::object(), &again);
  CHECK(!again.dirty);
  // gc keeps what the history needs and nothing else.
  const size_t before_gc = again.body_count();
  again.gc();
  CHECK(again.body_count() < before_gc);  // the superseded 40 mm results go
  CHECK_NEAR(total_volume(again), 60 * 30 * 10, 1e-6);
  commands::run("param", {{"name", "width"}, {"expr", "50 mm"}}, &again);
  CHECK_NEAR(total_volume(again), 50 * 25 * 10, 1e-6);
  std::filesystem::remove(path);
}

TEST(modify_features_follow_history) {
  Document doc = Document::create();
  commands::run("param", {{"name", "h"}, {"expr", "20 mm"}}, &doc);
  feature_cmd(doc, "box", {{"length", "40 mm"}, {"width", "30 mm"}, {"height", "h"}, {"centered", false}});
  Scene s = resolve(doc);
  const std::string body = s.all_bodies().front();
  // The top face: the planar face whose centre is highest.
  int top = -1, n_faces = subshape_count(node_world_shape(doc, s, body), Ref::Kind::Face);
  double best = -1e9;
  for (int i = 0; i < n_faces; ++i) {
    Ref r;
    r.body = body;
    r.kind = Ref::Kind::Face;
    r.index = i;
    const json info = inspect_ref(doc, s, r);
    if (info.contains("center") && info["center"][2].get<double>() > best) { best = info["center"][2].get<double>(); top = i; }
  }
  Ref top_ref;
  top_ref.body = body;
  top_ref.kind = Ref::Kind::Face;
  top_ref.index = top;
  const std::string shell = run_id(feature_cmd(doc, "shell", {{"faces", json::array({make_ref(doc, s, top_ref)})}, {"thickness", "2 mm"}}));
  CHECK_NEAR(total_volume(doc), 40 * 30 * 20 - 36 * 26 * 18, 1e-6);

  // Fillet the four vertical outer edges (those parallel to Z of length h at the box corners).
  s = resolve(doc);
  json edges = json::array();
  const int n_edges = subshape_count(node_world_shape(doc, s, body), Ref::Kind::Edge);
  for (int i = 0; i < n_edges; ++i) {
    Ref r;
    r.body = body;
    r.kind = Ref::Kind::Edge;
    r.index = i;
    const json info = inspect_ref(doc, s, r);
    if (!info.contains("length") || std::fabs(info["length"].get<double>() - 20) > 1e-9) continue;
    const json c = info.contains("center") ? info["center"] : info["bbox"]["center"];
    const double x = c[0].get<double>(), y = c[1].get<double>();
    if ((std::fabs(x) < 1e-9 || std::fabs(x - 40) < 1e-9) && (std::fabs(y) < 1e-9 || std::fabs(y - 30) < 1e-9)) edges.push_back(make_ref(doc, s, r));
  }
  CHECK_EQ(edges.size(), size_t(4));
  feature_cmd(doc, "fillet", {{"edges", edges}, {"radius", "3 mm"}});
  const double filleted = total_volume(doc);
  CHECK(filleted < 40 * 30 * 20 - 36 * 26 * 18);
  CHECK_THROWS(feature_cmd(doc, "fillet", {{"edges", edges}, {"radius", "300 mm"}}));

  // Taller box: shell and fillet are recomputed on the new body, and their references still hold.
  commands::run("param", {{"name", "h"}, {"expr", "30 mm"}}, &doc);
  CHECK(resolve(doc).unresolved.empty());
  CHECK(total_volume(doc) > filleted);

  // Suppress the shell: solid again (minus the fillets).
  commands::run("feature_edit", {{"target", shell}, {"suppressed", true}}, &doc);
  const double solid = total_volume(doc);
  CHECK(solid > 40 * 30 * 30 - 4 * 9 * 30 && solid < 40 * 30 * 30);
  commands::run("feature_edit", {{"target", shell}, {"suppressed", false}}, &doc);
  // Tombstone it instead, then restore by tombstoning the tombstone.
  const json del = commands::run("delete", {{"target", shell}}, &doc);
  CHECK_NEAR(total_volume(doc), solid, 1e-6);
  commands::run("delete", {{"target", del["id"]}}, &doc);
  CHECK(total_volume(doc) < solid - 1000);

  // Roll back: the state before the shell is the plain box.
  const Scene before = resolve(doc, shell);
  CHECK_NEAR(volume_of_node(doc, before, body), 40 * 30 * 30, 1e-6);

  // Chamfer, press-pull and draft do something sane.
  s = resolve(doc);
  feature_cmd(doc, "offset_face", {{"faces", json::array({[&] {
                                      Ref r;
                                      r.body = body;
                                      r.kind = Ref::Kind::Face;
                                      for (int i = 0; i < subshape_count(node_world_shape(doc, s, body), Ref::Kind::Face); ++i) {
                                        r.index = i;
                                        const json info = inspect_ref(doc, s, r);
                                        if (info.contains("normal") && info["normal"][2].get<double>() < -0.99) break;
                                      }
                                      return make_ref(doc, s, r);
                                    }()})},
                                   {"distance", "5 mm"}});
  CHECK(resolve(doc).unresolved.empty());
}

TEST(revolve_hole_pattern_combine_split) {
  Document doc = Document::create();
  // Revolve a rectangle standing off the Y axis of the XZ plane: a ring.
  const std::string sk = run_id(sketch_cmd(doc, rectangle(10, 0, 5, 8), {{"base", "xz"}}));
  feature_cmd(doc, "revolve", {{"profiles", json::array({json{{"sketch", sk}, {"at", {12, 4}}}})}, {"axis", {{"base", "z"}}}});
  CHECK_NEAR(total_volume(doc), M_PI * (15 * 15 - 10 * 10) * 8, 1e-5);
  feature_cmd(doc, "revolve", {{"profiles", json::array({json{{"sketch", sk}, {"all", true}}})}, {"axis", {{"base", "z"}}}, {"angle", "90 deg"}, {"operation", "new"}});
  CHECK_EQ(resolve(doc).all_bodies().size(), size_t(2));

  // A plate with holes at sketch points.
  Document plate = Document::create();
  feature_cmd(plate, "box", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "-8 mm"}});  // hangs below XY
  Sketch pts;
  for (double x : {-20.0, 0.0, 20.0}) {
    SkEntity e;
    e.id = pts.next_id() + 1;
    e.type = SkEntity::Type::Point;
    e.p = {pts.add_point(x, 0)};
    e.id = pts.next_id();
    pts.entities.push_back(e);
  }
  const std::string ps = run_id(sketch_cmd(plate, pts));
  json picks = json::array();
  for (const auto& p : pts.points) picks.push_back({{"sketch", ps}, {"point", p.id}});
  feature_cmd(plate, "hole", {{"points", picks}, {"diameter", "6 mm"}, {"extent", "all"}});
  CHECK_NEAR(total_volume(plate), 60 * 40 * 8 - 3 * M_PI * 9 * 8, 1e-5);
  feature_cmd(plate, "hole", {{"points", json::array({picks[1]})}, {"type", "counterbore"}, {"diameter", "6 mm"}, {"cb_diameter", "12 mm"}, {"cb_depth", "3 mm"}, {"extent", "all"}});
  CHECK_NEAR(total_volume(plate), 60 * 40 * 8 - 3 * M_PI * 9 * 8 - M_PI * (36 - 9) * 3, 1e-5);

  // Pattern a peg and join the copies to the plate; then split the plate and combine the halves again.
  Scene s = resolve(plate);
  const std::string plate_body = s.all_bodies().front();
  feature_cmd(plate, "cylinder", {{"x", "-25 mm"}, {"y", "15 mm"}, {"diameter", "4 mm"}, {"height", "5 mm"}});
  s = resolve(plate);
  std::string peg;
  for (const auto& b : s.all_bodies())
    if (b != plate_body) peg = b;
  feature_cmd(plate, "pattern_rect", {{"bodies", json::array({body_ref(peg)})}, {"count", "6"}, {"spacing", "10 mm"}, {"operation", "join"}});
  s = resolve(plate);
  CHECK_EQ(s.all_bodies().size(), size_t(2));  // the pegs joined the plate; the seed peg is still its own body
  feature_cmd(plate, "combine", {{"target", json::array({body_ref(plate_body)})}, {"tools", json::array({body_ref(peg)})}});
  CHECK_EQ(resolve(plate).all_bodies().size(), size_t(1));
  const double whole = total_volume(plate);
  feature_cmd(plate, "split", {{"bodies", json::array({body_ref(plate_body)})}, {"plane", {{"base", "yz"}}}});
  s = resolve(plate);
  CHECK_EQ(s.all_bodies().size(), size_t(2));
  CHECK_NEAR(total_volume(plate), whole, 1e-5);
  // Mirror, circular pattern, move/copy, scale, remove.
  feature_cmd(plate, "mirror", {{"bodies", json::array({body_ref(plate_body)})}, {"plane", {{"base", "xz"}}}, {"operation", "new"}});
  feature_cmd(plate, "move", {{"bodies", json::array({body_ref(plate_body)})}, {"dz", "50 mm"}, {"copy", true}});
  const size_t n = resolve(plate).all_bodies().size();
  CHECK(n >= 4);
  feature_cmd(plate, "remove", {{"bodies", json::array({body_ref(plate_body)})}});
  CHECK_EQ(resolve(plate).all_bodies().size(), n - 1);
  CHECK(resolve(plate).unresolved.empty());
}

TEST(construction_sweep_loft_pipe) {
  Document doc = Document::create();
  const std::string plane = run_id(feature_cmd(doc, "plane", {{"mode", "offset"}, {"plane", {{"base", "xy"}}}, {"distance", "30 mm"}}));
  Scene s = resolve(doc);
  CHECK_NEAR(Frame::from_json(s.feature(plane)->result["plane"]).origin[2], 30, 1e-12);
  const std::string low = run_id(sketch_cmd(doc, rectangle(-10, -10, 20, 20)));
  Sketch small;
  small.add_circle(small.add_point(0, 0), 4);
  const std::string high = run_id(sketch_cmd(doc, small, {{"feature", plane}}));
  CHECK_NEAR(resolve(doc).sketch(high)->frame.origin[2], 30, 1e-12);
  feature_cmd(doc, "loft", {{"profiles", json::array({json{{"sketch", low}, {"all", true}}, json{{"sketch", high}, {"all", true}}})}});
  const double v = total_volume(doc);
  CHECK(v > M_PI * 16 * 30 && v < 400 * 30);

  // An L-shaped path on XZ, a pipe along it.
  Sketch path;
  const int a = path.add_point(100, 0), b = path.add_point(100, 40), c = path.add_point(140, 40);
  path.add_line(a, b);
  path.add_line(b, c);
  const std::string ph = run_id(sketch_cmd(doc, path, {{"base", "xz"}}));
  feature_cmd(doc, "pipe", {{"path", {{"sketch", ph}}}, {"diameter", "6 mm"}, {"hollow", true}, {"thickness", "1 mm"}});
  CHECK_EQ(resolve(doc).all_bodies().size(), size_t(2));
  // Sweep a small square along the same path.
  const std::string prof = run_id(sketch_cmd(doc, rectangle(98, -2, 4, 4)));
  feature_cmd(doc, "sweep", {{"profiles", json::array({json{{"sketch", prof}, {"all", true}}})}, {"path", {{"sketch", ph}}}});
  CHECK_EQ(resolve(doc).all_bodies().size(), size_t(3));
  // Moving the construction plane regenerates the sketch on it and the loft.
  commands::run("feature_edit", {{"target", plane}, {"inputs", {{"distance", "60 mm"}}}}, &doc);
  CHECK_NEAR(resolve(doc).sketch(high)->frame.origin[2], 60, 1e-12);
  CHECK(total_volume(doc) > v);
  CHECK(resolve(doc).unresolved.empty());
}

CHECK_MAIN()
