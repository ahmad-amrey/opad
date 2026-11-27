// Flow studies (sim/flow.hpp): the fluid between a part's openings, or around it, solved with OpenFOAM.
#include "opad/sim/flow.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <BRepBuilderAPI_FindPlane.hxx>
#include <TopExp.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <Geom_Plane.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>

#include "foam_case.hpp"
#include "opad/geometry.hpp"
#include "opad/sim/cfd.hpp"
#include "opad/sim/fea.hpp"

namespace opad::sim {

namespace fs = std::filesystem;
using foam::num;
using foam::vec;

namespace {

using V = Vec3;
constexpr double kPi = 3.14159265358979323846;
constexpr double kWedge = 5.0 * kPi / 180;  // the axisymmetric mesh's wedge, all told
constexpr double kCfm = 4.719474432e-4;     // m3/s

V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V add(const V& a, const V& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V mul(const V& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double norm(const V& a) { return std::sqrt(dot(a, a)); }
V unit(const V& a) {
  const double n = norm(a);
  return n > 1e-12 ? mul(a, 1 / n) : V{0, 0, 0};
}
// A direction across a (unit): the world axis least along it, made square to it.
V across(const V& a) {
  V axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  std::sort(std::begin(axes), std::end(axes), [&](const V& x, const V& y) { return std::fabs(dot(x, a)) < std::fabs(dot(y, a)); });
  return unit(sub(axes[0], mul(a, dot(axes[0], a))));
}

// m3/s in one of a flow's units.
double flow_unit(const std::string& u) {
  if (u == "cfm") return kCfm;
  if (u == "m3/h" || u == "m3h") return 1.0 / 3600;
  if (u == "l/s" || u == "ls") return 1e-3;
  if (u == "m3/min" || u == "m3min") return 1.0 / 60;
  if (u == "m3/s") return 1.0;
  throw Error("flow: a flow's unit is cfm, m3/h or l/s (or m3/min, m3/s), not \"" + u + "\"");
}
// The same unit as sim/airflow.hpp names it (fan_from's keys).
std::string fan_flow_key(const std::string& u) {
  if (u == "cfm") return "cfm";
  if (u == "m3/h" || u == "m3h") return "m3h";
  if (u == "l/s" || u == "ls") return "ls";
  if (u == "m3/min" || u == "m3min") return "m3min";
  throw Error("flow: a fan's unit is cfm, m3/h, l/s or m3/min, not \"" + u + "\"");
}
double speed_unit(const std::string& u) {
  if (u == "m/s") return 1.0;
  if (u == "km/h") return 1 / 3.6;
  if (u == "mph") return 0.44704;
  if (u == "ft/min" || u == "fpm") return 0.00508;
  throw Error("flow: a speed's unit is m/s, km/h, mph or ft/min, not \"" + u + "\"");
}

// An opening as given: the edges of the loop around it (any order), or one flat face (older studies).
std::vector<Ref> opening_refs(const json& v, const char* what) {
  std::vector<Ref> out;
  const json list = v.is_array() ? v : json::array({v});
  for (const auto& r : list) {
    Ref ref;
    try {
      ref = Ref::from_json(r);
    } catch (const std::exception& e) {
      throw Error(std::string("flow: ") + what + ": " + e.what());
    }
    if (ref.kind != Ref::Kind::Face && ref.kind != Ref::Kind::Edge) {
      const std::string kind = Ref::kind_name(ref.kind);
      throw Error(std::string("flow: ") + what + " is the loop of edges around the opening (or the flat face at that end of the part), not " +
                  (kind.find_first_of("aeiou") == 0 ? "an " : "a ") + kind);
    }
    if (!out.empty() && (ref.kind == Ref::Kind::Face || out.front().kind == Ref::Kind::Face))
      throw Error(std::string("flow: ") + what + ": one face, or the edges of one loop");
    if (!out.empty() && ref.body != out.front().body) throw Error(std::string("flow: ") + what + "'s edges are on one body");
    out.push_back(ref);
  }
  if (out.empty()) throw Error(std::string("flow: ") + what + " names no edge or face");
  return out;
}

// Darcy friction factor of a smooth pipe (laminar 64/Re; Haaland above).
double friction_factor(double Re) {
  if (Re <= 0) return 0;
  if (Re < 2300) return 64 / Re;
  const double s = -1.8 * std::log10(6.9 / Re);
  return 1 / (s * s);
}

// ---------------------------------------------------------------- the geometry
// An end of the fluid: the opening (a face across it) and the face that was picked.
struct Opening {
  TopoDS_Face picked, face;  // face: across the opening (the picked face itself, or the face of its hole)
  bool hole = false;
  std::string body;
  V centre{0, 0, 0}, normal{0, 0, 1};  // out of the fluid
  double area = 0, perimeter = 0;      // mm2, mm
  double rmin = 0, rmax = 0;           // the rim's distance from the centre
  std::vector<V> rim;
  bool oriented = true;                // false: a loop in a wall, its normal's way not known yet (orient)
};

std::vector<V> wire_points(const TopoDS_Wire& w, const TopoDS_Face& f) {
  std::vector<V> out;
  for (BRepTools_WireExplorer e(w, f); e.More(); e.Next()) {
    BRepAdaptor_Curve c(e.Current());
    const int n = c.GetType() == GeomAbs_Line ? 2 : 48;
    // In the wire's order: a reversed edge from its last parameter (else a polygon's outline zigzags).
    const bool back = e.Current().Orientation() == TopAbs_REVERSED;
    const double t0 = back ? c.LastParameter() : c.FirstParameter(), t1 = back ? c.FirstParameter() : c.LastParameter();
    for (int k = 0; k < n; ++k) {
      const gp_Pnt p = c.Value(t0 + (t1 - t0) * k / n);
      out.push_back({p.X(), p.Y(), p.Z()});
    }
  }
  return out;
}

struct Geometry {
  FlowPlan plan;
  Opening in, out;
  std::vector<TopoDS_Shape> walls;  // world shapes of plan.walls
  V e1{1, 0, 0};                    // across the axis (the wedge's radial way, the cut's)
};

// The loop of edges `edges` (one closed planar loop in any order): its wire and plane, or why not.
std::string loop_wire(const std::vector<TopoDS_Edge>& edges, TopoDS_Wire& out, gp_Pln& plane) {
  Handle(TopTools_HSequenceOfShape) seq = new TopTools_HSequenceOfShape, wires;
  Bnd_Box box;
  for (const auto& e : edges) seq->Append(e), BRepBndLib::Add(e, box);
  if (box.IsVoid()) return " has no edges";
  const double size = std::sqrt(box.SquareExtent());
  ShapeAnalysis_FreeBounds::ConnectEdgesToWires(seq, std::max(1e-4, 1e-6 * size), false, wires);
  if (wires.IsNull() || wires->Length() != 1) return "'s edges are not one loop (" + std::to_string(wires.IsNull() ? 0 : wires->Length()) + " pieces)";
  out = TopoDS::Wire(wires->Value(1));
  TopoDS_Vertex a, b;
  TopExp::Vertices(out, a, b);
  if (a.IsNull() || b.IsNull() || BRep_Tool::Pnt(a).Distance(BRep_Tool::Pnt(b)) > std::max(1e-4, 1e-6 * size)) return "'s edges do not close round the opening";
  BRepBuilderAPI_FindPlane fp(out, std::max(1e-3, 1e-5 * size));
  if (!fp.Found()) return "'s edges do not lie in one plane";
  plane = fp.Plane()->Pln();
  return {};
}

const TopoDS_Shape& wall_shape(const Document& doc, const Scene& scene, const Ref& r, const char* what, Geometry& g) {
  const Node* n = scene.node(r.body);
  if (!n || n->kind != Node::Kind::Body) throw Error(std::string("flow: ") + what + " names no body (" + r.body + ")");
  if (n->body_missing || n->representation != "solid") throw Error(std::string("flow: ") + what + ": \"" + n->name + "\" is not a solid");
  auto it = std::find(g.plan.walls.begin(), g.plan.walls.end(), r.body);
  if (it == g.plan.walls.end()) {
    g.plan.walls.push_back(r.body);
    g.walls.push_back(node_world_shape(doc, scene, r.body));
    it = g.plan.walls.end() - 1;
  }
  return g.walls[size_t(it - g.plan.walls.begin())];
}

// The edges of a face's wire, its seams left out.
std::vector<TopoDS_Edge> wire_edges(const TopoDS_Wire& w, const TopoDS_Face& f) {
  std::vector<TopoDS_Edge> out;
  for (TopExp_Explorer e(w, TopAbs_EDGE); e.More(); e.Next()) {
    const TopoDS_Edge ed = TopoDS::Edge(e.Current());
    if (BRep_Tool::IsClosed(ed, f)) continue;
    if (std::none_of(out.begin(), out.end(), [&](const TopoDS_Edge& x) { return x.IsSame(ed); })) out.push_back(ed);
  }
  return out;
}
bool same_edges(const std::vector<TopoDS_Edge>& a, const std::vector<TopoDS_Edge>& b) {
  if (a.size() != b.size()) return false;
  for (const auto& e : b)
    if (std::none_of(a.begin(), a.end(), [&](const TopoDS_Edge& x) { return x.IsSame(e); })) return false;
  return true;
}

Opening opening_of(const Document& doc, const Scene& scene, const std::vector<Ref>& refs, const char* what, Geometry& g) {
  const Ref& r = refs.front();
  const TopoDS_Shape& shape = wall_shape(doc, scene, r, what, g);
  const std::string name = scene.node(r.body)->name;
  Opening o;
  o.body = r.body;
  if (r.kind == Ref::Kind::Edge) {
    // The loop round the opening: the disc it bounds is the opening. A loop that is a flat face's whole boundary is the end of
    // a body that is the fluid; any other (a rim's hole, the edge of a chamfer or of the bore) is an opening in a wall, whose
    // outward way is known once the other opening is (away from it).
    std::vector<TopoDS_Edge> edges;
    for (const Ref& e : refs) {
      const TopoDS_Shape s = subshape(shape, Ref::Kind::Edge, e.index);
      if (s.IsNull() || s.ShapeType() != TopAbs_EDGE) throw Error(std::string("flow: ") + what + ": \"" + name + "\" has no edge " + std::to_string(e.index));
      edges.push_back(TopoDS::Edge(s));
    }
    TopoDS_Wire wire;
    gp_Pln plane;
    if (const std::string why = loop_wire(edges, wire, plane); !why.empty()) throw Error(std::string("flow: the ") + what + why);
    TopoDS_Face whole;
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More() && whole.IsNull(); f.Next()) {
      const TopoDS_Face face = TopoDS::Face(f.Current());
      if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane) continue;
      int wires = 0;
      for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) ++wires;
      if (wires == 1 && same_edges(wire_edges(BRepTools::OuterWire(face), face), edges)) whole = face;
    }
    if (!whole.IsNull()) {
      Ref face{r.body, Ref::Kind::Face, subshape_index(shape, whole)};
      return opening_of(doc, scene, {face}, what, g);
    }
    BRepBuilderAPI_MakeFace mk(plane, wire, true);
    if (!mk.IsDone()) throw Error(std::string("flow: ") + what + ": its loop could not be made a face");
    ShapeFix_Face fix(mk.Face());
    fix.Perform();
    o.face = fix.Face();
    o.hole = true;
    o.oriented = false;
    const gp_Dir nd = plane.Axis().Direction();
    o.normal = {nd.X(), nd.Y(), nd.Z()};
    o.rim = wire_points(BRepTools::OuterWire(o.face), o.face);
  } else {
    if (refs.size() != 1) throw Error(std::string("flow: ") + what + ": pick the one flat face at that end (several faces for one end are not taken)");
    const TopoDS_Shape f = subshape(shape, Ref::Kind::Face, r.index);
    if (f.IsNull() || f.ShapeType() != TopAbs_FACE) throw Error(std::string("flow: ") + what + ": \"" + name + "\" has no face " + std::to_string(r.index));
    o.picked = TopoDS::Face(f);
    BRepAdaptor_Surface surface(o.picked);
    if (surface.GetType() != GeomAbs_Plane)
      throw Error(std::string("flow: ") + what + " must be a flat face: the rim at the end of a duct, or the end of a body that is the fluid");
    gp_Dir nd = surface.Plane().Axis().Direction();
    if (!surface.Plane().Position().Direct()) nd.Reverse();
    if (o.picked.Orientation() == TopAbs_REVERSED) nd.Reverse();
    o.normal = {nd.X(), nd.Y(), nd.Z()};
    const TopoDS_Wire outer = BRepTools::OuterWire(o.picked);
    std::vector<TopoDS_Wire> holes;
    for (TopExp_Explorer w(o.picked, TopAbs_WIRE); w.More(); w.Next())
      if (!w.Current().IsSame(outer)) holes.push_back(TopoDS::Wire(w.Current()));
    if (holes.size() > 1)
      throw Error(std::string("flow: ") + what + "'s face has " + std::to_string(holes.size()) + " holes: pick an end face with one opening");
    if (holes.size() == 1) {
      // The hole is the opening: a face of its own across it, on the rim's plane.
      BRepBuilderAPI_MakeFace mk(surface.Plane(), holes.front(), true);
      if (!mk.IsDone()) throw Error(std::string("flow: ") + what + ": its opening could not be made a face");
      ShapeFix_Face fix(mk.Face());
      fix.Perform();
      o.face = fix.Face();
      o.hole = true;
      o.rim = wire_points(holes.front(), o.picked);
    } else {
      o.face = o.picked;
      o.rim = wire_points(outer, o.picked);
    }
  }
  GProp_GProps p;
  BRepGProp::SurfaceProperties(o.face, p);
  o.area = std::fabs(p.Mass());
  o.centre = {p.CentreOfMass().X(), p.CentreOfMass().Y(), p.CentreOfMass().Z()};
  GProp_GProps l;
  BRepGProp::LinearProperties(o.face, l);
  o.perimeter = l.Mass();
  if (!(o.area > 0) || !(o.perimeter > 0)) throw Error(std::string("flow: ") + what + "'s opening has no area");
  o.rmin = 1e300, o.rmax = 0;
  for (const V& q : o.rim) {
    const double d = norm(sub(q, o.centre));
    o.rmin = std::min(o.rmin, d), o.rmax = std::max(o.rmax, d);
  }
  return o;
}

// A loop's opening faces away from the other opening (out of the fluid).
void orient(Opening& o, const Opening& other, const char* what) {
  if (o.oriented) return;
  const V d = sub(other.centre, o.centre);
  const double along = dot(d, o.normal);
  if (std::fabs(along) < 1e-3 * std::max(1.0, norm(d)))
    throw Error(std::string("flow: the ") + what + " lies in one plane with the other opening: which side of it the part is cannot be told (pick the rim face at that end)");
  if (along > 0) o.normal = mul(o.normal, -1.0);
  o.oriented = true;
}

// Douglas-Peucker on a profile (z, r): the points that keep it within tol.
std::vector<std::array<double, 2>> simplify(const std::vector<std::array<double, 2>>& pts, double tol) {
  if (pts.size() < 3) return pts;
  std::vector<char> keep(pts.size(), 0);
  keep.front() = keep.back() = 1;
  std::vector<std::pair<size_t, size_t>> todo{{0, pts.size() - 1}};
  while (!todo.empty()) {
    const auto [a, b] = todo.back();
    todo.pop_back();
    const double dz = pts[b][0] - pts[a][0], dr = pts[b][1] - pts[a][1], len = std::hypot(dz, dr);
    double worst = 0;
    size_t at = 0;
    for (size_t i = a + 1; i < b; ++i) {
      const double d = len > 0 ? std::fabs(dz * (pts[i][1] - pts[a][1]) - dr * (pts[i][0] - pts[a][0])) / len : std::hypot(pts[i][0] - pts[a][0], pts[i][1] - pts[a][1]);
      if (d > worst) worst = d, at = i;
    }
    if (worst > tol) {
      keep[at] = 1;
      todo.push_back({a, at}), todo.push_back({at, b});
    }
  }
  std::vector<std::array<double, 2>> out;
  for (size_t i = 0; i < pts.size(); ++i)
    if (keep[i]) out.push_back(pts[i]);
  return out;
}

struct Preset {
  int radial, across, wedge_iterations, iterations;
};
Preset preset(const std::string& q) {
  if (q == "quick") return {20, 8, 2000, 800};
  if (q == "fine") return {48, 16, 6000, 3000};
  return {32, 12, 3500, 1500};
}

// Whether the fluid between the openings is round about the line through their centres: rays from that axis meet the walls
// at one distance all round, all along it. Fills the profile; returns why not (empty: round).
std::string round_profile(Geometry& g, int radial) {
  FlowPlan& plan = g.plan;
  const V a = plan.axis;
  const double L = dot(sub(g.out.centre, g.in.centre), a);
  if (dot(g.out.normal, a) < std::cos(1.0 * kPi / 180)) return "the outlet's face is not square to the line from the inlet (an elbow or a bend)";
  if (!(L > 0)) return "the outlet is not ahead of the inlet";
  const double off = norm(sub(sub(g.out.centre, g.in.centre), mul(a, L)));
  if (off > 0.1 + 0.002 * std::max(g.in.rmax, g.out.rmax)) return "the outlet's centre is " + num(std::round(off * 100) / 100) + " mm off the inlet's axis";
  for (const Opening* o : {&g.in, &g.out})
    if (o->rmax - o->rmin > 0.05 + 0.004 * o->rmax) return std::string("the ") + (o == &g.in ? "inlet" : "outlet") + " is not round";
  std::vector<std::unique_ptr<IntCurvesFace_ShapeIntersector>> hit;
  for (const auto& w : g.walls) {
    hit.push_back(std::make_unique<IntCurvesFace_ShapeIntersector>());
    hit.back()->Load(w, 1e-7);
  }
  const V e1 = g.e1, e2 = cross(a, e1);
  auto first_hit = [&](const V& p, const V& d) {
    double best = -1;
    const gp_Lin line(gp_Pnt(p[0], p[1], p[2]), gp_Dir(d[0], d[1], d[2]));
    for (auto& h : hit) {
      h->Perform(line, 1e-6, 1e9);
      for (int i = 1; h->IsDone() && i <= h->NbPnt(); ++i)
        if (const double w = h->WParameter(i); w > 1e-6 && (best < 0 || w < best)) best = w;
    }
    return best;
  };
  const int n = std::clamp(int(L / 0.25), 120, 1200);
  std::vector<std::array<double, 2>> pts{{0.0, g.in.rmax}};
  for (int k = 0; k < n; ++k) {
    const double z = L * (k + 0.5) / n;
    const V p = add(g.in.centre, mul(a, z));
    const double r0 = first_hit(p, e1);
    if (r0 < 0) return "a ray from the axis " + num(std::round(z * 10) / 10) + " mm along meets no wall";
    const double tol = 0.05 + 0.004 * r0;
    for (int j = 1; j < 6; ++j) {
      const double phi = 2 * kPi * j / 6;
      const double r = first_hit(p, add(mul(e1, std::cos(phi)), mul(e2, std::sin(phi))));
      if (r < 0 || std::fabs(r - r0) > tol)
        return "the wall is not round about the openings' axis: " + num(std::round(r0 * 100) / 100) + " and " + (r < 0 ? std::string("no wall") : num(std::round(r * 100) / 100)) +
               " mm from it " + num(std::round(z * 10) / 10) + " mm along";
    }
    pts.push_back({z, r0});
  }
  pts.push_back({L, g.out.rmax});
  double rmax = 0;
  for (const auto& p : pts) rmax = std::max(rmax, p[1]);
  auto prof = simplify(pts, std::max(0.01, 2e-4 * rmax));
  // A sharp corner falls between two samples: the stretch between them (a sample's spacing or so) is the corner cut off.
  // Put the corner back where the lines on either side meet (or the stretch's middle when they do not meet near it).
  const double spacing = L / n;
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 1; i + 2 < prof.size(); ++i) {
      const auto &a = prof[i - 1], &b = prof[i], &c = prof[i + 1], &d = prof[i + 2];
      if (c[0] - b[0] > 3 * spacing) continue;
      const double s1 = (b[1] - a[1]) / std::max(1e-12, b[0] - a[0]), s2 = (d[1] - c[1]) / std::max(1e-12, d[0] - c[0]);
      // Lines that do not meet near the stretch (parallel ones: a sudden step) keep it as it is (refused below).
      if (std::fabs(s1 - s2) < 1e-9) continue;
      const double z = (c[1] - b[1] + s1 * b[0] - s2 * c[0]) / (s1 - s2);
      if (z < b[0] - spacing || z > c[0] + spacing) continue;
      prof[i] = {z, b[1] + s1 * (z - b[0])};
      prof.erase(prof.begin() + long(i) + 1);
      changed = true;
      break;
    }
  }
  // A wall square to the axis or nearly (a sudden step, steeper than 60 degrees): a step of the wedge's blocks (the cells
  // below its lower radius run on through it, those above it end on the step's face), never one block from the axis to the
  // wall (a sliver of cells sheared flat). Two steep stretches in a row the same way are one step; a step a third of a
  // cell high or less is no step (the stretch next to it takes it, when that leaves it gentle).
  auto angle = [&](size_t i) { return std::atan2(std::fabs(prof[i][1] - prof[i - 1][1]), std::max(0.0, prof[i][0] - prof[i - 1][0])) * 180 / kPi; };
  double rmin = 1e300;
  for (const auto& p : prof) rmin = std::min(rmin, p[1]);
  const double tiny = 0.33 * rmin / std::max(1, radial);
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 1; i < prof.size() && !changed; ++i) {
      if (angle(i) <= 60) continue;
      const double dr = prof[i][1] - prof[i - 1][1];
      if (i + 1 < prof.size() && angle(i + 1) > 60 && (prof[i + 1][1] - prof[i][1]) * dr > 0) {
        prof.erase(prof.begin() + long(i));
        changed = true;
      } else if (std::fabs(dr) < tiny) {
        // Drop the end whose loss leaves the gentler stretch (never an opening's own point).
        for (size_t drop : {i, i - 1}) {
          if (drop == 0 || drop + 1 == prof.size()) continue;
          auto tried = prof;
          tried.erase(tried.begin() + long(drop));
          const size_t j = drop;  // the stretch that now spans it ends at j
          const double a = std::atan2(std::fabs(tried[j][1] - tried[j - 1][1]), std::max(0.0, tried[j][0] - tried[j - 1][0])) * 180 / kPi;
          if (a <= 60) {
            prof = tried;
            changed = true;
            break;
          }
        }
      }
    }
  }
  double steepest = 0;
  plan.steps.clear();
  for (size_t i = 1; i < prof.size(); ++i) {
    const double ang = angle(i);
    if (ang > 60) {
      if (!plan.steps.empty() && plan.steps.back() + 1 == i - 1)
        return "a thin ring stands into the flow " + num(std::round(prof[i - 1][0] * 10) / 10) + " mm along (two steps with no wall between them)";
      plan.steps.push_back(i - 1);
      plan.step_deg = std::max(plan.step_deg, ang);
    } else {
      steepest = std::max(steepest, ang);
    }
  }
  plan.steepest_deg = steepest;
  plan.profile = prof;
  plan.length_mm = L;
  return {};
}

