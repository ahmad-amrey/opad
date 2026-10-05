// Feature recognition (TODO 11 UI-97): see recognize.hpp. Everything is lazy and cached per face and edge, so a
// selection's groups cost what they touch; all() walks the body once per kind.
#include "opad/recognize.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Curve2d.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_Curve.hxx>
#include <IntCurvesFace_Intersector.hxx>
#include <Precision.hxx>
#include <ShapeAnalysis_CanonicalRecognition.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <tuple>

#include "opad/util.hpp"

namespace opad {

namespace {

const double kSmooth = 0.5 * M_PI / 180;  // normals closer than this: the faces are tangent at the edge
const double kParallel = 1e-4;           // axes and directions (radians); fitted surfaces are a little off
const char* kDia = "\xC3\x98";           // Ø
const char* kDot = " \xC2\xB7 ";         // " · "

std::string num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.3f", std::round(v * 1000) / 1000);
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s == "-0" ? "0" : s;
}

template <class T>
void add_unique(std::vector<T>& v, const T& x) {
  if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

bool has(const std::vector<int>& sorted, int x) { return std::binary_search(sorted.begin(), sorted.end(), x); }

std::vector<int> sorted(std::vector<int> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

gp_Pnt on_axis(const gp_Ax1& a, const gp_Pnt& p) {
  const gp_Vec d(a.Direction());
  return a.Location().Translated(d * gp_Vec(a.Location(), p).Dot(d));
}

// The outward normal at a parameter point (the face's orientation applied); false where the surface has none.
bool normal_at(const BRepAdaptor_Surface& s, bool reversed, double u, double v, gp_Pnt& p, gp_Dir& n) {
  gp_Vec du, dv;
  s.D1(u, v, p, du, dv);
  const gp_Vec c = du.Crossed(dv);
  if (c.Magnitude() > 1e-12) {
    n = gp_Dir(c);
  } else {
    BRepLProp_SLProps props(s, u, v, 2, 1e-9);
    if (!props.IsNormalDefined()) return false;
    n = props.Normal();
  }
  if (reversed) n.Reverse();
  return true;
}

// Largest gap between sorted angles on the circle: what an arc of them leaves out.
double covered(std::vector<double> a) {
  if (a.size() < 2) return 0;
  std::sort(a.begin(), a.end());
  double gap = a.front() + 2 * M_PI - a.back();
  for (size_t i = 1; i < a.size(); ++i) gap = std::max(gap, a[i] - a[i - 1]);
  return 2 * M_PI - gap;
}

}  // namespace

const std::vector<std::string>& recognizer_kinds() {
  static const std::vector<std::string> kinds = {"hole", "fillet", "chamfer", "boss", "pocket", "wall", "tangent", "loop", "similar"};
  return kinds;
}

// ---------------------------------------------------------------- per face and per edge, made on first use
namespace {

enum class Kind { Plane, Cylinder, Cone, Sphere, Torus, Other };

struct Surf {
  Kind kind = Kind::Other;
  gp_Ax1 axis;     // cylinder, cone, torus: the axis; plane: its normal (as the surface has it)
  gp_Pnt centre;   // sphere, torus: the centre; cone: the apex
  double r = 0;    // cylinder, sphere: the radius; torus: the major radius
  double r2 = 0;   // torus: the minor radius
  double angle = 0;  // cone: the half angle
  bool fitted = false;  // read off a B-spline (canonical recognition), not the face's own surface
};

struct FaceData {
  bool ready = false, sampled = false, has_point = false, measured = false;
  Surf s;
  gp_Pnt at;
  gp_Dir n;               // outward, at `at`
  bool concave = false;   // curved: the outward normal turns to the axis or the centre (holes, inside rounds)
  double area = 0;
  gp_Pnt mass;
  int blend = -1;         // -1 unknown
  double blend_r = 0;
  bool blend_convex = false;
  int chamfer = -1;
  double chamfer_w = 0, chamfer_d = 0;
  std::vector<int> chamfer_sides;
};

struct EdgeData {
  bool ready = false;
  int type = 0;  // 0 other, 1 line, 2 circle
  gp_Lin line;
  gp_Circ circ;
  double length = -1;
};

}  // namespace

struct Recognizer::Impl {
  TopoDS_Shape body;
  std::function<bool()> cancelled;
  TopTools_IndexedMapOfShape F, E, V;
  std::vector<std::vector<int>> face_edges, edge_faces, edge_verts, vert_edges;
  std::vector<signed char> joins;
  std::vector<FaceData> fd;
  std::vector<EdgeData> ed;
  double diag = 1, tol = 1e-6, fit = 1e-4;
  std::vector<Bnd_Box> boxes;                                  // per face, for rays (all at the first)
  std::map<int, Handle(IntCurvesFace_Intersector)> crossings;  // per face, as rays meet their boxes
  std::map<int, std::optional<Recognized>> hole_memo, wall_memo;
  std::vector<std::pair<int, std::vector<int>>> inner;  // faces' inner loops: (face, edges)
  std::vector<std::vector<size_t>> inner_on;            // per face: its loops in `inner`
  std::vector<std::optional<std::vector<int>>> loop_memo;  // per loop: the faces it encloses
  bool inner_ready = false;
  std::optional<std::vector<Recognized>> enclosed_memo;

  Impl(const TopoDS_Shape& b, std::function<bool()> c) : body(b), cancelled(std::move(c)) {
    TopExp::MapShapes(body, TopAbs_FACE, F);
    TopExp::MapShapes(body, TopAbs_EDGE, E);
    TopExp::MapShapes(body, TopAbs_VERTEX, V);
    face_edges.resize(static_cast<size_t>(F.Extent()));
    edge_faces.resize(static_cast<size_t>(E.Extent()));
    edge_verts.resize(static_cast<size_t>(E.Extent()));
    vert_edges.resize(static_cast<size_t>(V.Extent()));
    for (int i = 1; i <= F.Extent(); ++i)
      for (TopExp_Explorer ex(F(i), TopAbs_EDGE); ex.More(); ex.Next())
        if (const int e = E.FindIndex(ex.Current()) - 1; e >= 0) {
          add_unique(face_edges[static_cast<size_t>(i - 1)], e);
          add_unique(edge_faces[static_cast<size_t>(e)], i - 1);
        }
    for (int i = 1; i <= E.Extent(); ++i)
      for (TopExp_Explorer ex(E(i), TopAbs_VERTEX); ex.More(); ex.Next())
        if (const int v = V.FindIndex(ex.Current()) - 1; v >= 0) {
          add_unique(edge_verts[static_cast<size_t>(i - 1)], v);
          add_unique(vert_edges[static_cast<size_t>(v)], i - 1);
        }
    joins.assign(static_cast<size_t>(E.Extent()), -1);
    fd.resize(static_cast<size_t>(F.Extent()));
    ed.resize(static_cast<size_t>(E.Extent()));
    Bnd_Box box;
    BRepBndLib::Add(body, box, Standard_False);
    diag = box.IsVoid() ? 1.0 : std::max(1e-3, std::sqrt(box.SquareExtent()));
    tol = std::max(1e-6, 1e-7 * diag);
    fit = std::max(1e-4, 1e-5 * diag);
  }

  void check() const {
    if (cancelled && cancelled()) throw Error("cancelled");
  }
  int nf() const { return F.Extent(); }
  TopoDS_Face face(int f) const { return TopoDS::Face(F(f + 1)); }
  TopoDS_Edge edge(int e) const { return TopoDS::Edge(E(e + 1)); }
  bool same(double a, double b) const { return std::fabs(a - b) <= std::max(fit * 0.1, 1e-5 * std::max(std::fabs(a), std::fabs(b))); }
  // An edge between two faces (not a seam, not degenerate).
  bool inner_edge(int e) const { return edge_faces[static_cast<size_t>(e)].size() == 2 && !BRep_Tool::Degenerated(edge(e)); }
  int other(int e, int f) const {
    for (int g : edge_faces[static_cast<size_t>(e)])
      if (g != f) return g;
    return -1;
  }

  // ---- surfaces
  const Surf& surf(int f) {
    FaceData& d = fd[static_cast<size_t>(f)];
    if (d.ready) return d.s;
    d.ready = true;
    Surf& s = d.s;
    const TopoDS_Face fc = face(f);
    try {
      BRepAdaptor_Surface a(fc, Standard_False);
      switch (a.GetType()) {
        case GeomAbs_Plane: s.kind = Kind::Plane; s.axis = a.Plane().Axis(); break;
        case GeomAbs_Cylinder: s.kind = Kind::Cylinder; s.axis = a.Cylinder().Axis(); s.r = a.Cylinder().Radius(); break;
        case GeomAbs_Cone: s.kind = Kind::Cone; s.axis = a.Cone().Axis(); s.angle = std::fabs(a.Cone().SemiAngle()); s.centre = a.Cone().Apex(); break;
        case GeomAbs_Sphere: s.kind = Kind::Sphere; s.centre = a.Sphere().Location(); s.r = a.Sphere().Radius(); break;
        case GeomAbs_Torus: s.kind = Kind::Torus; s.axis = a.Torus().Axis(); s.centre = a.Torus().Location(); s.r = a.Torus().MajorRadius(); s.r2 = a.Torus().MinorRadius(); break;
        default: {  // imported B-splines: what they are within the fit tolerance
          ShapeAnalysis_CanonicalRecognition rec(fc);
          gp_Pln pl;
          gp_Cylinder cy;
          gp_Cone co;
          gp_Sphere sp;
          s.fitted = true;
          if (rec.IsPlane(fit, pl)) { s.kind = Kind::Plane; s.axis = pl.Axis(); }
          else if (rec.IsCylinder(fit, cy)) { s.kind = Kind::Cylinder; s.axis = cy.Axis(); s.r = cy.Radius(); }
          else if (rec.IsCone(fit, co)) { s.kind = Kind::Cone; s.axis = co.Axis(); s.angle = std::fabs(co.SemiAngle()); s.centre = co.Apex(); }
          else if (rec.IsSphere(fit, sp)) { s.kind = Kind::Sphere; s.centre = sp.Location(); s.r = sp.Radius(); }
          else s.fitted = false;
        }
      }
    } catch (const Standard_Failure&) {
      s = Surf();
    }
    return s;
  }

  // A point inside the face near the middle of its parameters, and the outward normal there.
  bool sample(int f, gp_Pnt& p, gp_Dir& n) {
    FaceData& d = fd[static_cast<size_t>(f)];
    if (!d.sampled) {
      d.sampled = true;
      const TopoDS_Face fc = face(f);
      try {
        double u0, u1, v0, v1;
        BRepTools::UVBounds(fc, u0, u1, v0, v1);
        BRepTopAdaptor_FClass2d inside(fc, 1e-9);
        BRepAdaptor_Surface a(fc, Standard_False);
        const double su = std::max(u1 - u0, 1e-12), sv = std::max(v1 - v0, 1e-12);
        for (int k : {5, 15, 41}) {
          double best = 1e300, bu = 0, bv = 0;
          for (int i = 0; i < k; ++i)
            for (int j = 0; j < k; ++j) {
              const double u = u0 + su * (i + 0.5) / k, v = v0 + sv * (j + 0.5) / k;
              if (inside.Perform(gp_Pnt2d(u, v)) != TopAbs_IN) continue;
              const double dist = std::pow((u - (u0 + u1) / 2) / su, 2) + std::pow((v - (v0 + v1) / 2) / sv, 2);
              if (dist < best) best = dist, bu = u, bv = v;
            }
          if (best < 1e300 && normal_at(a, fc.Orientation() == TopAbs_REVERSED, bu, bv, d.at, d.n)) {
            d.has_point = true;
            break;
          }
        }
      } catch (const Standard_Failure&) {
      }
      if (d.has_point) {
        const Surf& s = surf(f);
        const gp_Vec n(d.n);
        switch (s.kind) {
          case Kind::Cylinder:
          case Kind::Cone: d.concave = n.Dot(gp_Vec(on_axis(s.axis, d.at), d.at)) < 0; break;
          case Kind::Sphere: d.concave = n.Dot(gp_Vec(s.centre, d.at)) < 0; break;
          case Kind::Torus: {
            const gp_Vec z(s.axis.Direction());
            gp_Vec radial(s.centre, d.at);
            radial -= z * radial.Dot(z);
            if (radial.Magnitude() > 1e-12) d.concave = n.Dot(gp_Vec(s.centre.Translated(radial.Normalized() * s.r), d.at)) < 0;
            break;
          }
          case Kind::Other: {  // the centre of the sharper curvature is outside the material
            try {
              double u0, u1, v0, v1;
              BRepTools::UVBounds(fc, u0, u1, v0, v1);
              BRepAdaptor_Surface a(fc, Standard_False);
              BRepLProp_SLProps props(a, (u0 + u1) / 2, (v0 + v1) / 2, 2, 1e-9);
              if (props.IsCurvatureDefined() && props.IsNormalDefined()) {
                const double k = std::fabs(props.MaxCurvature()) >= std::fabs(props.MinCurvature()) ? props.MaxCurvature() : props.MinCurvature();
                gp_Dir geo = props.Normal();
                if (std::fabs(k) > 1e-12) d.concave = gp_Vec(geo).Multiplied(1 / k).Dot(n) > 0;
              }
            } catch (const Standard_Failure&) {
            }
            break;
          }
          default: break;
        }
      }
    }
    p = d.at;
    n = d.n;
    return d.has_point;
  }
  bool concave(int f) {
    gp_Pnt p;
    gp_Dir n;
    sample(f, p, n);
    return fd[static_cast<size_t>(f)].concave;
  }
  void measure(int f) {
    FaceData& d = fd[static_cast<size_t>(f)];
    if (d.measured) return;
    d.measured = true;
    try {
      GProp_GProps g;
      BRepGProp::SurfaceProperties(face(f), g);
      d.area = g.Mass();
      d.mass = g.CentreOfMass();
    } catch (const Standard_Failure&) {
    }
  }
  double area(int f) {
    measure(f);
    return fd[static_cast<size_t>(f)].area;
  }
  // The nearest face a ray meets past `from`, and where: only faces whose boxes it passes, each with an intersector of its
  // own (one for the whole body loads every face first: 0.45 s on a 4000-face casting).
  std::pair<int, double> first_hit(const gp_Lin& ray, double from) {
    if (boxes.empty()) {
      boxes.resize(static_cast<size_t>(nf()));
      for (int f = 0; f < nf(); ++f) {
        check();
        BRepBndLib::Add(face(f), boxes[static_cast<size_t>(f)], Standard_True);
        boxes[static_cast<size_t>(f)].Enlarge(10 * tol);
      }
    }
    int hit = -1;
    double at = Precision::Infinite();
    for (int f = 0; f < nf(); ++f) {
      const Bnd_Box& b = boxes[static_cast<size_t>(f)];
      if (b.IsVoid() || b.IsOut(ray)) continue;
      Handle(IntCurvesFace_Intersector)& x = crossings[f];
      try {
        if (x.IsNull()) x = new IntCurvesFace_Intersector(face(f), tol);
        x->Perform(ray, from, at);
      } catch (const Standard_Failure&) {
        continue;
      }
      if (!x->IsDone()) continue;
      for (int i = 1; i <= x->NbPnt(); ++i)
        if (const double w = x->WParameter(i); w > from && w < at) at = w, hit = f;
    }
    return {hit, at};
  }
  double extent(int f) {  // the face's size: its box's diagonal
    Bnd_Box b;
    BRepBndLib::Add(face(f), b, Standard_False);
    return b.IsVoid() ? 0.0 : std::sqrt(b.SquareExtent());
  }
  // How far the face reaches round the axis (radians): parameters of analytic surfaces, else its boundary's angles.
  double span_about(int f, const gp_Ax1& axis) {
    const Surf& s = surf(f);
    if (!s.fitted && (s.kind == Kind::Cylinder || s.kind == Kind::Cone || s.kind == Kind::Torus)) {
      double u0, u1, v0, v1;
      BRepTools::UVBounds(face(f), u0, u1, v0, v1);
      return std::min(2 * M_PI, u1 - u0);
    }
    const gp_Ax3 frame(axis.Location(), axis.Direction());
    std::vector<double> angles;
    for (int e : face_edges[static_cast<size_t>(f)]) {
      if (BRep_Tool::Degenerated(edge(e))) continue;
      BRepAdaptor_Curve c(edge(e));
      for (int i = 0; i <= 32; ++i) {
        const gp_Vec v(axis.Location(), c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * i / 32));
        const double x = v.Dot(gp_Vec(frame.XDirection())), y = v.Dot(gp_Vec(frame.YDirection()));
        if (std::hypot(x, y) > tol) angles.push_back(std::atan2(y, x));
      }
    }
    return covered(angles);
  }
  // A torus face's reach round its tube (radians).
  double tube_span(int f) {
    const Surf& s = surf(f);
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face(f), u0, u1, v0, v1);
    return s.fitted ? 2 * M_PI : std::min(2 * M_PI, v1 - v0);
  }

