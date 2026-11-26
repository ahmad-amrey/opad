// Tools on point clouds (opad/point_cloud.hpp): the neighbour grid, slices and their profiles, outlier removal, voxel
// downsampling and normal estimation. Whole-cloud passes run in parallel blocks (OSD_Parallel).
#include <OSD_Parallel.hxx>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <numeric>
#include <queue>

#include "opad/point_cloud.hpp"

namespace opad::cloud {
namespace {

constexpr size_t kBlock = 65536;
template <class F>
void blocks(size_t n, const Cancel& cancel, F&& f) {
  std::atomic<bool> stop{false};
  OSD_Parallel::For(0, int((n + kBlock - 1) / kBlock), [&](int b) {
    if (stop) return;
    if (cancel && cancel()) { stop = true; return; }
    f(size_t(b) * kBlock, std::min(n, size_t(b + 1) * kBlock));
  });
  if (stop) throw Error("cancelled");
}

// The eigenvector of the smallest eigenvalue of a symmetric 3x3 matrix (Jacobi rotations), and the eigenvalues.
void smallest_eigen(double a[3][3], double out[3], double* curvature = nullptr) {
  double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int sweep = 0; sweep < 16; ++sweep) {
    const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    if (off < 1e-30) break;
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
  for (int k = 0; k < 3; ++k) out[k] = v[k][m];
  if (curvature) {
    const double sum = a[0][0] + a[1][1] + a[2][2];
    *curvature = sum > 0 ? a[m][m] / sum : 0;
  }
}

double dist2(const float* a, const float* b) {
  const double dx = double(a[0]) - b[0], dy = double(a[1]) - b[1], dz = double(a[2]) - b[2];
  return dx * dx + dy * dy + dz * dz;
}

}  // namespace

// ---------------------------------------------------------------- grid
Grid::Grid(const Points& p, double cell) : m_n(p.size()) {
  const auto b = bounds(p);
  const double ext[3] = {std::max(b[1][0] - b[0][0], 1e-6), std::max(b[1][1] - b[0][1], 1e-6), std::max(b[1][2] - b[0][2], 1e-6)};
  if (cell <= 0) {
    // About eight points a cell on a scanned surface: its area taken from the box's faces.
    const double area = std::max(ext[0] * ext[1] + ext[1] * ext[2] + ext[0] * ext[2], 1e-12);
    cell = std::sqrt(area / double(std::max<size_t>(m_n, 1)) * 8.0);
    cell = std::max(cell, std::max({ext[0], ext[1], ext[2]}) / 4096.0);
  }
  m_cell = cell;
  for (int k = 0; k < 3; ++k) {
    m_lo[k] = b[0][size_t(k)];
    m_dim[k] = std::max<int64_t>(1, int64_t(ext[k] / cell) + 1);
  }
  const double cells = double(m_dim[0]) * double(m_dim[1]) * double(m_dim[2]);
  m_dense = cells <= double(m_n) * 4 + 1024 && cells < 2e9;
  std::vector<int64_t> keys(m_n);
  blocks(m_n, {}, [&](size_t a, size_t e) {
    for (size_t i = a; i < e; ++i) {
      int64_t c[3];
      cellOf(&p.xyz[3 * i], c);
      keys[i] = key(c[0], c[1], c[2]);
    }
  });
  m_order.resize(m_n);
  if (m_dense) {
    m_start.assign(size_t(cells) + 1, 0);
    for (int64_t k : keys) ++m_start[size_t(k) + 1];
    for (size_t i = 1; i < m_start.size(); ++i) m_start[i] += m_start[i - 1];
    std::vector<uint32_t> fill(m_start.begin(), m_start.end() - 1);
    for (size_t i = 0; i < m_n; ++i) m_order[fill[size_t(keys[i])]++] = uint32_t(i);
  } else {
    std::iota(m_order.begin(), m_order.end(), 0u);
    std::sort(m_order.begin(), m_order.end(), [&](uint32_t a, uint32_t c) { return keys[a] < keys[c] || (keys[a] == keys[c] && a < c); });
    for (size_t i = 0; i < m_n; ++i) {
      const int64_t k = keys[m_order[i]];
      if (m_keys.empty() || m_keys.back() != k) m_keys.push_back(k), m_start.push_back(uint32_t(i));
    }
    m_start.push_back(uint32_t(m_n));
  }
  m_xyz.resize(m_n * 3);
  blocks(m_n, {}, [&](size_t a, size_t e) {
    for (size_t i = a; i < e; ++i)
      for (size_t k = 0; k < 3; ++k) m_xyz[3 * i + k] = p.xyz[3 * size_t(m_order[i]) + k];
  });
}

void Grid::cellOf(const float* q, int64_t c[3]) const {
  for (int k = 0; k < 3; ++k) c[k] = std::clamp<int64_t>(int64_t(std::floor((double(q[k]) - m_lo[k]) / m_cell)), 0, m_dim[k] - 1);
}

bool Grid::cellRange(int64_t x, int64_t y, int64_t z, uint32_t& a, uint32_t& b) const {
  if (x < 0 || y < 0 || z < 0 || x >= m_dim[0] || y >= m_dim[1] || z >= m_dim[2]) return false;
  const int64_t k = key(x, y, z);
  if (m_dense) {
    a = m_start[size_t(k)], b = m_start[size_t(k) + 1];
    return b > a;
  }
  const auto it = std::lower_bound(m_keys.begin(), m_keys.end(), k);
  if (it == m_keys.end() || *it != k) return false;
  const size_t i = size_t(it - m_keys.begin());
  a = m_start[i], b = m_start[i + 1];
  return true;
}

void Grid::nearest(const float* q, size_t k, std::vector<uint32_t>& out, std::vector<float>* d2out, int64_t self) const {
  out.clear();
  if (d2out) d2out->clear();
  if (m_n == 0 || k == 0) return;
  // The k best so distant as a max-heap on distance.
  std::vector<std::pair<float, uint32_t>> best;
  best.reserve(k + 1);
  int64_t c[3];
  cellOf(q, c);
  const int64_t maxRing = std::max({m_dim[0], m_dim[1], m_dim[2]});
  for (int64_t r = 0; r <= maxRing; ++r) {
    for (int64_t x = c[0] - r; x <= c[0] + r; ++x)
      for (int64_t y = c[1] - r; y <= c[1] + r; ++y)
        for (int64_t z = c[2] - r; z <= c[2] + r; ++z) {
          if (std::max({std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2])}) != r) continue;  // the shell only
          uint32_t a, b;
          if (!cellRange(x, y, z, a, b)) continue;
          for (uint32_t i = a; i < b; ++i) {
            if (int64_t(m_order[i]) == self) continue;
            const float d = float(dist2(q, &m_xyz[3 * size_t(i)]));
            if (best.size() < k) {
              best.push_back({d, m_order[i]});
              std::push_heap(best.begin(), best.end());
            } else if (d < best.front().first) {
              std::pop_heap(best.begin(), best.end());
              best.back() = {d, m_order[i]};
              std::push_heap(best.begin(), best.end());
            }
          }
        }
    // Everything nearer than the searched cube's inner wall is found.
    if (best.size() >= k && double(best.front().first) <= double(r) * m_cell * double(r) * m_cell) break;
    if (r > 24 && best.size() >= k) break;
  }
  std::sort_heap(best.begin(), best.end());
  for (const auto& [d, i] : best) {
    out.push_back(i);
    if (d2out) d2out->push_back(d);
  }
}

