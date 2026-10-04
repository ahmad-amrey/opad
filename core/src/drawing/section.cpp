// Section views (TODO 11 UI-82): the bodies a cutting line crosses, cut where it is swept through the model (what lies
// towards the viewer taken away), and the faces the cut leaves facing the viewer, for hatching. Bodies wholly on one side
// are kept or dropped by their boxes, without a boolean; each cut is cached by body key, placement and cut. An aligned
// section cuts each segment's strip on its own and revolves it about the joints before it onto the first segment's line.
// A broken-out section takes away what lies nearer the viewer than its depth within a closed outline.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Bnd_Box2d.hxx>
#include <BndLib_Add2dCurve.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Geom2dAPI_Interpolate.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <GeomAPI.hxx>
#include <NCollection_DataMap.hxx>
#include <OSD_Parallel.hxx>
#include <OSD_ThreadPool.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt2d.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <map>
#include <mutex>

#include "opad/drawing/sheet.hpp"
#include "opad/geometry.hpp"
#include "projection_internal.hpp"

namespace opad::drawing::detail {
namespace {

Vec2 sub(Vec2 a, Vec2 b) { return {a[0] - b[0], a[1] - b[1]}; }
Vec2 add(Vec2 a, Vec2 b) { return {a[0] + b[0], a[1] + b[1]}; }
Vec2 mul(Vec2 a, double s) { return {a[0] * s, a[1] * s}; }
double dot(Vec2 a, Vec2 b) { return a[0] * b[0] + a[1] * b[1]; }
double cross(Vec2 a, Vec2 b) { return a[0] * b[1] - a[1] * b[0]; }
Vec2 left(Vec2 a) { return {-a[1], a[0]}; }
Vec2 unit(Vec2 a) {
  const double l = std::hypot(a[0], a[1]);
  return l > 0 ? mul(a, 1 / l) : a;
}
Vec2 turned(Vec2 p, double a) { return {std::cos(a) * p[0] - std::sin(a) * p[1], std::sin(a) * p[0] + std::cos(a) * p[1]}; }

struct Cutter {
  gp_XYZ x, y, n;          // the cut plane's axes and its normal (the sweep)
  std::vector<Vec2> line;  // as given
  double reach = 1;
  Vec2 in_plane(const gp_XYZ& p) const { return {p.Dot(x), p.Dot(y)}; }
  gp_XYZ at(Vec2 p) const { return x * p[0] + y * p[1]; }
};

// The square's boundary point a ray from inside leaves it at.
Vec2 exit_point(Vec2 p, Vec2 u, double b) {
  double t = 1e300;
  for (int i = 0; i < 2; ++i)
    if (std::fabs(u[static_cast<size_t>(i)]) > 1e-15) t = std::min(t, ((u[static_cast<size_t>(i)] > 0 ? b : -b) - p[static_cast<size_t>(i)]) / u[static_cast<size_t>(i)]);
  return add(p, mul(u, t));
}

// What stays of the plane: the cutting line carried out at both ends to a square of half side b, closed round that square
// on the side away from the viewer (the line is walked so that this side is on its left, and the square anticlockwise).
std::vector<Vec2> kept_outline(std::vector<Vec2> line, Vec2 removed, double b) {
  const Vec2 t0 = unit(sub(line[1], line[0]));
  if (dot(removed, {-t0[1], t0[0]}) > 0) std::reverse(line.begin(), line.end());
  const Vec2 s = exit_point(line.front(), unit(sub(line[0], line[1])), b);
  const Vec2 e = exit_point(line.back(), unit(sub(line[line.size() - 1], line[line.size() - 2])), b);
  std::vector<Vec2> out{s};
  out.insert(out.end(), line.begin(), line.end());
  out.push_back(e);
  const auto turn = [](Vec2 p, Vec2 from) {
    double a = std::atan2(p[1], p[0]) - std::atan2(from[1], from[0]);
    while (a < 0) a += 2 * M_PI;
    while (a >= 2 * M_PI) a -= 2 * M_PI;
    return a;
  };
  const double to = turn(s, e);
  std::vector<std::pair<double, Vec2>> corners;
  for (const Vec2 c : {Vec2{b, b}, Vec2{-b, b}, Vec2{-b, -b}, Vec2{b, -b}})
    if (const double a = turn(c, e); a > 1e-12 && a < to - 1e-12) corners.push_back({a, c});
  std::sort(corners.begin(), corners.end(), [](const auto& p, const auto& q) { return p.first < q.first; });
  for (const auto& [a, c] : corners) out.push_back(c);
  return out;
}

// What stays of the cut plane and where it goes: an outline, then p -> R(turn) p + shift.
struct Piece {
  std::vector<Vec2> outline;
  double turn = 0;
  Vec2 shift{0, 0};
  bool moved() const { return turn != 0 || shift[0] != 0 || shift[1] != 0; }
  Vec2 apply(Vec2 p) const { return add(turned(p, turn), shift); }
  gp_Trsf move(const Cutter& c) const {
    gp_Trsf r, t;
    r.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(c.n)), turn);
    t.SetTranslation(gp_Vec(c.at(shift)));
    return t * r;
  }
};

// The pieces of a cutting line: one (kept_outline) or, aligned, one per segment: the strip behind it between its joints
// (the first and the last open at their ends), revolved about the joints before it until it lies along the first
// segment, so the faces of every segment come to face the viewer side by side.
std::vector<Piece> pieces_of(const std::vector<Vec2>& line, bool aligned, Vec2 removed, double b) {
  if (!aligned || line.size() < 3) return {Piece{kept_outline(line, removed, b)}};
  const Vec2 d0 = unit(sub(line[1], line[0]));
  const Vec2 keep0 = mul(left(d0), dot(left(d0), removed) > 0 ? -1 : 1);
  std::vector<Piece> out;
  double turn = 0;
  Vec2 shift{0, 0}, prev = d0;
  for (size_t i = 0; i + 1 < line.size(); ++i) {
    if (std::hypot(line[i + 1][0] - line[i][0], line[i + 1][1] - line[i][1]) < 1e-9) continue;
    const Vec2 d = unit(sub(line[i + 1], line[i]));
    if (i > 0) {  // T_i = T_(i-1) o R(P_i, phi): the segment turned about its first point onto the one before it
      const double phi = std::atan2(cross(d, prev), dot(d, prev));
      shift = add(shift, turned(sub(line[i], turned(line[i], phi)), turn));
      turn += phi;
    }
    prev = d;
    const Vec2 keep = turned(keep0, -turn);
    const Vec2 s = i == 0 ? sub(line[i], mul(d, 2 * b)) : line[i], e = i + 2 == line.size() ? add(line[i + 1], mul(d, 2 * b)) : line[i + 1];
    Piece p;
    p.outline = {s, e, add(e, mul(keep, 2 * b)), add(s, mul(keep, 2 * b))};
    p.turn = turn, p.shift = shift;
    out.push_back(std::move(p));
  }
  return out;
}

