// Mesh bodies as plain triangle arrays (opad/mesh_edit.hpp): read from and written back to the shape a mesh body is (one
// face, triangulation only), welded, mapped, measured and split into shells.
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <sstream>
#include <unordered_map>

#include "opad/geometry.hpp"
#include "opad/mesh_edit.hpp"
#include "opad/util.hpp"

namespace opad::meshops {
namespace {

struct Key {
  uint64_t x, y, z;
  bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = k.x * 0x9E3779B97F4A7C15ull;
    h ^= k.y + 0x7F4A7C159E3779B9ull + (h << 6) + (h >> 2);
    h ^= k.z + 0x94D049BB133111EBull + (h << 6) + (h >> 2);
    return static_cast<size_t>(h);
  }
};
uint64_t bits(double d) {
  if (d == 0.0) d = 0.0;  // -0 == +0
  uint64_t b;
  std::memcpy(&b, &d, 8);
  return b;
}

// Union-find over vertex indices.
struct Sets {
  std::vector<uint32_t> up;
  explicit Sets(size_t n) : up(n) { std::iota(up.begin(), up.end(), 0u); }
  uint32_t find(uint32_t a) {
    while (up[a] != a) a = up[a] = up[up[a]];
    return a;
  }
  void join(uint32_t a, uint32_t b) {
    a = find(a), b = find(b);
    if (a != b) up[std::max(a, b)] = std::min(a, b);
  }
};

}  // namespace

// ---------------------------------------------------------------- weld
// Vertices at exactly the same place become one; with `tol` > 0, those within tol of each other too (grid cells of tol,
// each vertex checked against the 27 cells around it; the first one met stands for the rest).
static Mesh welded(const Mesh& in, double tol, size_t* merged) {
  const size_t nv = in.vertices();
  std::vector<uint32_t> remap(nv);
  Mesh out;
  out.xyz.reserve(in.xyz.size());
  if (tol <= 0) {
    std::unordered_map<Key, uint32_t, KeyHash> index;
    index.reserve(nv);
    for (size_t v = 0; v < nv; ++v) {
      const Key k{bits(in.xyz[v * 3]), bits(in.xyz[v * 3 + 1]), bits(in.xyz[v * 3 + 2])};
      auto [it, added] = index.emplace(k, static_cast<uint32_t>(out.vertices()));
      if (added) out.xyz.insert(out.xyz.end(), {in.xyz[v * 3], in.xyz[v * 3 + 1], in.xyz[v * 3 + 2]});
      remap[v] = it->second;
    }
  } else {
    std::unordered_map<Key, std::vector<uint32_t>, KeyHash> cells;
    cells.reserve(nv);
    auto cell = [&](double c) { return static_cast<int64_t>(std::floor(c / tol)); };
    const double tol2 = tol * tol;
    for (size_t v = 0; v < nv; ++v) {
      const double* p = &in.xyz[v * 3];
      const int64_t cx = cell(p[0]), cy = cell(p[1]), cz = cell(p[2]);
      int64_t found = -1;
      for (int dx = -1; dx <= 1 && found < 0; ++dx)
        for (int dy = -1; dy <= 1 && found < 0; ++dy)
          for (int dz = -1; dz <= 1 && found < 0; ++dz) {
            auto it = cells.find({uint64_t(cx + dx), uint64_t(cy + dy), uint64_t(cz + dz)});
            if (it == cells.end()) continue;
            for (uint32_t o : it->second) {
              const double* q = &out.xyz[size_t(o) * 3];
              const double d2 = (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
              if (d2 <= tol2) {
                found = o;
                break;
              }
            }
          }
      if (found < 0) {
        found = static_cast<int64_t>(out.vertices());
        out.xyz.insert(out.xyz.end(), {p[0], p[1], p[2]});
        cells[{uint64_t(cx), uint64_t(cy), uint64_t(cz)}].push_back(static_cast<uint32_t>(found));
      }
      remap[v] = static_cast<uint32_t>(found);
    }
  }
  if (merged) *merged = nv - out.vertices();
  out.tri.resize(in.tri.size());
  for (size_t i = 0; i < in.tri.size(); ++i) out.tri[i] = remap[in.tri[i]];
  return out;
}

Mesh weld(const Mesh& in, double tol, size_t* merged) { return welded(in, tol, merged); }

// ---------------------------------------------------------------- shapes
Mesh from_shape(const TopoDS_Shape& shape, double weldTol) {
  Mesh raw;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face& face = TopoDS::Face(e.Current());
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(face, loc);
    if (t.IsNull() || t->NbTriangles() == 0) continue;
    const gp_Trsf where = loc.Transformation();
    const bool moved = !loc.IsIdentity();
    const uint32_t base = static_cast<uint32_t>(raw.vertices());
    for (int i = 1; i <= t->NbNodes(); ++i) {
      gp_Pnt p = t->Node(i);
      if (moved) p.Transform(where);
      raw.xyz.insert(raw.xyz.end(), {p.X(), p.Y(), p.Z()});
    }
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    for (int i = 1; i <= t->NbTriangles(); ++i) {
      int a, b, c;
      t->Triangle(i).Get(a, b, c);
      if (reversed) std::swap(b, c);
      raw.tri.insert(raw.tri.end(), {base + uint32_t(a - 1), base + uint32_t(b - 1), base + uint32_t(c - 1)});
    }
  }
  if (raw.empty()) throw Error("the body has no triangles");
  return welded(raw, weldTol, nullptr);
}