// The wedge's blocks: the profile cut at its steps into pieces; each piece's cells in bands (fractions of its wall's radius,
// the top one at the wall), one band per radius a step needs (a step's lower radius on its higher side, and every band
// radius that meets it from the other side), so the bands below a step's lower radius run on through it and those above it
// end on its face. Bands linked through steps are one group: the same cells across and the same grading.
struct WedgeLayout {
  struct Piece {
    size_t first = 0, last = 0;  // profile indices
    std::vector<double> lam;     // band tops as fractions of the wall's radius, rising, the last 1
    std::vector<std::vector<std::array<double, 2>>> st;  // stations: (x, r) of each band boundary, 0 = the axis
    std::vector<int> nx;                                  // cells along each stretch between stations
    std::vector<double> gx;                               // and their grading
    std::vector<int> group;                               // each band's group
    double lo_start = -1, lo_end = -1;                    // a step's lower radius at its first / last station (-1: an opening)
  };
  std::vector<Piece> pieces;
  std::vector<int> cells_across;      // per group
  std::vector<std::string> grading;   // per group: the radial grading
  size_t cells = 0;
};

std::string wedge_layout(const FlowPlan& plan, WedgeLayout& w) {
  const auto& prof = plan.profile;
  const double Lin = plan.inlet_extension_mm, Lout = plan.outlet_extension_mm, L = plan.length_mm, dx = plan.cell_mm;
  const int nr = plan.radial_cells;
  w = WedgeLayout();
  {
    size_t from = 0;
    for (size_t s : plan.steps) {
      w.pieces.push_back({});
      w.pieces.back().first = from, w.pieces.back().last = s;
      from = s + 1;
    }
    w.pieces.push_back({});
    w.pieces.back().first = from, w.pieces.back().last = prof.size() - 1;
  }
  const size_t np = w.pieces.size();
  for (size_t k = 0; k < np; ++k) {
    auto& p = w.pieces[k];
    p.lam = {1.0};
    const size_t stations = p.last - p.first + 1 + (k == 0 && Lin > 0) + (k + 1 == np && Lout > 0);
    if (stations < 2)
      return "a step right at the " + std::string(k == 0 ? "inlet" : "outlet") + " with no straight pipe beyond it (extend)";
  }
  auto insert = [](std::vector<double>& v, double x) {
    for (double y : v)
      if (std::fabs(y - x) < 1e-9) return false;
    v.insert(std::upper_bound(v.begin(), v.end(), x), x);
    return true;
  };
  // Band radii carried through the steps until none is new (a chain: two sweeps settle it).
  for (int pass = 0, changed = 1; changed && pass < 20; ++pass) {
    changed = 0;
    for (size_t k = 0; k + 1 < np; ++k) {
      auto &a = w.pieces[k], &b = w.pieces[k + 1];
      const double Ra = prof[a.last][1], Rb = prof[b.first][1], lo = std::min(Ra, Rb);
      for (double l : std::vector<double>(a.lam))
        if (l * Ra <= lo * (1 + 1e-9)) changed |= insert(b.lam, std::min(1.0, l * Ra / Rb));
      for (double l : std::vector<double>(b.lam))
        if (l * Rb <= lo * (1 + 1e-9)) changed |= insert(a.lam, std::min(1.0, l * Rb / Ra));
    }
  }
  for (const auto& p : w.pieces)
    if (p.lam.size() > 8) return "too many steps for a wedge (" + std::to_string(plan.steps.size()) + ")";
  // Groups: band j below a step's lower radius on one side is band j on the other.
  std::vector<std::vector<int>> id(np);
  int groups = 0;
  for (size_t k = 0; k < np; ++k)
    for (size_t j = 0; j < w.pieces[k].lam.size(); ++j) id[k].push_back(groups++);
  std::vector<int> parent(static_cast<size_t>(groups));
  std::iota(parent.begin(), parent.end(), 0);
  std::function<int(int)> root = [&](int i) { return parent[size_t(i)] == i ? i : parent[size_t(i)] = root(parent[size_t(i)]); };
  for (size_t k = 0; k + 1 < np; ++k) {
    auto &a = w.pieces[k], &b = w.pieces[k + 1];
    const double Ra = prof[a.last][1], Rb = prof[b.first][1], lo = std::min(Ra, Rb);
    a.lo_end = b.lo_start = lo;
    size_t na = 0, nb = 0;
    while (na < a.lam.size() && a.lam[na] * Ra <= lo * (1 + 1e-9)) ++na;
    while (nb < b.lam.size() && b.lam[nb] * Rb <= lo * (1 + 1e-9)) ++nb;
    if (na != nb || na == 0) return "the wedge's bands do not meet at a step";
    for (size_t j = 0; j < na; ++j) parent[size_t(root(id[k][j]))] = root(id[k + 1][j]);
  }
  // Each group's cells across: its widest band at the base cell count over the radius, a thin band at least as many as keep
  // its cells as fine as the graded cells beside it; the cells shrink to a fifth toward the wall in a group that meets it
  // (wall functions take the first cell where it falls), toward both ends in a band above another (a step's lip).
  std::map<int, double> dlam, thick;
  std::map<int, bool> top, above;
  for (size_t k = 0; k < np; ++k) {
    auto& p = w.pieces[k];
    double R = 0;
    for (size_t i = p.first; i <= p.last; ++i) R = std::max(R, prof[i][1]);
    for (size_t j = 0; j < p.lam.size(); ++j) {
      const int g = root(id[k][j]);
      const double d = p.lam[j] - (j ? p.lam[j - 1] : 0.0);
      dlam[g] = std::max(dlam[g], d), thick[g] = std::max(thick[g], d * R);
      top[g] = top[g] || j + 1 == p.lam.size();
      above[g] = above[g] || j > 0;
    }
  }
  std::map<int, int> dense;
  for (size_t k = 0; k < np; ++k)
    for (size_t j = 0; j < w.pieces[k].lam.size(); ++j) {
      const int g = root(id[k][j]);
      if (!dense.count(g)) {
        dense[g] = int(w.cells_across.size());
        int n = std::max(1, int(std::ceil(nr * dlam[g] - 1e-9)));
        std::string grading = "1";
        if (!above[g]) {
          n = std::max(n, 4);
          if (top[g]) grading = "0.2";
        } else {
          n = std::min(nr, std::max(n, int(std::ceil(thick[g] / (0.6 * dx)))));
          if (n >= 6) grading = top[g] ? "((0.5 0.5 3) (0.5 0.5 0.2))" : "((0.5 0.5 3) (0.5 0.5 0.33))";
        }
        w.cells_across.push_back(n);
        w.grading.push_back(grading);
      }
      w.pieces[k].group.push_back(dense[g]);
    }
  // Stations: the stubs' far ends, the profile's points; a step's station runs up its face (the bands above its lower
  // radius end on it), the bands below it at the face's foot.
  for (size_t k = 0; k < np; ++k) {
    auto& p = w.pieces[k];
    auto plain = [&](double x, double R) {
      std::vector<std::array<double, 2>> s{{x, 0.0}};
      for (double l : p.lam) s.push_back({x, l * R});
      return s;
    };
    auto on_step = [&](size_t a, size_t b, double R) {
      // The step between profile points a and b; this piece's wall radius there is R.
      const auto &P = prof[a], &Q = prof[b];
      const auto& low = P[1] < Q[1] ? P : Q;
      const auto& high = P[1] < Q[1] ? Q : P;
      std::vector<std::array<double, 2>> s{{low[0], 0.0}};
      for (double l : p.lam) {
        const double rho = l * R;
        if (rho <= low[1] * (1 + 1e-9)) s.push_back({low[0], std::min(rho, low[1])});
        else s.push_back({low[0] + (rho - low[1]) / (high[1] - low[1]) * (high[0] - low[0]), rho});
      }
      return s;
    };
    if (k == 0 && Lin > 0) p.st.push_back(plain(-Lin, prof[p.first][1]));
    for (size_t i = p.first; i <= p.last; ++i) {
      if (i == p.first && k > 0) p.st.push_back(on_step(i - 1, i, prof[i][1]));
      else if (i == p.last && k + 1 < np) p.st.push_back(on_step(i, i + 1, prof[i][1]));
      else p.st.push_back(plain(prof[i][0], prof[i][1]));
    }
    if (k + 1 == np && Lout > 0) p.st.push_back(plain(L + Lout, prof[p.last][1]));
    for (size_t i = 0; i + 1 < p.st.size(); ++i) {
      double len = 0;
      for (size_t j = 0; j < p.st[i].size(); ++j) len = std::max(len, p.st[i + 1][j][0] - p.st[i][j][0]);
      const bool stub_in = k == 0 && Lin > 0 && i == 0, stub_out = k + 1 == np && Lout > 0 && i + 2 == p.st.size();
      int nx;
      double grading = 1;
      if ((stub_in || stub_out) && len > 10 * dx) {
        nx = int(std::ceil(len / (2.5 * dx)));
        grading = stub_in ? 0.25 : 4;  // the cells next to the fitting at its size, four times it at the stub's other end
      } else {
        nx = std::max(1, int(std::lround(len / dx)));
      }
      p.nx.push_back(nx), p.gx.push_back(grading);
      for (int g : p.group) w.cells += size_t(nx) * size_t(w.cells_across[size_t(g)]);
    }
  }
  return {};
}

// About how long a run takes: meshing, then the cells times the iterations it usually needs times one iteration's cost per
// cell on one core. Measured with OPAD's OpenFOAM v2606 pack on one idle machine (16 cores, one used): on Windows (the
// MinGW build; OpenCFD's own v2512 build runs as fast) 6.3 to 6.9 us per cell and iteration on round 8" to 6" adapters as
// wedges (15k to 20k cells), 4.4 to 4.8 us whole (143k to 248k cells): 5.5 taken. Linux ran the same 278k-cell case 0.7
// times as long (WSL on that machine): 4 taken; macOS not measured (taken as Linux). snappyHexMesh about 0.36 ms per cell
// it makes. A solve settles in 115 to 460 iterations (the pressure drop's probes, or the residuals): 220 taken.
void estimate_seconds(FlowPlan& plan) {
#if defined(_WIN32)
  const double per_cell_iteration = 5.5e-6;
#else
  const double per_cell_iteration = 4e-6;
#endif
  const double cells = double(plan.cells), iterations = std::min(220.0, double(plan.iterations > 0 ? plan.iterations : 220));
  const double mesh = plan.mesh == "3d" ? 5 + 0.36e-3 * cells : 1;
  plan.per_cell_iteration = per_cell_iteration;
  plan.solve_seconds = cells * iterations * per_cell_iteration;
  plan.seconds = mesh + plan.solve_seconds + 5 + 2e-5 * cells;
}

