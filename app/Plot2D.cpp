#include "Plot2D.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "opad/geometry.hpp"

namespace plot {
opad::Frame plane(const opad::Document& doc, const opad::Scene& scene, const opad::Frame& fallback) {
  const auto frames = drawing2d::drawingFrames(doc, scene);
  if (frames.empty()) return fallback;
  const opad::Frame first = drawing2d::planeOf(frames.front());
  const opad::Vec3 n = first.normal();
  for (const auto& f : frames) {
    const opad::Frame p = drawing2d::planeOf(f);
    const opad::Vec3 m = p.normal();
    const double along = (p.origin[0] - first.origin[0]) * n[0] + (p.origin[1] - first.origin[1]) * n[1] + (p.origin[2] - first.origin[2]) * n[2];
    if (std::abs(n[0] * m[0] + n[1] * m[1] + n[2] * m[2]) < 1 - 1e-9 || std::abs(along) > 1e-6) return fallback;  // drawings in other planes: as seen
  }
  return first;
}

namespace {
// The edge's points in the plane, from its start to its end as the wire walks it (reversed: back), fine enough for paper.
void sample(const TopoDS_Edge& e, const opad::Frame& plane, double tolerance, std::vector<Pt>& out) {
  try {
    const BRepAdaptor_Curve c(e);
    const double t0 = c.FirstParameter(), t1 = c.LastParameter();
    std::vector<gp_Pnt> pts;
    if (c.GetType() == GeomAbs_Line) {
      pts = {c.Value(t0), c.Value(t1)};
    } else {
      GCPnts_TangentialDeflection sampler(c, t0, t1, 0.05, tolerance, 2);
      for (int i = 1; i <= sampler.NbPoints(); ++i) pts.push_back(sampler.Value(i));
    }
    if (e.Orientation() == TopAbs_REVERSED) std::reverse(pts.begin(), pts.end());
    for (const auto& p : pts) {
      double u, v;
      plane.to_local({p.X(), p.Y(), p.Z()}, u, v);
      if (out.empty() || std::abs(out.back()[0] - u) > 1e-12 || std::abs(out.back()[1] - v) > 1e-12) out.push_back({u, v});
    }
  } catch (const Standard_Failure&) {
  }
}
}  // namespace

Sheet collect(const opad::Document& doc, const opad::Scene& scene, const opad::Frame& plane, const std::function<bool()>& cancelled) {
  Sheet sheet;
  sheet.plane = plane;
  std::vector<std::pair<TopoDS_Shape, int>> shapes;
  Bnd_Box box;
  for (const auto& id : scene.all_bodies()) {
    const opad::Node* node = scene.node(id);
    if (!node || node->representation != "drawing2d" || node->body_missing || !scene.effectively_visible(id)) continue;
    const auto layer = drawing2d::layerAt(scene, id);
    if (layer && !layer->plot) continue;
    if (!node->raster.is_null()) {  // an image: its corners where the body is placed
      try {
        const opad::Mat4 world = scene.world(id);
        std::array<Pt, 3> at;
        for (int i = 0; i < 3; ++i) plane.to_local(world.apply(node->raster.at("corners").at(i).get<opad::Vec3>()), at[i][0], at[i][1]);
        sheet.images.push_back({node->raster.value("href", ""), node->raster.value("preserveAspectRatio", ""), at[0], at[1], at[2]});
        ++sheet.bodies;
      } catch (const std::exception&) {
      }
      continue;
    }
    Style style;
    style.ink = !node->has_color;
    style.color = node->color;
    const drawing2d::LineStyle line = drawing2d::lineStyle(scene, *node);  // its layer's, or its own (UI-92)
    style.weight = line.lineweight;
    style.dashes = drawing2d::dashes(line.linetype, line.pattern);
    for (double& d : style.dashes) d *= line.scale;
    auto found = std::find(sheet.styles.begin(), sheet.styles.end(), style);
    if (found == sheet.styles.end()) found = sheet.styles.insert(sheet.styles.end(), style);
    shapes.push_back({opad::node_world_shape(doc, scene, id), int(found - sheet.styles.begin())});
    BRepBndLib::Add(shapes.back().first, box);
    ++sheet.bodies;
  }
  if (box.IsVoid() && sheet.images.empty()) return sheet;
  const double tolerance = box.IsVoid() ? 1e-4 : std::max(std::sqrt(box.SquareExtent()) * 2e-5, 1e-4);
  for (const auto& [shape, style] : shapes) {
    if (cancelled && cancelled()) throw opad::Error("cancelled");
    for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
      Item fill{style, true, false, {}};
      const TopoDS_Face& face = TopoDS::Face(f.Current());
      for (TopExp_Explorer w(face, TopAbs_WIRE); w.More(); w.Next()) {
        std::vector<Pt> ring;
        for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current()), face); e.More(); e.Next()) sample(e.Current(), plane, tolerance, ring);
        if (ring.size() > 2) fill.rings.push_back(std::move(ring));
      }
      if (!fill.rings.empty()) sheet.items.push_back(std::move(fill));
    }
    for (TopExp_Explorer e(shape, TopAbs_EDGE, TopAbs_FACE); e.More(); e.Next()) {
      if (BRep_Tool::Degenerated(TopoDS::Edge(e.Current()))) continue;
      std::vector<Pt> line;
      sample(TopoDS::Edge(e.Current()), plane, tolerance, line);
      if (line.size() > 1) sheet.items.push_back({style, false, false, {std::move(line)}});
    }
    for (TopExp_Explorer v(shape, TopAbs_VERTEX, TopAbs_EDGE); v.More(); v.Next()) {
      const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
      double u, w;
      plane.to_local({p.X(), p.Y(), p.Z()}, u, w);
      sheet.items.push_back({style, false, true, {{{u, w}}}});
    }
  }
  auto grow = [&](const Pt& p) {
    if (sheet.x0 > sheet.x1) sheet.x0 = sheet.x1 = p[0], sheet.y0 = sheet.y1 = p[1];
    sheet.x0 = std::min(sheet.x0, p[0]), sheet.x1 = std::max(sheet.x1, p[0]), sheet.y0 = std::min(sheet.y0, p[1]), sheet.y1 = std::max(sheet.y1, p[1]);
  };
  for (const Item& item : sheet.items)
    for (const auto& ring : item.rings)
      for (const Pt& p : ring) grow(p);
  for (const Image& image : sheet.images)
    for (const Pt& p : {image.origin, image.right, image.down, Pt{image.right[0] + image.down[0] - image.origin[0], image.right[1] + image.down[1] - image.origin[1]}}) grow(p);
  return sheet;
}

