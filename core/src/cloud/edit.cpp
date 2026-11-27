// Editing a point cloud by hand (opad/cloud_edit.hpp): the compact Edit a cloud_edit feature stores, applying it, and the
// Editor a session works through (brushes, deleting, adding points on the local surface).
#include "opad/cloud_edit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opad::cloud {
namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encode(const uint8_t* d, size_t n) {
  std::string out;
  out.reserve((n + 2) / 3 * 4);
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? uint32_t(d[i + 2]) : 0);
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
    out += i + 2 < n ? kAlphabet[v & 63] : '=';
  }
  return out;
}

std::vector<uint8_t> decode(const std::string& s) {
  static const auto table = [] {
    std::array<int8_t, 256> t{};
    t.fill(-1);
    for (int i = 0; i < 64; ++i) t[uint8_t(kAlphabet[i])] = int8_t(i);
    return t;
  }();
  std::vector<uint8_t> out;
  out.reserve(s.size() / 4 * 3);
  uint32_t v = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    const int x = table[uint8_t(c)];
    if (x < 0) throw Error("a point cloud edit holds text that is not base64");
    v = v << 6 | uint32_t(x);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(uint8_t(v >> bits));
    }
  }
  return out;
}

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The eigenvector of the smallest eigenvalue of a symmetric 3x3 matrix (Jacobi rotations).
Vec3 smallest_eigenvector(double a[3][3]) {
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 16; ++sweep) {
    if (a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2] < 1e-30) break;
    for (int p = 0; p < 2; ++p)
      for (int q = p + 1; q < 3; ++q) {
        if (std::abs(a[p][q]) < 1e-300) continue;
        const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
        const double t = (theta >= 0 ? 1 : -1) / (std::abs(theta) + std::sqrt(theta * theta + 1));
        const double c = 1 / std::sqrt(t * t + 1), s = t * c;
        for (int k = 0; k < 3; ++k) {
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq, a[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; ++k) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk, a[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 3; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq, v[k][q] = s * vkp + c * vkq;
        }
      }
  }
  int m = 0;
  for (int k = 1; k < 3; ++k)
    if (a[k][k] < a[m][m]) m = k;
  Vec3 n{v[0][m], v[1][m], v[2][m]};
  const double l = std::sqrt(dot(n, n));
  if (l > 0) n = {n[0] / l, n[1] / l, n[2] / l};
  return n;
}

// The plane through points: their centroid and the normal of least spread.
bool fit_plane(const std::vector<Vec3>& pts, Vec3& centre, Vec3& normal) {
  if (pts.size() < 3) return false;
  centre = {0, 0, 0};
  for (const auto& p : pts)
    for (int d = 0; d < 3; ++d) centre[size_t(d)] += p[size_t(d)];
  for (auto& c : centre) c /= double(pts.size());
  double m[3][3] = {};
  for (const auto& p : pts) {
    const Vec3 d = sub(p, centre);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) m[r][c] += d[size_t(r)] * d[size_t(c)];
  }
  normal = smallest_eigenvector(m);
  return dot(normal, normal) > 0.5;
}

// A point through a view (row-major, cloud frame -> clip) inside a polygon of normalised device coordinates.
bool inside(const std::array<double, 16>& m, const std::vector<std::array<double, 2>>& poly, const Vec3& q) {
  const double w = m[12] * q[0] + m[13] * q[1] + m[14] * q[2] + m[15];
  if (w <= 1e-12) return false;
  const double x = (m[0] * q[0] + m[1] * q[1] + m[2] * q[2] + m[3]) / w, y = (m[4] * q[0] + m[5] * q[1] + m[6] * q[2] + m[7]) / w;
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const auto &a = poly[i], &b = poly[j];
    if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
  }
  return in;
}

