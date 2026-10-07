// Mesh to solid, the general path (opad/mesh_solid.hpp): regions of the mesh that one surface fits, their borders made the
// edges, the faces sewn into a solid. Three tries, each valid where the one before is not: the fitted surfaces; planar
// facets (coplanar triangles merged); one face per triangle.
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_IntSS.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Line.hxx>
#include <Precision.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Shape.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax2.hxx>
#include <gp_Lin.hxx>

#include <Eigen/Dense>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <numeric>
#include <queue>
#include <set>

#include "meshsolid.hpp"
#include "opad/util.hpp"

namespace opad {
using namespace meshsolid;

namespace {

bool debugging() {
  static const bool on = std::getenv("OPAD_MESHSOLID_DEBUG") != nullptr;
  return on;
}

struct Region {
  Prim prim;
  bool flip = false;  // the mesh's outside is against the surface's own normal (a hole's wall, an inside fillet)
  std::vector<int> tris;
  Handle(Geom_Surface) surface;
};

struct Context {
  const TriMesh& m;
  const Topology& topo;
  double tol, angle, size;
  const std::function<bool()>& cancelled;
  double sagitta = 0;  // the mesh's chord error (mesh_sagitta)
  mutable std::vector<int> stamp;  // per triangle: the pass that took it (no clearing between passes)
  mutable int pass = 0;
  void check() const {
    if (cancelled && cancelled()) throw Error("cancelled");
  }
};

std::vector<gp_Pnt> vertices_of(const TriMesh& m, const std::vector<int>& tris) {
  std::vector<int> ids;
  ids.reserve(tris.size() * 3);
  for (int t : tris)
    for (int v : m.triangles[size_t(t)]) ids.push_back(v);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<gp_Pnt> out;
  out.reserve(ids.size());
  for (int v : ids) out.push_back(m.points[size_t(v)]);
  return out;
}

double area_of(const Context& c, const std::vector<int>& tris) {
  double a = 0;
  for (int t : tris) a += c.topo.area[size_t(t)];
  return a;
}

bool on(const Context& c, const Prim& p, int t) {
  for (int v : c.m.triangles[size_t(t)])
    if (std::abs(p.distance(c.m.points[size_t(v)])) > c.tol) return false;
  return true;
}

bool facing(const Context& c, const Prim& p, int t, double sign) {
  return c.topo.normal[size_t(t)].Dot(gp_Vec(p.normal(c.topo.centroid(t)))) * sign >= std::cos(c.angle);
}

double orientation(const Context& c, const Prim& p, const std::vector<int>& tris) {
  double s = 0;
  for (int t : tris) s += c.topo.area[size_t(t)] * c.topo.normal[size_t(t)].Dot(gp_Vec(p.normal(c.topo.centroid(t))));
  return s >= 0 ? 1.0 : -1.0;
}

// The triangles reachable from the seeds over smooth sides that the surface fits, among those still free.
std::vector<int> grow(const Context& c, const Prim& p, const std::vector<int>& seeds, const std::vector<int>& owner, double sign) {
  const int pass = ++c.pass;
  std::vector<int> out;
  for (int s : seeds)
    if (owner[size_t(s)] < 0 && c.stamp[size_t(s)] != pass && on(c, p, s) && facing(c, p, s, sign)) {
      c.stamp[size_t(s)] = pass;
      out.push_back(s);
    }
  for (size_t i = 0; i < out.size(); ++i) {
    const int t = out[i];
    for (int k = 0; k < 3; ++k) {
      const int n = c.topo.across[size_t(t)][size_t(k)];
      if (n < 0 || c.topo.dihedral[size_t(t)][size_t(k)] >= c.angle || owner[size_t(n)] >= 0 || c.stamp[size_t(n)] == pass) continue;
      if (!on(c, p, n) || !facing(c, p, n, sign)) continue;
      c.stamp[size_t(n)] = pass;
      out.push_back(n);
    }
  }
  return out;
}

// Connected groups of coplanar triangles (within the tolerance and a few degrees), largest triangles first.
std::vector<std::vector<int>> planar_clusters(const Context& c, const std::vector<int>& tris) {
  std::vector<int> order = tris;
  std::sort(order.begin(), order.end(), [&](int a, int b) { return c.topo.area[size_t(a)] > c.topo.area[size_t(b)]; });
  std::vector<char> wanted(c.m.triangles.size(), 0), taken(c.m.triangles.size(), 0);
  for (int t : tris) wanted[size_t(t)] = 1;
  const double flat = std::cos(std::min(c.angle / 4, 3 * M_PI / 180));
  std::vector<std::vector<int>> out;
  for (int seed : order) {
    if (taken[size_t(seed)]) continue;
    Prim plane;
    plane.type = Prim::Plane;
    plane.o = c.topo.centroid(seed);
    plane.a = gp_Dir(c.topo.normal[size_t(seed)]);
    std::vector<int> group{seed};
    taken[size_t(seed)] = 1;
    for (size_t i = 0; i < group.size(); ++i)
      for (int k = 0; k < 3; ++k) {
        const int n = c.topo.across[size_t(group[i])][size_t(k)];
        if (n < 0 || !wanted[size_t(n)] || taken[size_t(n)]) continue;
        if (c.topo.normal[size_t(n)].Dot(gp_Vec(plane.a)) < flat || !on(c, plane, n)) continue;
        taken[size_t(n)] = 1;
        group.push_back(n);
      }
    out.push_back(std::move(group));
  }
  return out;
}

Prim plane_of(const Context& c, const std::vector<int>& tris) {
  Prim seed;
  seed.type = Prim::Plane;
  seed.o = c.topo.centroid(tris.front());
  seed.a = gp_Dir(c.topo.normal[size_t(tris.front())]);
  const auto pts = vertices_of(c.m, tris);
  if (auto fit = fit_plane(pts); fit && max_distance(*fit, pts) <= c.tol) {
    if (fit->a.Dot(seed.a) < 0) fit->a.Reverse();
    return *fit;
  }
  return seed;
}

std::vector<int> largest_component(const Context& c, const std::vector<int>& tris) {
  const int pass = ++c.pass;
  for (int t : tris) c.stamp[size_t(t)] = pass;
  const int seen = ++c.pass;
  std::vector<int> best;
  for (int start : tris) {
    if (c.stamp[size_t(start)] != pass) continue;
    std::vector<int> group{start};
    c.stamp[size_t(start)] = seen;
    for (size_t i = 0; i < group.size(); ++i)
      for (int k = 0; k < 3; ++k) {
        const int n = c.topo.across[size_t(group[i])][size_t(k)];
        if (n >= 0 && c.stamp[size_t(n)] == pass) {
          c.stamp[size_t(n)] = seen;
          group.push_back(n);
        }
      }
    if (group.size() > best.size()) best = std::move(group);
  }
  return best;
}

struct Candidate {
  Prim prim;
  double sign = 1;
  std::vector<int> support;
  double area = 0;
  double error = 0;  // facet middles off the surface (RMS)
};

double centre_error(const Context& c, const Prim& p, const std::vector<int>& tris) {
  double s = 0;
  for (int t : tris) {
    const double d = p.distance(c.topo.centroid(t));
    s += d * d;
  }
  return std::sqrt(s / double(std::max<size_t>(1, tris.size())));
}

// The regions: every plane, then curved surfaces grown from seeds on smooth facets, taken largest first.
std::vector<Region> segment(const Context& c) {
  const size_t n = c.m.triangles.size();
  std::vector<int> all(n);
  std::iota(all.begin(), all.end(), 0);
  std::vector<int> owner(n, -1);
  std::vector<Candidate> cands;
  const auto clusters = planar_clusters(c, all);
  std::vector<int> cluster(n, -1);
  std::vector<double> clusterArea;
  for (size_t g = 0; g < clusters.size(); ++g) {
    for (int t : clusters[g]) cluster[size_t(t)] = int(g);
    clusterArea.push_back(area_of(c, clusters[g]));
    Candidate cd;
    cd.prim = plane_of(c, clusters[g]);
    cd.sign = orientation(c, cd.prim, clusters[g]);
    cd.area = clusterArea.back();
    cd.support = clusters[g];
    cands.push_back(std::move(cd));
  }
  c.check();
  // Curved surfaces. A seed is a facet beside another at a small angle; one region per surface: a seed already inside a
  // found region is skipped.
  std::vector<char> covered(n, 0);
  std::vector<int> order = all;
  std::sort(order.begin(), order.end(), [&](int a, int b) { return c.topo.area[size_t(a)] > c.topo.area[size_t(b)]; });
  const double minTurn = 0.3 * M_PI / 180;
  size_t done = 0;
  for (int seed : order) {
    if (++done % 256 == 0) c.check();
    if (covered[size_t(seed)]) continue;
    bool smooth = false;
    for (int k = 0; k < 3; ++k) {
      const double d = c.topo.dihedral[size_t(seed)][size_t(k)];
      smooth |= c.topo.across[size_t(seed)][size_t(k)] >= 0 && d > minTurn && d < c.angle;
    }
    if (!smooth) continue;
    // A compact patch about the seed: whole facets (coplanar groups) across smooth sides, none much larger than the
    // seed's own (a plane the curved surface runs into tangentially is not part of it).
    const int pass = ++c.pass;
    const int home = cluster[size_t(seed)];
    std::vector<int> patch, groups{home};
    std::set<int> taken{home};
    for (size_t g = 0; g < groups.size() && patch.size() < 40; ++g) {
      for (int t : clusters[size_t(groups[g])]) {
        c.stamp[size_t(t)] = pass;
        patch.push_back(t);
      }
      for (int t : clusters[size_t(groups[g])])
        for (int k = 0; k < 3; ++k) {
          const int nb = c.topo.across[size_t(t)][size_t(k)];
          if (nb < 0 || c.topo.dihedral[size_t(t)][size_t(k)] >= c.angle || c.stamp[size_t(nb)] == pass) continue;
          const int other = cluster[size_t(nb)];
          // Facets of a size: a curved surface's strips, not the plane it runs into nor the finer facets of the next one.
          if (taken.count(other) || clusterArea[size_t(other)] > 4 * clusterArea[size_t(home)] || clusterArea[size_t(other)] * 4 < clusterArea[size_t(home)]) continue;
          taken.insert(other);
          groups.push_back(other);
        }
    }
    covered[size_t(seed)] = 1;
    for (int k = 0; k < 3; ++k)
      if (const int nb = c.topo.across[size_t(seed)][size_t(k)]; nb >= 0) covered[size_t(nb)] = 1;
    const auto pts = vertices_of(c.m, patch);
    if (patch.size() < 4 || pts.size() < 7) continue;
    std::vector<gp_Vec> normals;
    std::vector<gp_Pnt> at;
    for (int t : patch) {
      normals.push_back(c.topo.normal[size_t(t)]);
      at.push_back(c.topo.centroid(t));
    }
    std::vector<Prim> fits;
    if (auto f = fit_sphere(pts, c.size)) fits.push_back(*f);
    if (auto f = fit_cylinder(pts, normals, c.size)) fits.push_back(*f);
    if (auto f = fit_cone(pts, normals, at, c.size)) fits.push_back(*f);
    if (fits.empty() || std::none_of(fits.begin(), fits.end(), [&](const Prim& f) { return max_distance(f, pts) <= c.tol; }))
      if (auto f = fit_torus(pts, normals, at, c.size)) fits.push_back(*f);
    Candidate best;
    std::vector<Candidate> tried;
    for (const auto& f : fits) {
      if (max_distance(f, pts) > c.tol) continue;
      const double sign = orientation(c, f, patch);
      auto support = grow(c, f, patch, owner, sign);
      if (support.size() < 3) continue;
      Prim prim = f;
      const Prim refined = refine(f, vertices_of(c.m, support), c.size);
      if (max_distance(refined, vertices_of(c.m, support)) <= c.tol) {
        auto more = grow(c, refined, support, owner, sign);
        if (more.size() >= support.size()) {
          prim = refined;
          support = std::move(more);
        }
      }
      // The facets' middles too: a mesh whose vertices are all on rims fits more than one surface (two rings of a
      // cylinder lie on a sphere); the facets between them only lie on the right one, within their sagitta.
      const double off = centre_error(c, prim, support);
      if (off > c.tol + 2 * c.sagitta) continue;
      // A large sphere or cylinder fits a flat face too: what a plane holds is a plane.
      if (const auto pts = vertices_of(c.m, support); [&] { const auto flat = fit_plane(pts); return flat && max_distance(*flat, pts) <= c.tol; }()) continue;
      const double a = area_of(c, support);
      tried.push_back({prim, sign, std::move(support), a, off});
    }
    double largest = 0;
    for (const auto& t : tried) largest = std::max(largest, t.area);
    for (auto& t : tried)
      if (t.area >= 0.9 * largest && (best.support.empty() || t.error < best.error)) best = std::move(t);
    if (best.support.empty()) continue;
    for (int t : best.support) covered[size_t(t)] = 1;
    cands.push_back(std::move(best));
  }
  // Largest first; a candidate that lost triangles to a larger one is measured again with what is left.
  std::priority_queue<std::pair<double, int>> queue;
  for (size_t i = 0; i < cands.size(); ++i) queue.push({cands[i].area, int(i)});
  std::vector<Region> regions;
  while (!queue.empty()) {
    const int i = queue.top().second;
    queue.pop();
    Candidate& cd = cands[size_t(i)];
    std::vector<int> live;
    for (int t : cd.support)
      if (owner[size_t(t)] < 0) live.push_back(t);
    if (live.size() != cd.support.size()) {
      if (live.empty()) continue;
      cd.support = largest_component(c, live);
      cd.area = area_of(c, cd.support);
      if (cd.prim.type != Prim::Plane && cd.support.size() < 3) continue;
      queue.push({cd.area, i});
      continue;
    }
    const int id = int(regions.size());
    for (int t : cd.support) owner[size_t(t)] = id;
    Region r;
    r.prim = cd.prim;
    r.flip = cd.sign < 0;
    r.tris = std::move(cd.support);
    regions.push_back(std::move(r));
  }
  // Where two surfaces meet tangentially, a band of facets lies within the tolerance of both and went to the one taken
  // first: each facet at a border goes to the neighbouring surface that holds its corners clearly better.
  for (int round = 0; round < 64; ++round) {
    c.check();
    std::vector<int> of(n, -1);
    for (size_t r = 0; r < regions.size(); ++r)
      for (int t : regions[r].tris) of[size_t(t)] = int(r);
    auto misfit = [&](const Region& r, int t) {
      double s = 0;
      for (int v : c.m.triangles[size_t(t)]) s += std::pow(r.prim.distance(c.m.points[size_t(v)]), 2);
      return s;
    };
    bool moved = false;
    for (size_t t = 0; t < n; ++t) {
      const int here = of[t];
      if (here < 0) continue;
      double best = misfit(regions[size_t(here)], int(t));
      int to = here;
      for (int k = 0; k < 3; ++k) {
        const int nb = c.topo.across[t][size_t(k)];
        if (nb < 0 || of[size_t(nb)] < 0 || of[size_t(nb)] == here || c.topo.dihedral[t][size_t(k)] >= c.angle) continue;
        const Region& r = regions[size_t(of[size_t(nb)])];
        if (!on(c, r.prim, int(t)) || !facing(c, r.prim, int(t), r.flip ? -1 : 1)) continue;
        const double m = misfit(r, int(t));
        if (m < best * 0.25 && best > std::pow(c.tol * 0.01, 2)) {
          best = m;
          to = of[size_t(nb)];
        }
      }
      if (to != here) {
        of[t] = to;
        moved = true;
      }
    }
    if (!moved) break;
    for (auto& r : regions) r.tris.clear();
    for (size_t t = 0; t < n; ++t)
      if (of[t] >= 0) regions[size_t(of[t])].tris.push_back(int(t));
    regions.erase(std::remove_if(regions.begin(), regions.end(), [](const Region& r) { return r.tris.empty(); }), regions.end());
  }
  for (auto& r : regions)
    if (r.tris.size() > 1 && r.tris.size() != largest_component(c, r.tris).size()) {
      // A region a move split in two keeps its larger part; the rest becomes facets below.
      r.tris = largest_component(c, r.tris);
    }
  std::vector<char> kept(n, 0);
  for (const auto& r : regions)
    for (int t : r.tris) kept[size_t(t)] = 1;
  for (size_t t = 0; t < n; ++t) owner[t] = kept[t] ? 0 : -1;
  // Each curved surface fitted again on its inner facets only: those at its border may belong to the neighbour (they were
  // taken because they are within the tolerance of both) and pull the fit off by as much.
  for (auto& r : regions) {
    if (r.prim.type == Prim::Plane) continue;
    c.check();
    const int pass = ++c.pass;
    for (int t : r.tris) c.stamp[size_t(t)] = pass;
    std::vector<int> inner;
    for (int t : r.tris) {
      bool all = true;
      for (int k = 0; k < 3; ++k) {
        const int nb = c.topo.across[size_t(t)][size_t(k)];
        all &= nb >= 0 && c.stamp[size_t(nb)] == pass;
      }
      if (all) inner.push_back(t);
    }
    if (inner.size() < 4 || vertices_of(c.m, inner).size() < 8) inner = r.tris;  // a narrow strip: all of it (its borders are settled)
    const auto pts = vertices_of(c.m, inner);
    if (pts.size() < 6) continue;
    const Prim fitted = refine(r.prim, pts, c.size);
    if (max_distance(fitted, pts) < max_distance(r.prim, pts) && max_distance(fitted, vertices_of(c.m, r.tris)) <= c.tol * 1.5) r.prim = fitted;
  }
  // Whatever no candidate kept (a curved one shrank below three triangles): planar facets.
  std::vector<int> rest;
  for (size_t t = 0; t < n; ++t)
    if (owner[t] < 0) rest.push_back(int(t));
  for (auto& group : planar_clusters(c, rest)) {
    Region r;
    r.prim = plane_of(c, group);
    r.flip = orientation(c, r.prim, group) < 0;
    r.tris = std::move(group);
    regions.push_back(std::move(r));
  }
  // Neighbours that are one surface (one fitted from each side of a crease it does not have) become one region.
  bool merged = true;
  while (merged) {
    merged = false;
    c.check();
    std::vector<int> of(n, -1);
    for (size_t r = 0; r < regions.size(); ++r)
      for (int t : regions[r].tris) of[size_t(t)] = int(r);
    std::set<std::pair<int, int>> pairs;
    for (size_t t = 0; t < n; ++t)
      for (int k = 0; k < 3; ++k) {
        const int nb = c.topo.across[t][size_t(k)];
        if (nb >= 0 && of[size_t(nb)] != of[t]) pairs.insert(std::minmax(of[t], of[size_t(nb)]));
      }
    for (const auto& [a, b] : pairs) {
      Region& ra = regions[size_t(a)];
      Region& rb = regions[size_t(b)];
      if (ra.tris.empty() || rb.tris.empty() || ra.flip != rb.flip || !ra.prim.same(rb.prim, c.tol, c.size)) continue;
      std::vector<int> both = ra.tris;
      both.insert(both.end(), rb.tris.begin(), rb.tris.end());
      const auto pts = vertices_of(c.m, both);
      Prim joined = ra.prim.type == Prim::Plane ? plane_of(c, both) : refine(ra.prim, pts, c.size);
      if (max_distance(joined, pts) > c.tol) continue;
      ra.prim = joined;
      ra.tris = std::move(both);
      rb.tris.clear();
      merged = true;
    }
    regions.erase(std::remove_if(regions.begin(), regions.end(), [](const Region& r) { return r.tris.empty(); }), regions.end());
  }
  return regions;
}

// ---------------------------------------------------------------- borders and faces

struct Chain {
  std::vector<int> verts;  // mesh vertices in order; first == last when closed
  int left = -1, right = -1;  // the region the chain runs forward in, the one on its other side (-1: open)
  bool closed = false;
  TopoDS_Edge edge;
};

struct HalfEdge {
  int t, k;
  int chain = -1;
  bool forward = true;
};

class Builder {
 public:
  Builder(const Context& c, std::vector<Region>& regions, bool fitted) : c(c), regions(regions), fitted(fitted) {}

