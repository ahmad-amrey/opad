// Hybrid hidden lines (TODO 11 UI-77): the exact B-rep curves of every body, their visibility read from a depth buffer
// of the bodies' meshes. Each edge is sampled every pixel against the nearest surfaces around the sample; run
// boundaries are refined by bisection and the analytic curve is cut there, so circles stay arcs. Silhouettes are
// analytic on cylinders, cones, spheres and tori and come from the mesh normals elsewhere. Exact HLR on the Hydrostatic
// (13,291 faces) took 94 s for a front view; this takes about a second.
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <ElSLib.hxx>
#include <Geom2d_Curve.hxx>
#include <GeomAdaptor_Curve.hxx>
#include <Geom_Circle.hxx>
#include <Geom_Line.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Cone.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>
#include <gp_Pln.hxx>
#include <gp_Sphere.hxx>
#include <gp_Torus.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <list>
#include <unordered_map>

#include "opad/cache.hpp"
#include "opad/geometry.hpp"
#include "projection_internal.hpp"

namespace opad::drawing::detail {
namespace {

constexpr double kTangentSin = 0.0087;  // faces meeting within half a degree: a tangent edge

enum class Surface : uint8_t { Plane, Cylinder, Cone, Sphere, Torus, Other };

struct EdgeInfo {
  bool skip = false;  // degenerate
  Curve::Kind kind = Curve::Kind::Sharp;
  int f1 = -1, f2 = -1;  // face ordinals on either side
  gp_Dir normal;         // smooth edges: a face normal at their middle; across the view, the edge is a silhouette
};

// Per body key, shared by every node showing it.
struct KeyInfo {
  TopTools_IndexedMapOfShape edges, faces;
  std::vector<EdgeInfo> e;
  std::vector<Surface> f;
  std::vector<int> range;  // face ordinal -> index into mesh->faces, -1 when unmeshed
  std::vector<std::array<int32_t, 3>> next;  // per mesh triangle: the triangles across its sides (Tri::next), mesh-local
  std::shared_ptr<const Mesh> mesh;
  double defl = 0.1;
};

bool normal_at(const TopoDS_Face& face, const TopoDS_Edge& edge, double t, gp_Dir& out) {
  double f, l;
  const Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(edge, face, f, l);
  if (pc.IsNull()) return false;
  const gp_Pnt2d uv = pc->Value(t);
  BRepAdaptor_Surface s(face, Standard_False);
  gp_Pnt p;
  gp_Vec du, dv;
  s.D1(uv.X(), uv.Y(), p, du, dv);
  const gp_Vec n = du.Crossed(dv);
  if (n.Magnitude() < 1e-12) return false;
  out = gp_Dir(n);
  return true;
}

void classify(const TopoDS_Shape& proto, KeyInfo& k) {
  TopExp::MapShapes(proto, TopAbs_EDGE, k.edges);
  TopExp::MapShapes(proto, TopAbs_FACE, k.faces);
  TopTools_IndexedDataMapOfShapeListOfShape around;
  TopExp::MapShapesAndAncestors(proto, TopAbs_EDGE, TopAbs_FACE, around);
  k.e.resize(static_cast<size_t>(k.edges.Extent()));
  for (int i = 1; i <= k.edges.Extent(); ++i) {
    const TopoDS_Edge& edge = TopoDS::Edge(k.edges(i));
    EdgeInfo& info = k.e[static_cast<size_t>(i - 1)];
    if (BRep_Tool::Degenerated(edge)) {
      info.skip = true;
      continue;
    }
    std::vector<TopoDS_Face> faces;
    if (const TopTools_ListOfShape* list = around.Seek(edge))
      for (const auto& f : *list)
        if (std::none_of(faces.begin(), faces.end(), [&](const TopoDS_Face& g) { return g.IsSame(f); })) faces.push_back(TopoDS::Face(f));
    if (!faces.empty()) info.f1 = k.faces.FindIndex(faces[0]) - 1;
    if (faces.size() > 1) info.f2 = k.faces.FindIndex(faces[1]) - 1;
    double a, b;
    BRep_Tool::Range(edge, a, b);
    const double mid = 0.5 * (a + b);
    if (faces.size() == 1 && BRep_Tool::IsClosed(edge, faces[0])) {
      info.kind = Curve::Kind::Seam;
    } else if (faces.size() == 2) {
      TopLoc_Location l1, l2;
      if (BRep_Tool::Surface(faces[0], l1) == BRep_Tool::Surface(faces[1], l2) && l1 == l2) {
        info.kind = Curve::Kind::Seam;  // one surface split in two faces
      } else {
        bool tangent = true;
        for (double s : {0.2, 0.5, 0.8}) {
          gp_Dir n1, n2;
          const double t = a + (b - a) * s;
          if (!normal_at(faces[0], edge, t, n1) || !normal_at(faces[1], edge, t, n2) || gp_Vec(n1).Crossed(gp_Vec(n2)).Magnitude() > kTangentSin) {
            tangent = false;
            break;
          }
        }
        if (tangent) info.kind = Curve::Kind::Tangent;
      }
    }
    if (info.kind != Curve::Kind::Sharp && !normal_at(faces[0], edge, mid, info.normal)) info.kind = Curve::Kind::Sharp;
  }
  k.f.resize(static_cast<size_t>(k.faces.Extent()), Surface::Other);
  for (int i = 1; i <= k.faces.Extent(); ++i) {
    switch (BRepAdaptor_Surface(TopoDS::Face(k.faces(i)), Standard_False).GetType()) {
      case GeomAbs_Plane: k.f[static_cast<size_t>(i - 1)] = Surface::Plane; break;
      case GeomAbs_Cylinder: k.f[static_cast<size_t>(i - 1)] = Surface::Cylinder; break;
      case GeomAbs_Cone: k.f[static_cast<size_t>(i - 1)] = Surface::Cone; break;
      case GeomAbs_Sphere: k.f[static_cast<size_t>(i - 1)] = Surface::Sphere; break;
      case GeomAbs_Torus: k.f[static_cast<size_t>(i - 1)] = Surface::Torus; break;
      default: break;
    }
  }
}

// ---- the depth buffer: pixel (i, j) has its centre at u0 + (i + 0.5) px, v0 + (j + 0.5) px and keeps the nearest
// triangle there. Depth grows towards the viewer and is kept relative to `mid` so floats keep their precision.

struct Context;
bool covers(const Context& cx, int owner, double u, double v);  // the planar face's exact outline holds (u, v)

struct Tri {
  uint32_t a, b, c;
  int32_t owner;               // global face id
  std::array<int32_t, 3> next;  // the triangles across sides ab, bc, ca within the same face; -1 on its boundary
};

struct Depth {
  int W = 0, H = 0;
  double u0 = 0, v0 = 0, px = 1, mid = 0;
  std::vector<float> z;
  std::vector<int32_t> tri;       // the nearest triangle at each pixel centre, -1 for none
  std::vector<float> owner_defl;  // global face id -> the deflection of its mesh (how far its outline may be off)
  std::vector<float> owner_err;   // global face id -> how far its mesh may be off the surface (0 for planes)
  std::vector<float> X, Y, Z;     // the meshes' vertices in pixels and depth
  std::vector<Tri> tris;
  const Context* context = nullptr;
  // Seen (nearly) edge-on: its projected area under a thousandth of its true area. Such triangles (a wall seen along
  // itself) cover nothing, and their depth across the view is ill-conditioned.
  bool edge_on(const Tri& T, double area) const {
    const double ux = X[T.b] - X[T.a], uy = Y[T.b] - Y[T.a], uz = (Z[T.b] - Z[T.a]) / px;
    const double vx = X[T.c] - X[T.a], vy = Y[T.c] - Y[T.a], vz = (Z[T.c] - Z[T.a]) / px;
    const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz;
    return std::fabs(area) < 1e-12 || std::fabs(area) < 1e-3 * std::sqrt(cx * cx + cy * cy + area * area);
  }
  // The pixel buffer only finds candidates: a sample is hidden when the face of one of the nearest triangles around it
  // covers its position and lies in front of it there by more than its mesh's error. From each candidate the face's
  // triangles are walked towards the sample (thin triangles win no pixel centre); stepping over the face's boundary,
  // further than a tenth of a pixel or the mesh's error, means the face does not cover it. So an edge on the outline of a
  // nearer face is hidden (as exact HLR has it) while a line a pixel beside a nearer face's outline is not. A
  // silhouette's own face is left out: no face hides its own outline.
  bool visible(double u, double v, double depth, int own1, int own2, bool outline) const {
    const double sx = (u - u0) / px, sy = (v - v0) / px, d = depth - mid;
    const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
    // Candidates: the pixels around nearer than the sample, its own pixel first. None: visible (most visible samples).
    int candidates[9], n = 0;
    for (int k = 0; k < 9; ++k) {
      const int x = ix + (k == 0 ? 0 : (k - 1) % 3 - 1), y = iy + (k == 0 ? 0 : (k - 1) / 3 - 1);
      if (x < 0 || y < 0 || x >= W || y >= H || (k > 0 && x == ix && y == iy)) continue;
      const size_t i = static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x);
      const int t = tri[i];
      if (t < 0) continue;
      const int owner = tris[static_cast<size_t>(t)].owner;
      if (z[i] <= d + 2 * owner_err[static_cast<size_t>(owner)] + 0.5 * px || (outline && (owner == own1 || owner == own2))) continue;
      candidates[n++] = t;
    }
    int seen[9], count = 0;
    for (int c = 0; c < n; ++c) {
        int t = candidates[c];
        const double bulge = std::min(1.0, owner_defl[static_cast<size_t>(tris[static_cast<size_t>(t)].owner)] / px);
        double w[3] = {0, 0, 0};
        double facing = 0;
        for (int step = 0; t >= 0; ++step) {
          const Tri& T = tris[static_cast<size_t>(t)];
          const double ax = X[T.a], ay = Y[T.a], bx = X[T.b], by = Y[T.b], cx = X[T.c], cy = Y[T.c];
          const double area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
          // Lost, edge-on, or folded over (a curved face turning away): this face does not cover the sample here.
          if (step == 24 || edge_on(T, area) || area * facing < 0) {
            t = -1;
            break;
          }
          facing = area;
          w[0] = ((bx - sx) * (cy - sy) - (cx - sx) * (by - sy)) / area;
          w[1] = ((cx - sx) * (ay - sy) - (ax - sx) * (cy - sy)) / area;
          w[2] = 1 - w[0] - w[1];
          // Distances to sides ab, bc, ca in pixels, negative outside.
          const double side[3] = {w[2] * std::fabs(area) / std::hypot(bx - ax, by - ay), w[0] * std::fabs(area) / std::hypot(cx - bx, cy - by),
                                  w[1] * std::fabs(area) / std::hypot(ax - cx, ay - cy)};
          const int worst = static_cast<int>(std::min_element(side, side + 3) - side);
          if (side[worst] >= -1e-6) break;  // inside, or on a side (no stepping back and forth over it)
          const int across = T.next[static_cast<size_t>(worst)];
          if (across < 0) {  // a side on the face's outline: a curved outline bulges past its chord, most at its middle
            const double px0[3] = {ax, bx, cx}, py0[3] = {ay, by, cy};
            const int e = worst, f = (worst + 1) % 3;
            const double ex = px0[f] - px0[e], ey = py0[f] - py0[e];
            const double along = std::clamp(((sx - px0[e]) * ex + (sy - py0[e]) * ey) / std::max(ex * ex + ey * ey, 1e-12), 0.0, 1.0);
            if (side[worst] < -(0.05 + bulge * 4 * along * (1 - along))) t = -1;
            // Within the bulge a plane's exact outline decides for a silhouette: one meets its top face's rim tangentially,
            // so a long stretch of it lies in the band (classifying near a boundary is slow: outlines only).
            else if (outline && side[worst] < -0.05 && context && owner_err[static_cast<size_t>(T.owner)] == 0 && !covers(*context, T.owner, u, v)) t = -1;
            break;
          }
          t = across;
        }
        if (t < 0 || std::find(seen, seen + count, t) != seen + count) continue;
        seen[count++] = t;
        const Tri& T = tris[static_cast<size_t>(t)];
        const double zt = w[0] * Z[T.a] + w[1] * Z[T.b] + w[2] * Z[T.c];
        if (zt > d + 2 * owner_err[static_cast<size_t>(T.owner)] + 0.5 * px) return false;
      }
    return true;
  }
};

void raster(Depth& D, const Tri& t, int y0, int y1) {
  const double ax = D.X[t.a], ay = D.Y[t.a], bx = D.X[t.b], by = D.Y[t.b], cx = D.X[t.c], cy = D.Y[t.c];
  const double area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
  if (D.edge_on(t, area)) return;  // covers nothing
  const int minx = std::max(0, static_cast<int>(std::floor(std::min({ax, bx, cx}))));
  const int maxx = std::min(D.W - 1, static_cast<int>(std::ceil(std::max({ax, bx, cx}))));
  const int miny = std::max(y0, static_cast<int>(std::floor(std::min({ay, by, cy}))));
  const int maxy = std::min(y1 - 1, static_cast<int>(std::ceil(std::max({ay, by, cy}))));
  const double inv = 1.0 / area, az = D.Z[t.a], bz = D.Z[t.b], cz = D.Z[t.c];
  const int32_t id = static_cast<int32_t>(&t - D.tris.data());
  for (int y = miny; y <= maxy; ++y) {
    const double py = y + 0.5;
    for (int x = minx; x <= maxx; ++x) {
      const double px = x + 0.5;
      const double w0 = ((bx - px) * (cy - py) - (cx - px) * (by - py)) * inv;
      const double w1 = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) * inv;
      const double w2 = 1 - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      const float z = static_cast<float>(w0 * az + w1 * bz + w2 * cz);
      const size_t i = static_cast<size_t>(y) * static_cast<size_t>(D.W) + static_cast<size_t>(x);
      if (z > D.z[i]) D.z[i] = z, D.tri[i] = id;
    }
  }
}

