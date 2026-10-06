// Involute gears for the gear feature (features.cpp): a spur gear, an internal (ring) gear and a rack, as standard full-depth
// teeth: addendum m, dedendum 1.25 m, flanks the involute of the base circle (r cos pressure angle) traced by B-splines
// through exact points, tips and roots arcs. A spur gear's tooth 0 points along +X, a ring's gap 0 does (so a pinion and a
// ring both placed at phase 0 mesh on the X axis); the profile is extruded along +Z by the face width.
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GeomAPI_Interpolate.hxx>
#include <TColgp_HArray1OfPnt.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>

#include <cmath>
#include <vector>

#include "opad/util.hpp"

namespace opad::design {

namespace {

constexpr double kPi = 3.14159265358979323846;
double inv(double a) { return std::tan(a) - a; }

gp_Pnt polar(double r, double a) { return gp_Pnt(r * std::cos(a), r * std::sin(a), 0); }

TopoDS_Edge arc(double r, double a0, double a1) {
  // Counter-clockwise from a0 to a1 on the circle of radius r about the origin.
  const gp_Circ c(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), r);
  return BRepBuilderAPI_MakeEdge(GC_MakeArcOfCircle(c, polar(r, a0), polar(r, a1), true).Value()).Edge();
}

TopoDS_Edge through(const std::vector<gp_Pnt>& pts) {
  if (pts.size() == 2) return BRepBuilderAPI_MakeEdge(pts[0], pts[1]).Edge();
  Handle(TColgp_HArray1OfPnt) a = new TColgp_HArray1OfPnt(1, int(pts.size()));
  for (size_t i = 0; i < pts.size(); ++i) a->SetValue(int(i) + 1, pts[i]);
  GeomAPI_Interpolate interp(a, false, 1e-7);
  interp.Perform();
  if (!interp.IsDone()) throw Error("the gear's tooth flank could not be traced");
  return BRepBuilderAPI_MakeEdge(interp.Curve()).Edge();
}

// The flank angle about a tooth's centre line at radius rho (external tooth): pi/(2z) + inv(alpha) - inv(alpha_rho).
struct Involute {
  double rb, half;  // base radius, the tooth's half angle at the base circle (inv terms included)
  double angle(double rho) const {
    const double a = rho <= rb ? 0.0 : std::acos(rb / rho);
    return half - inv(a);
  }
};

std::vector<double> radii(double from, double to, int n) {
  std::vector<double> out;
  for (int i = 0; i <= n; ++i) {
    // Even steps in the roll angle: more points near the base circle, where the involute bends most.
    const double rb = from;
    const double t0 = 0, t1 = std::sqrt(std::max(0.0, (to / rb) * (to / rb) - 1));
    const double t = t0 + (t1 - t0) * i / n;
    out.push_back(rb * std::sqrt(1 + t * t));
  }
  return out;
}

}  // namespace

// Gear profile as a closed wire in the XY plane.
TopoDS_Wire gear_wire(int z, double m, double alpha, double backlash, bool internal, double phase) {
  if (z < 6 || z > 1000) throw Error("a gear has 6 to 1000 teeth");
  if (m <= 0) throw Error("the module must be positive");
  if (alpha <= 0 || alpha >= 45 * kPi / 180) throw Error("the pressure angle is between 0 and 45 degrees");
  const double r = m * z / 2, rb = r * std::cos(alpha);
  // External: tips at r + m, roots at r - 1.25 m. Internal: tips (inward) at r - m, roots (outward) at r + 1.25 m.
  const double tip = internal ? r - m : r + m, root = internal ? r + 1.25 * m : r - 1.25 * m;
  if (root <= 0 || tip <= 0) throw Error("too few teeth for this module");
  // Half thickness at the base circle, less half the backlash on each flank (measured on the pitch circle).
  const double shrink = backlash / 4 / r;
  Involute inv_ext{rb, kPi / (2 * z) + inv(alpha) - shrink};
  // An internal tooth is an external gap: its flank angle about its centre is pi/z - the external one.
  auto flank = [&](double rho) { return internal ? kPi / z - inv_ext.angle(rho) + 2 * shrink : inv_ext.angle(rho); };
  const double lo = std::min(tip, root), hi = std::max(tip, root);
  const double start = std::max(lo, rb);  // the involute starts on the base circle; below it the flank is radial
  BRepBuilderAPI_MakeWire wire;
  const double pitch = 2 * kPi / z;
  for (int k = 0; k < z; ++k) {
    const double c = phase + k * pitch + (internal ? pitch / 2 : 0.0);  // the tooth's centre line
    // One tooth, counter-clockwise: the flank at c - angle from the root to the tip, the tip arc, the flank at c + angle
    // back to the root, then the root arc to the next tooth.
    const std::vector<double> rs = radii(start, internal ? hi : tip, 10);
    if (!internal) {
      const bool radial = root < rb;  // below the base circle the flank is a radial line
      std::vector<gp_Pnt> up;           // the involute on the c - angle side, base (or root) to tip
      for (double rho : rs) up.push_back(polar(rho, c - flank(rho)));
      if (radial) wire.Add(BRepBuilderAPI_MakeEdge(polar(root, c - flank(rb)), up.front()).Edge());
      wire.Add(through(up));
      wire.Add(arc(tip, c - flank(tip), c + flank(tip)));
      std::vector<gp_Pnt> down;  // the c + angle side, tip back to the base
      for (auto it = rs.rbegin(); it != rs.rend(); ++it) down.push_back(polar(*it, c + flank(*it)));
      wire.Add(through(down));
      if (radial) wire.Add(BRepBuilderAPI_MakeEdge(down.back(), polar(root, c + flank(rb))).Edge());
      const double edge = radial ? flank(rb) : flank(root);
      wire.Add(arc(root, c + edge, c + pitch - edge));
    } else {
      // Internal: the tooth points inwards; going counter-clockwise round the hole, its flank at c - angle runs from the
      // root (outside) in to the tip, the tip arc, the flank at c + angle back out, the root arc to the next tooth.
      std::vector<gp_Pnt> in_flank, out_flank;
      std::vector<double> down;
      for (double rho : rs)
        if (rho >= std::max(tip, rb) - 1e-12 && rho <= root + 1e-12) down.push_back(rho);
      if (down.empty() || std::fabs(down.back() - root) > 1e-9) down.push_back(root);
      if (std::fabs(down.front() - std::max(tip, rb)) > 1e-9) down.insert(down.begin(), std::max(tip, rb));
      // root -> tip on the c - angle side
      for (auto it = down.rbegin(); it != down.rend(); ++it) in_flank.push_back(polar(*it, c - flank(*it)));
      if (tip < rb) in_flank.push_back(polar(tip, c - flank(rb)));
      wire.Add(through(std::vector<gp_Pnt>(in_flank.begin(), in_flank.begin() + long(tip < rb ? in_flank.size() - 1 : in_flank.size()))));
      if (tip < rb) wire.Add(BRepBuilderAPI_MakeEdge(in_flank[in_flank.size() - 2], in_flank.back()).Edge());
      const double ta = tip < rb ? flank(rb) : flank(tip);
      wire.Add(arc(tip, c - ta, c + ta));
      if (tip < rb) out_flank.push_back(polar(tip, c + flank(rb)));
      for (double rho : down) out_flank.push_back(polar(rho, c + flank(rho)));
      if (tip < rb) wire.Add(BRepBuilderAPI_MakeEdge(out_flank[0], out_flank[1]).Edge());
      wire.Add(through(std::vector<gp_Pnt>(out_flank.begin() + long(tip < rb ? 1 : 0), out_flank.end())));
      wire.Add(arc(root, c + flank(root), c + pitch - flank(root)));
    }
  }
  if (!wire.IsDone()) throw Error("the gear's outline could not be closed");
  return wire.Wire();
}