  // The faces of every region; throws Error when a border or a face cannot be made.
  std::vector<TopoDS_Face> faces() {
    label();
    find_half_edges();
    find_chains();
    place_corners();
    for (auto& ch : chains) {
      c.check();
      make_edge(ch);
    }
    std::vector<TopoDS_Face> out;
    for (size_t r = 0; r < regions.size(); ++r) {
      c.check();
      out.push_back(make_face(int(r)));
    }
    return out;
  }

 private:
  const Context& c;
  std::vector<Region>& regions;
  bool fitted;
  std::vector<int> owner;
  std::vector<HalfEdge> half;
  std::map<std::pair<int, int>, int> halfAt;  // (triangle, side) -> half-edge
  std::vector<std::vector<int>> outgoing;     // per mesh vertex: boundary half-edges leaving it
  std::vector<char> corner;
  std::vector<Chain> chains;
  std::map<int, gp_Pnt> cornerAt;
  std::map<int, TopoDS_Vertex> vertexAt;

  int start(const HalfEdge& h) const { return c.m.triangles[size_t(h.t)][size_t(h.k)]; }
  int end(const HalfEdge& h) const { return c.m.triangles[size_t(h.t)][(size_t(h.k) + 1) % 3]; }
  int other(const HalfEdge& h) const {
    const int n = c.topo.across[size_t(h.t)][size_t(h.k)];
    return n < 0 ? -1 : owner[size_t(n)];
  }

