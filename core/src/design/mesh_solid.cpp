// Mesh to solid as design ops (design/mesh_solid.hpp): extrusions and revolutions found in a mesh become a sketch and a
// feature; anything else the "mesh_solid" feature (the general B-rep of opad/mesh_solid.hpp). Every result is measured
// against the mesh before it is offered.
#include "opad/design/mesh_solid.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <GeomAPI_PointsToBSpline.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <Eigen/Dense>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "../meshsolid/meshsolid.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"

namespace opad::design {
using meshsolid::Topology;
using P2 = std::array<double, 2>;

int MeshProfile::curves() const {
  int n = 0;
  for (const auto& e : sketch.entities) n += !e.construction && e.type != SkEntity::Type::Point;
  return n;
}

namespace {

bool debugging() {
  static const bool on = std::getenv("OPAD_MESHSOLID_DEBUG") != nullptr;
  return on;
}
#define MS_FAIL(why)                                                                   \
  do {                                                                                 \
    if (debugging()) std::fprintf(stderr, "mesh_solid: %s: %s\n", __func__, why); \
    return std::nullopt;                                                               \
  } while (0)

double cross(const P2& a, const P2& b) { return a[0] * b[1] - a[1] * b[0]; }
P2 sub(const P2& a, const P2& b) { return {a[0] - b[0], a[1] - b[1]}; }
double len(const P2& a) { return std::hypot(a[0], a[1]); }

double segment_distance(const P2& p, const P2& a, const P2& b) {
  const P2 ab = sub(b, a), ap = sub(p, a);
  const double l2 = ab[0] * ab[0] + ab[1] * ab[1];
  const double t = l2 > 0 ? std::clamp((ap[0] * ab[0] + ap[1] * ab[1]) / l2, 0.0, 1.0) : 0.0;
  return len({ap[0] - ab[0] * t, ap[1] - ab[1] * t});
}

double wrap(double a) {
  while (a > M_PI) a -= 2 * M_PI;
  while (a <= -M_PI) a += 2 * M_PI;
  return a;
}

std::string mm(double v) {
  std::ostringstream s;
  s.precision(12);
  s << v << " mm";
  return s.str();
}

// ---------------------------------------------------------------- 2D fitting

struct Piece {
  enum Kind { Line, Arc, Circle, Spline } kind = Line;
  size_t a = 0, b = 0;  // unrolled vertex indices (b > a)
  double cx = 0, cy = 0, r = 0;
  bool ccw = true;
  Handle(Geom_BSplineCurve) spline;
};

class ProfileFitter {
 public:
  ProfileFitter(const std::vector<P2>& pts, bool closed, double tol, double maxTurn, double chord)
      : pts(pts), closed(closed), tol(tol), maxTurn(maxTurn), chord(std::max(chord, tol)) {
    for (const auto& p : pts) size = std::max(size, std::max(std::abs(p[0]), std::abs(p[1])));
  }

  std::vector<Piece> run() {
    const size_t n = pts.size();
    std::vector<size_t> corners;
    if (closed) {
      for (size_t i = 0; i < n; ++i)
        if (std::abs(turn(i)) > maxTurn) corners.push_back(i);
      if (corners.empty()) {
        Piece c;
        if (n >= 6 && arc(0, n, c, true)) {
          c.kind = Piece::Circle;
          return {c};
        }
        corners = {0, n / 2};
      }
    } else {
      corners.push_back(0);
      for (size_t i = 1; i + 1 < n; ++i)
        if (std::abs(turn(i)) > maxTurn) corners.push_back(i);
      corners.push_back(n - 1);
    }
    std::vector<Piece> out;
    const size_t m = closed ? corners.size() : corners.size() - 1;
    for (size_t k = 0; k < m; ++k) {
      const size_t s = corners[k], e = k + 1 < corners.size() ? corners[k + 1] : corners[0] + n;
      auto piece = segment(s, e);
      out.insert(out.end(), piece.begin(), piece.end());
    }
    return out;
  }

  const P2& at(size_t i) const { return pts[i % pts.size()]; }

 private:
  const std::vector<P2>& pts;
  bool closed;
  double tol, maxTurn, chord, size = 1;

  double turn(size_t i) const {
    const size_t n = pts.size();
    const P2& prev = pts[(i + n - 1) % n];
    const P2& next = pts[(i + 1) % n];
    const P2 u = sub(pts[i], prev), v = sub(next, pts[i]);
    return std::atan2(cross(u, v), u[0] * v[0] + u[1] * v[1]);
  }

  bool line(size_t i, size_t j) const {
    const P2 &a = at(i), &b = at(j);
    if (len(sub(b, a)) < 1e-12) return false;
    for (size_t k = i + 1; k < j; ++k)
      if (segment_distance(at(k), a, b) > tol) return false;
    return true;
  }