void Grid::within(const float* q, double radius, std::vector<uint32_t>& out) const {
  out.clear();
  const double r2 = radius * radius;
  int64_t lo[3], hi[3];
  const float a[3] = {float(q[0] - radius), float(q[1] - radius), float(q[2] - radius)};
  const float b[3] = {float(q[0] + radius), float(q[1] + radius), float(q[2] + radius)};
  cellOf(a, lo);
  cellOf(b, hi);
  for (int64_t x = lo[0]; x <= hi[0]; ++x)
    for (int64_t y = lo[1]; y <= hi[1]; ++y)
      for (int64_t z = lo[2]; z <= hi[2]; ++z) {
        uint32_t s, e;
        if (!cellRange(x, y, z, s, e)) continue;
        for (uint32_t i = s; i < e; ++i)
          if (dist2(q, &m_xyz[3 * size_t(i)]) <= r2) out.push_back(m_order[i]);
      }
}

double spacing(const Points& p) {
  const size_t n = p.size();
  if (n < 2) return 0;
  // A big cloud is measured on a uniform sample of it; scanned points lie on surfaces, so the spacing grows as the square
  // root of the thinning (a guess off for a cloud filling a volume, which scans do not).
  constexpr size_t kSample = 400000;
  Points sample;
  double scale = 1;
  const Points* q = &p;
  if (n > kSample) {
    sample.xyz.resize(kSample * 3);
    for (size_t k = 0; k < kSample; ++k) {
      const size_t i = size_t((uint64_t(k) * 2654435761ull + 12345) % n);
      for (size_t d = 0; d < 3; ++d) sample.xyz[3 * k + d] = p.xyz[3 * i + d];
    }
    scale = std::sqrt(double(kSample) / double(n));
    q = &sample;
  }
  const size_t m = q->size();
  const Grid grid(*q);
  const size_t samples = std::min<size_t>(m, 2000);
  std::vector<double> d(samples, 0);
  OSD_Parallel::For(0, int(samples), [&](int s) {
    const size_t i = size_t(s) * m / samples;
    std::vector<uint32_t> nb;
    std::vector<float> d2;
    grid.nearest(&q->xyz[3 * i], 1, nb, &d2, int64_t(i));
    d[size_t(s)] = d2.empty() ? 0 : std::sqrt(double(d2[0]));
  });
  std::nth_element(d.begin(), d.begin() + std::ptrdiff_t(samples / 2), d.end());
  return d[samples / 2] * scale;
}