Mesh mesh_solid_copy(const TopoDS_Shape& shape, double deflection) {
  TopoDS_Shape copy = BRepBuilderAPI_Copy(shape, Standard_True, Standard_False).Shape();
  BRepTools::Clean(copy);
  Bnd_Box box;
  BRepBndLib::Add(copy, box);
  if (box.IsVoid()) throw Error("the body is empty");
  const double diag = std::sqrt(box.SquareExtent());
  if (deflection <= 0) deflection = std::max(diag / 2000, 1e-4);
  BRepMesh_IncrementalMesh(copy, deflection, Standard_False, 0.2, Standard_True);
  // Faces share their edges' nodes up to rounding: welded at a billionth of the size. A sphere's or cone's pole is one
  // vertex then, and the triangles that had two of its copies are slivers with a repeated corner: dropped.
  Mesh m = from_shape(copy, diag * 1e-9);
  std::vector<uint32_t> kept;
  kept.reserve(m.tri.size());
  for (size_t t = 0; t < m.triangles(); ++t) {
    const uint32_t a = m.tri[t * 3], b = m.tri[t * 3 + 1], c = m.tri[t * 3 + 2];
    if (a != b && b != c && a != c) kept.insert(kept.end(), {a, b, c});
  }
  m.tri = std::move(kept);
  return m;
}

