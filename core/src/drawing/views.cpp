// Section, detail, auxiliary, cropped and broken views on a sheet (TODO 11 UI-82): the linework clipped to a crop box or a
// detail circle and closed up across breaks (curves cut exactly at the boundary), hatching of section faces (ISO
// 128-50), the cutting lines, detail circles and labels the views draw, and break lines.
#include <Geom2dAPI_InterCurveCurve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <Geom2d_Circle.hxx>
#include <Geom2d_Ellipse.hxx>
#include <Geom2d_Line.hxx>
#include <Geom2d_TrimmedCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <gp_Ax22d.hxx>
#include <gp_Lin2d.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "opad/drawing/sheet.hpp"
#include "opad/materials.hpp"
#include "projection_internal.hpp"
#include "views_internal.hpp"

namespace opad::drawing {
namespace {

constexpr double kTau = 2 * M_PI;

Vec2 add(Vec2 a, Vec2 b) { return {a[0] + b[0], a[1] + b[1]}; }
Vec2 sub(Vec2 a, Vec2 b) { return {a[0] - b[0], a[1] - b[1]}; }
Vec2 mul(Vec2 a, double s) { return {a[0] * s, a[1] * s}; }
double dot(Vec2 a, Vec2 b) { return a[0] * b[0] + a[1] * b[1]; }
double len(Vec2 a) { return std::hypot(a[0], a[1]); }
Vec2 unit(Vec2 a, Vec2 fallback = {1, 0}) {
  const double l = len(a);
  return l > 1e-12 ? mul(a, 1 / l) : fallback;
}
Vec2 left(Vec2 a) { return {-a[1], a[0]}; }

// What a frame keeps of its view: inside the crop box and the circle, outside every break's band.
struct Region {
  bool box = false, round = false;
  std::array<double, 4> crop{0, 0, 0, 0};
  Vec2 c{0, 0};
  double r = 0;
  std::vector<ViewFrame::Break> bands;
  explicit Region(const ViewFrame& f) : box(f.crop[2] > f.crop[0]), round(f.radius > 0), crop(f.crop), c(f.circle), r(f.radius), bands(f.breaks) {}
  bool inside(Vec2 p) const {
    if (box && (p[0] < crop[0] || p[0] > crop[2] || p[1] < crop[1] || p[1] > crop[3])) return false;
    if (round && len(sub(p, c)) > r) return false;
    for (const auto& b : bands)
      if (p[static_cast<size_t>(b.axis)] > b.from && p[static_cast<size_t>(b.axis)] < b.to) return false;
    return true;
  }
  // 1: wholly inside, -1: wholly outside, 0: may cross the boundary (a box holding the curve).
  int classify(const std::array<double, 4>& b) const {
    if (box && (b[2] < crop[0] || b[0] > crop[2] || b[3] < crop[1] || b[1] > crop[3])) return -1;
    if (round) {
      const double dx = std::max({c[0] - b[2], 0.0, b[0] - c[0]}), dy = std::max({c[1] - b[3], 0.0, b[1] - c[1]});
      if (std::hypot(dx, dy) > r) return -1;
    }
    for (const auto& x : bands)
      if (b[x.axis] >= x.from && b[x.axis + 2] <= x.to) return -1;
    bool in = !box || (b[0] >= crop[0] && b[2] <= crop[2] && b[1] >= crop[1] && b[3] <= crop[3]);
    if (round)
      for (int k = 0; k < 4 && in; ++k) in = len(sub({k & 1 ? b[2] : b[0], k & 2 ? b[3] : b[1]}, c)) <= r;
    for (const auto& x : bands)
      in = in && (b[x.axis + 2] <= x.from || b[x.axis] >= x.to);
    return in ? 1 : 0;
  }
  // The boundary as 2D curves: the box's sides and the bands' edges as lines, the circle.
  std::vector<Handle(Geom2d_Curve)> boundary() const {
    std::vector<Handle(Geom2d_Curve)> out;
    const auto line = [&](Vec2 p, Vec2 d) { out.push_back(new Geom2d_TrimmedCurve(new Geom2d_Line(gp_Pnt2d(p[0], p[1]), gp_Dir2d(d[0], d[1])), -1e7, 1e7)); };
    if (box) line({crop[0], 0}, {0, 1}), line({crop[2], 0}, {0, 1}), line({0, crop[1]}, {1, 0}), line({0, crop[3]}, {1, 0});
    for (const auto& b : bands)
      for (double v : {b.from, b.to}) b.axis ? line({0, v}, {1, 0}) : line({v, 0}, {0, 1});
    if (round) out.push_back(new Geom2d_Circle(gp_Ax2d(gp_Pnt2d(c[0], c[1]), gp_Dir2d(1, 0)), r));
    return out;
  }
};

std::array<double, 4> rough_box(const Curve& k) {
  if (k.type == Curve::Type::Arc || k.type == Curve::Type::Ellipse) return {k.c[0] - k.r1, k.c[1] - k.r1, k.c[0] + k.r1, k.c[1] + k.r1};
  std::array<double, 4> b{1e300, 1e300, -1e300, -1e300};
  for (const auto& p : k.pts) b = {std::min(b[0], p[0]), std::min(b[1], p[1]), std::max(b[2], p[0]), std::max(b[3], p[1])};
  return b;
}

// Where a segment a -> b crosses the region's boundary (parameters in (0, 1)).
void segment_cuts(Vec2 a, Vec2 b, const Region& g, std::vector<double>& t) {
  const Vec2 d = sub(b, a);
  const auto across = [&](int axis, double v) {
    const double den = d[static_cast<size_t>(axis)];
    if (std::fabs(den) > 1e-15) t.push_back((v - a[static_cast<size_t>(axis)]) / den);
  };
  if (g.box) across(0, g.crop[0]), across(0, g.crop[2]), across(1, g.crop[1]), across(1, g.crop[3]);
  for (const auto& x : g.bands) across(x.axis, x.from), across(x.axis, x.to);
  if (g.round) {  // |a + t d - c| = r
    const Vec2 f = sub(a, g.c);
    const double A = dot(d, d), B = 2 * dot(f, d), C = dot(f, f) - g.r * g.r, D = B * B - 4 * A * C;
    if (A > 1e-30 && D >= 0) t.push_back((-B - std::sqrt(D)) / (2 * A)), t.push_back((-B + std::sqrt(D)) / (2 * A));
  }
}

// The pieces [t_i, t_i+1] of [t0, t1] whose middle the region keeps.
std::vector<std::pair<double, double>> kept(std::vector<double> t, double t0, double t1, const std::function<Vec2(double)>& at, const Region& g) {
  t.push_back(t0), t.push_back(t1);
  std::sort(t.begin(), t.end());
  std::vector<std::pair<double, double>> out;
  const double eps = 1e-12 * std::max(1.0, std::fabs(t1 - t0));
  for (size_t i = 1; i < t.size(); ++i) {
    const double a = std::max(t0, t[i - 1]), b = std::min(t1, t[i]);
    if (b - a <= eps || !g.inside(at(0.5 * (a + b)))) continue;
    if (!out.empty() && a - out.back().second <= eps) out.back().second = b;
    else out.push_back({a, b});
  }
  return out;
}

Handle(Geom2d_Curve) geom_of(const Curve& k) {
  if (k.type == Curve::Type::Arc) return new Geom2d_Circle(gp_Ax2d(gp_Pnt2d(k.c[0], k.c[1]), gp_Dir2d(1, 0)), k.r1);
  if (k.type == Curve::Type::Ellipse)
    return new Geom2d_Ellipse(gp_Ax22d(gp_Pnt2d(k.c[0], k.c[1]), gp_Dir2d(std::cos(k.rot), std::sin(k.rot)), gp_Dir2d(-std::sin(k.rot), std::cos(k.rot))), k.r1, k.r2);
  return detail::curve2d(k);
}

void clip_curve(const Curve& k, const Region& g, std::vector<Curve>& out) {
  const int where = g.classify(rough_box(k));
  if (where > 0) {
    out.push_back(k);
    return;
  }
  if (where < 0) return;
  if (k.type == Curve::Type::Line || k.type == Curve::Type::Polyline) {
    Curve run = k;
    run.pts.clear();
    const auto flush = [&] {
      if (run.pts.size() >= 2) {
        if (k.type == Curve::Type::Line && run.pts.size() > 2) run.type = Curve::Type::Polyline;
        out.push_back(run);
      }
      run.pts.clear();
    };
    for (size_t i = 1; i < k.pts.size(); ++i) {
      const Vec2 a = k.pts[i - 1], b = k.pts[i];
      std::vector<double> t;
      segment_cuts(a, b, g, t);
      const auto at = [&](double s) { return add(a, mul(sub(b, a), s)); };
      for (const auto& [s0, s1] : kept(t, 0, 1, at, g)) {
        const Vec2 p = at(s0), q = at(s1);
        if (run.pts.empty() || len(sub(run.pts.back(), p)) > 1e-9) {
          flush();
          run.pts.push_back(p);
        }
        run.pts.push_back(q);
      }
    }
    flush();
    return;
  }
  Handle(Geom2d_Curve) geom;
  try {
    geom = geom_of(k);
  } catch (const Standard_Failure&) {
  }
  if (geom.IsNull()) return;
  double t0 = k.a0, t1 = k.a1;
  if (k.type == Curve::Type::Spline) t0 = geom->FirstParameter(), t1 = geom->LastParameter();
  std::vector<double> t;
  for (const auto& b : g.boundary()) {
    try {
      Geom2dAPI_InterCurveCurve inter(geom, b, 1e-9);
      for (int i = 1; i <= inter.NbPoints(); ++i) {
        double u = inter.Intersector().Point(i).ParamOnFirst();
        if (k.type != Curve::Type::Spline)  // periodic: every turn of it within [a0, a1]
          for (u = t0 + std::fmod(std::fmod(u - t0, kTau) + kTau, kTau); u <= t1 + 1e-12; u += kTau) t.push_back(u);
        else
          t.push_back(u);
      }
      for (int i = 1; i <= inter.NbSegments(); ++i) {  // a curve along a boundary: its ends
        const auto& s = inter.Intersector().Segment(i);
        if (s.HasFirstPoint()) t.push_back(s.FirstPoint().ParamOnFirst());
        if (s.HasLastPoint()) t.push_back(s.LastPoint().ParamOnFirst());
      }
    } catch (const Standard_Failure&) {
    }
  }
  const auto at = [&](double u) {
    const gp_Pnt2d p = geom->Value(u);
    return Vec2{p.X(), p.Y()};
  };
  for (const auto& [a, b] : kept(t, t0, t1, at, g)) {
    Curve piece = k;
    if (k.type == Curve::Type::Spline) {
      if (a <= t0 + 1e-12 && b >= t1 - 1e-12) {
        out.push_back(k);
        continue;
      }
      try {
        Handle(Geom2d_BSplineCurve) s = Handle(Geom2d_BSplineCurve)::DownCast(geom->Copy());
        s->Segment(a, b);
        piece.pts.clear(), piece.knots.clear(), piece.weights.clear();
        piece.degree = s->Degree();
        for (int i = 1; i <= s->NbPoles(); ++i) piece.pts.push_back({s->Pole(i).X(), s->Pole(i).Y()});
        if (s->IsRational())
          for (int i = 1; i <= s->NbPoles(); ++i) piece.weights.push_back(s->Weight(i));
        const TColStd_Array1OfReal& flat = s->KnotSequence();
        for (int i = flat.Lower(); i <= flat.Upper(); ++i) piece.knots.push_back(flat(i));
      } catch (const Standard_Failure&) {
        continue;
      }
    } else {
      piece.a0 = std::fmod(a, kTau);
      if (piece.a0 < 0) piece.a0 += kTau;
      piece.a1 = piece.a0 + (b - a);
    }
    out.push_back(std::move(piece));
  }
}

void translate(Curve& k, Vec2 d) {
  for (auto& p : k.pts) p = add(p, d);
  k.c = add(k.c, d);
}

// A closed outline clipped by the half-plane n . p <= c (Sutherland-Hodgman).
std::vector<Vec2> clip_half(const std::vector<Vec2>& loop, Vec2 n, double c) {
  std::vector<Vec2> out;
  for (size_t i = 0; i < loop.size(); ++i) {
    const Vec2 a = loop[i], b = loop[(i + 1) % loop.size()];
    const double da = dot(n, a) - c, db = dot(n, b) - c;
    if (da <= 0) out.push_back(a);
    if ((da < 0 && db > 0) || (da > 0 && db < 0)) out.push_back(add(a, mul(sub(b, a), da / (da - db))));
  }
  return out;
}

}  // namespace

bool view_direction(const SheetView& view, Vec2& d) {
  const json& def = view.def;
  if (view.kind == "section") {
    const json cut = def.value("cut", json());
    if (!cut.is_array() || cut.size() < 2) return false;
    const Vec2 a{cut[0][0].get<double>(), cut[0][1].get<double>()}, b{cut[1][0].get<double>(), cut[1][1].get<double>()};
    if (len(sub(b, a)) < 1e-9) return false;
    d = left(unit(sub(b, a)));
    if (def.value("flip", false)) d = mul(d, -1);
    return true;
  }
  if (view.kind == "auxiliary") {
    const json a = def.value("angle", json());
    if (!a.is_number()) return false;
    const double r = a.get<double>() * M_PI / 180;
    d = {std::cos(r), std::sin(r)};
    return true;
  }
  if (view.kind == "projected") {
    const std::string side = def.value("side", "");
    if (side == "right") d = {1, 0};
    else if (side == "left") d = {-1, 0};
    else if (side == "top") d = {0, 1};
    else if (side == "bottom") d = {0, -1};
    else return false;
    return true;
  }
  return false;
}

bool inclined_cut(const std::vector<Vec2>& cut) {
  if (cut.size() < 3 || len(sub(cut[1], cut[0])) < 1e-9) return false;
  const Vec2 t0 = unit(sub(cut[1], cut[0]));
  for (size_t i = 2; i < cut.size(); ++i) {
    const Vec2 d = sub(cut[i], cut[i - 1]);
    if (len(d) < 1e-9) continue;
    const double c = dot(unit(d), t0);
    if (std::fabs(c) > 1e-6 && std::fabs(c) < 1 - 1e-6) return true;
  }
  return false;
}

std::string next_view_letter(const Scene& scene, const Sheet& sheet) {
  std::set<std::string> used;
  for (const auto& s : scene.sheets)
    if (s.id == sheet.id || (!sheet.drawing.empty() && s.drawing == sheet.drawing))
      for (const auto& id : s.views)
        if (const SheetView* v = scene.sheet_view(id)) used.insert(v->def.value("letter", ""));
  static const std::string letters = "ABCDEFGHJKLMNPRSTUVWXYZ";  // I, O and Q read as numbers
  for (size_t n = 0;; ++n) {
    std::string s = n < letters.size() ? std::string(1, letters[n]) : std::string(1, letters[n / letters.size() - 1]) + letters[n % letters.size()];
    if (!used.count(s)) return s;
  }
}

std::shared_ptr<const ViewGeometry> shape_linework(const std::shared_ptr<const ViewGeometry>& g, const ViewFrame& f) {
  if (!g || !f.shaped()) return g;
  const Region region(f);
  auto out = std::make_shared<ViewGeometry>(*g);
  out->curves.clear();
  out->sections.clear();
  for (const auto& k : g->curves) {
    const size_t from = out->curves.size();
    clip_curve(k, region, out->curves);
    for (size_t i = from; i < out->curves.size(); ++i) {  // a piece lies between bands: one shift closes them up
      Curve& c = out->curves[i];
      const auto pts = c.type == Curve::Type::Arc || c.type == Curve::Type::Ellipse ? c.sample(std::max(c.r1, 1e-6) * 1e-2) : c.pts;
      if (pts.empty() || f.breaks.empty()) continue;
      const Vec2 mid = pts[pts.size() / 2];
      translate(c, sub(f.fold(mid), mid));
    }
  }
  // Section faces: clipped by the crop box and the circle (a polygon close to it), then cut into the pieces between the
  // bands, each moved as the bands close up.
  std::vector<std::pair<Vec2, double>> halves;
  if (region.box)
    halves = {{{-1, 0}, -f.crop[0]}, {{1, 0}, f.crop[2]}, {{0, -1}, -f.crop[1]}, {{0, 1}, f.crop[3]}};
  if (region.round)
    for (int i = 0; i < 256; ++i) {
      const double a = (i + 0.5) * kTau / 256;
      halves.push_back({{std::cos(a), std::sin(a)}, dot({std::cos(a), std::sin(a)}, f.circle) + f.radius});
    }
  std::vector<std::vector<std::pair<Vec2, double>>> slabs{{}};
  for (int axis = 0; axis < 2; ++axis) {
    std::vector<std::vector<std::pair<Vec2, double>>> more;
    std::vector<std::pair<double, double>> spans;  // between the bands along this axis
    double lo = -1e300;
    for (const auto& b : f.breaks)
      if (b.axis == axis) spans.push_back({lo, b.from}), lo = b.to;
    spans.push_back({lo, 1e300});
    const Vec2 n = axis ? Vec2{0, 1} : Vec2{1, 0};
    for (const auto& s : slabs)
      for (const auto& [a, b] : spans) {
        auto slab = s;
        if (a > -1e299) slab.push_back({mul(n, -1), -a});
        if (b < 1e299) slab.push_back({n, b});
        more.push_back(std::move(slab));
      }
    slabs.swap(more);
  }
  for (const auto& r : g->sections) {
    ViewGeometry::Region kept_region{r.body, {}};
    for (const auto& slab : slabs)
      for (const auto& loop : r.loops) {
        std::vector<Vec2> l = loop;
        for (const auto& [n, c] : halves) l = clip_half(l, n, c);
        for (const auto& [n, c] : slab) l = clip_half(l, n, c);
        if (l.size() < 3) continue;
        Vec2 mid{0, 0};
        for (const auto& p : l) mid = add(mid, mul(p, 1.0 / static_cast<double>(l.size())));
        const Vec2 shift = sub(f.fold(mid), mid);
        for (auto& p : l) p = add(p, shift);
        kept_region.loops.push_back(std::move(l));
      }
    if (!kept_region.loops.empty()) out->sections.push_back(std::move(kept_region));
  }
  bool any = false;
  std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
  for (const auto& k : out->curves)
    for (const auto& p : k.type == Curve::Type::Arc || k.type == Curve::Type::Ellipse ? k.sample(std::max(k.r1, 1e-6) * 1e-3) : k.pts) {
      box = {std::min(box[0], p[0]), std::min(box[1], p[1]), std::max(box[2], p[0]), std::max(box[3], p[1])};
      any = true;
    }
  out->bounds = any ? box : std::array<double, 4>{0, 0, 0, 0};
  return out;
}

namespace {

// A family of parallel lines in a section lining: turned `turn` degrees from the pattern's angle, `offset` + k * `period`
// pitches from the origin, dashed (paper mm: dash, gap, dash, ...; empty: solid).
struct Family {
  double turn, offset, period;
  std::vector<double> dashes;
};
struct Pattern {
  const char* name;
  std::vector<Family> families;
};
const std::vector<Pattern>& pattern_table() {
  static const std::vector<Pattern> all = {
      {"general", {{0, 0, 1, {}}}},
      {"steel", {{0, 0, 2, {}}, {0, 0.45, 2, {}}}},
      {"copper", {{0, 0, 2, {}}, {0, 1, 2, {2.5, 1}}}},
      {"aluminium", {{0, 0, 1, {}}, {90, 0.5, 2, {1, 2}}}},
      {"plastic", {{0, 0, 3, {}}, {0, 0.4, 3, {}}, {0, 0.8, 3, {}}}},
      {"insulation", {{0, 0, 1, {}}, {90, 0, 1, {}}}},
      {"glass", {{0, 0, 1, {2.5, 0.8, 0.4, 0.8}}}},
  };
  return all;
}

// The lines y = (offset + k) * period of the plane turned by `angle` (through the origin, so pieces of one part line up),
// inside the loops (even-odd), dashed from x = 0 on.
void scan(const std::vector<std::vector<Vec2>>& loops, double angle, double period, double offset, const std::vector<double>& dashes,
          std::vector<std::array<Vec2, 2>>& out) {
  if (!(period > 0)) return;
  const double c = std::cos(angle), s = std::sin(angle);
  std::vector<std::vector<Vec2>> q(loops.size());
  double y0 = 1e300, y1 = -1e300;
  for (size_t i = 0; i < loops.size(); ++i)
    for (const auto& p : loops[i]) {
      q[i].push_back({c * p[0] + s * p[1], -s * p[0] + c * p[1]});
      y0 = std::min(y0, q[i].back()[1]), y1 = std::max(y1, q[i].back()[1]);
    }
  if (y0 > y1 || (y1 - y0) / period > 20000) return;
  double cycle = 0;
  for (double l : dashes) cycle += l;
  const auto put = [&](double x0, double x1, double y) {
    if (x1 - x0 > 1e-9) out.push_back({Vec2{c * x0 - s * y, s * x0 + c * y}, Vec2{c * x1 - s * y, s * x1 + c * y}});
  };
  std::vector<double> xs;
  for (long k = static_cast<long>(std::ceil(y0 / period - offset)); (static_cast<double>(k) + offset) * period <= y1; ++k) {
    const double y = (static_cast<double>(k) + offset) * period;
    xs.clear();
    for (const auto& l : q)
      for (size_t i = 0; i < l.size(); ++i) {
        const Vec2 a = l[i], b = l[(i + 1) % l.size()];
        if ((a[1] <= y && y < b[1]) || (b[1] <= y && y < a[1])) xs.push_back(a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]));
      }
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2) {
      if (!(cycle > 0)) {
        put(xs[i], xs[i + 1], y);
        continue;
      }
      for (double m = std::floor(xs[i] / cycle) * cycle; m < xs[i + 1]; m += cycle) {
        double from = m;
        for (size_t j = 0; j < dashes.size(); from += dashes[j], ++j)
          if (j % 2 == 0) put(std::max(xs[i], from), std::min(xs[i + 1], from + dashes[j]), y);
      }
    }
  }
}

}  // namespace

