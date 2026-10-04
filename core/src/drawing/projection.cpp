// Hidden-line projection (TODO 11 UI-77): the API, typed curves, the cache, and the exact and draft tiers. The hybrid
// tier is in hybrid.cpp.
#include "opad/drawing/projection.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Geom2dAdaptor_Curve.hxx>
#include <Geom2dConvert_ApproxCurve.hxx>
#include <Geom2dConvert_BSplineCurveToBezierCurve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_BezierCurve.hxx>
#include <GeomConvert.hxx>
#include <GeomConvert_ApproxCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BezierCurve.hxx>
#include <HLRAlgo_EdgeIterator.hxx>
#include <HLRAlgo_Projector.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_Data.hxx>
#include <HLRBRep_EdgeData.hxx>
#include <HLRBRep_FaceData.hxx>
#include <HLRBRep_FaceIterator.hxx>
#include <HLRBRep_PolyAlgo.hxx>
#include <HLRBRep_ShapeBounds.hxx>
#include <NCollection_DataMap.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt2d.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ShapeMapHasher.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_GTrsf.hxx>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <list>
#include <set>
#include <unordered_map>

#include "opad/cache.hpp"
#include "opad/geometry.hpp"
#include "projection_internal.hpp"

namespace opad::drawing {

using detail::Run;
using detail::Source;
using detail::View;

namespace {

constexpr const char* kAlgorithm = "proj-6";  // part of every fingerprint: bump when the output of any tier changes
constexpr double kTwoPi = 2 * M_PI;

Vec2 operator+(Vec2 a, Vec2 b) { return {a[0] + b[0], a[1] + b[1]}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a[0] - b[0], a[1] - b[1]}; }
Vec2 operator*(Vec2 a, double s) { return {a[0] * s, a[1] * s}; }
double dot(Vec2 a, Vec2 b) { return a[0] * b[0] + a[1] * b[1]; }
double norm(Vec2 a) { return std::hypot(a[0], a[1]); }

json vec_json(const Vec3& v) { return json::array({v[0], v[1], v[2]}); }
Vec3 vec_of(const json& j, const Vec3& fallback) {
  if (!j.is_array() || j.size() != 3) return fallback;
  return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

View view_of(const ViewSpec& s) {
  gp_Vec z(s.dir[0], s.dir[1], s.dir[2]);
  if (z.Magnitude() < 1e-12) throw Error("project: the view direction is zero");
  z.Normalize();
  const gp_Vec forward = -z;
  gp_Vec up(s.up[0], s.up[1], s.up[2]);
  // As the renderer and the viewport: right = forward x up, then up again from right and forward.
  if (up.Magnitude() < 1e-12 || std::fabs(up.Normalized().Dot(forward)) > 0.999) up = std::fabs(forward.Z()) > 0.9 ? gp_Vec(0, 1, 0) : gp_Vec(0, 0, 1);
  const gp_Vec x = forward.Crossed(up).Normalized();
  const gp_Vec y = x.Crossed(forward);
  return {gp_Dir(x), gp_Dir(y), gp_Dir(z)};
}

// Arc, ellipse or segment from a conic: centre `c` and the world vectors to the points at parameters 0 and pi/2.
void conic(const gp_Pnt& centre, const gp_XYZ& p3, const gp_XYZ& q3, double t0, double t1, const View& v, const Curve& like, std::vector<Curve>& out) {
  const Vec2 c = v.at(centre), p = v.along(p3), q = v.along(q3);
  // P(t) = c + p cos t + q sin t; turned by th so that u and w are the principal semi-axes: P = c + u cos(t-th) + w sin(t-th).
  double th = 0.5 * std::atan2(2 * dot(p, q), dot(p, p) - dot(q, q));
  Vec2 u = p * std::cos(th) + q * std::sin(th), w = q * std::cos(th) - p * std::sin(th);
  if (norm(w) > norm(u)) {
    th += M_PI / 2;
    const Vec2 old = u;
    u = w;
    w = old * -1;
  }
  const double a = norm(u), b = norm(w);
  if (a < 1e-12) return;
  if (b < 1e-9 * a) {  // seen edge-on: a segment along u, as far as cos(t - th) reaches over the range
    const double s0 = t0 - th, s1 = t1 - th;
    double lo = std::min(std::cos(s0), std::cos(s1)), hi = std::max(std::cos(s0), std::cos(s1));
    if (std::floor(s1 / kTwoPi) > std::floor(s0 / kTwoPi) || s1 - s0 >= kTwoPi) hi = 1;
    if (std::floor((s1 - M_PI) / kTwoPi) > std::floor((s0 - M_PI) / kTwoPi) || s1 - s0 >= kTwoPi) lo = -1;
    if ((hi - lo) * a < 1e-9) return;
    Curve k = like;
    k.type = Curve::Type::Line;
    k.pts = {c + u * lo, c + u * hi};
    out.push_back(std::move(k));
    return;
  }
  const double rot = std::atan2(u[1], u[0]);
  const bool ccw = u[0] * w[1] - u[1] * w[0] > 0;  // else the conic runs clockwise on the sheet: its parameter is mirrored
  double s0 = ccw ? t0 - th : th - t1;
  Curve k = like;
  k.c = c;
  if (std::fabs(a - b) <= 1e-9 * a) {
    k.type = Curve::Type::Arc;
    k.r1 = k.r2 = a;
    s0 += rot;
  } else {
    k.type = Curve::Type::Ellipse;
    k.r1 = a;
    k.r2 = b;
    k.rot = rot;
  }
  s0 = std::fmod(s0, kTwoPi);
  if (s0 < 0) s0 += kTwoPi;
  k.a0 = s0;
  k.a1 = s0 + std::min(t1 - t0, kTwoPi);
  out.push_back(std::move(k));
}

// A B-spline's piece with its poles projected (exact: an orthographic projection is affine, weights unchanged).
bool spline(Handle(Geom_BSplineCurve) s, double t0, double t1, const View& v, const Curve& like, std::vector<Curve>& out) {
  if (s.IsNull()) return false;
  if (s->IsPeriodic()) {
    if (t1 - t0 < s->Period() - 1e-9) s->Segment(t0, t1);
    if (s->IsPeriodic()) s->SetNotPeriodic();
  } else {
    t0 = std::max(t0, s->FirstParameter());
    t1 = std::min(t1, s->LastParameter());
    if (t1 - t0 < 1e-12) return true;
    if (t0 > s->FirstParameter() + 1e-12 || t1 < s->LastParameter() - 1e-12) s->Segment(t0, t1);
  }
  Curve k = like;
  k.type = Curve::Type::Spline;
  k.degree = s->Degree();
  for (int i = 1; i <= s->NbPoles(); ++i) k.pts.push_back(v.at(s->Pole(i)));
  if (s->IsRational())
    for (int i = 1; i <= s->NbPoles(); ++i) k.weights.push_back(s->Weight(i));
  const TColStd_Array1OfReal& flat = s->KnotSequence();
  for (int i = flat.Lower(); i <= flat.Upper(); ++i) k.knots.push_back(flat(i));
  out.push_back(std::move(k));
  return true;
}

int arc_steps(double r, double span, double tol) {
  const double h = r > tol ? 2 * std::acos(std::max(-1.0, 1 - tol / r)) : M_PI / 4;
  return std::clamp(static_cast<int>(std::ceil(span / std::max(h, 1e-6))), 2, 8192);
}

// How far the cubic of a unit circle's arc of angle h (controls 4/3 tan(h/4) along the tangents) strays from the circle;
// an ellipse's pieces are its affine images, at most r1 times as far off.
double unit_arc_error(double h) {
  const double k = 4.0 / 3 * std::tan(h / 4);
  const Vec2 p0{1, 0}, p1{1, k}, p2{std::cos(h) + k * std::sin(h), std::sin(h) - k * std::cos(h)}, p3{std::cos(h), std::sin(h)};
  double worst = 0;
  for (int i = 1; i < 32; ++i) {
    const double t = i / 32.0, s = 1 - t;
    worst = std::max(worst, std::fabs(norm(p0 * (s * s * s) + p1 * (3 * s * s * t) + p2 * (3 * s * t * t) + p3 * (t * t * t)) - 1));
  }
  return worst;
}

}  // namespace

Handle(Geom2d_BSplineCurve) detail::curve2d(const Curve& k) {
  const int n = static_cast<int>(k.pts.size());
  if (n < 2 || k.degree < 1 || static_cast<int>(k.knots.size()) != n + k.degree + 1) return nullptr;
  std::vector<double> knots;
  std::vector<int> mults;
  for (double t : k.knots) {
    if (!knots.empty() && t - knots.back() <= 1e-12) ++mults.back();
    else knots.push_back(t), mults.push_back(1);
  }
  TColgp_Array1OfPnt2d poles(1, n);
  for (int i = 0; i < n; ++i) poles(i + 1) = gp_Pnt2d(k.pts[static_cast<size_t>(i)][0], k.pts[static_cast<size_t>(i)][1]);
  TColStd_Array1OfReal kv(1, static_cast<int>(knots.size()));
  TColStd_Array1OfInteger mv(1, static_cast<int>(knots.size()));
  for (size_t i = 0; i < knots.size(); ++i) kv(static_cast<int>(i) + 1) = knots[i], mv(static_cast<int>(i) + 1) = mults[i];
  if (k.weights.size() == static_cast<size_t>(n)) {
    TColStd_Array1OfReal wv(1, n);
    for (int i = 0; i < n; ++i) wv(i + 1) = k.weights[static_cast<size_t>(i)];
    return new Geom2d_BSplineCurve(poles, wv, kv, mv, k.degree);
  }
  return new Geom2d_BSplineCurve(poles, kv, mv, k.degree);
}

namespace {

// ---- gathering, fingerprint, tier

TopoDS_Shape placed_copy(const Source& s, const TopoDS_Shape& copy) {
  if (s.rigid) return copy.Moved(TopLoc_Location(s.trsf));
  gp_GTrsf g;
  for (int r = 1; r <= 3; ++r) {
    for (int c = 1; c <= 3; ++c) g.SetValue(r, c, s.world.at(r - 1, c - 1));
    g.SetValue(r, 4, s.world.at(r - 1, 3));
  }
  return BRepBuilderAPI_GTransform(copy, g, Standard_True).Shape();
}

std::mutex g_faces_mu;
std::unordered_map<std::string, int> g_faces;  // body key -> face count (keys are content addresses: valid everywhere)

int known_faces(const std::string& key) {
  std::lock_guard<std::mutex> lock(g_faces_mu);
  auto it = g_faces.find(key);
  return it == g_faces.end() ? -1 : it->second;
}

// The view's body nodes and their placements; no geometry yet (a cached result needs none).
std::vector<Source> gather(const Scene& scene, const ViewSpec& spec) {
  std::vector<std::string> ids;
  if (spec.nodes.empty()) ids = scene.all_bodies();
  for (const auto& n : spec.nodes) {
    if (!scene.node(n)) throw Error("project: unknown node " + n);
    for (const auto& b : scene.bodies_under(n)) ids.push_back(b);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::set<std::string> hidden, whole;
  for (const auto& h : spec.hide)
    for (const auto& b : scene.bodies_under(h)) hidden.insert(b);
  for (const auto& h : spec.whole)
    if (scene.node(h))
      for (const auto& b : scene.bodies_under(h)) whole.insert(b);
  std::vector<Source> out;
  for (const auto& id : ids) {
    const Node* n = scene.node(id);
    if (!n || n->kind != Node::Kind::Body || n->body_missing || n->representation == "drawing2d" || hidden.count(id)) continue;
    if (spec.visible_only && !scene.effectively_visible(id)) continue;
    Source s;
    s.node = id;
    s.key = n->body_key;
    s.world = scene.world(id);
    if (!spec.offsets.empty()) {
      Vec3 shift{0, 0, 0};
      for (const auto& p : scene.path_to(id))
        if (auto it = spec.offsets.find(p); it != spec.offsets.end())
          for (int k = 0; k < 3; ++k) shift[static_cast<size_t>(k)] += it->second[static_cast<size_t>(k)];
      s.world = Mat4::translation(shift[0], shift[1], shift[2]) * s.world;
    }
    s.rigid = mat_is_rigid(s.world);
    s.mesh = n->representation == "mesh";
    s.whole = whole.count(id) > 0;
    if (s.rigid) s.trsf = trsf_from_mat(s.world);
    out.push_back(std::move(s));
  }
  return out;
}

// Parses the bodies' BREP side by side (a document straight from disk has none parsed) and counts their faces;
// with `shapes` every source also gets its shape and placement, else only keys without a face count are read.
void load(const Document& doc, std::vector<Source>& sources, bool shapes) {
  std::vector<std::string> keys;
  for (const auto& s : sources)
    if (shapes || known_faces(s.key) < 0) keys.push_back(s.key);
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  OSD_Parallel::For(0, static_cast<int>(keys.size()), [&](int i) {
    try {
      const auto& key = keys[static_cast<size_t>(i)];
      const TopoDS_Shape proto = body_shape(doc, key);
      if (known_faces(key) >= 0) return;
      TopTools_IndexedMapOfShape faces;
      TopExp::MapShapes(proto, TopAbs_FACE, faces);
      std::lock_guard<std::mutex> lock(g_faces_mu);
      g_faces[key] = faces.Extent();
    } catch (const std::exception&) {
    } catch (const Standard_Failure&) {
    }
  });
  if (!shapes) return;
  for (auto& s : sources) {
    s.proto = body_shape(doc, s.key);
    s.mesh = s.mesh || is_mesh_shape(s.proto);
    s.placed = placed_copy(s, s.proto);
  }
}

Quality auto_tier(const Document& doc, std::vector<Source>& sources, const ViewSpec& spec) {
  if (spec.quality != Quality::Auto) return spec.quality;
  load(doc, sources, false);
  long faces = 0;
  for (const auto& s : sources) {
    if (s.mesh) return Quality::Hybrid;  // exact HLR needs surfaces
    faces += std::max(0, known_faces(s.key));
  }
  return faces <= spec.exact_faces ? Quality::Exact : Quality::Hybrid;
}

std::string fingerprint_of(const std::vector<Source>& sources, const ViewSpec& spec, Quality tier) {
  const View v = view_of(spec);
  auto rounded = [](double x) { return std::round(x * 1e9) / 1e9; };
  json j = {{"algorithm", kAlgorithm}, {"tier", quality_name(tier)},
            {"x", {rounded(v.x.X()), rounded(v.x.Y()), rounded(v.x.Z())}}, {"y", {rounded(v.y.X()), rounded(v.y.Y()), rounded(v.y.Z())}},
            {"hidden", spec.hidden}, {"tangent", spec.tangent}, {"silhouettes", spec.silhouettes}, {"seams", spec.seams}, {"tolerance", spec.tolerance}};
  if (tier == Quality::Hybrid) j["resolution"] = spec.resolution;
  if (!spec.cut.empty()) {
    json line = json::array();
    for (const auto& p : spec.cut) line.push_back({rounded(p[0]), rounded(p[1])});
    j["cut"] = {{"line", line}, {"x", {rounded(spec.cut_x[0]), rounded(spec.cut_x[1]), rounded(spec.cut_x[2])}},
                {"y", {rounded(spec.cut_y[0]), rounded(spec.cut_y[1]), rounded(spec.cut_y[2])}}, {"version", 1}};
    if (spec.aligned) j["cut"]["aligned"] = true;
    for (const auto& s : sources)
      if (s.whole) j["cut"]["whole"].push_back(s.node);
  }
  if (!spec.breakouts.empty()) {
    json all = json::array();
    for (const auto& b : spec.breakouts) {
      json outline = json::array();
      for (const auto& p : b.outline) outline.push_back({rounded(p[0]), rounded(p[1])});
      all.push_back({{"outline", outline}, {"depth", rounded(b.depth)}});
    }
    j["breakouts"] = {{"cuts", all}, {"version", 1}};
    for (const auto& s : sources)
      if (s.whole) j["breakouts"]["whole"].push_back(s.node);
  }
  json bodies = json::array();
  for (const auto& s : sources) {
    json m = json::array();
    for (double x : s.world.m) m.push_back(rounded(x));
    bodies.push_back({s.node, s.key, m});
  }
  j["bodies"] = bodies;
  return sha256_hex(j.dump());
}

// ---- exact tier: HLRBRep_Algo over copies (it encodes regularity and builds outlines on the shapes it is given, and
// the body store's shapes are read by other threads). Edges come back split where outlines cross them: a piece is
// traced to its body edge through the shape, else through its 3D curve and parameter.

struct EdgeIndex {
  TopTools_IndexedMapOfShape edges, faces;
  std::unordered_map<const Geom_Curve*, std::vector<std::array<double, 3>>> curves;  // curve -> (ordinal, first, last)
  std::unordered_map<const Geom_Surface*, int> surfaces;
  void build(const TopoDS_Shape& placed) {
    TopExp::MapShapes(placed, TopAbs_EDGE, edges);
    TopExp::MapShapes(placed, TopAbs_FACE, faces);
    for (int i = 1; i <= edges.Extent(); ++i) {
      double f, l;
      TopLoc_Location loc;
      const Handle(Geom_Curve) c = BRep_Tool::Curve(TopoDS::Edge(edges(i)), loc, f, l);
      if (!c.IsNull()) curves[c.get()].push_back({static_cast<double>(i - 1), std::min(f, l), std::max(f, l)});
    }
    for (int i = faces.Extent(); i >= 1; --i) {
      TopLoc_Location loc;
      const Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(faces(i)), loc);
      if (!s.IsNull()) surfaces[s.get()] = i - 1;  // the first face on a shared surface wins
    }
  }
  int edge(const TopoDS_Shape& e, double mid) const {
    if (const int i = edges.FindIndex(e); i > 0) return i - 1;
    double f, l;
    TopLoc_Location loc;
    const Handle(Geom_Curve) c = BRep_Tool::Curve(TopoDS::Edge(e), loc, f, l);
    if (c.IsNull()) return -1;
    auto it = curves.find(c.get());
    if (it == curves.end()) return -1;
    for (const auto& [ordinal, first, last] : it->second)
      if (mid >= first - 1e-9 && mid <= last + 1e-9) return static_cast<int>(ordinal);
    return static_cast<int>(it->second.front()[0]);
  }
  int face(const TopoDS_Shape& f) const {
    if (const int i = faces.FindIndex(f); i > 0) return i - 1;
    TopLoc_Location loc;
    const Handle(Geom_Surface) s = BRep_Tool::Surface(TopoDS::Face(f), loc);
    auto it = s.IsNull() ? surfaces.end() : surfaces.find(s.get());
    return it == surfaces.end() ? -1 : it->second;
  }
};

void exact(const std::vector<Source>& sources, const ViewSpec& spec, const View& v, Run& run, ViewGeometry& out) {
  run.report(0, "hidden lines: preparing", true);
  std::map<std::string, TopoDS_Shape> copies;
  for (const auto& s : sources)
    if (!copies.count(s.key)) copies[s.key] = BRepBuilderAPI_Copy(s.proto, Standard_True, Standard_False).Shape();
  Handle(HLRBRep_Algo) algo = new HLRBRep_Algo();
  std::vector<EdgeIndex> index(sources.size());
  std::vector<TopoDS_Shape> placed(sources.size());
  for (size_t i = 0; i < sources.size(); ++i) {
    placed[i] = placed_copy(sources[i], copies.at(sources[i].key));
    index[i].build(placed[i]);
    algo->Add(placed[i]);
  }
  run.check();
  run.report(-1, "hidden lines: exact", true);
  algo->Projector(HLRAlgo_Projector(gp_Ax2(gp_Pnt(0, 0, 0), v.z, v.x)));
  algo->Update();
  run.check();
  algo->Hide();  // monolithic: a cancel lands after it
  run.check();
  Handle(HLRBRep_Data) ds = algo->DataStructure();
  if (ds.IsNull()) return;
  const int ne = ds->NbEdges(), nf = ds->NbFaces();
  std::vector<int> edge_owner(static_cast<size_t>(ne) + 1, -1), face_owner(static_cast<size_t>(nf) + 1, -1);
  for (size_t i = 0; i < sources.size(); ++i) {
    const int at = algo->Index(placed[i]);
    if (at <= 0) continue;
    int v1, v2, e1, e2, f1, f2;
    algo->ShapeBounds(at).Bounds(v1, v2, e1, e2, f1, f2);
    for (int e = std::max(1, e1); e <= std::min(ne, e2); ++e) edge_owner[static_cast<size_t>(e)] = static_cast<int>(i);
    for (int f = std::max(1, f1); f <= std::min(nf, f2); ++f) face_owner[static_cast<size_t>(f)] = static_cast<int>(i);
  }
  // What the faces say about each edge (as HLRBRep_HLRToShape draws them): an outline flag on a smooth edge makes it a
  // contour; internal edges are the outlines the algorithm added inside a face; iso lines are never drawn.
  enum : uint8_t { kOutline = 1, kInternal = 2, kIso = 4 };
  std::vector<uint8_t> flags(static_cast<size_t>(ne) + 1, 0);
  std::vector<int> inside(static_cast<size_t>(ne) + 1, 0);
  for (int f = 1; f <= nf; ++f) {
    HLRBRep_FaceIterator it;
    for (it.InitEdge(ds->FDataArray().ChangeValue(f)); it.MoreEdge(); it.NextEdge()) {
      const size_t e = static_cast<size_t>(it.Edge());
      if (e > static_cast<size_t>(ne)) continue;
      flags[e] |= (it.OutLine() ? kOutline : 0) | (it.Internal() ? kInternal : 0) | (it.IsoLine() ? kIso : 0);
      if (it.Internal()) inside[e] = f;
    }
  }
  for (int ie = 1; ie <= ne; ++ie) {
    HLRBRep_EdgeData& ed = ds->EDataArray().ChangeValue(ie);
    const uint8_t fl = flags[static_cast<size_t>(ie)];
    if (!ed.Selected() || ed.Vertical() || (fl & kIso)) continue;
    Curve like;
    like.body = edge_owner[static_cast<size_t>(ie)];
    if (fl & kInternal) like.kind = Curve::Kind::Silhouette;
    else if (ed.Rg1Line() && !(fl & kOutline)) like.kind = ed.RgNLine() ? Curve::Kind::Seam : Curve::Kind::Tangent;
    else like.kind = ed.Rg1Line() ? Curve::Kind::Silhouette : Curve::Kind::Sharp;
    if ((like.kind == Curve::Kind::Tangent && !spec.tangent) || (like.kind == Curve::Kind::Seam && !spec.seams) ||
        ((fl & kInternal) && !spec.silhouettes))
      continue;
    BRepAdaptor_Curve& curve = ed.ChangeGeometry().Curve();
    if (like.body >= 0) {
      const auto& idx = index[static_cast<size_t>(like.body)];
      if (fl & kInternal) {
        const int f = inside[static_cast<size_t>(ie)];
        if (f > 0 && face_owner[static_cast<size_t>(f)] == like.body) like.face = idx.face(ds->FaceMap()(f));
      } else {
        like.edge = idx.edge(ds->EdgeMap()(ie), 0.5 * (curve.FirstParameter() + curve.LastParameter()));
      }
    }
    HLRAlgo_EdgeIterator it;
    double s, e;
    Standard_ShortReal ts, te;
    for (it.InitVisible(ed.Status()); it.MoreVisible(); it.NextVisible()) {
      it.Visible(s, ts, e, te);
      detail::emit(curve, s, e, v, like, spec.tolerance, out.curves);
    }
    if (!spec.hidden) continue;
    like.hidden = true;
    for (it.InitHidden(ed.Status()); it.MoreHidden(); it.NextHidden()) {
      it.Hidden(s, ts, e, te);
      detail::emit(curve, s, e, v, like, spec.tolerance, out.curves);
    }
  }
}

// ---- draft tier: HLRBRep_PolyAlgo over meshed copies; mesh-fragmented segments, joined into polylines per edge.

void draft(const Document& doc, const std::vector<Source>& sources, const ViewSpec& spec, const View& v, Run& run, ViewGeometry& out) {
  std::vector<std::string> keys;
  for (const auto& s : sources)
    if (std::find(keys.begin(), keys.end(), s.key) == keys.end()) keys.push_back(s.key);
  std::vector<TopoDS_Shape> meshed(keys.size());
  std::atomic<size_t> done{0};
  OSD_Parallel::For(0, static_cast<int>(keys.size()), [&](int i) {
    if (run.cancelled()) return;
    const auto& s = *std::find_if(sources.begin(), sources.end(), [&](const Source& x) { return x.key == keys[static_cast<size_t>(i)]; });
    TopoDS_Shape copy = BRepBuilderAPI_Copy(s.proto, Standard_True, s.mesh).Shape();
    if (!s.mesh) mesh_shape(copy, detail::deflection_for(doc, s.body_key()));
    meshed[static_cast<size_t>(i)] = copy;
    run.report(static_cast<double>(++done) / static_cast<double>(keys.size()) * 0.5, "hidden lines: meshing " + std::to_string(done.load()) + "/" + std::to_string(keys.size()));
  });
  run.check();
  Handle(HLRBRep_PolyAlgo) poly = new HLRBRep_PolyAlgo();
  // An edge's pieces name the edge; an internal outline (a silhouette inside a face) names the face.
  NCollection_DataMap<TopoDS_Shape, std::pair<int, int>, TopTools_ShapeMapHasher> where, where_face;
  for (size_t i = 0; i < sources.size(); ++i) {
    const size_t k = static_cast<size_t>(std::find(keys.begin(), keys.end(), sources[i].key) - keys.begin());
    const TopoDS_Shape placed = placed_copy(sources[i], meshed[k]);
    TopTools_IndexedMapOfShape edges, faces;
    TopExp::MapShapes(placed, TopAbs_EDGE, edges);
    TopExp::MapShapes(placed, TopAbs_FACE, faces);
    for (int e = 1; e <= edges.Extent(); ++e) where.Bind(edges(e), {static_cast<int>(i), e - 1});
    for (int f = 1; f <= faces.Extent(); ++f) where_face.Bind(faces(f), {static_cast<int>(i), f - 1});
    poly->Load(placed);
  }
  run.report(0.5, "hidden lines: polygonal", true);
  poly->Projector(HLRAlgo_Projector(gp_Ax2(gp_Pnt(0, 0, 0), v.z, v.x)));
  poly->Update();
  run.check();
  struct Piece {
    Curve like;
    Vec2 a, b;
  };
  std::vector<Piece> pieces;
  for (poly->InitHide(); poly->MoreHide(); poly->NextHide()) {
    HLRAlgo_EdgeStatus status;
    TopoDS_Shape shape;
    Standard_Boolean reg1, regn, outline, internal;
    const HLRAlgo_BiPoint::PointsT& p = poly->Hide(status, shape, reg1, regn, outline, internal);
    Curve like;
    like.type = Curve::Type::Polyline;
    // As the exact tier: a sharp edge on the outline stays sharp, a smooth one there is a silhouette.
    if (internal) like.kind = Curve::Kind::Silhouette;
    else if (reg1 && !outline) like.kind = regn ? Curve::Kind::Seam : Curve::Kind::Tangent;
    else if (reg1) like.kind = Curve::Kind::Silhouette;
    if ((like.kind == Curve::Kind::Tangent && !spec.tangent) || (like.kind == Curve::Kind::Seam && !spec.seams) || (internal && !spec.silhouettes)) continue;
    if (const auto* w = (internal ? where_face : where).Seek(shape)) {
      like.body = w->first;
      (internal ? like.face : like.edge) = w->second;
    }
    const Vec2 a{p.PntP1.X(), p.PntP1.Y()}, b{p.PntP2.X(), p.PntP2.Y()};
    like.z = 0.5 * (p.PntP1.Z() + p.PntP2.Z());  // the projector's z points towards the viewer
    HLRAlgo_EdgeIterator it;
    double s, e;
    Standard_ShortReal ts, te;
    for (it.InitVisible(status); it.MoreVisible(); it.NextVisible()) {
      it.Visible(s, ts, e, te);
      pieces.push_back({like, a + (b - a) * s, a + (b - a) * e});
    }
    if (!spec.hidden) continue;
    like.hidden = true;
    for (it.InitHidden(status); it.MoreHidden(); it.NextHidden()) {
      it.Hidden(s, ts, e, te);
      pieces.push_back({like, a + (b - a) * s, a + (b - a) * e});
    }
  }
  // A segment comes once for its edge and again as an outline: repeats are dropped (edges first), then each edge's
  // segments are joined into polylines.
  std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& x, const Piece& y) {
    return (x.like.face >= 0) < (y.like.face >= 0);
  });
  using Key = std::pair<long long, long long>;
  auto key = [](const Vec2& p) { return Key{std::llround(p[0] * 1e6), std::llround(p[1] * 1e6)}; };
  std::set<std::tuple<Key, Key, bool>> seen;
  std::map<std::tuple<int, int, int, int, bool>, std::vector<size_t>> groups;
  for (size_t i = 0; i < pieces.size(); ++i) {
    Key ka = key(pieces[i].a), kb = key(pieces[i].b);
    if (ka == kb) continue;
    if (kb < ka) std::swap(ka, kb);
    if (!seen.insert({ka, kb, pieces[i].like.hidden}).second) continue;
    const Curve& l = pieces[i].like;
    groups[{l.body, l.edge, l.face, static_cast<int>(l.kind), l.hidden}].push_back(i);
  }
  for (const auto& [group, members] : groups) {
    std::map<Key, std::vector<size_t>> at;
    for (size_t i : members) at[key(pieces[i].a)].push_back(i), at[key(pieces[i].b)].push_back(i);
    std::set<size_t> used;
    for (size_t i : members) {
      if (!used.insert(i).second) continue;
      std::deque<Vec2> line{pieces[i].a, pieces[i].b};
      for (int end = 0; end < 2; ++end)
        for (Key k = key(end ? line.front() : line.back());;) {
          size_t next = SIZE_MAX;
          for (size_t j : at[k])
            if (!used.count(j)) { next = j; break; }
          if (next == SIZE_MAX) break;
          used.insert(next);
          const Vec2 p = key(pieces[next].a) == k ? pieces[next].b : pieces[next].a;
          if (end) line.push_front(p);
          else line.push_back(p);
          k = key(p);
        }
      Curve k = pieces[i].like;
      k.pts.assign(line.begin(), line.end());
      out.curves.push_back(std::move(k));
    }
  }
}