TopoDS_Shape to_shape(const Mesh& m) {
  const size_t nt = m.triangles(), nv = m.vertices();
  if (nt == 0) throw Error("the mesh has no triangles");
  // Per-vertex normals split at creases (30 degrees), as an imported mesh is: a node per group of corners facing alike, so
  // flat sides stay flat when shaded (the stored text keeps the nodes, the normals are worked out again from them).
  std::vector<double> fn(nt * 3);
  for (size_t t = 0; t < nt; ++t) {
    const double* a = &m.xyz[m.tri[t * 3] * 3];
    const double* b = &m.xyz[m.tri[t * 3 + 1] * 3];
    const double* c = &m.xyz[m.tri[t * 3 + 2] * 3];
    const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    fn[t * 3] = u[1] * w[2] - u[2] * w[1];
    fn[t * 3 + 1] = u[2] * w[0] - u[0] * w[2];
    fn[t * 3 + 2] = u[0] * w[1] - u[1] * w[0];
  }
  std::vector<uint32_t> start(nv + 1, 0), corners(nt * 3);
  for (size_t i = 0; i < nt * 3; ++i) ++start[m.tri[i] + 1];
  for (size_t v = 0; v < nv; ++v) start[v + 1] += start[v];
  {
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t i = 0; i < nt * 3; ++i) corners[fill[m.tri[i]]++] = static_cast<uint32_t>(i);
  }
  const double crease = std::cos(30.0 * M_PI / 180.0);
  std::vector<uint32_t> cornerNode(nt * 3);
  std::vector<double> nodes, normals;
  nodes.reserve(nv * 3);
  normals.reserve(nv * 3);
  struct Group {
    double rep[3], sum[3];
  };
  std::vector<Group> groups;
  for (size_t v = 0; v < nv; ++v) {
    if (start[v] == start[v + 1]) continue;  // unused
    groups.clear();
    const size_t base = nodes.size() / 3;
    for (uint32_t k = start[v]; k < start[v + 1]; ++k) {
      const uint32_t c = corners[k], t = c / 3;
      const double* n = &fn[t * 3];
      const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      size_t g = groups.size();
      if (len <= 0) g = groups.empty() ? 0 : 0;
      else
        for (size_t i = 0; i < groups.size(); ++i)
          if ((n[0] * groups[i].rep[0] + n[1] * groups[i].rep[1] + n[2] * groups[i].rep[2]) / len >= crease) {
            g = i;
            break;
          }
      if (g >= groups.size()) {
        Group fresh{{0, 0, 1}, {0, 0, 0}};
        if (len > 0)
          for (int i = 0; i < 3; ++i) fresh.rep[i] = n[i] / len;
        groups.push_back(fresh);
        g = groups.size() - 1;
      }
      for (int i = 0; i < 3; ++i) groups[g].sum[i] += n[i];
      cornerNode[c] = static_cast<uint32_t>(base + g);
    }
    for (const Group& g : groups) {
      nodes.insert(nodes.end(), {m.xyz[v * 3], m.xyz[v * 3 + 1], m.xyz[v * 3 + 2]});
      double len = std::sqrt(g.sum[0] * g.sum[0] + g.sum[1] * g.sum[1] + g.sum[2] * g.sum[2]);
      const double* d = len > 0 ? g.sum : g.rep;
      if (len <= 0) len = 1;
      normals.insert(normals.end(), {d[0] / len, d[1] / len, d[2] / len});
    }
  }
  const int count = static_cast<int>(nodes.size() / 3);
  Handle(Poly_Triangulation) tri = new Poly_Triangulation(count, static_cast<int>(nt), Standard_False, Standard_True);
  for (int i = 0; i < count; ++i) {
    tri->SetNode(i + 1, gp_Pnt(nodes[size_t(i) * 3], nodes[size_t(i) * 3 + 1], nodes[size_t(i) * 3 + 2]));
    tri->SetNormal(i + 1, gp_Vec3f(float(normals[size_t(i) * 3]), float(normals[size_t(i) * 3 + 1]), float(normals[size_t(i) * 3 + 2])));
  }
  for (size_t t = 0; t < nt; ++t)
    tri->SetTriangle(int(t) + 1, Poly_Triangle(int(cornerNode[t * 3]) + 1, int(cornerNode[t * 3 + 1]) + 1, int(cornerNode[t * 3 + 2]) + 1));
  TopoDS_Face face;
  BRep_Builder().MakeFace(face, tri);
  return face;
}

std::string to_brep(const TopoDS_Shape& shape) {
  std::ostringstream ss;
  ss.precision(17);
  BRepTools::Write(shape, ss, Standard_True, Standard_False, TopTools_FormatVersion_VERSION_1);
  std::string brep;
  const std::string text = ss.str();
  brep.reserve(text.size() + 1);
  for (char c : text)
    if (c != '\r') brep.push_back(c);
  if (brep.empty() || brep.back() != '\n') brep.push_back('\n');
  return brep;
}

// ---------------------------------------------------------------- maps and measures
void transform(Mesh& m, const gp_GTrsf& t) {
  for (size_t v = 0; v < m.vertices(); ++v) {
    gp_XYZ p(m.xyz[v * 3], m.xyz[v * 3 + 1], m.xyz[v * 3 + 2]);
    t.Transforms(p);
    m.xyz[v * 3] = p.X(), m.xyz[v * 3 + 1] = p.Y(), m.xyz[v * 3 + 2] = p.Z();
  }
  // The map's own handedness (taken from where it sends the unit vectors: VectorialPart leaves a gp_Trsf's scale out).
  gp_XYZ o(0, 0, 0), x(1, 0, 0), y(0, 1, 0), z(0, 0, 1);
  t.Transforms(o), t.Transforms(x), t.Transforms(y), t.Transforms(z);
  if ((x - o).Crossed(y - o).Dot(z - o) < 0)
    for (size_t i = 0; i < m.triangles(); ++i) std::swap(m.tri[i * 3 + 1], m.tri[i * 3 + 2]);
}

TopoDS_Shape transformed(const TopoDS_Shape& shape, const gp_GTrsf& t) {
  Mesh m = from_shape(shape);
  transform(m, t);
  return to_shape(m);
}

