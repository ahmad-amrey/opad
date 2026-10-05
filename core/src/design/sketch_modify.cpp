#include "opad/design/sketch_modify.hpp"
#include "opad/design/sketch_pattern.hpp"
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
  // The two lines or arcs ending there. Construction curves ending there too stay out of it when two others do: the whole
  // side a fillet on the next corner left (what measures a side's length), a centre rectangle's diagonal.
  std::vector<const SkEntity*> ending;
  for(const auto& e:sk.entities) {
    if(e.type==SkEntity::Type::Spline && !e.p.empty() && (e.p.front()==point || e.p.back()==point))return false;
    const bool line=e.type==SkEntity::Type::Line;
    if(!line && e.type!=SkEntity::Type::Arc)continue;
    const int a=line?e.p[0]:e.p[1],b=line?e.p[1]:e.p[2];
    if(a==point || b==point)ending.push_back(&e);
  }
  if(std::count_if(ending.begin(),ending.end(),[](const SkEntity* e){return !e->construction;})==2)
    std::erase_if(ending,[](const SkEntity* e){return e->construction;});
  if(ending.size()!=2)return false;
  int n=0;
  for(const SkEntity* edge:ending) {
    const SkEntity& e=*edge;
    const bool line=e.type==SkEntity::Type::Line;
    const int a=line?e.p[0]:e.p[1],b=line?e.p[1]:e.p[2];
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

namespace {
// A box sure to hold a curve, from its points alone (no kernel): a spline's fit points or poles with a margin, a round's or an
// ellipse's centre and radius. False: a point.
bool rough_box(const Sketch& sk,const SkEntity& e,Bnd_Box& box) {
  auto at=[&](int id)->const SkPoint*{return sk.point(id);};
  switch(e.type) {
    case SkEntity::Type::Point:return false;
    case SkEntity::Type::Line:for(int id:e.p)if(const auto* p=at(id))box.Add(gp_Pnt(p->x,p->y,0));return !box.IsVoid();
    case SkEntity::Type::Circle:case SkEntity::Type::Arc:case SkEntity::Type::Ellipse: {
      const SkPoint* c=e.p.empty()?nullptr:at(e.p[0]);if(!c)return false;double r=e.r;
      if(e.type!=SkEntity::Type::Circle && e.p.size()>1)if(const auto* q=at(e.p[1]))r=std::max(r,std::hypot(q->x-c->x,q->y-c->y));
      box.Update(c->x-r,c->y-r,0,c->x+r,c->y+r,0);return true;
    }
    case SkEntity::Type::Spline: {
      for(int id:e.p)
        if(const auto* p=at(id))box.Add(gp_Pnt(p->x,p->y,0));
      if(box.IsVoid())return false;
      double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);box.Enlarge(0.5*std::max(x1-x0,y1-y0)+1e-6);return true;  // an interpolation swings past its points
    }
  }
  return false;
}
Handle(Geom_Curve) curve_of(const TopoDS_Edge& edge,double& first,double& last) {
  BRepAdaptor_Curve c(edge);first=c.FirstParameter();last=c.LastParameter();
  return Handle(Geom_Curve)::DownCast(c.Curve().Curve()->Transformed(c.Trsf()));
}
Handle(Geom2d_Curve) flat(const TopoDS_Edge& edge) {
  double first,last;const auto c=curve_of(edge,first,last);
  return new Geom2d_TrimmedCurve(GeomAPI::To2d(c,gp_Pln(gp::XOY())),first,last);
}
void cross(const Handle(Geom2d_Curve)& a,const Handle(Geom2d_Curve)& b,std::vector<std::array<double,2>>& out) {
  Geom2dAPI_InterCurveCurve crossing(a,b,1e-8);
  for(int i=1;i<=crossing.NbPoints();++i)out.push_back({crossing.Point(i).X(),crossing.Point(i).Y()});
}
}  // namespace