// Added points carry what the base carries, so the edited cloud keeps every attribute for every point.
void conform(Points& added, const Points& base) {
  const size_t n = added.size();
  if (base.colored()) added.rgb.resize(3 * n, 128);
  else added.rgb.clear();
  if (base.has_normals()) {
    if (added.normals.size() != 3 * n) {
      added.normals.resize(3 * n, 0.f);
      for (size_t i = 0; i < n; ++i)
        if (added.normals[3 * i] == 0 && added.normals[3 * i + 1] == 0 && added.normals[3 * i + 2] == 0) added.normals[3 * i + 2] = 1;
    }
  } else {
    added.normals.clear();
  }
  if (base.has_scalar()) added.scalar.resize(n, 0.f);
  else added.scalar.clear();
  added.scalar_name = base.scalar_name;
  added.offset = base.offset;
}

}  // namespace

std::string floats_to_base64(const float* v, size_t n) {
  std::vector<uint8_t> b(4 * n);
  for (size_t i = 0; i < n; ++i) {
    uint32_t u;
    std::memcpy(&u, &v[i], 4);
    for (int k = 0; k < 4; ++k) b[4 * i + size_t(k)] = uint8_t(u >> (8 * k));
  }
  return encode(b.data(), b.size());
}

std::vector<float> floats_from_base64(const std::string& s) {
  const std::vector<uint8_t> b = decode(s);
  if (b.size() % 4) throw Error("a point cloud edit's numbers are cut short");
  std::vector<float> out(b.size() / 4);
  for (size_t i = 0; i < out.size(); ++i) {
    uint32_t u = 0;
    for (int k = 0; k < 4; ++k) u |= uint32_t(b[4 * i + size_t(k)]) << (8 * k);
    std::memcpy(&out[i], &u, 4);
  }
  return out;
}

std::string bytes_to_base64(const uint8_t* v, size_t n) { return encode(v, n); }
std::vector<uint8_t> bytes_from_base64(const std::string& s) { return decode(s); }

// ---------------------------------------------------------------- the edit
json Edit::to_json() const {
  json j = {{"base", base}, {"strokes", strokes}};
  j["moved"] = Region::of(moved).to_json();  // sorted and unique: the ranges list the points in the offsets' order
  j["offsets"] = floats_to_base64(offsets.data(), offsets.size());
  j["deleted"] = deleted.to_json();
  json a = {{"count", added.size()}, {"xyz", floats_to_base64(added.xyz.data(), added.xyz.size())}};
  if (added.colored()) a["rgb"] = bytes_to_base64(added.rgb.data(), added.rgb.size());
  if (added.has_normals()) a["normals"] = floats_to_base64(added.normals.data(), added.normals.size());
  if (added.has_scalar()) a["scalar"] = floats_to_base64(added.scalar.data(), added.scalar.size());
  j["added"] = a;
  return j;
}

Edit Edit::from_json(const json& j) {
  if (!j.is_object()) throw Error("a point cloud edit is an object {base, moved, offsets, deleted, added}");
  Edit e;
  if (j.empty()) return e;  // no change
  const json base = j.value("base", json());
  if (!base.is_number_unsigned() && !base.is_number_integer()) throw Error("a point cloud edit names the number of points it was made on (base)");
  e.base = base.get<size_t>();
  e.strokes = j.value("strokes", size_t(0));
  e.moved = Region::from_json(j.value("moved", json::array())).indices();
  e.offsets = floats_from_base64(j.value("offsets", std::string()));
  if (e.offsets.size() != 3 * e.moved.size()) throw Error("a point cloud edit has " + std::to_string(e.offsets.size()) + " offsets for " + std::to_string(e.moved.size()) + " moved points");
  if (!e.moved.empty() && e.moved.back() >= e.base) throw Error("a point cloud edit moves a point the cloud does not have");
  if (const json a = j.value("added", json()); a.is_object()) {
    e.added.xyz = floats_from_base64(a.value("xyz", std::string()));
    if (e.added.xyz.size() % 3) throw Error("a point cloud edit's added points are cut short");
    const size_t n = e.added.size();
    if (a.contains("rgb")) e.added.rgb = bytes_from_base64(a["rgb"].get<std::string>());
    if (a.contains("normals")) e.added.normals = floats_from_base64(a["normals"].get<std::string>());
    if (a.contains("scalar")) e.added.scalar = floats_from_base64(a["scalar"].get<std::string>());
    if ((!e.added.rgb.empty() && e.added.rgb.size() != 3 * n) || (!e.added.normals.empty() && e.added.normals.size() != 3 * n) ||
        (!e.added.scalar.empty() && e.added.scalar.size() != n))
      throw Error("a point cloud edit's added points have attributes for another number of points");
  }
  e.deleted = Region::from_json(j.value("deleted", json::array()));
  if (!e.deleted.empty() && e.deleted.ranges.back()[1] >= e.base + e.added.size()) throw Error("a point cloud edit deletes a point the cloud does not have");
  return e;
}

