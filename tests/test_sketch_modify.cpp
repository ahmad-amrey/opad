#include "check.hpp"
#include "opad/design/sketch_modify.hpp"
#include "opad/design/sketch_pattern.hpp"
#include "opad/design/sketch_create.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <cmath>
using namespace opad::design;
using E=SkEntity::Type;

TEST(transforms_preserve_analytic_geometry) {
  Sketch sk;int circle=sk.add_circle(sk.add_point(2,3),4);
  SketchTransform t;t.x=10;t.y=-2;t.scale=2;
  auto copy=transform_entities(sk,{circle},t,true);
  CHECK_NEAR(sk.entity(circle)->r,4,0);CHECK_NEAR(sk.entity(copy[0])->r,8,0);
  CHECK_NEAR(sk.point(sk.entity(copy[0])->p[0])->x,14,0);
  int arc=sk.add_arc(sk.add_point(0,0),sk.add_point(10,0),sk.add_point(0,10));
  t={};t.mirror=true;copy=transform_entities(sk,{arc},t,true);
  BRepAdaptor_Curve curve(entity_edge(sk,*sk.entity(copy[0]),{}));CHECK_NEAR(curve.LastParameter()-curve.FirstParameter(),M_PI/2,1e-9);
}

TEST(pattern_instances_follow_seed_and_keep_ids) {
  Sketch sk;int circle=sk.add_circle(sk.add_point(0,0),3);
  int pattern=create_pattern(sk,{circle},{{"count",3},{"rows",2},{"dx",10},{"dy",15}});
  CHECK_EQ(sk.entities.size(),size_t(6));const int instance=sk.patterns[0]["instances"][0]["map"][1][1].get<int>();
  sk.entity(circle)->r=4;sk.point(sk.entity(circle)->p[0])->y=2;CHECK(solve(sk).converged);
  CHECK_NEAR(sk.entity(instance)->r,4,1e-8);CHECK_NEAR(sk.point(sk.entity(instance)->p[0])->y,2,1e-8);
  edit_pattern(sk,pattern,{{"count",4},{"rows",2},{"dx",12},{"dy",20}});
  CHECK_EQ(sk.entities.size(),size_t(8));CHECK(sk.entity(instance));CHECK_NEAR(sk.point(sk.entity(instance)->p[0])->x,12,1e-8);
  auto loaded=Sketch::from_json(sk.to_json());CHECK(loaded.to_json()==sk.to_json());
  edit_pattern(sk,pattern,{{"count",2},{"rows",1},{"dx",12}});CHECK_EQ(sk.entities.size(),size_t(2));CHECK(sk.entity(instance));
  remove_pattern(sk,pattern,true);CHECK(sk.patterns.empty());CHECK(!sk.entity(instance)->fixed);CHECK(!sk.point(sk.entity(instance)->p[0])->fixed);
}

TEST(pattern_parameters_and_polar_spacing) {
  Sketch sk;int circle=sk.add_circle(sk.add_point(10,0),1);
  ParamTable params({{"count","count","4",""}});
  create_pattern(sk,{circle},{{"polar",true},{"count","count"},{"angle","360 deg"}},params);
  CHECK_EQ(sk.entities.size(),size_t(4));CHECK_NEAR(sk.point(sk.entities[1].p[0])->x,0,1e-8);CHECK_NEAR(sk.point(sk.entities[1].p[0])->y,10,1e-8);
  evaluate_patterns(sk,ParamTable({{"count","count","3",""}}));CHECK_EQ(sk.entities.size(),size_t(3));
  sk.remove(circle);CHECK(sk.entities.empty());CHECK(sk.patterns.empty());
}

TEST(split_break_extend_and_heal) {
  Sketch sk;int line=sk.add_line(sk.add_point(0,0),sk.add_point(10,0));split_entity(sk,line,4,0);CHECK_EQ(sk.entities.size(),size_t(2));CHECK(sk.entity(line));
  int cross=sk.add_line(sk.add_point(7,-5),sk.add_point(7,5));std::vector<int> ids;for(const auto& e:sk.entities)ids.push_back(e.id);break_intersections(sk,ids);CHECK_EQ(sk.entities.size(),size_t(5));
  Sketch extend;int a=extend.add_line(extend.add_point(0,0),extend.add_point(5,0)),b=extend.add_line(extend.add_point(10,-5),extend.add_point(10,5));
  extend_entity(extend,a,b,4,0);CHECK_NEAR(extend.point(extend.entity(a)->p[1])->x,10,1e-8);
  Sketch gap;int p=gap.add_point(0,0),q=gap.add_point(10,0),r=gap.add_point(10.02,0),s=gap.add_point(10,10);gap.add_line(p,q);gap.add_line(r,s);
  CHECK_EQ(heal_endpoints(gap,.05),1);CHECK_EQ(gap.points.size(),size_t(3));CHECK(gap.entities[0].p[1]==gap.entities[1].p[0]);(void)cross;
}