Geometry geometry(const Document& doc, const Scene& scene, const FlowSettings& s) {
  Geometry g;
  FlowPlan& plan = g.plan;
  const Preset q = preset(s.quality);
  auto add_wall = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (!n) throw Error("flow: no body " + id);
    for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id)) {
      const Node* m = scene.node(b);
      if (!m || m->body_missing || m->representation != "solid") throw Error("flow: \"" + (m ? m->name : b) + "\" is not a solid");
      if (std::find(plan.walls.begin(), plan.walls.end(), b) != plan.walls.end()) continue;
      plan.walls.push_back(b);
      g.walls.push_back(node_world_shape(doc, scene, b));
    }
  };
  if (s.domain == "around") {
    if (!s.bodies.empty())
      for (const auto& b : s.bodies) add_wall(b);
    else
      for (const auto& id : scene.all_bodies()) {
        const Node* n = scene.node(id);
        if (n && !n->body_missing && n->representation == "solid" && scene.effectively_visible(id)) add_wall(id);
      }
    if (plan.walls.empty()) throw Error("flow around: no solid to put in the flow (bodies)");
    plan.mesh = "3d";
    plan.axis = unit(s.direction);
    g.e1 = across(plan.axis);
    Bnd_Box box;
    for (const auto& w : g.walls) BRepBndLib::Add(w, box);
    double c[6];
    box.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
    const double size = std::max({c[3] - c[0], c[4] - c[1], c[5] - c[2]});
    plan.inlet.centre = plan.outlet.centre = {0.5 * (c[0] + c[3]), 0.5 * (c[1] + c[4]), 0.5 * (c[2] + c[5])};
    plan.inlet.diameter_mm = plan.outlet.diameter_mm = size;
    plan.length_mm = size;
    plan.cell_mm = size / (q.across * 0.75);
    plan.iterations = s.iterations > 0 ? s.iterations : q.iterations;
    plan.cells = size_t(6.5 * 4 * 4 * std::pow(size / plan.cell_mm, 3) * 1.6);
    estimate_seconds(plan);
    return g;
  }
  g.in = opening_of(doc, scene, s.inlet, "inlet", g);
  g.out = opening_of(doc, scene, s.outlet, "outlet", g);
  if (norm(sub(g.in.centre, g.out.centre)) < 1e-6 * (1 + g.in.rmax)) throw Error("flow: the inlet and the outlet are the same opening");
  orient(g.in, g.out, "inlet");
  orient(g.out, g.in, "outlet");
  if (g.in.hole != g.out.hole)
    throw Error("flow: pick both ends the same way: the rims of a duct's walls (faces with the opening as a hole) or the end faces of a body that is the fluid");
  plan.mode = g.in.hole ? "wall" : "volume";
  for (const auto& b : s.bodies) add_wall(b);
  for (auto [o, p] : {std::pair<const Opening*, FlowPlan::Opening*>{&g.in, &plan.inlet}, {&g.out, &plan.outlet}}) {
    p->centre = o->centre, p->normal = o->normal, p->area_mm2 = o->area;
    p->diameter_mm = 4 * o->area / o->perimeter;
    p->radius_mm = o->rmax;
    const size_t step = std::max<size_t>(1, o->rim.size() / 96);
    for (size_t i = 0; i < o->rim.size(); i += step) p->outline.push_back(o->rim[i]);
  }
  plan.axis = mul(g.in.normal, -1.0);
  g.e1 = across(plan.axis);
  plan.inlet_extension_mm = s.extend_inlet * plan.inlet.diameter_mm;
  plan.outlet_extension_mm = s.extend_outlet * plan.outlet.diameter_mm;
  plan.iterations = s.iterations > 0 ? s.iterations : 0;
  std::string why;
  if (s.mesh != "3d") why = round_profile(g, q.radial);
  if (s.mesh != "3d" && why.empty()) {
    double rmin = 1e300;
    for (const auto& p : plan.profile) rmin = std::min(rmin, p[1]);
    plan.radial_cells = q.radial;
    plan.cell_mm = rmin / q.radial;
    WedgeLayout layout;
    why = wedge_layout(plan, layout);
    plan.cells = layout.cells;  // the fitting at the cell size, the stubs growing to four times it away from it
  }
  if (s.mesh == "axisymmetric" && !why.empty()) throw Error("flow: mesh axisymmetric: " + why);
  if (s.mesh == "auto" && !why.empty()) plan.why_3d = why;
  if (s.mesh != "3d" && why.empty()) {
    plan.mesh = "axisymmetric";
    if (plan.iterations == 0) plan.iterations = q.wedge_iterations;
  } else {
    plan.steps.clear(), plan.step_deg = 0, plan.radial_cells = 0;
    plan.mesh = "3d";
    plan.profile.clear();
    plan.length_mm = norm(sub(g.out.centre, g.in.centre));
    plan.cell_mm = std::min(plan.inlet.diameter_mm, plan.outlet.diameter_mm) / q.across;
    if (plan.iterations == 0) plan.iterations = q.iterations;
    Bnd_Box box;
    for (const auto& w : g.walls) BRepBndLib::Add(w, box);
    for (const Opening* o : {&g.in, &g.out}) {
      const double ext = o == &g.in ? plan.inlet_extension_mm : plan.outlet_extension_mm;
      for (const V& p : o->rim) {
        const V q2 = add(p, mul(o->normal, ext));
        box.Add(gp_Pnt(q2[0], q2[1], q2[2]));
      }
    }
    // Cells (snappyHexMesh, write_3d_mesh): the stubs' volume at the background cells; the part's own walls at level 2 (a
    // quarter of them), about ten cells deep with their layers; three layers along the stubs. Measured on a round 8" to 6" adapter
    // (stubs 2 and 12 diameters): 143k cells, of which 42k at level 0, 82k at level 2, 57k in layers; this gives 143k.
    const double c = plan.cell_mm;
    double wall_area = 0;
    for (const auto& w : g.walls) {
      GProp_GProps p;
      BRepGProp::SurfaceProperties(w, p);
      wall_area += p.Mass();
    }
    if (plan.mode == "wall") wall_area *= 0.5;  // a wall's inside, not its outside
    else wall_area -= plan.inlet.area_mm2 + plan.outlet.area_mm2;
    const double stub_volume = plan.inlet.area_mm2 * plan.inlet_extension_mm + plan.outlet.area_mm2 * plan.outlet_extension_mm;
    const double stub_area = 4 * plan.inlet.area_mm2 / plan.inlet.diameter_mm * plan.inlet_extension_mm + 4 * plan.outlet.area_mm2 / plan.outlet.diameter_mm * plan.outlet_extension_mm;
    plan.cells = size_t(stub_volume / (c * c * c) + 130 * std::max(0.0, wall_area) / (c * c) + 3 * stub_area / (c * c));
  }
  estimate_seconds(plan);
  return g;
}

// ---------------------------------------------------------------- the case
void stl_faces(std::ostream& out, const TopoDS_Shape& shape, const std::function<bool(const TopoDS_Face&)>& keep = {}) {
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face face = TopoDS::Face(e.Current());
    if (keep && !keep(face)) continue;
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    const bool rev = face.Orientation() == TopAbs_REVERSED;
    for (int t = 1; t <= tri->NbTriangles(); ++t) {
      int n[3];
      tri->Triangle(t).Get(n[0], n[1], n[2]);
      if (rev) std::swap(n[1], n[2]);
      V p[3];
      for (int k = 0; k < 3; ++k) {
        const gp_Pnt q = tri->Node(n[k]).Transformed(loc.Transformation());
        p[k] = {q.X(), q.Y(), q.Z()};
      }
      const V nn = unit(cross(sub(p[1], p[0]), sub(p[2], p[0])));
      out << " facet normal " << nn[0] << " " << nn[1] << " " << nn[2] << "\n  outer loop\n";
      for (const auto& q : p) out << "   vertex " << q[0] * 1e-3 << " " << q[1] * 1e-3 << " " << q[2] * 1e-3 << "\n";
      out << "  endloop\n endfacet\n";
    }
  }
}

struct Turbulence {
  bool laminar = false;
  double Re = 0, V = 0, D = 0;  // the reference: the faster end (inside) or the parts' size (around)
};
Turbulence turbulence_of(const FlowSettings& s, const FlowPlan& plan) {
  Turbulence t;
  if (s.domain == "around") {
    t.V = s.speed_ms, t.D = plan.inlet.diameter_mm;
  } else {
    const FlowPlan::Opening& small = plan.inlet.area_mm2 <= plan.outlet.area_mm2 ? plan.inlet : plan.outlet;
    const double Q = s.drive == FlowSettings::Drive::Flow ? s.flow_m3s : 0.5 * s.fan.Qmax;  // a fan: about where it runs
    t.V = Q / (small.area_mm2 * 1e-6), t.D = small.diameter_mm;
  }
  t.Re = t.V * t.D * 1e-3 / s.nu;
  t.laminar = s.turbulence == "laminar" || (s.turbulence == "auto" && s.domain == "inside" && t.Re < 2000);
  return t;
}

// The fitting's wedge (wedge_layout): x along the axis from the inlet face (mm), each band boundary a pair of vertices on
// either side of the wedge, the axis one vertex (its blocks collapse there).
void write_wedge_mesh(const FlowPlan& plan, const fs::path& dir) {
  WedgeLayout w;
  if (const std::string why = wedge_layout(plan, w); !why.empty()) throw Error("flow: the wedge's mesh: " + why);
  const double h = 0.5 * kWedge, c = std::cos(h), s = std::sin(h);
  struct Point {
    double x, r;
    int minus, plus;
  };
  std::vector<Point> pts;
  std::ostringstream vs;
  int nv = 0;
  auto vertex = [&](const std::array<double, 2>& q) -> Point {
    const double tol = 1e-7 * (1 + std::fabs(q[0]) + q[1]);
    for (const auto& p : pts)
      if (std::fabs(p.x - q[0]) < tol && std::fabs(p.r - q[1]) < tol) return p;
    Point p{q[0], q[1], nv, nv};
    if (q[1] < 1e-9) {
      vs << "  (" << num(q[0]) << " 0 0)\n";
      ++nv;
    } else {
      vs << "  (" << num(q[0]) << " " << num(q[1] * c) << " " << num(-q[1] * s) << ")\n  (" << num(q[0]) << " " << num(q[1] * c) << " " << num(q[1] * s) << ")\n";
      p.plus = nv + 1;
      nv += 2;
    }
    pts.push_back(p);
    return p;
  };
  std::ostringstream blocks, wall, front, back, inlet, outlet;
  auto quad = [](std::ostringstream& o, int a, int b, int cc, int d) { o << "      (" << a << " " << b << " " << cc << " " << d << ")\n"; };
  // A station's face of band j (between boundaries j and j + 1): a triangle at the axis.
  auto across = [&](std::ostringstream& o, const std::vector<std::array<double, 2>>& st, size_t j) {
    const Point L = vertex(st[j]), H = vertex(st[j + 1]);
    quad(o, L.minus, L.plus, H.plus, H.minus);
  };
  for (const auto& p : w.pieces) {
    const size_t bands = p.lam.size();
    for (size_t i = 0; i + 1 < p.st.size(); ++i)
      for (size_t j = 0; j < bands; ++j) {
        const Point Li = vertex(p.st[i][j]), Lj = vertex(p.st[i + 1][j]), Hi = vertex(p.st[i][j + 1]), Hj = vertex(p.st[i + 1][j + 1]);
        const int g = p.group[j];
        blocks << "  hex (" << Li.minus << " " << Lj.minus << " " << Hj.minus << " " << Hi.minus << " " << Li.plus << " " << Lj.plus << " " << Hj.plus << " "
               << Hi.plus << ") (" << p.nx[i] << " " << w.cells_across[size_t(g)] << " 1) simpleGrading (" << num(p.gx[i]) << " " << w.grading[size_t(g)] << " 1)\n";
        quad(front, Li.plus, Lj.plus, Hj.plus, Hi.plus);
        quad(back, Li.minus, Hi.minus, Hj.minus, Lj.minus);
        if (j + 1 == bands) quad(wall, Hi.minus, Hi.plus, Hj.plus, Hj.minus);
      }
    // The first and last stations: an opening, or a step (its face above the step's lower radius is wall; below it the cells
    // go on into the next piece).
    for (const bool first : {true, false}) {
      const auto& st = first ? p.st.front() : p.st.back();
      const double lo = first ? p.lo_start : p.lo_end;
      for (size_t j = 0; j < bands; ++j) {
        if (lo < 0) across(first ? inlet : outlet, st, j);
        else if (st[j + 1][1] > lo * (1 + 1e-9)) across(wall, st, j);
      }
    }
  }
  std::ostringstream bm;
  bm << "convertToMeters 0.001;\nvertices\n(\n" << vs.str() << ");\nblocks\n(\n" << blocks.str() << ");\nedges\n(\n);\nboundary\n(\n"
     << "  inlet { type patch; faces (\n" << inlet.str() << "    ); }\n"
     << "  outlet { type patch; faces (\n" << outlet.str() << "    ); }\n"
     << "  wall { type wall; faces (\n" << wall.str() << "    ); }\n"
     << "  front { type wedge; faces (\n" << front.str() << "    ); }\n"
     << "  back { type wedge; faces (\n" << back.str() << "    ); }\n);\nmergePatchPairs\n(\n);\n";
  foam::put(dir / "system" / "blockMeshDict", "dictionary", "blockMeshDict", bm.str());
}

