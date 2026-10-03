#include "opad/design/sketch_modify.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <BRepOffsetAPI_MakeOffset.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <GeomAPI.hxx>
#include <Geom2dAPI_InterCurveCurve.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <algorithm>
#include <cmath>
#include <set>

namespace opad::design {
std::vector<int> connected_entities(const Sketch& sk,const std::vector<int>& seeds) {
  std::set<int> ids(seeds.begin(),seeds.end()),points;
  for(int id:ids)if(sk.point(id))points.insert(id);
  std::map<int,std::vector<const SkEntity*>> incident;
  for(const auto& e:sk.entities)for(int p:e.p){incident[p].push_back(&e);if(ids.count(e.id))points.insert(p);}
  std::vector<int> pending(points.begin(),points.end());
  for(size_t i=0;i<pending.size();++i)for(const auto* e:incident[pending[i]])
    if(ids.insert(e->id).second)for(int p:e->p)if(points.insert(p).second)pending.push_back(p);
  return {ids.begin(),ids.end()};
}
std::vector<int> transform_entities(Sketch& sk,const std::vector<int>& ids,const SketchTransform& t,bool copy) {
  if(!std::isfinite(t.scale)||t.scale<=0||!std::isfinite(t.angle)||!std::isfinite(t.x)||!std::isfinite(t.y))throw Error("invalid sketch transform");
  std::map<int,int> mapped;std::vector<SkEntity> source;std::vector<int> result;std::set<int> points;
  for(int id:ids)if(const auto* e=sk.entity(id)){source.push_back(*e);for(int p:e->p)points.insert(p);}
  if(source.empty())throw Error("select curves to transform");
  const std::set<int> selected(ids.begin(),ids.end());
  if(!copy)for(const auto& e:source)if(e.fixed)throw Error("break projection or pattern links before transforming fixed curves");
  if(!copy)for(const auto& c:sk.constraints)if(c.type==SkConstraint::Type::Fix)for(int ref:c.refs)if(selected.count(ref)||points.count(ref))throw Error("unlock fixed geometry before transforming it");
  auto coordinate=[](const SkConstraint& c){return (c.type==SkConstraint::Type::HDistance||c.type==SkConstraint::Type::VDistance)&&c.refs.size()==1;};
  if(!copy)for(const auto& c:sk.constraints)if(coordinate(c)&&points.count(c.refs[0]))throw Error("remove the coordinate dimensions of points before transforming them");
  // A signed distance keeps its meaning after a quarter turn, a half turn or a mirror: its sign follows the points.
  auto follow=[&](SkConstraint& c){
    if(!c.is_signed||c.refs.size()!=2)return;const double now=dimension_value(sk,c);
    if(std::fabs(now+c.value)<1e-6*std::max(1.0,std::fabs(c.value))&&std::fabs(c.value)>1e-12){c.value=-c.value;if(!c.expr.empty())c.expr="-("+c.expr+")";}
  };
  if(!copy && std::fabs(std::sin(t.angle*2))>1e-9)for(const auto& c:sk.constraints)
    if((c.type==SkConstraint::Type::Horizontal||c.type==SkConstraint::Type::Vertical||c.type==SkConstraint::Type::HDistance||c.type==SkConstraint::Type::VDistance)&&std::all_of(c.refs.begin(),c.refs.end(),[&](int id){return selected.count(id)||points.count(id);}))throw Error("remove horizontal or vertical constraints before rotating to an oblique angle");
  for(int id:points){auto p=*sk.point(id);const auto original=p;double x=p.x-t.cx,y=p.y-t.cy;if(t.mirror)y=-y;const double a=t.angle;
    p.x=t.cx+t.x+t.scale*(x*std::cos(a)-y*std::sin(a));p.y=t.cy+t.y+t.scale*(x*std::sin(a)+y*std::cos(a));
    if(!copy && p.fixed && std::hypot(p.x-original.x,p.y-original.y)>1e-9)throw Error("fixed geometry cannot be transformed; break its link first");
    if(copy){mapped[id]=sk.add_point(p.x,p.y);}else{*sk.point(id)=p;mapped[id]=id;}}
  for(auto e:source){const int old=e.id;if(copy){e.id=sk.next_id();e.fixed=false;e.source=nullptr;for(int& p:e.p)p=mapped.at(p);}if(t.mirror&&e.type==SkEntity::Type::Arc)std::swap(e.p[1],e.p[2]);e.r*=t.scale;result.push_back(e.id);mapped[old]=e.id;if(copy)sk.entities.push_back(e);else *sk.entity(e.id)=e;}
  if(copy){const auto constraints=sk.constraints;for(auto c:constraints){if(!std::all_of(c.refs.begin(),c.refs.end(),[&](int id){return mapped.count(id);}))continue;
    if(c.type==SkConstraint::Type::Fix||coordinate(c))continue;
    if(std::fabs(std::sin(t.angle))>1e-9 && (c.type==SkConstraint::Type::Horizontal||c.type==SkConstraint::Type::Vertical||c.type==SkConstraint::Type::HDistance||c.type==SkConstraint::Type::VDistance))continue;
    c.id=sk.next_id();for(int& r:c.refs)r=mapped.at(r);for(int& r:c.anchors)r=mapped.at(r);if(c.is_dimension()&&c.type!=SkConstraint::Type::Angle){c.value*=t.scale;if(!c.expr.empty()&&t.scale!=1)c.expr="("+c.expr+")*"+json(t.scale).dump();}follow(c);c.pos[0]+=t.x;c.pos[1]+=t.y;sk.constraints.push_back(c);}}
  if(!copy)for(auto& c:sk.constraints)if(std::all_of(c.refs.begin(),c.refs.end(),[&](int id){return mapped.count(id);})) {
    if(c.is_dimension() && c.type!=SkConstraint::Type::Angle && !c.reference){c.value*=t.scale;if(!c.expr.empty() && t.scale!=1)c.expr="("+c.expr+")*"+json(t.scale).dump();}
    if(std::fabs(std::sin(t.angle))>1-1e-9){using T=SkConstraint::Type;if(c.type==T::Horizontal)c.type=T::Vertical;else if(c.type==T::Vertical)c.type=T::Horizontal;else if(c.type==T::HDistance)c.type=T::VDistance;else if(c.type==T::VDistance)c.type=T::HDistance;}
    follow(c);
  }
  sk.validate();return result;
}

std::vector<int> append_sketch_shape(Sketch& sk,const TopoDS_Shape& shape,const Frame& frame,bool construction,bool fixed) {
  std::map<std::pair<long long,long long>,int> points;std::vector<int> made;
  auto point=[&](const gp_Pnt& p){double x,y;frame.to_local({p.X(),p.Y(),p.Z()},x,y);const auto key=std::make_pair(std::llround(x*1e8),std::llround(y*1e8));auto it=points.find(key);return it==points.end()?points[key]=sk.add_point(x,y,fixed):it->second;};
  const auto n=frame.normal();gp_Dir normal(n[0],n[1],n[2]);TopTools_IndexedMapOfShape edges;TopExp::MapShapes(shape,TopAbs_EDGE,edges);
  for(int i=1;i<=edges.Extent();++i){BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));const double first=c.FirstParameter(),last=c.LastParameter();int id=0;
    if(c.GetType()==GeomAbs_Line){int a=point(c.Value(first)),b=point(c.Value(last));if(a!=b)id=sk.add_line(a,b,construction);}
    else if(c.GetType()==GeomAbs_Circle&&std::fabs(c.Circle().Axis().Direction().Dot(normal))>1-1e-9){const int o=point(c.Circle().Location());if(last-first>2*M_PI-1e-8)id=sk.add_circle(o,c.Circle().Radius(),construction);else{int a=point(c.Value(first)),b=point(c.Value(last));if(c.Circle().Axis().Direction().Dot(normal)<0)std::swap(a,b);if(a!=b)id=sk.add_arc(o,a,b,construction);}}
    else {Handle(Geom_Curve) source=Handle(Geom_Curve)::DownCast(c.Curve().Curve()->Transformed(c.Trsf()));auto curve=GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(source,first,last));
      SkEntity e;e.type=SkEntity::Type::Spline;e.degree=curve->Degree();e.periodic=curve->IsPeriodic();e.construction=construction;e.fixed=fixed;
      for(int k=1;k<=curve->NbPoles();++k){e.p.push_back(point(curve->Pole(k)));e.weights.push_back(curve->Weight(k));}for(int k=1;k<=curve->NbKnots();++k){e.knots.push_back(curve->Knot(k));e.multiplicities.push_back(curve->Multiplicity(k));}e.id=sk.next_id();id=e.id;sk.entities.push_back(std::move(e));}
    if(id){sk.entity(id)->fixed=fixed;made.push_back(id);}
  }
  sk.validate();return made;
}