TEST(chamfer_and_round_sharp_offsets) {
  Sketch sk;create_primitive(sk,"rect3",{{0,0},{20,0},{20,10}});int corner=sk.entities.front().p[0];
  chamfer_corner(sk,corner,2,3);CHECK_EQ(sk.entities.size(),size_t(5));CHECK(solve(sk).converged);
  for(bool round:{false,true}) {Sketch offset;create_primitive(offset,"rect3",{{0,0},{20,0},{20,10}});std::vector<int> ids;for(const auto& e:offset.entities)ids.push_back(e.id);offset_entities(offset,ids,2,round);CHECK(offset.entities.size()>4);CHECK(!sketch_regions(offset,{}).empty());}
}

TEST(delete_polyline_and_spline_nodes) {
  Sketch sk;int a=sk.add_point(0,0),b=sk.add_point(5,5),c=sk.add_point(10,0);sk.add_line(a,b);sk.add_line(b,c);
  delete_curve_node(sk,b);CHECK_EQ(sk.entities.size(),size_t(1));CHECK(!sk.point(b));CHECK(sk.entities[0].p==std::vector<int>({a,c}));
  auto ids=create_primitive(sk,"control_spline",{{0,10},{5,20},{10,20},{15,10},{20,10}});int point=sk.entity(ids[0])->p[2];delete_curve_node(sk,point);
  CHECK_EQ(sk.entity(ids[0])->p.size(),size_t(4));CHECK(!entity_edge(sk,*sk.entity(ids[0]),{}).IsNull());
}

TEST(region_booleans_remain_exact_editable_curves) {
  for(const std::string operation:{"union","subtract","intersect"}) {
    Sketch sk;sk.add_circle(sk.add_point(0,0),10);sk.add_circle(sk.add_point(10,0),10);
    boolean_regions(sk,-5,0,15,0,operation);
    auto regions=sketch_regions(sk,{});CHECK_EQ(regions.size(),size_t(1));
    const double lens=200*M_PI/3-50*std::sqrt(3.0);
    CHECK_NEAR(regions[0].area,operation=="union"?200*M_PI-lens:operation=="subtract"?100*M_PI-lens:lens,1e-4);
    for(const auto& e:sk.entities)CHECK(e.type==E::Circle||e.type==E::Arc);
  }
}
TEST(scale_updates_driving_dimensions_and_rotation_respects_orientation_constraints) {
  Sketch sk;int line=sk.add_line(sk.add_point(0,0),sk.add_point(10,0));int h=sk.add_constraint(SkConstraint::Type::Horizontal,{line});int d=sk.add_constraint(SkConstraint::Type::Distance,{line},10,"width");
  SketchTransform t;t.scale=2;transform_entities(sk,{line},t,false);evaluate_dimensions(sk,ParamTable({{"width","width","10 mm",""}}));CHECK(solve(sk).converged);CHECK_NEAR(sk.constraint(d)->value,20,1e-9);
  t={};t.angle=M_PI/2;transform_entities(sk,{line},t,false);CHECK(sk.constraint(h)->type==SkConstraint::Type::Vertical);CHECK(solve(sk).converged);
  t.angle=.3;CHECK_THROWS(transform_entities(sk,{line},t,false));
}
TEST(healing_connects_near_endpoint_to_curve_interior) {
  Sketch sk;int line=sk.add_line(sk.add_point(0,0),sk.add_point(10,0));int near=sk.add_point(5,.02);sk.add_line(near,sk.add_point(5,5));CHECK_EQ(heal_to_curves(sk,.05),1);CHECK_NEAR(sk.point(near)->y,0,1e-8);CHECK(solve(sk).converged);(void)line;
}
TEST(deleting_shared_pattern_seed_removes_all_dependent_patterns) {
  Sketch sk;int circle=sk.add_circle(sk.add_point(0,0),2);create_pattern(sk,{circle},{{"count",2},{"dx",10}});create_pattern(sk,{circle},{{"count",2},{"dy",10}});sk.remove(circle);CHECK(sk.patterns.empty());CHECK(sk.entities.empty());sk.validate();
}
TEST(healing_includes_the_seam_of_a_closed_circle) {
  Sketch sk;sk.add_circle(sk.add_point(0,0),10);
  const int endpoint=sk.add_point(10.02,0);sk.add_line(endpoint,sk.add_point(20,0));
  CHECK_EQ(heal_to_curves(sk,.05),1);CHECK_NEAR(sk.point(endpoint)->x,10,1e-8);CHECK(solve(sk).converged);
}
CHECK_MAIN()
TEST(origin_shift_preserves_ids_constraints_images_and_polar_pattern) {
  Sketch sk;const int circle=sk.add_circle(sk.add_point(10,20),3);
  sk.add_constraint(SkConstraint::Type::Radius,{circle},3);
  create_pattern(sk,{circle},{{"polar",true},{"count",3},{"cx",5},{"cy",6}});
  sk.images.push_back({{"id",sk.next_id()},{"data","png"},{"position",{2,4}},{"width",10},{"height",10}});
  const auto before=sk;shift_sketch_origin(sk,7,-4);refresh_patterns(sk);
  CHECK_EQ(sk.next_id(),before.next_id());CHECK_EQ(sk.constraints.size(),before.constraints.size());
  for(const auto& p:before.points){CHECK_NEAR(sk.point(p.id)->x+7,p.x,1e-8);CHECK_NEAR(sk.point(p.id)->y-4,p.y,1e-8);}
  CHECK_NEAR(sk.images[0]["position"][0].get<double>(),-5,1e-8);CHECK(solve(sk).converged);
}
