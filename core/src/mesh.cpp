#include "opad/mesh.hpp"

#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Version.hxx>
#if OCC_VERSION_HEX >= 0x070800
#include <BRepLib_ToolTriangulatedShape.hxx>
#else
#include <Geom_Surface.hxx>
#include <GeomLib.hxx>
#include <Poly.hxx>
#include <Poly_Connect.hxx>
#include <Precision.hxx>
#include <TopLoc_Location.hxx>
#endif
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include "opad/cache.hpp"
#include "opad/geometry.hpp"

namespace opad {

namespace {

// Per-node normals for a face triangulation. OCCT 7.8 ships this as BRepLib_ToolTriangulatedShape::ComputeNormals;
// 7.6/7.7 only have it in the visualisation toolkit (StdPrs), which core must not link, so this mirrors that
// algorithm: the surface normal at each node's UV, falling back to the average of the adjacent triangle normals
// where the surface normal is degenerate. Orientation is not applied here; the caller reverses for REVERSED faces.
void compute_normals(const TopoDS_Face& face, const Handle(Poly_Triangulation)& tri) {
#if OCC_VERSION_HEX >= 0x070800
  BRepLib_ToolTriangulatedShape::ComputeNormals(face, tri);
#else
  if (tri.IsNull() || tri->HasNormals()) return;
  const TopoDS_Face zero_face = TopoDS::Face(face.Located(TopLoc_Location()));
  Handle(Geom_Surface) surf = BRep_Tool::Surface(zero_face);
  if (!tri->HasUVNodes() || surf.IsNull()) {
    Poly::ComputeNormals(tri);
    return;
  }
  const double tol = Precision::Confusion();
  Poly_Connect connect;
  tri->AddNormals();
  for (int ni = 1; ni <= tri->NbNodes(); ++ni) {
    gp_Dir n;
    if (GeomLib::NormEstim(surf, tri->UVNode(ni), tol, n) > 1) {
      if (connect.Triangulation() != tri) connect.Load(tri);
      gp_XYZ sum(0.0, 0.0, 0.0);
      for (connect.Initialize(ni); connect.More(); connect.Next()) {
        int a, b, c;
        tri->Triangle(connect.Value()).Get(a, b, c);
        const gp_XYZ v1 = tri->Node(b).Coord() - tri->Node(a).Coord();
        const gp_XYZ v2 = tri->Node(c).Coord() - tri->Node(b).Coord();
        const gp_XYZ cross = v1 ^ v2;
        const double len = cross.Modulus();
        if (len >= tol) sum += cross / len;
      }
      n = sum.Modulus() > tol ? gp_Dir(sum) : gp::DZ();
    }
    tri->SetNormal(ni, n);
  }
#endif
}

}  // namespace

Mesh tessellate(const TopoDS_Shape& s, double linear_tol, double angular_deg) {
  Mesh mesh;
  if (s.IsNull()) return mesh;
  mesh_shape(s, linear_tol, angular_deg);

  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  for (int fi = 1; fi <= faces.Extent(); ++fi) {
    const TopoDS_Face& face = TopoDS::Face(faces(fi));
    TopLoc_Location loc;
    Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (tri.IsNull() || tri->NbTriangles() == 0) continue;
    if (!tri->HasNormals()) compute_normals(face, tri);
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
  std::string cache_key = key + "-recovery1-" + tol;
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

bool FaceColors::empty() const { return std::none_of(face.begin(), face.end(), [](int c) { return c >= 0; }); }

json FaceColors::to_json() const {
  if (empty()) return nullptr;
  size_t last = face.size();
  while (last > 0 && face[last - 1] < 0) --last;  // faces past the runs are the body's colour
  json runs = json::array(), list = json::array();
  for (size_t i = 0; i < last;) {
    size_t j = i;
    while (j < last && face[j] == face[i]) ++j;
    runs.push_back(j - i);
    runs.push_back(face[i]);
    i = j;
  }
  for (const auto& c : colors) list.push_back({std::round(c[0] * 1e6) / 1e6, std::round(c[1] * 1e6) / 1e6, std::round(c[2] * 1e6) / 1e6});
  return {{"colors", list}, {"runs", runs}};
}

FaceColors FaceColors::from_json(const json& j) {
  FaceColors out;
  try {
    if (!j.is_object()) return {};
    for (const auto& c : j.at("colors")) out.colors.push_back({c.at(0).get<double>(), c.at(1).get<double>(), c.at(2).get<double>()});
    const json& runs = j.at("runs");
    for (size_t i = 0; i + 1 < runs.size(); i += 2) {
      const int64_t count = runs[i].get<int64_t>();
      const int64_t color = runs[i + 1].get<int64_t>();
      if (count < 0 || color >= static_cast<int64_t>(out.colors.size()) || out.face.size() + static_cast<size_t>(count) > (size_t(1) << 26)) return {};
      out.face.insert(out.face.end(), static_cast<size_t>(count), color < 0 ? -1 : static_cast<int>(color));
    }
  } catch (const std::exception&) {
    return {};
  }
  return out;
}

FaceColors face_colors(const Document& doc, const std::string& key) {
  const BodyEntry* body = doc.body(key);
  return body && body->meta.contains("face_colors") ? FaceColors::from_json(body->meta["face_colors"]) : FaceColors{};
}

}  // namespace opad
