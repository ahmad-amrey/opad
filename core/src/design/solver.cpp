// Geometric constraint solver for the 2D sketch: Levenberg-Marquardt on the constraint residuals, every step
// taken in the minimum-norm sense (dx = J^T (J J^T + lambda I)^-1 (-f)), so an under-constrained sketch moves
// to the *nearest* solution instead of wherever an arbitrary elimination would send it. Jacobians are exact
// (forward-mode dual numbers over the handful of variables one constraint touches).
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>

#include "opad/design/sketch.hpp"

namespace opad::design {

namespace {

using EType = SkEntity::Type;
using CType = SkConstraint::Type;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTiny = 1e-9;       // mm: a line shorter than this has no direction
constexpr double kMinRadius = 1e-6;  // mm: a circle squeezed below this is no circle any more
constexpr double kDragSpring = 1e-3; // drag spring against "leave the sketch alone"; small = the mouse wins
constexpr double kRankTol = 1e-7;    // a unit row this close to the span of the earlier ones adds no rank
constexpr double kFreeTol = 1e-9;    // squared null-space share of a coordinate that counts as "can move"
constexpr int kSlots = 12;           // variables one constraint can touch (two lines or two arcs = 8)

// ------------------------------------------------------------- dual numbers
struct Dual {
  double v = 0;
  std::array<double, kSlots> d{};
};

Dual K(double v) {
  Dual r;
  r.v = v;
  return r;
}
Dual operator+(Dual a, const Dual& b) {
  a.v += b.v;
  for (int i = 0; i < kSlots; ++i) a.d[i] += b.d[i];
  return a;
}
Dual operator-(Dual a, const Dual& b) {
  a.v -= b.v;
  for (int i = 0; i < kSlots; ++i) a.d[i] -= b.d[i];
  return a;
}
Dual operator*(const Dual& a, const Dual& b) {
  Dual r;
  r.v = a.v * b.v;
  for (int i = 0; i < kSlots; ++i) r.d[i] = a.d[i] * b.v + b.d[i] * a.v;
  return r;
}
Dual operator*(Dual a, double k) {
  a.v *= k;
  for (int i = 0; i < kSlots; ++i) a.d[i] *= k;
  return a;
}
Dual operator/(const Dual& a, const Dual& b) {
  Dual r;
  r.v = a.v / b.v;
  for (int i = 0; i < kSlots; ++i) r.d[i] = (a.d[i] - r.v * b.d[i]) / b.v;
  return r;
}
Dual operator-(Dual a, double k) {
  a.v -= k;
  return a;
}

struct P2 {
  Dual x, y;
};
P2 operator-(const P2& a, const P2& b) { return {a.x - b.x, a.y - b.y}; }
Dual cross(const P2& a, const P2& b) { return a.x * b.y - a.y * b.x; }
Dual dot(const P2& a, const P2& b) { return a.x * b.x + a.y * b.y; }

// |v|. At zero length the true gradient does not exist; pretending the vector points along +u keeps the
// Jacobian finite and gives the solver a (deterministic) direction to pull two coincident points apart.
Dual norm(const P2& v) {
  const double l = std::hypot(v.x.v, v.y.v);
  const double ux = l > 1e-12 ? v.x.v / l : 1.0, uy = l > 1e-12 ? v.y.v / l : 0.0;
  Dual r;
  r.v = l;
  for (int i = 0; i < kSlots; ++i) r.d[i] = ux * v.x.d[i] + uy * v.y.d[i];
  return r;
}

// Signed distance from p to the infinite line a->b (positive on the left). A degenerate line constrains nothing.
Dual sdist(const P2& p, const P2& a, const P2& b) {
  const P2 d = b - a;
  const Dual len = norm(d);
  const Dual cr = cross(d, p - a);
  return len.v < kTiny ? cr * (1.0 / kTiny) : cr / len;
}

Dual atan2d(const Dual& y, const Dual& x) {
  Dual r;
  r.v = std::atan2(y.v, x.v);
  const double den = x.v * x.v + y.v * y.v;
  if (den > 0)
    for (int i = 0; i < kSlots; ++i) r.d[i] = (x.v * y.d[i] - y.v * x.d[i]) / den;
  return r;
}

// ------------------------------------------------------------- the equation system
struct Row {
  std::vector<std::pair<int, double>> j;  // (variable, df/dvar), each variable once
  double f = 0;
  int owner = 0;  // index into sk.constraints, or -1 - entity index for an arc's own equal-radius rule
};

struct System {
  const Sketch& sk;
  std::unordered_map<int, int> pidx, eidx;
  std::vector<int> vx, vy, vr;  // variable index per point (x, y) and per entity (r); -1 = not a variable
  std::vector<int> arcs;        // entity indices of the arcs the solver keeps round
  int nvars = 0;

  // Choices made once, from the state the solve started in, so the solution never flips: which side of a
  // line, inner or outer tangency, which of the two angles, and the values a Fix holds.
  struct Aux {
    double s = 1, target = 0;
    std::vector<double> fix;
  };
  std::vector<Aux> aux;