// ---------------------------------------------------------------- slices
Slice slice(const Points& p, const Vec3& origin, const Vec3& normal, double half_band, const Vec3& xin) {
  if (half_band <= 0) throw Error("a slice needs a band wider than zero");
  const double l = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
  if (l <= 0) throw Error("the slice plane's normal is zero");
  const Vec3 n = {normal[0] / l, normal[1] / l, normal[2] / l};
  Vec3 x = xin;
  double xd = x[0] * n[0] + x[1] * n[1] + x[2] * n[2];
  x = {x[0] - xd * n[0], x[1] - xd * n[1], x[2] - xd * n[2]};
  double xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
  if (xl < 1e-9) {
    const Vec3 a = std::abs(n[2]) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
    // x = a x n would turn; take the in-plane direction of a world axis instead: for a horizontal plane, X.
    const Vec3 w = std::abs(n[2]) < 0.9 ? Vec3{-n[1], n[0], 0} : Vec3{1, 0, 0};
    (void)a;
    xd = w[0] * n[0] + w[1] * n[1] + w[2] * n[2];
    x = {w[0] - xd * n[0], w[1] - xd * n[1], w[2] - xd * n[2]};
    xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
  }
  x = {x[0] / xl, x[1] / xl, x[2] / xl};
  const Vec3 y = {n[1] * x[2] - n[2] * x[1], n[2] * x[0] - n[0] * x[2], n[0] * x[1] - n[1] * x[0]};
  Slice s;
  s.frame.origin = origin, s.frame.x = x, s.frame.y = y;
  // The points' local origin: the plane's origin less the cloud's offset.
  const Vec3 o = {origin[0] - p.offset[0], origin[1] - p.offset[1], origin[2] - p.offset[2]};
  const Region r = in_band(p, o, n, half_band);
  s.indices = r.indices();
  s.uv.resize(s.indices.size() * 2);
  OSD_Parallel::For(0, int((s.indices.size() + kBlock - 1) / kBlock), [&](int b) {
    for (size_t k = size_t(b) * kBlock; k < std::min(s.indices.size(), size_t(b + 1) * kBlock); ++k) {
      const size_t i = s.indices[k];
      const double d[3] = {p.xyz[3 * i] - o[0], p.xyz[3 * i + 1] - o[1], p.xyz[3 * i + 2] - o[2]};
      s.uv[2 * k] = float(d[0] * x[0] + d[1] * x[1] + d[2] * x[2]);
      s.uv[2 * k + 1] = float(d[0] * y[0] + d[1] * y[1] + d[2] * y[2]);
    }
  });
  return s;
}

