// Mesh checks and repair (opad/mesh_edit.hpp): edges counted by sorting the triangles' sides, shells and holes by union-find,
// crossings through a box tree over the triangles, holes closed by earcut on the hole's own plane.
#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include "../../third_party/earcut/earcut.hpp"
#include "opad/mesh_edit.hpp"
#include "opad/util.hpp"

namespace opad::meshops {
namespace {

struct Side {
  uint64_t key;  // smaller vertex << 32 | larger
  uint32_t tri;
  bool forward;  // runs smaller -> larger in its triangle
};

uint64_t edge_key(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

std::vector<Side> sides_of(const Mesh& m) {
  std::vector<Side> s;
  s.reserve(m.tri.size());
  for (size_t t = 0; t < m.triangles(); ++t)
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = m.tri[t * 3 + k], b = m.tri[t * 3 + (k + 1) % 3];
      if (a == b) continue;
      s.push_back({edge_key(a, b), static_cast<uint32_t>(t), a < b});
    }
  std::sort(s.begin(), s.end(), [](const Side& x, const Side& y) { return x.key != y.key ? x.key < y.key : x.tri < y.tri; });
  return s;
}

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

double diagonal(const Mesh& m) {
  const Bnd_Box b = bounds(m);
  return b.IsVoid() ? 0 : std::sqrt(b.SquareExtent());
}

double twice_area(const Mesh& m, size_t t) {
  const double* a = &m.xyz[size_t(m.tri[t * 3]) * 3];
  const double* b = &m.xyz[size_t(m.tri[t * 3 + 1]) * 3];
  const double* c = &m.xyz[size_t(m.tri[t * 3 + 2]) * 3];
  const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  const double x = u[1] * w[2] - u[2] * w[1], y = u[2] * w[0] - u[0] * w[2], z = u[0] * w[1] - u[1] * w[0];
  return std::sqrt(x * x + y * y + z * z);
}

bool degenerate(const Mesh& m, size_t t, double minArea2) {
  const uint32_t a = m.tri[t * 3], b = m.tri[t * 3 + 1], c = m.tri[t * 3 + 2];
  return a == b || b == c || a == c || twice_area(m, t) <= minArea2;
}

struct TriKey {
  uint32_t v[3];
  bool operator==(const TriKey& o) const { return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2]; }
};
struct TriHash {
  size_t operator()(const TriKey& k) const { return (size_t(k.v[0]) * 73856093u) ^ (size_t(k.v[1]) * 19349663u) ^ (size_t(k.v[2]) * 83492791u); }
};
TriKey sorted_key(const Mesh& m, size_t t) {
  TriKey k{{m.tri[t * 3], m.tri[t * 3 + 1], m.tri[t * 3 + 2]}};
  std::sort(k.v, k.v + 3);
  return k;
}

// ---------------------------------------------------------------- triangle crossings
using V3 = std::array<double, 3>;
V3 sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Whether two triangles that share no vertex cross (Moller's interval test); coplanar pairs are not counted.
bool triangles_cross(const V3 p[3], const V3 q[3], double eps) {
  const V3 n1 = cross(sub(p[1], p[0]), sub(p[2], p[0]));
  double dq[3];
  for (int i = 0; i < 3; ++i) dq[i] = dot(n1, sub(q[i], p[0]));
  const double s1 = std::sqrt(dot(n1, n1)) * eps;
  for (double& d : dq)
    if (std::fabs(d) < s1) d = 0;
  if ((dq[0] > 0 && dq[1] > 0 && dq[2] > 0) || (dq[0] < 0 && dq[1] < 0 && dq[2] < 0)) return false;
  const V3 n2 = cross(sub(q[1], q[0]), sub(q[2], q[0]));
  double dp[3];
  for (int i = 0; i < 3; ++i) dp[i] = dot(n2, sub(p[i], q[0]));
  const double s2 = std::sqrt(dot(n2, n2)) * eps;
  for (double& d : dp)
    if (std::fabs(d) < s2) d = 0;
  if ((dp[0] > 0 && dp[1] > 0 && dp[2] > 0) || (dp[0] < 0 && dp[1] < 0 && dp[2] < 0)) return false;
  if (dp[0] == 0 && dp[1] == 0 && dp[2] == 0) return false;  // coplanar
  const V3 dir = cross(n1, n2);
  // Each triangle's interval along the line where the planes meet.
  auto interval = [&](const V3 t[3], const double d[3], double& lo, double& hi) {
    double proj[3];
    for (int i = 0; i < 3; ++i) proj[i] = dot(dir, t[i]);
    // The vertex alone on its side (or the first on the plane) and the other two.
    int lone = 0;
    if ((d[0] > 0) == (d[1] > 0) && d[0] != 0 && d[1] != 0) lone = 2;
    else if ((d[0] > 0) == (d[2] > 0) && d[0] != 0 && d[2] != 0) lone = 1;
    else lone = 0;
    const int a = (lone + 1) % 3, b = (lone + 2) % 3;
    auto at = [&](int i, int j) { return d[i] == d[j] ? proj[i] : proj[i] + (proj[j] - proj[i]) * d[i] / (d[i] - d[j]); };
    const double t1 = at(lone, a), t2 = at(lone, b);
    lo = std::min(t1, t2), hi = std::max(t1, t2);
  };
  double a0, a1, b0, b1;
  interval(p, dp, a0, a1);
  interval(q, dq, b0, b1);
  const double slack = (std::fabs(a1 - a0) + std::fabs(b1 - b0)) * 1e-9;
  return a1 > b0 + slack && b1 > a0 + slack;
}

struct BoxTree {
  struct Node {
    float lo[3], hi[3];
    int left = -1, right = -1, first = 0, count = 0;
  };
  const Mesh& m;
  std::vector<uint32_t> order;
  std::vector<std::array<float, 6>> box;
  std::vector<Node> nodes;
  explicit BoxTree(const Mesh& mesh) : m(mesh) {
    const size_t n = m.triangles();
    order.resize(n);
    std::iota(order.begin(), order.end(), 0u);
    box.resize(n);
    for (size_t t = 0; t < n; ++t) {
      auto& b = box[t];
      for (int k = 0; k < 3; ++k) b[k] = INFINITY, b[k + 3] = -INFINITY;
      for (int c = 0; c < 3; ++c)
        for (int k = 0; k < 3; ++k) {
          const float v = static_cast<float>(m.xyz[size_t(m.tri[t * 3 + c]) * 3 + k]);
          b[k] = std::min(b[k], std::nextafter(v, -INFINITY));
          b[k + 3] = std::max(b[k + 3], std::nextafter(v, INFINITY));
        }
    }
    nodes.reserve(n / 2 + 1);
    if (n) build(0, static_cast<int>(n));
  }
  int build(int first, int count) {
    const int id = static_cast<int>(nodes.size());
    nodes.emplace_back();
    Node nd;
    for (int k = 0; k < 3; ++k) nd.lo[k] = INFINITY, nd.hi[k] = -INFINITY;
    for (int i = first; i < first + count; ++i)
      for (int k = 0; k < 3; ++k) nd.lo[k] = std::min(nd.lo[k], box[order[i]][k]), nd.hi[k] = std::max(nd.hi[k], box[order[i]][k + 3]);
    if (count <= 8) {
      nd.first = first, nd.count = count;
      nodes[id] = nd;
      return id;
    }
    int axis = 0;
    for (int k = 1; k < 3; ++k)
      if (nd.hi[k] - nd.lo[k] > nd.hi[axis] - nd.lo[axis]) axis = k;
    const int mid = first + count / 2;
    std::nth_element(order.begin() + first, order.begin() + mid, order.begin() + first + count,
                     [&](uint32_t a, uint32_t b) { return box[a][axis] + box[a][axis + 3] < box[b][axis] + box[b][axis + 3]; });
    nodes[id] = nd;
    const int l = build(first, mid - first);
    const int r = build(mid, first + count - mid);
    nodes[id].left = l, nodes[id].right = r;
    return id;
  }
  template <class F>
  void query(const std::array<float, 6>& q, F&& visit) const {
    int stack[128], top = 0;
    stack[top++] = 0;
    while (top) {
      const Node& nd = nodes[size_t(stack[--top])];
      if (nd.lo[0] > q[3] || nd.hi[0] < q[0] || nd.lo[1] > q[4] || nd.hi[1] < q[1] || nd.lo[2] > q[5] || nd.hi[2] < q[2]) continue;
      if (nd.left < 0) {
        for (int i = nd.first; i < nd.first + nd.count; ++i) {
          const auto& b = box[order[size_t(i)]];
          if (b[0] > q[3] || b[3] < q[0] || b[1] > q[4] || b[4] < q[1] || b[2] > q[5] || b[5] < q[2]) continue;
          visit(order[size_t(i)]);
        }
      } else if (top < 126) {
        stack[top++] = nd.left;
        stack[top++] = nd.right;
      }
    }
  }
};

size_t count_crossings(const Mesh& m, size_t limit, bool& capped, const Cancel& cancel) {
  capped = false;
  if (m.triangles() < 2) return 0;
  const BoxTree tree(m);
  const double eps = 1e-10;
  size_t found = 0;
  auto corner = [&](uint32_t t, int k) {
    const double* p = &m.xyz[size_t(m.tri[size_t(t) * 3 + k]) * 3];
    return V3{p[0], p[1], p[2]};
  };
  for (uint32_t t = 0; t < m.triangles(); ++t) {
    if ((t & 0x3FFF) == 0 && cancel && cancel()) throw Error("cancelled");
    const V3 p[3] = {corner(t, 0), corner(t, 1), corner(t, 2)};
    const uint32_t* tv = &m.tri[size_t(t) * 3];
    tree.query(tree.box[t], [&](uint32_t o) {
      if (o <= t || found >= limit) return;
      const uint32_t* ov = &m.tri[size_t(o) * 3];
      for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k)
          if (tv[i] == ov[k]) return;  // neighbours meet along their shared vertex or side
      const V3 q[3] = {corner(o, 0), corner(o, 1), corner(o, 2)};
      if (triangles_cross(p, q, eps)) ++found;
    });
    if (found >= limit) {
      capped = true;
      break;
    }
  }
  return found;
}

}  // namespace