// ---- the meshes, cached in memory (body key + deflection) on top of the user cache

std::mutex g_mesh_mu;
std::list<std::pair<std::string, std::shared_ptr<const Mesh>>> g_meshes;  // most recent first
size_t g_mesh_bytes = 0;
constexpr size_t kMeshBudget = size_t(768) << 20;

size_t bytes_of(const Mesh& m) { return m.positions.size() * 8 + m.indices.size() * 4 + m.faces.size() * 12; }

// ---- per-node work

// A face's exact outline in its parameters, shared by every node showing the body (built on first use).
struct Outline {
  bool plane = false;
  gp_Pln pln;  // in the body's frame
  std::unique_ptr<BRepTopAdaptor_FClass2d> inside;
  std::mutex mu;  // Perform() is not safe from two threads at once
  bool holds(double u, double v) {
    std::lock_guard<std::mutex> lock(mu);
    return inside->Perform(gp_Pnt2d(u, v)) != TopAbs_OUT;
  }
};

struct Context {
  const std::vector<Source>& sources;
  const ViewSpec& spec;
  const View& view;
  const Depth& depth;
  std::vector<const KeyInfo*> keys;  // per source
  std::vector<size_t> slot;          // per source: its body key's index
  std::vector<int> face_base;        // per source: its first global face id
  std::vector<std::vector<TopoDS_Shape>> placed_edges, placed_faces;  // non-rigid sources: their transformed copies' sub-shapes
  std::vector<size_t> outline_base;  // per body key: its first outline
  mutable std::vector<std::atomic<Outline*>> outlines;
  mutable std::mutex outlines_mu;
  mutable std::vector<std::unique_ptr<Outline>> owned;
  Context(const std::vector<Source>& s, const ViewSpec& sp, const View& v, const Depth& d) : sources(s), spec(sp), view(v), depth(d) {}
  Outline& outline(size_t s, int f) const {
    std::atomic<Outline*>& at = outlines[outline_base[slot[s]] + static_cast<size_t>(f)];
    if (Outline* o = at.load(std::memory_order_acquire)) return *o;
    auto made = std::make_unique<Outline>();
    const TopoDS_Face face = TopoDS::Face(keys[s]->faces(f + 1));
    const BRepAdaptor_Surface surface(face, Standard_False);
    if (surface.GetType() == GeomAbs_Plane) made->plane = true, made->pln = surface.Plane();
    made->inside = std::make_unique<BRepTopAdaptor_FClass2d>(face, Precision::PConfusion());
    std::lock_guard<std::mutex> lock(outlines_mu);
    if (Outline* o = at.load(std::memory_order_acquire)) return *o;
    at.store(made.get(), std::memory_order_release);
    owned.push_back(std::move(made));
    return *owned.back();
  }
};

