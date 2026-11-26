// Fitted references on meshes (opad/scan_fit.hpp): region growing, robust plane / line / cylinder fits and the candidate
// frames of a scan. Robust here means least median of squares over sampled hypotheses (no threshold to guess: half the
// data may be noise, a broken edge or the next face), then least squares on the inliers it implies.
#include "opad/scan_fit.hpp"

#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <numeric>
#include <random>
#include <unordered_map>

#include "meshsolid/meshsolid.hpp"
#include "opad/util.hpp"

namespace opad::scanfit {

namespace {

constexpr double kPi = 3.14159265358979323846;
double rad(double deg) { return deg * kPi / 180; }

// Deterministic sampling (std::uniform_int_distribution differs between standard libraries; mt19937's raw output does not).
struct Rng {
  std::mt19937 g{0x5ca9f17u};
  size_t below(size_t n) { return n ? size_t(g()) % n : 0; }
};

double weighted_median(std::vector<std::pair<double, double>>& v) {  // (value, weight)
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  double total = 0;
  for (const auto& p : v) total += p.second;
  double run = 0;
  for (const auto& p : v) {
    run += p.second;
    if (run >= total / 2) return p.first;
  }
  return v.back().first;
}

double extent_of(const std::vector<gp_Pnt>& pts, const std::vector<int>* only = nullptr) {
  gp_XYZ lo(1e300, 1e300, 1e300), hi(-1e300, -1e300, -1e300);
  auto add = [&](const gp_Pnt& p) {
    for (int k = 1; k <= 3; ++k) {
      lo.SetCoord(k, std::min(lo.Coord(k), p.Coord(k)));
      hi.SetCoord(k, std::max(hi.Coord(k), p.Coord(k)));
    }
  };
  if (only)
    for (int i : *only) add(pts[size_t(i)]);
  else
    for (const auto& p : pts) add(p);
  return lo.X() > hi.X() ? 0 : (hi - lo).Modulus();
}

// Weighted mean and covariance's eigen axes (ascending eigenvalues) of the chosen points.
struct Pca {
  gp_Pnt mean;
  gp_Dir axis[3];  // smallest to largest variance
  double value[3] = {0, 0, 0};
};
Pca pca(const std::vector<gp_Pnt>& pts, const std::vector<double>& w, const std::vector<int>& use) {
  Eigen::Vector3d m = Eigen::Vector3d::Zero();
  double sw = 0;
  for (int i : use) {
    const double wi = w.empty() ? 1.0 : w[size_t(i)];
    m += wi * Eigen::Vector3d(pts[size_t(i)].X(), pts[size_t(i)].Y(), pts[size_t(i)].Z());
    sw += wi;
  }
  if (sw <= 0) throw Error("nothing to fit");
  m /= sw;
  Eigen::Matrix3d c = Eigen::Matrix3d::Zero();
  for (int i : use) {
    const double wi = w.empty() ? 1.0 : w[size_t(i)];
    const Eigen::Vector3d d = Eigen::Vector3d(pts[size_t(i)].X(), pts[size_t(i)].Y(), pts[size_t(i)].Z()) - m;
    c += wi * d * d.transpose();
  }
  Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(c / sw);
  Pca out;
  out.mean = gp_Pnt(m.x(), m.y(), m.z());
  for (int k = 0; k < 3; ++k) {
    const Eigen::Vector3d v = es.eigenvectors().col(k);
    out.axis[k] = v.norm() > 0 ? gp_Dir(v.x(), v.y(), v.z()) : gp_Dir(k == 0, k == 1, k == 2);
    out.value[k] = std::max(0.0, es.eigenvalues()(k));
  }
  return out;
}

std::vector<int> all_of(size_t n) {
  std::vector<int> v(n);
  std::iota(v.begin(), v.end(), 0);
  return v;
}

std::vector<int> sample_of(size_t n, size_t most, Rng& rng) {
  if (n <= most) return all_of(n);
  std::vector<int> v(most);
  for (auto& i : v) i = int(rng.below(n));
  return v;
}

// A sign that does not depend on the data's order: the largest component positive.
gp_Dir canonical(gp_Dir d) {
  const double ax = std::fabs(d.X()), ay = std::fabs(d.Y()), az = std::fabs(d.Z());
  const double big = ax >= ay && ax >= az ? d.X() : ay >= az ? d.Y() : d.Z();
  if (big < 0) d.Reverse();
  return d;
}

gp_Dir laid_into(const gp_Dir& d, const gp_Dir& n) {  // d with its part along n taken out (some direction in the plane when none is left)
  gp_Vec v = gp_Vec(d) - gp_Vec(n) * gp_Vec(d).Dot(gp_Vec(n));
  if (v.Magnitude() < 1e-9) v = gp_Vec(n).Crossed(std::fabs(n.X()) < 0.9 ? gp_Vec(1, 0, 0) : gp_Vec(0, 1, 0));
  return gp_Dir(v);
}

// A direction in the plane square to n: world X laid onto it, else world Y.
gp_Dir plane_x(const gp_Dir& n) {
  gp_Vec v = gp_Vec(1, 0, 0) - gp_Vec(n) * n.X();
  if (v.Magnitude() < 1e-6) v = gp_Vec(0, 1, 0) - gp_Vec(n) * n.Y();
  return gp_Dir(v);
}

void grade(Quality& q) {
  const double ratio = q.rms / std::max(q.extent, 1e-9);
  const double kept = q.total ? double(q.used) / q.total : 1.0;
  q.grade = ratio < 0.004 && kept >= 0.75 ? "good" : ratio > 0.015 || kept < 0.4 ? "poor" : "fair";
}

}  // namespace

json Quality::to_json() const {
  return {{"rms_mm", rms}, {"max_mm", max}, {"used", used}, {"total", total}, {"extent_mm", extent}, {"grade", grade}};
}

// ---------------------------------------------------------------- the mesh
std::shared_ptr<const FitMesh> fit_mesh(const TopoDS_Shape& shape, const std::function<bool()>& cancelled) {
  auto m = std::make_shared<FitMesh>();
  auto check = [&] {
    if (cancelled && cancelled()) throw Error("cancelled");
  };
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {  // as opad::subshape numbers a mesh
    const TopoDS_Face face = TopoDS::Face(f.Current());
    TopLoc_Location loc;
    const auto t = BRep_Tool::Triangulation(face, loc);
    if (t.IsNull()) continue;
    const gp_Trsf tr = loc.Transformation();
    const int first = int(m->nodes.size());
    for (int i = 1; i <= t->NbNodes(); ++i) m->nodes.push_back(loc.IsIdentity() ? t->Node(i) : t->Node(i).Transformed(tr));
    const bool reversed = face.Orientation() == TopAbs_REVERSED;
    for (int i = 1; i <= t->NbTriangles(); ++i) {
      int a, b, c;
      t->Triangle(i).Get(a, b, c);
      if (reversed) std::swap(b, c);
      m->tri.push_back({first + a - 1, first + b - 1, first + c - 1});
    }
  }
  if (m->tri.empty()) throw Error("the body has no triangles to fit to");
  check();
  const size_t n = m->tri.size();
  m->normal.resize(n);
  m->area.resize(n);
  m->centre.resize(n);
  gp_XYZ weighted(0, 0, 0);
  for (size_t t = 0; t < n; ++t) {
    const auto& v = m->tri[t];
    const gp_Pnt &a = m->nodes[size_t(v[0])], &b = m->nodes[size_t(v[1])], &c = m->nodes[size_t(v[2])];
    const gp_Vec cr = gp_Vec(a, b).Crossed(gp_Vec(a, c));
    const double mag = cr.Magnitude();
    m->area[t] = mag / 2;
    m->normal[t] = mag > 1e-300 ? cr / mag : gp_Vec(0, 0, 0);
    m->centre[t] = gp_Pnt((a.XYZ() + b.XYZ() + c.XYZ()) / 3);
    m->total_area += m->area[t];
    weighted += m->centre[t].XYZ() * m->area[t];
  }
  m->centroid = m->total_area > 0 ? gp_Pnt(weighted / m->total_area) : m->centre[0];
  m->diagonal = extent_of(m->nodes);
  check();
  // Welded vertices (scan files repeat every vertex per triangle), then sides by their welded ends, sorted.
  const double weld = std::max(1e-9, m->diagonal * 1e-7);
  std::vector<int> welded(m->nodes.size());
  {
    struct Key {
      long long x, y, z;
      bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct Hash {
      size_t operator()(const Key& k) const { return size_t(k.x * 73856093LL ^ k.y * 19349663LL ^ k.z * 83492791LL); }
    };
    std::unordered_map<Key, int, Hash> ids;
    ids.reserve(m->nodes.size());
    for (size_t i = 0; i < m->nodes.size(); ++i) {
      const gp_Pnt& p = m->nodes[i];
      const Key k{std::llround(p.X() / weld), std::llround(p.Y() / weld), std::llround(p.Z() / weld)};
      welded[i] = ids.emplace(k, int(ids.size())).first->second;
    }
  }
  check();
  struct Side {
    unsigned long long key;
    int at;  // 3 t + k
  };
  std::vector<Side> sides;
  sides.reserve(3 * n);
  for (size_t t = 0; t < n; ++t)
    for (int k = 0; k < 3; ++k) {
      const unsigned long long a = unsigned(welded[size_t(m->tri[t][size_t(k)])]), b = unsigned(welded[size_t(m->tri[t][size_t((k + 1) % 3)])]);
      if (a == b) continue;
      sides.push_back({std::min(a, b) << 32 | std::max(a, b), int(3 * t + size_t(k))});
    }
  std::sort(sides.begin(), sides.end(), [](const Side& x, const Side& y) { return x.key < y.key; });
  m->across.assign(n, {-1, -1, -1});
  for (size_t i = 0; i < sides.size();) {
    size_t j = i + 1;
    while (j < sides.size() && sides[j].key == sides[i].key) ++j;
    if (j - i == 2) {  // a manifold side
      const int p = sides[i].at, q = sides[i + 1].at;
      m->across[size_t(p / 3)][size_t(p % 3)] = q / 3;
      m->across[size_t(q / 3)][size_t(q % 3)] = p / 3;
    }
    i = j;
  }
  check();
  // How noisy the mesh is: the area-weighted median of each triangle's smallest angle to a neighbour. A CAD mesh's flats are
  // exact (0); a scan's facets lean a few degrees. Noisy meshes classify by normals averaged over the neighbours, clean
  // ones by their own (averaging would bend every facet next to a sharp edge, and a CAD face is often two triangles).
  std::vector<std::pair<double, double>> lean;
  lean.reserve(std::min<size_t>(n, 200000));
  const size_t stride = std::max<size_t>(1, n / 200000);
  for (size_t t = 0; t < n; t += stride) {
    double least = kPi;
    for (int k = 0; k < 3; ++k)
      if (const int o = m->across[t][size_t(k)]; o >= 0) least = std::min(least, std::acos(std::clamp(m->normal[t].Dot(m->normal[size_t(o)]), -1.0, 1.0)));
    if (least < kPi) lean.push_back({least, m->area[t]});
  }
  const bool noisy = weighted_median(lean) > rad(0.5);
  m->smooth = m->normal;
  if (noisy)
    for (int pass = 0; pass < 6; ++pass) {
      std::vector<gp_Vec> next(n);
      for (size_t t = 0; t < n; ++t) {
        gp_Vec s = m->smooth[t] * m->area[t];
        for (int k = 0; k < 3; ++k)
          if (const int o = m->across[t][size_t(k)]; o >= 0) s += m->smooth[size_t(o)] * m->area[size_t(o)];
        next[t] = s.Magnitude() > 1e-300 ? s.Normalized() : m->normal[t];
      }
      m->smooth.swap(next);
      check();
    }
  return m;
}

std::shared_ptr<const FitMesh> cached_fit_mesh(const std::string& key, const TopoDS_Shape& shape, const std::function<bool()>& cancelled) {
  static std::mutex lock;
  static std::list<std::pair<std::string, std::shared_ptr<const FitMesh>>> kept;  // most recent first
  if (!key.empty()) {
    std::lock_guard<std::mutex> hold(lock);
    for (auto it = kept.begin(); it != kept.end(); ++it)
      if (it->first == key) {
        kept.splice(kept.begin(), kept, it);
        return kept.front().second;
      }
  }
  auto m = fit_mesh(shape, cancelled);
  if (!key.empty()) {
    std::lock_guard<std::mutex> hold(lock);
    kept.emplace_front(key, m);
    while (kept.size() > 3) kept.pop_back();
  }
  return m;
}

// ---------------------------------------------------------------- regions
std::vector<int> grow_region(const FitMesh& m, int seed, double angle, bool follow, size_t limit) {
  if (seed < 0 || seed >= m.triangles()) throw Error("the picked triangle is not on the mesh");
  const double c = std::cos(angle);
  std::vector<char> seen(size_t(m.triangles()), 0);
  std::vector<int> out{seed}, queue{seed};
  seen[size_t(seed)] = 1;
  // The region starts facing as the seed's neighbourhood does (two rings), not the seed alone: a click often lands on a
  // sliver whose own normal the noise turned.
  gp_Vec sum = m.smooth[size_t(seed)] * std::max(m.area[size_t(seed)], 1e-300);
  {
    std::vector<int> ring{seed};
    for (int depth = 0, from = 0; depth < 2; ++depth) {
      const int to = int(ring.size());
      for (int i = from; i < to; ++i)
        for (int k = 0; k < 3; ++k)
          if (const int o = m.across[size_t(ring[size_t(i)])][size_t(k)]; o >= 0 && std::find(ring.begin(), ring.end(), o) == ring.end()) ring.push_back(o);
      from = to;
    }
    gp_Vec around(0, 0, 0);
    for (int t : ring) around += m.smooth[size_t(t)] * m.area[size_t(t)];
    // Only when the neighbourhood is one surface (not a seed beside a sharp edge, whose rings reach round it).
    if (around.Magnitude() > 0 && around.Normalized().Dot(m.smooth[size_t(seed)]) > std::cos(std::max(angle, 0.35))) sum = around;
  }
  auto close_to = [&](int t, const gp_Vec& d) {
    return m.smooth[size_t(t)].Dot(d) >= c || m.normal[size_t(t)].Dot(d) >= c;  // either: noise or a CAD face's exact normal
  };
  // A flat also takes a facet the noise turned too far when it lies on the region's plane (its middle within three times
  // the region's own spread of it) and leans less than twice the tolerance: without this, a scan's region stopped wherever
  // a ring of noisy facets closed round it. A sharp edge still stops it (the wall's facets lean far more).
  const double loose = std::cos(std::min(2 * angle, 1.4));
  gp_XYZ middle = m.centre[size_t(seed)].XYZ() * std::max(m.area[size_t(seed)], 1e-300);
  double weight = std::max(m.area[size_t(seed)], 1e-300), spread = 0, spread_weight = 0;
  const double floor = m.diagonal * 1e-6;
  auto on_plane = [&](int o, const gp_Vec& mean) {
    if (out.size() < 30 || m.smooth[size_t(o)].Dot(mean) < loose) return false;
    const double d = std::fabs(gp_Vec(gp_Pnt(middle / weight), m.centre[size_t(o)]).Dot(mean));
    return d <= std::max(3 * std::sqrt(spread / std::max(spread_weight, 1e-300)), floor);
  };
  // Round faces: neighbour to neighbour by the facets' own normals (the averaged ones ramp round a sharp rim and let the
  // region run on into the flat), or by the averaged ones within a quarter of the tolerance (a scan's noise).
  const double close = std::cos(angle / 4);
  for (size_t at = 0; at < queue.size() && out.size() < limit; ++at) {
    const int t = queue[at];
    const gp_Vec mean = sum.Magnitude() > 0 ? sum.Normalized() : m.smooth[size_t(t)];
    for (int k = 0; k < 3; ++k) {
      const int o = m.across[size_t(t)][size_t(k)];
      if (o < 0 || seen[size_t(o)]) continue;
      if (m.area[size_t(o)] <= 0) {  // a sliver: taken along, never steering
        seen[size_t(o)] = 1;
        queue.push_back(o);
        continue;
      }
      const bool take = follow ? m.normal[size_t(o)].Dot(m.normal[size_t(t)]) >= c || m.smooth[size_t(o)].Dot(m.smooth[size_t(t)]) >= close
                               : close_to(o, mean) || on_plane(o, mean);
      if (!take) continue;
      seen[size_t(o)] = 1;
      queue.push_back(o);
      out.push_back(o);
      sum += m.smooth[size_t(o)] * m.area[size_t(o)];
      if (!follow) {
        const double d = gp_Vec(gp_Pnt(middle / weight), m.centre[size_t(o)]).Dot(mean);
        spread += m.area[size_t(o)] * d * d, spread_weight += m.area[size_t(o)];
        middle += m.centre[size_t(o)].XYZ() * m.area[size_t(o)], weight += m.area[size_t(o)];
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------- planes
Plane fit_plane(const std::vector<gp_Pnt>& pts, const std::vector<double>& weights, const std::vector<gp_Vec>& normals, const gp_Vec& facing) {
  const size_t n = pts.size();
  if (n < 3) throw Error("a plane needs at least three points");
  const double extent = extent_of(pts);
  if (extent <= 0) throw Error("the points are all in one place");
  const std::vector<double>& w = weights;
  Rng rng;
  const std::vector<int> all = all_of(n);
  const std::vector<int> sample = sample_of(n, 3000, rng);
  auto score = [&](const gp_Pnt& o, const gp_Dir& d) {
    std::vector<std::pair<double, double>> r;
    r.reserve(sample.size());
    for (int i : sample) r.push_back({std::fabs(gp_Vec(o, pts[size_t(i)]).Dot(gp_Vec(d))), w.empty() ? 1.0 : w[size_t(i)]});
    return weighted_median(r);
  };
  Pca first = pca(pts, w, all);
  gp_Pnt bo = first.mean;
  gp_Dir bn = first.axis[0];
  if (n > 5) {
    double best = score(bo, bn);
    auto consider = [&](const gp_Pnt& o, const gp_Vec& d) {
      if (d.Magnitude() < 1e-12) return;
      const double s = score(o, gp_Dir(d));
      if (s < best) best = s, bo = o, bn = gp_Dir(d);
    };
    if (!normals.empty())
      for (int k = 0; k < 150; ++k) {
        const size_t i = size_t(sample[rng.below(sample.size())]);
        consider(pts[i], normals[i]);
      }
    for (int k = 0; k < 150; ++k) {
      const gp_Pnt &a = pts[size_t(sample[rng.below(sample.size())])], &b = pts[size_t(sample[rng.below(sample.size())])], &c = pts[size_t(sample[rng.below(sample.size())])];
      const gp_Vec cr = gp_Vec(a, b).Crossed(gp_Vec(a, c));
      if (cr.Magnitude() > 1e-9 * extent * extent) consider(a, cr);
    }
  }
  // Least squares on the inliers the best hypothesis implies (3 robust sigmas), three times over.
  std::vector<int> in = all;
  Pca fit = first;
  for (int round = 0; round < (n > 5 ? 4 : 1); ++round) {
    std::vector<std::pair<double, double>> r;
    r.reserve(sample.size());
    for (int i : sample) r.push_back({std::fabs(gp_Vec(bo, pts[size_t(i)]).Dot(gp_Vec(bn))), w.empty() ? 1.0 : w[size_t(i)]});
    const double sigma = 1.4826 * weighted_median(r);
    const double limit = std::max(3 * sigma, 1e-7 * extent);
    in.clear();
    for (int i : all)
      if (std::fabs(gp_Vec(bo, pts[size_t(i)]).Dot(gp_Vec(bn))) <= limit) in.push_back(i);
    if (in.size() < 3) in = all;
    fit = pca(pts, w, in);
    bo = fit.mean;
    bn = fit.axis[0];
  }
  Plane out;
  out.normal = bn;
  if (facing.Magnitude() > 1e-12) {
    if (gp_Vec(out.normal).Dot(facing) < 0) out.normal.Reverse();
  } else {
    // The turn of the first points picked (the first that are not in a line), right-handed.
    for (size_t k = 2; k < n; ++k) {
      const gp_Vec rh = gp_Vec(pts[0], pts[1]).Crossed(gp_Vec(pts[0], pts[k]));
      if (rh.Magnitude() <= 1e-3 * extent * extent) continue;
      if (gp_Vec(out.normal).Dot(rh) < 0) out.normal.Reverse();
      break;
    }
  }
  out.origin = fit.mean;
  out.x = laid_into(fit.axis[2], out.normal);
  if (gp_Vec(out.x).Dot(gp_Vec(plane_x(out.normal))) < 0) out.x.Reverse();
  double ss = 0, sw = 0, most = 0;
  for (int i : in) {
    const double d = std::fabs(gp_Vec(out.origin, pts[size_t(i)]).Dot(gp_Vec(out.normal)));
    const double wi = w.empty() ? 1.0 : w[size_t(i)];
    ss += wi * d * d;
    sw += wi;
    most = std::max(most, d);
  }
  out.q.rms = sw > 0 ? std::sqrt(ss / sw) : 0;
  out.q.max = most;
  out.q.used = int(in.size());
  out.q.total = int(n);
  out.q.extent = extent_of(pts, &in);
  grade(out.q);
  return out;
}

Plane fit_plane_region(const FitMesh& m, const std::vector<int>& tris) {
  if (tris.size() < 1) throw Error("the region has no triangles");
  std::vector<gp_Pnt> pts;
  std::vector<double> w;
  std::vector<gp_Vec> normals;
  gp_Vec facing(0, 0, 0);
  // One triangle is a plane already (a CAD face may be two): its corners make the fit.
  if (tris.size() < 3)
    for (int t : tris)
      for (int k = 0; k < 3; ++k) {
        pts.push_back(m.nodes[size_t(m.tri[size_t(t)][size_t(k)])]);
        w.push_back(std::max(m.area[size_t(t)], 1e-300));
        normals.push_back(m.normal[size_t(t)]);
      }
  else
    for (int t : tris) {
      pts.push_back(m.centre[size_t(t)]);
      w.push_back(std::max(m.area[size_t(t)], 1e-300));
      normals.push_back(m.normal[size_t(t)]);
    }
  for (int t : tris) facing += m.normal[size_t(t)] * m.area[size_t(t)];
  Plane p = fit_plane(pts, w, normals, facing);
  if (tris.size() < 3) {  // counted in triangles, as the region is
    p.q.used = p.q.total = int(tris.size());
  }
  return p;
}

// ---------------------------------------------------------------- lines and axes
Line fit_line(const std::vector<gp_Pnt>& pts) {
  const size_t n = pts.size();
  if (n < 2) throw Error("a line needs at least two points");
  const double extent = extent_of(pts);
  if (extent <= 0) throw Error("the points are all in one place");
  auto dist = [&](const gp_Pnt& o, const gp_Dir& d, const gp_Pnt& p) { return gp_Vec(o, p).Crossed(gp_Vec(d)).Magnitude(); };
  Rng rng;
  const std::vector<int> all = all_of(n), sample = sample_of(n, 3000, rng);
  Pca first = pca(pts, {}, all);
  gp_Pnt bo = first.mean;
  gp_Dir bd = first.axis[2];
  auto score = [&](const gp_Pnt& o, const gp_Dir& d) {
    std::vector<std::pair<double, double>> r;
    for (int i : sample) r.push_back({dist(o, d, pts[size_t(i)]), 1.0});
    return weighted_median(r);
  };
  if (n > 5) {
    double best = score(bo, bd);
    for (int k = 0; k < 200; ++k) {
      const gp_Pnt &a = pts[size_t(sample[rng.below(sample.size())])], &b = pts[size_t(sample[rng.below(sample.size())])];
      if (a.Distance(b) < 1e-6 * extent) continue;
      const gp_Dir d(gp_Vec(a, b));
      const double s = score(a, d);
      if (s < best) best = s, bo = a, bd = d;
    }
  }
  std::vector<int> in = all;
  for (int round = 0; round < (n > 5 ? 4 : 1); ++round) {
    std::vector<std::pair<double, double>> r;
    for (int i : sample) r.push_back({dist(bo, bd, pts[size_t(i)]), 1.0});
    const double limit = std::max(3 * 1.4826 * weighted_median(r), 1e-7 * extent);
    in.clear();
    for (int i : all)
      if (dist(bo, bd, pts[size_t(i)]) <= limit) in.push_back(i);
    if (in.size() < 2) in = all;
    const Pca f = pca(pts, {}, in);
    bo = f.mean;
    bd = f.axis[2];
  }
  Line out;
  out.dir = bd;
  if (gp_Vec(pts[0], pts[n - 1]).Dot(gp_Vec(out.dir)) < 0) out.dir.Reverse();  // from the first point picked towards the last
  out.origin = bo;
  double ss = 0, most = 0;
  for (int i : in) {
    const double d = dist(bo, bd, pts[size_t(i)]);
    ss += d * d;
    most = std::max(most, d);
  }
  out.q.rms = std::sqrt(ss / double(in.size()));
  out.q.max = most;
  out.q.used = int(in.size());
  out.q.total = int(n);
  out.q.extent = extent_of(pts, &in);
  grade(out.q);
  return out;
}

Line fit_cylinder_region(const FitMesh& m, const std::vector<int>& tris) {
  if (tris.size() < 8) throw Error("too few triangles for a round face: pick a hole or a boss");
  Rng rng;
  const std::vector<int> sample = sample_of(tris.size(), 3000, rng);
  auto nrm = [&](int i) { return m.smooth[size_t(tris[size_t(i)])]; };
  auto wt = [&](int i) { return m.area[size_t(tris[size_t(i)])]; };
  // The axis: square to the normals of the round part; pairs of normals at an angle give hypotheses, the share of the
  // area square to them scores them (the flats around a hole lean along the axis and do not count).
  const double square = std::sin(rad(6));
  // Normals square to the axis that all point one way are a flat (or a wall) seen edge on, not a round face: the share is
  // scaled down by how much they agree (their mean's length: about 0 round a whole hole, 0.64 round half of it, 1 for a flat).
  auto share = [&](const gp_Dir& a) {
    double s = 0, all = 0;
    gp_Vec mean(0, 0, 0);
    for (int i : sample) {
      all += wt(i);
      if (std::fabs(nrm(i).Dot(gp_Vec(a))) < square) s += wt(i), mean += nrm(i) * wt(i);
    }
    const double agree = s > 0 ? mean.Magnitude() / s : 1;
    return all > 0 ? s / all * (1 - 0.9 * agree) : 0.0;
  };
  gp_Dir axis(0, 0, 1);
  double best = -1;
  for (int k = 0; k < 400; ++k) {
    const int i = sample[rng.below(sample.size())], j = sample[rng.below(sample.size())];
    const gp_Vec c = nrm(i).Crossed(nrm(j));
    if (c.Magnitude() < std::sin(rad(15))) continue;
    const gp_Dir a(c);
    if (const double s = share(a); s > best) best = s, axis = a;
  }
  if (best < 0) throw Error("the picked region is flat: pick a hole or a boss");
  std::vector<int> round;  // indices into tris square to the axis
  for (int pass = 0; pass < 3; ++pass) {
    round.clear();
    Eigen::Matrix3d s = Eigen::Matrix3d::Zero();
    for (size_t i = 0; i < tris.size(); ++i) {
      const gp_Vec& v = nrm(int(i));
      if (std::fabs(v.Dot(gp_Vec(axis))) >= std::sin(rad(pass == 0 ? 10 : 8))) continue;
      round.push_back(int(i));
      const Eigen::Vector3d e(v.X(), v.Y(), v.Z());
      s += wt(int(i)) * e * e.transpose();
    }
    if (round.size() < 6) break;
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(s);
    const Eigen::Vector3d a = es.eigenvectors().col(0);
    axis = gp_Dir(a.x(), a.y(), a.z());
  }
  double round_area = 0, all_area = 0;
  for (size_t i = 0; i < tris.size(); ++i) all_area += wt(int(i));
  for (int i : round) round_area += wt(i);
  if (round.size() < 6 || round_area < 0.3 * all_area) throw Error("the picked region is not round: pick a hole or a boss");
  // The circle in the plane across the axis, robustly, then refined on the inliers.
  const gp_Dir e1 = laid_into(plane_x(axis), axis), e2(gp_Vec(axis).Crossed(gp_Vec(e1)));
  // The round triangles' corners: a mesh's vertices lie on the surface (a facet's middle lies inside a curved one).
  const gp_Pnt c0 = m.centre[size_t(tris[size_t(round[0])])];
  std::vector<int> corners;
  for (int i : round)
    for (int k = 0; k < 3; ++k) corners.push_back(m.tri[size_t(tris[size_t(i)])][size_t(k)]);
  std::sort(corners.begin(), corners.end());
  corners.erase(std::unique(corners.begin(), corners.end()), corners.end());
  std::vector<std::array<double, 2>> q;
  std::vector<gp_Pnt> p3;
  for (int c : corners) {
    const gp_Pnt& p = m.nodes[size_t(c)];
    const gp_Vec d(c0, p);
    q.push_back({d.Dot(gp_Vec(e1)), d.Dot(gp_Vec(e2))});
    p3.push_back(p);
  }
  const std::vector<int> qs = sample_of(q.size(), 3000, rng);
  auto circle_score = [&](double cx, double cy, double r) {
    std::vector<std::pair<double, double>> v;
    for (int i : qs) v.push_back({std::fabs(std::hypot(q[size_t(i)][0] - cx, q[size_t(i)][1] - cy) - r), 1.0});
    return weighted_median(v);
  };
  double cx = 0, cy = 0, r = 0;
  if (!meshsolid::fit_circle_2d(q, cx, cy, r)) r = -1;
  double bs = r > 0 ? circle_score(cx, cy, r) : 1e300;
  for (int k = 0; k < 300; ++k) {
    const auto &a = q[size_t(qs[rng.below(qs.size())])], &b = q[size_t(qs[rng.below(qs.size())])], &c = q[size_t(qs[rng.below(qs.size())])];
    const double d = 2 * (a[0] * (b[1] - c[1]) + b[0] * (c[1] - a[1]) + c[0] * (a[1] - b[1]));
    if (std::fabs(d) < 1e-12) continue;
    const double a2 = a[0] * a[0] + a[1] * a[1], b2 = b[0] * b[0] + b[1] * b[1], c2 = c[0] * c[0] + c[1] * c[1];
    const double x = (a2 * (b[1] - c[1]) + b2 * (c[1] - a[1]) + c2 * (a[1] - b[1])) / d;
    const double y = (a2 * (c[0] - b[0]) + b2 * (a[0] - c[0]) + c2 * (b[0] - a[0])) / d;
    const double rr = std::hypot(a[0] - x, a[1] - y);
    if (const double s = circle_score(x, y, rr); s < bs) bs = s, cx = x, cy = y, r = rr;
  }
  if (!(r > 0)) throw Error("no circle fits the picked region");
  std::vector<int> in;
  for (int pass = 0; pass < 3; ++pass) {
    const double limit = std::max(3 * 1.4826 * circle_score(cx, cy, r), 1e-7 * m.diagonal);
    std::vector<std::array<double, 2>> kept;
    in.clear();
    for (size_t i = 0; i < q.size(); ++i)
      if (std::fabs(std::hypot(q[i][0] - cx, q[i][1] - cy) - r) <= limit) in.push_back(int(i)), kept.push_back(q[i]);
    double x = cx, y = cy, rr = r;
    if (kept.size() >= 3 && meshsolid::fit_circle_2d(kept, x, y, rr)) cx = x, cy = y, r = rr;
  }
  // The axis and circle refined together (Levenberg-Marquardt on the inliers' distances to the cylinder).
  meshsolid::Prim cyl;
  cyl.type = meshsolid::Prim::Cylinder;
  cyl.o = c0.Translated(gp_Vec(e1) * cx + gp_Vec(e2) * cy);
  cyl.a = axis;
  cyl.r = r;
  {
    std::vector<gp_Pnt> pick;
    for (size_t k = 0; k < in.size() && pick.size() < 20000; k += std::max<size_t>(1, in.size() / 20000)) pick.push_back(p3[size_t(in[k])]);
    if (pick.size() >= 6) cyl = meshsolid::refine(cyl, pick, m.diagonal);
  }
  const double extent = [&] {
    std::vector<gp_Pnt> kept;
    for (int i : in) kept.push_back(p3[size_t(i)]);
    return extent_of(kept);
  }();
  if (cyl.r > 50 * std::max(extent, 1e-9)) throw Error("the picked region is nearly flat: pick a hole or a boss");
  Line out;
  out.dir = canonical(cyl.a);
  out.radius = cyl.r;
  double lo = 1e300, hi = -1e300, ss = 0, most = 0;
  for (int i : in) {
    const gp_Vec d(cyl.o, p3[size_t(i)]);
    const double t = d.Dot(gp_Vec(out.dir));
    lo = std::min(lo, t), hi = std::max(hi, t);
    const double e = std::fabs((d - gp_Vec(out.dir) * t).Magnitude() - cyl.r);
    ss += e * e;
    most = std::max(most, e);
  }
  out.origin = cyl.o.Translated(gp_Vec(out.dir) * ((lo + hi) / 2));
  out.q.rms = in.empty() ? 0 : std::sqrt(ss / double(in.size()));
  out.q.max = most;
  out.q.used = int(in.size());
  out.q.total = int(p3.size());  // corners of the round part
  out.q.extent = extent;
  grade(out.q);
  return out;
}

std::vector<int> grow_round(const FitMesh& m, int seed, double angle) {
  std::vector<int> region = grow_region(m, seed, angle, true, 400);
  const double lean = std::cos(std::max(angle, rad(25)));
  for (int round = 0; round < 4; ++round) {
    const Line fit = fit_cylinder_region(m, region);
    const double tol = std::max({3 * fit.q.rms, 0.002 * fit.radius, m.diagonal * 1e-6});
    auto on = [&](int t) {
      double d = 0;
      for (int k = 0; k < 3; ++k) {
        const gp_Vec v(fit.origin, m.nodes[size_t(m.tri[size_t(t)][size_t(k)])]);
        d += std::fabs((v - gp_Vec(fit.dir) * v.Dot(gp_Vec(fit.dir))).Magnitude() - fit.radius);
      }
      if (d / 3 > tol) return false;
      gp_Vec radial(fit.origin, m.centre[size_t(t)]);
      radial -= gp_Vec(fit.dir) * radial.Dot(gp_Vec(fit.dir));
      if (radial.Magnitude() <= 0) return false;
      radial.Normalize();
      return std::fabs(m.normal[size_t(t)].Dot(radial)) >= lean || std::fabs(m.smooth[size_t(t)].Dot(radial)) >= lean;
    };
    std::vector<char> seen(size_t(m.triangles()), 0);
    std::vector<int> next{seed};
    seen[size_t(seed)] = 1;
    for (size_t at = 0; at < next.size(); ++at)
      for (int k = 0; k < 3; ++k) {
        const int o = m.across[size_t(next[at])][size_t(k)];
        if (o < 0 || seen[size_t(o)]) continue;
        seen[size_t(o)] = 1;
        if (on(o)) next.push_back(o);
      }
    if (next.size() < 8) break;
    const bool settled = next.size() <= region.size() + region.size() / 50 && next.size() + next.size() / 50 >= region.size();
    region.swap(next);
    if (settled) break;
  }
  return region;
}

Line wall_direction(const FitMesh& m, const std::vector<int>& tris) {
  const Plane p = fit_plane_region(m, tris);
  Line out;
  out.origin = p.origin;
  out.dir = p.x;
  out.q = p.q;
  return out;
}

Line plane_intersection(const Plane& a, const Plane& b, double min_angle) {
  const gp_Vec d = gp_Vec(a.normal).Crossed(gp_Vec(b.normal));
  if (d.Magnitude() < std::sin(min_angle)) throw Error("the two planes are (nearly) parallel: they meet nowhere useful");
  const gp_Dir dir(d);
  const gp_Pnt mid((a.origin.XYZ() + b.origin.XYZ()) / 2);
  Eigen::Matrix3d M;
  M << a.normal.X(), a.normal.Y(), a.normal.Z(), b.normal.X(), b.normal.Y(), b.normal.Z(), dir.X(), dir.Y(), dir.Z();
  const Eigen::Vector3d rhs(gp_Vec(a.normal).Dot(gp_Vec(a.origin.XYZ())), gp_Vec(b.normal).Dot(gp_Vec(b.origin.XYZ())), gp_Vec(dir).Dot(gp_Vec(mid.XYZ())));
  const Eigen::Vector3d x = M.colPivHouseholderQr().solve(rhs);
  Line out;
  out.origin = gp_Pnt(x.x(), x.y(), x.z());
  out.dir = dir;
  out.q.rms = std::max(a.q.rms, b.q.rms);
  out.q.max = std::max(a.q.max, b.q.max);
  out.q.used = a.q.used + b.q.used;
  out.q.total = a.q.total + b.q.total;
  out.q.extent = std::min(a.q.extent, b.q.extent);
  const auto rank = [](const std::string& g) { return g == "poor" ? 2 : g == "fair" ? 1 : 0; };
  out.q.grade = rank(a.q.grade) >= rank(b.q.grade) ? a.q.grade : b.q.grade;
  if (out.q.grade.empty()) grade(out.q);
  return out;
}

// ---------------------------------------------------------------- lining a scan up
namespace {

// The cube-map bin of a direction (n bins a side on each of six faces).
int sphere_bin(const gp_Vec& d, int n) {
  const double ax = std::fabs(d.X()), ay = std::fabs(d.Y()), az = std::fabs(d.Z());
  int face;
  double u, v, big;
  if (ax >= ay && ax >= az) face = d.X() > 0 ? 0 : 1, big = ax, u = d.Y(), v = d.Z();
  else if (ay >= az) face = d.Y() > 0 ? 2 : 3, big = ay, u = d.X(), v = d.Z();
  else face = d.Z() > 0 ? 4 : 5, big = az, u = d.X(), v = d.Y();
  if (big <= 0) return -1;
  const int i = std::clamp(int((u / big + 1) / 2 * n), 0, n - 1), j = std::clamp(int((v / big + 1) / 2 * n), 0, n - 1);
  return (face * n + i) * n + j;
}

// How much of the mesh lies behind the plane (on the side away from its normal), by node count.
double behind(const FitMesh& m, const gp_Pnt& o, const gp_Dir& n) {
  const double tol = std::max(1e-6, m.diagonal * 0.01);
  size_t count = 0, total = 0;
  const size_t stride = std::max<size_t>(1, m.nodes.size() / 100000);
  for (size_t i = 0; i < m.nodes.size(); i += stride, ++total)
    if (gp_Vec(o, m.nodes[i]).Dot(gp_Vec(n)) <= tol) ++count;
  return total ? double(count) / double(total) : 0;
}

}  // namespace

ScanAxes scan_axes(const FitMesh& m, const std::function<bool()>& cancelled, double min_share) {
  auto check = [&] {
    if (cancelled && cancelled()) throw Error("cancelled");
  };
  const size_t n = size_t(m.triangles());
  ScanAxes out;
  const double total = std::max(m.total_area, 1e-300);
  // 1. Area-weighted normals binned on the sphere; the bins heavier than their neighbours start a mean shift.
  constexpr int R = 24;
  std::vector<double> bins(6 * R * R, 0);
  std::vector<gp_Vec> sums(6 * R * R, gp_Vec(0, 0, 0));
  for (size_t t = 0; t < n; ++t) {
    if (m.area[t] <= 0) continue;
    const int b = sphere_bin(m.smooth[t], R);
    if (b < 0) continue;
    bins[size_t(b)] += m.area[t];
    sums[size_t(b)] += m.smooth[t] * m.area[t];
  }
  check();
  std::vector<int> starts;
  for (int f = 0; f < 6; ++f)
    for (int i = 0; i < R; ++i)
      for (int j = 0; j < R; ++j) {
        const int b = (f * R + i) * R + j;
        if (bins[size_t(b)] < 0.004 * total) continue;
        bool peak = true;
        for (int di = -1; di <= 1 && peak; ++di)
          for (int dj = -1; dj <= 1 && peak; ++dj) {
            const int ii = i + di, jj = j + dj;
            if ((di || dj) && ii >= 0 && jj >= 0 && ii < R && jj < R && bins[size_t((f * R + ii) * R + jj)] > bins[size_t(b)]) peak = false;
          }
        if (peak) starts.push_back(b);
      }
  std::sort(starts.begin(), starts.end(), [&](int a, int b) { return bins[size_t(a)] > bins[size_t(b)]; });
  if (starts.size() > 16) starts.resize(16);
  struct Mode {
    gp_Dir d;
    double weight;
  };
  std::vector<Mode> modes;
  const double kernel = std::cos(rad(6)), reach = std::cos(rad(25));
  for (int b : starts) {
    check();
    if (sums[size_t(b)].Magnitude() <= 0) continue;
    gp_Dir d(sums[size_t(b)]);
    std::vector<int> near;
    for (size_t t = 0; t < n; ++t)
      if (m.area[t] > 0 && m.smooth[t].Dot(gp_Vec(d)) >= reach) near.push_back(int(t));
    double weight = 0;
    for (int it = 0; it < 20; ++it) {
      gp_Vec s(0, 0, 0);
      weight = 0;
      for (int t : near)
        if (m.smooth[size_t(t)].Dot(gp_Vec(d)) >= kernel) s += m.smooth[size_t(t)] * m.area[size_t(t)], weight += m.area[size_t(t)];
      if (s.Magnitude() <= 0) break;
      const gp_Dir next(s);
      const bool still = next.Angle(d) < rad(0.01);
      d = next;
      if (still) break;
    }
    bool merged = false;
    for (auto& mo : modes)
      if (mo.d.Angle(d) < rad(4)) {
        if (weight > mo.weight) mo = {d, weight};
        merged = true;
      }
    if (!merged && weight > 0) modes.push_back({d, weight});
  }
  std::sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) { return a.weight > b.weight; });
  // 2. Each direction's largest flat: the triangles facing that way, by their distance along it, the heaviest band fitted.
  const double strongest = modes.empty() ? 0 : modes.front().weight / total;
  if (strongest >= min_share) {
    out.method = "flats";
    for (const auto& mo : modes) {
      check();
      if (mo.weight / total < 0.01 || out.bases.size() >= 10) break;
      std::vector<int> facing;
      for (size_t t = 0; t < n; ++t)
        if (m.area[t] > 0 && m.smooth[t].Dot(gp_Vec(mo.d)) >= kernel) facing.push_back(int(t));
      if (facing.size() < 1) continue;
      const double step = std::max(m.diagonal / 500, 1e-6);
      std::unordered_map<long long, double> band;
      for (int t : facing) band[std::llround(gp_Vec(m.centre[size_t(t)].XYZ()).Dot(gp_Vec(mo.d)) / step)] += m.area[size_t(t)];
      long long peak = 0;
      double heaviest = -1;
      for (const auto& [k, a] : band) {
        double around = 0;  // the band and its neighbours: noise spreads one flat over a few
        for (long long d = -2; d <= 2; ++d)
          if (auto it = band.find(k + d); it != band.end()) around += it->second;
        if (around > heaviest || (around == heaviest && k < peak)) heaviest = around, peak = k;
      }
      std::vector<int> flat;
      for (int t : facing)
        if (std::llabs(std::llround(gp_Vec(m.centre[size_t(t)].XYZ()).Dot(gp_Vec(mo.d)) / step) - peak) <= 3) flat.push_back(t);
      if (flat.empty()) continue;
      ScanBase b;
      const Plane p = fit_plane_region(m, flat);
      b.normal = p.normal;
      b.origin = p.origin;
      b.q = p.q;
      b.share = heaviest / total;
      if (gp_Vec(b.origin, m.centroid).Dot(gp_Vec(b.normal)) > 0) b.normal.Reverse();  // the body lies behind its base
      bool same = false;
      for (const auto& o : out.bases)
        if (o.normal.Angle(b.normal) < rad(3) && std::fabs(gp_Vec(o.origin, b.origin).Dot(gp_Vec(o.normal))) < 3 * step) same = true;
      if (!same) out.bases.push_back(b);
    }
    // Bases the whole scan stands behind first (a plate's top and bottom, not a rib's top), then by area.
    std::vector<std::pair<double, ScanBase>> ranked;
    for (const auto& b : out.bases) ranked.push_back({behind(m, b.origin, b.normal) >= 0.97 ? 1.0 + b.share : b.share, b});
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    out.bases.clear();
    for (auto& [s, b] : ranked) out.bases.push_back(b);
  }
  if (out.bases.empty()) {
    // 3. No flats (an organic scan): the principal axes of the surface, the least spread one standing up first.
    out.method = "pca";
    std::vector<double> w(m.area.begin(), m.area.end());
    const Pca p = pca(m.centre, w, all_of(n));
    for (int k : {0, 1, 2})
      for (int s : {-1, 1}) {
        ScanBase b;
        gp_Dir d = canonical(p.axis[k]);
        if (s < 0) d.Reverse();
        b.normal = d;
        b.origin = p.mean;
        b.q.grade = "pca";
        b.q.total = b.q.used = int(n);
        b.q.extent = m.diagonal;
        out.bases.push_back(b);
      }
  }
  return out;
}

std::vector<gp_Dir> scan_turns(const FitMesh& m, const ScanAxes& axes, int base) {
  if (axes.bases.empty()) throw Error("the scan has no base to stand on");
  const ScanBase& b = axes.bases[size_t(((base % int(axes.bases.size())) + int(axes.bases.size())) % int(axes.bases.size()))];
  const gp_Dir n = b.normal, e1 = plane_x(n), e2(gp_Vec(n).Crossed(gp_Vec(e1)));
  const size_t count = size_t(m.triangles());
  std::vector<double> peaks;  // angles in the base plane (from e1 about n)
  if (axes.method == "flats") {
    // Walls: upright facets; their normals laid in the plane, histogram of the doubled angle (opposite walls agree).
    constexpr int B = 720;
    std::vector<double> h(B, 0);
    const double upright = std::sin(rad(12));
    double wall = 0;
    for (size_t t = 0; t < count; ++t) {
      const gp_Vec& s = m.smooth[t];
      if (m.area[t] <= 0 || std::fabs(s.Dot(gp_Vec(n))) > upright) continue;
      const double th = std::atan2(s.Dot(gp_Vec(e2)), s.Dot(gp_Vec(e1)));
      double phi = std::fmod(2 * th, 2 * kPi);
      if (phi < 0) phi += 2 * kPi;
      h[size_t(std::min(B - 1, int(phi / (2 * kPi) * B)))] += m.area[t];
      wall += m.area[t];
    }
    if (wall > 0.002 * m.total_area) {
      std::vector<double> g(B, 0);
      for (int i = 0; i < B; ++i)
        for (int d = -6; d <= 6; ++d) g[size_t(i)] += h[size_t((i + d + B) % B)] * std::exp(-d * d / 8.0);
      const double top = *std::max_element(g.begin(), g.end());
      std::vector<int> found;
      for (int i = 0; i < B; ++i)
        if (g[size_t(i)] >= 0.15 * top && g[size_t(i)] >= g[size_t((i + 1) % B)] && g[size_t(i)] > g[size_t((i + B - 1) % B)]) found.push_back(i);
      std::sort(found.begin(), found.end(), [&](int a, int c) { return g[size_t(a)] > g[size_t(c)]; });
      for (int i : found) {
        // The weighted circular mean of the doubled angles within 4 degrees of the bin, halved.
        const double centre = (i + 0.5) / B * 2 * kPi;
        double sx = 0, sy = 0;
        for (size_t t = 0; t < count; ++t) {
          const gp_Vec& s = m.smooth[t];
          if (m.area[t] <= 0 || std::fabs(s.Dot(gp_Vec(n))) > upright) continue;
          const double phi = 2 * std::atan2(s.Dot(gp_Vec(e2)), s.Dot(gp_Vec(e1)));
          const double diff = std::remainder(phi - centre, 2 * kPi);
          if (std::fabs(diff) > rad(8)) continue;
          sx += m.area[t] * std::cos(phi), sy += m.area[t] * std::sin(phi);
        }
        peaks.push_back((sx || sy ? std::atan2(sy, sx) : centre) / 2);
        if (peaks.size() >= 4) break;
      }
    }
  }
  if (peaks.empty()) {
    // No walls (a disc, a blob): the surface's longest direction in the plane.
    Eigen::Matrix2d c = Eigen::Matrix2d::Zero();
    Eigen::Vector2d mean = Eigen::Vector2d::Zero();
    double sw = 0;
    for (size_t t = 0; t < count; ++t) {
      const gp_Vec d(b.origin, m.centre[t]);
      mean += m.area[t] * Eigen::Vector2d(d.Dot(gp_Vec(e1)), d.Dot(gp_Vec(e2)));
      sw += m.area[t];
    }
    if (sw > 0) mean /= sw;
    for (size_t t = 0; t < count; ++t) {
      const gp_Vec d(b.origin, m.centre[t]);
      const Eigen::Vector2d q = Eigen::Vector2d(d.Dot(gp_Vec(e1)), d.Dot(gp_Vec(e2))) - mean;
      c += m.area[t] * q * q.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> es(c);
    const Eigen::Vector2d v = es.eigenvectors().col(1);
    peaks.push_back(std::atan2(v.y(), v.x()));
  }
  std::vector<double> angles;
  for (double p : peaks) {
    bool known = false;
    for (double a : angles)
      if (std::fabs(std::remainder(p - a, kPi / 2)) < rad(3)) known = true;
    if (known) continue;
    for (int k = 0; k < 4; ++k) angles.push_back(p + k * kPi / 2);
  }
  std::vector<gp_Dir> out;
  for (double a : angles) out.push_back(gp_Dir(gp_Vec(e1) * std::cos(a) + gp_Vec(e2) * std::sin(a)));
  return out;
}

ScanFrame scan_frame(const FitMesh& m, const ScanAxes& axes, int base, int turn, const std::string& origin) {
  if (axes.bases.empty()) throw Error("the scan has no base to stand on");
  const int nb = int(axes.bases.size());
  const ScanBase& b = axes.bases[size_t(((base % nb) + nb) % nb)];
  const std::vector<gp_Dir> turns = scan_turns(m, axes, base);
  const int nt = int(turns.size());
  ScanFrame f;
  f.normal = b.normal;
  f.x = turns[size_t(((turn % nt) + nt) % nt)];
  f.bases = nb;
  f.turns = nt;
  f.method = axes.method;
  f.q = b.q;
  f.share = b.share;
  // The box in the frame the scan will stand in: X along x, Z out of the base into the material, Y = Z x X.
  const gp_Vec X(f.x), Z = -gp_Vec(f.normal), Y = Z.Crossed(X);
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (const auto& p : m.nodes) {
    const gp_Vec d(b.origin, p);
    const double c[3] = {d.Dot(X), d.Dot(Y), d.Dot(Z)};
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
  }
  const bool centre = origin == "centre" || ((origin.empty() || origin == "auto") && axes.method == "pca");
  const double z = centre ? (lo[2] + hi[2]) / 2 : axes.method == "pca" ? lo[2] : 0.0;
  f.origin = b.origin.Translated(X * ((lo[0] + hi[0]) / 2) + Y * ((lo[1] + hi[1]) / 2) + Z * z);
  return f;
}

}  // namespace opad::scanfit