// ---- coincident pieces: where lines, arcs, ellipses or identical splines lie on one another, the nearest visible piece
// is kept, and hidden ones only where no visible one lies (a visible line is drawn over a hidden one). Exact HLR returns
// an edge-on circle as two coincident segments, a box's back edges lie under its front ones, and the hybrid tier sees
// the outline of an identical part right behind another one as visible.

using Span = std::pair<double, double>;

// [a, b] less the covered spans (sorted, disjoint); pieces under eps are dropped.
std::vector<Span> uncovered(double a, double b, const std::vector<Span>& covered, double eps) {
  std::vector<Span> out;
  for (const auto& [c0, c1] : covered) {
    if (c1 <= a) continue;
    if (c0 >= b) break;
    if (c0 - a > eps) out.push_back({a, c0});
    a = std::max(a, c1);
  }
  if (b - a > eps) out.push_back({a, b});
  return out;
}

void cover(std::vector<Span>& covered, const Span& s) {
  covered.push_back(s);
  std::sort(covered.begin(), covered.end());
  std::vector<Span> merged;
  for (const auto& c : covered)
    if (!merged.empty() && c.first <= merged.back().second) merged.back().second = std::max(merged.back().second, c.second);
    else merged.push_back(c);
  covered.swap(merged);
}