  // ---- edges
  const EdgeData& curve(int e) {
    EdgeData& d = ed[static_cast<size_t>(e)];
    if (d.ready) return d;
    d.ready = true;
    const TopoDS_Edge x = edge(e);
    if (BRep_Tool::Degenerated(x)) return d;
    try {
      BRepAdaptor_Curve c(x);
      if (c.GetType() == GeomAbs_Line) {
        d.type = 1;
        d.line = c.Line();
      } else if (c.GetType() == GeomAbs_Circle) {
        d.type = 2;
        d.circ = c.Circle();
      } else if (c.GetType() != GeomAbs_Ellipse && c.GetType() != GeomAbs_Hyperbola && c.GetType() != GeomAbs_Parabola) {
        ShapeAnalysis_CanonicalRecognition rec(x);
        gp_Lin l;
        gp_Circ ci;
        if (rec.IsLine(fit, l)) d.type = 1, d.line = l;
        else if (rec.IsCircle(fit, ci)) d.type = 2, d.circ = ci;
      }
      d.length = GCPnts_AbscissaPoint::Length(c);
    } catch (const Standard_Failure&) {
    }
    return d;
  }
  double length(int e) { return std::max(0.0, curve(e).length); }
  gp_Pnt middle(int e) {
    BRepAdaptor_Curve c(edge(e));
    return c.Value((c.FirstParameter() + c.LastParameter()) / 2);
  }
  bool coaxial_circle(int e, const gp_Ax1& axis) {
    const EdgeData& c = curve(e);
    return c.type == 2 && c.circ.Axis().IsParallel(axis, kParallel) && gp_Lin(axis).Distance(c.circ.Location()) < fit;
  }
  // The distance from a point to an edge's curve (its nearest point within the edge's range).
  double distance_to(const gp_Pnt& p, int e) {
    double first = 0, last = 0;
    const Handle(Geom_Curve) c = BRep_Tool::Curve(edge(e), first, last);
    if (c.IsNull()) return 1e300;
    GeomAPI_ProjectPointOnCurve project(p, c, first, last);
    double d = std::min(p.Distance(c->Value(first)), p.Distance(c->Value(last)));
    if (project.NbPoints() > 0) d = std::min(d, project.LowerDistance());
    return d;
  }
  TopoDS_Edge in_face(const TopoDS_Face& f, const TopoDS_Edge& e) const {
    for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next())
      if (ex.Current().IsSame(e)) return TopoDS::Edge(ex.Current());
    return e;
  }
  // The outward normal of face f at the edge's parameter t (on its curve on that face).
  bool normal_on(int f, int e, double t, gp_Dir& n) {
    const TopoDS_Face fc = face(f);
    const TopoDS_Edge x = in_face(fc, edge(e));
    BRepAdaptor_Curve2d c(x, fc);
    const gp_Pnt2d uv = c.Value(t);
    BRepAdaptor_Surface s(fc, Standard_False);
    gp_Pnt p;
    return normal_at(s, fc.Orientation() == TopAbs_REVERSED, uv.X(), uv.Y(), p, n);
  }

  // Smooth when the faces' normals agree; else convex when (n1 x n2) runs along the edge as face 1 goes round it.
  Join join(int e) {
    signed char& j = joins[static_cast<size_t>(e)];
    if (j >= 0) return static_cast<Join>(j);
    Join out = Join::Open;
    if (inner_edge(e)) {
      try {
        const int a = edge_faces[static_cast<size_t>(e)][0], b = edge_faces[static_cast<size_t>(e)][1];
        const TopoDS_Edge x = edge(e);
        if (BRep_Tool::Continuity(x, face(a), face(b)) >= GeomAbs_G1) {
          out = Join::Smooth;
        } else {
          BRepAdaptor_Curve c(x);
          const TopAbs_Orientation along = in_face(face(a), x).Orientation();
          for (double k : {0.5, 0.31, 0.69}) {
            const double t = c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * k;
            gp_Pnt p;
            gp_Vec tangent;
            c.D1(t, p, tangent);
            gp_Dir na, nb;
            if (tangent.Magnitude() < 1e-12 || !normal_on(a, e, t, na) || !normal_on(b, e, t, nb)) continue;
            if (na.Angle(nb) < kSmooth) {
              out = Join::Smooth;
              break;
            }
            if (along == TopAbs_REVERSED) tangent.Reverse();
            out = gp_Vec(na).Crossed(gp_Vec(nb)).Dot(tangent) > 0 ? Join::Convex : Join::Concave;
            break;
          }
        }
      } catch (const Standard_Failure&) {
        out = Join::Open;
      }
    }
    j = static_cast<signed char>(out);
    return out;
  }

  // Faces reachable from the seeds through edges `cross` allows; empty when it grows past `cap` faces.
  std::vector<int> flood(const std::vector<int>& seeds, const std::function<bool(int e, int from, int to)>& cross, size_t cap) {
    std::vector<char> seen(static_cast<size_t>(nf()), 0);
    std::vector<int> out, queue;
    for (int s : seeds)
      if (s >= 0 && s < nf() && !seen[static_cast<size_t>(s)]) seen[static_cast<size_t>(s)] = 1, queue.push_back(s);
    while (!queue.empty()) {
      check();
      const int f = queue.back();
      queue.pop_back();
      out.push_back(f);
      if (out.size() > cap) return {};
      for (int e : face_edges[static_cast<size_t>(f)])
        for (int g : edge_faces[static_cast<size_t>(e)])
          if (!seen[static_cast<size_t>(g)] && cross(e, f, g)) seen[static_cast<size_t>(g)] = 1, queue.push_back(g);
    }
    return sorted(out);
  }

  bool tangent_of(int e, int v, gp_Vec& t) {
    const TopoDS_Edge x = edge(e);
    if (BRep_Tool::Degenerated(x)) return false;
    for (TopExp_Explorer ex(x, TopAbs_VERTEX); ex.More(); ex.Next())
      if (ex.Current().IsSame(V(v + 1))) {
        BRepAdaptor_Curve c(x);
        gp_Pnt p;
        c.D1(BRep_Tool::Parameter(TopoDS::Vertex(ex.Current()), x), p, t);
        return t.Magnitude() > 1e-12;
      }
    return false;
  }

  // ---- blends: a cylinder, torus or sphere (or a surface of one cross radius) with two or more smooth edges
  bool blend(int f, double& r, bool& convex) {
    FaceData& d = fd[static_cast<size_t>(f)];
    if (d.blend < 0) {
      d.blend = 0;
      int smooth = 0;
      for (int e : face_edges[static_cast<size_t>(f)])
        if (inner_edge(e) && join(e) == Join::Smooth) ++smooth;
      if (smooth >= 2) {
        const Surf& s = surf(f);
        double radius = 0;
        bool ok = false;
        switch (s.kind) {
          case Kind::Cylinder: radius = s.r; ok = span_about(f, s.axis) < M_PI - 1e-3; break;  // not a slot's half round
          case Kind::Torus: radius = s.r2; ok = tube_span(f) < M_PI - 1e-3; break;
          case Kind::Sphere: radius = s.r; ok = true; break;
          case Kind::Other: ok = cross_radius(f, radius); break;
          default: break;
        }
        if (ok && radius > tol) {
          d.blend = 1;
          d.blend_r = radius;
          d.blend_convex = !concave(f);
        }
      }
    }
    r = d.blend_r;
    convex = d.blend_convex;
    return d.blend == 1;
  }
  // A free-form face whose sharper curvature is the same all over it (a rolling-ball blend): that radius.
  bool cross_radius(int f, double& radius) {
    try {
      const TopoDS_Face fc = face(f);
      double u0, u1, v0, v1;
      BRepTools::UVBounds(fc, u0, u1, v0, v1);
      BRepTopAdaptor_FClass2d inside(fc, 1e-9);
      BRepAdaptor_Surface a(fc, Standard_False);
      std::vector<double> ks;
      for (int i = 1; i <= 3; ++i)
        for (int j = 1; j <= 3; ++j) {
          const double u = u0 + (u1 - u0) * i / 4, v = v0 + (v1 - v0) * j / 4;
          if (inside.Perform(gp_Pnt2d(u, v)) != TopAbs_IN) continue;
          BRepLProp_SLProps props(a, u, v, 2, 1e-9);
          if (props.IsCurvatureDefined()) ks.push_back(std::max(std::fabs(props.MaxCurvature()), std::fabs(props.MinCurvature())));
        }
      if (ks.size() < 3) return false;
      const auto [lo, hi] = std::minmax_element(ks.begin(), ks.end());
      if (*lo < 1e-9 || *hi > *lo * 1.03) return false;
      double mean = 0;
      for (double k : ks) mean += k;
      radius = ks.size() / mean;
      return true;
    } catch (const Standard_Failure&) {
      return false;
    }
  }

  // ---- chamfers: a narrow plane or cone between two faces, met along two opposite sharp edges at equal angles
  bool chamfer_face(int f) {
    FaceData& d = fd[static_cast<size_t>(f)];
    if (d.chamfer >= 0) return d.chamfer == 1;
    d.chamfer = 0;
    const Surf& s = surf(f);
    if (s.kind != Kind::Plane && s.kind != Kind::Cone) return false;
    std::vector<int> es;
    for (int e : face_edges[static_cast<size_t>(f)])
      if (inner_edge(e)) es.push_back(e);
    if (es.size() < 2 || es.size() > 12) return false;
    double best = -1;
    for (size_t i = 0; i < es.size(); ++i)
      for (size_t k = i + 1; k < es.size(); ++k) {
        check();
        const int a = es[i], b = es[k];
        const Join ja = join(a);
        if ((ja != Join::Convex && ja != Join::Concave) || join(b) != ja) continue;
        bool touching = false;
        for (int v : edge_verts[static_cast<size_t>(a)]) touching |= std::find(edge_verts[static_cast<size_t>(b)].begin(), edge_verts[static_cast<size_t>(b)].end(), v) != edge_verts[static_cast<size_t>(b)].end();
        const int ga = other(a, f), gb = other(b, f);
        if (touching || ga < 0 || gb < 0 || ga == gb) continue;
        try {
          const double w = distance_to(middle(a), b), w2 = distance_to(middle(b), a);
          if (w < tol || std::fabs(w - w2) > 0.02 * std::max(w, w2) + tol) continue;
          BRepAdaptor_Curve ca(edge(a)), cb(edge(b));
          const double ta = (ca.FirstParameter() + ca.LastParameter()) / 2, tb = (cb.FirstParameter() + cb.LastParameter()) / 2;
          gp_Dir fa, na, fb, nb;
          if (!normal_on(f, a, ta, fa) || !normal_on(ga, a, ta, na) || !normal_on(f, b, tb, fb) || !normal_on(gb, b, tb, nb)) continue;
          const double tha = fa.Angle(na), thb = fb.Angle(nb);
          if (std::fabs(tha - thb) > 1.5 * M_PI / 180 || tha < M_PI / 180 || tha > 85 * M_PI / 180) continue;
          if (na.Angle(nb) > 179 * M_PI / 180) continue;  // the side of a thin plate, not a bevel
          if (w > 0.2 * std::min(extent(ga), extent(gb))) continue;
          const double score = length(a) + length(b);
          if (score <= best) continue;
          best = score;
          d.chamfer_w = w;
          d.chamfer_d = w / (2 * std::cos((tha + thb) / 2));
          d.chamfer_sides = {a, b};
        } catch (const Standard_Failure&) {
        }
      }
    d.chamfer = best >= 0 ? 1 : 0;
    return d.chamfer == 1;
  }

  // ---- holes
  std::optional<Recognized> hole(int f) {
    if (auto it = hole_memo.find(f); it != hole_memo.end()) return it->second;
    std::optional<Recognized> out = make_hole(f);
    hole_memo[f] = out;
    if (out)
      for (int g : out->faces) hole_memo[g] = out;
    return out;
  }
  std::optional<Recognized> make_hole(int f) {
    const Surf& s = surf(f);
    if (s.kind != Kind::Cylinder || !concave(f)) return std::nullopt;
    const gp_Ax1 axis = s.axis;
    auto coaxial = [&](const Surf& t) { return t.axis.IsParallel(axis, kParallel) && gp_Lin(axis).Distance(t.axis.Location()) < fit; };
    auto bore = [&](int g) {
      const Surf& t = surf(g);
      return (t.kind == Kind::Cylinder || t.kind == Kind::Cone) && coaxial(t) && concave(g);
    };
    auto step = [&](int g) {  // a plane square to the axis bounded by coaxial circles between hole walls: a step or the floor
      const Surf& t = surf(g);
      if (t.kind != Kind::Plane || !t.axis.IsParallel(axis, kParallel)) return false;
      for (int e : face_edges[static_cast<size_t>(g)]) {
        if (BRep_Tool::Degenerated(edge(e))) continue;
        if (!coaxial_circle(e, axis) || !inner_edge(e) || !bore(other(e, g))) return false;
      }
      return true;
    };
    const std::vector<int> faces = flood({f}, [&](int, int, int to) { return bore(to) || step(to); }, static_cast<size_t>(nf()));
    double rmin = 1e300, rmax = 0;
    for (int g : faces)
      if (surf(g).kind == Kind::Cylinder) rmin = std::min(rmin, surf(g).r), rmax = std::max(rmax, surf(g).r);
    // All the way round: the bore's faces together.
    double round = 0;
    for (int g : faces)
      if (surf(g).kind == Kind::Cylinder && same(surf(g).r, rmin)) round += span_about(g, axis);
    if (round < 2 * M_PI - 0.3) return std::nullopt;
    const gp_Vec z(axis.Direction());
    auto along = [&](const gp_Pnt& p) { return gp_Vec(axis.Location(), p).Dot(z); };
    bool floor = false, tip = false, step_seen = false, sink = false;
    double tip_angle = 0, cs_r = 0, cs_angle = 0, lo = 1e300, hi = -1e300, cb_lo = 1e300, cb_hi = -1e300;
    for (int g : faces) {
      const Surf& t = surf(g);
      bool pointed = false;
      if (t.kind == Kind::Plane) {
        int wires = 0;
        for (TopExp_Explorer w(face(g), TopAbs_WIRE); w.More(); w.Next()) ++wires;
        (wires == 1 ? floor : step_seen) = true;
      } else if (t.kind == Kind::Cone) {
        for (int e : face_edges[static_cast<size_t>(g)]) pointed |= BRep_Tool::Degenerated(edge(e));
        for (TopExp_Explorer v(face(g), TopAbs_VERTEX); v.More() && !pointed; v.Next()) pointed = BRep_Tool::Pnt(TopoDS::Vertex(v.Current())).Distance(t.centre) < fit;
        if (pointed) {
          tip = true;
          tip_angle = 2 * t.angle;
        } else {
          sink = true;
          cs_angle = 2 * t.angle;
          for (TopExp_Explorer v(face(g), TopAbs_VERTEX); v.More(); v.Next()) cs_r = std::max(cs_r, gp_Lin(axis).Distance(BRep_Tool::Pnt(TopoDS::Vertex(v.Current()))));
          for (int e : face_edges[static_cast<size_t>(g)])
            if (curve(e).type == 2) cs_r = std::max(cs_r, curve(e).circ.Radius());
        }
      }
      if (pointed) continue;
      for (TopExp_Explorer v(face(g), TopAbs_VERTEX); v.More(); v.Next()) {
        const double h = along(BRep_Tool::Pnt(TopoDS::Vertex(v.Current())));
        lo = std::min(lo, h), hi = std::max(hi, h);
        if (t.kind == Kind::Cylinder && rmax > rmin && same(t.r, rmax)) cb_lo = std::min(cb_lo, h), cb_hi = std::max(cb_hi, h);
      }
    }
    const bool through = !floor && !tip;
    const bool bored = step_seen && rmax > rmin && !same(rmax, rmin);
    Recognized r;
    r.kind = "hole";
    r.faces = faces;
    const double d = 2 * rmin, depth = hi > lo ? hi - lo : 0.0;
    r.params = {{"diameter", d}, {"depth", depth}, {"through", through}, {"type", bored ? "counterbore" : sink ? "countersink" : "simple"},
                {"axis", {{"origin", {axis.Location().X(), axis.Location().Y(), axis.Location().Z()}}, {"dir", {z.X(), z.Y(), z.Z()}}}}};
    if (bored) r.params["cb_diameter"] = 2 * rmax, r.params["cb_depth"] = cb_hi > cb_lo ? cb_hi - cb_lo : 0.0;
    if (sink) r.params["cs_diameter"] = 2 * cs_r, r.params["cs_angle"] = cs_angle * 180 / M_PI;
    if (tip) r.params["tip_angle"] = tip_angle * 180 / M_PI;
    r.label = std::string(bored ? "Counterbored hole " : sink ? "Countersunk hole " : "Hole ") + kDia + num(d) + (through ? " through" : " \xC3\x97 " + num(depth));
    return r;
  }

  // ---- walls
  std::optional<Recognized> wall(int f) {
    if (auto it = wall_memo.find(f); it != wall_memo.end()) return it->second;
    std::optional<Recognized> out = make_wall(f);
    wall_memo[f] = out;
    return out;
  }
  std::vector<gp_Pnt> points_in(int f, size_t want) {
    std::vector<gp_Pnt> out;
    const TopoDS_Face fc = face(f);
    double u0, u1, v0, v1;
    BRepTools::UVBounds(fc, u0, u1, v0, v1);
    BRepTopAdaptor_FClass2d inside(fc, 1e-9);
    BRepAdaptor_Surface a(fc, Standard_False);
    for (int n : {5, 15, 41}) {
      std::vector<gp_Pnt2d> in;
      for (int i = 0; i < n; ++i)
        for (int k = 0; k < n; ++k) {
          const gp_Pnt2d uv(u0 + (u1 - u0) * (i + 0.5) / n, v0 + (v1 - v0) * (k + 0.5) / n);
          if (inside.Perform(uv) == TopAbs_IN) in.push_back(uv);
        }
      if (in.empty()) continue;
      const size_t take = std::min(want, in.size());
      for (size_t j = 0; j < take; ++j) {
        const gp_Pnt2d& uv = in[take == 1 ? in.size() / 2 : j * (in.size() - 1) / (take - 1)];
        out.push_back(a.Value(uv.X(), uv.Y()));
      }
      break;
    }
    return out;
  }
  std::optional<Recognized> make_wall(int f) {
    const Surf& s = surf(f);
    gp_Pnt p;
    gp_Dir n;
    if (s.kind != Kind::Plane || !sample(f, p, n)) return std::nullopt;
    int g = -1;
    double t = -1;
    for (const gp_Pnt& q : points_in(f, 5)) {
      check();
      const auto [h, w] = first_hit(gp_Lin(q, n.Reversed()), 10 * tol);
      if (h < 0 || h == f) return std::nullopt;
      if (g < 0) g = h, t = w;
      else if (h != g || std::fabs(w - t) > 1e-3 * t + 10 * tol) return std::nullopt;
    }
    gp_Pnt pg;
    gp_Dir ng;
    if (g < 0 || surf(g).kind != Kind::Plane || !sample(g, pg, ng) || ng.Angle(n.Reversed()) > kSmooth) return std::nullopt;
    // Thinner than a quarter of the face's smaller side, measured in its plane.
    const gp_Ax3 frame(p, n);
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (int e : face_edges[static_cast<size_t>(f)]) {
      if (BRep_Tool::Degenerated(edge(e))) continue;
      BRepAdaptor_Curve c(edge(e));
      for (int i = 0; i <= 8; ++i) {
        const gp_Vec v(p, c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * i / 8));
        const double x = v.Dot(gp_Vec(frame.XDirection())), y = v.Dot(gp_Vec(frame.YDirection()));
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
      }
    }
    if (x1 < x0 || t >= 0.25 * std::min(x1 - x0, y1 - y0)) return std::nullopt;
    std::vector<int> faces = {f, g};
    for (int e : face_edges[static_cast<size_t>(f)]) {  // the rims: faces between the two, as wide as the wall there
      const int h = other(e, f);
      if (h < 0 || h == g || !inner_edge(e)) continue;
      for (int x : face_edges[static_cast<size_t>(h)])
        if (inner_edge(x) && other(x, h) == g && std::fabs(distance_to(middle(e), x) - t) <= 0.05 * t + 10 * tol) {
          add_unique(faces, h);
          break;
        }
    }
    Recognized r;
    r.kind = "wall";
    r.faces = sorted(faces);
    r.params = {{"thickness", t}};
    r.label = "Wall " + num(t) + " mm";
    return r;
  }

  // ---- bosses and pockets
  void find_inner() {
    if (inner_ready) return;
    inner_ready = true;
    for (int f = 0; f < nf(); ++f) {
      check();
      const TopoDS_Face fc = face(f);
      const TopoDS_Wire outer = BRepTools::OuterWire(fc);
      for (TopExp_Explorer w(fc, TopAbs_WIRE); w.More(); w.Next()) {
        if (w.Current().IsSame(outer)) continue;
        std::vector<int> loop;
        for (TopExp_Explorer x(w.Current(), TopAbs_EDGE); x.More(); x.Next())
          if (const int e = E.FindIndex(x.Current()) - 1; e >= 0 && inner_edge(e)) add_unique(loop, e);
        if (!loop.empty()) inner.push_back({f, sorted(loop)});
      }
    }
    inner_on.resize(static_cast<size_t>(nf()));
    for (size_t i = 0; i < inner.size(); ++i) inner_on[static_cast<size_t>(inner[i].first)].push_back(i);
    loop_memo.resize(inner.size());
  }
  // +1: the faces look at each other (a pocket: walls facing in, a floor facing the opening); -1: away (a boss).
  int facing(const std::vector<int>& region) {
    double total = 0, score = 0;
    gp_XYZ c(0, 0, 0);
    for (int f : region) {
      measure(f);
      c += fd[static_cast<size_t>(f)].mass.XYZ() * fd[static_cast<size_t>(f)].area;
      total += fd[static_cast<size_t>(f)].area;
    }
    if (total <= 0) return 0;
    c /= total;
    for (int f : region) {
      gp_Pnt p;
      gp_Dir n;
      if (!sample(f, p, n)) continue;
      const double d = gp_Vec(n).Dot(gp_Vec(p, gp_Pnt(c)));
      if (std::fabs(d) > 10 * tol) score += (d > 0 ? 1 : -1) * fd[static_cast<size_t>(f)].area;
    }
    score /= total;
    return score > 0.2 ? 1 : score < -0.2 ? -1 : 0;
  }
  size_t detail_cap() const { return static_cast<size_t>(std::max(1, nf() / 2)); }  // more faces: the body, no detail of it
  bool local(const std::vector<int>& region) {  // a detail of the body, not most of it: no more than half its area
    if (region.empty() || static_cast<int>(region.size()) >= nf()) return false;
    double a = 0;
    for (int f : region) a += area(f);
    // As much of the rest as it takes to outweigh it, the faces around it first (the whole body only for most of it).
    std::vector<char> in(static_cast<size_t>(nf()), 0);
    for (int f : region) in[static_cast<size_t>(f)] = 1;
    double rest = 0;
    auto add = [&](int f) {
      if (in[static_cast<size_t>(f)]) return;
      in[static_cast<size_t>(f)] = 1;
      rest += area(f);
    };
    for (int f : region)
      for (int e : face_edges[static_cast<size_t>(f)])
        for (int g : edge_faces[static_cast<size_t>(e)]) add(g);
    for (int f = 0; f < nf() && rest < a; ++f) add(f);
    return rest >= a;
  }
  Recognized region_result(std::vector<int> region, bool boss, int base) {
    Recognized r;
    r.kind = boss ? "boss" : "pocket";
    r.faces = std::move(region);
    r.params = {{"faces", r.faces.size()}};
    if (base >= 0) r.params["base"] = base;
    if (base >= 0 && surf(base).kind == Kind::Plane) {  // how far it stands out of (goes into) the plane it is on
      const gp_Ax1& plane = surf(base).axis;
      double h = 0;
      for (int f : r.faces)
        for (TopExp_Explorer v(face(f), TopAbs_VERTEX); v.More(); v.Next())
          h = std::max(h, std::fabs(gp_Vec(plane.Location(), BRep_Tool::Pnt(TopoDS::Vertex(v.Current()))).Dot(gp_Vec(plane.Direction()))));
      r.params[boss ? "height" : "depth"] = h;
    }
    r.label = std::string(boss ? "Boss" : "Pocket") + kDot + std::to_string(r.faces.size()) + (r.faces.size() == 1 ? " face" : " faces");
    return r;
  }
  // The faces an inner loop encloses: reached from it without crossing it or touching the face it is on. Empty when they
  // reach that face elsewhere or are most of the body. Topology only: cheap.
  const std::vector<int>& loop_faces(size_t i) {
    if (loop_memo[i]) return *loop_memo[i];
    const int base = inner[i].first;
    const std::vector<int>& loop = inner[i].second;
    std::vector<int> start;
    for (int e : loop)
      if (const int g = other(e, base); g >= 0 && g != base) add_unique(start, g);
    bool back = false;
    std::vector<int> region = flood(start, [&](int e, int, int to) {
      if (has(loop, e)) return false;
      if (to == base) back = true;
      return to != base;
    }, detail_cap());
    if (back) region.clear();
    return loop_memo[i].emplace(std::move(region));
  }
  // A loop's region as a boss or a pocket: a detail of the body that stands out or sinks in (areas, samples: the costly part).
  std::optional<Recognized> loop_result(size_t i, const std::vector<int>& region) {
    if (!local(region)) return std::nullopt;
    int side = facing(region);
    if (side == 0) {  // flat regions: the loop's edges say (convex round a pocket's mouth, concave round a boss's foot)
      int convex = 0, concave_ = 0;
      for (int e : inner[i].second) {
        convex += join(e) == Join::Convex;
        concave_ += join(e) == Join::Concave;
      }
      side = convex > concave_ ? 1 : concave_ > convex ? -1 : 0;
    }
    if (side == 0) return std::nullopt;
    return region_result(region, side < 0, inner[i].first);
  }
  // The faces each inner loop encloses (not reaching back to the face the loop is on).
  const std::vector<Recognized>& enclosed() {
    if (enclosed_memo) return *enclosed_memo;
    find_inner();
    std::vector<Recognized>& out = enclosed_memo.emplace();
    std::set<std::vector<int>> seen;
    for (size_t i = 0; i < inner.size(); ++i) {
      check();
      const std::vector<int>& region = loop_faces(i);
      if (region.empty() || !seen.insert(region).second) continue;
      if (auto r = loop_result(i, region)) out.push_back(std::move(*r));
    }
    return out;
  }
  std::optional<Recognized> boss_or_pocket(const std::vector<int>& seeds) {
    if (seeds.empty()) return std::nullopt;
    const std::vector<int> want = sorted(seeds);
    auto holds = [&](const std::vector<int>& region) { return std::all_of(want.begin(), want.end(), [&](int f) { return has(region, f); }); };
    // The smallest region a loop encloses holding them all. The loops on the faces nearest the seeds are found first, and
    // a loop on a face d steps away encloses d faces at least: once the smallest region found is smaller than that, no
    // loop further away can beat it, so it is checked (areas, samples: the costly part) and is the answer when it is one.
    find_inner();
    std::vector<std::pair<size_t, size_t>> found;  // (faces, loop), smallest last
    auto smallest = [&](size_t below) -> std::optional<Recognized> {
      while (!found.empty() && found.back().first < below) {
        const size_t i = found.back().second;
        found.pop_back();
        if (auto r = loop_result(i, loop_faces(i))) return r;
      }
      return std::nullopt;
    };
    std::vector<int> steps(static_cast<size_t>(nf()), -1), queue;
    for (int f : want)
      if (f >= 0 && f < nf() && steps[static_cast<size_t>(f)] < 0) steps[static_cast<size_t>(f)] = 0, queue.push_back(f);
    for (size_t q = 0; q < queue.size(); ++q) {
      check();
      const int f = queue[q];
      if (auto r = smallest(static_cast<size_t>(steps[static_cast<size_t>(f)]))) return r;
      const size_t before = found.size();
      for (size_t i : inner_on[static_cast<size_t>(f)])
        if (const std::vector<int>& region = loop_faces(i); !region.empty() && holds(region)) found.emplace_back(region.size(), i);
      if (found.size() > before) std::sort(found.begin(), found.end(), std::greater<>());
      for (int e : face_edges[static_cast<size_t>(f)])
        for (int g : edge_faces[static_cast<size_t>(e)])
          if (steps[static_cast<size_t>(g)] < 0) steps[static_cast<size_t>(g)] = steps[static_cast<size_t>(f)] + 1, queue.push_back(g);
    }
    if (auto r = smallest(std::numeric_limits<size_t>::max())) return r;
    // A region closed by concave edges (a boss) or convex ones (a pocket): through cuts, open slots, ribs.
    const size_t cap = detail_cap();
    for (const bool boss : {false, true}) {
      const Join barrier = boss ? Join::Concave : Join::Convex;
      const std::vector<int> region = flood({want.front()}, [&](int e, int, int) { return join(e) != barrier; }, cap);
      if (!holds(region) || !local(region)) continue;
      bool formed = true;  // no face whose outer loop is all barrier (the top a boss stands on is no pocket)
      for (int f : region) {
        int sharp = 0, all = 0;
        for (TopExp_Explorer x(BRepTools::OuterWire(face(f)), TopAbs_EDGE); x.More(); x.Next())
          if (const int e = E.FindIndex(x.Current()) - 1; e >= 0 && inner_edge(e)) ++all, sharp += join(e) == barrier;
        formed &= all == 0 || sharp < all;
      }
      if (formed && facing(region) == (boss ? -1 : 1)) return region_result(region, boss, -1);
    }
    return std::nullopt;
  }

  // ---- chains
  Recognized fillet_chain(int f, double r) {
    bool any_convex = false, any_concave = false;
    const std::vector<int> faces = flood({f}, [&](int e, int, int to) {
      double rt;
      bool cv;
      return join(e) == Join::Smooth && blend(to, rt, cv) && same(rt, r);
    }, static_cast<size_t>(nf()));
    for (int g : faces) (fd[static_cast<size_t>(g)].blend_convex ? any_convex : any_concave) = true;
    Recognized out;
    out.kind = "fillet";
    out.faces = faces;
    out.params = {{"radius", r}, {"faces", faces.size()}};
    if (any_convex != any_concave) out.params["convex"] = any_convex;
    out.label = std::string(faces.size() == 1 ? "Fillet R" : "Fillet chain R") + num(r) + (faces.size() == 1 ? std::string() : kDot + std::to_string(faces.size()) + " faces");
    return out;
  }
  Recognized chamfer_chain(int f) {
    const FaceData& d = fd[static_cast<size_t>(f)];
    const double w = d.chamfer_w;
    const std::vector<int> faces = flood({f}, [&](int e, int from, int to) {
      const auto& sides = fd[static_cast<size_t>(from)].chamfer_sides;
      return std::find(sides.begin(), sides.end(), e) == sides.end() && chamfer_face(to) && std::fabs(fd[static_cast<size_t>(to)].chamfer_w - w) <= 0.02 * w + tol;
    }, static_cast<size_t>(nf()));
    Recognized out;
    out.kind = "chamfer";
    out.faces = faces;
    out.params = {{"distance", d.chamfer_d}, {"width", w}, {"faces", faces.size()}};
    out.label = std::string(faces.size() == 1 ? "Chamfer " : "Chamfer chain ") + num(d.chamfer_d) + " mm" + (faces.size() == 1 ? std::string() : kDot + std::to_string(faces.size()) + " faces");
    return out;
  }
};

