// Point-pair registration (opad/point_pairs.hpp): Kabsch / Umeyama without scaling, and one stray pair left out.
#include "opad/point_pairs.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <string>

#include "opad/util.hpp"

namespace opad::pairs {
namespace {

using Eigen::Matrix3d;
using Eigen::Vector3d;

Vector3d v(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }

struct Rigid {
  Matrix3d r = Matrix3d::Identity();
  Vector3d t = Vector3d::Zero();
  Vector3d apply(const gp_Pnt& p) const { return r * v(p) + t; }
};

// How the points spread about their centre: the square roots of the scatter's eigenvalues, largest first.
Vector3d spread(const std::vector<gp_Pnt>& pts, const std::vector<int>& use) {
  Vector3d c = Vector3d::Zero();
  for (int i : use) c += v(pts[size_t(i)]);
  c /= double(use.size());
  Matrix3d s = Matrix3d::Zero();
  for (int i : use) {
    const Vector3d d = v(pts[size_t(i)]) - c;
    s += d * d.transpose();
  }
  const Eigen::SelfAdjointEigenSolver<Matrix3d> eig(s / double(use.size()));
  Vector3d out;
  for (int k = 0; k < 3; ++k) out[k] = std::sqrt(std::max(0.0, eig.eigenvalues()[2 - k]));
  return out;
}

// Why these points cannot fix a turn, or empty: all at one place, or on one line (the spread across it under a thousandth
// of the spread along it: noise would choose the turn about it).
std::string degenerate(const std::vector<gp_Pnt>& pts, const std::vector<int>& use) {
  const Vector3d s = spread(pts, use);
  if (s[0] < 1e-9) return "at one place";
  if (s[1] < 1e-3 * s[0]) return "on one line";
  return {};
}

Rigid solve(const std::vector<gp_Pnt>& a, const std::vector<gp_Pnt>& b, const std::vector<int>& use) {
  Vector3d ca = Vector3d::Zero(), cb = Vector3d::Zero();
  for (int i : use) {
    ca += v(a[size_t(i)]);
    cb += v(b[size_t(i)]);
  }
  ca /= double(use.size());
  cb /= double(use.size());
  Matrix3d h = Matrix3d::Zero();
  for (int i : use) h += (v(a[size_t(i)]) - ca) * (v(b[size_t(i)]) - cb).transpose();
  const Eigen::JacobiSVD<Matrix3d> svd(h, Eigen::ComputeFullU | Eigen::ComputeFullV);
  const Matrix3d u = svd.matrixU(), w = svd.matrixV();
  Matrix3d d = Matrix3d::Identity();
  d(2, 2) = (w * u.transpose()).determinant() < 0 ? -1.0 : 1.0;  // a turn, never a mirror (coplanar points allow both)
  Rigid out;
  out.r = w * d * u.transpose();
  out.t = cb - out.r * ca;
  return out;
}

double residual(const Rigid& m, const gp_Pnt& a, const gp_Pnt& b) { return (m.apply(a) - v(b)).norm(); }

}  // namespace

PairFit fit_pairs(const std::vector<gp_Pnt>& from, const std::vector<gp_Pnt>& to, bool drop_outlier) {
  const int n = int(from.size());
  if (from.size() != to.size())
    throw Error("pick one target point for each point on the bodies: " + std::to_string(from.size()) + " on the bodies, " + std::to_string(to.size()) + " on the target");
  if (n < 3)
    throw Error("three pairs of points or more fix the move (" + std::to_string(n) + (n == 1 ? " pair" : " pairs") +
                " so far): with fewer the bodies could still turn about the line through them");
  std::vector<int> all(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) all[size_t(i)] = i;
  for (const auto& [pts, side] : {std::pair{&from, "the points on the bodies"}, std::pair{&to, "the target points"}})
    if (const std::string why = degenerate(*pts, all); !why.empty())
      throw Error(std::string(side) + " are all " + why +
                  (why == "at one place" ? ": pick pairs spread over the part" : ": the move could still turn about it; pick a pair off that line"));

  std::vector<int> used = all;
  int dropped = -1;
  if (drop_outlier && n >= 4) {
    // Each pair judged against a fit made without it (a studentized deleted residual): its error d, against the scatter the
    // others leave (sigma^2 = their squared residuals over 3k - 6 degrees of freedom) widened by how poorly they pin the
    // move down out where it is (the leverage H of the move's six parameters there): T = d' (I + H)^-1 d / 3 sigma^2 goes
    // as F(3, 3k - 6). Over its 0.1% point: left out (the worst one only).
    const double floor = 1e-6 * spread(from, all)[0] + 1e-12;
    double worst = 1;
    for (int i = 0; i < n; ++i) {
      std::vector<int> rest;
      for (int j : all)
        if (j != i) rest.push_back(j);
      if (!degenerate(from, rest).empty() || !degenerate(to, rest).empty()) continue;
      const Rigid m = solve(from, to, rest);
      Vector3d c = Vector3d::Zero();
      for (int j : rest) c += m.apply(from[size_t(j)]);
      c /= double(rest.size());
      auto jacobian = [&](int j) {  // d(moved point) / d(small turn about c, shift)
        const Vector3d q = m.apply(from[size_t(j)]) - c;
        Eigen::Matrix<double, 3, 6> out;
        out << 0, q.z(), -q.y(), 1, 0, 0, -q.z(), 0, q.x(), 0, 1, 0, q.y(), -q.x(), 0, 0, 0, 1;
        return out;
      };
      Eigen::Matrix<double, 6, 6> normal = Eigen::Matrix<double, 6, 6>::Zero();
      double sum = 0;
      for (int j : rest) {
        const auto jj = jacobian(j);
        normal += jj.transpose() * jj;
        sum += (m.apply(from[size_t(j)]) - v(to[size_t(j)])).squaredNorm();
      }
      const int dof = 3 * int(rest.size()) - 6;
      const double sigma = std::max(std::sqrt(sum / double(dof)), floor);
      const auto ji = jacobian(i);
      const Matrix3d spreadout = Matrix3d::Identity() + ji * normal.ldlt().solve(ji.transpose());
      const Vector3d d = m.apply(from[size_t(i)]) - v(to[size_t(i)]);
      const double t = d.dot(spreadout.ldlt().solve(d)) / (3 * sigma * sigma);
      // F(3, dof) at 0.999 (dof = 3, 6, 9, ...), towards 5.42 for many pairs.
      static const double crit[] = {141.1, 23.70, 13.90, 10.80, 9.34, 8.49, 7.94, 7.55, 7.27, 7.05};
      const int at = dof / 3 - 1;
      const double limit = at < 10 ? crit[at] : 5.42 + (7.05 - 5.42) * 30.0 / double(dof);
      if (t / limit > worst) worst = t / limit, dropped = i;
    }
    if (dropped >= 0) used.erase(std::remove(used.begin(), used.end(), dropped), used.end());
  }

  const Rigid m = solve(from, to, used);
  PairFit out;
  out.dropped = dropped;
  double sum = 0;
  for (int i = 0; i < n; ++i) {
    const double r = residual(m, from[size_t(i)], to[size_t(i)]);
    out.residuals.push_back(r);
    if (i == dropped) continue;
    sum += r * r;
    out.max = std::max(out.max, r);
  }
  out.rms = std::sqrt(sum / double(used.size()));
  out.motion.SetValues(m.r(0, 0), m.r(0, 1), m.r(0, 2), m.t[0], m.r(1, 0), m.r(1, 1), m.r(1, 2), m.t[1], m.r(2, 0), m.r(2, 1), m.r(2, 2), m.t[2]);
  return out;
}

}  // namespace opad::pairs