std::vector<std::array<double,2>> curve_crossings(const Sketch& sk,const SkEntity& a,const SkEntity& b) {
  std::vector<std::array<double,2>> out;
  Bnd_Box ra,rb;if(a.id==b.id || !rough_box(sk,a,ra) || !rough_box(sk,b,rb) || ra.IsOut(rb))return out;
  const TopoDS_Edge ea=entity_edge(sk,a,{}),eb=entity_edge(sk,b,{});if(ea.IsNull() || eb.IsNull())return out;
  cross(flat(ea),flat(eb),out);
  return out;
}

CurveCuts curve_cuts(const Sketch& sk,int id,const CurveFilter& filter) {
  const SkEntity* e=sk.entity(id);if(!e || e->type==SkEntity::Type::Point)throw Error("pick a curve to trim");
  const TopoDS_Edge edge=entity_edge(sk,*e,{});if(edge.IsNull())throw Error("that curve cannot be trimmed");
  CurveCuts out;out.curve=curve_of(edge,out.first,out.last);out.closed=BRepAdaptor_Curve(edge).IsClosed();
  const double span=out.last-out.first;
  // The curve once, in its own box (one from its points is wide): only what reaches that box goes to the kernel.
  const Handle(Geom2d_Curve) mine=flat(edge);Bnd_Box box;BRepBndLib::Add(edge,box);box.Enlarge(1e-6);
  for(const auto& o:sk.entities) {
    Bnd_Box rough;if(o.id==id || !rough_box(sk,o,rough) || rough.IsOut(box) || (filter && !filter(o)))continue;
    const TopoDS_Edge other=entity_edge(sk,o,{});if(other.IsNull())continue;
    std::vector<std::array<double,2>> hits;cross(mine,flat(other),hits);
    for(const auto& [x,y]:hits) {
      GeomAPI_ProjectPointOnCurve on(gp_Pnt(x,y,0),out.curve,out.first,out.last);if(!on.NbPoints())continue;
      double t=on.LowerDistanceParameter();
      if(out.closed && out.last-t<1e-9*span)t=out.first;  // the seam
      if(!out.closed && (t-out.first<1e-7*span || out.last-t<1e-7*span))continue;  // touched at an end: nothing cut there
      out.at.push_back({t,o.id});
    }
  }
  std::sort(out.at.begin(),out.at.end());
  out.at.erase(std::unique(out.at.begin(),out.at.end(),[&](const auto& a,const auto& b){return b.first-a.first<1e-9*span;}),out.at.end());
  return out;
}

TopoDS_Edge piece_edge(const CurveCuts& cuts,const TrimPiece& piece) {
  return BRepBuilderAPI_MakeEdge(new Geom_TrimmedCurve(cuts.curve,piece.from,piece.to)).Edge();
}

bool trim_pieces(const CurveCuts& c,double x,double y,std::vector<TrimPiece>& keep,std::vector<TrimPiece>& gone) {
  keep.clear();gone.clear();
  GeomAPI_ProjectPointOnCurve on(gp_Pnt(x,y,0),c.curve,c.first,c.last);if(!on.NbPoints())return false;
  const double tc=on.LowerDistanceParameter(),period=c.last-c.first;const auto& at=c.at;const size_t n=at.size();
  if(at.empty()){gone.push_back({c.first,c.last,0,0});return true;}
  if(!c.closed) {
    const std::pair<double,int>*lo=nullptr,*hi=nullptr;
    for(const auto& a:at){if(a.first<tc)lo=&a;else if(!hi)hi=&a;}
    if(lo)keep.push_back({c.first,lo->first,0,lo->second});
    if(hi)keep.push_back({hi->first,c.last,hi->second,0});
    gone.push_back({lo?lo->first:c.first,hi?hi->first:c.last,lo?lo->second:0,hi?hi->second:0});
    return true;
  }
  if(n==1)return false;
  // The span clicked lies between two neighbouring cuts (across the seam before the first one or after the last); the rest
  // runs from its end round to its start.
  size_t hi=0;while(hi<n && at[hi].first<tc)++hi;
  const auto &start=at[hi%n],&end=at[(hi+n-1)%n];
  const bool periodic=c.curve->IsPeriodic();
  auto add=[&](std::vector<TrimPiece>& to,const std::pair<double,int>& a,const std::pair<double,int>& b) {
    if(b.first>a.first)to.push_back({a.first,b.first,a.second,b.second});
    else if(periodic)to.push_back({a.first,b.first+period,a.second,b.second});
    else{to.push_back({a.first,c.last,a.second,0});to.push_back({c.first,b.first,0,b.second});}
  };
  add(keep,start,end);add(gone,end,start);
  return true;
}

