// The last resort of mesh_shape: a face BRepMesh left without triangles is triangulated from its boundary alone.
// BRepMesh refuses any face whose wires touch or cross themselves within their tolerances (status SelfIntersectingWire),
// and the whole face then vanished from the view, the renders and the exports. STEP files carry such faces: a cover's
// rib floor whose outline runs round two pockets from points on itself, 0.006 mm off, was a dark hole in it.
// Here every wire is sampled along its edges' pcurves in the face's (u, v) plane; a loop is cut in two where it comes
// back to itself (closer than twice the edges' tolerance, or a tenth of the deflection), so every piece is simple, and a
// piece turning against its loop is a hole (or, cut from a hole, an island). Each outer piece and the holes inside it
// go to earcut (third_party/earcut, Mapbox's polygon triangulator, which bridges holes and survives touching corners).
// Curved faces are then split evenly until the triangles stay within the deflection.
#include "mesh_fallback.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "../../third_party/earcut/earcut.hpp"

namespace opad::detail {
namespace {

struct UV { double u, v; };
using Ring = std::vector<int>;  // indices into the face's points

struct Points {
  std::vector<UV> uv;
  std::vector<gp_Pnt> xyz;  // where each point is on the surface (in the face's frame), for distances in model units
  const UV& operator[](int i) const { return uv[size_t(i)]; }
  double area(const Ring& r) const {  // > 0: counter-clockwise in (u, v)
    double a = 0;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) a += uv[size_t(r[j])].u * uv[size_t(r[i])].v - uv[size_t(r[i])].u * uv[size_t(r[j])].v;
    return a / 2;
  }
};

// One wire in (u, v), in order, each edge sampled within the deflection; the last point of an edge is the next one's first.
std::vector<UV> wire_samples(const TopoDS_Wire& wire, const TopoDS_Face& face, double tolerance, double angle) {
  std::vector<UV> ring;
  for (BRepTools_WireExplorer e(wire, face); e.More(); e.Next()) {
    const TopoDS_Edge& edge = e.Current();
    double first = 0, last = 0;
    const Handle(Geom2d_Curve) pcurve = BRep_Tool::CurveOnSurface(edge, face, first, last);
    if (pcurve.IsNull()) return {};
    std::vector<double> at;
    const bool curve = !BRep_Tool::Degenerated(edge) && !BRep_Tool::Curve(edge, first, last).IsNull();
    BRep_Tool::Range(edge, face, first, last);
    if (curve) {
      GCPnts_TangentialDeflection sample(BRepAdaptor_Curve(edge), first, last, angle, tolerance, 2);
      for (int i = 1; i <= sample.NbPoints(); ++i) at.push_back(sample.Parameter(i));
    } else {  // a pole: a line in (u, v), one point in space
      for (int i = 0; i <= 8; ++i) at.push_back(first + (last - first) * i / 8);
    }
    if (edge.Orientation() == TopAbs_REVERSED) std::reverse(at.begin(), at.end());
    for (size_t i = 0; i + 1 < at.size(); ++i) {
      const gp_Pnt2d p = pcurve->Value(at[i]);
      ring.push_back({p.X(), p.Y()});
    }
  }
  ring.erase(std::unique(ring.begin(), ring.end(), [](const UV& a, const UV& b) { return std::abs(a.u - b.u) < 1e-12 && std::abs(a.v - b.v) < 1e-12; }), ring.end());
  return ring;
}

// A loop that comes back to itself (two points that are not neighbours closer than `pinch`) cut there into two loops,
// again until none does. Close in (u, v) too: a periodic face's seam has its two sides at the same place in space, far
// apart in (u, v). A piece turning counter-clockwise is face, a clockwise one a hole: the outline of the report ran
// round two pockets clockwise from points on itself, holes touching it (filled, the face came out six times too big).
// Pieces without area (a loop sampled more finely than `pinch`) go.
void split_pinches(const Points& pts, Ring ring, double pinch, double pinch_uv, std::vector<Ring>& faces, std::vector<Ring>& holes) {
  const size_t n = ring.size();
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 2; j < n; ++j) {
      if (i == 0 && j == n - 1) continue;
      const UV &a = pts[ring[i]], &b = pts[ring[j]];
      if (std::abs(a.u - b.u) > pinch_uv || std::abs(a.v - b.v) > pinch_uv || pts.xyz[size_t(ring[i])].Distance(pts.xyz[size_t(ring[j])]) >= pinch) continue;
      // Both pieces keep the same point there (i, not j a hair away), so they touch exactly: a gap of 0.003 mm let
      // earcut's triangles slip into a pocket.
      split_pinches(pts, Ring(ring.begin() + long(i), ring.begin() + long(j)), pinch, pinch_uv, faces, holes);
      Ring rest{ring[i]};
      rest.insert(rest.end(), ring.begin() + long(j) + 1, ring.end());
      rest.insert(rest.end(), ring.begin(), ring.begin() + long(i));
      split_pinches(pts, std::move(rest), pinch, pinch_uv, faces, holes);
      return;
    }
  const double area = n >= 3 ? pts.area(ring) : 0;
  if (std::abs(area) > pinch_uv * pinch_uv) (area > 0 ? faces : holes).push_back(std::move(ring));
}