// ---------------------------------------------------------------- public
Recognizer::Recognizer(const TopoDS_Shape& body, std::function<bool()> cancelled) : m(std::make_unique<Impl>(body, std::move(cancelled))) {}
Recognizer::~Recognizer() = default;

int Recognizer::face_count() const { return m->nf(); }
int Recognizer::edge_count() const { return m->E.Extent(); }
Recognizer::Join Recognizer::join(int edge) { return edge >= 0 && edge < edge_count() ? m->join(edge) : Join::Open; }

Recognized Recognizer::tangent_faces(const std::vector<int>& faces) {
  Recognized r;
  r.kind = "tangent";
  r.faces = m->flood(faces, [&](int e, int, int) { return m->join(e) == Join::Smooth; }, static_cast<size_t>(m->nf()));
  r.params = {{"faces", r.faces.size()}};
  r.label = "Tangent faces" + std::string(kDot) + std::to_string(r.faces.size());
  return r;
}

Recognized Recognizer::tangent_edges(const std::vector<int>& edges) {
  Recognized r;
  r.kind = "tangent";
  std::vector<char> seen(static_cast<size_t>(edge_count()), 0);
  std::vector<int> queue;
  for (int e : edges)
    if (e >= 0 && e < edge_count() && !seen[static_cast<size_t>(e)]) seen[static_cast<size_t>(e)] = 1, queue.push_back(e);
  while (!queue.empty()) {
    m->check();
    const int e = queue.back();
    queue.pop_back();
    r.edges.push_back(e);
    for (int v : m->edge_verts[static_cast<size_t>(e)]) {
      gp_Vec t;
      if (!m->tangent_of(e, v, t)) continue;
      for (int x : m->vert_edges[static_cast<size_t>(v)]) {
        gp_Vec u;
        if (seen[static_cast<size_t>(x)] || !m->inner_edge(x) || !m->tangent_of(x, v, u) || !t.IsParallel(u, M_PI / 180)) continue;
        seen[static_cast<size_t>(x)] = 1;
        queue.push_back(x);
      }
    }
  }
  r.edges = sorted(r.edges);
  r.params = {{"edges", r.edges.size()}};
  r.label = "Tangent edges" + std::string(kDot) + std::to_string(r.edges.size());
  return r;
}