std::vector<int> trim_curve(Sketch& sk,int id,double x,double y) {
  const SkEntity* found=sk.entity(id);
  if(!found || found->fixed || (found->type!=SkEntity::Type::Spline && found->type!=SkEntity::Type::Ellipse))throw Error("trim: pick an editable spline or ellipse");
  if(pattern_of(sk,id,true))throw Error("break the pattern before trimming its curves");
  const SkEntity source=*found;
  const CurveCuts cuts=curve_cuts(sk,id);std::vector<TrimPiece> keep,gone;
  if(!trim_pieces(cuts,x,y,keep,gone))throw Error("the curve is crossed only once; nothing to cut between");
  // Where a piece ends at an old end (or a closed curve's seam) it takes that point, so what joined it stays joined.
  const int startPoint=source.type==SkEntity::Type::Spline && !source.p.empty()?source.p.front():0,endPoint=cuts.closed?startPoint:source.type==SkEntity::Type::Spline && !source.p.empty()?source.p.back():0;
  const Sketch before=sk;
  try {
    std::vector<int> made;int seam=0;
    for(const auto& piece:keep) {
      const auto ids=append_sketch_shape(sk,piece_edge(cuts,piece),{},source.construction);
      if(ids.size()!=1)throw Error("the trimmed piece could not be made");
      made.push_back(ids[0]);
      for(const bool start:{true,false}) {
        SkEntity& e=*sk.entity(ids[0]);int& slot=start?e.p.front():e.p.back();
        const int by=start?piece.from_by:piece.to_by,fresh=slot;
        const bool atStart=std::fabs((start?piece.from:piece.to)-cuts.first)<1e-12;
        if(by) {
          const SkEntity* cutter=sk.entity(by);
          if(cutter && (cutter->type==SkEntity::Type::Line || cutter->type==SkEntity::Type::Circle || cutter->type==SkEntity::Type::Arc))sk.add_constraint(SkConstraint::Type::Coincident,{fresh,by});
          continue;
        }
        const int old=atStart?startPoint:endPoint,join=old && sk.point(old)?old:seam;  // else the two pieces meet at the seam
        if(join){slot=join;sk.remove(fresh);}
        else seam=fresh;
      }
    }
    sk.remove(id);
    if(!made.empty()) {
      sk.entity(made.front())->id=id;
      made.front()=id;
    }
    sk.validate();
    return made;
  } catch(...) {
    sk=before;
    throw;
  }
}

namespace {
double turn(double a){a=std::fmod(a,2*M_PI);return a<0?a+2*M_PI:a;}
// Where a line a->b meets a circle (c, r): parameters along the line.
std::vector<double> line_circle(double ax,double ay,double bx,double by,double cx,double cy,double r) {
  const double dx=bx-ax,dy=by-ay,fx=ax-cx,fy=ay-cy;
  const double A=dx*dx+dy*dy,B=2*(fx*dx+fy*dy),C=fx*fx+fy*fy-r*r,disc=B*B-4*A*C;
  if(A<1e-18 || disc<0)return {};
  const double s=std::sqrt(disc);
  return {(-B-s)/(2*A),(-B+s)/(2*A)};
}
}  // namespace

