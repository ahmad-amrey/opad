// Mesh to solid: the surfaces fitted to a mesh's regions (planes, cylinders, cones, spheres, tori) and their refinement.
#include <Eigen/Dense>
#include <Geom_ConicalSurface.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_SphericalSurface.hxx>
#include <Geom_ToroidalSurface.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cone.hxx>

#include <algorithm>
#include <cmath>

#include "meshsolid.hpp"

namespace opad::meshsolid {
namespace {

gp_Dir any_perpendicular(const gp_Dir& a) {
  const gp_Vec t = std::abs(a.X()) < 0.6 ? gp_Vec(1, 0, 0) : gp_Vec(0, 1, 0);
  return gp_Dir(gp_Vec(a).Crossed(t));
}

// Radial unit vector and the axial and radial coordinates of p about the axis (o, a).
void about_axis(const gp_Pnt& o, const gp_Dir& a, const gp_Pnt& p, double& h, double& rho, gp_Dir& e) {
  const gp_Vec v(o, p);
  h = v.Dot(gp_Vec(a));
  const gp_Vec radial = v - gp_Vec(a) * h;
  rho = radial.Magnitude();
  e = rho > 1e-12 ? gp_Dir(radial) : any_perpendicular(a);
}

Eigen::Matrix3d covariance(const std::vector<gp_Pnt>& points, gp_XYZ& mean) {
  mean = gp_XYZ(0, 0, 0);
  for (const auto& p : points) mean += p.XYZ();
  mean /= double(std::max<size_t>(1, points.size()));
  Eigen::Matrix3d m = Eigen::Matrix3d::Zero();
  for (const auto& p : points) {
    const gp_XYZ d = p.XYZ() - mean;
    const Eigen::Vector3d v(d.X(), d.Y(), d.Z());
    m += v * v.transpose();
  }
  return m;
}

// Every n-th point, so a fit over a big region stays quick.
std::vector<gp_Pnt> thin(const std::vector<gp_Pnt>& points, size_t most) {
  if (points.size() <= most) return points;
  std::vector<gp_Pnt> out;
  const double step = double(points.size()) / double(most);
  for (size_t i = 0; i < most; ++i) out.push_back(points[size_t(double(i) * step)]);
  return out;
}

}  // namespace

double Prim::distance(const gp_Pnt& p) const {
  switch (type) {
    case Plane: return gp_Vec(o, p).Dot(gp_Vec(a));
    case Sphere: return o.Distance(p) - r;
    default: break;
  }
  double h, rho;
  gp_Dir e;
  about_axis(o, a, p, h, rho, e);
  if (type == Cylinder) return rho - r;
  if (type == Cone) return rho * std::cos(angle) - h * std::sin(angle);
  return std::hypot(rho - r, h) - r2;  // torus
}

gp_Dir Prim::normal(const gp_Pnt& p) const {
  switch (type) {
    case Plane: return a;
    case Sphere: return p.Distance(o) > 1e-12 ? gp_Dir(gp_Vec(o, p)) : a;
    default: break;
  }
  double h, rho;
  gp_Dir e;
  about_axis(o, a, p, h, rho, e);
  if (type == Cylinder) return e;
  if (type == Cone) return gp_Dir(gp_Vec(e) * std::cos(angle) - gp_Vec(a) * std::sin(angle));
  const gp_Pnt c = o.Translated(gp_Vec(e) * r);
  const gp_Vec n(c, p);
  return n.Magnitude() > 1e-12 ? gp_Dir(n) : e;
}

gp_Pnt Prim::project(const gp_Pnt& p) const { return p.Translated(gp_Vec(normal(p)) * -distance(p)); }

Handle(Geom_Surface) Prim::surface(const gp_Pnt& near) const {
  const gp_Dir x = any_perpendicular(a);
  switch (type) {
    case Plane: return new Geom_Plane(gp_Ax3(project(near), a, x));
    case Sphere: return new Geom_SphericalSurface(gp_Ax3(o, a, x), r);
    case Torus: return new Geom_ToroidalSurface(gp_Ax3(o, a, x), r, r2);
    default: break;
  }
  const double h = gp_Vec(o, near).Dot(gp_Vec(a));
  const gp_Pnt at = o.Translated(gp_Vec(a) * h);
  if (type == Cylinder) return new Geom_CylindricalSurface(gp_Ax3(at, a, x), r);
  const double height = std::max(h, 1e-6);
  return new Geom_ConicalSurface(gp_Cone(gp_Ax3(o.Translated(gp_Vec(a) * height), a, x), angle, height * std::tan(angle)));
}

const char* Prim::name() const {
  switch (type) {
    case Plane: return "plane";
    case Cylinder: return "cylinder";
    case Cone: return "cone";
    case Sphere: return "sphere";
    case Torus: return "torus";
  }
  return "plane";
}

bool Prim::same(const Prim& other, double tolerance, double size) const {
  if (type != other.type) return false;
  const double turn = tolerance / std::max(size, 1e-9);
  auto parallel = [&](const gp_Dir& x, const gp_Dir& y) { return std::abs(x.Dot(y)) > std::cos(std::max(turn, 1e-6)); };
  auto onAxis = [&](const gp_Pnt& p) { return gp_Vec(o, p).Crossed(gp_Vec(a)).Magnitude() <= tolerance; };
  switch (type) {
    case Plane: return parallel(a, other.a) && std::abs(distance(other.o)) <= tolerance;
    case Sphere: return o.Distance(other.o) <= tolerance && std::abs(r - other.r) <= tolerance;
    case Cylinder: return parallel(a, other.a) && onAxis(other.o) && std::abs(r - other.r) <= tolerance;
    case Cone: return a.Dot(other.a) > std::cos(std::max(turn, 1e-6)) && o.Distance(other.o) <= tolerance * 4 && std::abs(angle - other.angle) <= std::max(turn, 1e-6);
    case Torus:
      return parallel(a, other.a) && o.Distance(other.o) <= tolerance && std::abs(r - other.r) <= tolerance && std::abs(r2 - other.r2) <= tolerance;
  }
  return false;
}

double max_distance(const Prim& p, const std::vector<gp_Pnt>& points) {
  double worst = 0;
  for (const auto& q : points) worst = std::max(worst, std::abs(p.distance(q)));
  return worst;
}

bool fit_circle_2d(const std::vector<std::array<double, 2>>& pts, double& cx, double& cy, double& r) {
  if (pts.size() < 3) return false;
  // Kasa: x^2 + y^2 + D x + E y + F = 0, centred on the mean for conditioning.
  double mx = 0, my = 0;
  for (const auto& p : pts) {
    mx += p[0];
    my += p[1];
  }
  mx /= double(pts.size());
  my /= double(pts.size());
  Eigen::MatrixXd A(pts.size(), 3);
  Eigen::VectorXd b(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) {
    const double x = pts[i][0] - mx, y = pts[i][1] - my;
    A(Eigen::Index(i), 0) = x;
    A(Eigen::Index(i), 1) = y;
    A(Eigen::Index(i), 2) = 1;
    b(Eigen::Index(i)) = -(x * x + y * y);
  }
  const Eigen::Vector3d s = A.colPivHouseholderQr().solve(b);
  cx = -s(0) / 2;
  cy = -s(1) / 2;
  const double rr = cx * cx + cy * cy - s(2);
  if (!(rr > 0) || !std::isfinite(rr)) return false;
  r = std::sqrt(rr);
  // Geometric refinement (Gauss-Newton on |p - c| - r).
  for (int it = 0; it < 20; ++it) {
    Eigen::Matrix3d JtJ = Eigen::Matrix3d::Zero();
    Eigen::Vector3d Jtr = Eigen::Vector3d::Zero();
    for (const auto& p : pts) {
      const double dx = p[0] - mx - cx, dy = p[1] - my - cy, d = std::hypot(dx, dy);
      if (d < 1e-15) continue;
      const Eigen::Vector3d J(-dx / d, -dy / d, -1);
      JtJ += J * J.transpose();
      Jtr += J * (d - r);
    }
    const Eigen::Vector3d step = JtJ.ldlt().solve(-Jtr);
    if (!step.allFinite()) break;
    cx += step(0);
    cy += step(1);
    r += step(2);
    if (step.norm() < 1e-12 * std::max(1.0, r)) break;
  }
  cx += mx;
  cy += my;
  return r > 0 && std::isfinite(r) && std::isfinite(cx) && std::isfinite(cy);
}

std::optional<Prim> fit_plane(const std::vector<gp_Pnt>& points) {
  if (points.size() < 3) return std::nullopt;
  gp_XYZ mean;
  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(covariance(points, mean));
  const Eigen::Vector3d n = eig.eigenvectors().col(0);
  if (!n.allFinite() || n.norm() < 1e-12) return std::nullopt;
  Prim p;
  p.type = Prim::Plane;
  p.o = gp_Pnt(mean);
  p.a = gp_Dir(n(0), n(1), n(2));
  return p;
}

std::optional<Prim> fit_sphere(const std::vector<gp_Pnt>& points, double size) {
  if (points.size() < 4) return std::nullopt;
  gp_XYZ mean(0, 0, 0);
  for (const auto& p : points) mean += p.XYZ();
  mean /= double(points.size());
  Eigen::MatrixXd A(points.size(), 4);
  Eigen::VectorXd b(points.size());
  for (size_t i = 0; i < points.size(); ++i) {
    const gp_XYZ q = points[i].XYZ() - mean;
    A.row(Eigen::Index(i)) << q.X(), q.Y(), q.Z(), 1;
    b(Eigen::Index(i)) = -q.SquareModulus();
  }
  const Eigen::Vector4d s = A.colPivHouseholderQr().solve(b);
  const gp_XYZ c(-s(0) / 2, -s(1) / 2, -s(2) / 2);
  const double rr = c.SquareModulus() - s(3);
  if (!(rr > 0) || !std::isfinite(rr)) return std::nullopt;
  Prim p;
  p.type = Prim::Sphere;
  p.o = gp_Pnt(c + mean);
  p.r = std::sqrt(rr);
  p.a = gp_Dir(0, 0, 1);
  if (p.r > size * 50) return std::nullopt;
  return refine(p, points, size);
}

std::optional<Prim> fit_cylinder(const std::vector<gp_Pnt>& points, const std::vector<gp_Vec>& normals, double size) {
  if (points.size() < 5 || normals.size() < 2) return std::nullopt;
  Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
  for (const auto& n : normals) {
    const Eigen::Vector3d v(n.X(), n.Y(), n.Z());
    M += v * v.transpose();
  }
  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(M);
  // The normals must spread in a plane (two eigenvalues) and not along the axis (the smallest).
  if (eig.eigenvalues()(1) < 1e-10 * eig.eigenvalues()(2)) return std::nullopt;
  const Eigen::Vector3d ax = eig.eigenvectors().col(0);
  const gp_Dir a(ax(0), ax(1), ax(2));
  const gp_Dir u = any_perpendicular(a), w = a.Crossed(u);
  std::vector<std::array<double, 2>> flat;
  for (const auto& p : points) flat.push_back({gp_Vec(p.XYZ()).Dot(gp_Vec(u)), gp_Vec(p.XYZ()).Dot(gp_Vec(w))});
  double cx, cy, r;
  if (!fit_circle_2d(flat, cx, cy, r) || r > size * 50) return std::nullopt;
  Prim p;
  p.type = Prim::Cylinder;
  p.a = a;
  p.r = r;
  p.o = gp_Pnt(gp_Vec(u).XYZ() * cx + gp_Vec(w).XYZ() * cy);
  // The axis point nearest the points' middle (only the line matters).
  gp_XYZ mean(0, 0, 0);
  for (const auto& q : points) mean += q.XYZ();
  mean /= double(points.size());
  p.o.Translate(gp_Vec(a) * gp_Vec(p.o, gp_Pnt(mean)).Dot(gp_Vec(a)));
  return refine(p, points, size);
}

std::optional<Prim> fit_cone(const std::vector<gp_Pnt>& points, const std::vector<gp_Vec>& normals, const std::vector<gp_Pnt>& at, double size) {
  if (points.size() < 6 || normals.size() < 3 || normals.size() != at.size()) return std::nullopt;
  // A cone's normals make one angle with its axis: they end on a plane (a circle on the unit sphere).
  std::vector<gp_Pnt> tips;
  for (const auto& n : normals) tips.emplace_back(n.XYZ());
  gp_XYZ mean;
  const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(covariance(tips, mean));
  if (eig.eigenvalues()(1) < 1e-10) return std::nullopt;
  const Eigen::Vector3d ax = eig.eigenvectors().col(0);
  gp_Dir a(ax(0), ax(1), ax(2));
  const double c = gp_Vec(mean).Dot(gp_Vec(a));
  if (std::abs(c) < 0.02 || std::abs(c) > 0.995) return std::nullopt;  // a cylinder, or nearly a plane
  // The apex is on every tangent plane: n . x = n . p.
  Eigen::Matrix3d A = Eigen::Matrix3d::Zero();
  Eigen::Vector3d b = Eigen::Vector3d::Zero();
  for (size_t i = 0; i < normals.size(); ++i) {
    const Eigen::Vector3d n(normals[i].X(), normals[i].Y(), normals[i].Z());
    A += n * n.transpose();
    b += n * (normals[i].Dot(gp_Vec(at[i].XYZ())));
  }
  const Eigen::Vector3d x = A.ldlt().solve(b);
  if (!x.allFinite()) return std::nullopt;
  Prim p;
  p.type = Prim::Cone;
  p.o = gp_Pnt(x(0), x(1), x(2));
  double side = 0;
  for (const auto& q : points) side += gp_Vec(p.o, q).Dot(gp_Vec(a));
  if (side < 0) a.Reverse();
  p.a = a;
  p.angle = std::asin(std::min(0.999, std::abs(c)));
  if (p.o.Distance(gp_Pnt(mean)) > size * 50) return std::nullopt;
  return refine(p, points, size);
}

std::optional<Prim> fit_torus(const std::vector<gp_Pnt>& points, const std::vector<gp_Vec>& normals, const std::vector<gp_Pnt>& at, double size) {
  if (points.size() < 8 || normals.size() < 6 || normals.size() != at.size()) return std::nullopt;
  // Points moved in along their normal by the tube radius lie on the tube's middle circle: scan the radius (either side).
  struct Try {
    double residual = 1e300, r2 = 0;
    gp_Pnt centre;
    gp_Dir axis;
    double R = 0;
  };
  auto attempt = [&](double r2) {
    Try t;
    t.r2 = r2;
    std::vector<gp_Pnt> q;
    for (size_t i = 0; i < at.size(); ++i) q.push_back(at[i].Translated(normals[i] * -r2));
    const auto plane = fit_plane(q);
    if (!plane) return t;
    const gp_Dir u = any_perpendicular(plane->a), w = plane->a.Crossed(u);
    std::vector<std::array<double, 2>> flat;
    for (const auto& p : q) flat.push_back({gp_Vec(plane->o, p).Dot(gp_Vec(u)), gp_Vec(plane->o, p).Dot(gp_Vec(w))});
    double cx, cy, R;
    if (!fit_circle_2d(flat, cx, cy, R)) return t;
    double s = 0;
    for (size_t i = 0; i < q.size(); ++i) {
      const double off = gp_Vec(plane->o, q[i]).Dot(gp_Vec(plane->a));
      const double radial = std::hypot(flat[i][0] - cx, flat[i][1] - cy) - R;
      s += off * off + radial * radial;
    }
    t.residual = std::sqrt(s / double(q.size()));
    t.centre = plane->o.Translated(gp_Vec(u) * cx + gp_Vec(w) * cy);
    t.axis = plane->a;
    t.R = R;
    return t;
  };
  Try best;
  for (int sign : {1, -1})
    for (int i = 0; i <= 48; ++i) {
      const double r2 = sign * size * std::pow(10.0, -3.0 + 3.0 * i / 48);
      const Try t = attempt(r2);
      if (t.residual < best.residual) best = t;
    }
  if (best.residual > 1e299) return std::nullopt;
  // Golden section about the best of the scan.
  double lo = best.r2 / std::pow(10.0, 3.0 / 48), hi = best.r2 * std::pow(10.0, 3.0 / 48);
  for (int it = 0; it < 40; ++it) {
    const double m1 = lo + (hi - lo) * 0.382, m2 = lo + (hi - lo) * 0.618;
    if (attempt(m1).residual < attempt(m2).residual) hi = m2;
    else lo = m1;
  }
  const Try t = attempt((lo + hi) / 2);
  if (t.residual < best.residual) best = t;
  // A concave tube (a fillet in an inside corner) was moved outward: its radius is still positive.
  if (best.R < size * 1e-4 || best.R > size * 50 || std::abs(best.r2) >= best.R) return std::nullopt;
  Prim p;
  p.type = Prim::Torus;
  p.o = best.centre;
  p.a = best.axis;
  p.r = best.R;
  p.r2 = std::abs(best.r2);
  return refine(p, points, size);
}

Prim refine(const Prim& start, const std::vector<gp_Pnt>& all, double size) {
  if (start.type == Prim::Plane) {
    auto p = fit_plane(all);
    if (!p) return start;
    if (p->a.Dot(start.a) < 0) p->a.Reverse();
    return *p;
  }
  const std::vector<gp_Pnt> points = thin(all, 1500);
  // Parameters: o (3), a (3), then the radii / angle of the kind.
  auto pack = [](const Prim& p) {
    std::vector<double> x{p.o.X(), p.o.Y(), p.o.Z()};
    if (p.type != Prim::Sphere) x.insert(x.end(), {p.a.X(), p.a.Y(), p.a.Z()});
    if (p.type == Prim::Cone) x.push_back(p.angle);
    else x.push_back(p.r);
    if (p.type == Prim::Torus) x.push_back(p.r2);
    return x;
  };
  auto unpack = [&](const std::vector<double>& x) {
    Prim p = start;
    p.o = gp_Pnt(x[0], x[1], x[2]);
    size_t i = 3;
    if (p.type != Prim::Sphere) {
      const gp_Vec v(x[3], x[4], x[5]);
      if (v.Magnitude() > 1e-12) p.a = gp_Dir(v);
      i = 6;
    }
    if (p.type == Prim::Cone) p.angle = x[i];
    else p.r = x[i];
    if (p.type == Prim::Torus) p.r2 = x[i + 1];
    return p;
  };
  auto cost = [&](const Prim& p, Eigen::VectorXd* res) {
    double s = 0;
    if (res) res->resize(Eigen::Index(points.size()));
    for (size_t i = 0; i < points.size(); ++i) {
      const double d = p.distance(points[i]);
      if (res) (*res)(Eigen::Index(i)) = d;
      s += d * d;
    }
    return s;
  };
  std::vector<double> x = pack(start);
  const size_t n = x.size();
  Prim current = start;
  Eigen::VectorXd r;
  double c = cost(current, &r);
  double lambda = 1e-3;
  for (int it = 0; it < 30; ++it) {
    Eigen::MatrixXd J(Eigen::Index(points.size()), Eigen::Index(n));
    for (size_t k = 0; k < n; ++k) {
      const double h = k >= 3 && k < 6 && current.type != Prim::Sphere ? 1e-7 : (current.type == Prim::Cone && k == n - 1 ? 1e-7 : 1e-7 * std::max(1.0, size));
      std::vector<double> xp = x, xm = x;
      xp[k] += h;
      xm[k] -= h;
      const Prim pp = unpack(xp), pm = unpack(xm);
      for (size_t i = 0; i < points.size(); ++i) J(Eigen::Index(i), Eigen::Index(k)) = (pp.distance(points[i]) - pm.distance(points[i])) / (2 * h);
    }
    const Eigen::MatrixXd JtJ = J.transpose() * J;
    const Eigen::VectorXd g = J.transpose() * r;
    bool improved = false;
    for (int tries = 0; tries < 8 && !improved; ++tries) {
      Eigen::MatrixXd A = JtJ;
      const double scale = JtJ.diagonal().maxCoeff();
      for (Eigen::Index k = 0; k < A.rows(); ++k) A(k, k) += lambda * (JtJ(k, k) + 1e-12 * scale + 1e-30);
      const Eigen::VectorXd step = A.ldlt().solve(-g);
      if (!step.allFinite()) break;
      std::vector<double> xn = x;
      for (size_t k = 0; k < n; ++k) xn[k] += step(Eigen::Index(k));
      Prim candidate = unpack(xn);
      if ((candidate.type == Prim::Cone && (candidate.angle <= 1e-4 || candidate.angle >= M_PI / 2 - 1e-4)) ||
          (candidate.type != Prim::Cone && candidate.r <= 0) || (candidate.type == Prim::Torus && candidate.r2 <= 0)) {
        lambda *= 4;
        continue;
      }
      Eigen::VectorXd rn;
      const double cn = cost(candidate, &rn);
      if (cn < c) {
        current = candidate;
        x = pack(current);  // the axis renormalised
        r = rn;
        const double gain = c - cn;
        c = cn;
        lambda = std::max(1e-9, lambda / 3);
        improved = true;
        if (gain < 1e-14 * std::max(1.0, c)) it = 1000;
      } else {
        lambda *= 4;
      }
    }
    if (!improved) break;
  }
  return current;
}

}  // namespace opad::meshsolid