// The full part: a box of cells around the fluid, snappyHexMesh keeping the cells on the fluid's side of the walls and the
// openings (inside), or outside the parts (around), with layers along the walls.
void write_3d_mesh(const Geometry& g, const FlowSettings& s, const fs::path& dir) {
  const FlowPlan& plan = g.plan;
  const double cell = plan.cell_mm;
  fs::create_directories(dir / "constant" / "triSurface");
  const double defl = std::max(0.02, cell / 10);
  for (const auto& w : g.walls) BRepMesh_IncrementalMesh(w, defl, false, 0.3, true);
  V lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  auto grow = [&](const TopoDS_Shape& sh) {
    Bnd_Box b;
    BRepBndLib::Add(sh, b);
    double c[6];
    b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
    for (int k = 0; k < 3; ++k) lo[size_t(k)] = std::min(lo[size_t(k)], c[k]), hi[size_t(k)] = std::max(hi[size_t(k)], c[k + 3]);
  };
  V seed;
  std::ostringstream geo, refine, layers, regions;  // regions: snappy's refinementRegions
  const bool around = s.domain == "around";
  if (around) {
    std::ostringstream stl;
    stl.precision(10);
    stl << "solid part\n";
    for (const auto& w : g.walls) stl_faces(stl, w);
    stl << "endsolid part\n";
    write_text_file(dir / "constant" / "triSurface" / "part.stl", stl.str());
    geo << "  part { type triSurfaceMesh; file \"part.stl\"; }\n";
    refine << "    part { level (2 3); patchInfo { type wall; } }\n";
    layers << " \"part.*\" { nSurfaceLayers 3; }";
  } else {
    // The walls: every face of the wall bodies (a body that is the fluid: all but its two ends); the stubs' sides (straight
    // pipe: refined less than the part); the ends.
    std::ostringstream wall, stubs, ends[2];
    wall.precision(10), stubs.precision(10);
    wall << "solid wall\n";
    stubs << "solid stub\n";
    for (size_t i = 0; i < g.walls.size(); ++i) {
      grow(g.walls[i]);
      stl_faces(wall, g.walls[i], [&](const TopoDS_Face& f) { return g.plan.mode == "wall" || !(f.IsSame(g.in.picked) || f.IsSame(g.out.picked)); });
    }
    int k = 0;
    for (const Opening* o : {&g.in, &g.out}) {
      const double ext = o == &g.in ? plan.inlet_extension_mm : plan.outlet_extension_mm;
      std::ostringstream& end = ends[k++];
      end.precision(10);
      const std::string name = o == &g.in ? "inlet" : "outlet";
      end << "solid " << name << "\n";
      if (ext > 0) {
        BRepPrimAPI_MakePrism prism(o->face, gp_Vec(o->normal[0], o->normal[1], o->normal[2]) * ext);
        const TopoDS_Shape stub = prism.Shape();
        BRepMesh_IncrementalMesh(stub, defl, false, 0.3, true);
        grow(stub);
        const V end_centre = add(o->centre, mul(o->normal, ext));
        auto plane_at = [&](const TopoDS_Face& f, const V& p) {
          GProp_GProps gp;
          BRepGProp::SurfaceProperties(f, gp);
          const gp_Pnt c = gp.CentreOfMass();
          return std::fabs(dot(sub(V{c.X(), c.Y(), c.Z()}, p), o->normal)) < 1e-6 * (1 + ext);
        };
        stl_faces(stubs, stub, [&](const TopoDS_Face& f) { return !plane_at(f, end_centre) && !plane_at(f, o->centre); });
        stl_faces(end, stub, [&](const TopoDS_Face& f) { return plane_at(f, end_centre); });
      } else {
        BRepMesh_IncrementalMesh(o->face, defl, false, 0.3, true);
        stl_faces(end, o->face);
      }
      end << "endsolid " << name << "\n";
      write_text_file(dir / "constant" / "triSurface" / (name + ".stl"), end.str());
      geo << "  " << name << " { type triSurfaceMesh; file \"" << name << ".stl\"; }\n";
      refine << "    " << name << " { level (1 1); patchInfo { type patch; } }\n";
    }
    wall << "endsolid wall\n";
    stubs << "endsolid stub\n";
    write_text_file(dir / "constant" / "triSurface" / "wall.stl", wall.str());
    geo << "  wall { type triSurfaceMesh; file \"wall.stl\"; }\n";
    refine << "    wall { level (1 2); patchInfo { type wall; } }\n";
    layers << " \"wall.*\" { nSurfaceLayers 3; }";
    if (plan.inlet_extension_mm > 0 || plan.outlet_extension_mm > 0) {
      write_text_file(dir / "constant" / "triSurface" / "stub.stl", stubs.str());
      geo << "  stub { type triSurfaceMesh; file \"stub.stl\"; }\n";
      // Level 0 to 1 (the part's walls 1 to 2): at level 1 throughout, the stubs' volume was 180k of a round 8" to 6" adapter's 278k cells.
      refine << "    stub { level (0 1); patchInfo { type wall; } }\n";
      layers << " \"stub.*\" { nSurfaceLayers 3; }";
    }
    // A point in the fluid: the middle of the inlet's stub, else a little inside the inlet; a little off its centre line.
    const double inward = plan.inlet_extension_mm > 0 ? 0.5 * plan.inlet_extension_mm : -1.13 * cell;
    seed = add(add(g.in.centre, mul(g.in.normal, inward)), mul(g.e1, 0.137 * std::min(cell, g.in.rmin)));
  }
  // The box: around the fluid by two cells (inside), or 1.5 sizes ahead and across, 4 behind (around).
  const V a = plan.axis, e1 = g.e1, e2 = cross(a, e1);
  double d0, d1, a0, a1, b0, b1;
  if (around) {
    for (const auto& w : g.walls) grow(w);
    double plo[3] = {1e300, 1e300, 1e300}, phi[3] = {-1e300, -1e300, -1e300};
    for (double x : {lo[0], hi[0]})
      for (double y : {lo[1], hi[1]})
        for (double z : {lo[2], hi[2]}) {
          const V p{x, y, z};
          const double c[3] = {dot(p, a), dot(p, e1), dot(p, e2)};
          for (int k = 0; k < 3; ++k) plo[k] = std::min(plo[k], c[k]), phi[k] = std::max(phi[k], c[k]);
        }
    const double size = plan.length_mm;
    d0 = plo[0] - 1.5 * size, d1 = phi[0] + 4 * size, a0 = plo[1] - 1.5 * size, a1 = phi[1] + 1.5 * size, b0 = plo[2] - 1.5 * size, b1 = phi[2] + 1.5 * size;
    seed = add(add(mul(a, d0 + 0.37 * size), mul(e1, a0 + 0.41 * size)), mul(e2, b0 + 0.43 * size));
  } else {
    // World axes for the inside (the fluid's own way may be anything).
    const double pad = 2 * cell;
    d0 = lo[0] - pad, d1 = hi[0] + pad, a0 = lo[1] - pad, a1 = hi[1] + pad, b0 = lo[2] - pad, b1 = hi[2] + pad;
  }
  const V X = around ? a : V{1, 0, 0}, Y = around ? e1 : V{0, 1, 0}, Z = around ? e2 : V{0, 0, 1};
  const double off = 0.37 * cell;  // no face of the parts on a grid plane
  d0 -= off, a0 -= off, b0 -= off;
  auto corner = [&](double d, double aa, double bb) { return add(add(mul(X, d), mul(Y, aa)), mul(Z, bb)); };
  const int nx = std::max(4, int(std::ceil((d1 - d0) / cell))), ny = std::max(4, int(std::ceil((a1 - a0) / cell))), nz = std::max(4, int(std::ceil((b1 - b0) / cell)));
  if (double(nx) * ny * nz > 4e6) throw Error("flow: the mesh's box would take too many cells (" + std::to_string(nx) + " x " + std::to_string(ny) + " x " + std::to_string(nz) + "): quality quick, or shorter stubs (extend)");
  std::ostringstream bm;
  bm << "convertToMeters 0.001;\nvertices (\n";
  for (const V& v : {corner(d0, a0, b0), corner(d1, a0, b0), corner(d1, a1, b0), corner(d0, a1, b0), corner(d0, a0, b1), corner(d1, a0, b1), corner(d1, a1, b1),
                     corner(d0, a1, b1)})
    bm << "  " << vec(v) << "\n";
  bm << ");\nblocks ( hex (0 1 2 3 4 5 6 7) (" << nx << " " << ny << " " << nz << ") simpleGrading (1 1 1) );\nboundary (\n"
     << (around ? "  inlet { type patch; faces ((0 4 7 3)); }\n  outlet { type patch; faces ((1 2 6 5)); }\n"
                  "  sides { type patch; faces ((0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7)); }\n);\n"
                : "  box { type wall; faces ((0 4 7 3) (1 2 6 5) (0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7)); }\n);\n");
  foam::put(dir / "system" / "blockMeshDict", "dictionary", "blockMeshDict", bm.str());
  std::ostringstream sh;
  sh << "castellatedMesh true; snap true; addLayers true;\ngeometry {\n" << geo.str() << "}\n"
     << "castellatedMeshControls {\n  maxLocalCells 4000000; maxGlobalCells 8000000; minRefinementCells 0; maxLoadUnbalance 0.1; nCellsBetweenLevels 3;\n"
     << "  features ();\n  refinementSurfaces {\n" << refine.str() << "  }\n  resolveFeatureAngle 30;\n  refinementRegions {\n" << regions.str() << "  }\n"
     << "  locationInMesh " << vec(mul(seed, 1e-3)) << ";\n  allowFreeStandingZoneFaces false;\n}\n"
     << "snapControls { nSmoothPatch 3; tolerance 2.0; nSolveIter 50; nRelaxIter 5; nFeatureSnapIter 10; implicitFeatureSnap true; explicitFeatureSnap false; "
        "multiRegionFeatureSnap false; }\n"
     << "addLayersControls { relativeSizes true; layers {" << layers.str() << " } expansionRatio 1.2; finalLayerThickness 0.4; minThickness 0.05; nGrow 0; "
        "featureAngle 130; nRelaxIter 3; nSmoothSurfaceNormals 1; nSmoothNormals 3; nSmoothThickness 10; maxFaceThicknessRatio 0.5; "
        "maxThicknessToMedialRatio 0.3; minMedialAxisAngle 90; nBufferCellsNoExtrude 0; nLayerIter 50; }\n"
     << "meshQualityControls { maxNonOrtho 65; maxBoundarySkewness 20; maxInternalSkewness 4; maxConcave 80; minVol 1e-18; minTetQuality 1e-15; "
        "minArea -1; minTwist 0.02; minDeterminant 0.001; minFaceWeight 0.05; minVolRatio 0.01; minTriangleTwist -1; nSmoothScale 4; "
        "errorReduction 0.75; relaxed { maxNonOrtho 75; } }\nmergeTolerance 1e-6;\n";
  foam::put(dir / "system" / "snappyHexMeshDict", "dictionary", "snappyHexMeshDict", sh.str());
}

// Points the solver samples every few iterations (OpenFOAM's probes, libsampling): across the inlet just inside it, and
// across the outlet's stub at 0.6 and 0.9 of it (the stub's own friction, as the summary takes it out), in the mesh's frame
// (mm), each with its share of the section's area. Their pressure drop tells when the solve has settled.
struct Probes {
  std::vector<V> at;
  std::vector<double> weight;
  size_t n_in = 0, n_mid = 0, n_far = 0;  // how many of each, in that order
  double mid_mm = 0, far_mm = 0;    // along the outlet's stub (0: none)
};
Probes probes_of(const Geometry& g) {
  Probes pr;
  const FlowPlan& plan = g.plan;
  if (plan.mode.empty()) return pr;  // around: no openings
  const bool wedge = plan.mesh == "axisymmetric";
  const double cell = plan.cell_mm, Lout = plan.outlet_extension_mm;
  pr.mid_mm = Lout >= 4 * plan.outlet.diameter_mm ? 0.6 * Lout : 0, pr.far_mm = 0.9 * Lout;
  auto section = [&](const Opening& o, double along, size_t& count) {
    if (wedge) {
      const double x = &o == &g.in ? along : plan.length_mm + along, R = &o == &g.in ? plan.profile.front()[1] : plan.profile.back()[1];
      for (int k = 0; k < 6; ++k) pr.at.push_back({x, R * (k + 0.5) / 6, 0}), pr.weight.push_back((k + 0.5) / 6), ++count;
      return;
    }
    // Across the opening's face: a grid over its rim, the points inside it, moved along its normal.
    const V e1 = across(o.normal), e2 = cross(o.normal, e1);
    for (int i = 0; i < 6; ++i)
      for (int j = 0; j < 6; ++j) {
        const V q = add(o.centre, add(mul(e1, o.rmax * (-1 + 2 * (i + 0.5) / 6)), mul(e2, o.rmax * (-1 + 2 * (j + 0.5) / 6))));
        if (BRepClass_FaceClassifier(o.face, gp_Pnt(q[0], q[1], q[2]), 1e-6).State() != TopAbs_IN) continue;
        pr.at.push_back(add(q, mul(o.normal, &o == &g.in ? -along : along))), pr.weight.push_back(1), ++count;
      }
  };
  section(g.in, 0.5 * cell, pr.n_in);
  if (pr.mid_mm > 0) section(g.out, pr.mid_mm, pr.n_mid);
  section(g.out, pr.far_mm > 0 ? pr.far_mm : -0.5 * cell, pr.n_far);
  return pr;
}

// The pressure drop the probes give at each iteration they were written (kinematic, m2/s2), the stub's friction taken out as
// the summary does; empty while they have not written or a point was lost (outside the mesh).
std::vector<std::pair<double, double>> probe_drops(const fs::path& dir, const Probes& pr) {
  std::vector<std::pair<double, double>> out;
  auto rows = [&](const char* field) {
    std::map<double, std::vector<double>> r;
    std::string t;
    try {
      t = read_text_file(dir / "postProcessing" / "opadProbes" / "0" / field);
    } catch (...) {
      return r;
    }
    std::istringstream in(t);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      for (char& ch : line)
        if (ch == '(' || ch == ')') ch = ' ';
      std::istringstream ls(line);
      double time;
      if (!(ls >> time)) continue;
      std::vector<double> v;
      for (double x; ls >> x;) v.push_back(x);
      r[time] = v;
    }
    return r;
  };
  const auto p = rows("p"), U = rows("U");
  const size_t n = pr.at.size();
  for (const auto& [time, pv] : p) {
    const auto u = U.find(time);
    if (u == U.end() || pv.size() != n || u->second.size() != 3 * n) continue;
    double tot[3] = {0, 0, 0}, w[3] = {0, 0, 0};
    for (size_t i = 0; i < n; ++i) {
      const int set = i < pr.n_in ? 0 : i < pr.n_in + pr.n_mid ? 1 : 2;
      const double* uu = &u->second[3 * i];
      tot[set] += pr.weight[i] * (pv[i] + 0.5 * (uu[0] * uu[0] + uu[1] * uu[1] + uu[2] * uu[2])), w[set] += pr.weight[i];
    }
    for (int k = 0; k < 3; ++k) tot[k] = w[k] > 0 ? tot[k] / w[k] : 0;
    double drop = tot[0] - tot[2];
    if (pr.n_mid && pr.far_mm > pr.mid_mm) drop -= (tot[1] - tot[2]) / (pr.far_mm - pr.mid_mm) * pr.far_mm;
    out.push_back({time, drop});
  }
  return out;
}

// Settled: over the last `window` iterations the probes' pressure drop has stayed within `tol` (kinematic) of its last value,
// after `least` iterations.
bool drop_settled(const std::vector<std::pair<double, double>>& d, double tol, double window = 100, double least = 120) {
  if (d.size() < 8 || d.back().first < least) return false;
  const double last = d.back().second;
  size_t seen = 0;
  for (auto it = d.rbegin(); it != d.rend() && it->first >= d.back().first - window; ++it, ++seen)
    if (std::fabs(it->second - last) > tol) return false;
  return seen >= 8;
}

