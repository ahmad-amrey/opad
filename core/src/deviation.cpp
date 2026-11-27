// Deviation between two bodies (opad/deviation.hpp): a BVH over B's triangles, signed nearest distances for samples of A,
// their statistics, histogram and worst places, and an optional best fit (ICP, point-to-plane) of A onto B first.
#include "opad/deviation.hpp"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRep_Tool.hxx>
#include <OSD_Parallel.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax1.hxx>
#include <gp_XYZ.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <cmath>
#include <numeric>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "opad/geometry.hpp"
#include "opad/mesh.hpp"

namespace opad::deviation {
namespace {
constexpr double kInf = std::numeric_limits<double>::infinity();

gp_XYZ vertex(const std::vector<float>& xyz, uint32_t i) { return {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]}; }

// Runs fn(i) for i in [0, n) on OCCT's thread pool, in batches so a cancel stops it between them.
template <class F>
bool parallel(size_t n, const std::function<bool()>& cancelled, F fn) {
  constexpr size_t kBatch = 1 << 16, kChunk = 1024;
  for (size_t start = 0; start < n; start += kBatch) {
    if (cancelled && cancelled()) return false;
    const size_t end = std::min(n, start + kBatch), chunks = (end - start + kChunk - 1) / kChunk;
    OSD_Parallel::For(0, int(chunks), [&](int c) {
      const size_t from = start + size_t(c) * kChunk, to = std::min(end, from + kChunk);
      for (size_t i = from; i < to; ++i) fn(i);
    });
  }
  return true;
}

// Closest point of triangle abc to p (Ericson 5.1.5) and where it lies: 0 inside, 1..3 corner a, b, c, 4 side ab, 5 side
// bc, 6 side ca.
gp_XYZ closest_on_triangle(const gp_XYZ& p, const gp_XYZ& a, const gp_XYZ& b, const gp_XYZ& c, int& region) {
  const gp_XYZ ab = b - a, ac = c - a, ap = p - a;
  const double d1 = ab.Dot(ap), d2 = ac.Dot(ap);
  if (d1 <= 0 && d2 <= 0) return region = 1, a;
  const gp_XYZ bp = p - b;
  const double d3 = ab.Dot(bp), d4 = ac.Dot(bp);
  if (d3 >= 0 && d4 <= d3) return region = 2, b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return region = 4, a + ab * (d1 / (d1 - d3));
  const gp_XYZ cp = p - c;
  const double d5 = ab.Dot(cp), d6 = ac.Dot(cp);
  if (d6 >= 0 && d5 <= d6) return region = 3, c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return region = 6, a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return region = 5, b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double sum = va + vb + vc;
  if (!(std::abs(sum) > 0)) return region = 1, a;  // degenerate
  return region = 0, a + ab * (vb / sum) + ac * (vc / sum);
}

double box_distance2(const float lo[3], const float hi[3], const gp_XYZ& p) {
  double d = 0;
  for (int k = 0; k < 3; ++k) {
    const double v = p.Coord(k + 1);
    if (v < lo[k]) d += (lo[k] - v) * (lo[k] - v);
    else if (v > hi[k]) d += (v - hi[k]) * (v - hi[k]);
  }
  return d;
}

struct CellKey {
  int64_t x, y, z;
  bool operator==(const CellKey&) const = default;
};
struct CellHash {
  size_t operator()(const CellKey& k) const {
    uint64_t h = uint64_t(k.x) * 0x9E3779B97F4A7C15ull;
    h ^= uint64_t(k.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= uint64_t(k.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return size_t(h);
  }
};

double r4(double v) { return std::isfinite(v) ? std::round(v * 1e4) / 1e4 : 0.0; }
json point_json(const gp_Pnt& p) { return json::array({r4(p.X()), r4(p.Y()), r4(p.Z())}); }
}  // namespace

// ---------------------------------------------------------------- triangles

size_t append_offset(Triangles& into, const Triangles& other) {
  int base = 0;
  for (int f : into.face) base = std::max(base, f + 1);
  const uint32_t shift = uint32_t(into.vertex_count());
  into.xyz.insert(into.xyz.end(), other.xyz.begin(), other.xyz.end());
  for (uint32_t i : other.tri) into.tri.push_back(i + shift);
  for (int f : other.face) into.face.push_back(f < 0 ? f : f + base);
  into.cloud = into.cloud || other.cloud;
  return size_t(base);
}

void Triangles::append(const Triangles& other) { append_offset(*this, other); }

Triangles triangles_of(const TopoDS_Shape& world, double deflection) {
  Triangles out;
  if (world.IsNull()) return out;
  const bool mesh = is_mesh_shape(world);
  TopoDS_Shape shape = world;
  if (!mesh) {
    shape = BRepBuilderAPI_Copy(world, Standard_True, Standard_False).Shape();  // the cache keeps its own triangulation
    mesh_shape(shape, deflection, 10.0);
  }
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  // Raw corners first (their box sets the weld's grid), then welded.
  std::vector<double> raw;
  std::vector<uint32_t> rawTri;
  std::vector<int> rawFace;
  int facet = 0;
  for (int i = 1; i <= faces.Extent(); ++i) {
    const TopoDS_Face& face = TopoDS::Face(faces(i));
    TopLoc_Location location;
    const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(face, location);
    if (t.IsNull()) continue;
    const gp_Trsf& place = location.Transformation();
    const uint32_t base = uint32_t(raw.size() / 3);
    for (int n = 1; n <= t->NbNodes(); ++n) {
      const gp_Pnt p = t->Node(n).Transformed(place);
      raw.insert(raw.end(), {p.X(), p.Y(), p.Z()});
    }
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    for (int k = 1; k <= t->NbTriangles(); ++k) {
      int a, b, c;
      t->Triangle(k).Get(a, b, c);
      if (reversed) std::swap(b, c);
      rawTri.insert(rawTri.end(), {base + uint32_t(a - 1), base + uint32_t(b - 1), base + uint32_t(c - 1)});
      rawFace.push_back(mesh ? facet : i - 1);
      ++facet;
    }
  }
  if (rawTri.empty()) {  // a point cloud (a mesh body with nodes and no triangles): its points as they are, point i = node i
    if (mesh)
      for (double c : raw) out.xyz.push_back(float(c));
    out.cloud = !out.xyz.empty();
    return out;
  }
  Bnd_Box box;
  for (size_t i = 0; i < raw.size(); i += 3) box.Add(gp_Pnt(raw[i], raw[i + 1], raw[i + 2]));
  const double diagonal = box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent());
  const double q = std::max(diagonal * 1e-7, 1e-7);
  std::unordered_map<CellKey, uint32_t, CellHash> welded;
  welded.reserve(raw.size() / 3);
  std::vector<uint32_t> map(raw.size() / 3);
  for (size_t v = 0; v < map.size(); ++v) {
    const CellKey key{std::llround(raw[3 * v] / q), std::llround(raw[3 * v + 1] / q), std::llround(raw[3 * v + 2] / q)};
    const auto [it, added] = welded.emplace(key, uint32_t(out.xyz.size() / 3));
    if (added) out.xyz.insert(out.xyz.end(), {float(raw[3 * v]), float(raw[3 * v + 1]), float(raw[3 * v + 2])});
    map[v] = it->second;
  }
  out.tri.reserve(rawTri.size());
  for (size_t t = 0; t < rawTri.size(); t += 3) {
    const uint32_t a = map[rawTri[t]], b = map[rawTri[t + 1]], c = map[rawTri[t + 2]];
    if (a == b || b == c || a == c) continue;  // collapsed by the weld
    out.tri.insert(out.tri.end(), {a, b, c});
    out.face.push_back(rawFace[t / 3]);
  }
  return out;
}

double deflection_for(double tolerance, double diagonal) {
  const double d = std::isfinite(diagonal) && diagonal > 0 ? diagonal : 100.0;
  return std::clamp(tolerance > 0 ? tolerance / 8 : d * 1e-4, std::max(d * 1e-5, 1e-3), std::max(d * 2e-3, 1e-3));
}

// ---------------------------------------------------------------- surface index

std::shared_ptr<const Surface> Surface::build(Triangles t, const std::function<bool()>& cancelled) {
  auto s = std::make_shared<Surface>();
  s->m_xyz = std::move(t.xyz);
  s->m_tri = std::move(t.tri);
  s->m_face = std::move(t.face);
  const size_t nt = s->m_tri.size() / 3, nv = s->m_xyz.size() / 3;
  if (nt == 0) return s;
  for (size_t v = 0; v < nv; ++v) s->m_box.Add(gp_Pnt(s->m_xyz[3 * v], s->m_xyz[3 * v + 1], s->m_xyz[3 * v + 2]));
  // Neighbours across each side: sides sorted by their vertex pair, equal pairs next to each other.
  struct Side { uint64_t key; uint32_t at; };
  std::vector<Side> sides(3 * nt);
  for (size_t i = 0; i < nt; ++i)
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = s->m_tri[3 * i + k], b = s->m_tri[3 * i + (k + 1) % 3];
      sides[3 * i + k] = {uint64_t(std::min(a, b)) << 32 | std::max(a, b), uint32_t(3 * i + k)};
    }
  std::sort(sides.begin(), sides.end(), [](const Side& x, const Side& y) { return x.key < y.key || (x.key == y.key && x.at < y.at); });
  s->m_next.assign(3 * nt, -1);
  for (size_t i = 0; i < sides.size();) {
    size_t j = i + 1;
    while (j < sides.size() && sides[j].key == sides[i].key) ++j;
    if (j - i >= 2)  // a pair (or a non-manifold fan: each with the next one round)
      for (size_t k = i; k < j; ++k) s->m_next[sides[k].at] = int32_t(sides[k + 1 < j ? k + 1 : i].at / 3);
    i = j;
  }
  sides.clear();
  sides.shrink_to_fit();
  if (cancelled && cancelled()) return s;
  s->m_vborder.assign(nv, 0);
  for (size_t i = 0; i < 3 * nt; ++i)
    if (s->m_next[i] < 0) {
      ++s->m_border;
      s->m_vborder[s->m_tri[i]] = s->m_vborder[s->m_tri[i - i % 3 + (i % 3 + 1) % 3]] = 1;
    }
  // Normals, area and volume (turned round when it faces inwards: a closed surface whose volume comes out negative).
  s->m_normal.assign(3 * nt, 0.f);
  double volume = 0;
  for (size_t i = 0; i < nt; ++i) {
    const gp_XYZ a = vertex(s->m_xyz, s->m_tri[3 * i]), b = vertex(s->m_xyz, s->m_tri[3 * i + 1]), c = vertex(s->m_xyz, s->m_tri[3 * i + 2]);
    const gp_XYZ n = (b - a).Crossed(c - a);
    const double len = n.Modulus();
    s->m_area += len / 2;
    volume += a.Dot(b.Crossed(c)) / 6;
    if (len > 0)
      for (int k = 0; k < 3; ++k) s->m_normal[3 * i + k] = float(n.Coord(k + 1) / len);
  }
  if (volume < 0 && s->m_border * 100 < 3 * nt) {
    s->m_flipped = true;
    for (size_t i = 0; i < nt; ++i) {
      std::swap(s->m_tri[3 * i + 1], s->m_tri[3 * i + 2]);
      for (int k = 0; k < 3; ++k) s->m_normal[3 * i + k] = -s->m_normal[3 * i + k];
      // the sides' numbering moved with the corners: side 0 (a,b) is now (a,c) = old side 2, side 2 = old side 0
      std::swap(s->m_next[3 * i], s->m_next[3 * i + 2]);
    }
  }
  // Angle-weighted vertex pseudo-normals (Baerentzen and Aanaes): the sign is right at corners and edges too.
  std::vector<double> vn(3 * nv, 0.0);
  for (size_t i = 0; i < nt; ++i)
    for (int k = 0; k < 3; ++k) {
      const uint32_t v = s->m_tri[3 * i + k];
      const gp_XYZ p = vertex(s->m_xyz, v), e1 = vertex(s->m_xyz, s->m_tri[3 * i + (k + 1) % 3]) - p, e2 = vertex(s->m_xyz, s->m_tri[3 * i + (k + 2) % 3]) - p;
      const double l1 = e1.Modulus(), l2 = e2.Modulus();
      if (!(l1 > 0 && l2 > 0)) continue;
      const double angle = std::acos(std::clamp(e1.Dot(e2) / (l1 * l2), -1.0, 1.0));
      for (int c = 0; c < 3; ++c) vn[3 * v + c] += angle * s->m_normal[3 * i + c];
    }
  s->m_vnormal.assign(3 * nv, 0.f);
  for (size_t v = 0; v < nv; ++v) {
    const double len = std::sqrt(vn[3 * v] * vn[3 * v] + vn[3 * v + 1] * vn[3 * v + 1] + vn[3 * v + 2] * vn[3 * v + 2]);
    if (len > 0)
      for (int c = 0; c < 3; ++c) s->m_vnormal[3 * v + c] = float(vn[3 * v + c] / len);
  }
  if (cancelled && cancelled()) return s;
  s->buildTree();
  return s;
}