Bnd_Box bounds(const Mesh& m) {
  Bnd_Box box;
  for (size_t v = 0; v < m.vertices(); ++v) box.Update(m.xyz[v * 3], m.xyz[v * 3 + 1], m.xyz[v * 3 + 2]);
  return box;
}

Bnd_Box tight_box(const TopoDS_Shape& shape) {
  Bnd_Box box;
  if (shape.IsNull()) return box;
  if (is_mesh_shape(shape)) {
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(e.Current()), loc);
      if (t.IsNull()) continue;
      const gp_Trsf where = loc.Transformation();
      for (int i = 1; i <= t->NbNodes(); ++i) box.Add(t->Node(i).Transformed(where));
    }
    return box;
  }
  BRepBndLib::AddOptimal(shape, box, Standard_False, Standard_False);
  return box;
}

double volume(const Mesh& m) {
  double v6 = 0;
  for (size_t t = 0; t < m.triangles(); ++t) {
    const double* a = &m.xyz[m.tri[t * 3] * 3];
    const double* b = &m.xyz[m.tri[t * 3 + 1] * 3];
    const double* c = &m.xyz[m.tri[t * 3 + 2] * 3];
    v6 += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
  }
  return v6 / 6;
}

std::array<double, 3> centroid(const Mesh& m) {
  double v6 = 0, cx = 0, cy = 0, cz = 0;
  for (size_t t = 0; t < m.triangles(); ++t) {
    const double* a = &m.xyz[m.tri[t * 3] * 3];
    const double* b = &m.xyz[m.tri[t * 3 + 1] * 3];
    const double* c = &m.xyz[m.tri[t * 3 + 2] * 3];
    const double d = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    v6 += d;
    cx += d * (a[0] + b[0] + c[0]);
    cy += d * (a[1] + b[1] + c[1]);
    cz += d * (a[2] + b[2] + c[2]);
  }
  const Bnd_Box box = bounds(m);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  const double size = std::max({x1 - x0, y1 - y0, z1 - z0, 1e-12});
  // Closed (or nearly): the volume's centre; a sheet encloses nothing, so the box's middle.
  if (std::fabs(v6) > 1e-9 * size * size * size) return {cx / (4 * v6), cy / (4 * v6), cz / (4 * v6)};
  return {(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2};
}

std::vector<Mesh> components(const Mesh& m) {
  Sets sets(m.vertices());
  for (size_t t = 0; t < m.triangles(); ++t) {
    sets.join(m.tri[t * 3], m.tri[t * 3 + 1]);
    sets.join(m.tri[t * 3], m.tri[t * 3 + 2]);
  }
  // Triangles by part (in the order parts are first met), then each part's vertices through one shared map.
  std::unordered_map<uint32_t, uint32_t> which;
  std::vector<uint32_t> part(m.triangles());
  std::vector<size_t> sizes;
  for (size_t t = 0; t < m.triangles(); ++t) {
    auto [it, added] = which.emplace(sets.find(m.tri[t * 3]), static_cast<uint32_t>(sizes.size()));
    if (added) sizes.push_back(0);
    part[t] = it->second;
    ++sizes[it->second];
  }
  std::vector<std::vector<uint32_t>> tris(sizes.size());
  for (size_t i = 0; i < sizes.size(); ++i) tris[i].reserve(sizes[i]);
  for (size_t t = 0; t < m.triangles(); ++t) tris[part[t]].push_back(static_cast<uint32_t>(t));
  std::vector<int64_t> map(m.vertices(), -1);
  std::vector<Mesh> out(sizes.size());
  for (size_t i = 0; i < tris.size(); ++i) {
    Mesh& p = out[i];
    p.tri.reserve(tris[i].size() * 3);
    for (uint32_t t : tris[i])
      for (int k = 0; k < 3; ++k) {
        const uint32_t v = m.tri[t * 3 + k];
        if (map[v] < 0) {
          map[v] = static_cast<int64_t>(p.vertices());
          p.xyz.insert(p.xyz.end(), {m.xyz[v * 3], m.xyz[v * 3 + 1], m.xyz[v * 3 + 2]});
        }
        p.tri.push_back(static_cast<uint32_t>(map[v]));
      }
    for (uint32_t t : tris[i])
      for (int k = 0; k < 3; ++k) map[m.tri[t * 3 + k]] = -1;
  }
  std::stable_sort(out.begin(), out.end(), [](const Mesh& a, const Mesh& b) { return a.triangles() > b.triangles(); });
  return out;
}

}  // namespace opad::meshops