void write_case(const Geometry& g, const FlowSettings& s, const fs::path& dir) {
  const FlowPlan& plan = g.plan;
  const bool wedge = plan.mesh == "axisymmetric", around = s.domain == "around";
  const double share = wedge ? kWedge / (2 * kPi) : 1.0;  // the wedge carries this share of the flow
  if (wedge) write_wedge_mesh(plan, dir);
  else write_3d_mesh(g, s, dir);
  const Turbulence tb = turbulence_of(s, plan);
  // A write every 20 iterations, the last five kept (a flow that never settles to one state is taken as their average), the
  // end rounded up to a multiple of it (OpenFOAM writes only on the interval). The probes every 10 iterations; the
  // controlDict read again while it runs (run_flow says stopAt writeNow once settled).
  const int every = 20, iters = (plan.iterations + every - 1) / every * every;
  std::string functions;
  if (const Probes pr = probes_of(g); !pr.at.empty()) {
    std::ostringstream f;
    f << "functions\n{\n  opadProbes { type probes; libs (sampling); writeControl timeStep; writeInterval 10; fields (p U); probeLocations (";
    for (const V& q : pr.at) f << " " << vec(mul(q, 1e-3));
    f << " ); }\n}\n";
    functions = f.str();
  }
  foam::put(dir / "system" / "controlDict", "dictionary", "controlDict",
            "application simpleFoam; startFrom startTime; startTime 0; stopAt endTime; endTime " + std::to_string(iters) +
                "; deltaT 1; writeControl timeStep; writeInterval " + std::to_string(every) +
                "; purgeWrite 5; writeFormat ascii; writePrecision 10; writeCompression off; timeFormat general; timePrecision 8; runTimeModifiable true;\n" +
                functions);
  // The wedge's faces across the axis meet a sloping wall at its angle: above 30 degrees its cells are skewed enough to
  // need the correction the full part's mesh has.
  const bool skewed = !wedge || plan.steepest_deg > 30;
  const std::string corrected = skewed ? "limited corrected 0.33" : "corrected";
  foam::put(dir / "system" / "fvSchemes", "dictionary", "fvSchemes",
            "ddtSchemes { default steadyState; }\ngradSchemes { default Gauss linear; grad(U) cellLimited Gauss linear 1; }\n"
            "divSchemes { default none; div(phi,U) bounded Gauss linearUpwind grad(U); div(phi,k) bounded Gauss upwind; div(phi,omega) bounded Gauss upwind; "
            "div((nuEff*dev2(T(grad(U))))) Gauss linear; }\nlaplacianSchemes { default Gauss linear " + corrected + "; }\ninterpolationSchemes { default linear; }\n"
            "snGradSchemes { default " + corrected + "; }\nwallDist { method meshWave; }\n");
  foam::put(dir / "system" / "fvSolution", "dictionary", "fvSolution",
            "solvers {\n  p { solver GAMG; smoother GaussSeidel; tolerance 1e-7; relTol 0.05; }\n"
            "  \"(U|k|omega)\" { solver smoothSolver; smoother symGaussSeidel; tolerance 1e-8; relTol 0.1; }\n}\n"
            "SIMPLE { nNonOrthogonalCorrectors " + std::string(skewed ? "1" : "0") + "; consistent yes; residualControl { " +
                // With the probes the pressure drop's settling ends the solve (run_flow); the residuals only stop one that has
                // fully converged. These residuals alone stopped an adapter's wedge with its drop still moving 2 %.
                std::string(probes_of(g).at.empty() ? "p 1e-4; " + std::string(wedge ? "" : "U 1e-5; ") + "\"(k|omega)\" 1e-5;"
                                                    : "p 1e-5; " + std::string(wedge ? "" : "U 1e-6; ") + "\"(k|omega)\" 1e-6;") +
                " } }\n"
            // The wedge at SIMPLEC's own relaxation: an adapter's settled in 115 iterations, not 334 at U 0.8 and 0.7. The
            // whole part at 0.7: an expansion's jet, solved whole, swung by 2 to 3 Pa for 600 iterations at 0.8 or 0.9 and
            // settled within 1.5 % at 0.7.
            "relaxationFactors { equations { U " + std::string(wedge ? "0.9" : "0.7") + "; \".*\" " + (wedge ? "0.9" : "0.7") + "; } }\n");
  // A wedge's velocity across it (Uz) is zero to round-off, so its normalised residual is noise that never falls below any
  // tolerance: U is left out of residualControl there (run_flow checks Ux and Uy itself), else every wedge ran to the end.
  // Both spellings: openfoam.com (transportProperties, turbulenceProperties) and openfoam.org (physicalProperties, momentumTransport).
  const std::string transport = "transportModel Newtonian;\nviscosityModel constant;\nnu " + num(s.nu) + ";\n";
  foam::put(dir / "constant" / "transportProperties", "dictionary", "transportProperties", transport);
  foam::put(dir / "constant" / "physicalProperties", "dictionary", "physicalProperties", transport);
  const std::string turbulence = tb.laminar ? "simulationType laminar;\n" : "simulationType RAS;\nRAS { model kOmegaSST; RASModel kOmegaSST; turbulence on; printCoeffs off; }\n";
  foam::put(dir / "constant" / "turbulenceProperties", "dictionary", "turbulenceProperties", turbulence);
  foam::put(dir / "constant" / "momentumTransport", "dictionary", "momentumTransport", turbulence);

  // ---- the fields
  // Inlet turbulence: 5 % intensity, a mixing length of 7 % of the opening's diameter (developed pipe flow); around: 1 %.
  const double I = around ? 0.01 : 0.05;
  const double Dref = plan.inlet.diameter_mm * 1e-3;
  const double Vin = around ? s.speed_ms : s.drive == FlowSettings::Drive::Flow ? s.flow_m3s / (plan.inlet.area_mm2 * 1e-6) : 0.5 * s.fan.Qmax / (plan.inlet.area_mm2 * 1e-6);
  const double k0 = std::max(1e-8, 1.5 * std::pow(I * Vin, 2)), mix = 0.07 * Dref, w0 = std::sqrt(k0) / (std::pow(0.09, 0.25) * mix);
  const std::string wedges = wedge ? "\n  \"(front|back)\" { type wedge; }" : "";
  const V init = wedge ? V{Vin, 0, 0} : around ? mul(plan.axis, s.speed_ms) : mul(plan.axis, Vin);  // the flow under way: fewer iterations
  std::string u_in, p_in;
  if (around) {
    u_in = "inlet { type fixedValue; value uniform " + vec(mul(plan.axis, s.speed_ms)) + "; }";
    p_in = "inlet { type zeroGradient; }";
  } else if (s.drive == FlowSettings::Drive::Flow) {
    u_in = "\"inlet.*\" { type flowRateInletVelocity; volumetricFlowRate constant " + num(s.flow_m3s * share) + "; extrapolateProfile false; value uniform (0 0 0); }";
    p_in = "\"inlet.*\" { type zeroGradient; }";
  } else {
    // The fan's curve (static pressure against flow) as the inlet's total pressure: it finds its own operating point (a fan's
    // static curve taken as total pressure: the usual approximation, a little on the safe side). Inline as a Function1 table:
    // openfoam.org reads no curve file; past its ends the table clamps.
    std::ostringstream curve;
    curve << "(";
    const int n = s.fan.curve.size() >= 2 ? int(s.fan.curve.size()) : 2;
    for (int i = 0; i < n; ++i) {
      const double Q = s.fan.curve.size() >= 2 ? s.fan.curve[size_t(i)][0] : s.fan.Qmax * i / (n - 1);
      curve << " (" << num(Q * share) << " " << num(s.fan.pressure(Q) / s.rho) << ")";
    }
    curve << " )";
    u_in = "\"inlet.*\" { type pressureInletOutletVelocity; value uniform (0 0 0); }";
    p_in = "\"inlet.*\" { type fanPressure; fanCurve table " + curve.str() + "; direction in; p0 uniform 0; value uniform 0; }";
  }
  const std::string sides_u = around ? "\n  sides { type slip; }" : "", sides_s = around ? "\n  sides { type slip; }" : "";
  const std::string out_name = around ? "outlet" : "\"outlet.*\"", in_name = around ? "inlet" : "\"inlet.*\"";
  foam::put(dir / "0" / "U", "volVectorField", "U",
            "dimensions [0 1 -1 0 0 0 0];\ninternalField uniform " + vec(init) + ";\nboundaryField\n{\n  \".*\" { type noSlip; }\n  " + u_in + "\n  " + out_name +
                " { type inletOutlet; inletValue uniform (0 0 0); value uniform (0 0 0); }" + wedges + sides_u + "\n}\n");
  foam::put(dir / "0" / "p", "volScalarField", "p",
            "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type zeroGradient; }\n  " + p_in + "\n  " + out_name +
                " { type fixedValue; value uniform 0; }" + wedges + sides_s + "\n}\n");
  foam::put(dir / "0" / "k", "volScalarField", "k",
            "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform " + num(k0) + ";\nboundaryField\n{\n  \".*\" { type kqRWallFunction; value uniform " + num(k0) + "; }\n  " +
                in_name + " { type turbulentIntensityKineticEnergyInlet; intensity " + num(I) + "; value uniform " + num(k0) + "; }\n  " + out_name +
                " { type inletOutlet; inletValue uniform " + num(k0) + "; value uniform " + num(k0) + "; }" + wedges + sides_s + "\n}\n");
  foam::put(dir / "0" / "omega", "volScalarField", "omega",
            "dimensions [0 0 -1 0 0 0 0];\ninternalField uniform " + num(w0) + ";\nboundaryField\n{\n  \".*\" { type omegaWallFunction; value uniform " + num(w0) + "; }\n  " +
                in_name + " { type turbulentMixingLengthFrequencyInlet; mixingLength " + num(mix) + "; value uniform " + num(w0) + "; }\n  " + out_name +
                " { type inletOutlet; inletValue uniform " + num(w0) + "; value uniform " + num(w0) + "; }" + wedges + sides_s + "\n}\n");
  foam::put(dir / "0" / "nut", "volScalarField", "nut",
            "dimensions [0 2 -1 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type nutUSpaldingWallFunction; value uniform 0; }\n  " + in_name +
                " { type calculated; value uniform 0; }\n  " + out_name + " { type calculated; value uniform 0; }" + wedges +
                (around ? "\n  sides { type calculated; value uniform 0; }" : "") + "\n}\n");
}

// ---------------------------------------------------------------- the results
// The solved fluid: its cells in the mesh's frame (mm; the wedge's: x along the axis from the inlet face, y out from it),
// their velocity (m/s, the same frame) and pressure (Pa), and the way from the world to that frame.
class FlowMesh {
 public:
  bool wedge = false;
  V origin{0, 0, 0}, axis{1, 0, 0}, e1{0, 1, 0};
  double rho = 1.2;
  std::vector<V> c;                   // centres (wedge: (x, r, 0))
  std::vector<double> vol;            // mm3 (the wedge: its sector's)
  std::vector<std::array<double, 6>> box;  // each cell's extent (mm, mesh frame)
  std::vector<V> U;
  std::vector<double> p;
  std::vector<int> own, nei;
  std::vector<foam::Patch> patches;
  std::vector<V> points;  // mm, mesh frame
  std::vector<std::vector<int>> faces;

  void index() {
    double mean = 0;
    for (const auto& b : box) mean += std::max({b[3] - b[0], b[4] - b[1], b[5] - b[2]});
    mean /= std::max<size_t>(1, box.size());
    m_near = std::make_unique<foam::Nearest>(c, mean);
  }
  V to_mesh(const V& w) const {
    if (!wedge) return w;
    const V d = sub(w, origin);
    const double x = dot(d, axis);
    return {x, norm(sub(d, mul(axis, x))), 0};
  }
  V to_world_point(const V& m) const { return wedge ? add(add(origin, mul(axis, m[0])), mul(e1, m[1])) : m; }
  // A velocity of the mesh's frame at a world point, in the world.
  V to_world(const V& u, const V& at) const {
    if (!wedge) return u;
    const V d = sub(at, origin);
    const V radial = sub(d, mul(axis, dot(d, axis)));
    const V r = norm(radial) > 1e-9 ? unit(radial) : e1;
    return add(add(mul(axis, u[0]), mul(r, u[1])), mul(cross(axis, r), u[2]));
  }
  size_t cell_at(const V& world, bool inside_only) const {
    const V m = to_mesh(world);
    const size_t i = (*m_near)(m);
    if (!inside_only) return i;
    const auto& b = box[i];
    const double pad = 0.6 * std::max({b[3] - b[0], b[4] - b[1], wedge ? 0.0 : b[5] - b[2]});
    if (m[0] < b[0] - pad || m[0] > b[3] + pad || m[1] < b[1] - pad || m[1] > b[4] + pad || (!wedge && (m[2] < b[2] - pad || m[2] > b[5] + pad))) return foam::Nearest::npos;
    return i;
  }
  bool at(const V& world, V& u, double& pres) const {
    const size_t i = cell_at(world, true);
    if (i == foam::Nearest::npos) return false;
    u = to_world(U[i], world), pres = p[i];
    return true;
  }
  double share() const { return wedge ? kWedge / (2 * kPi) : 1.0; }
  // The cells a plane passes through (mesh frame, within `reach` of `s`): flow (m3/s through the plane along n, the whole
  // part's), mass-weighted total and area-weighted static pressure (Pa), mean and largest speed, area (mm2).
  struct Section {
    double Q = 0, total = 0, stat = 0, mean = 0, vmax = 0, area = 0;
    size_t cells = 0;
  };
  Section section(const V& s, const V& n, double reach) const {
    Section out;
    double wt = 0, wq = 0, aw = 0;
    for (size_t i = 0; i < c.size(); ++i) {
      const auto& b = box[i];
      const double width = std::fabs(n[0]) * (b[3] - b[0]) + std::fabs(n[1]) * (b[4] - b[1]) + (wedge ? 0.0 : std::fabs(n[2]) * (b[5] - b[2]));
      const V d = sub(c[i], s);
      const double along = dot(d, n);
      if (std::fabs(along) > 0.5 * width || width <= 0) continue;
      if (norm(sub(d, mul(n, along))) > reach) continue;
      const double area = vol[i] / width;  // mm2
      const double un = dot(U[i], n), sp = norm(U[i]);
      out.Q += un * area * 1e-6;
      wt += un * area * (p[i] + 0.5 * rho * sp * sp), wq += un * area;
      out.stat += p[i] * area, aw += area;
      out.vmax = std::max(out.vmax, sp);
      ++out.cells;
    }
    if (!out.cells) return out;
    out.Q /= share();
    out.area = aw / share();
    out.total = std::fabs(wq) > 0 ? wt / wq : 0;
    out.stat = aw > 0 ? out.stat / aw : 0;
    out.mean = out.area > 0 ? out.Q / (out.area * 1e-6) : 0;
    return out;
  }

 private:
  std::unique_ptr<foam::Nearest> m_near;
};

// The fluid at one written time, or the average of several (their velocity and pressure).
std::shared_ptr<FlowMesh> read_flow(const fs::path& dir, const std::vector<std::string>& times, const Geometry& g, double rho) {
  auto m = std::make_shared<FlowMesh>();
  m->wedge = g.plan.mesh == "axisymmetric";
  m->origin = g.in.centre, m->axis = g.plan.axis, m->e1 = g.e1, m->rho = rho;
  const fs::path poly = dir / "constant" / "polyMesh";
  for (V& q : foam::read_points(read_text_file(poly / "points"))) m->points.push_back(mul(q, 1e3));
  m->faces = foam::read_faces(read_text_file(poly / "faces"));
  m->own = foam::read_labels(read_text_file(poly / "owner"));
  m->nei = foam::read_labels(read_text_file(poly / "neighbour"));
  m->patches = foam::read_boundary(read_text_file(poly / "boundary"));
  const foam::Cells cells = foam::read_cells(poly);
  const size_t n = cells.centre.size();
  for (size_t i = 0; i < n; ++i) {
    V q = mul(cells.centre[i], 1e3);
    if (m->wedge) q = {q[0], std::hypot(q[1], q[2]), 0};
    m->c.push_back(q);
    m->vol.push_back(cells.volume[i] * 1e9);
  }
  m->box.assign(n, {1e300, 1e300, 1e300, -1e300, -1e300, -1e300});
  auto extend = [&](size_t cell, const V& q0) {
    const V q = m->wedge ? V{q0[0], std::hypot(q0[1], q0[2]), 0} : q0;
    auto& b = m->box[cell];
    for (int k = 0; k < 3; ++k) b[size_t(k)] = std::min(b[size_t(k)], q[size_t(k)]), b[size_t(k) + 3] = std::max(b[size_t(k) + 3], q[size_t(k)]);
  };
  for (size_t f = 0; f < m->faces.size(); ++f)
    for (int q : m->faces[f]) {
      extend(size_t(m->own[f]), m->points[size_t(q)]);
      if (f < m->nei.size()) extend(size_t(m->nei[f]), m->points[size_t(q)]);
    }
  std::vector<double> U(3 * n, 0.0), p(n, 0.0);
  for (const auto& time : times) {
    const auto u = foam::read_internal(read_text_file(dir / time / "U"), n, 3);
    const auto q = foam::read_internal(read_text_file(dir / time / "p"), n, 1);
    for (size_t i = 0; i < 3 * n; ++i) U[i] += u[i] / double(times.size());
    for (size_t i = 0; i < n; ++i) p[i] += q[i] / double(times.size());
  }
  for (size_t i = 0; i < n; ++i) m->U.push_back({U[3 * i], U[3 * i + 1], U[3 * i + 2]}), m->p.push_back(p[i] * rho);
  m->index();
  return m;
}
std::shared_ptr<FlowMesh> read_flow(const fs::path& dir, const std::string& time, const Geometry& g, double rho) {
  return read_flow(dir, std::vector<std::string>{time}, g, rho);
}

// The times a case wrote (not 0), oldest first.
std::vector<std::string> written_times(const fs::path& dir) {
  std::vector<std::pair<double, std::string>> t;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_directory()) continue;
    const std::string s = e.path().filename().string();
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end && *end == 0 && !s.empty() && s != "0" && v > 0 && fs::exists(e.path() / "U") && fs::exists(e.path() / "p")) t.push_back({v, s});
  }
  std::sort(t.begin(), t.end());
  std::vector<std::string> out;
  for (auto& x : t) out.push_back(x.second);
  return out;
}

// The second newest time a case wrote ("" when one or none).
std::string previous_time(const fs::path& dir, const std::string& last) {
  double best = -1;
  std::string name;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_directory()) continue;
    const std::string s = e.path().filename().string();
    char* end = nullptr;
    const double t = std::strtod(s.c_str(), &end);
    if (end && *end == 0 && !s.empty() && s != last && s != "0" && t > best) best = t, name = s;
  }
  return name;
}

}  // namespace

