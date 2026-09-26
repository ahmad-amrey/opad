#include "opad/design/sketch_modify.hpp"
#include "opad/design/sketch_geom.hpp"

#include <BOPAlgo_Tools.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_HArray1OfBoolean.hxx>
#include <TColgp_Array1OfVec.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>

#include <algorithm>
#include <cmath>

namespace opad::design {

namespace {
gp_Pnt pnt(const Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }
gp_Dir dir(const Vec3& v) { return gp_Dir(v[0], v[1], v[2]); }
gp_Pnt world(const Frame& f, const SkPoint& p) { return pnt(f.to_world(p.x, p.y)); }

}  // namespace

// The closed C2 cubic spline through the points, knots at chord lengths (gap log #5). GeomAPI_Interpolate's periodic
// mode fixes an estimated tangent at the first point, so its curve is only C1 there and moves with the seam (up to
// 0.05 mm on a 24-point outline); this one is the same curve whichever point comes first or which way they run.
// Control points d from Farin's C2 interpolation conditions, cyclic: a_i d[i-1] + b_i d[i] + c_i d[i+1] = (h[i-1] +
// h[i]) P[i], solved as a cyclic tridiagonal system (Sherman-Morrison).
Handle(Geom_BSplineCurve) closed_spline(const std::vector<gp_Pnt>& p) {
  const int n = static_cast<int>(p.size());
  const size_t count = p.size();
  auto at = [n](int i) { return size_t(((i % n) + n) % n); };
  std::vector<double> h(count);
  for (int i = 0; i < n; ++i) h[size_t(i)] = p[size_t(i)].Distance(p[at(i + 1)]);
  for (double d : h)
    if (d < 1e-12) return nullptr;
  std::vector<double> a(count), b(count), c(count);
  std::vector<gp_XYZ> r(count);
  for (int i = 0; i < n; ++i) {
    const double h2 = h[at(i - 2)], h1 = h[at(i - 1)], h0 = h[at(i)], hn = h[at(i + 1)];
    const double s1 = h2 + h1 + h0, s2 = h1 + h0 + hn;
    a[size_t(i)] = h0 * h0 / s1;
    c[size_t(i)] = h1 * h1 / s2;
    b[size_t(i)] = h0 * (h2 + h1) / s1 + h1 * (h0 + hn) / s2;
    r[size_t(i)] = p[size_t(i)].XYZ() * (h1 + h0);
  }
  // Thomas on the tridiagonal part (a[0] and c[n-1] left out), for one right-hand side.
  auto tridiagonal = [&](const std::vector<double>& diag, const std::vector<gp_XYZ>& rhs) {
    std::vector<double> cp(count);
    std::vector<gp_XYZ> x(count);
    cp[0] = c[0] / diag[0];
    x[0] = rhs[0] / diag[0];
    for (int i = 1; i < n; ++i) {
      const double m = diag[size_t(i)] - a[size_t(i)] * cp[size_t(i) - 1];
      cp[size_t(i)] = c[size_t(i)] / m;
      x[size_t(i)] = (rhs[size_t(i)] - x[size_t(i) - 1] * a[size_t(i)]) / m;
    }
    for (int i = n - 2; i >= 0; --i) x[size_t(i)] -= x[size_t(i) + 1] * cp[size_t(i)];
    return x;
  };
  const double gamma = -b[0], alpha = c[size_t(n) - 1], beta = a[0];
  std::vector<double> diag = b;
  diag[0] -= gamma;
  diag[size_t(n) - 1] -= alpha * beta / gamma;
  const std::vector<gp_XYZ> x = tridiagonal(diag, r);
  std::vector<gp_XYZ> u(count, gp_XYZ(0, 0, 0));
  u[0] = gp_XYZ(gamma, gamma, gamma);
  u[size_t(n) - 1] = gp_XYZ(alpha, alpha, alpha);
  const std::vector<gp_XYZ> z = tridiagonal(diag, u);
  std::vector<gp_Pnt> d(count);
  for (int k = 1; k <= 3; ++k) {  // per coordinate: x - z (x0 + beta x[n-1] / gamma) / (1 + z0 + beta z[n-1] / gamma)
    const double fact = (x[0].Coord(k) + beta * x[size_t(n) - 1].Coord(k) / gamma) / (1 + z[0].Coord(k) + beta * z[size_t(n) - 1].Coord(k) / gamma);
    for (int i = 0; i < n; ++i) d[size_t(i)].SetCoord(k, x[size_t(i)].Coord(k) - fact * z[size_t(i)].Coord(k));
  }
  TColStd_Array1OfReal knots(1, n + 1);
  TColStd_Array1OfInteger mults(1, n + 1);
  double t = 0;
  for (int i = 0; i <= n; ++i) {
    knots.SetValue(i + 1, t);
    mults.SetValue(i + 1, 1);
    if (i < n) t += h[size_t(i)];
  }
  // Farin's d_i and OCCT's first periodic pole differ by a fixed shift: take the one that meets the points.
  for (int shift = 0; shift < n; ++shift) {
    TColgp_Array1OfPnt poles(1, n);
    for (int i = 0; i < n; ++i) poles.SetValue(i + 1, d[at(i + shift)]);
    Handle(Geom_BSplineCurve) curve = new Geom_BSplineCurve(poles, knots, mults, 3, Standard_True);
    double worst = 0;
    for (int i = 0; i < n && worst < 1e-7; ++i) worst = std::max(worst, curve->Value(knots(i + 1)).Distance(p[size_t(i)]));
    if (worst < 1e-7) return curve;
  }
  return nullptr;
}
gp_Ax3 frame_ax3(const Frame& f) { return gp_Ax3(pnt(f.origin), dir(f.normal()), dir(f.x)); }
gp_Pln frame_plane(const Frame& f) { return gp_Pln(frame_ax3(f)); }

Frame frame_from_ax3(const gp_Ax3& a) {
  Frame f;
  f.origin = {a.Location().X(), a.Location().Y(), a.Location().Z()};
  f.x = {a.XDirection().X(), a.XDirection().Y(), a.XDirection().Z()};
  f.y = {a.YDirection().X(), a.YDirection().Y(), a.YDirection().Z()};
  return f;
}

Frame base_frame(const std::string& base) {
  Frame f;
  // Looking at each plane from its positive normal, x runs to the right: XY from +Z, XZ (front) from -Y,
  // YZ (right) from +X, the same way the view cube's TOP / FRONT / RIGHT faces show them.
  if (base == "xz") { f.x = {1, 0, 0}; f.y = {0, 0, 1}; }
  else if (base == "yz") { f.x = {0, 1, 0}; f.y = {0, 0, 1}; }
  else if (base != "xy") throw Error("unknown base plane \"" + base + "\" (xy, xz or yz)");
  return f;
}

TopoDS_Edge entity_edge(const Sketch& sk, const SkEntity& e, const Frame& frame) {
  auto P = [&](size_t i) -> const SkPoint& {
    const SkPoint* p = i < e.p.size() ? sk.point(e.p[i]) : nullptr;
    if (!p) throw Error("sketch entity " + std::to_string(e.id) + " refers to a missing point");
    return *p;
  };
  try {
    switch (e.type) {
      case SkEntity::Type::Point:
        return TopoDS_Edge();
      case SkEntity::Type::Line: {
        const gp_Pnt a = world(frame, P(0)), b = world(frame, P(1));
        if (a.Distance(b) < 1e-7) return TopoDS_Edge();
        return BRepBuilderAPI_MakeEdge(a, b).Edge();
      }
      case SkEntity::Type::Circle: {
        if (e.r < 1e-7) return TopoDS_Edge();
        return BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(world(frame, P(0)), dir(frame.normal()), dir(frame.x)), e.r)).Edge();
      }
      case SkEntity::Type::Arc: {
        const SkPoint &c = P(0), &s = P(1), &t = P(2);
        const double r = std::hypot(s.x - c.x, s.y - c.y);
        if (r < 1e-7) return TopoDS_Edge();
        const double a0 = std::atan2(s.y - c.y, s.x - c.x);
        double a1 = std::atan2(t.y - c.y, t.x - c.x);
        while (a1 <= a0 + 1e-12) a1 += 2 * M_PI;
        return BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(world(frame, c), dir(frame.normal()), dir(frame.x)), r), a0, a1).Edge();
      }
      case SkEntity::Type::Ellipse: {
        const SkPoint &c = P(0), &m = P(1);
        double major = std::hypot(m.x - c.x, m.y - c.y), minor = e.r;
        if (major < 1e-7 || minor < 1e-7) return TopoDS_Edge();
        Vec3 axis = frame.to_world(m.x, m.y);
        const Vec3 o = frame.to_world(c.x, c.y);
        gp_Dir xd(axis[0] - o[0], axis[1] - o[1], axis[2] - o[2]);
        if (minor > major) {  // gp_Elips wants major >= minor: turn the axes
          std::swap(major, minor);
          xd = dir(frame.normal()).Crossed(xd);
        }
        return BRepBuilderAPI_MakeEdge(gp_Elips(gp_Ax2(pnt(o), dir(frame.normal()), xd), major, minor)).Edge();
      }
      case SkEntity::Type::Spline: {
        if (e.p.size() < 2) return TopoDS_Edge();
        if (e.degree) {
          TColgp_Array1OfPnt poles(1,int(e.p.size())); TColStd_Array1OfReal weights(1,int(e.p.size())),knots(1,int(e.knots.size()));
          TColStd_Array1OfInteger mults(1,int(e.knots.size()));
          for(size_t i=0;i<e.p.size();++i) { poles.SetValue(int(i)+1,world(frame,P(i))); weights.SetValue(int(i)+1,e.weights[i]); }
          for(size_t i=0;i<e.knots.size();++i) { knots.SetValue(int(i)+1,e.knots[i]); mults.SetValue(int(i)+1,e.multiplicities[i]); }
          Handle(Geom_BSplineCurve) curve=new Geom_BSplineCurve(poles,weights,knots,mults,e.degree,e.periodic);
          return BRepBuilderAPI_MakeEdge(curve).Edge();
        }
        // Closed, C2 through the seam (gap log #5): the first point id repeated at the end, `periodic`, or a last point
        // lying on the first (the same point given twice).
        const bool repeated = e.p.size() > 2 && e.p.front() == e.p.back();
        int n = static_cast<int>(e.p.size()) - (repeated ? 1 : 0);
        const bool coincident = !repeated && n > 3 && std::hypot(P(0).x - P(size_t(n) - 1).x, P(0).y - P(size_t(n) - 1).y) < 1e-9;
        if (coincident) --n;
        const bool closed = repeated || coincident || e.periodic;
        if (closed && n >= 4) {
          std::vector<gp_Pnt> points;
          for (int i = 0; i < n; ++i) points.push_back(world(frame, P(static_cast<size_t>(i))));
          if (Handle(Geom_BSplineCurve) curve = closed_spline(points); !curve.IsNull()) return BRepBuilderAPI_MakeEdge(curve).Edge();
        }
        Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, n);
        for (int i = 0; i < n; ++i) pts->SetValue(i + 1, world(frame, P(static_cast<size_t>(i))));
        GeomAPI_Interpolate fit(pts, closed, 1e-7);
        if (!closed && (!e.start_tangent.empty() || !e.end_tangent.empty())) {
          // End directions, in the sketch's plane; the magnitude follows the point spacing.
          TColgp_Array1OfVec tangents(1, n);
          Handle(TColStd_HArray1OfBoolean) given = new TColStd_HArray1OfBoolean(1, n, Standard_False);
          const Vec3 o = frame.to_world(0, 0);
          auto direction = [&](const std::vector<double>& t) {
            const Vec3 w = frame.to_world(t[0], t[1]);
            return gp_Vec(w[0] - o[0], w[1] - o[1], w[2] - o[2]);
          };
          if (!e.start_tangent.empty()) { tangents.SetValue(1, direction(e.start_tangent)); given->SetValue(1, Standard_True); }
          if (!e.end_tangent.empty()) { tangents.SetValue(n, direction(e.end_tangent)); given->SetValue(n, Standard_True); }
          fit.Load(tangents, given, Standard_True);
        }
        fit.Perform();
        if (!fit.IsDone()) return TopoDS_Edge();
        return BRepBuilderAPI_MakeEdge(fit.Curve()).Edge();
      }
    }
  } catch (const Standard_Failure&) {
  }
  return TopoDS_Edge();
}