TopoDS_Edge edge_of(const Context& cx, size_t s, int e) {
  const Source& src = cx.sources[s];
  if (!src.rigid) return TopoDS::Edge(cx.placed_edges[s][static_cast<size_t>(e)]);
  return TopoDS::Edge(cx.keys[s]->edges(e + 1).Moved(TopLoc_Location(src.trsf)));
}

TopoDS_Face face_of(const Context& cx, size_t s, int f) {
  const Source& src = cx.sources[s];
  if (!src.rigid) return TopoDS::Face(cx.placed_faces[s][static_cast<size_t>(f)]);
  return TopoDS::Face(cx.keys[s]->faces(f + 1).Moved(TopLoc_Location(src.trsf)));
}

bool covers(const Context& cx, int owner, double u, double v) {
  const auto at = std::upper_bound(cx.face_base.begin(), cx.face_base.end(), owner);
  if (at == cx.face_base.begin()) return true;
  const size_t s = static_cast<size_t>(at - cx.face_base.begin() - 1);
  const int f = owner - cx.face_base[s];
  if (f < 0 || f >= cx.keys[s]->faces.Extent() || !cx.sources[s].rigid) return true;
  Outline& o = cx.outline(s, f);
  if (!o.plane) return true;
  const gp_Pln plane = o.pln.Transformed(cx.sources[s].trsf);
  const gp_XYZ n = plane.Axis().Direction().XYZ(), z = cx.view.z.XYZ();
  const double nz = n.Dot(z);
  if (std::fabs(nz) < 1e-12) return true;
  const gp_XYZ base = cx.view.x.XYZ() * u + cx.view.y.XYZ() * v;
  const gp_Pnt on(base + z * (n.Dot(plane.Location().XYZ() - base) / nz));
  double pu, pv;
  ElSLib::Parameters(plane, on, pu, pv);  // the same parameters as the body-frame plane's
  return o.holds(pu, pv);
}

// Runs under two pixels (two samples) take the visibility of their longer neighbour: the buffer's noise where an edge
// meets a face or passes an occluder's outline.
void smooth_runs(std::vector<uint8_t>& v) {
  std::vector<std::pair<size_t, size_t>> runs;  // first sample, length
  for (size_t i = 0; i < v.size();) {
    size_t j = i;
    while (j < v.size() && v[j] == v[i]) ++j;
    runs.push_back({i, j - i});
    i = j;
  }
  if (runs.size() < 2) return;
  for (size_t r = 0; r < runs.size(); ++r) {
    if (runs[r].second >= 2) continue;
    const size_t prev = r > 0 ? runs[r - 1].second : 0, next = r + 1 < runs.size() ? runs[r + 1].second : 0;
    const uint8_t to = prev >= next ? v[runs[r - 1].first] : v[runs[r + 1].first];
    std::fill(v.begin() + static_cast<std::ptrdiff_t>(runs[r].first), v.begin() + static_cast<std::ptrdiff_t>(runs[r].first + runs[r].second), to);
  }
}

// Samples the curve between a and b every pixel, cuts it where visibility changes (refined by bisection) and emits
// the pieces, typed.
void trace(const Context& cx, const Adaptor3d_Curve& c, double a, double b, int own1, int own2, const Curve& like, std::vector<Curve>& out) {
  const Depth& D = cx.depth;
  auto vis = [&](double t) {
    const gp_Pnt p = c.Value(t);
    const Vec2 uv = cx.view.at(p);
    return D.visible(uv[0], uv[1], cx.view.depth(p), own1, own2, like.kind == Curve::Kind::Silhouette);
  };
  double length = 0;
  Vec2 last = cx.view.at(c.Value(a));
  for (int i = 1; i <= 8; ++i) {
    const Vec2 q = cx.view.at(c.Value(a + (b - a) * i / 8));
    length += std::hypot(q[0] - last[0], q[1] - last[1]);
    last = q;
  }
  const int n = std::clamp(static_cast<int>(std::ceil(length / D.px)), 2, 1 << 16);
  std::vector<uint8_t> v(static_cast<size_t>(n) + 1);
  for (int i = 0; i <= n; ++i) v[static_cast<size_t>(i)] = vis(a + (b - a) * i / n);
  smooth_runs(v);
  double start = a;
  for (int i = 0; i < n; ++i) {
    if (v[static_cast<size_t>(i)] == v[static_cast<size_t>(i + 1)]) continue;
    double lo = a + (b - a) * i / n, hi = a + (b - a) * (i + 1) / n;
    for (int k = 0; k < 5; ++k) {
      const double m = 0.5 * (lo + hi);
      (vis(m) == v[static_cast<size_t>(i)] ? lo : hi) = m;
    }
    const double cut = 0.5 * (lo + hi);
    if (v[static_cast<size_t>(i)] || cx.spec.hidden) {
      Curve k = like;
      k.hidden = !v[static_cast<size_t>(i)];
      emit(c, start, cut, cx.view, k, cx.spec.tolerance, out);
    }
    start = cut;
  }
  if (v[static_cast<size_t>(n)] || cx.spec.hidden) {
    Curve k = like;
    k.hidden = !v[static_cast<size_t>(n)];
    emit(c, start, b, cx.view, k, cx.spec.tolerance, out);
  }
}