json Report::to_json() const {
  json j = {{"vertices", vertices},
            {"triangles", triangles},
            {"shells", shells},
            {"open_edges", open_edges},
            {"holes", holes},
            {"non_manifold_edges", non_manifold_edges},
            {"flipped_edges", flipped_edges},
            {"degenerate_triangles", degenerate},
            {"duplicate_triangles", duplicates},
            {"self_intersections", self_intersections},
            {"closed", closed()},
            {"printable", printable()},
            {"volume_mm3", volume},
            {"area_mm2", area}};
  if (self_intersections_capped) j["self_intersections_capped"] = true;
  return j;
}

Report check(const Mesh& m, const CheckOptions& opt, const Cancel& cancel) {
  Report r;
  r.vertices = m.vertices();
  r.triangles = m.triangles();
  const double diag = diagonal(m);
  const double minArea2 = diag * diag * 1e-14;
  std::unordered_set<TriKey, TriHash> seen;
  seen.reserve(m.triangles());
  for (size_t t = 0; t < m.triangles(); ++t) {
    const double a2 = twice_area(m, t);
    r.area += a2 / 2;
    if (degenerate(m, t, minArea2)) ++r.degenerate;
    else if (!seen.insert(sorted_key(m, t)).second) ++r.duplicates;
  }
  if (cancel && cancel()) throw Error("cancelled");
  r.volume = volume(m);
  const std::vector<Side> sides = sides_of(m);
  Sets shells(m.vertices()), rims(m.vertices());
  for (size_t t = 0; t < m.triangles(); ++t) {
    shells.join(m.tri[t * 3], m.tri[t * 3 + 1]);
    shells.join(m.tri[t * 3], m.tri[t * 3 + 2]);
  }
  std::vector<char> onRim(m.vertices(), 0);
  for (size_t i = 0; i < sides.size();) {
    size_t j = i;
    while (j < sides.size() && sides[j].key == sides[i].key) ++j;
    const size_t n = j - i;
    if (n == 1) {
      ++r.open_edges;
      const uint32_t a = uint32_t(sides[i].key >> 32), b = uint32_t(sides[i].key & 0xFFFFFFFFu);
      rims.join(a, b);
      onRim[a] = onRim[b] = 1;
    } else if (n > 2) {
      ++r.non_manifold_edges;
    } else if (sides[i].forward == sides[i + 1].forward) {
      ++r.flipped_edges;
    }
    i = j;
  }
  std::unordered_set<uint32_t> roots, rimRoots;
  std::vector<char> used(m.vertices(), 0);
  for (uint32_t v : m.tri) used[v] = 1;
  for (size_t v = 0; v < m.vertices(); ++v) {
    if (used[v]) roots.insert(shells.find(uint32_t(v)));
    if (onRim[v]) rimRoots.insert(rims.find(uint32_t(v)));
  }
  r.shells = roots.size();
  r.holes = rimRoots.size();
  if (opt.self_intersections) r.self_intersections = count_crossings(m, opt.limit ? opt.limit : SIZE_MAX, r.self_intersections_capped, cancel);
  return r;
}

