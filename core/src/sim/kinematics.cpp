#include "opad/sim/kinematics.hpp"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <numeric>
#include <set>

#include "opad/sim/joints.hpp"

namespace opad::sim {

namespace {

using V3 = Eigen::Vector3d;
using M3 = Eigen::Matrix3d;
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct Rigid {
  M3 R = M3::Identity();
  V3 p = V3::Zero();
  Rigid operator*(const Rigid& o) const { return {R * o.R, R * o.p + p}; }
  Rigid inverse() const { return {R.transpose(), -(R.transpose() * p)}; }
};

Rigid rigid_of(const Mat4& m) {
  Rigid r;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) r.R(i, j) = m.at(i, j);
    r.p(i) = m.at(i, 3);
  }
  // Re-square the rotation (placements written as text lose the last digits): nearest rotation by SVD.
  Eigen::JacobiSVD<M3> svd(r.R, Eigen::ComputeFullU | Eigen::ComputeFullV);
  M3 q = svd.matrixU() * svd.matrixV().transpose();
  if (q.determinant() < 0) throw Error("a part is mirrored: joints need parts placed without mirroring or scaling");
  const double scale = svd.singularValues()(0);
  if (std::fabs(scale - 1) > 1e-6 || std::fabs(svd.singularValues()(2) - 1) > 1e-6)
    throw Error("a part is scaled: joints need parts placed without scaling");
  r.R = q;
  return r;
}

Mat4 mat_of(const Rigid& r) {
  Mat4 m;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m.at(i, j) = r.R(i, j);
    m.at(i, 3) = r.p(i);
  }
  return m;
}

Rigid rigid_of(const Frame& f) {
  Rigid r;
  const V3 x(f.x[0], f.x[1], f.x[2]);
  const V3 y(f.y[0], f.y[1], f.y[2]);
  r.R.col(0) = x.normalized();
  r.R.col(2) = x.cross(y).normalized();
  r.R.col(1) = r.R.col(2).cross(r.R.col(0));
  r.p = V3(f.origin[0], f.origin[1], f.origin[2]);
  return r;
}

Frame frame_of(const Rigid& r) {
  Frame f;
  f.origin = {r.p(0), r.p(1), r.p(2)};
  f.x = {r.R(0, 0), r.R(1, 0), r.R(2, 0)};
  f.y = {r.R(0, 1), r.R(1, 1), r.R(2, 1)};
  return f;
}

M3 rot_exp(const V3& w) {
  const double t = w.norm();
  if (t < 1e-15) return M3::Identity();
  return Eigen::AngleAxisd(t, w / t).toRotationMatrix();
}

double wrap(double a) {
  a = std::fmod(a + kPi, 2 * kPi);
  if (a < 0) a += 2 * kPi;
  return a - kPi;
}

bool same(const Rigid& a, const Rigid& b, double tol = 1e-9) {
  return (a.p - b.p).norm() < tol && (a.R - b.R).norm() < tol;
}

}  // namespace

struct Mechanism::Impl {
  struct Part {
    std::string node;
    Rigid T, T0;
    bool ground = false;
    int depth = 0;
    int carrier = -1;  // the nearest part above it in the tree (moves it along)
  };
  struct Jt {
    std::string id, name, kind;
    const JointKind* k = nullptr;
    int a = -1, b = -1;  // part indices, -1 = the world
    Rigid fa, fb;
    std::vector<double> q, ref;                       // coordinates now and the unwrap reference (rad, mm)
    std::vector<std::pair<double, double>> limits;    // NaN = none
    bool locked = false;
    double pitch = 0;  // screw: mm per turn
    // relations
    int j1 = -1, j2 = -1, c1 = -1, c2 = -1;
    double ratio = 0, q1c = 0, q2c = 0;
    // A relation measured against a carrier (planet gears): each joint's part frame against where it sat in the carrier.
    int carrier = -1;
    Rigid fc1, fc2;
    std::array<double, 2> cref{0, 0};
  };
  // A block of equations and the parts they depend on.
  struct Group {
    std::vector<int> parts;
    int rows = 0;
    std::function<void(double*)> f;
    int joint = -1;  // what it comes from (for messages)
  };

  std::vector<Part> parts;
  std::map<std::string, int> part_at;
  std::vector<std::string> part_ids;
  std::vector<Jt> joints;
  std::map<std::string, int> joint_at;
  std::vector<std::string> problems;
  double L = 100;

  Rigid placed(int part) const { return part < 0 ? Rigid() : parts[size_t(part)].T; }
  Rigid frame_a(const Jt& j) const { return placed(j.a) * j.fa; }
  Rigid frame_b(const Jt& j) const { return placed(j.b) * j.fb; }