void offset_entities(Sketch& sk,const std::vector<int>& ids,double distance,bool round) {
  if(!std::isfinite(distance)||std::fabs(distance)<1e-9)throw Error("offset distance must be nonzero");
  auto wire=sketch_wire(sk,Frame{},ids);BRepOffsetAPI_MakeOffset offset(wire,round?GeomAbs_Arc:GeomAbs_Intersection);
  offset.Perform(distance);if(!offset.IsDone()||offset.Shape().IsNull())throw Error("that offset leaves no curves");
  if(append_sketch_shape(sk,offset.Shape()).empty())throw Error("that offset leaves no curves");
}

int heal_endpoints(Sketch& sk,double tolerance) {
  if(!std::isfinite(tolerance)||tolerance<=0)throw Error("healing tolerance must be positive");
  std::set<int> ends;for(const auto& e:sk.entities)if(!e.construction && !e.fixed) {
    if(e.type==SkEntity::Type::Line || e.type==SkEntity::Type::Spline){ends.insert(e.p.front());ends.insert(e.p.back());}
    if(e.type==SkEntity::Type::Arc){ends.insert(e.p[1]);ends.insert(e.p[2]);}
  }
  std::map<std::pair<long long,long long>,std::vector<int>> buckets;int merged=0;
  for(int id:ends){const auto p=*sk.point(id);const long long x=std::llround(p.x/tolerance),y=std::llround(p.y/tolerance);int into=0;
    for(long long dx=-1;dx<=1&&!into;++dx)for(long long dy=-1;dy<=1&&!into;++dy)for(int candidate:buckets[{x+dx,y+dy}]){const auto* q=sk.point(candidate);if(std::hypot(q->x-p.x,q->y-p.y)<=tolerance){bool collapses=false;for(const auto& e:sk.entities)if(std::find(e.p.begin(),e.p.end(),id)!=e.p.end()&&std::find(e.p.begin(),e.p.end(),candidate)!=e.p.end())collapses=true;if(!collapses){into=candidate;break;}}}
    if(!into){buckets[{x,y}].push_back(id);continue;}if(p.fixed)continue;
    for(auto& e:sk.entities)for(int& ref:e.p)if(ref==id)ref=into;for(auto& c:sk.constraints){for(int& ref:c.refs)if(ref==id)ref=into;for(int& ref:c.anchors)if(ref==id)ref=into;}
    std::erase_if(sk.constraints,[](const SkConstraint& c){return c.type==SkConstraint::Type::Coincident && c.refs.size()==2 && c.refs[0]==c.refs[1];});sk.remove(id);++merged;
  }
  sk.validate();return merged;
}