// ---------------------------------------------------------------- repair
namespace {

// Turns triangles so each pair across a side runs it opposite ways, shell by shell (breadth first over sides two
// triangles share), then turns a closed shell whose volume comes out negative inside out. Returns the triangles turned.
size_t orient(Mesh& m) {
  const size_t nt = m.triangles();
  const std::vector<Side> sides = sides_of(m);
  // Neighbours across manifold sides.
  std::vector<std::array<int64_t, 3>> across(nt, {-1, -1, -1});
  auto slot = [&](uint32_t t, uint64_t key) {
    for (int k = 0; k < 3; ++k)
      if (edge_key(m.tri[size_t(t) * 3 + k], m.tri[size_t(t) * 3 + (k + 1) % 3]) == key) return k;
    return 0;
  };
  for (size_t i = 0; i < sides.size();) {
    size_t j = i;
    while (j < sides.size() && sides[j].key == sides[i].key) ++j;
    if (j - i == 2) {
      across[sides[i].tri][size_t(slot(sides[i].tri, sides[i].key))] = sides[i + 1].tri;
      across[sides[i + 1].tri][size_t(slot(sides[i + 1].tri, sides[i].key))] = sides[i].tri;
    }
    i = j;
  }
  std::vector<int> shell(nt, -1);
  std::vector<char> flip(nt, 0);
  std::vector<uint32_t> queue;
  std::vector<std::vector<uint32_t>> shells;
  auto runs = [&](uint32_t t, uint32_t a, uint32_t b) {  // does t (as flipped so far) run a -> b?
    const uint32_t* v = &m.tri[size_t(t) * 3];
    for (int k = 0; k < 3; ++k) {
      const uint32_t p = v[k], q = v[(k + 1) % 3];
      if (p == a && q == b) return !flip[t];
      if (p == b && q == a) return bool(flip[t]);
    }
    return false;
  };
  for (uint32_t seed = 0; seed < nt; ++seed) {
    if (shell[seed] >= 0) continue;
    const int id = static_cast<int>(shells.size());
    shells.emplace_back();
    shell[seed] = id;
    queue.assign(1, seed);
    for (size_t qi = 0; qi < queue.size(); ++qi) {
      const uint32_t t = queue[qi];
      shells[size_t(id)].push_back(t);
      for (int k = 0; k < 3; ++k) {
        const int64_t o = across[t][size_t(k)];
        if (o < 0 || shell[size_t(o)] >= 0) continue;
        uint32_t a = m.tri[size_t(t) * 3 + k], b = m.tri[size_t(t) * 3 + (k + 1) % 3];
        if (flip[t]) std::swap(a, b);  // the way t runs this side now
        // The neighbour must run b -> a; it runs a -> b as it stands: turn it.
        if (runs(uint32_t(o), a, b)) flip[size_t(o)] = 1;
        shell[size_t(o)] = id;
        queue.push_back(uint32_t(o));
      }
    }
  }
  for (size_t t = 0; t < nt; ++t)
    if (flip[t]) std::swap(m.tri[t * 3 + 1], m.tri[t * 3 + 2]);
  // Inside out: a closed shell with a negative volume.
  for (const auto& tris : shells) {
    double v6 = 0;
    bool closed = true;
    for (uint32_t t : tris) {
      for (int k = 0; k < 3; ++k)
        if (across[t][size_t(k)] < 0) closed = false;
      const double* a = &m.xyz[size_t(m.tri[size_t(t) * 3]) * 3];
      const double* b = &m.xyz[size_t(m.tri[size_t(t) * 3 + 1]) * 3];
      const double* c = &m.xyz[size_t(m.tri[size_t(t) * 3 + 2]) * 3];
      v6 += a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]);
    }
    if (closed && v6 < 0)
      for (uint32_t t : tris) {
        std::swap(m.tri[size_t(t) * 3 + 1], m.tri[size_t(t) * 3 + 2]);
        flip[t] = !flip[t];
      }
  }
  return static_cast<size_t>(std::count(flip.begin(), flip.end(), 1));
}

