// Drawings ready to be written (TODO 11 UI-86): the display list, shapes and hidden-line views turned into it, and
// dimensions as geometry. The writers are dxf_writer.cpp and svg_writer.cpp.
#include "opad/drawing/display.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp.hxx>

#include <algorithm>
#include <cmath>
#include <map>

#include "opad/drawing/sheet.hpp"
#include "opad/drawing/symbols.hpp"
#include "opad/util.hpp"
#include "projection_internal.hpp"
#include "writer_common.hpp"

namespace opad::drawing {
namespace {

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

// Text along a direction, turned so it reads from below or from the right (ISO 129).
double readable(double angle) {
  while (angle > M_PI / 2 + 1e-9) angle -= M_PI;
  while (angle <= -M_PI / 2 + 1e-9) angle += M_PI;
  return angle;
}

void arrowhead(Display& d, int layer, Vec2 tip, Vec2 towards, const DimStyle& s) {
  const double l = s.arrow * s.scale, w = l / 6;
  const Vec2 back = sub(tip, mul(towards, l)), side = mul(left(towards), w);
  d.fill(layer, {{tip, add(back, side), sub(back, side)}});
}

const char* entity_name(const Prim& p) {
  switch (p.kind) {
    case Prim::Kind::Fill: return p.loops.size() == 1 && (p.loops[0].size() == 3 || p.loops[0].size() == 4) ? "SOLID" : "HATCH";
    case Prim::Kind::Text: return p.text.find('\n') == std::string::npos ? "TEXT" : "MTEXT";
    case Prim::Kind::Image: return "IMAGE";
    default: break;
  }
  switch (p.curve.type) {
    case Curve::Type::Line: return "LINE";
    case Curve::Type::Arc: return detail::full_turn(p.curve) ? "CIRCLE" : "ARC";
    case Curve::Type::Ellipse: return "ELLIPSE";
    case Curve::Type::Spline: return "SPLINE";
    default: return "LWPOLYLINE";
  }
}

}  // namespace

const char* line_type_name(LineType t) {
  switch (t) {
    case LineType::Hidden: return "HIDDEN";
    case LineType::Center: return "CENTER";
    case LineType::Phantom: return "PHANTOM";
    case LineType::Dotted: return "DOT";
    default: return "Continuous";
  }
}

const std::vector<double>& line_type_dashes(LineType t) {
  static const std::vector<double> none, hidden{3, -1.5}, center{12, -3, 1.5, -3}, phantom{12, -3, 1.5, -3, 1.5, -3}, dotted{0, -1.5};
  switch (t) {
    case LineType::Hidden: return hidden;
    case LineType::Center: return center;
    case LineType::Phantom: return phantom;
    case LineType::Dotted: return dotted;
    default: return none;
  }
}

int Display::layer(const Layer& l) {
  for (size_t i = 0; i < layers.size(); ++i)
    if (layers[i].name == l.name) return static_cast<int>(i);
  layers.push_back(l);
  return static_cast<int>(layers.size()) - 1;
}

void Display::curve(int layer, const Curve& c, uint32_t rgb) {
  Prim p;
  p.layer = layer;
  p.rgb = rgb;
  p.curve = c;
  prims.push_back(std::move(p));
}

void Display::line(int layer, Vec2 a, Vec2 b, uint32_t rgb) {
  if (len(sub(b, a)) < 1e-12) return;
  Curve c;
  c.pts = {a, b};
  curve(layer, c, rgb);
}

void Display::polyline(int layer, const std::vector<Vec2>& pts, bool closed, uint32_t rgb) {
  if (pts.size() < 2) return;
  Curve c;
  c.type = Curve::Type::Polyline;
  c.pts = pts;
  if (closed && len(sub(pts.back(), pts.front())) > 1e-12) c.pts.push_back(pts.front());
  curve(layer, c, rgb);
}

void Display::arc(int layer, Vec2 centre, double r, double a0, double a1, uint32_t rgb) {
  if (!(r > 0)) return;
  while (a1 <= a0) a1 += 2 * M_PI;
  Curve c;
  c.type = Curve::Type::Arc;
  c.c = centre;
  c.r1 = c.r2 = r;
  c.a0 = a0;
  c.a1 = std::min(a1, a0 + 2 * M_PI);
  curve(layer, c, rgb);
}

void Display::circle(int layer, Vec2 c, double r, uint32_t rgb) { arc(layer, c, r, 0, 2 * M_PI, rgb); }

void Display::fill(int layer, std::vector<std::vector<Vec2>> loops, uint32_t rgb) {
  for (auto& l : loops)
    while (l.size() > 1 && len(sub(l.back(), l.front())) < 1e-12) l.pop_back();
  loops.erase(std::remove_if(loops.begin(), loops.end(), [](const auto& l) { return l.size() < 3; }), loops.end());
  if (loops.empty()) return;
  Prim p;
  p.kind = Prim::Kind::Fill;
  p.layer = layer;
  p.rgb = rgb;
  p.loops = std::move(loops);
  prims.push_back(std::move(p));
}

void Display::text(int layer, const std::string& s, Vec2 at, double height, double angle, int halign, int valign, uint32_t rgb) {
  if (s.find_first_not_of(" \n") == std::string::npos) return;
  Prim p;
  p.kind = Prim::Kind::Text;
  p.layer = layer;
  p.rgb = rgb;
  p.text = s;
  p.at = at;
  p.height = height;
  p.angle = angle;
  p.halign = std::clamp(halign, 0, 2);
  p.valign = std::clamp(valign, 0, 3);
  prims.push_back(std::move(p));
}

std::vector<TextLine> text_lines(const Prim& p) {
  std::vector<TextLine> lines(1);
  for (char ch : p.text) {
    if (ch == '\n') lines.emplace_back();
    else lines.back().text += ch;
  }
  const double pitch = 1.6 * p.height, block = p.height + pitch * static_cast<double>(lines.size() - 1);
  const double first = p.valign == 3 ? -p.height : p.valign == 2 ? block / 2 - p.height : p.valign == 1 ? block - p.height + 0.3 * p.height : 0;
  const Vec2 v{-std::sin(p.angle), std::cos(p.angle)};
  for (size_t i = 0; i < lines.size(); ++i) {
    const double up = first - pitch * static_cast<double>(i);
    lines[i].at = {p.at[0] + v[0] * up, p.at[1] + v[1] * up};
  }
  return lines;
}

std::array<double, 4> Display::bounds(size_t from, size_t to) const {
  std::array<double, 4> b{1e300, 1e300, -1e300, -1e300};
  auto take = [&](Vec2 p) { b = {std::min(b[0], p[0]), std::min(b[1], p[1]), std::max(b[2], p[0]), std::max(b[3], p[1])}; };
  for (size_t i = from; i < std::min(to, prims.size()); ++i) {
    const Prim& p = prims[i];
    if (p.kind == Prim::Kind::Fill) {
      for (const auto& l : p.loops)
        for (const auto& q : l) take(q);
    } else if (p.kind == Prim::Kind::Image) {
      for (const auto& q : p.corners) take(q);
      take(add(p.corners[1], sub(p.corners[2], p.corners[0])));
    } else if (p.kind == Prim::Kind::Text) {
      // A rough box a line: 0.7 of the height per character, from its descenders (0.3 heights below its baseline) to
      // its capitals.
      const Vec2 u{std::cos(p.angle), std::sin(p.angle)}, v = left(u);
      for (const auto& line : text_lines(p)) {
        size_t n = 0;
        for (char ch : line.text) n += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
        const double w = 0.7 * p.height * static_cast<double>(n), x0 = p.halign == 1 ? -w / 2 : p.halign == 2 ? -w : 0;
        for (const auto& [x, y] : std::initializer_list<std::pair<double, double>>{{x0, -0.3 * p.height}, {x0 + w, -0.3 * p.height}, {x0, p.height}, {x0 + w, p.height}})
          take(add(line.at, add(mul(u, x), mul(v, y))));
      }
    } else if (p.curve.type == Curve::Type::Arc || p.curve.type == Curve::Type::Ellipse) {
      for (const auto& q : p.curve.sample(std::max(p.curve.r1, 1e-6) * 1e-3)) take(q);
    } else {
      for (const auto& q : p.curve.pts) take(q);  // a spline's poles: around the curve
    }
  }
  if (b[0] > b[2]) return {0, 0, 0, 0};
  return b;
}

json Display::counts() const {
  json entities = json::object(), byLayer = json::object();
  for (const auto& l : layers) byLayer[l.name] = 0;
  for (const auto& p : prims) {
    const std::string e = entity_name(p);
    entities[e] = entities.value(e, 0) + 1;
    if (p.layer >= 0 && p.layer < static_cast<int>(layers.size())) {
      auto& n = byLayer[layers[static_cast<size_t>(p.layer)].name];
      n = n.get<int>() + 1;
    }
  }
  return {{"entities", entities}, {"layers", byLayer}, {"total", prims.size()}};
}

// ---------------------------------------------------------------- shapes

void add_shape(Display& d, int layer, const TopoDS_Shape& shape, double tol, uint32_t rgb) {
  if (shape.IsNull()) return;
  tol = std::max(tol, 1e-6);
  const detail::View top{gp::DX(), gp::DY(), gp::DZ()};
  const auto sampled = [&](const TopoDS_Edge& e) {
    std::vector<Vec2> out;
    BRepAdaptor_Curve c(e);
    try {
      GCPnts_QuasiUniformDeflection q(c, tol, c.FirstParameter(), c.LastParameter());
      if (q.IsDone())
        for (int i = 1; i <= q.NbPoints(); ++i) out.push_back(top.at(q.Value(i)));
    } catch (const Standard_Failure&) {
    }
    if (out.size() < 2) {
      out.clear();
      for (int i = 0; i <= 32; ++i) out.push_back(top.at(c.Value(c.FirstParameter() + (c.LastParameter() - c.FirstParameter()) * i / 32)));
    }
    if (e.Orientation() == TopAbs_REVERSED) std::reverse(out.begin(), out.end());
    return out;
  };
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
    const TopoDS_Face face = TopoDS::Face(f.Current());
    std::vector<std::vector<Vec2>> loops;
    for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
      std::vector<Vec2> loop;
      for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) {
        if (BRep_Tool::Degenerated(e.Current())) continue;
        const auto pts = sampled(e.Current());
        for (const auto& p : pts)
          if (loop.empty() || len(sub(p, loop.back())) > 1e-9) loop.push_back(p);
      }
      loops.push_back(std::move(loop));
    }
    d.fill(layer, std::move(loops), rgb);
  }
  std::vector<Curve> curves;
  for (TopExp_Explorer e(shape, TopAbs_EDGE, TopAbs_FACE); e.More(); e.Next()) {
    const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
    if (BRep_Tool::Degenerated(edge)) continue;
    BRepAdaptor_Curve c(edge);
    detail::emit(c, c.FirstParameter(), c.LastParameter(), top, Curve{}, tol, curves);
  }
  for (auto& c : curves) {
    c.z = 0;
    d.curve(layer, c, rgb);
  }
}