void edge_work(const Context& cx, size_t s, int e, std::vector<Curve>& out) {
  const KeyInfo& key = *cx.keys[s];
  const EdgeInfo& info = key.e[static_cast<size_t>(e)];
  if (info.skip) return;
  Curve like;
  like.body = static_cast<int>(s);
  like.edge = e;
  like.kind = info.kind;
  if (like.kind != Curve::Kind::Sharp) {  // a smooth edge whose faces are seen edge-on is the outline there
    const Source& src = cx.sources[s];
    gp_Vec n(info.normal);
    if (src.rigid) n.Transform(src.trsf);
    else {
      const Vec3 w = src.world.apply_dir({n.X(), n.Y(), n.Z()});
      n = gp_Vec(w[0], w[1], w[2]);
    }
    if (n.Magnitude() > 1e-12 && std::fabs(n.Normalized().Dot(gp_Vec(cx.view.z))) < 1e-6) like.kind = Curve::Kind::Silhouette;
  }
  if ((like.kind == Curve::Kind::Tangent && !cx.spec.tangent) || (like.kind == Curve::Kind::Seam && !cx.spec.seams)) return;
  const BRepAdaptor_Curve c(edge_of(cx, s, e));
  const int base = cx.face_base[s];
  trace(cx, c, c.FirstParameter(), c.LastParameter(), info.f1 >= 0 ? base + info.f1 : -2, info.f2 >= 0 ? base + info.f2 : -2, like, out);
}

// The parameter runs of [a, b] where `inside` holds, ends refined by bisection.
std::vector<std::pair<double, double>> inside_runs(double a, double b, const std::function<bool(double)>& inside, int samples = 64) {
  std::vector<std::pair<double, double>> runs;
  bool was = inside(a);
  double start = a;
  for (int i = 1; i <= samples; ++i) {
    const double t = a + (b - a) * i / samples;
    const bool now = inside(t);
    if (now == was) continue;
    double lo = a + (b - a) * (i - 1) / samples, hi = t;
    for (int k = 0; k < 12; ++k) {
      const double m = 0.5 * (lo + hi);
      (inside(m) == was ? lo : hi) = m;
    }
    const double cut = 0.5 * (lo + hi);
    if (was) runs.push_back({start, cut});
    start = cut;
    was = now;
  }
  if (was) runs.push_back({start, b});
  return runs;
}

// Analytic silhouettes of quadrics: generator lines of cylinders and cones, a circle of spheres and of tori seen along
// their axis. Only where the face really is (its trimming), and not where a face boundary already runs.
bool analytic_silhouette(const Context& cx, size_t s, int f, std::vector<Curve>& out) {
  const KeyInfo& key = *cx.keys[s];
  const Surface type = key.f[static_cast<size_t>(f)];
  const TopoDS_Face face = face_of(cx, s, f);
  const BRepAdaptor_Surface surface(face, Standard_False);
  const gp_Vec d(cx.view.z);
  double u0, u1, v0, v1;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  Outline* shared = nullptr;
  std::unique_ptr<BRepTopAdaptor_FClass2d> own_classifier;  // a transformed copy's face (not rigid)
  auto in_face = [&](double u, double v) {
    if (cx.sources[s].rigid) {
      if (!shared) shared = &cx.outline(s, f);
      return shared->holds(u, v);
    }
    if (!own_classifier) own_classifier = std::make_unique<BRepTopAdaptor_FClass2d>(face, Precision::PConfusion());
    return own_classifier->Perform(gp_Pnt2d(u, v)) != TopAbs_OUT;
  };
  auto at_bound = [&](double u) {  // on a boundary of the face's u range: an edge is drawn there
    while (u < u0 - 1e-9) u += 2 * M_PI;
    while (u > u0 + 2 * M_PI - 1e-9) u -= 2 * M_PI;
    return std::fabs(u - u0) < 1e-7 || std::fabs(u - u1) < 1e-7 || std::fabs(u - u0 - 2 * M_PI) < 1e-7 || u > u1 + 1e-9;
  };
  Curve like;
  like.body = static_cast<int>(s);
  like.face = f;
  like.kind = Curve::Kind::Silhouette;
  const int own = cx.face_base[s] + f;
  // A generator is the iso line u: where the face's boundary curves cross it cuts it; between cuts it is in or out.
  auto lines = [&](double u, const gp_Pnt& p0, const gp_Dir& along) {
    if (at_bound(u)) return;
    std::vector<double> cuts{v0, v1};
    for (TopExp_Explorer e(face, TopAbs_EDGE); e.More(); e.Next()) {
      double f, l;
      const Handle(Geom2d_Curve) pc = BRep_Tool::CurveOnSurface(TopoDS::Edge(e.Current()), face, f, l);
      if (pc.IsNull()) continue;
      auto off = [&](double t) { return std::remainder(pc->Value(t).X() - u, 2 * M_PI); };
      double ta = f, da = off(f);
      for (int i = 1; i <= 32; ++i) {
        const double tb = f + (l - f) * i / 32, db = off(tb);
        if (std::fabs(da) < 1e-12) cuts.push_back(pc->Value(ta).Y());
        else if ((da < 0) != (db < 0) && std::fabs(da) < M_PI / 2 && std::fabs(db) < M_PI / 2) {
          double lo = ta, hi = tb;
          for (int k = 0; k < 40; ++k) {
            const double m = 0.5 * (lo + hi);
            ((off(m) < 0) == (da < 0) ? lo : hi) = m;
          }
          cuts.push_back(pc->Value(0.5 * (lo + hi)).Y());
        }
        ta = tb, da = db;
      }
    }
    std::sort(cuts.begin(), cuts.end());
    const GeomAdaptor_Curve line(new Geom_Line(gp_Lin(p0, along)));
    double start = 0;
    bool open = false;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
      const double a = std::max(cuts[i], v0), b = std::min(cuts[i + 1], v1);
      const bool in = b - a > 1e-9 && in_face(u, 0.5 * (a + b));
      if (in && !open) start = a, open = true;
      if (!in && open) {
        trace(cx, line, start, a, own, own, like, out);
        open = false;
      }
    }
    if (open) trace(cx, line, start, v1, own, own, like, out);
  };
  // Seen along the axis of a sphere or torus split at its equator, the outline is the boundary edge there.
  auto on_v_bound = [&](double v) { return std::fabs(v - v0) < 1e-7 || std::fabs(v - v1) < 1e-7; };
  switch (type) {
    case Surface::Cylinder: {
      const gp_Cylinder cy = surface.Cylinder();
      const gp_Ax3& pos = cy.Position();
      const gp_Vec axis(pos.Direction());
      const gp_Vec across = axis.Crossed(d);
      if (across.Magnitude() < 1e-9) return true;  // seen along its axis: the rims are its outline
      for (double side : {1.0, -1.0}) {
        const gp_Vec r = across.Normalized() * side;
        const double u = std::atan2(r.Dot(gp_Vec(pos.YDirection())), r.Dot(gp_Vec(pos.XDirection())));
        lines(u, pos.Location().Translated(r * cy.Radius()), pos.Direction());
      }
      return true;
    }
    case Surface::Cone: {
      const gp_Cone co = surface.Cone();
      const gp_Ax3& pos = co.Position();
      const double alpha = co.SemiAngle();
      const double x = gp_Vec(pos.XDirection()).Dot(d), y = gp_Vec(pos.YDirection()).Dot(d), z = gp_Vec(pos.Direction()).Dot(d) * std::tan(alpha);
      const double rho = std::hypot(x, y);
      if (rho < 1e-12 || std::fabs(z) > rho) return true;
      const double phi = std::atan2(y, x), delta = std::acos(std::clamp(z / rho, -1.0, 1.0));
      for (double u : {phi + delta, phi - delta}) {
        const gp_Vec r = gp_Vec(pos.XDirection()) * std::cos(u) + gp_Vec(pos.YDirection()) * std::sin(u);
        const gp_Vec along = r * std::sin(alpha) + gp_Vec(pos.Direction()) * std::cos(alpha);
        lines(u, pos.Location().Translated(r * co.RefRadius()), gp_Dir(along));
      }
      return true;
    }
    case Surface::Sphere: {
      const gp_Sphere sp = surface.Sphere();
      if (std::fabs(gp_Vec(sp.Position().Direction()).Dot(d)) > 1 - 1e-9 && on_v_bound(0)) return true;
      const GeomAdaptor_Curve circle(new Geom_Circle(gp_Ax2(sp.Location(), cx.view.z, cx.view.x), sp.Radius()));
      // Seen across its axis the outline is two meridians; one may be the face's seam, drawn as an edge.
      const bool meridians = std::fabs(gp_Vec(sp.Position().Direction()).Dot(d)) < 1e-9;
      for (const auto& [a, b] : inside_runs(0, 2 * M_PI, [&](double t) {
             double u, v;
             ElSLib::Parameters(sp, circle.Value(t), u, v);
             return in_face(u, v) && !(meridians && at_bound(u));
           }, 128))
        if (b - a > 1e-9) trace(cx, circle, a, b, own, own, like, out);
      return true;
    }
    case Surface::Torus: {
      const gp_Torus to = surface.Torus();
      const gp_Ax3& pos = to.Position();
      if (std::fabs(gp_Vec(pos.Direction()).Dot(d)) < 1 - 1e-9) return false;  // a quartic: the mesh's
      for (double v : {0.0, M_PI}) {
        const double radius = to.MajorRadius() + std::cos(v) * to.MinorRadius();
        if (radius < 1e-9 || on_v_bound(v) || on_v_bound(v + 2 * M_PI) || on_v_bound(v - 2 * M_PI)) continue;
        const GeomAdaptor_Curve circle(new Geom_Circle(gp_Ax2(pos.Location(), pos.Direction(), pos.XDirection()), radius));
        for (const auto& [a, b] : inside_runs(0, 2 * M_PI, [&](double t) { return in_face(t, v); }, 128))
          if (b - a > 1e-9) trace(cx, circle, a, b, own, own, like, out);
      }
      return true;
    }
    default: return false;
  }
}