// Closes each loop of open sides (up to max edges) with triangles laid by earcut on the loop's own plane (Newell normal).
size_t fill_holes(Mesh& m, size_t maxEdges, size_t& added, const Cancel& cancel) {
  const std::vector<Side> sides = sides_of(m);
  // Open sides as the hole runs them: opposite to their triangle (so the patch's triangles join the right way round).
  std::unordered_map<uint32_t, std::vector<uint32_t>> next;
  size_t open = 0;
  for (size_t i = 0; i < sides.size();) {
    size_t j = i;
    while (j < sides.size() && sides[j].key == sides[i].key) ++j;
    if (j - i == 1) {
      const uint32_t lo = uint32_t(sides[i].key >> 32), hi = uint32_t(sides[i].key & 0xFFFFFFFFu);
      const uint32_t a = sides[i].forward ? lo : hi, b = sides[i].forward ? hi : lo;  // the triangle runs a -> b
      next[b].push_back(a);
      ++open;
    }
    i = j;
  }
  size_t filled = 0;
  added = 0;
  std::vector<uint32_t> loop;
  std::vector<uint32_t> fresh;
  while (!next.empty()) {
    if (cancel && cancel()) throw Error("cancelled");
    auto it = next.begin();
    const uint32_t start = it->first;
    loop.assign(1, start);
    uint32_t at = start;
    bool closed = false;
    for (size_t guard = 0; guard <= open; ++guard) {
      auto n = next.find(at);
      if (n == next.end() || n->second.empty()) break;
      const uint32_t to = n->second.back();
      n->second.pop_back();
      if (n->second.empty()) next.erase(n);
      if (to == start) {
        closed = true;
        break;
      }
      // A pinched rim (a vertex met twice) is walked as one loop through it; earcut takes the touching corner.
      loop.push_back(to);
      at = to;
    }
    if (!closed || loop.size() < 3 || (maxEdges && loop.size() > maxEdges)) continue;
    // Newell normal, then the loop in 2D on that plane.
    double nx = 0, ny = 0, nz = 0;
    for (size_t i = 0; i < loop.size(); ++i) {
      const double* p = &m.xyz[size_t(loop[i]) * 3];
      const double* q = &m.xyz[size_t(loop[(i + 1) % loop.size()]) * 3];
      nx += (p[1] - q[1]) * (p[2] + q[2]);
      ny += (p[2] - q[2]) * (p[0] + q[0]);
      nz += (p[0] - q[0]) * (p[1] + q[1]);
    }
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    fresh.clear();
    if (loop.size() == 3) {
      fresh = {loop[0], loop[1], loop[2]};
    } else if (len > 0) {
      nx /= len, ny /= len, nz /= len;
      V3 ux = std::fabs(nx) < 0.9 ? V3{1, 0, 0} : V3{0, 1, 0};
      const V3 n{nx, ny, nz};
      V3 u = cross(n, ux);
      const double ul = std::sqrt(dot(u, u));
      for (auto& c : u) c /= ul;
      const V3 w = cross(n, u);
      std::vector<std::vector<std::array<double, 2>>> poly(1);
      poly[0].reserve(loop.size());
      for (uint32_t v : loop) {
        const V3 p{m.xyz[size_t(v) * 3], m.xyz[size_t(v) * 3 + 1], m.xyz[size_t(v) * 3 + 2]};
        poly[0].push_back({dot(p, u), dot(p, w)});
      }
      const std::vector<uint32_t> idx = mapbox::earcut<uint32_t>(poly);
      for (size_t i = 0; i + 2 < idx.size(); i += 3) fresh.insert(fresh.end(), {loop[idx[i]], loop[idx[i + 1]], loop[idx[i + 2]]});
      if (fresh.empty())  // earcut gave up (a twisted loop): a fan from the first vertex
        for (size_t i = 1; i + 1 < loop.size(); ++i) fresh.insert(fresh.end(), {loop[0], loop[i], loop[i + 1]});
    } else {
      for (size_t i = 1; i + 1 < loop.size(); ++i) fresh.insert(fresh.end(), {loop[0], loop[i], loop[i + 1]});
    }
    // Each patch triangle the same way round as the loop (which runs opposite to the rim's triangles).
    for (size_t i = 0; i + 2 < fresh.size(); i += 3) {
      const double* a = &m.xyz[size_t(fresh[i]) * 3];
      const double* b = &m.xyz[size_t(fresh[i + 1]) * 3];
      const double* c = &m.xyz[size_t(fresh[i + 2]) * 3];
      const V3 tn = cross(V3{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, V3{c[0] - a[0], c[1] - a[1], c[2] - a[2]});
      if (dot(tn, V3{nx, ny, nz}) < 0) std::swap(fresh[i + 1], fresh[i + 2]);
    }
    m.tri.insert(m.tri.end(), fresh.begin(), fresh.end());
    added += fresh.size() / 3;
    ++filled;
  }
  return filled;
}

}  // namespace

