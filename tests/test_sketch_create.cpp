#include "check.hpp"
#include "opad/design/sketch_create.hpp"
#include "opad/design/sketch_geom.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <cmath>
using namespace opad::design;
using E=SkEntity::Type;

TEST(advanced_primitives_form_exact_profiles) {
  for(const std::string kind:{"rect3","circle2","polygon_outer","cslot","arcslot"}) {
    Sketch sk;
    std::vector<std::pair<double,double>> picks={{0,0},{20,0},{0,10}};
    if(kind=="cslot")picks={{0,0},{20,0},{20,3}};
    if(kind=="arcslot")picks={{0,0},{20,0},{0,20}};
    auto ids=create_primitive(sk,kind,picks,{{"sides",5},{"width",4.0}});
    CHECK(!ids.empty());CHECK(solve(sk).converged);
    auto profiles=sketch_regions(sk,opad::Frame{});
    CHECK_EQ(profiles.size(),size_t(1));
    if(kind=="rect3")CHECK_NEAR(profiles[0].area,200,1e-5);
    if(kind=="circle2")CHECK_NEAR(profiles[0].area,100*M_PI,1e-5);
    if(kind=="arcslot")CHECK_NEAR(profiles[0].area,40*M_PI+4*M_PI,1e-5);
  }
}

TEST(control_spline_and_rational_conic_are_native_curves) {
  Sketch sk;
  auto ids=create_primitive(sk,"control_spline",{{0,0},{5,10},{15,10},{20,0}});
  const auto* e=sk.entity(ids[0]);CHECK_EQ(e->degree,3);
  BRepAdaptor_Curve curve(entity_edge(sk,*e,opad::Frame{}));CHECK(curve.GetType()==GeomAbs_BSplineCurve);
  CHECK_NEAR(curve.Value(0.5).Y(),7.5,1e-8);
  ids=create_primitive(sk,"conic",{{30,0},{40,20},{50,0}},{{"rho",0.5}});
  e=sk.entity(ids[0]);CHECK_EQ(e->degree,2);CHECK_EQ(e->weights.size(),size_t(3));
  BRepAdaptor_Curve conic(entity_edge(sk,*e,opad::Frame{}));CHECK_NEAR(conic.Value(0.5).Y(),10,1e-8);
  const auto before=sk.to_json();CHECK_THROWS(create_primitive(sk,"conic",{{0,0},{1,1},{2,0}},{{"rho",1}}));CHECK(sk.to_json()==before);
}

TEST(tangent_primitives_preserve_tangency) {
  Sketch sk;
  int a=sk.add_point(-20,0,true),b=sk.add_point(20,0,true),c=sk.add_point(0,-20,true),d=sk.add_point(0,20,true);
  int l1=sk.add_line(a,b,true),l2=sk.add_line(c,d,true);
  auto ids=create_primitive(sk,"tangent_circle",{{4,4}},{{"lines",{l1,l2}},{"radius",3.0}});
  CHECK(solve(sk).converged);auto* center=sk.point(sk.entity(ids[0])->p[0]);CHECK_NEAR(center->x,3,1e-7);CHECK_NEAR(center->y,3,1e-7);
  ids=create_primitive(sk,"tangent_arc",{{20,0},{30,10}},{{"line",l1}});
  CHECK(solve(sk).converged);auto* e=sk.entity(ids[0]);CHECK(e->type==E::Arc);
  CHECK_NEAR(sk.point(e->p[0])->x,20,1e-8);CHECK_NEAR(sk.point(e->p[0])->y,10,1e-8);
}
CHECK_MAIN()