size_t root(std::vector<size_t>& up, size_t i) {
  while (up[i] != i) i = up[i] = up[up[i]];
  return i;
}

// Joins the items `same` says coincide. Candidates share a bucket of width q on k0 (twice, shifted by half a bucket, so
// values closer than q/2 always share one) and lie within w of each other on k1, which may depend on the bucket's centre.
template <class K0, class K1, class Same>
void join_close(const std::vector<size_t>& items, double q, double w, K0 k0, K1 k1, Same same, std::vector<size_t>& up) {
  for (double shift : {0.0, 0.5}) {
    std::vector<std::pair<long long, size_t>> b;
    b.reserve(items.size());
    for (size_t i : items) b.push_back({static_cast<long long>(std::floor(k0(i) / q + shift)), i});
    std::sort(b.begin(), b.end());
    std::vector<std::pair<double, size_t>> v;
    for (size_t s = 0, e = 0; s < b.size(); s = e) {
      for (e = s; e < b.size() && b[e].first == b[s].first; ++e) {
      }
      if (e - s < 2) continue;
      const double centre = (static_cast<double>(b[s].first) - shift + 0.5) * q;
      v.clear();
      for (size_t x = s; x < e; ++x) v.push_back({k1(b[x].second, centre), b[x].second});
      std::sort(v.begin(), v.end());
      for (size_t x = 0; x < v.size(); ++x)
        for (size_t y = x + 1; y < v.size() && v[y].first - v[x].first <= w; ++y) {
          const size_t rx = root(up, v[x].second), ry = root(up, v[y].second);
          if (rx != ry && same(v[x].second, v[y].second)) up[std::max(rx, ry)] = std::min(rx, ry);
        }
    }
  }
}