  // The joint's coordinates measured now, angles unwrapped about j.ref.
  std::vector<double> measure(const Jt& j) const {
    std::vector<double> out;
    if (!j.k || j.k->relation) return out;
    const Rigid A = frame_a(j), B = frame_b(j);
    const V3 d = B.p - A.p;
    const V3 ax = A.R.col(0), ay = A.R.col(1), az = A.R.col(2), bx = B.R.col(0);
    for (size_t i = 0; i < j.k->coords.size(); ++i) {
      const Coord& c = j.k->coords[i];
      double v = 0;
      if (c.name == "rotation") {
        const double m = std::atan2(bx.dot(ay), bx.dot(ax));
        const double r = i < j.ref.size() ? j.ref[i] : 0.0;
        v = r + wrap(m - r);
      } else if (c.name == "translation") {
        v = j.kind == "pin_slot" ? d.dot(ax) : d.dot(az);
      } else if (c.name == "x") {
        v = d.dot(ax);
      } else if (c.name == "y") {
        v = d.dot(ay);
      }
      out.push_back(v);
    }
    return out;
  }

  // The coordinate a relation reads of its joint `side` (0, 1): the joint's own, or with a carrier the joint's part frame
  // against where it sat in the carrier (a rotation about its z, a translation along it).
  double relation_coord(const Jt& j, int side) const {
    const Jt& jt = joints[size_t(side == 0 ? j.j1 : j.j2)];
    const int c = side == 0 ? j.c1 : j.c2;
    if (j.carrier < 0) return measure(jt)[size_t(c)];
    const Rigid P = placed(jt.b) * jt.fb;
    const Rigid C = placed(j.carrier) * (side == 0 ? j.fc1 : j.fc2);
    const Rigid R = C.inverse() * P;
    if (!jt.k->coords[size_t(c)].angle) return R.p(2);
    const double m = std::atan2(R.R(1, 0), R.R(0, 0));
    const double r = j.cref[size_t(side)];
    return r + wrap(m - r);
  }

  // The scale an angle residual is weighed at, so a radian counts as much as the mechanism's size in mm.
  double weight(const Coord& c) const { return c.angle ? L : 1.0; }

  // The joint's own equations (relative position the kind does not allow), written into out.
  int constraint_rows(const Jt& j) const {
    if (j.kind == "rigid" || j.kind == "ground") return 6;
    if (j.kind == "screw") return 5;
    return j.k ? j.k->constraints : 0;
  }
  void constraints(const Jt& j, double* out) const {
    const Rigid A = frame_a(j), B = frame_b(j);
    const V3 d = B.p - A.p;
    const V3 ax = A.R.col(0), ay = A.R.col(1), az = A.R.col(2);
    const V3 bx = B.R.col(0), by = B.R.col(1);
    int n = 0;
    auto put = [&](double v) { out[n++] = v; };
    const std::string& k = j.kind;
    if (k == "rigid" || k == "ground") {
      put(d(0)), put(d(1)), put(d(2));
      put(L * bx.dot(ay)), put(L * bx.dot(az)), put(L * by.dot(az));
    } else if (k == "revolute") {
      put(d(0)), put(d(1)), put(d(2));
      put(L * az.dot(bx)), put(L * az.dot(by));
    } else if (k == "slider") {
      put(d.dot(ax)), put(d.dot(ay));
      put(L * az.dot(bx)), put(L * az.dot(by)), put(L * ay.dot(bx));
    } else if (k == "cylindrical") {
      put(d.dot(ax)), put(d.dot(ay));
      put(L * az.dot(bx)), put(L * az.dot(by));
    } else if (k == "pin_slot") {
      put(d.dot(ay)), put(d.dot(az));
      put(L * az.dot(bx)), put(L * az.dot(by));
    } else if (k == "planar") {
      put(d.dot(az));
      put(L * az.dot(bx)), put(L * az.dot(by));
    } else if (k == "ball") {
      put(d(0)), put(d(1)), put(d(2));
    } else if (k == "screw") {
      put(d.dot(ax)), put(d.dot(ay));
      put(L * az.dot(bx)), put(L * az.dot(by));
      const double theta = measure(j)[0];
      put(d.dot(az) - j.pitch * theta / (2 * kPi));
    }
  }