std::vector<Recognized> Recognizer::loops(const std::vector<int>& edges) {
  std::vector<Recognized> out;
  if (edges.empty() || edges.front() < 0 || edges.front() >= edge_count()) return out;
  std::set<std::vector<int>> seen;
  for (int f : m->edge_faces[static_cast<size_t>(edges.front())]) {
    const TopoDS_Face fc = m->face(f);
    const TopoDS_Wire outer = BRepTools::OuterWire(fc);
    for (TopExp_Explorer w(fc, TopAbs_WIRE); w.More(); w.Next()) {
      std::vector<int> wire;
      for (TopExp_Explorer x(w.Current(), TopAbs_EDGE); x.More(); x.Next())
        if (const int e = m->E.FindIndex(x.Current()) - 1; e >= 0 && m->inner_edge(e)) add_unique(wire, e);
      // A wire through a seam is two loops (a cylinder's rims): the part connected to the first pick.
      std::vector<int> loop = {edges.front()};
      if (std::find(wire.begin(), wire.end(), edges.front()) == wire.end()) continue;
      for (size_t i = 0; i < loop.size(); ++i)
        for (int v : m->edge_verts[static_cast<size_t>(loop[i])])
          for (int x : m->vert_edges[static_cast<size_t>(v)])
            if (std::find(wire.begin(), wire.end(), x) != wire.end()) add_unique(loop, x);
      loop = sorted(loop);
      if (!std::all_of(edges.begin(), edges.end(), [&](int e) { return has(loop, e); }) || !seen.insert(loop).second) continue;
      Recognized r;
      r.kind = "loop";
      r.edges = loop;
      r.params = {{"face", f}, {"outer", w.Current().IsSame(outer)}, {"edges", loop.size()}};
      r.label = "Loop" + std::string(kDot) + std::to_string(loop.size()) + (loop.size() == 1 ? " edge" : " edges");
      out.push_back(std::move(r));
    }
  }
  return out;
}