json Edit::summary() const {
  return {{"moved", moved.size()}, {"deleted", deleted.count()}, {"added", added.size()}, {"strokes", strokes}};
}

double falloff(double t, Falloff kind) {
  if (!(t >= 0) || t >= 1) return 0;
  if (kind == Falloff::Sharp) return (1 - t) * (1 - t) * (1 - t);
  if (kind == Falloff::Flat) {
    if (t <= 0.6) return 1;
    t = (t - 0.6) / 0.4;
  }
  const double s = 1 - t * t;
  return s * s;
}

Points apply(const Points& base, const Edit& e) {
  const size_t n = base.size();
  if (e.base != n)
    throw Error("the cloud has " + std::to_string(n) + " points, its edit was made on " + std::to_string(e.base) + ": the steps before it changed the points");
  Points added = e.added;
  conform(added, base);
  Points out;
  out.offset = base.offset;
  out.scalar_name = base.scalar_name;
  const size_t total = n + added.size(), kept = total - std::min(total, e.deleted.count());
  out.xyz.reserve(3 * kept);
  if (base.colored()) out.rgb.reserve(3 * kept);
  if (base.has_normals()) out.normals.reserve(3 * kept);
  if (base.has_scalar()) out.scalar.reserve(kept);
  size_t m = 0, d = 0;  // the next moved point, the next deleted range
  auto take = [&](const Points& from, size_t i, const float* shift) {
    for (int k = 0; k < 3; ++k) out.xyz.push_back(from.xyz[3 * i + size_t(k)] + (shift ? shift[k] : 0.f));
    if (from.colored()) out.rgb.insert(out.rgb.end(), from.rgb.begin() + std::ptrdiff_t(3 * i), from.rgb.begin() + std::ptrdiff_t(3 * i + 3));
    if (from.has_normals()) out.normals.insert(out.normals.end(), from.normals.begin() + std::ptrdiff_t(3 * i), from.normals.begin() + std::ptrdiff_t(3 * i + 3));
    if (from.has_scalar()) out.scalar.push_back(from.scalar[i]);
  };
  for (size_t i = 0; i < total; ++i) {
    while (d < e.deleted.ranges.size() && e.deleted.ranges[d][1] < i) ++d;
    const bool gone = d < e.deleted.ranges.size() && e.deleted.ranges[d][0] <= i;
    const float* shift = nullptr;
    if (i < n && m < e.moved.size() && e.moved[m] == i) shift = &e.offsets[3 * m++];
    if (gone) continue;
    if (i < n) take(base, i, shift);
    else take(added, i - n, nullptr);
  }
  return out;
}

// ---------------------------------------------------------------- the editor
Editor::Editor(std::shared_ptr<const Points> base, const Cancel& cancel) : m_base(std::move(base)) {
  if (!m_base) throw Error("no cloud to edit");
  if (cancel && cancel()) throw Error("cancelled");
  m_grid = std::make_unique<Grid>(*m_base);
  if (cancel && cancel()) throw Error("cancelled");
  m_spacing = cloud::spacing(*m_base);
  reset();
}

Editor::~Editor() = default;

