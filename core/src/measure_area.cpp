// The area a pick encloses and its perimeter (UI-90): fills and faces, loops of edges (picked, or grown from one edge of a
// drawing through its body's edges), the polygon through picked points. Declared in opad/inspect.hpp.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Circ.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>

#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

namespace opad {
namespace {
constexpr int kBoundaryPoints = 2000;  // the outline handed to the view (and kept by a pinned result), all loops together
constexpr double kTie = 1e-7;          // directions leaving a node closer than this (radians) are tangent: curvature decides
// Gauss-Legendre, 10 points on [-1, 1] (symmetric pairs).
constexpr std::array<double, 5> kGaussX{0.1488743389816312, 0.4333953941292472, 0.6794095682990244, 0.8650633666889845, 0.9739065285171717};
constexpr std::array<double, 5> kGaussW{0.2955242247147529, 0.2692667193099963, 0.2190863625159820, 0.1494513491505806, 0.0666713443086881};

json pnt(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }

struct Plane {
  gp_Pnt o;
  gp_Vec n{0, 0, 1}, u{1, 0, 0}, v{0, 1, 0};
  double x(const gp_Pnt& p) const { return gp_Vec(o, p).Dot(u); }
  double y(const gp_Pnt& p) const { return gp_Vec(o, p).Dot(v); }
};
Plane plane(const gp_Pnt& o, gp_Vec n) {
  Plane p;
  p.o = o;
  if (n.Magnitude() < 1e-12) n = gp_Vec(0, 0, 1);
  p.n = n.Normalized();
  const gp_Vec seed = std::abs(p.n.X()) < 0.9 ? gp_Vec(1, 0, 0) : gp_Vec(0, 1, 0);
  p.u = (seed - p.n * seed.Dot(p.n)).Normalized();
  p.v = p.n.Crossed(p.u);
  return p;
}
// A drawing body's plane: its own xy plane where it is placed.
Plane bodyPlane(const Scene& scene, const std::string& body) {
  const Mat4 w = scene.world(body);
  const Vec3 o = w.apply({0, 0, 0}), z = w.apply_dir({0, 0, 1});
  return plane(gp_Pnt(o[0], o[1], o[2]), gp_Vec(z[0], z[1], z[2]));
}
// Newell's normal of a closed polygon: its length is twice the polygon's area.
gp_Vec newell(const std::vector<gp_Pnt>& pts) {
  gp_Vec n(0, 0, 0);
  for (size_t i = 0; i < pts.size(); ++i) n += gp_Vec(pts[i].XYZ()).Crossed(gp_Vec(pts[(i + 1) % pts.size()].XYZ()));
  return n;
}

// One edge of a boundary (or the part of it between two parameters), walked from its first parameter to its last
// (forward) or back.
struct Piece {
  TopoDS_Edge edge;
  bool forward = true;
  double t0 = 0, t1 = -1;  // t1 < t0: the whole edge
};
std::pair<double, double> span(const Piece& piece, const BRepAdaptor_Curve& c) {
  return piece.t1 < piece.t0 ? std::make_pair(c.FirstParameter(), c.LastParameter()) : std::make_pair(piece.t0, piece.t1);
}

// ∮ (x dy - y dx) / 2 along the piece in the plane: a closed walk's signed area (positive counter-clockwise about n).
double green(const Piece& piece, const Plane& p) {
  const BRepAdaptor_Curve c(piece.edge);
  const auto [t0, t1] = span(piece, c);
  double sum = 0;
  if (c.GetType() == GeomAbs_Line) {
    const gp_Pnt a = c.Value(t0), b = c.Value(t1);
    sum = (p.x(a) * p.y(b) - p.y(a) * p.x(b)) / 2;
  } else {
    const int pieces = c.GetType() == GeomAbs_Circle || c.GetType() == GeomAbs_Ellipse ? std::max(4, int(std::ceil(std::abs(t1 - t0) / (M_PI / 8))))
                                                                                         : std::max(16, 4 * c.NbIntervals(GeomAbs_C2));
    const double h = (t1 - t0) / pieces;
    for (int k = 0; k < pieces; ++k) {
      const double mid = t0 + (k + 0.5) * h, half = h / 2;
      for (size_t g = 0; g < kGaussX.size(); ++g)
        for (const double s : {-1.0, 1.0}) {
          gp_Pnt at;
          gp_Vec d;
          c.D1(mid + s * half * kGaussX[g], at, d);
          sum += kGaussW[g] * half * (p.x(at) * d.Dot(p.v) - p.y(at) * d.Dot(p.u)) / 2;
        }
    }
  }
  return piece.forward ? sum : -sum;
}

double length(const Piece& piece) {
  try {
    const BRepAdaptor_Curve c(piece.edge);
    const auto [t0, t1] = span(piece, c);
    return GCPnts_AbscissaPoint::Length(c, t0, t1);
  } catch (const Standard_Failure&) {
    return 0;
  }
}
double length(const TopoDS_Edge& e) { return length(Piece{e}); }

// Points along the piece in walking order, the last one left out (the next piece starts there).
void sample(const Piece& piece, std::vector<gp_Pnt>& out, int count) {
  const BRepAdaptor_Curve c(piece.edge);
  const auto [t0, t1] = span(piece, c);
  const int n = c.GetType() == GeomAbs_Line ? 1 : std::max(2, count);
  for (int i = 0; i < n; ++i) {
    const double s = double(i) / n;
    out.push_back(c.Value(piece.forward ? t0 + (t1 - t0) * s : t1 - (t1 - t0) * s));
  }
}
int samplesFor(const Piece& piece, double size) {
  if (BRepAdaptor_Curve(piece.edge).GetType() == GeomAbs_Line) return 1;
  return std::clamp(int(length(piece) / std::max(size, 1e-9) * 200), 12, 256);
}

// Endpoints merged within a tolerance (a grid of cells that size, neighbours searched).
struct Nodes {
  struct Hash {
    size_t operator()(const std::array<long long, 3>& k) const { return std::hash<long long>()(k[0] * 73856093LL ^ k[1] * 19349663LL ^ k[2] * 83492791LL); }
  };
  double tol;
  std::vector<gp_Pnt> at;
  std::unordered_map<std::array<long long, 3>, std::vector<int>, Hash> cells;
  int id(const gp_Pnt& p) {
    const std::array<long long, 3> key{std::llround(std::floor(p.X() / tol)), std::llround(std::floor(p.Y() / tol)), std::llround(std::floor(p.Z() / tol))};
    for (long long dx = -1; dx <= 1; ++dx)
      for (long long dy = -1; dy <= 1; ++dy)
        for (long long dz = -1; dz <= 1; ++dz)
          if (const auto it = cells.find({key[0] + dx, key[1] + dy, key[2] + dz}); it != cells.end())
            for (int i : it->second)
              if (at[i].Distance(p) <= tol) return i;
    at.push_back(p);
    cells[key].push_back(int(at.size()) - 1);
    return int(at.size()) - 1;
  }
};

struct Edge {
  Piece piece;  // walked forward from a to b
  int a = -1, b = -1;
  gp_Vec leaveA, leaveB, bendA, bendB;  // first and second derivatives leaving each end into the edge
  gp_Pnt mid;
  bool alive = true;
};
Edge makeEdge(const Piece& piece, Nodes& nodes) {
  Edge out;
  out.piece = piece;
  const BRepAdaptor_Curve c(piece.edge);
  const auto [t0, t1] = span(piece, c);
  gp_Pnt p0, p1;
  gp_Vec d0, d1, s0, s1;
  c.D2(t0, p0, d0, s0);
  c.D2(t1, p1, d1, s1);
  // A cusp or a degenerate derivative: the chord towards a point just inside, no bend.
  if (d0.Magnitude() < 1e-12) d0 = gp_Vec(p0, c.Value(t0 + (t1 - t0) * 1e-3)), s0 = gp_Vec(0, 0, 0);
  if (d1.Magnitude() < 1e-12) d1 = gp_Vec(c.Value(t1 - (t1 - t0) * 1e-3), p1), s1 = gp_Vec(0, 0, 0);
  out.a = nodes.id(p0);
  out.b = nodes.id(p1);
  out.leaveA = d0, out.bendA = s0;
  out.leaveB = -d1, out.bendB = s1;
  out.mid = c.Value((t0 + t1) / 2);
  return out;
}

// ---------------------------------------------------------------- the planar arrangement
// Edges of a drawing cut where others cross them or end on them (an X, a T), so the cells are traced in what is drawn,
// not only between objects that meet at their ends. Lines and circles in the plane meet exactly; any other curve is met
// on its samples and the crossing refined on both curves (Newton). Candidate pairs come from a grid of short pieces.
struct P2 {
  double x = 0, y = 0;
};
double dist(P2 a, P2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

struct Flat {
  BRepAdaptor_Curve c;
  enum Type { Line, Circle, Other } type = Other;
  double t0 = 0, t1 = 0;
  P2 a, b;                           // its ends
  P2 centre;                         // a circle in the plane: angle = base + sign * t
  double r = 0, base = 0, sign = 1;
  std::vector<P2> pts;               // samples (a line: its ends)
  std::vector<double> ts;            // their parameters
  P2 low{1e300, 1e300}, high{-1e300, -1e300};  // their box
  std::vector<double> cuts;
};

class Arrangement {
 public:
  std::vector<Edge> edges;
  std::vector<std::vector<int>> of;  // per input edge, its pieces (a piece drawn twice is kept once, under both)
  size_t nodes = 0;
  std::vector<gp_Pnt> points;  // where the nodes are

  Arrangement(const std::vector<TopoDS_Edge>& in, const Plane& p, double tol, const std::function<bool()>& cancelled) : m_p(p), m_tol(tol), m_cancelled(cancelled) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& e : in) {
      Flat f;
      try {
        flatten(e, f);
      } catch (const Standard_Failure&) {
        f.pts.clear();  // a curve that cannot be evaluated is not cut
      }
      for (const P2& q : f.pts) f.low = {std::min(f.low.x, q.x), std::min(f.low.y, q.y)}, f.high = {std::max(f.high.x, q.x), std::max(f.high.y, q.y)};
      x0 = std::min(x0, f.low.x), y0 = std::min(y0, f.low.y), x1 = std::max(x1, f.high.x), y1 = std::max(y1, f.high.y);
      m_flats.push_back(std::move(f));
    }
    if (x0 <= x1) {
      size_t n = 0;
      for (const auto& [i, j] : candidates(x0, y0, std::max({x1 - x0, y1 - y0, tol}))) {
        tick(n++);
        try {
          meet(m_flats[i], m_flats[j]);
        } catch (const Standard_Failure&) {
        }
      }
    }
    Nodes merged{tol, {}, {}};
    of.resize(in.size());
    std::map<std::pair<int, int>, std::vector<int>> between;  // node pair -> pieces, for pieces drawn twice
    for (size_t i = 0; i < in.size(); ++i) {
      tick(i);
      for (const auto& [t0, t1] : parts(m_flats[i], in[i])) {
        Edge e;
        try {
          e = makeEdge({in[i], true, t0, t1}, merged);
        } catch (const Standard_Failure&) {
          continue;
        }
        if (e.a == e.b && length(e.piece) < 4 * tol) continue;  // a sliver left between two cuts
        const auto key = std::minmax(e.a, e.b);
        int same = -1;
        for (int k : between[key])
          if (edges[k].mid.Distance(e.mid) <= 4 * tol) same = k;
        if (same < 0) {
          same = int(edges.size());
          between[key].push_back(same);
          edges.push_back(std::move(e));
        }
        of[i].push_back(same);
      }
    }
    nodes = merged.at.size();
    points = std::move(merged.at);
  }

