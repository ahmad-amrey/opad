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

// Gap log #11: a signed distance keeps its meaning through a half turn and a mirror; a coordinate dimension pins its
// point to the sketch origin, so moving that point is refused until it is removed.
TEST(signed_distances_follow_transforms_and_coordinates_pin_points) {
  Sketch sk;
  const int a = sk.add_point(0, 0), b = sk.add_point(-5, 2), line = sk.add_line(a, b);
  const int d = sk.add_constraint(SkConstraint::Type::HDistance, {a, b}, -5);
  sk.constraint(d)->is_signed = true;
  sk.constraint(d)->expr = "shift";
  CHECK(solve(sk).converged);
  SketchTransform t;
  t.angle = M_PI;
  transform_entities(sk, {line}, t, false);
  CHECK_NEAR(sk.constraint(d)->value, 5, 1e-9);
  CHECK_EQ(sk.constraint(d)->expr, std::string("-(shift)"));
  CHECK(solve(sk).converged);
  CHECK_NEAR(sk.point(b)->x - sk.point(a)->x, 5, 1e-9);
  const auto copies = transform_entities(sk, {line}, SketchTransform{}, true);
  CHECK_EQ(copies.size(), size_t(1));
  sk.add_constraint(SkConstraint::Type::HDistance, {a}, 1);
  CHECK_THROWS(transform_entities(sk, {line}, t, false));
}

// UI-28: a fillet where a line meets a line, a line meets an arc, an arc meets an arc; the nearest one that fits, tangent
// to both, the curves ending where it touches them; refused (the sketch as it was) where none fits.
TEST(fillets_round_line_and_arc_corners) {
  using T=SkConstraint::Type;
  Sketch box;const int o=box.add_point(0,0),b=box.add_point(40,0),c=box.add_point(40,20);
  const int bottom=box.add_line(o,b),right=box.add_line(b,c);box.add_constraint(T::Horizontal,{bottom});box.add_constraint(T::Vertical,{right});
  FilletCorner f;CHECK(fillet_geometry(box,b,3,f));
  CHECK_NEAR(f.cx,37,1e-9);CHECK_NEAR(f.cy,3,1e-9);CHECK_NEAR(f.ax,37,1e-9);CHECK_NEAR(f.ay,0,1e-9);CHECK_NEAR(f.bx,40,1e-9);CHECK_NEAR(f.by,3,1e-9);
  const int round=fillet_corner(box,b,3);CHECK(!box.point(b));CHECK(solve(box).converged);
  CHECK_NEAR(box.point(box.entity(bottom)->p[1])->x,37,1e-7);CHECK_NEAR(box.point(box.entity(right)->p[0])->y,3,1e-7);
  CHECK_EQ(std::count_if(box.constraints.begin(),box.constraints.end(),[&](const SkConstraint& k){return k.type==T::Tangent && k.refs[1]==round;}),2);
  // A line along +X and a quarter arc about (-10, 0) leaving the corner upwards: the fillet outside the arc's circle.
  Sketch mixed;const int corner=mixed.add_point(0,0);
  const int line=mixed.add_line(corner,mixed.add_point(20,0)),arc=mixed.add_arc(mixed.add_point(-10,0),corner,mixed.add_point(-10,10));
  CHECK(fillet_geometry(mixed,corner,2,f));
  CHECK_NEAR(f.cx,-10+std::sqrt(140.0),1e-9);CHECK_NEAR(f.cy,2,1e-9);CHECK_NEAR(std::hypot(f.bx+10,f.by),10,1e-9);
  CHECK((f.first==line && f.second==arc) || (f.first==arc && f.second==line));
  const int fillet=fillet_corner(mixed,corner,2,"r");
  CHECK(solve(mixed).converged);mixed.validate();
  const SkEntity* made=mixed.entity(fillet);
  CHECK_NEAR(std::hypot(mixed.point(made->p[1])->x-mixed.point(made->p[0])->x,mixed.point(made->p[1])->y-mixed.point(made->p[0])->y),2,1e-7);
  CHECK(mixed.constraints.back().expr=="r" || std::any_of(mixed.constraints.begin(),mixed.constraints.end(),[](const SkConstraint& k){return k.type==T::Radius && k.expr=="r";}));
  CHECK_NEAR(mixed.point(mixed.entity(arc)->p[1])->x,f.bx,1e-7);  // the arc starts where the fillet touches it
  // Two arcs: the quarter about (-10, 0) up from the corner, a quarter about (0, 10) out along +X.
  Sketch arcs;const int k=arcs.add_point(0,0);
  arcs.add_arc(arcs.add_point(-10,0),k,arcs.add_point(-10,10));arcs.add_arc(arcs.add_point(0,10),k,arcs.add_point(10,10));
  CHECK(fillet_geometry(arcs,k,1,f));fillet_corner(arcs,k,1);CHECK(solve(arcs).converged);arcs.validate();
  // Too large, three curves, nothing there: refused, nothing changed.
  Sketch small;const int s0=small.add_point(0,0),s1=small.add_point(2,0);small.add_line(s0,s1);small.add_line(s1,small.add_point(2,2));
  const auto before=small.to_json();
  CHECK(!fillet_geometry(small,s1,5,f));CHECK_THROWS(fillet_corner(small,s1,5));CHECK(small.to_json()==before);
  small.add_line(s1,small.add_point(5,5));CHECK(!fillet_geometry(small,s1,0.1,f));
  CHECK(!fillet_geometry(small,s0,0.1,f));
}

