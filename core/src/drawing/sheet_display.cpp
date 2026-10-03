// A drawing sheet as a drawing::Display (TODO 11 UI-86): the frame, the views placed by layout() and projected, the
// dimensions drawn as geometry from their references now, the notes; every writer (DXF, SVG, PDF, PNG) takes it.
#include <cmath>
#include <map>

#include "opad/drawing/sheet.hpp"

namespace opad::drawing {
namespace {

Vec2 vec2(const json& j, Vec2 fallback = {0, 0}) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) return fallback;
  return {j[0].get<double>(), j[1].get<double>()};
}
Vec2 plus(Vec2 a, Vec2 b) { return {a[0] + b[0], a[1] + b[1]}; }

// A curve of a view moved onto the sheet: model point p -> at + scale * (p - centre) (no turn, one scale).
Curve placed(Curve c, const ViewFrame& f) {
  const auto map = [&](Vec2 p) { return Vec2{f.at[0] + f.scale * (p[0] - f.centre[0]), f.at[1] + f.scale * (p[1] - f.centre[1])}; };
  for (auto& p : c.pts) p = map(p);
  c.c = map(c.c);
  c.r1 *= f.scale;
  c.r2 *= f.scale;
  return c;
}

}  // namespace

Display sheet_display(const Document& doc, const Scene& scene, const Sheet& sheet, const ProjectionProgress& progress, json* report) {
  Display d;
  d.title = sheet.name;
  d.paper = {0, 0, sheet.width, sheet.height};
  const int frame = d.layer({"Frame", kInk, LineType::Continuous, 0.7});
  const int visible = d.layer({"Visible", kInk, LineType::Continuous, 0.5});
  const int tangent = d.layer({"Tangent", kInk, LineType::Continuous, 0.25});
  const int hidden = d.layer({"Hidden", kInk, LineType::Hidden, 0.25});
  const int dims = d.layer({"Dimensions", kInk, LineType::Continuous, 0.25});
  const int notes = d.layer({"Text", kInk, LineType::Continuous, 0.25});
  const double w = sheet.width, h = sheet.height;
  if (w > 60 && h > 40) {  // ISO 5457: the frame, a centring mark in the middle of each side from 5 mm outside to 5 inside
    d.polyline(frame, {{20, 10}, {w - 10, 10}, {w - 10, h - 10}, {20, h - 10}}, true);
    const double cx = (20 + w - 10) / 2, cy = h / 2;
    d.line(frame, {cx, 5}, {cx, 15});
    d.line(frame, {cx, h - 5}, {cx, h - 15});
    d.line(frame, {15, cy}, {25, cy});
    d.line(frame, {w - 5, cy}, {w - 15, cy});
  }
  json skipped = json::array();
  int views = 0, items = 0, bodies = 0;
  const auto frames = layout(doc, scene, sheet);
  std::map<std::string, const ViewFrame*> by_id;
  for (const auto& f : frames) by_id[f.id] = &f;
  for (size_t i = 0; i < frames.size(); ++i) {
    const ViewFrame& f = frames[i];
    const SheetView* v = scene.sheet_view(f.id);
    if (!f.error.empty() || !v) {
      skipped.push_back({{"id", f.id}, {"error", f.error}});
      continue;
    }
    const ViewSpec spec = view_spec(scene, *v);
    const double n = static_cast<double>(frames.size());
    const auto g = project(doc, scene, spec, [&](double t, const std::string& phase) {
      return !progress || progress(t < 0 ? -1 : (static_cast<double>(i) + t) / n, phase);
    });
    const json style = v->def.value("style", json::object());
    const bool thin = !style.contains("tangent") || style["tangent"] != "show";
    for (const auto& c : g->curves) {
      const bool smooth = c.kind == Curve::Kind::Tangent || c.kind == Curve::Kind::Seam;
      d.curve(c.hidden ? hidden : smooth && thin ? tangent : visible, placed(c, f));
    }
    bodies += static_cast<int>(g->bodies.size());
    ++views;
  }
  DimStyle style;
  for (const auto& id : sheet.items) {
    const SheetItem* t = scene.sheet_item(id);
    if (!t) continue;
    const ViewFrame* f = t->view.empty() ? nullptr : by_id.count(t->view) ? by_id[t->view] : nullptr;
    const Vec2 origin = f ? f->at : Vec2{0, 0};
    if (t->kind == "note") {
      d.text(notes, t->def.value("text", ""), plus(origin, vec2(t->def.value("at", json()))), t->def.value("height", 3.5));
      ++items;
      continue;
    }
    if (t->kind != "dimension") {
      skipped.push_back({{"id", id}, {"error", "needs a newer OPAD (sheet_item kind '" + t->kind + "')"}});
      continue;
    }
    const Vec2 place = plus(origin, vec2(t->def.value("place", json::object()).value("text", json())));
    try {
      if (!t->error.empty()) throw Error(t->error);
      if (!f || !f->error.empty()) throw Error("its view cannot be drawn");
      const json now = evaluate_item(doc, scene, sheet, *t, *f);
      const json& g = now["geometry"];
      const std::string text = now["shown"].get<std::string>();
      if (g.contains("lines")) {
        const json& l = g["lines"];
        angular_dimension(d, dims, {plus(origin, vec2(l[0][0])), plus(origin, vec2(l[0][1]))}, {plus(origin, vec2(l[1][0])), plus(origin, vec2(l[1][1]))}, place, text, style);
      } else if (g.contains("centre")) {
        radial_dimension(d, dims, plus(origin, vec2(g["centre"])), g["r"].get<double>(), place, text, t->type == "diameter", style);
      } else {
        const Vec2 a = plus(origin, vec2(g["from"])), b = plus(origin, vec2(g["to"]));
        const Vec2 axis = t->type == "horizontal" ? Vec2{1, 0} : t->type == "vertical" ? Vec2{0, 1} : Vec2{b[0] - a[0], b[1] - a[1]};
        linear_dimension(d, dims, a, b, axis, place, text, style);
      }
      ++items;
    } catch (const std::exception& e) {  // dangling: the value it was made with, in magenta, where its text was
      d.text(dims, t->def.value("result", json::object()).value("shown", "?"), place, style.text, 0, 1, 0, 0xFF00FF);
      skipped.push_back({{"id", id}, {"error", e.what()}});
    }
  }
  if (report) *report = {{"views", views}, {"items", items}, {"bodies", bodies}, {"skipped", skipped}};
  return d;
}

}  // namespace opad::drawing