  void label() {
    owner.assign(c.m.triangles.size(), -1);
    for (size_t r = 0; r < regions.size(); ++r)
      for (int t : regions[r].tris) owner[size_t(t)] = int(r);
    for (size_t r = 0; r < regions.size(); ++r) {
      auto& reg = regions[r];
      gp_XYZ mid(0, 0, 0);
      for (const auto& p : vertices_of(c.m, reg.tris)) mid += p.XYZ();
      const auto count = vertices_of(c.m, reg.tris).size();
      reg.surface = reg.prim.surface(gp_Pnt(mid / double(std::max<size_t>(1, count))));
    }
  }

  void find_half_edges() {
    outgoing.assign(c.m.points.size(), {});
    for (size_t t = 0; t < c.m.triangles.size(); ++t)
      for (int k = 0; k < 3; ++k) {
        const int n = c.topo.across[t][size_t(k)];
        if (n >= 0 && owner[size_t(n)] == owner[t]) continue;
        HalfEdge h{int(t), k};
        halfAt[{int(t), k}] = int(half.size());
        outgoing[size_t(start(h))].push_back(int(half.size()));
        half.push_back(h);
      }
    // A border vertex where more than two regions meet, or where a region touches itself, ends chains.
    corner.assign(c.m.points.size(), 0);
    for (size_t v = 0; v < c.m.points.size(); ++v) {
      const auto& out = outgoing[v];
      if (out.empty()) continue;
      bool simple = false;
      if (out.size() == 1) simple = other(half[size_t(out[0])]) < 0;
      if (out.size() == 2) {
        const auto& a = half[size_t(out[0])];
        const auto& b = half[size_t(out[1])];
        simple = other(a) >= 0 && other(a) == owner[size_t(b.t)] && other(b) == owner[size_t(a.t)] && owner[size_t(a.t)] != owner[size_t(b.t)];
      }
      corner[v] = !simple;
    }
  }