TEST(one_click_extend_reaches_the_nearest_curve) {
  Sketch sk;const int a=sk.add_line(sk.add_point(0,0),sk.add_point(5,0));
  sk.add_line(sk.add_point(20,-5),sk.add_point(20,5));const int near=sk.add_line(sk.add_point(10,-5),sk.add_point(10,5));
  CHECK_EQ(extend_entity(sk,a,4.0,0.0),near);CHECK_NEAR(sk.point(sk.entity(a)->p[1])->x,10,1e-8);
  extend_entity(sk,a,9.0,0.0);CHECK_NEAR(sk.point(sk.entity(a)->p[1])->x,20,1e-8);
  CHECK_THROWS(extend_entity(sk,a,19.0,0.0));CHECK_NEAR(sk.point(sk.entity(a)->p[1])->x,20,1e-8);  // nothing further
  // An arc runs on round its circle to the line it meets.
  Sketch round;const int arc=round.add_arc(round.add_point(0,0),round.add_point(10,0),round.add_point(0,10));
  round.add_line(round.add_point(-20,5),round.add_point(0,5));
  extend_entity(round,arc,1.0,9.0);
  const SkPoint* end=round.point(round.entity(arc)->p[2]);CHECK_NEAR(end->y,5,1e-8);CHECK_NEAR(end->x,-std::sqrt(75.0),1e-8);
}

TEST(merging_points_shares_one_point) {
  using T=SkConstraint::Type;
  Sketch sk;const int a=sk.add_point(0,0),b=sk.add_point(10,0),c=sk.add_point(10.5,0.5),d=sk.add_point(20,5);
  const int first=sk.add_line(a,b),second=sk.add_line(c,d);sk.add_constraint(T::Coincident,{c,first});sk.add_constraint(T::Horizontal,{b,c});
  merge_points(sk,c,b);
  CHECK(!sk.point(c));CHECK(sk.entity(second)->p[0]==b);
  CHECK(std::none_of(sk.constraints.begin(),sk.constraints.end(),[](const SkConstraint& k){return k.type==T::Horizontal;}));  // on one point twice
  CHECK(solve(sk).converged);sk.validate();
  CHECK_THROWS(merge_points(sk,a,b));  // the first line would collapse
  const int e=sk.add_point(30,0);sk.add_constraint(T::Distance,{d,e},10);
  CHECK_THROWS(merge_points(sk,e,d));  // a dimension keeps them apart
  CHECK_THROWS(merge_points(sk,d,d));
}

