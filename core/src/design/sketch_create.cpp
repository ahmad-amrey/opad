#include "opad/design/sketch_create.hpp"
#include <algorithm>
#include <cmath>

namespace opad::design {
namespace {
using V=std::pair<double,double>;
V operator+(V a,V b){return {a.first+b.first,a.second+b.second};}
V operator-(V a,V b){return {a.first-b.first,a.second-b.second};}
V operator*(V a,double t){return {a.first*t,a.second*t};}
double cross(V a,V b){return a.first*b.second-a.second*b.first;}
double dot(V a,V b){return a.first*b.first+a.second*b.second;}
double length(V a){return std::hypot(a.first,a.second);}
V normal(V a){return {-a.second,a.first};}
V unit(V a){double n=length(a);if(n<1e-9)throw Error("the picked points must be different");return a*(1/n);}
}

std::vector<int> create_primitive(Sketch& sketch,const std::string& kind,const std::vector<V>& picks,const json& options) {
  Sketch sk=sketch; // the whole creation, including validation, is atomic
  const bool construction=options.value("construction",false);
  std::vector<int> made;
  auto at=[&](size_t i){if(i>=picks.size())throw Error("more points are needed");const auto p=picks[i];if(!std::isfinite(p.first)||!std::isfinite(p.second))throw Error("invalid point");return p;};
  auto point=[&](V p){return sk.add_point(p.first,p.second);};
  auto line=[&](int a,int b){int id=sk.add_line(a,b,construction);made.push_back(id);return id;};
  auto arc=[&](int o,int a,int b){int id=sk.add_arc(o,a,b,construction);made.push_back(id);return id;};
  auto curve=[&](SkEntity e){e.id=sk.next_id();e.construction=construction;made.push_back(e.id);sk.entities.push_back(std::move(e));};
  auto get=[&](int id){const auto* p=sk.point(id);if(!p)throw Error("missing point");return V{p->x,p->y};};
  if(kind=="rect3") {
    V a=at(0),b=at(1),n=normal(unit(b-a));double h=dot(at(2)-b,n);if(std::fabs(h)<1e-9)throw Error("rectangle height must be positive");
    int p=point(a),q=point(b),r=point(b+n*h),s=point(a+n*h);
    int l0=line(p,q),l1=line(q,r),l2=line(r,s),l3=line(s,p);
    sk.add_constraint(SkConstraint::Type::Perpendicular,{l0,l1});sk.add_constraint(SkConstraint::Type::Parallel,{l0,l2});sk.add_constraint(SkConstraint::Type::Parallel,{l1,l3});
  } else if(kind=="circle2") {
    V a=at(0),b=at(1);double radius=length(b-a)/2;if(radius<1e-9)throw Error("circle needs two different points");
    made.push_back(sk.add_circle(point((a+b)*0.5),radius,construction));
  } else if(kind=="polygon_outer") {
    const int count=options.value("sides",6);if(count<3||count>256)throw Error("polygon needs 3 to 256 sides");
    V o=at(0),p=at(1);const double r=length(p-o)/std::cos(M_PI/count),start=std::atan2(p.second-o.second,p.first-o.first)+M_PI/count;
    std::vector<int> ids;for(int i=0;i<count;++i){double a=start+2*M_PI*i/count;ids.push_back(point(o+V{std::cos(a),std::sin(a)}*r));}
    std::vector<int> lines;for(int i=0;i<count;++i)lines.push_back(line(ids[i],ids[(i+1)%count]));
    const int circle=sk.add_circle(point(o),r,true);made.push_back(circle);
    for(int p:ids)sk.add_constraint(SkConstraint::Type::Coincident,{p,circle});
    for(int i=1;i<count;++i)sk.add_constraint(SkConstraint::Type::Equal,{lines[0],lines[i]});
  } else if(kind=="cslot") {
    V o=at(0),end=at(1),start=o*2-end,n=normal(unit(end-start));double r=std::fabs(dot(at(2)-end,n));if(r<1e-9)throw Error("slot width must be positive");
    int c1=point(start),c2=point(end),a=point(start+n*r),b=point(end+n*r),c=point(end-n*r),d=point(start-n*r);
    int l0=line(a,b),l1=line(c,d),cap0=arc(c1,a,d),cap1=arc(c2,c,b);
    for(int l:{l0,l1})for(int cap:{cap0,cap1})sk.add_constraint(SkConstraint::Type::Tangent,{l,cap});sk.add_constraint(SkConstraint::Type::Equal,{cap0,cap1});
  } else if(kind=="arcslot") {
    V o=at(0),start=at(1),end=at(2);double r=length(start-o),w=options.value("width",2.0)/2;
    if(w<=0||w>=r)throw Error("arc slot width must be positive and smaller than its diameter");
    V a=unit(start-o),b=unit(end-o);if(std::fabs(cross(a,b))<1e-9&&dot(a,b)>0)throw Error("arc slot ends must be different");
    int center=point(o),cs=point(o+a*r),ce=point(o+b*r),os=point(o+a*(r+w)),oe=point(o+b*(r+w)),is=point(o+a*(r-w)),ie=point(o+b*(r-w));
    arc(center,os,oe);arc(center,is,ie);arc(ce,oe,ie);arc(cs,is,os);
  } else if(kind=="conic" || kind=="control_spline") {
    SkEntity e;e.type=SkEntity::Type::Spline;
    if(kind=="conic") {
      double rho=options.value("rho",0.5);if(rho<=0||rho>=1)throw Error("conic rho must be between zero and one");
      e.degree=2;e.weights={1,rho/(1-rho),1};for(int i=0;i<3;++i)e.p.push_back(point(at(i)));
      e.knots={0,1};e.multiplicities={3,3};
    } else {
      if(picks.size()<2)throw Error("a spline needs at least two control points");
      e.degree=std::min(options.value("degree",3),int(picks.size())-1);if(e.degree<1||e.degree>25)throw Error("invalid spline degree");
      for(size_t i=0;i<picks.size();++i)e.p.push_back(point(at(i)));
      e.weights.assign(e.p.size(),1);
      const int spans=int(e.p.size())-e.degree;
      for(int i=0;i<=spans;++i){e.knots.push_back(double(i)/spans);e.multiplicities.push_back(i==0||i==spans?e.degree+1:1);}
    }
    curve(std::move(e));
  } else if(kind=="tangent_circle") {
    const auto ids=options.at("lines").get<std::vector<int>>();if(ids.size()!=2)throw Error("pick two lines");
    auto l1=sk.entity(ids[0]),l2=sk.entity(ids[1]);if(!l1||!l2||l1->type!=SkEntity::Type::Line||l2->type!=SkEntity::Type::Line)throw Error("pick two straight lines");
    V a=get(l1->p[0]),b=get(l2->p[0]),n1=normal(unit(get(l1->p[1])-a)),n2=normal(unit(get(l2->p[1])-b));
    const double den=cross(n1,n2),r=options.value("radius",2.0);if(std::fabs(den)<1e-9||r<=0)throw Error("tangent circle needs nonparallel lines and a positive radius");
    V center;double best=INFINITY;
    for(int s:{-1,1})for(int t:{-1,1}){double h1=dot(n1,a)+s*r,h2=dot(n2,b)+t*r;V c={(h1*n2.second-n1.second*h2)/den,(n1.first*h2-h1*n2.first)/den};double d=length(c-at(0));if(d<best){best=d;center=c;}}
    const int id=sk.add_circle(point(center),r,construction);made.push_back(id);
    sk.add_constraint(SkConstraint::Type::Tangent,{ids[0],id});sk.add_constraint(SkConstraint::Type::Tangent,{ids[1],id});sk.add_constraint(SkConstraint::Type::Radius,{id},r);
  } else if(kind=="tangent_arc") {
    const int id=options.at("line").get<int>();const auto* e=sk.entity(id);if(!e||e->type!=SkEntity::Type::Line)throw Error("pick a line end");
    const int end=length(get(e->p[0])-at(0))<length(get(e->p[1])-at(0))?e->p[0]:e->p[1];
    const V a=get(end),b=at(1),n=normal(unit(get(e->p[1])-get(e->p[0]))),d=b-a;
    const double denominator=2*dot(d,n);if(std::fabs(denominator)<1e-9)throw Error("arc endpoint must lie off the tangent line");
    const V center=a+n*(dot(d,d)/denominator);int o=point(center),last=point(b);
    const int madeArc=cross(a-center,b-center)>=0?arc(o,end,last):arc(o,last,end);
    sk.add_constraint(SkConstraint::Type::Tangent,{id,madeArc});
  } else throw Error("unknown sketch primitive: "+kind);
  sk.validate();sketch=std::move(sk);return made;
}
}