void Editor::reset(const Edit& e) {
  const size_t n = m_base->size();
  if (!e.empty() && e.base != n)
    throw Error("the cloud has " + std::to_string(n) + " points, its edit was made on " + std::to_string(e.base) + ": the steps before it changed the points");
  m_offset.clear();
  m_reach = 0;
  for (size_t k = 0; k < e.moved.size(); ++k) {
    const std::array<float, 3> o{e.offsets[3 * k], e.offsets[3 * k + 1], e.offsets[3 * k + 2]};
    m_offset[e.moved[k]] = o;
    m_reach = std::max(m_reach, std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]));
  }
  m_added = e.added;
  conform(m_added, *m_base);
  m_deleted.assign(n + m_added.size(), 0);
  for (const auto& r : e.deleted.ranges)
    for (uint64_t i = r[0]; i <= r[1] && i < m_deleted.size(); ++i) m_deleted[size_t(i)] = 1;
  m_strokes = e.strokes;
}

Edit Editor::edit() const {
  Edit e;
  e.base = m_base->size();
  e.strokes = m_strokes;
  e.moved.reserve(m_offset.size());
  for (const auto& [i, o] : m_offset)
    if (o[0] != 0 || o[1] != 0 || o[2] != 0) e.moved.push_back(i);
  std::sort(e.moved.begin(), e.moved.end());
  e.offsets.reserve(3 * e.moved.size());
  for (uint32_t i : e.moved) {
    const auto& o = m_offset.at(i);
    e.offsets.insert(e.offsets.end(), o.begin(), o.end());
  }
  std::vector<std::array<uint32_t, 2>> ranges;
  for (size_t i = 0; i < m_deleted.size(); ++i) {
    if (!m_deleted[i]) continue;
    if (!ranges.empty() && ranges.back()[1] + 1 == i) ranges.back()[1] = uint32_t(i);
    else ranges.push_back({uint32_t(i), uint32_t(i)});
  }
  e.deleted.ranges = std::move(ranges);
  e.added = m_added;
  return e;
}

Vec3 Editor::at(uint32_t i) const {
  const size_t n = m_base->size();
  if (i >= n) {
    const size_t k = i - n;
    return {m_added.xyz[3 * k], m_added.xyz[3 * k + 1], m_added.xyz[3 * k + 2]};
  }
  Vec3 p = m_base->point(i);
  if (!m_offset.empty())
    if (const auto o = m_offset.find(i); o != m_offset.end()) p = {p[0] + o->second[0], p[1] + o->second[1], p[2] + o->second[2]};
  return p;
}

void Editor::offsetBy(uint32_t i, float dx, float dy, float dz) {
  const size_t n = m_base->size();
  if (i >= n) {
    float* p = &m_added.xyz[3 * (i - n)];
    p[0] += dx, p[1] += dy, p[2] += dz;
    return;
  }
  auto& o = m_offset[i];
  o[0] += dx, o[1] += dy, o[2] += dz;
  m_reach = std::max(m_reach, std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]));
}

void Editor::candidates(const Vec3& c, double radius, std::vector<uint32_t>& out) const {
  out.clear();
  std::vector<uint32_t> near;
  const float q[3] = {float(c[0]), float(c[1]), float(c[2])};
  m_grid->within(q, radius + m_reach, near);  // the old places: a moved point is found as far as it went
  const double r2 = radius * radius;
  for (uint32_t i : near) {
    if (deleted(i)) continue;
    const Vec3 d = sub(at(i), c);
    if (dot(d, d) <= r2) out.push_back(i);
  }
  const size_t n = m_base->size();
  for (size_t k = 0; k < m_added.size(); ++k) {
    const uint32_t i = uint32_t(n + k);
    if (deleted(i)) continue;
    const Vec3 d = sub(at(i), c);
    if (dot(d, d) <= r2) out.push_back(i);
  }
  std::sort(out.begin(), out.end());
}