int heal_to_curves(Sketch& sk,double tolerance) {
  if(!(tolerance>0)||!std::isfinite(tolerance))throw Error("healing tolerance must be positive");
  std::map<int,int> degree;
  for(const auto& e:sk.entities)if(!e.construction){if(e.type==SkEntity::Type::Line||e.type==SkEntity::Type::Spline){++degree[e.p.front()];++degree[e.p.back()];}else if(e.type==SkEntity::Type::Arc){++degree[e.p[1]];++degree[e.p[2]];}}
  int healed=0;
  for(const auto& [id,count]:degree)if(count==1) {
    const auto* p=sk.point(id);if(!p||p->fixed)continue;const gp_Pnt point(p->x,p->y,0);int target=0;gp_Pnt closest;double best=tolerance;
    for(const auto& e:sk.entities)if(!e.construction&&e.type!=SkEntity::Type::Point&&std::find(e.p.begin(),e.p.end(),id)==e.p.end()) {
      const auto edge=entity_edge(sk,e,{});if(edge.IsNull())continue;Bnd_Box box;BRepBndLib::Add(edge,box);box.Enlarge(tolerance);if(box.IsOut(point))continue;
      BRepAdaptor_Curve curve(edge);const auto source=Handle(Geom_Curve)::DownCast(curve.Curve().Curve()->Transformed(curve.Trsf()));GeomAPI_ProjectPointOnCurve nearest(point,source,curve.FirstParameter(),curve.LastParameter());
      if(!nearest.NbPoints()||nearest.LowerDistance()>best)continue;const auto q=nearest.NearestPoint();
      const auto start=curve.Value(curve.FirstParameter()),end=curve.Value(curve.LastParameter());
      if(start.Distance(end)>1e-7 && (q.Distance(start)<1e-7 || q.Distance(end)<1e-7))continue;
      best=nearest.LowerDistance();closest=q;target=e.id;
    }
    if(!target)continue;auto* endpoint=sk.point(id);endpoint->x=closest.X();endpoint->y=closest.Y();
    const auto curve=*sk.entity(target);
    if(curve.type==SkEntity::Type::Line||curve.type==SkEntity::Type::Circle||curve.type==SkEntity::Type::Arc)sk.add_constraint(SkConstraint::Type::Coincident,{id,target});
    else if(!curve.fixed)split_entity(sk,target,closest.X(),closest.Y());
    ++healed;
  }
  sk.validate();return healed;
}

void split_entity(Sketch& sk,int id,double x,double y) {
  const auto* found=sk.entity(id);if(!found || found->fixed)throw Error("pick an editable curve to split");const auto source=*found;
  const auto edge=entity_edge(sk,source,Frame{});if(edge.IsNull())throw Error("that entity cannot be split");
  BRepAdaptor_Curve c(edge);Handle(Geom_Curve) curve=Handle(Geom_Curve)::DownCast(c.Curve().Curve()->Transformed(c.Trsf()));
  GeomAPI_ProjectPointOnCurve nearest(gp_Pnt(x,y,0),curve,c.FirstParameter(),c.LastParameter());if(!nearest.NbPoints())throw Error("no split point on the curve");const double at=nearest.LowerDistanceParameter();
  if(at-c.FirstParameter()<1e-8 || c.LastParameter()-at<1e-8)throw Error("pick inside the curve, away from its ends");
  const auto left=BRepBuilderAPI_MakeEdge(curve,c.FirstParameter(),at).Edge(),right=BRepBuilderAPI_MakeEdge(curve,at,c.LastParameter()).Edge();
  sk.remove(id);const auto a=append_sketch_shape(sk,left,{},source.construction),b=append_sketch_shape(sk,right,{},source.construction);
  // Keep the original ID on the first segment for existing entity selections and downstream references.
  if(!a.empty())sk.entity(a.front())->id=id;
  heal_endpoints(sk,1e-7);sk.validate();
}