TopoDS_Shape make_gear(const std::string& type, int z, double m, double alpha, double width, double bore, double rim, double backlash, double phase,
                       double rack_height) {
  if (width <= 0) throw Error("the face width must be positive");
  const gp_Vec along(0, 0, width);
  if (type == "rack") {
    // Teeth along +X from x = 0, pitch line on y = 0, tips at +m, roots at -1.25 m, body below.
    const double p = kPi * m, t = std::tan(alpha), shift = phase / (2 * kPi) * p;
    const double base = -1.25 * m - std::max(rack_height, 0.5 * m);
    BRepBuilderAPI_MakeWire wire;
    std::vector<gp_Pnt> pts;
    pts.push_back(gp_Pnt(0, base, 0));
    for (int k = 0; k < z; ++k) {
      const double c = shift + (k + 0.5) * p;  // a tooth's centre
      const double half_pitch = p / 4 - backlash / 4;
      auto x = [&](double y, double side) { return c + side * (half_pitch - y * t); };
      pts.push_back(gp_Pnt(std::max(0.0, std::min(z * p, x(-1.25 * m, -1))), -1.25 * m, 0));
      pts.push_back(gp_Pnt(x(m, -1), m, 0));
      pts.push_back(gp_Pnt(x(m, 1), m, 0));
      pts.push_back(gp_Pnt(std::max(0.0, std::min(z * p, x(-1.25 * m, 1))), -1.25 * m, 0));
    }
    pts.push_back(gp_Pnt(z * p, -1.25 * m, 0));
    pts.push_back(gp_Pnt(z * p, base, 0));
    // The left end: from the base corner up to the root line at x = 0.
    pts.insert(pts.begin() + 1, gp_Pnt(0, -1.25 * m, 0));
    std::vector<gp_Pnt> clean;
    for (const auto& q : pts)
      if (clean.empty() || clean.back().Distance(q) > 1e-9) clean.push_back(q);
    for (size_t i = 0; i < clean.size(); ++i) wire.Add(BRepBuilderAPI_MakeEdge(clean[i], clean[(i + 1) % clean.size()]).Edge());
    const TopoDS_Face face = BRepBuilderAPI_MakeFace(wire.Wire(), true).Face();
    return BRepPrimAPI_MakePrism(face, along).Shape();
  }
  const bool internal = type == "internal";
  const TopoDS_Wire profile = gear_wire(z, m, alpha, backlash, internal, phase);
  if (!internal) {
    TopoDS_Shape s = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(profile, true).Face(), along).Shape();
    if (bore > 0) {
      if (bore >= m * z - 2.5 * m) throw Error("the bore is wider than the gear's roots");
      s = BRepAlgoAPI_Cut(s, BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -1), gp_Dir(0, 0, 1)), bore / 2, width + 2).Shape()).Shape();
    }
    return s;
  }
  const double root = m * z / 2 + 1.25 * m;
  const double outer = rim > 0 ? rim / 2 : root + 3 * m;
  if (outer <= root) throw Error("the ring's rim must be wider than its tooth roots (" + std::to_string(2 * root) + " mm)");
  const TopoDS_Shape disc = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), outer, width).Shape();
  const TopoDS_Shape hole = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(profile, true).Face(), gp_Vec(0, 0, width + 2)).Shape();
  gp_Trsf down;
  down.SetTranslation(gp_Vec(0, 0, -1));
  return BRepAlgoAPI_Cut(disc, hole.Moved(TopLoc_Location(down))).Shape();
}

}  // namespace opad::design
