// Point clouds as bodies (opad/point_cloud.hpp): the nodes-only triangulation they are stored as, regions of points and the
// reports the commands give.
#include <BRep_Builder.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <OSD_Parallel.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <sstream>

#include "opad/point_cloud.hpp"

namespace opad::cloud {

void Points::normalise() {
  const size_t n = size();
  xyz.resize(n * 3);
  if (rgb.size() != n * 3) rgb.clear();
  if (normals.size() != n * 3) normals.clear();
  if (scalar.size() != n) scalar.clear(), scalar_name.clear();
}

void Points::transform(const Mat4& m) {
  if (m.is_identity()) return;
  const size_t n = size();
  OSD_Parallel::For(0, int((n + 65535) / 65536), [&](int block) {
    const size_t a = size_t(block) * 65536, b = std::min(n, a + 65536);
    for (size_t i = a; i < b; ++i) {
      const Vec3 p = m.apply({xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]});
      for (int k = 0; k < 3; ++k) xyz[3 * i + size_t(k)] = float(p[size_t(k)]);
      if (!normals.empty()) {
        Vec3 d = m.apply_dir({normals[3 * i], normals[3 * i + 1], normals[3 * i + 2]});
        const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; ++k) normals[3 * i + size_t(k)] = float(l > 0 ? d[size_t(k)] / l : 0);
      }
    }
  });
}

// ---------------------------------------------------------------- the body
TopoDS_Shape to_shape(const Points& p) {
  const int n = int(p.size());
  if (n <= 0) throw Error("the point cloud has no points");
  Handle(Poly_Triangulation) t = new Poly_Triangulation();
  t->SetDoublePrecision(false);
  const bool uv = p.colored() || p.has_scalar();
  if (uv) t->InternalUVNodes().SetDoublePrecision(false);
  t->ResizeNodes(n, false);
  if (uv) t->AddUVNodes();
  if (p.has_normals()) t->AddNormals();
  OSD_Parallel::For(0, (n + 65535) / 65536, [&](int block) {
    const int a = block * 65536, b = std::min(n, a + 65536);
    for (int i = a; i < b; ++i) {
      const size_t k = size_t(i) * 3;
      t->SetNode(i + 1, gp_Pnt(p.xyz[k], p.xyz[k + 1], p.xyz[k + 2]));
      if (uv) {
        const double c = p.colored() ? double(uint32_t(p.rgb[k]) << 16 | uint32_t(p.rgb[k + 1]) << 8 | p.rgb[k + 2]) : -1.0;
        t->SetUVNode(i + 1, gp_Pnt2d(c, p.has_scalar() ? double(p.scalar[size_t(i)]) : 0.0));
      }
      if (p.has_normals()) t->SetNormal(i + 1, gp_Vec3f(p.normals[k], p.normals[k + 1], p.normals[k + 2]));
    }
  });
  BRep_Builder b;
  TopoDS_Face face;
  b.MakeFace(face, t);
  return face;
}

namespace {
template <class F>
void each_triangulation(const TopoDS_Shape& shape, F&& f) {
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Face& face = TopoDS::Face(e.Current());
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(face, loc);
    if (!t.IsNull()) f(face, t, loc);
  }
}
}  // namespace

bool is_cloud(const TopoDS_Shape& shape) {
  if (shape.IsNull()) return false;
  bool any = false, cloud = true;
  each_triangulation(shape, [&](const TopoDS_Face& face, const Handle(Poly_Triangulation)& t, const TopLoc_Location&) {
    if (!BRep_Tool::Surface(face).IsNull() || t->NbTriangles() > 0 || t->NbNodes() == 0) cloud = false;
    any = true;
  });
  return any && cloud;
}

size_t point_count(const TopoDS_Shape& shape) {
  size_t n = 0;
  each_triangulation(shape, [&](const TopoDS_Face&, const Handle(Poly_Triangulation)& t, const TopLoc_Location&) {
    if (t->NbTriangles() == 0) n += size_t(t->NbNodes());
  });
  return n;
}

