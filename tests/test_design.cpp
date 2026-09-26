#include "opad/design/sketch_edit.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <cstdio>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
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

TEST(document_units_are_undoable_and_do_not_resize_stored_features) {
  ParamTable inches({},"in");CHECK_NEAR(inches.length("2"),50.8,1e-10);CHECK_NEAR(inches.length("2 mm"),2,1e-10);
  CHECK_NEAR(ParamTable().length(inches.explicit_length("2+1")),76.2,1e-10);
  Document doc=Document::create();auto sketch=rectangle(0,0,10,10);
  const auto id=run_id(sketch_cmd(doc,sketch));const auto regions=sketch_regions(sketch,{});
  feature_cmd(doc,"extrude",{{"profiles",json::array({{{"sketch",id},{"at",{5,5}},{"boundary",regions[0].boundary}}})},{"distance","10 mm"},{"operation","new"}});
  const auto count=doc.ops.size();apply_ops(doc,{{{"op","units"},{"length","in"}}});CHECK_EQ(resolve(doc).units,std::string("in"));CHECK_NEAR(total_volume(doc),1000,1e-7);
  auto removed=doc.truncate_ops(count);CHECK_EQ(resolve(doc).units,std::string("mm"));doc.restore_ops(std::move(removed));CHECK_EQ(resolve(doc).units,std::string("in"));
  for(auto& p:sketch.points)p.x+=100;
  apply_ops(doc,{make_edit_op(id,{{"geometry",sketch.to_json()}})});
  CHECK_NEAR(total_volume(doc),1000,1e-6);CHECK(resolve(doc).unresolved.empty());
}