namespace {
using P2 = std::array<double, 2>;
void douglas_peucker(const std::vector<P2>& in, double tol, std::vector<P2>& out) {
  if (in.size() < 3) { out = in; return; }
  std::vector<char> keep(in.size(), 0);
  keep.front() = keep.back() = 1;
  std::vector<std::pair<size_t, size_t>> stack{{0, in.size() - 1}};
  while (!stack.empty()) {
    auto [a, b] = stack.back();
    stack.pop_back();
    double worst = -1;
    size_t at = a;
    const double dx = in[b][0] - in[a][0], dy = in[b][1] - in[a][1];
    const double len = std::sqrt(dx * dx + dy * dy);
    for (size_t i = a + 1; i < b; ++i) {
      const double ex = in[i][0] - in[a][0], ey = in[i][1] - in[a][1];
      const double d = len > 1e-12 ? std::abs(ex * dy - ey * dx) / len : std::sqrt(ex * ex + ey * ey);
      if (d > worst) worst = d, at = i;
    }
    if (worst > tol) {
      keep[at] = 1;
      stack.push_back({a, at});
      stack.push_back({at, b});
    }
  }
  out.clear();
  for (size_t i = 0; i < in.size(); ++i)
    if (keep[i]) out.push_back(in[i]);
}
}  // namespace

std::vector<std::vector<std::array<double, 2>>> profile(const std::vector<float>& uv, double gap, double tolerance) {
  std::vector<std::vector<P2>> result;
  const size_t n = uv.size() / 2;
  if (n < 3) return result;
  // The 2D points as a flat cloud, for the neighbour grid.
  Points flat;
  flat.xyz.resize(n * 3);
  for (size_t i = 0; i < n; ++i) flat.xyz[3 * i] = uv[2 * i], flat.xyz[3 * i + 1] = uv[2 * i + 1], flat.xyz[3 * i + 2] = 0;
  const double sp = std::max(spacing(flat), 1e-6);
  if (gap <= 0) gap = 4 * sp;
  if (tolerance <= 0) tolerance = sp;
  const double step = std::max(2.5 * sp, std::min(gap, 2 * tolerance));
  const Grid grid(flat, step);
  std::vector<char> used(n, 0);
  std::vector<uint32_t> nearby;
  // From a centre, the next centre: the mean of the unused points within `step`, which are used up; with none there, the
  // nearest unused point within `gap`, and the points within `step` of it.
  auto advance = [&](P2& c) {
    for (int pass = 0; pass < 2; ++pass) {
      const float q[3] = {float(c[0]), float(c[1]), 0};
      grid.within(q, step, nearby);
      double sx = 0, sy = 0;
      size_t m = 0;
      for (uint32_t i : nearby)
        if (!used[i]) sx += uv[2 * i], sy += uv[2 * i + 1], ++m, used[i] = 1;
      if (m) {
        c = {sx / double(m), sy / double(m)};
        return true;
      }
      if (pass == 1) return false;
      grid.within(q, gap, nearby);
      uint32_t best = 0;
      double bd = 1e300;
      for (uint32_t i : nearby)
        if (!used[i]) {
          const double d = (uv[2 * i] - c[0]) * (uv[2 * i] - c[0]) + (uv[2 * i + 1] - c[1]) * (uv[2 * i + 1] - c[1]);
          if (d < bd) bd = d, best = i;
        }
      if (bd == 1e300) return false;
      c = {uv[2 * best], uv[2 * best + 1]};
    }
    return false;
  };
  // Chains start from the leftmost unused point (an end of an open profile, a place on a closed one).
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return uv[2 * a] < uv[2 * b] || (uv[2 * a] == uv[2 * b] && uv[2 * a + 1] < uv[2 * b + 1]); });
  for (uint32_t seed : order) {
    if (used[seed]) continue;
    P2 start = {uv[2 * seed], uv[2 * seed + 1]};
    std::vector<P2> forward{start};
    P2 c = start;
    if (!advance(c)) { used[seed] = 1; continue; }
    forward.back() = c;  // the first cluster's middle
    const P2 first = c;
    while (advance(c)) forward.push_back(c);
    // The other way from the start.
    std::vector<P2> backward;
    c = first;
    while (advance(c)) backward.push_back(c);
    std::vector<P2> chain(backward.rbegin(), backward.rend());
    chain.insert(chain.end(), forward.begin(), forward.end());
    if (chain.size() < 3) continue;
    const double cx = chain.front()[0] - chain.back()[0], cy = chain.front()[1] - chain.back()[1];
    const bool closed = chain.size() > 6 && std::sqrt(cx * cx + cy * cy) <= gap;
    if (closed) chain.push_back(chain.front());
    std::vector<P2> thin;
    douglas_peucker(chain, tolerance, thin);
    if (thin.size() >= 2) result.push_back(std::move(thin));
  }
  // Longest first.
  auto length = [](const std::vector<P2>& c) {
    double s = 0;
    for (size_t i = 1; i < c.size(); ++i) s += std::hypot(c[i][0] - c[i - 1][0], c[i][1] - c[i - 1][1]);
    return s;
  };
  std::stable_sort(result.begin(), result.end(), [&](const auto& a, const auto& b) { return length(a) > length(b); });
  return result;
}