  // evaluation state
  const std::vector<double>* x = nullptr;
  std::vector<Row> rows;
  size_t nrows = 0;
  bool init = false;
  int slot[kSlots] = {};
  int nslots = 0;
  int factorisations = 0;  // work counter: the drag loop stops on it (a count, not a clock, so runs repeat exactly)

  explicit System(const Sketch& s) : sk(s) {
    for (size_t i = 0; i < sk.points.size(); ++i) pidx[sk.points[i].id] = int(i);
    for (size_t i = 0; i < sk.entities.size(); ++i) eidx[sk.entities[i].id] = int(i);
    std::vector<char> pinned(sk.points.size(), 0);
    for (size_t i = 0; i < sk.points.size(); ++i) pinned[i] = sk.points[i].fixed;
    for (const auto& e : sk.entities)
      if (e.fixed)
        for (int pid : e.p) pinned[pidx.at(pid)] = 1;
    vx.assign(sk.points.size(), -1);
    vy.assign(sk.points.size(), -1);
    vr.assign(sk.entities.size(), -1);
    for (size_t i = 0; i < sk.points.size(); ++i)
      if (!pinned[i]) {
        vx[i] = nvars++;
        vy[i] = nvars++;
      }
    for (size_t i = 0; i < sk.entities.size(); ++i) {
      const auto& e = sk.entities[i];
      if (!e.fixed && (e.type == EType::Circle || e.type == EType::Ellipse)) vr[i] = nvars++;
      // A projected arc, or one whose three points are all pinned, is taken as it is: nothing could repair it.
      if (e.type == EType::Arc && !e.fixed && !(pinned[pidx.at(e.p[0])] && pinned[pidx.at(e.p[1])] && pinned[pidx.at(e.p[2])]))
        arcs.push_back(int(i));
    }
    aux.resize(sk.constraints.size());
  }

  std::vector<double> start() const {
    std::vector<double> v(size_t(nvars), 0.0);
    for (size_t i = 0; i < sk.points.size(); ++i)
      if (vx[i] >= 0) {
        v[size_t(vx[i])] = sk.points[i].x;
        v[size_t(vy[i])] = sk.points[i].y;
      }
    for (size_t i = 0; i < sk.entities.size(); ++i)
      if (vr[i] >= 0) v[size_t(vr[i])] = sk.entities[i].r;
    return v;
  }

  void store(Sketch& out, const std::vector<double>& v) const {
    for (size_t i = 0; i < out.points.size(); ++i)
      if (vx[i] >= 0) {
        out.points[i].x = v[size_t(vx[i])];
        out.points[i].y = v[size_t(vy[i])];
      }
    for (size_t i = 0; i < out.entities.size(); ++i)
      if (vr[i] >= 0) out.entities[i].r = v[size_t(vr[i])];
  }

  // ---- variables as duals
  Dual var(int gi, double fallback) {
    if (gi < 0) return K(fallback);
    int s = 0;
    while (s < nslots && slot[s] != gi) ++s;
    if (s == nslots) {
      if (nslots == kSlots) throw Error("sketch solver: constraint touches too many variables");
      slot[nslots++] = gi;
    }
    Dual r;
    r.v = (*x)[size_t(gi)];
    r.d[size_t(s)] = 1;
    return r;
  }
  P2 pt(int pid) {
    const int i = pidx.at(pid);
    return {var(vx[size_t(i)], sk.points[size_t(i)].x), var(vy[size_t(i)], sk.points[size_t(i)].y)};
  }
  bool is_point(int ref) const {
    if (pidx.count(ref)) return true;
    auto it = eidx.find(ref);
    return it != eidx.end() && sk.entities[size_t(it->second)].type == EType::Point;
  }
  P2 ref_pt(int ref) { return pidx.count(ref) ? pt(ref) : pt(ent(ref).p[0]); }
  const SkEntity& ent(int ref) const { return sk.entities[size_t(eidx.at(ref))]; }
  bool is_line(int ref) const { return !pidx.count(ref) && ent(ref).type == EType::Line; }
  Dual radius(const SkEntity& e) {
    if (e.type == EType::Arc) return norm(pt(e.p[1]) - pt(e.p[0]));
    return var(vr[size_t(eidx.at(e.id))], e.r);
  }

  Row& next_row(int owner) {
    if (nrows == rows.size()) rows.emplace_back();
    Row& r = rows[nrows++];
    r.j.clear();
    r.owner = owner;
    return r;
  }
  void emit(int owner, const Dual& f) {
    Row& r = next_row(owner);
    r.f = f.v;
    for (int s = 0; s < nslots; ++s)
      if (f.d[size_t(s)] != 0) r.j.emplace_back(slot[s], f.d[size_t(s)]);
  }