double cross(Vec2 a, Vec2 b) { return a[0] * b[1] - a[1] * b[0]; }

// Returns how many curves were cut back or dropped.
size_t drop_overlaps(std::vector<Curve>& curves, double eps) {
  enum Class { kLine, kArc, kEllipse, kSpline };
  const size_t n = curves.size();
  std::vector<int> cls(n, -1);
  std::vector<Vec2> dir(n, Vec2{0, 0});  // lines: unit direction, pointing up (angle in [0, pi))
  std::vector<double> angle(n, 0);
  std::vector<size_t> items[4];
  double size = 0;  // of the lines: how far their middles may slide across a bucket's direction
  std::unordered_map<size_t, std::vector<Curve>> replaced;  // curve -> what is left of it
  for (size_t i = 0; i < n; ++i) {
    const Curve& k = curves[i];
    double length = -1;  // lines, arcs and polylines shorter than eps are dropped: nothing on paper
    if (k.type == Curve::Type::Arc) length = k.r1 * (k.a1 - k.a0);
    if (k.type == Curve::Type::Line || k.type == Curve::Type::Polyline) {
      length = 0;
      for (size_t p = 1; p < k.pts.size(); ++p) length += norm(k.pts[p] - k.pts[p - 1]);
    }
    if (length >= 0 && length < eps) {
      replaced[i];
      continue;
    }
    if (k.type == Curve::Type::Line || (k.type == Curve::Type::Polyline && k.pts.size() == 2)) {
      const Vec2 d = k.pts[1] - k.pts[0];
      const double l = norm(d);  // at least eps
      for (const auto& p : k.pts) size = std::max({size, std::fabs(p[0]), std::fabs(p[1])});
      Vec2 u = d * (1 / l);
      if (u[1] < 0 || (u[1] == 0 && u[0] < 0)) u = u * -1;
      dir[i] = u;
      angle[i] = std::atan2(u[1], u[0]);
      if (angle[i] > M_PI - 1e-3) angle[i] -= M_PI;  // nearly horizontal both ways: one bucket
      cls[i] = kLine;
    } else if (k.type == Curve::Type::Arc && k.r1 > eps) {
      cls[i] = kArc;
    } else if (k.type == Curve::Type::Ellipse && k.r2 > eps) {
      cls[i] = kEllipse;
    } else if (k.type == Curve::Type::Spline && !k.pts.empty()) {
      cls[i] = kSpline;
    }
    if (cls[i] >= 0) items[cls[i]].push_back(i);
  }
  std::vector<size_t> up(n);
  for (size_t i = 0; i < n; ++i) up[i] = i;
  auto x0 = [&](size_t i) { return curves[i].type == Curve::Type::Spline ? curves[i].pts[0][0] : curves[i].c[0]; };
  auto y0 = [&](size_t i, double) { return curves[i].type == Curve::Type::Spline ? curves[i].pts[0][1] : curves[i].c[1]; };
  auto close = [&](double a, double b) { return std::fabs(a - b) <= eps; };
  // Lines: radians per bucket. Pieces of one edge keep its direction exactly; lines of different parts further apart in
  // direction are left as they are (1e-4 joined 0.1% more on the Engine and took seconds, 1e-6 1% less).
  const double q = 1e-5;
  join_close(items[kLine], q, 2 * size * q + 2 * eps, [&](size_t i) { return angle[i]; },
             [&](size_t i, double centre) {
               const Vec2 m = (curves[i].pts[0] + curves[i].pts[1]) * 0.5;
               return -std::sin(centre) * m[0] + std::cos(centre) * m[1];
             },
             [&](size_t i, size_t j) {
               const Curve &a = curves[i], &b = curves[j];
               const bool shorter = norm(a.pts[1] - a.pts[0]) < norm(b.pts[1] - b.pts[0]);
               const Curve& s = shorter ? a : b;
               const Curve& l = shorter ? b : a;
               const Vec2 u = dir[shorter ? j : i];
               return std::fabs(cross(u, s.pts[0] - l.pts[0])) <= eps && std::fabs(cross(u, s.pts[1] - l.pts[0])) <= eps;
             },
             up);
  join_close(items[kArc], 4 * eps, eps, x0, y0,
             [&](size_t i, size_t j) { return close(curves[i].c[0], curves[j].c[0]) && close(curves[i].c[1], curves[j].c[1]) && close(curves[i].r1, curves[j].r1); },
             up);
  join_close(items[kEllipse], 4 * eps, eps, x0, y0,
             [&](size_t i, size_t j) {
               const Curve &a = curves[i], &b = curves[j];
               return close(a.c[0], b.c[0]) && close(a.c[1], b.c[1]) && close(a.r1, b.r1) && close(a.r2, b.r2) &&
                      std::fabs(std::remainder(a.rot - b.rot, M_PI)) * a.r1 <= eps;
             },
             up);
  join_close(items[kSpline], 4 * eps, eps, x0, y0,
             [&](size_t i, size_t j) {
               const Curve &a = curves[i], &b = curves[j];
               if (a.degree != b.degree || a.pts.size() != b.pts.size() || a.knots.size() != b.knots.size() || a.weights.size() != b.weights.size()) return false;
               for (size_t p = 0; p < a.pts.size(); ++p)
                 if (norm(a.pts[p] - b.pts[p]) > eps) return false;
               for (size_t p = 0; p < a.knots.size(); ++p)
                 if (std::fabs(a.knots[p] - b.knots[p]) > 1e-9 * (1 + std::fabs(a.knots[p]))) return false;
               for (size_t p = 0; p < a.weights.size(); ++p)
                 if (std::fabs(a.weights[p] - b.weights[p]) > 1e-9 * (1 + std::fabs(a.weights[p]))) return false;
               return true;
             },
             up);
  std::unordered_map<size_t, std::vector<size_t>> groups;
  for (size_t i = 0; i < n; ++i)
    if (cls[i] >= 0 && root(up, i) != i) groups[root(up, i)].push_back(i);
  auto rank = [](Curve::Kind k) { return k == Curve::Kind::Sharp ? 0 : k == Curve::Kind::Silhouette ? 1 : k == Curve::Kind::Tangent ? 2 : 3; };
  for (auto& [first, rest] : groups) {
    std::vector<size_t> members{first};
    members.insert(members.end(), rest.begin(), rest.end());
    std::sort(members.begin(), members.end(), [&](size_t a, size_t b) {
      const Curve &x = curves[a], &y = curves[b];
      if (x.hidden != y.hidden) return !x.hidden;
      if (x.z != y.z) return x.z > y.z;
      if (rank(x.kind) != rank(y.kind)) return rank(x.kind) < rank(y.kind);
      return a < b;
    });
    const int c = cls[first];
    // The members' spans on one parameter: lines along the longest one, circles by angle, ellipses by the first one's
    // parameter (another one turned by pi runs half a turn on), splines as a whole.
    size_t longest = members[0];
    for (size_t m : members)
      if (c == kLine && norm(curves[m].pts[1] - curves[m].pts[0]) > norm(curves[longest].pts[1] - curves[longest].pts[0])) longest = m;
    const Vec2 origin = c == kLine ? curves[longest].pts[0] : Vec2{0, 0}, u = dir[longest];
    const double rot = curves[members[0]].rot;
    const double peps = c == kLine ? eps : c == kSpline ? 1e-12 : eps / curves[members[0]].r1;
    std::vector<Span> covered;
    for (size_t m : members) {
      const Curve& k = curves[m];
      double shift = 0, s0 = 0, s1 = 1;
      if (c == kLine) s0 = dot(u, k.pts[0] - origin), s1 = dot(u, k.pts[1] - origin);
      if (c == kEllipse) shift = std::round((k.rot - rot) / M_PI) * M_PI;
      if (c == kArc || c == kEllipse) {
        s0 = std::fmod(k.a0 + shift, kTwoPi);
        if (s0 < 0) s0 += kTwoPi;
        s1 = s0 + std::min(k.a1 - k.a0, kTwoPi);
      }
      const double lo = std::min(s0, s1), hi = std::max(s0, s1);
      std::vector<Span> left;
      if (hi <= kTwoPi || c == kLine || c == kSpline) {
        left = uncovered(lo, hi, covered, peps);
        cover(covered, {lo, hi});
      } else {  // across 2 pi: two spans, joined again after
        left = uncovered(lo, kTwoPi, covered, peps);
        const auto wrap = uncovered(0, hi - kTwoPi, covered, peps);
        cover(covered, {lo, kTwoPi});
        cover(covered, {0, hi - kTwoPi});
        const bool joined = !left.empty() && !wrap.empty() && left.back().second >= kTwoPi - 1e-12 && wrap.front().first <= 1e-12;
        if (joined) left.back().second = kTwoPi + wrap.front().second;
        left.insert(left.end(), wrap.begin() + (joined ? 1 : 0), wrap.end());
      }
      if (left.size() == 1 && std::fabs(left[0].first - lo) <= 1e-12 && std::fabs(left[0].second - hi) <= 1e-12) continue;  // untouched
      auto& out = replaced[m];
      if (c == kLine && s0 > s1) std::reverse(left.begin(), left.end());
      for (const auto& [a, b] : left) {
        Curve piece = k;
        if (c == kLine) {
          auto at = [&](double s) { return k.pts[0] + (k.pts[1] - k.pts[0]) * ((s - s0) / (s1 - s0)); };
          piece.pts = s0 <= s1 ? std::vector<Vec2>{at(a), at(b)} : std::vector<Vec2>{at(b), at(a)};
        } else if (c != kSpline) {
          piece.a0 = std::fmod(a - shift, kTwoPi);
          if (piece.a0 < 0) piece.a0 += kTwoPi;
          piece.a1 = piece.a0 + (b - a);
        }
        out.push_back(std::move(piece));
      }
    }
  }
  if (replaced.empty()) return 0;
  std::vector<Curve> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    auto it = replaced.find(i);
    if (it == replaced.end()) out.push_back(std::move(curves[i]));
    else std::move(it->second.begin(), it->second.end(), std::back_inserter(out));
  }
  curves.swap(out);
  return replaced.size();
}