Points from_shape(const TopoDS_Shape& shape, bool attributes) {
  Points out;
  std::vector<std::pair<Handle(Poly_Triangulation), TopLoc_Location>> parts;
  size_t n = 0;
  bool colour = false, normals = attributes, scalar = false;
  each_triangulation(shape, [&](const TopoDS_Face&, const Handle(Poly_Triangulation)& t, const TopLoc_Location& loc) {
    if (t->NbTriangles() > 0) return;
    parts.push_back({t, loc});
    n += size_t(t->NbNodes());
    normals = normals && t->HasNormals();
  });
  if (attributes)
    for (const auto& [t, loc] : parts)
      if (t->HasUVNodes()) {
        // A part's colour is unknown when its first point says so (-1): every point of a cloud has a colour or none does.
        if (t->UVNode(1).X() >= 0) colour = true;
        scalar = true;
      }
  out.xyz.resize(n * 3);
  if (colour) out.rgb.assign(n * 3, 200);
  if (normals && n) out.normals.resize(n * 3);
  if (scalar) out.scalar.assign(n, 0.f);
  size_t first = 0;
  for (const auto& [t, loc] : parts) {
    const int m = t->NbNodes();
    const bool moved = !loc.IsIdentity();
    const gp_Trsf trsf = loc.Transformation();
    const bool uv = attributes && t->HasUVNodes();
    OSD_Parallel::For(0, (m + 65535) / 65536, [&](int block) {
      const int a = block * 65536, b = std::min(m, a + 65536);
      for (int i = a; i < b; ++i) {
        const size_t k = (first + size_t(i)) * 3;
        gp_Pnt q = t->Node(i + 1);
        if (moved) q.Transform(trsf);
        out.xyz[k] = float(q.X()), out.xyz[k + 1] = float(q.Y()), out.xyz[k + 2] = float(q.Z());
        if (uv) {
          const gp_Pnt2d c = t->UVNode(i + 1);
          if (colour && c.X() >= 0) {
            const uint32_t v = uint32_t(c.X());
            out.rgb[k] = uint8_t(v >> 16), out.rgb[k + 1] = uint8_t(v >> 8), out.rgb[k + 2] = uint8_t(v);
          }
          if (scalar) out.scalar[first + size_t(i)] = float(c.Y());
        }
        if (!out.normals.empty()) {
          gp_Vec3f v;
          t->Normal(i + 1, v);
          if (moved) {
            gp_Dir d(v.x(), v.y(), v.z());
            if (v.SquareModulus() > 0) d.Transform(trsf);
            v = gp_Vec3f(float(d.X()), float(d.Y()), float(d.Z()));
          }
          out.normals[k] = v.x(), out.normals[k + 1] = v.y(), out.normals[k + 2] = v.z();
        }
      }
    });
    first += size_t(m);
  }
  return out;
}

json meta_of(const Points& p) {
  json attributes = json::array();
  if (p.colored()) attributes.push_back("color");
  if (p.has_normals()) attributes.push_back("normals");
  if (p.has_scalar()) attributes.push_back(p.scalar_name.empty() ? "scalar" : p.scalar_name);
  json m = {{"representation", "points"}, {"points", p.size()}, {"attributes", attributes}};
  if (p.has_scalar()) m["scalar"] = p.scalar_name.empty() ? "scalar" : p.scalar_name;
  return m;
}

std::array<Vec3, 2> bounds(const Points& p) {
  const size_t n = p.size();
  if (n == 0) return {Vec3{0, 0, 0}, Vec3{0, 0, 0}};
  const int blocks = int((n + 262143) / 262144);
  std::vector<std::array<float, 6>> part(static_cast<size_t>(blocks));
  OSD_Parallel::For(0, blocks, [&](int block) {
    std::array<float, 6> b = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    const size_t a = size_t(block) * 262144, e = std::min(n, a + 262144);
    for (size_t i = a; i < e; ++i)
      for (size_t k = 0; k < 3; ++k) {
        const float v = p.xyz[3 * i + k];
        b[k] = std::min(b[k], v), b[k + 3] = std::max(b[k + 3], v);
      }
    part[size_t(block)] = b;
  });
  std::array<Vec3, 2> r = {Vec3{1e30, 1e30, 1e30}, Vec3{-1e30, -1e30, -1e30}};
  for (const auto& b : part)
    for (size_t k = 0; k < 3; ++k) r[0][k] = std::min(r[0][k], double(b[k])), r[1][k] = std::max(r[1][k], double(b[k + 3]));
  return r;
}

// ---------------------------------------------------------------- regions
size_t Region::count() const {
  size_t n = 0;
  for (const auto& r : ranges) n += size_t(r[1]) - r[0] + 1;
  return n;
}

