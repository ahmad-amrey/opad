// Section views (TODO 11 UI-82): the bodies a cutting line crosses, cut where it is swept through the model (what lies
// towards the viewer taken away), and the faces the cut leaves facing the viewer, for hatching. Bodies wholly on one side
// are kept or dropped by their boxes, without a boolean; each cut is cached by body key, placement and cut.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <NCollection_DataMap.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <list>
#include <mutex>

#include "opad/geometry.hpp"
#include "projection_internal.hpp"

namespace opad::drawing::detail {
namespace {

Vec2 sub(Vec2 a, Vec2 b) { return {a[0] - b[0], a[1] - b[1]}; }
Vec2 add(Vec2 a, Vec2 b) { return {a[0] + b[0], a[1] + b[1]}; }
Vec2 mul(Vec2 a, double s) { return {a[0] * s, a[1] * s}; }
double dot(Vec2 a, Vec2 b) { return a[0] * b[0] + a[1] * b[1]; }
double cross(Vec2 a, Vec2 b) { return a[0] * b[1] - a[1] * b[0]; }
Vec2 unit(Vec2 a) {
  const double l = std::hypot(a[0], a[1]);
  return l > 0 ? mul(a, 1 / l) : a;
}

struct Cutter {
  gp_XYZ x, y, n;                         // the cut plane's axes and its normal (the sweep)
  std::vector<Vec2> line;                 // as given
  std::vector<Vec2> kept;                 // the outline of what stays, in the plane
  std::vector<std::array<Vec2, 2>> hatched;  // the lines of the segments parallel to the first one (their faces face the viewer)
  double reach = 1;
  Vec2 in_plane(const gp_XYZ& p) const { return {p.Dot(x), p.Dot(y)}; }
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

bool inside(const std::vector<Vec2>& poly, Vec2 p) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    if ((poly[i][1] > p[1]) != (poly[j][1] > p[1]) && p[0] < poly[j][0] + (poly[i][0] - poly[j][0]) * (p[1] - poly[j][1]) / (poly[i][1] - poly[j][1])) in = !in;
  return in;
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

Side side_of(const Cutter& c, const Bnd_Box& world) {
  if (world.IsVoid()) return Side::Kept;
  double lo[3], hi[3];
  world.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
  std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
  for (int k = 0; k < 8; ++k) {
    const Vec2 p = c.in_plane(gp_XYZ(k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]));
    box = {std::min(box[0], p[0]), std::min(box[1], p[1]), std::max(box[2], p[0]), std::max(box[3], p[1])};
  }
  const double pad = 1e-6 * c.reach;
  box = {box[0] - pad, box[1] - pad, box[2] + pad, box[3] + pad};
  for (size_t i = 1; i < c.kept.size(); ++i)
    if (meets(c.kept[i - 1], c.kept[i], box)) return Side::Crossed;
  return inside(c.kept, {(box[0] + box[2]) / 2, (box[1] + box[3]) / 2}) ? Side::Kept : Side::Removed;
}

struct CutBody {
  TopoDS_Shape shape;
  std::shared_ptr<const std::vector<int>> edges, faces;
};

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
std::shared_ptr<const std::vector<int>> trace(BRepAlgoAPI_Common& algo, const TopoDS_Shape& original, const TopoDS_Shape& result, TopAbs_ShapeEnum type) {
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

}  // namespace

void cut_sources(const Document& doc, const ViewSpec& spec, const View& view, std::vector<Source>& sources, Run& run,
                 std::vector<ViewGeometry::Region>& regions) {
  if (spec.cut.size() < 2) return;
  Cutter c;
  c.x = gp_XYZ(spec.cut_x[0], spec.cut_x[1], spec.cut_x[2]).Normalized();
  c.y = gp_XYZ(spec.cut_y[0], spec.cut_y[1], spec.cut_y[2]).Normalized();
  c.n = c.x.Crossed(c.y).Normalized();
  c.line = spec.cut;
  // Large enough to hold every body and the line, small enough for the booleans' tolerances.
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
  double r = 1;
  if (!all.IsVoid()) {
    double lo[3], hi[3];
    all.Get(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
    for (int k = 0; k < 3; ++k) r = std::max({r, std::fabs(lo[k]), std::fabs(hi[k])});
  }
  for (const auto& p : c.line) r = std::max({r, std::fabs(p[0]), std::fabs(p[1])});
  c.reach = 2 * r + 10;
  const Vec2 removed{view.z.XYZ().Dot(c.x), view.z.XYZ().Dot(c.y)};
  c.kept = kept_outline(c.line, unit(removed), c.reach);
  const Vec2 t0 = unit(sub(c.line[1], c.line[0]));
  for (size_t i = 1; i < c.line.size(); ++i) {
    const Vec2 d = sub(c.line[i], c.line[i - 1]);
    if (std::hypot(d[0], d[1]) > 1e-9 && std::fabs(cross(unit(d), t0)) < 1e-9) c.hatched.push_back({c.line[i - 1], c.line[i]});
  }
  // The swept outline as a solid, from -reach to +reach along the normal.
  TopoDS_Shape tool;
  try {
    BRepBuilderAPI_MakePolygon poly;
    for (const auto& p : c.kept) poly.Add(gp_Pnt(c.x * p[0] + c.y * p[1] - c.n * c.reach));
    poly.Close();
    const TopoDS_Face base = BRepBuilderAPI_MakeFace(poly.Wire(), Standard_True).Face();
    tool = BRepPrimAPI_MakePrism(base, gp_Vec(c.n * (2 * c.reach))).Shape();
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
    std::snprintf(buf, sizeof buf, "|%.9f,%.9f", unit(removed)[0], unit(removed)[1]);
    cut_id += buf;
  }
  std::vector<Side> sides(sources.size(), Side::Kept);
  std::vector<size_t> crossed;
  for (size_t i = 0; i < sources.size(); ++i) {
    sides[i] = side_of(c, boxes[i]);
    if (sides[i] == Side::Crossed && !sources[i].whole && !sources[i].mesh) crossed.push_back(i);
    else if (sides[i] == Side::Crossed) sides[i] = Side::Kept;  // shown whole
  }
  std::vector<std::shared_ptr<const CutBody>> cuts(sources.size());
  std::vector<std::string> ids(sources.size());
  std::atomic<size_t> done{0};
  std::mutex failed_mu;
  std::string failed;
  OSD_Parallel::For(0, static_cast<int>(crossed.size()), [&](int k) {
    if (run.cancelled()) return;
    const size_t i = crossed[static_cast<size_t>(k)];
    const Source& s = sources[i];
    std::string id = cut_id + "|" + s.key;
    char buf[32];
    for (double v : s.world.m) {
      std::snprintf(buf, sizeof buf, "|%.9f", std::fabs(v) < 5e-10 ? 0.0 : v);
      id += buf;
    }
    id = sha256_hex(id);
    ids[i] = id;
    auto hit = remembered(id);
    if (!hit) {
      try {
        // Copies: a boolean writes into its arguments, and the body store's shapes are read by other threads.
        const TopoDS_Shape copy = BRepBuilderAPI_Copy(s.proto, Standard_True, Standard_False).Shape();
        TopoDS_Shape placed = copy.Moved(TopLoc_Location(s.trsf));
        if (!s.rigid) placed = s.placed;  // a scaled body: its transformed copy (made for this view alone)
        BRepAlgoAPI_Common common(placed, BRepBuilderAPI_Copy(tool, Standard_True, Standard_False).Shape());
        if (!common.IsDone() || common.HasErrors()) throw Error("the boolean failed");
        auto body = std::make_shared<CutBody>();
        body->shape = common.Shape();
        body->edges = trace(common, placed, body->shape, TopAbs_EDGE);
        body->faces = trace(common, placed, body->shape, TopAbs_FACE);
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
    if (const auto& cut = cuts[i]) {
      s.base = s.key;
      s.key = ids[i];
      s.proto = s.placed = cut->shape;
      s.world = Mat4::identity();
      s.rigid = true;
      s.trsf = gp_Trsf();
      s.edges = cut->edges;
      s.faces = cut->faces;
      // The faces the cut left facing the viewer: flat, square to the view, on one of the hatched lines.
      ViewGeometry::Region region;
      region.body = static_cast<int>(out.size());
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(cut->shape, TopAbs_FACE, faces);
      for (int f = 1; f <= faces.Extent(); ++f) {
        if ((*cut->faces)[static_cast<size_t>(f - 1)] >= 0) continue;  // one of the body's own
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
          const bool on = std::any_of(c.hatched.begin(), c.hatched.end(), [&](const std::array<Vec2, 2>& h) {
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
    rename(k.edge, s.edges);
    rename(k.face, s.faces);
  }
}

}  // namespace opad::drawing::detail