// ---------------------------------------------------------------- views

Display view_display(const ViewGeometry& g, const std::string& title) {
  Display d;
  d.title = title;
  const double w = g.bounds[2] - g.bounds[0], h = g.bounds[3] - g.bounds[1];
  d.pen_scale = std::max(1.0, 1 / fit_scale(w, h, 390, 267));  // true size, or the scale that fits A3 when that is smaller
  int visible = -1, tangent = -1, hidden = -1;
  for (const auto& k : g.curves) {
    int l;
    if (k.hidden) {
      if (hidden < 0) hidden = d.layer({"Hidden", kInk, LineType::Hidden, 0.25});
      l = hidden;
    } else if (k.kind == Curve::Kind::Tangent || k.kind == Curve::Kind::Seam || k.kind == Curve::Kind::Break) {
      if (tangent < 0) tangent = d.layer({"Tangent", kInk, LineType::Continuous, 0.25});
      l = tangent;
    } else {
      if (visible < 0) visible = d.layer({"Visible", kInk, LineType::Continuous, 0.5});
      l = visible;
    }
    d.curve(l, k);
  }
  // Layers in a fixed order (visible first) whichever curve came first.
  std::vector<int> order;
  for (const char* name : {"Visible", "Tangent", "Hidden"})
    for (size_t i = 0; i < d.layers.size(); ++i)
      if (d.layers[i].name == name) order.push_back(static_cast<int>(i));
  std::vector<int> renumber(d.layers.size());
  std::vector<Layer> layers;
  for (size_t i = 0; i < order.size(); ++i) {
    renumber[static_cast<size_t>(order[i])] = static_cast<int>(i);
    layers.push_back(d.layers[static_cast<size_t>(order[i])]);
  }
  d.layers = std::move(layers);
  for (auto& p : d.prims) p.layer = renumber[static_cast<size_t>(p.layer)];
  std::stable_sort(d.prims.begin(), d.prims.end(), [](const Prim& a, const Prim& b) { return a.layer < b.layer; });
  return d;
}

