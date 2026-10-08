#include "opad/design/sketch_edit.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
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
#include <Bnd_Box.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <cmath>
#include <filesystem>
#include <functional>
#include <map>

#include "check.hpp"
#include "opad/agent.hpp"
#include "opad/checks.hpp"
#include "opad/core.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"
#include "opad/mass.hpp"

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
  // UI-26: a plain number is stored with its unit as a word, not as "(15) * 1 mm" (which panels then showed), and that
  // older form reads back the same way; expressions keep their form.
  CHECK_EQ(ParamTable().explicit_length("15"),std::string("15 mm"));
  CHECK_EQ(inches.explicit_length(" -2.5 "),std::string("-2.5 in"));
  CHECK_EQ(inches.explicit_length("1e1"),std::string("1e1 in"));
  CHECK_EQ(ParamTable().explicit_length("(15) * 1 mm"),std::string("15 mm"));
  CHECK_EQ(inches.explicit_length("(0.5)*1 in"),std::string("0.5 in"));
  CHECK_EQ(inches.explicit_length("(15) * 1 mm"),std::string("15 mm"));  // the unit it was stored with
  CHECK_EQ(inches.explicit_length("2+1"),std::string("(2+1) * 1 in"));
  CHECK_EQ(ParamTable().explicit_length("(15) * 1 mm + 2 mm"),std::string("(15) * 1 mm + 2 mm"));
  CHECK_EQ(ParamTable().explicit_length("15 cm"),std::string("15 cm"));
  CHECK_EQ(ParamTable({{"w","w","4 mm",""}}).explicit_length("w"),std::string("w"));
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
  {  // blind counterbored holes (to a depth, not through): the GUI said "the cut does not touch any body"
    Document cube = Document::create();
    feature_cmd(cube, "box", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "-20 mm"}});
    Sketch four;
    for (double x : {-5.0, 5.0})
      for (double y : {-5.0, 5.0}) {
        SkEntity e;
        e.type = SkEntity::Type::Point;
        e.p = {four.add_point(x, y)};
        e.id = four.next_id();
        four.entities.push_back(e);
      }
    const std::string fs = run_id(sketch_cmd(cube, four));
    json at = json::array();
    for (const auto& p : four.points) at.push_back({{"sketch", fs}, {"point", p.id}});
    for (const char* type : {"simple", "counterbore", "countersink"}) {
      Document d = cube;
      feature_cmd(d, "hole", {{"points", at}, {"type", type}, {"diameter", "5 mm"}, {"depth", "10 mm"}, {"cb_diameter", "9 mm"}, {"cb_depth", "3 mm"}});
      CHECK(total_volume(d) < 8000 - 4 * M_PI * 6.25 * 10 + 1e-3);
    }
    // The same on a sketch drawn on the top face of a box standing on XY, origin moved to the face centre.
    Document up = Document::create();
    feature_cmd(up, "box", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "20 mm"}});
    const Scene us = resolve(up);
    const std::string ub = us.all_bodies().front();
    json top;
    int index = 0;
    for (TopExp_Explorer faces(node_world_shape(up, us, ub), TopAbs_FACE); faces.More(); faces.Next(), ++index) {
      BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
      if (surface.GetType() == GeomAbs_Plane && std::abs(surface.Plane().Location().Z() - 20) < 1e-8) top = {{"body", ub}, {"kind", "face"}, {"index", index}};
    }
    Frame frame = resolve_plane(up, us, {{"face", top}});
    frame.origin = {0, 0, 20};
    // 8.9 mm apart: the 9 mm counterbores overlap.
    Sketch close;
    for (double x : {-4.45, 4.45})
      for (double y : {-4.45, 4.45}) {
        SkEntity e;
        e.type = SkEntity::Type::Point;
        e.p = {close.add_point(x, y)};
        e.id = close.next_id();
        close.entities.push_back(e);
      }
    const std::string ts = run_id(sketch_cmd(up, close, {{"face", top}, {"frame", frame.to_json()}}));
    json onTop = json::array();
    for (const auto& p : close.points) onTop.push_back({{"sketch", ts}, {"point", p.id}});
    for (const char* type : {"simple", "counterbore", "countersink"}) {
      Document d = up;
      feature_cmd(d, "hole", {{"points", onTop}, {"type", type}, {"diameter", "5 mm"}, {"depth", "10 mm"}, {"cb_diameter", "9 mm"}, {"cb_depth", "3 mm"}});
      CHECK(total_volume(d) < 8000 - 4 * M_PI * 6.25 * 10 + 1e-3);
    }
  }

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

// UI-71: moving, scaling or fading a backdrop stores only the fields that changed (a nudge stored the whole picture again),
// a regeneration's result leaves the pictures out, and replay brings them back from the sketch as given.
TEST(sketch_backdrop_edits_never_store_the_picture_again) {
  Document doc = Document::create();
  commands::run("param", {{"name", "width"}, {"expr", "40 mm"}}, &doc);
  Sketch sk = rectangle(0, 0, 40, 20, "width");
  const std::string picture(200000, 'A');  // a photo's base64 (never decoded here)
  sk.images.push_back({{"id", sk.next_id()}, {"name", "photo.jpg"}, {"data", picture}, {"position", {0, 0}}, {"width", 50}, {"height", 25}, {"opacity", 0.5}});
  const std::string id = run_id(sketch_cmd(doc, sk));
  auto grown = [&, size = doc.serialize().size()]() mutable { const size_t now = doc.serialize().size(), by = now - size; size = now; return by; };
  auto nudge = [&](double x, double y) {
    const json before = resolve(doc).sketch(id)->geometry;
    json after = before;
    after["images"][0]["position"] = {x, y};
    after["images"][0]["opacity"] = 0.7;
    after["images"][0].erase("name");
    const json delta = sketch_delta(before, after);
    CHECK(!delta.contains("images"));
    CHECK_EQ(delta["image_fields"].size(), size_t(1));
    CHECK(!delta["image_fields"][0].contains("data") && !delta["image_fields"][0].contains("width"));
    CHECK(!before["images"][0].contains("name") || delta["image_fields"][0].at("name").is_null());
    CHECK(apply_sketch_delta(before, delta) == after);
    design::apply_ops(doc, {make_edit_op(id, {{"geometry_delta", delta}})});
  };
  nudge(5, 2);
  CHECK(grown() < 1024);  // was the whole picture again
  json shown = resolve(doc).sketch(id)->geometry;
  CHECK(shown["images"][0]["data"] == picture);
  CHECK(shown["images"][0]["position"] == json({5, 2}));
  // A parameter re-solves the sketch: its result has the new points, never the pictures.
  commands::run("param", {{"name", "width"}, {"expr", "60 mm"}}, &doc);
  CHECK(grown() < 8192);
  const Op& regen = doc.ops.back();
  CHECK_EQ(regen.type, std::string("regen"));
  CHECK(regen.data["results"][id].contains("geometry") && !regen.data["results"][id]["geometry"].contains("images"));
  shown = resolve(doc).sketch(id)->geometry;
  CHECK(shown["images"][0]["data"] == picture);
  CHECK_NEAR(shown["points"][1]["x"].get<double>(), 60, 1e-9);
  // Moved again after the regeneration: the solved points stay, the picture is still stored once.
  nudge(-3, 1);
  CHECK(grown() < 1024);
  shown = resolve(Document::parse(doc.serialize())).sketch(id)->geometry;
  CHECK(shown["images"][0]["position"] == json({-3, 1}));
  CHECK_NEAR(shown["points"][1]["x"].get<double>(), 60, 1e-9);
  size_t copies = 0;
  for (size_t at = 0; (at = doc.serialize().find(picture, at)) != std::string::npos; at += picture.size()) ++copies;
  CHECK_EQ(copies, size_t(1));
  // Another picture is a whole record; what an older build kept of field changes (a sketch key) is dropped on replay.
  json before = shown, after = shown;
  after["images"][0]["data"] = std::string(1000, 'B');
  CHECK_EQ(sketch_delta(before, after)["images"].size(), size_t(1));
  CHECK(!sketch_delta(before, after).contains("image_fields"));
  before["image_fields"] = json::array({{{"id", 99}, {"width", 1}}});
  CHECK(!apply_sketch_delta(before, json::object()).contains("image_fields"));
  CHECK_THROWS(apply_sketch_delta(shown, {{"image_fields", json::array({{{"id", 99}, {"width", 1}}})}}));
  // What the viewport compares on every sync: the picture by its samples, never its bytes; a move, a fade or another picture
  // of the same length (bytes in the middle) changes it.
  const std::string stamp = geometry_stamp(shown);
  CHECK(stamp.size() < 2048 && stamp.find(std::string(64, 'A')) == std::string::npos);
  CHECK(geometry_stamp(shown) == stamp);
  json moved = shown, faded = shown, other = shown;
  moved["images"][0]["position"] = {-3, 2};
  faded["images"][0]["opacity"] = 0.2;
  std::string middle = picture;
  middle[picture.size() / 2] = 'B';
  other["images"][0]["data"] = middle;
  CHECK(geometry_stamp(moved) != stamp && geometry_stamp(faded) != stamp && geometry_stamp(other) != stamp);
  shown["points"][1]["x"] = 61;
  CHECK(geometry_stamp(shown) != stamp);
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
// TODO 11 P2: the arrows a feature's panel shows, where the help guides draw them.
TEST(feature_handles_sit_where_the_guides_draw_them) {
  Document doc = Document::create();
  commands::run("param", {{"name", "r"}, {"expr", "3 mm"}}, &doc);
  feature_cmd(doc, "box", {{"length", "40 mm"}, {"width", "30 mm"}, {"height", "20 mm"}, {"centered", false}});
  const Scene s = resolve(doc);
  const std::string body = s.all_bodies().front();
  const TopoDS_Shape shape = node_world_shape(doc, s, body);
  // The top edge along X at y = 0, z = 20, and the top face.
  int edge = -1, top = -1;
  TopTools_IndexedMapOfShape edges, faces;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  for (int i = 1; i <= edges.Extent(); ++i) {
    BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
    const gp_Pnt a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
    if (std::abs(a.Y()) < 1e-9 && std::abs(b.Y()) < 1e-9 && std::abs(a.Z() - 20) < 1e-9 && std::abs(b.Z() - 20) < 1e-9) edge = i - 1;
  }
  for (int i = 1; i <= faces.Extent(); ++i) {
    BRepAdaptor_Surface f(TopoDS::Face(faces(i)));
    if (f.GetType() == GeomAbs_Plane && std::abs(f.Plane().Location().Z() - 20) < 1e-9 && std::abs(f.Plane().Axis().Direction().Z()) > 0.99) top = i - 1;
  }
  CHECK(edge >= 0);
  CHECK(top >= 0);
  const json edgeRef = {{"body", body}, {"kind", "edge"}, {"index", edge}}, faceRef = {{"body", body}, {"kind", "face"}, {"index", top}};
  auto close = [](const json& v, double x, double y, double z) {
    return std::abs(v[0].get<double>() - x) < 1e-6 && std::abs(v[1].get<double>() - y) < 1e-6 && std::abs(v[2].get<double>() - z) < 1e-6;
  };
  // Fillet: half way along the edge, out between the top and the front face, the radius as its expression evaluates.
  json h = feature_handles(doc, s, "fillet", {{"edges", json::array({edgeRef})}, {"radius", "r"}});
  CHECK_EQ(h.size(), size_t(1));
  CHECK_EQ(h[0]["input"].get<std::string>(), std::string("radius"));
  CHECK(close(h[0]["origin"], 20, 0, 20));
  CHECK(close(h[0]["axis"], 0, -std::sqrt(0.5), std::sqrt(0.5)));
  CHECK_NEAR(h[0]["value"].get<double>(), 3, 1e-9);
  h = feature_handles(doc, s, "chamfer", {{"edges", json::array({edgeRef})}, {"type", "equal"}, {"distance", "2 mm"}});
  CHECK(h.size() == 1 && h[0]["input"] == "distance" && close(h[0]["axis"], 0, -std::sqrt(0.5), std::sqrt(0.5)));
  // Press pull: the top face's centre, up; thicken: up, down with Other side.
  h = feature_handles(doc, s, "offset_face", {{"faces", json::array({faceRef})}, {"distance", "-4 mm"}});
  CHECK(h.size() == 1 && close(h[0]["origin"], 20, 15, 20) && close(h[0]["axis"], 0, 0, 1));
  CHECK_NEAR(h[0]["value"].get<double>(), -4, 1e-9);
  h = feature_handles(doc, s, "thicken", {{"faces", json::array({faceRef})}, {"thickness", "2 mm"}, {"flip", true}});
  CHECK(h.size() == 1 && h[0]["input"] == "thickness" && close(h[0]["origin"], 20, 15, 20) && close(h[0]["axis"], 0, 0, -1));
  // An offset plane from XY; a cone's height on XZ at x 5; a box made corner to corner.
  h = feature_handles(doc, s, "plane", {{"mode", "offset"}, {"plane", {{"base", "xy"}}}, {"distance", "10 mm"}});
  CHECK(h.size() == 1 && close(h[0]["origin"], 0, 0, 0) && close(h[0]["axis"], 0, 0, 1) && h[0]["value"] == 10.0);
  CHECK(feature_handles(doc, s, "plane", {{"mode", "angle"}, {"plane", {{"base", "xy"}}}}).empty());
  // From the top face: off its middle (the guide's arrow), not off the corner its frame starts at.
  h = feature_handles(doc, s, "plane", {{"mode", "offset"}, {"plane", {{"face", faceRef}}}, {"distance", "8 mm"}});
  CHECK(h.size() == 1 && close(h[0]["origin"], 20, 15, 20) && close(h[0]["axis"], 0, 0, 1) && h[0]["value"] == 8.0);
  h = feature_handles(doc, s, "box", {{"plane", {{"base", "xy"}}}, {"x", "5 mm"}, {"y", "0 mm"}, {"length", "10 mm"}, {"width", "6 mm"}, {"height", "-8 mm"}, {"centered", false}});
  CHECK(h.size() == 1 && h[0]["input"] == "height" && close(h[0]["origin"], 10, 3, 0) && close(h[0]["axis"], 0, 0, 1) && h[0]["value"] == -8.0);
  // A move that turns: the ring's axis (the edge's line here) and the angle; none without Rotate.
  h = feature_handles(doc, s, "move", {{"bodies", json::array({{{"body", body}, {"kind", "body"}}})}, {"rotate", true}, {"axis", {{"edge", edgeRef}}}, {"angle", "30 deg"}});
  CHECK(h.size() == 1 && h[0]["input"] == "angle" && h[0].value("ring", false) && std::abs(std::abs(h[0]["axis"][0].get<double>()) - 1) < 1e-9);
  CHECK(std::abs(h[0]["origin"][1].get<double>()) < 1e-9 && std::abs(h[0]["origin"][2].get<double>() - 20) < 1e-9);
  CHECK_NEAR(h[0]["value"].get<double>(), M_PI / 6, 1e-9);
  CHECK(feature_handles(doc, s, "move", {{"bodies", json::array({{{"body", body}, {"kind", "body"}}})}, {"rotate", false}, {"axis", {{"base", "z"}}}}).empty());
  // Nothing to show: no pick yet, a stale pick, a kind without a handle.
  CHECK(feature_handles(doc, s, "fillet", {{"edges", json::array()}, {"radius", "2 mm"}}).empty());
  CHECK(feature_handles(doc, s, "fillet", {{"edges", json::array({{{"body", "nobody"}, {"kind", "edge"}, {"index", 0}}})}, {"radius", "2 mm"}}).empty());
  CHECK(feature_handles(doc, s, "shell", {{"faces", json::array({faceRef})}, {"thickness", "1 mm"}}).empty());
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

// A new sketch's frame as committed (plan_ops + commit, as the sketch editor's Finish does) is the frame the plane picker
// resolved and previewed: on a face with its origin typed (uv), left where it starts (world), on a vertex (ref), and on the
// face picked first (no origin step).
TEST(new_sketch_commits_the_frame_the_picker_resolved) {
  Document doc=Document::create();
  feature_cmd(doc,"box",{{"length","40 mm"},{"width","30 mm"},{"height","20 mm"}});  // centred: its corners away from the origin
  const auto scene=resolve(doc);const auto body=scene.all_bodies().front();
  const auto shape=node_world_shape(doc,scene,body);json top;int index=0;
  Bnd_Box box;BRepBndLib::Add(shape,box);const double zTop=box.CornerMax().Z();
  for(TopExp_Explorer faces(shape,TopAbs_FACE);faces.More();faces.Next(),++index){
    BRepAdaptor_Surface surface(TopoDS::Face(faces.Current()));
    if(surface.GetType()==GeomAbs_Plane && std::abs(surface.Plane().Location().Z()-zTop)<1e-6 && std::abs(surface.Plane().Axis().Direction().Z())>.99)
      top={{"body",body},{"kind","face"},{"index",index}};
  }
  CHECK(!top.is_null());
  const json support={{"face",make_ref(doc,scene,Ref::from_json(top))}};
  auto check_commit=[&](const json& picked){
    const Frame previewed=resolve_plane(doc,scene,picked);
    json plane=picked;plane["frame"]=previewed.to_json();
    Document d=doc;
    commit(d,plan_ops(d,{make_sketch_op("S",plane,rectangle(0,0,5,5).to_json())}),"test");
    const Frame made=resolve(d).sketches.back().frame;
    for(int i=0;i<3;++i){CHECK_NEAR(made.origin[i],previewed.origin[i],1e-7);CHECK_NEAR(made.x[i],previewed.x[i],1e-9);CHECK_NEAR(made.y[i],previewed.y[i],1e-9);}
    return made;
  };
  // The picker's origin step starts at the face's own origin, its lower-left corner (uv 0, 0), as the face picked first does.
  const Frame corner=check_commit({{"support",support},{"origin",{{"uv",{0,0}}}}});
  const Frame first=check_commit(support);
  CHECK_NEAR(corner.origin[0],-20,1e-7);CHECK_NEAR(corner.origin[1],-15,1e-7);CHECK_NEAR(corner.origin[2],20,1e-7);
  for(int i=0;i<3;++i)CHECK_NEAR(first.origin[i],corner.origin[i],1e-7);
  const Frame typed=check_commit({{"support",support},{"origin",{{"uv",{5,7}}}}});
  CHECK_NEAR(typed.origin[0],-15,1e-7);CHECK_NEAR(typed.origin[1],-8,1e-7);
  // Sketches made before keep their stored origin: under the world origin, or on a vertex.
  const Frame world=check_commit({{"support",support},{"origin",{{"world",{0,0,0}}}}});
  CHECK_NEAR(world.origin[0],0,1e-7);CHECK_NEAR(world.origin[1],0,1e-7);CHECK_NEAR(world.origin[2],20,1e-7);
  check_commit({{"support",support},{"origin",{{"ref",make_ref(doc,scene,Ref{body,Ref::Kind::Vertex,0})}}}});
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

namespace {

json at_height(double z) { return {{"origin", {0, 0, z}}, {"normal", {0, 0, 1}}}; }

json square_profile(Document& doc, double z, double x = -5, double y = -5, double size = 10) {
  const std::string sketch = run_id(sketch_cmd(doc, rectangle(x, y, size, size), at_height(z)));
  return json::array({json{{"sketch", sketch}, {"at", {x + size / 2, y + size / 2}}}});
}

// The face (as "uuid/face/N") of a body that `want` picks from its description.
std::string face_where(const Document& doc, const std::string& body, const std::function<bool(const json&)>& want) {
  const Scene s = resolve(doc);
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(node_world_shape(doc, s, body), TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i)
    if (want(describe_entity(faces(i)))) return body + "/face/" + std::to_string(i - 1);
  return {};
}

// The solid a planned extrusion adds or removes (Plan::tools) and the operation it was taken as.
std::pair<std::string, double> planned_tool(const Document& doc, const json& inputs, const std::string& component = {}) {
  json op = make_feature_op("extrude", "Probe", inputs);
  if (!component.empty()) op["component"] = component;
  const Plan p = plan_ops(doc, {op});
  if (p.tools.size() != 1 || !p.tools[0].shape) return {"", 0};
  GProp_GProps g;
  BRepGProp::VolumeProperties(*p.tools[0].shape, g);
  return {p.tools[0].operation, g.Mass()};
}

double top_of(const TopoDS_Shape& s) {
  Bnd_Box b;
  BRepBndLib::AddOptimal(s, b, Standard_False, Standard_False);
  double x0, y0, z0, x1, y1, z1;
  b.Get(x0, y0, z0, x1, y1, z1);
  return z1;
}

}  // namespace

// Fusion's automatic operation: an extrusion into material cuts, one away from a face it sits on (or touching a body)
// joins, one in the clear makes a new body. "auto" is written into the op as the operation it was taken as; a feature made
// in a component looks only at that component's bodies, for the choice and for the automatic targets.
TEST(extrude_automatic_operation) {
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}});  // x, y -10..10, z 0..10
  double v = total_volume(doc);
  auto extrude = [&](const json& profile, json more) {
    more["profiles"] = profile;
    more["operation"] = "auto";
    const json made = feature_cmd(doc, "extrude", more);
    const Scene s = resolve(doc);
    const Feature* f = s.feature(made["feature_id"]);
    CHECK(f && f->error.empty());
    return f ? f->inputs.value("operation", std::string()) : std::string();
  };
  // From the box's bottom (on XY) up into it: a cut, written into the op.
  CHECK_EQ(extrude(square_profile(doc, 0), {{"distance", "4 mm"}}), std::string("cut"));
  CHECK_EQ(resolve(doc).all_bodies().size(), 1u);
  CHECK_NEAR(total_volume(doc), v - 400, 1e-6);
  v = total_volume(doc);
  // On the top face, away from it: a join; flipped, into the box: a cut.
  CHECK_EQ(extrude(square_profile(doc, 10), {{"distance", "5 mm"}}), std::string("join"));
  CHECK_EQ(resolve(doc).all_bodies().size(), 1u);
  CHECK_NEAR(total_volume(doc), v + 500, 1e-6);
  v = total_volume(doc);
  CHECK_EQ(extrude(square_profile(doc, 10, 4, 4, 4), {{"distance", "3 mm"}, {"flip", true}}), std::string("cut"));
  CHECK_NEAR(total_volume(doc), v - 48, 1e-6);
  v = total_volume(doc);
  // Half in, half out (symmetric about the top face): not mostly into material, so a join.
  CHECK_EQ(extrude(square_profile(doc, 10, -9, -9, 3), {{"distance", "4 mm"}, {"direction", "symmetric"}}), std::string("join"));
  CHECK_NEAR(total_volume(doc), v + 18, 1e-6);
  // In the clear: a new body.
  CHECK_EQ(extrude(square_profile(doc, 40), {{"distance", "5 mm"}}), std::string("new"));
  CHECK_EQ(resolve(doc).all_bodies().size(), 2u);
  // The plan hands the preview the tool and what it is for.
  const auto [op, volume] = planned_tool(doc, {{"profiles", square_profile(doc, 0, -8, -8, 2)}, {"distance", "2 mm"}, {"operation", "auto"}});
  CHECK_EQ(op, std::string("cut"));
  CHECK_NEAR(volume, 8, 1e-6);
  // Made in a component: the root's box is not its material. Auto makes a new body there, an explicit cut finds nothing.
  const std::string lid = commands::run("component", {{"name", "Lid"}}, &doc)["component_id"];
  const json low = square_profile(doc, 0, 5, 5, 3);
  CHECK_EQ(planned_tool(doc, {{"profiles", low}, {"distance", "2 mm"}, {"operation", "auto"}}, lid).first, std::string("new"));
  CHECK_EQ(planned_tool(doc, {{"profiles", low}, {"distance", "2 mm"}, {"operation", "auto"}}).first, std::string("cut"));
  bool refused = false;
  try {
    commands::run("feature", {{"kind", "extrude"}, {"inputs", {{"profiles", low}, {"distance", "2 mm"}, {"operation", "cut"}}}, {"component", lid}}, &doc);
  } catch (const Error& e) {
    refused = std::string(e.what()).find("does not touch any body") != std::string::npos;
  }
  CHECK(refused);
  // An op of another creation feature holding "auto" (only the extrusion offers it; written by hand, or by a newer build)
  // is decided from its tool's overlap: a box mostly inside the first one cuts.
  const Plan cube = plan_ops(doc, {make_feature_op("box", "Cube", {{"length", "2 mm"}, {"width", "2 mm"}, {"height", "2 mm"}, {"x", "-6 mm"}, {"y", "6 mm"}, {"operation", "auto"}})});
  CHECK_EQ(cube.tools.size(), 1u);
  CHECK_EQ(cube.tools[0].operation, std::string("cut"));
  CHECK_EQ(cube.ops[0]["inputs"]["operation"], json("cut"));
}

