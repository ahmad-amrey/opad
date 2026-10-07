// Mesh to solid: reading the mesh, its adjacency, and how far a rebuilt shape is from it (opad/mesh_solid.hpp).
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <Poly_Triangulation.hxx>
#include <ShapeAnalysis_Surface.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>

#include "meshsolid.hpp"
#include "opad/util.hpp"

namespace opad {

double TriMesh::area() const {
  double s = 0;
  for (const auto& t : triangles)
    s += gp_Vec(points[t[0]], points[t[1]]).Crossed(gp_Vec(points[t[0]], points[t[2]])).Magnitude() / 2;
  return s;
}

double TriMesh::volume() const {
  double v = 0;
  for (const auto& t : triangles) v += gp_Vec(points[t[0]].XYZ()).Dot(gp_Vec(points[t[1]].XYZ()).Crossed(gp_Vec(points[t[2]].XYZ()))) / 6;
  return std::abs(v);
}

double TriMesh::diagonal() const {
  if (points.empty()) return 0;
  gp_XYZ lo = points[0].XYZ(), hi = lo;
  for (const auto& p : points)
    for (int i = 1; i <= 3; ++i) {
      lo.SetCoord(i, std::min(lo.Coord(i), p.Coord(i)));
      hi.SetCoord(i, std::max(hi.Coord(i), p.Coord(i)));
    }
  return (hi - lo).Modulus();
}

int TriMesh::open_edges() const {
  std::map<std::pair<int, int>, int> count;
  for (const auto& t : triangles)
    for (int k = 0; k < 3; ++k) ++count[std::minmax(t[k], t[(k + 1) % 3])];
  int open = 0;
  for (const auto& [e, n] : count) open += n == 1;
  return open;
}

TriMesh mesh_of_shape(const TopoDS_Shape& shape, double weld) {
  TriMesh out;
  if (weld <= 0) {
    Bnd_Box box;
    BRepBndLib::Add(shape, box);
    if (box.IsVoid()) throw Error("the mesh has no triangles");
    weld = std::max(1e-9, box.CornerMin().Distance(box.CornerMax()) * 1e-7);
  }
  std::unordered_map<long long, std::vector<int>> grid;  // hashed cells, then an exact check against the few points in one
  auto vertex = [&](const gp_Pnt& p) {
    const long long x = std::llround(p.X() / weld), y = std::llround(p.Y() / weld), z = std::llround(p.Z() / weld);
    const long long key = (x * 73856093LL) ^ (y * 19349663LL) ^ (z * 83492791LL);
    auto& bucket = grid[key];
    for (int i : bucket)
      if (out.points[i].SquareDistance(p) <= weld * weld) return i;
    out.points.push_back(p);
    bucket.push_back(int(out.points.size()) - 1);
    return int(out.points.size()) - 1;
  };
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    TopLoc_Location loc;
    const auto tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    const gp_Trsf t = loc.Transformation();
    std::vector<int> ids(size_t(tri->NbNodes()) + 1);
    for (int i = 1; i <= tri->NbNodes(); ++i) ids[size_t(i)] = vertex(tri->Node(i).Transformed(t));
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int a, b, c;
      tri->Triangle(i).Get(a, b, c);
      std::array<int, 3> v{ids[size_t(a)], ids[size_t(b)], ids[size_t(c)]};
      if (reversed) std::swap(v[1], v[2]);
      if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) continue;
      if (gp_Vec(out.points[v[0]], out.points[v[1]]).Crossed(gp_Vec(out.points[v[0]], out.points[v[2]])).SquareMagnitude() < 1e-30) continue;
      out.triangles.push_back(v);
    }
  }
  if (out.triangles.empty()) throw Error("the mesh has no triangles");
  return out;
}

double mesh_tolerance(const TriMesh& mesh) { return std::max(0.001, mesh.diagonal() / 2000); }