// ---------------------------------------------------------------- openings picked as loops of edges
namespace {

// The edges of `es` connected to `seed` through shared vertices.
std::vector<TopoDS_Edge> connected(const std::vector<TopoDS_Edge>& es, const TopoDS_Edge& seed) {
  std::vector<TopoDS_Edge> out{seed};
  std::vector<char> taken(es.size(), 0);
  for (size_t i = 0; i < es.size(); ++i)
    if (es[i].IsSame(seed)) taken[i] = 1;
  for (bool grew = true; grew;) {
    grew = false;
    for (size_t i = 0; i < es.size(); ++i) {
      if (taken[i]) continue;
      TopoDS_Vertex a, b;
      TopExp::Vertices(es[i], a, b);
      for (const auto& e : out) {
        TopoDS_Vertex c, d;
        TopExp::Vertices(e, c, d);
        if (a.IsSame(c) || a.IsSame(d) || b.IsSame(c) || b.IsSame(d)) {
          taken[i] = 1, out.push_back(es[i]), grew = true;
          break;
        }
      }
    }
  }
  return out;
}

double extent(const std::vector<TopoDS_Edge>& es) {
  Bnd_Box b;
  for (const auto& e : es) BRepBndLib::Add(e, b);
  return b.IsVoid() ? 0.0 : std::sqrt(b.SquareExtent());
}

// A closed flat loop's size across as the plan gives an opening's: 4 x its area / its perimeter (a circle's diameter).
double loop_size(const std::vector<TopoDS_Edge>& es) {
  TopoDS_Wire wire;
  gp_Pln plane;
  if (!loop_wire(es, wire, plane).empty()) return extent(es) / std::sqrt(2.0);
  BRepBuilderAPI_MakeFace mk(plane, wire, true);
  if (!mk.IsDone()) return extent(es) / std::sqrt(2.0);
  GProp_GProps a, l;
  BRepGProp::SurfaceProperties(mk.Face(), a);
  BRepGProp::LinearProperties(wire, l);
  return l.Mass() > 0 ? 4 * std::fabs(a.Mass()) / l.Mass() : extent(es) / std::sqrt(2.0);
}

V loop_centre(const std::vector<TopoDS_Edge>& es) {
  GProp_GProps p;
  for (const auto& e : es) {
    GProp_GProps one;
    BRepGProp::LinearProperties(e, one);
    p.Add(one);
  }
  return {p.CentreOfMass().X(), p.CentreOfMass().Y(), p.CentreOfMass().Z()};
}

std::vector<Ref> refs_of(const std::string& body, const TopoDS_Shape& shape, const std::vector<TopoDS_Edge>& es) {
  std::vector<Ref> out;
  TopTools_IndexedMapOfShape map;
  TopExp::MapShapes(shape, TopAbs_EDGE, map);
  for (const auto& e : es) {
    Ref r;
    r.body = body, r.kind = Ref::Kind::Edge, r.index = map.FindIndex(e) - 1;
    out.push_back(r);
  }
  std::sort(out.begin(), out.end(), [](const Ref& a, const Ref& b) { return a.index < b.index; });
  return out;
}

// The closed flat loops through `edge`, best first: a hole in a flat face, a flat face's whole boundary, any other (the edges
// of a curved face's wire that close round, such as a chamfer's or a bore's end); the smaller first within a kind.
std::vector<std::vector<TopoDS_Edge>> loops_through(const TopoDS_Shape& shape, const TopoDS_Edge& edge) {
  TopTools_IndexedDataMapOfShapeListOfShape faces;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, faces);
  struct Cand {
    int rank;
    double size;
    std::vector<TopoDS_Edge> edges;
  };
  std::vector<Cand> cands;
  if (!faces.Contains(edge)) return {};
  for (TopTools_ListIteratorOfListOfShape it(faces.FindFromKey(edge)); it.More(); it.Next()) {
    const TopoDS_Face f = TopoDS::Face(it.Value());
    const bool flat = BRepAdaptor_Surface(f).GetType() == GeomAbs_Plane;
    const TopoDS_Wire outer = BRepTools::OuterWire(f);
    for (TopExp_Explorer w(f, TopAbs_WIRE); w.More(); w.Next()) {
      const auto es = wire_edges(TopoDS::Wire(w.Current()), f);
      if (std::none_of(es.begin(), es.end(), [&](const TopoDS_Edge& e) { return e.IsSame(edge); })) continue;
      const auto loop = connected(es, edge);
      TopoDS_Wire wire;
      gp_Pln plane;
      if (!loop_wire(loop, wire, plane).empty()) continue;
      const int rank = flat && !w.Current().IsSame(outer) ? 0 : flat ? 1 : 2;
      if (std::any_of(cands.begin(), cands.end(), [&](const Cand& c) { return same_edges(c.edges, loop); })) continue;
      cands.push_back({rank, extent(loop), loop});
    }
  }
  std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.rank != b.rank ? a.rank < b.rank : a.size < b.size - 1e-9; });
  std::vector<std::vector<TopoDS_Edge>> out;
  for (auto& c : cands) out.push_back(std::move(c.edges));
  return out;
}

}  // namespace

std::vector<Ref> opening_loop(const Document& doc, const Scene& scene, const Ref& edge) {
  if (edge.kind != Ref::Kind::Edge) throw Error("flow: an opening's loop is found from one of its edges");
  const Node* n = scene.node(edge.body);
  if (!n || n->kind != Node::Kind::Body || n->body_missing || n->representation != "solid") throw Error("flow: the edge is not on a solid");
  const TopoDS_Shape shape = node_world_shape(doc, scene, edge.body);
  const TopoDS_Shape e = subshape(shape, Ref::Kind::Edge, edge.index);
  if (e.IsNull() || e.ShapeType() != TopAbs_EDGE) throw Error("flow: \"" + n->name + "\" has no edge " + std::to_string(edge.index));
  const auto loops = loops_through(shape, TopoDS::Edge(e));
  if (loops.empty()) throw Error("flow: that edge goes round no opening (no closed flat loop of edges runs through it)");
  return refs_of(edge.body, shape, loops.front());
}

json propose_openings(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies) {
  struct End {
    std::string body;
    std::vector<Ref> edges;
    V centre;
    double size;
  };
  std::vector<End> rings, discs;
  for (const auto& id : bodies) {
    const Node* n = scene.node(id);
    if (!n || n->kind != Node::Kind::Body || n->body_missing || n->representation != "solid") continue;
    const TopoDS_Shape shape = node_world_shape(doc, scene, id);
    TopTools_IndexedDataMapOfShapeListOfShape faces;
    TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, faces);
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
      const TopoDS_Face face = TopoDS::Face(f.Current());
      if (BRepAdaptor_Surface(face).GetType() != GeomAbs_Plane) continue;
      const TopoDS_Wire outer = BRepTools::OuterWire(face);
      std::vector<TopoDS_Wire> holes;
      for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next())
        if (!w.Current().IsSame(outer)) holes.push_back(TopoDS::Wire(w.Current()));
      if (holes.empty()) {
        const auto es = wire_edges(outer, face);
        discs.push_back({id, refs_of(id, shape, es), loop_centre(es), loop_size(es)});
        continue;
      }
      if (holes.size() != 1) continue;
      // A rim's hole; then in past a chamfer or a rounded edge (a short band of one curved face whose other loop is smaller),
      // so the opening is the bore itself.
      std::vector<TopoDS_Edge> loop = wire_edges(holes.front(), face);
      TopoDS_Face from = face;
      for (int step = 0; step < 3; ++step) {
        TopoDS_Face next;
        int others = 0;
        for (const auto& e : loop) {
          if (!faces.Contains(e)) continue;
          for (TopTools_ListIteratorOfListOfShape it(faces.FindFromKey(e)); it.More(); it.Next())
            if (!it.Value().IsSame(from) && (next.IsNull() || !it.Value().IsSame(next))) next = TopoDS::Face(it.Value()), ++others;
        }
        if (others != 1 || next.IsNull()) break;
        const GeomAbs_SurfaceType kind = BRepAdaptor_Surface(next).GetType();
        if (kind == GeomAbs_Plane || kind == GeomAbs_Cylinder) break;
        std::vector<TopoDS_Edge> rest;
        for (TopExp_Explorer w(next, TopAbs_WIRE); w.More(); w.Next())
          for (const auto& e : wire_edges(TopoDS::Wire(w.Current()), next))
            if (std::none_of(loop.begin(), loop.end(), [&](const TopoDS_Edge& x) { return x.IsSame(e); }) &&
                std::none_of(rest.begin(), rest.end(), [&](const TopoDS_Edge& x) { return x.IsSame(e); }))
              rest.push_back(e);
        TopoDS_Wire wire;
        gp_Pln plane;
        Bnd_Box band;
        BRepBndLib::Add(next, band);
        const double size = extent(loop);
        if (rest.empty() || !loop_wire(rest, wire, plane).empty() || extent(rest) >= size) break;
        const V c0 = loop_centre(loop), c1 = loop_centre(rest);
        if (norm(sub(c1, c0)) > 0.1 * size) break;  // a long band: the bore itself, not an edge's chamfer
        loop = rest, from = next;
      }
      rings.push_back({id, refs_of(id, shape, loop), loop_centre(loop), loop_size(loop)});
    }
  }
  // Rings first (a hollow duct's ends); else the two flat faces farthest apart. The wider end first: the air goes in there.
  const std::vector<End>& from = rings.size() >= 2 ? rings : discs;
  const End *a = nullptr, *b = nullptr;
  double farthest = -1;
  for (size_t i = 0; i < from.size(); ++i)
    for (size_t j = i + 1; j < from.size(); ++j)
      if (const double d = norm(sub(from[i].centre, from[j].centre)); d > farthest) farthest = d, a = &from[i], b = &from[j];
  json out = json::array();
  if (!a || !b) return out;
  if (b->size > a->size) std::swap(a, b);
  for (const End* e : {a, b}) {
    json edges = json::array();
    for (const auto& r : e->edges) edges.push_back(r.to_json());
    out.push_back({{"edges", edges}, {"centre", e->centre}, {"size", e->size}, {"body", e->body}});
  }
  return out;
}

// ---------------------------------------------------------------- settings
void water_properties(double T, double& rho, double& nu) {
  rho = 1000 * (1 - (T + 288.9414) / (508929.2 * (T + 68.12963)) * std::pow(T - 3.9863, 2));
  const double mu = 2.414e-5 * std::pow(10.0, 247.8 / (T + 273.15 - 140));
  nu = mu / rho;
}

FlowSettings flow_settings(const json& st) {
  if (!st.is_object()) throw Error("flow: settings is an object");
  // check / guided: what Simulate's guided checks keep with their study (which check, the panel's choices: read back by
  // the panel after an undo or a reopen); nothing here reads them.
  static const std::set<std::string> known = {"domain", "inlet", "outlet", "bodies", "flow", "fan", "direction", "speed", "fluid", "ambient", "quality",
                                              "mesh", "extend", "iterations", "turbulence", "streamlines", "timeout", "check", "guided"};
  for (const auto& [k, v] : st.items())
    if (!known.count(k)) throw Error("flow: unknown setting \"" + k + "\"");
  FlowSettings s;
  s.domain = st.value("domain", std::string("inside"));
  if (s.domain != "inside" && s.domain != "around") throw Error("flow: domain is inside (between an inlet and an outlet) or around (a part in a stream)");
  s.quality = st.value("quality", std::string("normal"));
  if (s.quality != "quick" && s.quality != "normal" && s.quality != "fine") throw Error("flow: quality is quick, normal or fine");
  s.mesh = st.value("mesh", std::string("auto"));
  if (s.mesh != "auto" && s.mesh != "axisymmetric" && s.mesh != "3d") throw Error("flow: mesh is auto, axisymmetric or 3d");
  s.turbulence = st.value("turbulence", std::string("auto"));
  if (s.turbulence == "sst" || s.turbulence == "kOmegaSST") s.turbulence = "sst";
  if (s.turbulence != "auto" && s.turbulence != "sst" && s.turbulence != "laminar") throw Error("flow: turbulence is auto, sst or laminar");
  s.fluid = st.value("fluid", std::string("air"));
  if (s.fluid != "air" && s.fluid != "water") throw Error("flow: fluid is air or water");
  if (st.contains("ambient") && !st["ambient"].is_number()) throw Error("flow: ambient is the fluid's temperature in degC");
  s.ambient = st.value("ambient", 20.0);
  if (s.fluid == "water" && !(s.ambient >= 1 && s.ambient <= 99)) throw Error("flow: water between 1 and 99 degC");
  if (s.fluid == "air" && !(s.ambient >= -40 && s.ambient <= 200)) throw Error("flow: air between -40 and 200 degC");
  if (s.fluid == "air") {
    const air::Air a = air::properties(s.ambient);
    s.rho = a.rho, s.nu = a.nu;
  } else {
    water_properties(s.ambient, s.rho, s.nu);
  }
  for (const auto& b : st.value("bodies", json::array())) {
    if (!b.is_string()) throw Error("flow: bodies is a list of body (or component) ids");
    s.bodies.push_back(b.get<std::string>());
  }
  if (st.contains("extend")) {
    const json e = st["extend"];
    if (e == false || e == 0) s.extend_inlet = s.extend_outlet = 0;
    else if (e.is_object()) {
      for (const auto& [k, v] : e.items())
        if ((k != "inlet" && k != "outlet") || !v.is_number() || v.get<double>() < 0 || v.get<double>() > 50)
          throw Error("flow: extend is {inlet, outlet}: diameters of straight pipe (0 to 50) before the inlet and after the outlet");
      s.extend_inlet = e.value("inlet", s.extend_inlet), s.extend_outlet = e.value("outlet", s.extend_outlet);
    } else {
      throw Error("flow: extend is {inlet, outlet} in diameters, or false");
    }
  }
  if (st.contains("iterations")) {
    if (!st["iterations"].is_number_integer() || st["iterations"].get<int>() < 50 || st["iterations"].get<int>() > 100000)
      throw Error("flow: iterations is a whole number from 50 to 100000");
    s.iterations = st["iterations"].get<int>();
  }
  if (s.domain == "around") {
    for (const char* k : {"inlet", "outlet", "flow", "fan"})
      if (st.contains(k)) throw Error(std::string("flow around a part: ") + k + " is for the inside of a part (domain inside); give direction and speed");
    if (!st.contains("direction") || !st.contains("speed")) throw Error("flow around a part needs direction [x, y, z] (the way the fluid comes) and speed");
    try {
      s.direction = st["direction"].get<Vec3>();
    } catch (const std::exception&) {
      throw Error("flow: direction is [x, y, z]");
    }
    if (norm(s.direction) < 1e-9) throw Error("flow: direction has no length");
    s.direction = unit(s.direction);
    const json sp = st["speed"];
    if (sp.is_number()) s.speed_ms = sp.get<double>();
    else if (sp.is_object() && sp.contains("value") && sp["value"].is_number()) s.speed_ms = sp["value"].get<double>() * speed_unit(sp.value("unit", std::string("m/s")));
    else throw Error("flow: speed is m/s, or {value, unit: m/s | km/h | mph}");
    if (!(s.speed_ms > 0)) throw Error("flow: speed must be above 0");
    s.drive = FlowSettings::Drive::Speed;
    return s;
  }
  for (const char* k : {"direction", "speed"})
    if (st.contains(k)) throw Error(std::string("flow: ") + k + " is for a part in a stream (domain around); inside a part give flow or fan");
  if (!st.contains("inlet") || !st.contains("outlet")) throw Error("flow: give inlet and outlet: the flat faces at the part's two ends");
  s.inlet = opening_refs(st["inlet"], "inlet");
  s.outlet = opening_refs(st["outlet"], "outlet");
  for (const auto& a : s.inlet)
    for (const auto& b : s.outlet)
      if (a.body == b.body && a.kind == b.kind && a.index == b.index) throw Error(std::string("flow: the inlet and the outlet are the same ") + (a.kind == Ref::Kind::Face ? "face" : "loop"));
  if (st.contains("flow") == st.contains("fan")) throw Error("flow: give flow ({value, unit: cfm | m3/h | l/s}) or fan (its curve), one of them");
  if (st.contains("flow")) {
    const json f = st["flow"];
    if (!f.is_object() || !f.contains("value") || !f["value"].is_number()) throw Error("flow: flow is {value, unit: cfm | m3/h | l/s}");
    s.flow_m3s = f["value"].get<double>() * flow_unit(f.value("unit", std::string("m3/h")));
    if (!(s.flow_m3s > 0)) throw Error("flow: the flow must be above 0");
    s.drive = FlowSettings::Drive::Flow;
  } else {
    const json f = st["fan"];
    json spec;
    if (f.is_string() || (f.is_object() && f.contains("model"))) {
      spec = f;
    } else if (f.is_object()) {
      const std::string u = fan_flow_key(f.value("unit", std::string("m3/h")));
      const std::string pu = f.value("pressure_unit", std::string("Pa"));
      try {
        air::pressure_unit(pu);
      } catch (const Error& e) {
        throw Error(std::string("flow: ") + e.what());
      }
      if (f.contains("curve")) {
        const json c = f["curve"];
        if (!c.is_array() || c.size() < 2) throw Error("flow: a fan's curve is [[flow, pressure], ...], two points or more");
        for (size_t i = 0; i < c.size(); ++i) {
          if (!c[i].is_array() || c[i].size() != 2 || !c[i][0].is_number() || !c[i][1].is_number()) throw Error("flow: a fan's curve is [[flow, pressure], ...]");
          if (i && c[i][0].get<double>() <= c[i - 1][0].get<double>())
            throw Error("flow: a fan's curve goes from no flow up: each point at a larger flow than the one before");
        }
        spec = {{"curve", f["curve"]}, {"curve_units", {{"flow", u}, {"pressure", pu}}}};
      } else if (f.contains("free_flow") && f.contains("shutoff_pressure")) {
        spec = {{u == "m3h" ? std::string("flow") : u, f["free_flow"]}, {pu == "Pa" ? std::string("pressure") : pu, f["shutoff_pressure"]}};
      } else {
        throw Error("flow: fan is {curve: [[flow, pressure], ...]} or {free_flow, shutoff_pressure}, with unit (m3/h default, cfm, l/s) and pressure_unit (Pa default)");
      }
      if (f.contains("speed")) spec["speed"] = f["speed"];
      if (f.contains("name")) spec["name"] = f["name"];
    } else {
      throw Error("flow: fan is {curve | free_flow and shutoff_pressure} or a library fan's id");
    }
    try {
      s.fan = air::fan_from(spec);
    } catch (const Error& e) {
      throw Error(std::string("flow: ") + e.what());
    }
    if (!(s.fan.pressure(0) > 0) || !(s.fan.Qmax > 0)) throw Error("flow: the fan's curve has no pressure or no flow");
    s.drive = FlowSettings::Drive::Fan;
  }
  return s;
}

