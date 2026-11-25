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
#include <cstdint>
#include <unordered_map>
#include <map>

namespace opad {
namespace {
struct KeyHash {
  size_t operator()(const std::array<long long, 3>& k) const {
    uint64_t h = 1469598103934665603ull;
    for (long long v : k) h = (h ^ uint64_t(v)) * 1099511628211ull, h ^= h >> 29;
    return size_t(h);
  }
};
}  // namespace

// Rims of a mesh: closed loops of its boundary and sharp-crease edges that lie on a circle. A 3D scan is millions of
// triangles: the nodes are welded through a hash (once per node, then numbered in the order the triangles first use them,
// as a weld per corner numbered them) and the sides sorted by their two ends, where maps of vectors took 13 s on a
// 3.3M-triangle scan.
std::vector<MeshCircle> mesh_circles(const TopoDS_Shape& shape) {
  if (!is_mesh_shape(shape)) return {};
  Bnd_Box box; BRepBndLib::Add(shape,box); if(box.IsVoid()) return {};
  const double tolerance=std::max(1e-7,box.CornerMin().Distance(box.CornerMax())*1e-7);
  struct Part { Handle(Poly_Triangulation) mesh; gp_Trsf trsf; bool moved; int firstNode; };
  std::vector<Part> parts;
  size_t nodes = 0, triangles = 0;
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
    TopLoc_Location loc; const auto mesh = BRep_Tool::Triangulation(TopoDS::Face(f.Current()), loc);
    if (mesh.IsNull()) continue;
    parts.push_back({mesh, loc.Transformation(), !loc.IsIdentity(), int(nodes)});
    nodes += size_t(mesh->NbNodes());
    triangles += size_t(mesh->NbTriangles());
  }
  // Each node to a weld class (first node of its key), then the classes numbered by first use.
  std::vector<int> klass(nodes);
  std::vector<gp_Pnt> at(nodes);
  {
    std::unordered_map<std::array<long long, 3>, int, KeyHash> welded;
    welded.reserve(nodes);
    for (const Part& part : parts)
      for (int i = 1; i <= part.mesh->NbNodes(); ++i) {
        const gp_Pnt p = part.moved ? part.mesh->Node(i).Transformed(part.trsf) : part.mesh->Node(i);
        const size_t n = size_t(part.firstNode + i - 1);
        at[n] = p;
        const std::array<long long, 3> key{std::llround(p.X() / tolerance), std::llround(p.Y() / tolerance), std::llround(p.Z() / tolerance)};
        klass[n] = welded.emplace(key, int(n)).first->second;
      }
  }
  std::vector<int> id(nodes, -1);  // weld class (its first node) -> vertex number by first use
  std::vector<gp_Pnt> points;
  auto vertex = [&](size_t n) {
    int& v = id[size_t(klass[n])];
    if (v < 0) v = int(points.size()), points.push_back(at[n]);
    return v;
  };
  // Every side once per triangle: its ends (lower first), its ordinal, its triangle's normal.
  struct Side { uint64_t key; int ordinal; int normal; };
  std::vector<Side> sides;
  sides.reserve(triangles * 3);
  std::vector<gp_Vec> normals;
  normals.reserve(triangles);
  int ordinal = 0;
  for (const Part& part : parts)
    for (int i = 1; i <= part.mesh->NbTriangles(); ++i) {
      int a, b, c; part.mesh->Triangle(i).Get(a, b, c);
      const int v[]{vertex(size_t(part.firstNode + a - 1)), vertex(size_t(part.firstNode + b - 1)), vertex(size_t(part.firstNode + c - 1))};
      gp_Vec n = gp_Vec(points[size_t(v[0])], points[size_t(v[1])]).Crossed(gp_Vec(points[size_t(v[0])], points[size_t(v[2])]));
      if (n.SquareMagnitude() < 1e-24) { ordinal += 3; continue; }
      n.Normalize();
      normals.push_back(n);
      for (int k = 0; k < 3; ++k) {
        const auto [lo, hi] = std::minmax(v[k], v[(k + 1) % 3]);
        sides.push_back({uint64_t(uint32_t(lo)) << 32 | uint32_t(hi), ordinal++, int(normals.size() - 1)});
      }
    }
  std::sort(sides.begin(), sides.end(), [](const Side& x, const Side& y) { return x.key != y.key ? x.key < y.key : x.ordinal < y.ordinal; });
  // Boundary and sharp crease loops, excluding the internal triangulation diagonals.
  std::map<int,std::vector<int>> graph;
  std::unordered_map<uint64_t, std::pair<size_t, size_t>> rims;  // a loop side's run in `sides`
  for (size_t s = 0; s < sides.size();) {
    size_t e = s;
    while (e < sides.size() && sides[e].key == sides[s].key) ++e;
    const size_t count = e - s;
    if (count == 1 || (count == 2 && std::abs(normals[size_t(sides[s].normal)].Dot(normals[size_t(sides[s + 1].normal)])) < 0.866)) {
      const int lo = int(sides[s].key >> 32), hi = int(sides[s].key & 0xffffffffu);
      graph[lo].push_back(hi); graph[hi].push_back(lo);
      rims.emplace(sides[s].key, std::make_pair(s, e));
    }
    s = e;
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
      const auto [lo, hi] = std::minmax(ring[i], ring[(i + 1) % ring.size()]);
      const auto [first, last] = rims.at(uint64_t(uint32_t(lo)) << 32 | uint32_t(hi));
      for (size_t s = first; s < last; ++s) result.edges.push_back(sides[s].ordinal);
    }
    result.index=*std::min_element(result.edges.begin(),result.edges.end()); out.push_back(std::move(result));
  }
  std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return a.index<b.index;});
  return out;
}
}
