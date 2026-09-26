// Volume and area integrated span by span (gap log #4), against formulas and dense polygons.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepPrimAPI_MakeTorus.hxx>
#include <GProp_GProps.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <chrono>
#include <cmath>
#include <vector>

#include "check.hpp"
#include "opad/mass.hpp"

using namespace opad;

namespace {

void near_rel(double got, double want, double rel, const char* what) {
  if (std::abs(got - want) > rel * std::max(1.0, std::abs(want)))
    throw check::Failure(std::string(what) + ": " + std::to_string(got) + " instead of " + std::to_string(want));
}

void near_point(const gp_Pnt& got, const gp_Pnt& want, double tol, const char* what) {
  if (got.Distance(want) > tol)
    throw check::Failure(std::string(what) + ": centre off by " + std::to_string(got.Distance(want)));
}

// The arm's cycloidal disc outline (pin circle 30, pin radius 2.65, eccentricity 0.8, 25 lobes) through 640 points,
// as the gap log's agent built it: one closed interpolating spline.
Handle(Geom_BSplineCurve) disc_outline(int points = 640, int lobes = 25) {
  const double R = 30, rr = 2.65, e = 0.8;
  const int n = lobes + 1;
  Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, points);
  for (int i = 0; i < points; ++i) {
    const double t = 2 * M_PI * i / points;
    const double psi = std::atan2(std::sin((1 - n) * t), R / (e * n) - std::cos((1 - n) * t));
    pts->SetValue(i + 1, gp_Pnt(R * std::cos(t) - rr * std::cos(t + psi) - e * std::cos(n * t),
                                -R * std::sin(t) + rr * std::sin(t + psi) + e * std::sin(n * t), 0));
  }
  GeomAPI_Interpolate fit(pts, Standard_True, 1e-9);
  fit.Perform();
  return fit.Curve();
}

// Area and centroid of a closed planar curve from a dense polygon (shoelace), in its XY plane or XZ plane.
void polygon(const Handle(Geom_Curve)& c, bool xz, double& area, double& cx, double& cy) {
  const int n = 400000;
  const double a = c->FirstParameter(), b = c->LastParameter();
  area = cx = cy = 0;
  gp_Pnt p0 = c->Value(a);
  for (int i = 1; i <= n; ++i) {
    const gp_Pnt p1 = c->Value(i == n ? a : a + (b - a) * i / n);
    const double x0 = p0.X(), y0 = xz ? p0.Z() : p0.Y(), x1 = p1.X(), y1 = xz ? p1.Z() : p1.Y();
    const double cross = x0 * y1 - x1 * y0;
    area += cross;
    cx += (x0 + x1) * cross;
    cy += (y0 + y1) * cross;
    p0 = p1;
  }
  area *= 0.5;
  cx /= 6 * area;
  cy /= 6 * area;
  area = std::abs(area);
}

}  // namespace

TEST(analytic_solids_match_their_formulas) {
  {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(1, 2, 3), 10, 20, 30).Shape();
    const auto v = volume_properties(box), a = area_properties(box);
    near_rel(v.mass, 6000, 1e-12, "box volume");
    near_rel(a.mass, 2 * (200 + 300 + 600), 1e-12, "box area");
    near_point(v.centre, gp_Pnt(6, 12, 18), 1e-9, "box");
  }
  {
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 5), gp_Dir(1, 1, 0)), 5, 10).Shape();
    const auto v = volume_properties(cyl), a = area_properties(cyl);
    near_rel(v.mass, M_PI * 25 * 10, 1e-12, "cylinder volume");
    near_rel(a.mass, 2 * M_PI * 5 * 10 + 2 * M_PI * 25, 1e-12, "cylinder area");
    near_point(v.centre, gp_Pnt(5 / std::sqrt(2.0), 5 / std::sqrt(2.0), 5), 1e-9, "cylinder");
  }
  {
    const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(3, 0, 0), 7).Shape();
    near_rel(volume_properties(sphere).mass, 4.0 / 3 * M_PI * 343, 1e-12, "sphere volume");
    near_rel(area_properties(sphere).mass, 4 * M_PI * 49, 1e-12, "sphere area");
    near_point(volume_properties(sphere).centre, gp_Pnt(3, 0, 0), 1e-9, "sphere");
  }
  {
    const TopoDS_Shape torus = BRepPrimAPI_MakeTorus(10, 3).Shape();
    near_rel(volume_properties(torus).mass, 2 * M_PI * M_PI * 10 * 9, 1e-12, "torus volume");
    near_rel(area_properties(torus).mass, 4 * M_PI * M_PI * 10 * 3, 1e-12, "torus area");
  }
  {
    const TopoDS_Shape cone = BRepPrimAPI_MakeCone(5, 2, 8).Shape();
    near_rel(volume_properties(cone).mass, M_PI * 8 / 3 * (25 + 10 + 4), 1e-12, "frustum volume");
    near_rel(area_properties(cone).mass, M_PI * (5 + 2) * std::sqrt(9.0 + 64) + M_PI * 25 + M_PI * 4, 1e-12, "frustum area");
  }
  {
    // A plate with a hole, one face reversed by the cut.
    const TopoDS_Shape plate = BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(40, 30, 5).Shape(),
                                               BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(20, 15, -1), gp_Dir(0, 0, 1)), 6, 7).Shape()).Shape();
    near_rel(volume_properties(plate).mass, 40 * 30 * 5 - M_PI * 36 * 5, 1e-12, "plate volume");
    near_rel(area_properties(plate).mass, 2 * (40 * 30 - M_PI * 36) + 2 * (40 + 30) * 5 + 2 * M_PI * 6 * 5, 1e-12, "plate area");
  }
}