// ---------------------------------------------------------------- dimensions

void linear_dimension(Display& d, int layer, Vec2 a, Vec2 b, Vec2 axis, Vec2 place, const std::string& text, const DimStyle& s) {
  const Vec2 u = unit(axis), n = left(u);
  const Vec2 a1 = add(a, mul(n, dot(sub(place, a), n))), b1 = add(b, mul(n, dot(sub(place, b), n)));
  const double gap = s.gap * s.scale, over = s.overshoot * s.scale, arrow = s.arrow * s.scale;
  for (const auto& [from, to] : {std::pair{a, a1}, std::pair{b, b1}}) {
    const Vec2 off = sub(to, from);
    if (len(off) <= gap) continue;
    const Vec2 dir = unit(off);
    d.line(layer, add(from, mul(dir, gap)), add(to, mul(dir, over)));
  }
  const double length = len(sub(b1, a1));
  const Vec2 along = unit(sub(b1, a1), u);
  if (length > 2.5 * arrow) {  // arrows inside, pointing out to the extension lines
    d.line(layer, a1, b1);
    arrowhead(d, layer, a1, mul(along, -1), s);
    arrowhead(d, layer, b1, along, s);
  } else {  // too short: arrows outside, pointing in
    d.line(layer, sub(a1, mul(along, 2 * arrow)), add(b1, mul(along, 2 * arrow)));
    arrowhead(d, layer, a1, along, s);
    arrowhead(d, layer, b1, mul(along, -1), s);
  }
  const double angle = readable(std::atan2(along[1], along[0]));
  const Vec2 up = left({std::cos(angle), std::sin(angle)});
  rich_text(d, layer, text, add(mul(add(a1, b1), 0.5), mul(up, gap)), s.text * s.scale, angle, 1, 0);
}