// ---- caches

constexpr size_t kMemoryBudget = size_t(256) << 20;
constexpr std::uintmax_t kDiskBudget = 1ull << 30;
std::mutex g_memo_mu;
std::list<std::pair<std::shared_ptr<const ViewGeometry>, size_t>> g_memo;  // most recent first, with its size
size_t g_memo_bytes = 0;

size_t bytes_of(const ViewGeometry& g) {
  size_t b = sizeof g;
  for (const auto& c : g.curves) b += sizeof c + c.pts.size() * sizeof(Vec2) + (c.knots.size() + c.weights.size()) * sizeof(double);
  return b;
}

std::shared_ptr<const ViewGeometry> memo_get(const std::string& fp) {
  std::lock_guard<std::mutex> lock(g_memo_mu);
  for (auto it = g_memo.begin(); it != g_memo.end(); ++it)
    if (it->first->fingerprint == fp) {
      g_memo.splice(g_memo.begin(), g_memo, it);
      return g_memo.front().first;
    }
  return nullptr;
}

void memo_put(const std::shared_ptr<const ViewGeometry>& g) {
  const size_t bytes = bytes_of(*g);
  std::lock_guard<std::mutex> lock(g_memo_mu);
  g_memo.emplace_front(g, bytes);
  g_memo_bytes += bytes;
  while (g_memo_bytes > kMemoryBudget && g_memo.size() > 1) {
    g_memo_bytes -= g_memo.back().second;
    g_memo.pop_back();
  }
}

void disk_evict() {
  std::error_code error;
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
  std::uintmax_t total = 0;
  for (const auto& e : std::filesystem::directory_iterator(cache_dir() / "projection", error)) {
    if (!e.is_regular_file(error)) continue;
    total += e.file_size(error);
    files.push_back({e.last_write_time(error), e.path()});
  }
  std::sort(files.begin(), files.end());
  for (const auto& [time, path] : files) {
    if (total <= kDiskBudget) break;
    const auto size = std::filesystem::file_size(path, error);
    if (std::filesystem::remove(path, error)) total -= size;
  }
}

// ---- binary form

struct Writer {
  std::string out;
  template <class T> void put(const T& v) { out.append(reinterpret_cast<const char*>(&v), sizeof v); }
  void doubles(const std::vector<double>& v) {
    put(static_cast<uint32_t>(v.size()));
    out.append(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(double));
  }
};

struct Reader {
  const char* p;
  const char* end;
  template <class T> T get() {
    if (p + sizeof(T) > end) throw Error("truncated projection blob");
    T v;
    std::memcpy(&v, p, sizeof v);
    p += sizeof v;
    return v;
  }
  std::vector<double> doubles() {
    const uint32_t n = get<uint32_t>();
    if (p + static_cast<size_t>(n) * sizeof(double) > end) throw Error("truncated projection blob");
    std::vector<double> v(n);
    std::memcpy(v.data(), p, static_cast<size_t>(n) * sizeof(double));
    p += static_cast<size_t>(n) * sizeof(double);
    return v;
  }
};

constexpr char kMagic[] = "OPADPRJ2", kMagicSections[] = "OPADPRJ3";  // 3: section faces follow the curves

}  // namespace