  // Angle from l1's direction to l2's, against a target chosen at the start; the difference is wrapped so the
  // residual is smooth through +-pi (anti-parallel lines).
  void angular(int ci, const SkConstraint& c) {
    Aux& a = aux[size_t(ci)];
    const SkEntity &l1 = ent(c.refs[0]), &l2 = ent(c.refs[1]);
    const P2 d1 = pt(l1.p[1]) - pt(l1.p[0]), d2 = pt(l2.p[1]) - pt(l2.p[0]);
    const bool degenerate = std::hypot(d1.x.v, d1.y.v) < kTiny || std::hypot(d2.x.v, d2.y.v) < kTiny;
    Dual th = degenerate ? K(0) : atan2d(cross(d1, d2), dot(d1, d2));
    if (init) {
      if (c.type == CType::Parallel) a.target = std::fabs(th.v) <= kPi / 2 ? 0.0 : kPi;
      else if (c.type == CType::Perpendicular) a.target = th.v >= 0 ? kPi / 2 : -kPi / 2;
      else a.target = th.v >= 0 ? c.value : -c.value;
    }
    if (degenerate) return emit(ci, K(0));  // no direction, nothing to ask for (and no NaN)
    th.v -= a.target;
    while (th.v > kPi) th.v -= 2 * kPi;
    while (th.v <= -kPi) th.v += 2 * kPi;
    emit(ci, th);
  }

  void fix(int ci, const SkConstraint& c) {
    Aux& a = aux[size_t(ci)];
    std::vector<std::pair<int, double>> vals;  // (variable, current value)
    auto add_pt = [&](int pid) {
      const size_t i = size_t(pidx.at(pid));
      vals.emplace_back(vx[i], vx[i] >= 0 ? (*x)[size_t(vx[i])] : sk.points[i].x);
      vals.emplace_back(vy[i], vy[i] >= 0 ? (*x)[size_t(vy[i])] : sk.points[i].y);
    };
    if (pidx.count(c.refs[0])) {
      add_pt(c.refs[0]);
    } else {
      const SkEntity& e = ent(c.refs[0]);
      for (int pid : e.p) add_pt(pid);
      const int gr = vr[size_t(eidx.at(e.id))];
      if (gr >= 0) vals.emplace_back(gr, (*x)[size_t(gr)]);
    }
    if (init) {
      a.fix.clear();
      for (const auto& v : vals) a.fix.push_back(v.second);
    }
    for (size_t k = 0; k < vals.size(); ++k) {
      Row& r = next_row(ci);
      r.f = vals[k].second - a.fix[k];
      if (vals[k].first >= 0) r.j.emplace_back(vals[k].first, 1.0);
    }
  }

