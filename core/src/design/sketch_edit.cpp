#include "opad/design/sketch_edit.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomConvert.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <algorithm>
#include <cmath>
#include <set>

namespace opad::design {
int add_cubic_spline(Sketch& sk,const std::vector<int>& nodes,bool construction) {
  if(nodes.size()<2) throw Error("A spline needs at least two nodes");
  std::vector<gp_Pnt> p;for(int id:nodes) {const auto* n=sk.point(id);if(!n)throw Error("Missing spline node");p.emplace_back(n->x,n->y,0);}
  const bool closed=nodes.size()>2 && nodes.front()==nodes.back();
  auto tangent=[&](size_t i) {
    if(closed && (i==0 || i==p.size()-1)) return gp_Vec(p[p.size()-2],p[1])*0.5;
    if(i==0) return gp_Vec(p[0],p[1]);
    if(i==p.size()-1) return gp_Vec(p[i-1],p[i]);
    return gp_Vec(p[i-1],p[i+1])*0.5;
  };
  SkEntity e;e.type=SkEntity::Type::Spline;e.degree=3;e.construction=construction;e.p.push_back(nodes[0]);
  for(size_t i=1;i<p.size();++i) {
    const auto out=p[i-1].Translated(tangent(i-1)/3),in=p[i].Translated(-tangent(i)/3);
    e.p.push_back(sk.add_point(out.X(),out.Y()));e.p.push_back(sk.add_point(in.X(),in.Y()));e.p.push_back(nodes[i]);
  }
  for(size_t i=0;i<p.size();++i) {e.knots.push_back(double(i));e.multiplicities.push_back(i==0 || i==p.size()-1?4:3);}
  e.weights.assign(e.p.size(),1);e.id=sk.next_id();sk.entities.push_back(e);return e.id;
}
int insert_spline_node(Sketch& sk,int entity,double u,double v) {
  auto* e=sk.entity(entity);if(!e || e->type!=SkEntity::Type::Spline || e->fixed) throw Error("Select an editable spline");
  BRepAdaptor_Curve adaptor(entity_edge(sk,*e,Frame{}));
  auto curve=GeomConvert::CurveToBSplineCurve(new Geom_TrimmedCurve(adaptor.Curve().Curve(),adaptor.FirstParameter(),adaptor.LastParameter()));
  GeomAPI_ProjectPointOnCurve projection(gp_Pnt(u,v,0),curve);if(!projection.NbPoints()) throw Error("Cannot locate spline node");
  const double t=projection.LowerDistanceParameter();const auto at=curve->Value(t);
  if(t<=curve->FirstParameter()+1e-9 || t>=curve->LastParameter()-1e-9) throw Error("Pick inside the spline, away from its endpoints");
  curve->InsertKnot(t,curve->Degree(),1e-10,false);
  const auto old=e->p;e->p.clear();e->knots.clear();e->weights.clear();e->multiplicities.clear();e->degree=curve->Degree();e->periodic=curve->IsPeriodic();
  int inserted=0;double closest=1e100;
  for(int i=1;i<=curve->NbPoles();++i) {
    const auto p=curve->Pole(i);int id=0;
    for(int candidate:old) {const auto* q=sk.point(candidate);if(q && std::hypot(q->x-p.X(),q->y-p.Y())<1e-9) {id=candidate;break;}}
    if(!id) id=sk.add_point(p.X(),p.Y());
    e->p.push_back(id);e->weights.push_back(curve->Weight(i));
    if(p.Distance(at)<closest) {closest=p.Distance(at);inserted=id;}
  }
  for(int i=1;i<=curve->NbKnots();++i) {e->knots.push_back(curve->Knot(i));e->multiplicities.push_back(curve->Multiplicity(i));}
  for(int id:old) {
    bool constrained=false;for(const auto& c:sk.constraints) constrained|=std::find(c.refs.begin(),c.refs.end(),id)!=c.refs.end();
    if(!constrained) sk.remove(id);
  }
  return inserted;
}
std::vector<int> dangling_vertices(const Sketch& sk,double tolerance) {
  struct Curve {int id;Handle(Geom_Curve) curve;double first,last;Bnd_Box box;};
  std::vector<Curve> curves;std::vector<std::pair<int,int>> ends;
  for(const auto& e:sk.entities) {
    if(e.construction || e.type==SkEntity::Type::Point) continue;
    auto edge=entity_edge(sk,e,Frame{});if(edge.IsNull()) continue;
    BRepAdaptor_Curve c(edge);Curve item{e.id,c.Curve().Curve(),c.FirstParameter(),c.LastParameter(),{}};BRepBndLib::Add(edge,item.box);item.box.Enlarge(tolerance);curves.push_back(item);
    if(e.type==SkEntity::Type::Line) {ends.push_back({e.p[0],e.id});ends.push_back({e.p[1],e.id});}
    else if(e.type==SkEntity::Type::Arc) {ends.push_back({e.p[1],e.id});ends.push_back({e.p[2],e.id});}
    else if(e.type==SkEntity::Type::Spline && !e.periodic && e.p.front()!=e.p.back()) {ends.push_back({e.p.front(),e.id});ends.push_back({e.p.back(),e.id});}
  }
  std::set<int> result;
  for(const auto& [id,owner]:ends) {
    const auto* p=sk.point(id);if(!p)continue;const gp_Pnt at(p->x,p->y,0);bool joined=false;
    for(const auto& c:curves) {
      if(c.id==owner || c.box.IsOut(at)) continue;
      GeomAPI_ProjectPointOnCurve nearest(at,c.curve,c.first,c.last);
      if(nearest.NbPoints() && nearest.LowerDistance()<=tolerance) {joined=true;break;}
    }
    if(!joined) result.insert(id);
  }
  return {result.begin(),result.end()};
}
}