// Polylines through `pts` with visibility sampled every pixel; cut where it changes.
void trace_polyline(const Context& cx, const std::vector<gp_Pnt>& pts, int own1, int own2, const Curve& like, std::vector<Curve>& out) {
  if (pts.size() < 2) return;
  const Depth& D = cx.depth;
  Curve piece = like;
  piece.type = Curve::Type::Polyline;
  int state = -1;
  auto flush = [&]() {
    if (piece.pts.size() >= 2 && (!piece.hidden || cx.spec.hidden)) out.push_back(piece);
    piece.pts.clear();
  };
  for (size_t i = 0; i + 1 < pts.size(); ++i) {
    const Vec2 a = cx.view.at(pts[i]), b = cx.view.at(pts[i + 1]);
    const double da = cx.view.depth(pts[i]), db = cx.view.depth(pts[i + 1]);
    const int n = std::clamp(static_cast<int>(std::ceil(std::hypot(b[0] - a[0], b[1] - a[1]) / D.px)), 1, 1 << 14);
    for (int k = i == 0 ? 0 : 1; k <= n; ++k) {
      const double f = static_cast<double>(k) / n;
      const Vec2 p{a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f};
      const int now = D.visible(p[0], p[1], da + (db - da) * f, own1, own2, like.kind == Curve::Kind::Silhouette) ? 1 : 0;
      if (state >= 0 && now != state) {
        piece.pts.push_back(p);
        flush();
      }
      if (now != state) piece.hidden = !now;
      state = now;
      if (piece.pts.empty() || k == n || k == 0) piece.pts.push_back(p);
    }
  }
  flush();
}

// Joins segments (pairs of keyed points) into polylines.
std::vector<std::vector<gp_Pnt>> chain(const std::vector<std::array<uint64_t, 2>>& keys, const std::vector<std::array<gp_Pnt, 2>>& segments) {
  std::unordered_map<uint64_t, std::vector<size_t>> at;
  for (size_t i = 0; i < keys.size(); ++i)
    for (uint64_t k : keys[i]) at[k].push_back(i);
  std::vector<char> used(keys.size(), 0);
  std::vector<std::vector<gp_Pnt>> out;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (used[i]) continue;
    used[i] = 1;
    std::vector<gp_Pnt> fwd{segments[i][1]}, back{segments[i][0]};
    for (int dir = 0; dir < 2; ++dir) {
      uint64_t k = keys[i][dir == 0 ? 1 : 0];
      auto& line = dir == 0 ? fwd : back;
      for (bool more = true; more;) {
        more = false;
        for (size_t j : at[k]) {
          if (used[j]) continue;
          used[j] = 1;
          const int end = keys[j][0] == k ? 1 : 0;
          line.push_back(segments[j][static_cast<size_t>(end)]);
          k = keys[j][static_cast<size_t>(end)];
          more = true;
          break;
        }
      }
    }
    std::reverse(back.begin(), back.end());
    back.insert(back.end(), fwd.begin(), fwd.end());
    out.push_back(std::move(back));
  }
  return out;
}

Vec3 to_local_dir(const Source& src, const gp_Dir& d) {
  if (src.rigid) {
    gp_Dir l = d;
    l.Transform(src.trsf.Inverted());
    return {l.X(), l.Y(), l.Z()};
  }
  // Normals map through the inverse transpose, so n_world . d = n_local . (M^-1 d).
  const auto& m = src.world;
  const double a = m.at(0, 0), b = m.at(0, 1), c = m.at(0, 2), e = m.at(1, 0), f = m.at(1, 1), g = m.at(1, 2), h = m.at(2, 0), i = m.at(2, 1), j = m.at(2, 2);
  const double det = a * (f * j - g * i) - b * (e * j - g * h) + c * (e * i - f * h);
  if (std::fabs(det) < 1e-18) return {d.X(), d.Y(), d.Z()};
  const double x = d.X(), y = d.Y(), z = d.Z();
  return {((f * j - g * i) * x + (c * i - b * j) * y + (b * g - c * f) * z) / det, ((g * h - e * j) * x + (a * j - c * h) * y + (c * e - a * g) * z) / det,
          ((e * i - f * h) * x + (b * h - a * i) * y + (a * f - b * e) * z) / det};
}