 private:
  const Plane& m_p;
  double m_tol;
  const std::function<bool()>& m_cancelled;
  std::vector<Flat> m_flats;

  void tick(size_t i) const {
    if (m_cancelled && i % 1024 == 0 && m_cancelled()) throw Error("cancelled");
  }
  P2 flat(const gp_Pnt& q) const { return {m_p.x(q), m_p.y(q)}; }
  P2 at(const Flat& f, double t) const { return flat(f.c.Value(t)); }

  void flatten(const TopoDS_Edge& e, Flat& f) const {
    f.c.Initialize(e);
    f.t0 = f.c.FirstParameter(), f.t1 = f.c.LastParameter();
    f.a = at(f, f.t0), f.b = at(f, f.t1);
    if (f.c.GetType() == GeomAbs_Line) {
      f.type = Flat::Line;
      f.pts = {f.a, f.b}, f.ts = {f.t0, f.t1};
      return;
    }
    if (f.c.GetType() == GeomAbs_Circle) {
      const gp_Circ circle = f.c.Circle();
      const double dz = gp_Vec(circle.Axis().Direction()).Dot(m_p.n);
      if (std::abs(dz) > 1 - 1e-9) {
        const gp_Vec x(circle.XAxis().Direction());
        f.type = Flat::Circle, f.centre = flat(circle.Location()), f.r = circle.Radius(), f.sign = dz > 0 ? 1 : -1;
        f.base = std::atan2(x.Dot(m_p.v), x.Dot(m_p.u));
      }
    }
    GCPnts_TangentialDeflection s(f.c, f.t0, f.t1, 0.1, std::max(m_tol, deflection()), 2);
    for (int k = 1; k <= s.NbPoints(); ++k) f.pts.push_back(flat(s.Value(k))), f.ts.push_back(s.Parameter(k));
  }
  double deflection() const { return m_tol * 100; }

