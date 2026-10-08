// Faces of two versions of a body matched by geometry (Compare): what a join, a cut or a fillet added, changed and
// removed, whatever history made them. A face kept as it was has the same surface, area and centre on both sides (a
// boolean keeps the surfaces it does not cut); one that lost or gained extent keeps its surface but not its area.
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

#include <algorithm>
#include <cmath>
#include <numeric>

#include "opad/diff.hpp"
#include "opad/geometry.hpp"

namespace opad {
namespace {
constexpr int kMaxFaces = 20000;   // per side: a bigger body is shown changed as a whole
constexpr double kMaxPairs = 4e6;  // unmatched faces of both sides tried against each other for a shared surface
constexpr double kAngle = 1e-6;    // radians

struct Face {
  GeomAbs_SurfaceType type = GeomAbs_OtherSurface;
  bool analytic = false;
  gp_Ax3 axis;   // a plane's position, a cylinder's, cone's or torus's axis, a sphere's centre
  gp_Dir normal;  // a plane's, as the face faces
  gp_Pnt apex;
  double r1 = 0, r2 = 0;  // radius (cylinder, sphere), semi-angle (cone), major and minor radius (torus)
  double area = 0;
  gp_Pnt centre;
  Bnd_Box box;
  bool ok = false;
  bool taken = false;  // matched as unchanged
};

Face describe(const TopoDS_Face& f, const gp_Trsf& t) {
  Face d;
  try {
    GProp_GProps g;
    BRepGProp::SurfaceProperties(f, g);
    d.area = g.Mass();
    d.centre = g.CentreOfMass().Transformed(t);
    Bnd_Box box;
    BRepBndLib::Add(f, box, Standard_False);
    if (!box.IsVoid()) d.box = box.Transformed(t);
    const BRepAdaptor_Surface s(f, Standard_False);
    d.type = s.GetType();
    switch (d.type) {
      case GeomAbs_Plane: {
        d.axis = s.Plane().Position().Transformed(t);
        gp_Dir n = d.axis.Direction();
        if (!d.axis.Direct()) n.Reverse();
        if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
        d.normal = n;
        d.analytic = true;
        break;
      }
      case GeomAbs_Cylinder:
        d.axis = s.Cylinder().Position().Transformed(t);
        d.r1 = s.Cylinder().Radius();
        d.analytic = true;
        break;
      case GeomAbs_Cone:
        d.axis = s.Cone().Position().Transformed(t);
        d.apex = s.Cone().Apex().Transformed(t);
        d.r1 = s.Cone().SemiAngle();
        d.analytic = true;
        break;
      case GeomAbs_Sphere:
        d.axis = s.Sphere().Position().Transformed(t);
        d.r1 = s.Sphere().Radius();
        d.analytic = true;
        break;
      case GeomAbs_Torus:
        d.axis = s.Torus().Position().Transformed(t);
        d.r1 = s.Torus().MajorRadius();
        d.r2 = s.Torus().MinorRadius();
        d.analytic = true;
        break;
      default: break;
    }
    d.ok = true;
  } catch (const Standard_Failure&) {
  }
  return d;
}

bool parallel(const gp_Dir& x, const gp_Dir& y) { return std::abs(x.Dot(y)) > std::cos(kAngle); }
bool onLine(const gp_Pnt& p, const gp_Ax3& axis, double tol) { return gp_Lin(axis.Axis()).Distance(p) <= tol; }

// The same analytic surface (planes: facing the same way).
bool sameSurface(const Face& x, const Face& y, double tol) {
  if (!x.analytic || !y.analytic || x.type != y.type) return false;
  const gp_Pnt& p = x.axis.Location();
  const gp_Pnt& q = y.axis.Location();
  switch (x.type) {
    case GeomAbs_Plane: return x.normal.Dot(y.normal) > std::cos(kAngle) && std::abs(gp_Vec(p, q).Dot(gp_Vec(x.normal))) <= tol;
    case GeomAbs_Cylinder: return std::abs(x.r1 - y.r1) <= tol && parallel(x.axis.Direction(), y.axis.Direction()) && onLine(q, x.axis, tol);
    case GeomAbs_Cone:
      return x.apex.Distance(y.apex) <= tol && parallel(x.axis.Direction(), y.axis.Direction()) &&
             (std::abs(x.r1 - y.r1) <= kAngle || std::abs(x.r1 + y.r1) <= kAngle);
    case GeomAbs_Sphere: return p.Distance(q) <= tol && std::abs(x.r1 - y.r1) <= tol;
    case GeomAbs_Torus:
      return p.Distance(q) <= tol && parallel(x.axis.Direction(), y.axis.Direction()) && std::abs(x.r1 - y.r1) <= tol && std::abs(x.r2 - y.r2) <= tol;
    default: return false;
  }
}

double areaTolerance(double a, double b, double tol) { return 1e-5 * std::max(a, b) + tol * tol; }

std::vector<Face> describeAll(const TopTools_IndexedMapOfShape& faces, const gp_Trsf& t) {
  std::vector<Face> out(size_t(faces.Extent()));
  OSD_Parallel::For(0, faces.Extent(), [&](int i) { out[size_t(i)] = describe(TopoDS::Face(faces(i + 1)), t); });
  return out;
}
}  // namespace

FaceChanges face_changes(const TopoDS_Shape& a, const Mat4& world_a, const TopoDS_Shape& b, const Mat4& world_b) {
  FaceChanges out;
  if (a.IsNull() || b.IsNull() || !mat_is_rigid(world_a) || !mat_is_rigid(world_b)) return out;
  TopTools_IndexedMapOfShape fa, fb;
  TopExp::MapShapes(a, TopAbs_FACE, fa);
  TopExp::MapShapes(b, TopAbs_FACE, fb);
  if (fa.Extent() > kMaxFaces || fb.Extent() > kMaxFaces || (fa.IsEmpty() && fb.IsEmpty())) return out;
  std::vector<Face> xa = describeAll(fa, trsf_from_mat(world_a)), xb = describeAll(fb, trsf_from_mat(world_b));
  Bnd_Box all;
  for (const auto* side : {&xa, &xb})
    for (const auto& f : *side) all.Add(f.box);
  const double tol = 1e-7 + (all.IsVoid() ? 0.0 : 1e-6 * std::sqrt(all.SquareExtent()));
  for (auto* side : {&xa, &xb})
    for (auto& f : *side)
      if (!f.box.IsVoid()) f.box.Enlarge(tol);
  out.a.assign(xa.size(), FaceChanges::Removed);
  out.b.assign(xb.size(), FaceChanges::Added);
  // Unchanged: one of a's faces of the same type, area, centre and (analytic) surface; each taken once.
  std::vector<size_t> byArea(xa.size());
  std::iota(byArea.begin(), byArea.end(), size_t(0));
  std::sort(byArea.begin(), byArea.end(), [&](size_t i, size_t j) { return xa[i].area < xa[j].area; });
  for (size_t j = 0; j < xb.size(); ++j) {
    const Face& y = xb[j];
    if (!y.ok) continue;
    const double lo = y.area - areaTolerance(y.area, y.area, tol);
    auto it = std::lower_bound(byArea.begin(), byArea.end(), lo, [&](size_t i, double v) { return xa[i].area < v; });
    for (; it != byArea.end(); ++it) {
      Face& x = xa[*it];
      if (x.area - y.area > areaTolerance(x.area, y.area, tol)) break;
      if (x.taken || !x.ok || x.type != y.type || std::abs(x.area - y.area) > areaTolerance(x.area, y.area, tol)) continue;
      if (x.centre.Distance(y.centre) > std::max(10 * tol, 1e-5 * std::sqrt(y.area))) continue;
      if (x.analytic && !sameSurface(x, y, 10 * tol)) continue;
      x.taken = true;
      out.a[size_t(*it)] = FaceChanges::Unchanged;
      out.b[j] = FaceChanges::Unchanged;
      break;
    }
  }
  // Modified: a face left over on the surface of one of the other side's leftovers, the two overlapping.
  std::vector<size_t> la, lb;
  for (size_t i = 0; i < xa.size(); ++i)
    if (out.a[i] != FaceChanges::Unchanged && xa[i].analytic) la.push_back(i);
  for (size_t j = 0; j < xb.size(); ++j)
    if (out.b[j] != FaceChanges::Unchanged && xb[j].analytic) lb.push_back(j);
  if (double(la.size()) * double(lb.size()) <= kMaxPairs)
    for (size_t j : lb)
      for (size_t i : la)
        if (!xa[i].box.IsOut(xb[j].box) && sameSurface(xa[i], xb[j], 10 * tol)) {
          out.a[i] = FaceChanges::Modified;
          out.b[j] = FaceChanges::Modified;
        }
  return out;
}
}  // namespace opad