bool inside(const std::vector<Vec2>& poly, Vec2 p) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    if ((poly[i][1] > p[1]) != (poly[j][1] > p[1]) && p[0] < poly[j][0] + (poly[i][0] - poly[j][0]) * (p[1] - poly[j][1]) / (poly[i][1] - poly[j][1])) in = !in;
  return in;
}

// A polygon clipped by a convex one (Sutherland-Hodgman).
std::vector<Vec2> clip_convex(std::vector<Vec2> poly, const std::vector<Vec2>& by) {
  double area = 0;
  for (size_t i = 0; i < by.size(); ++i) area += cross(by[i], by[(i + 1) % by.size()]);
  const double s = area >= 0 ? 1 : -1;
  for (size_t k = 0; k < by.size() && !poly.empty(); ++k) {
    const Vec2 a = by[k], b = by[(k + 1) % by.size()];
    const auto side = [&](Vec2 p) { return s * cross(sub(b, a), sub(p, a)); };
    std::vector<Vec2> out;
    for (size_t i = 0; i < poly.size(); ++i) {
      const Vec2 p = poly[i], q = poly[(i + 1) % poly.size()];
      const double sp = side(p), sq = side(q);
      if (sp >= 0) out.push_back(p);
      if ((sp < 0 && sq > 0) || (sp > 0 && sq < 0)) out.push_back(add(p, mul(sub(q, p), sp / (sp - sq))));
    }
    poly.swap(out);
  }
  return poly;
}

// Whether a segment meets a box (Liang-Barsky).
bool meets(Vec2 a, Vec2 b, const std::array<double, 4>& box) {
  double t0 = 0, t1 = 1;
  const Vec2 d = sub(b, a);
  const double p[4] = {-d[0], d[0], -d[1], d[1]}, q[4] = {a[0] - box[0], box[2] - a[0], a[1] - box[1], box[3] - a[1]};
  for (int i = 0; i < 4; ++i) {
    if (std::fabs(p[i]) < 1e-15) {
      if (q[i] < 0) return false;
    } else {
      const double r = q[i] / p[i];
      if (p[i] < 0) t0 = std::max(t0, r);
      else t1 = std::min(t1, r);
      if (t0 > t1) return false;
    }
  }
  return true;
}

enum class Side { Kept, Removed, Crossed };

// A world box in the cut plane: xmin, ymin, xmax, ymax.
std::array<double, 4> plane_box(const Cutter& c, const Bnd_Box& world) {
  double lo[3], hi[3];
  world.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
  for (int k = 0; k < 8; ++k) {
    const Vec2 p = c.in_plane(gp_XYZ(k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]));
    box = {std::min(box[0], p[0]), std::min(box[1], p[1]), std::max(box[2], p[0]), std::max(box[3], p[1])};
  }
  return box;
}

Side side_of(const Cutter& c, const std::vector<Vec2>& kept, const Bnd_Box& world) {
  if (world.IsVoid()) return Side::Kept;
  std::array<double, 4> box = plane_box(c, world);
  const double pad = 1e-6 * c.reach;
  box = {box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad};
  for (size_t i = 0; i < kept.size(); ++i)
    if (meets(kept[i], kept[(i + 1) % kept.size()], box)) return Side::Crossed;
  return inside(kept, {(box[0] + box[2]) / 2, (box[1] + box[3]) / 2}) ? Side::Kept : Side::Removed;
}

struct CutBody {
  TopoDS_Shape shape;
  std::shared_ptr<const std::vector<int>> edges, faces;
  std::shared_ptr<const std::vector<int>> lies_on;  // per edge the cut made: the body's face it lies on (-1: none)
};

// The faces of the body the edges the cut made lie on: of the two faces an edge bounds, the one that was the body's.
std::shared_ptr<const std::vector<int>> lying_on(const TopoDS_Shape& shape, const std::vector<int>& edges, const std::vector<int>& faces) {
  TopTools_IndexedMapOfShape all_edges, all_faces;
  TopExp::MapShapes(shape, TopAbs_EDGE, all_edges);
  TopExp::MapShapes(shape, TopAbs_FACE, all_faces);
  TopTools_IndexedDataMapOfShapeListOfShape around;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, around);
  auto out = std::make_shared<std::vector<int>>(static_cast<size_t>(all_edges.Extent()), -1);
  for (int i = 1; i <= all_edges.Extent() && static_cast<size_t>(i - 1) < edges.size(); ++i) {
    if (edges[static_cast<size_t>(i - 1)] >= 0 || !around.Contains(all_edges(i))) continue;
    for (const auto& f : around.FindFromKey(all_edges(i)))
      if (const int at = all_faces.FindIndex(f); at > 0 && static_cast<size_t>(at - 1) < faces.size() && faces[static_cast<size_t>(at - 1)] >= 0) {
        (*out)[static_cast<size_t>(i - 1)] = faces[static_cast<size_t>(at - 1)];
        break;
      }
  }
  return out;
}

// Cuts by content (body key, placement, cut), for the process: a section and its detail view share them.
std::mutex g_cuts_mu;
std::list<std::pair<std::string, std::shared_ptr<const CutBody>>> g_cuts;

std::shared_ptr<const CutBody> remembered(const std::string& id) {
  std::lock_guard<std::mutex> lock(g_cuts_mu);
  for (auto it = g_cuts.begin(); it != g_cuts.end(); ++it)
    if (it->first == id) {
      g_cuts.splice(g_cuts.begin(), g_cuts, it);
      return it->second;
    }
  return nullptr;
}

void remember(const std::string& id, std::shared_ptr<const CutBody> cut) {
  std::lock_guard<std::mutex> lock(g_cuts_mu);
  g_cuts.emplace_front(id, std::move(cut));
  while (g_cuts.size() > 256) g_cuts.pop_back();
}