// ---------------------------------------------------------------- cleaning
Region outliers(const Points& p, int k, double ratio, const Cancel& cancel) {
  const size_t n = p.size();
  if (n < size_t(k) + 2 || k < 1) return {};
  const Grid grid(p);
  std::vector<float> mean(n);
  blocks(n, cancel, [&](size_t a, size_t e) {
    std::vector<uint32_t> nb;
    std::vector<float> d2;
    for (size_t i = a; i < e; ++i) {
      grid.nearest(&p.xyz[3 * i], size_t(k), nb, &d2, int64_t(i));
      double s = 0;
      for (float d : d2) s += std::sqrt(double(d));
      mean[i] = float(d2.empty() ? 0 : s / double(d2.size()));
    }
  });
  double sum = 0, sum2 = 0;
  for (float m : mean) sum += m, sum2 += double(m) * m;
  const double mu = sum / double(n), sd = std::sqrt(std::max(0.0, sum2 / double(n) - mu * mu));
  const double limit = mu + ratio * sd;
  std::vector<uint32_t> out;
  for (size_t i = 0; i < n; ++i)
    if (mean[i] > limit) out.push_back(uint32_t(i));
  return Region::of(std::move(out));
}

Points voxel_downsample(const Points& p, double size, const Cancel& cancel) {
  if (size <= 0) throw Error("the voxel size must be above zero");
  const size_t n = p.size();
  const auto b = bounds(p);
  const double ext = std::max({b[1][0] - b[0][0], b[1][1] - b[0][1], b[1][2] - b[0][2]});
  if (ext / size > 2e6) throw Error("the voxel size is too small for this cloud");
  std::vector<std::pair<uint64_t, uint32_t>> keys(n);
  blocks(n, cancel, [&](size_t a, size_t e) {
    for (size_t i = a; i < e; ++i) {
      uint64_t c[3];
      for (size_t k = 0; k < 3; ++k) c[k] = uint64_t(std::floor((double(p.xyz[3 * i + k]) - b[0][k]) / size));
      keys[i] = {(c[0] << 42) | (c[1] << 21) | c[2], uint32_t(i)};
    }
  });
  std::sort(keys.begin(), keys.end());
  if (cancel && cancel()) throw Error("cancelled");
  // Voxel by voxel: the point nearest the voxel's centroid.
  std::vector<uint32_t> keep;
  for (size_t s = 0; s < n;) {
    size_t e = s;
    double c[3] = {0, 0, 0};
    while (e < n && keys[e].first == keys[s].first) {
      for (size_t k = 0; k < 3; ++k) c[k] += p.xyz[3 * size_t(keys[e].second) + k];
      ++e;
    }
    for (double& v : c) v /= double(e - s);
    uint32_t best = keys[s].second;
    double bd = 1e300;
    for (size_t j = s; j < e; ++j) {
      const float* q = &p.xyz[3 * size_t(keys[j].second)];
      const double d = (q[0] - c[0]) * (q[0] - c[0]) + (q[1] - c[1]) * (q[1] - c[1]) + (q[2] - c[2]) * (q[2] - c[2]);
      if (d < bd) bd = d, best = keys[j].second;
    }
    keep.push_back(best);
    s = e;
  }
  Points out = subset(p, Region::of(std::move(keep)));
  out.offset = p.offset;
  return out;
}

