#include "opad/design/drawing_sketch.hpp"
#include "opad/geometry.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <GeomConvert.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <GC_MakeCircle.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <algorithm>
#include <cmath>
#include <set>

namespace opad::design {
namespace {
// Only join smooth degree-two chains. Branches and sharp joins remain explicit
// endpoints, so fitting cannot round off corners or change connectivity.
void reconstruct(Sketch& sk,double tolerance) {
  std::map<int,std::vector<size_t>> adjacent;
  std::map<int,gp_Pnt> pts;
  for(const auto& p:sk.points) pts[p.id]=gp_Pnt(p.x,p.y,0);
  for(size_t i=0;i<sk.entities.size();++i) {
    const auto& e=sk.entities[i];
    if(e.type==SkEntity::Type::Line) for(int id:e.p) adjacent[id].push_back(i);
    else for(int id:e.p) adjacent[id].push_back(sk.entities.size()); // stop at native curves
  }
  std::vector<bool> used(sk.entities.size()); std::vector<SkEntity> out;
  int next=sk.next_id();
  auto addPoint=[&](const gp_Pnt& p) {int id=next++; sk.points.push_back({id,p.X(),p.Y(),false}); return id;};
  for(size_t seed=0;seed<sk.entities.size();++seed) {
    if(used[seed]) continue;
    const auto original=sk.entities[seed];
    if(original.type!=SkEntity::Type::Line) {out.push_back(original);used[seed]=true;continue;}
    std::vector<int> chain=original.p; std::vector<size_t> members{seed};used[seed]=true;
    for(int side=0;side<2;++side) {
      if(side) std::reverse(chain.begin(),chain.end());
      while(chain.size()<256) {
        const int end=chain.back(), prev=chain[chain.size()-2]; const auto& links=adjacent[end];
        if(links.size()!=2) break;
        size_t candidate=sk.entities.size(); for(auto i:links) if(i<used.size() && !used[i]) candidate=i;
        if(candidate==sk.entities.size()) break;
        const auto& e=sk.entities[candidate]; if(e.construction!=original.construction) break;
        int other=e.p[0]==end?e.p[1]:e.p[0];
        gp_Vec incoming(pts[prev],pts[end]), outgoing(pts[end],pts[other]);
        if(incoming.SquareMagnitude()<1e-16 || outgoing.SquareMagnitude()<1e-16 || incoming.Angle(outgoing)>0.18) break;
        chain.push_back(other); members.push_back(candidate);used[candidate]=true;
        if(chain.back()==chain.front()) break;
      }
    }
    bool fitted=false;
    if(chain.size()>=5) {
      const auto& a=pts[chain.front()];const auto& b=pts[chain.back()];
      const bool closed=chain.front()==chain.back();
      GC_MakeCircle fit(a,pts[chain[chain.size()/3]],pts[chain[2*chain.size()/3]]);
      if(fit.IsDone()) {
        const auto circle=fit.Value();const auto c=circle->Location(); const double r=circle->Radius();bool good=true;
        // Include chord midpoints: fitting is bounded against the source polyline,
        // not only its vertices. Reject loops with inconsistent winding.
        double sweep=0;
        for(size_t j=1;j<chain.size();++j) {
          const auto p=pts[chain[j-1]],q=pts[chain[j]];
          gp_Pnt mid((p.X()+q.X())/2,(p.Y()+q.Y())/2,0);
          good &= std::abs(c.Distance(p)-r)<=tolerance && std::abs(c.Distance(q)-r)<=tolerance && std::abs(c.Distance(mid)-r)<=tolerance;
          const double turn=std::atan2((p.X()-c.X())*(q.Y()-c.Y())-(p.Y()-c.Y())*(q.X()-c.X()),(p.X()-c.X())*(q.X()-c.X())+(p.Y()-c.Y())*(q.Y()-c.Y()));
          if(sweep*turn<0) good=false;
          sweep+=turn;
        }
        if(good && (closed || std::abs(sweep)<2*M_PI-1e-7)) {
          SkEntity e=original;e.type=SkEntity::Type::Arc; int first=chain.front(),last=chain.back(); if(sweep<0) std::swap(first,last);
          e.p={addPoint(c),first,last};
          if(closed) {e.type=SkEntity::Type::Circle;e.p.resize(1);e.r=r;}
          out.push_back(e);fitted=true;
        }
      }
      if(!fitted && !closed) {
        TColgp_Array1OfPnt samples(1,int(chain.size()));for(size_t j=0;j<chain.size();++j)samples.SetValue(int(j)+1,pts[chain[j]]);
        GeomAPI_PointsToBSpline fit(samples,3,5,GeomAbs_C1,tolerance*0.25);
        if(fit.IsDone()) {
          auto curve=fit.Curve(); bool good=curve->StartPoint().Distance(a)<1e-7 && curve->EndPoint().Distance(b)<1e-7;
          for(size_t j=1;j<chain.size() && good;++j) for(int k=0;k<=4;++k) {
            const auto p=pts[chain[j-1]],q=pts[chain[j]]; const gp_Pnt sample(p.X()+(q.X()-p.X())*k/4,p.Y()+(q.Y()-p.Y())*k/4,0);
            GeomAPI_ProjectPointOnCurve projection(sample,curve);good &= projection.NbPoints()>0 && projection.LowerDistance()<=tolerance;
          }
          // Also bound curve-to-source deviation to reject spline overshoot.
          for(int j=0;j<=int(chain.size())*8 && good;++j) {
            const gp_Pnt sample=curve->Value(curve->FirstParameter()+(curve->LastParameter()-curve->FirstParameter())*j/(chain.size()*8));double best=1e100;
            for(size_t k=1;k<chain.size();++k) {const auto p=pts[chain[k-1]],q=pts[chain[k]];const gp_Vec v(p,q),w(p,sample);double t=std::clamp(w.Dot(v)/v.SquareMagnitude(),0.0,1.0);best=std::min(best,sample.Distance(p.Translated(v*t)));}
            good &= best<=tolerance;
          }
          if(good) {
            SkEntity e=original;e.type=SkEntity::Type::Spline;e.p.clear();e.degree=curve->Degree();
            for(int j=1;j<=curve->NbPoles();++j) { e.p.push_back(j==1?chain.front():j==curve->NbPoles()?chain.back():addPoint(curve->Pole(j)));e.weights.push_back(curve->Weight(j)); }
            for(int j=1;j<=curve->NbKnots();++j) {e.knots.push_back(curve->Knot(j));e.multiplicities.push_back(curve->Multiplicity(j));}
            out.push_back(std::move(e));fitted=true;
          }
        }
      }
    }
    if(!fitted) for(auto i:members) out.push_back(sk.entities[i]);
  }
  sk.entities=std::move(out);std::set<int> retained;for(const auto& e:sk.entities)for(int id:e.p)retained.insert(id);
  std::erase_if(sk.points,[&](const SkPoint& p){return !retained.count(p.id);});
}
}
void simplify_sketch(Sketch& sketch,double tolerance) {
  if(!sketch.constraints.empty() || !sketch.patterns.empty())throw Error("simplify unconstrainted imported or traced curves before adding dimensions");
  for(const auto& e:sketch.entities)if(!e.source.is_null())throw Error("break projection links before simplifying");
  if(!(tolerance>0)||!std::isfinite(tolerance))throw Error("curve tolerance must be positive");
  sketch.id_watermark=sketch.next_id()-1;reconstruct(sketch,tolerance);sketch.validate();
}
Sketch drawing_sketch(const Document& doc,const Scene& scene,const std::vector<DrawingLayer>& layers,const Frame& frame,double tolerance) {
  if(!(tolerance>0) || !std::isfinite(tolerance)) throw Error("Curve tolerance must be positive");
  Sketch result; std::set<std::string> used;
  std::map<std::pair<long long,long long>,int> points;
  auto point=[&](const gp_Pnt& p) {
    double u,v; frame.to_local({p.X(),p.Y(),p.Z()},u,v);
    const auto key=std::make_pair(std::llround(u*1e7),std::llround(v*1e7));
    auto [it,added]=points.emplace(key,0); if(added) it->second=result.add_point(u,v); return it->second;
  };
  const auto normal=frame.normal(); const gp_Dir axis(normal[0],normal[1],normal[2]);
  for(const auto& layer:layers) {
    if(!used.insert(layer.id).second) continue;
    const auto* node=scene.node(layer.id);
    if(!node || node->representation!="drawing2d") throw Error("Select drawing layers to convert");
    if(!node->raster.is_null()) throw Error("Raster images have no editable vector curves; exclude the image layer");
    TopTools_IndexedMapOfShape edges; TopExp::MapShapes(node_world_shape(doc,scene,layer.id),TopAbs_EDGE,edges);
    for(int i=1;i<=edges.Extent();++i) {
      BRepAdaptor_Curve c(TopoDS::Edge(edges(i))); const double first=c.FirstParameter(),last=c.LastParameter();
      if(c.GetType()==GeomAbs_Circle && std::abs(c.Circle().Axis().Direction().Dot(axis))>1-1e-8) {
        const int center=point(c.Circle().Location());
        if(last-first>=2*M_PI-1e-8) result.add_circle(center,c.Circle().Radius(),layer.construction);
        else {
          int a=point(c.Value(first)),b=point(c.Value(last));
          if(c.Circle().Axis().Direction().Dot(axis)<0) std::swap(a,b);
          if(a!=b) result.add_arc(center,a,b,layer.construction);
        }
      } else {
        auto line=[&](const gp_Pnt& a,const gp_Pnt& b) { int ia=point(a),ib=point(b); if(ia!=ib) result.add_line(ia,ib,layer.construction); };
        if(c.GetType()==GeomAbs_Line) line(c.Value(first),c.Value(last));
        else {
          // Preserve the source basis exactly. Projecting its poles is an exact affine
          // projection, including rational conics and transformed SVG Beziers.
          Handle(Geom_Curve) source=Handle(Geom_Curve)::DownCast(c.Curve().Curve()->Transformed(c.Trsf()));
          Handle(Geom_TrimmedCurve) trimmed=new Geom_TrimmedCurve(source,first,last);
          auto spline=GeomConvert::CurveToBSplineCurve(trimmed);
          SkEntity e; e.type=SkEntity::Type::Spline; e.degree=spline->Degree(); e.periodic=spline->IsPeriodic(); e.construction=layer.construction;
          for(int j=1;j<=spline->NbPoles();++j) { e.p.push_back(point(spline->Pole(j))); e.weights.push_back(spline->Weight(j)); }
          for(int j=1;j<=spline->NbKnots();++j) { e.knots.push_back(spline->Knot(j)); e.multiplicities.push_back(spline->Multiplicity(j)); }
          e.id=result.next_id(); result.entities.push_back(std::move(e));
        }
      }
      if(result.entities.size()>100000) throw Error("Converted sketch exceeds 100,000 entities; select fewer layers or increase tolerance");
    }
  }
  if(result.entities.empty()) throw Error("No vector curves in the selected layers");
  reconstruct(result,tolerance);
  result.validate(); return result;
}
}