// To all goes through every body in the way and ends where the last one ends (on each side when two-sided), for a cut and
// for a join, instead of running on for twice the model's size; with nothing in the way it is refused.
TEST(extrude_to_all_ends_at_the_last_body_in_the_way) {
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}});                           // z 0..10
  feature_cmd(doc, "box", {{"plane", at_height(20)}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "5 mm"}});  // z 20..25
  const json below = square_profile(doc, -5);
  // The tool ends at z = 25 (the top of the far box), not past the model.
  Plan p = plan_ops(doc, {make_feature_op("extrude", "All", {{"profiles", below}, {"extent", "all"}, {"operation", "cut"}})});
  CHECK_EQ(p.tools.size(), 1u);
  CHECK_NEAR(top_of(*p.tools[0].shape), 25, 1e-4);
  // A join fills up to the farthest material and joins both boxes into one body.
  {
    Document joined = doc;
    feature_cmd(joined, "extrude", {{"profiles", below}, {"extent", "all"}, {"operation", "join"}});
    const Scene s = resolve(joined);
    CHECK_EQ(s.all_bodies().size(), 1u);
    CHECK_NEAR(total_volume(joined), 4000 + 2000 + 3000 - 1000 - 500, 1e-4);
  }
  // A cut goes through both.
  const double v = total_volume(doc);
  feature_cmd(doc, "extrude", {{"profiles", below}, {"extent", "all"}, {"operation", "cut"}});
  CHECK_NEAR(total_volume(doc), v - 1000 - 500, 1e-4);
  // Two ways from between the boxes: 13 mm up to the top of one, 12 mm down to the bottom of the other; symmetric: the
  // farther both ways.
  const json between = square_profile(doc, 12, -9, -9, 3);
  p = plan_ops(doc, {make_feature_op("extrude", "Both", {{"profiles", between}, {"extent", "all"}, {"direction", "two"}, {"operation", "cut"}})});
  {
    Bnd_Box b;
    BRepBndLib::AddOptimal(*p.tools.at(0).shape, b, Standard_False, Standard_False);
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    CHECK_NEAR(z0, 0, 1e-4);
    CHECK_NEAR(z1, 25, 1e-4);
  }
  p = plan_ops(doc, {make_feature_op("extrude", "Sym", {{"profiles", between}, {"extent", "all"}, {"direction", "symmetric"}, {"operation", "cut"}})});
  {
    Bnd_Box b;
    BRepBndLib::AddOptimal(*p.tools.at(0).shape, b, Standard_False, Standard_False);
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    CHECK_NEAR(z0, -1, 1e-4);
    CHECK_NEAR(z1, 25, 1e-4);
  }
  // Above everything: nothing ahead (refused, saying so); flipped, down through both.
  const json above = square_profile(doc, 40, -9, 6, 3);
  bool refused = false;
  try {
    feature_cmd(doc, "extrude", {{"profiles", above}, {"extent", "all"}, {"operation", "cut"}});
  } catch (const Error& e) {
    refused = std::string(e.what()).find("nothing lies ahead of the profile") != std::string::npos;
  }
  CHECK(refused);
  const double w = total_volume(doc);
  feature_cmd(doc, "extrude", {{"profiles", above}, {"extent", "all"}, {"operation", "cut"}, {"flip", true}});
  CHECK_NEAR(total_volume(doc), w - 9 * 10 - 9 * 5, 1e-4);
}

