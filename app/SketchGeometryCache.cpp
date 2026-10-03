#include "SketchGeometryCache.hpp"
#include "CurveSamples.hpp"
#include "opad/design/sketch_geom.hpp"
#include <algorithm>
#include <cmath>

using namespace opad::design;
namespace {
bool sameCurve(const SkEntity& a,const SkEntity& b) {
  return a.id==b.id&&a.type==b.type&&a.p==b.p&&a.r==b.r&&a.degree==b.degree&&a.periodic==b.periodic&&a.knots==b.knots&&a.weights==b.weights&&a.multiplicities==b.multiplicities&&a.start_tangent==b.start_tangent&&a.end_tangent==b.end_tangent;
}
}
void SketchGeometryCache::Box::add(double x,double y){x0=std::min(x0,x);y0=std::min(y0,y);x1=std::max(x1,x);y1=std::max(y1,y);}
void SketchGeometryCache::Box::add(const Box& b){if(b.x0<=b.x1){add(b.x0,b.y0);add(b.x1,b.y1);}}
bool SketchGeometryCache::Box::intersects(const Box& b)const{return x0<=b.x1&&x1>=b.x0&&y0<=b.y1&&y1>=b.y0;}
const SkPoint* SketchGeometryCache::point(const Sketch& sk,int id) const {
  auto found=m_pointIndex.find(id);
  if(found!=m_pointIndex.end()&&found->second<sk.points.size()&&sk.points[found->second].id==id)return &sk.points[found->second];
  return sk.point(id);
}
const SkEntity* SketchGeometryCache::entity(const Sketch& sk,int id) const {
  auto found=m_entityIndex.find(id);
  if(found!=m_entityIndex.end()&&found->second<sk.entities.size()&&sk.entities[found->second].id==id)return &sk.entities[found->second];
  return sk.entity(id);
}
const std::vector<size_t>& SketchGeometryCache::curvesAt(int point) const {
  static const std::vector<size_t> none;
  const auto found=m_pointCurves.find(point);
  return found==m_pointCurves.end()?none:found->second;
}
std::vector<double> SketchGeometryCache::signature(const Sketch& sk,const SkEntity& e)const {
  std::vector<double> out{double(e.type),e.r,double(e.degree),double(e.periodic),double(e.p.size()),double(e.knots.size())};
  for(int id:e.p){const auto* p=point(sk,id);if(!p)throw opad::Error("missing sketch control point");out.insert(out.end(),{double(id),p->x,p->y});}
  out.insert(out.end(),e.knots.begin(),e.knots.end());out.insert(out.end(),e.weights.begin(),e.weights.end());
  for(int v:e.multiplicities)out.push_back(v);
  out.insert(out.end(),e.start_tangent.begin(),e.start_tangent.end());out.push_back(-1);out.insert(out.end(),e.end_tangent.begin(),e.end_tangent.end());
  return out;
}
bool SketchGeometryCache::matches(const Sketch& sk,double deflection)const {
  if(m_deflection<=0||deflection<m_deflection*.75||sk.points.size()!=m_points.size()||sk.entities.size()!=m_entities.size())return false;
  for(size_t i=0;i<sk.points.size();++i){const auto& a=sk.points[i];const auto& b=m_points[i];if(a.id!=b.id||a.x!=b.x||a.y!=b.y)return false;}
  for(size_t i=0;i<sk.entities.size();++i)if(!sameCurve(sk.entities[i],m_entities[i]))return false;
  return true;
}
const SketchGeometryCache::Polyline* SketchGeometryCache::samples(const Sketch& sk,const SkEntity& e)const {
  const auto found=m_curves.find(e.id);
  return found!=m_curves.end()&&found->second.signature==signature(sk,e)?&found->second.poly:nullptr;
}
void SketchGeometryCache::update(const Sketch& sk,double deflection) {
  deflection=std::max(1e-7,deflection);
  const bool finer=m_deflection<=0||deflection<m_deflection*.75;
  if(finer)m_deflection=deflection;
  m_points=sk.points;m_entities=sk.entities;m_pointIndex.clear();m_entityIndex.clear();m_centres.clear();m_pointCurves.clear();m_entries.clear();m_tree.clear();
  for(size_t i=0;i<sk.points.size();++i){const auto& p=sk.points[i];m_pointIndex[p.id]=i;Box box;box.add(p.x,p.y);m_entries.push_back({box,i,true});}
  std::unordered_map<int,Curve> curves;
  for(size_t i=0;i<sk.entities.size();++i) {
    const auto& e=sk.entities[i];auto key=signature(sk,e);auto old=m_curves.find(e.id);Curve curve;
    m_entityIndex[e.id]=i;
    if((e.type==SkEntity::Type::Circle||e.type==SkEntity::Type::Arc||e.type==SkEntity::Type::Ellipse)&&!e.p.empty())m_centres.insert(e.p[0]);
    for(int id:e.p){auto& at=m_pointCurves[id];if(at.empty()||at.back()!=i)at.push_back(i);}
    if(!finer&&old!=m_curves.end()&&old->second.signature==key)curve=std::move(old->second);
    else {
      ++builds;curve.signature=std::move(key);
      // Limit point lookup to this curve instead of searching every point for each pole.
      Sketch local;for(int id:e.p)if(const auto* p=point(sk,id))local.points.push_back(*p);
      if(e.type==SkEntity::Type::Point){const auto* p=point(sk,e.p[0]);curve.poly.push_back({p->x,p->y});}
      else if(e.type==SkEntity::Type::Line){for(int id:e.p){const auto* p=point(sk,id);curve.poly.push_back({p->x,p->y});}}
      else {const auto edge=entity_edge(local,e,{});if(!edge.IsNull())for(const auto& p:curveSamples(edge,m_deflection))curve.poly.push_back({p.X(),p.Y()});}
      for(auto [x,y]:curve.poly)curve.box.add(x,y);
    }
    if(curve.box.x0<=curve.box.x1)m_entries.push_back({curve.box,i,false});
    curves.emplace(e.id,std::move(curve));
  }
  m_curves=std::move(curves);
  if(!m_entries.empty())partition(0,m_entries.size());
}
int SketchGeometryCache::partition(size_t begin,size_t end) {
  const int index=int(m_tree.size());m_tree.push_back({});Box bounds;
  for(size_t i=begin;i<end;++i)bounds.add(m_entries[i].box);
  m_tree[index].box=bounds;m_tree[index].begin=begin;m_tree[index].end=end;
  if(end-begin>12){
    const bool x=bounds.x1-bounds.x0>=bounds.y1-bounds.y0;const size_t mid=begin+(end-begin)/2;
    std::nth_element(m_entries.begin()+begin,m_entries.begin()+mid,m_entries.begin()+end,[x](const Entry& a,const Entry& b){return x?a.box.x0+a.box.x1<b.box.x0+b.box.x1:a.box.y0+a.box.y1<b.box.y0+b.box.y1;});
    const int left=partition(begin,mid),right=partition(mid,end);m_tree[index].left=left;m_tree[index].right=right;
  }return index;
}
SketchGeometryCache::Query SketchGeometryCache::query(double x0,double y0,double x1,double y1)const {
  Query out;if(m_tree.empty())return out;Box box;box.add(x0,y0);box.add(x1,y1);std::vector<int> pending{0};
  while(!pending.empty()){const auto& n=m_tree[pending.back()];pending.pop_back();if(!n.box.intersects(box))continue;
    if(n.left>=0){pending.push_back(n.left);pending.push_back(n.right);continue;}
    for(size_t i=n.begin;i<n.end;++i){const auto& e=m_entries[i];if(e.box.intersects(box))(e.point?out.points:out.entities).push_back(e.index);}
  }
  std::sort(out.points.begin(),out.points.end());std::sort(out.entities.begin(),out.entities.end());return out;
}
