#include "opad/design/drawing_sketch.hpp"
#include "opad/geometry.hpp"
#include <BRepAdaptor_Curve.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <cmath>
#include <set>

namespace opad::design {
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
          GCPnts_QuasiUniformDeflection samples(c,tolerance,first,last);
          if(!samples.IsDone()) throw Error("Could not flatten drawing curve at the requested tolerance");
          for(int j=2;j<=samples.NbPoints();++j) line(samples.Value(j-1),samples.Value(j));
        }
      }
      if(result.entities.size()>100000) throw Error("Converted sketch exceeds 100,000 entities; select fewer layers or increase tolerance");
    }
  }
  if(result.entities.empty()) throw Error("No vector curves in the selected layers");
  result.validate(); return result;
}
}