// To face as Fusion's To object: a curved face, extended past its edges unless Extend is off; an offset past or short of the
// target; the side the target is on found by itself; a vertex, an origin plane or a construction plane as the target; a
// plane the extrusion runs along refused.
TEST(extrude_to_face_extends_offsets_and_finds_its_side) {
  Document doc = Document::create();
  // A slab z 20..25 and a short cylinder lying along X above the origin: x 0..20, axis at z 40, radius 10 (bottom at z 30).
  const std::string slab = feature_cmd(doc, "box", {{"plane", at_height(20)}, {"length", "80 mm"}, {"width", "80 mm"}, {"height", "5 mm"}})["body_ids"][0];
  const std::string bar = feature_cmd(doc, "cylinder", {{"plane", {{"origin", {0, 0, 40}}, {"normal", {1, 0, 0}}}}, {"diameter", "20 mm"}, {"height", "20 mm"}})["body_ids"][0];
  const std::string round = face_where(doc, bar, [](const json& d) { return d.value("surface", "") == "cylinder"; });
  const std::string slab_top = face_where(doc, slab, [](const json& d) { return d.contains("normal") && d["normal"][2].get<double>() > 0.99; });
  const std::string slab_under = face_where(doc, slab, [](const json& d) { return d.contains("normal") && d["normal"][2].get<double>() < -0.99; });
  CHECK(!round.empty() && !slab_top.empty() && !slab_under.empty());
  // From z = 26 (above the slab) up to the round face: x -5..5 is only half under the bar, so the face is extended along it.
  const json profile = square_profile(doc, 26);
  const double band = 2 * (2.5 * std::sqrt(75.0) + 50 * std::asin(0.5));  // the integral of sqrt(100 - y^2) over -5..5
  auto tool = [&](json inputs) {
    inputs["profiles"] = inputs.value("profiles", profile);
    return planned_tool(doc, inputs).second;
  };
  CHECK_NEAR(tool({{"extent", "to_face"}, {"extent_face", json::array({round})}}), 10 * (10 * 14 - band), 1e-3);
  bool refused = false;
  try {
    tool({{"extent", "to_face"}, {"extent_face", json::array({round})}, {"extend", false}});
  } catch (const Error& e) {
    refused = std::string(e.what()).find("Extend the face") != std::string::npos;
  }
  CHECK(refused);
  // An offset moves the end along the extrusion: past the target, or short of it.
  const json under = square_profile(doc, 0);
  CHECK_NEAR(tool({{"profiles", under}, {"extent", "to_face"}, {"extent_face", json::array({slab_under})}, {"extent_offset", "-2 mm"}}), 100 * 18, 1e-4);
  CHECK_NEAR(tool({{"profiles", under}, {"extent", "to_body"}, {"extent_body", json::array({slab})}, {"extent_offset", "3 mm"}}), 100 * 23, 1e-4);
  // The target behind the profile: it goes that way (from z = 26 down to the slab's top, 1 mm).
  CHECK_NEAR(tool({{"extent", "to_face"}, {"extent_face", json::array({slab_top})}}), 100, 1e-4);
  // A vertex (the plane through it, parallel to the profile), an origin plane, a construction plane.
  const std::string corner = slab + "/vertex/0";
  const double corner_z = [&] {
    const Scene s = resolve(doc);
    TopTools_IndexedMapOfShape vertices;
    TopExp::MapShapes(node_world_shape(doc, s, slab), TopAbs_VERTEX, vertices);
    return BRep_Tool::Pnt(TopoDS::Vertex(vertices(1))).Z();
  }();
  CHECK_NEAR(tool({{"profiles", under}, {"extent", "to_face"}, {"extent_face", json::array({corner})}}), 100 * corner_z, 1e-4);
  CHECK_NEAR(tool({{"extent", "to_face"}, {"extent_face", json::array({{{"base", "xy"}}})}}), 100 * 26, 1e-4);
  const std::string plane = feature_cmd(doc, "plane", {{"mode", "offset"}, {"plane", {{"base", "xy"}}}, {"distance", "12 mm"}})["feature_id"];
  CHECK_NEAR(tool({{"profiles", under}, {"extent", "to_face"}, {"extent_face", json::array({{{"feature", plane}}})}}), 100 * 12, 1e-4);
  refused = false;
  try {
    tool({{"extent", "to_face"}, {"extent_face", json::array({{{"base", "xz"}}})}});
  } catch (const Error& e) {
    refused = std::string(e.what()).find("parallel") != std::string::npos;
  }
  CHECK(refused);
}

// What used to end in the kernel's own message ("the modelling kernel failed: BRep_API: command not done") or in a wrong
// result with To face and To all: a profile on the target's plane, a profile flush with a body's faces (coincident start and
// side faces, through all), two-sided and symmetric To all from a face, a round target tangent to the extrusion's sides, a
// tilted plane through the profile, a profile wider than a round target. Each either works or says why in words.
TEST(extrude_up_to_and_through_all_with_touching_geometry) {
  auto words = [](const std::function<void()>& f) {
    try {
      f();
    } catch (const Error& e) {
      return std::string(e.what());
    }
    return std::string();
  };
  auto plain = [](const std::string& why) { return !why.empty() && why.find("kernel") == std::string::npos && why.find("BRep") == std::string::npos; };
  Document doc = Document::create();
  feature_cmd(doc, "box", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}});  // x, y -10..10, z 0..10
  const std::string slab = feature_cmd(doc, "box", {{"plane", at_height(20)}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "5 mm"}})["body_ids"][0];
  const std::string under = face_where(doc, slab, [](const json& d) { return d.contains("normal") && d["normal"][2].get<double>() < -0.99; });
  // On the target's own plane: refused in words.
  std::string why = words([&] { feature_cmd(doc, "extrude", {{"profiles", square_profile(doc, 20)}, {"extent", "to_face"}, {"extent_face", json::array({under})}}); });
  CHECK(plain(why) && why.find("lies on that plane") != std::string::npos);
  // From the box's top face down through all (the tool's start face on the box's face, one side flush with its side).
  double v = total_volume(doc);
  feature_cmd(doc, "extrude", {{"profiles", square_profile(doc, 10, -10, -3, 6)}, {"extent", "all"}, {"flip", true}, {"operation", "cut"}});
  CHECK_NEAR(total_volume(doc), v - 36 * 10, 1e-4);
  // Two-sided To all from the top face: down through the box, up through the slab (z 20..25). Symmetric: the farther of
  // the two (15 up to the slab's top) both ways.
  v = total_volume(doc);
  feature_cmd(doc, "extrude", {{"profiles", square_profile(doc, 10, 4, 4, 3)}, {"extent", "all"}, {"direction", "two"}, {"operation", "cut"}});
  CHECK_NEAR(total_volume(doc), v - 9 * 10 - 9 * 5, 1e-4);
  {
    const Plan p = plan_ops(doc, {make_feature_op("extrude", "Sym", {{"profiles", square_profile(doc, 10, 4, -8, 3)}, {"extent", "all"}, {"direction", "symmetric"}, {"operation", "cut"}})});
    Bnd_Box b;
    BRepBndLib::AddOptimal(*p.tools.at(0).shape, b, Standard_False, Standard_False);
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    CHECK_NEAR(z0, -5, 1e-4);
    CHECK_NEAR(z1, 25, 1e-4);
  }
  // A bar lying along X (axis z 40, radius 10) exactly as wide as the profile: its round face is tangent to the extrusion's
  // sides. The extrusion ends under it.
  Document round = Document::create();
  const std::string bar = feature_cmd(round, "cylinder", {{"plane", {{"origin", {-20, 0, 40}}, {"normal", {1, 0, 0}}}}, {"diameter", "20 mm"}, {"height", "40 mm"}})["body_ids"][0];
  const std::string face = face_where(round, bar, [](const json& d) { return d.value("surface", "") == "cylinder"; });
  const double before = total_volume(round);
  feature_cmd(round, "extrude", {{"profiles", square_profile(round, 26, -5, -10, 20)}, {"extent", "to_face"}, {"extent_face", json::array({face})}});
  CHECK_NEAR(total_volume(round) - before, 20 * (14 * 20 - 50 * M_PI), 1e-2);
  // Wider than the bar: part of it never meets the round face, extended or not.
  why = words([&] { feature_cmd(round, "extrude", {{"profiles", square_profile(round, 26, -5, -12, 24)}, {"extent", "to_face"}, {"extent_face", json::array({face})}}); });
  CHECK(plain(why) && why.find("misses") != std::string::npos);
  // A tilted plane through the profile: it would end on both sides of it.
  why = words([&] {
    feature_cmd(round, "extrude", {{"profiles", square_profile(round, 26, -5, -5, 10)}, {"extent", "to_face"}, {"extent_face", json::array({{{"origin", {0, 0, 26}}, {"normal", {0.5, 0, 1}}}})}});
  });
  CHECK(plain(why) && why.find("cuts through the profile") != std::string::npos);
  // Up to a body the profile sits on: not reached that way, starting inside it the other way.
  why = words([&] { feature_cmd(doc, "extrude", {{"profiles", square_profile(doc, 25, -3, -3, 2)}, {"extent", "to_body"}, {"extent_body", json::array({slab})}}); });
  CHECK(plain(why));
}

namespace {

// The planned extrusion's tool: its volume and its lowest and highest point along Z.
struct Planned {
  std::string operation;
  double volume = 0, low = 0, high = 0;
};

Planned plan_extrude(const Document& doc, const json& inputs) {
  const Plan p = plan_ops(doc, {make_feature_op("extrude", "Probe", inputs)});
  Planned out;
  if (p.tools.size() != 1 || !p.tools[0].shape) return out;
  out.operation = p.tools[0].operation;
  GProp_GProps g;
  BRepGProp::VolumeProperties(*p.tools[0].shape, g);
  out.volume = g.Mass();
  Bnd_Box b;
  BRepBndLib::AddOptimal(*p.tools[0].shape, b, Standard_False, Standard_False);
  double x0, y0, x1, y1;
  b.Get(x0, y0, out.low, x1, y1, out.high);
  return out;
}

// A bar lying along X (x -20..20, axis at z 40, radius 10) and a slab tilted about Y: its lower face the plane through
// (0, 60, 30) with normal (0.5, 0, 1), clear of the bar (y 40..90).
struct Targets {
  Document doc = Document::create();
  std::string bar, round, slab, tilted;
  Targets() {
    bar = feature_cmd(doc, "cylinder", {{"plane", {{"origin", {-20, 0, 40}}, {"normal", {1, 0, 0}}}}, {"diameter", "20 mm"}, {"height", "40 mm"}})["body_ids"][0];
    round = face_where(doc, bar, [](const json& d) { return d.value("surface", "") == "cylinder"; });
    slab = feature_cmd(doc, "box", {{"plane", {{"origin", {0, 65, 30}}, {"normal", {0.5, 0, 1}}}}, {"length", "60 mm"}, {"width", "50 mm"}, {"height", "5 mm"}})["body_ids"][0];
    tilted = face_where(doc, slab, [](const json& d) { return d.contains("normal") && d["normal"][2].get<double>() < -0.8; });
  }
};

}  // namespace

// End at: a round or tilted target ends the extrusion following it, or flat where the extrusion first touches it (nearest
// contact) or where all of it has reached it (farthest contact); for New body, Join and Cut.
TEST(extrude_to_face_end_at_nearest_farthest_or_following) {
  Targets t;
  const json under = square_profile(t.doc, 26);  // x, y -5..5 under the bar
  const double band = 2 * (2.5 * std::sqrt(75.0) + 50 * std::asin(0.5));
  auto to = [&](const json& profile, const std::string& face, const char* end, const char* operation = "new") {
    return plan_extrude(t.doc, {{"profiles", profile}, {"extent", "to_face"}, {"extent_face", json::array({face})}, {"extent_end", end}, {"operation", operation}});
  };
  CHECK_NEAR(to(under, t.round, "follow_face").volume, 10 * (10 * 14 - band), 1e-3);
  CHECK_NEAR(to(under, t.round, "nearest_contact").volume, 100 * 4, 1e-4);                         // the bar's bottom, z 30
  CHECK_NEAR(to(under, t.round, "farthest_contact").volume, 100 * (14 - std::sqrt(75.0)), 1e-4);  // at y = 5, z 40 - sqrt(75)
  // The tilted plane z = 30 - 0.5 (x - 0) over y 55..65: from z 0, 27.5 to 32.5 over x -5..5.
  const json beside = square_profile(t.doc, 0, -5, 55, 10);
  CHECK_NEAR(to(beside, t.tilted, "follow_face").volume, 100 * 30, 1e-3);
  CHECK_NEAR(to(beside, t.tilted, "nearest_contact").volume, 100 * 27.5, 1e-3);
  CHECK_NEAR(to(beside, t.tilted, "farthest_contact").volume, 100 * 32.5, 1e-3);
  // Up to a body the same way.
  CHECK_NEAR(plan_extrude(t.doc, {{"profiles", under}, {"extent", "to_body"}, {"extent_body", json::array({t.bar})}, {"extent_end", "nearest_contact"}}).volume, 400, 1e-4);
  // A join at the farthest contact runs into the bar and becomes part of it.
  {
    Document joined = t.doc;
    feature_cmd(joined, "extrude", {{"profiles", under}, {"extent", "to_face"}, {"extent_face", json::array({t.round})}, {"extent_end", "farthest_contact"}, {"operation", "join"}});
    const Scene s = resolve(joined);
    CHECK(s.features.back().error.empty());
    CHECK_EQ(s.all_bodies().size(), 2u);  // the bar (joined) and the slab
  }
  // A cut from inside the bar (its axis plane) up to its round face: follow it, or flat at the nearest or farthest contact.
  const json inside = square_profile(t.doc, 40, -5, -3, 6);  // x -5..1, y -3..3
  const double bar = M_PI * 100 * 40;
  const double arch = 6 * (3 * std::sqrt(91.0) + 100 * std::asin(0.3));  // 6 x the integral of sqrt(100 - y^2) over -3..3
  // (the farthest contact pokes out of the bar beside the top: it takes the same as following the face)
  for (const auto& [end, removed] : {std::pair<const char*, double>{"follow_face", arch}, {"nearest_contact", 36 * std::sqrt(91.0)}, {"farthest_contact", arch}}) {
    Document cut = t.doc;
    feature_cmd(cut, "extrude", {{"profiles", inside}, {"extent", "to_face"}, {"extent_face", json::array({t.round})}, {"extent_end", end}, {"operation", "cut"}});
    const Scene s = resolve(cut);
    CHECK(s.features.back().error.empty());
    CHECK_NEAR(volume_of_node(cut, s, t.bar), bar - removed, 1e-2);
  }
}

// Start at: a round or tilted start face starts the extrusion following it (the distance runs from the face everywhere), or
// flat at its nearest or farthest contact; a planar face square to the axis is a start distance as before.
TEST(extrude_from_a_face_follow_nearest_farthest) {
  Targets t;
  const json under = square_profile(t.doc, 0);  // x, y -5..5 under the bar
  auto from = [&](const json& profile, const std::string& face, const char* shape, const char* offset = "0 mm") {
    return plan_extrude(t.doc, {{"profiles", profile}, {"start", "face"}, {"start_face", json::array({face})}, {"start_shape", shape}, {"face_offset", offset}, {"distance", "5 mm"}});
  };
  const double low = 40 - std::sqrt(75.0);  // where the bar's bottom is highest over the profile (y = 5)
  Planned p = from(under, t.round, "nearest_contact");
  CHECK_NEAR(p.volume, 500, 1e-4);
  CHECK_NEAR(p.low, 30, 1e-4);
  p = from(under, t.round, "farthest_contact");
  CHECK_NEAR(p.low, low, 1e-4);
  CHECK_NEAR(p.high, low + 5, 1e-4);
  p = from(under, t.round, "follow_face");
  CHECK_NEAR(p.volume, 500, 1e-3);  // the region swept 5 mm on from the face
  CHECK_NEAR(p.low, 30, 1e-4);
  CHECK_NEAR(p.high, low + 5, 1e-4);
  p = from(under, t.round, "nearest_contact", "2 mm");
  CHECK_NEAR(p.low, 32, 1e-4);
  // The tilted face: 27.5 to 32.5 over the profile.
  const json beside = square_profile(t.doc, 0, -5, 55, 10);
  p = from(beside, t.tilted, "nearest_contact");
  CHECK_NEAR(p.low, 27.5, 1e-4);
  p = from(beside, t.tilted, "farthest_contact");
  CHECK_NEAR(p.low, 32.5, 1e-4);
  p = from(beside, t.tilted, "follow_face");
  CHECK_NEAR(p.volume, 500, 1e-3);
  CHECK_NEAR(p.low, 27.5, 1e-4);
  CHECK_NEAR(p.high, 37.5, 1e-4);
  // Following goes one way.
  bool refused = false;
  try {
    feature_cmd(t.doc, "extrude", {{"profiles", under}, {"start", "face"}, {"start_face", json::array({t.round})}, {"direction", "symmetric"}});
  } catch (const Error& e) {
    refused = std::string(e.what()).find("goes one way") != std::string::npos;
  }
  CHECK(refused);
}