// Gap log #4: a disc bounded by a 640-point spline read 3.9 % over in volume and 7 % in face area.
TEST(a_prism_on_a_long_spline_matches_its_outline) {
  const Handle(Geom_BSplineCurve) outline = disc_outline();
  double area = 0, cx = 0, cy = 0;
  polygon(outline, false, area, cx, cy);
  const TopoDS_Face face = BRepBuilderAPI_MakeFace(gp_Pln(), BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(outline)).Wire());
  const TopoDS_Shape disc = BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, 6)).Shape();
  const auto started = std::chrono::steady_clock::now();
  const auto v = volume_properties(disc);
  const auto a = area_properties(face);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  std::printf("  spline disc: face %.6f (outline %.6f), volume %.6f in %.3f s\n", a.mass, area, v.mass, seconds);
  near_rel(a.mass, area, 1e-8, "face area");
  near_rel(v.mass, 6 * area, 1e-8, "volume");
  near_point(a.centre, gp_Pnt(cx, cy, 0), 1e-6, "face");
  near_point(v.centre, gp_Pnt(cx, cy, 3), 1e-6, "disc");
  // What BRepGProp gives, for the record: the error this replaces.
  GProp_GProps g;
  BRepGProp::SurfaceProperties(face, g);
  CHECK(std::abs(g.Mass() - area) > 1e-3 * area);
  CHECK(seconds < 2);
}

TEST(a_revolved_spline_matches_pappus) {
  // A wavy closed profile in the XZ plane, off the axis, turned about Z.
  const int n = 120;
  Handle(TColgp_HArray1OfPnt) pts = new TColgp_HArray1OfPnt(1, n);
  for (int i = 0; i < n; ++i) {
    const double t = 2 * M_PI * i / n, r = 4 + 0.6 * std::cos(9 * t);
    pts->SetValue(i + 1, gp_Pnt(20 + r * std::cos(t), 0, r * std::sin(t)));
  }
  GeomAPI_Interpolate fit(pts, Standard_True, 1e-9);
  fit.Perform();
  const Handle(Geom_BSplineCurve) profile = fit.Curve();
  double area = 0, cx = 0, cz = 0;
  polygon(profile, true, area, cx, cz);
  const TopoDS_Face face = BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)), BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(profile)).Wire());
  const TopoDS_Shape ring = BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1))).Shape();
  const auto v = volume_properties(ring);
  near_rel(v.mass, 2 * M_PI * cx * area, 1e-8, "revolved volume");
  near_point(v.centre, gp_Pnt(0, 0, cz), 1e-6, "revolved");
  // Half a turn: the centroid moves off the axis by 4/(3 pi) of ... checked through the volume only.
  const TopoDS_Shape half = BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), M_PI).Shape();
  near_rel(volume_properties(half).mass, M_PI * cx * area, 1e-8, "half-turn volume");
}

int main(int argc, char** argv) { return check::run_all(argc, argv); }