  // The parameter of the point of f nearest q (a guess for curves without a closed form), within f's range.
  double paramOf(const Flat& f, P2 q) const {
    if (f.type == Flat::Line) {
      const double dx = f.b.x - f.a.x, dy = f.b.y - f.a.y, len2 = dx * dx + dy * dy;
      const double s = len2 < 1e-300 ? 0 : std::clamp(((q.x - f.a.x) * dx + (q.y - f.a.y) * dy) / len2, 0.0, 1.0);
      return f.t0 + s * (f.t1 - f.t0);
    }
    if (f.type == Flat::Circle) {
      double t = f.sign * (std::atan2(q.y - f.centre.y, q.x - f.centre.x) - f.base) - f.t0;
      t = f.t0 + (t - 2 * M_PI * std::floor(t / (2 * M_PI)));  // in [t0, t0 + 2 pi)
      if (t > f.t1) t = t - f.t1 < f.t0 + 2 * M_PI - t ? f.t1 : f.t0;  // outside the arc: its nearer end
      return t;
    }
    // Nearest sample piece, then Newton on the curve.
    double best = 1e300, t = f.t0;
    for (size_t k = 0; k + 1 < f.pts.size(); ++k) {
      const P2 a = f.pts[k], b = f.pts[k + 1];
      const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
      const double s = len2 < 1e-300 ? 0 : std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / len2, 0.0, 1.0);
      const double d = std::hypot(a.x + s * dx - q.x, a.y + s * dy - q.y);
      if (d < best) best = d, t = f.ts[k] + s * (f.ts[k + 1] - f.ts[k]);
    }
    for (int i = 0; i < 30; ++i) {
      gp_Pnt pt;
      gp_Vec d1, d2;
      f.c.D2(t, pt, d1, d2);
      const double dx = m_p.x(pt) - q.x, dy = m_p.y(pt) - q.y, ux = d1.Dot(m_p.u), uy = d1.Dot(m_p.v);
      const double g = ux * dx + uy * dy, h = d2.Dot(m_p.u) * dx + d2.Dot(m_p.v) * dy + ux * ux + uy * uy;
      if (std::abs(h) < 1e-300) break;
      const double next = std::clamp(t - g / h, f.t0, f.t1);
      const bool done = std::abs(next - t) <= 1e-14 * (1 + std::abs(t));
      t = next;
      if (done) break;
    }
    return t;
  }
  // Whether q lies on f (within tolerance, its ends included): then t is where.
  bool on(const Flat& f, P2 q, double& t) const {
    t = paramOf(f, q);
    return dist(at(f, t), q) <= m_tol;
  }
  void cut(Flat& f, P2 q) const {
    double t;
    if (on(f, q, t)) f.cuts.push_back(t);
  }
  void cross(Flat& f, Flat& g, P2 q) const {
    double s, t;
    if (on(f, q, s) && on(g, q, t)) f.cuts.push_back(s), g.cuts.push_back(t);
  }