void Editor::under(const Brush& b, std::vector<uint32_t>& ids, std::vector<float>& weights) const {
  ids.clear();
  weights.clear();
  if (!(b.radius > 0)) return;
  // A brush reaches the surface under it, not the far side of a part: half its radius along the view (at least a few
  // point spacings, as deep as a scan's noise).
  const double depth = b.front ? std::max(0.5 * b.radius, 3 * m_spacing) : 4 * b.radius;
  std::vector<uint32_t> near;
  candidates(b.centre, std::hypot(b.radius, depth), near);
  for (uint32_t i : near) {
    const Vec3 d = sub(at(i), b.centre);
    const double along = dot(d, b.toward);
    if (std::abs(along) > depth) continue;
    const double lateral = std::sqrt(std::max(0.0, dot(d, d) - along * along));
    const double w = falloff(lateral / b.radius, b.falloff);
    if (w <= 0) continue;
    ids.push_back(i);
    weights.push_back(float(w));
  }
}

std::vector<uint32_t> Editor::push(const Brush& b, double amount) {
  std::vector<uint32_t> ids;
  std::vector<float> w;
  under(b, ids, w);
  const size_t n = m_base->size();
  const bool normals = m_base->has_normals();
  for (size_t k = 0; k < ids.size(); ++k) {
    const uint32_t i = ids[k];
    Vec3 dir = b.toward;
    if (normals) {
      const float* nn = i < n ? &m_base->normals[3 * size_t(i)] : &m_added.normals[3 * (size_t(i) - n)];
      Vec3 v{nn[0], nn[1], nn[2]};
      const double l = std::sqrt(dot(v, v));
      if (l > 1e-6) {
        v = {v[0] / l, v[1] / l, v[2] / l};
        dir = dot(v, b.toward) < 0 ? Vec3{-v[0], -v[1], -v[2]} : v;
      }
    }
    const double s = amount * w[k];
    offsetBy(i, float(dir[0] * s), float(dir[1] * s), float(dir[2] * s));
  }
  return ids;
}

void Editor::nearest(const Vec3& q, size_t k, std::vector<uint32_t>& out) const {
  out.clear();
  if (k == 0) return;
  const float f[3] = {float(q[0]), float(q[1]), float(q[2])};
  std::vector<std::pair<double, uint32_t>> best;
  std::vector<uint32_t> found;
  // Asked on the old places, with room for points moved or deleted since; asked again wider while too few are live.
  for (size_t ask = k + 8; ; ask *= 4) {
    m_grid->nearest(f, std::min(ask, m_base->size()), found);
    best.clear();
    for (uint32_t i : found)
      if (!deleted(i)) {
        const Vec3 d = sub(at(i), q);
        best.push_back({dot(d, d), i});
      }
    if (best.size() >= k || ask >= m_base->size() || ask > 4096) break;
  }
  const size_t n = m_base->size();
  for (size_t a = 0; a < m_added.size(); ++a)
    if (!deleted(uint32_t(n + a))) {
      const Vec3 d = sub(at(uint32_t(n + a)), q);
      best.push_back({dot(d, d), uint32_t(n + a)});
    }
  std::sort(best.begin(), best.end());
  for (size_t i = 0; i < best.size() && out.size() < k; ++i) out.push_back(best[i].second);
}

std::vector<uint32_t> Editor::smooth(const Brush& b, double strength, int k) {
  std::vector<uint32_t> ids;
  std::vector<float> w;
  under(b, ids, w);
  // Every target from the places as they were before this dab, then all moved at once.
  std::vector<std::array<float, 3>> step(ids.size(), {0, 0, 0});
  std::vector<uint32_t> nb;
  std::vector<Vec3> pts;
  for (size_t j = 0; j < ids.size(); ++j) {
    const Vec3 p = at(ids[j]);
    nearest(p, size_t(std::max(3, k)) + 1, nb);
    pts.clear();
    for (uint32_t i : nb)
      if (i != ids[j]) pts.push_back(at(i));
    Vec3 c, nrm;
    if (!fit_plane(pts, c, nrm)) continue;
    const double off = dot(sub(p, c), nrm);
    const double s = -off * std::clamp(strength, 0.0, 1.0) * w[j];
    step[j] = {float(nrm[0] * s), float(nrm[1] * s), float(nrm[2] * s)};
  }
  for (size_t j = 0; j < ids.size(); ++j) offsetBy(ids[j], step[j][0], step[j][1], step[j][2]);
  return ids;
}