std::vector<std::array<Vec2, 2>> hatch_lines(const std::vector<std::vector<Vec2>>& loops, double angle, double pitch) {
  std::vector<std::array<Vec2, 2>> out;
  scan(loops, angle, pitch, 0, {}, out);
  return out;
}

const std::vector<std::string>& hatch_patterns() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> out;
    for (const auto& p : pattern_table()) out.push_back(p.name);
    return out;
  }();
  return names;
}

std::string material_hatch(const std::string& material) {
  static const std::map<std::string, std::string> by = {
      {"steel", "steel"},     {"stainless", "steel"}, {"titanium", "steel"}, {"aluminium-6061", "aluminium"}, {"brass", "copper"},
      {"copper", "copper"},   {"abs", "plastic"},     {"pla", "plastic"},    {"petg", "plastic"},             {"nylon", "plastic"},
      {"nylon-12", "plastic"}, {"polycarbonate", "plastic"}, {"pom", "plastic"}, {"fr4", "insulation"},       {"glass", "glass"}};
  const auto it = by.find(material);
  return it == by.end() ? "general" : it->second;
}

std::vector<std::array<Vec2, 2>> hatch_pattern(const std::vector<std::vector<Vec2>>& loops, const std::string& pattern, double angle, double pitch) {
  std::vector<std::array<Vec2, 2>> out;
  const auto& all = pattern_table();
  auto it = std::find_if(all.begin(), all.end(), [&](const Pattern& p) { return pattern == p.name; });
  if (it == all.end()) it = all.begin();  // a pattern of a newer OPAD: the general one
  for (const auto& f : it->families) scan(loops, angle + f.turn * M_PI / 180, f.period * pitch, f.offset / f.period, f.dashes, out);
  return out;
}