// ---------------------------------------------------------------- public

const char* quality_name(Quality q) {
  switch (q) {
    case Quality::Exact: return "exact";
    case Quality::Draft: return "draft";
    case Quality::Hybrid: return "hybrid";
    default: return "auto";
  }
}

Quality quality_from_name(const std::string& name) {
  if (name == "auto" || name.empty()) return Quality::Auto;
  if (name == "exact") return Quality::Exact;
  if (name == "draft") return Quality::Draft;
  if (name == "hybrid") return Quality::Hybrid;
  throw Error("unknown projection quality '" + name + "' (auto, exact, draft, hybrid)");
}

ViewSpec ViewSpec::preset(const std::string& view) {
  const Camera c = Camera::preset(view);
  ViewSpec s;
  s.dir = c.eye;
  s.up = c.up;
  return s;
}

json ViewSpec::to_json() const {
  json j = {{"dir", vec_json(dir)}, {"up", vec_json(up)}, {"hidden", hidden}, {"tangent", tangent}, {"silhouettes", silhouettes},
            {"seams", seams}, {"quality", quality_name(quality)}, {"exact_faces", exact_faces}, {"resolution", resolution},
            {"tolerance", tolerance}, {"visible_only", visible_only}, {"nodes", nodes}, {"hide", hide}};
  if (!offsets.empty()) {
    json o = json::object();
    for (const auto& [node, shift] : offsets) o[node] = vec_json(shift);
    j["offsets"] = o;
  }
  if (!cut.empty()) {
    json line = json::array();
    for (const auto& p : cut) line.push_back({p[0], p[1]});
    j["cut"] = {{"line", line}, {"x", vec_json(cut_x)}, {"y", vec_json(cut_y)}, {"whole", whole}};
    if (aligned) j["cut"]["aligned"] = true;
  }
  for (const auto& b : breakouts) {
    json outline = json::array();
    for (const auto& p : b.outline) outline.push_back({p[0], p[1]});
    j["breakouts"].push_back({{"outline", outline}, {"depth", b.depth}});
    if (cut.empty()) j["whole"] = whole;
  }
  return j;
}

ViewSpec ViewSpec::from_json(const json& j) {
  ViewSpec s = j.contains("view") && j["view"].is_string() ? preset(j["view"].get<std::string>()) : ViewSpec{};
  s.dir = vec_of(j.value("dir", json()), s.dir);
  s.up = vec_of(j.value("up", json()), s.up);
  s.hidden = j.value("hidden", s.hidden);
  s.tangent = j.value("tangent", s.tangent);
  s.silhouettes = j.value("silhouettes", s.silhouettes);
  s.seams = j.value("seams", s.seams);
  s.quality = quality_from_name(j.value("quality", std::string("auto")));
  s.exact_faces = j.value("exact_faces", s.exact_faces);
  s.resolution = std::clamp(j.value("resolution", s.resolution), 256, 16384);
  s.tolerance = std::max(1e-6, j.value("tolerance", s.tolerance));
  s.visible_only = j.value("visible_only", s.visible_only);
  if (j.contains("nodes") && j["nodes"].is_array()) s.nodes = j["nodes"].get<std::vector<std::string>>();
  if (j.contains("hide") && j["hide"].is_array()) s.hide = j["hide"].get<std::vector<std::string>>();
  if (j.contains("offsets") && j["offsets"].is_object())
    for (const auto& [node, shift] : j["offsets"].items()) s.offsets[node] = vec_of(shift, {0, 0, 0});
  if (const json c = j.value("cut", json()); c.is_object()) {
    for (const auto& p : c.value("line", json::array()))
      if (p.is_array() && p.size() == 2) s.cut.push_back({p[0].get<double>(), p[1].get<double>()});
    s.cut_x = vec_of(c.value("x", json()), s.cut_x);
    s.cut_y = vec_of(c.value("y", json()), s.cut_y);
    if (c.contains("whole") && c["whole"].is_array()) s.whole = c["whole"].get<std::vector<std::string>>();
    s.aligned = c.value("aligned", false);
  }
  if (j.contains("whole") && j["whole"].is_array()) s.whole = j["whole"].get<std::vector<std::string>>();
  for (const auto& b : j.value("breakouts", json::array())) {
    if (!b.is_object()) continue;
    Breakout cut;
    for (const auto& p : b.value("outline", json::array()))
      if (p.is_array() && p.size() == 2) cut.outline.push_back({p[0].get<double>(), p[1].get<double>()});
    cut.depth = b.value("depth", 0.0);
    if (cut.outline.size() >= 3) s.breakouts.push_back(std::move(cut));
  }
  return s;
}

const char* Curve::type_name(Type t) {
  switch (t) {
    case Type::Arc: return "arc";
    case Type::Ellipse: return "ellipse";
    case Type::Spline: return "spline";
    case Type::Polyline: return "polyline";
    default: return "line";
  }
}

const char* Curve::kind_name(Kind k) {
  switch (k) {
    case Kind::Tangent: return "tangent";
    case Kind::Seam: return "seam";
    case Kind::Silhouette: return "silhouette";
    case Kind::Break: return "break";
    default: return "sharp";
  }
}

std::vector<Vec2> Curve::sample(double tol) const {
  tol = std::max(tol, 1e-9);
  switch (type) {
    case Type::Arc:
    case Type::Ellipse: {
      const int n = arc_steps(r1, a1 - a0, tol);
      const double cr = std::cos(rot), sr = std::sin(rot);
      std::vector<Vec2> out;
      out.reserve(static_cast<size_t>(n) + 1);
      for (int i = 0; i <= n; ++i) {
        const double s = a0 + (a1 - a0) * i / n, x = r1 * std::cos(s), y = r2 * std::sin(s);
        out.push_back({c[0] + x * cr - y * sr, c[1] + x * sr + y * cr});
      }
      return out;
    }
    case Type::Spline: {
      try {
        const Handle(Geom2d_BSplineCurve) s = detail::curve2d(*this);
        if (!s.IsNull()) {
          Geom2dAdaptor_Curve a(s);
          GCPnts_QuasiUniformDeflection d(a, tol);
          if (d.IsDone() && d.NbPoints() >= 2) {
            std::vector<Vec2> out;
            for (int i = 1; i <= d.NbPoints(); ++i) out.push_back({d.Value(i).X(), d.Value(i).Y()});
            return out;
          }
        }
      } catch (const Standard_Failure&) {
      }
      return pts;
    }
    default: return pts;
  }
}

std::vector<std::array<Vec2, 4>> Curve::beziers(double tol) const {
  tol = std::max(tol, 1e-9);
  std::vector<std::array<Vec2, 4>> out;
  auto straight = [&](const std::vector<Vec2>& p) {
    for (size_t i = 1; i < p.size(); ++i) out.push_back({p[i - 1], p[i - 1] + (p[i] - p[i - 1]) * (1.0 / 3), p[i - 1] + (p[i] - p[i - 1]) * (2.0 / 3), p[i]});
  };
  if (type == Type::Arc || type == Type::Ellipse) {
    const double span = a1 - a0;
    if (!(span > 0)) return out;
    int n = std::max(1, static_cast<int>(std::ceil(span / (M_PI / 2) - 1e-9)));
    while (n < 4096 && r1 * unit_arc_error(span / n) > tol) n += std::max(1, n / 4);
    // The pieces of the unit circle (controls along the tangents, 4/3 tan(h/4) long) mapped onto the conic.
    const double h = span / n, k = 4.0 / 3 * std::tan(h / 4), cr = std::cos(rot), sr = std::sin(rot);
    auto at = [&](double x, double y) { return Vec2{c[0] + x * cr - y * sr, c[1] + x * sr + y * cr}; };
    for (int i = 0; i < n; ++i) {
      const double s0 = a0 + h * i, s1 = i + 1 == n ? a1 : s0 + h;
      const double x0 = r1 * std::cos(s0), y0 = r2 * std::sin(s0), x1 = r1 * std::cos(s1), y1 = r2 * std::sin(s1);
      out.push_back({at(x0, y0), at(x0 - k * r1 * std::sin(s0), y0 + k * r2 * std::cos(s0)), at(x1 + k * r1 * std::sin(s1), y1 - k * r2 * std::cos(s1)), at(x1, y1)});
    }
    return out;
  }
  if (type == Type::Spline) {
    try {
      Handle(Geom2d_BSplineCurve) s = detail::curve2d(*this);
      if (!s.IsNull() && (s->IsRational() || s->Degree() > 3)) {
        Geom2dConvert_ApproxCurve approx(s, tol, GeomAbs_C1, 2000, 3);
        s = approx.HasResult() && approx.MaxError() <= tol ? approx.Curve() : nullptr;
      }
      if (!s.IsNull()) {
        if (s->Degree() < 3) s->IncreaseDegree(3);
        Geom2dConvert_BSplineCurveToBezierCurve split(s);
        for (int i = 1; i <= split.NbArcs(); ++i) {
          const Handle(Geom2d_BezierCurve) b = split.Arc(i);
          if (b->NbPoles() != 4 || b->IsRational()) {
            out.clear();
            break;
          }
          std::array<Vec2, 4> q;
          for (int p = 0; p < 4; ++p) q[static_cast<size_t>(p)] = {b->Pole(p + 1).X(), b->Pole(p + 1).Y()};
          out.push_back(q);
        }
        if (!out.empty()) return out;
      }
    } catch (const Standard_Failure&) {
      out.clear();
    }
    straight(sample(tol));  // no cubic within tol: its polyline
    return out;
  }
  straight(pts);
  return out;
}