std::vector<uint32_t> Editor::erase(const Brush& b) {
  std::vector<uint32_t> ids;
  std::vector<float> w;
  under(b, ids, w);
  for (uint32_t i : ids) m_deleted[i] = 1;
  return ids;
}

size_t Editor::erase(const Region& r) {
  size_t count = 0;
  for (const auto& range : r.ranges)
    for (uint64_t i = range[0]; i <= range[1] && i < m_deleted.size(); ++i)
      if (!m_deleted[size_t(i)]) m_deleted[size_t(i)] = 1, ++count;
  return count;
}

size_t Editor::keep(const Region& r) {
  size_t count = 0, j = 0;
  for (size_t i = 0; i < m_deleted.size(); ++i) {
    while (j < r.ranges.size() && r.ranges[j][1] < i) ++j;
    const bool in = j < r.ranges.size() && r.ranges[j][0] <= i;
    if (!in && !m_deleted[i]) m_deleted[i] = 1, ++count;
  }
  return count;
}

std::optional<uint32_t> Editor::add(const Vec3& seed, const Vec3& toward, int k) {
  std::vector<uint32_t> nb;
  nearest(seed, size_t(std::max(3, k)), nb);
  std::vector<Vec3> pts;
  for (uint32_t i : nb) pts.push_back(at(i));
  Vec3 c, n;
  if (!fit_plane(pts, c, n)) return std::nullopt;
  // Where the pointer's line meets the plane; along the plane's normal when the line runs nearly in it.
  Vec3 p;
  const double along = dot(toward, n), off = dot(sub(seed, c), n);
  if (std::abs(along) > 0.25) p = {seed[0] - toward[0] * off / along, seed[1] - toward[1] * off / along, seed[2] - toward[2] * off / along};
  else p = {seed[0] - n[0] * off, seed[1] - n[1] * off, seed[2] - n[2] * off};
  // Its attributes from the points nearest to it now, the nearer the more.
  nearest(p, size_t(std::max(3, k)), nb);
  if (nb.empty()) return std::nullopt;
  const size_t base = m_base->size();
  double wsum = 0, rgb[3] = {0, 0, 0}, scalar = 0;
  Vec3 mean{0, 0, 0};
  for (uint32_t i : nb) {
    const Vec3 d = sub(at(i), p);
    const double w = 1 / (std::sqrt(dot(d, d)) + 1e-3 * std::max(m_spacing, 1e-3));
    wsum += w;
    const Points& from = i < base ? *m_base : m_added;
    const size_t j = i < base ? i : i - base;
    if (from.colored())
      for (int a = 0; a < 3; ++a) rgb[a] += w * from.rgb[3 * j + size_t(a)];
    if (from.has_normals())
      for (int a = 0; a < 3; ++a) mean[size_t(a)] += w * from.normals[3 * j + size_t(a)];
    if (from.has_scalar()) scalar += w * from.scalar[j];
  }
  if (m_base->has_normals() ? dot(n, mean) < 0 : dot(n, toward) < 0) n = {-n[0], -n[1], -n[2]};
  for (int a = 0; a < 3; ++a) m_added.xyz.push_back(float(p[size_t(a)]));
  if (m_base->colored())
    for (int a = 0; a < 3; ++a) m_added.rgb.push_back(uint8_t(std::clamp(std::lround(rgb[a] / wsum), 0L, 255L)));
  if (m_base->has_normals())
    for (int a = 0; a < 3; ++a) m_added.normals.push_back(float(n[size_t(a)]));
  if (m_base->has_scalar()) m_added.scalar.push_back(float(scalar / wsum));
  m_deleted.push_back(0);
  return uint32_t(base + m_added.size() - 1);
}