// The ordinals of `result`'s sub-shapes of a type in `original`'s (-1 for those the boolean made).
std::shared_ptr<const std::vector<int>> trace(BRepAlgoAPI_BooleanOperation& algo, const TopoDS_Shape& original, const TopoDS_Shape& result, TopAbs_ShapeEnum type) {
  TopTools_IndexedMapOfShape before, after;
  TopExp::MapShapes(original, type, before);
  TopExp::MapShapes(result, type, after);
  NCollection_DataMap<TopoDS_Shape, int, TopTools_ShapeMapHasher> from;
  for (int i = 1; i <= before.Extent(); ++i) {
    from.Bind(before(i), i - 1);
    for (const auto& m : algo.Modified(before(i))) from.Bind(m, i - 1);
  }
  auto out = std::make_shared<std::vector<int>>(static_cast<size_t>(after.Extent()), -1);
  for (int i = 1; i <= after.Extent(); ++i)
    if (const int* at = from.Seek(after(i))) (*out)[static_cast<size_t>(i - 1)] = *at;
  return out;
}

// The ordinals of a compound's sub-shapes from those of its parts (each part's map in its own ordinals).
std::shared_ptr<const std::vector<int>> combined(const TopoDS_Shape& all, const std::vector<std::pair<TopoDS_Shape, std::shared_ptr<const std::vector<int>>>>& parts,
                                                 TopAbs_ShapeEnum type) {
  NCollection_DataMap<TopoDS_Shape, int, TopTools_ShapeMapHasher> from;
  for (const auto& [shape, map] : parts) {
    TopTools_IndexedMapOfShape own;
    TopExp::MapShapes(shape, type, own);
    for (int i = 1; i <= own.Extent() && static_cast<size_t>(i - 1) < map->size(); ++i) from.Bind(own(i), (*map)[static_cast<size_t>(i - 1)]);
  }
  TopTools_IndexedMapOfShape every;
  TopExp::MapShapes(all, type, every);
  auto out = std::make_shared<std::vector<int>>(static_cast<size_t>(every.Extent()), -1);
  for (int i = 1; i <= every.Extent(); ++i)
    if (const int* at = from.Seek(every(i))) (*out)[static_cast<size_t>(i - 1)] = *at;
  return out;
}

// A face's outlines in view coordinates, each edge followed within tol.
std::vector<std::vector<Vec2>> loops_of(const TopoDS_Face& face, const View& v, double tol) {
  std::vector<std::vector<Vec2>> out;
  for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
    std::vector<Vec2> loop;
    for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) {
      const TopoDS_Edge& edge = e.Current();
      if (BRep_Tool::Degenerated(edge)) continue;
      std::vector<Vec2> pts;
      try {
        const BRepAdaptor_Curve c(edge);
        GCPnts_QuasiUniformDeflection d(c, tol, c.FirstParameter(), c.LastParameter());
        if (d.IsDone())
          for (int i = 1; i <= d.NbPoints(); ++i) pts.push_back(v.at(d.Value(i)));
      } catch (const Standard_Failure&) {
      }
      if (pts.size() < 2) continue;
      if (edge.Orientation() == TopAbs_REVERSED) std::reverse(pts.begin(), pts.end());
      for (const auto& p : pts)
        if (loop.empty() || std::hypot(p[0] - loop.back()[0], p[1] - loop.back()[1]) > 1e-9) loop.push_back(p);
    }
    if (loop.size() > 2 && std::hypot(loop.front()[0] - loop.back()[0], loop.front()[1] - loop.back()[1]) <= 1e-9) loop.pop_back();
    if (loop.size() > 2) out.push_back(std::move(loop));
  }
  return out;
}

Cutter cutter_of(const ViewSpec& spec) {
  Cutter c;
  c.x = gp_XYZ(spec.cut_x[0], spec.cut_x[1], spec.cut_x[2]).Normalized();
  c.y = gp_XYZ(spec.cut_y[0], spec.cut_y[1], spec.cut_y[2]).Normalized();
  c.n = c.x.Crossed(c.y).Normalized();
  c.line = spec.cut;
  return c;
}

// The sources' world boxes, and the largest coordinate of any of them (at least 1).
std::vector<Bnd_Box> world_boxes(const Document& doc, const std::vector<Source>& sources, double& r) {
  Bnd_Box all;
  std::vector<Bnd_Box> boxes(sources.size());
  for (size_t i = 0; i < sources.size(); ++i) {
    try {
      const Bnd_Box local = body_bbox(doc, sources[i].key);
      if (local.IsVoid()) continue;
      boxes[i] = sources[i].rigid ? local.Transformed(sources[i].trsf) : local;
      if (!sources[i].rigid) {  // a scaled placement: the corners moved
        double lo[3], hi[3];
        local.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        boxes[i].SetVoid();
        for (int k = 0; k < 8; ++k) {
          const Vec3 p = sources[i].world.apply({k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]});
          boxes[i].Add(gp_Pnt(p[0], p[1], p[2]));
        }
      }
      all.Add(boxes[i]);
    } catch (const std::exception&) {
    }
  }
  r = 1;
  if (!all.IsVoid()) {
    double lo[3], hi[3];
    all.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    for (int k = 0; k < 3; ++k) r = std::max({r, std::fabs(lo[k]), std::fabs(hi[k])});
  }
  return boxes;
}

// A cut body in the source's place: its cut shape (placed in the world) and the maps of its edges and faces.
void take_cut(Source& s, const CutBody& cut, const std::string& id) {
  s.base = s.key;
  s.key = id;
  s.proto = s.placed = cut.shape;
  s.world = Mat4::identity();
  s.rigid = true;
  s.trsf = gp_Trsf();
  s.edges = cut.edges;
  s.faces = cut.faces;
  s.lies_on = cut.lies_on;
}

// Booleans side by side contend for the heap: past about four threads they get slower, not faster (the Engine's front
// section, 104 bodies cut: 23-27 s on 16 threads, 11.5 s on 4).
int cut_threads() { return std::max(1, std::min(4, OSD_ThreadPool::DefaultPool()->NbDefaultThreadsToLaunch())); }

std::string cache_id(const std::string& cut, const Source& s) {
  std::string id = cut + "|" + s.key;
  char buf[32];
  for (double v : s.world.m) {
    std::snprintf(buf, sizeof buf, "|%.9f", std::fabs(v) < 5e-10 ? 0.0 : v);
    id += buf;
  }
  return sha256_hex(id);
}

}  // namespace