// Silhouettes of a freeform face from its mesh: where the interpolated normal turns across the view in each triangle.
void mesh_silhouette(const Context& cx, size_t s, int f, std::vector<Curve>& out) {
  const KeyInfo& key = *cx.keys[s];
  if (!key.mesh || f >= static_cast<int>(key.range.size()) || key.range[static_cast<size_t>(f)] < 0) return;
  const Mesh& m = *key.mesh;
  const Mesh::FaceRange& r = m.faces[static_cast<size_t>(key.range[static_cast<size_t>(f)])];
  const Source& src = cx.sources[s];
  const Vec3 d = to_local_dir(src, cx.view.z);
  auto value = [&](uint32_t i) { return m.normals[i * 3] * d[0] + m.normals[i * 3 + 1] * d[1] + m.normals[i * 3 + 2] * d[2]; };
  auto point = [&](uint32_t i) { return gp_Pnt(m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]); };
  std::vector<std::array<uint64_t, 2>> keys;
  std::vector<std::array<gp_Pnt, 2>> segments;
  for (uint32_t t = r.first; t + 2 < r.first + r.count; t += 3) {
    const uint32_t v[3] = {m.indices[t], m.indices[t + 1], m.indices[t + 2]};
    const double s3[3] = {value(v[0]), value(v[1]), value(v[2])};
    std::array<uint64_t, 2> k{};
    std::array<gp_Pnt, 2> p;
    int found = 0;
    for (int e = 0; e < 3 && found < 2; ++e) {
      const int a = e, b = (e + 1) % 3;
      if ((s3[a] > 0) == (s3[b] > 0)) continue;
      const double w = s3[a] / (s3[a] - s3[b]);
      const gp_Pnt pa = point(v[a]), pb = point(v[b]);
      p[static_cast<size_t>(found)] = gp_Pnt(pa.XYZ() + (pb.XYZ() - pa.XYZ()) * w);
      k[static_cast<size_t>(found)] = (static_cast<uint64_t>(std::min(v[a], v[b])) << 32) | std::max(v[a], v[b]);
      ++found;
    }
    if (found == 2) keys.push_back(k), segments.push_back(p);
  }
  if (segments.empty()) return;
  Curve like;
  like.body = static_cast<int>(s);
  like.face = f;
  like.kind = Curve::Kind::Silhouette;
  const int own = cx.face_base[s] + f;
  for (auto& line : chain(keys, segments)) {
    for (auto& p : line) {
      const Vec3 w = src.world.apply({p.X(), p.Y(), p.Z()});
      p.SetCoord(w[0], w[1], w[2]);
    }
    trace_polyline(cx, line, own, own, like, out);
  }
}

struct Cell {
  long long x, y, z;
  bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct CellHash {
  size_t operator()(const Cell& c) const { return std::hash<long long>()(c.x * 73856093LL ^ c.y * 19349663LL ^ c.z * 83492791LL); }
};

// Triangulation-only bodies: their boundaries, creases (over 30 degrees) and silhouettes, from the welded triangles.
void mesh_body_lines(const Context& cx, size_t s, std::vector<Curve>& out) {
  const KeyInfo& key = *cx.keys[s];
  if (!key.mesh || key.mesh->empty()) return;
  const Mesh& m = *key.mesh;
  const Source& src = cx.sources[s];
  const Vec3 d = to_local_dir(src, cx.view.z);
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (size_t i = 0; i + 2 < m.positions.size(); i += 3)
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], double(m.positions[i + k])), hi[k] = std::max(hi[k], double(m.positions[i + k]));
  const double q = std::max(1e-9, 1e-6 * std::hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]));
  std::unordered_map<Cell, uint32_t, CellHash> weld;
  std::vector<uint32_t> id(m.positions.size() / 3);
  std::vector<gp_Pnt> pts;
  for (size_t i = 0; i < id.size(); ++i) {
    const Cell cell{std::llround((m.positions[i * 3] - lo[0]) / q), std::llround((m.positions[i * 3 + 1] - lo[1]) / q), std::llround((m.positions[i * 3 + 2] - lo[2]) / q)};
    auto [it, fresh] = weld.emplace(cell, static_cast<uint32_t>(pts.size()));
    if (fresh) pts.emplace_back(m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]);
    id[i] = it->second;
  }
  std::vector<int> tri_face(m.indices.size() / 3, -1);
  for (const auto& r : m.faces)
    for (uint32_t t = r.first / 3; t < (r.first + r.count) / 3; ++t) tri_face[t] = r.face;
  struct Side { int t1 = -1, t2 = -1, count = 0; };
  std::unordered_map<uint64_t, Side> sides;
  std::vector<gp_Vec> normal(m.indices.size() / 3);
  for (size_t t = 0; t < normal.size(); ++t) {
    const uint32_t a = id[m.indices[t * 3]], b = id[m.indices[t * 3 + 1]], c = id[m.indices[t * 3 + 2]];
    const gp_Vec n = gp_Vec(pts[a], pts[b]).Crossed(gp_Vec(pts[a], pts[c]));
    normal[t] = n.Magnitude() > 1e-30 ? n.Normalized() : gp_Vec(0, 0, 0);
    for (auto [x, y] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
      if (x == y) continue;
      Side& side = sides[(static_cast<uint64_t>(std::min(x, y)) << 32) | std::max(x, y)];
      (side.count++ == 0 ? side.t1 : side.t2) = static_cast<int>(t);
    }
  }
  const gp_Vec dl(d[0], d[1], d[2]);
  std::vector<std::array<uint64_t, 2>> keys[2];
  std::vector<std::array<gp_Pnt, 2>> segments[2];
  for (const auto& [k, side] : sides) {
    int kind = -1;  // 0 sharp, 1 silhouette
    if (side.count != 2) kind = 0;
    else {
      const gp_Vec& n1 = normal[static_cast<size_t>(side.t1)];
      const gp_Vec& n2 = normal[static_cast<size_t>(side.t2)];
      if (n1.Dot(n2) < std::cos(M_PI / 6)) kind = 0;
      else if ((n1.Dot(dl) > 0) != (n2.Dot(dl) > 0)) kind = 1;
    }
    if (kind < 0 || (kind == 1 && !cx.spec.silhouettes)) continue;
    const uint32_t a = static_cast<uint32_t>(k >> 32), b = static_cast<uint32_t>(k & 0xffffffffu);
    keys[kind].push_back({a, b});
    segments[kind].push_back({pts[a], pts[b]});
  }
  const int own = cx.face_base[s] + (m.faces.empty() ? 0 : m.faces.front().face);
  for (int kind = 0; kind < 2; ++kind) {
    Curve like;
    like.body = static_cast<int>(s);
    like.kind = kind ? Curve::Kind::Silhouette : Curve::Kind::Sharp;
    for (auto& line : chain(keys[kind], segments[kind])) {
      for (auto& p : line) {
        const Vec3 w = src.world.apply({p.X(), p.Y(), p.Z()});
        p.SetCoord(w[0], w[1], w[2]);
      }
      trace_polyline(cx, line, own, own, like, out);
    }
  }
}

}  // namespace

double deflection_for(const Document& doc, const std::string& key) {
  const Bnd_Box box = body_bbox(doc, key);
  if (box.IsVoid()) return 0.1;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  return std::clamp(std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0)) * 0.0008, 0.005, 1.0);
}

std::shared_ptr<const Mesh> body_mesh(const Document& doc, const Source& s, double deflection) {
  (void)doc;
  char tol[32];
  std::snprintf(tol, sizeof tol, "%.6g", deflection);
  const std::string id = s.key + "-recovery1-" + tol;  // tessellate_body()'s entries
  {
    std::lock_guard<std::mutex> lock(g_mesh_mu);
    for (auto it = g_meshes.begin(); it != g_meshes.end(); ++it)
      if (it->first == id) {
        auto hit = it->second;
        g_meshes.splice(g_meshes.begin(), g_meshes, it);
        return hit;
      }
  }
  std::shared_ptr<Mesh> m;
  if (auto blob = cache_get("mesh", id)) {
    try {
      m = std::make_shared<Mesh>(Mesh::deserialize(*blob));
    } catch (const std::exception&) {
    }
  }
  if (!m) {
    // A copy: the shared shape may be meshed by the display at the same time, and BRepMesh writes into it.
    const TopoDS_Shape copy = BRepBuilderAPI_Copy(s.proto, Standard_True, s.mesh).Shape();
    m = std::make_shared<Mesh>(tessellate(copy, deflection));
    cache_put("mesh", id, m->serialize());
  }
  std::lock_guard<std::mutex> lock(g_mesh_mu);
  g_meshes.emplace_front(id, m);
  g_mesh_bytes += bytes_of(*m);
  while (g_mesh_bytes > kMeshBudget && g_meshes.size() > 1) {
    g_mesh_bytes -= bytes_of(*g_meshes.back().second);
    g_meshes.pop_back();
  }
  return m;
}

