// A drawing sheet as a drawing::Display (TODO 11 UI-86): the paper (template or frame), the views placed by layout() and
// projected, the dimensions drawn as geometry from their references now, the notes; every writer (DXF, SVG, PDF, PNG)
// takes it, and the sheet canvas (UI-78) shows its parts one by one.
#include <algorithm>
#include <cmath>
#include <map>

#include "opad/drawing/annotate.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/drawing/symbols.hpp"
#include "views_internal.hpp"

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

void draw_view(Display& d, const ViewFrame& f, const SheetView& v, const ViewGeometry& g, const Document* doc, const Scene* scene) {
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
  detail::draw_section_faces(d, f, v, g, doc, scene);
  if (style.value("centermarks", false)) {
    std::vector<std::array<Vec2, 2>> axes;
    if (doc && scene) axes = cylinder_axes(*doc, *scene, f, view_spec(*scene, v));
    if (f.shaped()) {  // the axes within what the view keeps
      std::vector<std::array<Vec2, 2>> kept;
      for (const auto& [a, b] : axes) {
        Curve line;
        line.pts = {f.unfold({f.centre[0] + (a[0] - f.at[0]) / f.scale, f.centre[1] + (a[1] - f.at[1]) / f.scale}),
                    f.unfold({f.centre[0] + (b[0] - f.at[0]) / f.scale, f.centre[1] + (b[1] - f.at[1]) / f.scale})};
        ViewGeometry one;
        one.curves.push_back(line);
        for (const auto& c : shape_linework(std::make_shared<const ViewGeometry>(one), f)->curves)
          kept.push_back({placed(c, f).pts.front(), placed(c, f).pts.back()});
      }
      axes.swap(kept);
    }
    view_centre_marks(d, f, g, {}, doc && scene ? &axes : nullptr);
  }
  if (scene) detail::draw_view_marks(d, f, v, *scene);
  tag(d, from, v.id);
}

int draw_items(Display& d, const Document& doc, const Scene& scene, const Sheet& sheet, const std::vector<ViewFrame>& frames, const std::string& view,
               json& skipped) {
  const int dims = d.layer({"Dimensions", kInk, LineType::Continuous, 0.25});
  d.layer({"Text", kInk, LineType::Continuous, 0.25});
  std::map<std::string, const ViewFrame*> by_id;
  for (const auto& f : frames) by_id[f.id] = &f;
  DimStyle style;
  int items = 0;
  for (const auto& id : sheet.items) {
    const SheetItem* t = scene.sheet_item(id);
    if (!t || t->view != view) continue;
    const size_t from = d.prims.size(), records = d.dimensions.size();
    const ViewFrame* f = t->view.empty() ? nullptr : by_id.count(t->view) ? by_id[t->view] : nullptr;
    const Vec2 origin = f ? f->at : Vec2{0, 0};
    if (!known_item(t->kind, t->type) && t->kind != "note") {
      skipped.push_back({{"id", id}, {"error", t->error.empty() ? "needs a newer OPAD (sheet_item kind '" + t->kind + "')" : t->error}});
      continue;
    }
    try {
      if (!t->error.empty()) throw Error(t->error);
      if (!t->view.empty() && (!f || !f->error.empty())) throw Error("its view cannot be drawn");
      draw_item(d, sheet, t->def, measure_item(doc, scene, sheet, *t, f), origin, style);
      ++items;
    } catch (const std::exception& e) {  // dangling: what it showed when it was made, in magenta, where it stood
      d.prims.resize(from);
      d.dimensions.resize(records);
      const json& def = t->def;
      const json result = def.value("result", json::object());
      std::string shown = result.value("shown", json()).is_string() ? result["shown"].get<std::string>() : std::string();
      if (result.value("shown", json()).is_array())
        for (const auto& s : result["shown"]) shown += (shown.empty() ? "" : "  ") + s.get<std::string>();
      if (shown.empty()) shown = def.value("letter", def.value("text", def.value("value", json("?")).is_string() ? def.value("value", "?") : std::string("?")));
      const Vec2 at = def.contains("place") ? vec2(def["place"].value("text", json())) : vec2(def.value("at", json()));
      rich_text(d, dims, shown, plus(t->kind == "hole_table" ? Vec2{0, 0} : origin, at), style.text, 0, 1, 0, 0xFF00FF);
      skipped.push_back({{"id", id}, {"error", e.what()}});
    }
    tag(d, from, id);
    for (size_t k = records; k < d.dimensions.size(); ++k) d.dimensions[k].source = id;
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
    const auto g = shape_linework(project(doc, scene, view_spec(scene, *v), [&](double t, const std::string& phase) {
      return !progress || progress(t < 0 ? -1 : (static_cast<double>(i) + t) / n, phase);
    }), f);
    draw_view(d, f, *v, *g, &doc, &scene);
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