void break_intersections(Sketch& sk,const std::vector<int>& ids) {
  struct Curve {Bnd_Box box;Handle(Geom2d_Curve) curve;};std::vector<Curve> curves;
  for(int id:ids)if(const auto* e=sk.entity(id);e&&!e->fixed) {
    const auto edge=entity_edge(sk,*e,{});if(edge.IsNull())continue;BRepAdaptor_Curve c(edge);
    const auto source=Handle(Geom_Curve)::DownCast(c.Curve().Curve()->Transformed(c.Trsf()));Bnd_Box box;BRepBndLib::Add(edge,box);
    curves.push_back({box,new Geom2d_TrimmedCurve(GeomAPI::To2d(source,gp_Pln(gp::XOY())),c.FirstParameter(),c.LastParameter())});
  }
  std::vector<gp_Pnt2d> cuts;
  for(size_t i=0;i<curves.size();++i)for(size_t j=i+1;j<curves.size();++j) {
    if(curves[i].box.IsOut(curves[j].box))continue;
    Geom2dAPI_InterCurveCurve crossing(curves[i].curve,curves[j].curve,1e-8);
    for(int k=1;k<=crossing.NbPoints();++k)cuts.push_back(crossing.Point(k));
  }
  std::set<int> targets(ids.begin(),ids.end());
  for(const auto& p:cuts){const auto current=targets;for(int id:current)if(const auto* e=sk.entity(id)) {
    const auto edge=entity_edge(sk,*e,{});if(edge.IsNull())continue;BRepAdaptor_Curve c(edge);const auto source=Handle(Geom_Curve)::DownCast(c.Curve().Curve()->Transformed(c.Trsf()));
    GeomAPI_ProjectPointOnCurve nearest(gp_Pnt(p.X(),p.Y(),0),source,c.FirstParameter(),c.LastParameter());
    if(!nearest.NbPoints()||nearest.LowerDistance()>1e-6)continue;const double at=nearest.LowerDistanceParameter();if(at-c.FirstParameter()<1e-7||c.LastParameter()-at<1e-7)continue;
    const int next=sk.next_id();split_entity(sk,id,p.X(),p.Y());for(const auto& made:sk.entities)if(made.id>=next)targets.insert(made.id);
  }}
}

namespace {
// How far the end of `e` nearer (x, y) goes on to meet `f` (a share of a line's length, an arc's angle), and where; false:
// it never does.
bool reach(const Sketch& sk,const SkEntity& e,const SkEntity& f,double x,double y,double& best,gp_Pnt2d& target) {
  const auto ea=entity_edge(sk,e,{}),fb=entity_edge(sk,f,{});
  if(ea.IsNull()||fb.IsNull())return false;
  BRepAdaptor_Curve a(ea),b(fb);
  auto curve=[](const BRepAdaptor_Curve& c){return GeomAPI::To2d(Handle(Geom_Curve)::DownCast(c.Curve().Curve()->Transformed(c.Trsf())),gp_Pln(gp::XOY()));};
  Geom2dAPI_InterCurveCurve crossing(curve(a),new Geom2d_TrimmedCurve(curve(b),b.FirstParameter(),b.LastParameter()),1e-8);
  const int start=e.p[e.type==SkEntity::Type::Arc?1:0],end=e.p[e.type==SkEntity::Type::Arc?2:1];
  const auto p=*sk.point(start),q=*sk.point(end);const bool fromStart=std::hypot(x-p.x,y-p.y)<std::hypot(x-q.x,y-q.y);
  bool found=false;
  for(int i=1;i<=crossing.NbPoints();++i){const auto at=crossing.Point(i);double amount=0;
    if(e.type==SkEntity::Type::Line){const double dx=q.x-p.x,dy=q.y-p.y,t=((at.X()-p.x)*dx+(at.Y()-p.y)*dy)/(dx*dx+dy*dy);if(fromStart?t>=0:t<=1)continue;amount=fromStart?-t:t-1;}
    else{const auto o=*sk.point(e.p[0]);auto angle=[&](double px,double py){return std::atan2(py-o.y,px-o.x);};auto positive=[](double t){t=std::fmod(t,2*M_PI);return t<0?t+2*M_PI:t;};double sweep=positive(angle(q.x,q.y)-angle(p.x,p.y));amount=fromStart?positive(angle(p.x,p.y)-angle(at.X(),at.Y())):positive(angle(at.X(),at.Y())-angle(q.x,q.y));if(amount<1e-9 || amount+sweep>=2*M_PI-1e-8)continue;}
    if(amount<best){best=amount;target=at;found=true;}
  }
  return found;
}
void move_end(Sketch& sk,SkEntity& e,double x,double y,const gp_Pnt2d& target) {
  const int start=e.p[e.type==SkEntity::Type::Arc?1:0],end=e.p[e.type==SkEntity::Type::Arc?2:1];
  const auto p=*sk.point(start),q=*sk.point(end);const bool fromStart=std::hypot(x-p.x,y-p.y)<std::hypot(x-q.x,y-q.y);
  auto* point=sk.point(fromStart?start:end);if(point->fixed)throw Error("the endpoint is fixed");point->x=target.X();point->y=target.Y();
}
}  // namespace