Vec3 Editor::normalOf(uint32_t i, std::vector<uint32_t>& scratch) const {
  const size_t n = m_base->size();
  const Points& from = i < n ? *m_base : m_added;
  const size_t j = i < n ? i : i - n;
  Vec3 v{0, 0, 0};
  if (from.has_normals()) {
    v = {from.normals[3 * j], from.normals[3 * j + 1], from.normals[3 * j + 2]};
  } else {
    nearest(at(i), 10, scratch);
    std::vector<Vec3> pts;
    for (uint32_t k : scratch) pts.push_back(at(k));
    Vec3 c;
    if (!fit_plane(pts, c, v)) return {0, 0, 0};
  }
  const double l = std::sqrt(dot(v, v));
  return l > 1e-9 ? Vec3{v[0] / l, v[1] / l, v[2] / l} : Vec3{0, 0, 0};
}

Region Editor::grow(uint32_t seed, double angle, double offset, size_t limit, bool* stopped, const Cancel& cancel) {
  if (stopped) *stopped = false;
  if (seed >= size() || deleted(seed)) return {};
  const double cosMax = std::cos(std::clamp(angle, 0.0, 180.0) * 3.14159265358979323846 / 180);
  const size_t k = 12;
  std::vector<uint8_t> seen(size(), 0);
  std::vector<uint32_t> taken{seed}, queue{seed}, near, scratch;
  std::unordered_map<uint32_t, Vec3> normals;  // fitted ones, each once
  auto normal = [&](uint32_t i) {
    if (const auto it = normals.find(i); it != normals.end()) return it->second;
    return normals[i] = normalOf(i, scratch);
  };
  seen[seed] = 1;
  for (size_t head = 0; head < queue.size(); ++head) {
    if ((head & 4095) == 0 && cancel && cancel()) throw Error("cancelled");
    const uint32_t p = queue[head];
    const Vec3 at_p = at(p), n_p = normal(p);
    nearest(at_p, k, near);
    for (uint32_t q : near) {
      if (seen[q]) continue;
      const Vec3 n_q = normal(q);
      // An unoriented normal (fitted, or a scan's) may point either way: the angle between the lines.
      if (dot(n_p, n_p) > 0 && dot(n_q, n_q) > 0 && std::abs(dot(n_p, n_q)) < cosMax) continue;
      if (dot(n_p, n_p) > 0 && std::abs(dot(sub(at(q), at_p), n_p)) > offset) continue;
      seen[q] = 1;
      taken.push_back(q);
      queue.push_back(q);
      if (taken.size() >= limit) {
        if (stopped) *stopped = true;
        return Region::of(std::move(taken));
      }
    }
  }
  return Region::of(std::move(taken));
}

Region Editor::inPolygon(const std::array<double, 16>& view, const std::vector<std::array<double, 2>>& polygon, const Cancel& cancel) const {
  if (polygon.size() < 3) return {};
  Region r = in_polygon(*m_base, view, polygon, cancel);  // the old places, in parallel; moved and added points below
  std::vector<uint32_t> moved, movedIn, extra, gone;
  moved.reserve(m_offset.size());
  for (const auto& [i, o] : m_offset) {
    moved.push_back(i);
    if (inside(view, polygon, at(i))) movedIn.push_back(i);
  }
  const size_t n = m_base->size();
  for (size_t k = 0; k < m_added.size(); ++k)
    if (inside(view, polygon, at(uint32_t(n + k)))) extra.push_back(uint32_t(n + k));
  for (size_t i = 0; i < m_deleted.size(); ++i)
    if (m_deleted[i]) gone.push_back(uint32_t(i));
  return r.subtract(Region::of(std::move(moved))).unite(Region::of(std::move(movedIn))).unite(Region::of(std::move(extra))).subtract(Region::of(std::move(gone)));
}

}  // namespace opad::cloud