bool Region::contains(uint32_t i) const {
  auto it = std::upper_bound(ranges.begin(), ranges.end(), i, [](uint32_t v, const std::array<uint32_t, 2>& r) { return v < r[0]; });
  return it != ranges.begin() && i <= (it - 1)->at(1);
}

std::vector<uint32_t> Region::indices() const {
  std::vector<uint32_t> out;
  out.reserve(count());
  for (const auto& r : ranges)
    for (uint64_t i = r[0]; i <= r[1]; ++i) out.push_back(uint32_t(i));
  return out;
}

Region Region::of(std::vector<uint32_t> idx) {
  std::sort(idx.begin(), idx.end());
  Region r;
  for (uint32_t i : idx) {
    if (!r.ranges.empty() && uint64_t(r.ranges.back()[1]) + 1 >= i) r.ranges.back()[1] = std::max(r.ranges.back()[1], i);
    else r.ranges.push_back({i, i});
  }
  return r;
}

namespace {
Region normalised(std::vector<std::array<uint32_t, 2>> v) {
  std::sort(v.begin(), v.end());
  Region r;
  for (const auto& a : v) {
    if (!r.ranges.empty() && uint64_t(r.ranges.back()[1]) + 1 >= a[0]) r.ranges.back()[1] = std::max(r.ranges.back()[1], a[1]);
    else r.ranges.push_back(a);
  }
  return r;
}
}  // namespace

Region Region::unite(const Region& o) const {
  auto all = ranges;
  all.insert(all.end(), o.ranges.begin(), o.ranges.end());
  return normalised(std::move(all));
}

Region Region::subtract(const Region& o) const {
  Region r;
  size_t j = 0;
  for (auto a : ranges) {
    uint64_t lo = a[0];
    const uint64_t hi = a[1];
    while (j < o.ranges.size() && o.ranges[j][1] < lo) ++j;
    size_t k = j;
    while (lo <= hi) {
      if (k >= o.ranges.size() || o.ranges[k][0] > hi) {
        r.ranges.push_back({uint32_t(lo), uint32_t(hi)});
        break;
      }
      if (o.ranges[k][0] > lo) r.ranges.push_back({uint32_t(lo), o.ranges[k][0] - 1});
      lo = uint64_t(o.ranges[k][1]) + 1;
      ++k;
    }
  }
  return r;
}

json Region::to_json() const {
  json a = json::array();
  for (const auto& r : ranges) a.push_back({r[0], r[1]});
  return a;
}

Region Region::from_json(const json& j) {
  if (!j.is_array()) throw Error("a region's vertices are a list of [first, last] ranges");
  std::vector<std::array<uint32_t, 2>> v;
  v.reserve(j.size());
  auto ordinal = [](const json& x) {
    if (!x.is_number_integer() && !x.is_number_unsigned()) throw Error("a region names points by whole numbers");
    const int64_t i = x.get<int64_t>();
    if (i < 0 || i > int64_t(0xFFFFFFFEu)) throw Error("a region names a point out of range");
    return uint32_t(i);
  };
  for (const auto& r : j) {
    if (r.is_array()) {
      if (r.size() != 2) throw Error("a region's range is [first, last]");
      const uint32_t a = ordinal(r[0]), b = ordinal(r[1]);
      if (b < a) throw Error("a region's range ends before it starts");
      v.push_back({a, b});
    } else {
      const uint32_t a = ordinal(r);
      v.push_back({a, a});
    }
  }
  return normalised(std::move(v));
}

json region_ref(const std::string& body, const Region& region) {
  return {{"body", body}, {"kind", "region"}, {"vertices", region.to_json()}, {"count", region.count()}};
}

Region region_of_ref(const json& ref, size_t points) {
  if (!ref.is_object() || !ref.contains("vertices")) throw Error("a region reference is {\"body\", \"kind\": \"region\", \"vertices\": [[first, last], ...]}");
  Region r = Region::from_json(ref["vertices"]);
  if (points && !r.empty() && r.ranges.back()[1] >= points)
    throw Error("the region names points the cloud does not have (it has " + std::to_string(points) + ")");
  return r;
}