void extend_entity(Sketch& sk,int id,int boundary,double x,double y) {
  auto* e=sk.entity(id);const auto* f=sk.entity(boundary);
  if(!e||!f||(e->type!=SkEntity::Type::Line&&e->type!=SkEntity::Type::Arc)||e->fixed)throw Error("extend needs an editable line or arc and a boundary curve");
  double best=INFINITY;gp_Pnt2d target;
  if(!reach(sk,*e,*f,x,y,best,target))throw Error("no intersection extends the chosen end; pick nearer the other end or use trim");
  move_end(sk,*e,x,y,target);
}

int extend_entity(Sketch& sk,int id,double x,double y) {
  auto* e=sk.entity(id);
  if(!e||(e->type!=SkEntity::Type::Line&&e->type!=SkEntity::Type::Arc)||e->fixed)throw Error("pick an editable line or arc to extend");
  double best=INFINITY;gp_Pnt2d target;int boundary=0;
  for(const auto& f:sk.entities)if(f.id!=id && f.type!=SkEntity::Type::Point && reach(sk,*e,f,x,y,best,target))boundary=f.id;
  if(!boundary)throw Error("nothing to extend to on that side; pick nearer the other end");
  move_end(sk,*e,x,y,target);
  return boundary;
}

namespace {
double positive_angle(double t){t=std::fmod(t,2*M_PI);return t<0?t+2*M_PI:t;}
// One side of a corner: a line or an arc that ends at the corner point.
struct Side {
  int id=0;bool line=true;
  double px=0,py=0;                              // the corner
  double dx=0,dy=0,length=0;                     // a line: the way from the corner, its length
  double cx=0,cy=0,R=0,sweep=0;bool start=true;  // an arc: centre, radius, sweep, the corner its start
  // How far along the curve from the corner lies its point nearest (x, y) (a length), and that point; false: off the curve.
  bool along(double x,double y,double& t,double& fx,double& fy) const {
    if(line){t=(x-px)*dx+(y-py)*dy;fx=px+t*dx;fy=py+t*dy;return t>1e-9 && t<length-1e-9;}
    const double a=std::atan2(y-cy,x-cx),corner=std::atan2(py-cy,px-cx),turn=start?positive_angle(a-corner):positive_angle(corner-a);
    t=turn*R;fx=cx+R*std::cos(a);fy=cy+R*std::sin(a);
    return turn>1e-9 && turn<sweep-1e-9;
  }
};
bool corner_sides(const Sketch& sk,int point,Side out[2]) {
  const SkPoint* p=sk.point(point);
  if(!p)return false;
  int n=0;
  for(const auto& e:sk.entities) {
    if(e.type==SkEntity::Type::Spline && !e.p.empty() && (e.p.front()==point || e.p.back()==point))return false;
    const bool line=e.type==SkEntity::Type::Line;
    if(!line && e.type!=SkEntity::Type::Arc)continue;
    const int a=line?e.p[0]:e.p[1],b=line?e.p[1]:e.p[2];
    if(a!=point && b!=point)continue;
    if(n==2)return false;
    Side& s=out[n++];
    s.id=e.id;s.line=line;s.px=p->x;s.py=p->y;
    if(line) {
      const SkPoint* q=sk.point(a==point?b:a);
      s.dx=q->x-p->x;s.dy=q->y-p->y;s.length=std::hypot(s.dx,s.dy);
      if(s.length<1e-12)return false;
      s.dx/=s.length;s.dy/=s.length;
    } else {
      const SkPoint *c=sk.point(e.p[0]),*from=sk.point(e.p[1]),*to=sk.point(e.p[2]);
      s.cx=c->x;s.cy=c->y;s.R=std::hypot(from->x-c->x,from->y-c->y);s.start=a==point;
      s.sweep=positive_angle(std::atan2(to->y-c->y,to->x-c->x)-std::atan2(from->y-c->y,from->x-c->x));
      if(s.R<1e-12 || s.sweep<1e-12)return false;
    }
  }
  return n==2;
}
// Centres r off both curves: a line moved r to one side (k = +1 or -1, along its left normal), an arc's circle r larger
// or smaller.
int centres(const Side& a,int sa,const Side& b,int sb,double r,double x[2],double y[2]) {
  struct L{double x,y,dx,dy;};
  struct C{double x,y,r;};
  auto line=[&](const Side& s,int k){return L{s.px-s.dy*k*r,s.py+s.dx*k*r,s.dx,s.dy};};
  auto circle=[&](const Side& s,int k){return C{s.cx,s.cy,s.R+k*r};};
  if(a.line && b.line) {
    const L p=line(a,sa),q=line(b,sb);const double den=p.dx*q.dy-p.dy*q.dx;
    if(std::fabs(den)<1e-12)return 0;
    const double t=((q.x-p.x)*q.dy-(q.y-p.y)*q.dx)/den;
    x[0]=p.x+t*p.dx;y[0]=p.y+t*p.dy;return 1;
  }
  if(a.line!=b.line) {
    const L p=line(a.line?a:b,a.line?sa:sb);const C c=circle(a.line?b:a,a.line?sb:sa);
    if(c.r<1e-12)return 0;
    const double fx=p.x-c.x,fy=p.y-c.y,B=fx*p.dx+fy*p.dy,D=B*B-(fx*fx+fy*fy-c.r*c.r);
    if(D<0)return 0;
    int n=0;
    for(const double t:{-B-std::sqrt(D),-B+std::sqrt(D)}){x[n]=p.x+t*p.dx;y[n]=p.y+t*p.dy;++n;}
    return n;
  }
  const C p=circle(a,sa),q=circle(b,sb);
  const double d=std::hypot(q.x-p.x,q.y-p.y);
  if(p.r<1e-12 || q.r<1e-12 || d<1e-12 || d>p.r+q.r || d<std::fabs(p.r-q.r))return 0;
  const double m=(p.r*p.r-q.r*q.r+d*d)/(2*d),h=std::sqrt(std::max(0.0,p.r*p.r-m*m)),mx=p.x+m*(q.x-p.x)/d,my=p.y+m*(q.y-p.y)/d;
  x[0]=mx+h*(q.y-p.y)/d;y[0]=my-h*(q.x-p.x)/d;x[1]=mx-h*(q.y-p.y)/d;y[1]=my+h*(q.x-p.x)/d;
  return 2;
}
}  // namespace

