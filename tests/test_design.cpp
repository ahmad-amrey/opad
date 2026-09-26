#include "opad/design/sketch_edit.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <cstdio>
#include <set>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
// Design engine: expressions, parameters, sketches -> profiles, features, regeneration, history edits.
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <filesystem>
#include <functional>
#include <map>

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
    if (s.node(b)->name.rfind("Box", 0) == 0) box = b;  // bodies are named after their feature
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

// TODO 10 C3: editing one feature regenerated unrelated ones (a combine's target and tools were not in its fingerprint,
// so it counted as depending on every body) and saved their unchanged bodies under new keys. As the agent writes them:
// references as plain strings, a pattern whose copies a combine joins, a fillet on "uuid/edge/N" tokens.
TEST(regeneration_touches_only_dependents_and_keeps_keys) {
  Document doc = Document::create();
  auto created = [&](const std::string& kind, const json& inputs) {
    feature_cmd(doc, kind, inputs);
    const Scene s = resolve(doc);
    const Feature& f = s.features.back();
    CHECK(f.error.empty());
    std::vector<std::string> bodies;
    for (const auto& b : f.result.value("bodies", json::array())) bodies.push_back(b["id"].get<std::string>());
    return std::make_pair(f.id, bodies);
  };
  // Earlier in the history than the combine, as the phone's battery label was.
  const auto [label, labels] = created("box", {{"x", "-40 mm"}, {"length", "8 mm"}, {"width", "8 mm"}, {"height", "1 mm"}});
  const auto [pad, pads] = created("box", {{"length", "2 mm"}, {"width", "1 mm"}, {"height", "0.5 mm"}});
  const auto [array, copies] = created("pattern_rect", {{"bodies", json::array({pads[0]})}, {"count", "4"}, {"spacing", "1.5 mm"}, {"axis", {{"base", "x"}}}});
  CHECK_EQ(copies.size(), 3u);
  // The copies overlap the pad: joined into it they make one strip.
  json tools = json::array();
  for (const auto& c : copies) tools.push_back(c);
  const auto [joined, joinedBodies] = created("combine", {{"target", json::array({pads[0]})}, {"tools", tools}, {"operation", "join"}});
  const auto [block, blocks] = created("box", {{"x", "40 mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "5 mm"}});
  const auto [round, rounds] = created("fillet", {{"edges", json::array({blocks[0] + "/edge/0", blocks[0] + "/edge/2"})}, {"radius", "1 mm"}});
  auto edit = [&](const std::string& target, const json& inputs) {
    const size_t bodies = doc.bodies().size();
    commands::run("feature_edit", {{"target", target}, {"inputs", inputs}}, &doc);
    std::set<std::string> regenerated;
    for (const auto& op : doc.ops)
      if (op.type == "regen" && op.id == doc.ops.back().id)
        for (const auto& [id, result] : op.data.at("results").items()) regenerated.insert(id);
    return std::make_pair(regenerated, doc.bodies().size() - bodies);
  };
  // An unrelated edit regenerates the edited feature alone and adds its one new body.
  const auto [unrelated, unrelatedBodies] = edit(label, {{"height", "2 mm"}});
  CHECK(unrelated == std::set<std::string>{label});
  CHECK_EQ(unrelatedBodies, 1u);
  // Editing the patterned pad regenerates the pattern and the combine (both name it as plain strings).
  const auto [dependent, dependentBodies] = edit(pad, {{"length", "2.5 mm"}});
  CHECK(dependent.count(pad) && dependent.count(array) && dependent.count(joined));
  CHECK(!dependent.count(block) && !dependent.count(round) && !dependent.count(label));
  CHECK(dependentBodies >= 2u);
  // A body under a fillet on edge tokens: the fillet follows.
  const auto [filleted, filletBodies] = edit(block, {{"height", "6 mm"}});
  CHECK(filleted == (std::set<std::string>{block, round}));
  CHECK_EQ(filletBodies, 2u);
  // Recomputing everything from the stored bodies reproduces every stored body: no new entries, no changed keys.
  const Scene before = resolve(doc);
  Plan all = plan_regenerate(doc, true);
  CHECK(all.bodies.empty());
  commit(doc, std::move(all), "test");
  const Scene after = resolve(doc);
  for (const auto& id : before.all_bodies()) CHECK_EQ(after.node(id)->body_key, before.node(id)->body_key);
}

// TODO 10 C1: a body may hold several separate solids. Joining material that does not touch a named target (three
// screws, the letters of a word) used to fail, so agents added hidden tie bars; a cut that parted a body scattered the
// pieces as new top-level bodies.
TEST(bodies_hold_several_separate_solids) {
  auto solids = [](const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_SOLID); ex.More(); ex.Next()) ++n;
    return n;
  };
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "2 mm"}});
  std::string plate = resolve(doc).all_bodies().front();
  // Material apart from the named target joins it: one body, two solids.
  feature_cmd(doc, "cylinder", {{"x", "30 mm"}, {"diameter", "4 mm"}, {"height", "3 mm"}, {"operation", "join"}, {"targets", json::array({plate})}});
  Scene s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_EQ(s.all_bodies().size(), 1u);
  CHECK_EQ(solids(node_world_shape(doc, s, plate)), 2);
  CHECK_NEAR(volume_of_node(doc, s, plate), 10 * 10 * 2 + M_PI * 4 * 3, 1e-6);
  // Three screws as one body: a pattern of separate copies combined.
  feature_cmd(doc, "cylinder", {{"x", "0 mm"}, {"y", "40 mm"}, {"diameter", "3 mm"}, {"height", "8 mm"}});
  s = resolve(doc);
  const std::string screw = s.features.back().result["bodies"][0]["id"];
  feature_cmd(doc, "pattern_rect", {{"bodies", json::array({screw})}, {"count", "3"}, {"spacing", "10 mm"}, {"axis", {{"base", "x"}}}});
  s = resolve(doc);
  json tools = json::array();
  for (const auto& b : s.features.back().result["bodies"]) tools.push_back(b["id"]);
  CHECK_EQ(tools.size(), 2u);
  feature_cmd(doc, "combine", {{"target", json::array({screw})}, {"tools", tools}, {"operation", "join"}});
  s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_EQ(solids(node_world_shape(doc, s, screw)), 3);
  CHECK(!s.node(tools[0].get<std::string>()) || s.node(tools[0].get<std::string>())->body_missing);
  // A cut through the plate parts it: the pieces stay the plate, no new top-level bodies.
  const size_t bodies = s.all_bodies().size();
  feature_cmd(doc, "box", {{"x", "0 mm"}, {"length", "2 mm"}, {"width", "20 mm"}, {"height", "10 mm"}, {"operation", "cut"}, {"targets", json::array({plate})}});
  s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_EQ(s.all_bodies().size(), bodies);
  CHECK_EQ(solids(node_world_shape(doc, s, plate)), 3);  // two halves of the plate and the joined cylinder
  // A multi-solid body is still an ordinary target: cut every piece at once.
  feature_cmd(doc, "box", {{"length", "100 mm"}, {"width", "100 mm"}, {"height", "1 mm"}, {"operation", "cut"}, {"targets", json::array({plate})}});
  s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_NEAR(volume_of_node(doc, s, plate), 10 * 10 * 1 - 2 * 10 * 1 + M_PI * 4 * 2, 1e-6);
  // Automatic joins still take only the bodies the material touches: far away it makes a new body.
  feature_cmd(doc, "box", {{"x", "300 mm"}, {"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}, {"operation", "join"}});
  s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_EQ(s.all_bodies().size(), bodies + 1);
}

// TODO 10 B14 / C2: a new body is named after its feature (numbered when it makes several); a copy or a piece is named
// after its source and goes into its component with its colour, placed where it was made.
TEST(new_bodies_are_named_placed_and_coloured_when_made) {
  auto bounds = [](const Document& doc, const Scene& s, const std::string& id) {
    Bnd_Box b;
    BRepBndLib::Add(node_world_shape(doc, s, id), b);
    return b;
  };
  Document doc = Document::create();
  const std::string group = commands::run("component", {{"name", "Fasteners"}}, &doc)["component_id"];
  commands::run("transform", {{"target", group}, {"matrix", Mat4::translation(0, 0, 50).to_json()}}, &doc);
  const json made = commands::run("feature", {{"kind", "cylinder"}, {"name", "Screw"}, {"inputs", {{"diameter", "3 mm"}, {"height", "8 mm"}}},
                                              {"color", json::array({0.8, 0.1, 0.1})}, {"parent", group}}, &doc);
  CHECK_EQ(made["style_ids"].size(), 2u);  // an appearance and a reparent op, no rename: the name is the feature's
  const std::string screw = made["body_ids"][0];
  Scene s = resolve(doc);
  CHECK_EQ(s.node(screw)->name, "Screw");
  CHECK_EQ(s.node(screw)->parent, group);
  CHECK(s.node(screw)->has_color && std::fabs(s.node(screw)->color[0] - 0.8) < 1e-12);
  // Pattern copies: "Screw 2", "Screw 3", in the fasteners with the screw's colour, one spacing apart.
  const json row = feature_cmd(doc, "pattern_rect", {{"bodies", json::array({screw})}, {"count", "3"}, {"spacing", "10 mm"}, {"axis", {{"base", "x"}}}});
  CHECK_EQ(row["body_ids"].size(), 2u);
  CHECK_EQ(row["all_body_ids"].size(), 3u);
  CHECK_EQ(row["all_body_ids"][0], screw);
  s = resolve(doc);
  const Bnd_Box original = bounds(doc, s, screw);
  CHECK_NEAR(original.CornerMin().Z(), 50, 1e-3);
  for (size_t i = 0; i < 2; ++i) {
    const Node* copy = s.node(row["body_ids"][i]);
    CHECK_EQ(copy->name, "Screw " + std::to_string(i + 2));
    CHECK_EQ(copy->parent, group);
    CHECK(copy->has_color && copy->color == s.node(screw)->color);
    const Bnd_Box b = bounds(doc, s, copy->id);
    CHECK_NEAR(b.CornerMin().X() - original.CornerMin().X(), 10.0 * (i + 1), 1e-6);
    CHECK_NEAR(b.CornerMin().Z() - original.CornerMin().Z(), 0, 1e-6);
  }
  // A moved copy and a split piece are named, placed and coloured the same way.
  const json copied = feature_cmd(doc, "move", {{"bodies", json::array({screw})}, {"dy", "20 mm"}, {"copy", true}});
  s = resolve(doc);
  const Node* moved = s.node(copied["body_ids"][0]);
  CHECK_EQ(moved->name, "Screw 4");
  CHECK_EQ(moved->parent, group);
  CHECK(moved->has_color);
  CHECK_NEAR(bounds(doc, s, moved->id).CornerMin().Y() - original.CornerMin().Y(), 20, 1e-6);
  const json split = feature_cmd(doc, "split", {{"bodies", json::array({row["body_ids"][0]})}, {"plane", {{"base", "xz"}}}});
  s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_EQ(split["body_ids"].size(), 2u);
  const Node* piece = s.node(split["body_ids"][1]);
  CHECK_EQ(piece->name, "Screw 5");
  CHECK_EQ(piece->parent, group);
  CHECK(piece->has_color);
  CHECK_NEAR(volume_of_node(doc, s, piece->id) + volume_of_node(doc, s, row["body_ids"][0]), M_PI * 1.5 * 1.5 * 8, 1e-6);
  // A regeneration keeps what the copies were given; a copy the new count adds is made the same way.
  const std::string pattern = row["feature_id"];
  commands::run("feature_edit", {{"target", pattern}, {"inputs", {{"count", "4"}}}}, &doc);
  s = resolve(doc);
  const json& now = s.feature(pattern)->result["bodies"];
  CHECK_EQ(now.size(), 3u);
  CHECK_EQ(now[0]["id"], row["body_ids"][0]);
  CHECK_EQ(s.node(now[1]["id"])->name, "Screw 3");
  CHECK_EQ(s.node(now[2]["id"])->name, "Screw 6");
  CHECK_EQ(s.node(now[2]["id"])->parent, group);
  // Several bodies from scratch are numbered after the feature.
  Sketch dots;
  json profiles = json::array();
  for (int i = 0; i < 3; ++i) {
    dots.add_circle(dots.add_point(100.0 + 10 * i, 0), 2);
    profiles.push_back({{"at", {100.0 + 10 * i, 0}}});
  }
  const std::string sketch = run_id(sketch_cmd(doc, dots));
  for (auto& p : profiles) p["sketch"] = sketch;
  const json pins = commands::run("feature", {{"kind", "extrude"}, {"name", "Board screw"}, {"inputs", {{"profiles", profiles}, {"distance", "4 mm"}}}}, &doc);
  s = resolve(doc);
  CHECK_EQ(pins["body_ids"].size(), 3u);
  for (size_t i = 0; i < 3; ++i) CHECK_EQ(s.node(pins["body_ids"][i])->name, "Board screw " + std::to_string(i + 1));
  CHECK(s.unresolved.empty());
}

// TODO 10 B14 / B15: body_name, color and parent on a feature, and targets on rename / appearance / reparent, write
// exactly the ops of the long form: the same scene, record for record.
TEST(body_style_and_targets_equal_the_long_form) {
  auto build = [](Document& doc, bool short_form) {
    const std::string group = commands::run("component", {{"name", "Pins"}}, &doc)["component_id"];
    Sketch sk;
    json profiles = json::array();
    for (int i = 0; i < 10; ++i) {
      sk.add_circle(sk.add_point(10.0 * i, 0), 2);
      profiles.push_back({{"at", {10.0 * i, 0}}});
    }
    const std::string sketch = run_id(sketch_cmd(doc, sk));
    for (auto& p : profiles) p["sketch"] = sketch;
    json args = {{"kind", "extrude"}, {"name", "Pin row"}, {"inputs", {{"profiles", profiles}, {"distance", "6 mm"}}}};
    if (short_form) {
      args["body_name"] = "Pin {n}";
      args["color"] = json::array({0.1, 0.4, 0.8});
      args["parent"] = group;
    }
    const json made = commands::run("feature", args, &doc);
    CHECK_EQ(made["body_ids"].size(), 10u);
    if (!short_form)
      for (size_t i = 0; i < 10; ++i) {
        const json id = made["body_ids"][i];
        commands::run("rename", {{"target", id}, {"name", "Pin " + std::to_string(i + 1)}}, &doc);
        commands::run("appearance", {{"target", id}, {"color", json::array({0.1, 0.4, 0.8})}}, &doc);
        commands::run("reparent", {{"target", id}, {"parent", group}}, &doc);
      }
    // Bulk edits: every pin renamed with a template, recoloured and moved to a second component.
    const std::string spare = commands::run("component", {{"name", "Spares"}}, &doc)["component_id"];
    if (short_form) {
      const json renamed = commands::run("rename", {{"targets", made["body_ids"]}, {"name", "Spare {n}"}}, &doc);
      CHECK_EQ(renamed["ids"].size(), 10u);
      commands::run("appearance", {{"targets", made["body_ids"]}, {"color", json::array({0.9, 0.9, 0.2})}}, &doc);
      commands::run("reparent", {{"targets", made["body_ids"]}, {"parent", spare}}, &doc);
    } else {
      for (size_t i = 0; i < 10; ++i) commands::run("rename", {{"target", made["body_ids"][i]}, {"name", "Spare " + std::to_string(i + 1)}}, &doc);
      for (size_t i = 0; i < 10; ++i) commands::run("appearance", {{"target", made["body_ids"][i]}, {"color", json::array({0.9, 0.9, 0.2})}}, &doc);
      for (size_t i = 0; i < 10; ++i) commands::run("reparent", {{"target", made["body_ids"][i]}, {"parent", spare}}, &doc);
    }
  };
  Document a = Document::create(), b = Document::create();
  build(a, true);
  build(b, false);
  // Ids, times and fingerprints differ; everything else, op by op, is the same.
  auto records = [](const Document& doc) {
    std::map<std::string, std::string> ids;
    auto canon = [&](const std::string& id) {
      if (!ids.count(id)) ids[id] = "#" + std::to_string(ids.size());
      return ids[id];
    };
    std::vector<std::string> out;
    std::function<void(json&)> scrub = [&](json& v) {
      if (v.is_string() && v.get<std::string>().size() == 36 && v.get<std::string>()[8] == '-') v = canon(v.get<std::string>());
      else if (v.is_array() || v.is_object())
        for (auto& c : v) scrub(c);
    };
    for (const auto& op : doc.ops) {
      json d = op.data;
      d.erase("ts");
      d.erase("by");
      d.erase("id");
      if (d.contains("result")) d["result"].erase("in");
      scrub(d);
      out.push_back(d.dump());
    }
    return out;
  };
  const auto ra = records(a), rb = records(b);
  CHECK_EQ(ra.size(), rb.size());
  for (size_t i = 0; i < std::min(ra.size(), rb.size()); ++i) CHECK_EQ(ra[i], rb[i]);
  const Scene sa = resolve(a), sb = resolve(b);
  CHECK(sa.unresolved.empty());
  auto shape_of = [](const Scene& s) {
    std::vector<std::string> out;
    for (const auto& id : s.all_bodies()) {
      const Node* n = s.node(id);
      out.push_back(n->name + "|" + s.node(n->parent)->name + "|" + json(n->color).dump() + "|" + n->body_key + "|" + s.world(id).to_json().dump());
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  CHECK(shape_of(sa) == shape_of(sb));
  size_t spares = 0;
  for (const auto& id : sa.all_bodies()) spares += sa.node(id)->name.rfind("Spare ", 0) == 0 && sa.node(sa.node(id)->parent)->name == "Spares";
  CHECK_EQ(spares, 10u);
  // One target and targets are not given together; an empty list is refused.
  bool refused = false;
  try { commands::run("rename", {{"target", sa.all_bodies()[0]}, {"targets", json::array({sa.all_bodies()[1]})}, {"name", "X"}}, &a); } catch (const Error&) { refused = true; }
  CHECK(refused);
  refused = false;
  try { commands::run("appearance", {{"targets", json::array()}, {"visible", false}}, &a); } catch (const Error&) { refused = true; }
  CHECK(refused);
  // A parent that is not a component is refused before anything is computed.
  refused = false;
  try { commands::run("feature", {{"kind", "box"}, {"inputs", json::object()}, {"parent", sa.all_bodies()[0]}}, &a); } catch (const Error& e) { refused = std::string(e.what()).find("not a component") != std::string::npos; }
  CHECK(refused);
}

// TODO 10 B3 / B10: results name the frame a plane resolved to, and sizes are the tight box, not the padded one.
TEST(results_report_frames_and_tight_boxes) {
  Document doc = Document::create();
  const json box = commands::run("feature", {{"kind", "box"}, {"inputs", {{"plane", {{"base", "xz"}}}, {"length", "60 mm"}, {"width", "31 mm"}, {"height", "21 mm"}}}}, &doc);
  CHECK_EQ(box["frame"]["normal"], json::array({0.0, -1.0, 0.0}));
  CHECK_EQ(box["frame"]["x"], json::array({1.0, 0.0, 0.0}));
  CHECK_EQ(box["frame"]["y"], json::array({0.0, 0.0, 1.0}));
  Scene s = resolve(doc);
  const std::string body = box["body_ids"][0];
  // The box stands on XZ: length along X, width along Z, height along -Y; centred in X and Z only.
  const Bnd_Box tight = node_tight_bbox(doc, s, body), padded = node_world_bbox(doc, s, body);
  double x0, y0, z0, x1, y1, z1;
  tight.Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(x1 - x0, 60, 1e-9);
  CHECK_NEAR(z1 - z0, 31, 1e-9);
  CHECK_NEAR(y1 - y0, 21, 1e-9);
  CHECK_NEAR(y1, 0, 1e-9);
  CHECK_NEAR(x0, -30, 1e-9);
  CHECK(padded.SquareExtent() > tight.SquareExtent());
  // Moved, the cached box follows the translation; turned, it is measured in place and still tight.
  commands::run("transform", {{"target", body}, {"matrix", Mat4::translation(5, 0, 0).to_json()}}, &doc);
  s = resolve(doc);
  node_tight_bbox(doc, s, body).Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(x0, -25, 1e-9);
  CHECK_NEAR(x1 - x0, 60, 1e-9);
  Mat4 quarter;  // a quarter turn about Z
  quarter.m = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  commands::run("transform", {{"target", body}, {"matrix", quarter.to_json()}}, &doc);
  s = resolve(doc);
  node_tight_bbox(doc, s, body).Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(y1 - y0, 60, 1e-9);
  CHECK_NEAR(x1 - x0, 21, 1e-9);
  // A sketch reports the frame it was made in; so does an edit of a feature's plane.
  const json sketch = sketch_cmd(doc, rectangle(0, 0, 10, 10), {{"base", "yz"}});
  CHECK_EQ(sketch["frame"]["normal"], json::array({1.0, 0.0, 0.0}));
  CHECK_EQ(sketch["sketch_id"], sketch["ids"][0]);
  const json edited = commands::run("feature_edit", {{"target", box["feature_id"]}, {"inputs", {{"plane", {{"base", "yz"}}}}}}, &doc);
  CHECK_EQ(edited["frame"]["normal"], json::array({1.0, 0.0, 0.0}));
  CHECK_EQ(edited["feature_id"], box["feature_id"]);
  // Features without a plane input report none.
  CHECK(!feature_cmd(doc, "move", {{"bodies", json::array({body})}, {"dz", "5 mm"}}).contains("frame"));
}

// Edge and face features on a body placed by a transform (an assembly instance, a moved part): every lookup of the
// body locates it anew, and the picked edges and faces must still be found in it.
TEST(edge_and_face_features_work_on_placed_bodies) {
  Document doc = Document::create();
  Mat4 placed;  // a quarter turn about Z and a shift
  placed.m = {0, -1, 0, 10, 1, 0, 0, 5, 0, 0, 1, 0, 0, 0, 0, 1};
  auto placed_box = [&] {
    const std::string body = feature_cmd(doc, "box", {{"length", "60 mm"}, {"width", "31 mm"}, {"height", "21 mm"}})["body_ids"][0];
    commands::run("transform", {{"target", body}, {"matrix", placed.to_json()}}, &doc);
    return body;
  };
  const std::string body = placed_box();
  const Scene s0 = resolve(doc);
  const double before = volume_of_node(doc, s0, body);
  for (const auto& [kind, inputs] : std::vector<std::pair<std::string, json>>{
           {"fillet", {{"edges", json::array({body + "/edge/0"})}, {"radius", "2 mm"}}},
           {"chamfer", {{"edges", json::array({body + "/edge/5"})}, {"distance", "1 mm"}}},
           {"offset_face", {{"faces", json::array({body + "/face/1"})}, {"distance", "-2 mm"}}}}) {
    feature_cmd(doc, kind, inputs);
    const Scene s = resolve(doc);
    CHECK(s.features.back().error.empty());
    CHECK(s.unresolved.empty());
  }
  Scene s = resolve(doc);
  CHECK(volume_of_node(doc, s, body) < before);
  // Still where the transform put it: the turned box spans 31 mm in X around x = 10 and 60 mm in Y around y = 5.
  double x0, y0, z0, x1, y1, z1;
  node_tight_bbox(doc, s, body).Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR((x0 + x1) / 2, 10, 2.5);
  CHECK_NEAR((y0 + y1) / 2, 5, 2.5);
  // A shell of a placed body.
  const std::string other = placed_box();
  feature_cmd(doc, "shell", {{"faces", json::array({other + "/face/5"})}, {"thickness", "1 mm"}});
  s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_NEAR(volume_of_node(doc, s, other), 60 * 31 * 21 - 58 * 29 * 20, 1e-6);
}

// The kernel cannot open a face that blends smoothly into a fillet, and used to hand the body back unchanged while
// saying it was done: the shell was recorded but did nothing. It is refused now, naming the reason.
TEST(a_shell_the_kernel_cannot_make_is_refused) {
  Document doc = Document::create();
  const std::string body = feature_cmd(doc, "box", {{"length", "60 mm"}, {"width", "31 mm"}, {"height", "21 mm"}})["body_ids"][0];
  feature_cmd(doc, "fillet", {{"edges", json::array({body + "/edge/0"})}, {"radius", "2 mm"}});
  const size_t features = resolve(doc).features.size();
  std::string error;
  try {
    feature_cmd(doc, "shell", {{"faces", json::array({body + "/face/2"})}, {"thickness", "1 mm"}});
  } catch (const Error& e) {
    error = e.what();
  }
  CHECK(error.find("blends smoothly into a fillet") != std::string::npos);
  CHECK_EQ(resolve(doc).features.size(), features);
  // A face away from the fillet still opens.
  feature_cmd(doc, "shell", {{"faces", json::array({body + "/face/1"})}, {"thickness", "1 mm"}});
  const Scene s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK(volume_of_node(doc, s, body) < 10000);
}

// TODO 10 B4: a label from shapes, engraved with text, all through the sketch and feature commands: no hand-placed
// points, no arc centres, and the letters come back as profiles to cut.
TEST(a_label_is_engraved_with_text_from_shapes) {
  Document doc = Document::create();
  const json made = commands::run("sketch", {{"plane", {{"base", "xy"}}},
      {"geometry", {{"shapes", json::array({{{"kind", "rounded_rect"}, {"picks", {{0, 0}, {60, 20}}}, {"options", {{"radius", 3.0}}}},
                                            {{"kind", "text"}, {"picks", {{30, 5}}}, {"options", {{"text", "OPAD"}, {"height", 10.0}, {"align", "center"}}}}})}}}}, &doc);
  const std::string sketch = made["sketch_id"];
  CHECK_EQ(made["id_map"].size(), 2u);
  const json letters = made["id_map"][1]["profiles"];
  CHECK_EQ(letters.size(), 4u);  // one point inside each letter
  // The whole plate (every region), then the letters cut 1 mm deep from its top.
  feature_cmd(doc, "extrude", {{"profiles", json::array({{{"sketch", sketch}}})}, {"distance", "3 mm"}});
  const double plate = total_volume(doc);
  CHECK_NEAR(plate, (60 * 20 - (4 - M_PI) * 9) * 3, 1e-4);
  json profiles = json::array();
  for (const auto& p : letters) profiles.push_back({{"sketch", sketch}, {"at", p["at"]}});
  feature_cmd(doc, "extrude", {{"profiles", profiles}, {"start", "offset"}, {"start_offset", "2 mm"}, {"distance", "1 mm"}, {"operation", "cut"}});
  const Scene s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_EQ(s.all_bodies().size(), 1u);
  const double engraved = plate - total_volume(doc);
  CHECK(engraved > 40 && engraved < 200);  // four block letters 10 mm high, 1.4 mm strokes, 1 mm deep
}

// TODO 10 B5: a plane by origin and normal wherever a plane is taken, with a deterministic x; and the construction
// plane through a point, square to an edge, axis or face.
TEST(planes_by_origin_and_normal) {
  Document doc = Document::create();
  // A cylinder along Y: just the normal, no frame worked out by hand.
  const json cyl = feature_cmd(doc, "cylinder", {{"plane", {{"origin", {0, 5, 0}}, {"normal", {0, 1, 0}}}}, {"diameter", "10 mm"}, {"height", "30 mm"}});
  CHECK_EQ(cyl["frame"]["normal"], json::array({0.0, 1.0, 0.0}));
  CHECK_EQ(cyl["frame"]["x"], json::array({1.0, 0.0, 0.0}));  // world X laid onto the plane
  Scene s = resolve(doc);
  double x0, y0, z0, x1, y1, z1;
  node_tight_bbox(doc, s, cyl["body_ids"][0]).Get(x0, y0, z0, x1, y1, z1);
  CHECK_NEAR(y0, 5, 1e-9);
  CHECK_NEAR(y1, 35, 1e-9);
  CHECK_NEAR(x1 - x0, 10, 1e-9);
  // X is the normal: world Y becomes x. A given x is laid onto the plane.
  CHECK_EQ(feature_cmd(doc, "box", {{"plane", {{"origin", {50, 0, 0}}, {"normal", {1, 0, 0}}}}})["frame"]["x"], json::array({0.0, 1.0, 0.0}));
  const json turned = feature_cmd(doc, "box", {{"plane", {{"origin", {0, 60, 0}}, {"normal", {0, 0, 2}}, {"x", {1, 1, 5}}}}});
  CHECK_NEAR(turned["frame"]["x"][0].get<double>(), std::sqrt(0.5), 1e-12);
  CHECK_NEAR(turned["frame"]["x"][1].get<double>(), std::sqrt(0.5), 1e-12);
  CHECK_NEAR(turned["frame"]["x"][2].get<double>(), 0, 1e-12);
  // A sketch stores the frame it resolved to.
  const json sketch = commands::run("sketch", {{"plane", {{"origin", {0, 0, 7}}, {"normal", {0, 0, -1}}}}, {"geometry", rectangle(0, 0, 5, 5).to_json()}}, &doc);
  s = resolve(doc);
  CHECK_NEAR(s.sketch(sketch["sketch_id"])->frame.origin[2], 7, 1e-12);
  CHECK_NEAR(s.sketch(sketch["sketch_id"])->frame.normal()[2], -1, 1e-12);
  // Mistakes are named.
  bool named = false;
  try { feature_cmd(doc, "box", {{"plane", {{"origin", {0, 0, 0}}, {"normal", {0, 0, 0}}}}}); } catch (const Error& e) { named = std::string(e.what()).find("normal must not be zero") != std::string::npos; }
  CHECK(named);
  named = false;
  try { feature_cmd(doc, "box", {{"plane", {{"origin", {0, 0, 0}}, {"normal", {0, 0, 1}}, {"x", {0, 0, 3}}}}}); } catch (const Error& e) { named = std::string(e.what()).find("parallel to its normal") != std::string::npos; }
  CHECK(named);
  // The construction plane through a point, square to the cylinder's axis (its round face).
  const std::string body = cyl["body_ids"][0];
  const json plane = feature_cmd(doc, "plane", {{"mode", "point_normal"}, {"point", json::array({{{"kind", "point"}, {"point", {3, 20, 1}}}})},
                                               {"normal", {{"face", body + "/face/0"}}}});
  s = resolve(doc);
  const Feature* made = s.feature(plane["feature_id"]);
  CHECK(made && made->error.empty());
  const Frame f = Frame::from_json(made->result["plane"]);
  CHECK_NEAR(std::fabs(f.normal()[1]), 1, 1e-9);
  CHECK_NEAR(f.origin[1], 20, 1e-9);
}

// TODO 10 B7: a rule selector re-picks the intended edges when an earlier edit renumbers the body's edges.
TEST(rule_selectors_pick_the_same_edges_after_an_earlier_edit) {
  Document doc = Document::create();
  const std::string body = feature_cmd(doc, "box", {{"length", "40 mm"}, {"width", "30 mm"}, {"height", "20 mm"}})["body_ids"][0];
  // A hole through the side, left out for now.
  const json hole = feature_cmd(doc, "cylinder", {{"plane", {{"origin", {0, -15, 10}}, {"normal", {0, 1, 0}}}}, {"diameter", "4 mm"}, {"height", "30 mm"},
                                                  {"operation", "cut"}, {"targets", json::array({body})}});
  commands::run("feature_edit", {{"target", hole["feature_id"]}, {"suppressed", true}}, &doc);
  const double plain = total_volume(doc);
  // The four top edges: straight, lying in the top face's plane.
  const json top = {{"body", body}, {"kind", "edge"}, {"select", {{"curve", "line"}, {"at_plane", {{"axis", "z"}, {"value", 20}}}}}, {"expect", 4}};
  const std::string fillet = feature_cmd(doc, "fillet", {{"edges", json::array({top})}, {"radius", "2 mm"}})["feature_id"];
  const double removed = plain - total_volume(doc);
  CHECK(removed > 50 && removed < 200);
  Scene s = resolve(doc);
  CHECK_EQ(s.feature(fillet)->result["selected"][0]["ordinals"].size(), 4u);
  CHECK_EQ(s.feature(fillet)->result["selected"][0]["hints"].size(), 4u);
  // The hole comes back: more edges, numbered differently; the rule still picks the four top edges.
  commands::run("feature_edit", {{"target", hole["feature_id"]}, {"suppressed", false}}, &doc);
  s = resolve(doc);
  CHECK(s.feature(fillet)->error.empty());
  CHECK_EQ(s.feature(fillet)->result["selected"][0]["ordinals"].size(), 4u);
  CHECK_NEAR(total_volume(doc), plain - M_PI * 4 * 30 - removed, 1e-6);
  // A rule whose count no longer holds fails where it is, instead of guessing.
  bool refused = false;
  try {
    json five = top;
    five["expect"] = 5;
    feature_cmd(doc, "chamfer", {{"edges", json::array({five})}, {"distance", "0.5 mm"}});
  } catch (const Error& e) {
    refused = std::string(e.what()).find("matches 4 edges, expected 5") != std::string::npos;
  }
  CHECK(refused);
  // The same filters query_entities takes, with a direction as a vector.
  const json along_x = {{"body", body}, {"kind", "edge"}, {"select", {{"parallel_to", {1, 0, 0}}, {"at_plane", {{"axis", "z"}, {"value", 0}}}}}, {"expect", 2}};
  feature_cmd(doc, "chamfer", {{"edges", json::array({along_x})}, {"distance", "0.5 mm"}});
  s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK(s.unresolved.empty());
}

// TODO 10 B8: extrude up to a face (a plane: its whole plane, also tilted) or up to a body.
TEST(extrude_up_to_a_face_or_a_body) {
  Document doc = Document::create();
  // A slab above the sketch: its underside at z = 20.
  const std::string slab = feature_cmd(doc, "box", {{"plane", {{"origin", {0, 0, 20}}, {"normal", {0, 0, 1}}}}, {"length", "80 mm"}, {"width", "80 mm"}, {"height", "5 mm"}})["body_ids"][0];
  const std::string sketch = run_id(sketch_cmd(doc, rectangle(-5, -5, 10, 10)));
  const json profile = json::array({json{{"sketch", sketch}, {"at", {0, 0}}}});
  Scene s = resolve(doc);
  int under = -1;  // the slab's face at z = 20, facing down
  {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(node_world_shape(doc, s, slab), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      const json d = describe_entity(faces(i));
      if (d.contains("normal") && d["normal"][2].get<double>() < -0.99) under = i - 1;
    }
  }
  CHECK(under >= 0);
  const double before = total_volume(doc);
  const json to_face = feature_cmd(doc, "extrude", {{"profiles", profile}, {"extent", "to_face"}, {"extent_face", json::array({slab + "/face/" + std::to_string(under)})}});
  CHECK_NEAR(total_volume(doc) - before, 10 * 10 * 20, 1e-6);
  CHECK(!resolve(doc).feature(to_face["feature_id"])->result.contains("distance_handle"));  // no drag arrow for it
  commands::run("feature_edit", {{"target", to_face["feature_id"]}, {"suppressed", true}}, &doc);
  feature_cmd(doc, "extrude", {{"profiles", profile}, {"extent", "to_body"}, {"extent_body", json::array({slab})}, {"operation", "join"}, {"targets", json::array({slab})}});
  s = resolve(doc);
  CHECK(s.features.back().error.empty());
  CHECK_EQ(s.all_bodies().size(), 1u);  // joined into the slab it reached
  CHECK_NEAR(total_volume(doc) - before, 10 * 10 * 20, 1e-6);
  // A tilted plane: the extrusion ends on it, not at a distance.
  Document tilted = Document::create();
  const std::string wedge = feature_cmd(tilted, "box", {{"plane", {{"origin", {0, 0, 30}}, {"normal", {0.5, 0, 1}}}}, {"length", "100 mm"}, {"width", "100 mm"}, {"height", "5 mm"}})["body_ids"][0];
  const std::string sk = run_id(sketch_cmd(tilted, rectangle(-5, -5, 10, 10)));
  Scene ts = resolve(tilted);
  int low = -1;
  {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(node_world_shape(tilted, ts, wedge), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      const json d = describe_entity(faces(i));
      if (d.contains("normal") && d["normal"][2].get<double>() < -0.8) low = i - 1;
    }
  }
  const double w0 = total_volume(tilted);
  feature_cmd(tilted, "extrude", {{"profiles", json::array({json{{"sketch", sk}, {"at", {0, 0}}}})}, {"extent", "to_face"}, {"extent_face", json::array({wedge + "/face/" + std::to_string(low)})}});
  CHECK_NEAR(total_volume(tilted) - w0, 10 * 10 * 30, 1e-6);  // the plane through (0,0,30): the mean height is 30
  // Refused rather than guessed: two directions, a profile that misses the target.
  bool refused = false;
  try { feature_cmd(doc, "extrude", {{"profiles", profile}, {"extent", "to_body"}, {"extent_body", json::array({slab})}, {"direction", "symmetric"}}); } catch (const Error& e) { refused = std::string(e.what()).find("goes one way") != std::string::npos; }
  CHECK(refused);
  const std::string wide = run_id(sketch_cmd(doc, rectangle(30, 30, 30, 30)));
  refused = false;
  try { feature_cmd(doc, "extrude", {{"profiles", json::array({json{{"sketch", wide}, {"at", {45, 45}}}})}, {"extent", "to_body"}, {"extent_body", json::array({slab})}}); } catch (const Error& e) { refused = std::string(e.what()).find("misses the target") != std::string::npos; }
  CHECK(refused);
}

// TODO 10 B9: several fitted views in one labelled image, each with its camera in the receipt.
TEST(render_views_grid) {
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"length", "40 mm"}, {"width", "30 mm"}, {"height", "20 mm"}});
  const Scene s = resolve(doc);
  RenderOptions o;
  o.width = 400;
  o.height = 300;
  o.views = {"iso", "front", "top", "right"};
  o.edge_lines = true;
  json receipt;
  const Image grid = render_scene(doc, s, o, &receipt);
  CHECK_EQ(grid.width, 400);
  CHECK_EQ(grid.height, 300);
  CHECK_EQ(receipt["views"].size(), 4u);
  CHECK_EQ(receipt["views"][2]["view"], "top");
  CHECK_EQ(receipt["views"][3]["cell"], json::array({200, 150, 200, 150}));
  // Each cell's label sits on a plate of background in its top-left corner.
  for (const auto& cell : receipt["views"]) {
    const int x = cell["cell"][0], y = cell["cell"][1];
    CHECK_EQ(grid.px(x + 4, y + 4)[0], 255);
    int ink = 0;
    for (int yy = y + 8; yy < y + 20; ++yy)
      for (int xx = x + 8; xx < x + 40; ++xx) ink += grid.px(xx, yy)[0] < 100;
    CHECK(ink > 10);
  }
  CHECK(encode_png(grid) == encode_png(render_scene(doc, s, o)));
  bool refused = false;
  try { commands::run("render", {{"out", "unused.png"}, {"views", "iso,sideways"}}, &doc); } catch (const Error& e) { refused = std::string(e.what()).find("sideways") != std::string::npos; }
  CHECK(refused);
}