bool TrimRound::holds(double x,double y) const { return sweep>=2*M_PI-1e-12 || turn(std::atan2(y-cy,x-cx)-a0)<=sweep+1e-9; }

TrimRound trim_round(const Sketch& sk,const SkEntity& e) {
  const SkPoint* c=sk.point(e.p[0]);
  TrimRound k{c->x,c->y,e.r,0,2*M_PI};
  if(e.type==SkEntity::Type::Arc) {
    const SkPoint *from=sk.point(e.p[1]),*to=sk.point(e.p[2]);
    k.r=std::hypot(from->x-k.cx,from->y-k.cy);
    k.a0=std::atan2(from->y-k.cy,from->x-k.cx);
    k.sweep=turn(std::atan2(to->y-k.cy,to->x-k.cx)-k.a0);
    if(k.sweep<1e-12)k.sweep=2*M_PI;
  }
  return k;
}

TrimCrossings trim_crossings(const Sketch& sk,const SkEntity& target,const TrimSamples& samples) {
  using T=SkEntity::Type;
  auto P=[&](int id){return sk.point(id);};
  const double eps=1e-7;
  TrimCrossings out;
  out.line=target.type==T::Line;
  if(out.line){out.ax=P(target.p[0])->x;out.ay=P(target.p[0])->y;out.bx=P(target.p[1])->x;out.by=P(target.p[1])->y;}
  else out.self=trim_round(sk,target);
  const bool isLine=out.line;const TrimRound& self=out.self;
  const double ax=out.ax,ay=out.ay,bx=out.bx,by=out.by;
  for(const auto& o:sk.entities) {
    if(o.id==target.id)continue;
    std::vector<std::pair<double,double>> hits;  // where they cross
    if(o.type==T::Line) {
      const double cx=P(o.p[0])->x,cy=P(o.p[0])->y,dx=P(o.p[1])->x,dy=P(o.p[1])->y;
      if(isLine) {
        const double den=(bx-ax)*(dy-cy)-(by-ay)*(dx-cx);
        if(std::fabs(den)<1e-14)continue;
        const double t=((cx-ax)*(dy-cy)-(cy-ay)*(dx-cx))/den,s=((cx-ax)*(by-ay)-(cy-ay)*(bx-ax))/den;
        if(s>=-eps && s<=1+eps)hits.push_back({ax+t*(bx-ax),ay+t*(by-ay)});
      } else {
        for(double s:line_circle(cx,cy,dx,dy,self.cx,self.cy,self.r))
          if(s>=-eps && s<=1+eps)hits.push_back({cx+s*(dx-cx),cy+s*(dy-cy)});
      }
    } else if(o.type==T::Circle || o.type==T::Arc) {
      const TrimRound k=trim_round(sk,o);
      if(isLine) {
        for(double t:line_circle(ax,ay,bx,by,k.cx,k.cy,k.r)) {
          const double x=ax+t*(bx-ax),y=ay+t*(by-ay);
          if(k.holds(x,y))hits.push_back({x,y});
        }
      } else {
        const double d=std::hypot(k.cx-self.cx,k.cy-self.cy);
        if(d<1e-12 || d>self.r+k.r || d<std::fabs(self.r-k.r))continue;
        const double a=(self.r*self.r-k.r*k.r+d*d)/(2*d),hh=std::sqrt(std::max(0.0,self.r*self.r-a*a));
        const double mx=self.cx+a*(k.cx-self.cx)/d,my=self.cy+a*(k.cy-self.cy)/d;
        for(double sign:{1.0,-1.0}) {
          const double x=mx+sign*hh*(k.cy-self.cy)/d,y=my-sign*hh*(k.cx-self.cx)/d;
          if(k.holds(x,y))hits.push_back({x,y});
        }
      }
    } else if(o.type==T::Ellipse || o.type==T::Spline) {  // the kernel's crossings (UI-28), a preview's on the samples
      if(const auto* poly=samples?samples(o):nullptr) {
        auto within=[&](double s,size_t i){return s>=-1e-9 && (s<1-1e-9 || (i+1==poly->size() && s<=1+1e-9));};  // a vertex once, the ends too
        for(size_t i=1;i<poly->size();++i) {
          const double cx=(*poly)[i-1].first,cy=(*poly)[i-1].second,dx=(*poly)[i].first,dy=(*poly)[i].second;
          if(isLine) {
            const double den=(bx-ax)*(dy-cy)-(by-ay)*(dx-cx);
            if(std::fabs(den)<1e-14)continue;
            const double t=((cx-ax)*(dy-cy)-(cy-ay)*(dx-cx))/den,s=((cx-ax)*(by-ay)-(cy-ay)*(bx-ax))/den;
            if(within(s,i))hits.push_back({ax+t*(bx-ax),ay+t*(by-ay)});
          } else
            for(double s:line_circle(cx,cy,dx,dy,self.cx,self.cy,self.r))
              if(within(s,i))hits.push_back({cx+s*(dx-cx),cy+s*(dy-cy)});
        }
      } else
        try {
          for(const auto& [x,y]:curve_crossings(sk,target,o))hits.push_back({x,y});
        } catch(...) {
        }
    } else {
      continue;
    }
    for(const auto& [x,y]:hits) {
      if(isLine) {
        const double len2=(bx-ax)*(bx-ax)+(by-ay)*(by-ay);
        const double t=((x-ax)*(bx-ax)+(y-ay)*(by-ay))/len2;
        if(t>eps && t<1-eps)out.cuts.push_back({t,o.id});
      } else if(self.holds(x,y)) {
        const double t=turn(std::atan2(y-self.cy,x-self.cx)-self.a0);
        if(self.sweep>=2*M_PI-1e-12 || (t>eps && t<self.sweep-eps))out.cuts.push_back({t,o.id});
      }
    }
  }
  std::sort(out.cuts.begin(),out.cuts.end(),[](const TrimCut& a,const TrimCut& b){return a.t<b.t;});
  return out;
}