  // Points i..j (unrolled; j == i + n for a whole loop) on one circle, turning one way in steps no larger than a facet's.
  bool arc(size_t i, size_t j, Piece& out, bool whole = false) const {
    if (j < i + 3) return false;
    std::vector<P2> run;
    for (size_t k = i; k <= j; ++k) run.push_back(at(k));
    if (whole) run.pop_back();
    double cx, cy, r;
    if (!meshsolid::fit_circle_2d(run, cx, cy, r) || r > size * 1e3 + 1e3) return false;
    if (!whole) {
      // The centre on the ends' bisector, so both ends are on the arc exactly (the sketch joins its curves there).
      const P2 &a = run.front(), &b = run.back();
      const P2 mid{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2}, chord = sub(b, a);
      const double l = len(chord);
      if (l < 1e-12) return false;
      const P2 n{-chord[1] / l, chord[0] / l};
      const double along = (cx - mid[0]) * n[0] + (cy - mid[1]) * n[1];
      cx = mid[0] + n[0] * along;
      cy = mid[1] + n[1] * along;
      r = len(sub(a, {cx, cy}));
    }
    double sweep = 0, sign = 0;
    for (size_t k = 0; k < run.size(); ++k) {
      if (std::abs(std::hypot(run[k][0] - cx, run[k][1] - cy) - r) > tol) return false;
      if (k + 1 == run.size() && !whole) break;
      const P2& p = run[k];
      const P2& q = run[(k + 1) % run.size()];
      const double step = wrap(std::atan2(q[1] - cy, q[0] - cx) - std::atan2(p[1] - cy, p[0] - cx));
      if (std::abs(step) > maxTurn * 1.25 || std::abs(step) < 1e-12) return false;
      // A chord cuts inside its arc by its sagitta; one deeper than the mesh's facets ever do is a straight side.
      if (r * (1 - std::cos(step / 2)) > chord * 1.5) return false;
      if (sign == 0) sign = step > 0 ? 1 : -1;
      if (step * sign < 0) return false;
      sweep += step;
    }
    if (!whole && std::abs(sweep) >= 2 * M_PI - 1e-6) return false;
    if (whole && std::abs(std::abs(sweep) - 2 * M_PI) > 1e-3) return false;
    // The chords between the points lie inside the circle by their sagitta; past it they are not this circle.
    out.kind = Piece::Arc;
    out.a = i;
    out.b = j;
    out.cx = cx;
    out.cy = cy;
    out.r = r;
    out.ccw = sweep > 0;
    return true;
  }

  // The furthest end from i that `fits` (galloping, then halving).
  template <class Fits>
  size_t reach(size_t i, size_t first, size_t e, Fits fits) const {
    if (first > e || !fits(first)) return 0;
    size_t good = first, step = 1;
    while (good + step <= e && fits(good + step)) {
      good += step;
      step *= 2;
    }
    size_t lo = good, hi = std::min(e + 1, good + step);
    while (hi - lo > 1) {
      const size_t mid = (lo + hi) / 2;
      if (fits(mid)) lo = mid;
      else hi = mid;
    }
    return lo;
  }

  std::vector<Piece> segment(size_t s, size_t e) const {
    std::vector<Piece> out;
    size_t i = s;
    while (i < e) {
      const size_t jl = std::max(i + 1, reach(i, i + 1, e, [&](size_t j) { return line(i, j); }));
      Piece a;
      const size_t ja = reach(i, i + 3, e, [&](size_t j) { return arc(i, j, a); });
      if (ja > jl && arc(i, ja, a)) {
        out.push_back(a);
        i = ja;
      } else {
        Piece l;
        l.a = i;
        l.b = jl;
        out.push_back(l);
        i = jl;
      }
    }
    // Arcs of one circle next to each other: one arc.
    std::vector<Piece> merged;
    for (const auto& p : out) {
      if (!merged.empty() && merged.back().kind == Piece::Arc && p.kind == Piece::Arc && merged.back().ccw == p.ccw &&
          std::hypot(merged.back().cx - p.cx, merged.back().cy - p.cy) <= tol && std::abs(merged.back().r - p.r) <= tol) {
        Piece joined;
        if (arc(merged.back().a, p.b, joined)) {
          merged.back() = joined;
          continue;
        }
      }
      merged.push_back(p);
    }
    // Smooth runs of single segments that no line or arc holds: one spline through their points.
    std::vector<Piece> final;
    for (size_t k = 0; k < merged.size();) {
      size_t end = k;
      while (end < merged.size() && merged[end].kind == Piece::Line && merged[end].b == merged[end].a + 1) ++end;
      if (end - k >= 4) {
        if (auto sp = spline(merged[k].a, merged[end - 1].b)) {
          final.push_back(*sp);
          k = end;
          continue;
        }
      }
      if (end == k) end = k + 1;
      for (; k < end; ++k) final.push_back(merged[k]);
    }
    return final;
  }