void Surface::buildTree() {
  const size_t nt = m_tri.size() / 3;
  std::vector<float> centre(3 * nt);
  for (size_t i = 0; i < nt; ++i)
    for (int k = 0; k < 3; ++k)
      centre[3 * i + k] = (m_xyz[3 * m_tri[3 * i] + k] + m_xyz[3 * m_tri[3 * i + 1] + k] + m_xyz[3 * m_tri[3 * i + 2] + k]) / 3.f;
  m_order.resize(nt);
  std::iota(m_order.begin(), m_order.end(), 0u);
  m_nodes.clear();
  m_nodes.reserve(2 * nt / 4 + 2);
  m_nodes.push_back({});
  struct Task { uint32_t node, first, count; };
  std::vector<Task> stack{{0, 0, uint32_t(nt)}};
  while (!stack.empty()) {
    const Task task = stack.back();
    stack.pop_back();
    Node bounds;
    float clo[3], chi[3];
    for (int k = 0; k < 3; ++k) bounds.lo[k] = clo[k] = std::numeric_limits<float>::max(), bounds.hi[k] = chi[k] = -std::numeric_limits<float>::max();
    for (uint32_t i = task.first; i < task.first + task.count; ++i) {
      const uint32_t t = m_order[i];
      for (int c = 0; c < 3; ++c) {
        const float* p = &m_xyz[3 * m_tri[3 * t + c]];
        for (int k = 0; k < 3; ++k) bounds.lo[k] = std::min(bounds.lo[k], p[k]), bounds.hi[k] = std::max(bounds.hi[k], p[k]);
      }
      for (int k = 0; k < 3; ++k) clo[k] = std::min(clo[k], centre[3 * t + k]), chi[k] = std::max(chi[k], centre[3 * t + k]);
    }
    Node& node = m_nodes[task.node];
    std::copy(bounds.lo, bounds.lo + 3, node.lo);
    std::copy(bounds.hi, bounds.hi + 3, node.hi);
    int axis = 0;
    for (int k = 1; k < 3; ++k)
      if (chi[k] - clo[k] > chi[axis] - clo[axis]) axis = k;
    if (task.count <= 4 || !(chi[axis] > clo[axis])) {
      node.first = task.first;
      node.count = task.count;
      continue;
    }
    const uint32_t half = task.count / 2;
    std::nth_element(m_order.begin() + task.first, m_order.begin() + task.first + half, m_order.begin() + task.first + task.count,
                     [&](uint32_t x, uint32_t y) { return centre[3 * x + axis] < centre[3 * y + axis]; });
    const uint32_t child = uint32_t(m_nodes.size());
    m_nodes[task.node].first = child;
    m_nodes[task.node].count = 0;
    m_nodes.push_back({});
    m_nodes.push_back({});
    stack.push_back({child, task.first, half});
    stack.push_back({child + 1, task.first + half, task.count - half});
  }
}