namespace {
// The points a test keeps, in parallel blocks whose ranges are joined in order.
template <class Keep>
Region select(const Points& p, Keep&& keep, const Cancel& cancel = {}) {
  const size_t n = p.size();
  const int blocks = int((n + 262143) / 262144);
  std::vector<std::vector<std::array<uint32_t, 2>>> part(static_cast<size_t>(blocks));
  std::atomic<bool> stop{false};
  OSD_Parallel::For(0, blocks, [&](int block) {
    if (stop || (cancel && cancel())) { stop = true; return; }
    auto& out = part[size_t(block)];
    const size_t a = size_t(block) * 262144, e = std::min(n, a + 262144);
    for (size_t i = a; i < e; ++i) {
      if (!keep(&p.xyz[3 * i])) continue;
      if (!out.empty() && out.back()[1] + 1 == i) out.back()[1] = uint32_t(i);
      else out.push_back({uint32_t(i), uint32_t(i)});
    }
  });
  if (stop) throw Error("cancelled");
  std::vector<std::array<uint32_t, 2>> all;
  for (auto& v : part) all.insert(all.end(), v.begin(), v.end());
  return normalised(std::move(all));
}
}  // namespace

Region in_box(const Points& p, const Vec3& lo, const Vec3& hi) {
  return select(p, [&](const float* q) {
    return q[0] >= lo[0] && q[0] <= hi[0] && q[1] >= lo[1] && q[1] <= hi[1] && q[2] >= lo[2] && q[2] <= hi[2];
  });
}

Region in_sphere(const Points& p, const Vec3& c, double radius) {
  const double r2 = radius * radius;
  return select(p, [&](const float* q) {
    const double dx = q[0] - c[0], dy = q[1] - c[1], dz = q[2] - c[2];
    return dx * dx + dy * dy + dz * dz <= r2;
  });
}

Region in_band(const Points& p, const Vec3& o, const Vec3& normal, double half_band) {
  const double l = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
  if (l <= 0) throw Error("the plane's normal is zero");
  const Vec3 n = {normal[0] / l, normal[1] / l, normal[2] / l};
  return select(p, [&](const float* q) { return std::abs((q[0] - o[0]) * n[0] + (q[1] - o[1]) * n[1] + (q[2] - o[2]) * n[2]) <= half_band; });
}

Region in_polygon(const Points& p, const std::array<double, 16>& m, const std::vector<std::array<double, 2>>& poly, const Cancel& cancel) {
  if (poly.size() < 3) return {};
  double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
  for (const auto& v : poly) x0 = std::min(x0, v[0]), y0 = std::min(y0, v[1]), x1 = std::max(x1, v[0]), y1 = std::max(y1, v[1]);
  return select(
      p,
      [&](const float* q) {
        const double w = m[12] * q[0] + m[13] * q[1] + m[14] * q[2] + m[15];
        if (w <= 1e-12) return false;
        const double x = (m[0] * q[0] + m[1] * q[1] + m[2] * q[2] + m[3]) / w, y = (m[4] * q[0] + m[5] * q[1] + m[6] * q[2] + m[7]) / w;
        if (x < x0 || x > x1 || y < y0 || y > y1) return false;
        bool inside = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
          const auto &a = poly[i], &b = poly[j];
          if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
        }
        return inside;
      },
      cancel);
}

Points subset(const Points& p, const Region& keep) {
  Points out;
  out.offset = p.offset;
  out.scalar_name = p.scalar_name;
  const size_t n = keep.count();
  out.xyz.reserve(n * 3);
  if (p.colored()) out.rgb.reserve(n * 3);
  if (p.has_normals()) out.normals.reserve(n * 3);
  if (p.has_scalar()) out.scalar.reserve(n);
  for (const auto& r : keep.ranges) {
    if (r[1] >= p.size()) throw Error("the region names points the cloud does not have");
    const size_t a = r[0], b = size_t(r[1]) + 1;
    out.xyz.insert(out.xyz.end(), p.xyz.begin() + std::ptrdiff_t(3 * a), p.xyz.begin() + std::ptrdiff_t(3 * b));
    if (p.colored()) out.rgb.insert(out.rgb.end(), p.rgb.begin() + std::ptrdiff_t(3 * a), p.rgb.begin() + std::ptrdiff_t(3 * b));
    if (p.has_normals()) out.normals.insert(out.normals.end(), p.normals.begin() + std::ptrdiff_t(3 * a), p.normals.begin() + std::ptrdiff_t(3 * b));
    if (p.has_scalar()) out.scalar.insert(out.scalar.end(), p.scalar.begin() + std::ptrdiff_t(a), p.scalar.begin() + std::ptrdiff_t(b));
  }
  if (!p.has_scalar()) out.scalar_name.clear();
  return out;
}