std::vector<TopoDS_Edge> sketch_edges(const Sketch& sk, const Frame& frame, bool with_construction) {
  std::vector<TopoDS_Edge> out;
  for (const auto& e : sk.entities) {
    if (e.construction && !with_construction) continue;
    TopoDS_Edge edge = entity_edge(sk, e, frame);
    if (!edge.IsNull()) out.push_back(edge);
  }
  return out;
}

namespace {
// A point well inside the face: the centroid when it is inside, otherwise the deepest of a grid of candidates
// (rings and L shapes have their centroid outside).
bool interior_point(const TopoDS_Face& face, const Frame& frame, double& u, double& v) {
  Bnd_Box box;
  BRepBndLib::Add(face, box);
  if (box.IsVoid()) return false;
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  BRepClass_FaceClassifier cls;
  auto accept = [&](const gp_Pnt& p) {
    cls.Perform(face,p,1e-7);
    if(cls.State()!=TopAbs_IN) return false;
    frame.to_local({p.X(),p.Y(),p.Z()},u,v); return true;
  };
  if(accept(props.CentreOfMass())) return true;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
  for (int i = 0; i < 8; ++i) {
    double pu, pv;
    frame.to_local({i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0}, pu, pv);
    u0 = std::min(u0, pu); u1 = std::max(u1, pu);
    v0 = std::min(v0, pv); v1 = std::max(v1, pv);
  }
  // Thin rings and slivers may lie between all grid samples. Probe on both sides
  // of a boundary tangent, shrinking the offset until the classifier finds the face.
  const double scale=std::max(u1-u0,v1-v0);
  for(TopExp_Explorer ex(face,TopAbs_EDGE);ex.More();ex.Next()) {
    BRepAdaptor_Curve curve(TopoDS::Edge(ex.Current()));gp_Pnt at;gp_Vec tangent;
    curve.D1((curve.FirstParameter()+curve.LastParameter())*0.5,at,tangent);
    gp_Vec inward=gp_Vec(dir(frame.normal())).Crossed(tangent);
    if(inward.SquareMagnitude()<1e-20) continue;
    inward.Normalize();
    for(double offset=scale*0.001;offset>1e-7;offset*=0.25)
      if(accept(at.Translated(inward*offset)) || accept(at.Translated(inward*-offset))) return true;
  }
  // Classification is sufficient: no all-edge distance query for every grid point.
  for (int n : {7, 15, 41, 101})
    for(int i=1;i<n;++i) for(int k=1;k<n;++k)
      if(accept(pnt(frame.to_world(u0+(u1-u0)*i/n,v0+(v1-v0)*k/n)))) return true;
  return false;
}
}  // namespace