std::optional<Recognized> Recognizer::fillet(int face) {
  double r;
  bool convex;
  if (face < 0 || face >= m->nf() || !m->blend(face, r, convex)) return std::nullopt;
  return m->fillet_chain(face, r);
}

std::optional<Recognized> Recognizer::chamfer(int face) {
  if (face < 0 || face >= m->nf() || !m->chamfer_face(face)) return std::nullopt;
  return m->chamfer_chain(face);
}

std::optional<Recognized> Recognizer::hole(int face) {
  if (face < 0 || face >= m->nf()) return std::nullopt;
  return m->hole(face);
}

std::optional<Recognized> Recognizer::wall(int face) {
  if (face < 0 || face >= m->nf()) return std::nullopt;
  return m->wall(face);
}

std::optional<Recognized> Recognizer::boss_or_pocket(const std::vector<int>& faces) {
  for (int f : faces)
    if (f < 0 || f >= m->nf()) return std::nullopt;
  return m->boss_or_pocket(faces);
}

std::vector<Recognized> Recognizer::all(const std::string& kind) {
  if (m->nf() > 20000) throw Error("recognition scans at most 20000 faces; this body has " + std::to_string(m->nf()));
  std::vector<Recognized> out;
  if (kind == "boss" || kind == "pocket") {
    for (const auto& r : m->enclosed())
      if (r.kind == kind) out.push_back(r);
    return out;
  }
  if (kind != "hole" && kind != "fillet" && kind != "chamfer" && kind != "wall") throw Error("no recogniser for \"" + kind + "\": hole, fillet, chamfer, wall, boss or pocket");
  std::vector<char> taken(static_cast<size_t>(m->nf()), 0);
  std::set<std::vector<int>> seen;
  for (int f = 0; f < m->nf(); ++f) {
    m->check();
    if (taken[static_cast<size_t>(f)]) continue;
    std::optional<Recognized> r = kind == "hole" ? hole(f) : kind == "fillet" ? fillet(f) : kind == "chamfer" ? chamfer(f) : wall(f);
    if (!r || !seen.insert(r->faces).second) continue;
    if (kind != "wall")
      for (int g : r->faces) taken[static_cast<size_t>(g)] = 1;
    out.push_back(std::move(*r));
  }
  return out;
}