double Curve::length() const {
  if (type == Type::Arc) return r1 * (a1 - a0);
  double size = std::max(r1, 1e-3);
  for (const auto& p : pts) size = std::max({size, std::fabs(p[0]), std::fabs(p[1])});
  const auto s = sample(size * 1e-7);
  double l = 0;
  for (size_t i = 1; i < s.size(); ++i) l += norm(s[i] - s[i - 1]);
  return l;
}

json Curve::to_json(double bezier_tol) const {
  json j = {{"type", type_name(type)}, {"kind", kind_name(kind)}, {"hidden", hidden}};
  if (body >= 0) j["body"] = body;
  if (edge >= 0) j["edge"] = edge;
  if (face >= 0) j["face"] = face;
  j["z"] = z;
  auto points = [](const std::vector<Vec2>& v) {
    json a = json::array();
    for (const auto& p : v) a.push_back({p[0], p[1]});
    return a;
  };
  switch (type) {
    case Type::Arc: j["c"] = {c[0], c[1]}; j["r"] = r1; j["a0"] = a0; j["a1"] = a1; break;
    case Type::Ellipse: j["c"] = {c[0], c[1]}; j["r1"] = r1; j["r2"] = r2; j["rot"] = rot; j["a0"] = a0; j["a1"] = a1; break;
    case Type::Spline:
      j["degree"] = degree;
      j["poles"] = points(pts);
      j["knots"] = knots;
      if (!weights.empty()) j["weights"] = weights;
      break;
    default: j["pts"] = points(pts);
  }
  if (bezier_tol > 0 && (type == Type::Arc || type == Type::Ellipse || type == Type::Spline)) {
    json b = json::array();
    for (const auto& q : beziers(bezier_tol)) b.push_back({q[0][0], q[0][1], q[1][0], q[1][1], q[2][0], q[2][1], q[3][0], q[3][1]});
    j["bezier"] = b;
  }
  return j;
}

json ViewGeometry::counts() const {
  json j = {{"curves", curves.size()}, {"visible", 0}, {"hidden", 0}};
  for (const char* k : {"sharp", "tangent", "seam", "silhouette", "line", "arc", "ellipse", "spline", "polyline"}) j[k] = 0;
  for (const auto& c : curves) {
    j[c.hidden ? "hidden" : "visible"] = j[c.hidden ? "hidden" : "visible"].get<int>() + 1;
    j[Curve::kind_name(c.kind)] = j.value(Curve::kind_name(c.kind), 0) + 1;
    j[Curve::type_name(c.type)] = j[Curve::type_name(c.type)].get<int>() + 1;
  }
  return j;
}

json ViewGeometry::to_json(bool with_curves, double bezier_tol) const {
  json b = json::array();
  for (const auto& body : bodies) b.push_back({{"node", body.node}, {"key", body.key}});
  json j = {{"tier", quality_name(tier)}, {"fingerprint", fingerprint}, {"x", vec_json(x)}, {"y", vec_json(y)}, {"dir", vec_json(dir)},
            {"bounds", bounds}, {"bodies", b}, {"counts", counts()}, {"stats", stats}};
  if (with_curves) {
    json c = json::array();
    for (const auto& k : curves) c.push_back(k.to_json(bezier_tol));
    j["curves"] = c;
  }
  return j;
}

std::string ViewGeometry::serialize() const {
  json b = json::array();
  for (const auto& body : bodies) b.push_back({body.node, body.key});
  const std::string head = json{{"tier", quality_name(tier)}, {"fingerprint", fingerprint}, {"x", vec_json(x)}, {"y", vec_json(y)},
                                {"dir", vec_json(dir)}, {"bounds", bounds}, {"bodies", b}, {"stats", stats}}.dump();
  Writer w;
  w.out = sections.empty() ? kMagic : kMagicSections;
  w.put(static_cast<uint32_t>(head.size()));
  w.out += head;
  w.put(static_cast<uint32_t>(curves.size()));
  for (const auto& k : curves) {
    w.put(static_cast<uint8_t>(k.type));
    w.put(static_cast<uint8_t>(k.kind));
    w.put(static_cast<uint8_t>(k.hidden));
    w.put(static_cast<int32_t>(k.body));
    w.put(static_cast<int32_t>(k.edge));
    w.put(static_cast<int32_t>(k.face));
    std::vector<double> p;
    for (const auto& q : k.pts) p.insert(p.end(), {q[0], q[1]});
    w.doubles(p);
    w.doubles({k.c[0], k.c[1], k.r1, k.r2, k.rot, k.a0, k.a1, static_cast<double>(k.degree), k.z});
    w.doubles(k.knots);
    w.doubles(k.weights);
  }
  if (sections.empty()) return w.out;
  w.put(static_cast<uint32_t>(sections.size()));
  for (const auto& r : sections) {
    w.put(static_cast<int32_t>(r.body));
    w.put(static_cast<uint32_t>(r.loops.size()));
    for (const auto& l : r.loops) {
      std::vector<double> p;
      for (const auto& q : l) p.insert(p.end(), {q[0], q[1]});
      w.doubles(p);
    }
  }
  return w.out;
}

ViewGeometry ViewGeometry::deserialize(const std::string& blob) {
  const size_t m = sizeof kMagic - 1;
  const bool with_sections = blob.size() >= m && blob.compare(0, m, kMagicSections) == 0;
  if (blob.size() < m + 4 || (blob.compare(0, m, kMagic) != 0 && !with_sections)) throw Error("bad projection blob");
  Reader r{blob.data() + m, blob.data() + blob.size()};
  const uint32_t n = r.get<uint32_t>();
  if (r.p + n > r.end) throw Error("truncated projection blob");
  const json head = json::parse(std::string(r.p, n));
  r.p += n;
  ViewGeometry g;
  g.tier = quality_from_name(head.at("tier").get<std::string>());
  g.fingerprint = head.at("fingerprint").get<std::string>();
  g.x = vec_of(head.at("x"), g.x);
  g.y = vec_of(head.at("y"), g.y);
  g.dir = vec_of(head.at("dir"), g.dir);
  g.bounds = head.at("bounds").get<std::array<double, 4>>();
  g.stats = head.value("stats", json::object());
  for (const auto& b : head.at("bodies")) g.bodies.push_back({b.at(0).get<std::string>(), b.at(1).get<std::string>()});
  const uint32_t count = r.get<uint32_t>();
  g.curves.resize(count);
  for (auto& k : g.curves) {
    k.type = static_cast<Curve::Type>(r.get<uint8_t>());
    k.kind = static_cast<Curve::Kind>(r.get<uint8_t>());
    k.hidden = r.get<uint8_t>() != 0;
    k.body = r.get<int32_t>();
    k.edge = r.get<int32_t>();
    k.face = r.get<int32_t>();
    const auto p = r.doubles();
    for (size_t i = 0; i + 1 < p.size(); i += 2) k.pts.push_back({p[i], p[i + 1]});
    const auto a = r.doubles();
    if (a.size() != 9) throw Error("bad projection blob");
    k.c = {a[0], a[1]};
    k.r1 = a[2], k.r2 = a[3], k.rot = a[4], k.a0 = a[5], k.a1 = a[6];
    k.degree = static_cast<int>(a[7]);
    k.z = a[8];
    k.knots = r.doubles();
    k.weights = r.doubles();
  }
  if (with_sections) {
    g.sections.resize(r.get<uint32_t>());
    for (auto& s : g.sections) {
      s.body = r.get<int32_t>();
      s.loops.resize(r.get<uint32_t>());
      for (auto& l : s.loops) {
        const auto p = r.doubles();
        for (size_t i = 0; i + 1 < p.size(); i += 2) l.push_back({p[i], p[i + 1]});
      }
    }
  }
  return g;
}

void detail::Run::report(double fraction, const std::string& phase, bool force) {
  if (!m_callback) return;
  std::unique_lock<std::mutex> lock(m_mu, std::try_to_lock);
  if (!lock.owns_lock()) return;
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - m_last < std::chrono::milliseconds(100)) return;
  m_last = now;
  if (!m_callback(fraction, phase)) m_stop = true;
}