// Whether point q is inside the ring (even-odd).
bool contains(const Points& pts, const Ring& r, const UV& q) {
  bool in = false;
  for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++) {
    const UV &a = pts[r[i]], &b = pts[r[j]];
    if ((a.v > q.v) != (b.v > q.v) && q.u < a.u + (q.v - a.v) * (b.u - a.u) / (b.v - a.v)) in = !in;
  }
  return in;
}

// A curved face's triangles cut along a grid whose cells keep the surface within the deflection and are about square in
// space: earcut's triangles run long and thin across a face (they only join boundary points), which folds on a curved
// surface (a cylinder's wall came out 2.5 % too big). Each triangle is clipped to every cell it crosses and the pieces
// fanned; two triangles cut their shared edge at the same grid crossings, so no cracks open between them.
template <typename Point>
std::vector<std::array<int, 3>> cut_by_grid(Points& pts, const std::vector<std::array<int, 3>>& tris, const Ring& outer, double tolerance, const Point& point) {
  double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
  for (int q : outer) a0 = std::min(a0, pts[q].u), a1 = std::max(a1, pts[q].u), b0 = std::min(b0, pts[q].v), b1 = std::max(b1, pts[q].v);
  if (!(a1 > a0) || !(b1 > b0)) return tris;
  auto at = [&](bool along_u, double s, double t) { return along_u ? point(UV{t, s}) : point(UV{s, t}); };
  auto bend = [&](bool along_u, int cells) {  // how far the surface leaves the chords of `cells` steps along u (or v)
    double worst = 0;
    const double s0 = along_u ? b0 : a0, s1 = along_u ? b1 : a1, t0 = along_u ? a0 : b0, t1 = along_u ? a1 : b1;
    for (int i = 0; i <= 8; ++i)
      for (int k = 0; k < cells; ++k) {
        const double s = s0 + (s1 - s0) * i / 8, t = t0 + (t1 - t0) * k / cells, dt = (t1 - t0) / cells;
        const gp_Pnt p = at(along_u, s, t), q = at(along_u, s, t + dt), m = at(along_u, s, t + dt / 2);
        worst = std::max(worst, m.Distance(gp_Pnt((p.XYZ() + q.XYZ()) / 2)));
      }
    return worst;
  };
  auto length = [&](bool along_u) {  // across the middle of the face
    const double s = along_u ? (b0 + b1) / 2 : (a0 + a1) / 2, t0 = along_u ? a0 : b0, t1 = along_u ? a1 : b1;
    double sum = 0;
    for (int k = 0; k < 32; ++k) sum += at(along_u, s, t0 + (t1 - t0) * k / 32).Distance(at(along_u, s, t0 + (t1 - t0) * (k + 1) / 32));
    return sum;
  };
  int nu = 1, nv = 1;
  while (nu < 128 && bend(true, nu) > tolerance) nu *= 2;
  while (nv < 128 && bend(false, nv) > tolerance) nv *= 2;
  const double lu = length(true), lv = length(false), cell = std::min(lu / nu, lv / nv);
  if (cell > 0) {
    nu = std::clamp(int(std::ceil(lu / cell)), nu, 128);
    nv = std::clamp(int(std::ceil(lv / cell)), nv, 128);
  }
  if (nu * nv <= 1) return tris;
  auto gu = [&](int i) { return i >= nu ? a1 : a0 + (a1 - a0) * i / nu; };
  auto gv = [&](int k) { return k >= nv ? b1 : b0 + (b1 - b0) * k / nv; };

  std::map<std::pair<double, double>, int> ids;  // a point made twice (both sides of a cut) is one point
  for (const auto& t : tris)
    for (int q : t) ids.emplace(std::make_pair(pts[q].u, pts[q].v), q);
  auto id = [&](const UV& p) {
    const auto [it, made] = ids.emplace(std::make_pair(p.u, p.v), int(pts.uv.size()));
    if (made) pts.uv.push_back(p), pts.xyz.push_back(point(p));
    return it->second;
  };
  // Where segment p-q crosses the line u = c (or v = c), computed from its lesser end, so both triangles sharing the
  // segment get the very same point.
  auto crossing = [](UV p, UV q, bool on_u, double c) {
    if (std::make_pair(q.u, q.v) < std::make_pair(p.u, p.v)) std::swap(p, q);
    const double t = on_u ? (c - p.u) / (q.u - p.u) : (c - p.v) / (q.v - p.v);
    return on_u ? UV{c, p.v + (q.v - p.v) * t} : UV{p.u + (q.u - p.u) * t, c};
  };
  auto clip = [&](const std::vector<UV>& in, bool on_u, double c, bool keep_above) {
    std::vector<UV> out;
    for (size_t i = 0; i < in.size(); ++i) {
      const UV &p = in[i], &q = in[(i + 1) % in.size()];
      const double dp = (on_u ? p.u : p.v) - c, dq = (on_u ? q.u : q.v) - c;
      const bool pin = keep_above ? dp >= 0 : dp <= 0, qin = keep_above ? dq >= 0 : dq <= 0;
      if (pin) out.push_back(p);
      if (pin != qin && dp != 0 && dq != 0) out.push_back(crossing(p, q, on_u, c));
    }
    return out;
  };
  std::vector<std::array<int, 3>> out;
  for (const auto& t : tris) {
    const UV &a = pts[t[0]], &b = pts[t[1]], &c = pts[t[2]];
    const double lo_u = std::min({a.u, b.u, c.u}), hi_u = std::max({a.u, b.u, c.u}), lo_v = std::min({a.v, b.v, c.v}), hi_v = std::max({a.v, b.v, c.v});
    const int i0 = std::clamp(int((lo_u - a0) / (a1 - a0) * nu), 0, nu - 1), i1 = std::clamp(int((hi_u - a0) / (a1 - a0) * nu), 0, nu - 1);
    const int k0 = std::clamp(int((lo_v - b0) / (b1 - b0) * nv), 0, nv - 1), k1 = std::clamp(int((hi_v - b0) / (b1 - b0) * nv), 0, nv - 1);
    if (i0 == i1 && k0 == k1) { out.push_back(t); continue; }
    for (int i = i0; i <= i1; ++i)
      for (int k = k0; k <= k1; ++k) {
        std::vector<UV> piece{a, b, c};
        if (i > 0) piece = clip(piece, true, gu(i), true);
        if (piece.size() >= 3 && i < nu - 1) piece = clip(piece, true, gu(i + 1), false);
        if (piece.size() >= 3 && k > 0) piece = clip(piece, false, gv(k), true);
        if (piece.size() >= 3 && k < nv - 1) piece = clip(piece, false, gv(k + 1), false);
        if (piece.size() < 3) continue;
        std::vector<int> corner;
        for (const auto& p : piece)
          if (const int q = id(p); corner.empty() || (q != corner.back() && q != corner.front())) corner.push_back(q);
        for (size_t f = 1; f + 1 < corner.size(); ++f) {
          const UV &p0 = pts[corner[0]], &p1 = pts[corner[f]], &p2 = pts[corner[f + 1]];
          if ((p1.u - p0.u) * (p2.v - p0.v) - (p1.v - p0.v) * (p2.u - p0.u) > 0) out.push_back({corner[0], corner[f], corner[f + 1]});
        }
      }
  }
  return out;
}

}  // namespace