  void constraint(int ci) {
    const SkConstraint& c = sk.constraints[size_t(ci)];
    if (c.reference) return;
    Aux& a = aux[size_t(ci)];
    nslots = 0;
    auto side = [&](const Dual& d) {  // remembers which side at the start
      if (init) a.s = d.v >= 0 ? 1.0 : -1.0;
      return a.s;
    };
    auto two_points = [&](P2& p, P2& q) {  // [line] or [point, point]
      if (c.refs.size() == 1) {
        p = pt(ent(c.refs[0]).p[0]);
        q = pt(ent(c.refs[0]).p[1]);
      } else {
        p = ref_pt(c.refs[0]);
        q = ref_pt(c.refs[1]);
      }
    };
    switch (c.type) {
      case CType::Coincident: {
        const P2 p = ref_pt(c.refs[0]);
        if (is_point(c.refs[1])) {
          const P2 q = ref_pt(c.refs[1]);
          emit(ci, p.x - q.x);
          emit(ci, p.y - q.y);
        } else if (is_line(c.refs[1])) {
          const SkEntity& l = ent(c.refs[1]);
          emit(ci, sdist(p, pt(l.p[0]), pt(l.p[1])));
        } else {
          const SkEntity& e = ent(c.refs[1]);
          emit(ci, norm(p - pt(e.p[0])) - radius(e));
        }
        break;
      }
      case CType::Horizontal:
      case CType::Vertical: {
        P2 p, q;
        two_points(p, q);
        emit(ci, c.type == CType::Horizontal ? q.y - p.y : q.x - p.x);
        break;
      }
      case CType::Parallel:
      case CType::Perpendicular:
      case CType::Angle: angular(ci, c); break;
      case CType::Collinear: {
        const SkEntity &l1 = ent(c.refs[0]), &l2 = ent(c.refs[1]);
        const P2 p0 = pt(l1.p[0]), p1 = pt(l1.p[1]);
        emit(ci, sdist(pt(l2.p[0]), p0, p1));
        emit(ci, sdist(pt(l2.p[1]), p0, p1));
        break;
      }
      case CType::Tangent: {
        const bool line0 = is_line(c.refs[0]), line1 = is_line(c.refs[1]);
        if (line0 || line1) {
          const SkEntity &l = ent(c.refs[line0 ? 0 : 1]), &e = ent(c.refs[line0 ? 1 : 0]);
          const Dual d = sdist(pt(e.p[0]), pt(l.p[0]), pt(l.p[1]));
          emit(ci, d - radius(e) * side(d));
        } else {
          const SkEntity &e1 = ent(c.refs[0]), &e2 = ent(c.refs[1]);
          const Dual d = norm(pt(e1.p[0]) - pt(e2.p[0])), r1 = radius(e1), r2 = radius(e2);
          if (init) {  // target 0: outside each other; 1: one inside the other, s = which one is bigger
            a.target = std::fabs(d.v - (r1.v + r2.v)) <= std::fabs(d.v - std::fabs(r1.v - r2.v)) ? 0 : 1;
            a.s = r1.v >= r2.v ? 1.0 : -1.0;
          }
          emit(ci, a.target == 0 ? d - (r1 + r2) : d - (r1 - r2) * a.s);
        }
        break;
      }
      case CType::Equal: {
        const SkEntity &e1 = ent(c.refs[0]), &e2 = ent(c.refs[1]);
        if (e1.type == EType::Line) emit(ci, norm(pt(e1.p[1]) - pt(e1.p[0])) - norm(pt(e2.p[1]) - pt(e2.p[0])));
        else emit(ci, radius(e1) - radius(e2));
        break;
      }
      case CType::Concentric: {
        const P2 p = pt(ent(c.refs[0]).p[0]), q = pt(ent(c.refs[1]).p[0]);
        emit(ci, p.x - q.x);
        emit(ci, p.y - q.y);
        break;
      }
      case CType::Midpoint: {
        const P2 p = ref_pt(c.refs[0]);
        const SkEntity& l = ent(c.refs[1]);
        const P2 p0 = pt(l.p[0]), p1 = pt(l.p[1]);
        emit(ci, p.x - (p0.x + p1.x) * 0.5);
        emit(ci, p.y - (p0.y + p1.y) * 0.5);
        break;
      }
      case CType::Symmetric: {
        const P2 p = ref_pt(c.refs[0]), q = ref_pt(c.refs[1]);
        const SkEntity& l = ent(c.refs[2]);
        const P2 p0 = pt(l.p[0]), p1 = pt(l.p[1]);
        const P2 mid{(p.x + q.x) * 0.5, (p.y + q.y) * 0.5};
        emit(ci, sdist(mid, p0, p1));
        // Not divided by |pq|: both points sitting on the axis is a legitimate mirror pair.
        const P2 d = p1 - p0;
        const Dual len = norm(d), along = dot(q - p, d);
        emit(ci, len.v < kTiny ? along * (1.0 / kTiny) : along / len);
        break;
      }
      case CType::Fix: fix(ci, c); break;
      case CType::Distance: {
        if (c.refs.size() == 1 || is_point(c.refs[1])) {
          P2 p, q;
          two_points(p, q);
          emit(ci, norm(q - p) - c.value);
        } else {  // to a line, from a point or from the other line's first point; stays on its side
          const SkEntity& l = ent(c.refs[is_line(c.refs[0]) ? 0 : 1]);
          const P2 p = is_line(c.refs[0]) ? pt(ent(c.refs[1]).p[0]) : ref_pt(c.refs[0]);
          const Dual d = sdist(p, pt(l.p[0]), pt(l.p[1]));
          emit(ci, d - c.value * side(d));
        }
        break;
      }
      case CType::HDistance:
      case CType::VDistance: {
        const P2 p = ref_pt(c.refs[0]), q = ref_pt(c.refs[1]);
        const Dual d = c.type == CType::HDistance ? q.x - p.x : q.y - p.y;
        emit(ci, d - c.value * side(d));
        break;
      }
      case CType::Radius: emit(ci, radius(ent(c.refs[0])) - c.value); break;
      case CType::Diameter: emit(ci, radius(ent(c.refs[0])) * 2.0 - c.value); break;
      case CType::ArcLength: {
        const auto& e = ent(c.refs[0]);
        const P2 a = pt(e.p[1]) - pt(e.p[0]), b = pt(e.p[2]) - pt(e.p[0]);
        Dual angle = atan2d(cross(a, b), dot(a, b));
        if (angle.v < 0) angle.v += 2 * kPi;
        emit(ci, radius(e) * angle - c.value);
        break;
      }
    }
  }

  // Fills rows[0..nrows) at `at`. With `decide` the per-constraint choices are (re)made from that state.
  // False when anything came out non-finite.
  bool eval(const std::vector<double>& at, bool decide = false) {
    x = &at;
    init = decide;
    nrows = 0;
    for (int ei : arcs) {
      const SkEntity& e = sk.entities[size_t(ei)];
      nslots = 0;
      const P2 c = pt(e.p[0]);
      emit(-1 - ei, norm(pt(e.p[1]) - c) - norm(pt(e.p[2]) - c));
    }
    for (size_t ci = 0; ci < sk.constraints.size(); ++ci) constraint(int(ci));
    init = false;
    for (size_t i = 0; i < nrows; ++i) {
      if (!std::isfinite(rows[i].f)) return false;
      for (const auto& e : rows[i].j)
        if (!std::isfinite(e.second)) return false;
    }
    for (size_t i = 0; i < sk.entities.size(); ++i)
      if (vr[i] >= 0 && !std::isfinite(at[size_t(vr[i])])) return false;
    return true;
  }