void radial_dimension(Display& d, int layer, Vec2 centre, double r, Vec2 place, const std::string& text, bool diameter, const DimStyle& s) {
  const Vec2 dir = unit(sub(place, centre));
  const Vec2 rim = add(centre, mul(dir, r)), far = len(sub(place, centre)) > r ? place : rim;
  d.line(layer, diameter ? sub(centre, mul(dir, r)) : centre, far);
  arrowhead(d, layer, rim, dir, s);
  if (diameter) arrowhead(d, layer, sub(centre, mul(dir, r)), mul(dir, -1), s);
  const double angle = readable(std::atan2(dir[1], dir[0]));
  const Vec2 up = left({std::cos(angle), std::sin(angle)});
  const Vec2 at = far == rim ? add(centre, mul(dir, r * 0.5)) : sub(far, mul(dir, 0.5 * text_width(text, s.text * s.scale)));
  rich_text(d, layer, text, add(at, mul(up, s.gap * s.scale)), s.text * s.scale, angle, 1, 0);
}

void angular_dimension(Display& d, int layer, std::array<Vec2, 2> a, std::array<Vec2, 2> b, Vec2 place, const std::string& text, const DimStyle& s) {
  const auto cross = [](Vec2 p, Vec2 q) { return p[0] * q[1] - p[1] * q[0]; };
  const Vec2 da = sub(a[1], a[0]), db = sub(b[1], b[0]);
  const double c = cross(da, db), h = s.text * s.scale, gap = s.gap * s.scale;
  if (std::abs(c) <= 1e-9 * len(da) * len(db)) {
    rich_text(d, layer, text, place, h, 0, 1, 0);
    return;
  }
  const Vec2 v = add(a[0], mul(da, cross(sub(b[0], a[0]), db) / c));  // where the lines meet
  const Vec2 q = sub(place, v);
  const double r = std::max(len(q), 4 * s.arrow * s.scale);
  // The rays bounding the corner that holds `place`: it is a sum of them with both weights positive.
  Vec2 ua = unit(da), ub = unit(db);
  if (cross(q, ub) / cross(ua, ub) < 0) ua = mul(ua, -1);
  if (cross(ua, q) / cross(ua, ub) < 0) ub = mul(ub, -1);
  if (cross(ua, ub) < 0) std::swap(ua, ub), std::swap(a, b);  // counter-clockwise from ua to ub
  const double t0 = std::atan2(ua[1], ua[0]), t1 = std::atan2(ub[1], ub[0]);
  d.arc(layer, v, r, t0, t1);
  arrowhead(d, layer, add(v, mul(ua, r)), {ua[1], -ua[0]}, s);
  arrowhead(d, layer, add(v, mul(ub, r)), {-ub[1], ub[0]}, s);
  for (const auto& [u, ends] : {std::pair{ua, a}, std::pair{ub, b}}) {
    const double reach = std::max(dot(sub(ends[0], v), u), dot(sub(ends[1], v), u));
    if (r > reach + gap) d.line(layer, add(v, mul(u, std::max(reach, 0.0) + gap)), add(v, mul(u, r + s.overshoot * s.scale)));
  }
  const double mid = t0 + std::remainder(t1 - t0, 2 * M_PI) / 2;  // the sweep is under a half turn
  const double along = mid - M_PI / 2, angle = readable(along);
  rich_text(d, layer, text, add(v, mul({std::cos(mid), std::sin(mid)}, r + gap)), h, angle, 1, std::abs(std::remainder(angle - along, 2 * M_PI)) > 1e-9 ? 3 : 0);
}