Handle(Geom2d_BSplineCurve) breakout_spline(const std::vector<Vec2>& pts) {
  std::vector<Vec2> p;
  for (const auto& q : pts)
    if (p.empty() || std::hypot(q[0] - p.back()[0], q[1] - p.back()[1]) > 1e-6) p.push_back(q);
  while (p.size() > 1 && std::hypot(p.front()[0] - p.back()[0], p.front()[1] - p.back()[1]) <= 1e-6) p.pop_back();
  if (p.size() < 3) throw Error("a broken-out section's outline needs three points apart");
  Handle(TColgp_HArray1OfPnt2d) a = new TColgp_HArray1OfPnt2d(1, static_cast<int>(p.size()));
  for (size_t i = 0; i < p.size(); ++i) a->SetValue(static_cast<int>(i + 1), gp_Pnt2d(p[i][0], p[i][1]));
  try {
    Geom2dAPI_Interpolate smooth(a, Standard_True, 1e-7);
    smooth.Perform();
    if (smooth.IsDone()) return smooth.Curve();
  } catch (const Standard_Failure&) {
  }
  throw Error("a broken-out section's outline makes no closed curve");
}

void breakout_sources(const Document& doc, const ViewSpec& spec, const View& view, std::vector<Source>& sources, Run& run,
                      std::vector<ViewGeometry::Region>& regions) {
  if (spec.breakouts.empty()) return;
  double r = 1;
  const std::vector<Bnd_Box> boxes = world_boxes(doc, sources, r);
  for (const auto& b : spec.breakouts) {
    r = std::max(r, std::fabs(b.depth));
    for (const auto& p : b.outline) r = std::max({r, std::fabs(p[0]), std::fabs(p[1])});
  }
  const double reach = 2 * r + 10;
  // The pockets: each outline's curve at its depth, swept towards the viewer past every body; their outlines sampled.
  struct Pocket {
    TopoDS_Shape solid;
    std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
    double depth = 0;
  };
  std::vector<Pocket> pockets;
  std::string cut_id = "breakout-v1";
  char buf[96];
  for (const gp_Dir* a : {&view.x, &view.y, &view.z}) {
    std::snprintf(buf, sizeof buf, "|%.9f,%.9f,%.9f", a->X(), a->Y(), a->Z());
    cut_id += buf;
  }
  for (const auto& b : spec.breakouts) {
    for (const auto& p : b.outline) {
      std::snprintf(buf, sizeof buf, "|%.9f,%.9f", p[0], p[1]);
      cut_id += buf;
    }
    std::snprintf(buf, sizeof buf, "|d%.9f", b.depth);
    cut_id += buf;
    const Handle(Geom2d_BSplineCurve) curve = breakout_spline(b.outline);
    Pocket pocket;
    pocket.depth = b.depth;
    try {
      const gp_Pln plane(gp_Ax3(gp_Pnt(view.z.XYZ() * b.depth), view.z, view.x));
      const TopoDS_Wire wire = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(GeomAPI::To3d(curve, plane)).Edge()).Wire();
      pocket.solid = BRepPrimAPI_MakePrism(BRepBuilderAPI_MakeFace(wire, Standard_True).Face(), gp_Vec(view.z.XYZ() * (reach - b.depth))).Shape();
      Bnd_Box2d box;
      BndLib_Add2dCurve::Add(curve, 0, box);
      box.Get(pocket.box[0], pocket.box[1], pocket.box[2], pocket.box[3]);
    } catch (const Standard_Failure& e) {
      throw Error(std::string("broken-out section: its outline makes no solid (") + e.GetMessageString() + ")");
    }
    if (b.depth < reach) pockets.push_back(std::move(pocket));
  }
  // The bodies a pocket's box reaches nearer than its depth are cut; the others stay as they are.
  std::vector<std::vector<size_t>> reached(sources.size());
  std::vector<size_t> crossed;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (sources[i].whole || sources[i].mesh || boxes[i].IsVoid()) continue;
    double lo[3], hi[3];
    boxes[i].Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    std::array<double, 4> rect{1e300, 1e300, -1e300, -1e300};
    double front = -1e300;
    for (int k = 0; k < 8; ++k) {
      const gp_Pnt p(k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]);
      const Vec2 q = view.at(p);
      rect = {std::min(rect[0], q[0]), std::min(rect[1], q[1]), std::max(rect[2], q[0]), std::max(rect[3], q[1])};
      front = std::max(front, view.depth(p));
    }
    for (size_t k = 0; k < pockets.size(); ++k) {
      const auto& b = pockets[k].box;
      if (front > pockets[k].depth && rect[0] <= b[2] && b[0] <= rect[2] && rect[1] <= b[3] && b[1] <= rect[3]) reached[i].push_back(k);
    }
    if (!reached[i].empty()) crossed.push_back(i);
  }
  std::vector<std::shared_ptr<const CutBody>> cuts(sources.size());
  std::vector<std::string> ids(sources.size());
  std::atomic<size_t> done{0};
  std::mutex failed_mu;
  std::string failed;
  OSD_ThreadPool::Launcher launcher(*OSD_ThreadPool::DefaultPool(), cut_threads());
  launcher.Perform(0, static_cast<int>(crossed.size()), [&](int, int k) {
    if (run.cancelled()) return;
    const size_t i = crossed[static_cast<size_t>(k)];
    const Source& s = sources[i];
    ids[i] = cache_id(cut_id, s);
    auto hit = remembered(ids[i]);
    if (!hit) {
      try {
        // The body store's shapes are read by other threads: the boolean leaves its arguments as they are.
        const TopoDS_Shape placed = s.rigid ? s.proto.Moved(TopLoc_Location(s.trsf)) : s.placed;
        TopTools_ListOfShape arguments, tools;
        arguments.Append(placed);
        for (size_t p : reached[i]) tools.Append(pockets[p].solid);
        BRepAlgoAPI_Cut cut;
        cut.SetArguments(arguments);
        cut.SetTools(tools);
        cut.SetNonDestructive(Standard_True);
        cut.Build();
        if (!cut.IsDone() || cut.HasErrors()) throw Error("the boolean failed");
        auto body = std::make_shared<CutBody>();
        body->shape = cut.Shape();
        body->edges = trace(cut, placed, body->shape, TopAbs_EDGE);
        body->faces = trace(cut, placed, body->shape, TopAbs_FACE);
        body->lies_on = lying_on(body->shape, *body->edges, *body->faces);
        hit = body;
        remember(ids[i], hit);
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(failed_mu);
        failed = s.node + ": " + e.what();
      } catch (const Standard_Failure& e) {
        std::lock_guard<std::mutex> lock(failed_mu);
        failed = s.node + ": " + e.GetMessageString();
      }
    }
    cuts[i] = hit;
    run.report(0.1 * static_cast<double>(++done) / static_cast<double>(crossed.size()), "broken-out section: cutting " + std::to_string(done.load()) + "/" + std::to_string(crossed.size()));
  });
  run.check();
  if (!failed.empty()) throw Error("broken-out section: a body could not be cut (" + failed + ")");
  std::vector<Source> out;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (cuts[i] && cuts[i]->faces->empty()) continue;  // wholly within a pocket
    Source s = std::move(sources[i]);
    if (const auto& cut = cuts[i]) {
      take_cut(s, *cut, ids[i]);
      // The pockets' floors: faces the cut made, square to the view at a pocket's depth.
      ViewGeometry::Region region;
      region.body = static_cast<int>(out.size());
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(cut->shape, TopAbs_FACE, faces);
      for (int f = 1; f <= faces.Extent(); ++f) {
        if (static_cast<size_t>(f - 1) < cut->faces->size() && (*cut->faces)[static_cast<size_t>(f - 1)] >= 0) continue;
        const TopoDS_Face& face = TopoDS::Face(faces(f));
        try {
          const BRepAdaptor_Surface surface(face, Standard_False);
          if (surface.GetType() != GeomAbs_Plane) continue;
          gp_Dir normal = surface.Plane().Axis().Direction();
          if (face.Orientation() == TopAbs_REVERSED) normal.Reverse();
          if (normal.XYZ().Dot(view.z.XYZ()) < 1 - 1e-6) continue;
          TopExp_Explorer vx(face, TopAbs_VERTEX);
          if (!vx.More()) continue;
          const double z = view.depth(BRep_Tool::Pnt(TopoDS::Vertex(vx.Current())));
          if (std::none_of(pockets.begin(), pockets.end(), [&](const Pocket& p) { return std::fabs(z - p.depth) <= 1e-6 * reach; })) continue;
          for (auto& l : loops_of(face, view, spec.tolerance)) region.loops.push_back(std::move(l));
        } catch (const Standard_Failure&) {
        }
      }
      if (!region.loops.empty()) regions.push_back(std::move(region));
    }
    out.push_back(std::move(s));
  }
  sources.swap(out);
}