namespace detail {
namespace {

// One body's cut faces on paper, and what decides its lining.
struct Part {
  int body = -1;
  std::string node;
  std::vector<std::vector<Vec2>> loops;
  double area = 0, length = 0;
  std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
  std::vector<std::pair<double, double>> runs;  // outline directions (radians mod pi) and their lengths
  double principal = 0;                         // its main outlines' direction (radians mod pi/2)
  std::string pattern;
  double angle = 0, pitch = 0;
  bool fixed_angle = false, fixed_pitch = false, solid = false;
};

double turn_apart(double a, double b, double period) {  // |a - b| modulo period, folded into [0, period / 2]
  double t = std::fmod(std::fabs(a - b), period);
  return std::min(t, period - t);
}

// The share of its outline that runs within 3 degrees of `angle`.
double parallel_share(const Part& p, double angle) {
  double along = 0;
  for (const auto& [a, l] : p.runs)
    if (turn_apart(a, angle, M_PI) < 3 * M_PI / 180) along += l;
  return p.length > 0 ? along / p.length : 0;
}

// Its main outlines' direction: the most common one (by length, folded into a quarter turn) when it holds a quarter of the
// outline at least, else square to the sheet.
double principal_of(const Part& p) {
  std::array<double, 90> bins{};
  for (const auto& [a, l] : p.runs) bins[static_cast<size_t>(std::fmod(a * 180 / M_PI, 90.0)) % 90] += l;
  size_t best = 0;
  double most = -1;
  for (size_t i = 0; i < 90; ++i) {
    const double w = bins[(i + 89) % 90] + bins[i] + bins[(i + 1) % 90];
    if (w > most) most = w, best = i;
  }
  if (!(p.length > 0) || most < 0.25 * p.length) return 0;
  double sx = 0, sy = 0;  // the mean of the directions near the peak, on the quarter-turn circle
  for (const auto& [a, l] : p.runs) {
    const double q = std::fmod(a, M_PI / 2);
    if (turn_apart(q, (static_cast<double>(best) + 0.5) * M_PI / 180, M_PI / 2) < 2 * M_PI / 180) sx += l * std::cos(4 * q), sy += l * std::sin(4 * q);
  }
  double r = std::atan2(sy, sx) / 4;
  if (r < 0) r += M_PI / 2;
  return turn_apart(r, 0, M_PI / 2) < 0.5 * M_PI / 180 ? 0 : r;
}

// Whether two parts' cut faces meet: a corner of one within `tol` of the other's outline.
bool touching(const Part& a, const Part& b, double tol) {
  if (a.box[0] > b.box[2] + tol || b.box[0] > a.box[2] + tol || a.box[1] > b.box[3] + tol || b.box[1] > a.box[3] + tol) return false;
  const auto near = [&](const Part& p, const Part& q) {
    for (const auto& l : p.loops) {
      const size_t stride = std::max<size_t>(1, l.size() / 400);
      for (size_t i = 0; i < l.size(); i += stride) {
        const Vec2 x = l[i];
        if (x[0] < q.box[0] - tol || x[0] > q.box[2] + tol || x[1] < q.box[1] - tol || x[1] > q.box[3] + tol) continue;
        for (const auto& m : q.loops)
          for (size_t j = 0; j < m.size(); ++j) {
            const Vec2 s = m[j], e = m[(j + 1) % m.size()], d = sub(e, s);
            const double t = std::clamp(dot(sub(x, s), d) / std::max(dot(d, d), 1e-30), 0.0, 1.0);
            if (len(sub(x, add(s, mul(d, t)))) <= tol) return true;
          }
      }
    }
    return false;
  };
  return near(a, b) || near(b, a);
}

// The hatch settings that apply: the view's own, else (a detail view) those of the view it enlarges.
json hatch_of(const SheetView& v, const Scene* scene) {
  const SheetView* at = &v;
  for (int i = 0; at && i < 32; ++i) {
    if (at->def.contains("hatch") && at->def["hatch"].is_object()) return at->def["hatch"];
    if (at->kind != "detail" || !scene) break;
    at = scene->sheet_view(at->parent);
  }
  return json::object();
}

}  // namespace

void draw_section_faces(Display& d, const ViewFrame& f, const SheetView& v, const ViewGeometry& g, const Document* doc, const Scene* scene) {
  if (g.sections.empty()) return;
  const int layer = d.layer({"Hatch", kInk, LineType::Continuous, 0.18});
  const json hatch = hatch_of(v, scene);
  const json bodies = hatch.value("bodies", json::object());
  std::vector<Part> parts;
  for (const auto& r : g.sections) {
    auto it = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.body == r.body; });
    if (it == parts.end()) {
      parts.emplace_back();
      parts.back().body = r.body;
      if (r.body >= 0 && static_cast<size_t>(r.body) < g.bodies.size()) parts.back().node = g.bodies[static_cast<size_t>(r.body)].node;
      it = parts.end() - 1;
    }
    Part& p = *it;
    for (const auto& l : r.loops) {
      std::vector<Vec2> paper;
      for (const auto& q : l) paper.push_back({f.at[0] + f.scale * (q[0] - f.centre[0]), f.at[1] + f.scale * (q[1] - f.centre[1])});
      double a = 0;
      for (size_t k = 0; k < paper.size(); ++k) {
        const Vec2 s = paper[k], e = paper[(k + 1) % paper.size()];
        a += s[0] * e[1] - e[0] * s[1];
        const double l2 = len(sub(e, s));
        if (l2 > 1e-9) {
          double dir = std::atan2(e[1] - s[1], e[0] - s[0]);
          if (dir < 0) dir += M_PI;
          p.runs.push_back({std::fmod(dir, M_PI), l2});
          p.length += l2;
        }
        p.box = {std::min(p.box[0], s[0]), std::min(p.box[1], s[1]), std::max(p.box[2], s[0]), std::max(p.box[3], s[1])};
      }
      p.area += a / 2;  // holes run the other way round
      p.loops.push_back(std::move(paper));
    }
  }
  // What each part takes: its own settings (bodies), else the view's; ISO 128-50 lines for every material unless the view
  // asks for the material symbols; narrow faces filled (ISO 128-50: under about a millimetre on paper, no room for lines).
  const std::string view_pattern = hatch.value("pattern", "general");
  for (auto& p : parts) {
    const json own = bodies.value(p.node, json::object());
    std::string pattern = own.value("pattern", view_pattern);
    if (pattern == "material") pattern = doc && scene && scene->node(p.node) ? material_hatch(material_of(*doc, *scene, p.node).id) : "general";
    p.pattern = pattern;
    p.principal = principal_of(p);
    const double auto_pitch = std::clamp(std::sqrt(std::fabs(p.area)) / 8, 1.2, 4.0);
    p.fixed_pitch = own.contains("spacing") || hatch.contains("spacing");
    p.pitch = own.contains("spacing") ? own["spacing"].get<double>() : hatch.contains("spacing") ? hatch["spacing"].get<double>() : auto_pitch;
    p.fixed_angle = own.contains("angle");
    if (p.fixed_angle) p.angle = own["angle"].get<double>() * M_PI / 180;
    p.solid = hatch.value("thin", "fill") != "hatch" && p.length > 0 && 2 * std::fabs(p.area) / p.length < 0.9;
  }
  // Neighbours apart (ISO 128-50): by angle, then by spacing; the largest part first, at 45 degrees to its main outlines
  // (or the view's angle), never along much of its own outline.
  std::vector<size_t> order(parts.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return std::fabs(parts[a].area) > std::fabs(parts[b].area); });
  std::vector<std::vector<size_t>> next(parts.size());
  for (size_t i = 0; i < parts.size(); ++i)
    for (size_t j = i + 1; j < parts.size(); ++j)
      if (touching(parts[i], parts[j], 0.3)) next[i].push_back(j), next[j].push_back(i);
  static const double turns[] = {45, 135, 45, 135, 30, 120, 60, 150};
  static const double steps[] = {1, 1, 1.6, 1.6, 1.3, 1.3, 1.3, 1.3};
  std::vector<bool> done(parts.size(), false);
  for (size_t i : order) {
    Part& p = parts[i];
    const double base = hatch.contains("angle") ? hatch["angle"].get<double>() * M_PI / 180 - M_PI / 4 : p.principal;
    const auto clash = [&](double angle, double pitch) {
      for (size_t j : next[i])
        if (done[j] && turn_apart(parts[j].angle, angle, M_PI) < 10 * M_PI / 180 && std::fabs(parts[j].pitch - pitch) < 0.15 * std::max(pitch, parts[j].pitch)) return true;
      return false;
    };
    if (!p.fixed_angle) {
      int pick = -1, fallback = -1;
      for (int k = 0; k < 8 && pick < 0; ++k) {
        const double a = base + turns[k] * M_PI / 180, pitch = p.fixed_pitch ? p.pitch : p.pitch * steps[k];
        if (!hatch.contains("angle") && parallel_share(p, a) > 0.15) continue;
        if (fallback < 0) fallback = k;
        if (!clash(a, pitch)) pick = k;
      }
      if (pick < 0) pick = std::max(fallback, 0);
      p.angle = std::fmod(base + turns[pick] * M_PI / 180, M_PI);
      if (!p.fixed_pitch) p.pitch *= steps[pick];
    }
    done[i] = true;
  }
  for (const auto& p : parts) {
    if (p.solid) d.fill(layer, p.loops);
    else
      for (const auto& [a, b] : hatch_pattern(p.loops, p.pattern, p.angle, p.pitch)) d.line(layer, a, b);
  }
}