Surface::Hit Surface::nearest(const gp_Pnt& at, double reach) const {
  Hit hit;
  if (m_nodes.empty()) return hit;
  const gp_XYZ p = at.XYZ();
  double best = std::isfinite(reach) ? reach * reach : kInf;
  int bestTri = -1, bestRegion = 0;
  gp_XYZ bestPoint;
  uint32_t stack[128];
  int top = 0;
  if (box_distance2(m_nodes[0].lo, m_nodes[0].hi, p) > best) return hit;
  stack[top++] = 0;
  while (top > 0) {
    const Node& node = m_nodes[stack[--top]];
    if (box_distance2(node.lo, node.hi, p) > best) continue;
    if (node.count > 0) {
      for (uint32_t i = node.first; i < node.first + node.count; ++i) {
        const uint32_t t = m_order[i];
        int region = 0;
        const gp_XYZ q = closest_on_triangle(p, vertex(m_xyz, m_tri[3 * t]), vertex(m_xyz, m_tri[3 * t + 1]), vertex(m_xyz, m_tri[3 * t + 2]), region);
        const double d = (p - q).SquareModulus();
        if (d < best || (d == best && bestTri < 0)) best = d, bestTri = int(t), bestRegion = region, bestPoint = q;
      }
      continue;
    }
    const double d0 = box_distance2(m_nodes[node.first].lo, m_nodes[node.first].hi, p), d1 = box_distance2(m_nodes[node.first + 1].lo, m_nodes[node.first + 1].hi, p);
    if (top + 2 > 128) continue;  // never at these depths (median splits)
    if (d0 <= d1) {
      if (d1 <= best) stack[top++] = node.first + 1;
      if (d0 <= best) stack[top++] = node.first;
    } else {
      if (d0 <= best) stack[top++] = node.first;
      if (d1 <= best) stack[top++] = node.first + 1;
    }
  }
  if (bestTri < 0) return hit;
  const size_t t = size_t(bestTri);
  gp_XYZ n(m_normal[3 * t], m_normal[3 * t + 1], m_normal[3 * t + 2]);
  bool defined = true;
  if (bestRegion >= 1 && bestRegion <= 3) {
    const uint32_t v = m_tri[3 * t + size_t(bestRegion - 1)];
    n = gp_XYZ(m_vnormal[3 * v], m_vnormal[3 * v + 1], m_vnormal[3 * v + 2]);
    defined = !m_vborder[v];
  } else if (bestRegion >= 4) {
    const int across = m_next[3 * t + size_t(bestRegion - 4)];
    if (across < 0) defined = false;
    else n += gp_XYZ(m_normal[3 * size_t(across)], m_normal[3 * size_t(across) + 1], m_normal[3 * size_t(across) + 2]);
  }
  if (n.Modulus() < 1e-12) {
    n = gp_XYZ(m_normal[3 * t], m_normal[3 * t + 1], m_normal[3 * t + 2]);
    if (n.Modulus() < 1e-12) n = gp_XYZ(0, 0, 1), defined = false;
  }
  hit.normal = gp_Dir(n);
  hit.point = gp_Pnt(bestPoint);
  hit.triangle = bestTri;
  const double d = std::sqrt(best);
  const double side = (p - bestPoint).Dot(hit.normal.XYZ());
  hit.distance = side < 0 ? -d : d;
  hit.sign_defined = defined || d < 1e-9;
  return hit;
}

