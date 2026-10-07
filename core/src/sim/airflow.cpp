#include "opad/sim/airflow.hpp"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>

namespace opad::sim::air {

namespace {
constexpr double kCfm = 4.719474e-4;    // m3/s
constexpr double kMmH2O = 9.80665;      // Pa
double dotv(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 scaled(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 minus(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 plus(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 crossv(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 unitv(const Vec3& a) {
  const double n = std::sqrt(dotv(a, a));
  return n > 1e-12 ? scaled(a, 1 / n) : Vec3{0, 0, 0};
}
}  // namespace

Air properties(double T_C, double p_Pa) {
  Air a;
  a.T = T_C;
  const double T = T_C + 273.15;
  a.rho = p_Pa / (287.05 * T);
  a.mu = 1.716e-5 * std::pow(T / 273.15, 1.5) * (273.15 + 110.4) / (T + 110.4);   // Sutherland
  a.k = 0.0241 * std::pow(T / 273.15, 1.5) * (273.15 + 194.0) / (T + 194.0);      // Sutherland's form for conductivity
  a.cp = 1006.0;
  a.nu = a.mu / a.rho;
  a.alpha = a.k / (a.rho * a.cp);
  a.Pr = a.nu / a.alpha;
  a.beta = 1 / T;
  return a;
}

double natural_h(double up, double L, double Ts, double Tinf, double gap, double g) {
  double dT = Ts - Tinf;
  if (std::fabs(dT) < 0.05) dT = dT < 0 ? -0.05 : 0.05;  // still air at the air's temperature: the smallest h, never zero
  const Air a = properties(0.5 * (Ts + Tinf));
  const double ga = g * a.beta * std::fabs(dT) / (a.nu * a.alpha);  // Ra = ga L^3
  up = std::clamp(up, -1.0, 1.0);
  const bool vertical = std::fabs(up) <= 0.7072;  // within 45 deg of vertical
  if (vertical && gap > 0 && L > 0) {
    // Two plates s apart (fins): Bar-Cohen and Rohsenow, symmetric isothermal plates. El = Ra_s s / L.
    const double s = gap;
    const double El = std::max(1e-9, ga * s * s * s * s / L * std::sqrt(1 - up * up));
    const double Nu = std::pow(576 / (El * El) + 2.873 / std::sqrt(El), -0.5);
    return Nu * a.k / s;
  }
  if (vertical) {
    // Churchill and Chu, the whole range; a tilted plate with gravity's component along it.
    const double Ra = std::max(1e-9, ga * L * L * L * std::sqrt(1 - up * up));
    const double Nu = std::pow(0.825 + 0.387 * std::pow(Ra, 1.0 / 6) / std::pow(1 + std::pow(0.492 / a.Pr, 9.0 / 16), 8.0 / 27), 2);
    return Nu * a.k / L;
  }
  const double Ra = std::max(1e-9, ga * L * L * L);
  if (Ra < 1e4) {
    // Below the level-plate correlations' range (a fin's narrow top): the strip as a horizontal cylinder of its width
    // (2 A / P), Churchill and Chu, which holds down to very small Rayleigh numbers.
    const double D = 2 * L, RaD = ga * D * D * D;
    const double Nu = std::pow(0.6 + 0.387 * std::pow(RaD, 1.0 / 6) / std::pow(1 + std::pow(0.559 / a.Pr, 9.0 / 16), 8.0 / 27), 2);
    return Nu * a.k / D;
  }
  const bool rising = (dT > 0) == (up > 0);  // hot looking up, or cold looking down: the plume leaves the face
  const double Nu = rising ? (Ra < 1e7 ? 0.54 * std::pow(Ra, 0.25) : 0.15 * std::cbrt(Ra))  // Lloyd and Moran
                           : 0.52 * std::pow(Ra, 0.2);                                       // Raithby and Hollands
  return Nu * a.k / L;
}

double forced_plate_h(double U, double L, double T) {
  const Air a = properties(T);
  const double Re = std::max(1.0, std::fabs(U) * L / a.nu);
  const double Nu = Re < 5e5 ? 0.664 * std::sqrt(Re) * std::cbrt(a.Pr) : (0.037 * std::pow(Re, 0.8) - 871) * std::cbrt(a.Pr);
  return Nu * a.k / L;
}

json FinArray::to_json() const {
  return {{"fins", fins}, {"fin_thickness_mm", t * 1e3}, {"gap_mm", gap * 1e3}, {"fin_height_mm", height * 1e3}, {"length_mm", length * 1e3},
          {"width_mm", width * 1e3}, {"base_mm", base * 1e3}, {"flow", flow}, {"fins_across", across}, {"fins_up", up}};
}

std::optional<FinArray> fin_array(const TopoDS_Shape& body, const Vec3& flow_in) {
  const Vec3 d = unitv(flow_in);
  if (dotv(d, d) < 0.5) return std::nullopt;
  // Two directions across the flow: the world axes least along it.
  Vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  std::sort(std::begin(axes), std::end(axes), [&](const Vec3& a, const Vec3& b) { return std::fabs(dotv(a, d)) < std::fabs(dotv(b, d)); });
  const Vec3 e1 = unitv(minus(axes[0], scaled(d, dotv(axes[0], d))));
  const Vec3 e2 = unitv(crossv(d, e1));
  Bnd_Box box;
  BRepBndLib::AddOptimal(body, box, false, false);
  if (box.IsVoid()) return std::nullopt;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  auto extent = [&](const Vec3& v, double& lo, double& hi) {
    lo = 1e300, hi = -1e300;
    for (double x : {x0, x1})
      for (double y : {y0, y1})
        for (double z : {z0, z1}) {
          const double p = dotv({x, y, z}, v);
          lo = std::min(lo, p), hi = std::max(hi, p);
        }
  };
  IntCurvesFace_ShapeIntersector hit;
  hit.Load(body, 1e-7);
  // The solid's intervals along a line: entry and exit pairs of its sorted crossings.
  auto intervals = [&](const Vec3& origin, const Vec3& dir, double from, double to) {
    const gp_Lin line(gp_Pnt(origin[0], origin[1], origin[2]), gp_Dir(dir[0], dir[1], dir[2]));
    hit.Perform(line, from - 1, to + 1);
    std::vector<double> w;
    for (int i = 1; hit.IsDone() && i <= hit.NbPnt(); ++i) w.push_back(hit.WParameter(i));
    std::sort(w.begin(), w.end());
    w.erase(std::unique(w.begin(), w.end(), [](double a, double b) { return std::fabs(a - b) < 1e-6; }), w.end());
    std::vector<std::array<double, 2>> out;
    for (size_t i = 0; i + 1 < w.size(); i += 2) out.push_back({w[i], w[i + 1]});
    return out;
  };
  double dl, dh;
  extent(d, dl, dh);
  const double dm = 0.5 * (dl + dh);
  struct Pick {
    Vec3 across, up;
    int fins = 0;
    std::vector<int> counts;
    std::vector<double> levels;
    double lo = 0, hi = 0, olo = 0, ohi = 0;
  } best;
  constexpr int kLevels = 160;
  for (int which = 0; which < 2; ++which) {
    const Vec3 c = which ? e2 : e1, o = which ? e1 : e2;
    Pick p;
    p.across = c;
    extent(c, p.lo, p.hi);
    extent(o, p.olo, p.ohi);
    for (int i = 0; i < kLevels; ++i) {
      const double lev = p.olo + (p.ohi - p.olo) * (i + 0.5) / kLevels;
      const Vec3 origin = plus(plus(scaled(d, dm), scaled(o, lev)), scaled(c, 0));
      const int n = int(intervals(origin, c, p.lo, p.hi).size());
      p.counts.push_back(n);
      p.levels.push_back(lev);
      p.fins = std::max(p.fins, n);
    }
    p.up = o;
    if (p.fins > best.fins) best = p;
  }
  if (best.fins < 3) return std::nullopt;
  // The base: levels crossed once; the fins: levels crossed fins times (allowing one less at a ragged tip).
  int first_fin = -1, last_fin = -1, base_lo = -1, base_hi = -1;
  for (int i = 0; i < kLevels; ++i) {
    if (best.counts[size_t(i)] >= best.fins - 1 && best.counts[size_t(i)] >= 3) {
      if (first_fin < 0) first_fin = i;
      last_fin = i;
    }
    if (best.counts[size_t(i)] == 1) {
      if (base_lo < 0) base_lo = i;
      base_hi = i;
    }
  }
  if (first_fin < 0) return std::nullopt;
  const double step = (best.ohi - best.olo) / kLevels;
  FinArray f;
  f.fins = best.fins;
  f.flow = d;
  f.across = best.across;
  // The base on the side of the fins its levels are on.
  const bool base_below = base_lo >= 0 && base_hi < first_fin;
  f.up = base_below || base_lo < 0 ? best.up : scaled(best.up, -1);
  f.base = base_lo >= 0 ? (base_hi - base_lo + 1) * step : 0;
  f.height = (last_fin - first_fin + 1) * step;
  // Thickness and gap half way up the fins.
  const double mid = best.levels[size_t((first_fin + last_fin) / 2)];
  const auto iv = intervals(plus(scaled(d, dm), scaled(best.up, mid)), best.across, best.lo, best.hi);
  double t = 0, gap = 0;
  for (size_t i = 0; i < iv.size(); ++i) {
    t += iv[i][1] - iv[i][0];
    if (i) gap += iv[i][0] - iv[i - 1][1];
  }
  f.t = t / double(iv.size());
  f.gap = iv.size() > 1 ? gap / double(iv.size() - 1) : 0;
  f.width = iv.empty() ? 0 : iv.back()[1] - iv.front()[0];
  // Length: along the flow through the middle of a fin.
  const double fin_mid = 0.5 * (iv[iv.size() / 2][0] + iv[iv.size() / 2][1]);
  const auto along = intervals(plus(scaled(best.up, mid), scaled(best.across, fin_mid)), d, dl, dh);
  f.length = 0;
  for (const auto& a : along) f.length += a[1] - a[0];
  // mm -> m
  for (double* v : {&f.t, &f.gap, &f.height, &f.length, &f.width, &f.base}) *v *= 1e-3;
  if (f.gap <= 0 || f.length <= 0) return std::nullopt;
  return f;
}

Channel channel(const FinArray& f, double Q, double T) {
  const Air a = properties(T);
  Channel c{};
  const double A = f.open();
  if (A <= 0) return c;
  c.V = Q / A;
  const double b = f.gap, H = f.height;
  c.Dh = 2 * b * H / (b + H);
  c.Re = c.V * c.Dh / a.nu;
  // Pressure drop: developing laminar flow in rectangular channels (Muzychka and Yovanovich), entrance and exit losses.
  const double ar = std::min(b, H) / std::max(b, H);
  const double fRe = 24 * (1 - 1.3553 * ar + 1.9467 * ar * ar - 1.7012 * std::pow(ar, 3) + 0.9564 * std::pow(ar, 4) - 0.2537 * std::pow(ar, 5));
  const double Lstar = f.length / (c.Dh * std::max(c.Re, 1e-9));
  const double fappRe = std::sqrt(std::pow(3.44 / std::sqrt(std::max(Lstar, 1e-12)), 2) + fRe * fRe);
  const double fapp = fappRe / std::max(c.Re, 1e-9);
  const double s2 = std::pow(f.sigma(), 2);
  const double Kc = 0.42 * (1 - s2), Ke = (1 - s2) * (1 - s2);
  c.dp = (fapp * 4 * f.length / c.Dh + Kc + Ke) * 0.5 * a.rho * c.V * c.V;
  // Heat transfer: Teertstra, Yovanovich and Culham, from fully developed to developing flow between plates.
  const double Reb = c.V * b / a.nu;
  const double Res = std::max(1e-9, Reb * b / f.length);
  const double Nu = std::pow(std::pow(Res * a.Pr / 2, -3) + std::pow(0.664 * std::sqrt(Res) * std::cbrt(a.Pr) * std::sqrt(1 + 3.65 / std::sqrt(Res)), -3), -1.0 / 3);
  c.h = Nu * a.k / b;
  return c;
}

double Fan::pressure(double Q) const {
  if (Q <= 0) return curve.empty() ? Pmax : curve.back()[1];
  if (curve.size() >= 2) {
    // Points from free flow to shut-off, or the other way: by flow.
    auto pts = curve;
    std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    if (Q >= pts.back()[0]) return 0;
    for (size_t i = 1; i < pts.size(); ++i)
      if (Q <= pts[i][0]) return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (Q - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
    return 0;
  }
  return std::max(0.0, Pmax * (1 - Q / Qmax));  // a straight line between shut-off and free flow
}

json Fan::to_json() const {
  json j = {{"id", id}, {"name", name}, {"free_flow_m3h", Qmax * 3600}, {"free_flow_cfm", Qmax / kCfm}, {"shut_off_Pa", Pmax}};
  if (size > 0) j["size_mm"] = size;
  if (rpm > 0) j["rpm"] = rpm;
  return j;
}

const std::vector<Fan>& fans() {
  // Typical datasheet values for fans of each size and speed (axial, 12 V); a fan's own curve is better: fan_from.
  static const std::vector<Fan> list = [] {
    std::vector<Fan> v;
    auto add = [&](const char* id, const char* name, double size, double rpm, double m3h, double Pa) {
      Fan f;
      f.id = id, f.name = name, f.size = size, f.rpm = rpm, f.Qmax = m3h / 3600, f.Pmax = Pa;
      v.push_back(f);
    };
    add("40x10", "40 mm fan, 4500 rpm", 40, 4500, 8.2, 17.5);
    add("40x28-server", "40 mm server fan, 15000 rpm", 40, 15000, 42, 590);
    add("60x15", "60 mm fan, 3000 rpm", 60, 3000, 29.7, 21.6);
    add("80x25", "80 mm fan, 2200 rpm", 80, 2200, 55.5, 23.2);
    add("92x25", "92 mm fan, 2000 rpm", 92, 2000, 78.9, 27.8);
    add("120x25", "120 mm fan, 1500 rpm", 120, 1500, 102, 22.9);
    add("120x25-high", "120 mm fan, 3000 rpm", 120, 3000, 200, 80);
    add("140x25", "140 mm fan, 1500 rpm", 140, 1500, 140, 20);
    return v;
  }();
  return list;
}

const Fan* fan(const std::string& id) {
  for (const auto& f : fans())
    if (f.id == id) return &f;
  return nullptr;
}

Fan fan_from(const json& spec) {
  if (spec.is_string()) {
    if (const Fan* f = fan(spec.get<std::string>())) return *f;
    std::string ids;
    for (const auto& f : fans()) ids += (ids.empty() ? "" : ", ") + f.id;
    throw Error("fan: no fan \"" + spec.get<std::string>() + "\" (" + ids + ", or {\"flow\": m3/h, \"pressure\": Pa})");
  }
  if (!spec.is_object()) throw Error("fan: a library id or {\"flow\": m3/h, \"pressure\": Pa, \"curve\": [[m3/h, Pa], ...]}");
  Fan f;
  f.id = "custom";
  f.name = spec.value("name", std::string("Fan"));
  if (spec.contains("flow")) f.Qmax = spec["flow"].get<double>() / 3600;
  if (spec.contains("cfm")) f.Qmax = spec["cfm"].get<double>() * kCfm;
  if (spec.contains("pressure")) f.Pmax = spec["pressure"].get<double>();
  if (spec.contains("mmH2O")) f.Pmax = spec["mmH2O"].get<double>() * kMmH2O;
  if (spec.contains("curve")) {
    for (const auto& p : spec["curve"]) f.curve.push_back({p.at(0).get<double>() / 3600, p.at(1).get<double>()});
    if (f.curve.size() < 2) throw Error("fan: a curve needs two points or more ([m3/h, Pa])");
    for (const auto& p : f.curve) f.Qmax = std::max(f.Qmax, p[0]), f.Pmax = std::max(f.Pmax, p[1]);
  }
  if (!(f.Qmax > 0) || !(f.Pmax > 0)) throw Error("fan: give its free flow (flow m3/h or cfm) and shut-off pressure (pressure Pa or mmH2O), or its curve");
  return f;
}

double operating_point(const Fan& fan, const std::function<double(double)>& system, int n) {
  n = std::max(1, n);
  auto g = [&](double Q) { return fan.pressure(Q / n) - system(Q); };
  double lo = 0, hi = fan.Qmax * n;
  if (g(lo) <= 0) return 0;
  for (int i = 0; i < 200; ++i) {
    const double m = 0.5 * (lo + hi);
    (g(m) > 0 ? lo : hi) = m;
  }
  return 0.5 * (lo + hi);
}

}  // namespace opad::sim::air