  std::optional<Piece> spline(size_t i, size_t j) const {
    const int n = int(j - i + 1);
    TColgp_Array1OfPnt arr(1, n);
    for (int k = 0; k < n; ++k) arr.SetValue(k + 1, gp_Pnt(at(i + size_t(k))[0], at(i + size_t(k))[1], 0));
    try {
      GeomAPI_PointsToBSpline fit(arr, 3, 8, GeomAbs_C2, tol * 0.5);
      if (!fit.IsDone()) return std::nullopt;
      Handle(Geom_BSplineCurve) c = fit.Curve();
      c->SetPole(1, arr.Value(1));
      c->SetPole(c->NbPoles(), arr.Value(n));
      for (int k = 1; k <= n; ++k) {
        GeomAPI_ProjectPointOnCurve proj(arr.Value(k), c);
        if (proj.NbPoints() == 0 || proj.LowerDistance() > tol) return std::nullopt;
      }
      Piece p;
      p.kind = Piece::Spline;
      p.a = i;
      p.b = j;
      p.spline = c;
      return p;
    } catch (const Standard_Failure&) {
      return std::nullopt;
    }
  }
};

// ---------------------------------------------------------------- recognition

gp_Dir across_of(const gp_Dir& d) {
  // The world axis least along d, made square to it: the sketch's x.
  const gp_Dir axes[] = {gp_Dir(1, 0, 0), gp_Dir(0, 1, 0), gp_Dir(0, 0, 1)};
  int best = 0;
  for (int i = 1; i < 3; ++i)
    if (std::abs(axes[i].Dot(d)) < std::abs(axes[best].Dot(d))) best = i;
  const gp_Vec x = gp_Vec(axes[best]) - gp_Vec(d) * axes[best].Dot(d);
  return gp_Dir(x);
}

Vec3 vec3(const gp_XYZ& v) { return {v.X(), v.Y(), v.Z()}; }

// Directions the mesh's facets face most (area-weighted, opposite ones merged), largest first.
std::vector<gp_Dir> facing_directions(const Topology& topo, size_t most) {
  std::vector<int> order(topo.area.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
  std::sort(order.begin(), order.end(), [&](int a, int b) { return topo.area[size_t(a)] > topo.area[size_t(b)]; });
  std::vector<std::pair<gp_Dir, double>> groups;
  const double same = std::cos(1.0 * M_PI / 180);
  for (int t : order) {
    const gp_Vec& n = topo.normal[size_t(t)];
    bool found = false;
    for (auto& g : groups)
      if (std::abs(n.Dot(gp_Vec(g.first))) > same) {
        g.second += topo.area[size_t(t)];
        found = true;
        break;
      }
    if (!found && groups.size() < 200) groups.push_back({gp_Dir(n), topo.area[size_t(t)]});
  }
  std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::vector<gp_Dir> out;
  for (size_t i = 0; i < groups.size() && out.size() < most; ++i) out.push_back(groups[i].first);
  return out;
}

void add_direction(std::vector<gp_Dir>& dirs, const gp_Dir& d) {
  for (const auto& e : dirs)
    if (std::abs(e.Dot(d)) > std::cos(0.5 * M_PI / 180)) return;
  dirs.push_back(d);
}

// Closed loops of directed border edges (a vertex with two leaving: nullopt).
std::optional<std::vector<std::vector<int>>> border_loops(const TriMesh& m, const std::vector<int>& tris) {
  std::set<std::pair<int, int>> edges;
  for (int t : tris)
    for (int k = 0; k < 3; ++k) edges.insert({m.triangles[size_t(t)][size_t(k)], m.triangles[size_t(t)][(size_t(k) + 1) % 3]});
  std::map<int, int> next;
  for (const auto& [a, b] : edges)
    if (!edges.count({b, a})) {
      if (next.count(a)) return std::nullopt;
      next[a] = b;
    }
  std::vector<std::vector<int>> loops;
  std::set<int> used;
  for (const auto& [start, unused] : next) {
    if (used.count(start)) continue;
    std::vector<int> loop;
    int at = start;
    while (!used.count(at)) {
      used.insert(at);
      loop.push_back(at);
      auto it = next.find(at);
      if (it == next.end()) return std::nullopt;
      at = it->second;
    }
    if (at != start || loop.size() < 3) return std::nullopt;
    loops.push_back(std::move(loop));
  }
  return loops;
}

bool inside_triangles(const std::vector<std::array<P2, 3>>& tris, const P2& p) {
  for (const auto& t : tris) {
    const double d1 = cross(sub(t[1], t[0]), sub(p, t[0])), d2 = cross(sub(t[2], t[1]), sub(p, t[1])), d3 = cross(sub(t[0], t[2]), sub(p, t[2]));
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    if (!(neg && pos)) return true;
  }
  return false;
}

bool inside_loops(const std::vector<std::vector<P2>>& loops, const P2& p) {
  bool in = false;
  for (const auto& loop : loops)
    for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++)
      if ((loop[i][1] > p[1]) != (loop[j][1] > p[1]) && p[0] < (loop[j][0] - loop[i][0]) * (p[1] - loop[i][1]) / (loop[j][1] - loop[i][1]) + loop[i][0]) in = !in;
  return in;
}

// Sketch curves for the loops, then the sketch's regions that `material` says are solid.
bool finish(MeshProfile& out, const std::vector<std::vector<P2>>& loops, double tol, double maxTurn, double chord,
            const std::function<bool(const P2&)>& material) {
  for (const auto& loop : loops) fit_profile(out.sketch, loop, true, tol, maxTurn, chord);

  try {
    out.sketch.validate();
    for (const auto& r : sketch_regions(out.sketch, out.frame)) {
      if (material({r.u, r.v})) out.regions.push_back({r.u, r.v});
    }
  } catch (const std::exception& e) {
    if (debugging()) std::fprintf(stderr, "mesh_solid: profile: %s\n", e.what());
    return false;
  } catch (const Standard_Failure& e) {
    if (debugging()) std::fprintf(stderr, "mesh_solid: profile: %s\n", e.GetMessageString());
    return false;
  }
  if (debugging() && out.regions.empty()) std::fprintf(stderr, "mesh_solid: profile: no region is material (%zu curves)\n", out.sketch.entities.size());
  return !out.regions.empty();
}

std::optional<MeshProfile> extrusion(const TriMesh& m, const Topology& topo, const gp_Dir& d, double tol, double maxTurn, double chord) {
  const gp_Dir x = across_of(d), y = d.Crossed(x);
  const size_t nv = m.points.size();
  std::vector<double> z(nv);
  std::vector<P2> uv(nv);
  for (size_t i = 0; i < nv; ++i) {
    const gp_Vec p(m.points[i].XYZ());
    z[i] = p.Dot(gp_Vec(d));
    uv[i] = {p.Dot(gp_Vec(x)), p.Dot(gp_Vec(y))};
  }
  const double z0 = *std::min_element(z.begin(), z.end()), z1 = *std::max_element(z.begin(), z.end());
  if (z1 - z0 < tol * 10) MS_FAIL("too flat along this direction");
  std::vector<int> bottom, top;
  std::vector<char> wall(nv, 0);
  for (size_t t = 0; t < m.triangles.size(); ++t) {
    const auto& v = m.triangles[t];
    const double nz = topo.normal[t].Dot(gp_Vec(d));
    auto all = [&](double level) { return std::abs(z[size_t(v[0])] - level) <= tol && std::abs(z[size_t(v[1])] - level) <= tol && std::abs(z[size_t(v[2])] - level) <= tol; };
    if (nz < -0.5 && all(z0)) {
      bottom.push_back(int(t));
      continue;
    }
    if (nz > 0.5 && all(z1)) {
      top.push_back(int(t));
      continue;
    }
    // A wall: flat seen along d (no wider than the tolerance across its longest side).
    const P2 &a = uv[size_t(v[0])], &b = uv[size_t(v[1])], &c = uv[size_t(v[2])];
    const double longest = std::max({len(sub(b, a)), len(sub(c, b)), len(sub(a, c))});
    if (longest > tol && std::abs(cross(sub(b, a), sub(c, a))) / longest > tol) MS_FAIL("a facet is neither a cap nor upright");
    for (int i : v) wall[size_t(i)] = 1;
  }
  if (bottom.empty() || top.empty()) MS_FAIL("no cap at one end");
  const auto lower = border_loops(m, bottom), upper = border_loops(m, top);
  if (!lower || !upper || lower->empty()) MS_FAIL("a cap's outline does not close");
  // Every wall vertex on the bottom's outline, and the top's outline on it too (one section all the way up). The outline as
  // upright fins in a nearest-point tree: the distance to a fin from a point in the bottom plane is the distance to its edge.
  auto fins = [&](const std::vector<std::vector<int>>& loops) {
    std::vector<gp_Pnt> pts;
    std::vector<std::array<int, 3>> tris;
    for (const auto& loop : loops)
      for (size_t i = 0; i < loop.size(); ++i) {
        const P2 &a = uv[size_t(loop[i])], &b = uv[size_t(loop[(i + 1) % loop.size()])];
        const int base = int(pts.size());
        pts.emplace_back(a[0], a[1], 0);
        pts.emplace_back(b[0], b[1], 0);
        pts.emplace_back(a[0], a[1], 1);
        tris.push_back({base, base + 1, base + 2});
      }
    return meshsolid::NearestTree(pts, tris);
  };
  const auto below = fins(*lower), above = fins(*upper);
  const double slack = tol * 1.5;
  for (size_t i = 0; i < nv; ++i)
    if (wall[i] && below.distance(gp_Pnt(uv[i][0], uv[i][1], 0)) > slack) MS_FAIL("a wall vertex is off the outline");
  for (const auto& loop : *upper)
    for (int i : loop)
      if (below.distance(gp_Pnt(uv[size_t(i)][0], uv[size_t(i)][1], 0)) > slack) MS_FAIL("the top outline differs from the bottom");
  for (const auto& loop : *lower)
    for (int i : loop)
      if (above.distance(gp_Pnt(uv[size_t(i)][0], uv[size_t(i)][1], 0)) > slack) MS_FAIL("the bottom outline differs from the top");
  MeshProfile out;
  out.kind = "extrude";
  out.distance = z1 - z0;
  out.frame.origin = vec3(gp_Vec(d).XYZ() * z0);
  out.frame.x = vec3(gp_Vec(x).XYZ());
  out.frame.y = vec3(gp_Vec(y).XYZ());
  std::vector<std::vector<P2>> loops;
  for (const auto& loop : *lower) {
    std::vector<P2> l;
    for (int i : loop) l.push_back(uv[size_t(i)]);
    loops.push_back(std::move(l));
  }
  std::vector<std::array<P2, 3>> cap;
  for (int t : bottom) cap.push_back({uv[size_t(m.triangles[size_t(t)][0])], uv[size_t(m.triangles[size_t(t)][1])], uv[size_t(m.triangles[size_t(t)][2])]});
  if (!finish(out, loops, tol, maxTurn, chord, [&](const P2& p) { return inside_triangles(cap, p); })) MS_FAIL("no profile region");
  return out;
}

std::optional<MeshProfile> revolution(const TriMesh& m, const Topology& topo, const gp_Dir& a, double tol, double maxTurn, double chord) {
  const gp_Dir e1 = across_of(a), e2 = a.Crossed(e1);
  // The axis passes through every facet's normal line: least squares in the plane square to it.
  Eigen::Matrix2d A = Eigen::Matrix2d::Zero();
  Eigen::Vector2d b = Eigen::Vector2d::Zero();
  double radial = 0;
  for (size_t t = 0; t < m.triangles.size(); ++t) {
    const gp_Vec& n = topo.normal[t];
    Eigen::Vector2d np(n.Dot(gp_Vec(e1)), n.Dot(gp_Vec(e2)));
    const double l = np.norm();
    if (l < 0.1) continue;
    np /= l;
    const gp_Pnt c = topo.centroid(int(t));
    const Eigen::Vector2d cc(gp_Vec(c.XYZ()).Dot(gp_Vec(e1)), gp_Vec(c.XYZ()).Dot(gp_Vec(e2)));
    const Eigen::Vector2d perp(-np(1), np(0));
    const double w = topo.area[t] * l;
    A += w * perp * perp.transpose();
    b += w * perp * perp.dot(cc);
    radial += topo.area[t];
  }
  if (radial <= 0) MS_FAIL("no facet faces away from the axis");
  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> eig(A);
  if (eig.eigenvalues()(0) < 1e-6 * eig.eigenvalues()(1) || eig.eigenvalues()(1) <= 0) MS_FAIL("the facets' normals are parallel");  // normals all parallel
  const Eigen::Vector2d o2 = A.ldlt().solve(b);
  const gp_Pnt o(gp_Vec(e1).XYZ() * o2(0) + gp_Vec(e2).XYZ() * o2(1));
  // Every vertex as (radius, height); the profile is what they collapse to.
  const size_t nv = m.points.size();
  std::vector<P2> rz(nv);
  for (size_t i = 0; i < nv; ++i) {
    const gp_Vec v(o, m.points[i]);
    const double h = v.Dot(gp_Vec(a));
    rz[i] = {(v - gp_Vec(a) * h).Magnitude(), h};
  }
  std::unordered_map<long long, std::vector<int>> grid;
  std::vector<P2> centres;
  std::vector<int> cluster(nv);
  auto key = [&](long long i, long long j) { return i * 1000003LL + j; };
  for (size_t i = 0; i < nv; ++i) {
    const long long ci = std::llround(rz[i][0] / tol), cj = std::llround(rz[i][1] / tol);
    int found = -1;
    double best = tol;
    for (long long di = -1; di <= 1; ++di)
      for (long long dj = -1; dj <= 1; ++dj) {
        auto it = grid.find(key(ci + di, cj + dj));
        if (it == grid.end()) continue;
        for (int c : it->second)
          if (const double d = len(sub(centres[size_t(c)], rz[i])); d <= best) {
            best = d;
            found = c;
          }
      }
    if (found < 0) {
      found = int(centres.size());
      centres.push_back(rz[i]);
      grid[key(ci, cj)].push_back(found);
      if (centres.size() > 4000) MS_FAIL("too many profile points");
    }
    cluster[i] = found;
  }
  // The profile's segments: mesh edges between two clusters, without those that pass over another cluster. A triangle
  // whose corners are all one cluster lies in a disc square to the axis: a cap (its rim the only vertices it has).
  std::set<std::pair<int, int>> links;
  std::vector<char> disc(centres.size(), 0);
  for (const auto& t : m.triangles) {
    if (cluster[size_t(t[0])] == cluster[size_t(t[1])] && cluster[size_t(t[1])] == cluster[size_t(t[2])]) disc[size_t(cluster[size_t(t[0])])] = 1;
    for (int k = 0; k < 3; ++k) {
      const int p = cluster[size_t(t[size_t(k)])], q = cluster[size_t(t[(size_t(k) + 1) % 3])];
      if (p != q) links.insert(std::minmax(p, q));
    }
  }
  std::vector<std::vector<int>> adj(centres.size());
  for (const auto& [p, q] : links) {
    const P2 &a2 = centres[size_t(p)], &b2 = centres[size_t(q)];
    const double l = len(sub(b2, a2));
    bool over = false;
    for (size_t c = 0; c < centres.size() && !over; ++c) {
      if (int(c) == p || int(c) == q) continue;
      const P2& x = centres[c];
      if (len(sub(x, a2)) <= tol || len(sub(x, b2)) <= tol || len(sub(x, a2)) >= l) continue;
      over = segment_distance(x, a2, b2) <= tol;
    }
    if (over) continue;
    adj[size_t(p)].push_back(q);
    adj[size_t(q)].push_back(p);
  }
  for (const auto& n : adj)
    if (n.size() > 2) MS_FAIL("the profile branches");
  // Walk the chains: open ones end on the axis (a flat end on a cap reaches it), closed ones are rings.
  std::vector<std::vector<P2>> loops;
  std::vector<char> seen(centres.size(), 0);
  auto walk = [&](int start) {
    std::vector<int> chain{start};
    seen[size_t(start)] = 1;
    int prev = -1, at = start;
    for (;;) {
      int nextC = -1;
      for (int q : adj[size_t(at)])
        if (q != prev && !seen[size_t(q)]) nextC = q;
      if (nextC < 0) break;
      seen[size_t(nextC)] = 1;
      chain.push_back(nextC);
      prev = at;
      at = nextC;
    }
    return chain;
  };
  for (size_t c = 0; c < centres.size(); ++c) {
    if (seen[c] || adj[c].size() != 1) continue;
    const auto chain = walk(int(c));
    const bool capped[2] = {disc[size_t(chain.front())] != 0, disc[size_t(chain.back())] != 0};
    std::vector<P2> loop;
    for (int i : chain) loop.push_back(centres[size_t(i)]);
    for (int end = 0; end < 2; ++end) {
      P2& tip = end ? loop.back() : loop.front();
      const P2& inner = end ? loop[loop.size() - 2] : loop[1];
      if (tip[0] <= 2 * tol) tip[0] = 0;
      else if (capped[end] || std::abs(inner[1] - tip[1]) <= tol) {  // a flat cap ending short of the axis (no vertex on it)
        if (end) loop.push_back({0, tip[1]});
        else loop.insert(loop.begin(), P2{0, tip[1]});
      } else {
        MS_FAIL("an open profile ends off the axis");
      }
    }
    if (loop.size() < 3) MS_FAIL("a profile too short");
    loops.push_back(std::move(loop));
  }
  for (size_t c = 0; c < centres.size(); ++c) {
    if (seen[c]) continue;
    if (adj[c].size() != 2) MS_FAIL("a profile point with one neighbour");
    const auto chain = walk(int(c));
    if (chain.size() < 3) MS_FAIL("a closed profile too short");
    std::vector<P2> loop;
    for (int i : chain) loop.push_back(centres[size_t(i)]);
    loops.push_back(std::move(loop));
  }
  if (loops.empty()) MS_FAIL("no profile");
  MeshProfile out;
  out.kind = "revolve";
  out.frame.origin = vec3(o.XYZ());
  out.frame.x = vec3(gp_Vec(e1).XYZ());
  out.frame.y = vec3(gp_Vec(a).XYZ());
  double lo = 1e300, hi = -1e300;
  for (const auto& l : loops)
    for (const auto& p : l) {
      lo = std::min(lo, p[1]);
      hi = std::max(hi, p[1]);
    }
  if (!finish(out, loops, tol, maxTurn, chord, [&](const P2& p) { return inside_loops(loops, p); })) MS_FAIL("no profile region");
  const double pad = std::max(tol, (hi - lo) * 0.05);
  out.axis = out.sketch.add_line(out.sketch.add_point(0, lo - pad), out.sketch.add_point(0, hi + pad), true);
  return out;
}

json face_kinds(const TopoDS_Shape& shape) {
  json kinds = json::object();
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    const char* kind = "other";
    switch (BRepAdaptor_Surface(TopoDS::Face(faces(i)), false).GetType()) {
      case GeomAbs_Plane: kind = "plane"; break;
      case GeomAbs_Cylinder: kind = "cylinder"; break;
      case GeomAbs_Cone: kind = "cone"; break;
      case GeomAbs_Sphere: kind = "sphere"; break;
      case GeomAbs_Torus: kind = "torus"; break;
      case GeomAbs_BezierSurface:
      case GeomAbs_BSplineSurface: kind = "spline"; break;
      case GeomAbs_SurfaceOfRevolution: kind = "revolution"; break;
      case GeomAbs_SurfaceOfExtrusion: kind = "extrusion"; break;
      default: break;
    }
    kinds[kind] = kinds.value(kind, 0) + 1;
  }
  return kinds;
}

json with_defaults(const std::string& kind, json inputs) {
  if (const FeatureSpec* spec = feature_spec(kind))
    for (const auto& in : spec->inputs)
      if (!inputs.contains(in.name) && !in.def.is_null()) inputs[in.name] = in.def;
  return inputs;
}

}  // namespace

