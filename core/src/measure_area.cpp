// The area a pick encloses and its perimeter (UI-90): fills and faces, loops of edges (picked, or grown from one edge of a
// drawing through its body's edges), the polygon through picked points. Declared in opad/inspect.hpp.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>

#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

namespace opad {
namespace {
constexpr int kBoundaryPoints = 2000;  // the outline handed to the view (and kept by a pinned result), all loops together
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
// Newell's normal of a closed polygon: its length is twice the polygon's area.
gp_Vec newell(const std::vector<gp_Pnt>& pts) {
  gp_Vec n(0, 0, 0);
  for (size_t i = 0; i < pts.size(); ++i) n += gp_Vec(pts[i].XYZ()).Crossed(gp_Vec(pts[(i + 1) % pts.size()].XYZ()));
  return n;
}

// One edge of a boundary, walked from its first parameter to its last (forward) or back.
struct Piece {
  TopoDS_Edge edge;
  bool forward = true;
};

// ∮ (x dy - y dx) / 2 along the piece in the plane: a closed walk's signed area (positive counter-clockwise about n).
double green(const Piece& piece, const Plane& p) {
  const BRepAdaptor_Curve c(piece.edge);
  const double t0 = c.FirstParameter(), t1 = c.LastParameter();
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

double length(const TopoDS_Edge& e) {
  try {
    return GCPnts_AbscissaPoint::Length(BRepAdaptor_Curve(e));
  } catch (const Standard_Failure&) {
    return 0;
  }
}

// Points along the piece in walking order, the last one left out (the next piece starts there).
void sample(const Piece& piece, std::vector<gp_Pnt>& out, int count) {
  const BRepAdaptor_Curve c(piece.edge);
  const double t0 = c.FirstParameter(), t1 = c.LastParameter();
  const int n = c.GetType() == GeomAbs_Line ? 1 : std::max(2, count);
  for (int i = 0; i < n; ++i) {
    const double s = double(i) / n;
    out.push_back(c.Value(piece.forward ? t0 + (t1 - t0) * s : t1 - (t1 - t0) * s));
  }
}
int samplesFor(const TopoDS_Edge& e, double size) {
  const BRepAdaptor_Curve c(e);
  if (c.GetType() == GeomAbs_Line) return 1;
  return std::clamp(int(length(e) / std::max(size, 1e-9) * 200), 12, 256);
}

// Endpoints merged within a tolerance (a grid of cells that size, neighbours searched).
struct Nodes {
  double tol;
  std::vector<gp_Pnt> at;
  std::map<std::array<long long, 3>, std::vector<int>> cells;
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
  TopoDS_Edge edge;
  int a = -1, b = -1;
  gp_Vec leaveA, leaveB;  // tangents leaving each end into the edge
  bool alive = true;
};
Edge makeEdge(const TopoDS_Edge& e, Nodes& nodes) {
  Edge out;
  out.edge = e;
  const BRepAdaptor_Curve c(e);
  const double t0 = c.FirstParameter(), t1 = c.LastParameter();
  gp_Pnt p0, p1;
  gp_Vec d0, d1;
  c.D1(t0, p0, d0);
  c.D1(t1, p1, d1);
  // A cusp or a degenerate derivative: the chord towards a point just inside.
  if (d0.Magnitude() < 1e-12) d0 = gp_Vec(p0, c.Value(t0 + (t1 - t0) * 1e-3));
  if (d1.Magnitude() < 1e-12) d1 = gp_Vec(c.Value(t1 - (t1 - t0) * 1e-3), p1);
  out.a = nodes.id(p0);
  out.b = nodes.id(p1);
  out.leaveA = d0;
  out.leaveB = -d1;
  return out;
}

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
    for (const Piece& piece : l.pieces) samples += samplesFor(piece.edge, size);
  const double keep = std::min(1.0, double(kBoundaryPoints) / std::max(1, samples));
  for (const Loop& l : loops) {
    std::vector<gp_Pnt> pts;
    for (const Piece& piece : l.pieces) {
      perimeter += length(piece.edge);
      sample(piece, pts, int(samplesFor(piece.edge, size) * keep));
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

// The smallest loop through `start` among `edges` (indices into all): faces of the planar graph traced on each side of
// it (at every end the sharpest turn towards that side), a bounded one kept (counter-clockwise on the left, clockwise on
// the right); dangling edges are pruned first. Empty when the edge closes nothing.
Loop growLoop(std::vector<Edge>& all, int start, const Plane& p, size_t nodeCount, const std::function<bool()>& cancelled) {
  std::vector<std::vector<int>> at(nodeCount);
  for (size_t i = 0; i < all.size(); ++i) {
    at[all[i].a].push_back(int(i));
    if (all[i].b != all[i].a) at[all[i].b].push_back(int(i));
  }
  // Prune: an end met by one edge only (a self-closed edge counts twice) cannot be on a loop.
  std::vector<int> degree(nodeCount, 0), stack;
  for (const Edge& e : all) degree[e.a]++, degree[e.b]++;
  for (size_t n = 0; n < nodeCount; ++n)
    if (degree[n] == 1) stack.push_back(int(n));
  while (!stack.empty()) {
    const int n = stack.back();
    stack.pop_back();
    for (int i : at[n])
      if (all[i].alive && degree[n] == 1) {
        all[i].alive = false;
        for (int end : {all[i].a, all[i].b})
          if (--degree[end] == 1) stack.push_back(end);
      }
  }
  if (!all[start].alive) return {};
  auto angle = [&](const gp_Vec& d) { return std::atan2(d.Dot(p.v), d.Dot(p.u)); };
  auto trace = [&](bool left) -> Loop {
    Loop loop;
    Piece cur{all[start].edge, true};
    int curEdge = start;
    const size_t limit = 2 * all.size() + 2;
    for (size_t step = 0; step < limit; ++step) {
      if (cancelled && step % 256 == 0 && cancelled()) throw Error("cancelled");
      loop.pieces.push_back(cur);
      loop.area += green(cur, p);
      const Edge& e = all[curEdge];
      const int node = cur.forward ? e.b : e.a;
      const double back = angle(cur.forward ? e.leaveB : e.leaveA);  // the way we came, leaving the node
      int bestEdge = -1;
      bool bestForward = true;
      double best = 1e9;
      for (int i : at[node]) {
        if (!all[i].alive) continue;
        for (const bool forward : {true, false}) {
          if ((forward ? all[i].a : all[i].b) != node) continue;
          if (i == curEdge && forward != cur.forward) continue;  // straight back (unless nothing else leaves)
          const double a = angle(forward ? all[i].leaveA : all[i].leaveB);
          double turn = left ? back - a : a - back;  // clockwise from the way back = the sharpest left turn
          turn = std::fmod(turn + 4 * M_PI, 2 * M_PI);
          if (turn < 1e-12) turn = 2 * M_PI;
          if (turn < best) best = turn, bestEdge = i, bestForward = forward;
        }
      }
      if (bestEdge < 0) return {};
      if (bestEdge == start && bestForward) return loop;
      cur = {all[bestEdge].edge, bestForward};
      curEdge = bestEdge;
    }
    return {};
  };
  Loop best;
  for (const bool left : {true, false}) {
    Loop l = trace(left);
    if (l.pieces.empty() || (left ? l.area <= 1e-12 : l.area >= -1e-12)) continue;  // the unbounded face around the drawing
    if (best.pieces.empty() || std::abs(l.area) < std::abs(best.area)) best = std::move(l);
  }
  return best;
}

gp_Pnt pointOf(const Document& doc, const Scene& scene, const Ref& r) {
  if (r.kind == Ref::Kind::Point) return gp_Pnt(r.point[0], r.point[1], r.point[2]);
  return BRep_Tool::Pnt(TopoDS::Vertex(subshape(node_world_shape(doc, scene, r.body), r.kind, r.index)));
}
}  // namespace

json measure_area(const Document& doc, const Scene& scene, const std::vector<Ref>& refs, const std::function<bool()>& cancelled) {
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
    int samples = 0;
    for (const Loop& l : loops)
      for (const Piece& piece : l.pieces) samples += samplesFor(piece.edge, std::sqrt(std::max(total.Mass(), 1e-12)) * 4);
    const double keep = std::min(1.0, double(kBoundaryPoints) / std::max(1, samples));
    json boundary = json::array();
    for (const Loop& l : loops) {
      std::vector<gp_Pnt> pts;
      for (const Piece& piece : l.pieces) sample(piece, pts, int(samplesFor(piece.edge, std::sqrt(std::max(total.Mass(), 1e-12)) * 4) * keep));
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
    if (picked.size() == 1 && !BRep_Tool::IsClosed(picked[0])) {
      // One edge that does not close: the loop it lies on, among the edges of its drawing body.
      const Ref& r = refs.front();
      const Node* body = scene.node(r.body);
      Edge probe = makeEdge(picked[0], nodes);
      if (probe.a != probe.b) {
        if (!body || body->representation != "drawing2d")
          return {{"kind", "area"}, {"value", 0.0}, {"unit", "mm2"}, {"perimeter", length(picked[0])}, {"loops", 0}, {"closed", false}, {"open_ends", 2}, {"refs", refList},
                  {"boundary", json::array()}};
        const Mat4 w = scene.world(r.body);
        const Vec3 o = w.apply({0, 0, 0}), z = w.apply_dir({0, 0, 1});
        const Plane p = plane(gp_Pnt(o[0], o[1], o[2]), gp_Vec(z[0], z[1], z[2]));
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(world(r.body), TopAbs_EDGE, map);
        Bnd_Box all;
        BRepBndLib::Add(world(r.body), all);
        Nodes bodyNodes{std::max(1e-6, (all.IsVoid() ? size : std::sqrt(all.SquareExtent())) * 1e-7), {}, {}};
        std::vector<Edge> edges;
        int start = -1;
        for (int i = 1; i <= map.Extent(); ++i) {
          if (cancelled && i % 4096 == 0 && cancelled()) throw Error("cancelled");
          const TopoDS_Edge& e = TopoDS::Edge(map(i));
          if (BRep_Tool::Degenerated(e)) continue;
          try {
            edges.push_back(makeEdge(e, bodyNodes));
          } catch (const Standard_Failure&) {
            continue;
          }
          if (e.IsSame(picked[0])) start = int(edges.size()) - 1;
        }
        Loop loop = start < 0 ? Loop() : growLoop(edges, start, p, bodyNodes.at.size(), cancelled);
        if (loop.pieces.empty())
          return {{"kind", "area"}, {"value", 0.0}, {"unit", "mm2"}, {"perimeter", length(picked[0])}, {"loops", 0}, {"closed", false}, {"open_ends", 2}, {"refs", refList},
                  {"boundary", json::array()}};
        out = loopsResult({loop}, p, size);
        out["grown"] = true;
        out["edges"] = int(loop.pieces.size());
        out["refs"] = refList;
        return out;
      }
    }
    // The picked edges alone: every end met by exactly two of them, walked into loops.
    std::vector<Edge> edges;
    for (const auto& e : picked) edges.push_back(makeEdge(e, nodes));
    std::vector<std::vector<int>> at(nodes.at.size());
    for (size_t i = 0; i < edges.size(); ++i) {
      at[edges[i].a].push_back(int(i));
      at[edges[i].b].push_back(int(i));
    }
    int ends = 0, branches = 0;
    for (const auto& list : at) ends += list.size() == 1, branches += list.size() > 2;
    double perimeter = 0;
    for (const auto& e : picked) perimeter += length(e);
    if (ends || branches) {
      json loose = json::array();
      for (size_t n = 0; n < at.size(); ++n)
        if (at[n].size() == 1) loose.push_back(pnt(nodes.at[n]));
      return {{"kind", "area"}, {"value", 0.0}, {"unit", "mm2"}, {"perimeter", perimeter}, {"loops", 0}, {"closed", false}, {"open_ends", ends}, {"branches", branches},
              {"ends", loose}, {"refs", refList}, {"boundary", json::array()}};
    }
    std::vector<Loop> loops;
    std::vector<bool> used(edges.size(), false);
    for (size_t s = 0; s < edges.size(); ++s) {
      if (used[s]) continue;
      Loop loop;
      int cur = int(s), node = edges[s].a;
      while (!used[cur]) {
        used[cur] = true;
        const bool forward = edges[cur].a == node;
        loop.pieces.push_back({edges[cur].edge, forward});
        node = forward ? edges[cur].b : edges[cur].a;
        for (int next : at[node])
          if (!used[next]) {
            cur = next;
            break;
          }
      }
      loops.push_back(std::move(loop));
    }
    // The plane of the loop that encloses the most (Newell's normal): the others are measured in it.
    gp_Vec normal(0, 0, 0);
    gp_XYZ c(0, 0, 0);
    for (const Loop& l : loops) {
      std::vector<gp_Pnt> pts;
      for (const Piece& piece : l.pieces) sample(piece, pts, samplesFor(piece.edge, size) / 4 + 1);
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
  }
  out["refs"] = refList;
  return out;
}

}  // namespace opad