void drop_extent_constraints(Sketch& sk,int id) {
  using T=SkConstraint::Type;
  const SkEntity* e=sk.entity(id);
  if(!e)return;
  const bool line=e->type==SkEntity::Type::Line,arc=e->type==SkEntity::Type::Arc;
  std::erase_if(sk.constraints,[&](const SkConstraint& c) {
    if(std::find(c.refs.begin(),c.refs.end(),id)==c.refs.end())return false;
    if(line)return (c.type==T::Distance && c.refs.size()==1) || c.type==T::Midpoint || c.type==T::Equal;
    return arc && c.type==T::ArcLength;
  });
}

TrimOutcome trim_entity(Sketch& sk,int id,double u,double v) {
  using ET=SkEntity::Type;using CT=SkConstraint::Type;
  SkEntity* target=sk.entity(id);
  if(!target || (target->type!=ET::Line && target->type!=ET::Circle && target->type!=ET::Arc))return TrimOutcome::NotACurve;
  const TrimCrossings found=trim_crossings(sk,*target);
  const bool isLine=found.line;
  const TrimRound self=found.self;
  const double ax=found.ax,ay=found.ay,bx=found.bx,by=found.by;
  const std::vector<TrimCut>& cuts=found.cuts;
  if(target->type==ET::Circle && cuts.size()==1)return TrimOutcome::CrossedOnce;
  if(cuts.empty()) {
    sk.remove(id);
    return TrimOutcome::Trimmed;
  }
  // What measured the curve's whole extent means something else on what is left of it: a line's length (a length dimension,
  // an "equal" with another line), a point held at its middle, an arc's length. Where it stays and what holds it there (its
  // direction, a radius, a tangency, a point on it) still holds.
  drop_extent_constraints(sk,id);
  auto cut_point=[&](double x,double y,int other) {  // on the cutting curve (a point can be held on a line, a circle or an arc)
    const int p=sk.add_point(x,y);
    if(const SkEntity* cutter=sk.entity(other); cutter && (cutter->type==ET::Line || cutter->type==ET::Circle || cutter->type==ET::Arc))sk.add_constraint(CT::Coincident,{p,other});
    return p;
  };
  if(isLine) {
    const double len2=(bx-ax)*(bx-ax)+(by-ay)*(by-ay);
    const double tc=((u-ax)*(bx-ax)+(v-ay)*(by-ay))/len2;
    const TrimCut *lo=nullptr,*hi=nullptr;
    for(const auto& c:cuts) {
      if(c.t<tc)lo=&c;
      else if(!hi)hi=&c;
    }
    const int oldStart=target->p[0],oldEnd=target->p[1];
    const bool construction=target->construction;
    if(lo && hi) {
      const int p1=cut_point(ax+lo->t*(bx-ax),ay+lo->t*(by-ay),lo->other),p2=cut_point(ax+hi->t*(bx-ax),ay+hi->t*(by-ay),hi->other);
      sk.entity(id)->p[1]=p1;
      const int rest=sk.add_line(p2,oldEnd,construction);
      sk.add_constraint(CT::Collinear,{id,rest});
    } else if(lo) {
      sk.entity(id)->p[1]=cut_point(ax+lo->t*(bx-ax),ay+lo->t*(by-ay),lo->other);
      sk.remove(oldEnd);
    } else {
      sk.entity(id)->p[0]=cut_point(ax+hi->t*(bx-ax),ay+hi->t*(by-ay),hi->other);
      sk.remove(oldStart);
    }
    return TrimOutcome::Trimmed;
  }
  const double tc=turn(std::atan2(v-self.cy,u-self.cx)-self.a0);
  auto at=[&](double t,double& x,double& y){x=self.cx+self.r*std::cos(self.a0+t);y=self.cy+self.r*std::sin(self.a0+t);};
  if(target->type==ET::Circle) {
    // The clicked span lies between two neighbouring cuts; what is left runs the other way round.
    size_t hi=0;
    while(hi<cuts.size() && cuts[hi].t<tc)++hi;
    const TrimCut& end=cuts[(hi+cuts.size()-1)%cuts.size()];  // the span starts here ...
    const TrimCut& start=cuts[hi%cuts.size()];                // ... and ends here: the arc kept starts here
    double x,y;
    at(start.t,x,y);
    const int ps=cut_point(x,y,start.other);
    at(end.t,x,y);
    const int pe=cut_point(x,y,end.other);
    SkEntity* e=sk.entity(id);
    e->type=ET::Arc;
    e->p={e->p[0],ps,pe};
    e->r=0;
    return TrimOutcome::Trimmed;
  }
  const TrimCut *lo=nullptr,*hi=nullptr;
  for(const auto& c:cuts) {
    if(c.t<tc)lo=&c;
    else if(!hi)hi=&c;
  }
  const int centre=target->p[0],oldStart=target->p[1],oldEnd=target->p[2];
  const bool construction=target->construction;
  double x,y;
  if(lo && hi) {
    at(lo->t,x,y);
    const int p1=cut_point(x,y,lo->other);
    at(hi->t,x,y);
    const int p2=cut_point(x,y,hi->other);
    sk.entity(id)->p[2]=p1;
    const int rest=sk.add_arc(centre,p2,oldEnd,construction);
    sk.add_constraint(CT::Equal,{id,rest});
  } else if(lo) {
    at(lo->t,x,y);
    sk.entity(id)->p[2]=cut_point(x,y,lo->other);
    sk.remove(oldEnd);
  } else {
    at(hi->t,x,y);
    sk.entity(id)->p[1]=cut_point(x,y,hi->other);
    sk.remove(oldStart);
  }
  return TrimOutcome::Trimmed;
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