  std::vector<int> parts_of(const Jt& j) const {
    std::vector<int> out;
    auto add = [&](int p) {
      if (p >= 0 && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };
    if (j.j1 >= 0) {
      for (int jj : {j.j1, j.j2}) add(joints[size_t(jj)].a), add(joints[size_t(jj)].b);
      add(j.carrier);
    } else {
      add(j.a), add(j.b);
    }
    return out;
  }

  // ---------------------------------------------------------------- building
  void build(const Scene& scene) {
    // Parts: every node a resolved joint names.
    auto part_index = [&](const std::string& node) {
      if (node.empty()) return -1;
      if (auto it = part_at.find(node); it != part_at.end()) return it->second;
      Part p;
      p.node = node;
      p.T = p.T0 = rigid_of(scene.world(node));
      p.depth = int(scene.path_to(node).size());
      parts.push_back(p);
      part_ids.push_back(node);
      return part_at[node] = int(parts.size() - 1);
    };
    for (const Joint& src : scene.joints) {
      const JointKind* k = joint_kind(src.kind);
      if (!src.error.empty() || !k) {
        problems.push_back(src.name + ": " + (src.error.empty() ? "unknown kind" : src.error));
        continue;
      }
      Jt j;
      j.id = src.id;
      j.name = src.name;
      j.kind = src.kind;
      j.k = k;
      if (k->relation) {
        joints.push_back(j);  // linked below, once every joint is in
        joint_at[j.id] = int(joints.size() - 1);
        continue;
      }
      try {
        j.a = src.kind == "ground" ? -1 : part_index(src.base);
        j.b = part_index(src.part);
      } catch (const std::exception& e) {
        problems.push_back(src.name + ": " + e.what());
        continue;
      }
      j.fa = rigid_of(src.at_base);
      j.fb = rigid_of(src.at_part);
      j.locked = src.def.value("locked", false);
      j.pitch = src.def.value("pitch", 0.0);
      for (size_t i = 0; i < k->coords.size(); ++i) {
        const double unit = k->coords[i].angle ? kDeg : 1.0;
        j.ref.push_back(i < src.values.size() ? src.values[i] * unit : 0.0);
        std::pair<double, double> lim{NAN, NAN};
        if (const auto l = src.def.find("limits"); l != src.def.end() && l->is_object())
          if (const auto c = l->find(k->coords[i].name); c != l->end() && c->is_array() && c->size() == 2)
            lim = {(*c)[0].get<double>() * unit, (*c)[1].get<double>() * unit};
        j.limits.push_back(lim);
      }
      if ((src.kind == "ground" || src.kind == "rigid") && j.a < 0) parts[size_t(j.b)].ground = true;
      joints.push_back(j);
      joint_at[j.id] = int(joints.size() - 1);
    }
    for (auto& j : joints) {
      if (!j.k->relation) continue;
      const Joint* src = scene.joint(j.id);
      const auto [n1, n2] = relation_coords(j.kind);
      const auto a = joint_at.find(src->joints.size() == 2 ? src->joints[0] : std::string());
      const auto b = joint_at.find(src->joints.size() == 2 ? src->joints[1] : std::string());
      if (a == joint_at.end() || b == joint_at.end()) {
        problems.push_back(j.name + ": a joint it couples is broken");
        j.k = nullptr;
        continue;
      }
      j.j1 = a->second, j.j2 = b->second;
      j.c1 = coord_index(*joints[size_t(j.j1)].k, n1);
      j.c2 = coord_index(*joints[size_t(j.j2)].k, n2);
      if (j.c1 < 0 || j.c2 < 0) {
        problems.push_back(j.name + ": the joints it couples have no " + n1 + " and " + n2);
        j.k = nullptr;
        continue;
      }
      const json& d = src->def;
      if (j.kind == "gear") j.ratio = d.value("ratio", -1.0);
      else if (j.kind == "rack_pinion") j.ratio = d.value("radius", 0.0) * (d.value("reverse", false) ? -1.0 : 1.0);  // mm per rad
      else j.ratio = d.value("lead", 0.0) / (2 * kPi);  // lead_screw: mm per rad
      if (d.contains("carrier") && d["carrier"].is_string()) {
        try {
          j.carrier = part_index(d["carrier"].get<std::string>());
        } catch (const std::exception& e) {
          problems.push_back(j.name + ": " + e.what());
          j.k = nullptr;
          continue;
        }
        const json cf = d.value("carrier_frames", json::array());
        if (cf.size() == 2) j.fc1 = rigid_of(Frame::from_json(cf[0])), j.fc2 = rigid_of(Frame::from_json(cf[1]));
      }
      const json values = d.value("values", json::array());
      const double u1 = joints[size_t(j.j1)].k->coords[size_t(j.c1)].angle ? kDeg : 1.0;
      const double u2 = joints[size_t(j.j2)].k->coords[size_t(j.c2)].angle ? kDeg : 1.0;
      j.q1c = values.size() == 2 ? values[0].get<double>() * u1 : 0.0;
      j.q2c = values.size() == 2 ? values[1].get<double>() * u2 : 0.0;
    }
    std::erase_if(joints, [](const Jt& j) { return !j.k; });
    joint_at.clear();
    for (size_t i = 0; i < joints.size(); ++i) joint_at[joints[i].id] = int(i);
    // Re-link relations by index after the erase.
    for (auto& j : joints)
      if (j.k->relation) {
        const Joint* src = scene.joint(j.id);
        j.j1 = joint_at.at(src->joints[0]);
        j.j2 = joint_at.at(src->joints[1]);
      }
    // Carriers: a part under another part's node moves with it.
    for (auto& p : parts) {
      const auto path = scene.path_to(p.node);
      for (auto it = path.rbegin() + 1; it < path.rend(); ++it)
        if (auto at = part_at.find(*it); at != part_at.end()) {
          p.carrier = at->second;
          break;
        }
    }
    // The size angles are weighed at: the spread of the joints.
    V3 lo = V3::Constant(1e300), hi = V3::Constant(-1e300);
    for (const auto& j : joints)
      if (!j.k->relation)
        for (const Rigid& f : {frame_a(j), frame_b(j)}) lo = lo.cwiseMin(f.p), hi = hi.cwiseMax(f.p);
    const double diag = joints.empty() ? 0 : (hi - lo).norm();
    L = std::clamp(diag, 10.0, 1e5);
    if (diag < 10) L = 100;
    for (auto& j : joints) j.q = measure(j), j.ref = j.q.empty() ? j.ref : j.q;
  }

  // ---------------------------------------------------------------- grouping parts
  // Which parts stay put in a solve: grounded ones, and per group joined to nothing that stays, its anchor.
  std::vector<bool> held(const std::vector<int>& prefer) const {
    std::vector<int> root(parts.size());
    std::iota(root.begin(), root.end(), 0);
    std::function<int(int)> find = [&](int x) { return root[size_t(x)] == x ? x : root[size_t(x)] = find(root[size_t(x)]); };
    auto join = [&](int a, int b) {
      if (a >= 0 && b >= 0) root[size_t(find(a))] = find(b);
    };
    for (const auto& j : joints) {
      const auto ps = parts_of(j);
      for (size_t i = 1; i < ps.size(); ++i) join(ps[0], ps[i]);
    }
    std::set<int> grounded;
    for (size_t i = 0; i < parts.size(); ++i)
      if (parts[i].ground) grounded.insert(find(int(i)));
    for (const auto& j : joints)
      if (!j.k->relation && j.a < 0 && j.b >= 0) grounded.insert(find(j.b));
    std::vector<bool> out(parts.size(), false);
    for (size_t i = 0; i < parts.size(); ++i) out[i] = parts[i].ground;
    std::set<int> anchored;
    auto anchor = [&](int p) {
      if (p < 0) return;
      const int r = find(p);
      if (grounded.count(r) || anchored.count(r)) return;
      anchored.insert(r);
      out[size_t(p)] = true;
    };
    for (int p : prefer) anchor(p);
    for (const auto& j : joints)
      if (!j.k->relation) anchor(j.a >= 0 ? j.a : j.b);
    return out;
  }

  // ---------------------------------------------------------------- the solve
  struct Target {
    int joint, coord;
    double value;  // rad / mm
  };

  std::vector<Group> groups(const std::vector<Target>& targets, const std::vector<Target>& extra) const {
    std::vector<Group> out;
    for (size_t ji = 0; ji < joints.size(); ++ji) {
      const Jt& j = joints[ji];
      Group g;
      g.joint = int(ji);
      g.parts = parts_of(j);
      if (j.k->relation) {
        const Jt& j1 = joints[size_t(j.j1)];
        const Jt& j2 = joints[size_t(j.j2)];
        const bool angular = j2.k->coords[size_t(j.c2)].angle;
        g.rows = 1;
        g.f = [this, &j, &j1, &j2, angular](double* out) {
          const double q1 = relation_coord(j, 0), q2 = relation_coord(j, 1);
          out[0] = (q2 - j.q2c - j.ratio * (q1 - j.q1c)) * (angular ? L : 1.0);
          (void)j1, (void)j2;
        };
      } else {
        g.rows = constraint_rows(j);
        g.f = [this, &j](double* out) { constraints(j, out); };
      }
      out.push_back(std::move(g));
      if (j.locked && !j.k->relation)
        for (size_t c = 0; c < j.q.size(); ++c) {
          Group lock;
          lock.joint = int(ji);
          lock.parts = parts_of(j);
          lock.rows = 1;
          const double at = j.q[c];
          lock.f = [this, &j, c, at](double* out) { out[0] = (measure(j)[c] - at) * weight(j.k->coords[c]); };
          out.push_back(std::move(lock));
        }
    }
    for (const auto* list : {&targets, &extra})
      for (const Target& t : *list) {
        const Jt& j = joints[size_t(t.joint)];
        Group g;
        g.joint = t.joint;
        g.parts = parts_of(j);
        g.rows = 1;
        g.f = [this, &j, t](double* out) { out[0] = (measure(j)[size_t(t.coord)] - t.value) * weight(j.k->coords[size_t(t.coord)]); };
        out.push_back(std::move(g));
      }
    return out;
  }

  // One Newton solve of these equations from where the parts are. held: parts that do not move.
  Result newton(const std::vector<Group>& gs, const std::vector<bool>& hold) {
    Result res;
    std::vector<int> var(parts.size(), -1);
    int n = 0;
    for (size_t i = 0; i < parts.size(); ++i)
      if (!hold[i]) var[i] = n, n += 6;
    int m = 0;
    for (const auto& g : gs) m += g.rows;
    Eigen::VectorXd r(m);
    auto eval = [&](Eigen::VectorXd& out) {
      int row = 0;
      for (const auto& g : gs) {
        g.f(out.data() + row);
        row += g.rows;
      }
    };
    auto apply = [&](const Eigen::VectorXd& dx, double alpha) {
      for (size_t i = 0; i < parts.size(); ++i) {
        if (var[i] < 0) continue;
        Part& p = parts[i];
        p.T.p += alpha * dx.segment<3>(var[i]);
        p.T.R = rot_exp(alpha * dx.segment<3>(var[i] + 3) / L) * p.T.R;
      }
    };
    const double tol = 1e-9 * L;
    const double h = 1e-6 * L;
    std::vector<Rigid> start(parts.size());
    for (int it = 0; it < 80; ++it) {
      eval(r);
      res.residual = m ? r.cwiseAbs().maxCoeff() : 0.0;
      res.iterations = it;
      if (res.residual < tol || n == 0) break;
      Eigen::MatrixXd J = Eigen::MatrixXd::Zero(m, n);
      int row = 0;
      std::vector<double> fp(64), fm(64);
      for (const auto& g : gs) {
        fp.resize(size_t(g.rows)), fm.resize(size_t(g.rows));
        for (int p : g.parts) {
          if (var[size_t(p)] < 0) continue;
          Part& part = parts[size_t(p)];
          const Rigid keep = part.T;
          for (int k = 0; k < 6; ++k) {
            V3 e = V3::Zero();
            e(k % 3) = h;
            if (k < 3) part.T.p = keep.p + e;
            else part.T.R = rot_exp(e / L) * keep.R;
            g.f(fp.data());
            if (k < 3) part.T.p = keep.p - e;
            else part.T.R = rot_exp(-e / L) * keep.R;
            g.f(fm.data());
            part.T = keep;
            for (int q = 0; q < g.rows; ++q) J(row + q, var[size_t(p)] + k) = (fp[size_t(q)] - fm[size_t(q)]) / (2 * h);
          }
        }
        row += g.rows;
      }
      // The least motion that meets the equations to first order (minimum-norm least squares).
      Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod(J);
      cod.setThreshold(1e-10);
      const Eigen::VectorXd dx = -cod.solve(r);
      for (size_t i = 0; i < parts.size(); ++i) start[i] = parts[i].T;
      const double before = r.norm();
      double alpha = 1;
      bool better = false;
      Eigen::VectorXd r2(m);
      for (int tries = 0; tries < 12; ++tries) {
        apply(dx, alpha);
        eval(r2);
        if (r2.norm() < before) {
          better = true;
          break;
        }
        for (size_t i = 0; i < parts.size(); ++i) parts[i].T = start[i];
        alpha *= 0.5;
      }
      if (!better) break;
    }
    eval(r);
    res.residual = m ? r.cwiseAbs().maxCoeff() : 0.0;
    res.ok = res.residual < 1e-6 * L;
    if (!res.ok) {
      // Name the joints whose equations are furthest off.
      std::vector<std::pair<double, int>> worst;
      int row = 0;
      for (const auto& g : gs) {
        double e = 0;
        for (int q = 0; q < g.rows; ++q) e = std::max(e, std::fabs(r(row + q)));
        row += g.rows;
        if (e > 1e-6 * L) worst.push_back({e, g.joint});
      }
      std::sort(worst.rbegin(), worst.rend());
      std::vector<std::string> names;
      for (const auto& [e, ji] : worst)
        if (ji >= 0 && std::find(names.begin(), names.end(), joints[size_t(ji)].name) == names.end() && names.size() < 4)
          names.push_back("\"" + joints[size_t(ji)].name + "\"");
      std::string list;
      for (size_t i = 0; i < names.size(); ++i) list += (i ? (i + 1 == names.size() ? " and " : ", ") : "") + names[i];
      res.error = "the joints cannot all be met there" + (list.empty() ? std::string() : ": " + list + " stay off by " + std::to_string(res.residual) + " mm") +
                  " (the mechanism locks up or that value is out of its reach)";
    }
    return res;
  }

  // Newton with limits: a coordinate that ends beyond a limit is held at it and the solve is run again.
  Result limited(const std::vector<Target>& targets, const std::vector<bool>& hold) {
    std::vector<Target> extra;
    Result res;
    for (int round = 0; round < 6; ++round) {
      const auto gs = groups(targets, extra);
      res = newton(gs, hold);
      if (!res.ok) return res;
      bool added = false;
      for (size_t ji = 0; ji < joints.size(); ++ji) {
        const Jt& j = joints[ji];
        if (j.k->relation) continue;
        const auto q = measure(j);
        for (size_t c = 0; c < q.size(); ++c) {
          const auto [lo, hi] = j.limits[c];
          const bool driven = std::any_of(targets.begin(), targets.end(), [&](const Target& t) { return t.joint == int(ji) && t.coord == int(c); });
          const bool pinned = std::any_of(extra.begin(), extra.end(), [&](const Target& t) { return t.joint == int(ji) && t.coord == int(c); });
          if (driven || pinned) continue;
          const double eps = j.k->coords[c].angle ? 1e-7 : 1e-7 * L;
          if (!std::isnan(lo) && q[c] < lo - eps) extra.push_back({int(ji), int(c), lo}), added = true;
          else if (!std::isnan(hi) && q[c] > hi + eps) extra.push_back({int(ji), int(c), hi}), added = true;
        }
      }
      if (!added) break;
      for (const auto& t : extra) res.notes.push_back("\"" + joints[size_t(t.joint)].name + "\" stopped at its limit");
    }
    return res;
  }

  void commit_values() {
    for (auto& j : joints)
      if (!j.k->relation) j.q = measure(j), j.ref = j.q;
    for (auto& j : joints)
      if (j.k->relation && j.carrier >= 0) j.cref = {relation_coord(j, 0), relation_coord(j, 1)};
  }

  // Parts under a part that moved, which did not move themselves, are carried along and the joints met again.
  void carry(const std::vector<Rigid>& before, const std::vector<bool>& hold) {
    bool any = false;
    for (size_t i = 0; i < parts.size(); ++i) {
      const int c = parts[i].carrier;
      if (c < 0 || hold[i]) continue;
      if (same(parts[size_t(c)].T, before[size_t(c)]) || !same(parts[i].T, before[i], 1e-7)) continue;
      parts[i].T = parts[size_t(c)].T * before[size_t(c)].inverse() * before[i];
      any = true;
    }
    if (any) limited({}, hold);
  }

  Result drive(const Values& wanted, const std::string& anchor) {
    Result res;
    std::vector<Target> goal;
    std::vector<int> prefer;
    if (!anchor.empty()) {
      if (auto it = part_at.find(anchor); it != part_at.end()) prefer.push_back(it->second);
    }
    for (const auto& [id, values] : wanted) {
      const auto it = joint_at.find(id);
      if (it == joint_at.end()) throw Error("joint " + id + " is not part of the mechanism (deleted, or broken: see problems)");
      const Jt& j = joints[size_t(it->second)];
      if (j.k->relation) throw Error("\"" + j.name + "\" is a " + j.kind + ": drive one of the joints it couples");
      if (values.size() > j.k->coords.size())
        throw Error("\"" + j.name + "\" has " + std::to_string(j.k->coords.size()) + " coordinate(s), " + std::to_string(values.size()) + " given");
      if (j.locked) throw Error("\"" + j.name + "\" is locked: unlock it to move it");
      if (j.a >= 0) prefer.push_back(j.a);
      for (size_t c = 0; c < values.size(); ++c) {
        if (std::isnan(values[c])) continue;
        const double unit = j.k->coords[c].angle ? kDeg : 1.0;
        double v = values[c] * unit;
        const auto [lo, hi] = j.limits[c];
        if (!std::isnan(lo) && v < lo) v = lo, res.notes.push_back("\"" + j.name + "\" held at its lower limit");
        if (!std::isnan(hi) && v > hi) v = hi, res.notes.push_back("\"" + j.name + "\" held at its upper limit");
        goal.push_back({it->second, int(c), v});
      }
    }
    const auto hold = held(prefer);
    std::vector<Rigid> before(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) before[i] = parts[i].T;
    // Sub-steps: at most 10 degrees or a tenth of the size per step.
    std::vector<double> from;
    int steps = 1;
    for (const auto& t : goal) {
      const Jt& j = joints[size_t(t.joint)];
      const double now = j.q[size_t(t.coord)];
      from.push_back(now);
      const double span = j.k->coords[size_t(t.coord)].angle ? 10 * kDeg : 0.1 * L;
      steps = std::max(steps, int(std::ceil(std::fabs(t.value - now) / span)));
    }
    steps = std::min(steps, 2000);
    std::vector<Rigid> good(parts.size());
    for (int s = 1; s <= steps; ++s) {
      for (size_t i = 0; i < parts.size(); ++i) good[i] = parts[i].T;
      std::vector<Target> step = goal;
      for (size_t i = 0; i < step.size(); ++i) step[i].value = from[i] + (goal[i].value - from[i]) * double(s) / steps;
      Result r = limited(step, hold);
      res.iterations += r.iterations;
      res.residual = r.residual;
      for (auto& n : r.notes)
        if (std::find(res.notes.begin(), res.notes.end(), n) == res.notes.end()) res.notes.push_back(n);
      if (!r.ok) {
        for (size_t i = 0; i < parts.size(); ++i) parts[i].T = good[i];
        res.ok = false;
        res.error = r.error;
        res.reached = double(s - 1) / steps;
        break;
      }
      commit_values();
    }
    carry(before, hold);
    commit_values();
    return res;
  }
};

Mechanism::Mechanism(const Scene& scene) : m(std::make_unique<Impl>()) { m->build(scene); }
Mechanism::~Mechanism() = default;
Mechanism::Mechanism(const Mechanism& o) : m(std::make_unique<Impl>(*o.m)) {}
Mechanism& Mechanism::operator=(const Mechanism& o) {
  m = std::make_unique<Impl>(*o.m);
  return *this;
}

Mechanism::Result Mechanism::drive(const Values& targets, const std::string& anchor) { return m->drive(targets, anchor); }
Mechanism::Result Mechanism::settle(const std::string& anchor) { return m->drive({}, anchor); }

Mechanism::Result Mechanism::place(const std::string& part, const Mat4& world, const std::string& anchor) {
  const auto it = m->part_at.find(part);
  if (it == m->part_at.end()) throw Error("node " + part + " is not a part of the mechanism (no joint names it)");
  Impl::Part& p = m->parts[size_t(it->second)];
  const Rigid keep = p.T;
  p.T = rigid_of(world);
  // The dragged part is held where it was put; the rest follow.
  auto hold = m->held({anchor.empty() ? -1 : (m->part_at.count(anchor) ? m->part_at.at(anchor) : -1)});
  hold[size_t(it->second)] = true;
  Result r = m->limited({}, hold);
  if (!r.ok) p.T = keep;
  m->commit_values();
  return r;
}

Values Mechanism::values() const {
  Values out;
  for (const auto& j : m->joints) {
    if (j.k->relation) continue;
    std::vector<double> v;
    for (size_t c = 0; c < j.q.size(); ++c) v.push_back(j.q[c] / (j.k->coords[c].angle ? kDeg : 1.0));
    out[j.id] = v;
  }
  return out;
}

std::map<std::string, Mat4> Mechanism::part_worlds() const {
  std::map<std::string, Mat4> out;
  for (const auto& p : m->parts) out[p.node] = mat_of(p.T);
  return out;
}

Mat4 Mechanism::part_world(const std::string& part) const {
  const auto it = m->part_at.find(part);
  if (it == m->part_at.end()) throw Error("node " + part + " is not a part of the mechanism");
  return mat_of(m->parts[size_t(it->second)].T);
}

const std::vector<std::string>& Mechanism::parts() const { return m->part_ids; }
bool Mechanism::has_part(const std::string& node) const { return m->part_at.count(node) > 0; }
const std::vector<std::string>& Mechanism::problems() const { return m->problems; }
double Mechanism::size() const { return m->L; }

std::pair<Frame, Frame> Mechanism::joint_frames(const std::string& joint) const {
  const auto it = m->joint_at.find(joint);
  if (it == m->joint_at.end()) throw Error("joint " + joint + " is not part of the mechanism");
  const auto& j = m->joints[size_t(it->second)];
  if (j.k->relation) throw Error("a relation has no frames");
  return {frame_of(m->frame_a(j)), frame_of(m->frame_b(j))};
}

std::vector<std::pair<std::string, Mat4>> Mechanism::placements(const Scene& scene) const {
  Scene copy;
  copy.nodes = scene.nodes;
  std::vector<size_t> order(m->parts.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return m->parts[a].depth < m->parts[b].depth; });
  std::vector<std::pair<std::string, Mat4>> out;
  for (size_t i : order) {
    const auto& p = m->parts[i];
    Node& n = copy.nodes.at(p.node);
    const Rigid parent = n.parent.empty() ? Rigid() : rigid_of(copy.world(n.parent));
    const Rigid local = parent.inverse() * p.T;
    if (same(local, rigid_of(n.local), 1e-10)) continue;
    n.local = mat_of(local);
    out.push_back({p.node, n.local});
  }
  return out;
}