Area areaOf(const Sheet& sheet, const Settings& settings) {
  Area a = settings.region == Region::Display ? settings.display : settings.region == Region::Window ? settings.window : Area{sheet.x0, sheet.y0, sheet.x1, sheet.y1};
  if (a.x0 > a.x1) std::swap(a.x0, a.x1);
  if (a.y0 > a.y1) std::swap(a.y0, a.y1);
  // A line along x or y alone: room for its width.
  const double pad = std::max({a.width(), a.height(), 1.0}) * 1e-3;
  if (a.width() < pad) a.x0 -= pad / 2, a.x1 += pad / 2;
  if (a.height() < pad) a.y0 -= pad / 2, a.y1 += pad / 2;
  return a;
}

Placement place(const Sheet& sheet, const Settings& settings) {
  Placement out;
  if (settings.region == Region::Extents && sheet.x0 > sheet.x1) return out;
  out.area = areaOf(sheet, settings);
  const double w = settings.paperWidth - 2 * settings.margin, h = settings.paperHeight - 2 * settings.margin;
  if (w <= 0 || h <= 0 || !(out.area.width() > 0) || !(out.area.height() > 0)) return out;
  out.scale = settings.fit ? std::min(w / out.area.width(), h / out.area.height()) : settings.scale;
  out.x = settings.margin + (w - out.area.width() * out.scale) / 2;
  out.y = settings.margin + (h - out.area.height() * out.scale) / 2;
  out.clipped = out.area.width() * out.scale > w * (1 + 1e-9) || out.area.height() * out.scale > h * (1 + 1e-9);
  return out;
}

drawing2d::Rgb paperColor(const Style& style, const Settings& settings) { return settings.monochrome || style.ink ? drawing2d::Rgb{0, 0, 0} : style.color; }

double paperWeight(const Style& style, const Settings& settings) {
  if (!settings.lineweights) return kThinnest;
  return style.weight < 0 ? kDefaultWeight : std::max(style.weight, 0.05);
}

std::string scaleText(double scale) {
  auto number = [](double v) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", v);
    std::string s = text;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
  };
  if (!(scale > 0)) return "-";
  return scale >= 1 ? number(scale) + ":1" : "1:" + number(1 / scale);
}
}  // namespace plot