namespace meshsolid {

Topology::Topology(const TriMesh& m) : mesh(&m) {
  const size_t n = m.triangles.size();
  across.assign(n, {-1, -1, -1});
  normal.resize(n);
  area.resize(n);
  dihedral.assign(n, {M_PI, M_PI, M_PI});
  std::unordered_map<long long, std::pair<int, int>> sides;  // directed edge key -> (triangle, side); collisions checked
  sides.reserve(n * 3);
  const long long stride = static_cast<long long>(m.points.size()) + 1;
  std::unordered_map<long long, int> uses;
  uses.reserve(n * 3);
  for (size_t t = 0; t < n; ++t) {
    const auto& v = m.triangles[t];
    const gp_Vec c = gp_Vec(m.points[v[0]], m.points[v[1]]).Crossed(gp_Vec(m.points[v[0]], m.points[v[2]]));
    area[t] = c.Magnitude() / 2;
    normal[t] = area[t] > 0 ? c / (2 * area[t]) : gp_Vec(0, 0, 1);
    for (int k = 0; k < 3; ++k) {
      const int a = v[k], b = v[(k + 1) % 3];
      ++uses[std::min(a, b) * stride + std::max(a, b)];
      sides[a * stride + b] = {int(t), k};
    }
  }
  for (size_t t = 0; t < n; ++t) {
    const auto& v = m.triangles[t];
    for (int k = 0; k < 3; ++k) {
      const int a = v[k], b = v[(k + 1) % 3];
      if (uses[std::min(a, b) * stride + std::max(a, b)] > 2) {
        across[t][k] = -2;
        continue;
      }
      auto it = sides.find(b * stride + a);
      if (it == sides.end()) continue;  // open, or the neighbour is wound the other way (treated as open)
      across[t][k] = it->second.first;
      const double c = std::clamp(normal[t].Dot(normal[size_t(it->second.first)]), -1.0, 1.0);
      dihedral[t][k] = std::acos(c);
    }
  }
}

gp_Pnt Topology::centroid(int t) const {
  const auto& v = mesh->triangles[size_t(t)];
  return gp_Pnt((mesh->points[v[0]].XYZ() + mesh->points[v[1]].XYZ() + mesh->points[v[2]].XYZ()) / 3);
}

int Topology::side_of(int t, int a, int b) const {
  const auto& v = mesh->triangles[size_t(t)];
  for (int k = 0; k < 3; ++k)
    if (v[k] == a && v[(k + 1) % 3] == b) return k;
  return -1;
}

double point_triangle_distance(const gp_Pnt& p, const gp_Pnt& a, const gp_Pnt& b, const gp_Pnt& c) {
  // Ericson, Real-Time Collision Detection 5.1.5.
  const gp_Vec ab(a, b), ac(a, c), ap(a, p);
  const double d1 = ab.Dot(ap), d2 = ac.Dot(ap);
  if (d1 <= 0 && d2 <= 0) return p.Distance(a);
  const gp_Vec bp(b, p);
  const double d3 = ab.Dot(bp), d4 = ac.Dot(bp);
  if (d3 >= 0 && d4 <= d3) return p.Distance(b);
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return p.Distance(a.Translated(ab * (d1 / (d1 - d3))));
  const gp_Vec cp(c, p);
  const double d5 = ab.Dot(cp), d6 = ac.Dot(cp);
  if (d6 >= 0 && d5 <= d6) return p.Distance(c);
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return p.Distance(a.Translated(ac * (d2 / (d2 - d6))));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return p.Distance(b.Translated(gp_Vec(b, c) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
  const double denom = 1 / (va + vb + vc);
  return p.Distance(a.Translated(ab * (vb * denom) + ac * (vc * denom)));
}

NearestTree::NearestTree(std::vector<gp_Pnt> points, std::vector<std::array<int, 3>> triangles)
    : m_points(std::move(points)), m_triangles(std::move(triangles)) {
  m_order.resize(m_triangles.size());
  std::iota(m_order.begin(), m_order.end(), 0);
  m_centroid.resize(m_triangles.size());
  for (size_t t = 0; t < m_triangles.size(); ++t)
    for (int i = 0; i < 3; ++i)
      m_centroid[t][size_t(i)] = (m_points[m_triangles[t][0]].Coord(i + 1) + m_points[m_triangles[t][1]].Coord(i + 1) + m_points[m_triangles[t][2]].Coord(i + 1)) / 3;
  if (!m_triangles.empty()) build(0, int(m_triangles.size()), 0);
}

int NearestTree::build(int first, int count, int depth) {
  Node node;
  for (int i = 0; i < 3; ++i) {
    node.lo[i] = std::numeric_limits<double>::max();
    node.hi[i] = -std::numeric_limits<double>::max();
  }
  for (int k = first; k < first + count; ++k)
    for (int v : m_triangles[size_t(m_order[size_t(k)])])
      for (int i = 0; i < 3; ++i) {
        node.lo[i] = std::min(node.lo[i], m_points[size_t(v)].Coord(i + 1));
        node.hi[i] = std::max(node.hi[i], m_points[size_t(v)].Coord(i + 1));
      }
  const int index = int(m_nodes.size());
  m_nodes.push_back(node);
  if (count <= 6 || depth > 40) {
    m_nodes[size_t(index)].first = first;
    m_nodes[size_t(index)].count = count;
    return index;
  }
  int axis = 0;
  for (int i = 1; i < 3; ++i)
    if (node.hi[i] - node.lo[i] > node.hi[axis] - node.lo[axis]) axis = i;
  const int half = count / 2;
  std::nth_element(m_order.begin() + first, m_order.begin() + first + half, m_order.begin() + first + count,
                   [&](int x, int y) { return m_centroid[size_t(x)][size_t(axis)] < m_centroid[size_t(y)][size_t(axis)]; });
  const int left = build(first, half, depth + 1);
  const int right = build(first + half, count - half, depth + 1);
  m_nodes[size_t(index)].left = left;
  m_nodes[size_t(index)].right = right;
  return index;
}

double NearestTree::distance(const gp_Pnt& p, int* triangle) const {
  double best = std::numeric_limits<double>::max();
  int found = -1;
  if (m_nodes.empty()) return best;
  auto box = [&](const Node& n) {
    double d = 0;
    for (int i = 0; i < 3; ++i) {
      const double c = p.Coord(i + 1);
      const double e = c < n.lo[i] ? n.lo[i] - c : c > n.hi[i] ? c - n.hi[i] : 0;
      d += e * e;
    }
    return d;
  };
  std::vector<int> stack{0};
  while (!stack.empty()) {
    const Node& n = m_nodes[size_t(stack.back())];
    stack.pop_back();
    if (box(n) >= best * best) continue;
    if (n.left < 0) {
      for (int k = n.first; k < n.first + n.count; ++k) {
        const auto& t = m_triangles[size_t(m_order[size_t(k)])];
        const double d = point_triangle_distance(p, m_points[size_t(t[0])], m_points[size_t(t[1])], m_points[size_t(t[2])]);
        if (d < best) {
          best = d;
          found = m_order[size_t(k)];
        }
      }
      continue;
    }
    const double l = box(m_nodes[size_t(n.left)]), r = box(m_nodes[size_t(n.right)]);
    if (l < r) {  // the nearer child last, so it is searched first
      stack.push_back(n.right);
      stack.push_back(n.left);
    } else {
      stack.push_back(n.left);
      stack.push_back(n.right);
    }
  }
  if (triangle) *triangle = found;
  return best;
}

double mesh_sagitta(const Topology& topo, double angle) {
  // Two facets of width w meeting at a small angle t stand for an arc of radius w / t; a chord of w sits w * t / 8 inside it.
  const auto& m = *topo.mesh;
  double worst = 0;
  for (size_t t = 0; t < m.triangles.size(); ++t)
    for (int k = 0; k < 3; ++k) {
      const double d = topo.dihedral[t][size_t(k)];
      if (topo.across[t][size_t(k)] < 0 || d < 1e-4 || d >= angle) continue;
      const auto& v = m.triangles[t];
      const gp_Pnt& a = m.points[size_t(v[size_t(k)])];
      const gp_Pnt& b = m.points[size_t(v[(size_t(k) + 1) % 3])];
      const gp_Pnt& c = m.points[size_t(v[(size_t(k) + 2) % 3])];
      const gp_Vec e(a, b);
      const double len = e.Magnitude();
      if (len <= 0) continue;
      const double width = gp_Vec(a, c).Crossed(e).Magnitude() / len;  // the facet across that edge
      worst = std::max(worst, width * d / 8);
    }
  return worst;
}

}  // namespace meshsolid

double MeshDeviation::score() const {
  if (samples == 0) return 0;
  double s = within * 100;
  // Surface of the result farther from the mesh than its facets explain: material the mesh does not have.
  const double allowed = tolerance + 1.5 * sagitta;
  if (back_max > allowed && allowed > 0) s *= std::max(0.0, 1 - (back_max - allowed) / (back_max + allowed));
  return std::clamp(s, 0.0, 100.0);
}

json MeshDeviation::to_json() const {
  auto pct = [](double a, double b) { return b > 0 ? (a - b) / b * 100 : 0.0; };
  return {{"tolerance", tolerance},         {"samples", samples},
          {"max", max},                     {"mean", mean},
          {"rms", rms},                     {"within", within * 100},
          {"back_max", back_max},           {"back_mean", back_mean},
          {"sagitta", sagitta},             {"mesh_volume", mesh_volume},
          {"shape_volume", shape_volume},   {"volume_change", pct(shape_volume, mesh_volume)},
          {"mesh_area", mesh_area},         {"shape_area", shape_area},
          {"area_change", pct(shape_area, mesh_area)}, {"mesh_triangles", mesh_triangles},
          {"shape_faces", shape_faces},     {"score", score()}};
}

MeshDeviation mesh_deviation(const TriMesh& mesh, const TopoDS_Shape& shape, double tolerance, const std::function<bool()>& cancelled) {
  using namespace meshsolid;
  MeshDeviation out;
  out.tolerance = tolerance;
  out.mesh_triangles = int(mesh.triangles.size());
  out.mesh_volume = mesh.volume();
  out.mesh_area = mesh.area();
  {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(shape, TopAbs_FACE, faces);
    out.shape_faces = faces.Extent();
  }
  GProp_GProps vp, sp;
  BRepGProp::VolumeProperties(shape, vp);
  BRepGProp::SurfaceProperties(shape, sp);
  out.shape_volume = std::abs(vp.Mass());
  out.shape_area = sp.Mass();
  const Topology topo(mesh);
  out.sagitta = mesh_sagitta(topo, 40 * M_PI / 180);

  // The shape's own triangles, fine enough that their chords stay well inside the tolerance; the faces they came from, so
  // a vertex near a curved face is measured to the exact surface.
  TopoDS_Shape copy = BRepBuilderAPI_Copy(shape, true, false).Shape();
  BRepMesh_IncrementalMesh(copy, std::max(tolerance / 4, mesh.diagonal() * 1e-6), false, 0.1, true);
  std::vector<gp_Pnt> pts;
  std::vector<std::array<int, 3>> tris;
  std::vector<TopoDS_Face> faceOf;
  std::vector<int> triFace;
  for (TopExp_Explorer f(copy, TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    TopLoc_Location loc;
    const auto tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull()) continue;
    const int base = int(pts.size());
    for (int i = 1; i <= tri->NbNodes(); ++i) pts.push_back(tri->Node(i).Transformed(loc.Transformation()));
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int a, b, c;
      tri->Triangle(i).Get(a, b, c);
      tris.push_back({base + a - 1, base + b - 1, base + c - 1});
      triFace.push_back(int(faceOf.size()));
    }
    faceOf.push_back(face);
  }
  if (tris.empty()) throw Error("the rebuilt shape could not be measured");
  const NearestTree shapeTree(pts, tris);
  std::vector<Handle(ShapeAnalysis_Surface)> surfaces(faceOf.size());
  std::vector<std::array<double, 4>> bounds(faceOf.size());
  for (size_t i = 0; i < faceOf.size(); ++i) {
    TopLoc_Location loc;
    Handle(Geom_Surface) s = BRep_Tool::Surface(faceOf[i], loc);
    if (s.IsNull()) continue;
    if (!loc.IsIdentity()) s = Handle(Geom_Surface)::DownCast(s->Transformed(loc.Transformation()));
    surfaces[i] = new ShapeAnalysis_Surface(s);
    BRepTools::UVBounds(faceOf[i], bounds[i][0], bounds[i][1], bounds[i][2], bounds[i][3]);
  }

  double sum = 0, sum2 = 0;
  int within = 0;
  for (size_t i = 0; i < mesh.points.size(); ++i) {
    if (cancelled && i % 4096 == 0 && cancelled()) throw Error("cancelled");
    const gp_Pnt& p = mesh.points[i];
    int t = -1;
    double d = shapeTree.distance(p, &t);
    if (t >= 0 && d > 1e-12) {
      const int f = triFace[size_t(t)];
      if (const auto& s = surfaces[size_t(f)]; !s.IsNull()) {
        const gp_Pnt2d uv = s->ValueOfUV(p, tolerance * 1e-3);
        const auto& b = bounds[size_t(f)];
        const double du = (b[1] - b[0]) * 0.01, dv = (b[3] - b[2]) * 0.01;
        if (uv.X() >= b[0] - du && uv.X() <= b[1] + du && uv.Y() >= b[2] - dv && uv.Y() <= b[3] + dv) d = std::min(d, s->Gap());
      }
    }
    out.max = std::max(out.max, d);
    sum += d;
    sum2 += d * d;
    within += d <= tolerance * (1 + 1e-9);
  }
  out.samples = int(mesh.points.size());
  out.mean = sum / std::max(1, out.samples);
  out.rms = std::sqrt(sum2 / std::max(1, out.samples));
  out.within = double(within) / std::max(1, out.samples);

  // The other way: every point of the shape's triangles to the mesh (centroids weighted by their area for the mean).
  const NearestTree meshTree(mesh.points, mesh.triangles);
  double backSum = 0, backArea = 0;
  for (size_t i = 0; i < pts.size(); ++i) {
    if (cancelled && i % 4096 == 0 && cancelled()) throw Error("cancelled");
    out.back_max = std::max(out.back_max, meshTree.distance(pts[i]));
  }
  for (const auto& t : tris) {
    const gp_Pnt c((pts[size_t(t[0])].XYZ() + pts[size_t(t[1])].XYZ() + pts[size_t(t[2])].XYZ()) / 3);
    const double a = gp_Vec(pts[size_t(t[0])], pts[size_t(t[1])]).Crossed(gp_Vec(pts[size_t(t[0])], pts[size_t(t[2])])).Magnitude() / 2;
    const double d = meshTree.distance(c);
    out.back_max = std::max(out.back_max, d);
    backSum += d * a;
    backArea += a;
  }
  out.back_mean = backArea > 0 ? backSum / backArea : 0;
  return out;
}

}  // namespace opad