std::vector<int> fit_profile(Sketch& sketch, const std::vector<std::array<double, 2>>& points, bool closed, double tolerance, double max_turn,
                             double chord) {
  // Points closer than a hundredth of the tolerance are one (a duplicated mesh vertex).
  std::vector<P2> pts;
  for (const auto& p : points)
    if (pts.empty() || len(sub(p, pts.back())) > tolerance * 0.01) pts.push_back(p);
  if (closed && pts.size() > 1 && len(sub(pts.front(), pts.back())) <= tolerance * 0.01) pts.pop_back();
  if (pts.size() < (closed ? 3u : 2u)) throw Error("a profile needs at least " + std::string(closed ? "three" : "two") + " points");
  ProfileFitter fitter(pts, closed, tolerance, max_turn, chord);
  const auto pieces = fitter.run();
  std::map<size_t, int> ids;
  auto point = [&](size_t i) {
    const size_t k = closed ? i % pts.size() : i;
    auto it = ids.find(k);
    if (it != ids.end()) return it->second;
    const int id = sketch.add_point(pts[k][0], pts[k][1]);
    ids[k] = id;
    return id;
  };
  std::vector<int> out;
  for (const auto& p : pieces) {
    switch (p.kind) {
      case Piece::Line: out.push_back(sketch.add_line(point(p.a), point(p.b))); break;
      case Piece::Circle: out.push_back(sketch.add_circle(sketch.add_point(p.cx, p.cy), p.r)); break;
      case Piece::Arc: {
        const int c = sketch.add_point(p.cx, p.cy);
        out.push_back(p.ccw ? sketch.add_arc(c, point(p.a), point(p.b)) : sketch.add_arc(c, point(p.b), point(p.a)));
        break;
      }
      case Piece::Spline: {
        SkEntity e;
        e.type = SkEntity::Type::Spline;
        e.degree = p.spline->Degree();
        for (int j = 1; j <= p.spline->NbPoles(); ++j) {
          const gp_Pnt pole = p.spline->Pole(j);
          e.p.push_back(j == 1 ? point(p.a) : j == p.spline->NbPoles() ? point(p.b) : sketch.add_point(pole.X(), pole.Y()));
          e.weights.push_back(p.spline->Weight(j));
        }
        for (int j = 1; j <= p.spline->NbKnots(); ++j) {
          e.knots.push_back(p.spline->Knot(j));
          e.multiplicities.push_back(p.spline->Multiplicity(j));
        }
        e.id = sketch.next_id();
        sketch.entities.push_back(std::move(e));
        out.push_back(sketch.entities.back().id);
        break;
      }
    }
  }
  return out;
}