void detail::emit(const Adaptor3d_Curve& c, double t0, double t1, const View& v, const Curve& as, double tol, std::vector<Curve>& out) {
  if (t1 < t0) std::swap(t0, t1);
  if (!(t1 - t0 > 1e-12)) return;
  Curve like = as;
  try {
    like.z = v.depth(c.Value(0.5 * (t0 + t1)));
    switch (c.GetType()) {
      case GeomAbs_Line: {
        Curve k = like;
        k.type = Curve::Type::Line;
        k.pts = {v.at(c.Value(t0)), v.at(c.Value(t1))};
        if (norm(k.pts[1] - k.pts[0]) < 1e-9) return;  // seen end-on
        out.push_back(std::move(k));
        return;
      }
      case GeomAbs_Circle: {
        const gp_Circ k = c.Circle();
        conic(k.Location(), k.XAxis().Direction().XYZ() * k.Radius(), k.YAxis().Direction().XYZ() * k.Radius(), t0, t1, v, like, out);
        return;
      }
      case GeomAbs_Ellipse: {
        const gp_Elips e = c.Ellipse();
        conic(e.Location(), e.XAxis().Direction().XYZ() * e.MajorRadius(), e.YAxis().Direction().XYZ() * e.MinorRadius(), t0, t1, v, like, out);
        return;
      }
      case GeomAbs_BSplineCurve:
        if (spline(Handle(Geom_BSplineCurve)::DownCast(c.BSpline()->Copy()), t0, t1, v, like, out)) return;
        break;
      case GeomAbs_BezierCurve:
        if (spline(GeomConvert::CurveToBSplineCurve(c.Bezier()), t0, t1, v, like, out)) return;
        break;
      default: {
        GeomConvert_ApproxCurve approx(c.Trim(t0, t1, 1e-12), tol, GeomAbs_C1, 64, 8);
        if (approx.HasResult() && spline(approx.Curve(), approx.Curve()->FirstParameter(), approx.Curve()->LastParameter(), v, like, out)) return;
      }
    }
  } catch (const Standard_Failure&) {
  }
  Curve k = like;
  k.type = Curve::Type::Polyline;
  try {
    GCPnts_QuasiUniformDeflection d(c, tol, t0, t1);
    if (d.IsDone())
      for (int i = 1; i <= d.NbPoints(); ++i) k.pts.push_back(v.at(d.Value(i)));
  } catch (const Standard_Failure&) {
  }
  if (k.pts.size() < 2) {
    k.pts.clear();
    for (int i = 0; i <= 32; ++i) k.pts.push_back(v.at(c.Value(t0 + (t1 - t0) * i / 32)));
  }
  out.push_back(std::move(k));
}

void view_axes(const ViewSpec& spec, Vec3& x, Vec3& y, Vec3& dir) {
  const View v = view_of(spec);
  x = {v.x.X(), v.x.Y(), v.x.Z()};
  y = {v.y.X(), v.y.Y(), v.y.Z()};
  dir = {v.z.X(), v.z.Y(), v.z.Z()};
}

namespace {
// A projection made before: from memory, else from the user cache (then remembered in memory too).
std::shared_ptr<const ViewGeometry> cached(const std::string& fp) {
  if (auto hit = memo_get(fp)) return hit;
  if (auto blob = cache_get("projection", fp)) {
    try {
      auto g = std::make_shared<const ViewGeometry>(ViewGeometry::deserialize(*blob));
      if (g->fingerprint == fp) {
        memo_put(g);
        std::error_code error;
        std::filesystem::last_write_time(cache_dir() / "projection" / (fp + ".bin"), std::filesystem::file_time_type::clock::now(), error);
        return g;
      }
    } catch (const std::exception&) {
    }
  }
  return nullptr;
}
}  // namespace

std::shared_ptr<const ViewGeometry> cached_projection(const Document& doc, const Scene& scene, const ViewSpec& spec) {
  auto sources = gather(scene, spec);
  return cached(fingerprint_of(sources, spec, auto_tier(doc, sources, spec)));
}

std::vector<std::pair<std::string, Mat4>> view_bodies(const Scene& scene, const ViewSpec& spec) {
  std::vector<std::pair<std::string, Mat4>> out;
  for (auto& s : gather(scene, spec)) out.emplace_back(std::move(s.node), s.world);
  return out;
}

Quality choose_tier(const Document& doc, const Scene& scene, const ViewSpec& spec) {
  auto sources = gather(scene, spec);
  return auto_tier(doc, sources, spec);
}

std::string projection_fingerprint(const Document& doc, const Scene& scene, const ViewSpec& spec, Quality tier) {
  auto sources = gather(scene, spec);
  return fingerprint_of(sources, spec, tier == Quality::Auto ? auto_tier(doc, sources, spec) : tier);
}

std::shared_ptr<const ViewGeometry> project(const Document& doc, const Scene& scene, const ViewSpec& spec, const ProjectionProgress& progress, bool use_cache) {
  Run run(progress);
  const auto start = std::chrono::steady_clock::now();
  auto sources = gather(scene, spec);
  const Quality tier = auto_tier(doc, sources, spec);
  const View v = view_of(spec);
  const std::string fp = fingerprint_of(sources, spec, tier);
  if (use_cache)
    if (auto hit = cached(fp)) return hit;
  load(doc, sources, true);
  auto g = std::make_shared<ViewGeometry>();
  if (!spec.cut.empty() || !spec.breakouts.empty()) {
    const auto cutting = std::chrono::steady_clock::now();
    try {
      if (!spec.cut.empty()) detail::cut_sources(doc, spec, v, sources, run, g->sections);
      else detail::breakout_sources(doc, spec, v, sources, run, g->sections);
    } catch (const Standard_Failure& e) {
      throw Error(std::string("section failed: ") + e.GetMessageString());
    }
    g->stats["cut_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cutting).count();
  }
  g->stats["gather_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  g->tier = tier;
  g->fingerprint = fp;
  g->x = {v.x.X(), v.x.Y(), v.x.Z()};
  g->y = {v.y.X(), v.y.Y(), v.y.Z()};
  g->dir = {v.z.X(), v.z.Y(), v.z.Z()};
  for (const auto& s : sources) g->bodies.push_back({s.node, s.body_key()});
  if (!sources.empty()) {
    try {
      if (tier == Quality::Exact) exact(sources, spec, v, run, *g);
      else if (tier == Quality::Draft) draft(doc, sources, spec, v, run, *g);
      else detail::hybrid(doc, sources, spec, v, run, *g);
    } catch (const Standard_Failure& e) {
      throw Error(std::string("projection failed: ") + e.GetMessageString());
    }
    if (!spec.cut.empty() || !spec.breakouts.empty()) {
      detail::name_cut_curves(sources, g->curves);
      detail::drop_joint_curves(spec, v, sources, g->curves);
      detail::mark_breakout_curves(spec, sources, g->curves);
    }
  }
  run.check();
  const auto finish = std::chrono::steady_clock::now();
  g->stats["overlaps"] = drop_overlaps(g->curves, 0.1 * spec.tolerance);  // closer than a tenth of the tolerance: one line
  bool any = false;
  std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
  for (const auto& k : g->curves) {
    const auto pts = k.type == Curve::Type::Arc || k.type == Curve::Type::Ellipse ? k.sample(std::max(k.r1, 1e-6) * 1e-3) : k.pts;
    for (const auto& p : pts) {
      box = {std::min(box[0], p[0]), std::min(box[1], p[1]), std::max(box[2], p[0]), std::max(box[3], p[1])};
      any = true;
    }
  }
  if (any) g->bounds = box;
  g->stats["finish_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - finish).count();
  g->stats["bodies"] = sources.size();
  g->stats["ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  run.report(1, "hidden lines: done", true);
  std::shared_ptr<const ViewGeometry> result = g;
  if (use_cache) {
    memo_put(result);
    cache_put("projection", fp, g->serialize());
    disk_evict();
  }
  return result;
}

void clear_projection_memory() {
  std::lock_guard<std::mutex> lock(g_memo_mu);
  g_memo.clear();
  g_memo_bytes = 0;
}

// ---------------------------------------------------------------- preview

Image preview_image(const ViewGeometry& g, int width, int height) {
  Image img;
  img.width = std::max(16, width);
  img.height = std::max(16, height);
  img.rgb.assign(static_cast<size_t>(img.width) * img.height * 3, 255);
  const double w = g.bounds[2] - g.bounds[0], h = g.bounds[3] - g.bounds[1];
  if (g.curves.empty() || (w <= 0 && h <= 0)) return img;
  const double scale = 0.92 * std::min(img.width / std::max(w, 1e-9), img.height / std::max(h, 1e-9));
  const double cx = 0.5 * (g.bounds[0] + g.bounds[2]), cy = 0.5 * (g.bounds[1] + g.bounds[3]);
  auto plot = [&](double x, double y, uint8_t shade) {
    const int px = static_cast<int>(std::lround(img.width / 2.0 + (x - cx) * scale)), py = static_cast<int>(std::lround(img.height / 2.0 - (y - cy) * scale));
    if (px < 0 || py < 0 || px >= img.width || py >= img.height) return;
    uint8_t* p = img.px(px, py);
    if (p[0] > shade) p[0] = p[1] = p[2] = shade;
  };
  for (int pass = 0; pass < 3; ++pass)  // hidden under tangent under visible
    for (const auto& k : g.curves) {
      const int mine = k.hidden ? 0 : k.kind == Curve::Kind::Tangent || k.kind == Curve::Kind::Seam || k.kind == Curve::Kind::Break ? 1 : 2;
      if (mine != pass) continue;
      const uint8_t shade = pass == 0 ? 185 : pass == 1 ? 140 : 0;
      const auto pts = k.sample(0.25 / scale);
      double run = 0;
      for (size_t i = 1; i < pts.size(); ++i) {
        const double ax = pts[i - 1][0], ay = pts[i - 1][1], bx = pts[i][0], by = pts[i][1];
        const double len = std::hypot(bx - ax, by - ay) * scale;
        const int steps = std::max(1, static_cast<int>(std::ceil(len * 2)));
        for (int s = 0; s <= steps; ++s) {
          const double f = static_cast<double>(s) / steps, at = run + f * len;
          if (pass == 0 && std::fmod(at, 9.0) > 5.0) continue;  // dashes for hidden lines
          plot(ax + (bx - ax) * f, ay + (by - ay) * f, shade);
        }
        run += len;
      }
    }
  return img;
}

}  // namespace opad::drawing