Scene Mechanism::posed(const Scene& scene) const {
  Scene out = scene;
  for (const auto& [id, local] : placements(scene)) out.nodes.at(id).local = local;
  const Values v = values();
  for (auto& j : out.joints)
    if (const auto it = v.find(j.id); it != v.end()) j.values = it->second;
  return out;
}

json Mechanism::analysis() const {
  Impl& im = *m;
  const auto hold = im.held({});
  std::vector<int> var(im.parts.size(), -1);
  int n = 0;
  for (size_t i = 0; i < im.parts.size(); ++i)
    if (!hold[i]) var[i] = n, n += 6;
  const auto gs = im.groups({}, {});
  int rows = 0;
  for (const auto& g : gs) rows += g.rows;
  Eigen::MatrixXd J = Eigen::MatrixXd::Zero(std::max(rows, 1), std::max(n, 1));
  const double h = 1e-6 * im.L;
  auto jac = [&](const Impl::Group& g, int row, Eigen::MatrixXd& into) {
    std::vector<double> fp(size_t(g.rows)), fm(size_t(g.rows));
    for (int p : g.parts) {
      if (var[size_t(p)] < 0) continue;
      auto& part = im.parts[size_t(p)];
      const Rigid keep = part.T;
      for (int k = 0; k < 6; ++k) {
        V3 e = V3::Zero();
        e(k % 3) = h;
        if (k < 3) part.T.p = keep.p + e;
        else part.T.R = rot_exp(e / im.L) * keep.R;
        g.f(fp.data());
        if (k < 3) part.T.p = keep.p - e;
        else part.T.R = rot_exp(-e / im.L) * keep.R;
        g.f(fm.data());
        part.T = keep;
        for (int q = 0; q < g.rows; ++q) into(row + q, var[size_t(p)] + k) = (fp[size_t(q)] - fm[size_t(q)]) / (2 * h);
      }
    }
  };
  int row = 0;
  for (const auto& g : gs) jac(g, row, J), row += g.rows;
  int rank = 0;
  Eigen::MatrixXd null;
  if (n > 0) {
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(J, Eigen::ComputeFullV);
    const auto& s = svd.singularValues();
    const double top = s.size() ? s(0) : 0.0;
    for (int i = 0; i < s.size(); ++i)
      if (s(i) > 1e-8 * std::max(top, 1.0)) ++rank;
    null = svd.matrixV().rightCols(n - rank);
  }
  json out;
  out["dof"] = n - rank;
  out["constraints"] = rows;
  out["redundant"] = std::max(0, rows - rank);
  out["parts"] = im.part_ids;
  json held = json::array(), free_parts = json::array();
  for (size_t i = 0; i < im.parts.size(); ++i) {
    if (hold[i]) {
      held.push_back(im.parts[i].node);
      continue;
    }
    if (null.cols() > 0 && null.middleRows(var[i], 6).norm() > 1e-6) free_parts.push_back(im.parts[i].node);
  }
  out["held"] = held;
  out["moving_parts"] = free_parts;
  json joints = json::array();
  for (const auto& j : im.joints) {
    json e = {{"id", j.id}, {"name", j.name}, {"kind", j.kind}};
    if (!j.k->relation) {
      json values = json::array(), free = json::array();
      for (size_t c = 0; c < j.q.size(); ++c) {
        values.push_back(j.q[c] / (j.k->coords[c].angle ? kDeg : 1.0));
        // The coordinate's gradient against the null space of the joints: can it change while every joint holds?
        Impl::Group g;
        g.parts = im.parts_of(j);
        g.rows = 1;
        g.f = [&im, &j, c](double* o) { o[0] = im.measure(j)[c] * im.weight(j.k->coords[c]); };
        Eigen::MatrixXd gr = Eigen::MatrixXd::Zero(1, std::max(n, 1));
        jac(g, 0, gr);
        const double scale = gr.norm();
        free.push_back(n > 0 && null.cols() > 0 && scale > 1e-12 && (gr.leftCols(n) * null).norm() > 1e-6 * scale && !j.locked);
      }
      e["coordinates"] = json::array();
      for (const auto& c : j.k->coords) e["coordinates"].push_back(c.name);
      e["values"] = values;
      e["free"] = free;
    }
    joints.push_back(e);
  }
  out["joints"] = joints;
  out["problems"] = im.problems;
  return out;
}