void mark_breakout_curves(const ViewSpec& spec, const std::vector<Source>& sources, std::vector<Curve>& curves) {
  if (spec.breakouts.empty()) return;
  std::vector<std::vector<Vec2>> rims;
  for (const auto& b : spec.breakouts) rims.push_back(breakout_outline(b.outline, 0.002));
  const double tol = 0.02 + spec.tolerance;
  const auto on_rim = [&](Vec2 p) {
    for (const auto& rim : rims)
      for (size_t i = 0; i < rim.size(); ++i) {
        const Vec2 a = rim[i], d = sub(rim[(i + 1) % rim.size()], a);
        const double t = std::clamp(dot(sub(p, a), d) / std::max(dot(d, d), 1e-30), 0.0, 1.0);
        if (std::hypot(p[0] - a[0] - t * d[0], p[1] - a[1] - t * d[1]) <= tol) return true;
      }
    return false;
  };
  // What the cut made along the outlines (the pockets' walls, seen edge on): the break line where it lies over a body,
  // visible; behind something, left out.
  std::vector<Curve> kept;
  for (auto& k : curves) {
    const bool made = k.edge < 0 && k.kind != Curve::Kind::Silhouette && k.body >= 0 && static_cast<size_t>(k.body) < sources.size() && sources[static_cast<size_t>(k.body)].edges;
    if (made) {
      const auto pts = k.sample(std::max(spec.tolerance, 1e-3));
      if (!pts.empty() && std::all_of(pts.begin(), pts.end(), on_rim)) {
        if (k.hidden) continue;
        k.kind = Curve::Kind::Break;
      }
    }
    kept.push_back(std::move(k));
  }
  curves.swap(kept);
}