json FlowPlan::to_json() const {
  auto opening = [](const Opening& o) {
    return json{{"centre", o.centre}, {"normal", o.normal}, {"area_mm2", o.area_mm2}, {"diameter_mm", o.diameter_mm}, {"radius_mm", o.radius_mm}, {"outline", o.outline}};
  };
  json j = {{"mesh", mesh}, {"walls", walls}, {"axis", axis}, {"length_mm", length_mm}, {"cell_mm", cell_mm}, {"cells_estimate", cells}, {"iterations", iterations}};
  if (!mode.empty()) j["mode"] = mode, j["inlet"] = opening(inlet), j["outlet"] = opening(outlet), j["extension_mm"] = {{"inlet", inlet_extension_mm}, {"outlet", outlet_extension_mm}};
  if (!why_3d.empty()) j["why_3d"] = why_3d;
  if (seconds > 0)
    j["seconds_estimate"] = std::round(seconds), j["solve_seconds_estimate"] = std::round(solve_seconds), j["seconds_per_cell_iteration"] = per_cell_iteration;
  if (mesh == "axisymmetric") j["radial_cells"] = radial_cells, j["profile_points"] = profile.size(), j["steps"] = steps.size();
  return j;
}

FlowPlan plan_flow(const Document& doc, const Scene& scene, const FlowSettings& settings) { return geometry(doc, scene, settings).plan; }

void write_flow_case(const Document& doc, const Scene& scene, const FlowSettings& settings, const FlowPlan& plan, const fs::path& dir) {
  Geometry g = geometry(doc, scene, settings);
  g.plan = plan;
  write_case(g, settings, dir);
}