// Sketch on face: the profile projected onto a tilted start face's plane (offset along its normal) and extruded along that
// normal; derived_extrude_ops makes it a real sketch ("... (derived)", linked to its source) on a construction plane, and an
// extrusion from it giving the same solid, which follows the source sketch when that changes.
TEST(extrude_sketch_on_a_tilted_start_face) {
  Targets t;
  const std::string source = run_id(sketch_cmd(t.doc, rectangle(-5, 55, 10, 10)));
  const json profile = json::array({json{{"sketch", source}, {"at", {0, 60}}}});
  const double cosine = 1 / std::sqrt(1.25);  // the tilted plane's normal against Z
  json inputs = {{"profiles", profile}, {"start", "face"}, {"start_face", json::array({t.tilted})}, {"start_shape", "sketch_on_face"}, {"distance", "5 mm"}};
  const Planned direct = plan_extrude(t.doc, inputs);
  CHECK_NEAR(direct.volume, 100 * cosine * 5, 1e-4);
  inputs["face_offset"] = "3 mm";
  const Planned offset = plan_extrude(t.doc, inputs);
  CHECK_NEAR(offset.volume, direct.volume, 1e-4);
  CHECK_NEAR(offset.low - direct.low, 3 * cosine, 1e-4);  // moved 3 mm along the plane's normal
  // As the panel commits it: a construction plane, the derived sketch and the extrusion from it, in one step.
  const std::vector<json> ops = derived_extrude_ops(t.doc, resolve(t.doc), inputs, "Extrude1");
  CHECK_EQ(ops.size(), 3u);
  CHECK_EQ(ops[0]["kind"], json("plane"));
  CHECK_EQ(ops[1]["op"], json("sketch"));
  CHECK(ops[1]["name"].get<std::string>().find("(derived)") != std::string::npos);
  CHECK_EQ(ops[2]["inputs"]["start"], json("profile"));
  const double before = total_volume(t.doc);
  apply_ops(t.doc, ops);
  Scene s = resolve(t.doc);
  CHECK(s.features.back().error.empty());
  CHECK_NEAR(total_volume(t.doc) - before, offset.volume, 1e-4);
  // The source sketch made larger: the derived sketch and the extrusion follow.
  apply_ops(t.doc, {make_edit_op(source, {{"geometry", rectangle(-6, 54, 12, 12).to_json()}})});
  s = resolve(t.doc);
  CHECK(s.features.back().error.empty());
  CHECK_NEAR(total_volume(t.doc) - before, 144 * cosine * 5, 1e-3);
  // No offset: the sketch goes on the face itself, no construction plane.
  inputs["face_offset"] = "0 mm";
  CHECK_EQ(derived_extrude_ops(t.doc, resolve(t.doc), inputs, "Extrude2").size(), 2u);
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

// TODO 10 B10: reported sizes are the tight box: properties, components, measurements and validate.
TEST(reported_boxes_are_tight) {
  Document doc = Document::create();
  const std::string group = commands::run("component", {{"name", "Parts"}}, &doc)["component_id"];
  const std::string block = commands::run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "31 mm"}, {"height", "21 mm"}}}, {"parent", group}}, &doc)["body_ids"][0];
  const std::string pin = commands::run("feature", {{"kind", "cylinder"}, {"inputs", {{"x", "50 mm"}, {"diameter", "10 mm"}, {"height", "40 mm"}}}, {"parent", group}}, &doc)["body_ids"][0];
  const Scene s = resolve(doc);
  auto size = [](const json& box) { return std::array<double, 3>{box["size"][0].get<double>(), box["size"][1].get<double>(), box["size"][2].get<double>()}; };
  auto same = [](std::array<double, 3> a, std::array<double, 3> b) { return std::fabs(a[0] - b[0]) < 1e-6 && std::fabs(a[1] - b[1]) < 1e-6 && std::fabs(a[2] - b[2]) < 1e-6; };
  CHECK(same(size(node_properties(doc, s, block)["bbox"]), {60, 31, 21}));
  CHECK(same(size(node_properties(doc, s, pin)["bbox"]), {10, 10, 40}));  // the padded box of a cylinder is larger
  // The component: both bodies, x from -30 to 55, y from -15.5 to 15.5, z from 0 to 40.
  CHECK(same(size(node_properties(doc, s, group)["bbox"]), {85, 31, 40}));
  Ref pin_ref;
  pin_ref.body = pin;
  CHECK(same(size(measure_bbox(doc, s, {pin_ref})), {10, 10, 40}));
  CHECK(same(size(measure_bbox(doc, s, {})), {85, 31, 40}));
  const json validated = commands::run("validate", {{"select", json::array({pin})}}, &doc);
  CHECK(same(size(validated["items"][0]["bbox"]), {10, 10, 40}));
}

// TODO 10 B17: overlapping, touching and nearly touching boxes; instances of one shape through their placements.
TEST(interference_and_clearance_checks) {
  Document doc = Document::create();
  auto box_at = [&](double x, double y, double z) {
    return feature_cmd(doc, "box", {{"plane", {{"origin", {x, y, z}}, {"normal", {0, 0, 1}}}}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}})["body_ids"][0].get<std::string>();
  };
  const std::string a1 = box_at(0, 0, 0), b1 = box_at(8, 0, 0);        // overlap 2 x 10 x 10
  const std::string a2 = box_at(100, 0, 0), c2 = box_at(110, 0, 0);    // touching at x = 105
  const std::string a3 = box_at(200, 0, 0), d3 = box_at(210.3, 0, 0);  // 0.3 mm apart
  Scene s = resolve(doc);
  json plain = check_interference(doc, s, json::object());
  CHECK_EQ(plain["interferences"], 1);
  CHECK_EQ(plain["too_close"], 0);
  CHECK_EQ(plain["status"], "interference");
  const json& hit = plain["items"][0];
  CHECK(hit["kind"] == "interference" && ((hit["a"] == a1 && hit["b"] == b1) || (hit["a"] == b1 && hit["b"] == a1)));
  CHECK_NEAR(hit["volume_mm3"].get<double>(), 200, 1e-6);
  CHECK_NEAR(hit["bbox"]["size"][0].get<double>(), 2, 1e-9);
  // With a clearance, the touching pair (0 mm) and the near pair (0.3 mm) are too close; the overlap comes first.
  const json close = check_interference(doc, s, {{"clearance_mm", 0.5}});
  CHECK_EQ(close["interferences"], 1);
  CHECK_EQ(close["too_close"], 2);
  CHECK_EQ(close["items"][0]["kind"], "interference");
  CHECK_NEAR(close["items"][1]["distance_mm"].get<double>(), 0, 1e-6);
  CHECK_NEAR(close["items"][2]["distance_mm"].get<double>(), 0.3, 1e-6);
  // A pair meant to overlap is left out; so is everything outside the selection.
  const json ignored = check_interference(doc, s, {{"ignore", json::array({json::array({b1, a1})})}});
  CHECK_EQ(ignored["interferences"], 0);
  CHECK_EQ(ignored["ignored_pairs"], 1);
  CHECK_EQ(ignored["status"], "clear");
  CHECK_EQ(check_interference(doc, s, {{"select", json::array({a2, c2, a3, d3})}})["interferences"], 0);
  // Two instances of one shape: the same body entry placed twice, 4 mm apart, overlap by 6 x 10 x 10.
  const std::string key = s.node(a3)->body_key;
  const std::string twin = new_uuid();
  doc.append({{"op", "import"}, {"source", ""}, {"units", "mm"},
              {"nodes", json::array({{{"type", "body"}, {"id", twin}, {"name", "Twin"}, {"key", key}, {"transform", Mat4::translation(4, 0, 0).to_json()}}})}});
  s = resolve(doc);
  const json twins = check_interference(doc, s, {{"select", json::array({a3, twin})}});
  CHECK_EQ(twins["interferences"], 1);
  CHECK_NEAR(twins["items"][0]["volume_mm3"].get<double>(), 600, 1e-6);
  // Through validate, as agents call it.
  const json v = commands::run("validate", {{"checks", json::array({"interference"})}, {"clearance_mm", 0.5}}, &doc);
  CHECK(v.contains("interference") && !v.contains("items"));
  CHECK(v["interference"]["interferences"].get<int>() >= 2);
}