void cut_sources(const Document& doc, const ViewSpec& spec, const View& view, std::vector<Source>& sources, Run& run,
                 std::vector<ViewGeometry::Region>& regions) {
  if (spec.cut.size() < 2) return;
  Cutter c = cutter_of(spec);
  // Large enough to hold every body and the line, small enough for the booleans' tolerances.
  double r = 1;
  const std::vector<Bnd_Box> boxes = world_boxes(doc, sources, r);
  for (const auto& p : c.line) r = std::max({r, std::fabs(p[0]), std::fabs(p[1])});
  c.reach = 2 * r + 10;
  const Vec2 removed = unit({view.z.XYZ().Dot(c.x), view.z.XYZ().Dot(c.y)});
  const bool aligned = spec.aligned && c.line.size() >= 3;
  const std::vector<Piece> pieces = pieces_of(c.line, aligned, removed, c.reach);
  // The faces hatched: on the segments parallel to the first one (offset and half sections), on the first one's line
  // (aligned: every segment revolved onto it).
  const Vec2 t0 = unit(sub(c.line[1], c.line[0]));
  std::vector<std::array<Vec2, 2>> hatched;
  for (size_t i = 1; i < c.line.size(); ++i) {
    const Vec2 d = sub(c.line[i], c.line[i - 1]);
    if (aligned || (std::hypot(d[0], d[1]) > 1e-9 && std::fabs(cross(unit(d), t0)) < 1e-9)) hatched.push_back({c.line[i - 1], c.line[i]});
    if (aligned) break;
  }
  // The swept outlines as solids, from -reach to +reach along the normal.
  std::vector<TopoDS_Shape> tools;
  std::vector<gp_Trsf> moves;
  try {
    for (const auto& piece : pieces) {
      BRepBuilderAPI_MakePolygon poly;
      for (const auto& p : piece.outline) poly.Add(gp_Pnt(c.at(p) - c.n * c.reach));
      poly.Close();
      const TopoDS_Face base = BRepBuilderAPI_MakeFace(poly.Wire(), Standard_True).Face();
      tools.push_back(BRepPrimAPI_MakePrism(base, gp_Vec(c.n * (2 * c.reach))).Shape());
      moves.push_back(piece.move(c));
    }
  } catch (const Standard_Failure& e) {
    throw Error(std::string("section: the cutting line makes no solid (") + e.GetMessageString() + ")");
  }
  std::string cut_id = "section-v1";
  {
    char buf[96];
    for (const gp_XYZ* a : {&c.x, &c.y, &c.n}) {
      std::snprintf(buf, sizeof buf, "|%.9f,%.9f,%.9f", a->X(), a->Y(), a->Z());
      cut_id += buf;
    }
    for (const auto& p : c.line) {
      std::snprintf(buf, sizeof buf, "|%.9f,%.9f", p[0], p[1]);
      cut_id += buf;
    }
    std::snprintf(buf, sizeof buf, "|%.9f,%.9f", removed[0], removed[1]);
    cut_id += buf;
    if (aligned) cut_id += "|aligned";
  }
  // Each body: dropped (no piece holds any of it), kept whole in the one piece that holds all of it (moved with it), or
  // cut by the pieces it reaches. Bodies drawn whole and meshes go whole with the piece their middle is in.
  std::vector<Side> sides(sources.size(), Side::Kept);
  std::vector<std::vector<size_t>> reached(sources.size());
  std::vector<size_t> crossed;
  for (size_t i = 0; i < sources.size(); ++i) {
    std::vector<Side> per(pieces.size());
    for (size_t k = 0; k < pieces.size(); ++k)
      if ((per[k] = side_of(c, pieces[k].outline, boxes[i])) != Side::Removed) reached[i].push_back(k);
    if (reached[i].empty()) {
      sides[i] = Side::Removed;
    } else if (reached[i].size() == 1 && per[reached[i][0]] == Side::Kept) {
      sides[i] = Side::Kept;
    } else if (sources[i].whole || sources[i].mesh) {
      sides[i] = Side::Kept;
      size_t home = reached[i][0];
      if (!boxes[i].IsVoid()) {
        const auto b = plane_box(c, boxes[i]);
        for (size_t k : reached[i])
          if (inside(pieces[k].outline, {(b[0] + b[2]) / 2, (b[1] + b[3]) / 2})) {
            home = k;
            break;
          }
      }
      reached[i] = {home};
    } else {
      sides[i] = Side::Crossed;
      crossed.push_back(i);
    }
  }
  // The biggest first, so the last to finish is a small one.
  {
    std::vector<std::pair<int, size_t>> sized;
    for (size_t i : crossed) {
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(sources[i].proto, TopAbs_FACE, faces);
      sized.push_back({faces.Extent(), i});
    }
    std::stable_sort(sized.begin(), sized.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t k = 0; k < sized.size(); ++k) crossed[k] = sized[k].second;
  }
  std::vector<std::shared_ptr<const CutBody>> cuts(sources.size());
  std::vector<std::string> ids(sources.size());
  std::atomic<size_t> done{0};
  std::mutex failed_mu;
  std::string failed;
  OSD_ThreadPool::Launcher launcher(*OSD_ThreadPool::DefaultPool(), cut_threads());
  launcher.Perform(0, static_cast<int>(crossed.size()), [&](int, int k) {
    if (run.cancelled()) return;
    const size_t i = crossed[static_cast<size_t>(k)];
    const Source& s = sources[i];
    const std::string id = cache_id(cut_id, s);
    ids[i] = id;
    auto hit = remembered(id);
    if (!hit) {
      try {
        auto body = std::make_shared<CutBody>();
        std::vector<std::pair<TopoDS_Shape, std::shared_ptr<const std::vector<int>>>> edge_parts, face_parts;
        for (size_t piece : reached[i]) {
          // The body store's shapes are read by other threads: the boolean leaves its arguments as they are.
          const TopoDS_Shape placed = s.rigid ? s.proto.Moved(TopLoc_Location(s.trsf)) : s.placed;
          BRepAlgoAPI_Common common;
          TopTools_ListOfShape arguments, tool;
          arguments.Append(placed);
          tool.Append(tools[piece]);
          common.SetArguments(arguments);
          common.SetTools(tool);
          common.SetNonDestructive(Standard_True);
          common.Build();
          if (!common.IsDone() || common.HasErrors()) throw Error("the boolean failed");
          TopoDS_Shape part = common.Shape();
          auto edges = trace(common, placed, part, TopAbs_EDGE), faces = trace(common, placed, part, TopAbs_FACE);
          if (faces->empty()) continue;
          if (pieces[piece].moved()) part = part.Moved(TopLoc_Location(moves[piece]));
          edge_parts.push_back({part, edges});
          face_parts.push_back({part, faces});
        }
        if (edge_parts.size() == 1 && !pieces[reached[i][0]].moved()) {
          body->shape = edge_parts[0].first;
          body->edges = edge_parts[0].second;
          body->faces = face_parts[0].second;
        } else {
          BRep_Builder b;
          TopoDS_Compound parts;
          b.MakeCompound(parts);
          for (const auto& [part, map] : edge_parts) b.Add(parts, part);
          body->shape = parts;
          body->edges = combined(parts, edge_parts, TopAbs_EDGE);
          body->faces = combined(parts, face_parts, TopAbs_FACE);
        }
        body->lies_on = lying_on(body->shape, *body->edges, *body->faces);
        hit = body;
        remember(id, hit);
      } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(failed_mu);
        failed = s.node + ": " + e.what();
      } catch (const Standard_Failure& e) {
        std::lock_guard<std::mutex> lock(failed_mu);
        failed = s.node + ": " + e.GetMessageString();
      }
    }
    cuts[i] = hit;
    run.report(0.1 * static_cast<double>(++done) / static_cast<double>(crossed.size()), "section: cutting " + std::to_string(done.load()) + "/" + std::to_string(crossed.size()));
  });
  run.check();
  if (!failed.empty()) throw Error("section: a body could not be cut (" + failed + ")");
  std::vector<Source> out;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (sides[i] == Side::Removed || (cuts[i] && cuts[i]->faces->empty())) continue;  // nothing left of it
    Source s = std::move(sources[i]);
    if (sides[i] == Side::Kept && !reached[i].empty() && pieces[reached[i][0]].moved()) {  // whole, revolved with its piece
      const gp_Trsf& m = moves[reached[i][0]];
      s.world = mat_from_trsf(m) * s.world;
      if (s.rigid) s.trsf = m * s.trsf;
      s.placed = s.placed.Moved(TopLoc_Location(m));
    }
    if (const auto& cut = cuts[i]) {
      take_cut(s, *cut, ids[i]);
      // The faces the cut left facing the viewer: flat, square to the view, on one of the hatched lines.
      ViewGeometry::Region region;
      region.body = static_cast<int>(out.size());
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(cut->shape, TopAbs_FACE, faces);
      for (int f = 1; f <= faces.Extent(); ++f) {
        if (static_cast<size_t>(f - 1) < cut->faces->size() && (*cut->faces)[static_cast<size_t>(f - 1)] >= 0) continue;  // one of the body's own
        const TopoDS_Face& face = TopoDS::Face(faces(f));
        try {
          const BRepAdaptor_Surface surface(face, Standard_False);
          if (surface.GetType() != GeomAbs_Plane) continue;
          gp_Dir normal = surface.Plane().Axis().Direction();
          if (face.Orientation() == TopAbs_REVERSED) normal.Reverse();
          if (normal.XYZ().Dot(view.z.XYZ()) < 1 - 1e-6) continue;
          TopExp_Explorer vx(face, TopAbs_VERTEX);
          if (!vx.More()) continue;
          const Vec2 p = c.in_plane(BRep_Tool::Pnt(TopoDS::Vertex(vx.Current())).XYZ());
          const bool on = std::any_of(hatched.begin(), hatched.end(), [&](const std::array<Vec2, 2>& h) {
            return std::fabs(cross(unit(sub(h[1], h[0])), sub(p, h[0]))) <= 1e-6 * c.reach;
          });
          if (!on) continue;
          for (auto& l : loops_of(face, view, spec.tolerance)) region.loops.push_back(std::move(l));
        } catch (const Standard_Failure&) {
        }
      }
      if (!region.loops.empty()) regions.push_back(std::move(region));
    }
    out.push_back(std::move(s));
  }
  sources.swap(out);
}