json RepairResult::to_json() const {
  return {{"before", before.to_json()},
          {"after", after.to_json()},
          {"welded_vertices", welded},
          {"removed_degenerate", removed_degenerate},
          {"removed_duplicates", removed_duplicates},
          {"flipped_triangles", flipped},
          {"holes_filled", holes_filled},
          {"fill_triangles", fill_triangles}};
}

RepairResult repair(const Mesh& in, const RepairOptions& opt, const Cancel& cancel) {
  RepairResult r;
  r.before = check(in, {false, 0}, cancel);
  Mesh m = weld(in, opt.weld, &r.welded);
  const double diag = diagonal(m);
  const double minArea2 = diag * diag * 1e-14;
  if (opt.remove_degenerate || opt.remove_duplicates) {
    // A triangle over the same corners as an earlier one: the same way round is a copy (one kept); the other way round is
    // a folded pair, a fin of no volume (both go, which leaves the sides they hung on to their other triangles).
    std::unordered_map<TriKey, size_t, TriHash> seen;  // -> index into kept / 3
    seen.reserve(m.triangles());
    std::vector<uint32_t> kept;
    std::vector<char> dropped;
    kept.reserve(m.tri.size());
    auto even = [](const uint32_t* v) {  // the corners' order is an even turn of their sorted order
      const int inversions = (v[0] > v[1]) + (v[0] > v[2]) + (v[1] > v[2]);
      return inversions % 2 == 0;
    };
    for (size_t t = 0; t < m.triangles(); ++t) {
      if (opt.remove_degenerate && degenerate(m, t, minArea2)) {
        ++r.removed_degenerate;
        continue;
      }
      if (opt.remove_duplicates) {
        auto [it, added] = seen.emplace(sorted_key(m, t), kept.size() / 3);
        if (!added) {
          ++r.removed_duplicates;
          const size_t first = it->second;
          if (!dropped[first] && even(&kept[first * 3]) != even(&m.tri[t * 3])) {
            dropped[first] = 1;
            ++r.removed_duplicates;
          }
          continue;
        }
      }
      kept.insert(kept.end(), {m.tri[t * 3], m.tri[t * 3 + 1], m.tri[t * 3 + 2]});
      dropped.push_back(0);
    }
    m.tri.clear();
    for (size_t i = 0; i < dropped.size(); ++i)
      if (!dropped[i]) m.tri.insert(m.tri.end(), {kept[i * 3], kept[i * 3 + 1], kept[i * 3 + 2]});
  }
  if (cancel && cancel()) throw Error("cancelled");
  if (opt.orient) r.flipped = orient(m);
  if (opt.fill_holes) {
    r.holes_filled = fill_holes(m, opt.max_hole_edges, r.fill_triangles, cancel);
    if (r.holes_filled && opt.orient) r.flipped += orient(m);  // a shell closed now may come out inside out
  }
  // Vertices no triangle uses any more are dropped.
  std::vector<int64_t> map(m.vertices(), -1);
  Mesh out;
  out.tri.reserve(m.tri.size());
  for (uint32_t v : m.tri) {
    if (map[v] < 0) {
      map[v] = static_cast<int64_t>(out.vertices());
      out.xyz.insert(out.xyz.end(), {m.xyz[size_t(v) * 3], m.xyz[size_t(v) * 3 + 1], m.xyz[size_t(v) * 3 + 2]});
    }
    out.tri.push_back(static_cast<uint32_t>(map[v]));
  }
  if (out.empty()) throw Error("nothing is left of the mesh after the repair");
  r.mesh = std::move(out);
  r.after = check(r.mesh, {opt.count_crossings, 1000}, cancel);
  return r;
}

}  // namespace opad::meshops