namespace {

// Several groups of one kind as one (two holes picked): their faces, the first's parameters.
Recognized merged(const std::vector<Recognized>& groups) {
  if (groups.size() == 1) return groups.front();
  Recognized r = groups.front();
  std::vector<int> faces;
  for (const auto& g : groups) faces.insert(faces.end(), g.faces.begin(), g.faces.end());
  r.faces = sorted(faces);
  r.params["groups"] = groups.size();
  static const std::map<std::string, std::string> plural = {{"hole", "holes"}, {"fillet", "fillets"}, {"chamfer", "chamfers"}, {"wall", "walls"}};
  r.label = std::to_string(groups.size()) + " " + plural.at(r.kind);
  return r;
}

// One rule over the body's faces or edges: the members, labelled.
Recognized rule(const std::string& rule, const std::string& label, std::vector<int> members, bool edges, json params) {
  Recognized r;
  r.kind = "similar";
  r.rule = rule;
  (edges ? r.edges : r.faces) = std::move(members);
  params["count"] = edges ? r.edges.size() : r.faces.size();
  r.params = std::move(params);
  r.label = label + kDot + std::to_string(edges ? r.edges.size() : r.faces.size());
  return r;
}

}  // namespace

std::vector<Recognized> Recognizer::similar_faces(int f) {
  std::vector<Recognized> out;
  if (f < 0 || f >= m->nf()) return out;
  auto members = [&](const std::function<bool(int)>& same) {
    std::vector<int> v;
    for (int g = 0; g < m->nf(); ++g) {
      m->check();
      if (same(g)) v.push_back(g);
    }
    return v;
  };
  // The groups it is in, generalised: holes of its size and type, fillets of its radius, chamfers, walls.
  if (const auto h = hole(f)) {
    std::vector<int> faces;
    size_t n = 0;
    for (const auto& o : all("hole"))
      if (o.params["type"] == h->params["type"] && o.params["through"] == h->params["through"] && m->same(o.params["diameter"].get<double>(), h->params["diameter"].get<double>()) &&
          (!h->params.contains("cb_diameter") || (o.params.contains("cb_diameter") && m->same(o.params["cb_diameter"].get<double>(), h->params["cb_diameter"].get<double>()))) &&
          (!h->params.contains("cs_diameter") || (o.params.contains("cs_diameter") && m->same(o.params["cs_diameter"].get<double>(), h->params["cs_diameter"].get<double>())))) {
        faces.insert(faces.end(), o.faces.begin(), o.faces.end());
        ++n;
      }
    Recognized r = rule("hole", "All holes " + std::string(kDia) + num(h->params["diameter"].get<double>()) + (h->params["through"].get<bool>() ? " through" : ""), sorted(faces), false, h->params);
    r.params["count"] = n;
    r.label = r.label.substr(0, r.label.rfind(kDot)) + kDot + std::to_string(n);
    out.push_back(std::move(r));
  }
  double radius;
  bool convex;
  if (m->blend(f, radius, convex))
    out.push_back(rule("fillet", "All fillets R" + num(radius), members([&](int g) { double r; bool c; return m->blend(g, r, c) && m->same(r, radius); }), false, {{"radius", radius}}));
  if (m->chamfer_face(f)) {
    const double d = m->fd[static_cast<size_t>(f)].chamfer_d;
    out.push_back(rule("chamfer", "All chamfers " + num(d) + " mm", members([&](int g) { return m->chamfer_face(g) && m->same(m->fd[static_cast<size_t>(g)].chamfer_d, d); }), false, {{"distance", d}}));
  }
  if (const auto w = m->nf() <= 5000 ? wall(f) : std::nullopt) {  // walls cast rays from every plane: not on huge bodies
    const double t = w->params["thickness"].get<double>();
    std::vector<int> faces;
    for (const auto& o : all("wall"))
      if (m->same(o.params["thickness"].get<double>(), t)) faces.insert(faces.end(), o.faces.begin(), o.faces.end());
    out.push_back(rule("wall", "All walls " + num(t) + " mm", sorted(faces), false, {{"thickness", t}}));
  }
  // The face itself: its surface's size, or for planes its direction and its area.
  const Surf& s = m->surf(f);
  const bool hollow = m->concave(f);
  switch (s.kind) {
    case Kind::Cylinder:
      out.push_back(rule("radius", std::string(hollow ? "Inside" : "Outside") + " cylindrical faces R" + num(s.r),
                         members([&](int g) { return m->surf(g).kind == Kind::Cylinder && m->same(m->surf(g).r, s.r) && m->concave(g) == hollow; }), false,
                         {{"radius", s.r}, {"concave", hollow}}));
      break;
    case Kind::Sphere:
      out.push_back(rule("radius", "Spherical faces R" + num(s.r), members([&](int g) { return m->surf(g).kind == Kind::Sphere && m->same(m->surf(g).r, s.r); }), false, {{"radius", s.r}}));
      break;
    case Kind::Torus:
      out.push_back(rule("radius", "Toroidal faces R" + num(s.r2), members([&](int g) { return m->surf(g).kind == Kind::Torus && m->same(m->surf(g).r2, s.r2) && m->same(m->surf(g).r, s.r); }), false,
                         {{"radius", s.r2}, {"major_radius", s.r}}));
      break;
    case Kind::Cone:
      out.push_back(rule("angle", "Conical faces " + num(2 * s.angle * 180 / M_PI) + "\xC2\xB0", members([&](int g) { return m->surf(g).kind == Kind::Cone && std::fabs(m->surf(g).angle - s.angle) < kParallel; }), false,
                         {{"angle", 2 * s.angle * 180 / M_PI}}));
      break;
    case Kind::Plane: {
      gp_Pnt p;
      gp_Dir n;
      if (m->sample(f, p, n)) {
        out.push_back(rule("normal", "Planar faces facing the same way", members([&](int g) {
                             gp_Pnt q;
                             gp_Dir k;
                             return m->surf(g).kind == Kind::Plane && m->sample(g, q, k) && k.Angle(n) < kParallel;
                           }), false, {{"normal", {n.X(), n.Y(), n.Z()}}}));
      }
      [[fallthrough]];
    }
    default: {
      const double a = m->area(f);
      const Kind kind = s.kind;
      out.push_back(rule("area", std::string(kind == Kind::Plane ? "Planar" : "Similar") + " faces of the same area",
                         members([&](int g) { return m->surf(g).kind == kind && std::fabs(m->area(g) - a) <= 1e-3 * a + m->tol; }), false, {{"area", a}}));
    }
  }
  return out;
}