  // The border half-edge of the same region that follows h (round its end vertex through the region's triangles).
  int next(int index) const {
    const HalfEdge& h = half[size_t(index)];
    const int region = owner[size_t(h.t)];
    const int b = end(h);
    int t = h.t;
    for (int guard = 0; guard < 1000; ++guard) {
      int j = -1;
      for (int k = 0; k < 3; ++k)
        if (c.m.triangles[size_t(t)][size_t(k)] == b) j = k;
      const int n = c.topo.across[size_t(t)][size_t(j)];
      if (n < 0 || owner[size_t(n)] != region) {
        auto it = halfAt.find({t, j});
        if (it == halfAt.end()) throw Error("a border of the mesh could not be followed");
        return it->second;
      }
      t = n;
    }
    throw Error("a border of the mesh could not be followed");
  }

  int twin(int index) const {
    const HalfEdge& h = half[size_t(index)];
    const int n = c.topo.across[size_t(h.t)][size_t(h.k)];
    if (n < 0) return -1;
    const int k = c.topo.side_of(n, end(h), start(h));
    if (k < 0) return -1;
    auto it = halfAt.find({n, k});
    return it == halfAt.end() ? -1 : it->second;
  }

  void find_chains() {
    // Walked in the region with the lower number (or the only one): the twin half-edges run it backwards.
    auto walk = [&](int first, bool loop) {
      Chain ch;
      ch.left = owner[size_t(half[size_t(first)].t)];
      ch.right = other(half[size_t(first)]);
      ch.closed = loop;
      const int id = int(chains.size());
      int at = first;
      ch.verts.push_back(start(half[size_t(at)]));
      for (int guard = 0; guard <= int(half.size()); ++guard) {
        half[size_t(at)].chain = id;
        half[size_t(at)].forward = true;
        if (const int tw = twin(at); tw >= 0) {
          half[size_t(tw)].chain = id;
          half[size_t(tw)].forward = false;
        }
        const int e = end(half[size_t(at)]);
        ch.verts.push_back(e);
        if (corner[size_t(e)] || (loop && e == ch.verts.front())) break;
        at = next(at);
        if (half[size_t(at)].chain >= 0) break;
      }
      chains.push_back(std::move(ch));
    };
    auto owned = [&](const HalfEdge& h) { return other(h) < 0 || owner[size_t(h.t)] < other(h); };
    for (size_t i = 0; i < half.size(); ++i)
      if (half[i].chain < 0 && owned(half[i]) && corner[size_t(start(half[i]))]) walk(int(i), false);
    for (size_t i = 0; i < half.size(); ++i)
      if (half[i].chain < 0 && owned(half[i])) walk(int(i), true);
    for (const auto& h : half)
      if (h.chain < 0) throw Error("a border between two surfaces is one-sided");
  }