std::vector<double> joint_coordinates(const std::string& kind, const Mat4& a, const Mat4& b, const std::vector<double>& ref) {
  Mechanism::Impl im;
  Mechanism::Impl::Jt j;
  j.kind = kind;
  j.k = joint_kind(kind);
  if (!j.k) throw Error("unknown joint kind " + kind);
  j.fa = rigid_of(a);
  j.fb = rigid_of(b);
  j.ref = ref;
  j.ref.resize(j.k->coords.size(), 0.0);
  return im.measure(j);
}

json pose_op(const Scene& before, const Mechanism& mech, const std::string& name) {
  json placements = json::array();
  for (const auto& [id, local] : mech.placements(before)) placements.push_back({{"target", id}, {"matrix", local.to_json()}});
  json values = json::object();
  bool changed = !placements.empty();
  for (const auto& [id, v] : mech.values()) {
    json a = json::array();
    for (double x : v) a.push_back(std::round(x * 1e9) / 1e9);
    values[id] = a;
    if (const Joint* j = before.joint(id); j && j->values.size() == v.size())
      for (size_t i = 0; i < v.size(); ++i) changed = changed || std::fabs(j->values[i] - v[i]) > 1e-9;
  }
  if (!changed) return nullptr;
  return {{"op", "pose"}, {"name", name}, {"values", values}, {"placements", placements}};
}

}  // namespace opad::sim