// ---------------------------------------------------------------- the run
StudyRun run_flow(const Document& doc, const Scene& scene, const json& st, const Progress& progress) {
  auto report = [&](double f, const std::string& phase) {
    if (progress && !progress(f, phase)) throw Error("cancelled");
  };
  StudyRun run;
  run.kind = "flow";
  const FlowSettings s = flow_settings(st);
  report(0.01, "Looking at the part's openings");
  Geometry g = geometry(doc, scene, s);
  const FlowPlan& plan = g.plan;
  const OpenFoam foam = openfoam();
  if (!foam.found()) throw Error(engines()["unavailable"].value("flow", std::string("flow studies need OpenFOAM")));
  const bool wedge = plan.mesh == "axisymmetric", around = s.domain == "around";
  const Turbulence tb = turbulence_of(s, plan);
  if (!plan.why_3d.empty()) run.warnings.push_back("meshed as the full part, not a wedge of it: " + plan.why_3d);
  if (tb.laminar && s.turbulence == "auto") run.warnings.push_back("laminar: the Reynolds number is " + num(std::round(tb.Re)) + " (below 2000)");

  struct Dir {
    fs::path p;
    bool keep = false;
    ~Dir() {
      std::error_code e;
      if (!keep && !p.empty()) fs::remove_all(p, e);
    }
  } dir;
  dir.p = fs::temp_directory_path() / ("opad-flow-" + new_uuid().substr(0, 12));
  fs::create_directories(dir.p);
  if (const char* k = std::getenv("OPAD_KEEP_CFD"); k && *k) dir.keep = true;
  const int timeout = int(st.value("timeout", 7200.0) * 1000);
  report(0.02, "Writing the OpenFOAM case");
  write_case(g, s, dir.p);
  foam::run(foam, "blockMesh", {}, dir.p, 0.04, -1, timeout, progress);
  if (!wedge) foam::run(foam, "snappyHexMesh", {"-overwrite"}, dir.p, 0.06, 0.3, timeout, progress);
  const double solve_from = wedge ? 0.06 : 0.3;
  // Stopped once the probes' pressure drop has settled: within 0.5 % of it (or 0.05 % of the faster opening's dynamic
  // pressure, for a drop near nothing) over the last 100 iterations; or, a flow that keeps swinging about one state (a jet
  // out of an expansion, solved whole), once the means of its last two 100 iterations agree within 3 % (the result is then
  // the average of the last writes).
  const Probes probes = probes_of(g);
  bool settled = false, swinging = false;
  double q_fast = 0;
  if (!around) {
    const double Q = s.drive == FlowSettings::Drive::Flow ? s.flow_m3s : 0.5 * s.fan.Qmax;
    const double v = Q / (std::min(plan.inlet.area_mm2, plan.outlet.area_mm2) * 1e-6);
    q_fast = 0.5 * v * v;
  }
  std::function<bool()> watch;
  if (!probes.at.empty() && q_fast > 0)
    watch = [&] {
      const auto d = probe_drops(dir.p, probes);
      settled = !d.empty() && drop_settled(d, std::max(0.005 * std::fabs(d.back().second), 5e-4 * q_fast));
      if (!settled && !d.empty() && d.back().first >= 300) {
        double a = 0, b = 0;
        int na = 0, nb = 0;
        for (const auto& [t, v] : d)
          if (t > d.back().first - 100) b += v, ++nb;
          else if (t > d.back().first - 200) a += v, ++na;
        if (na >= 8 && nb >= 8) swinging = std::fabs(b / nb - a / na) <= std::max(0.03 * std::fabs(b / nb), 1e-3 * q_fast);
      }
      return settled || swinging;
    };
  const std::string log = foam::run(foam, "simpleFoam", {}, dir.p, solve_from, 0.9, timeout, progress, watch);
  bool converged = log.find("solution converged") != std::string::npos || settled;
  if (converged && wedge)  // the velocity in the wedge's plane settled too (its last iteration's initial residuals)
    for (const char* c : {"Solving for Ux, Initial residual = ", "Solving for Uy, Initial residual = "})
      if (const size_t at = log.rfind(c); at != std::string::npos && std::atof(log.c_str() + at + std::strlen(c)) > 1e-4) converged = false;
  const std::string last = foam::latest_time(dir.p);
  if (last.empty() || last == "0") throw Error("flow: simpleFoam wrote no result");
  report(0.92, "Reading the flow");
  // Not settled to one state: the average of the last writes (100 iterations) stands for it.
  std::vector<std::string> averaged;
  if (!converged && !around)
    if (auto times = written_times(dir.p); times.size() >= 3) averaged = times;
  const auto m = averaged.empty() ? read_flow(dir.p, last, g, s.rho) : read_flow(dir.p, averaged, g, s.rho);
  if (m->c.empty()) throw Error("flow: the mesh has no cells in the fluid");
  for (const auto& pt : m->patches)  // the box's own faces left: the walls leak (a gap the cells went through)
    if (pt.name == "box" && pt.faces > 0)
      run.warnings.push_back("the fluid reached the edge of the mesh's box (" + std::to_string(pt.faces) + " faces): the walls are not closed between the inlet and the outlet");

  // ---- what went through, what it lost
  json summary = json::object();
  auto patch_flux = [&](const std::string& prefix) {
    const std::string phi = read_text_file(dir.p / last / "phi");
    double Q = 0;
    for (const auto& pt : m->patches)
      if (pt.name.rfind(prefix, 0) == 0)
        if (const auto v = foam::read_patch(phi, pt.name); v.size() == 1 && pt.faces > 1) Q += v[0] * double(pt.faces);  // written uniform
        else
          for (double x : v) Q += x;
    return Q / m->share();
  };
  double vmax = 0;
  size_t vmax_at = 0;
  for (size_t i = 0; i < m->U.size(); ++i)
    if (const double sp = norm(m->U[i]); sp > vmax) vmax = sp, vmax_at = i;
  summary["max_speed_ms"] = vmax;
  summary["max_speed_at"] = m->to_world_point(m->c[vmax_at]);
  double Vref = s.speed_ms;
  const V a_mesh = wedge ? V{1, 0, 0} : plan.axis;
  // Stations along the flow: the inlet face (x = 0) to the outlet face (L), a little of each stub on either side.
  const double L = plan.length_mm;
  if (!around) {
    const double Q = patch_flux("inlet") * -1;
    const FlowPlan::Opening& small = plan.inlet.area_mm2 <= plan.outlet.area_mm2 ? plan.inlet : plan.outlet;
    Vref = Q / (small.area_mm2 * 1e-6);
    const double cell = plan.cell_mm;
    // Planes in the mesh's frame: across the inlet face, just inside; across the outlet face; far down the outlet's stub.
    const V out_n = wedge ? V{1, 0, 0} : g.out.normal;
    const V in_s = wedge ? V{0.3 * cell, 0, 0} : add(g.in.centre, mul(plan.axis, 0.3 * cell));
    const V out_s = wedge ? V{L - 0.3 * cell, 0, 0} : sub(g.out.centre, mul(out_n, 0.3 * cell));
    const double far_at = 0.9 * plan.outlet_extension_mm;
    const V far_s = wedge ? V{L + far_at, 0, 0} : add(g.out.centre, mul(out_n, far_at));
    const double reach_in = 1.2 * g.in.rmax, reach_out = 1.2 * g.out.rmax;
    const auto in = m->section(in_s, a_mesh, reach_in), out = m->section(out_s, out_n, reach_out);
    const auto downstream = far_at > 0 ? m->section(far_s, out_n, reach_out) : out;
    if (!in.cells || !out.cells || !downstream.cells) run.warnings.push_back("the pressure was not measured across every opening (no cells found there)");
    const double Vout = Q / (plan.outlet.area_mm2 * 1e-6), Re_out = Vout * plan.outlet.diameter_mm * 1e-3 / s.nu;
    // The stub's own friction between the outlet face and the far plane: the solved stub's total-pressure slope where its
    // flow has settled (0.6 to 0.9 of it), carried back to the outlet face, so the model's own wall friction is the one taken
    // out; a stub under 4 diameters (or a slope that is no loss) takes the smooth-pipe friction factor instead.
    const double haaland = far_at > 0 ? friction_factor(Re_out) * far_at / plan.outlet.diameter_mm * 0.5 * s.rho * Vout * Vout : 0.0;
    double friction = haaland;
    std::string friction_from = far_at > 0 ? "smooth pipe (Haaland)" : "none";
    if (plan.outlet_extension_mm >= 4 * plan.outlet.diameter_mm) {
      const double mid_at = 0.6 * plan.outlet_extension_mm;
      const auto mid = m->section(wedge ? V{L + mid_at, 0, 0} : add(g.out.centre, mul(out_n, mid_at)), out_n, reach_out);
      const double slope = mid.cells && downstream.cells ? (mid.total - downstream.total) / (far_at - mid_at) : -1;  // Pa/mm
      if (slope > 0) friction = slope * far_at, friction_from = "the stub's own slope";
    }
    summary["friction_from"] = friction_from;
    summary["friction_smooth_pipe_pa"] = haaland;
    const double drop = in.total - downstream.total - friction;
    summary["pressure_drop_pa"] = drop;
    summary["flow_m3s"] = Q;
    summary["flow_cfm"] = Q / kCfm;
    summary["flow_m3h"] = Q * 3600;
    summary["pressure_drop_faces_pa"] = in.total - out.total;
    summary["static_drop_pa"] = in.stat - downstream.stat;
    summary["friction_removed_pa"] = friction;
    summary["inlet_speed_ms"] = Q / (plan.inlet.area_mm2 * 1e-6);
    summary["outlet_speed_ms"] = Vout;
    const double q_ref = 0.5 * s.rho * Vref * Vref;
    summary["loss_coefficient"] = q_ref > 0 ? drop / q_ref : 0.0;
    summary["loss_coefficient_ref"] = plan.inlet.area_mm2 <= plan.outlet.area_mm2 ? "inlet speed" : "outlet speed";
    summary["reynolds"] = Vref * small.diameter_mm * 1e-3 / s.nu;
    if (far_at <= 0) run.warnings.push_back("no straight pipe after the outlet (extend.outlet 0): the pressure the flow regains downstream of an expansion is not counted");
    if (s.drive == FlowSettings::Drive::Flow && std::fabs(Q - s.flow_m3s) > 0.02 * s.flow_m3s)
      run.warnings.push_back("the flow through the inlet came out " + num(Q * 3600) + " m3/h, not the " + num(s.flow_m3s * 3600) + " m3/h asked for");
    if (s.drive == FlowSettings::Drive::Fan) {
      summary["fan"] = {{"name", s.fan.name}, {"free_flow_m3s", s.fan.Qmax}, {"shutoff_pressure_pa", s.fan.pressure(0)},
                        {"operating_point", {{"flow_m3s", Q}, {"flow_cfm", Q / kCfm}, {"pressure_pa", s.fan.pressure(Q)}}},
                        {"share_of_free_flow", Q / s.fan.Qmax}};
      if (Q <= 0) run.warnings.push_back("the fan moved no air through the part");
    }
    // How settled: the probes' drop 100 iterations before the end, else the pressure drop at the write before the last.
    const auto drops = probes.at.empty() ? std::vector<std::pair<double, double>>() : probe_drops(dir.p, probes);
    if (drops.size() >= 2) {
      const double end_t = drops.back().first;
      auto before = drops.begin();
      while (before + 1 != drops.end() && (before + 1)->first <= end_t - 100) ++before;
      summary["pressure_drop_change_pa"] = (drops.back().second - before->second) * s.rho;
      summary["settled_at"] = settled ? json(end_t) : json(nullptr);
      double lo = 1e300, hi = -1e300;
      for (const auto& [t, v] : drops)
        if (t > end_t - 100) lo = std::min(lo, v), hi = std::max(hi, v);
      if (!averaged.empty()) {
        summary["pressure_drop_swing_pa"] = 0.5 * (hi - lo) * s.rho;  // either way about the average (a settled run has none)
        summary["averaged_iterations"] = {std::atof(averaged.front().c_str()), std::atof(averaged.back().c_str())};
        const std::string span = averaged.front() + " to " + averaged.back();
        if (swinging)
          run.warnings.push_back("the flow kept swinging a little about one state (by about " + num(std::round(0.5 * (hi - lo) * s.rho * 100) / 100) +
                                 " Pa either way): the result is the average of iterations " + span);
        else
          run.warnings.push_back("the pressure drop was still moving (by " + num((drops.back().second - before->second) * s.rho) +
                                 " Pa over the last 100 iterations): the result is the average of iterations " + span + "; a finer quality or more iterations");
      }
    } else if (const std::string prev = previous_time(dir.p, last); !prev.empty()) {
      try {
        const auto m2 = read_flow(dir.p, prev, g, s.rho);
        const auto in2 = m2->section(in_s, a_mesh, reach_in);
        const auto far2 = far_at > 0 ? m2->section(far_s, out_n, reach_out) : m2->section(out_s, out_n, reach_out);
        const double before = in2.total - far2.total - friction;
        summary["pressure_drop_change_pa"] = drop - before;
        if (std::fabs(drop - before) > 0.03 * std::max(std::fabs(drop), 0.5))
          run.warnings.push_back("the pressure drop was still moving (" + num(before) + " Pa at iteration " + prev + ", " + num(drop) + " Pa at " + last +
                                 "): a finer quality or more iterations");
      } catch (const std::exception&) {
      }
    }
  } else {
    // The parts' drag: the pressure and the wall's shear on their faces, along the stream.
    const std::string nut = read_text_file(dir.p / last / "nut");
    V force{0, 0, 0}, fp{0, 0, 0};
    for (const auto& pt : m->patches) {
      if (pt.name.rfind("part", 0) != 0) continue;
      const auto nut_f = foam::read_patch(nut, pt.name);
      for (size_t f = pt.start; f < pt.start + pt.faces; ++f) {
        const auto& F = m->faces[f];
        V centre{0, 0, 0}, S{0, 0, 0};
        for (int q : F) centre = add(centre, m->points[size_t(q)]);
        centre = mul(centre, 1.0 / double(F.size()));
        for (size_t q = 0; q < F.size(); ++q) S = add(S, mul(cross(sub(m->points[size_t(F[q])], centre), sub(m->points[size_t(F[(q + 1) % F.size()])], centre)), 0.5));
        const size_t cidx = size_t(m->own[f]);
        const V Sm = mul(S, 1e-6);  // m2, out of the fluid: into the part
        const V press = mul(Sm, m->p[cidx]);
        const V nhat = unit(S);
        const V Ut = sub(m->U[cidx], mul(nhat, dot(m->U[cidx], nhat)));
        const double d = std::max(1e-6, std::fabs(dot(sub(m->c[cidx], centre), nhat))) * 1e-3;
        const double nut_v = nut_f.empty() ? 0.0 : nut_f[std::min(nut_f.size() - 1, f - pt.start)];
        const V shear = mul(Ut, s.rho * (s.nu + nut_v) / d * norm(Sm));
        force = add(force, add(press, shear)), fp = add(fp, press);
      }
    }
    const double drag = dot(force, plan.axis);
    summary["drag_n"] = drag;
    summary["pressure_drag_n"] = dot(fp, plan.axis);
    summary["force_n"] = force;
    // Against the parts' box seen along the stream.
    Bnd_Box b;
    for (const auto& w : g.walls) BRepBndLib::Add(w, b);
    double c[6];
    b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
    const V e2 = cross(plan.axis, g.e1);
    double lo1 = 1e300, hi1 = -1e300, lo2 = 1e300, hi2 = -1e300;
    for (double x : {c[0], c[3]})
      for (double y : {c[1], c[4]})
        for (double z : {c[2], c[5]}) {
          const V q{x, y, z};
          lo1 = std::min(lo1, dot(q, g.e1)), hi1 = std::max(hi1, dot(q, g.e1)), lo2 = std::min(lo2, dot(q, e2)), hi2 = std::max(hi2, dot(q, e2));
        }
    const double A = (hi1 - lo1) * (hi2 - lo2) * 1e-6;
    summary["frontal_box_m2"] = A;
    summary["drag_coefficient_box"] = A > 0 ? drag / (0.5 * s.rho * s.speed_ms * s.speed_ms * A) : 0.0;
    summary["speed_ms"] = s.speed_ms;
    summary["reynolds"] = tb.Re;
  }
  if (!converged && !swinging) run.warnings.push_back("the solver had not converged after " + std::to_string(plan.iterations) + " iterations (its residuals)");

  // ---- recirculation: cells whose flow runs back against the way the fluid goes there, in groups that touch a wall
  {
    const size_t n = m->c.size();
    std::vector<V> way(n, a_mesh);
    if (!wedge && !around) {
      // The local way: the mean velocity of the cells at that distance along the line from the inlet to the outlet.
      const V c0 = g.in.centre, dirv = unit(sub(g.out.centre, g.in.centre));
      const double bin = 0.25 * std::min(plan.inlet.diameter_mm, plan.outlet.diameter_mm);
      std::map<long, V> mean;
      auto key = [&](size_t i) { return long(std::floor(dot(sub(m->c[i], c0), norm(dirv) > 0 ? dirv : plan.axis) / bin)); };
      for (size_t i = 0; i < n; ++i) mean[key(i)] = add(mean[key(i)], mul(m->U[i], m->vol[i]));
      for (size_t i = 0; i < n; ++i) way[i] = unit(mean[key(i)]);
    }
    std::vector<char> back(n, 0);
    for (size_t i = 0; i < n; ++i) back[i] = dot(m->U[i], way[i]) < -0.02 * std::max(Vref, 1e-6);
    std::vector<size_t> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<size_t(size_t)> root = [&](size_t i) {
      while (parent[i] != i) parent[i] = parent[parent[i]], i = parent[i];
      return i;
    };
    for (size_t f = 0; f < m->nei.size(); ++f) {
      const size_t a = size_t(m->own[f]), b = size_t(m->nei[f]);
      if (back[a] && back[b]) parent[root(a)] = root(b);
    }
    std::vector<char> at_wall(n, 0);
    for (const auto& pt : m->patches)
      if (pt.type == "wall")
        for (size_t f = pt.start; f < pt.start + pt.faces; ++f) at_wall[size_t(m->own[f])] = 1;
    struct Group {
      double vol = 0;
      V moment{0, 0, 0};
      std::array<double, 6> box{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
      size_t cells = 0;
      bool wall = false;
      V way{0, 0, 0};
    };
    std::map<size_t, Group> groups;
    for (size_t i = 0; i < n; ++i) {
      if (!back[i]) continue;
      Group& gr = groups[root(i)];
      gr.vol += m->vol[i], gr.moment = add(gr.moment, mul(m->c[i], m->vol[i])), ++gr.cells;
      gr.wall = gr.wall || at_wall[i];
      gr.way = add(gr.way, way[i]);
      for (int k = 0; k < 3; ++k) gr.box[size_t(k)] = std::min(gr.box[size_t(k)], m->box[i][size_t(k)]), gr.box[size_t(k) + 3] = std::max(gr.box[size_t(k) + 3], m->box[i][size_t(k) + 3]);
    }
    std::vector<Group> kept;
    for (auto& [r, gr] : groups)
      if (gr.cells >= 3 && (gr.wall || around)) kept.push_back(gr);
    std::sort(kept.begin(), kept.end(), [](const Group& x, const Group& y) { return x.vol > y.vol; });
    json list = json::array();
    for (size_t k = 0; k < kept.size() && k < 8; ++k) {
      const Group& gr = kept[k];
      const V centre = mul(gr.moment, 1 / gr.vol);
      const V w = unit(gr.way);
      double along_lo = 1e300, along_hi = -1e300;
      for (double x : {gr.box[0], gr.box[3]})
        for (double y : {gr.box[1], gr.box[4]})
          for (double z : {gr.box[2], gr.box[5]}) along_lo = std::min(along_lo, dot({x, y, z}, w)), along_hi = std::max(along_hi, dot({x, y, z}, w));
      const double size = std::max({gr.box[3] - gr.box[0], gr.box[4] - gr.box[1], wedge ? 0.0 : gr.box[5] - gr.box[2]});
      json e = {{"at", m->to_world_point(centre)}, {"size_mm", size}, {"length_mm", along_hi - along_lo}, {"volume_mm3", gr.vol / m->share()}, {"cells", gr.cells}};
      if (wedge) e["ring"] = true, e["from_mm"] = gr.box[0], e["to_mm"] = gr.box[3], e["radius_mm"] = {gr.box[1], gr.box[4]};
      list.push_back(e);
    }
    summary["recirculation"] = list;
  }

  // ---- the results to show
  auto res = std::make_shared<FeaResult>();
  res->kind = "flow";
  res->bodies = plan.walls;
  res->mesh_size = plan.cell_mm;
  for (size_t b = 0; b < g.walls.size(); ++b) {
    BRepMesh_IncrementalMesh(g.walls[b], std::max(0.05, plan.cell_mm / 4), false, 0.3, true);
    for (TopExp_Explorer e(g.walls[b], TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face face = TopoDS::Face(e.Current());
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
      if (tri.IsNull()) continue;
      const int base = int(res->nodes.size());
      for (int k = 1; k <= tri->NbNodes(); ++k) {
        const gp_Pnt q = tri->Node(k).Transformed(loc.Transformation());
        const V w{q.X(), q.Y(), q.Z()};
        const size_t i = m->cell_at(w, false);
        res->nodes.push_back(w);
        res->speed.push_back(norm(m->U[i]));
        res->pressure.push_back(m->p[i]);
      }
      const bool rev = face.Orientation() == TopAbs_REVERSED;
      for (int t = 1; t <= tri->NbTriangles(); ++t) {
        int n1, n2, n3;
        tri->Triangle(t).Get(n1, n2, n3);
        if (rev) std::swap(n2, n3);
        res->skin.push_back({base + n1 - 1, base + n2 - 1, base + n3 - 1});
        res->skin_body.push_back(int(b));
      }
    }
  }
  res->displacement.assign(res->nodes.size(), V{0, 0, 0});
  res->temperature.assign(res->nodes.size(), s.ambient);
  res->flow_at = [m](const Vec3& at, Vec3& u, double& p) { return m->at(at, u, p); };
  // The cut: the plane through the axis (inside) or through the parts along the stream (around), sampled on a grid.
  {
    V centre = g.in.centre, ex = plan.axis, ey = g.e1;
    double x0, x1, y0, y1;
    if (around) {
      centre = plan.inlet.centre;
      const double size = plan.length_mm;
      x0 = -1.0 * size, x1 = 3.0 * size, y0 = -1.0 * size, y1 = 1.0 * size;
    } else {
      if (!wedge) {
        const V d = sub(g.out.centre, g.in.centre);
        const V side = sub(d, mul(ex, dot(d, ex)));
        if (norm(side) > 0.1 * norm(d)) ey = unit(side);
      }
      double r = std::max(g.in.rmax, g.out.rmax);
      for (const auto& q : plan.profile) r = std::max(r, q[1]);
      Bnd_Box b;
      for (const auto& w : g.walls) BRepBndLib::Add(w, b);
      double c[6];
      b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
      x0 = -std::min(plan.inlet_extension_mm, plan.inlet.diameter_mm), x1 = -x0, y0 = -1.05 * r, y1 = 1.05 * r;
      for (double x : {c[0], c[3]})
        for (double y : {c[1], c[4]})
          for (double z : {c[2], c[5]}) {
            const V q = sub(V{x, y, z}, centre);
            x1 = std::max(x1, dot(q, ex)), y0 = std::min(y0, dot(q, ey)), y1 = std::max(y1, dot(q, ey));
          }
      x1 = std::max(x1, L) + std::min(plan.outlet_extension_mm, 2 * plan.outlet.diameter_mm);
    }
    const double step = std::max((x1 - x0) / 240, (y1 - y0) / 80);
    const int nx = int(std::ceil((x1 - x0) / step)) + 1, ny = int(std::ceil((y1 - y0) / step)) + 1;
    std::vector<int> idx(size_t(nx) * size_t(ny), -1);
    for (int i = 0; i < nx; ++i)
      for (int j = 0; j < ny; ++j) {
        const V w = add(centre, add(mul(ex, x0 + i * step), mul(ey, y0 + j * step)));
        V u;
        double pr;
        if (!m->at(w, u, pr)) continue;
        idx[size_t(i) * size_t(ny) + size_t(j)] = int(res->cut_nodes.size());
        res->cut_nodes.push_back(w), res->cut_speed.push_back(norm(u)), res->cut_pressure.push_back(pr);
      }
    for (int i = 0; i + 1 < nx; ++i)
      for (int j = 0; j + 1 < ny; ++j) {
        const int a = idx[size_t(i) * size_t(ny) + size_t(j)], b = idx[size_t(i + 1) * size_t(ny) + size_t(j)], c = idx[size_t(i + 1) * size_t(ny) + size_t(j + 1)],
                  d = idx[size_t(i) * size_t(ny) + size_t(j + 1)];
        if (a >= 0 && b >= 0 && c >= 0) res->cut.push_back({a, b, c});
        if (a >= 0 && c >= 0 && d >= 0) res->cut.push_back({a, c, d});
      }
  }
  // Streamlines: from across the inlet (around: across the stream ahead of the parts), and from each recirculation's middle.
  {
    const int across = std::clamp(st.value("streamlines", 8), 0, 40);
    std::vector<V> seeds;
    if (around) {
      const double size = plan.length_mm;
      const V e2 = cross(plan.axis, g.e1);
      for (int i = 0; i < across; ++i)
        for (int j = 0; j < across; ++j)
          seeds.push_back(add(plan.inlet.centre, add(mul(plan.axis, -0.8 * size), add(mul(g.e1, size * (-0.6 + 1.2 * (i + 0.5) / across)), mul(e2, size * (-0.6 + 1.2 * (j + 0.5) / across))))));
    } else {
      const double back = -std::min(plan.inlet_extension_mm, 0.5 * plan.inlet.diameter_mm) + 0.3 * plan.cell_mm;
      const V e2 = cross(plan.axis, g.e1);
      if (wedge) {
        for (int i = 0; i < across; ++i)
          for (int k = 0; k < 4; ++k) {
            const double r = g.in.rmax * (i + 0.5) / across, phi = kPi / 2 * k;
            seeds.push_back(add(add(g.in.centre, mul(plan.axis, back)), mul(add(mul(g.e1, std::cos(phi)), mul(e2, std::sin(phi))), r)));
          }
      } else {
        for (int i = 0; i < across; ++i)
          for (int j = 0; j < across; ++j) {
            const V q = add(g.in.centre, add(mul(g.e1, g.in.rmax * (-1 + 2 * (i + 0.5) / across)), mul(e2, g.in.rmax * (-1 + 2 * (j + 0.5) / across))));
            if (BRepClass_FaceClassifier(g.in.face, gp_Pnt(q[0], q[1], q[2]), 1e-6).State() == TopAbs_IN) seeds.push_back(add(q, mul(plan.axis, back)));
          }
      }
    }
    for (const auto& r : summary["recirculation"]) seeds.push_back(r["at"].get<Vec3>());
    const double reach = around ? 6 * plan.length_mm : L + plan.inlet.diameter_mm + 3 * plan.outlet.diameter_mm;
    for (const V& seed : seeds) {
      std::vector<Vec3> line;
      std::vector<double> speed;
      V p = seed;
      double travelled = 0;
      for (int step = 0; step < 6000 && travelled < 3 * reach; ++step) {
        V u;
        double pr;
        if (!m->at(p, u, pr)) break;
        const double sp = norm(u);
        if (sp < 1e-3 * std::max(Vref, 1e-6)) break;
        if (step % 2 == 0) line.push_back(p), speed.push_back(sp);
        const size_t ci = m->cell_at(p, false);
        const auto& b = m->box[ci];
        const double h = 0.5 * std::max(1e-3, std::min(b[3] - b[0], b[4] - b[1]));
        V um;
        if (!m->at(add(p, mul(u, 0.5 * h / sp)), um, pr) || norm(um) < 1e-9) break;
        p = add(p, mul(um, h / norm(um)));
        travelled += h;
        if (!around && dot(sub(p, g.out.centre), g.out.normal) > std::min(plan.outlet_extension_mm, 2 * plan.outlet.diameter_mm)) break;
        if (around && dot(sub(p, plan.inlet.centre), plan.axis) > 3 * plan.length_mm) break;
      }
      if (line.size() < 3) continue;
      res->streamline_temperature.push_back(std::vector<double>(line.size(), s.ambient));
      res->streamlines.push_back(std::move(line));
      res->streamline_speed.push_back(std::move(speed));
    }
  }

  // ---- series along the flow: from a little before the inlet face to a little past the outlet face
  {
    const double from = around ? -1.5 * plan.length_mm : -std::min(plan.inlet_extension_mm, plan.inlet.diameter_mm);
    const double to = around ? 3 * plan.length_mm : L + std::min(plan.outlet_extension_mm, 3 * plan.outlet.diameter_mm);
    const int n = 81;
    Series ps{"flow", "Static pressure", "Pa", "pressure", {}}, pt{"flow", "Total pressure", "Pa", "pressure", {}}, vm{"flow", "Mean speed", "m/s", "velocity", {}},
        vx{"flow", "Max speed", "m/s", "velocity", {}};
    const double reach = around || wedge ? 1e300 : 1.5 * std::max(g.in.rmax, g.out.rmax);
    const V base = wedge ? V{0, 0, 0} : around ? plan.inlet.centre : g.in.centre;
    const V along = wedge ? V{1, 0, 0} : around ? plan.axis : (norm(sub(g.out.centre, g.in.centre)) > 1e-6 ? unit(sub(g.out.centre, g.in.centre)) : plan.axis);
    for (int i = 0; i < n; ++i) {
      const double x = from + (to - from) * i / (n - 1);
      const auto sec = m->section(add(base, mul(along, x)), along, reach);
      if (!sec.cells) continue;
      run.t.push_back(x);
      ps.v.push_back(sec.stat), pt.v.push_back(sec.total), vm.v.push_back(sec.mean), vx.v.push_back(sec.vmax);
    }
    run.series = {ps, pt, vm, vx};
    summary["series_along"] = around ? "distance_mm along the stream from the parts' middle" : "distance_mm from the inlet face along the line to the outlet";
  }

  summary["engine"] = std::string("OpenFOAM (") + (wedge ? "blockMesh wedge" : "blockMesh, snappyHexMesh") + ", simpleFoam, " +
                      (tb.laminar ? "laminar" : "k-omega SST, Spalding wall function") + ")";
  summary["domain"] = s.domain;
  summary["mesh"] = plan.mesh;
  summary["plan"] = plan.to_json();
  summary["cells"] = m->c.size();
  summary["fluid"] = s.fluid;
  summary["ambient_C"] = s.ambient;
  summary["density_kg_m3"] = s.rho;
  summary["viscosity_m2_s"] = s.nu;
  summary["turbulence"] = tb.laminar ? "laminar" : "kOmegaSST";
  summary["iterations"] = std::stoi(last);
  // What one iteration cost here (one core), per cell: the estimate's figure, measured (the log's last execution time).
  if (const size_t at = log.rfind("ExecutionTime = "); at != std::string::npos && std::stoi(last) > 0 && !m->c.empty())
    summary["seconds_per_cell_iteration"] = std::atof(log.c_str() + at + 16) / (double(std::stoi(last)) * double(m->c.size()));
  summary["converged"] = converged;
  summary["streamlines"] = res->streamlines.size();
  summary["case_dir"] = dir.keep ? json(path_to_utf8(dir.p)) : json(nullptr);
  summary["warnings"] = run.warnings;
  run.summary = summary;
  run.fea = res;
  report(1.0, "Done");
  return run;
}

}  // namespace opad::sim