std::vector<Region> sketch_regions(const Sketch& sk, const Frame& frame) {
  std::vector<Region> out;
  const std::vector<TopoDS_Edge> edges = sketch_edges(sk, frame);
  if (edges.empty()) return out;
  TopoDS_Compound comp;
  BRep_Builder bb;
  bb.MakeCompound(comp);
  for (const auto& e : edges) bb.Add(comp, e);
  try {
    // EdgesToWires intersects the edges with each other first (unshared edges), so crossing curves split
    // into the minimal regions; WiresToFaces then sorts the loops into outer boundaries and holes.
    TopoDS_Shape wires, faces;
    if (BOPAlgo_Tools::EdgesToWires(comp, wires, Standard_False, 1e-8) != 0) return out;
    if (!BOPAlgo_Tools::WiresToFaces(wires, faces, 1e-8)) return out;
    for (TopExp_Explorer ex(faces, TopAbs_FACE); ex.More(); ex.Next()) {
      Region r;
      r.face = TopoDS::Face(ex.Current());
      // Faces come out with the normal of whatever loop was found first: make them all face the sketch normal.
      BRepAdaptor_Surface surf(r.face);
      if (surf.GetType() == GeomAbs_Plane) {
        gp_Dir n = surf.Plane().Axis().Direction();
        if (r.face.Orientation() == TopAbs_REVERSED) n.Reverse();
        if (n.Dot(dir(frame.normal())) < 0) r.face.Reverse();
      }
      GProp_GProps props;
      BRepGProp::SurfaceProperties(r.face, props);
      r.area = std::fabs(props.Mass());
      if (r.area < 1e-9) continue;
      if (!interior_point(r.face, frame, r.u, r.v)) continue;
      out.push_back(r);
    }
  } catch (const Standard_Failure&) {
    out.clear();
  }
  identify_regions(sk,out,frame);
  // A stable order (the kernel's depends on hashing): by interior point.
  std::sort(out.begin(), out.end(), [](const Region& a, const Region& b) { return a.u != b.u ? a.u < b.u : a.v < b.v; });
  return out;
}