void hybrid(const Document& doc, const std::vector<Source>& sources, const ViewSpec& spec, const View& view, Run& run, ViewGeometry& out) {
  using clock = std::chrono::steady_clock;
  auto ms = [](clock::time_point a) { return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - a).count(); };
  auto t = clock::now();
  // 1. Per body key: edge classes, face types, the mesh.
  std::vector<std::string> names;
  std::unordered_map<std::string, size_t> slot;
  for (const auto& s : sources)
    if (slot.emplace(s.key, names.size()).second) names.push_back(s.key);
  std::vector<KeyInfo> infos(names.size());
  std::atomic<size_t> done{0};
  OSD_Parallel::For(0, static_cast<int>(names.size()), [&](int i) {
    if (run.cancelled()) return;
    const Source& s = *std::find_if(sources.begin(), sources.end(), [&](const Source& x) { return x.key == names[static_cast<size_t>(i)]; });
    KeyInfo& k = infos[static_cast<size_t>(i)];
    try {
      if (!s.mesh) classify(s.proto, k);
      k.defl = deflection_for(doc, s.key);
      k.mesh = body_mesh(doc, s, k.defl);
      const auto& I = k.mesh->indices;
      k.next.assign(I.size() / 3, {-1, -1, -1});
      std::unordered_map<uint64_t, int32_t> first;  // a side's first triangle * 3 + side
      for (const auto& r : k.mesh->faces) {
        first.clear();
        for (uint32_t t = r.first; t + 2 < r.first + r.count; t += 3)
          for (int e = 0; e < 3; ++e) {
            const uint32_t a = I[t + static_cast<uint32_t>(e)], b = I[t + static_cast<uint32_t>(e + 1) % 3];
            const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
            const int32_t here = static_cast<int32_t>(t / 3) * 3 + e;
            auto [it, fresh] = first.emplace(key, here);
            if (fresh || it->second < 0) continue;
            k.next[static_cast<size_t>(it->second / 3)][static_cast<size_t>(it->second % 3)] = static_cast<int32_t>(t / 3);
            k.next[t / 3][static_cast<size_t>(e)] = it->second / 3;
            it->second = -1;  // a third triangle on one side (a non-manifold mesh) links to nothing
          }
      }
      k.range.assign(static_cast<size_t>(std::max(k.faces.Extent(), 1)), -1);
      for (size_t r = 0; r < k.mesh->faces.size(); ++r) {
        const int f = k.mesh->faces[r].face;
        if (f >= static_cast<int>(k.range.size())) k.range.resize(static_cast<size_t>(f) + 1, -1);
        if (f >= 0) k.range[static_cast<size_t>(f)] = static_cast<int>(r);
      }
    } catch (const Standard_Failure&) {
    } catch (const std::exception&) {
    }
    const size_t n = ++done;
    run.report(0.4 * static_cast<double>(n) / static_cast<double>(names.size()), "hidden lines: meshing " + std::to_string(n) + "/" + std::to_string(names.size()));
  });
  run.check();
  out.stats["prepare_ms"] = ms(t);
  t = clock::now();
  Depth D;
  Context cx(sources, spec, view, D);
  cx.keys.resize(sources.size());
  cx.slot.resize(sources.size());
  cx.face_base.resize(sources.size());
  size_t outlines = 0;
  for (const auto& k : infos) cx.outline_base.push_back(outlines), outlines += static_cast<size_t>(k.faces.Extent());
  cx.outlines = std::vector<std::atomic<Outline*>>(outlines);
  cx.placed_edges.resize(sources.size());
  cx.placed_faces.resize(sources.size());
  int faces = 0;
  for (size_t i = 0; i < sources.size(); ++i) {
    cx.slot[i] = slot.at(sources[i].key);
    cx.keys[i] = &infos[cx.slot[i]];
    cx.face_base[i] = faces;
    faces += std::max(cx.keys[i]->faces.Extent(), static_cast<int>(cx.keys[i]->range.size()));
    if (!sources[i].rigid) {
      TopTools_IndexedMapOfShape e, f;
      TopExp::MapShapes(sources[i].placed, TopAbs_EDGE, e);
      TopExp::MapShapes(sources[i].placed, TopAbs_FACE, f);
      for (int k = 1; k <= e.Extent(); ++k) cx.placed_edges[i].push_back(e(k));
      for (int k = 1; k <= f.Extent(); ++k) cx.placed_faces[i].push_back(f(k));
    }
  }
  D.owner_defl.assign(static_cast<size_t>(faces), 0.f);
  D.owner_err.assign(static_cast<size_t>(faces), 0.f);
  D.context = &cx;
  // 2. The meshes in view coordinates.
  std::vector<size_t> vbase(sources.size() + 1, 0);
  for (size_t i = 0; i < sources.size(); ++i) vbase[i + 1] = vbase[i] + (cx.keys[i]->mesh ? cx.keys[i]->mesh->positions.size() / 3 : 0);
  std::vector<float>& X = D.X;
  std::vector<float>& Y = D.Y;
  std::vector<float>& Z = D.Z;
  X.resize(vbase.back());
  Y.resize(vbase.back());
  Z.resize(vbase.back());
  std::vector<std::array<double, 6>> extent(sources.size(), {1e300, 1e300, 1e300, -1e300, -1e300, -1e300});
  OSD_Parallel::For(0, static_cast<int>(sources.size()), [&](int i) {
    const KeyInfo& k = *cx.keys[static_cast<size_t>(i)];
    if (!k.mesh) return;
    const Mat4& w = sources[static_cast<size_t>(i)].world;
    auto& e = extent[static_cast<size_t>(i)];
    const auto& P = k.mesh->positions;
    for (size_t v = 0, at = vbase[static_cast<size_t>(i)]; v + 2 < P.size(); v += 3, ++at) {
      const Vec3 p = w.apply({P[v], P[v + 1], P[v + 2]});
      const gp_Pnt q(p[0], p[1], p[2]);
      const Vec2 uv = view.at(q);
      const double dz = view.depth(q);
      X[at] = static_cast<float>(uv[0]), Y[at] = static_cast<float>(uv[1]), Z[at] = static_cast<float>(dz);
      e = {std::min(e[0], uv[0]), std::min(e[1], uv[1]), std::min(e[2], dz), std::max(e[3], uv[0]), std::max(e[4], uv[1]), std::max(e[5], dz)};
    }
  });
  std::array<double, 6> all{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
  for (const auto& e : extent)
    for (int k = 0; k < 3; ++k) all[static_cast<size_t>(k)] = std::min(all[static_cast<size_t>(k)], e[static_cast<size_t>(k)]), all[static_cast<size_t>(k + 3)] = std::max(all[static_cast<size_t>(k + 3)], e[static_cast<size_t>(k + 3)]);
  if (all[0] > all[3]) all = {0, 0, 0, 1, 1, 1};  // nothing meshed: an empty buffer, everything visible
  const double span = std::max({all[3] - all[0], all[4] - all[1], 1e-6});
  D.px = span / std::max(256, spec.resolution);
  D.u0 = all[0] - 2 * D.px;
  D.v0 = all[1] - 2 * D.px;
  D.mid = 0.5 * (all[2] + all[5]);
  D.W = static_cast<int>(std::ceil((all[3] - all[0]) / D.px)) + 4;
  D.H = static_cast<int>(std::ceil((all[4] - all[1]) / D.px)) + 4;
  D.z.assign(static_cast<size_t>(D.W) * static_cast<size_t>(D.H), -std::numeric_limits<float>::infinity());
  D.tri.assign(D.z.size(), -1);
  for (size_t i = 0; i < X.size(); ++i) {
    X[i] = static_cast<float>((X[i] - D.u0) / D.px);
    Y[i] = static_cast<float>((Y[i] - D.v0) / D.px);
    Z[i] = static_cast<float>(Z[i] - D.mid);
  }
  // 3. Rasterised in horizontal bands, side by side.
  std::vector<size_t> tbase(sources.size() + 1, 0);  // a mesh's triangles go in in order: local index + tbase
  for (size_t i = 0; i < sources.size(); ++i) tbase[i + 1] = tbase[i] + (cx.keys[i]->mesh ? cx.keys[i]->mesh->indices.size() / 3 : 0);
  std::vector<Tri>& tris = D.tris;
  tris.resize(tbase.back());
  OSD_Parallel::For(0, static_cast<int>(sources.size()), [&](int si) {
    const size_t i = static_cast<size_t>(si);
    const KeyInfo& k = *cx.keys[i];
    if (!k.mesh) return;
    for (const auto& r : k.mesh->faces) {
      const int owner = cx.face_base[i] + r.face;
      if (owner >= 0 && static_cast<size_t>(owner) < D.owner_defl.size()) {
        D.owner_defl[static_cast<size_t>(owner)] = static_cast<float>(k.defl);
        const bool plane = r.face >= 0 && static_cast<size_t>(r.face) < k.f.size() && k.f[static_cast<size_t>(r.face)] == Surface::Plane;
        D.owner_err[static_cast<size_t>(owner)] = plane ? 0.f : static_cast<float>(k.defl);
      }
      for (uint32_t t = r.first; t + 2 < r.first + r.count; t += 3) {
        Tri& tr = tris[tbase[i] + t / 3];
        tr = {static_cast<uint32_t>(vbase[i] + k.mesh->indices[t]), static_cast<uint32_t>(vbase[i] + k.mesh->indices[t + 1]),
              static_cast<uint32_t>(vbase[i] + k.mesh->indices[t + 2]), owner, {-1, -1, -1}};
        if (t / 3 < k.next.size())
          for (int e = 0; e < 3; ++e)
            if (const int32_t n = k.next[t / 3][static_cast<size_t>(e)]; n >= 0) tr.next[static_cast<size_t>(e)] = static_cast<int32_t>(tbase[i]) + n;
      }
    }
  });
  const int bands = std::clamp(D.H / 16, 1, 256), band = (D.H + bands - 1) / bands;
  const size_t parts = 64, per = (tris.size() + parts - 1) / parts;
  std::vector<std::vector<std::vector<uint32_t>>> local(parts, std::vector<std::vector<uint32_t>>(static_cast<size_t>(bands)));
  OSD_Parallel::For(0, static_cast<int>(parts), [&](int p) {
    for (size_t i = static_cast<size_t>(p) * per; i < std::min(tris.size(), static_cast<size_t>(p + 1) * per); ++i) {
      const auto& tr = tris[i];
      const int y0 = std::clamp(static_cast<int>(std::floor(std::min({Y[tr.a], Y[tr.b], Y[tr.c]}))) / band, 0, bands - 1);
      const int y1 = std::clamp(static_cast<int>(std::ceil(std::max({Y[tr.a], Y[tr.b], Y[tr.c]}))) / band, 0, bands - 1);
      for (int b = y0; b <= y1; ++b) local[static_cast<size_t>(p)][static_cast<size_t>(b)].push_back(static_cast<uint32_t>(i));
    }
  });
  run.report(0.45, "hidden lines: depth buffer", true);
  OSD_Parallel::For(0, bands, [&](int b) {
    if (run.cancelled()) return;
    for (const auto& part : local)
      for (uint32_t i : part[static_cast<size_t>(b)]) raster(D, tris[i], b * band, std::min(D.H, (b + 1) * band));
  });
  run.check();
  out.stats["triangles"] = tris.size();
  out.stats["pixels"] = {D.W, D.H};
  out.stats["pixel_mm"] = D.px;
  out.stats["depth_ms"] = ms(t);
  local.clear();
  t = clock::now();
  // 4. Edges and silhouettes, in chunks side by side; the chunks' curves are joined in order (deterministic output).
  struct Item {
    uint32_t source;
    int32_t index;  // edge ordinal, or -1 - face ordinal for a silhouette, or INT32_MIN for a mesh body's lines
  };
  std::vector<Item> items;
  int edges = 0;
  for (size_t i = 0; i < sources.size(); ++i) {
    const KeyInfo& k = *cx.keys[i];
    if (sources[i].mesh) {
      items.push_back({static_cast<uint32_t>(i), std::numeric_limits<int32_t>::min()});
      continue;
    }
    for (int e = 0; e < static_cast<int>(k.e.size()); ++e) items.push_back({static_cast<uint32_t>(i), e}), ++edges;
    if (spec.silhouettes)
      for (int f = 0; f < static_cast<int>(k.f.size()); ++f)
        if (k.f[static_cast<size_t>(f)] != Surface::Plane) items.push_back({static_cast<uint32_t>(i), -1 - f});
  }
  const size_t chunk = 64, chunks = (items.size() + chunk - 1) / chunk;
  std::vector<std::vector<Curve>> results(chunks);
  done = 0;
  std::atomic<long long> edge_cpu{0}, outline_cpu{0};  // microseconds summed over the workers
  OSD_Parallel::For(0, static_cast<int>(chunks), [&](int c) {
    if (run.cancelled()) return;
    auto& mine = results[static_cast<size_t>(c)];
    for (size_t i = static_cast<size_t>(c) * chunk; i < std::min(items.size(), static_cast<size_t>(c + 1) * chunk); ++i) {
      const Item& it = items[i];
      const auto start = clock::now();
      try {
        if (it.index == std::numeric_limits<int32_t>::min()) mesh_body_lines(cx, it.source, mine);
        else if (it.index >= 0) edge_work(cx, it.source, it.index, mine);
        else if (!analytic_silhouette(cx, it.source, -1 - it.index, mine)) mesh_silhouette(cx, it.source, -1 - it.index, mine);
      } catch (const Standard_Failure&) {
      } catch (const std::exception&) {
      }
      (it.index >= 0 ? edge_cpu : outline_cpu) += std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count();
    }
    const size_t n = done += std::min(chunk, items.size() - static_cast<size_t>(c) * chunk);
    run.report(0.5 + 0.5 * static_cast<double>(n) / static_cast<double>(items.size()), "hidden lines: edges " + std::to_string(n) + "/" + std::to_string(items.size()));
  });
  run.check();
  size_t total = out.curves.size();
  for (const auto& r : results) total += r.size();
  out.curves.reserve(total);
  for (auto& r : results) std::move(r.begin(), r.end(), std::back_inserter(out.curves));
  out.stats["edges"] = edges;
  out.stats["faces"] = faces;
  out.stats["edges_ms"] = ms(t);
  out.stats["edges_cpu_ms"] = edge_cpu.load() / 1000;
  out.stats["silhouettes_cpu_ms"] = outline_cpu.load() / 1000;
}

}  // namespace opad::drawing::detail