TEST(profile_boundaries_distinguish_overlapping_regions) {
  Sketch sk;sk.add_circle(sk.add_point(0,0),10);sk.add_circle(sk.add_point(10,0),10);
  const auto before=sketch_regions(sk,{});CHECK_EQ(before.size(),size_t(3));
  CHECK(before[0].boundary!=before[1].boundary);CHECK(before[1].boundary!=before[2].boundary);CHECK(before[0].boundary!=before[2].boundary);
  for(auto& p:sk.points){p.x+=100;p.y-=50;}
  const auto after=sketch_regions(sk,{});CHECK_EQ(after.size(),before.size());
  for(size_t i=0;i<after.size();++i)CHECK(before[i].boundary==after[i].boundary);
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

TEST(thin_ring_profile_is_not_lost_between_grid_samples) {
  Sketch sk;sk.add_circle(sk.add_point(0,0),100);sk.add_circle(sk.add_point(0,0),99.999);
  const auto regions=sketch_regions(sk,Frame{});CHECK_EQ(regions.size(),2u);
  for(const auto& r:regions) CHECK_EQ(region_at(regions,Frame{},r.u,r.v),int(&r-regions.data()));
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

TEST(coil_and_thicken) {
  Document doc = Document::create();
  feature_cmd(doc, "coil", {{"diameter", "20 mm"}, {"pitch", "5 mm"}, {"turns", "4"}, {"size", "2 mm"}});
  // Wire length of a helix times the section area.
  const double length = 4 * std::hypot(2 * M_PI * 10, 5.0);
  CHECK_NEAR(total_volume(doc), length * M_PI, length * M_PI * 0.02);
  feature_cmd(doc, "box", {{"x", "100 mm"}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}});
  Scene s = resolve(doc);
  std::string box;
  for (const auto& b : s.all_bodies())
    if (s.node(b)->name.rfind("Body", 0) == 0) box = b;
  Ref top;
  top.body = box;
  top.kind = Ref::Kind::Face;
  for (int i = 0; i < 6; ++i) {
    top.index = i;
    const json info = inspect_ref(doc, s, top);
    if (info.contains("normal") && info["normal"][2].get<double>() > 0.99) break;
  }
  const double before = total_volume(doc);
  feature_cmd(doc, "thicken", {{"faces", json::array({make_ref(doc, s, top)})}, {"thickness", "3 mm"}});
  CHECK_NEAR(total_volume(doc) - before, 20 * 20 * 3, 1e-4);
  CHECK(resolve(doc).unresolved.empty());
}

CHECK_MAIN()

TEST(spline_nodes_insert_without_shape_change_and_weights_persist) {
  Sketch sk;std::vector<int> nodes={sk.add_point(0,0),sk.add_point(10,8),sk.add_point(20,0)};
  const int id=add_cubic_spline(sk,nodes);sk.validate();
  BRepAdaptor_Curve before(entity_edge(sk,*sk.entity(id),Frame{}));std::vector<gp_Pnt> samples;
  for(int i=0;i<=40;++i)samples.push_back(before.Value(i/20.0));
  const int inserted=insert_spline_node(sk,id,5,4);CHECK(sk.point(inserted));sk.validate();
  BRepAdaptor_Curve after(entity_edge(sk,*sk.entity(id),Frame{}));
  for(int i=0;i<=40;++i)CHECK_NEAR(samples[i].Distance(after.Value(i/20.0)),0,1e-7);
  sk.entity(id)->weights[1]=0.5;sk.entity(id)->weights[2]=2.0;
  auto round=Sketch::from_json(sk.to_json());CHECK_NEAR(round.entity(id)->weights[1],0.5,1e-9);CHECK_NEAR(round.entity(id)->weights[2],2,1e-9);
  CHECK(!entity_edge(round,*round.entity(id),Frame{}).IsNull());
}
TEST(open_endpoint_detection_handles_t_junctions_and_construction) {
  Sketch sk;int a=sk.add_point(0,0),b=sk.add_point(10,0),c=sk.add_point(5,0),d=sk.add_point(5,5);
  sk.add_line(a,b);sk.add_line(c,d);auto ends=dangling_vertices(sk);
  CHECK_EQ(ends.size(),3u);CHECK(std::find(ends.begin(),ends.end(),c)==ends.end());
  sk.add_line(a,d);sk.add_line(d,b);CHECK(dangling_vertices(sk).empty());
  sk.add_line(sk.add_point(30,0),sk.add_point(31,0),true);CHECK(dangling_vertices(sk).empty());
  Sketch circle;circle.add_circle(circle.add_point(0,0),10);CHECK(dangling_vertices(circle).empty());
}

TEST(sketch_record_format_and_incremental_replay) {
  Document doc=Document::create();
  Sketch sk; int a=sk.add_point(0,0),b=sk.add_point(10,0); sk.add_line(a,b);
  const auto before=sk.to_json();
  const std::string id=doc.append(make_sketch_op("Sketch {\"quoted\"}",{{"base","xy"}},before)).id;
  sk.point(b)->x=14;
  doc.append(make_edit_op(id,{{"geometry_delta",sketch_delta(before,sk.to_json())}}));
  const std::string text=doc.serialize();
  CHECK(text.find("#opad 2\n")==0);
  CHECK(text.find("\"points\": [\n")!=std::string::npos);
  auto loaded=Document::parse(text);
  CHECK_EQ(loaded.serialize(),text);
  CHECK(resolve(loaded).sketch(id)->geometry==sk.to_json());
  doc.append({{"op","delete"},{"target",doc.ops.back().id}});
  CHECK(resolve(doc).sketch(id)->geometry==before);
  CHECK_THROWS(Document::parse(text.substr(0,text.find("#bodies")-3)));
}
TEST(extrude_start_offset_face_and_invalid_axis) {
  Document doc=Document::create();
  commands::run("param",{{"name","startHeight"},{"expr","7 mm"}},&doc);
  const auto sketch=run_id(sketch_cmd(doc,rectangle(0,0,20,10)));
  const auto profiles=json::array({{{"sketch",sketch},{"all",true}}});
  const auto extrude=run_id(feature_cmd(doc,"extrude",{{"profiles",profiles},{"distance","5 mm"},{"start","offset"},{"start_offset","startHeight"}}));
  auto scene=resolve(doc);auto bounds=node_world_bbox(doc,scene,scene.all_bodies().front());
  CHECK_NEAR(bounds.CornerMin().Z(),7,1e-6);CHECK_NEAR(bounds.CornerMax().Z(),12,1e-6);CHECK_NEAR(total_volume(doc),1000,1e-6);
  commands::run("param",{{"name","startHeight"},{"expr","11 mm"}},&doc);
  scene=resolve(doc);bounds=node_world_bbox(doc,scene,scene.all_bodies().front());CHECK_NEAR(bounds.CornerMin().Z(),11,1e-6);
  const auto body=scene.all_bodies().front();const auto shape=node_world_shape(doc,scene,body);
  json top,side;int index=0;for(TopExp_Explorer it(shape,TopAbs_FACE);it.More();it.Next(),++index) {
    BRepAdaptor_Surface surface(TopoDS::Face(it.Current()));if(surface.GetType()!=GeomAbs_Plane)continue;
    auto ref=json{{"body",body},{"kind","face"},{"index",index}};
    if(std::abs(surface.Plane().Axis().Direction().Z())>.99 && std::abs(surface.Plane().Location().Z()-16)<1e-6)top=ref;
    if(std::abs(surface.Plane().Axis().Direction().Z())<.01)side=ref;
  }
  CHECK(!top.is_null());CHECK(!side.is_null());
  const auto next=run_id(feature_cmd(doc,"extrude",{{"profiles",profiles},{"distance","3 mm"},{"start","face"},{"start_face",json::array({top})}}));
  scene=resolve(doc);const auto* result=scene.feature(next);CHECK(result);
  CHECK_NEAR(result->result.at("distance_handle").at("origin")[2].get<double>(),16,1e-6);
  CHECK_NEAR(total_volume(doc),1600,1e-5);
  const auto count=doc.ops.size();CHECK_THROWS(feature_cmd(doc,"extrude",{{"profiles",profiles},{"distance","3 mm"},{"start","face"},{"start_face",json::array({side})}}));CHECK_EQ(doc.ops.size(),count);
}
TEST(sketch_origin_support_regenerates_and_roundtrips) {
  Document doc=Document::create();
  const auto support=run_id(feature_cmd(doc,"plane",{{"mode","offset"},{"plane",{{"base","xy"}}},{"distance","30 mm"}}));
  const json plane={{"support",{{"feature",support}}},{"origin",{{"world",{12,13,200}}}}};
  const auto sketch=run_id(sketch_cmd(doc,rectangle(0,0,10,20),plane));
  auto frame=resolve(doc).sketch(sketch)->frame;
  CHECK_NEAR(frame.origin[0],12,1e-8);CHECK_NEAR(frame.origin[1],13,1e-8);CHECK_NEAR(frame.origin[2],30,1e-8);
  feature_cmd(doc,"extrude",{{"profiles",json::array({{{"sketch",sketch},{"all",true}}})},{"distance","5 mm"}});
  const double volume=total_volume(doc);
  commands::run("feature_edit",{{"target",support},{"inputs",{{"distance","60 mm"}}}},&doc);
  const auto restored=Document::parse(doc.serialize());const auto scene=resolve(restored);CHECK(scene.unresolved.empty());
  CHECK_NEAR(scene.sketch(sketch)->frame.origin[2],60,1e-8);CHECK_NEAR(total_volume(restored),volume,1e-7);
  const auto numeric=resolve_plane(doc,scene,{{"support",{{"base","xz"}}},{"origin",{{"uv",{4,8}}}}});
  CHECK_NEAR(numeric.origin[0],4,1e-9);CHECK_NEAR(numeric.origin[1],0,1e-9);CHECK_NEAR(numeric.origin[2],8,1e-9);
}

TEST(face_sketch_origin_uses_lower_left_corner_and_preserves_existing_placement) {
  Document doc=Document::create();
  feature_cmd(doc,"box",{{"length","40 mm"},{"width","30 mm"},{"height","20 mm"},{"centered",false}});
  const auto scene=resolve(doc);const auto body=scene.all_bodies().front();
  const auto shape=node_world_shape(doc,scene,body);json top;int index=0;
  for(TopExp_Explorer faces(shape,TopAbs_FACE);faces.More();faces.Next(),++index){
    BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
    if(surface.GetType()==GeomAbs_Plane && std::abs(surface.Plane().Location().Z()-20)<1e-8 && std::abs(surface.Plane().Axis().Direction().Z())>.99)
      top={{"body",body},{"kind","face"},{"index",index}};
  }
  CHECK(!top.is_null());
  auto frame=resolve_plane(doc,scene,{{"face",top}});
  CHECK_NEAR(frame.origin[0],0,1e-8);CHECK_NEAR(frame.origin[1],0,1e-8);CHECK_NEAR(frame.origin[2],20,1e-8);
  frame.origin={8,7,20};
  const auto kept=resolve_plane(doc,scene,{{"face",top},{"frame",frame.to_json()}});
  CHECK_NEAR(kept.origin[0],8,1e-8);CHECK_NEAR(kept.origin[1],7,1e-8);
}

// TODO 10 A1: an extruded profile with spline and arc edges is an exact extrusion of its sketch curves, at the bottom
// and at the top. (What did not follow them on screen was the display mesh: see test_mesh_recovery.)
TEST(extrude_follows_its_sketch_curves_exactly) {
  Document doc = Document::create();
  Sketch sk;
  const int a = sk.add_point(0, 0), b = sk.add_point(-3, 5), c = sk.add_point(0, 10), d = sk.add_point(20, 10), e = sk.add_point(20, 0),
            m = sk.add_point(20, 5);
  SkEntity spline;
  spline.type = SkEntity::Type::Spline;
  spline.id = sk.next_id();
  spline.p = {a, b, c};
  sk.entities.push_back(spline);
  sk.add_line(c, d);
  sk.add_arc(m, e, d);  // the right half circle
  sk.add_line(e, a);
  const std::string sketch = run_id(sketch_cmd(doc, sk));
  feature_cmd(doc, "extrude", {{"profiles", json::array({json{{"sketch", sketch}, {"at", {10, 5}}}})}, {"distance", "10 mm"}});
  const Scene s = resolve(doc);
  CHECK_EQ(s.all_bodies().size(), 1u);
  const TopoDS_Shape body = node_world_shape(doc, s, s.all_bodies().front());
  TopoDS_Compound edges;
  BRep_Builder builder;
  builder.MakeCompound(edges);
  for (TopExp_Explorer ex(body, TopAbs_EDGE); ex.More(); ex.Next()) builder.Add(edges, ex.Current());
  const SketchItem* item = s.sketch(sketch);
  CHECK(item);
  double worst = 0;
  int samples = 0;
  for (const auto& edge : sketch_edges(Sketch::from_json(item->geometry), item->frame)) {
    BRepAdaptor_Curve curve(edge);
    for (int i = 0; i <= 40; ++i) {
      const gp_Pnt p = curve.Value(curve.FirstParameter() + (curve.LastParameter() - curve.FirstParameter()) * i / 40);
      for (const double z : {0.0, 10.0}) {
        BRepExtrema_DistShapeShape distance(BRepBuilderAPI_MakeVertex(p.Translated(gp_Vec(0, 0, z))).Vertex(), edges);
        CHECK(distance.IsDone());
        worst = std::max(worst, distance.Value());
        ++samples;
      }
    }
  }
  std::printf("%d points on the sketch curves, farthest from the body's edges %.3g mm\n", samples, worst);
  CHECK(samples > 100);
  CHECK(worst < 1e-6);
}