// ---------------------------------------------------------------- sampling and measuring

Samples sample(const Triangles& a, bool mesh, size_t limit, const Region& region) {
  Samples out;
  const size_t nt = a.triangle_count();
  limit = std::max<size_t>(limit, 1);
  std::unordered_set<int> faces(region.faces.begin(), region.faces.end());
  auto inBox = [&](const gp_Pnt& p) { return region.box.IsVoid() || !region.box.IsOut(p); };
  std::vector<uint8_t> keepTri(nt, 1);
  if (!faces.empty())
    for (size_t t = 0; t < nt; ++t) keepTri[t] = faces.count(a.face[t]) ? 1 : 0;
  if (mesh) {
    out.source = a.cloud && a.tri.empty() ? "cloud points" : "mesh vertices";
    out.population = a.vertex_count();
    std::vector<int> faceOf(a.vertex_count(), -2);  // -2: on no kept triangle
    for (size_t t = 0; t < nt; ++t)
      if (keepTri[t])
        for (int k = 0; k < 3; ++k)
          if (faceOf[a.tri[3 * t + k]] == -2) faceOf[a.tri[3 * t + k]] = a.face[t];
    std::vector<uint32_t> chosen;
    for (uint32_t v = 0; v < faceOf.size(); ++v)
      if ((faceOf[v] != -2 || (a.cloud && faces.empty())) && inBox(gp_Pnt(vertex(a.xyz, v)))) chosen.push_back(v);  // a cloud's points belong to no triangle
    out.region_population = chosen.size();
    const size_t n = std::min(limit, chosen.size());
    out.subsampled = n < chosen.size();
    out.points.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t v = chosen[out.subsampled ? size_t((double(i) + 0.5) * double(chosen.size()) / double(n)) : i];
      out.points.emplace_back(vertex(a.xyz, v));
      out.face.push_back(std::max(faceOf[v], -1));
    }
    return out;
  }
  // A solid's surface: points spread by area over the kept triangles (fixed seed: the same body gives the same points).
  out.source = "surface samples";
  out.population = nt;
  std::vector<double> cumulative;
  std::vector<uint32_t> which;
  double total = 0;
  for (size_t t = 0; t < nt; ++t) {
    if (!keepTri[t]) continue;
    const gp_XYZ p = vertex(a.xyz, a.tri[3 * t]), q = vertex(a.xyz, a.tri[3 * t + 1]), r = vertex(a.xyz, a.tri[3 * t + 2]);
    if (!region.box.IsVoid()) {  // a triangle reaching into the box: its points outside are dropped below
      Bnd_Box own;
      own.Add(gp_Pnt(p)), own.Add(gp_Pnt(q)), own.Add(gp_Pnt(r));
      if (region.box.IsOut(own)) continue;
    }
    const double area = (q - p).Crossed(r - p).Modulus() / 2;
    if (!(area > 0)) continue;
    total += area;
    cumulative.push_back(total);
    which.push_back(uint32_t(t));
  }
  out.region_population = which.size();
  if (which.empty()) return out;
  std::mt19937_64 random(0x5eed);
  std::uniform_real_distribution<double> unit(0, 1);
  const size_t n = limit;
  out.points.reserve(n);
  for (size_t tries = 0; out.points.size() < n && tries < 50 * n; ++tries) {
    const auto at = std::lower_bound(cumulative.begin(), cumulative.end(), unit(random) * total);
    const uint32_t t = which[size_t(std::min<ptrdiff_t>(at - cumulative.begin(), ptrdiff_t(which.size()) - 1))];
    double u = unit(random), v = unit(random);
    if (u + v > 1) u = 1 - u, v = 1 - v;
    const gp_XYZ p = vertex(a.xyz, a.tri[3 * t]), q = vertex(a.xyz, a.tri[3 * t + 1]), r = vertex(a.xyz, a.tri[3 * t + 2]);
    const gp_Pnt point(p + (q - p) * u + (r - p) * v);
    if (!region.box.IsVoid() && region.box.IsOut(point)) continue;
    out.points.push_back(point);
    out.face.push_back(a.face[t]);
  }
  out.subsampled = true;  // a surface is always sampled
  return out;
}