// TODO 10 B13: 3D-print checks: plate contact, overhangs against the build direction, thin walls and thin features.
TEST(print_checks) {
  Document doc = Document::create();
  // A plain block on the plate: 20 x 20 contact, nothing to report.
  const std::string block = feature_cmd(doc, "box", {{"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}})["body_ids"][0];
  Scene s = resolve(doc);
  json r = check_print(doc, s, {{"select", json::array({block})}});
  CHECK_EQ(r["status"], "clear");
  CHECK_NEAR(r["items"][0]["contact_area_mm2"].get<double>(), 400, 1e-6);
  // A T: a stem with a wide plate on top. The plate's underside past the stem faces straight down.
  const std::string stem = feature_cmd(doc, "box", {{"x", "100 mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}})["body_ids"][0];
  feature_cmd(doc, "box", {{"plane", {{"origin", {100, 0, 10}}, {"normal", {0, 0, 1}}}}, {"length", "30 mm"}, {"width", "10 mm"}, {"height", "2 mm"},
                           {"operation", "join"}, {"targets", json::array({stem})}});
  s = resolve(doc);
  r = check_print(doc, s, {{"select", json::array({stem})}});
  const json& t = r["items"][0];
  CHECK_NEAR(t["contact_area_mm2"].get<double>(), 100, 1e-6);  // only the stem's foot
  CHECK(!t["overhangs"].empty());
  double down = 0;
  for (const auto& o : t["overhangs"]) down = std::max(down, o["overhang_deg"].get<double>());
  CHECK_NEAR(down, 90, 1e-6);
  // Printed on its side (+x up) the same part has no downward face over the plate but the stem's end.
  CHECK(check_print(doc, s, {{"select", json::array({stem})}, {"build_direction", "-z"}})["items"][0]["contact_area_mm2"].get<double>() > 250);
  // A shell 0.5 mm thick: thin walls; a rib 0.4 mm wide: a thin feature.
  const std::string cup = feature_cmd(doc, "box", {{"x", "200 mm"}, {"length", "20 mm"}, {"width", "20 mm"}, {"height", "10 mm"}})["body_ids"][0];
  s = resolve(doc);
  int top = -1;
  {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(node_world_shape(doc, s, cup), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i)
      if (const json d = describe_entity(faces(i)); d.contains("normal") && d["normal"][2].get<double>() > 0.99) top = i - 1;
  }
  feature_cmd(doc, "shell", {{"faces", json::array({cup + "/face/" + std::to_string(top)})}, {"thickness", "0.5 mm"}});
  const std::string rib = feature_cmd(doc, "box", {{"x", "300 mm"}, {"length", "0.4 mm"}, {"width", "20 mm"}, {"height", "8 mm"}})["body_ids"][0];
  s = resolve(doc);
  r = check_print(doc, s, {{"select", json::array({cup, rib})}, {"min_wall_mm", 0.8}});
  CHECK_EQ(r["bodies"], 2);
  const json& shell = r["items"][0]["id"] == cup ? r["items"][0] : r["items"][1];
  const json& fin = r["items"][0]["id"] == rib ? r["items"][0] : r["items"][1];
  CHECK(!shell["thin_walls"].empty());
  CHECK_NEAR(shell["thin_walls"][0]["thickness_mm"].get<double>(), 0.5, 1e-3);
  CHECK(!fin["thin_features"].empty() || !fin["thin_walls"].empty());
  CHECK(r["findings"].get<int>() >= 2);
  // Through validate.
  const json v = commands::run("validate", {{"checks", json::array({"print"})}, {"select", json::array({block})}}, &doc);
  CHECK_EQ(v["print"]["status"], "clear");
}

// Gap log #1: a parameter edit must reach every body downstream, also through features that consume a changed body
// without naming the parameter (a move, a pattern, a combine), however the body is referenced.
TEST(parameter_edits_reach_bodies_through_moves_patterns_and_combines) {
  for (int form = 0; form < 3; ++form) {  // plain id, {"body"} object, the component holding it
    Document doc = Document::create();
    commands::run("param", {{"name", "zz_len"}, {"expr", "10 mm"}}, &doc);
    const std::string group = commands::run("component", {{"name", "Part"}}, &doc)["component_id"];
    const std::string body = commands::run("feature", {{"kind", "box"}, {"inputs", {{"length", "zz_len"}, {"width", "10 mm"}, {"height", "10 mm"}}}, {"parent", group}}, &doc)["body_ids"][0];
    const json ref = form == 0 ? json(body) : form == 1 ? json{{"body", body}, {"kind", "body"}} : json(group);
    feature_cmd(doc, "move", {{"bodies", json::array({ref})}, {"dz", "20 mm"}});
    const json row = feature_cmd(doc, "pattern_rect", {{"bodies", json::array({ref})}, {"count", "2"}, {"spacing", "50 mm"}});
    const std::string lid = feature_cmd(doc, "box", {{"x", "200 mm"}, {"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}})["body_ids"][0];
    feature_cmd(doc, "combine", {{"target", json::array({row["body_ids"][0]})}, {"tools", json::array({lid})}, {"operation", "join"}});
    commands::run("param", {{"name", "zz_len"}, {"expr", "30 mm"}}, &doc);
    const Scene s = resolve(doc);
    double x0, y0, z0, x1, y1, z1;
    node_tight_bbox(doc, s, body).Get(x0, y0, z0, x1, y1, z1);
    CHECK_NEAR(x1 - x0, 30, 1e-9);  // 10 before the fix: the move kept its first result
    CHECK_NEAR(z0, 20, 1e-9);
    const std::string copy = row["body_ids"][0];
    CHECK_NEAR(volume_of_node(doc, s, copy), 30 * 10 * 10 + 5 * 5 * 5, 1e-6);  // the copy follows, and so does the combine
    CHECK(s.unresolved.empty());
    // An incremental regeneration and a forced one agree.
    Document forced = doc;
    commands::run("regenerate", {{"force", true}}, &forced);
    const Scene f = resolve(forced);
    for (const auto& id : s.all_bodies()) CHECK_EQ(s.node(id)->body_key, f.node(id)->body_key);
  }
}

// Gap log #8: a profile named by its boundary, as sketch_details lists it, is that region; a boundary alone used to
// take every region (the whole disc instead of the ring between two circles).
TEST(a_profile_named_by_its_boundary_is_that_region) {
  Document doc = Document::create();
  const json circles = json::array({{{"kind", "circle"}, {"picks", {{0, 0}}}, {"options", {{"radius", 20.0}}}},
                                    {{"kind", "circle"}, {"picks", {{0, 0}}}, {"options", {{"radius", 8.0}}}}});
  const std::string sk = commands::run("sketch", {{"geometry", {{"shapes", circles}}}}, &doc)["sketch_id"];
  Scene s = resolve(doc);
  const json profiles = agent::sketch_details(doc, s, {{"sketch", sk}, {"section", "profiles"}});
  CHECK_EQ(profiles["total"], 2);
  json ring;
  for (const auto& item : profiles["items"])
    if (item["boundary"].size() == 2) ring = item["boundary"];
  CHECK(ring.is_array());
  auto extrude = [&](const json& boundary) {
    return feature_cmd(doc, "extrude", {{"profiles", json::array({{{"sketch", sk}, {"boundary", boundary}}})}, {"distance", "5 mm"}, {"operation", "new"}})["body_ids"][0].get<std::string>();
  };
  const std::string exact = extrude(ring);
  // In another order and without signs it still names one region.
  const std::string loose = extrude(json::array({std::abs(ring[1].get<int>()), std::abs(ring[0].get<int>())}));
  s = resolve(doc);
  CHECK_NEAR(volume_of_node(doc, s, exact), M_PI * (400 - 64) * 5, 1e-6);
  CHECK_NEAR(volume_of_node(doc, s, loose), M_PI * (400 - 64) * 5, 1e-6);
  // One no region has is refused, naming the ones there are.
  try {
    extrude(json::array({999}));
    CHECK(false);
  } catch (const Error& e) {
    CHECK(std::string(e.what()).find(ring.dump()) != std::string::npos);
  }
}

// Gap log #5: a closed fit spline is periodic (smooth through its seam) and the same curve wherever the seam falls
// and whichever way the points run; an open one takes end directions; a control-point spline needs only its degree.
TEST(closed_fit_splines_are_periodic_and_control_splines_need_only_a_degree) {
  const int n = 24;
  auto outline = [&](int shift, bool reversed, bool repeat_first) {
    Sketch sk;
    std::vector<int> ids;
    for (int i = 0; i < n; ++i) {
      const int k = ((reversed ? n - i : i) + shift) % n;
      const double t = 2 * M_PI * k / n, r = 10 + 2 * std::cos(3 * t);
      ids.push_back(sk.add_point(r * std::cos(t), r * std::sin(t)));
    }
    if (repeat_first) ids.push_back(sk.add_point(sk.point(ids.front())->x, sk.point(ids.front())->y));  // as its own point
    SkEntity e;
    e.type = SkEntity::Type::Spline;
    e.p = ids;
    e.periodic = true;
    e.id = sk.next_id();
    sk.entities.push_back(e);
    return sk;
  };
  auto area = [](const Sketch& sk) {
    const auto regions = sketch_regions(sk, {});
    CHECK_EQ(regions.size(), size_t(1));
    return area_properties(regions[0].face).mass;
  };
  const Sketch base = outline(0, false, false);
  const double a0 = area(base);
  CHECK_NEAR(area(outline(7, false, false)), a0, 1e-9 * a0);
  CHECK_NEAR(area(outline(0, true, false)), a0, 1e-9 * a0);
  CHECK_NEAR(area(outline(0, false, true)), a0, 1e-9 * a0);
  const Handle(Geom_BSplineCurve) closed = Handle(Geom_BSplineCurve)::DownCast(BRepAdaptor_Curve(entity_edge(base, base.entities[0], {})).Curve().Curve());
  CHECK(!closed.IsNull() && closed->IsPeriodic());
  CHECK(Sketch::from_json(base.to_json()).entities[0].periodic);
  // Open, leaving along +y and arriving along -x.
  json open = {{"points", {{{"id", 1}, {"x", 0}, {"y", 0}}, {{"id", 2}, {"x", 10}, {"y", 5}}, {{"id", 3}, {"x", 20}, {"y", 0}}}},
               {"entities", {{{"id", 10}, {"type", "spline"}, {"p", {1, 2, 3}}, {"start_tangent", {0, 1}}, {"end_tangent", {-1, 0}}}}}};
  const Sketch tangent = Sketch::from_json(open);
  BRepAdaptor_Curve c(entity_edge(tangent, tangent.entities[0], {}));
  gp_Pnt p;
  gp_Vec d;
  c.D1(c.FirstParameter(), p, d);
  CHECK(std::abs(d.X()) < 1e-9 && d.Y() > 0);
  c.D1(c.LastParameter(), p, d);
  CHECK(std::abs(d.Y()) < 1e-9 && d.X() < 0);
  CHECK(Sketch::from_json(tangent.to_json()).entities[0].end_tangent == std::vector<double>({-1, 0}));
  open["entities"][0]["periodic"] = true;  // tangents belong to open splines
  CHECK_THROWS(Sketch::from_json(open).validate());
  // Control points with only a degree: uniform knots, clamped (through the end poles) or periodic, unit weights.
  json poles = {{"points", json::array()}, {"entities", {{{"id", 20}, {"type", "spline"}, {"p", {1, 2, 3, 4, 5, 6}}, {"degree", 3}}}}};
  for (int i = 0; i < 6; ++i) poles["points"].push_back({{"id", i + 1}, {"x", 10 * std::cos(i)}, {"y", 10 * std::sin(i)}});
  const Sketch clamped = Sketch::from_json(poles);
  clamped.validate();
  BRepAdaptor_Curve cc(entity_edge(clamped, clamped.entities[0], {}));
  CHECK(cc.Value(cc.FirstParameter()).Distance(gp_Pnt(10, 0, 0)) < 1e-9);
  poles["entities"][0]["periodic"] = true;
  const Sketch ring = Sketch::from_json(poles);
  ring.validate();
  CHECK(BRepAdaptor_Curve(entity_edge(ring, ring.entities[0], {})).IsPeriodic());
  CHECK_EQ(sketch_regions(ring, {}).size(), size_t(1));
}

// Gap log #11: a coordinate dimension driven by a parameter follows it to the negative side (the unsigned
// hdistance flipped it back to the positive one).
TEST(coordinates_follow_parameters_to_either_side) {
  Document doc = Document::create();
  commands::run("param", {{"name", "x0"}, {"expr", "12 mm"}}, &doc);
  const json geometry = {
      {"points", {{{"id", 1}, {"x", 1}, {"y", 1}}, {{"id", 2}, {"x", 20}, {"y", 1}}}},
      {"entities", {{{"id", 10}, {"type", "line"}, {"p", {1, 2}}}}},
      {"constraints", {{{"id", 20}, {"type", "hdistance"}, {"refs", {1}}, {"value", 12}, {"expr", "x0"}},
                       {{"id", 21}, {"type", "vdistance"}, {"refs", {1}}, {"value", -3}, {"expr", "-3 mm"}},
                       {{"id", 22}, {"type", "hdistance"}, {"refs", {1, 2}}, {"value", 8}, {"expr", "x0 - 4 mm"}, {"signed", true}}}}};
  const std::string sk = commands::run("sketch", {{"geometry", geometry}}, &doc)["sketch_id"];
  auto at = [&](int id) {
    const Scene s = resolve(doc);
    for (const auto& p : s.sketch(sk)->geometry["points"])
      if (p["id"] == id) return std::make_pair(p["x"].get<double>(), p["y"].get<double>());
    throw Error("no point");
  };
  CHECK_NEAR(at(1).first, 12, 1e-9);
  CHECK_NEAR(at(1).second, -3, 1e-9);
  CHECK_NEAR(at(2).first, 20, 1e-9);
  commands::run("param", {{"name", "x0"}, {"expr", "-12 mm"}}, &doc);
  CHECK_NEAR(at(1).first, -12, 1e-9);
  CHECK_NEAR(at(2).first, -28, 1e-9);  // -12 + (-12 - 4)
}

// Gap log #7: comparisons, conditions and choices, clamp and mod, assert with a message, guarded branches, and no
// silent degrees for a function's plain number.
TEST(expression_conditions_choices_and_checks) {
  ParamTable t({{"1", "R", "30 mm", ""}, {"2", "N", "26", ""}, {"3", "ecc", "0.8 mm", ""}, {"4", "pose", "2", ""}, {"5", "margin", "-1 mm", ""}});
  CHECK_EQ(t.number("ecc < R / N"), 1.0);
  CHECK_EQ(t.number("ecc >= R / N"), 0.0);
  CHECK_EQ(t.number("2 mm == 2.0 mm && !(1 > 2) || 0"), 1.0);
  CHECK_NEAR(t.length("if(ecc < R / N, 5 mm, 7 mm)"), 5, 1e-12);
  CHECK_NEAR(t.length("ecc > 1 mm ? 1 mm : 2"), 2, 1e-12);  // a typed 2 takes mm, as in a sum
  CHECK_NEAR(t.angle("select(pose, 0 deg, 45 deg, 90 deg)"), M_PI / 2, 1e-12);
  CHECK_THROWS(t.angle("select(pose + 1, 0 deg, 45 deg, 90 deg)"));
  CHECK_NEAR(t.length("clamp(margin, 0 mm, 5 mm)"), 0, 1e-12);
  CHECK_NEAR(t.angle("mod(-30 deg, 360 deg)"), 330 * M_PI / 180, 1e-12);
  CHECK_NEAR(t.length("7 mm % 3 mm"), 1, 1e-12);
  // A guard: the branch not taken may fail.
  CHECK_NEAR(t.number("if(margin > 0 mm, sqrt(margin / 1 mm), 0)"), 0, 1e-12);
  CHECK_NEAR(t.number("margin > 0 mm && sqrt(margin / 1 mm) > 1"), 0, 1e-12);
  CHECK_THROWS(t.number("sqrt(margin / 1 mm)"));
  CHECK_THROWS(t.number("if(margin > 0 mm, nosuchname, 0)"));  // unknown names still count
  // assert: 1 when true, its message when not.
  CHECK_EQ(t.number("assert(ecc < R / N, \"eccentricity above R/N\")"), 1.0);
  try {
    t.number("assert(margin > 0 mm, 'margin must be positive')");
    CHECK(false);
  } catch (const Error& e) {
    CHECK(std::string(e.what()).find("margin must be positive") != std::string::npos);
  }
  // A plain number from a function is not degrees (this read 181 deg).
  CHECK_THROWS(t.angle("tan(45 deg) + asin(1) + acos(0) + exp(0) + log(1) + sign(-2)"));
  CHECK_NEAR(t.angle("tan(45 deg) * 1 rad + asin(1)"), 1 + M_PI / 2, 1e-12);
  CHECK_NEAR(t.angle("30 deg + 15"), 45 * M_PI / 180, 1e-12);  // a typed number still takes degrees
  // Fractional powers: a unit that exists is fine; one that does not says how to get there.
  CHECK_NEAR(t.length("(4 mm * 1 mm)^0.5"), 2, 1e-12);
  try {
    t.number("(2 mm)^1.5");
    CHECK(false);
  } catch (const Error& e) {
    CHECK(std::string(e.what()).find("divide by 1 mm") != std::string::npos);
  }
  CHECK_NEAR(t.number("(2 mm / 1 mm)^1.5"), std::pow(2, 1.5), 1e-12);
  // An older parameter named like a new function still works where it is not called.
  ParamTable older({{"1", "mod", "3", ""}, {"2", "twice", "mod * 2", ""}});
  CHECK_NEAR(older.number("twice"), 6, 1e-12);
  CHECK(!valid_param_name("clamp"));
  CHECK_EQ(expr_identifiers("assert(width > 2 mm, \"width too small\")").size(), size_t(1));
}

// Gap log #12: a change that makes a check fail is refused naming the check and its message (a live edit said only
// "unresolved operations").
TEST(a_change_that_breaks_a_check_is_refused_naming_it) {
  Document doc = Document::create();
  commands::run("param", {{"name", "margin"}, {"expr", "1 mm"}}, &doc);
  commands::run("param", {{"name", "chk_margin"}, {"expr", "assert(margin > 0 mm, \"the margin must stay positive\")"}}, &doc);
  commands::run("param", {{"name", "chk_root"}, {"expr", "sqrt(margin / 1 mm)"}}, &doc);
  try {
    commands::run("param", {{"name", "margin"}, {"expr", "-1 mm"}}, &doc);
    CHECK(false);
  } catch (const Error& e) {
    const std::string m = e.what();
    CHECK(m.find("chk_margin: the margin must stay positive") != std::string::npos);
    CHECK(m.find("chk_root: sqrt of a negative value") != std::string::npos);
  }
  const Scene s = resolve(doc);
  CHECK(s.unresolved.empty());
  for (const auto& p : s.params)
    if (p.name == "margin") CHECK_NEAR(p.value, 1, 1e-12);
}

// Gap log #2: the arm's cycloidal disc from its equations (it took 640 points with 1,282 dimension expressions): the
// point count follows from the tolerance, parameters reshape it, and an open equation curve joins other geometry at
// the end points it keeps.
TEST(an_equation_curve_follows_its_parameters_within_its_tolerance) {
  Document doc = Document::create();
  for (const auto& [name, expr] : std::vector<std::pair<std::string, std::string>>{{"R", "30 mm"}, {"rr", "2.65 mm"}, {"e", "0.8 mm"}, {"N", "26"}})
    commands::run("param", {{"name", name}, {"expr", expr}}, &doc);
  const std::string psi = "atan2(sin((1 - N) * t), R / (e * N) - cos((1 - N) * t))";
  const json curve = {{"id", 10}, {"type", "spline"},
                      {"equation", {{"x", "e + R * cos(t) - rr * cos(t + " + psi + ") - e * cos(N * t)"},
                                    {"y", "-R * sin(t) + rr * sin(t + " + psi + ") + e * sin(N * t)"},
                                    {"t0", "0 deg"}, {"t1", "360 deg"}, {"tolerance", 0.001}}}};
  const std::string sk = commands::run("sketch", {{"geometry", {{"entities", json::array({curve})}}}}, &doc)["sketch_id"];
  const std::string disc = feature_cmd(doc, "extrude", {{"profiles", json::array({{{"sketch", sk}}})}, {"distance", "6 mm"}})["body_ids"][0];
  // The true outline's area, from dense samples of the equations themselves.
  auto area = [](double R, double rr, double e, int N) {
    const int n = 200000;
    double a = 0, px = 0, py = 0;
    for (int i = 0; i <= n; ++i) {
      const double t = 2 * M_PI * i / n, psi = std::atan2(std::sin((1 - N) * t), R / (e * N) - std::cos((1 - N) * t));
      const double x = e + R * std::cos(t) - rr * std::cos(t + psi) - e * std::cos(N * t), y = -R * std::sin(t) + rr * std::sin(t + psi) + e * std::sin(N * t);
      if (i) a += px * y - x * py;
      px = x;
      py = y;
    }
    return std::abs(a / 2);
  };
  Scene s = resolve(doc);
  CHECK_NEAR(volume_properties(node_world_shape(doc, s, disc)).mass / 6, area(30, 2.65, 0.8, 26), 1e-4 * area(30, 2.65, 0.8, 26));
  auto points = [&] { return resolve(doc).sketch(sk)->geometry["entities"][0]["p"].size(); };
  const size_t fine = points();
  CHECK(fine > 64 && fine < 5000);
  // A coarser tolerance needs fewer points; a parameter reshapes the curve (and the disc) on regeneration.
  json coarse = curve;
  coarse["equation"]["tolerance"] = 0.05;
  commands::run("sketch_edit", {{"target", sk}, {"geometry", {{"entities", json::array({coarse})}}}}, &doc);
  CHECK(points() < fine);
  commands::run("param", {{"name", "N"}, {"expr", "21"}}, &doc);
  s = resolve(doc);
  CHECK(s.unresolved.empty());
  CHECK_NEAR(volume_properties(node_world_shape(doc, s, disc)).mass / 6, area(30, 2.65, 0.8, 21), 5e-3 * area(30, 2.65, 0.8, 21));
  // An open half circle keeps its given end points, so a line joins them into a region.
  const json half = {{"points", {{{"id", 1}, {"x", 0}, {"y", 0}}, {{"id", 2}, {"x", 1}, {"y", 0}}}},
                     {"entities", {{{"id", 20}, {"type", "spline"}, {"p", {1, 2}},
                                    {"equation", {{"x", "20 mm * cos(t)"}, {"y", "20 mm * sin(t)"}, {"t0", "0 deg"}, {"t1", "180 deg"}, {"tolerance", 0.0005}}}},
                                   {{"id", 21}, {"type", "line"}, {"p", {2, 1}}}}}};
  const std::string lid = commands::run("sketch", {{"geometry", half}}, &doc)["sketch_id"];
  const std::string dome = feature_cmd(doc, "extrude", {{"profiles", json::array({{{"sketch", lid}}})}, {"distance", "1 mm"}})["body_ids"][0];
  CHECK_NEAR(volume_properties(node_world_shape(doc, resolve(doc), dome)).mass, M_PI * 400 / 2, 2e-4 * M_PI * 400 / 2);
  // A plain t added to an angle is refused rather than read as degrees.
  const json mixed = {{"entities", {{{"id", 30}, {"type", "spline"}, {"equation", {{"x", "10 mm * cos(t + 1 deg)"}, {"y", "10 mm * sin(t)"}, {"t0", 0}, {"t1", "2 * pi"}}}}}}};
  CHECK_THROWS(commands::run("sketch", {{"geometry", mixed}}, &doc));
}

// Gap log #14: an axis through two free points; combine with several targets; a sketch pattern given only its seeds
// and inputs.
TEST(axis_through_free_points_combine_with_several_targets_and_sketch_patterns) {
  Document doc = Document::create();
  for (const json& points : {json::array({"point/0,0,0", "point/0,0,10"}), json::array({{{"point", {0, 0, 0}}}, {{"point", {0, 0, 10}}}}),
                             json::array({json::array({0, 0, 0}), json::array({0, 0, 10})})}) {
    const json made = feature_cmd(doc, "axis", {{"mode", "two_points"}, {"points", points}});
    const Scene axes = resolve(doc);
    const Feature* f = axes.feature(run_id(made));
    CHECK(f && f->result.contains("axis"));
    CHECK_NEAR(f->result["axis"]["dir"][2].get<double>(), 1, 1e-12);
  }
  auto box = [&](double x) {
    return feature_cmd(doc, "box", {{"x", std::to_string(x) + " mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}})["body_ids"][0].get<std::string>();
  };
  const std::string a = box(0), b = box(20), bar = feature_cmd(doc, "box", {{"x", "10 mm"}, {"length", "40 mm"}, {"width", "2 mm"}, {"height", "20 mm"}})["body_ids"][0];
  // A cut goes through each target; both stay.
  const json cut = feature_cmd(doc, "combine", {{"target", {a, b}}, {"tools", {bar}}, {"operation", "cut"}, {"keep_tools", true}});
  CHECK_EQ(cut["body_ids"].size(), size_t(2));
  Scene s = resolve(doc);
  CHECK_NEAR(volume_of_node(doc, s, a), 1000 - 200, 1e-6);
  CHECK_NEAR(volume_of_node(doc, s, b), 1000 - 200, 1e-6);
  // A join makes the first target one body with the others and the tools.
  feature_cmd(doc, "combine", {{"target", {a, b}}, {"tools", {bar}}, {"operation", "join"}});
  s = resolve(doc);
  CHECK(s.node(b) == nullptr && s.node(bar) == nullptr);
  CHECK_NEAR(volume_of_node(doc, s, a), 2 * 800 + 40 * 2 * 20, 1e-6);  // the bar fills the two slots it cut
  // A pattern needs only its seeds and inputs; its count can be a parameter.
  commands::run("param", {{"name", "holes"}, {"expr", "6"}}, &doc);
  const json geometry = {{"points", {{{"id", 1}, {"x", 20}, {"y", 0}}}},
                         {"entities", {{{"id", 10}, {"type", "circle"}, {"p", {1}}, {"r", 2}}}},
                         {"patterns", {{{"id", 50}, {"seeds", {10}}, {"inputs", {{"polar", true}, {"count", "holes"}, {"angle", "360 deg"}}}}}}};
  const std::string sk = commands::run("sketch", {{"geometry", geometry}}, &doc)["sketch_id"];
  auto circles = [&] {
    const Scene now = resolve(doc);
    size_t n = 0;
    for (const auto& e : now.sketch(sk)->geometry["entities"]) n += e["type"] == "circle";
    return n;
  };
  CHECK_EQ(circles(), size_t(6));
  commands::run("param", {{"name", "holes"}, {"expr", "8"}}, &doc);
  CHECK_EQ(circles(), size_t(8));
  try {
    commands::run("sketch", {{"geometry", {{"entities", {{{"id", 10}, {"type", "line"}, {"p", json::array()}}}}, {"patterns", {{{"id", 50}, {"inputs", json::object()}}}}}}}, &doc);
    CHECK(false);
  } catch (const Error& e) {
    CHECK(std::string(e.what()).find("seeds") != std::string::npos);
  }
}

// Gap log #10: an interference check kept in the timeline and run again when its bodies change; with fail_on, a
// finding is the feature's error, which the edit that caused it reports.
TEST(an_interference_check_is_kept_and_run_again) {
  Document doc = Document::create();
  commands::run("param", {{"name", "gap"}, {"expr", "5 mm"}}, &doc);
  const std::string a = feature_cmd(doc, "box", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}})["body_ids"][0];
  const std::string b = feature_cmd(doc, "box", {{"x", "10 mm + gap"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}})["body_ids"][0];
  const std::string check = run_id(feature_cmd(doc, "interference", {{"bodies", {a, b}}, {"clearance", "2 mm"}}));
  const std::string strict = run_id(feature_cmd(doc, "interference", {{"bodies", {a, b}}, {"fail_on", "interference"}}));
  const std::string every = run_id(feature_cmd(doc, "interference", json::object()));  // every solid so far
  auto status = [&](const std::string& id) {
    const Scene s = resolve(doc);
    return s.feature(id)->result["check"]["status"].get<std::string>();
  };
  CHECK_EQ(status(check), std::string("clear"));
  commands::run("param", {{"name", "gap"}, {"expr", "1 mm"}}, &doc);
  CHECK_EQ(status(check), std::string("too_close"));
  CHECK_EQ(status(strict), std::string("clear"));
  CHECK_EQ(status(every), std::string("clear"));
  const json edit = commands::run("param", {{"name", "gap"}, {"expr", "-2 mm"}}, &doc);
  CHECK_EQ(status(check), std::string("interference"));
  CHECK_EQ(status(every), std::string("interference"));  // it names no body, yet follows them all
  const Scene s = resolve(doc);
  CHECK_NEAR(s.feature(check)->result["check"]["items"][0]["volume_mm3"].get<double>(), 2 * 10 * 10, 1e-6);
  CHECK(s.feature(check)->error.empty());
  CHECK(!s.feature(strict)->error.empty());
  bool reported = false;
  for (const auto& e : edit["errors"]) reported |= e["op"] == strict;
  CHECK(reported);
}

// Gap log #9: a feature suppressed by an expression over the parameters (a third joint only when joints >= 3).
TEST(features_suppressed_by_an_expression) {
  Document doc = Document::create();
  commands::run("param", {{"name", "joints"}, {"expr", "2"}}, &doc);
  feature_cmd(doc, "box", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}});
  const json made = commands::run("feature", {{"kind", "box"}, {"inputs", {{"x", "40 mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}}},
                                              {"suppress_if", "joints < 3"}}, &doc);
  const std::string third = made["feature_id"];
  auto state = [&] {
    const Scene s = resolve(doc);
    return std::make_pair(s.feature(third)->suppressed, s.all_bodies().size());
  };
  CHECK(state() == std::make_pair(true, size_t(1)));
  commands::run("param", {{"name", "joints"}, {"expr", "3"}}, &doc);
  CHECK(state() == std::make_pair(false, size_t(2)));
  const json back = commands::run("param", {{"name", "joints"}, {"expr", "2"}}, &doc);
  CHECK(state() == std::make_pair(true, size_t(1)));
  CHECK(std::find(back["regenerated"].begin(), back["regenerated"].end(), third) != back["regenerated"].end());
  CHECK_EQ(resolve(doc).feature(third)->suppress_if, std::string("joints < 3"));
  commands::run("feature_edit", {{"target", third}, {"suppress_if", ""}}, &doc);
  CHECK(state() == std::make_pair(false, size_t(2)));
  CHECK_THROWS(commands::run("feature_edit", {{"target", third}, {"suppress_if", "joints <"}}, &doc));
}

// TODO 11 UI-33: a sketch or feature made in a component says so ("component" on its op). The feature's new bodies go
// into that component, kept in its frame, and stay there when it regenerates; copies still follow their source. An
// older build ignores the key: the bodies are in the component all the same (the result's parent), the sketch is not.
TEST(features_and_sketches_made_in_a_component) {
  Document doc = Document::create();
  const std::string lid = commands::run("component", {{"name", "Lid"}}, &doc)["component_id"];
  commands::run("transform", {{"target", lid}, {"matrix", Mat4::translation(0, 0, 40).to_json()}}, &doc);
  const std::string sketch = commands::run("sketch", {{"plane", {{"base", "xy"}}}, {"geometry", rectangle(0, 0, 20, 10).to_json()}, {"component", lid}}, &doc)["sketch_id"];
  Scene s = resolve(doc);
  CHECK_EQ(s.sketch(sketch)->component, lid);
  CHECK(s.sketch(sketch)->placed.to_json() == Mat4::translation(0, 0, 40).to_json());
  auto z_range = [&](const std::string& id) {
    Bnd_Box b;
    BRepBndLib::Add(node_world_shape(doc, s, id), b);
    return std::make_pair(b.CornerMin().Z(), b.CornerMax().Z());
  };
  const json pad = commands::run("feature", {{"kind", "extrude"}, {"inputs", {{"profiles", json::array({{{"sketch", sketch}, {"at", {5, 5}}}})}, {"distance", "5 mm"}}}, {"component", lid}}, &doc);
  const std::string plate = pad["body_ids"][0];
  s = resolve(doc);
  CHECK_EQ(s.node(plate)->parent, lid);
  CHECK_EQ(s.feature(pad["feature_id"])->component, lid);
  CHECK_EQ(s.feature(pad["feature_id"])->result["bodies"][0]["parent"], lid);
  CHECK_NEAR(z_range(plate).first, 0, 1e-6);  // where the sketch is; stored in the lid's frame
  CHECK_NEAR(z_range(plate).second, 5, 1e-6);
  CHECK(s.unresolved.empty());
  // A regeneration keeps the body where it was made: the same node, in the lid.
  commands::run("feature_edit", {{"target", pad["feature_id"]}, {"inputs", {{"distance", "8 mm"}}}}, &doc);
  s = resolve(doc);
  CHECK_EQ(s.node(plate)->parent, lid);
  CHECK_NEAR(z_range(plate).second, 8, 1e-6);
  // A copy follows its source (into the lid), a body from scratch without a component goes to the root, and so does
  // one whose component is null.
  const json row = commands::run("feature", {{"kind", "pattern_rect"}, {"inputs", {{"bodies", json::array({plate})}, {"count", "2"}, {"spacing", "30 mm"}, {"axis", {{"base", "x"}}}}}}, &doc);
  const std::string loose = feature_cmd(doc, "box", {{"x", "100 mm"}, {"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}})["body_ids"][0];
  const std::string rooted = commands::run("feature", {{"kind", "box"}, {"inputs", {{"x", "120 mm"}, {"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}}}, {"component", nullptr}}, &doc)["body_ids"][0];
  s = resolve(doc);
  CHECK_EQ(s.node(row["body_ids"][0])->parent, lid);
  CHECK(s.node(loose)->parent.empty() && s.node(rooted)->parent.empty());
  // A construction plane made in the lid records it; features lists where each item was made.
  const json plane = commands::run("feature", {{"kind", "plane"}, {"inputs", {{"mode", "offset"}, {"plane", {{"base", "xy"}}}, {"distance", "30 mm"}}}, {"component", lid}}, &doc);
  CHECK_EQ(resolve(doc).feature(plane["feature_id"])->component, lid);
  size_t listed = 0;
  for (const auto& item : commands::run("features", json::object(), &doc)) listed += item.value("component", "") == lid;
  CHECK_EQ(listed, 3u);
  // A body or an unknown id is not a component: refused before anything is computed.
  const size_t ops = doc.ops.size();
  CHECK_THROWS(commands::run("feature", {{"kind", "box"}, {"inputs", json::object()}, {"component", plate}}, &doc));
  CHECK_THROWS(commands::run("sketch", {{"geometry", rectangle(0, 0, 1, 1).to_json()}, {"component", new_uuid()}}, &doc));
  CHECK_EQ(doc.ops.size(), ops);
  CHECK_THROWS(Document::validate_op({{"op", "sketch"}, {"name", "S"}, {"plane", json::object()}, {"geometry", json::object()}, {"component", "lid"}}));
  // The file round-trips; an older build (which ignores the key) still finds the bodies in the lid.
  const Document back = Document::parse(doc.serialize());
  CHECK_EQ(resolve(back).sketch(sketch)->component, lid);
  Document older = back;
  for (auto& op : older.ops) op.data.erase("component");
  const Scene o = resolve(older);
  CHECK_EQ(o.node(plate)->parent, lid);
  CHECK(o.sketch(sketch)->component.empty());
  CHECK(o.unresolved.empty());
  // Without the lid (tombstoned), the sketch and the plane belong to the root, as the lid's bodies do: where they were,
  // not moved by the lid's placement gone. Back (the delete deleted), they are in it again, still where they were.
  const std::string removal = commands::run("delete", {{"target", resolve(doc).node(lid)->source_op}}, &doc)["id"];
  s = resolve(doc);
  CHECK(s.sketch(sketch)->component.empty() && s.feature(plane["feature_id"])->component.empty());
  CHECK(s.node(plate)->parent.empty());
  CHECK_NEAR(z_range(plate).first, 0, 1e-6);
  CHECK_NEAR(z_range(plate).second, 8, 1e-6);
  commands::run("delete", {{"target", removal}}, &doc);
  s = resolve(doc);
  CHECK_EQ(s.node(plate)->parent, lid);
  CHECK_EQ(s.sketch(sketch)->component, lid);
  CHECK_NEAR(z_range(plate).first, 0, 1e-6);
  CHECK_NEAR(z_range(plate).second, 8, 1e-6);
  CHECK(s.unresolved.empty());
}

// TODO 11 UI-33 phase 2: a sketch, construction plane or axis made in a component moves with it when the component
// moves later, as its bodies do, and features read the sketch where it is now: a body extruded from it after the move
// lands on it. The op keeps where it was made (replay before the move and older builds see that); a plane chosen again
// for the sketch goes in as made, so the sketch ends up where it was chosen.
TEST(sketches_and_planes_follow_their_component) {
  CHECK(Mat4::translation(1, 2, 3).inverse().to_json() == Mat4::translation(-1, -2, -3).to_json());
  Mat4 turn;  // a quarter turn about z, then up 5
  turn.at(0, 0) = 0, turn.at(0, 1) = -1, turn.at(1, 0) = 1, turn.at(1, 1) = 0, turn.at(2, 3) = 5;
  CHECK((turn * turn.inverse()).is_identity(1e-12) && (turn.inverse() * turn).is_identity(1e-12));
  Document doc = Document::create();
  const std::string lid = commands::run("component", {{"name", "Lid"}}, &doc)["component_id"];
  const std::string sketch = commands::run("sketch", {{"plane", {{"base", "xy"}}}, {"geometry", rectangle(0, 0, 20, 10).to_json()}, {"component", lid}}, &doc)["sketch_id"];
  const std::string plane = commands::run("feature", {{"kind", "plane"}, {"inputs", {{"mode", "offset"}, {"plane", {{"base", "xy"}}}, {"distance", "30 mm"}}}, {"component", lid}}, &doc)["feature_id"];
  const std::string axis = commands::run("feature", {{"kind", "axis"}, {"inputs", {{"mode", "two_points"}, {"points", {{5, 5, 0}, {5, 5, 10}}}}}, {"component", lid}}, &doc)["feature_id"];
  auto extrude = [&](double distance, const json& component) {
    json a = {{"kind", "extrude"}, {"inputs", {{"profiles", json::array({{{"sketch", sketch}, {"at", {5, 5}}}})}, {"distance", distance}, {"operation", "new"}}}};
    if (!component.is_null()) a["component"] = component;
    return commands::run("feature", a, &doc)["body_ids"][0].get<std::string>();
  };
  const std::string first = extrude(5, lid);
  Scene s = resolve(doc);
  auto box = [&](const std::string& id) {
    Bnd_Box b;
    BRepBndLib::Add(node_world_shape(doc, s, id), b);
    return b;
  };
  auto origin = [&](const std::string& id) { return s.sketch(id)->frame.origin; };
  auto plane_at = [&] { return Frame::from_json(s.feature(plane)->result["plane"]).origin; };
  CHECK(s.sketch(sketch)->moved.is_identity());
  // The lid goes up 40: its sketch, plane, axis and body with it; the op still has the sketch where it was made.
  commands::run("transform", {{"target", lid}, {"matrix", Mat4::translation(0, 0, 40).to_json()}}, &doc);
  s = resolve(doc);
  CHECK_NEAR(origin(sketch)[2], 40, 1e-9);
  CHECK(s.sketch(sketch)->moved.to_json() == Mat4::translation(0, 0, 40).to_json());
  CHECK_NEAR(plane_at()[2], 70, 1e-9);
  CHECK_NEAR(s.feature(axis)->result["axis"]["origin"][2].get<double>(), 40, 1e-9);
  CHECK_NEAR(s.feature(axis)->result["axis"]["dir"][2].get<double>(), 1, 1e-9);
  CHECK_NEAR(box(first).CornerMin().Z(), 40, 1e-6);
  CHECK_NEAR(doc.find_op(sketch)->data["plane"]["frame"]["origin"][2].get<double>(), 0, 1e-9);
  // Made from it now, in the lid or at the root, and sketched on the plane: where they are now.
  const std::string second = extrude(3, lid), loose = extrude(2, json());
  const std::string on_plane = commands::run("sketch", {{"plane", {{"feature", plane}}}, {"geometry", rectangle(0, 0, 4, 4).to_json()}}, &doc)["sketch_id"];
  s = resolve(doc);
  CHECK_NEAR(box(second).CornerMin().Z(), 40, 1e-6);
  CHECK_NEAR(box(second).CornerMax().Z(), 43, 1e-6);
  CHECK_NEAR(box(loose).CornerMin().Z(), 40, 1e-6);
  CHECK_NEAR(box(loose).CornerMax().Z(), 42, 1e-6);
  CHECK_NEAR(origin(on_plane)[2], 70, 1e-9);
  CHECK(s.unresolved.empty());
  // Put into a moved assembly (a reparent), the lid takes what is in it along; the root's stay.
  const std::string frame = commands::run("component", {{"name", "Frame"}}, &doc)["component_id"];
  commands::run("transform", {{"target", frame}, {"matrix", Mat4::translation(100, 0, 0).to_json()}}, &doc);
  commands::run("reparent", {{"target", lid}, {"parent", frame}}, &doc);
  s = resolve(doc);
  CHECK_NEAR(origin(sketch)[0], 100, 1e-9);
  CHECK_NEAR(origin(sketch)[2], 40, 1e-9);
  CHECK_NEAR(plane_at()[0], 100, 1e-9);
  CHECK_NEAR(box(second).CornerMin().X(), 100, 1e-6);
  CHECK_NEAR(box(loose).CornerMin().X(), 0, 1e-6);
  CHECK_NEAR(origin(on_plane)[0], 0, 1e-9);
  // Recomputing everything reads the same frames the features were made from: no new bodies.
  CHECK(plan_regenerate(doc, true).bodies.empty());
  // A plane chosen again for the moved sketch: it is where it was chosen, and the op has it where the lid was then.
  const json edited = commands::run("sketch_edit", {{"target", sketch}, {"plane", {{"base", "xz"}}}}, &doc);
  s = resolve(doc);
  CHECK(s.sketch(sketch)->frame.to_json() == Frame::from_json(edited["frame"]).to_json());
  CHECK_NEAR(origin(sketch)[0], 0, 1e-9);
  CHECK_NEAR(origin(sketch)[2], 0, 1e-9);
  CHECK_NEAR(s.sketch(sketch)->frame.y[2], 1, 1e-9);
  json stored;
  for (const auto& op : doc.ops)
    if (op.type == "edit" && op.data.value("target", "") == sketch) stored = op.data["set"]["plane"]["frame"]["origin"];
  CHECK_NEAR(stored[0].get<double>(), -100, 1e-9);
  CHECK_NEAR(stored[2].get<double>(), -40, 1e-9);
  for (const auto& body : {first, second}) {  // the lid's bodies regenerated on it, there
    CHECK_NEAR(box(body).CornerMin().X(), 0, 1e-6);
    CHECK_NEAR(box(body).CornerMin().Z(), 0, 1e-6);
    CHECK_NEAR(box(body).CornerMax().Z(), 10, 1e-6);
    CHECK_NEAR(box(body).CornerMax().Y(), 0, 1e-6);
  }
  CHECK(s.unresolved.empty());
  // A plane through a picked point on a world plane, as the app's plane picker gives it: there too.
  const json picked = commands::run("sketch_edit", {{"target", sketch}, {"plane", {{"support", {{"base", "xy"}}}, {"origin", {{"world", {5, 6, 0}}}}}}}, &doc);
  s = resolve(doc);
  CHECK(s.sketch(sketch)->frame.to_json() == Frame::from_json(picked["frame"]).to_json());
  CHECK_NEAR(origin(sketch)[0], 5, 1e-9);
  CHECK_NEAR(origin(sketch)[1], 6, 1e-9);
  CHECK_NEAR(origin(sketch)[2], 0, 1e-9);
  CHECK_NEAR(box(first).CornerMin().Z(), 0, 1e-6);
  CHECK_NEAR(box(first).CornerMax().Z(), 5, 1e-6);
  // The file round-trips; an older build (no component keys) shows them where they were made.
  const Document back = Document::parse(doc.serialize());
  CHECK(resolve(back).sketch(sketch)->frame.to_json() == s.sketch(sketch)->frame.to_json());
  Document older = back;
  for (auto& op : older.ops) op.data.erase("component");
  const Scene o = resolve(older);
  CHECK(o.sketch(sketch)->moved.is_identity());
  CHECK_NEAR(o.sketch(sketch)->frame.origin[0], -95, 1e-9);
  CHECK_NEAR(o.sketch(sketch)->frame.origin[2], -40, 1e-9);
  CHECK_NEAR(Frame::from_json(o.feature(plane)->result["plane"]).origin[2], 30, 1e-9);
  // Without the lid (tombstoned), the sketch and the bodies made from it are back where they were made, together.
  commands::run("delete", {{"target", resolve(doc).node(lid)->source_op}}, &doc);
  s = resolve(doc);
  CHECK(s.sketch(sketch)->component.empty() && s.sketch(sketch)->moved.is_identity());
  CHECK_NEAR(origin(sketch)[0], -95, 1e-9);
  CHECK_NEAR(box(first).CornerMin().X(), -95, 1e-6);
  CHECK_NEAR(box(first).CornerMin().Z(), -40, 1e-6);
  CHECK_NEAR(plane_at()[2], 30, 1e-9);
}

// TODO 11 UI-33: while a component is active the timeline dims the ops that do not touch it; ops_in_component says
// which do: what made what is in it (also a body moved into it later), what was made in it, changes to a body in it,
// its moves and anything put into it. A body elsewhere, its name and a parameter do not.
TEST(ops_that_touch_a_component) {
  Document doc = Document::create();
  const std::string lid = commands::run("component", {{"name", "Lid"}}, &doc)["component_id"];
  const std::string lid_op = resolve(doc).node(lid)->source_op;
  const std::string sketch = commands::run("sketch", {{"geometry", rectangle(0, 0, 20, 10).to_json()}, {"component", lid}}, &doc)["sketch_id"];
  const json pad = commands::run("feature", {{"kind", "extrude"}, {"inputs", {{"profiles", json::array({{{"sketch", sketch}, {"at", {5, 5}}}})}, {"distance", "5 mm"}}}, {"component", lid}}, &doc);
  const std::string plate = pad["body_ids"][0];
  const json loose = feature_cmd(doc, "box", {{"x", "50 mm"}, {"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}});
  const json pin = feature_cmd(doc, "box", {{"x", "80 mm"}, {"length", "2 mm"}, {"width", "2 mm"}, {"height", "8 mm"}});
  const std::string moved = commands::run("transform", {{"target", lid}, {"matrix", Mat4::translation(0, 0, 10).to_json()}}, &doc)["id"];
  const std::string renamed = commands::run("rename", {{"target", loose["body_ids"][0]}, {"name", "Loose"}}, &doc)["id"];
  const std::string into = commands::run("reparent", {{"target", pin["body_ids"][0]}, {"parent", lid}}, &doc)["id"];
  const std::string round = feature_cmd(doc, "fillet", {{"edges", json::array({plate + "/edge/0"})}, {"radius", "1 mm"}})["feature_id"];
  const std::string note = commands::run("annotate", {{"anchor", plate + "/face/0"}, {"text", "check"}}, &doc)["id"];
  const std::string param = commands::run("param", {{"name", "gap"}, {"expr", "2 mm"}}, &doc)["ids"][0];
  const Scene s = resolve(doc);
  const std::set<std::string> in = ops_in_component(doc, s, lid);
  for (const std::string op : {lid_op, sketch, pad["feature_id"].get<std::string>(), pin["feature_id"].get<std::string>(), moved, into, round, note}) CHECK(in.count(op));
  for (const std::string op : {loose["feature_id"].get<std::string>(), renamed, param}) CHECK(!in.count(op));
  CHECK_EQ(in.size(), 8u);
  CHECK_EQ(ops_in_component(doc, s, "").size(), effective_ops(doc).size());
  // A feature made in it and tombstoned still touches it, as do its tombstone and the restore of it.
  const std::string box = commands::run("feature", {{"kind", "box"}, {"inputs", {{"x", "30 mm"}, {"length", "2 mm"}, {"width", "2 mm"}, {"height", "2 mm"}}}, {"component", lid}}, &doc)["feature_id"];
  const std::string gone = commands::run("delete", {{"target", box}}, &doc)["id"];
  const std::string other = commands::run("delete", {{"target", loose["feature_id"]}}, &doc)["id"];
  std::set<std::string> now = ops_in_component(doc, resolve(doc), lid);
  CHECK(now.count(box) && now.count(gone) && !now.count(other) && !now.count(loose["feature_id"].get<std::string>()));
  const std::string back = commands::run("delete", {{"target", gone}}, &doc)["id"];
  now = ops_in_component(doc, resolve(doc), lid);
  CHECK(now.count(box) && now.count(gone) && now.count(back));
  size_t listed = 0;
  for (const auto& op : doc.ops) listed += op.type != "edit" && op.type != "regen";
  CHECK_EQ(ops_in_component(doc, resolve(doc), "").size(), listed);
}

// TODO 11 UI-37: a locked body, or one under a locked component, is not changed, moved or removed: the change is
// refused naming it and the document stays as it was. It is still a reference (a sketch on its face) and a source (a
// copy of it), automatic join / cut targets leave it out, its colour and name still change, a component above it still
// moves (it with it), and an unlocked component that goes with it (a drawing deleted with a locked layer) still goes.
TEST(locked_bodies_are_left_alone) {
  Document doc = Document::create();
  const json made = feature_cmd(doc, "box", {{"length", "40 mm"}, {"width", "40 mm"}, {"height", "5 mm"}});
  const std::string plate = made["body_ids"][0], plate_op = made["feature_id"];
  const std::string block = feature_cmd(doc, "box", {{"x", "60 mm"}, {"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}})["body_ids"][0];
  CHECK(!has_locks(doc));
  commands::run("appearance", {{"target", plate}, {"locked", true}}, &doc);
  Scene s = resolve(doc);
  CHECK(has_locks(doc) && s.effectively_locked(plate) && !s.effectively_locked(block));
  const std::string plate_key = s.node(plate)->body_key, block_key = s.node(block)->body_key;
  auto refusal = [&](const std::string& command, const json& args) {
    const size_t ops = doc.ops.size();
    std::string why;
    try {
      commands::run(command, args, &doc);
    } catch (const Error& e) {
      why = e.what();
    }
    CHECK_EQ(doc.ops.size(), ops);
    return why;
  };
  auto locked = [&](const std::string& name, const char* verb) { return "\"" + name + "\" is locked: unlock it before " + verb + " it"; };
  auto held = [&](const std::string& name, const std::string& holder, const char* verb) {  // locked with a component above it: that is unlocked
    return "\"" + name + "\" is locked with \"" + holder + "\": unlock \"" + holder + "\" before " + verb + " it";
  };
  const std::string name = s.node(plate)->name;
  CHECK_EQ(refusal("feature", {{"kind", "fillet"}, {"inputs", {{"edges", json::array({plate + "/edge/0"})}, {"radius", "1 mm"}}}}), locked(name, "changing"));
  CHECK_EQ(refusal("feature", {{"kind", "box"}, {"inputs", {{"length", "4 mm"}, {"width", "4 mm"}, {"height", "20 mm"}, {"operation", "cut"}, {"targets", json::array({body_ref(plate)})}}}}),
           locked(name, "changing"));
  CHECK_EQ(refusal("feature", {{"kind", "move"}, {"inputs", {{"bodies", json::array({plate})}, {"dz", "5 mm"}}}}), locked(name, "moving"));  // placed, not rebuilt
  CHECK_EQ(refusal("feature", {{"kind", "remove"}, {"inputs", {{"bodies", json::array({plate})}}}}), locked(name, "removing"));
  CHECK_EQ(refusal("feature_edit", {{"target", plate_op}, {"inputs", {{"height", "6 mm"}}}}), locked(name, "changing"));
  CHECK_EQ(refusal("delete", {{"target", plate_op}}), locked(name, "removing"));
  CHECK_EQ(refusal("transform", {{"target", plate}, {"matrix", Mat4::translation(0, 0, 5).to_json()}}), locked(name, "moving"));
  const std::string shelf = commands::run("component", {{"name", "Shelf"}}, &doc)["component_id"];
  CHECK_EQ(refusal("reparent", {{"targets", json::array({block, plate})}, {"parent", shelf}}), locked(name, "moving"));
  // Still a reference and a source; its look and name are not edits.
  commands::run("sketch", {{"plane", {{"face", plate + "/face/0"}}}, {"geometry", rectangle(0, 0, 4, 4).to_json()}}, &doc);
  CHECK_EQ(feature_cmd(doc, "move", {{"bodies", json::array({plate})}, {"dx", "100 mm"}, {"copy", true}})["body_ids"].size(), 1u);
  commands::run("appearance", {{"target", plate}, {"color", json::array({0.2, 0.2, 0.2})}}, &doc);
  commands::run("rename", {{"target", plate}, {"name", "Base plate"}}, &doc);
  // An automatic cut through the plate and the block cuts the block alone, and says so in its targets.
  const json cut = feature_cmd(doc, "box", {{"x", "37.5 mm"}, {"length", "40 mm"}, {"width", "4 mm"}, {"height", "20 mm"}, {"operation", "cut"}});
  s = resolve(doc);
  CHECK_EQ(s.node(plate)->body_key, plate_key);
  CHECK(s.node(block)->body_key != block_key);
  CHECK_EQ(s.feature(cut["feature_id"])->inputs["targets"].size(), 1u);
  CHECK_EQ(s.feature(cut["feature_id"])->inputs["targets"][0]["body"], block);
  // A locked component locks what is in it; the component above it still moves, and it moves along.
  const std::string outer = commands::run("component", {{"name", "Outer"}}, &doc)["component_id"];
  const std::string inner = commands::run("component", {{"name", "Inner"}, {"parent", outer}}, &doc)["component_id"];
  const std::string pin = commands::run("feature", {{"kind", "box"}, {"inputs", {{"x", "200 mm"}, {"length", "4 mm"}, {"width", "4 mm"}, {"height", "10 mm"}}}, {"component", inner}}, &doc)["body_ids"][0];
  commands::run("appearance", {{"target", inner}, {"locked", true}}, &doc);
  s = resolve(doc);
  CHECK(s.effectively_locked(pin) && !s.node(pin)->locked);
  CHECK_EQ(refusal("feature", {{"kind", "fillet"}, {"inputs", {{"edges", json::array({pin + "/edge/0"})}, {"radius", "0.5 mm"}}}}), held(s.node(pin)->name, "Inner", "changing"));
  CHECK_EQ(refusal("transform", {{"target", inner}, {"matrix", Mat4::translation(0, 0, 5).to_json()}}), locked("Inner", "moving"));
  CHECK_EQ(refusal("reparent", {{"target", pin}, {"parent", nullptr}}), held(s.node(pin)->name, "Inner", "moving"));
  try {  // what a UI words in its own language
    commands::run("transform", {{"target", pin}, {"matrix", Mat4::translation(0, 0, 5).to_json()}}, &doc);
    CHECK(false);
  } catch (const LockedError& e) {
    CHECK(e.node == s.node(pin)->name && e.holder == "Inner" && e.change == "moving" && e.more == 0);
  }
  CHECK_EQ(refusal("reparent", {{"target", inner}, {"parent", shelf}}), locked("Inner", "moving"));
  commands::run("reparent", {{"target", outer}, {"parent", shelf}}, &doc);  // the component above it may, it along
  commands::run("reparent", {{"target", outer}, {"parent", nullptr}}, &doc);
  CHECK_EQ(refusal("delete", {{"target", s.node(inner)->source_op}}), locked("Inner", "removing"));
  commands::run("transform", {{"target", outer}, {"matrix", Mat4::translation(0, 0, 10).to_json()}}, &doc);
  CHECK_NEAR(resolve(doc).world(pin).at(2, 3), 10, 1e-12);
  // Several at once: the first is named and the rest counted.
  s = resolve(doc);
  Scene gone = s;
  gone.nodes.erase(plate);
  gone.nodes.erase(pin);
  const auto two = locked_change(s, gone);
  CHECK(two && two->more == 1 && two->change == "removing" && std::string(two->what()).find(" before removing it (and 1 more locked)") != std::string::npos);
  CHECK(!locked_change(s, s));
  // A drawing whose layer is locked (an import node's "locked"): the layer's body alone is not removed, the drawing is.
  const json line = {{"type", "body"}, {"id", new_uuid()}, {"name", "Walls"}, {"key", block_key}};
  const json walls = {{"type", "component"}, {"id", new_uuid()}, {"name", "Walls"}, {"locked", true}, {"layer", {{"name", "Walls"}, {"locked", true}}}, {"children", json::array({line})}};
  const json drawing = {{"op", "import"}, {"source", "walls.dxf"}, {"nodes", json::array({{{"type", "component"}, {"id", new_uuid()}, {"name", "walls"}, {"children", json::array({walls})}}})}};
  const std::string drawing_op = commands::run("append", {{"op", drawing}}, &doc)["appended"][0];
  s = resolve(doc);
  CHECK(s.effectively_locked(line["id"]) && s.node(walls["id"])->layer["locked"] == true);
  CHECK_EQ(refusal("feature", {{"kind", "remove"}, {"inputs", {{"bodies", json::array({line["id"]})}}}}), locked("Walls", "removing"));
  commands::run("delete", {{"target", drawing_op}}, &doc);
  CHECK(!resolve(doc).node(line["id"]));
  // A regeneration is a repair, never refused; unlocked, the plate changes again.
  commands::run("regenerate", {{"force", true}}, &doc);
  commands::run("appearance", {{"target", plate}, {"locked", false}}, &doc);
  feature_cmd(doc, "fillet", {{"edges", json::array({plate + "/edge/0"})}, {"radius", "1 mm"}});
  CHECK(resolve(doc).node(plate)->body_key != plate_key);
}

// TODO 11 UI-34: reparent with keep_place (the browser's drop, Move to component…, Component from selection) leaves what
// moves where it is in the world: a transform follows each node whose new parent is placed elsewhere, none for a reorder
// or a cycle (not replayed); without it a node goes with its new parent's placement.
TEST(reparent_keeps_place) {
  Document doc = Document::create();
  auto box = [&](const char* x) { return feature_cmd(doc, "box", {{"x", x}, {"length", "4 mm"}, {"width", "4 mm"}, {"height", "4 mm"}})["body_ids"][0].get<std::string>(); };
  const std::string a = box("10 mm"), b = box("20 mm"), c = box("30 mm");
  const std::string frame = commands::run("component", {{"name", "Frame"}}, &doc)["component_id"];
  const std::string inner = commands::run("component", {{"name", "Inner"}, {"parent", frame}}, &doc)["component_id"];
  Mat4 turned;  // a quarter turn about Z, raised
  turned.m = {0, -1, 0, 5, 1, 0, 0, 0, 0, 0, 1, 50, 0, 0, 0, 1};
  commands::run("transform", {{"target", frame}, {"matrix", turned.to_json()}}, &doc);
  Scene s = resolve(doc);
  const Mat4 wa = s.world(a), wb = s.world(b), wc = s.world(c);
  auto same = [](const Mat4& x, const Mat4& y) { return (x * y.inverse()).is_identity(1e-9); };
  size_t ops = doc.ops.size();
  const json moved = commands::run("reparent", {{"targets", json::array({a, b})}, {"parent", inner}, {"keep_place", true}}, &doc);
  CHECK_EQ(doc.ops.size(), ops + 4);
  CHECK_EQ(moved.value("transformed", 0), 2);
  CHECK_EQ(moved["ids"].size(), 2u);
  s = resolve(doc);
  CHECK(s.node(a)->parent == inner && s.node(b)->parent == inner);
  CHECK(same(s.world(a), wa) && same(s.world(b), wb) && !same(s.node(a)->local, wa));
  ops = doc.ops.size();
  commands::run("reparent", {{"target", b}, {"parent", inner}, {"index", 0}, {"keep_place", true}}, &doc);
  CHECK_EQ(doc.ops.size(), ops + 1);
  CHECK_EQ(resolve(doc).node(inner)->children.front(), b);
  commands::run("reparent", {{"target", a}, {"parent", nullptr}, {"keep_place", true}}, &doc);
  s = resolve(doc);
  CHECK(s.node(a)->parent.empty() && same(s.world(a), wa) && same(s.node(a)->local, wa));
  ops = doc.ops.size();
  commands::run("reparent", {{"target", frame}, {"parent", inner}, {"keep_place", true}}, &doc);
  CHECK_EQ(doc.ops.size(), ops + 1);
  CHECK(resolve(doc).node(frame)->parent.empty());
  commands::run("reparent", {{"target", c}, {"parent", inner}}, &doc);
  CHECK(!same(resolve(doc).world(c), wc));
}

// A body made in a placed component and moved out of it keeping its place (a transform counting on the frame it was
// made in) stays put when the component is deleted, as one moved into another component does; one left in it stays
// put too (it is made again in world coordinates).
TEST(bodies_moved_out_of_a_deleted_component_stay) {
  Document doc = Document::create();
  const std::string lid = commands::run("component", {{"name", "Lid"}}, &doc)["component_id"];
  const std::string shelf = commands::run("component", {{"name", "Shelf"}}, &doc)["component_id"];
  commands::run("transform", {{"target", lid}, {"matrix", Mat4::translation(0, 0, 40).to_json()}}, &doc);
  commands::run("transform", {{"target", shelf}, {"matrix", Mat4::translation(10, 0, 0).to_json()}}, &doc);
  auto box = [&](const char* x) {
    return commands::run("feature", {{"kind", "box"}, {"inputs", {{"x", x}, {"length", "4 mm"}, {"width", "4 mm"}, {"height", "3 mm"}}}, {"component", lid}}, &doc)["body_ids"][0].get<std::string>();
  };
  const std::string out = box("0 mm"), moved = box("10 mm"), stays = box("20 mm");
  auto where = [&](const std::string& id) {
    Bnd_Box b;
    BRepBndLib::Add(node_world_shape(doc, resolve(doc), id), b);
    return std::array<double, 2>{b.CornerMin().X(), b.CornerMin().Z()};
  };
  const auto out0 = where(out), moved0 = where(moved), stays0 = where(stays);
  commands::run("reparent", {{"target", out}, {"parent", nullptr}, {"keep_place", true}}, &doc);
  commands::run("reparent", {{"target", moved}, {"parent", shelf}, {"keep_place", true}}, &doc);
  auto unchanged = [&](const std::string& id, const std::array<double, 2>& at) {
    const auto now = where(id);
    CHECK_NEAR(now[0], at[0], 1e-6);
    CHECK_NEAR(now[1], at[1], 1e-6);
  };
  unchanged(out, out0);
  unchanged(moved, moved0);
  const std::string removal = commands::run("delete", {{"target", resolve(doc).node(lid)->source_op}}, &doc)["id"];
  Scene s = resolve(doc);
  CHECK(!s.node(lid) && s.node(out)->parent.empty() && s.node(moved)->parent == shelf && s.node(stays)->parent.empty());
  unchanged(out, out0);
  unchanged(moved, moved0);
  unchanged(stays, stays0);
  CHECK(design::plan_regenerate(doc).ops.empty());
  commands::run("delete", {{"target", removal}}, &doc);  // the lid back: each where it was, the last one in it again
  s = resolve(doc);
  CHECK(s.node(out)->parent.empty() && s.node(moved)->parent == shelf && s.node(stays)->parent == lid);
  unchanged(out, out0);
  unchanged(moved, moved0);
  unchanged(stays, stays0);
  // Moved back into the lid, a body counts as left in it again.
  commands::run("reparent", {{"target", out}, {"parent", lid}, {"keep_place", true}}, &doc);
  commands::run("delete", {{"target", resolve(doc).node(lid)->source_op}}, &doc);
  unchanged(out, out0);
  for (const auto& u : resolve(doc).unresolved) CHECK(u.op_type == "transform" || u.op_type == "reparent");  // of the lid, into the lid
}
