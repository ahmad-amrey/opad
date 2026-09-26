#include "opad/mass.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Domain.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp_MeshProps.hxx>
#include <BRepGProp_Sinert.hxx>
#include <BRepGProp_Vinert.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2d_Curve.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <math.hxx>
#include <math_Vector.hxx>

#include <algorithm>
#include <mutex>
#include <string>
#include <cmath>
#include <vector>

#include "opad/util.hpp"

namespace opad {
namespace {

// Gauss-Legendre rules on [-1, 1]: 8 points where a direction has few pieces, 5 where it has many (a bicubic span's
// volume integrand is of degree 8 in each parameter, which 5 points integrate exactly).
struct Rule {
  int n = 0;
  double x[8] = {}, w[8] = {};
};

Rule make_rule(int n) {
  Rule r;
  r.n = n;
  math_Vector x(1, n), w(1, n);
  math::GaussPoints(n, x);
  math::GaussWeights(n, w);
  for (int i = 0; i < n; ++i) {
    r.x[i] = x(i + 1);
    r.w[i] = w(i + 1);
  }
  return r;
}

const Rule& rule_for(size_t pieces) {
  static const Rule fine = make_rule(8), coarse = make_rule(5);
  return pieces > 8 ? coarse : fine;
}

// One face's integral: the mass and its first moments.
struct Sums {
  double m = 0, x = 0, y = 0, z = 0;
  void add(const Sums& o, double k) {
    m += k * o.m;
    x += k * o.x;
    y += k * o.y;
    z += k * o.z;
  }
};

// Break points of [a, b]: `inner` ones kept in order, then pieces longer than `step` (if any) cut evenly.
std::vector<double> pieces_of(double a, double b, const std::vector<double>& inner, double step) {
  std::vector<double> points{a};
  for (double t : inner)
    if (t > points.back() + 1e-12 && t < b - 1e-12) points.push_back(t);
  points.push_back(b);
  if (step <= 0) return points;
  std::vector<double> out{points.front()};
  for (size_t i = 0; i + 1 < points.size(); ++i) {
    const int n = std::max(1, static_cast<int>(std::ceil((points[i + 1] - points[i]) / step - 1e-9)));
    for (int k = 1; k <= n; ++k) out.push_back(points[i] + (points[i + 1] - points[i]) * k / n);
  }
  return out;
}

// Where one Gauss rule must not reach across in a direction of the surface: every knot of a B-spline (its CN
// intervals, which swept and offset surfaces take from their basis), and an eighth of a turn for angles.
std::vector<double> surface_pieces(const BRepAdaptor_Surface& s, bool u) {
  const double a = u ? s.FirstUParameter() : s.FirstVParameter(), b = u ? s.LastUParameter() : s.LastVParameter();
  std::vector<double> knots;
  const int n = u ? s.NbUIntervals(GeomAbs_CN) : s.NbVIntervals(GeomAbs_CN);
  if (n > 1) {
    TColStd_Array1OfReal t(1, n + 1);
    if (u) s.UIntervals(t, GeomAbs_CN);
    else s.VIntervals(t, GeomAbs_CN);
    for (int i = t.Lower(); i <= t.Upper(); ++i) knots.push_back(t(i));
  }
  double step = 0;
  const GeomAbs_SurfaceType type = s.GetType();
  if (u ? s.IsUPeriodic() : s.IsVPeriodic()) step = (u ? s.UPeriod() : s.VPeriod()) / 8;
  else if (!u && type == GeomAbs_Sphere) step = M_PI / 8;
  else if (type == GeomAbs_OtherSurface || type == GeomAbs_OffsetSurface) step = (b - a) / 4;
  return pieces_of(a, b, knots, step);
}

// The same along a boundary curve in the face's parameters.
std::vector<double> curve_pieces(const Geom2dAdaptor_Curve& c) {
  const double a = c.FirstParameter(), b = c.LastParameter();
  std::vector<double> knots;
  const int n = c.NbIntervals(GeomAbs_CN);
  if (n > 1) {
    TColStd_Array1OfReal t(1, n + 1);
    c.Intervals(t, GeomAbs_CN);
    for (int i = t.Lower(); i <= t.Upper(); ++i) knots.push_back(t(i));
  }
  double step = 0;
  if (c.IsPeriodic()) step = c.Period() / 8;
  else if (c.GetType() == GeomAbs_OtherCurve || c.GetType() == GeomAbs_OffsetCurve) step = (b - a) / 4;
  return pieces_of(a, b, knots, step);
}

// Cuts the curve's pieces where it crosses an interior knot of the surface in the outer direction, so that the
// outer rule never spans one either (found by bisection; a piece is short enough to be monotonic there).
void add_crossings(const Geom2dAdaptor_Curve& c, std::vector<double>& ts, const std::vector<double>& knots, bool outerV) {
  if (knots.size() <= 2) return;
  auto coord = [&](double t) {
    const gp_Pnt2d p = c.Value(t);
    return outerV ? p.Y() : p.X();
  };
  std::vector<double> out{ts.front()};
  for (size_t i = 0; i + 1 < ts.size(); ++i) {
    const double a = ts[i], b = ts[i + 1], ca = coord(a), cb = coord(b);
    std::vector<double> cuts;
    for (size_t k = 1; k + 1 < knots.size(); ++k) {
      const double kn = knots[k];
      if ((kn - ca) * (kn - cb) >= 0) continue;
      double lo = a, hi = b;
      const bool rising = cb > ca;
      for (int it = 0; it < 60 && hi - lo > 1e-14 * std::max(1.0, std::abs(hi)); ++it) {
        const double mid = 0.5 * (lo + hi);
        if ((coord(mid) < kn) == rising) lo = mid;
        else hi = mid;
      }
      cuts.push_back(0.5 * (lo + hi));
    }
    std::sort(cuts.begin(), cuts.end());
    for (double t : cuts)
      if (t > out.back() + 1e-12 && t < b - 1e-12) out.push_back(t);
    out.push_back(b);
  }
  ts = std::move(out);
}

// The integral over the face's domain, by Green's theorem: along the boundary, the integral of the integrand from
// the domain's lower bound in the inner direction. The inner direction is the one with more pieces, so the outer
// rule only has to respect the other direction's knots. `f(u, v)` returns the integrand's Sums at (u, v).
template <class F>
Sums integrate_face(const TopoDS_Face& face, const BRepAdaptor_Surface& s, F&& f) {
  const std::vector<double> us = surface_pieces(s, true), vs = surface_pieces(s, false);
  const bool innerU = us.size() >= vs.size();
  const std::vector<double>& inner = innerU ? us : vs;
  const std::vector<double>& outerKnots = innerU ? vs : us;
  const Rule& ri = rule_for(inner.size() - 1);
  auto along = [&](double to, double other) {
    Sums sum;
    for (size_t k = 0; k + 1 < inner.size() && inner[k] < to; ++k) {
      const double a = inner[k], b = std::min(inner[k + 1], to), mid = 0.5 * (a + b), half = 0.5 * (b - a);
      for (int i = 0; i < ri.n; ++i) {
        const double t = mid + half * ri.x[i];
        sum.add(innerU ? f(t, other) : f(other, t), half * ri.w[i]);
      }
    }
    return sum;
  };
  Sums total;
  // dA = F dv with F integrated along u, or -G du with G integrated along v, at p moving by d.
  auto at = [&](const gp_Pnt2d& p, const gp_Vec2d& d, double weight) {
    const double step = innerU ? d.Y() : -d.X();
    if (step != 0) total.add(innerU ? along(p.X(), p.Y()) : along(p.Y(), p.X()), weight * step);
  };
  const TopoDS_Face forward = TopoDS::Face(face.Oriented(TopAbs_FORWARD));
  auto edge_sums = [&](const TopoDS_Edge& edge, gp_Pnt2d* from, gp_Pnt2d* to) {
    double first = 0, last = 0;
    const Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(edge, forward, first, last);
    if (pc.IsNull()) throw Standard_Failure("no curve on the face");
    const Geom2dAdaptor_Curve c(pc, first, last);
    std::vector<double> ts = curve_pieces(c);
    add_crossings(c, ts, outerKnots, innerU);
    const Rule& ro = rule_for(ts.size() - 1);
    const bool reversed = edge.Orientation() == TopAbs_REVERSED;
    for (size_t k = 0; k + 1 < ts.size(); ++k) {
      const double mid = 0.5 * (ts[k] + ts[k + 1]), half = 0.5 * (ts[k + 1] - ts[k]);
      for (int i = 0; i < ro.n; ++i) {
        gp_Pnt2d p;
        gp_Vec2d d;
        c.D1(mid + half * ro.x[i], p, d);
        at(p, d, (reversed ? -half : half) * ro.w[i]);
      }
    }
    if (from) *from = c.Value(reversed ? last : first);
    if (to) *to = c.Value(reversed ? first : last);
  };
  // Consecutive curves of a wire may not meet in the parameters (they meet within the edges' tolerance in space,
  // which can be a visible gap on the surface). A straight step closes each gap: with an open loop the result
  // would depend on the inner direction.
  auto gap = [&](const gp_Pnt2d& a, const gp_Pnt2d& b) {
    if (a.SquareDistance(b) < 1e-24) return;
    const Rule& r = rule_for(1);
    const gp_Vec2d d(a, b);
    for (int i = 0; i < r.n; ++i) at(gp_Pnt2d(a.XY() + d.XY() * (0.5 + 0.5 * r.x[i])), d, 0.5 * r.w[i]);
  };
  auto boundary = [](const TopoDS_Shape& edge) { return edge.Orientation() == TopAbs_FORWARD || edge.Orientation() == TopAbs_REVERSED; };
  for (TopExp_Explorer w(forward, TopAbs_WIRE); w.More(); w.Next()) {
    const TopoDS_Wire& wire = TopoDS::Wire(w.Current());
    int edges = 0, ordered = 0;
    for (TopExp_Explorer e(wire, TopAbs_EDGE); e.More(); e.Next()) edges += boundary(e.Current());
    for (BRepTools_WireExplorer e(wire, forward); e.More(); e.Next()) ordered += boundary(e.Current());
    if (ordered != edges) {  // not a chain the explorer can follow: the edges as they come, no gaps closed
      for (TopExp_Explorer e(wire, TopAbs_EDGE); e.More(); e.Next())
        if (boundary(e.Current())) edge_sums(TopoDS::Edge(e.Current()), nullptr, nullptr);
      continue;
    }
    bool started = false;
    gp_Pnt2d start, end;
    for (BRepTools_WireExplorer e(wire, forward); e.More(); e.Next()) {
      if (!boundary(e.Current())) continue;
      gp_Pnt2d from, to;
      edge_sums(e.Current(), &from, &to);
      if (started) gap(end, from);
      else start = from;
      started = true;
      end = to;
    }
    if (started) gap(end, start);
  }
  return total;
}

Sums about(double mass, const gp_Pnt& centre, const gp_Pnt& o) {
  const gp_Vec c(o, centre);
  return {mass, mass * c.X(), mass * c.Y(), mass * c.Z()};
}

// One face's sums about `o`: its area, or (volume) the cone from `o` over it, which the faces of a closed shell add
// up to the enclosed volume. Faces of mesh bodies have only triangles, integrated as BRepGProp does.
Sums face_sums(const TopoDS_Face& face, const gp_Pnt& o, bool volume) {
  TopLoc_Location loc;
  if (BRep_Tool::Surface(face, loc).IsNull()) {
    const Handle(Poly_Triangulation)& mesh = BRep_Tool::Triangulation(face, loc);
    if (mesh.IsNull() || mesh->NbTriangles() == 0) return {};
    BRepGProp_MeshProps props(volume ? BRepGProp_MeshProps::Vinert : BRepGProp_MeshProps::Sinert);
    props.SetLocation(o);
    props.Perform(mesh, loc, face.Orientation());
    return about(props.Mass(), props.CentreOfMass(), o);
  }
  try {
    const BRepAdaptor_Surface surface(face);
    const double outward = face.Orientation() == TopAbs_REVERSED ? -1.0 : 1.0;
    return integrate_face(face, surface, [&](double u, double v) {
      gp_Pnt p;
      gp_Vec du, dv;
      surface.D1(u, v, p, du, dv);
      const gp_Vec n = du.Crossed(dv), r(o, p);
      if (!volume) {
        const double a = n.Magnitude();
        return Sums{a, a * r.X(), a * r.Y(), a * r.Z()};
      }
      // Divergence theorem: the volume is 1/3 of r.n dA and its moments 1/4 of r (r.n) dA.
      const double rn = outward * r.Dot(n);
      return Sums{rn / 3, r.X() * rn / 4, r.Y() * rn / 4, r.Z() * rn / 4};
    });
  } catch (const Standard_Failure&) {
    // A face this cannot walk (no curve on the surface for an edge): BRepGProp's own integration.
    BRepGProp_Face bf(face);
    BRepGProp_Domain bd(face);
    if (volume) {
      BRepGProp_Vinert g(bf, bd, o);
      return about(g.Mass(), g.CentreOfMass(), o);
    }
    BRepGProp_Sinert g(bf, bd, o);
    return about(g.Mass(), g.CentreOfMass(), o);
  }
}

// The faces side by side, summed in order afterwards so that the result does not depend on the threads. Like
// BRepGProp, the volume leaves out internal and external faces, and the sums are taken about the average vertex.
MassProperties properties(const TopoDS_Shape& shape, bool volume) {
  std::vector<TopoDS_Face> faces;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next())
    if (!volume || e.Current().Orientation() == TopAbs_FORWARD || e.Current().Orientation() == TopAbs_REVERSED)
      faces.push_back(TopoDS::Face(e.Current()));
  gp_XYZ sum(0, 0, 0);
  int vertices = 0;
  for (TopExp_Explorer e(shape, TopAbs_VERTEX); e.More(); e.Next(), ++vertices) sum += BRep_Tool::Pnt(TopoDS::Vertex(e.Current())).XYZ();
  const gp_Pnt o(vertices ? sum / vertices : sum);
  std::vector<Sums> parts(faces.size());
  std::mutex mu;
  std::string failure;
  OSD_Parallel::For(0, static_cast<int>(faces.size()), [&](int i) {
    std::string error;
    try {
      parts[static_cast<size_t>(i)] = face_sums(faces[static_cast<size_t>(i)], o, volume);
      return;
    } catch (const Standard_Failure& e) {
      error = e.GetMessageString();
    } catch (const std::exception& e) {
      error = e.what();
    }
    std::lock_guard<std::mutex> lock(mu);
    if (failure.empty()) failure = "face " + std::to_string(i + 1) + ": " + (error.empty() ? "kernel failure" : error);
  }, faces.size() < 2);
  if (!failure.empty()) throw Error(std::string(volume ? "the volume could not be integrated (" : "the area could not be integrated (") + failure + ")");
  Sums total;
  for (const auto& p : parts) total.add(p, 1.0);
  MassProperties out;
  out.mass = total.m;
  out.centre = std::abs(total.m) > 0 ? gp_Pnt(o.XYZ() + gp_XYZ(total.x, total.y, total.z) / total.m) : o;
  return out;
}

}  // namespace

MassProperties area_properties(const TopoDS_Shape& shape) { return properties(shape, false); }

MassProperties volume_properties(const TopoDS_Shape& shape) { return properties(shape, true); }

}  // namespace opad