std::vector<Recognized> Recognizer::similar_edges(int e) {
  std::vector<Recognized> out;
  if (e < 0 || e >= edge_count() || BRep_Tool::Degenerated(m->edge(e))) return out;
  auto members = [&](const std::function<bool(int)>& same) {
    std::vector<int> v;
    for (int x = 0; x < edge_count(); ++x) {
      m->check();
      if (m->inner_edge(x) && same(x)) v.push_back(x);
    }
    return v;
  };
  const EdgeData c = m->curve(e);
  const double len = m->length(e);
  if (c.type == 1) {
    const gp_Dir d = c.line.Direction();
    out.push_back(rule("direction", "Parallel edges", members([&](int x) { return m->curve(x).type == 1 && m->curve(x).line.Direction().IsParallel(d, kParallel); }), true,
                       {{"direction", {d.X(), d.Y(), d.Z()}}}));
    out.push_back(rule("length", "Parallel edges of the same length", members([&](int x) {
                         return m->curve(x).type == 1 && m->curve(x).line.Direction().IsParallel(d, kParallel) && std::fabs(m->length(x) - len) <= 1e-3 * len + m->tol;
                       }), true, {{"length", len}}));
  } else if (c.type == 2) {
    const double r = c.circ.Radius();
    out.push_back(rule("radius", "Circular edges R" + num(r), members([&](int x) { return m->curve(x).type == 2 && m->same(m->curve(x).circ.Radius(), r); }), true, {{"radius", r}}));
  } else {
    const GeomAbs_CurveType type = BRepAdaptor_Curve(m->edge(e)).GetType();
    out.push_back(rule("length", "Edges of the same kind and length", members([&](int x) {
                         return m->curve(x).type == 0 && BRepAdaptor_Curve(m->edge(x)).GetType() == type && std::fabs(m->length(x) - len) <= 1e-3 * len + m->tol;
                       }), true, {{"length", len}}));
  }
  return out;
}