bool fillet_geometry(const Sketch& sk,int point,double r,FilletCorner& out) {
  Side side[2];
  if(!(r>0) || !std::isfinite(r) || !corner_sides(sk,point,side))return false;
  double best=INFINITY;
  for(const int sa:{1,-1})for(const int sb:{1,-1}) {
    double x[2],y[2];
    for(int k=0,n=centres(side[0],sa,side[1],sb,r,x,y);k<n;++k) {
      double ta,tb,ax,ay,bx,by;
      if(!side[0].along(x[k],y[k],ta,ax,ay) || !side[1].along(x[k],y[k],tb,bx,by) || ta+tb>=best)continue;
      best=ta+tb;
      out.cx=x[k];out.cy=y[k];out.ax=ax;out.ay=ay;out.bx=bx;out.by=by;out.r=r;out.first=side[0].id;out.second=side[1].id;
      // The arc runs round the side that faces the corner.
      const double from=std::atan2(ay-y[k],ax-x[k]),to=std::atan2(by-y[k],bx-x[k]),corner=std::atan2(side[0].py-y[k],side[0].px-x[k]);
      out.ccw=positive_angle(corner-from)<positive_angle(to-from);
    }
  }
  return std::isfinite(best);
}

int fillet_corner(Sketch& sk,int point,double r,const std::string& expr) {
  FilletCorner f;
  if(!fillet_geometry(sk,point,r,f))throw Error("no fillet of that radius fits there: pick a corner where two lines or arcs end, with room for it");
  using T=SkConstraint::Type;
  const int ta=sk.add_point(f.ax,f.ay),tb=sk.add_point(f.bx,f.by),centre=sk.add_point(f.cx,f.cy);
  for(const auto& [id,to]:{std::pair{f.first,ta},std::pair{f.second,tb}}) {
    SkEntity* e=sk.entity(id);
    for(size_t i=e->type==SkEntity::Type::Arc?1:0;i<e->p.size();++i)if(e->p[i]==point){e->p[i]=to;break;}
  }
  const int arc=sk.add_arc(centre,f.ccw?ta:tb,f.ccw?tb:ta);
  sk.add_constraint(T::Tangent,{f.first,arc});
  sk.add_constraint(T::Tangent,{f.second,arc});
  sk.add_constraint(T::Radius,{arc},r,expr);
  // The sides are shorter now. What measured a whole line (a polygon's "equal"s, a length, a midpoint) moves to a
  // construction line along the old side, from its far end to the old corner: held on the trimmed side it pulled the whole
  // shape out of place. An arc's length means another arc now.
  bool referenced=false;
  for(const int id:{f.first,f.second}) {
    auto on=[id](const SkConstraint& c){return std::find(c.refs.begin(),c.refs.end(),id)!=c.refs.end();};
    if(sk.entity(id)->type==SkEntity::Type::Arc){std::erase_if(sk.constraints,[&](const SkConstraint& c){return c.type==T::ArcLength && on(c);});continue;}
    auto measures=[&](const SkConstraint& c){return on(c) && (c.type==T::Equal || c.type==T::Midpoint || (c.type==T::Distance && c.refs.size()==1));};
    if(std::none_of(sk.constraints.begin(),sk.constraints.end(),measures))continue;
    const SkEntity* e=sk.entity(id);
    const int whole=sk.add_line(e->p[0]==ta || e->p[0]==tb?e->p[1]:e->p[0],point,true);
    for(auto& c:sk.constraints)if(measures(c))std::replace(c.refs.begin(),c.refs.end(),id,whole);
    referenced=true;
  }
  // The old corner stays as a virtual sharp on both curves when something still refers to it; otherwise it goes.
  for(const auto& c:sk.constraints)referenced=referenced || std::find(c.refs.begin(),c.refs.end(),point)!=c.refs.end();
  for(const auto& e:sk.entities)referenced=referenced || std::find(e.p.begin(),e.p.end(),point)!=e.p.end();
  if(referenced) {
    sk.add_constraint(T::Coincident,{point,f.first});
    sk.add_constraint(T::Coincident,{point,f.second});
  } else {
    sk.remove(point);
  }
  return arc;
}

