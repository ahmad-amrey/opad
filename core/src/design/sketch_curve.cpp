#include "opad/design/sketch_curve.hpp"

#include <GeomAPI_Interpolate.hxx>
#include <GeomAPI_ProjectPointOnCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_HArray1OfPnt.hxx>

#include <algorithm>
#include <cmath>
#include <set>

#include "opad/design/sketch_geom.hpp"

namespace opad::design {
namespace {

constexpr size_t kMostSamples = 20000;

struct Spec {
  std::string x, y;
  Quantity t0, t1;
  double tolerance = 0.01;
  int start = 64;
};

bool is_equation(const SkEntity& e) { return e.type == SkEntity::Type::Spline && !e.equation.is_null(); }

Spec spec_of(const json& eq, const ParamTable& params) {
  auto bound = [&](const char* key, double fallback) {
    if (!eq.contains(key)) return Quantity{fallback, 0, false};
    const json& v = eq.at(key);
    if (v.is_number()) return Quantity{v.get<double>(), 0, false};
    if (v.is_string()) return params.eval(v.get<std::string>());
    throw Error(std::string("an equation's ") + key + " is a number or an expression");
  };
  Spec s;
  s.x = eq.at("x").get<std::string>();
  s.y = eq.at("y").get<std::string>();
  s.t0 = bound("t0", 0);
  s.t1 = bound("t1", 1);
  if (s.t0.len != 0 || s.t1.len != 0 || s.t0.angle != s.t1.angle) throw Error("an equation's t0 and t1 are both plain numbers or both angles");
  if (!(s.t1.value > s.t0.value)) throw Error("an equation's t1 must be above its t0");
  s.tolerance = eq.value("tolerance", 0.01);
  s.start = std::clamp(eq.value("min_points", 64), 8, 5000);
  return s;
}

gp_Pnt point_at(const Spec& s, const ParamTable& params, double t) {
  // A plain t is never taken as degrees (derived): give t0 and t1 in deg for an angle.
  const Quantity at{t, 0, s.t0.angle, !s.t0.angle};
  return gp_Pnt(params.as_with(Dim::Length, s.x, "t", at), params.as_with(Dim::Length, s.y, "t", at), 0);
}

Handle(Geom_BSplineCurve) through(const std::vector<gp_Pnt>& pts, bool closed) {
  if (closed) return closed_spline(pts);
  Handle(TColgp_HArray1OfPnt) arr = new TColgp_HArray1OfPnt(1, static_cast<int>(pts.size()));
  for (size_t i = 0; i < pts.size(); ++i) arr->SetValue(static_cast<int>(i) + 1, pts[i]);
  GeomAPI_Interpolate fit(arr, Standard_False, 1e-9);
  fit.Perform();
  return fit.IsDone() ? fit.Curve() : Handle(Geom_BSplineCurve)();
}

// Samples until the spline through them is within the tolerance at a third and two thirds of every span; a span
// that is not gets its midpoint. The spline passes sample i at its knot i (both interpolations put knots there).
std::vector<gp_Pnt> sample(const Spec& s, const ParamTable& params, bool& closed) {
  const double a = s.t0.value, b = s.t1.value;
  closed = point_at(s, params, a).Distance(point_at(s, params, b)) <= s.tolerance;
  std::vector<double> ts;
  for (int i = 0; i < s.start; ++i) ts.push_back(a + (b - a) * i / s.start);
  if (!closed) ts.push_back(b);
  for (int round = 0; round < 24; ++round) {
    std::vector<gp_Pnt> pts;
    for (double t : ts) pts.push_back(point_at(s, params, t));
    const Handle(Geom_BSplineCurve) curve = through(pts, closed);
    if (curve.IsNull()) throw Error("the equation's samples do not make a curve (two of them coincide)");
    const size_t spans = closed ? ts.size() : ts.size() - 1;
    std::vector<double> next;
    for (size_t i = 0; i < spans; ++i) {
      const double ta = ts[i], tb = i + 1 < ts.size() ? ts[i + 1] : b;
      const double ka = curve->Knot(static_cast<int>(i) + 1), kb = curve->Knot(static_cast<int>(i) + 2);
      next.push_back(ta);
      for (double f : {1.0 / 3, 2.0 / 3}) {
        const gp_Pnt p = point_at(s, params, ta + (tb - ta) * f);
        GeomAPI_ProjectPointOnCurve on(p, curve, ka, kb);
        const double off = on.NbPoints() ? on.LowerDistance() : std::min(p.Distance(curve->Value(ka)), p.Distance(curve->Value(kb)));
        if (off > s.tolerance) {
          next.push_back(0.5 * (ta + tb));
          break;
        }
      }
    }
    if (!closed) next.push_back(ts.back());
    if (next.size() == ts.size()) return pts;
    if (next.size() > kMostSamples) throw Error("the equation needs more than " + std::to_string(kMostSamples) + " points for that tolerance");
    ts = std::move(next);
  }
  throw Error("the equation's samples did not settle within its tolerance");
}

// The samples into the entity's points: the ends keep their ids (lines joined there stay joined), interior ids are
// reused in order, then new fixed points are added and spare ones removed if nothing else uses them.
void place(Sketch& sk, size_t entity, const std::vector<gp_Pnt>& pts, bool closed) {
  std::vector<int> old = sk.entities[entity].p;
  if (old.size() > 1 && old.front() == old.back()) old.pop_back();
  const size_t n = pts.size();
  std::vector<int> ids(n, 0);
  size_t next_old = 0;
  if (!closed && old.size() >= 2) {
    ids.front() = old.front();
    ids.back() = old.back();
    old = std::vector<int>(old.begin() + 1, old.end() - 1);
  }
  for (size_t i = 0; i < n; ++i) {
    if (ids[i]) continue;
    if (next_old < old.size()) ids[i] = old[next_old++];
  }
  std::set<int> spare(old.begin() + static_cast<std::ptrdiff_t>(std::min(next_old, old.size())), old.end());
  for (size_t i = 0; i < n; ++i) {
    if (SkPoint* p = ids[i] ? sk.point(ids[i]) : nullptr) {
      p->x = pts[i].X();
      p->y = pts[i].Y();
      p->fixed = true;
    } else {
      ids[i] = sk.add_point(pts[i].X(), pts[i].Y(), true);
    }
  }
  sk.entities[entity].p = ids;
  if (closed) sk.entities[entity].p.push_back(ids.front());
  // Spare points go unless another curve or a constraint still uses them.
  for (const auto& e : sk.entities)
    for (int id : e.p) spare.erase(id);
  for (const auto& c : sk.constraints)
    for (int id : c.refs) spare.erase(id);
  sk.points.erase(std::remove_if(sk.points.begin(), sk.points.end(), [&](const SkPoint& p) { return spare.count(p.id) > 0; }), sk.points.end());
}

}  // namespace

bool has_equation_curves(const Sketch& sk) { return std::any_of(sk.entities.begin(), sk.entities.end(), is_equation); }

std::string equation_inputs(const Sketch& sk, const ParamTable& params) {
  std::string s;
  for (const auto& e : sk.entities) {
    if (!is_equation(e)) continue;
    s += std::to_string(e.id) + e.equation.dump() + "{";
    for (const char* key : {"x", "y", "t0", "t1"}) {
      if (!e.equation.contains(key) || !e.equation[key].is_string()) continue;
      for (const auto& name : expr_identifiers(e.equation[key].get<std::string>())) {
        if (name == "t") continue;
        try {
          const Quantity q = params.value_of(name);
          s += name + "=" + json(q.value).dump() + ";";
        } catch (const std::exception&) {
          s += name + "=?;";
        }
      }
    }
    s += "}";
  }
  return s;
}

void evaluate_curves(Sketch& sk, const ParamTable& params) {
  for (size_t i = 0; i < sk.entities.size(); ++i) {
    if (!is_equation(sk.entities[i])) continue;
    try {
      bool closed = false;
      const std::vector<gp_Pnt> pts = sample(spec_of(sk.entities[i].equation, params), params, closed);
      place(sk, i, pts, closed);
    } catch (const Standard_Failure& ex) {
      throw Error("equation curve " + std::to_string(sk.entities[i].id) + ": " + ex.GetMessageString());
    } catch (const std::exception& ex) {
      throw Error("equation curve " + std::to_string(sk.entities[i].id) + ": " + ex.what());
    }
  }
}

}  // namespace opad::design