void name_cut_curves(const std::vector<Source>& sources, std::vector<Curve>& curves) {
  for (auto& k : curves) {
    if (k.body < 0 || static_cast<size_t>(k.body) >= sources.size()) continue;
    const Source& s = sources[static_cast<size_t>(k.body)];
    const auto rename = [](int& ordinal, const std::shared_ptr<const std::vector<int>>& map) {
      if (!map || ordinal < 0) return;
      ordinal = static_cast<size_t>(ordinal) < map->size() ? (*map)[static_cast<size_t>(ordinal)] : -1;
    };
    const int cut_edge = k.edge;
    rename(k.edge, s.edges);
    rename(k.face, s.faces);
    // An edge the cut made, named after the face of the body it lies on (a section's outline: dimensioned through it).
    if (k.edge < 0 && cut_edge >= 0 && k.face < 0 && k.kind != Curve::Kind::Silhouette && s.lies_on && static_cast<size_t>(cut_edge) < s.lies_on->size())
      k.face = (*s.lies_on)[static_cast<size_t>(cut_edge)];
  }
}

void drop_joint_curves(const ViewSpec& spec, const View& view, const std::vector<Source>& sources, std::vector<Curve>& curves) {
  if (!spec.aligned || spec.cut.size() < 3) return;
  const Cutter c = cutter_of(spec);
  const Vec2 d0 = unit(sub(c.line[1], c.line[0]));
  const auto pieces = pieces_of(c.line, true, left(d0), 1);
  // Where the pieces meet once revolved: planes square to the first segment through the joints, seen edge on.
  const gp_XYZ along = c.at(d0);
  const double a = along.Dot(view.x.XYZ()), b = along.Dot(view.y.XYZ());
  std::vector<double> at;
  for (size_t i = 1; i < pieces.size(); ++i) at.push_back(dot(pieces[i].apply(pieces[i].outline[0]), d0));
  if (at.empty() || std::hypot(a, b) < 1e-9) return;
  const double tol = 1e-4;
  curves.erase(std::remove_if(curves.begin(), curves.end(), [&](const Curve& k) {
                 if (k.body < 0 || static_cast<size_t>(k.body) >= sources.size() || !sources[static_cast<size_t>(k.body)].edges) return false;
                 const auto pts = k.sample(std::max(spec.tolerance, 1e-3));
                 return std::any_of(at.begin(), at.end(), [&](double v) {
                   return !pts.empty() && std::all_of(pts.begin(), pts.end(), [&](const Vec2& p) { return std::fabs(a * p[0] + b * p[1] - v) <= tol; });
                 });
               }),
               curves.end());
}

namespace {
// Bodies' tight boxes turned with an aligned section's pieces (key, placement, turn), for the process.
std::mutex g_turned_mu;
std::map<std::string, std::array<double, 6>> g_turned;
}  // namespace