TEST(trim_cuts_splines_and_ellipses_at_any_curve) {
  using T=SkConstraint::Type;
  // A fit spline through (0,0) ... (40,0), crossed by vertical lines at x = 15 and 25.
  Sketch sk;SkEntity fit;fit.type=E::Spline;
  for(auto [x,y]:std::vector<std::pair<double,double>>{{0,0},{10,4},{20,0},{30,4},{40,0}})fit.p.push_back(sk.add_point(x,y));
  fit.id=sk.next_id();sk.entities.push_back(fit);
  const int spline=fit.id,start=fit.p.front(),end=fit.p.back();
  const int left=sk.add_line(sk.add_point(15,-10),sk.add_point(15,10)),right=sk.add_line(sk.add_point(25,-10),sk.add_point(25,10));
  const CurveCuts cuts=curve_cuts(sk,spline);
  CHECK_EQ(cuts.at.size(),size_t(2));CHECK_EQ(cuts.at[0].second,left);CHECK_EQ(cuts.at[1].second,right);
  CHECK_EQ(curve_crossings(sk,*sk.entity(left),*sk.entity(spline)).size(),size_t(1));  // a spline cuts a line too
  // A filter (an editor's samples) keeps a curve it rules out from the kernel: the right line only.
  const CurveCuts filtered=curve_cuts(sk,spline,[&](const SkEntity& o){return o.id!=left;});
  CHECK_EQ(filtered.at.size(),size_t(1));CHECK_EQ(filtered.at[0].second,right);CHECK_NEAR(filtered.at[0].first,cuts.at[1].first,1e-12);
  // The middle goes: from the old start to the left line, from the right line to the old end; the ends on the lines.
  const auto pieces=trim_curve(sk,spline,20,0.5);
  CHECK_EQ(pieces.size(),size_t(2));CHECK_EQ(pieces[0],spline);
  const SkEntity a=*sk.entity(pieces[0]),b=*sk.entity(pieces[1]);
  CHECK(a.type==E::Spline && a.degree>0 && a.p.front()==start && b.p.back()==end);
  CHECK_NEAR(sk.point(a.p.back())->x,15,1e-6);CHECK_NEAR(sk.point(b.p.front())->x,25,1e-6);
  auto on=[&](int point,int line){return std::any_of(sk.constraints.begin(),sk.constraints.end(),[&](const SkConstraint& c){return c.type==T::Coincident && c.refs==std::vector<int>{point,line};});};
  CHECK(on(a.p.back(),left) && on(b.p.front(),right));
  CHECK(!sk.point(fit.p[2]));  // the fit points went with the old curve
  CHECK(solve(sk).converged);sk.validate();
  // Nothing crosses the right piece past its start: a trim takes all of it, its old end point too.
  CHECK(trim_curve(sk,pieces[1],35,2).empty());CHECK(!sk.entity(pieces[1]));CHECK(!sk.point(end));
  // An ellipse crossed twice by a line loses the side clicked: one piece round the other side, its ends on the line.
  Sketch el;SkEntity ellipse;ellipse.type=E::Ellipse;ellipse.p={el.add_point(0,0),el.add_point(10,0)};ellipse.r=5;ellipse.id=el.next_id();el.entities.push_back(ellipse);
  const int cut=el.add_line(el.add_point(5,-10),el.add_point(5,10));
  CHECK_EQ(curve_cuts(el,ellipse.id).at.size(),size_t(2));
  std::vector<TrimPiece> keep,gone;CHECK(trim_pieces(curve_cuts(el,ellipse.id),10,0,keep,gone));CHECK_EQ(gone.size(),size_t(1));
  const auto rest=trim_curve(el,ellipse.id,10,0);
  CHECK_EQ(rest.size(),size_t(1));
  const SkEntity& r=*el.entity(rest[0]);
  CHECK(r.type==E::Spline);CHECK_NEAR(el.point(r.p.front())->x,5,1e-6);CHECK_NEAR(el.point(r.p.back())->x,5,1e-6);
  BRepAdaptor_Curve c(entity_edge(el,r,{}));
  double leftmost=0;for(int i=0;i<=20;++i)leftmost=std::min(leftmost,c.Value(c.FirstParameter()+(c.LastParameter()-c.FirstParameter())*i/20).X());
  CHECK_NEAR(leftmost,-10,1e-3);  // the far side stays
  CHECK(solve(el).converged);
  // Crossed once (a line from the centre out), a closed curve has nothing to cut between: refused, as it was.
  Sketch once;SkEntity e2=ellipse;e2.p={once.add_point(0,0),once.add_point(10,0)};e2.id=once.next_id();once.entities.push_back(e2);
  once.add_line(once.add_point(0,0),once.add_point(20,0));
  const auto json=once.to_json();
  CHECK_THROWS(trim_curve(once,e2.id,-10,0));CHECK(once.to_json()==json);
  CHECK_THROWS(trim_curve(once,once.entities.back().id,5,0));  // a line is the editor's to trim
}