int region_at(const std::vector<Region>& regions, const Frame& frame, double u, double v) {
  const gp_Pnt p = pnt(frame.to_world(u, v));
  BRepClass_FaceClassifier cls;
  int best = -1;
  for (size_t i = 0; i < regions.size(); ++i) {
    cls.Perform(regions[i].face, p, 1e-7);
    if (cls.State() != TopAbs_IN && cls.State() != TopAbs_ON) continue;
    if (best < 0 || regions[i].area < regions[static_cast<size_t>(best)].area) best = static_cast<int>(i);
  }
  return best;
}

TopoDS_Wire sketch_wire(const Sketch& sk, const Frame& frame, const std::vector<int>& entities) {
  Handle(TopTools_HSequenceOfShape) edges = new TopTools_HSequenceOfShape();
  for (const auto& e : sk.entities) {
    if (entities.empty() ? e.construction : std::find(entities.begin(), entities.end(), e.id) == entities.end()) continue;
    TopoDS_Edge edge = entity_edge(sk, e, frame);
    if (!edge.IsNull()) edges->Append(edge);
  }
  if (edges->IsEmpty()) throw Error("the sketch has no curves to follow");
  Handle(TopTools_HSequenceOfShape) wires;
  ShapeAnalysis_FreeBounds::ConnectEdgesToWires(edges, 1e-6, Standard_False, wires);
  if (wires.IsNull() || wires->Length() != 1) throw Error("the path must be one connected chain of curves");
  return TopoDS::Wire(wires->Value(1));
}

bool sketch_bounds(const Sketch& sk, double& u0, double& v0, double& u1, double& v1) {
  if (sk.points.empty()) return false;
  u0 = v0 = 1e300;
  u1 = v1 = -1e300;
  for (const auto& p : sk.points) {
    u0 = std::min(u0, p.x); u1 = std::max(u1, p.x);
    v0 = std::min(v0, p.y); v1 = std::max(v1, p.y);
  }
  for (const auto& e : sk.entities)
    if (e.type == SkEntity::Type::Circle)
      if (const SkPoint* c = sk.point(e.p.empty() ? 0 : e.p[0])) {
        u0 = std::min(u0, c->x - e.r); u1 = std::max(u1, c->x + e.r);
        v0 = std::min(v0, c->y - e.r); v1 = std::max(v1, c->y + e.r);
      }
  return true;
}

}  // namespace opad::design