void estimate_normals(Points& p, int k, const Cancel& cancel) {
  const size_t n = p.size();
  if (n < 3) throw Error("a cloud needs at least three points for normals");
  k = std::clamp(k, 3, 64);
  const Grid grid(p);
  p.normals.assign(n * 3, 0.f);
  blocks(n, cancel, [&](size_t a, size_t e) {
    std::vector<uint32_t> nb;
    for (size_t i = a; i < e; ++i) {
      grid.nearest(&p.xyz[3 * i], size_t(k), nb);
      double c[3] = {0, 0, 0};
      for (uint32_t j : nb)
        for (size_t d = 0; d < 3; ++d) c[d] += p.xyz[3 * size_t(j) + d];
      for (double& v : c) v /= double(std::max<size_t>(nb.size(), 1));
      double m[3][3] = {};
      for (uint32_t j : nb) {
        const double d[3] = {p.xyz[3 * j] - c[0], p.xyz[3 * j + 1] - c[1], p.xyz[3 * j + 2] - c[2]};
        for (int r = 0; r < 3; ++r)
          for (int s = 0; s < 3; ++s) m[r][s] += d[r] * d[s];
      }
      double v[3];
      smallest_eigen(m, v);
      for (size_t d = 0; d < 3; ++d) p.normals[3 * i + d] = float(v[d]);
    }
  });
  // Consistent orientation: each grid cell's normals agree with the cell's own direction, and the cells are walked from
  // the one farthest from the centre (facing away from it) across their neighbours, each turned to agree with the one it
  // was reached from. A cloud is many cells, so this needs no neighbour lists per point.
  const auto b = bounds(p);
  const double centre[3] = {(b[0][0] + b[1][0]) / 2, (b[0][1] + b[1][1]) / 2, (b[0][2] + b[1][2]) / 2};
  const double ext = std::max({b[1][0] - b[0][0], b[1][1] - b[0][1], b[1][2] - b[0][2], 1e-6});
  const double cell = std::max(grid.cell() * 1.5, ext / 2000);
  std::vector<std::pair<uint64_t, uint32_t>> keys(n);
  for (size_t i = 0; i < n; ++i) {
    uint64_t c[3];
    for (size_t d = 0; d < 3; ++d) c[d] = uint64_t(std::floor((double(p.xyz[3 * i + d]) - b[0][d]) / cell));
    keys[i] = {(c[0] << 42) | (c[1] << 21) | c[2], uint32_t(i)};
  }
  std::sort(keys.begin(), keys.end());
  std::vector<uint64_t> cellKey;
  std::vector<uint32_t> cellStart;
  for (size_t i = 0; i < n; ++i)
    if (cellKey.empty() || cellKey.back() != keys[i].first) cellKey.push_back(keys[i].first), cellStart.push_back(uint32_t(i));
  cellStart.push_back(uint32_t(n));
  const size_t cells = cellKey.size();
  std::vector<std::array<float, 3>> cn(cells), cc(cells);
  for (size_t c = 0; c < cells; ++c) {
    const float* n0 = &p.normals[3 * size_t(keys[cellStart[c]].second)];
    double s[3] = {0, 0, 0}, q[3] = {0, 0, 0};
    for (uint32_t j = cellStart[c]; j < cellStart[c + 1]; ++j) {
      const size_t i = keys[j].second;
      const float* v = &p.normals[3 * i];
      const double sign = v[0] * n0[0] + v[1] * n0[1] + v[2] * n0[2] < 0 ? -1 : 1;
      for (size_t d = 0; d < 3; ++d) s[d] += sign * v[d], q[d] += p.xyz[3 * i + d];
    }
    const double l = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    const double m = double(cellStart[c + 1] - cellStart[c]);
    for (size_t d = 0; d < 3; ++d) cn[c][d] = float(l > 0 ? s[d] / l : n0[d]), cc[c][d] = float(q[d] / m);
  }
  auto find = [&](uint64_t key) -> int64_t {
    const auto it = std::lower_bound(cellKey.begin(), cellKey.end(), key);
    return it != cellKey.end() && *it == key ? int64_t(it - cellKey.begin()) : -1;
  };
  std::vector<char> seen(cells, 0);
  // Seeds: farthest cells from the centre first.
  std::vector<uint32_t> seeds(cells);
  std::iota(seeds.begin(), seeds.end(), 0u);
  auto distant = [&](uint32_t c) {
    double s = 0;
    for (size_t d = 0; d < 3; ++d) s += (cc[c][d] - centre[d]) * (cc[c][d] - centre[d]);
    return s;
  };
  std::sort(seeds.begin(), seeds.end(), [&](uint32_t a, uint32_t c) { return distant(a) > distant(c); });
  std::deque<uint32_t> queue;
  for (uint32_t seed : seeds) {
    if (seen[seed]) continue;
    double out = 0;
    for (size_t d = 0; d < 3; ++d) out += cn[seed][d] * (cc[seed][d] - centre[d]);
    if (out < 0)
      for (auto& v : cn[seed]) v = -v;
    seen[seed] = 1;
    queue.push_back(seed);
    while (!queue.empty()) {
      const uint32_t c = queue.front();
      queue.pop_front();
      const uint64_t key = cellKey[c];
      const int64_t x = int64_t(key >> 42), y = int64_t((key >> 21) & 0x1FFFFF), z = int64_t(key & 0x1FFFFF);
      for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dz = -1; dz <= 1; ++dz) {
            if (!dx && !dy && !dz) continue;
            if (x + dx < 0 || y + dy < 0 || z + dz < 0) continue;
            const int64_t o = find(uint64_t(x + dx) << 42 | uint64_t(y + dy) << 21 | uint64_t(z + dz));
            if (o < 0 || seen[size_t(o)]) continue;
            // Across a sharp edge the neighbour's direction says little: such cells wait for a smoother way in.
            const double dot = cn[c][0] * cn[size_t(o)][0] + cn[c][1] * cn[size_t(o)][1] + cn[c][2] * cn[size_t(o)][2];
            if (dot < 0)
              for (auto& v : cn[size_t(o)]) v = -v;
            seen[size_t(o)] = 1;
            if (std::abs(dot) > 0.5) queue.push_front(uint32_t(o));
            else queue.push_back(uint32_t(o));
          }
    }
  }
  blocks(cells, cancel, [&](size_t a, size_t e) {
    for (size_t c = a; c < e; ++c)
      for (uint32_t j = cellStart[c]; j < cellStart[c + 1]; ++j) {
        float* v = &p.normals[3 * size_t(keys[j].second)];
        if (v[0] * cn[c][0] + v[1] * cn[c][1] + v[2] * cn[c][2] < 0) v[0] = -v[0], v[1] = -v[1], v[2] = -v[2];
      }
  });
}

}  // namespace opad::cloud