bool triangulate_from_boundary(TopoDS_Face face, double tolerance, double angle) {
  try {
    const TopoDS_Face forward = TopoDS::Face(face.Oriented(TopAbs_FORWARD));
    TopLoc_Location surface_location;
    const Handle(Geom_Surface) surface = BRep_Tool::Surface(forward, surface_location);
    if (surface.IsNull()) return false;
    // The face's own frame: the triangulation is stored without the face's location; the surface may have its own.
    const gp_Trsf to_local = face.Location().Transformation().Inverted() * surface_location.Transformation();
    auto point = [&](const UV& p) { return surface->Value(p.u, p.v).Transformed(to_local); };

    Points pts;
    std::vector<Ring> rings;
    double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
    const TopoDS_Wire outer_wire = BRepTools::OuterWire(forward);
    for (TopoDS_Iterator w(forward); w.More(); w.Next()) {
      if (w.Value().ShapeType() != TopAbs_WIRE) continue;
      const std::vector<UV> samples = wire_samples(TopoDS::Wire(w.Value()), forward, tolerance, angle);
      if (samples.size() < 3) continue;
      Ring ring;
      for (const auto& p : samples) {
        ring.push_back(int(pts.uv.size()));
        pts.uv.push_back(p);
        pts.xyz.push_back(point(p));
        u0 = std::min(u0, p.u), u1 = std::max(u1, p.u), v0 = std::min(v0, p.v), v1 = std::max(v1, p.v);
      }
      const bool is_outer = w.Value().IsSame(outer_wire);
      if ((pts.area(ring) > 0) != is_outer) std::reverse(ring.begin(), ring.end());  // outer counter-clockwise, holes clockwise
      rings.push_back(std::move(ring));
    }
    if (rings.empty() || pts.uv.size() > 8000) return false;  // the pinch search is quadratic: ordinary faces only
    double pinch = 0.1 * tolerance;
    for (TopExp_Explorer e(forward, TopAbs_EDGE); e.More(); e.Next()) pinch = std::max(pinch, 2 * BRep_Tool::Tolerance(TopoDS::Edge(e.Current())));
    const double pinch_uv = 1e-3 * std::max(u1 - u0, v1 - v0);
    std::vector<Ring> outers, holes;
    for (auto& ring : rings) split_pinches(pts, std::move(ring), pinch, pinch_uv, outers, holes);
    if (outers.empty()) return false;

    // Each hole goes into the outer piece holding most of its corners (a hole pinched off the outline touches it).
    std::vector<std::vector<Ring>> holes_of(outers.size());
    for (auto& hole : holes) {
      size_t best = outers.size(), most = 0;
      for (size_t o = 0; o < outers.size(); ++o) {
        size_t in = 0;
        for (int q : hole) in += contains(pts, outers[o], pts[q]);
        if (in > most) most = in, best = o;
      }
      if (best < outers.size()) holes_of[best].push_back(std::move(hole));
    }
    const bool plane = surface->IsKind(STANDARD_TYPE(Geom_Plane));
    std::vector<std::array<int, 3>> tris;
    for (size_t o = 0; o < outers.size(); ++o) {
      std::vector<std::vector<std::array<double, 2>>> polygon;
      std::vector<int> index;  // earcut's vertex numbers (rings one after the other) -> points
      auto add = [&](const Ring& r) {
        polygon.emplace_back();
        for (int q : r) polygon.back().push_back({pts[q].u, pts[q].v}), index.push_back(q);
      };
      add(outers[o]);
      for (const auto& hole : holes_of[o]) add(hole);
      const std::vector<uint32_t> cut = mapbox::earcut<uint32_t>(polygon);
      std::vector<std::array<int, 3>> part;
      for (size_t t = 0; t + 2 < cut.size(); t += 3) {
        std::array<int, 3> tri{index[cut[t]], index[cut[t + 1]], index[cut[t + 2]]};
        const UV &a = pts[tri[0]], &b = pts[tri[1]], &c = pts[tri[2]];
        const double turn = (b.u - a.u) * (c.v - a.v) - (b.v - a.v) * (c.u - a.u);
        if (turn == 0) continue;
        if (turn < 0) std::swap(tri[1], tri[2]);  // counter-clockwise in (u, v): along the surface's own normal
        part.push_back(tri);
      }
      if (!plane && !part.empty()) part = cut_by_grid(pts, part, outers[o], tolerance, point);
      tris.insert(tris.end(), part.begin(), part.end());
    }
    if (tris.empty()) return false;

    // Curved faces: every triangle split in four until the surface stays within the deflection of them (a few levels).
    if (!plane) {
      for (int level = 0; level < 4 && tris.size() * 4 < 400000; ++level) {
        double worst = 0;
        for (const auto& t : tris) {
          const gp_Pnt &a = pts.xyz[size_t(t[0])], &b = pts.xyz[size_t(t[1])], &c = pts.xyz[size_t(t[2])];
          const gp_Vec n = gp_Vec(a, b).Crossed(gp_Vec(a, c));
          if (n.Magnitude() < 1e-18) continue;
          const UV &p0 = pts[t[0]], &p1 = pts[t[1]], &p2 = pts[t[2]];
          worst = std::max(worst, std::abs(gp_Vec(a, point({(p0.u + p1.u + p2.u) / 3, (p0.v + p1.v + p2.v) / 3})).Dot(n.Normalized())));
        }
        if (worst <= tolerance) break;
        std::map<std::pair<int, int>, int> middles;
        auto middle = [&](int a, int b) {
          const auto key = std::minmax(a, b);
          if (const auto it = middles.find(key); it != middles.end()) return it->second;
          const UV m{(pts[a].u + pts[b].u) / 2, (pts[a].v + pts[b].v) / 2};
          pts.uv.push_back(m);
          pts.xyz.push_back(point(m));
          return middles[key] = int(pts.uv.size()) - 1;
        };
        std::vector<std::array<int, 3>> finer;
        finer.reserve(tris.size() * 4);
        for (const auto& t : tris) {
          const int ab = middle(t[0], t[1]), bc = middle(t[1], t[2]), ca = middle(t[2], t[0]);
          finer.push_back({t[0], ab, ca});
          finer.push_back({ab, t[1], bc});
          finer.push_back({ca, bc, t[2]});
          finer.push_back({ab, bc, ca});
        }
        tris = std::move(finer);
      }
    }

    Handle(Poly_Triangulation) mesh = new Poly_Triangulation(int(pts.uv.size()), int(tris.size()), Standard_True);
    for (size_t i = 0; i < pts.uv.size(); ++i) {
      mesh->SetNode(int(i) + 1, pts.xyz[i]);
      mesh->SetUVNode(int(i) + 1, gp_Pnt2d(pts.uv[i].u, pts.uv[i].v));
    }
    for (size_t i = 0; i < tris.size(); ++i) mesh->SetTriangle(int(i) + 1, Poly_Triangle(tris[i][0] + 1, tris[i][1] + 1, tris[i][2] + 1));
    mesh->Deflection(tolerance);
    BRep_Builder builder;
    const bool modified = face.Modified(), checked = face.Checked();
    builder.UpdateFace(face, mesh);
    face.Modified(modified);
    face.Checked(checked);
    return true;
  } catch (const Standard_Failure&) {
    return false;
  }
}

}  // namespace opad::detail