void merge_points(Sketch& sk,int from,int into) {
  if(from==into || !sk.point(from) || !sk.point(into))throw Error("pick two different points to merge");
  auto holds=[](const std::vector<int>& ids,int id){return std::find(ids.begin(),ids.end(),id)!=ids.end();};
  for(const auto& e:sk.entities)if(holds(e.p,from) && holds(e.p,into))throw Error("merging those points would collapse a curve");
  for(const auto& c:sk.constraints)if(c.is_dimension() && holds(c.refs,from) && holds(c.refs,into))throw Error("a dimension keeps those points apart");
  for(const auto& p:sk.patterns)for(const auto& instance:p.value("instances",json::array()))for(const auto& pair:instance.value("map",json::array()))
    for(const auto& id:pair)if(id==from)throw Error("break the pattern before merging its points");
  for(auto& e:sk.entities)std::replace(e.p.begin(),e.p.end(),from,into);
  for(auto& c:sk.constraints){std::replace(c.refs.begin(),c.refs.end(),from,into);std::replace(c.anchors.begin(),c.anchors.end(),from,into);}
  // On one point twice, a coincidence, a horizontal or a vertical holds nothing any more.
  using T=SkConstraint::Type;
  std::erase_if(sk.constraints,[&](const SkConstraint& c){return (c.type==T::Coincident || c.type==T::Horizontal || c.type==T::Vertical) && std::count(c.refs.begin(),c.refs.end(),into)>1;});
  if(sk.point(from)->fixed)sk.point(into)->fixed=true;
  sk.remove(from);
}

void chamfer_corner(Sketch& sk,int point,double first,double second) {
  if(first<=0||second<=0)throw Error("chamfer distances must be positive");std::vector<int> lines;
  for(const auto& e:sk.entities)if(e.type==SkEntity::Type::Line&&std::find(e.p.begin(),e.p.end(),point)!=e.p.end())lines.push_back(e.id);
  if(lines.size()!=2)throw Error("pick a corner joining exactly two lines");const auto corner=*sk.point(point);std::vector<int> cuts;
  for(size_t i=0;i<2;++i){const auto* e=sk.entity(lines[i]);const auto end=*sk.point(e->p[e->p[0]==point?1:0]);const double length=std::hypot(end.x-corner.x,end.y-corner.y),distance=i?second:first;if(distance>=length)throw Error("chamfer distance exceeds a line length");const int p=sk.add_point(corner.x+(end.x-corner.x)*distance/length,corner.y+(end.y-corner.y)*distance/length);cuts.push_back(p);auto* line=sk.entity(lines[i]);for(int& id:line->p)if(id==point)id=p;}
  sk.add_line(cuts[0],cuts[1]);sk.remove(point);
}