std::vector<Recognized> Recognizer::body_rules() {
  std::vector<Recognized> out;
  auto keep = [&](Recognized r) {
    if (!r.faces.empty() || !r.edges.empty()) out.push_back(std::move(r));
  };
  std::vector<int> lines;                           // edges users see: no seams, nothing degenerate
  std::vector<std::pair<double, double>> heights;   // the lowest and highest z along each of them
  double top = -1e300, bottom = 1e300;
  for (int e = 0; e < edge_count(); ++e) {
    m->check();
    const TopoDS_Edge x = m->edge(e);
    const auto& faces = m->edge_faces[static_cast<size_t>(e)];
    if (BRep_Tool::Degenerated(x) || (faces.size() == 1 && BRep_Tool::IsClosed(x, m->face(faces.front())))) continue;
    try {
      BRepAdaptor_Curve c(x);
      double lo = 1e300, hi = -1e300;
      for (int i = 0; i <= 8; ++i) {
        const double z = c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * i / 8).Z();
        lo = std::min(lo, z), hi = std::max(hi, z);
      }
      lines.push_back(e);
      heights.emplace_back(lo, hi);
      top = std::max(top, hi), bottom = std::min(bottom, lo);
    } catch (const Standard_Failure&) {
    }
  }
  auto edges = [&](const std::function<bool(size_t)>& follows) {
    std::vector<int> v;
    for (size_t i = 0; i < lines.size(); ++i) {
      m->check();
      if (follows(i)) v.push_back(lines[i]);
    }
    return v;
  };
  const double t = m->fit;
  if (top - bottom > t) {  // a flat body (a drawing) has no top and bottom: both would be every edge
    keep(rule("top", "Top perimeter", edges([&](size_t i) { return heights[i].first >= top - t; }), true, {{"z", top}}));
    keep(rule("bottom", "Bottom perimeter", edges([&](size_t i) { return heights[i].second <= bottom + t; }), true, {{"z", bottom}}));
  }
  for (int k = 0; k < 3; ++k) {
    const gp_Dir d(k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0);
    keep(rule(std::string(1, char('x' + k)), std::string("Edges parallel to ") + char('X' + k), edges([&](size_t i) {
                const EdgeData& c = m->curve(lines[i]);
                return c.type == 1 && c.line.Direction().IsParallel(d, kParallel);
              }), true, {{"direction", {d.X(), d.Y(), d.Z()}}}));
  }
  keep(rule("circle", "Circular edges", edges([&](size_t i) { return m->curve(lines[i]).type == 2; }), true, json::object()));
  std::vector<int> up;
  for (int f = 0; f < m->nf(); ++f) {
    m->check();
    gp_Pnt p;
    gp_Dir n;
    if (m->surf(f).kind == Kind::Plane && m->sample(f, p, n) && n.Angle(gp::DZ()) < kParallel) up.push_back(f);
  }
  keep(rule("up", "Upward planar faces", up, false, {{"normal", {0, 0, 1}}}));
  if (m->nf() <= 20000)  // all() refuses more
    for (const auto& [kind, name, label] : {std::tuple{"hole", "holes", "All holes"}, std::tuple{"fillet", "fillets", "All fillets"}, std::tuple{"chamfer", "chamfers", "All chamfers"}}) {
      std::vector<int> faces;
      size_t n = 0;
      for (const auto& g : all(kind)) {
        faces.insert(faces.end(), g.faces.begin(), g.faces.end());
        ++n;
      }
      Recognized r = rule(name, label, sorted(faces), false, json::object());
      r.params["count"] = n;  // the holes, not their faces
      r.label = std::string(label) + kDot + std::to_string(n);
      keep(std::move(r));
    }
  return out;
}

std::vector<Recognized> Recognizer::around(const std::vector<int>& faces_in, const std::vector<int>& edges_in, const std::set<std::string>& kinds) {
  const std::vector<int> faces = sorted(faces_in), edges = sorted(edges_in);
  for (int f : faces)
    if (f < 0 || f >= m->nf()) throw Error("face " + std::to_string(f) + " is not on the body");
  for (int e : edges)
    if (e < 0 || e >= edge_count()) throw Error("edge " + std::to_string(e) + " is not on the body");
  auto wants = [&](const char* k) { return kinds.count(k) > 0; };
  // Held: every picked face is in it, every picked edge is in it or bounds one of its faces.
  auto holds = [&](const Recognized& r) {
    for (int f : faces)
      if (!has(r.faces, f)) return false;
    for (int e : edges) {
      bool in = has(r.edges, e);
      for (int f : m->edge_faces[static_cast<size_t>(e)]) in |= has(r.faces, f);
      if (!in) return false;
    }
    return true;
  };
  auto just_picks = [&](const Recognized& r) { return r.faces == faces && r.edges == edges; };
  std::vector<int> seeds = faces;
  for (int e : edges)
    for (int f : m->edge_faces[static_cast<size_t>(e)]) add_unique(seeds, f);
  std::vector<Recognized> groups, chains, similar;
  for (const char* kind : {"hole", "fillet", "chamfer", "wall"}) {
    if (!wants(kind)) continue;
    std::vector<Recognized> found;
    std::set<std::vector<int>> seen;
    for (int f : seeds) {
      std::optional<Recognized> r = std::string(kind) == "hole" ? hole(f) : std::string(kind) == "fillet" ? fillet(f) : std::string(kind) == "chamfer" ? chamfer(f) : wall(f);
      if (r && seen.insert(r->faces).second) found.push_back(std::move(*r));
    }
    if (found.empty()) continue;
    Recognized r = merged(found);
    if (holds(r)) groups.push_back(std::move(r));
  }
  if (wants("boss") || wants("pocket")) {
    std::optional<Recognized> r = boss_or_pocket(faces);
    for (size_t i = 0; !r && faces.empty() && !edges.empty() && i < m->edge_faces[static_cast<size_t>(edges.front())].size(); ++i)
      r = boss_or_pocket({m->edge_faces[static_cast<size_t>(edges.front())][i]});
    if (r && wants(r->kind.c_str()) && holds(*r)) groups.push_back(std::move(*r));
  }
  // One group per set of faces: a pocket that is a hole is the hole; a countersink is no chamfer.
  std::vector<Recognized> kept;
  for (auto& g : groups) {
    const bool dup = std::any_of(kept.begin(), kept.end(), [&](const Recognized& k) {
      if (k.faces == g.faces) return true;
      return k.kind == "hole" && g.kind == "chamfer" && std::all_of(g.faces.begin(), g.faces.end(), [&](int f) { return has(k.faces, f); });
    });
    if (!dup) kept.push_back(std::move(g));
  }
  std::stable_sort(kept.begin(), kept.end(), [](const Recognized& a, const Recognized& b) { return a.faces.size() + a.edges.size() < b.faces.size() + b.edges.size(); });
  if (wants("tangent")) {
    if (!faces.empty())
      if (Recognized r = tangent_faces(faces); holds(r) && !just_picks(r)) chains.push_back(std::move(r));
    if (!edges.empty() && faces.empty())
      if (Recognized r = tangent_edges(edges); holds(r) && !just_picks(r)) chains.push_back(std::move(r));
  }
  if (wants("loop") && !edges.empty() && faces.empty())
    for (auto& r : loops(edges))
      if (!just_picks(r)) chains.push_back(std::move(r));
  if (wants("similar") && faces.size() + edges.size() >= 1) {
    for (auto& r : !faces.empty() ? similar_faces(faces.front()) : similar_edges(edges.front()))
      if (holds(r) && !just_picks(r)) similar.push_back(std::move(r));
  }
  kept.insert(kept.end(), std::make_move_iterator(chains.begin()), std::make_move_iterator(chains.end()));
  kept.insert(kept.end(), std::make_move_iterator(similar.begin()), std::make_move_iterator(similar.end()));
  return kept;
}

std::pair<json, json> split_recognized(const json& filters) {
  if (!filters.is_object() || !filters.contains("recognized")) return {json(), filters};
  static const std::vector<std::string> keys = {"recognized", "diameter", "radius", "distance", "thickness", "depth", "type", "through", "cb_diameter", "cs_diameter"};
  json recognition = json::object(), rest = filters;
  for (const auto& k : keys)
    if (rest.contains(k)) {
      recognition[k] = rest[k];
      rest.erase(k);
    }
  return {recognition, rest};
}

std::vector<int> recognized_faces(const TopoDS_Shape& body, const json& recognition, double tolerance, const std::function<bool()>& cancelled) {
  if (!recognition.contains("recognized") || !recognition["recognized"].is_string()) throw Error("\"recognized\" names a kind: hole, fillet, chamfer, wall, boss or pocket");
  Recognizer r(body, cancelled);
  std::vector<int> out;
  for (const auto& g : r.all(recognition["recognized"].get<std::string>())) {
    bool ok = true;
    for (const auto& [key, want] : recognition.items()) {
      if (key == "recognized") continue;
      if (!g.params.contains(key)) { ok = false; break; }
      const json& have = g.params[key];
      if (want.is_number() && have.is_number()) ok = std::fabs(have.get<double>() - want.get<double>()) <= tolerance + 1e-9 * std::fabs(want.get<double>());
      else ok = have == want;
      if (!ok) break;
    }
    if (ok) out.insert(out.end(), g.faces.begin(), g.faces.end());
  }
  return sorted(out);
}

}  // namespace opad