  // A corner where three or more surfaces meet: the point nearest all of them, close to the mesh's vertex.
  gp_Pnt settle(const gp_Pnt& v, const std::vector<int>& around) const {
    if (!fitted || around.empty()) return v;
    Eigen::Vector3d x(v.X(), v.Y(), v.Z());
    const Eigen::Vector3d v0 = x;
    const double lambda = 1e-4;
    for (int it = 0; it < 12; ++it) {
      Eigen::Matrix3d A = Eigen::Matrix3d::Identity() * lambda;
      Eigen::Vector3d b = v0 * lambda;
      const gp_Pnt p(x(0), x(1), x(2));
      for (int r : around) {
        const Prim& prim = regions[size_t(r)].prim;
        const gp_Pnt q = prim.project(p);
        const gp_Dir nd = prim.normal(p);
        const Eigen::Vector3d n(nd.X(), nd.Y(), nd.Z()), qq(q.X(), q.Y(), q.Z());
        A += n * n.transpose();
        b += n * n.dot(qq);
      }
      const Eigen::Vector3d y = A.ldlt().solve(b);
      if (!y.allFinite()) return v;
      if ((y - x).norm() < 1e-12 * std::max(1.0, c.size)) {
        x = y;
        break;
      }
      x = y;
    }
    const gp_Pnt out(x(0), x(1), x(2));
    return out.Distance(v) <= 4 * c.tol ? out : v;
  }

  void place_corners() {
    for (size_t v = 0; v < c.m.points.size(); ++v) {
      if (!corner[v]) continue;
      std::set<int> around;
      for (int i : outgoing[v]) {
        around.insert(owner[size_t(half[size_t(i)].t)]);
        if (other(half[size_t(i)]) >= 0) around.insert(other(half[size_t(i)]));
      }
      cornerAt[int(v)] = settle(c.m.points[v], std::vector<int>(around.begin(), around.end()));
    }
    for (auto& ch : chains)
      if (ch.closed) {
        std::vector<int> around{ch.left};
        if (ch.right >= 0) around.push_back(ch.right);
        cornerAt[ch.verts.front()] = settle(c.m.points[size_t(ch.verts.front())], around);
      }
  }