Values measure(const Samples& from, const Surface& to, double max_distance, const std::vector<TopoDS_Shape>& exact_faces, const std::function<bool()>& cancelled) {
  Values out;
  const size_t n = from.points.size();
  out.value.assign(n, std::numeric_limits<float>::quiet_NaN());
  out.nearest.assign(n, gp_Pnt());
  out.undefined.assign(n, 0);
  const double reach = max_distance > 0 ? max_distance : kInf;
  out.exact = !exact_faces.empty();
  parallel(n, cancelled, [&](size_t i) {
    const Surface::Hit hit = to.nearest(from.points[i], reach);
    if (!hit.found()) return;
    double d = hit.distance;
    gp_Pnt q = hit.point;
    if (out.exact) {
      const int face = to.face_of(hit.triangle);
      if (face >= 0 && size_t(face) < exact_faces.size() && !exact_faces[size_t(face)].IsNull()) {
        BRepExtrema_DistShapeShape exact(BRepBuilderAPI_MakeVertex(from.points[i]).Vertex(), exact_faces[size_t(face)], Extrema_ExtFlag_MIN);
        if (exact.IsDone() && exact.NbSolution() > 0) {
          q = exact.PointOnShape2(1);
          const double side = gp_Vec(q, from.points[i]).Dot(gp_Vec(hit.normal));
          d = (std::abs(side) > 1e-12 ? (side < 0 ? -1 : 1) : (d < 0 ? -1 : 1)) * exact.Value();
        }
      }
    }
    if (std::isfinite(reach) && std::abs(d) > reach) return;
    out.value[i] = float(d);
    out.nearest[i] = q;
    out.undefined[i] = hit.sign_defined ? 0 : 1;
  });
  for (size_t i = 0; i < n; ++i) {
    if (!std::isfinite(out.value[i])) ++out.ignored;
    else if (out.undefined[i]) ++out.undefined_count;
  }
  return out;
}

Statistics statistics(const std::vector<float>& values, double tolerance) {
  Statistics s;
  std::vector<float> v, a;
  v.reserve(values.size());
  for (float x : values)
    if (std::isfinite(x)) v.push_back(x);
  s.count = v.size();
  if (v.empty()) return s;
  double sum = 0, sumAbs = 0, sum2 = 0;
  s.min = kInf, s.max = -kInf;
  size_t within = 0, outside = 0, inside = 0;
  a.reserve(v.size());
  for (float x : v) {
    sum += x, sumAbs += std::abs(x), sum2 += double(x) * x;
    s.min = std::min(s.min, double(x)), s.max = std::max(s.max, double(x));
    a.push_back(std::abs(x));
    if (std::abs(x) <= tolerance) ++within;
    else if (x > 0) ++outside;
    else ++inside;
  }
  const double n = double(v.size());
  s.mean = sum / n, s.mean_abs = sumAbs / n, s.rms = std::sqrt(sum2 / n);
  s.std_dev = std::sqrt(std::max(0.0, sum2 / n - s.mean * s.mean));
  s.max_abs = std::max(std::abs(s.min), std::abs(s.max));
  s.within = within / n, s.outside = outside / n, s.inside = inside / n;
  auto at = [](std::vector<float>& x, double q) {
    const size_t k = std::min(x.size() - 1, size_t(q * double(x.size() - 1) + 0.5));
    std::nth_element(x.begin(), x.begin() + ptrdiff_t(k), x.end());
    return double(x[k]);
  };
  s.median = at(v, 0.5);
  s.median_abs = at(a, 0.5), s.p90_abs = at(a, 0.9), s.p95_abs = at(a, 0.95), s.p99_abs = at(a, 0.99);
  return s;
}