  // Where f and g meet: an end of either on the other, and their crossings.
  void meet(Flat& f, Flat& g) const {
    const double pad = deflection() + m_tol;
    if (f.pts.empty() || g.pts.empty() || f.high.x + pad < g.low.x || g.high.x + pad < f.low.x || f.high.y + pad < g.low.y || g.high.y + pad < f.low.y) return;
    for (const P2 end : {f.a, f.b}) cut(g, end);
    for (const P2 end : {g.a, g.b}) cut(f, end);
    if (f.type == Flat::Line && g.type == Flat::Line) {
      const double dx = f.b.x - f.a.x, dy = f.b.y - f.a.y, ex = g.b.x - g.a.x, ey = g.b.y - g.a.y, den = dx * ey - dy * ex;
      if (std::abs(den) <= 1e-12 * std::hypot(dx, dy) * std::hypot(ex, ey)) return;  // parallel: overlaps are cut at the ends above
      const double s = ((g.a.x - f.a.x) * ey - (g.a.y - f.a.y) * ex) / den;
      cross(f, g, {f.a.x + s * dx, f.a.y + s * dy});
    } else if (f.type == Flat::Line && g.type == Flat::Circle) {
      lineCircle(f, g);
    } else if (f.type == Flat::Circle && g.type == Flat::Line) {
      lineCircle(g, f);
    } else if (f.type == Flat::Circle && g.type == Flat::Circle) {
      const double d = dist(f.centre, g.centre);
      if (d < m_tol || d > f.r + g.r + m_tol || d < std::abs(f.r - g.r) - m_tol) return;  // concentric (overlaps: the ends above), apart, inside
      const double along = (f.r * f.r - g.r * g.r + d * d) / (2 * d), h = std::sqrt(std::max(0.0, f.r * f.r - along * along));
      const double ux = (g.centre.x - f.centre.x) / d, uy = (g.centre.y - f.centre.y) / d;
      const P2 m{f.centre.x + ux * along, f.centre.y + uy * along};
      cross(f, g, {m.x - uy * h, m.y + ux * h});
      if (h > m_tol) cross(f, g, {m.x + uy * h, m.y - ux * h});
    } else {
      sampled(f, g);
    }
  }
  void lineCircle(Flat& l, Flat& c) const {
    const double dx = l.b.x - l.a.x, dy = l.b.y - l.a.y, len = std::hypot(dx, dy);
    if (len < 1e-300) return;
    const double ux = dx / len, uy = dy / len, s = (c.centre.x - l.a.x) * ux + (c.centre.y - l.a.y) * uy;
    const P2 foot{l.a.x + s * ux, l.a.y + s * uy};
    const double h = dist(foot, c.centre);
    if (h > c.r + m_tol) return;
    if (h >= c.r - m_tol) return cross(l, c, foot);  // touching
    const double w = std::sqrt(c.r * c.r - h * h);
    cross(l, c, {foot.x - ux * w, foot.y - uy * w});
    cross(l, c, {foot.x + ux * w, foot.y + uy * w});
  }
  // Crossings of the sample polylines, each refined where both curves meet: Newton on f(s) = g(t).
  void sampled(Flat& f, Flat& g) const {
    const double pad = deflection() + m_tol;
    for (size_t i = 0; i + 1 < f.pts.size(); ++i)
      for (size_t j = 0; j + 1 < g.pts.size(); ++j) {
        const P2 a = f.pts[i], b = f.pts[i + 1], c = g.pts[j], d = g.pts[j + 1];
        if (std::max(a.x, b.x) + pad < std::min(c.x, d.x) || std::max(c.x, d.x) + pad < std::min(a.x, b.x) || std::max(a.y, b.y) + pad < std::min(c.y, d.y) ||
            std::max(c.y, d.y) + pad < std::min(a.y, b.y))
          continue;
        const double dx = b.x - a.x, dy = b.y - a.y, ex = d.x - c.x, ey = d.y - c.y, den = dx * ey - dy * ex;
        if (std::abs(den) < 1e-300) continue;
        const double u = ((c.x - a.x) * ey - (c.y - a.y) * ex) / den, w = ((c.x - a.x) * dy - (c.y - a.y) * dx) / den;
        if (u < -0.5 || u > 1.5 || w < -0.5 || w > 1.5) continue;  // far past these pieces: the neighbours find it
        double s = f.ts[i] + std::clamp(u, 0.0, 1.0) * (f.ts[i + 1] - f.ts[i]), t = g.ts[j] + std::clamp(w, 0.0, 1.0) * (g.ts[j + 1] - g.ts[j]);
        for (int k = 0; k < 30; ++k) {
          gp_Pnt pf, pg;
          gp_Vec df, dg;
          f.c.D1(s, pf, df);
          g.c.D1(t, pg, dg);
          const double fx = m_p.x(pf) - m_p.x(pg), fy = m_p.y(pf) - m_p.y(pg);
          const double ax = df.Dot(m_p.u), ay = df.Dot(m_p.v), bx = -dg.Dot(m_p.u), by = -dg.Dot(m_p.v), det = ax * by - ay * bx;
          if (std::abs(det) < 1e-300) break;
          const double ns = std::clamp(s - (fx * by - fy * bx) / det, f.t0, f.t1), nt = std::clamp(t - (ax * fy - ay * fx) / det, g.t0, g.t1);
          const bool done = std::abs(ns - s) <= 1e-14 * (1 + std::abs(s)) && std::abs(nt - t) <= 1e-14 * (1 + std::abs(t));
          s = ns, t = nt;
          if (done) break;
        }
        if (dist(at(f, s), at(g, t)) <= m_tol) f.cuts.push_back(s), g.cuts.push_back(t);
      }
  }

