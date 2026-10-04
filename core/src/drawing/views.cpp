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
#include <set>

#include "opad/drawing/sheet.hpp"
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

std::vector<std::array<Vec2, 2>> hatch_lines(const std::vector<std::vector<Vec2>>& loops, double angle, double pitch) {
  std::vector<std::array<Vec2, 2>> out;
  if (!(pitch > 0)) return out;
  const double c = std::cos(angle), s = std::sin(angle);
  // Turned so the lines run along x: q = R(-angle) p; lines at y = k * pitch (through the origin, so pieces line up).
  std::vector<std::vector<Vec2>> q(loops.size());
  double y0 = 1e300, y1 = -1e300;
  for (size_t i = 0; i < loops.size(); ++i)
    for (const auto& p : loops[i]) {
      q[i].push_back({c * p[0] + s * p[1], -s * p[0] + c * p[1]});
      y0 = std::min(y0, q[i].back()[1]), y1 = std::max(y1, q[i].back()[1]);
    }
  if (y0 > y1 || (y1 - y0) / pitch > 20000) return out;
  std::vector<double> xs;
  for (long k = static_cast<long>(std::ceil(y0 / pitch)); k * pitch <= y1; ++k) {
    const double y = static_cast<double>(k) * pitch;
    xs.clear();
    for (const auto& l : q)
      for (size_t i = 0; i < l.size(); ++i) {
        const Vec2 a = l[i], b = l[(i + 1) % l.size()];
        if ((a[1] <= y && y < b[1]) || (b[1] <= y && y < a[1])) xs.push_back(a[0] + (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]));
      }
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2)
      if (xs[i + 1] - xs[i] > 1e-9) out.push_back({Vec2{c * xs[i] - s * y, s * xs[i] + c * y}, Vec2{c * xs[i + 1] - s * y, s * xs[i + 1] + c * y}});
  }
  return out;
}

namespace detail {

void draw_section_faces(Display& d, const ViewFrame& f, const ViewGeometry& g) {
  if (g.sections.empty()) return;
  const int layer = d.layer({"Hatch", kInk, LineType::Continuous, 0.18});
  // ISO 128-50: one pattern per part; neighbours apart by angle, then by spacing. Spacing grows with the part's cut area.
  static const double angles[] = {45, 135, 45, 135, 30, 120, 60, 150};
  static const double pitches[] = {1, 1, 1.6, 1.6, 1.3, 1.3, 1.3, 1.3};
  std::vector<int> bodies;
  for (const auto& r : g.sections)
    if (std::find(bodies.begin(), bodies.end(), r.body) == bodies.end()) bodies.push_back(r.body);
  for (size_t i = 0; i < bodies.size(); ++i) {
    std::vector<std::vector<Vec2>> loops;
    double area = 0;
    for (const auto& r : g.sections) {
      if (r.body != bodies[i]) continue;
      for (const auto& l : r.loops) {
        std::vector<Vec2> paper;
        for (const auto& p : l) paper.push_back({f.at[0] + f.scale * (p[0] - f.centre[0]), f.at[1] + f.scale * (p[1] - f.centre[1])});
        double a = 0;
        for (size_t k = 0; k < paper.size(); ++k) a += paper[k][0] * paper[(k + 1) % paper.size()][1] - paper[(k + 1) % paper.size()][0] * paper[k][1];
        area += a / 2;  // holes run the other way round
        loops.push_back(std::move(paper));
      }
    }
    const double pitch = std::clamp(std::sqrt(std::fabs(area)) / 8, 1.2, 4.0) * pitches[i % 8];
    for (const auto& [a, b] : hatch_lines(loops, angles[i % 8] * M_PI / 180, pitch)) d.line(layer, a, b);
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
  // Breaks: two thin lines with a zigzag across the view, where the halves meet.
  if (!f.breaks.empty()) {
    const int layer = d.layer({"Break", kInk, LineType::Continuous, 0.25});
    for (const auto& b : f.breaks) {
      const size_t a = static_cast<size_t>(b.axis), o = 1 - a;
      const Vec2 at = f.local(b.axis ? Vec2{0, b.from} : Vec2{b.from, 0});
      const double lo = f.box[o] - 3, hi = f.box[o + 2] + 3, mid = (lo + hi) / 2, z = std::min(3.0, (hi - lo) / 8);
      for (double edge : {f.at[a] + at[a], f.at[a] + at[a] + b.gap * f.scale}) {
        std::vector<Vec2> pts;
        for (const auto& [along, off] : std::initializer_list<std::pair<double, double>>{{lo, 0}, {mid - z, 0}, {mid - z / 2, z}, {mid + z / 2, -z}, {mid + z, 0}, {hi, 0}}) {
          Vec2 p;
          p[a] = edge + off, p[o] = along;
          pts.push_back(p);
        }
        d.polyline(layer, pts);
      }
    }
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
      const int arrows = d.layer({"Dimensions", kInk, LineType::Continuous, 0.25});
      for (const auto& [end, inner] : {std::pair{pts.front(), pts[1]}, std::pair{pts.back(), pts[pts.size() - 2]}}) {
        arrow(arrows, end, add(end, mul(look, 8)));
        if (!mark.empty()) d.text(text, mark, add(add(end, mul(look, 5)), mul(unit(sub(end, inner)), 4)), h, 0, 1, 2);
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