Points without(const Points& p, const Region& drop) {
  Region all;
  if (p.size()) all.ranges.push_back({0, uint32_t(p.size() - 1)});
  return subset(p, all.subtract(drop));
}

// ---------------------------------------------------------------- reports
json describe(const Points& p, bool measure) {
  const auto b = bounds(p);
  json attributes = json::array();
  if (p.colored()) attributes.push_back("color");
  if (p.has_normals()) attributes.push_back("normals");
  if (p.has_scalar()) attributes.push_back(p.scalar_name.empty() ? "scalar" : p.scalar_name);
  json j = {{"points", p.size()},
            {"bounds", {{"min", b[0]}, {"max", b[1]}}},
            {"size", {b[1][0] - b[0][0], b[1][1] - b[0][1], b[1][2] - b[0][2]}},
            {"attributes", attributes}};
  if (measure && p.size() > 1) j["spacing"] = spacing(p);
  return j;
}

json points_json(const Points& p, const Region* region, size_t offset, size_t limit) {
  json pts = json::array(), colors = json::array(), normals = json::array(), indices = json::array();
  const size_t total = region ? region->count() : p.size();
  auto round = [](double v) { return std::round(v * 1e4) / 1e4; };
  size_t seen = 0, taken = 0;
  auto take = [&](size_t i) {
    if (seen++ < offset || taken >= limit) return;
    ++taken;
    indices.push_back(i);
    pts.push_back({round(p.xyz[3 * i]), round(p.xyz[3 * i + 1]), round(p.xyz[3 * i + 2])});
    if (p.colored()) colors.push_back({p.rgb[3 * i], p.rgb[3 * i + 1], p.rgb[3 * i + 2]});
    if (p.has_normals()) normals.push_back({round(p.normals[3 * i]), round(p.normals[3 * i + 1]), round(p.normals[3 * i + 2])});
  };
  if (region) {
    for (const auto& r : region->ranges) {
      if (seen + (size_t(r[1]) - r[0] + 1) <= offset) { seen += size_t(r[1]) - r[0] + 1; continue; }
      for (uint64_t i = r[0]; i <= r[1] && taken < limit; ++i) take(size_t(i));
      if (taken >= limit) break;
    }
  } else {
    seen = std::min(offset, p.size());
    for (size_t i = seen; i < p.size() && taken < limit; ++i) take(i);
  }
  json j = {{"count", total}, {"offset", offset}, {"returned", taken}, {"indices", indices}, {"points", pts}};
  if (p.colored()) j["colors"] = colors;
  if (p.has_normals()) j["normals"] = normals;
  if (offset + taken < total) j["next_offset"] = offset + taken;
  return j;
}

}  // namespace opad::cloud

namespace opad::cloud {
std::string to_brep(const TopoDS_Shape& shape) {
  // Format 3 keeps the normals; nine digits give every float back exactly (and the colours, whole numbers below 2^24).
  std::ostringstream ss;
  ss.precision(9);
  BRepTools::Write(shape, ss, Standard_True, Standard_True, TopTools_FormatVersion_VERSION_3);
  std::string brep;
  const std::string text = ss.str();
  brep.reserve(text.size() + 1);
  for (char c : text)
    if (c != '\r') brep.push_back(c);
  if (brep.empty() || brep.back() != '\n') brep.push_back('\n');
  return brep;
}
}  // namespace opad::cloud

namespace opad::cloud {
json meta_of_shape(const TopoDS_Shape& shape) {
  size_t n = 0;
  bool colour = false, normals = true, scalar = false, any = false;
  each_triangulation(shape, [&](const TopoDS_Face&, const Handle(Poly_Triangulation)& t, const TopLoc_Location&) {
    if (t->NbTriangles() > 0) return;
    any = true;
    n += size_t(t->NbNodes());
    normals = normals && t->HasNormals();
    if (t->HasUVNodes()) scalar = true, colour = colour || t->UVNode(1).X() >= 0;
  });
  json attributes = json::array();
  if (colour) attributes.push_back("color");
  if (any && normals) attributes.push_back("normals");
  if (scalar) attributes.push_back("scalar");
  return {{"representation", "points"}, {"points", n}, {"attributes", attributes}};
}
}  // namespace opad::cloud
