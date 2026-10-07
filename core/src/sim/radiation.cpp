// Surface-to-surface radiation by rays (sim/radiation.hpp): a bounding volume tree over the surfaces' triangles, rays from each
// surface (stratified over its area and, cosine-weighted, over its half of the sky), first hits counted per surface, rays that
// meet nothing gone to the room; the shares made reciprocal (A_i F_ij = A_j F_ji); radiosities by Gauss-Seidel.
#include "radiation.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <thread>

namespace opad::sim::radiation {
namespace {

using V = Vec3;
V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V add(const V& a, const V& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V mul(const V& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V unit(const V& a) {
  const double l = std::sqrt(dot(a, a));
  return l > 0 ? mul(a, 1 / l) : V{0, 0, 1};
}

constexpr double kSigma = 5.670374419e-8;  // W/m2K4

// Binary tree over triangles, split at the median of the longest axis of their centres.
struct Tree {
  struct Node {
    V lo, hi;
    int left = -1, right = -1;  // children, or -1 for a leaf holding [first, first + count) of `order`
    int first = 0, count = 0;
  };
  std::vector<Node> nodes;
  std::vector<int> order;
  const std::vector<Surface>* s = nullptr;

  explicit Tree(const std::vector<Surface>& surfaces) : s(&surfaces) {
    order.resize(surfaces.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    if (!order.empty()) build(0, int(order.size()));
  }
  int build(int first, int count) {
    Node n;
    n.lo = {1e300, 1e300, 1e300};
    n.hi = {-1e300, -1e300, -1e300};
    V clo = n.lo, chi = n.hi;
    for (int i = first; i < first + count; ++i) {
      const Surface& t = (*s)[size_t(order[size_t(i)])];
      V c{0, 0, 0};
      for (const V& p : t.corners)
        for (int k = 0; k < 3; ++k) n.lo[k] = std::min(n.lo[k], p[k]), n.hi[k] = std::max(n.hi[k], p[k]), c[k] += p[k] / 3;
      for (int k = 0; k < 3; ++k) clo[k] = std::min(clo[k], c[k]), chi[k] = std::max(chi[k], c[k]);
    }
    const int at = int(nodes.size());
    nodes.push_back(n);
    if (count <= 4) {
      nodes[size_t(at)].first = first;
      nodes[size_t(at)].count = count;
      return at;
    }
    int axis = 0;
    for (int k = 1; k < 3; ++k)
      if (chi[k] - clo[k] > chi[axis] - clo[axis]) axis = k;
    const int mid = first + count / 2;
    auto centre = [&](int i) {
      const Surface& t = (*s)[size_t(i)];
      return t.corners[0][axis] + t.corners[1][axis] + t.corners[2][axis];
    };
    std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count, [&](int a, int b) { return centre(a) < centre(b); });
    const int l = build(first, mid - first);
    const int r = build(mid, first + count - mid);
    nodes[size_t(at)].left = l;
    nodes[size_t(at)].right = r;
    return at;
  }
  // The nearest triangle the ray meets past 0 (skipping `self`), or -1.
  int hit(const V& o, const V& d, int self) const {
    if (nodes.empty()) return -1;
    const V inv{1 / (d[0] != 0 ? d[0] : 1e-300), 1 / (d[1] != 0 ? d[1] : 1e-300), 1 / (d[2] != 0 ? d[2] : 1e-300)};
    double best = std::numeric_limits<double>::infinity();
    int found = -1;
    int stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top > 0) {
      const Node& n = nodes[size_t(stack[--top])];
      double t0 = 0, t1 = best;
      bool miss = false;
      for (int k = 0; k < 3 && !miss; ++k) {
        double a = (n.lo[k] - o[k]) * inv[k], b = (n.hi[k] - o[k]) * inv[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        miss = t0 > t1;
      }
      if (miss) continue;
      if (n.left < 0) {
        for (int i = n.first; i < n.first + n.count; ++i) {
          const int j = order[size_t(i)];
          if (j == self) continue;
          const auto& c = (*s)[size_t(j)].corners;
          // Moller-Trumbore, both sides.
          const V e1 = sub(c[1], c[0]), e2 = sub(c[2], c[0]);
          const V p = cross(d, e2);
          const double det = dot(e1, p);
          if (std::fabs(det) < 1e-300) continue;
          const double id = 1 / det;
          const V tv = sub(o, c[0]);
          const double u = dot(tv, p) * id;
          if (u < 0 || u > 1) continue;
          const V q = cross(tv, e1);
          const double v = dot(d, q) * id;
          if (v < 0 || u + v > 1) continue;
          const double t = dot(e2, q) * id;
          if (t > 0 && t < best) best = t, found = j;
        }
      } else if (top + 2 <= 128) {
        stack[top++] = n.left;
        stack[top++] = n.right;
      }
    }
    return found;
  }
};

// Small, fast, repeatable random numbers (splitmix64).
struct Random {
  uint64_t x;
  double next() {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return double((z ^ (z >> 31)) >> 11) * (1.0 / 9007199254740992.0);
  }
};

}  // namespace

ViewFactors view_factors(const std::vector<Surface>& surfaces, int rays, int threads) {
  const size_t n = surfaces.size();
  ViewFactors vf;
  vf.to.resize(n);
  vf.room.assign(n, 1.0);
  if (n == 0) return vf;
  rays = std::max(rays, 16);
  const Tree tree(surfaces);
  // Rays start a hair off their surface: a millionth of the scene's size.
  const auto& root = tree.nodes.front();
  const double lift = 1e-7 * std::sqrt(dot(sub(root.hi, root.lo), sub(root.hi, root.lo)));
  const int side = std::max(1, int(std::sqrt(double(rays))));
  std::atomic<size_t> next{0};
  auto work = [&] {
    std::vector<int> hits;
    for (size_t i; (i = next.fetch_add(1)) < n;) {
      const Surface& s = surfaces[i];
      const V nrm = unit(s.normal);
      const V a = unit(cross(nrm, std::fabs(nrm[0]) < 0.9 ? V{1, 0, 0} : V{0, 1, 0})), b = cross(nrm, a);
      Random rnd{0x5EEDull * (i + 1)};
      hits.clear();
      int escaped = 0;
      for (int r = 0; r < rays; ++r) {
        // A point on the triangle, a direction stratified over the cosine-weighted hemisphere (Malley).
        double u = rnd.next(), v = rnd.next();
        if (u + v > 1) u = 1 - u, v = 1 - v;
        const V p = add(s.corners[0], add(mul(sub(s.corners[1], s.corners[0]), u), mul(sub(s.corners[2], s.corners[0]), v)));
        const int cell = r % (side * side);
        const double r1 = (cell % side + rnd.next()) / side, r2 = (cell / side + rnd.next()) / side;
        const double rad = std::sqrt(r1), phi = 2 * M_PI * r2;
        const double z = std::sqrt(std::max(0.0, 1 - r1));
        const V d = add(add(mul(a, rad * std::cos(phi)), mul(b, rad * std::sin(phi))), mul(nrm, z));
        const int j = tree.hit(add(p, mul(nrm, lift)), d, int(i));
        if (j < 0) ++escaped;
        else hits.push_back(j);
      }
      std::sort(hits.begin(), hits.end());
      auto& row = vf.to[i];
      for (size_t k = 0; k < hits.size();) {
        size_t e = k;
        while (e < hits.size() && hits[e] == hits[k]) ++e;
        row.push_back({hits[k], float(double(e - k) / rays)});
        k = e;
      }
      vf.room[i] = double(escaped) / rays;
    }
  };
  if (threads <= 0) threads = int(std::max(1u, std::thread::hardware_concurrency()));
  threads = int(std::min<size_t>(size_t(threads), n));
  std::vector<std::thread> pool;
  for (int t = 1; t < threads; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
  vf.rays = (long long)rays * (long long)n;

  // Reciprocity: the exchange S_ij = A_i F_ij and A_j F_ji averaged, then scaled symmetrically (x_i x_j S_ij) until each
  // surface's row again adds up to the share of its rays that met something: exact reciprocity, the room shares as cast.
  std::map<std::pair<int, int>, double> both;
  for (size_t i = 0; i < n; ++i)
    for (const auto& [j, f] : vf.to[i]) both[{std::min(int(i), j), std::max(int(i), j)}] += 0.5 * surfaces[i].area * double(f);
  struct Pair {
    int i, j;
    double S;
  };
  std::vector<Pair> pairs;
  pairs.reserve(both.size());
  for (const auto& [key, S] : both) pairs.push_back({key.first, key.second, S});
  std::vector<double> x(n, 1.0), row(n);
  for (int it = 0; it < 100; ++it) {
    std::fill(row.begin(), row.end(), 0.0);
    for (const Pair& p : pairs) {
      const double s = x[size_t(p.i)] * x[size_t(p.j)] * p.S;
      row[size_t(p.i)] += s;
      row[size_t(p.j)] += s;
    }
    double worst = 0;
    for (size_t i = 0; i < n; ++i) {
      const double want = surfaces[i].area * (1 - vf.room[i]);
      if (row[i] <= 0 || want <= 0) continue;
      worst = std::max(worst, std::fabs(row[i] / want - 1));
      x[i] *= std::sqrt(want / row[i]);
    }
    if (worst < 1e-6) break;
  }
  for (auto& r : vf.to) r.clear();
  std::fill(row.begin(), row.end(), 0.0);
  for (const Pair& p : pairs) {
    const double s = x[size_t(p.i)] * x[size_t(p.j)] * p.S;
    const double Ai = surfaces[size_t(p.i)].area, Aj = surfaces[size_t(p.j)].area;
    if (Ai > 0) vf.to[size_t(p.i)].push_back({p.j, float(s / Ai)}), row[size_t(p.i)] += s / Ai;
    if (Aj > 0) vf.to[size_t(p.j)].push_back({p.i, float(s / Aj)}), row[size_t(p.j)] += s / Aj;
  }
  for (size_t i = 0; i < n; ++i) vf.room[i] = std::max(0.0, 1 - row[i]);
  return vf;
}

std::vector<double> sinks(const ViewFactors& vf, const std::vector<Surface>& surfaces, const std::vector<double>& T, double room_C) {
  const size_t n = surfaces.size();
  auto E = [](double C) {
    const double K = C + 273.15;
    return kSigma * K * K * K * K;
  };
  const double Eroom = E(room_C);
  std::vector<double> J(n), G(n, Eroom);
  for (size_t i = 0; i < n; ++i) J[i] = E(T[i]);
  // J_i = eps_i E_i + (1 - eps_i) G_i, G_i = sum_j F_ij J_j + F_i,room E_room. (1 - eps) F has a spectral radius below one.
  for (int it = 0; it < 500; ++it) {
    double change = 0, scale = 0;
    for (size_t i = 0; i < n; ++i) {
      double g = vf.room[i] * Eroom;
      for (const auto& [j, f] : vf.to[i]) g += double(f) * J[size_t(j)];
      G[i] = g;
      const double eps = std::clamp(surfaces[i].emissivity, 0.0, 1.0);
      const double j = eps * E(T[i]) + (1 - eps) * g;
      change = std::max(change, std::fabs(j - J[i]));
      scale = std::max(scale, j);
      J[i] = j;
    }
    if (change <= 1e-9 * scale) break;
  }
  std::vector<double> out(n);
  for (size_t i = 0; i < n; ++i) out[i] = std::pow(std::max(G[i], 0.0) / kSigma, 0.25) - 273.15;
  return out;
}

}  // namespace opad::sim::radiation