void identify_regions(const Sketch& sk,std::vector<Region>& regions,const Frame& frame) {
  struct Source {int id;Bnd_Box box;Handle(Geom_Curve) curve;double first,last;};std::vector<Source> sources;
  for(const auto& e:sk.entities)if(!e.construction&&e.type!=SkEntity::Type::Point){auto edge=entity_edge(sk,e,frame);if(edge.IsNull())continue;BRepAdaptor_Curve c(edge);Bnd_Box box;BRepBndLib::Add(edge,box);box.Enlarge(1e-6);sources.push_back({e.id,box,Handle(Geom_Curve)::DownCast(c.Curve().Curve()->Transformed(c.Trsf())),c.FirstParameter(),c.LastParameter()});}
  for(auto& region:regions) {
  std::set<int> ids;
  for(TopExp_Explorer ex(region.face,TopAbs_EDGE);ex.More();ex.Next()){const auto edge=TopoDS::Edge(ex.Current());BRepAdaptor_Curve c(edge);gp_Pnt p;gp_Vec tangent;c.D1(c.FirstParameter()+(c.LastParameter()-c.FirstParameter())*.37,p,tangent);if(edge.Orientation()==TopAbs_REVERSED)tangent.Reverse();
    for(const auto& source:sources){if(source.box.IsOut(p))continue;GeomAPI_ProjectPointOnCurve nearest(p,source.curve,source.first,source.last);if(!nearest.NbPoints()||nearest.LowerDistance()>1e-6)continue;gp_Pnt q;gp_Vec direction;source.curve->D1(nearest.LowerDistanceParameter(),q,direction);ids.insert(tangent.Dot(direction)<0?-source.id:source.id);break;}}
  region.boundary={ids.begin(),ids.end()};
  }
}
std::vector<int> region_sources(const Sketch& sk,const Region& region,const Frame& frame) {
  std::vector<Region> regions{region};identify_regions(sk,regions,frame);return regions.front().boundary;
}

void delete_curve_node(Sketch& sk,int point) {
  for(auto& e:sk.entities)if(e.type==SkEntity::Type::Spline && !e.fixed) {
    auto at=std::find(e.p.begin(),e.p.end(),point);if(at==e.p.end())continue;
    if(e.p.size()<=2)throw Error("a spline needs at least two nodes");
    const size_t index=size_t(at-e.p.begin());e.p.erase(at);
    if(e.degree) {
      e.weights.erase(e.weights.begin()+index);e.degree=std::min(e.degree,int(e.p.size())-1);e.periodic=false;
      e.knots.clear();e.multiplicities.clear();const int spans=int(e.p.size())-e.degree;
      for(int i=0;i<=spans;++i){e.knots.push_back(double(i)/spans);e.multiplicities.push_back(i==0||i==spans?e.degree+1:1);}
    }
    sk.remove(point);sk.validate();return;
  }
  std::vector<int> lines;for(const auto& e:sk.entities)if(e.type==SkEntity::Type::Line&&!e.fixed&&std::find(e.p.begin(),e.p.end(),point)!=e.p.end())lines.push_back(e.id);
  if(lines.size()!=2)throw Error("select a spline control node or a corner joining two editable lines");
  const auto first=*sk.entity(lines[0]),second=*sk.entity(lines[1]);const int a=first.p[first.p[0]==point?1:0],b=second.p[second.p[0]==point?1:0];
  if(a==b)throw Error("deleting this node would collapse the curve");sk.entity(lines[0])->p={a,b};sk.remove(lines[1]);sk.remove(point);sk.validate();
}

void boolean_regions(Sketch& sk,double ax,double ay,double bx,double by,const std::string& operation) {
  // Boolean operands are complete original loops, before overlap splitting into the minimal display cells.
  // Otherwise intersection would always be empty because distinct display regions have disjoint interiors.
  Handle(TopTools_HSequenceOfShape) edges=new TopTools_HSequenceOfShape(),wires;
  for(const auto& edge:sketch_edges(sk,Frame{}))edges->Append(edge);
  ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges,1e-7,false,wires);
  std::vector<Region> regions;
  if(!wires.IsNull())for(int i=1;i<=wires->Length();++i) {
    const auto wire=TopoDS::Wire(wires->Value(i));if(!wire.Closed())continue;
    BRepBuilderAPI_MakeFace face(wire,true);if(!face.IsDone())continue;
    Region r;r.face=face.Face();GProp_GProps props;BRepGProp::SurfaceProperties(r.face,props);r.area=std::fabs(props.Mass());regions.push_back(r);
  }
  const int a=region_at(regions,{},ax,ay),b=region_at(regions,{},bx,by);
  if(a<0||b<0||a==b)throw Error("pick two different closed regions");TopoDS_Shape result;
  if(operation=="union"){BRepAlgoAPI_Fuse op(regions[a].face,regions[b].face);op.Build();if(op.IsDone()){op.SimplifyResult();result=op.Shape();}}
  else if(operation=="subtract"){BRepAlgoAPI_Cut op(regions[a].face,regions[b].face);op.Build();if(op.IsDone())result=op.Shape();}
  else if(operation=="intersect"){BRepAlgoAPI_Common op(regions[a].face,regions[b].face);op.Build();if(op.IsDone())result=op.Shape();}
  else throw Error("unknown region boolean");if(result.IsNull())throw Error("region boolean failed");
  auto ids=region_sources(sk,regions[a]);auto other=region_sources(sk,regions[b]);ids.insert(ids.end(),other.begin(),other.end());
  for(int id:ids)if(auto* e=sk.entity(std::abs(id)))e->construction=true;
  append_sketch_shape(sk,result);
}
}
