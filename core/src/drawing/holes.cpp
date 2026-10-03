// Holes recognised from a solid's faces (TODO 11 UI-80): drilled diameters and what their axis meets at each end.
#include "opad/drawing/holes.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Circ.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace opad::drawing {
namespace {

constexpr double kTol = 1e-5;

struct FaceInfo {
  enum Kind { Other, Cylinder, Cone, Plane } kind = Other;
  gp_Pnt loc;
  gp_Dir axis;
  double r = 0, semi = 0, v0 = 0, v1 = 0, turn = 0;  // cylinder radius; cone reference radius and half angle; v range; u span
  bool wall = false;                       // a cylinder with the material outside it
};

FaceInfo info_of(const TopoDS_Face& f) {
  FaceInfo i;
  try {
    const BRepAdaptor_Surface s(f);
    double u0, u1, v0, v1;
    BRepTools::UVBounds(f, u0, u1, v0, v1);
    i.v0 = v0, i.v1 = v1, i.turn = u1 - u0;
    if (s.GetType() == GeomAbs_Cylinder) {
      const gp_Cylinder c = s.Cylinder();
      i.kind = FaceInfo::Cylinder, i.loc = c.Location(), i.axis = c.Axis().Direction(), i.r = c.Radius();
      gp_Pnt p;
      gp_Vec du, dv;
      s.D1((u0 + u1) / 2, (v0 + v1) / 2, p, du, dv);
      gp_Vec n = du.Crossed(dv);
      if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
      const gp_Vec along(i.axis);
      gp_Vec radial(i.loc, p);
      radial -= along * radial.Dot(along);
      i.wall = n.Magnitude() > 1e-12 && radial.Magnitude() > 1e-12 && n.Dot(radial) < 0;
    } else if (s.GetType() == GeomAbs_Cone) {
      const gp_Cone c = s.Cone();
      i.kind = FaceInfo::Cone, i.loc = c.Location(), i.axis = c.Axis().Direction(), i.r = c.RefRadius(), i.semi = c.SemiAngle();
    } else if (s.GetType() == GeomAbs_Plane) {
      const gp_Pln p = s.Plane();
      i.kind = FaceInfo::Plane, i.loc = p.Location(), i.axis = p.Axis().Direction();
    }
  } catch (const Standard_Failure&) {
    i.kind = FaceInfo::Other;
  }
  return i;
}

// The hole's axis: a point on it and its direction; t is the coordinate along it.
struct Axis {
  gp_Pnt p;
  gp_Dir d;
  double t(const gp_Pnt& q) const { return gp_Vec(p, q).Dot(gp_Vec(d)); }
  double off(const gp_Pnt& q) const {  // distance from the axis
    gp_Vec v(p, q);
    v -= gp_Vec(d) * v.Dot(gp_Vec(d));
    return v.Magnitude();
  }
  bool along(const FaceInfo& f) const { return f.axis.IsParallel(d, 1e-6) && off(f.loc) < kTol * std::max(1.0, std::fabs(t(f.loc))); }
  // The face's range along the axis and its radius at both ends (a cone's from its apex out).
  void range(const FaceInfo& f, double& a, double& b, double& ra, double& rb) const {
    const double sign = gp_Vec(f.axis).Dot(gp_Vec(d)) > 0 ? 1 : -1, base = t(f.loc);
    if (f.kind == FaceInfo::Cone) {
      const double c = std::cos(f.semi), s = std::sin(f.semi);
      a = base + sign * f.v0 * c, b = base + sign * f.v1 * c;
      ra = f.r + f.v0 * s, rb = f.r + f.v1 * s;
    } else {
      a = base + sign * f.v0, b = base + sign * f.v1;
      ra = rb = f.r;
    }
    if (a > b) std::swap(a, b), std::swap(ra, rb);
  }
};

struct Body {
  TopTools_IndexedMapOfShape faces;
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  std::vector<FaceInfo> info;
  explicit Body(const TopoDS_Shape& s) {
    TopExp::MapShapes(s, TopAbs_FACE, faces);
    TopExp::MapShapesAndAncestors(s, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
    for (int i = 1; i <= faces.Extent(); ++i) info.push_back(info_of(TopoDS::Face(faces(i))));
  }
  int index(const TopoDS_Shape& f) const { return faces.FindIndex(f) - 1; }
  // Faces next to `face` across its circular edges centred on the axis at t (within a tolerance).
  std::vector<int> across(int face, const Axis& ax, double t, std::vector<TopoDS_Edge>* edges = nullptr) const {
    std::vector<int> out;
    for (TopExp_Explorer e(faces(face + 1), TopAbs_EDGE); e.More(); e.Next()) {
      const TopoDS_Edge edge = TopoDS::Edge(e.Current());
      if (BRep_Tool::Degenerated(edge)) continue;
      try {
        const BRepAdaptor_Curve c(edge);
        if (c.GetType() != GeomAbs_Circle) continue;
        const gp_Circ ci = c.Circle();
        if (ax.off(ci.Location()) > kTol * std::max(1.0, ci.Radius()) || std::fabs(ax.t(ci.Location()) - t) > kTol * std::max(1.0, std::fabs(t))) continue;
      } catch (const Standard_Failure&) {
        continue;
      }
      if (edges) edges->push_back(edge);
      const int at = edgeFaces.FindIndex(edge);
      if (at <= 0) continue;
      for (const auto& f : edgeFaces(at))
        if (const int k = index(f); k >= 0 && k != face && std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
    }
    return out;
  }
};

// What a hole's end meets: an opening, a closed bottom, or a counterbore or countersink and then the opening.
struct End {
  enum Kind { Open, Flat, Point, Counterbore, Countersink } kind = Open;
  double outer = 0;        // the opening's coordinate along the axis (past a counterbore or countersink)
  double size = 0, angle = 0, depth = 0;
  std::vector<int> faces;
};

End end_of(const Body& b, const std::vector<int>& drill, const Axis& ax, double r, double t, double out, const std::set<int>& drillSet) {
  End e;
  e.outer = t;
  std::vector<int> next;
  for (int f : drill)
    for (int k : b.across(f, ax, t))
      if (!drillSet.count(k) && std::find(next.begin(), next.end(), k) == next.end()) next.push_back(k);
  const auto counterbore = [&](int k) {  // a wider wall beyond this end: its far end is the opening
    const FaceInfo& f = b.info[static_cast<size_t>(k)];
    if (f.kind != FaceInfo::Cylinder || !f.wall || !ax.along(f) || f.r <= r + kTol) return false;
    double a0, a1, ra, rb;
    ax.range(f, a0, a1, ra, rb);
    std::vector<int> walls{k};  // the same wall in pieces
    for (size_t j = 0; j < b.info.size(); ++j) {
      const FaceInfo& g = b.info[j];
      if (static_cast<int>(j) == k || g.kind != FaceInfo::Cylinder || !g.wall || !ax.along(g) || std::fabs(g.r - f.r) > kTol) continue;
      double c0, c1, rc, rd;
      ax.range(g, c0, c1, rc, rd);
      if (c1 < a0 - kTol || c0 > a1 + kTol) continue;
      a0 = std::min(a0, c0), a1 = std::max(a1, c1);
      walls.push_back(static_cast<int>(j));
    }
    const double reach = out > 0 ? a1 : a0;
    if ((reach - t) * out < kTol) return false;
    e.kind = End::Counterbore, e.size = 2 * f.r, e.depth = std::fabs(reach - t), e.outer = reach;
    e.faces.insert(e.faces.end(), walls.begin(), walls.end());
    return true;
  };
  for (int k : next) {
    const FaceInfo& f = b.info[static_cast<size_t>(k)];
    if (counterbore(k)) return e;
    if (f.kind == FaceInfo::Cone && ax.along(f)) {
      double a0, a1, ra, rb;
      ax.range(f, a0, a1, ra, rb);
      const bool startsHere = std::fabs((out > 0 ? a0 : a1) - t) < kTol * std::max(1.0, std::fabs(t));
      const double farR = out > 0 ? rb : ra, farT = out > 0 ? a1 : a0;
      if (!startsHere) continue;
      if (farR < kTol) {
        e.kind = End::Point, e.angle = 2 * std::fabs(f.semi) * 180 / M_PI;
      } else if (farR > r + kTol) {
        e.kind = End::Countersink, e.size = 2 * farR, e.angle = 2 * std::fabs(f.semi) * 180 / M_PI, e.outer = farT;
      } else {
        continue;
      }
      e.faces.push_back(k);
      return e;
    }
    if (f.kind == FaceInfo::Plane && f.axis.IsParallel(ax.d, 1e-6)) {
      // A disc closes the hole; a ring is a shoulder whose outer circle leads to a counterbore; else it is the surface the
      // hole opens on.
      bool other = false;
      double outerR = 0;
      int outerFace = -1;
      for (TopExp_Explorer x(b.faces(k + 1), TopAbs_EDGE); x.More(); x.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(x.Current());
        if (BRep_Tool::Degenerated(edge)) continue;
        try {
          const BRepAdaptor_Curve c(edge);
          if (c.GetType() == GeomAbs_Circle && ax.off(c.Circle().Location()) < kTol * std::max(1.0, c.Circle().Radius())) {
            const double cr = c.Circle().Radius();
            if (cr > r + kTol) {
              outerR = std::max(outerR, cr);
              if (const int at = b.edgeFaces.FindIndex(edge); at > 0)
                for (const auto& g : b.edgeFaces(at))
                  if (const int j = b.index(g); j != k) outerFace = j;
            }
            continue;
          }
        } catch (const Standard_Failure&) {
        }
        other = true;
      }
      if (!other && outerR == 0) {
        e.kind = End::Flat;
        e.faces.push_back(k);
        return e;
      }
      if (!other && outerFace >= 0 && counterbore(outerFace)) {
        e.faces.push_back(k);
        return e;
      }
    }
  }
  return e;
}

std::string fixed(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.9g", std::fabs(v) < 1e-12 ? 0.0 : v);
  return buf;
}

}  // namespace

double Hole::outer() const { return type == "counterbore" ? cb_diameter : type == "countersink" ? cs_diameter : diameter; }

bool Hole::same_size(const Hole& o) const {
  const auto eq = [](double a, double b) { return std::fabs(a - b) < 1e-6; };
  const double along = dir[0] * o.dir[0] + dir[1] * o.dir[1] + dir[2] * o.dir[2];
  return eq(diameter, o.diameter) && through == o.through && (through || eq(depth, o.depth)) && type == o.type && eq(cb_diameter, o.cb_diameter) &&
         eq(cb_depth, o.cb_depth) && eq(cs_diameter, o.cs_diameter) && eq(cs_angle, o.cs_angle) && eq(tip_angle, o.tip_angle) &&
         (through && type == "simple" ? std::fabs(along) > 1 - 1e-9 : along > 1 - 1e-9);
}

json Hole::to_json() const {
  json j = {{"entry", entry}, {"dir", dir}, {"diameter", diameter}, {"through", through}, {"type", type}, {"faces", faces}};
  if (!through) j["depth"] = depth;
  if (tip_angle > 0) j["tip_angle"] = tip_angle;
  if (type == "counterbore") j["cb_diameter"] = cb_diameter, j["cb_depth"] = cb_depth;
  if (type == "countersink") j["cs_diameter"] = cs_diameter, j["cs_angle"] = cs_angle;
  if (!feature.empty()) j["feature"] = feature;
  return j;
}

std::vector<Hole> find_holes(const TopoDS_Shape& shape) {
  std::vector<Hole> out;
  if (shape.IsNull()) return out;
  const Body b(shape);
  std::vector<int> walls;
  for (size_t i = 0; i < b.info.size(); ++i)
    if (b.info[i].kind == FaceInfo::Cylinder && b.info[i].wall) walls.push_back(static_cast<int>(i));
  std::stable_sort(walls.begin(), walls.end(), [&](int x, int y) { return b.info[size_t(x)].r < b.info[size_t(y)].r; });  // drills before their counterbores
  std::set<int> used;
  for (int seed : walls) {
    if (used.count(seed)) continue;
    const FaceInfo& s = b.info[static_cast<size_t>(seed)];
    Axis ax{s.loc, s.axis};
    // The drilled wall, in pieces: coaxial walls of its radius whose ranges touch.
    double a, z, ra, rb;
    ax.range(s, a, z, ra, rb);
    std::vector<int> drill{seed};
    for (bool grew = true; grew;) {
      grew = false;
      for (int k : walls) {
        const FaceInfo& f = b.info[static_cast<size_t>(k)];
        if (std::find(drill.begin(), drill.end(), k) != drill.end() || used.count(k) || std::fabs(f.r - s.r) > kTol || !ax.along(f)) continue;
        double c0, c1, rc, rd;
        ax.range(f, c0, c1, rc, rd);
        if (c1 < a - kTol || c0 > z + kTol) continue;
        a = std::min(a, c0), z = std::max(z, c1);
        drill.push_back(k);
        grew = true;
      }
    }
    // A whole turn around the axis (a fillet or a slot's end is a part of one).
    double turn = 0;
    for (int k : drill) turn += b.info[static_cast<size_t>(k)].turn;
    if (turn < 2 * M_PI - 1e-6) continue;
    const std::set<int> drillSet(drill.begin(), drill.end());
    End lo = end_of(b, drill, ax, s.r, a, -1, drillSet), hi = end_of(b, drill, ax, s.r, z, 1, drillSet);
    const bool loClosed = lo.kind == End::Flat || lo.kind == End::Point, hiClosed = hi.kind == End::Flat || hi.kind == End::Point;
    if (loClosed && hiClosed) continue;  // a closed cavity, not a hole
    for (int k : drill) used.insert(k);
    Hole h;
    h.diameter = 2 * s.r;
    h.through = !loClosed && !hiClosed;
    // The opening: the end with a counterbore or countersink; of a plain through hole the end facing up (z, then y, x).
    bool fromHi = loClosed;
    if (!loClosed && !hiClosed) {
      const bool loMore = lo.kind == End::Counterbore || lo.kind == End::Countersink, hiMore = hi.kind == End::Counterbore || hi.kind == End::Countersink;
      if (loMore != hiMore) fromHi = hiMore;
      else {
        const gp_Dir& d = ax.d;
        const double up = std::fabs(d.Z()) > 1e-9 ? d.Z() : std::fabs(d.Y()) > 1e-9 ? d.Y() : d.X();
        fromHi = up > 0;
      }
    }
    const End& entry = fromHi ? hi : lo;
    const End& bottom = fromHi ? lo : hi;
    const gp_Pnt at = ax.p.Translated(gp_Vec(ax.d) * entry.outer);
    const gp_Dir into = fromHi ? ax.d.Reversed() : ax.d;
    h.entry = {at.X(), at.Y(), at.Z()};
    h.dir = {into.X(), into.Y(), into.Z()};
    if (!h.through) {
      h.depth = std::fabs((fromHi ? a : z) - entry.outer);
      if (bottom.kind == End::Point) h.tip_angle = bottom.angle;
    }
    if (entry.kind == End::Counterbore) h.type = "counterbore", h.cb_diameter = entry.size, h.cb_depth = entry.depth;
    if (entry.kind == End::Countersink) h.type = "countersink", h.cs_diameter = entry.size, h.cs_angle = entry.angle;
    h.faces = drill;
    for (const End* e : {&lo, &hi})
      for (int k : e->faces) {
        h.faces.push_back(k);
        used.insert(k);
      }
    std::sort(h.faces.begin(), h.faces.end());
    h.faces.erase(std::unique(h.faces.begin(), h.faces.end()), h.faces.end());
    out.push_back(std::move(h));
  }
  std::stable_sort(out.begin(), out.end(), [](const Hole& x, const Hole& y) { return x.faces.front() < y.faces.front(); });
  return out;
}

int hole_of(const std::vector<Hole>& holes, const TopoDS_Shape& body, const TopoDS_Shape& sub) {
  if (sub.IsNull()) return -1;
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(body, TopAbs_FACE, faces);
  std::set<int> touching;
  if (sub.ShapeType() == TopAbs_FACE) {
    touching.insert(faces.FindIndex(sub) - 1);
  } else {
    for (int i = 1; i <= faces.Extent(); ++i)
      for (TopExp_Explorer e(faces(i), sub.ShapeType()); e.More(); e.Next())
        if (e.Current().IsSame(sub)) {
          touching.insert(i - 1);
          break;
        }
  }
  for (size_t i = 0; i < holes.size(); ++i)
    for (int f : holes[i].faces)
      if (touching.count(f)) return static_cast<int>(i);
  return -1;
}

std::string hole_callout(const Hole& h, int count, const std::string& standard, double per_mm, const std::function<std::string(double)>& number) {
  std::string first = count > 1 ? std::to_string(count) + (standard == "asme" ? "X " : "× ") : std::string();
  first += "⌀" + number(h.diameter * per_mm) + (h.through ? " THRU" : " ↧" + number(h.depth * per_mm));
  if (h.type == "counterbore") first += "\n⌴ ⌀" + number(h.cb_diameter * per_mm) + " ↧" + number(h.cb_depth * per_mm);
  if (h.type == "countersink") first += "\n⌵ ⌀" + number(h.cs_diameter * per_mm) + " × " + fixed(std::round(h.cs_angle * 100) / 100) + "°";
  return first;
}

}  // namespace opad::drawing