std::vector<MeshProfile> mesh_profiles(const TriMesh& mesh, double tolerance, double angle_deg, const Cancel& cancel) {
  if (mesh.empty() || mesh.open_edges() != 0) return {};
  const Topology topo(mesh);
  const double maxTurn = std::clamp(angle_deg, 5.0, 80.0) * M_PI / 180;
  std::vector<gp_Dir> dirs = facing_directions(topo, 6);
  for (const auto& w : {gp_Dir(0, 0, 1), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)}) add_direction(dirs, w);
  // The axis curved facets turn about most: the direction their normals spread least along.
  {
    Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
    for (size_t t = 0; t < mesh.triangles.size(); ++t) {
      bool curved = false;
      for (int k = 0; k < 3; ++k) {
        const double d = topo.dihedral[t][size_t(k)];
        curved |= topo.across[t][size_t(k)] >= 0 && d > 1e-3 && d < maxTurn;
      }
      if (!curved) continue;
      const Eigen::Vector3d n(topo.normal[t].X(), topo.normal[t].Y(), topo.normal[t].Z());
      M += topo.area[t] * n * n.transpose();
    }
    if (M.norm() > 0) {
      const Eigen::Vector3d ax = Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d>(M).eigenvectors().col(0);
      add_direction(dirs, gp_Dir(ax(0), ax(1), ax(2)));
    }
  }
  // How deep the mesh's facets cut inside its curved surfaces: an arc's chords in the profile cut no deeper.
  const double chord = meshsolid::mesh_sagitta(topo, maxTurn);
  std::vector<MeshProfile> out;
  for (const auto& d : dirs) {
    if (cancel && cancel()) throw Error("cancelled");
    if (auto e = extrusion(mesh, topo, d, tolerance, maxTurn, chord)) out.push_back(std::move(*e));
    if (auto r = revolution(mesh, topo, d, tolerance, maxTurn, chord)) out.push_back(std::move(*r));
  }
  std::stable_sort(out.begin(), out.end(), [](const MeshProfile& a, const MeshProfile& b) {
    if (a.curves() != b.curves()) return a.curves() < b.curves();
    return a.kind == "extrude" && b.kind != "extrude";
  });
  return out;
}

