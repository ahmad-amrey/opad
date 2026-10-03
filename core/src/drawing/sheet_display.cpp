// A drawing sheet as a drawing::Display (TODO 11 UI-86): the paper (template or frame), the views placed by layout() and
// projected, the dimensions drawn as geometry from their references now, the notes; every writer (DXF, SVG, PDF, PNG)
// takes it, and the sheet canvas (UI-78) shows its parts one by one.
#include <algorithm>
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

void tag(Display& d, size_t from, const std::string& source) {
  for (size_t i = from; i < d.prims.size(); ++i) d.prims[i].source = source;
}

}  // namespace

void draw_view(Display& d, const ViewFrame& f, const SheetView& v, const ViewGeometry& g) {
  const int visible = d.layer({"Visible", kInk, LineType::Continuous, 0.5});
  const int tangent = d.layer({"Tangent", kInk, LineType::Continuous, 0.25});
  const int hidden = d.layer({"Hidden", kInk, LineType::Hidden, 0.25});
  const json style = v.def.value("style", json::object());
  const bool thin = !style.contains("tangent") || style["tangent"] != "show";
  const size_t from = d.prims.size();
  for (const auto& c : g.curves) {
    const bool smooth = c.kind == Curve::Kind::Tangent || c.kind == Curve::Kind::Seam;
    d.curve(c.hidden ? hidden : smooth && thin ? tangent : visible, placed(c, f));
  }
  tag(d, from, v.id);
}

int draw_items(Display& d, const Document& doc, const Scene& scene, const Sheet& sheet, const std::vector<ViewFrame>& frames, const std::string& view,
               json& skipped) {
  const int dims = d.layer({"Dimensions", kInk, LineType::Continuous, 0.25});
  const int notes = d.layer({"Text", kInk, LineType::Continuous, 0.25});
  std::map<std::string, const ViewFrame*> by_id;
  for (const auto& f : frames) by_id[f.id] = &f;
  DimStyle style;
  int items = 0;
  for (const auto& id : sheet.items) {
    const SheetItem* t = scene.sheet_item(id);
    if (!t || t->view != view) continue;
    const size_t from = d.prims.size();
    const ViewFrame* f = t->view.empty() ? nullptr : by_id.count(t->view) ? by_id[t->view] : nullptr;
    const Vec2 origin = f ? f->at : Vec2{0, 0};
    if (t->kind == "note") {
      d.text(notes, t->def.value("text", ""), plus(origin, vec2(t->def.value("at", json()))), t->def.value("height", 3.5));
      tag(d, from, id);
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
      d.prims.resize(from);
      d.text(dims, t->def.value("result", json::object()).value("shown", "?"), place, style.text, 0, 1, 0, 0xFF00FF);
      skipped.push_back({{"id", id}, {"error", e.what()}});
    }
    tag(d, from, id);
  }
  return items;
}

Display sheet_display(const Document& doc, const Scene& scene, const Sheet& sheet, const ProjectionProgress& progress, json* report) {
  Display d;
  d.title = sheet.name;
  d.paper = {0, 0, sheet.width, sheet.height};
  // The layers in a fixed order whatever is on the sheet (DXF tables, SVG groups), the template's after them.
  d.layer({"Frame", kInk, LineType::Continuous, 0.7});
  d.layer({"Visible", kInk, LineType::Continuous, 0.5});
  d.layer({"Tangent", kInk, LineType::Continuous, 0.25});
  d.layer({"Hidden", kInk, LineType::Hidden, 0.25});
  d.layer({"Dimensions", kInk, LineType::Continuous, 0.25});
  d.layer({"Text", kInk, LineType::Continuous, 0.25});
  draw_paper(d, doc, scene, sheet);
  json skipped = json::array();
  int views = 0, items = 0, bodies = 0;
  const auto frames = layout(doc, scene, sheet);
  for (size_t i = 0; i < frames.size(); ++i) {
    const ViewFrame& f = frames[i];
    const SheetView* v = scene.sheet_view(f.id);
    if (!f.error.empty() || !v) {
      skipped.push_back({{"id", f.id}, {"error", f.error}});
      continue;
    }
    const double n = static_cast<double>(frames.size());
    const auto g = project(doc, scene, view_spec(scene, *v), [&](double t, const std::string& phase) {
      return !progress || progress(t < 0 ? -1 : (static_cast<double>(i) + t) / n, phase);
    });
    draw_view(d, f, *v, *g);
    bodies += static_cast<int>(g->bodies.size());
    ++views;
  }
  std::vector<std::string> owners;  // the views items hang on, the sheet's own ("") among them, in the items' order
  for (const auto& id : sheet.items)
    if (const SheetItem* t = scene.sheet_item(id); t && std::find(owners.begin(), owners.end(), t->view) == owners.end()) owners.push_back(t->view);
  for (const auto& owner : owners) items += draw_items(d, doc, scene, sheet, frames, owner, skipped);
  if (report) *report = {{"views", views}, {"items", items}, {"bodies", bodies}, {"skipped", skipped}};
  return d;
}

}  // namespace opad::drawing