std::array<double, 4> aligned_extent(const Document& doc, const Scene& scene, const ViewSpec& spec) {
  const Cutter c = cutter_of(spec);
  Vec3 vx, vy, vz;
  view_axes(spec, vx, vy, vz);
  const auto bodies = view_bodies(scene, spec);
  std::vector<std::string> keys;
  for (const auto& [node, world] : bodies)
    if (const Node* n = scene.node(node)) keys.push_back(n->body_key);
  warm_tight_bboxes(doc, keys);
  double r = 1;
  for (const auto& p : c.line) r = std::max({r, std::fabs(p[0]), std::fabs(p[1])});
  std::vector<std::array<gp_XYZ, 8>> corners;  // the bodies' world boxes (explode offsets added)
  for (const auto& [node, world] : bodies) {
    const Bnd_Box b = node_tight_bbox(doc, scene, node, false);
    std::array<gp_XYZ, 8> k{};
    if (!b.IsVoid()) {
      const Mat4 placed = scene.world(node);
      double lo[3], hi[3];
      b.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
      const gp_XYZ shift(world.at(0, 3) - placed.at(0, 3), world.at(1, 3) - placed.at(1, 3), world.at(2, 3) - placed.at(2, 3));
      for (int i = 0; i < 8; ++i) {
        k[static_cast<size_t>(i)] = gp_XYZ(i & 1 ? hi[0] : lo[0], i & 2 ? hi[1] : lo[1], i & 4 ? hi[2] : lo[2]) + shift;
        r = std::max({r, std::fabs(k[static_cast<size_t>(i)].X()), std::fabs(k[static_cast<size_t>(i)].Y()), std::fabs(k[static_cast<size_t>(i)].Z())});
      }
    }
    corners.push_back(k);
  }
  const gp_XYZ z(vz[0], vz[1], vz[2]);
  const auto pieces = pieces_of(c.line, true, unit({z.Dot(c.x), z.Dot(c.y)}), 2 * r + 10);
  // A small model (the exact tier's): each body measured turned with each piece, so a round part stays round; else its box's
  // corners turned (up to a few tenths too big where a piece turns far).
  bool small = false;
  {
    ViewSpec probe = spec;
    probe.quality = Quality::Auto;
    try {
      small = choose_tier(doc, scene, probe) == Quality::Exact;
    } catch (const std::exception&) {
    }
  }
  std::vector<std::array<double, 6>> boxes(bodies.size() * pieces.size(), {1, 1, 1, -1, -1, -1});  // in-plane u, v, n
  if (small) {
    std::vector<std::string> ids(boxes.size());
    std::vector<size_t> missing;
    for (size_t i = 0; i < bodies.size(); ++i) {
      const Node* n = scene.node(bodies[i].first);
      if (!n || !mat_is_rigid(bodies[i].second)) continue;
      for (size_t k = 0; k < pieces.size(); ++k) {
        std::string id = n->body_key;
        char buf[40];
        for (double v : bodies[i].second.m) std::snprintf(buf, sizeof buf, "|%.9f", std::fabs(v) < 5e-10 ? 0.0 : v), id += buf;
        for (const gp_XYZ* a : {&c.x, &c.y}) std::snprintf(buf, sizeof buf, "|%.9f,%.9f,%.9f", a->X(), a->Y(), a->Z()), id += buf;
        std::snprintf(buf, sizeof buf, "|%.12f", pieces[k].turn), id += buf;
        ids[i * pieces.size() + k] = id;
        std::lock_guard<std::mutex> lock(g_turned_mu);
        if (const auto it = g_turned.find(id); it != g_turned.end()) boxes[i * pieces.size() + k] = it->second;
        else missing.push_back(i * pieces.size() + k);
      }
    }
    OSD_Parallel::For(0, static_cast<int>(missing.size()), [&](int m) {
      const size_t at = missing[static_cast<size_t>(m)], i = at / pieces.size(), k = at % pieces.size();
      try {
        gp_Trsf turn;
        turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(c.n)), pieces[k].turn);
        const Bnd_Box b = tight_bbox(body_shape(doc, scene.node(bodies[i].first)->body_key).Moved(TopLoc_Location(turn * trsf_from_mat(bodies[i].second))));
        if (b.IsVoid()) return;
        double lo[3], hi[3];
        b.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
        std::array<double, 6> e{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
        for (int q = 0; q < 8; ++q) {
          const gp_XYZ p(q & 1 ? hi[0] : lo[0], q & 2 ? hi[1] : lo[1], q & 4 ? hi[2] : lo[2]);
          const double w[3] = {p.Dot(c.x), p.Dot(c.y), p.Dot(c.n)};
          for (int d = 0; d < 3; ++d) e[static_cast<size_t>(d)] = std::min(e[static_cast<size_t>(d)], w[d]), e[static_cast<size_t>(d + 3)] = std::max(e[static_cast<size_t>(d + 3)], w[d]);
        }
        boxes[at] = e;
      } catch (const std::exception&) {
      } catch (const Standard_Failure&) {
      }
    });
    std::lock_guard<std::mutex> lock(g_turned_mu);
    if (g_turned.size() > 20000) g_turned.clear();
    for (size_t at : missing) g_turned[ids[at]] = boxes[at];
  }
  // Each body's box in each piece's developed frame, cut to the piece (developed too), in the view.
  std::array<double, 4> e{1e300, 1e300, -1e300, -1e300};
  for (size_t i = 0; i < bodies.size(); ++i)
    for (size_t k = 0; k < pieces.size(); ++k) {
      const Piece& piece = pieces[k];
      std::array<double, 6> b = boxes[i * pieces.size() + k];
      if (b[0] > b[3]) {  // the box's corners, turned
        b = {1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
        for (const gp_XYZ& p : corners[i]) {
          const Vec2 q = turned(c.in_plane(p), piece.turn);
          const double w[3] = {q[0], q[1], p.Dot(c.n)};
          for (int d = 0; d < 3; ++d) b[static_cast<size_t>(d)] = std::min(b[static_cast<size_t>(d)], w[d]), b[static_cast<size_t>(d + 3)] = std::max(b[static_cast<size_t>(d + 3)], w[d]);
        }
        if (b[0] > b[3]) continue;
      }
      const Vec2 s = piece.shift;
      std::vector<Vec2> outline;
      for (const auto& p : piece.outline) outline.push_back(piece.apply(p));
      for (const Vec2& q : clip_convex({{b[0] + s[0], b[1] + s[1]}, {b[3] + s[0], b[1] + s[1]}, {b[3] + s[0], b[4] + s[1]}, {b[0] + s[0], b[4] + s[1]}}, outline))
        for (double w : {b[2], b[5]}) {
          const gp_XYZ p = c.at(q) + c.n * w;
          const double u = p.X() * vx[0] + p.Y() * vx[1] + p.Z() * vx[2], v = p.X() * vy[0] + p.Y() * vy[1] + p.Z() * vy[2];
          e = {std::min(e[0], u), std::min(e[1], v), std::max(e[2], u), std::max(e[3], v)};
        }
    }
  if (e[0] > e[2]) e = {0, 0, 0, 0};
  return e;
}

}  // namespace opad::drawing::detail

namespace opad::drawing {

std::vector<Vec2> breakout_outline(const std::vector<Vec2>& points, double tol) {
  const Handle(Geom2d_BSplineCurve) c = detail::breakout_spline(points);
  const Geom2dAdaptor_Curve a(c);
  GCPnts_QuasiUniformDeflection d(a, std::max(tol, 1e-6), a.FirstParameter(), a.LastParameter());
  std::vector<Vec2> out;
  if (d.IsDone())
    for (int i = 1; i < d.NbPoints(); ++i) out.push_back({d.Value(i).X(), d.Value(i).Y()});  // closed: the last is the first
  return out;
}

}  // namespace opad::drawing