  // Pairs of edges whose samples come near each other: each sample piece, cut to the grid's cell size, files itself
  // under the cells its padded box touches; two edges in one cell are a pair.
  std::vector<std::pair<int, int>> candidates(double x0, double y0, double extent) const {
    const int side = std::clamp(int(std::sqrt(double(m_flats.size()))), 1, 1024);
    const double cell = std::max(extent / side, m_tol * 8), pad = deflection() + m_tol;
    const long long nx = (long long)(extent / cell) + 2;
    std::vector<std::pair<long long, int>> filed;
    for (size_t i = 0; i < m_flats.size(); ++i) {
      tick(i);
      const auto& pts = m_flats[i].pts;
      for (size_t k = 0; k + 1 < pts.size(); ++k) {
        const int parts = std::max(1, int(std::ceil(dist(pts[k], pts[k + 1]) / cell)));
        for (int s = 0; s < parts; ++s) {
          const double u0 = double(s) / parts, u1 = double(s + 1) / parts;
          const P2 a{pts[k].x + (pts[k + 1].x - pts[k].x) * u0, pts[k].y + (pts[k + 1].y - pts[k].y) * u0};
          const P2 b{pts[k].x + (pts[k + 1].x - pts[k].x) * u1, pts[k].y + (pts[k + 1].y - pts[k].y) * u1};
          const long long cx0 = (long long)std::floor((std::min(a.x, b.x) - pad - x0) / cell), cx1 = (long long)std::floor((std::max(a.x, b.x) + pad - x0) / cell);
          const long long cy0 = (long long)std::floor((std::min(a.y, b.y) - pad - y0) / cell), cy1 = (long long)std::floor((std::max(a.y, b.y) + pad - y0) / cell);
          for (long long cy = cy0; cy <= cy1; ++cy)
            for (long long cx = cx0; cx <= cx1; ++cx) filed.push_back({(cy + 1) * (nx + 2) + cx + 1, int(i)});
        }
      }
    }
    std::sort(filed.begin(), filed.end());
    filed.erase(std::unique(filed.begin(), filed.end()), filed.end());
    std::vector<std::pair<int, int>> pairs;
    for (size_t s = 0; s < filed.size();) {
      size_t e = s;
      while (e < filed.size() && filed[e].first == filed[s].first) ++e;
      for (size_t i = s; i < e; ++i) {
        tick(pairs.size());
        for (size_t j = i + 1; j < e; ++j) pairs.push_back({filed[i].second, filed[j].second});
      }
      s = e;
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
  }

  // The parameter ranges between f's cuts (cuts nearer than the tolerance to each other or an end merged into it).
  std::vector<std::pair<double, double>> parts(Flat& f, const TopoDS_Edge& e) const {
    if (f.pts.empty()) {
      const BRepAdaptor_Curve c(e);
      return {{c.FirstParameter(), c.LastParameter()}};
    }
    std::sort(f.cuts.begin(), f.cuts.end());
    std::vector<double> at{f.t0};
    const gp_Pnt last = f.c.Value(f.t1);
    for (const double t : f.cuts) {
      const gp_Pnt q = f.c.Value(t);
      if (t > at.back() && q.Distance(f.c.Value(at.back())) > m_tol && q.Distance(last) > m_tol) at.push_back(t);
    }
    at.push_back(f.t1);
    std::vector<std::pair<double, double>> out;
    for (size_t k = 0; k + 1 < at.size(); ++k) out.push_back({at[k], at[k + 1]});
    return out;
  }
};

struct Loop {
  std::vector<Piece> pieces;
  double area = 0;  // signed in the plane
};

// The result: the loops' areas (a loop inside an odd number of others is a hole), perimeter and outline.
json loopsResult(const std::vector<Loop>& loops, const Plane& p, double size) {
  std::vector<std::vector<gp_Pnt>> outlines;
  double perimeter = 0;
  int samples = 0;
  for (const Loop& l : loops)
    for (const Piece& piece : l.pieces) samples += samplesFor(piece, size);
  const double keep = std::min(1.0, double(kBoundaryPoints) / std::max(1, samples));
  for (const Loop& l : loops) {
    std::vector<gp_Pnt> pts;
    for (const Piece& piece : l.pieces) {
      perimeter += length(piece);
      sample(piece, pts, int(samplesFor(piece, size) * keep));
    }
    outlines.push_back(std::move(pts));
  }
  auto inside = [&](const gp_Pnt& q, const std::vector<gp_Pnt>& poly) {  // even-odd, in the plane
    bool in = false;
    const double qx = p.x(q), qy = p.y(q);
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
      const double xi = p.x(poly[i]), yi = p.y(poly[i]), xj = p.x(poly[j]), yj = p.y(poly[j]);
      if ((yi > qy) != (yj > qy) && qx < (xj - xi) * (qy - yi) / (yj - yi) + xi) in = !in;
    }
    return in;
  };
  double area = 0;
  int holes = 0;
  gp_XYZ weighted(0, 0, 0);
  for (size_t i = 0; i < loops.size(); ++i) {
    int depth = 0;
    for (size_t j = 0; j < loops.size(); ++j)
      if (i != j && !outlines[i].empty() && outlines[j].size() > 2 && std::abs(loops[j].area) > std::abs(loops[i].area) && inside(outlines[i].front(), outlines[j])) ++depth;
    const double a = std::abs(loops[i].area) * (depth % 2 ? -1 : 1);
    holes += depth % 2;
    area += a;
    gp_XYZ c(0, 0, 0);
    for (const auto& q : outlines[i]) c += q.XYZ();
    if (!outlines[i].empty()) weighted += c / double(outlines[i].size()) * a;
  }
  json boundary = json::array();
  for (const auto& pts : outlines) {
    json loop = json::array();
    for (const auto& q : pts) loop.push_back(pnt(q));
    boundary.push_back(std::move(loop));
  }
  gp_XYZ centre = std::abs(area) > 1e-12 ? weighted / area : (outlines.empty() || outlines[0].empty() ? p.o.XYZ() : outlines[0][0].XYZ());
  if (loops.size() == 1 && outlines[0].size() > 2) {  // the outline's own centroid (its area's), inside a convex loop
    double a2 = 0, cx = 0, cy = 0;
    const auto& poly = outlines[0];
    for (size_t i = 0; i < poly.size(); ++i) {
      const double x0 = p.x(poly[i]), y0 = p.y(poly[i]), x1 = p.x(poly[(i + 1) % poly.size()]), y1 = p.y(poly[(i + 1) % poly.size()]);
      const double cross = x0 * y1 - x1 * y0;
      a2 += cross, cx += (x0 + x1) * cross, cy += (y0 + y1) * cross;
    }
    if (std::abs(a2) > 1e-18) centre = p.o.XYZ() + p.u.XYZ() * (cx / (3 * a2)) + p.v.XYZ() * (cy / (3 * a2));
  }
  return {{"kind", "area"}, {"value", std::abs(area)}, {"unit", "mm2"}, {"perimeter", perimeter}, {"loops", int(loops.size())}, {"holes", holes},
          {"closed", true}, {"boundary", boundary}, {"center", pnt(gp_Pnt(centre))}, {"normal", {p.n.X(), p.n.Y(), p.n.Z()}}};
}

// Leaves only edges that can be on a loop: an end met by one edge (a self-closed edge counts twice) cannot be, unless it
// is held (outside a window: what it leads to is not known).
std::vector<std::vector<int>> prune(std::vector<Edge>& all, size_t nodeCount, const std::vector<bool>& held = {}) {
  std::vector<std::vector<int>> at(nodeCount);
  std::vector<int> degree(nodeCount, 0), stack;
  for (size_t i = 0; i < all.size(); ++i) {
    if (!all[i].alive) continue;
    at[all[i].a].push_back(int(i));
    if (all[i].b != all[i].a) at[all[i].b].push_back(int(i));
    degree[all[i].a]++, degree[all[i].b]++;
  }
  for (size_t n = 0; n < nodeCount; ++n)
    if (degree[n] == 1 && (held.empty() || !held[n])) stack.push_back(int(n));
  while (!stack.empty()) {
    const int n = stack.back();
    stack.pop_back();
    for (int i : at[n])
      if (all[i].alive && degree[n] == 1) {
        all[i].alive = false;
        for (int end : {all[i].a, all[i].b})
          if (--degree[end] == 1 && (held.empty() || !held[end])) stack.push_back(end);
      }
  }
  return at;
}

// A face traced from an edge: its loop, whether it is bounded (counter-clockwise on the left, clockwise on the right;
// else it is the face around a whole drawing or an island) and its box in the plane. No loop: the trace never came back.
struct Face {
  Loop loop;
  bool bounded = false;
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
};

