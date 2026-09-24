#include "opad/geometry.hpp"
#include <BRep_Tool.hxx>
#include <BRepBndLib.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <Poly_Triangulation.hxx>
#include <GC_MakeCircle.hxx>
#include <Geom_Circle.hxx>
#include <set>
#include <algorithm>
#include <cmath>

namespace opad {
std::vector<MeshCircle> mesh_circles(const TopoDS_Shape& shape) {
  if (!is_mesh_shape(shape)) return {};
  Bnd_Box box; BRepBndLib::Add(shape,box); if(box.IsVoid()) return {};
  const double tolerance=std::max(1e-7,box.CornerMin().Distance(box.CornerMax())*1e-7);
  std::map<std::array<long long,3>,int> welded;
  std::vector<gp_Pnt> points;
  auto vertex=[&](const gp_Pnt& p) {
    const std::array<long long,3> key{std::llround(p.X()/tolerance),std::llround(p.Y()/tolerance),std::llround(p.Z()/tolerance)};
    auto [it,added]=welded.emplace(key,int(points.size())); if(added) points.push_back(p); return it->second;
  };
  struct Edge { std::vector<int> ordinals; std::vector<gp_Vec> normals; };
  std::map<std::pair<int,int>,Edge> edges;
  int ordinal=0;
  for(TopExp_Explorer f(shape,TopAbs_FACE);f.More();f.Next()) {
    TopLoc_Location loc; const auto mesh=BRep_Tool::Triangulation(TopoDS::Face(f.Current()),loc);
    if(mesh.IsNull()) continue;
    for(int i=1;i<=mesh->NbTriangles();++i) {
      int a,b,c; mesh->Triangle(i).Get(a,b,c);
      const int v[]{vertex(mesh->Node(a).Transformed(loc.Transformation())),vertex(mesh->Node(b).Transformed(loc.Transformation())),vertex(mesh->Node(c).Transformed(loc.Transformation()))};
      gp_Vec n=gp_Vec(points[v[0]],points[v[1]]).Crossed(gp_Vec(points[v[0]],points[v[2]]));
      if(n.SquareMagnitude()<1e-24) { ordinal+=3; continue; } n.Normalize();
      for(int k=0;k<3;++k) { auto& e=edges[std::minmax(v[k],v[(k+1)%3])]; e.ordinals.push_back(ordinal++); e.normals.push_back(n); }
    }
  }
  // Boundary and sharp crease loops, excluding the internal triangulation diagonals.
  std::map<int,std::vector<int>> graph;
  for(const auto& [pair,e]:edges) if(e.normals.size()==1 || (e.normals.size()==2 && std::abs(e.normals[0].Dot(e.normals[1]))<0.866)) {
    graph[pair.first].push_back(pair.second); graph[pair.second].push_back(pair.first);
  }
  std::set<int> visited; std::vector<MeshCircle> out;
  for(const auto& [start,neighbors]:graph) {
    if(visited.count(start) || neighbors.size()!=2) continue;
    std::vector<int> ring; int prev=-1,at=start;
    do {
      if(visited.count(at) || graph[at].size()!=2) break;
      visited.insert(at); ring.push_back(at);
      const auto& next=graph[at]; int target=next[0]==prev?next[1]:next[0]; prev=at; at=target;
    } while(at!=start);
    if(at!=start || ring.size()<8) continue;
    GC_MakeCircle fit(points[ring[0]],points[ring[ring.size()/3]],points[ring[2*ring.size()/3]]);
    if(!fit.IsDone()) continue;
    const gp_Circ circle=fit.Value()->Circ(); const double tol=std::max(tolerance*4,circle.Radius()*1e-5);
    bool valid=true; double winding=0;
    for(size_t i=0;i<ring.size();++i) {
      const gp_Vec v(circle.Location(),points[ring[i]]), w(circle.Location(),points[ring[(i+1)%ring.size()]]);
      if(std::abs(v.Magnitude()-circle.Radius())>tol || std::abs(v.Dot(gp_Vec(circle.Axis().Direction())))>tol) valid=false;
      const double angle=std::atan2(gp_Vec(circle.Axis().Direction()).Dot(v.Crossed(w)),v.Dot(w));
      if(std::abs(angle)>M_PI/2) valid=false;
      winding+=angle;
    }
    if(!valid || std::abs(std::abs(winding)-2*M_PI)>1e-4) continue;
    MeshCircle result; result.circle=circle; result.segments=int(ring.size());
    for(size_t i=0;i<ring.size();++i) {
      result.rim.push_back(points[ring[i]]);
      const auto& e=edges.at(std::minmax(ring[i],ring[(i+1)%ring.size()])); result.edges.insert(result.edges.end(),e.ordinals.begin(),e.ordinals.end());
    }
    result.index=*std::min_element(result.edges.begin(),result.edges.end()); out.push_back(std::move(result));
  }
  std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.index<b.index;});
  return out;
}
}
