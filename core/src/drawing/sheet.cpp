// Drawing sheets (TODO 11 UI-76): paper sizes and scales, the records' checks, a view's projection and its place on the
// sheet, and dimension values.
#include "opad/drawing/sheet.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Elips.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>

#include "../design/engine.hpp"
#include "opad/geometry.hpp"
#include "opad/render.hpp"

namespace opad::drawing {
namespace {

double dot3(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross3(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
Vec3 scaled(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 plus3(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 minus3(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 unit(const Vec3& a) {
  const double l = std::sqrt(dot3(a, a));
  return l > 0 ? scaled(a, 1 / l) : a;
}
Vec3 of(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
Vec3 of(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }
Vec3 vec3(const json& j) {
  if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number()) throw Error("a direction is [x, y, z]");
  return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

bool finite(const json& v) { return v.is_number() && std::isfinite(v.get<double>()); }
bool point2(const json& v) { return v.is_array() && v.size() == 2 && finite(v[0]) && finite(v[1]); }
bool body_key(const json& v) {
  if (!v.is_string()) return false;
  const auto& s = v.get_ref<const std::string&>();
  return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// A projected view's side as a step on the sheet (x right, y up).
bool side_step(const std::string& side, int& sx, int& sy) {
  static const std::map<std::string, std::pair<int, int>> sides = {
      {"right", {1, 0}},      {"left", {-1, 0}},     {"top", {0, 1}},           {"bottom", {0, -1}},
      {"top-right", {1, 1}},  {"top-left", {-1, 1}}, {"bottom-right", {1, -1}}, {"bottom-left", {-1, -1}}};
  const auto it = sides.find(side);
  if (it == sides.end()) return false;
  sx = it->second.first;
  sy = it->second.second;
  return true;
}

std::string number(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 12), v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s == "-0" ? "0" : s;
}

void apply_style(ViewSpec& s, const json& style) {
  if (!style.is_object()) return;
  if (style.contains("hidden") && style["hidden"].is_boolean()) s.hidden = style["hidden"].get<bool>();
  if (style.contains("tangent")) {
    const json& t = style["tangent"];
    s.tangent = t.is_boolean() ? t.get<bool>() : t != "hide";
  }
  if (style.contains("silhouettes") && style["silhouettes"].is_boolean()) s.silhouettes = style["silhouettes"].get<bool>();
  if (style.contains("quality") && style["quality"].is_string()) s.quality = quality_from_name(style["quality"].get<std::string>());
}

// A view projected from `s` to the side (sx, sy) of it. First angle: the view right of its parent shows the object from the
// left; third angle from the right. Corners are pictorial views from that corner.
void turn_to_side(ViewSpec& s, int sx, int sy, bool third) {
  const double sign = third ? 1 : -1;
  Vec3 x, y, z;
  view_axes(s, x, y, z);
  if (sy == 0) {
    s.dir = scaled(x, sign * sx);
    s.up = y;
  } else if (sx == 0) {
    s.dir = scaled(y, sign * sy);
    s.up = cross3(s.dir, x);
  } else {
    s.dir = unit(plus3(plus3(scaled(x, sx), scaled(y, sy)), z));
    s.up = y;
  }
}

void apply_source(const Scene& scene, ViewSpec& s, const json& src) {
  if (!src.is_object()) return;
  s.nodes.clear();
  s.hide.clear();
  for (const char* key : {"nodes", "hide"})
    if (src.contains(key) && src[key].is_array())
      for (const auto& n : src[key]) {
        if (!n.is_string() || !scene.node(n.get<std::string>())) throw Error("its source node " + n.dump() + " does not exist");
        (std::string(key) == "nodes" ? s.nodes : s.hide).push_back(n.get<std::string>());
      }
  s.visible_only = src.value("visible_only", false);
}

ViewSpec spec_of(const Scene& scene, const SheetView& v, int depth) {
  if (depth > 32) throw Error("its parent views form a loop");
  const json& d = v.def;
  ViewSpec s;
  if (v.kind == "projected") {
    const SheetView* parent = scene.sheet_view(v.parent);
    if (!parent) throw Error("its parent view " + v.parent + " does not exist");
    s = spec_of(scene, *parent, depth + 1);
    int sx = 0, sy = 0;
    const std::string side = d.value("side", "");
    if (!side_step(side, sx, sy)) throw Error("side is left, right, top, bottom or a corner such as top-right, not '" + side + "'");
    const Sheet* sheet = scene.sheet(v.sheet);
    turn_to_side(s, sx, sy, sheet && sheet->projection == "third");
  } else if (v.kind == "base") {
    const json o = d.value("orient", json::object());
    if (o.contains("preset") && o["preset"].is_string()) {
      s = ViewSpec::preset(o["preset"].get<std::string>());
    } else if (o.contains("dir")) {
      s.dir = vec3(o["dir"]);
      if (o.contains("up")) s.up = vec3(o["up"]);
    } else if (o.contains("view") && o["view"].is_string()) {
      const std::string id = o["view"].get<std::string>();
      const auto it = std::find_if(scene.views.begin(), scene.views.end(), [&](const ViewBookmark& b) { return b.id == id; });
      if (it == scene.views.end()) throw Error("its view bookmark " + id + " does not exist");
      const Camera c = Camera::from_json(it->camera);
      s.dir = c.absolute ? minus3(c.eye, c.target) : c.eye;
      s.up = c.up;
    } else {
      throw Error("needs a newer OPAD (an orientation this build does not know)");
    }
    if (dot3(s.dir, s.dir) < 1e-24) throw Error("its view direction is zero");
    s.hidden = false;  // sheets draw hidden lines only when asked
  } else {
    throw Error("needs a newer OPAD (sheet_view kind '" + v.kind + "')");
  }
  if (d.contains("source")) apply_source(scene, s, d["source"]);
  apply_style(s, d.value("style", json()));
  return s;
}

}  // namespace

// ---------------------------------------------------------------- paper and scale
const std::vector<PaperSize>& paper_sizes() {
  static const std::vector<PaperSize> sizes = {
      {"A4", 210, 297},          {"A3", 297, 420},          {"A2", 420, 594},          {"A1", 594, 841},
      {"A0", 841, 1189},         {"ANSI-A", 215.9, 279.4},  {"ANSI-B", 279.4, 431.8}, {"ANSI-C", 431.8, 558.8},
      {"ANSI-D", 558.8, 863.6},  {"ANSI-E", 863.6, 1117.6}};
  return sizes;
}

json paper_size(const std::string& preset, bool landscape) {
  std::string want;
  for (char c : preset) want += c == ' ' || c == '_' ? '-' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (const auto& p : paper_sizes())
    if (want == p.name || "ANSI-" + want == p.name)
      return {{"preset", p.name}, {"w", landscape ? p.h : p.w}, {"h", landscape ? p.w : p.h}};
  throw Error("paper size is A4, A3, A2, A1, A0 or ANSI-A to ANSI-E, not '" + preset + "'");
}

double parse_scale(const std::string& text) {
  const size_t colon = text.find(':');
  double a = 0, b = 0;
  try {
    size_t used = 0;
    if (colon != std::string::npos) {
      a = std::stod(text.substr(0, colon), &used);
      if (used != colon) a = 0;
      const std::string rest = text.substr(colon + 1);
      b = std::stod(rest, &used);
      if (used != rest.size()) b = 0;
    }
  } catch (const std::exception&) {
  }
  if (!(a > 0 && b > 0 && std::isfinite(a) && std::isfinite(b))) throw Error("scale is a ratio such as 1:2 or 2:1, not '" + text + "'");
  return a / b;
}

std::string scale_text(double scale) { return scale >= 1 ? number(scale, 4) + ":1" : "1:" + number(1 / scale, 4); }

const std::vector<double>& standard_scales() {
  static const std::vector<double> s = {50, 20, 10, 5, 2, 1, 0.5, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.002, 0.001};
  return s;
}

double fit_scale(double model_w, double model_h, double room_w, double room_h) {
  if (model_w <= 0 && model_h <= 0) return 1;
  for (double s : standard_scales())
    if (model_w * s <= room_w && model_h * s <= room_h) return s;
  return standard_scales().back();
}

// ---------------------------------------------------------------- records
bool is_sheet_record(const std::string& op_type) { return op_type == "sheet" || op_type == "sheet_view" || op_type == "sheet_item"; }

void validate_record(const json& op) {
  const std::string type = op.value("op", "");
  const auto fail = [&](const std::string& why) { throw Error(type + ": " + why); };
  for (const char* k : {"sheet", "view", "parent"})
    if (op.contains(k) && !(op[k].is_string() && is_uuid(op[k].get<std::string>()))) fail(std::string("'") + k + "' must be an op id");
  for (const char* k : {"name", "drawing", "kind", "type", "standard", "projection", "units", "side", "scale", "text", "prefix", "suffix"})
    if (op.contains(k) && !op[k].is_string()) fail(std::string("'") + k + "' must be text");
  for (const char* k : {"size", "template", "values", "source", "orient", "style", "label", "place", "result", "tol", "frozen"})
    if (op.contains(k) && !op[k].is_object()) fail(std::string("'") + k + "' must be an object");
  for (const char* k : {"gap", "height", "precision"})
    if (op.contains(k) && !finite(op[k])) fail(std::string("'") + k + "' must be a number");
  if (op.contains("at") && !point2(op["at"])) fail("'at' must be [x, y] in paper mm");
  if (op.contains("place"))
    for (const auto& [k, v] : op["place"].items())
      if (!point2(v)) fail("place." + k + " must be [x, y] in paper mm");
  if (op.contains("refs")) {
    if (!op["refs"].is_array()) fail("'refs' must be a list of references");
    for (const auto& r : op["refs"]) Ref::from_json(r);
  }
  if (op.contains("frozen"))
    for (const auto& [k, v] : op["frozen"].items())
      if (!body_key(v)) fail("frozen linework must be body keys");
  if (type == "sheet") {
    if (!op.contains("name")) fail("needs a name");
    const json size = op.value("size", json());
    if (!size.is_object() || !finite(size.value("w", json())) || !finite(size.value("h", json())) || size["w"].get<double>() <= 0 ||
        size["h"].get<double>() <= 0)
      fail("size needs w and h, the paper's width and height in mm");
    if (op.contains("template") && op["template"].contains("geometry") && !op["template"]["geometry"].is_null() &&
        !body_key(op["template"]["geometry"]))
      fail("a template's geometry is a body key");
  } else if (type == "sheet_view" || type == "sheet_item") {
    if (!op.contains("sheet")) fail("needs its sheet");
    if (!op.contains("kind")) fail("needs a kind");
  }
}

void record_body_keys(const json& op, std::vector<std::string>& keys) {
  if (op.contains("template") && op["template"].is_object() && body_key(op["template"].value("geometry", json())))
    keys.push_back(op["template"]["geometry"].get<std::string>());
  if (op.contains("frozen") && op["frozen"].is_object())
    for (const auto& [k, v] : op["frozen"].items())
      if (body_key(v)) keys.push_back(v.get<std::string>());
}

bool is_drawing_op(const std::string& op_type) { return is_sheet_record(op_type) || op_type == "properties"; }

// ---------------------------------------------------------------- views
ViewSpec view_spec(const Scene& scene, const SheetView& view) { return spec_of(scene, view, 0); }

std::string view_orientation(const Scene& scene, const SheetView& view) {
  if (!view.error.empty()) return "";
  try {
    const Vec3 dir = unit(view_spec(scene, view).dir);
    for (const char* name : {"front", "top", "right", "left", "back", "bottom", "iso", "iso-back"})
      if (dot3(dir, unit(Camera::preset(name).eye)) > 1 - 1e-9) return name;
  } catch (const std::exception&) {
  }
  return "";
}

std::vector<OutlineRow> outline(const Scene& scene) {
  std::vector<OutlineRow> out;
  std::map<std::string, size_t> drawings;
  std::unordered_map<std::string, const SheetView*> views;
  std::unordered_map<std::string, const SheetItem*> items;
  std::unordered_map<std::string, std::string> why;
  for (const auto& v : scene.sheet_views) views[v.id] = &v;
  for (const auto& t : scene.sheet_items) items[t.id] = &t;
  for (const auto& u : scene.unresolved) why[u.op_id] = u.reason;
  for (const auto& s : scene.sheets) {
    OutlineRow sheet{s.id, "sheet", s.name, "", why.count(s.id) ? why[s.id] : "", {}};
    std::map<std::string, size_t> view_rows;
    for (const auto& id : s.views) {
      const SheetView* v = views.at(id);
      view_rows[id] = sheet.children.size();
      sheet.children.push_back({id, "view", v->name, v->name.empty() ? view_orientation(scene, *v) : "", v->error, {}});
    }
    for (const auto& id : s.items) {
      const SheetItem* t = items.at(id);
      std::string name = t->kind == "dimension" ? t->def.value("result", json::object()).value("shown", "") : t->def.value("text", "");
      name = name.substr(0, name.find('\n'));
      OutlineRow row{id, "item", name, "", t->error, {}};
      if (const auto at = view_rows.find(t->view); at != view_rows.end()) sheet.children[at->second].children.push_back(std::move(row));
      else sheet.children.push_back(std::move(row));
    }
    if (s.drawing.empty()) {
      out.push_back(std::move(sheet));
      continue;
    }
    auto at = drawings.find(s.drawing);
    if (at == drawings.end()) {
      at = drawings.emplace(s.drawing, out.size()).first;
      out.push_back({"drawing:" + s.drawing, "drawing", s.drawing, "", "", {}});
    }
    out[at->second].children.push_back(std::move(sheet));
  }
  return out;
}

Vec2 ViewFrame::view(const Vec3& p) const { return {dot3(p, x), dot3(p, y)}; }

Vec2 ViewFrame::paper(const Vec3& p) const {
  const Vec2 v = view(p);
  return {at[0] + scale * (v[0] - centre[0]), at[1] + scale * (v[1] - centre[1])};
}

json ViewFrame::to_json() const {
  json j = {{"id", id}, {"scale", scale_text(scale)}, {"at", at}, {"centre", centre}, {"box", box}, {"x", x}, {"y", y}, {"dir", dir}};
  if (!error.empty()) j["error"] = error;
  return j;
}

std::array<double, 4> view_extent(const Document& doc, const Scene& scene, const ViewSpec& spec) {
  // The corners of every body's tight box (moved by its explode offset), in view coordinates.
  Vec3 x, y, z;
  view_axes(spec, x, y, z);
  std::array<double, 4> e{1e300, 1e300, -1e300, -1e300};
  const auto bodies = view_bodies(scene, spec);
  std::vector<std::string> keys;  // measured side by side first (the Engine's boxes one by one: minutes)
  for (const auto& [node, world] : bodies)
    if (const Node* n = scene.node(node)) keys.push_back(n->body_key);
  warm_tight_bboxes(doc, keys);
  for (const auto& [node, world] : bodies) {
    const Bnd_Box b = node_tight_bbox(doc, scene, node, false);
    if (b.IsVoid()) continue;
    const Mat4 placed = scene.world(node);
    const Vec3 shift{world.at(0, 3) - placed.at(0, 3), world.at(1, 3) - placed.at(1, 3), world.at(2, 3) - placed.at(2, 3)};
    double c[6];
    b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
    for (int k = 0; k < 8; ++k) {
      const Vec3 p = plus3({c[(k & 1) ? 3 : 0], c[(k & 2) ? 4 : 1], c[(k & 4) ? 5 : 2]}, shift);
      const double u = dot3(p, x), v = dot3(p, y);
      e = {std::min(e[0], u), std::min(e[1], v), std::max(e[2], u), std::max(e[3], v)};
    }
  }
  if (e[0] > e[2]) e = {0, 0, 0, 0};
  return e;
}

std::vector<ViewFrame> layout(const Document& doc, const Scene& scene, const Sheet& sheet) {
  std::map<std::string, ViewFrame> done;
  std::function<const ViewFrame&(const std::string&, int)> place = [&](const std::string& id, int depth) -> const ViewFrame& {
    if (auto it = done.find(id); it != done.end()) return it->second;
    ViewFrame f;
    f.id = id;
    f.scale = sheet.scale;
    try {
      const SheetView* v = scene.sheet_view(id);
      if (!v) throw Error("view " + id + " does not exist");
      if (!v->error.empty()) throw Error(v->error);
      if (depth > 32) throw Error("its parent views form a loop");
      const ViewSpec spec = view_spec(scene, *v);
      view_axes(spec, f.x, f.y, f.dir);
      const std::array<double, 4> e = view_extent(doc, scene, spec);
      const double lo[2] = {e[0], e[1]}, hi[2] = {e[2], e[3]};
      f.centre = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2};
      const json& d = v->def;
      if (v->kind == "projected") {
        // Aligned with its parent: the coordinate they share is the parent's, the other puts `gap` between the frames.
        const ViewFrame& p = place(v->parent, depth + 1);
        if (!p.error.empty()) throw Error("its parent view cannot be drawn: " + p.error);
        f.scale = p.scale;
        int sx = 0, sy = 0;
        side_step(d.value("side", ""), sx, sy);
        const double gap = d.value("gap", 20.0);
        if (!d.value("align", true) && d.contains("at")) {
          f.at = {d["at"][0].get<double>(), d["at"][1].get<double>()};
        } else {
          if (sy == 0) f.centre[1] = p.centre[1], f.at[1] = p.at[1];
          if (sx == 0) f.centre[0] = p.centre[0], f.at[0] = p.at[0];
          if (sx > 0) f.at[0] = p.box[2] + gap + (f.centre[0] - lo[0]) * f.scale;
          if (sx < 0) f.at[0] = p.box[0] - gap - (hi[0] - f.centre[0]) * f.scale;
          if (sy > 0) f.at[1] = p.box[3] + gap + (f.centre[1] - lo[1]) * f.scale;
          if (sy < 0) f.at[1] = p.box[1] - gap - (hi[1] - f.centre[1]) * f.scale;
        }
      } else {
        if (const std::string s = d.value("scale", "sheet"); s != "sheet") f.scale = parse_scale(s);
        f.at = d.contains("at") ? Vec2{d["at"][0].get<double>(), d["at"][1].get<double>()} : Vec2{sheet.width / 2, sheet.height / 2};
      }
      f.box = {f.at[0] + (lo[0] - f.centre[0]) * f.scale, f.at[1] + (lo[1] - f.centre[1]) * f.scale,
               f.at[0] + (hi[0] - f.centre[0]) * f.scale, f.at[1] + (hi[1] - f.centre[1]) * f.scale};
    } catch (const std::exception& e) {
      f.error = e.what();
    }
    return done[id] = f;
  };
  std::vector<ViewFrame> out;
  for (const auto& id : sheet.views) out.push_back(place(id, 0));
  return out;
}

json plan_views(const Document& doc, const Scene& scene, const json& sheet, const std::vector<std::string>& views, const json& source,
                const std::string& scale, const json& style, double gap) {
  if (views.empty()) throw Error("a drawing needs at least one view");
  const bool third = sheet.value("projection", sheet.value("standard", "iso") == "asme" ? "third" : "first") == "third";
  struct Planned {
    std::string view, side;
    ViewSpec spec;
    int cx = 0, cy = 0;
    bool pictorial = false;
    double w = 0, h = 0;
  };
  const auto standard = [&](const std::string& name) {
    ViewSpec s = ViewSpec::preset(name);  // throws for a name it does not know
    apply_source(scene, s, source);
    s.hidden = false;
    apply_style(s, style);
    return s;
  };
  std::vector<Planned> plan(1);
  plan[0].view = views[0] == "side" ? "front" : views[0];
  plan[0].spec = standard(plan[0].view);
  std::set<std::pair<int, int>> used{{0, 0}};
  std::vector<std::string> pictorial;
  for (size_t i = 1; i < views.size(); ++i) {
    Planned p;
    p.view = views[i];
    p.spec = plan[0].spec;
    if (p.view == "side") {
      p.side = "right";
    } else {
      const Vec3 want = unit(Camera::preset(p.view).eye);
      for (const char* side : {"right", "left", "top", "bottom"}) {
        int sx = 0, sy = 0;
        side_step(side, sx, sy);
        ViewSpec turned = plan[0].spec;
        turn_to_side(turned, sx, sy, third);
        if (dot3(unit(turned.dir), want) > 1 - 1e-9) p.side = side;
      }
    }
    if (p.side.empty()) {  // no side shows it: a pictorial view of its own
      p.pictorial = true;
      p.spec = standard(p.view);
    } else {
      side_step(p.side, p.cx, p.cy);
      if (!used.insert({p.cx, p.cy}).second) continue;
      turn_to_side(p.spec, p.cx, p.cy, third);
    }
    plan.push_back(std::move(p));
  }
  // Pictorial views go into the corner between the projected ones (first angle: below right; third: above right), else
  // beside the base view.
  int hx = 0, vy = 0;
  for (const auto& p : plan)
    if (!p.pictorial) {
      if (p.cy == 0 && p.cx != 0 && !hx) hx = p.cx;
      if (p.cx == 0 && p.cy != 0 && !vy) vy = p.cy;
    }
  for (auto& p : plan) {
    if (!p.pictorial) continue;
    std::vector<std::pair<int, int>> cells;
    if (hx || vy) cells.push_back({hx ? hx : 1, vy ? vy : (third ? 1 : -1)});
    for (const auto& c : std::initializer_list<std::pair<int, int>>{{1, 0}, {1, third ? 1 : -1}, {1, third ? -1 : 1}, {-1, 0}, {0, third ? 1 : -1}, {-1, -1}, {-1, 1}, {2, 0}, {3, 0}})
      cells.push_back(c);
    for (const auto& c : cells)
      if (used.insert(c).second) {
        p.cx = c.first, p.cy = c.second;
        break;
      }
  }
  std::map<int, double> cols, rows;
  for (auto& p : plan) {
    const std::array<double, 4> e = view_extent(doc, scene, p.spec);
    p.w = e[2] - e[0], p.h = e[3] - e[1];
    cols[p.cx] = std::max(cols[p.cx], p.w);
    rows[p.cy] = std::max(rows[p.cy], p.h);
  }
  double sumW = 0, sumH = 0;
  for (const auto& [c, w] : cols) sumW += w;
  for (const auto& [r, h] : rows) sumH += h;
  const std::array<double, 4> room = drawing_room(sheet);
  const double pad = 5, roomW = room[2] - room[0] - 2 * pad, roomH = room[3] - room[1] - 2 * pad;
  const double gapsW = gap * static_cast<double>(cols.size() - 1), gapsH = gap * static_cast<double>(rows.size() - 1);
  double s = 1;
  if (scale.empty() || scale == "auto") {
    s = roomW - gapsW > 0 && roomH - gapsH > 0 ? fit_scale(sumW, sumH, roomW - gapsW, roomH - gapsH) : standard_scales().back();
  } else {
    s = parse_scale(scale);
  }
  std::map<int, double> cx, cy;
  double at = room[0] + pad + (roomW - (sumW * s + gapsW)) / 2;
  for (const auto& [c, w] : cols) {
    cx[c] = at + w * s / 2;
    at += w * s + gap;
  }
  at = room[3] - pad - (roomH - (sumH * s + gapsH)) / 2;
  for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
    cy[it->first] = at - it->second * s / 2;
    at -= it->second * s + gap;
  }
  const auto r2 = [](double v) { return std::round(v * 100) / 100; };
  const auto base_record = [&](const Planned& p, bool pictorial) {
    json r = {{"op", "sheet_view"}, {"kind", "base"}, {"orient", {{"preset", p.view}}}, {"at", {r2(cx[p.cx]), r2(cy[p.cy])}}};
    if (source.is_object() && !source.empty()) r["source"] = source;
    json st = style.is_object() ? style : json::object();
    if (pictorial) st["hidden"] = false;  // pictorial views show no hidden lines
    if (!st.empty()) r["style"] = st;
    return r;
  };
  json out = json::array();
  for (const auto& p : plan) {
    json record;
    if (&p == &plan[0] || p.pictorial) {
      record = base_record(p, p.pictorial);
    } else {
      const Planned& b = plan[0];
      const double between = p.cy == 0 ? std::fabs(cx[p.cx] - cx[0]) - (b.w + p.w) * s / 2 : std::fabs(cy[p.cy] - cy[0]) - (b.h + p.h) * s / 2;
      record = {{"op", "sheet_view"}, {"kind", "projected"}, {"parent", "base"}, {"side", p.side}, {"gap", r2(std::max(1.0, between))}};
    }
    out.push_back({{"view", p.view}, {"record", record}});
  }
  return {{"scale", scale_text(s)}, {"views", out}};
}

// ---------------------------------------------------------------- dimensions
std::string format_value(double value, const json& item) {
  const int precision = std::clamp(item.value("precision", 2), 0, 8);
  const std::string type = item.value("type", "");
  std::string shown = number(value, precision);
  if (item.contains("text") && item["text"].is_string()) {  // "<>" stands for the value, as drafting tools write it
    std::string text = item["text"].get<std::string>();
    if (const size_t at = text.find("<>"); at != std::string::npos) text.replace(at, 2, shown);
    return text;
  }
  shown = (type == "diameter" ? "⌀" : type == "radius" ? "R" : "") + shown + (type == "angle" ? "°" : "");
  if (item.contains("tol") && item["tol"].is_object()) {
    const json& t = item["tol"];
    const double plus = t.value("plus", 0.0), minus = t.value("minus", -plus);
    const int decimals = std::max(precision, 3);
    const auto sign = [&](double x) { return (x < 0 ? "-" : "+") + number(std::fabs(x), decimals); };
    shown += t.value("type", "sym") == "sym" ? " ±" + number(std::fabs(plus), decimals) : " " + sign(plus) + "/" + sign(minus);
  }
  return item.value("prefix", "") + shown + item.value("suffix", "");
}

json evaluate_item(const Document& doc, const Scene& scene, const Sheet& sheet, const SheetItem& item, const ViewFrame& frame) {
  if (item.kind != "dimension") throw Error("only dimensions have a value");
  if (!frame.error.empty()) throw Error("its view cannot be drawn: " + frame.error);
  std::vector<design::ParamDef> defs;
  for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  const design::ParamTable params(defs);
  const std::map<std::string, TopoDS_Shape> fresh;
  json notes = json::object();
  const design::Ctx ctx{doc, params, scene, fresh, {}, &notes};

  // What a reference gives a dimension: a point (its aspect), and the line, circle or cylinder it lies on.
  struct Pick {
    Vec3 p{0, 0, 0}, a{0, 0, 0}, b{0, 0, 0}, centre{0, 0, 0}, axis{0, 0, 1};
    bool edge = false, line = false, circle = false, cylinder = false;
    double r = 0;
  };
  const auto pick = [&](json r) {
    Pick k;
    if (r.is_string()) r = Ref::parse(r.get<std::string>()).to_json();
    std::string aspect = r.value("aspect", "");
    if (r.value("kind", "") == "center") r["kind"] = "edge", aspect = "center";
    if (r.value("kind", "") == "point") {
      k.p = Ref::from_json(r).point;
      return k;
    }
    const TopoDS_Shape s = ctx.resolve(r).sub;
    if (s.ShapeType() == TopAbs_VERTEX) {
      k.p = of(BRep_Tool::Pnt(TopoDS::Vertex(s)));
      return k;
    }
    if (s.ShapeType() == TopAbs_FACE) {
      const BRepAdaptor_Surface f(TopoDS::Face(s));
      if (f.GetType() != GeomAbs_Cylinder) throw Error("a dimension takes edges and vertices (a cylindrical face for a diameter)");
      const gp_Cylinder c = f.Cylinder();
      k.cylinder = true, k.r = c.Radius(), k.p = k.centre = of(c.Location()), k.axis = of(c.Axis().Direction());
      return k;
    }
    if (s.ShapeType() != TopAbs_EDGE) throw Error("a dimension takes edges and vertices");
    const TopoDS_Edge e = TopoDS::Edge(s);
    const BRepAdaptor_Curve c(e);
    const double t0 = c.FirstParameter(), t1 = c.LastParameter();
    k.edge = true;
    k.a = of(c.Value(t0)), k.b = of(c.Value(t1));
    if (e.Orientation() == TopAbs_REVERSED) std::swap(k.a, k.b);
    k.line = c.GetType() == GeomAbs_Line;
    const bool conic = c.GetType() == GeomAbs_Circle || c.GetType() == GeomAbs_Ellipse;
    if (c.GetType() == GeomAbs_Circle) {
      const gp_Circ ci = c.Circle();
      k.circle = true, k.r = ci.Radius(), k.centre = of(ci.Location()), k.axis = of(ci.Axis().Direction());
    } else if (c.GetType() == GeomAbs_Ellipse) {
      k.centre = of(c.Ellipse().Location());
    }
    if (aspect == "start") k.p = k.a;
    else if (aspect == "end") k.p = k.b;
    else if (aspect == "mid" || (aspect.empty() && !conic)) k.p = of(c.Value((t0 + t1) / 2));
    else if (aspect == "center" || aspect.empty()) {
      if (!conic) throw Error("only a circle or an ellipse has a centre");
      k.p = k.centre;
    } else throw Error("aspect is start, end, mid or center, not '" + aspect + "'");
    return k;
  };

  std::vector<Pick> picks;
  for (const auto& r : item.def.value("refs", json::array())) picks.push_back(pick(r));
  const std::string& type = item.type;
  const double units = sheet.def.value("units", "mm") == "in" ? 1 / 25.4 : 1;
  double value = 0;
  Vec3 anchor{0, 0, 0};
  // What it measures on paper, for drawing it (paper mm from the view's centre, as the anchor).
  json geometry = json::object();
  const auto paper_of = [&](const Vec2& v) { return json::array({frame.scale * (v[0] - frame.centre[0]), frame.scale * (v[1] - frame.centre[1])}); };
  if (type == "horizontal" || type == "vertical" || type == "aligned") {
    Vec2 a, b;
    if (picks.size() == 1 && picks[0].edge) {
      a = frame.view(picks[0].a), b = frame.view(picks[0].b);
      anchor = scaled(plus3(picks[0].a, picks[0].b), 0.5);
    } else if (picks.size() == 2) {
      a = frame.view(picks[0].p), b = frame.view(picks[1].p);
      anchor = scaled(plus3(picks[0].p, picks[1].p), 0.5);
    } else {
      throw Error("a " + type + " dimension takes two references or one edge");
    }
    value = type == "horizontal" ? std::fabs(b[0] - a[0]) : type == "vertical" ? std::fabs(b[1] - a[1]) : std::hypot(b[0] - a[0], b[1] - a[1]);
    geometry = {{"from", paper_of(a)}, {"to", paper_of(b)}};
    if (type == "aligned" && picks.size() == 2 && picks[0].line && picks[1].line) {  // two parallel lines: across them
      const Vec2 a0 = frame.view(picks[0].a), a1 = frame.view(picks[0].b), b0 = frame.view(picks[1].a), b1 = frame.view(picks[1].b);
      const double la = std::hypot(a1[0] - a0[0], a1[1] - a0[1]), lb = std::hypot(b1[0] - b0[0], b1[1] - b0[1]);
      if (la > 1e-9 && lb > 1e-9) {
        const Vec2 u{(a1[0] - a0[0]) / la, (a1[1] - a0[1]) / la}, w{(b1[0] - b0[0]) / lb, (b1[1] - b0[1]) / lb};
        if (std::fabs(u[0] * w[1] - u[1] * w[0]) < 1e-6) {
          value = std::fabs(u[0] * (b0[1] - a0[1]) - u[1] * (b0[0] - a0[0]));
          const Vec2 m{(a0[0] + a1[0]) / 2, (a0[1] + a1[1]) / 2};  // from the middle of the first line straight across
          const double t = (m[0] - b0[0]) * w[0] + (m[1] - b0[1]) * w[1];
          geometry = {{"from", paper_of(m)}, {"to", paper_of({b0[0] + w[0] * t, b0[1] + w[1] * t})}};
        }
      }
    }
  } else if (type == "radius" || type == "diameter") {
    if (picks.size() != 1 || !(picks[0].circle || picks[0].cylinder)) throw Error("a " + type + " takes one circle or cylinder");
    const double along = std::fabs(dot3(picks[0].axis, frame.dir));
    if (picks[0].circle && along < 0.9999) throw Error("the circle is foreshortened in this view: dimension it in a view along its axis");
    if (picks[0].cylinder && along < 0.9999 && along > 1e-4) throw Error("the cylinder is seen at a slant: dimension it in a view along or across its axis");
    value = type == "radius" ? picks[0].r : 2 * picks[0].r;
    anchor = picks[0].centre;
    const Vec2 c = frame.view(picks[0].centre);
    if (along >= 0.9999) {
      geometry = {{"centre", paper_of(c)}, {"r", picks[0].r * frame.scale}};
    } else {  // a cylinder seen from the side: across it, square to its axis
      Vec2 n = frame.view(picks[0].axis);
      const double l = std::hypot(n[0], n[1]);
      n = {-n[1] / l, n[0] / l};
      const double r = picks[0].r;
      geometry = {{"from", paper_of(type == "radius" ? c : Vec2{c[0] - n[0] * r, c[1] - n[1] * r})}, {"to", paper_of({c[0] + n[0] * r, c[1] + n[1] * r})}};
    }
  } else if (type == "angle") {
    if (picks.size() != 2 || !picks[0].line || !picks[1].line) throw Error("an angle takes two straight edges");
    Vec2 d[2];
    for (int i = 0; i < 2; ++i) {
      const Vec3 e = unit(minus3(picks[size_t(i)].b, picks[size_t(i)].a));
      if (std::fabs(dot3(e, frame.dir)) > 1e-4) throw Error("an edge is foreshortened in this view: dimension the angle in a view normal to both");
      const Vec2 u = frame.view(e);
      const double l = std::hypot(u[0], u[1]);
      d[i] = {u[0] / l, u[1] / l};
    }
    value = std::acos(std::clamp(std::fabs(d[0][0] * d[1][0] + d[0][1] * d[1][1]), 0.0, 1.0)) * 180 / M_PI;
    if (item.def.value("obtuse", false)) value = 180 - value;
    anchor = scaled(plus3(plus3(picks[0].a, picks[0].b), plus3(picks[1].a, picks[1].b)), 0.25);
    geometry = {{"lines", {{paper_of(frame.view(picks[0].a)), paper_of(frame.view(picks[0].b))}, {paper_of(frame.view(picks[1].a)), paper_of(frame.view(picks[1].b))}}}};
  } else {
    throw Error("needs a newer OPAD (dimension type '" + type + "')");
  }
  if (type != "angle") value *= units;
  const Vec2 at = frame.paper(anchor);
  json out = {{"value", value}, {"shown", format_value(value, item.def)}, {"anchor", {at[0] - frame.at[0], at[1] - frame.at[1]}}, {"geometry", geometry}};
  if (notes.contains("rehinted")) out["rehinted"] = notes["rehinted"];
  return out;
}

}  // namespace opad::drawing