// ---------------------------------------------------------------- files

namespace {
PaintWriter& painter() {
  static PaintWriter w;
  return w;
}
}  // namespace

void set_paint_writer(PaintWriter writer) { painter() = std::move(writer); }
bool can_paint() { return static_cast<bool>(painter()); }

json write_pages(const std::vector<const Display*>& pages, const std::filesystem::path& file, const std::string& format, int decimals, const json& options) {
  if (pages.empty()) throw Error("nothing to write");
  if (pages.size() == 1) return write_drawing(*pages[0], file, format, decimals, options);
  if (format != "pdf") throw Error("several sheets go into one PDF (a page each), or one sheet at a time into " + format);
  if (!can_paint()) throw Error("PDF and PNG drawings are written by the OPAD app and opad-cli, not by this build");
  if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
  return painter()(pages, file, format, options);
}

json write_drawing(const Display& d, const std::filesystem::path& file, const std::string& format, int decimals, const json& options) {
  if (format == "pdf" || format == "png") {
    if (!can_paint()) throw Error("PDF and PNG drawings are written by the OPAD app and opad-cli, not by this build");
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
    return painter()({&d}, file, format, options);
  }
  const std::string text = format == "dxf" ? dxf_text(d, decimals) : format == "svg" ? svg_text(d, decimals) : throw Error("2D formats are dxf, svg, pdf and png, not " + format);
  if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
  write_text_file(file, text);
  return json::object();
}

}  // namespace opad::drawing