void draw_view_marks(Display& d, const ViewFrame& f, const SheetView& v, const Scene& scene) {
  const Sheet* sheet = scene.sheet(v.sheet);
  const bool asme = sheet && sheet->standard == "asme", third = sheet && sheet->projection == "third";
  const int text = d.layer({"Text", kInk, LineType::Continuous, 0.25});
  const double h = 5;  // view designations: a size up from the dimensions' 3.5 (ISO 3098)
  const auto paper = [&](Vec2 p) { return add(f.at, f.local(p)); };
  const auto arrow = [&](int layer, Vec2 tail, Vec2 tip) {
    const Vec2 u = unit(sub(tip, tail)), back = sub(tip, mul(u, 3)), side = mul(left(u), 0.5);
    d.line(layer, tail, back);
    d.fill(layer, {{tip, add(back, side), sub(back, side)}});
  };
  // Its own label.
  const std::string letter = v.def.value("letter", "");
  if (!letter.empty() && (v.kind == "section" || v.kind == "detail" || v.kind == "auxiliary")) {
    std::string s;
    if (v.kind == "section") s = asme ? "SECTION " + letter + "-" + letter : letter + "-" + letter;
    else if (v.kind == "detail") s = asme ? "DETAIL " + letter + "\nSCALE " + scale_text(f.scale) : letter + " (" + scale_text(f.scale) + ")";
    else s = asme ? "VIEW " + letter : letter;
    const double cx = (f.box[0] + f.box[2]) / 2;
    if (asme) d.text(text, s, {cx, f.box[1] - 4}, h, 0, 1, 3);
    else d.text(text, s, {cx, f.box[3] + 4}, h, 0, 1, 1);
  }
  // A detail view's boundary.
  if (v.kind == "detail" && f.radius > 0) d.circle(d.layer({"Detail", kInk, LineType::Continuous, 0.25}), f.at, f.radius * f.scale);
  // Breaks: two thin lines with a zigzag across the view, where the halves meet; a partial view's the same where its crop
  // box cuts through it (ISO 128-34).
  const auto zigzag = [&](size_t a, double edge, double lo, double hi) {
    const size_t o = 1 - a;
    const double mid = (lo + hi) / 2, z = std::min(3.0, (hi - lo) / 8);
    std::vector<Vec2> pts;
    for (const auto& [along, off] : std::initializer_list<std::pair<double, double>>{{lo, 0}, {mid - z, 0}, {mid - z / 2, z}, {mid + z / 2, -z}, {mid + z, 0}, {hi, 0}}) {
      Vec2 p;
      p[a] = edge + off, p[o] = along;
      pts.push_back(p);
    }
    d.polyline(d.layer({"Break", kInk, LineType::Continuous, 0.25}), pts);
  };
  for (const auto& b : f.breaks) {
    const size_t a = static_cast<size_t>(b.axis), o = 1 - a;
    const Vec2 at = f.local(b.axis ? Vec2{0, b.from} : Vec2{b.from, 0});
    for (double edge : {f.at[a] + at[a], f.at[a] + at[a] + b.gap * f.scale}) zigzag(a, edge, f.box[o] - 3, f.box[o + 2] + 3);
  }
  for (int k = 0; k < 4; ++k)
    if (f.crop_cuts & (1 << k)) {
      const size_t a = static_cast<size_t>(k % 2), o = 1 - a;
      zigzag(a, f.box[k < 2 ? a : a + 2], f.box[o] - 2, f.box[o + 2] + 2);
    }
  // What its section, detail and lettered auxiliary views mark on it.
  const SheetView* self = scene.sheet_view(v.id);
  for (const auto& id : self ? self->children : std::vector<std::string>{}) {
    const SheetView* c = scene.sheet_view(id);
    if (!c || !c->error.empty()) continue;
    const std::string mark = c->def.value("letter", "");
    if (c->kind == "section") {
      std::vector<Vec2> pts;
      for (const auto& p : c->def.value("cut", json::array())) pts.push_back(paper({p[0].get<double>(), p[1].get<double>()}));
      Vec2 toward;
      if (pts.size() < 2 || !view_direction(*c, toward)) continue;
      // The cutting line (ISO 128-44): thin long-dashed dotted, thick at the ends and where it turns; arrows at its ends in
      // the direction of viewing; its letter at each arrow.
      d.polyline(d.layer({"Section line", kInk, LineType::Center, 0.25}), pts);
      const int thick = d.layer({"Section ends", kInk, LineType::Continuous, 0.5});
      const auto stub = [&](Vec2 from, Vec2 to, double l) { d.line(thick, from, add(from, mul(unit(sub(to, from)), std::min(l, len(sub(to, from)))))); };
      stub(pts.front(), pts[1], 6), stub(pts.back(), pts[pts.size() - 2], 6);
      for (size_t i = 1; i + 1 < pts.size(); ++i) stub(pts[i], pts[i - 1], 3), stub(pts[i], pts[i + 1], 3);
      const Vec2 look = mul(toward, third ? -1 : 1);  // first angle: the view lies where one looks; third: where one stands
      // Aligned: the last segment is looked at square to itself (turned with it).
      Vec2 last_look = look;
      if (c->def.value("aligned", false)) {
        const Vec2 a = sub(pts[1], pts[0]), b = sub(pts.back(), pts[pts.size() - 2]);
        const double t = std::atan2(b[1], b[0]) - std::atan2(a[1], a[0]);
        last_look = {std::cos(t) * look[0] - std::sin(t) * look[1], std::sin(t) * look[0] + std::cos(t) * look[1]};
      }
      const int arrows = d.layer({"Dimensions", kInk, LineType::Continuous, 0.25});
      for (const auto& [end, inner] : {std::pair{pts.front(), pts[1]}, std::pair{pts.back(), pts[pts.size() - 2]}}) {
        const Vec2 way = end == pts.front() ? look : last_look;
        arrow(arrows, end, add(end, mul(way, 8)));
        if (!mark.empty()) d.text(text, mark, add(add(end, mul(way, 5)), mul(unit(sub(end, inner)), 4)), h, 0, 1, 2);
      }
    } else if (c->kind == "detail") {
      const json at = c->def.value("center", json());
      const double r = c->def.value("radius", 0.0) * f.scale;
      if (!at.is_array() || !(r > 0)) continue;
      const Vec2 centre = paper({at[0].get<double>(), at[1].get<double>()});
      d.circle(d.layer({"Detail", kInk, LineType::Continuous, 0.25}), centre, r);
      if (!mark.empty()) d.text(text, mark, add(centre, mul(Vec2{M_SQRT1_2, M_SQRT1_2}, r + 4)), h, 0, 1, 2);
    } else if (c->kind == "auxiliary" && !mark.empty()) {
      Vec2 toward;
      if (!view_direction(*c, toward)) continue;
      const Vec2 look = mul(toward, third ? -1 : 1), centre{(f.box[0] + f.box[2]) / 2, (f.box[1] + f.box[3]) / 2};
      const double reach = std::fabs(look[0]) * (f.box[2] - f.box[0]) / 2 + std::fabs(look[1]) * (f.box[3] - f.box[1]) / 2;
      const Vec2 tip = sub(centre, mul(look, reach + 4)), tail = sub(tip, mul(look, 10));
      arrow(d.layer({"Dimensions", kInk, LineType::Continuous, 0.25}), tail, tip);
      d.text(text, mark, sub(tail, mul(look, 4)), h, 0, 1, 2);
    }
  }
}

}  // namespace detail
}  // namespace opad::drawing
