#include "opad/design/sketch_create.hpp"
#include "opad/design/sketch_text.hpp"
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

std::vector<int> create_primitive(Sketch& sketch,const std::string& kind,const std::vector<V>& picks,const json& given) {
  const json options=given.is_object()?given:json::object();  // null (no options) reads as none
  Sketch sk=sketch; // the whole creation, including validation, is atomic
  const bool construction=options.value("construction",false);
  std::vector<int> made;
  auto at=[&](size_t i){if(i>=picks.size())throw Error("more points are needed");const auto p=picks[i];if(!std::isfinite(p.first)||!std::isfinite(p.second))throw Error("invalid point");return p;};
  auto point=[&](V p){return sk.add_point(p.first,p.second);};
  auto line=[&](int a,int b){int id=sk.add_line(a,b,construction);made.push_back(id);return id;};
  auto arc=[&](int o,int a,int b){int id=sk.add_arc(o,a,b,construction);made.push_back(id);return id;};
  auto curve=[&](SkEntity e){e.id=sk.next_id();e.construction=construction;made.push_back(e.id);sk.entities.push_back(std::move(e));};
  auto get=[&](int id){const auto* p=sk.point(id);if(!p)throw Error("missing point");return V{p->x,p->y};};
  auto constrain=[&](SkConstraint::Type t,std::vector<int> refs){sk.add_constraint(t,std::move(refs));};
  auto positive=[&](const char* name){const double value=options.value(name,0.0);if(!(value>0)||!std::isfinite(value))throw Error(std::string(name)+" must be a positive number of mm");return value;};
  // An arc from a to b about o, running counter-clockwise when ccw (else clockwise, stored the other way round).
  auto arc_between=[&](int o,int a,int b,bool ccw){return ccw?arc(o,a,b):arc(o,b,a);};
  if(kind=="point") {
    point(at(0));
  } else if(kind=="line") {
    line(point(at(0)),point(at(1)));
  } else if(kind=="circle") {
    const double radius=options.contains("radius")?positive("radius"):options.contains("diameter")?positive("diameter")/2:length(at(1)-at(0));
    if(!(radius>1e-9))throw Error("a circle needs a radius, a diameter or a point on it");
    made.push_back(sk.add_circle(point(at(0)),radius,construction));
  } else if(kind=="rect2" || kind=="rect_center") {
    // Axis-aligned: two opposite corners, or the centre and a corner.
    const V a=at(0),b=at(1),c=kind=="rect2"?a:a*2-b;
    const double x0=std::min(c.first,b.first),x1=std::max(c.first,b.first),y0=std::min(c.second,b.second),y1=std::max(c.second,b.second);
    if(x1-x0<1e-9||y1-y0<1e-9)throw Error("a rectangle needs a width and a height");
    const int p0=point({x0,y0}),p1=point({x1,y0}),p2=point({x1,y1}),p3=point({x0,y1});
    const int l0=line(p0,p1),l1=line(p1,p2),l2=line(p2,p3),l3=line(p3,p0);
    constrain(SkConstraint::Type::Horizontal,{l0});constrain(SkConstraint::Type::Horizontal,{l2});
    constrain(SkConstraint::Type::Vertical,{l1});constrain(SkConstraint::Type::Vertical,{l3});
    if(kind=="rect_center"){const int o=point(a),d=sk.add_line(p0,p2,true);made.push_back(d);constrain(SkConstraint::Type::Midpoint,{o,d});}
  } else if(kind=="rounded_rect") {
    // Axis-aligned with the corners rounded: two opposite corners (options.center: the centre and a corner).
    const V a=at(0),b=at(1),c=options.value("center",false)?a*2-b:a;
    const double x0=std::min(c.first,b.first),x1=std::max(c.first,b.first),y0=std::min(c.second,b.second),y1=std::max(c.second,b.second),r=positive("radius");
    if(2*r>=std::min(x1-x0,y1-y0)-1e-9)throw Error("the corner radius must be less than half the rectangle's width and height");
    const int b0=point({x0+r,y0}),b1=point({x1-r,y0}),r0=point({x1,y0+r}),r1=point({x1,y1-r}),t0=point({x1-r,y1}),t1=point({x0+r,y1}),l0=point({x0,y1-r}),l1=point({x0,y0+r});
    const int bottom=line(b0,b1),right=line(r0,r1),top=line(t0,t1),left=line(l0,l1);
    const int br=arc(point({x1-r,y0+r}),b1,r0),tr=arc(point({x1-r,y1-r}),r1,t0),tl=arc(point({x0+r,y1-r}),t1,l0),bl=arc(point({x0+r,y0+r}),l1,b0);
    constrain(SkConstraint::Type::Horizontal,{bottom});constrain(SkConstraint::Type::Horizontal,{top});
    constrain(SkConstraint::Type::Vertical,{right});constrain(SkConstraint::Type::Vertical,{left});
    for(auto [l,k]:std::vector<std::pair<int,int>>{{bottom,br},{right,br},{right,tr},{top,tr},{top,tl},{left,tl},{left,bl},{bottom,bl}})constrain(SkConstraint::Type::Tangent,{l,k});
    for(int k:{tr,tl,bl})constrain(SkConstraint::Type::Equal,{br,k});
  } else if(kind=="arc3") {
    // Through three points, from the first to the last.
    const V s=at(0),m=at(1),e=at(2);const double d=2*cross(m-s,e-s);
    if(std::fabs(d)<1e-12)throw Error("the three arc points must not lie on one line");
    const double ss=dot(s,s),mm=dot(m,m),ee=dot(e,e);
    const V o={(ss*(m.second-e.second)+mm*(e.second-s.second)+ee*(s.second-m.second))/d,(ss*(e.first-m.first)+mm*(s.first-e.first)+ee*(m.first-s.first))/d};
    arc_between(point(o),point(s),point(e),cross(m-s,e-m)>0);
  } else if(kind=="arc_radius") {
    // From start to end with a radius: counter-clockwise unless options.direction is "cw", the shorter way round
    // unless options.large.
    const V s=at(0),e=at(1);const double r=positive("radius"),half=length(e-s)/2;
    if(half<1e-9)throw Error("the arc ends must be different");
    if(r<half-1e-9)throw Error("the radius is less than half the distance between the ends");
    const bool ccw=options.value("direction","ccw")!="cw",large=options.value("large",false);
    const double h=std::sqrt(std::max(0.0,r*r-half*half));
    const V o=(s+e)*0.5+normal(unit(e-s))*(h*((ccw!=large)?1.0:-1.0));
    arc_between(point(o),point(s),point(e),ccw);
  } else if(kind=="slot") {
    // Two end centres and options.width: a stadium.
    const V c1=at(0),c2=at(1),n=normal(unit(c2-c1));const double r=positive("width")/2;
    const int o1=point(c1),o2=point(c2),a=point(c1+n*r),b=point(c2+n*r),c=point(c2-n*r),d=point(c1-n*r);
    const int top=line(a,b),bottom=line(c,d),cap1=arc(o1,a,d),cap2=arc(o2,c,b);
    for(int l:{top,bottom})for(int cap:{cap1,cap2})constrain(SkConstraint::Type::Tangent,{l,cap});
    constrain(SkConstraint::Type::Equal,{cap1,cap2});
  } else if(kind=="path") {
    // A polyline through the picks, closed unless options.closed is false. options.segments[i] makes segment i
    // (pick i to pick i+1) an arc: "tangent" (tangent to the segment before it), "tangent_next" (tangent to the one
    // after it) or a radius (positive: counter-clockwise, negative: clockwise; the shorter way round). options.fillet
    // rounds every corner between two straight segments; options.fillets[i] sets pick i's own (0: none).
    const bool closed=options.value("closed",true);const size_t n=picks.size();
    if(n<(closed?3u:2u))throw Error(closed?"a closed path needs at least three points":"a path needs at least two points");
    const size_t segments=closed?n:n-1;
    const json specs=options.value("segments",json::array());
    auto spec=[&](size_t i)->json{return i<specs.size()?specs[i]:json("line");};
    auto straight=[&](size_t i){const json s=spec(i%segments);return s.is_null()||(s.is_string()&&s.get<std::string>()=="line");};
    std::vector<V> pts;for(size_t i=0;i<n;++i)pts.push_back(at(i));
    // Fillets at corners between two straight segments.
    const double all=options.value("fillet",0.0);const json each=options.value("fillets",json::array());
    std::vector<double> radius(n,0);
    for(size_t i=0;i<n;++i){
      if(!closed&&(i==0||i==n-1))continue;
      const size_t before=(i+segments-1)%segments,after=i%segments;
      if(!straight(before)||!straight(after))continue;
      radius[i]=i<each.size()&&each[i].is_number()?each[i].get<double>():all;
      if(radius[i]<0||!std::isfinite(radius[i]))throw Error("fillet radii must not be negative");
    }
    // Where each straight segment starts and ends once its corners are rounded.
    std::vector<V> start(segments),end(segments);std::vector<V> centre(n);
    for(size_t i=0;i<segments;++i){start[i]=pts[i];end[i]=pts[(i+1)%n];}
    std::vector<double> reach(n,0);
    for(size_t i=0;i<n;++i)if(radius[i]>0){
      const V v=pts[i],u1=unit(pts[(i+n-1)%n]-v),u2=unit(pts[(i+1)%n]-v);
      const double angle=std::acos(std::clamp(dot(u1,u2),-1.0,1.0));
      if(angle>M_PI-1e-6){radius[i]=0;continue;}  // straight on: no corner to round
      if(angle<1e-6)throw Error("the path turns back on itself at point "+std::to_string(i));
      reach[i]=radius[i]/std::tan(angle/2);centre[i]=v+unit(u1+u2)*(radius[i]/std::sin(angle/2));
      start[i%segments]=v+u2*reach[i];end[(i+segments-1)%segments]=v+u1*reach[i];
    }
    for(size_t i=0;i<segments;++i)if(straight(i)&&length(pts[(i+1)%n]-pts[i])<reach[i]+reach[(i+1)%n]-1e-9)
      throw Error("the fillets at the ends of segment "+std::to_string(i)+" do not fit on it");
    // Points: the picks where no fillet replaces them, and the fillets' tangent points.
    std::vector<int> at_pick(n,0);for(size_t i=0;i<n;++i)if(radius[i]<=0)at_pick[i]=point(pts[i]);
    std::vector<int> seg_start(segments),seg_end(segments);
    for(size_t i=0;i<segments;++i){seg_start[i]=radius[i]>0?point(start[i]):at_pick[i];}
    for(size_t i=0;i<segments;++i){const size_t j=(i+1)%n;seg_end[i]=radius[j]>0?point(end[i]):at_pick[j];}
    std::vector<int> ids(segments,0);
    auto direction=[&](size_t i){return unit(pts[(i+1)%n]-pts[i]);};
    for(size_t i=0;i<segments;++i){
      const json s=spec(i);const V a=get(seg_start[i]),b=get(seg_end[i]);
      if(straight(i)){ids[i]=line(seg_start[i],seg_end[i]);continue;}
      if(s.is_string()&&(s.get<std::string>()=="tangent"||s.get<std::string>()=="tangent_next")){
        const bool next=s.get<std::string>()=="tangent_next";
        if(!closed&&(next?i+1>=segments:i==0))throw Error("segment "+std::to_string(i)+" has no neighbour to be tangent to");
        const size_t other=next?(i+1)%segments:(i+segments-1)%segments;
        if(!straight(other))throw Error("segment "+std::to_string(i)+" can only be tangent to a straight segment");
        // The centre lies on the neighbour's normal at the shared point, equally far from both ends.
        const V p=next?b:a,q=next?a:b,t=direction(other),nn=normal(t),d=q-p;const double den=2*dot(d,nn);
        if(std::fabs(den)<1e-12)throw Error("segment "+std::to_string(i)+" cannot be an arc tangent there");
        const V o=p+nn*(dot(d,d)/den);
        // Counter-clockwise when that way the arc leaves (or reaches) the shared point along the neighbour.
        const bool ccw=dot(normal(p-o),t)>0;
        ids[i]=arc_between(point(o),seg_start[i],seg_end[i],ccw);continue;
      }
      if(!s.is_number())throw Error("segment "+std::to_string(i)+": use \"line\", \"tangent\", \"tangent_next\" or a radius");
      const double r=std::fabs(s.get<double>()),half=length(b-a)/2;const bool ccw=s.get<double>()>0;
      if(r<half-1e-9)throw Error("segment "+std::to_string(i)+": the radius is less than half its length");
      const double h=std::sqrt(std::max(0.0,r*r-half*half));
      ids[i]=arc_between(point((a+b)*0.5+normal(unit(b-a))*(ccw?h:-h)),seg_start[i],seg_end[i],ccw);
    }
    for(size_t i=0;i<segments;++i){
      const json s=spec(i);if(!s.is_string())continue;const std::string t=s.get<std::string>();
      if(t=="tangent")constrain(SkConstraint::Type::Tangent,{ids[(i+segments-1)%segments],ids[i]});
      if(t=="tangent_next")constrain(SkConstraint::Type::Tangent,{ids[i],ids[(i+1)%segments]});
    }
    for(size_t i=0;i<n;++i)if(radius[i]>0){
      const size_t before=(i+segments-1)%segments,after=i%segments;
      const int k=arc_between(point(centre[i]),seg_end[before],seg_start[after],cross(get(seg_end[before])-centre[i],get(seg_start[after])-centre[i])>=0);
      constrain(SkConstraint::Type::Tangent,{ids[before],k});constrain(SkConstraint::Type::Tangent,{ids[after],k});
    }
  } else if(kind=="text") {
    // Built-in font from the first pick along the sketch's x: options.text, height (mm), style (outline: closed
    // letters to extrude, stroke: single lines), weight, align (left, center, right).
    TextOptions t;t.height=options.value("height",10.0);t.style=options.value("style","outline");t.weight=options.value("weight",0.14);t.align=options.value("align","left");t.construction=construction;
    const V o=at(0);
    for(int id:add_text(sk,options.value("text",std::string()),o.first,o.second,t))made.push_back(id);
  } else if(kind=="rect3") {
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