  // The curve's parameters at the chain's ends, running the chain's way (the curve reversed when it runs the other way).
  bool trim(Handle(Geom_Curve)& curve, const std::vector<gp_Pnt>& pts, bool closed, double& u0, double& u1, double& gap) const {
    auto param = [&](const Handle(Geom_Curve)& cv, const gp_Pnt& p, double& u, double& d) {
      GeomAPI_ProjectPointOnCurve proj(p, cv);
      if (proj.NbPoints() == 0) return false;
      u = proj.LowerDistanceParameter();
      d = proj.LowerDistance();
      return true;
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
      double d0 = 0, d1 = 0, dm = 0, um = 0;
      if (!param(curve, pts.front(), u0, d0)) return false;
      const gp_Pnt middle = closed ? pts[1] : pts.size() >= 3 ? pts[pts.size() / 2] : gp_Pnt((pts.front().XYZ() + pts.back().XYZ()) / 2);
      if (!param(curve, middle, um, dm)) return false;
      if (closed) {
        u1 = u0;
        d1 = d0;
      } else if (!param(curve, pts.back(), u1, d1)) {
        return false;
      }
      gap = std::max(d0, d1);
      bool forward;
      if (curve->IsPeriodic()) {
        const double T = curve->Period();
        auto after = [&](double u) {
          while (u < u0) u += T;
          while (u >= u0 + T) u -= T;
          return u;
        };
        um = after(um);
        if (closed) {
          forward = um - u0 < T / 2;
          u1 = u0 + T;
        } else {
          u1 = after(u1);
          if (u1 - u0 < 1e-9) u1 += T;
          forward = um < u1;
        }
      } else {
        if (closed) {
          // A closed curve that is not periodic: usable when it starts and ends at the chain's start.
          if (curve->Value(curve->FirstParameter()).Distance(pts.front()) > c.tol || curve->Value(curve->LastParameter()).Distance(pts.front()) > c.tol)
            return false;
          u0 = curve->FirstParameter();
          u1 = curve->LastParameter();
          forward = um - u0 < (u1 - u0) / 2;
        } else {
          forward = u0 < um && um < u1;
          if (!forward && !(u1 < um && um < u0)) return false;
        }
      }
      if (forward) return u1 - u0 > 1e-12;
      if (attempt == 1) return false;
      curve = curve->Reversed();
    }
    return false;
  }

  double deviation(const Handle(Geom_Curve)& curve, const std::vector<gp_Pnt>& pts) const {
    double worst = 0;
    const size_t step = std::max<size_t>(1, pts.size() / 40);
    for (size_t i = 0; i < pts.size(); i += step) {
      GeomAPI_ProjectPointOnCurve proj(pts[i], curve);
      if (proj.NbPoints() == 0) return 1e300;
      worst = std::max(worst, proj.LowerDistance());
    }
    return worst;
  }

  Handle(Geom_Curve) polyline(const std::vector<gp_Pnt>& pts) const {
    const int n = int(pts.size());
    TColgp_Array1OfPnt poles(1, n);
    TColStd_Array1OfReal knots(1, n);
    TColStd_Array1OfInteger mults(1, n);
    double s = 0;
    for (int i = 0; i < n; ++i) {
      if (i) s += std::max(1e-9, pts[size_t(i)].Distance(pts[size_t(i - 1)]));
      poles.SetValue(i + 1, pts[size_t(i)]);
      knots.SetValue(i + 1, s);
      mults.SetValue(i + 1, i == 0 || i == n - 1 ? 2 : 1);
    }
    return new Geom_BSplineCurve(poles, knots, mults, 1);
  }

  void make_edge(Chain& ch) {
    std::vector<gp_Pnt> pts;
    for (int v : ch.verts) pts.push_back(c.m.points[size_t(v)]);
    if (ch.closed) pts.back() = pts.front() = cornerAt[ch.verts.front()];
    else {
      pts.front() = cornerAt[ch.verts.front()];
      pts.back() = cornerAt[ch.verts.back()];
    }
    if (!ch.closed && pts.front().Distance(pts.back()) < 1e-12 && pts.size() <= 2) throw Error("a border collapsed to a point");
    const double limit = 1.5 * c.tol;
    std::vector<Handle(Geom_Curve)> tries;
    if (fitted && ch.right >= 0) {
      try {
        GeomAPI_IntSS inter(regions[size_t(ch.left)].surface, regions[size_t(ch.right)].surface, c.tol * 0.1);
        if (inter.IsDone())
          for (int i = 1; i <= inter.NbLines(); ++i) tries.push_back(inter.Line(i));
      } catch (const Standard_Failure&) {
      }
      std::sort(tries.begin(), tries.end(), [&](const auto& a, const auto& b) { return deviation(a, pts) < deviation(b, pts); });
    }
    // A straight border.
    if (!ch.closed) {
      const gp_Vec d(pts.front(), pts.back());
      if (d.Magnitude() > 1e-9) {
        const gp_Lin line(pts.front(), gp_Dir(d));
        bool straight = true;
        for (const auto& p : pts) straight &= line.Distance(p) <= c.tol;
        if (straight) tries.push_back(new Geom_Line(line));
      }
    }
    // A circle or an arc.
    if (fitted && pts.size() >= 5) {
      if (auto plane = fit_plane(pts); plane && max_distance(*plane, pts) <= c.tol) {
        const gp_Dir x = std::abs(plane->a.X()) < 0.6 ? gp_Dir(plane->a.Crossed(gp_Dir(1, 0, 0))) : gp_Dir(plane->a.Crossed(gp_Dir(0, 1, 0)));
        const gp_Dir y = plane->a.Crossed(x);
        std::vector<std::array<double, 2>> flat;
        for (const auto& p : pts) flat.push_back({gp_Vec(plane->o, p).Dot(gp_Vec(x)), gp_Vec(plane->o, p).Dot(gp_Vec(y))});
        double cx, cy, r;
        if (fit_circle_2d(flat, cx, cy, r) && r < c.size * 50) {
          const gp_Pnt centre = plane->o.Translated(gp_Vec(x) * cx + gp_Vec(y) * cy);
          tries.push_back(new Geom_Circle(gp_Ax2(centre, plane->a, x), r));
        }
      }
    }
    if (fitted && pts.size() >= 4 && !ch.closed) {
      TColgp_Array1OfPnt arr(1, int(pts.size()));
      for (size_t i = 0; i < pts.size(); ++i) arr.SetValue(int(i) + 1, pts[i]);
      try {
        GeomAPI_PointsToBSpline fit(arr, 3, 8, GeomAbs_C2, c.tol * 0.5);
        if (fit.IsDone()) tries.push_back(fit.Curve());
      } catch (const Standard_Failure&) {
      }
    }
    tries.push_back(polyline(pts));
    for (auto curve : tries) {
      if (curve.IsNull()) continue;
      const bool exact = curve->DynamicType() == STANDARD_TYPE(Geom_BSplineCurve) && Handle(Geom_BSplineCurve)::DownCast(curve)->Degree() == 1;
      if (!exact && deviation(curve, pts) > limit) continue;
      double u0, u1, gap;
      try {
        if (!trim(curve, pts, ch.closed, u0, u1, gap) || gap > limit) continue;
      } catch (const Standard_Failure&) {
        continue;
      }
      BRep_Builder B;
      TopoDS_Vertex v0 = vertex(ch.verts.front()), v1 = ch.closed ? v0 : vertex(ch.verts.back());
      TopoDS_Edge e;
      B.MakeEdge(e, curve, std::max(gap, Precision::Confusion()));
      B.Add(e, v0.Oriented(TopAbs_FORWARD));
      B.Add(e, v1.Oriented(TopAbs_REVERSED));
      B.Range(e, u0, u1);
      B.UpdateVertex(v0, std::max(BRep_Tool::Tolerance(v0), curve->Value(u0).Distance(BRep_Tool::Pnt(v0)) * 1.01 + Precision::Confusion()));
      B.UpdateVertex(v1, std::max(BRep_Tool::Tolerance(v1), curve->Value(u1).Distance(BRep_Tool::Pnt(v1)) * 1.01 + Precision::Confusion()));
      ch.edge = e;
      return;
    }
    throw Error("a border between two surfaces could not be made a curve");
  }