MeshConversion convert_mesh(const Document& doc, const Scene& scene, const std::string& node, const MeshConversionOptions& options, const Cancel& cancel) {
  const Node* n = scene.node(node);
  if (!n || n->kind != Node::Kind::Body) throw Error("Select a mesh body to convert");
  const TopoDS_Shape world = node_world_shape(doc, scene, node);
  if (n->representation != "mesh" && !is_mesh_shape(world)) throw Error("This body is already a solid; Mesh to solid takes mesh bodies (STL, OBJ, 3MF, PLY ...)");
  const TriMesh mesh = mesh_of_shape(world);
  const double tol = options.solid.tolerance > 0 ? options.solid.tolerance : mesh_tolerance(mesh);
  const std::string name = options.name.empty() ? n->name : options.name;
  auto place = [&](json op) {
    op["id"] = new_uuid();
    if (!options.component.empty()) op["component"] = options.component;
    return op;
  };
  auto plan = [&](std::vector<json> ops, const std::string& feature, const std::string& method, int curves) {
    if (options.remove_source)
      ops.push_back(place(make_feature_op("remove", next_name(scene, "Remove"), with_defaults("remove", {{"bodies", json::array({{{"body", node}}})}}))));
    MeshConversion c;
    c.method = method;
    c.plan = plan_ops(doc, std::move(ops), true, cancel);
    // Every body the feature made (a mesh of several pieces gives several), measured together.
    std::vector<TopoDS_Shape> made;
    for (const auto& ch : c.plan.changed)
      if (ch.op == feature && !ch.removed && ch.shape && !ch.shape->IsNull()) made.push_back(*ch.shape);
    if (made.size() == 1) c.shape = made.front();
    else if (!made.empty()) {
      BRep_Builder b;
      TopoDS_Compound all;
      b.MakeCompound(all);
      for (const auto& s : made) b.Add(all, s);
      c.shape = all;
    }
    if (c.shape.IsNull()) throw Error("the conversion made no body");
    style_new_bodies(c.plan, feature, {{"body_name", name}});
    c.deviation = mesh_deviation(mesh, c.shape, tol, cancel);
    c.report = {{"method", method},
                {"tolerance", tol},
                {"angle", options.solid.angle},
                {"triangles", int(mesh.triangles.size())},
                {"faces", face_kinds(c.shape)},
                {"face_count", c.deviation.shape_faces},
                {"deviation", c.deviation.to_json()}};
    if (curves > 0) c.report["curves"] = curves;
    return c;
  };
  if (options.mode != "solid") {
    int tried = 0;
    for (auto& profile : mesh_profiles(mesh, tol, options.solid.angle, cancel)) {
      if (++tried > 4) break;
      json sketch = place(make_sketch_op(name + " profile", {{"frame", profile.frame.to_json()}}, profile.sketch.to_json()));
      const std::string sketchId = sketch["id"];
      json regions = json::array();
      for (const auto& r : profile.regions) regions.push_back({{"sketch", sketchId}, {"at", {r[0], r[1]}}});
      json inputs = {{"profiles", regions}, {"operation", "new"}};
      if (profile.kind == "extrude") inputs["distance"] = mm(profile.distance);
      else {
        inputs["axis"] = {{"sketch", sketchId}, {"entity", profile.axis}};
        inputs["angle"] = "360 deg";
      }
      const std::string prefix = profile.kind == "extrude" ? "Extrude" : "Revolve";
      json feature = place(make_feature_op(profile.kind, next_name(scene, prefix), with_defaults(profile.kind, inputs)));
      const std::string featureId = feature["id"];
      try {
        MeshConversion c = plan({sketch, feature}, featureId, profile.kind, profile.curves());
        const auto& d = c.deviation;
        // Offered only when it is the mesh: every vertex on it, and none of it farther from the mesh than the facets explain.
        if (d.max <= tol * 1.5 && d.back_max <= tol * 2 + 1.5 * d.sagitta && std::abs(d.shape_volume - d.mesh_volume) <= 0.1 * d.mesh_volume) return c;
      } catch (const Error& e) {
        if (std::string(e.what()) == "cancelled") throw;
      }
    }
  }
  std::ostringstream angle;
  angle << options.solid.angle << " deg";
  json inputs = {{"bodies", json::array({{{"body", node}}})}, {"tolerance", options.solid.tolerance > 0 ? mm(tol) : "0 mm"}, {"angle", angle.str()}, {"operation", "new"}};
  json feature = place(make_feature_op("mesh_solid", next_name(scene, "MeshToSolid"), with_defaults("mesh_solid", inputs)));
  const std::string featureId = feature["id"];
  return plan({feature}, featureId, "solid", 0);
}

}  // namespace opad::design