// The faces of the planar graph on each side of the starts: at every end the sharpest turn towards that side, directions
// that leave tangent told apart by how they bend; dangling edges are pruned first (starts pruned give no face), a trace that
// meets a held end gives no loop.
std::vector<Face> traceFaces(std::vector<Edge>& all, const std::vector<int>& starts, const Plane& p, size_t nodeCount, const std::function<bool()>& cancelled,
                             const std::vector<bool>& held = {}) {
  const auto at = prune(all, nodeCount, held);
  auto angle = [&](const gp_Vec& d) { return std::atan2(d.Dot(p.v), d.Dot(p.u)); };
  auto bend = [&](const gp_Vec& d, const gp_Vec& dd) {  // signed curvature, counter-clockwise positive
    const double m = d.Magnitude();
    return m < 1e-12 ? 0.0 : d.Crossed(dd).Dot(p.n) / (m * m * m);
  };
  auto trace = [&](int start, bool left) -> Loop {
    Loop loop;
    Piece cur = all[start].piece;
    bool forward = true;
    int curEdge = start;
    const size_t limit = 2 * all.size() + 2;
    for (size_t step = 0; step < limit; ++step) {
      if (cancelled && step % 256 == 0 && cancelled()) throw Error("cancelled");
      cur.forward = forward;
      loop.pieces.push_back(cur);
      loop.area += green(cur, p);
      const Edge& e = all[curEdge];
      const int node = forward ? e.b : e.a;
      const gp_Vec& backDir = forward ? e.leaveB : e.leaveA;  // the way we came, leaving the node
      const double back = angle(backDir), backBend = bend(backDir, forward ? e.bendB : e.bendA);
      int bestEdge = -1;
      bool bestForward = true;
      double best = 1e9, bestSecond = 1e300;
      for (int i : at[node]) {
        if (!all[i].alive) continue;
        for (const bool f : {true, false}) {
          if ((f ? all[i].a : all[i].b) != node) continue;
          if (i == curEdge && f != forward) continue;  // straight back (unless nothing else leaves)
          const gp_Vec& d = f ? all[i].leaveA : all[i].leaveB;
          const double a = angle(d), k = bend(d, f ? all[i].bendA : all[i].bendB);
          double turn = std::fmod((left ? back - a : a - back) + 4 * M_PI, 2 * M_PI);  // clockwise from the way back = the sharpest left turn
          const double second = left ? backBend - k : k - backBend;                   // a little further along: which way it bends off
          if (turn < kTie || turn > 2 * M_PI - kTie) turn = second > 1e-12 ? 0 : 2 * M_PI;  // leaves the way we came: before or after it
          if (turn < best - kTie || (std::abs(turn - best) <= kTie && second < bestSecond)) best = turn, bestSecond = second, bestEdge = i, bestForward = f;
        }
      }
      if (bestEdge < 0) return {};
      if (bestEdge == start && bestForward) return loop;
      cur = all[bestEdge].piece;
      forward = bestForward;
      curEdge = bestEdge;
    }
    return {};
  };
  std::vector<Face> faces;
  for (const int start : starts) {
    if (start < 0 || !all[start].alive) continue;
    for (const bool left : {true, false}) {
      Face f;
      f.loop = trace(start, left);
      f.bounded = !f.loop.pieces.empty() && (left ? f.loop.area > 1e-12 : f.loop.area < -1e-12);
      std::vector<gp_Pnt> pts;
      for (const Piece& piece : f.loop.pieces) sample(piece, pts, 8);
      for (const gp_Pnt& q : pts) f.x0 = std::min(f.x0, p.x(q)), f.y0 = std::min(f.y0, p.y(q)), f.x1 = std::max(f.x1, p.x(q)), f.y1 = std::max(f.y1, p.y(q));
      faces.push_back(std::move(f));
    }
  }
  return faces;
}

// An edge's box in the plane (a curve's may be loose).
std::array<double, 4> boxOf(const TopoDS_Edge& e, const Plane& p) {
  std::array<double, 4> b{1e300, 1e300, -1e300, -1e300};
  auto add = [&](const gp_Pnt& q) { b = {std::min(b[0], p.x(q)), std::min(b[1], p.y(q)), std::max(b[2], p.x(q)), std::max(b[3], p.y(q))}; };
  const BRepAdaptor_Curve c(e);
  if (c.GetType() == GeomAbs_Line) {
    add(c.Value(c.FirstParameter())), add(c.Value(c.LastParameter()));
    return b;
  }
  Bnd_Box box;
  BRepBndLib::Add(e, box);
  if (box.IsVoid()) return b;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  for (const double x : {x0, x1})
    for (const double y : {y0, y1})
      for (const double z : {z0, z1}) add(gp_Pnt(x, y, z));
  return b;
}

// Closed loops of edges each of whose ends meets exactly one other (none when any end is loose or branches).
std::vector<Loop> walkLoops(const std::vector<Edge>& edges, size_t nodeCount) {
  std::vector<std::vector<int>> at(nodeCount);
  for (size_t i = 0; i < edges.size(); ++i)
    if (edges[i].alive) at[edges[i].a].push_back(int(i)), at[edges[i].b].push_back(int(i));
  for (const auto& list : at)
    if (!list.empty() && list.size() != 2) return {};
  std::vector<Loop> loops;
  std::vector<bool> used(edges.size(), false);
  for (size_t s = 0; s < edges.size(); ++s) {
    if (used[s] || !edges[s].alive) continue;
    Loop loop;
    int cur = int(s), node = edges[s].a;
    while (!used[cur]) {
      used[cur] = true;
      Piece piece = edges[cur].piece;
      piece.forward = edges[cur].a == node;
      loop.pieces.push_back(piece);
      node = piece.forward ? edges[cur].b : edges[cur].a;
      for (int next : at[node])
        if (!used[next]) {
          cur = next;
          break;
        }
    }
    loops.push_back(std::move(loop));
  }
  return loops;
}

gp_Pnt pointOf(const Document& doc, const Scene& scene, const Ref& r) {
  if (r.kind == Ref::Kind::Point) return gp_Pnt(r.point[0], r.point[1], r.point[2]);
  return BRep_Tool::Pnt(TopoDS::Vertex(subshape(node_world_shape(doc, scene, r.body), r.kind, r.index)));
}
}  // namespace