  TopoDS_Vertex vertex(int v) {
    auto it = vertexAt.find(v);
    if (it != vertexAt.end()) return it->second;
    TopoDS_Vertex out;
    BRep_Builder().MakeVertex(out, cornerAt.count(v) ? cornerAt[v] : c.m.points[size_t(v)], Precision::Confusion());
    vertexAt[v] = out;
    return out;
  }

  TopoDS_Face make_face(int r) {
    Region& reg = regions[size_t(r)];
    // The region's border loops, as chains with their direction.
    std::vector<std::vector<std::pair<int, bool>>> loops;
    std::set<int> seen;
    for (size_t i = 0; i < half.size(); ++i) {
      if (owner[size_t(half[i].t)] != r || seen.count(int(i))) continue;
      std::vector<int> ring;
      int at = int(i);
      while (!seen.count(at)) {
        seen.insert(at);
        ring.push_back(at);
        at = next(at);
      }
      if (at != int(i)) throw Error("a face's border does not close");
      // One entry per chain: a closed chain is the whole ring; an open one starts at its first corner.
      auto begins = [&](const HalfEdge& h) {
        const Chain& ch = chains[size_t(h.chain)];
        return start(h) == (h.forward ? ch.verts.front() : ch.verts.back());
      };
      std::vector<std::pair<int, bool>> loop;
      const HalfEdge& h0 = half[size_t(ring.front())];
      if (chains[size_t(h0.chain)].closed) {
        loop.push_back({h0.chain, h0.forward});
      } else {
        size_t first = ring.size();
        for (size_t k = 0; k < ring.size() && first == ring.size(); ++k)
          if (begins(half[size_t(ring[k])])) first = k;
        if (first == ring.size()) throw Error("a face's border does not close");
        for (size_t k = 0; k < ring.size(); ++k) {
          const HalfEdge& h = half[size_t(ring[(first + k) % ring.size()])];
          if (begins(h)) loop.push_back({h.chain, h.forward});
        }
      }
      loops.push_back(std::move(loop));
    }
    BRep_Builder B;
    TopoDS_Face face;
    if (loops.empty()) {
      BRepBuilderAPI_MakeFace natural(reg.surface, Precision::Confusion());
      if (!natural.IsDone()) throw Error("a closed surface could not be made a face");
      face = natural.Face();
    } else {
      B.MakeFace(face, reg.surface, Precision::Confusion());
      for (auto loop : loops) {
        if (reg.flip) std::reverse(loop.begin(), loop.end());
        TopoDS_Wire w;
        B.MakeWire(w);
        for (const auto& [chain, forward] : loop) {
          const bool along = reg.flip ? !forward : forward;
          B.Add(w, chains[size_t(chain)].edge.Oriented(along ? TopAbs_FORWARD : TopAbs_REVERSED));
        }
        w.Closed(true);
        B.Add(face, w);
      }
    }
    ShapeFix_Face fix(face);
    fix.SetPrecision(Precision::Confusion());
    fix.SetMaxTolerance(c.tol * 10);
    fix.Perform();
    face = fix.Face();
    if (reg.flip) face.Reverse();
    return face;
  }
};

TopoDS_Shape sew(const std::vector<TopoDS_Face>& faces, double tol, bool closedMesh) {
  BRepBuilderAPI_Sewing sewing(tol);
  for (const auto& f : faces) sewing.Add(f);
  sewing.Perform();
  const TopoDS_Shape sewn = sewing.SewedShape();
  BRep_Builder B;
  TopoDS_Compound out;
  B.MakeCompound(out);
  int solids = 0, pieces = 0;
  for (TopExp_Explorer s(sewn, TopAbs_SHELL); s.More(); s.Next()) {
    const TopoDS_Shell shell = TopoDS::Shell(s.Current());
    ++pieces;
    if (closedMesh && BRep_Tool::IsClosed(shell)) {
      const TopoDS_Solid solid = ShapeFix_Solid().SolidFromShell(shell);
      if (!solid.IsNull()) {
        B.Add(out, solid);
        ++solids;
        continue;
      }
    }
    B.Add(out, shell);
  }
  for (TopExp_Explorer f(sewn, TopAbs_FACE, TopAbs_SHELL); f.More(); f.Next()) {
    // A face closed on itself (a whole sphere or torus) is a shell of its own.
    TopoDS_Shell shell;
    B.MakeShell(shell);
    B.Add(shell, f.Current());
    ++pieces;
    if (closedMesh && BRep_Tool::IsClosed(shell)) {
      shell.Closed(true);
      const TopoDS_Solid solid = ShapeFix_Solid().SolidFromShell(shell);
      if (!solid.IsNull()) {
        B.Add(out, solid);
        ++solids;
        continue;
      }
    }
    B.Add(out, f.Current());
  }
  if (pieces == 0) throw Error("the faces could not be sewn");
  TopoDS_Shape result = out;
  if (pieces == 1) {
    TopoDS_Iterator it(out);
    result = it.Value();
  }
  if (closedMesh && solids != pieces) throw Error("the faces do not close into a solid");
  return result;
}

bool usable(const TopoDS_Shape& shape, const TriMesh& mesh, bool closedMesh) {
  if (shape.IsNull() || !BRepCheck_Analyzer(shape).IsValid()) return false;
  if (!closedMesh) return true;
  GProp_GProps g;
  BRepGProp::VolumeProperties(shape, g);
  const double v = mesh.volume();
  return g.Mass() > 0 && std::abs(g.Mass() - v) <= 0.2 * v;
}

}  // namespace

MeshBrep mesh_to_brep(const TriMesh& mesh, const MeshSolidOptions& options, const std::function<bool()>& cancelled) {
  if (mesh.empty()) throw Error("the mesh has no triangles");
  const Topology topo(mesh);
  for (const auto& a : topo.across)
    for (int n : a)
      if (n == -2) throw Error("the mesh has edges shared by more than two triangles; repair it first");
  const double tol = options.tolerance > 0 ? options.tolerance : mesh_tolerance(mesh);
  const double angle = std::clamp(options.angle, 5.0, 80.0) * M_PI / 180;
  const Context c{mesh, topo, tol, angle, std::max(mesh.diagonal(), tol), cancelled, mesh_sagitta(topo, angle), std::vector<int>(mesh.triangles.size(), 0)};
  const bool closedMesh = mesh.open_edges() == 0;
  MeshBrep out;
  auto count = [](const std::vector<Region>& regions, bool facets) {
    json j = json::object();
    for (const auto& r : regions) {
      const std::string kind = facets ? "facet" : r.prim.name();
      j[kind] = j.value(kind, 0) + 1;
    }
    return j;
  };
  auto attempt = [&](std::vector<Region> regions, bool fitted) -> bool {
    if (debugging()) {
      std::fprintf(stderr, "mesh_solid: %s attempt, %zu regions:", fitted ? "fitted" : "faceted", regions.size());
      if (regions.size() < 200)
        for (const auto& r : regions) std::fprintf(stderr, " %s(%zu%s r=%g)", r.prim.name(), r.tris.size(), r.flip ? ",flip" : "", r.prim.r);
      std::fprintf(stderr, "\n");
    }
    try {
      Builder builder(c, regions, fitted);
      const auto faces = builder.faces();
      TopoDS_Shape shape = sew(faces, tol * 2, closedMesh);
      if (!BRepCheck_Analyzer(shape).IsValid()) {
        ShapeFix_Shape fix(shape);
        fix.SetPrecision(Precision::Confusion());
        fix.SetMaxTolerance(tol * 10);
        fix.Perform();
        shape = fix.Shape();
      }
      if (!usable(shape, mesh, closedMesh)) {
        if (debugging()) std::fprintf(stderr, "mesh_solid: not usable (valid %d)\n", int(BRepCheck_Analyzer(shape).IsValid()));
        return false;
      }
      out.shape = shape;
      out.surfaces = count(regions, !fitted);
      out.faceted = !fitted;
      return true;
    } catch (const Error& e) {
      if (std::string(e.what()) == "cancelled") throw;
      if (debugging()) std::fprintf(stderr, "mesh_solid: failed: %s\n", e.what());
      return false;
    } catch (const Standard_Failure& e) {
      if (debugging()) std::fprintf(stderr, "mesh_solid: OCCT failure: %s\n", e.GetMessageString());
      return false;
    }
  };
  if (attempt(segment(c), true)) return out;
  c.check();
  // Planar facets: coplanar triangles merged, every border straight between its corners.
  std::vector<int> all(mesh.triangles.size());
  std::iota(all.begin(), all.end(), 0);
  std::vector<Region> flat;
  for (auto& group : planar_clusters(c, all)) {
    Region r;
    r.prim = plane_of(c, group);
    r.flip = orientation(c, r.prim, group) < 0;
    r.tris = std::move(group);
    flat.push_back(std::move(r));
  }
  if (attempt(flat, false)) return out;
  c.check();
  // One face per triangle: always closes for a closed, consistently wound mesh.
  std::vector<TopoDS_Face> faces;
  std::vector<TopoDS_Vertex> vertices(mesh.points.size());
  for (size_t i = 0; i < mesh.points.size(); ++i) BRep_Builder().MakeVertex(vertices[i], mesh.points[i], Precision::Confusion());
  for (const auto& t : mesh.triangles) {
    BRepBuilderAPI_MakePolygon poly(vertices[size_t(t[0])], vertices[size_t(t[1])], vertices[size_t(t[2])], true);
    if (!poly.IsDone()) continue;
    BRepBuilderAPI_MakeFace face(poly.Wire(), true);
    if (face.IsDone()) faces.push_back(face.Face());
  }
  const TopoDS_Shape shape = sew(faces, tol, closedMesh);
  if (!BRepCheck_Analyzer(shape).IsValid()) throw Error("the mesh could not be made a valid solid; check that it is closed and does not cross itself");
  out.shape = shape;
  out.surfaces = {{"facet", int(faces.size())}};
  out.faceted = true;
  return out;
}

}  // namespace opad
