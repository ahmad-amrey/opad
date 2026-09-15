#include "opad/mesh.hpp"

#include <BRepLib_ToolTriangulatedShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Trsf.hxx>

#include <cmath>
#include <cstring>
#include <sstream>

#include "opad/cache.hpp"
#include "opad/geometry.hpp"

namespace opad {

Mesh tessellate(const TopoDS_Shape& s, double linear_tol, double angular_deg) {
  Mesh mesh;
  if (s.IsNull()) return mesh;
  BRepMesh_IncrementalMesh mesher(s, linear_tol, Standard_False, angular_deg * M_PI / 180.0, Standard_True);
  (void)mesher;

  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  for (int fi = 1; fi <= faces.Extent(); ++fi) {
    const TopoDS_Face& face = TopoDS::Face(faces(fi));
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull() || tri->NbTriangles() == 0) continue;
    if (!tri->HasNormals()) BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
    const gp_Trsf trsf = loc.Transformation();
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    const uint32_t base = static_cast<uint32_t>(mesh.positions.size() / 3);
    Mesh::FaceRange range{fi - 1, static_cast<uint32_t>(mesh.indices.size()), 0};
    for (int i = 1; i <= tri->NbNodes(); ++i) {
      gp_Pnt p = tri->Node(i).Transformed(trsf);
      gp_Dir n = tri->Normal(i);
      if (trsf.Form() != gp_Identity) n.Transform(trsf);
      if (reversed) n.Reverse();
      mesh.positions.insert(mesh.positions.end(), {static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z())});
      mesh.normals.insert(mesh.normals.end(), {static_cast<float>(n.X()), static_cast<float>(n.Y()), static_cast<float>(n.Z())});
    }
    for (int i = 1; i <= tri->NbTriangles(); ++i) {
      int a, b, c;
      tri->Triangle(i).Get(a, b, c);
      if (reversed) std::swap(b, c);
      mesh.indices.push_back(base + static_cast<uint32_t>(a - 1));
      mesh.indices.push_back(base + static_cast<uint32_t>(b - 1));
      mesh.indices.push_back(base + static_cast<uint32_t>(c - 1));
    }
    range.count = static_cast<uint32_t>(mesh.indices.size()) - range.first;
    mesh.faces.push_back(range);
  }
  return mesh;
}

// Binary layout: magic, counts, then raw arrays. Little-endian only (cache is per machine).
std::string Mesh::serialize() const {
  std::string out;
  auto put = [&](const void* p, size_t n) { out.append(static_cast<const char*>(p), n); };
  put("OPADMESH1", 9);
  uint32_t np = static_cast<uint32_t>(positions.size()), ni = static_cast<uint32_t>(indices.size()),
           nf = static_cast<uint32_t>(faces.size());
  put(&np, 4);
  put(&ni, 4);
  put(&nf, 4);
  put(positions.data(), positions.size() * sizeof(float));
  put(normals.data(), normals.size() * sizeof(float));
  put(indices.data(), indices.size() * sizeof(uint32_t));
  for (const auto& f : faces) {
    put(&f.face, 4);
    put(&f.first, 4);
    put(&f.count, 4);
  }
  return out;
}

Mesh Mesh::deserialize(const std::string& blob) {
  Mesh m;
  if (blob.size() < 21 || blob.compare(0, 9, "OPADMESH1") != 0) throw Error("bad mesh blob");
  const char* p = blob.data() + 9;
  const char* end = blob.data() + blob.size();
  auto get = [&](void* dst, size_t n) {
    if (p + n > end) throw Error("truncated mesh blob");
    std::memcpy(dst, p, n);
    p += n;
  };
  uint32_t np, ni, nf;
  get(&np, 4);
  get(&ni, 4);
  get(&nf, 4);
  m.positions.resize(np);
  m.normals.resize(np);
  m.indices.resize(ni);
  get(m.positions.data(), np * sizeof(float));
  get(m.normals.data(), np * sizeof(float));
  get(m.indices.data(), ni * sizeof(uint32_t));
  m.faces.resize(nf);
  for (auto& f : m.faces) {
    get(&f.face, 4);
    get(&f.first, 4);
    get(&f.count, 4);
  }
  return m;
}

Mesh tessellate_body(const Document& doc, const std::string& key, double linear_tol) {
  char tol[32];
  std::snprintf(tol, sizeof tol, "%.6g", linear_tol);
  std::string cache_key = key + "-" + tol;
  if (auto blob = cache_get("mesh", cache_key)) {
    try {
      return Mesh::deserialize(*blob);
    } catch (const std::exception&) {
      // fall through and rebuild
    }
  }
  Mesh m = tessellate(body_shape(doc, key), linear_tol);
  cache_put("mesh", cache_key, m.serialize());
  return m;
}

}  // namespace opad