json measure_area(const Document& doc, const Scene& scene, const std::vector<Ref>& refs, const std::function<bool()>& cancelled, const std::optional<Vec3>& clicked) {
  if (refs.empty()) throw Error("Pick fills, closed objects or points around an area");
  const auto kind = [](const Ref& r) {
    return r.kind == Ref::Kind::Face ? 0 : r.kind == Ref::Kind::Edge ? 1 : r.kind == Ref::Kind::Body ? 3 : 2;  // points: vertices, centres, free points
  };
  for (const Ref& r : refs) {
    if (kind(r) == 3) throw Error("Pick fills, faces, objects or points around an area, not whole bodies");
    if (kind(r) != kind(refs.front())) throw Error("Pick fills, objects or points for an area, not a mix of them");
  }
  json refList = json::array();
  for (const Ref& r : refs) refList.push_back(r.str());
  std::map<std::string, TopoDS_Shape> worlds;  // each body's world shape once
  auto world = [&](const std::string& body) -> const TopoDS_Shape& {
    auto it = worlds.find(body);
    if (it == worlds.end()) it = worlds.emplace(body, node_world_shape(doc, scene, body)).first;
    return it->second;
  };
  auto drawing = [&](const Ref& r) {
    const Node* body = scene.node(r.body);
    return body && body->representation == "drawing2d";
  };
  json out;
  if (kind(refs.front()) == 2) {  // the polygon through the points, closed back to the first
    std::vector<gp_Pnt> pts;
    for (const Ref& r : refs) pts.push_back(pointOf(doc, scene, r));
    double perimeter = 0;
    for (size_t i = 0; i + 1 < pts.size(); ++i) perimeter += pts[i].Distance(pts[i + 1]);
    json boundary = json::array();
    for (const auto& q : pts) boundary.push_back(pnt(q));
    if (pts.size() < 3) {
      out = {{"kind", "area"}, {"value", 0.0}, {"unit", "mm2"}, {"perimeter", perimeter}, {"loops", 0}, {"closed", false}, {"open_ends", int(std::min<size_t>(pts.size(), 2))},
             {"boundary", json::array({boundary})}};
    } else {
      perimeter += pts.back().Distance(pts.front());
      const gp_Vec n = newell(pts);
      gp_XYZ c(0, 0, 0);
      for (const auto& q : pts) c += q.XYZ();
      out = {{"kind", "area"}, {"value", n.Magnitude() / 2}, {"unit", "mm2"}, {"perimeter", perimeter}, {"loops", 1}, {"holes", 0}, {"closed", true},
             {"boundary", json::array({boundary})}, {"center", pnt(gp_Pnt(c / double(pts.size())))}};
      if (n.Magnitude() > 1e-12) out["normal"] = {n.X() / n.Magnitude(), n.Y() / n.Magnitude(), n.Z() / n.Magnitude()};
    }
    out["points"] = int(pts.size());
  } else if (kind(refs.front()) == 0) {  // faces: exact areas, every wire's length
    GProp_GProps total;
    double perimeter = 0;
    int holes = 0;
    std::vector<Loop> loops;
    std::vector<TopoDS_Face> faces;
    for (const Ref& r : refs) {
      const TopoDS_Face face = TopoDS::Face(subshape(world(r.body), r.kind, r.index));
      GProp_GProps props;
      BRepGProp::SurfaceProperties(face, props);
      total.Add(props);
      int wires = 0;
      for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next(), ++wires) {
        Loop loop;
        for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) {
          loop.pieces.push_back({e.Current(), e.Current().Orientation() != TopAbs_REVERSED});
          perimeter += length(e.Current());
        }
        loops.push_back(std::move(loop));
      }
      holes += std::max(0, wires - 1);
      faces.push_back(face);
    }
    const double size = std::sqrt(std::max(total.Mass(), 1e-12)) * 4;
    int samples = 0;
    for (const Loop& l : loops)
      for (const Piece& piece : l.pieces) samples += samplesFor(piece, size);
    const double keep = std::min(1.0, double(kBoundaryPoints) / std::max(1, samples));
    json boundary = json::array();
    for (const Loop& l : loops) {
      std::vector<gp_Pnt> pts;
      for (const Piece& piece : l.pieces) sample(piece, pts, int(samplesFor(piece, size) * keep));
      json loop = json::array();
      for (const auto& q : pts) loop.push_back(pnt(q));
      boundary.push_back(std::move(loop));
    }
    out = {{"kind", "area"}, {"value", total.Mass()}, {"unit", "mm2"}, {"perimeter", perimeter}, {"loops", int(loops.size())}, {"holes", holes}, {"closed", true},
           {"boundary", boundary}, {"center", pnt(total.CentreOfMass())}, {"faces", int(faces.size())}};
    const BRepAdaptor_Surface surface(faces.front());
    if (surface.GetType() == GeomAbs_Plane) {
      gp_Dir n = surface.Plane().Axis().Direction();
      if (faces.front().Orientation() == TopAbs_REVERSED) n.Reverse();
      out["normal"] = {n.X(), n.Y(), n.Z()};
    }
  } else {  // edges
    std::vector<TopoDS_Edge> picked;
    for (const Ref& r : refs) picked.push_back(TopoDS::Edge(subshape(world(r.body), r.kind, r.index)));
    Bnd_Box box;
    for (const auto& e : picked) BRepBndLib::Add(e, box);
    const double size = box.IsVoid() ? 1 : std::sqrt(box.SquareExtent());
    Nodes nodes{std::max(1e-6, size * 1e-7), {}, {}};
    auto open = [&](double perimeter, int ends, int branches, const json& loose) {
      json o = {{"kind", "area"}, {"value", 0.0}, {"unit", "mm2"}, {"perimeter", perimeter}, {"loops", 0}, {"closed", false}, {"open_ends", ends}, {"refs", refList},
                {"boundary", json::array()}};
      if (branches) o["branches"] = branches;
      if (!loose.is_null()) o["ends"] = loose;
      return o;
    };
    if (picked.size() == 1 && !BRep_Tool::IsClosed(picked[0])) {
      // One edge that does not close: the cell it bounds among the edges of its drawing body, every edge cut where
      // another crosses or meets it; the part of it under the click (else its middle), the smaller side. Traced among the
      // edges near it, more of them while a face reaches the window's edge (inside the window the cut edges are exact),
      // so a small cell in a big drawing costs what is around it. Cells narrower than a few tolerances (where lines miss
      // a common point by a hair) are noise, not areas.
      const Ref& r = refs.front();
      Edge probe = makeEdge({picked[0]}, nodes);
      if (probe.a != probe.b) {
        if (!drawing(r)) return open(length(picked[0]), 2, 0, json());
        const Plane p = bodyPlane(scene, r.body);
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(world(r.body), TopAbs_EDGE, map);
        std::vector<TopoDS_Edge> edges;
        std::vector<std::array<double, 4>> boxes;
        std::array<double, 4> total{1e300, 1e300, -1e300, -1e300};
        int pickedIndex = -1;
        for (int i = 1; i <= map.Extent(); ++i) {
          if (cancelled && i % 4096 == 0 && cancelled()) throw Error("cancelled");
          const TopoDS_Edge& e = TopoDS::Edge(map(i));
          if (BRep_Tool::Degenerated(e)) continue;
          try {
            boxes.push_back(boxOf(e, p));
          } catch (const Standard_Failure&) {
            continue;
          }
          const auto& b = boxes.back();
          total = {std::min(total[0], b[0]), std::min(total[1], b[1]), std::max(total[2], b[2]), std::max(total[3], b[3])};
          if (e.IsSame(picked[0])) pickedIndex = int(edges.size());
          edges.push_back(e);
        }
        if (pickedIndex < 0) return open(length(picked[0]), 2, 0, json());
        const double extent = std::max(total[2] - total[0], total[3] - total[1]), tol = std::max(1e-6, extent * 1e-7);
        const auto& pb = boxes[pickedIndex];
        gp_Pnt q;  // where on the picked edge: the click, else its middle
        if (clicked) q.SetCoord((*clicked)[0], (*clicked)[1], (*clicked)[2]);
        else q = makeEdge({picked[0], true, 0, -1}, nodes).mid;
        Face found;
        for (double reach = std::max({pb[2] - pb[0], pb[3] - pb[1], extent / 1024, tol});; reach *= 2) {
          const std::array<double, 4> w{pb[0] - reach, pb[1] - reach, pb[2] + reach, pb[3] + reach};
          const bool everything = w[0] <= total[0] && w[1] <= total[1] && w[2] >= total[2] && w[3] >= total[3];
          std::vector<TopoDS_Edge> near;
          int pickedNear = -1;
          for (size_t i = 0; i < edges.size(); ++i)
            if (boxes[i][0] <= w[2] && boxes[i][2] >= w[0] && boxes[i][1] <= w[3] && boxes[i][3] >= w[1]) {
              if (int(i) == pickedIndex) pickedNear = int(near.size());
              near.push_back(edges[i]);
            }
          Arrangement cells(near, p, tol, cancelled);
          int start = -1;  // the part of the picked edge nearest q
          double best = 1e300;
          for (const int s : cells.of[pickedNear]) {
            std::vector<gp_Pnt> pts;
            const Piece& piece = cells.edges[s].piece;
            sample(piece, pts, 32);
            const BRepAdaptor_Curve c(piece.edge);
            pts.push_back(c.Value(span(piece, c).second));
            for (size_t k = 0; k + 1 < pts.size(); ++k) {
              const gp_Vec ab(pts[k], pts[k + 1]), aq(pts[k], q);
              const double len2 = ab.SquareMagnitude(), t = len2 < 1e-300 ? 0 : std::clamp(aq.Dot(ab) / len2, 0.0, 1.0);
              const double d = pts[k].Translated(ab * t).Distance(q);
              if (d < best) best = d, start = s;
            }
          }
          const double margin = reach * 1e-3 + tol;
          std::vector<bool> held(cells.nodes, false);  // ends outside the window may lead on
          if (!everything)
            for (size_t n = 0; n < cells.nodes; ++n) {
              const double x = p.x(cells.points[n]), y = p.y(cells.points[n]);
              held[n] = x <= w[0] + margin || y <= w[1] + margin || x >= w[2] - margin || y >= w[3] - margin;
            }
          const std::vector<Face> faces = traceFaces(cells.edges, {start}, p, cells.nodes, cancelled, held);
          auto inside = [&](const Face& f) {
            return everything || (!f.loop.pieces.empty() && f.x0 > w[0] + margin && f.y0 > w[1] + margin && f.x1 < w[2] - margin && f.y1 < w[3] - margin);
          };
          found = Face();
          for (const Face& f : faces) {
            if (!f.bounded || !inside(f)) continue;
            double perimeter = 0;
            for (const Piece& piece : f.loop.pieces) perimeter += length(piece);
            if (2 * std::abs(f.loop.area) < 64 * tol * perimeter) continue;  // a sliver or a speck
            if (found.loop.pieces.empty() || std::abs(f.loop.area) < std::abs(found.loop.area)) found = f;
          }
          // Settled when every face is inside; with a cell found and many edges taken in, the face that still reaches out
          // on the other side is left (it spans a good part of the drawing).
          if (everything || (!faces.empty() && std::all_of(faces.begin(), faces.end(), inside)) || (!found.loop.pieces.empty() && near.size() > 20000)) break;
        }
        if (found.loop.pieces.empty()) return open(length(picked[0]), 2, 0, json());
        out = loopsResult({found.loop}, p, size);
        out["grown"] = true;
        out["edges"] = int(found.loop.pieces.size());
        out["refs"] = refList;
        return out;
      }
    }
    // The picked edges alone: every end met by exactly two of them, walked into loops.
    std::vector<Edge> edges;
    for (const auto& e : picked) edges.push_back(makeEdge({e}, nodes));
    std::vector<Loop> loops = walkLoops(edges, nodes.at.size());
    bool trimmed = false;
    if (loops.empty()) {
      // Objects drawn past each other (corners that overshoot, a # around a cell): cut where they cross or meet, the
      // parts that lead nowhere left out, when what remains closes. Drawings only (their plane).
      if (std::all_of(refs.begin(), refs.end(), drawing)) {
        const Plane p = bodyPlane(scene, refs.front().body);
        Arrangement cut(picked, p, nodes.tol, cancelled);
        prune(cut.edges, cut.nodes);
        if (std::any_of(cut.edges.begin(), cut.edges.end(), [](const Edge& e) { return e.alive; })) loops = walkLoops(cut.edges, cut.nodes);
        trimmed = !loops.empty();
      }
      if (loops.empty()) {
        std::vector<std::vector<int>> at(nodes.at.size());
        for (size_t i = 0; i < edges.size(); ++i) at[edges[i].a].push_back(int(i)), at[edges[i].b].push_back(int(i));
        int ends = 0, branches = 0;
        json loose = json::array();
        for (size_t n = 0; n < at.size(); ++n) {
          ends += at[n].size() == 1, branches += at[n].size() > 2;
          if (at[n].size() == 1) loose.push_back(pnt(nodes.at[n]));
        }
        double perimeter = 0;
        for (const auto& e : picked) perimeter += length(e);
        return open(perimeter, ends, branches, loose);
      }
    }
    // The plane of the loop that encloses the most (Newell's normal): the others are measured in it.
    gp_Vec normal(0, 0, 0);
    gp_XYZ c(0, 0, 0);
    for (const Loop& l : loops) {
      std::vector<gp_Pnt> pts;
      for (const Piece& piece : l.pieces) sample(piece, pts, samplesFor(piece, size) / 4 + 1);
      const gp_Vec n = newell(pts);
      if (n.Magnitude() > normal.Magnitude()) {
        normal = n;
        c = gp_XYZ(0, 0, 0);
        for (const auto& q : pts) c += q.XYZ();
        c /= double(std::max<size_t>(1, pts.size()));
      }
    }
    const Plane p = plane(gp_Pnt(c), normal);
    for (Loop& l : loops)
      for (const Piece& piece : l.pieces) l.area += green(piece, p);
    out = loopsResult(loops, p, size);
    out["edges"] = int(picked.size());
    if (trimmed) out["trimmed"] = true;
  }
  out["refs"] = refList;
  return out;
}

}  // namespace opad