  double max_residual() const {
    double m = 0;
    for (size_t i = 0; i < nrows; ++i) m = std::max(m, std::fabs(rows[i].f));
    return m;
  }
  bool radii_positive(const std::vector<double>& at) const {
    for (int g : vr)
      if (g >= 0 && !(at[size_t(g)] > kMinRadius)) return false;
    return true;
  }
};

// ------------------------------------------------------------- linear algebra
// In-place Cholesky of the symmetric positive definite m x m matrix `a` (lower triangle used). False when a
// pivot is not safely positive.
bool cholesky(std::vector<double>& a, size_t m) {
  for (size_t i = 0; i < m; ++i) {
    for (size_t j = 0; j <= i; ++j) {
      double s = a[i * m + j];
      for (size_t k = 0; k < j; ++k) s -= a[i * m + k] * a[j * m + k];
      if (i == j) {
        if (!(s > 1e-300) || !std::isfinite(s)) return false;
        a[i * m + i] = std::sqrt(s);
      } else {
        a[i * m + j] = s / a[j * m + j];
      }
    }
  }
  return true;
}

// Solves a y = b in place with the factor left by cholesky().
void cholesky_apply(const std::vector<double>& a, size_t m, std::vector<double>& b) {
  for (size_t i = 0; i < m; ++i) {
    double s = b[i];
    for (size_t k = 0; k < i; ++k) s -= a[i * m + k] * b[k];
    b[i] = s / a[i * m + i];
  }
  for (size_t i = m; i-- > 0;) {
    double s = b[i];
    for (size_t k = i + 1; k < m; ++k) s -= a[k * m + i] * b[k];
    b[i] = s / a[i * m + i];
  }
}

// The scaled Jacobian A = rs * J by columns, and M = A A^T (lower triangle), accumulated column by column: a
// column holds only the few rows that touch that variable, so this is far cheaper than a dense product.
using Columns = std::vector<std::vector<std::pair<size_t, double>>>;
void gram(const System& sys, const std::vector<double>& rs, Columns& cols, std::vector<double>& M) {
  const size_t m = sys.nrows;
  cols.resize(size_t(sys.nvars));
  for (auto& c : cols) c.clear();
  for (size_t i = 0; i < m; ++i)
    for (const auto& e : sys.rows[i].j) cols[size_t(e.first)].emplace_back(i, rs[i] * e.second);
  M.assign(m * m, 0.0);
  for (const auto& c : cols)
    for (const auto& p : c)
      for (const auto& q : c)
        if (q.first <= p.first) M[p.first * m + q.first] += p.second * q.second;
}

// Levenberg-Marquardt from `x`. Returns whether every residual got below the tolerance; `x` is the best state
// reached either way. `polish` spends up to two more Newton steps after the tolerance is met.
bool minimise(System& sys, std::vector<double>& x, const SolveOptions& opt, bool polish_after = true) {
  const double tol = opt.tolerance;
  if (!sys.eval(x)) return false;
  const size_t m = sys.nrows, n = size_t(sys.nvars);
  if (m == 0) return true;
  if (n == 0) return sys.max_residual() <= tol;

  // Row scales, fixed for the whole run so the merit function is one function: 1 / |gradient| turns an angular
  // residual (gradient 1/length) into the arc length it stands for, so big sketches behave like small ones.
  std::vector<double> rs(m, 1.0);
  for (size_t i = 0; i < m; ++i) {
    double g = 0;
    for (const auto& e : sys.rows[i].j) g += e.second * e.second;
    if (g > 0) rs[i] = std::clamp(1.0 / std::sqrt(g), 1e-4, 1e4);
  }
  auto merit = [&] {
    double s = 0;
    for (size_t i = 0; i < m; ++i) s += rs[i] * rs[i] * sys.rows[i].f * sys.rows[i].f;
    return s;
  };

  double err = merit(), res = sys.max_residual();
  double lambda = 1e-6;
  int polish = 0, stalled = 0;
  std::vector<double> good;  // the first state inside the tolerance, in case polishing does harm
  Columns cols;
  std::vector<double> M, A, y(m), xn(n);

  for (int it = 0; it < opt.max_iterations; ++it) {
    if (res <= tol && sys.radii_positive(x)) {
      // Newton converges quadratically here, so one or two more steps reach round-off; the rank analysis
      // wants the Jacobian *at* the solution to see dependent constraints as dependent.
      if (good.empty()) good = x;
      if (!polish_after || res <= tol * 1e-4 || polish++ >= 2) break;
    }
    gram(sys, rs, cols, M);

    bool moved = false;
    for (int tries = 0; tries < 40 && lambda <= 1e12; ++tries, lambda *= 10) {
      A = M;
      for (size_t i = 0; i < m; ++i) {
        A[i * m + i] += lambda;
        y[i] = -rs[i] * sys.rows[i].f;
      }
      ++sys.factorisations;
      if (!cholesky(A, m)) continue;
      cholesky_apply(A, m, y);
      xn = x;
      for (size_t j = 0; j < n; ++j) {
        double s = 0;
        for (const auto& p : cols[j]) s += p.second * y[p.first];
        xn[j] += s;
      }
      // Probe on a copy of the rows: a rejected step must leave the current Jacobian intact.
      const std::vector<Row> keep(sys.rows.begin(), sys.rows.begin() + std::ptrdiff_t(m));
      const bool finite = sys.eval(xn);
      const double en = finite ? merit() : 0;
      if (finite && en < err) {
        stalled = err - en <= 1e-10 * err ? stalled + 1 : 0;
        x = xn;
        err = en;
        res = sys.max_residual();
        lambda = std::max(lambda * 0.1, 1e-10);
        moved = true;
        break;
      }
      std::copy(keep.begin(), keep.end(), sys.rows.begin());
      sys.nrows = m;
    }
    if (!moved || stalled >= 3) break;  // a minimum that is not a solution: the constraints conflict
  }

  if (!good.empty() && !(res <= tol && sys.radii_positive(x))) {
    x = good;
    sys.eval(x);
    res = sys.max_residual();
  }
  return res <= tol && sys.radii_positive(x);
}

// Orthonormal basis of the row space of the constraint Jacobian at `x`, by Gram-Schmidt over the rows in order.
// A row that lies in the span of the earlier ones adds nothing; `adds` (per constraint, optional) records
// which constraints contributed at least one direction. False when the state does not evaluate.
bool row_basis(System& sys, const std::vector<double>& x, std::vector<std::vector<double>>& basis, std::vector<char>* adds) {
  const size_t n = size_t(sys.nvars);
  basis.clear();
  if (!sys.eval(x)) return false;
  std::vector<double> v(n);
  for (size_t i = 0; i < sys.nrows && basis.size() < n; ++i) {
    const Row& r = sys.rows[i];
    std::fill(v.begin(), v.end(), 0.0);
    double len = 0;
    for (const auto& e : r.j) {
      v[size_t(e.first)] = e.second;
      len += e.second * e.second;
    }
    len = std::sqrt(len);
    if (!(len > 1e-14)) continue;
    for (double& c : v) c /= len;
    // First pass against the row as it came (a dozen non-zeros, so the dot products cost nothing); a second,
    // dense pass only when most of the row was removed, which is when one pass loses orthogonality.
    for (const auto& b : basis) {
      double d = 0;
      for (const auto& e : r.j) d += b[size_t(e.first)] * e.second;
      d /= len;
      if (d != 0)
        for (size_t k = 0; k < n; ++k) v[k] -= d * b[k];
    }
    auto norm_of = [&] {
      double q = 0;
      for (double c : v) q += c * c;
      return std::sqrt(q);
    };
    double rest = norm_of();
    if (rest < 0.5) {
      for (const auto& b : basis) {
        double d = 0;
        for (size_t k = 0; k < n; ++k) d += b[k] * v[k];
        for (size_t k = 0; k < n; ++k) v[k] -= d * b[k];
      }
      rest = norm_of();
    }
    if (rest <= kRankTol) continue;
    for (double& c : v) c /= rest;
    basis.push_back(v);
    if (adds && r.owner >= 0) (*adds)[size_t(r.owner)] = 1;
  }
  return true;
}

// dof = variables - rank; a constraint none of whose rows add rank is redundant; a coordinate that is not
// wholly inside the row space still has a share in the null space, i.e. its point can move.
void analyse(System& sys, const std::vector<double>& x, SolveResult& out) {
  const size_t n = size_t(sys.nvars);
  out.dof = int(n);
  std::vector<std::vector<double>> basis;
  std::vector<char> has_rows(sys.sk.constraints.size(), 0), adds(sys.sk.constraints.size(), 0);
  if (!row_basis(sys, x, basis, &adds)) return;
  for (size_t i = 0; i < sys.nrows; ++i)
    if (sys.rows[i].owner >= 0) has_rows[size_t(sys.rows[i].owner)] = 1;
  out.dof = int(n) - int(basis.size());
  for (size_t ci = 0; ci < has_rows.size(); ++ci)
    if (has_rows[ci] && !adds[ci]) out.redundant.push_back(sys.sk.constraints[ci].id);
  if (out.dof == 0) return;
  auto can_move = [&](int g) {
    if (g < 0) return false;
    double in_rows = 0;
    for (const auto& b : basis) in_rows += b[size_t(g)] * b[size_t(g)];
    return 1.0 - in_rows > kFreeTol;
  };
  for (size_t i = 0; i < sys.sk.points.size(); ++i)
    if (can_move(sys.vx[i]) || can_move(sys.vy[i])) out.free_points.push_back(sys.sk.points[i].id);
}

// Dragging. The dragged points never jump to the mouse (projecting back from there lands on mirrored
// solutions: a dimensioned rectangle flips when its corner is pulled through the opposite side). Instead,
// from a solved state, take the step *inside the null space of the constraints* that best closes the gap to
// the targets, with a weak penalty on moving anything (dx = P S^T (S P S^T + eps I)^-1 gap, P the null-space
// projector, S the selector of the dragged coordinates), repair the second-order error with the ordinary
// minimum-norm solve, and repeat. A fully constrained sketch has P = 0 and does not move at all.
void drag(System& sys, std::vector<double>& x, const std::vector<std::pair<size_t, double>>& pulls, const SolveOptions& opt) {
  const size_t n = x.size(), k = pulls.size();
  auto gap2 = [&](const std::vector<double>& at) {
    double s = 0;
    for (const auto& p : pulls) s += (p.second - at[p.first]) * (p.second - at[p.first]);
    return s;
  };

  std::vector<std::vector<double>> cols(k, std::vector<double>(n));
  std::vector<double> A(k * k), w(k), dx(n), probe, rs, M, F, y;
  Columns jc;
  double gap = gap2(x);
  // One mouse event must stay interactive on a big sketch: the m^3 factorisations are the cost, so they are
  // the budget (about a quarter of a second); the next event carries on from where this one stopped.
  const double m3 = std::pow(double(std::max<size_t>(sys.nrows, 1)), 3);
  const int budget = sys.factorisations + int(std::clamp(1.5e9 / m3, 8.0, 200.0));
  for (int outer = 0; outer < 10 && gap > 1e-14 && sys.factorisations < budget; ++outer) {
    // Column a of P S^T = e_a - J^T (J J^T)^-1 J e_a, on unit rows. The small shift keeps dependent
    // (redundant) rows factorable; J e_a lies in the range of J J^T, so it does not blur the projector.
    if (!sys.eval(x)) break;
    const size_t m = sys.nrows;
    rs.assign(m, 1.0);
    for (size_t i = 0; i < m; ++i) {
      double g = 0;
      for (const auto& e : sys.rows[i].j) g += e.second * e.second;
      if (g > 0) rs[i] = 1.0 / std::sqrt(g);
    }
    gram(sys, rs, jc, M);
    bool factored = m == 0;
    for (double shift = 1e-10; !factored && shift < 1e-3; shift *= 100) {
      F = M;
      for (size_t i = 0; i < m; ++i) F[i * m + i] += shift;
      factored = cholesky(F, m);
    }
    if (!factored) break;
    for (size_t a = 0; a < k; ++a) {
      std::fill(cols[a].begin(), cols[a].end(), 0.0);
      cols[a][pulls[a].first] = 1;
      y.assign(m, 0.0);
      for (const auto& e : jc[pulls[a].first]) y[e.first] = e.second;
      cholesky_apply(F, m, y);
      for (size_t j = 0; j < n; ++j)
        for (const auto& e : jc[j]) cols[a][j] -= e.second * y[e.first];
    }
    for (size_t a = 0; a < k; ++a) {
      for (size_t b = 0; b < k; ++b) A[a * k + b] = cols[b][pulls[a].first];
      A[a * k + a] += kDragSpring;
      w[a] = pulls[a].second - x[pulls[a].first];
    }
    if (!cholesky(A, k)) break;
    cholesky_apply(A, k, w);
    std::fill(dx.begin(), dx.end(), 0.0);
    double any = 0;
    for (size_t a = 0; a < k; ++a)
      for (size_t i = 0; i < n; ++i) dx[i] += cols[a][i] * w[a];
    for (size_t i = 0; i < n; ++i) any = std::max(any, std::fabs(dx[i]));
    if (!(any > 1e-9) || !std::isfinite(any)) break;  // cannot move (or no further)
    // The step is tangent to the constraints, so what it breaks is curvature. Shorten it until that error is
    // half the step (about 50 deg of a rotation): long tangent steps are where flips come from, while linear
    // constraints (a rectangle following its corner) never shorten anything.
    double scale = 1;
    bool moved = false;
    for (int halvings = 0; halvings < 40 && !moved; ++halvings, scale *= 0.5) {
      probe = x;
      for (size_t i = 0; i < n; ++i) probe[i] += scale * dx[i];
      if (!sys.eval(probe) || sys.max_residual() > 0.5 * scale * any) continue;
      if (!minimise(sys, probe, opt, false)) continue;
      const double g = gap2(probe);
      if (!(g < gap)) continue;
      moved = true;
      x = probe;
      gap = g;
    }
    if (!moved) break;
  }
  minimise(sys, x, opt);  // the probes stopped at the tolerance; the analysis wants the polished state
}

}  // namespace

static SolveResult solve_system(Sketch& sk, const SolveOptions& opt, bool keep_best) {
  if (!(opt.tolerance > 0) || !std::isfinite(opt.tolerance) || opt.max_iterations < 1 || opt.max_iterations > 10000)
    throw Error("sketch solver: invalid tolerance or iteration limit");
  sk.validate();  // the residuals index by reference kind; a malformed sketch throws here, not deep inside
  System sys(sk);
  SolveResult out;
  const std::vector<double> x0 = sys.start();

  // Drag targets per variable; pinned or unknown points cannot be dragged, and a point named twice follows
  // its last target.
  std::vector<std::pair<size_t, double>> pulls;
  for (const auto& d : opt.drags) {
    auto it = sys.pidx.find(d.point);
    if (it == sys.pidx.end() || sys.vx[size_t(it->second)] < 0) continue;
    if (!std::isfinite(d.x) || !std::isfinite(d.y)) continue;
    const size_t gx = size_t(sys.vx[size_t(it->second)]), gy = size_t(sys.vy[size_t(it->second)]);
    std::erase_if(pulls, [&](const auto& p) { return p.first == gx || p.first == gy; });
    pulls.emplace_back(gx, d.x);
    pulls.emplace_back(gy, d.y);
  }

  std::vector<double> x = x0;
  bool ok = false;
  if (sys.eval(x0, true)) {  // sides, tangency kind, angle branch and Fix values come from the untouched sketch
    ok = minimise(sys, x, opt);
    if (ok && !pulls.empty()) drag(sys, x, pulls, opt);
  }

  // Report on the state reached, then either keep it or leave the sketch untouched.
  const bool finite = sys.eval(x);
  out.converged = ok && finite;
  out.residual = finite ? sys.max_residual() : INFINITY;
  if (!out.converged) {
    for (size_t i = 0; i < sys.nrows; ++i) {
      const Row& r = sys.rows[i];
      if (std::fabs(r.f) <= opt.tolerance) continue;
      // An arc that cannot be kept round has no constraint to blame: its own id stands in.
      const int id = r.owner >= 0 ? sk.constraints[size_t(r.owner)].id : sk.entities[size_t(-1 - r.owner)].id;
      if (std::find(out.failed.begin(), out.failed.end(), id) == out.failed.end()) out.failed.push_back(id);
    }
    // Residuals fine but a circle collapsed: blame whatever acts on it.
    for (size_t ei = 0; ei < sk.entities.size() && finite; ++ei) {
      if (sys.vr[ei] < 0 || x[size_t(sys.vr[ei])] > kMinRadius) continue;
      for (const auto& c : sk.constraints)
        if (std::find(c.refs.begin(), c.refs.end(), sk.entities[ei].id) != c.refs.end() &&
            std::find(out.failed.begin(), out.failed.end(), c.id) == out.failed.end())
          out.failed.push_back(c.id);
    }
  }
  const bool keep = out.converged || (keep_best && finite);
  analyse(sys, keep ? x : x0, out);
  if (keep) {
    sys.store(sk, x);
    for (auto& c : sk.constraints) if (c.reference && c.is_dimension()) c.value = dimension_value(sk, c);
  }
  return out;
}

SolveResult solve(Sketch& sk, const SolveOptions& opt, bool keep_best) {
  sk.validate();
  // Disconnected islands do not share variables: solve their small systems instead of one dense matrix.
  // ID ordering makes the partition and the floating-point operation order deterministic.
  std::map<int, int> parent;
  for (const auto& p : sk.points) parent[p.id] = p.id;
  for (const auto& e : sk.entities) parent[e.id] = e.id;
  auto root = [&](int id) { while (parent.at(id) != id) { parent[id] = parent.at(parent[id]); id = parent[id]; } return id; };
  auto join = [&](int a, int b) { a=root(a); b=root(b); if (a!=b) parent[std::max(a,b)]=std::min(a,b); };
  for (const auto& e : sk.entities) for (int p : e.p) join(e.id,p);
  for (const auto& c : sk.constraints) if (!c.reference)
    for (size_t i=1; i<c.refs.size(); ++i) join(c.refs[0],c.refs[i]);
  // Reference dimensions can span independent islands; measure them only after all islands are solved.
  std::map<int, Sketch> islands;
  for (const auto& p : sk.points) islands[root(p.id)].points.push_back(p);
  for (const auto& e : sk.entities) islands[root(e.id)].entities.push_back(e);
  for (const auto& c : sk.constraints) if (!c.reference) islands[root(c.refs[0])].constraints.push_back(c);
  if (islands.size() < 2) return solve_system(sk,opt,keep_best);
  SolveResult out; out.converged=true;
  std::map<int, SkPoint> points;
  std::map<int, double> radii;
  for (auto& [id, part] : islands) {
    SolveOptions local=opt;
    std::erase_if(local.drags,[&](const auto& d) { return !parent.count(d.point) || root(d.point)!=id; });
    const auto r=solve_system(part,local,keep_best);
    out.converged &= r.converged; out.dof+=r.dof; out.residual=std::max(out.residual,r.residual);
    out.failed.insert(out.failed.end(),r.failed.begin(),r.failed.end());
    out.redundant.insert(out.redundant.end(),r.redundant.begin(),r.redundant.end());
    out.free_points.insert(out.free_points.end(),r.free_points.begin(),r.free_points.end());
    for (const auto& p : part.points) points[p.id]=p;
    for (const auto& e : part.entities) radii[e.id]=e.r;
  }
  if (out.converged || keep_best) {
    for (auto& p : sk.points) p=points.at(p.id);
    for (auto& e : sk.entities) e.r=radii.at(e.id);
    for (auto& c : sk.constraints) if (c.reference && c.is_dimension()) c.value=dimension_value(sk,c);
  }
  std::sort(out.free_points.begin(),out.free_points.end());
  std::sort(out.failed.begin(),out.failed.end());
  std::sort(out.redundant.begin(),out.redundant.end());
  return out;
}

}  // namespace opad::design