Histogram histogram(const std::vector<float>& values, double tolerance, int bins, double range) {
  Histogram h;
  bins = std::clamp(bins, 2, 400);
  if (!(range > 0)) {
    const Statistics s = statistics(values, tolerance);
    range = std::max({s.p99_abs, 2 * tolerance, 1e-3});
    // A round number: 1, 2 or 5 times a power of ten.
    const double ten = std::pow(10.0, std::floor(std::log10(range)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
      if (m * ten >= range * 0.999) { range = m * ten; break; }
  }
  h.from = -range, h.to = range, h.bin = 2 * range / bins;
  h.counts.assign(size_t(bins), 0);
  for (float x : values) {
    if (!std::isfinite(x)) continue;
    if (x < h.from) ++h.below;
    else if (x > h.to) ++h.above;
    else ++h.counts[std::min(size_t(bins) - 1, size_t((x - h.from) / h.bin))];
  }
  return h;
}

std::vector<Spot> worst_spots(const Samples& s, const Values& v, double tolerance, int count, double radius) {
  std::vector<Spot> out;
  if (count <= 0) return out;
  std::vector<uint32_t> beyond;
  for (uint32_t i = 0; i < v.value.size(); ++i)
    if (std::isfinite(v.value[i]) && std::abs(v.value[i]) > tolerance) beyond.push_back(i);
  std::sort(beyond.begin(), beyond.end(), [&](uint32_t a, uint32_t b) { return std::abs(v.value[a]) > std::abs(v.value[b]); });
  const double r2 = radius * radius;
  for (uint32_t i : beyond) {
    const gp_Pnt& p = s.points[i];
    bool taken = false;
    for (const Spot& spot : out)
      if (spot.at.SquareDistance(p) < r2) { taken = true; break; }
    if (taken) continue;
    out.push_back({p, v.nearest[i], double(v.value[i]), 0});
    if (int(out.size()) >= count) break;
  }
  for (uint32_t i : beyond)
    for (Spot& spot : out)
      if (spot.at.SquareDistance(s.points[i]) < r2) { ++spot.points; break; }
  return out;
}

// ---------------------------------------------------------------- best fit

namespace {
// Solves the 6x6 system m x = b (Gaussian elimination, partial pivoting); false when singular.
bool solve6(std::array<std::array<double, 6>, 6> m, std::array<double, 6> b, std::array<double, 6>& x) {
  for (int c = 0; c < 6; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 6; ++r)
      if (std::abs(m[r][c]) > std::abs(m[pivot][c])) pivot = r;
    if (!(std::abs(m[pivot][c]) > 1e-300)) return false;
    std::swap(m[c], m[pivot]);
    std::swap(b[c], b[pivot]);
    for (int r = c + 1; r < 6; ++r) {
      const double f = m[r][c] / m[c][c];
      for (int k = c; k < 6; ++k) m[r][k] -= f * m[c][k];
      b[r] -= f * b[c];
    }
  }
  for (int r = 5; r >= 0; --r) {
    double s = b[r];
    for (int k = r + 1; k < 6; ++k) s -= m[r][k] * x[k];
    x[r] = s / m[r][r];
  }
  return true;
}
}  // namespace

Fit best_fit(const std::vector<gp_Pnt>& points, const Surface& to, double reach, int max_iterations, const std::function<bool()>& cancelled) {
  Fit fit;
  if (points.size() < 6 || to.triangle_count() == 0) return fit;
  gp_XYZ centre(0, 0, 0);
  Bnd_Box box;
  for (const auto& p : points) centre += p.XYZ(), box.Add(p);
  centre /= double(points.size());
  const double size = std::max(std::sqrt(box.SquareExtent()), 1e-6);
  struct Pair { gp_XYZ q, n; double r = 0, d = kInf; };
  std::vector<Pair> pairs(points.size());
  gp_Trsf motion;
  auto gather = [&](const gp_Trsf& m) {
    parallel(points.size(), cancelled, [&](size_t i) {
      const gp_Pnt q = points[i].Transformed(m);
      const Surface::Hit hit = to.nearest(q, reach);
      Pair& pr = pairs[i];
      pr.q = q.XYZ();
      if (!hit.found()) { pr.d = kInf; return; }
      pr.n = hit.normal.XYZ();
      pr.r = (q.XYZ() - hit.point.XYZ()).Dot(pr.n);
      pr.d = std::abs(hit.distance);
    });
  };
  auto threshold = [&](bool first) {
    if (first) return reach;
    std::vector<double> d;
    for (const auto& p : pairs)
      if (std::isfinite(p.d)) d.push_back(p.d);
    if (d.empty()) return reach;
    std::nth_element(d.begin(), d.begin() + ptrdiff_t(d.size() / 2), d.end());
    return std::min(reach, std::max(3 * d[d.size() / 2], size * 1e-5));
  };
  auto rms = [&](double limit, int* used) {
    double s = 0;
    int n = 0;
    for (const auto& p : pairs)
      if (p.d <= limit) s += p.r * p.r, ++n;
    if (used) *used = n;
    return n ? std::sqrt(s / n) : 0.0;
  };
  gather(motion);
  double limit = threshold(true);
  fit.rms_before = rms(limit, nullptr);
  for (int it = 0; it < max_iterations; ++it) {
    if (cancelled && cancelled()) break;
    const gp_XYZ c = gp_Pnt(centre).Transformed(motion).XYZ();
    std::array<std::array<double, 6>, 6> m{};
    std::array<double, 6> b{};
    int used = 0;
    for (const auto& p : pairs) {
      if (!(p.d <= limit)) continue;
      const gp_XYZ arm = (p.q - c).Crossed(p.n);
      const double j[6] = {arm.X(), arm.Y(), arm.Z(), p.n.X(), p.n.Y(), p.n.Z()};
      for (int r = 0; r < 6; ++r) {
        for (int k = 0; k < 6; ++k) m[r][k] += j[r] * j[k];
        b[r] -= j[r] * p.r;
      }
      ++used;
    }
    if (used < 6) break;
    double trace = 0;
    for (int r = 0; r < 6; ++r) trace += m[r][r];
    for (int r = 0; r < 6; ++r) m[r][r] += trace * 1e-9 + 1e-12;  // directions the shape does not fix (a plane slides) stay put
    std::array<double, 6> x{};
    if (!solve6(m, b, x)) break;
    const gp_XYZ w(x[0], x[1], x[2]), t(x[3], x[4], x[5]);
    gp_Trsf step;
    if (w.Modulus() > 1e-15) step.SetRotation(gp_Ax1(gp_Pnt(c), gp_Dir(w)), w.Modulus());
    gp_Trsf shift;
    shift.SetTranslation(gp_Vec(t));
    motion = shift * step * motion;
    fit.iterations = it + 1;
    gather(motion);
    limit = threshold(false);
    if (w.Modulus() < 1e-8 && t.Modulus() < size * 1e-8) {
      fit.converged = true;
      break;
    }
  }
  fit.motion = motion;
  fit.rms_after = rms(limit, &fit.pairs);
  return fit;
}

// ---------------------------------------------------------------- the study and the command

namespace {
std::vector<std::string> bodies_of(const Scene& scene, const json& v, const char* side) {
  std::vector<std::string> ids, out;
  if (v.is_string()) ids.push_back(v.get<std::string>());
  else if (v.is_array())
    for (const auto& x : v)
      if (x.is_string()) ids.push_back(x.get<std::string>());
  if (ids.empty()) throw Error(std::string("Name the ") + side + " body: a body or component id.");
  for (const auto& id : ids) {
    if (!scene.node(id)) throw Error("Unknown body or component " + id + " for " + side + ".");
    for (const auto& b : scene.bodies_under(id))
      if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
  }
  if (out.empty()) throw Error(std::string("The ") + side + " side holds no body.");
  return out;
}

double diagonal_of(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies) {
  Bnd_Box box;
  for (const auto& b : bodies) {
    const Bnd_Box one = node_world_bbox(doc, scene, b);
    if (!one.IsVoid()) box.Add(one);
  }
  return box.IsVoid() ? 100.0 : std::sqrt(box.SquareExtent());
}

json stats_json(const Statistics& s) {
  return {{"count", s.count}, {"mean", r4(s.mean)}, {"mean_abs", r4(s.mean_abs)}, {"median", r4(s.median)}, {"median_abs", r4(s.median_abs)},
          {"rms", r4(s.rms)}, {"std_dev", r4(s.std_dev)}, {"min", r4(s.min)}, {"max", r4(s.max)}, {"max_abs", r4(s.max_abs)},
          {"p90_abs", r4(s.p90_abs)}, {"p95_abs", r4(s.p95_abs)}, {"p99_abs", r4(s.p99_abs)},
          {"within_tolerance_pct", r4(100 * s.within)}, {"outside_pct", r4(100 * s.outside)}, {"inside_pct", r4(100 * s.inside)}};
}

json histogram_json(const Histogram& h) {
  return {{"from", r4(h.from)}, {"to", r4(h.to)}, {"bin_mm", r4(h.bin)}, {"counts", h.counts}, {"below", h.below}, {"above", h.above}};
}

json spots_json(const std::vector<Spot>& spots) {
  json out = json::array();
  for (const auto& s : spots)
    out.push_back({{"position", point_json(s.at)}, {"value", r4(s.value)}, {"nearest", point_json(s.nearest)}, {"points", s.points}});
  return out;
}

json sampling_json(const Samples& s, size_t limit) {
  json j = {{"source", s.source}, {"points", s.points.size()}, {"limit", limit}, {"subsampled", s.subsampled}};
  j[s.source == "mesh vertices" ? "vertices" : s.source == "cloud points" ? "cloud_points" : "triangles"] = s.population;
  if (s.region_population != s.population) j["in_region"] = s.region_population;
  return j;
}

json side_report(const Samples& s, const Values& v, double tolerance, int bins, double range, int worst, double radius, size_t limit) {
  json j = {{"sampling", sampling_json(s, limit)}, {"stats", stats_json(statistics(v.value, tolerance))}, {"histogram", histogram_json(histogram(v.value, tolerance, bins, range))},
            {"worst", spots_json(worst_spots(s, v, tolerance, worst, radius))}, {"ignored", v.ignored}, {"sign_undefined", v.undefined_count}};
  j["sampling"]["exact"] = v.exact;
  return j;
}

std::vector<TopoDS_Shape> exact_faces_of(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies) {
  std::vector<TopoDS_Shape> out;
  for (const auto& b : bodies) {  // the same ordinals triangles_of + append gave (each body's faces after the previous ones)
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(node_world_shape(doc, scene, b), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) out.push_back(faces(i));
  }
  return out;
}
}  // namespace

Study run(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled, const ProgressFn& progress) {
  auto say = [&](const std::string& phase, int percent) {
    if (progress) progress(phase, percent);
  };
  auto stopped = [&] {
    if (cancelled && cancelled()) throw Error("Deviation cancelled.");
  };
  Study study;
  study.from_bodies = bodies_of(scene, args.value("from", json()), "from");
  study.to_bodies = bodies_of(scene, args.value("to", json()), "to");
  for (const auto& b : study.from_bodies)
    if (std::find(study.to_bodies.begin(), study.to_bodies.end(), b) != study.to_bodies.end())
      throw Error("A body cannot be measured against itself: from and to share " + b + ".");
  const double tolerance = args.contains("tolerance") ? args["tolerance"].get<double>() : 0.1;
  if (!(tolerance >= 0)) throw Error("tolerance must be 0 or more (mm).");
  const double max_distance = args.value("max_distance", 0.0);
  const size_t limit = std::clamp<size_t>(args.value("sample", size_t(500000)), 100, 5000000);
  const int bins = std::clamp(args.value("bins", 40), 2, 400), worst = std::clamp(args.value("worst", 8), 0, 100);
  const double range = args.value("range", 0.0);
  const bool both = args.value("both_ways", false), align = args.value("align", false);

  // Both sides' triangles, world mm, each body's faces numbered after the previous body's.
  auto collect = [&](const std::vector<std::string>& bodies, double deflection, bool& mesh, std::map<std::string, size_t>* bases) {
    Triangles all;
    mesh = true;
    for (const auto& b : bodies) {
      stopped();
      const TopoDS_Shape world = node_world_shape(doc, scene, b);
      if (!is_mesh_shape(world)) mesh = false;
      const size_t base = append_offset(all, triangles_of(world, deflection));
      if (bases) (*bases)[b] = base;
    }
    return all;
  };
  const double diagA = diagonal_of(doc, scene, study.from_bodies), diagB = diagonal_of(doc, scene, study.to_bodies);
  const double deflA = deflection_for(tolerance, diagA), deflB = deflection_for(tolerance, diagB);
  say("Meshing the reference", 2);
  bool meshB = false, meshA = false;
  Triangles trisB = collect(study.to_bodies, deflB, meshB, nullptr);
  if (trisB.tri.empty())
    throw Error(trisB.cloud ? "The to side is a point cloud, which has no surface to measure against: measure the cloud (from) against a mesh or solid (to)."
                            : "The to side has no surface to measure against (no faces).");
  const size_t trianglesB = trisB.triangle_count();
  say("Indexing the reference", 10);
  study.to = Surface::build(std::move(trisB), cancelled);
  stopped();
  say("Sampling", 25);
  std::map<std::string, size_t> bases;
  Triangles trisA = collect(study.from_bodies, deflA, meshA, &bases);
  if (trisA.tri.empty() && !trisA.cloud) throw Error("The from side has no surface to measure (no faces).");
  Region region;
  if (const json r = args.value("region", json()); r.is_object()) {
    const json lo = r.value("min", json()), hi = r.value("max", json());
    if (!lo.is_array() || !hi.is_array() || lo.size() != 3 || hi.size() != 3) throw Error("region box needs min and max, [x, y, z] in mm.");
    region.box.Update(lo[0].get<double>(), lo[1].get<double>(), lo[2].get<double>(), hi[0].get<double>(), hi[1].get<double>(), hi[2].get<double>());
  } else if (r.is_array()) {
    for (const auto& item : r) {
      const Ref ref = item.is_string() ? Ref::parse(item.get<std::string>()) : Ref::from_json(item);
      const auto base = bases.find(ref.body);
      if (base == bases.end()) throw Error("region names " + ref.str() + ", which is not on the from side.");
      if (ref.kind == Ref::Kind::Body) {  // a whole body of several: its faces, which lie in [its base, the next body's)
        size_t next = std::numeric_limits<size_t>::max();
        for (const auto& [id, b] : bases)
          if (b > base->second) next = std::min(next, b);
        std::unordered_set<int> seen;
        for (int f : trisA.face)
          if (f >= 0 && size_t(f) >= base->second && size_t(f) < next && seen.insert(f).second) region.faces.push_back(f);
        continue;
      }
      if (ref.kind != Ref::Kind::Face) throw Error("region takes faces (or facets of a mesh) of the from body, or a box {min, max}.");
      region.faces.push_back(int(base->second) + ref.index);
    }
    if (region.faces.empty() && !r.empty()) throw Error("region selects nothing.");
  }
  study.samples = sample(trisA, meshA, limit, region);
  if (study.samples.points.empty()) throw Error("Nothing to measure: the region holds no part of the from body.");
  stopped();
  json report;
  if (align) {
    say("Best fit", 35);
    std::vector<gp_Pnt> subset;
    const size_t step = std::max<size_t>(1, study.samples.points.size() / 20000);
    for (size_t i = 0; i < study.samples.points.size(); i += step) subset.push_back(study.samples.points[i]);
    const Fit fit = best_fit(subset, *study.to, max_distance > 0 ? max_distance : kInf, 60, cancelled);
    stopped();
    study.alignment = fit.motion;
    for (auto& p : study.samples.points) p.Transform(fit.motion);
    const gp_XYZ moved = gp_Pnt(0, 0, 0).Transformed(fit.motion).XYZ();
    gp_XYZ axis;
    double angle = 0;
    fit.motion.GetRotation(axis, angle);
    report["alignment"] = {{"matrix", mat_from_trsf(fit.motion).to_json()}, {"rms_before", r4(fit.rms_before)}, {"rms_after", r4(fit.rms_after)},
                           {"iterations", fit.iterations}, {"pairs", fit.pairs}, {"converged", fit.converged}, {"turned_deg", r4(angle * 180 / M_PI)},
                           {"origin_moved_to", point_json(gp_Pnt(moved))},
                           {"note", "Measured after moving from by matrix (world). The document is unchanged: apply it with transform to keep it."}};
  }
  const bool exactWanted = args.contains("exact") && args["exact"].is_boolean() ? args["exact"].get<bool>() : (!meshB && study.samples.points.size() <= 2000);
  const std::vector<TopoDS_Shape> exact = exactWanted && !meshB ? exact_faces_of(doc, scene, study.to_bodies) : std::vector<TopoDS_Shape>();
  say("Measuring", 45);
  study.values = measure(study.samples, *study.to, max_distance, exact, cancelled);
  stopped();
  study.size = diagA;
  study.limit = limit;
  const double radius = std::max(10 * tolerance, diagA * 0.06);
  json forward = side_report(study.samples, study.values, tolerance, bins, range, worst, radius, limit);
  report["from"] = {{"bodies", study.from_bodies}, {"kind", trisA.tri.empty() ? "cloud" : meshA ? "mesh" : "solid"}, {"triangles", trisA.triangle_count()}};
  if (!meshA) report["from"]["deflection_mm"] = r4(deflA);
  report["to"] = {{"bodies", study.to_bodies}, {"kind", meshB ? "mesh" : "solid"}, {"triangles", trianglesB}, {"closed", study.to->closed()},
                  {"open_border_edges", study.to->border_edges()}};
  if (!meshB) report["to"]["deflection_mm"] = r4(deflB);
  report["tolerance_mm"] = r4(tolerance);
  if (max_distance > 0) report["max_distance_mm"] = r4(max_distance);
  report["sign"] = "+ = from lies outside to (material added), - = inside to (material missing)";
  report["sign_defined"] = study.to->closed() && study.values.undefined_count == 0;
  if (!study.to->closed())
    report["sign_note"] = "to is open (" + std::to_string(study.to->border_edges()) + " border edges): where the nearest point lies on its border the sign is undefined (sign_undefined counts those points; their size still counts).";
  for (auto& [k, v] : forward.items()) report[k] = v;
  if (both && trisA.tri.empty()) {
    report["reverse_note"] = "from is a point cloud: it has no surface to measure to against, so both_ways was skipped.";
  } else if (both) {
    stopped();
    say("Measuring back", 75);
    if (align)  // A where it was measured: moved by the fit
      for (size_t v = 0; v < trisA.vertex_count(); ++v) {
        gp_Pnt p(trisA.xyz[3 * v], trisA.xyz[3 * v + 1], trisA.xyz[3 * v + 2]);
        p.Transform(study.alignment);
        trisA.xyz[3 * v] = float(p.X()), trisA.xyz[3 * v + 1] = float(p.Y()), trisA.xyz[3 * v + 2] = float(p.Z());
      }
    study.from = Surface::build(trisA, cancelled);
    stopped();
    bool meshAgain = false;
    const Triangles trisB2 = collect(study.to_bodies, deflB, meshAgain, nullptr);
    const Samples back = sample(trisB2, meshB, limit);
    const Values backValues = measure(back, *study.from, max_distance, {}, cancelled);
    stopped();
    json reverse = side_report(back, backValues, tolerance, bins, range, worst, std::max(10 * tolerance, diagB * 0.06), limit);
    reverse["sign"] = "+ = to lies outside from, - = inside from";
    reverse["from_closed"] = study.from->closed();
    report["reverse"] = reverse;
  }
  say("Done", 100);
  study.report = std::move(report);
  return study;
}

json summary(const Study& study, double tolerance, int bins, double range, int worst) {
  return side_report(study.samples, study.values, tolerance, bins, range, worst, std::max(10 * tolerance, study.size * 0.06), study.limit);
}

json command(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled) {
  return run(doc, scene, args, cancelled).report;
}

}  // namespace opad::deviation
