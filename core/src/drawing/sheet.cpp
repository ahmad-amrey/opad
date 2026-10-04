// Drawing sheets (TODO 11 UI-76): paper sizes and scales, the records' checks, a view's projection and its place on the
// sheet (annotations and their values: annotate.cpp).
#include "opad/drawing/sheet.hpp"

#include <Bnd_Box.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "opad/drawing/symbols.hpp"
#include "opad/geometry.hpp"
#include "opad/render.hpp"
#include "projection_internal.hpp"

namespace opad::drawing {
namespace {

double dot3(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Bodies measured in a turned view's axes (view_extent): body key + turn -> xmin, ymin, xmax, ymax (empty when it could not
// be measured so). Keys are content addresses: valid for every document.
std::mutex g_turned_mu;
std::unordered_map<std::string, std::array<double, 4>> g_turned;
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

// A view folded out from `s` towards d (a unit direction on the sheet, in its axes), as a projected view to that side
// but at any angle (UI-82): first angle shows the object from the opposite side, third angle from that side; the axis
// across d stays where it was on the sheet, so the two views line up along it.
void fold_to(ViewSpec& s, Vec2 d, bool third) {
  const double sign = third ? 1 : -1;
  Vec3 x, y, z;
  view_axes(s, x, y, z);
  const Vec3 along = plus3(scaled(x, d[0]), scaled(y, d[1])), across = plus3(scaled(x, -d[1]), scaled(y, d[0]));
  s.dir = scaled(along, sign);
  s.up = plus3(scaled(z, -sign * d[1]), scaled(across, d[0]));
}

std::vector<Vec2> points(const json& j) {
  std::vector<Vec2> out;
  if (j.is_array())
    for (const auto& p : j)
      if (point2(p)) out.push_back({p[0].get<double>(), p[1].get<double>()});
  return out;
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

std::vector<std::string> node_ids(const json& j) {
  std::vector<std::string> out;
  if (j.is_array())
    for (const auto& n : j)
      if (n.is_string()) out.push_back(n.get<std::string>());
  return out;
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

}  // namespace

std::vector<std::string> unsectioned(const Scene& scene, const json& sectioned) {
  std::vector<std::string> out;
  const auto ids = node_ids(sectioned);
  for (const auto& [id, n] : scene.nodes)
    if (n.kind == Node::Kind::Body && left_whole(scene, id, ids)) out.push_back(id);
  std::sort(out.begin(), out.end());
  return out;
}

namespace {
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
    s.cut.clear(), s.whole.clear(), s.aligned = false, s.breakouts.clear(), s.parts_whole = false, s.sectioned.clear();  // the model, also when projected from a section
  } else if (v.kind == "section" || v.kind == "auxiliary") {
    const SheetView* parent = scene.sheet_view(v.parent);
    if (!parent) throw Error("its parent view " + v.parent + " does not exist");
    s = spec_of(scene, *parent, depth + 1);
    Vec2 toward;
    if (!view_direction(v, toward))
      throw Error(v.kind == "section" ? "a section needs a cutting line: cut [[u, v], ...], two or more points apart" : "an auxiliary view needs its angle");
    Vec3 px, py, pz;
    view_axes(s, px, py, pz);
    const Sheet* sheet = scene.sheet(v.sheet);
    fold_to(s, toward, sheet && sheet->projection == "third");
    s.cut.clear(), s.whole.clear(), s.aligned = false, s.breakouts.clear(), s.parts_whole = false, s.sectioned.clear();
    if (v.kind == "section") {
      s.cut = points(d["cut"]);
      s.cut_x = px, s.cut_y = py;
      s.aligned = d.value("aligned", false) && s.cut.size() >= 3;
      for (const auto& n : d.value("whole", json::array()))
        if (n.is_string()) s.whole.push_back(n.get<std::string>());
      s.parts_whole = true;
      s.sectioned = node_ids(d.value("sectioned", json()));
      s.hidden = false;  // section views draw hidden lines only when asked
    }
  } else if (v.kind == "detail") {
    const SheetView* parent = scene.sheet_view(v.parent);
    if (!parent) throw Error("its parent view " + v.parent + " does not exist");
    s = spec_of(scene, *parent, depth + 1);  // the parent's projection: one cache entry for both
    if (!point2(d.value("center", json())) || !(d.value("radius", 0.0) > 0)) throw Error("a detail view needs its center [u, v] and radius");
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
  if (v.kind == "base" || v.kind == "projected" || v.kind == "auxiliary") {  // broken-out sections (UI-82)
    for (const auto& b : d.value("breakouts", json::array())) {
      if (!b.is_object()) continue;
      ViewSpec::Breakout cut;
      cut.outline = points(b.value("outline", json()));
      cut.depth = b.value("depth", 0.0);
      if (cut.outline.size() >= 3) s.breakouts.push_back(std::move(cut));
    }
    if (!s.breakouts.empty()) {
      for (const auto& n : d.value("whole", json::array()))
        if (n.is_string()) s.whole.push_back(n.get<std::string>());
      s.parts_whole = true;
      s.sectioned = node_ids(d.value("sectioned", json()));
    }
  }
  if (d.contains("source")) apply_source(scene, s, d["source"]);
  apply_style(s, d.value("style", json()));
  return s;
}

// An item's name in the outline: what it showed when it was made (a dimension's value, a callout, a set's values), a
// note's text, a datum's letter, a frame's characteristic and tolerance, a surface's requirement.
std::string item_name(const SheetItem& t) {
  const json& d = t.def;
  const json shown = d.value("result", json::object()).value("shown", json());
  if (shown.is_string()) return shown.get<std::string>();
  if (shown.is_array()) {
    std::string out;
    for (const auto& s : shown)
      if (s.is_string()) out += (out.empty() ? "" : ", ") + s.get<std::string>();
    return out;
  }
  if (t.kind == "note") return d.value("text", "");
  if (t.kind == "datum") return d.value("letter", "");
  if (t.kind == "issue") return d.value("rev", "");
  const json v = d.value("value", json());
  const std::string value = v.is_string() ? v.get<std::string>() : v.is_number() ? number(v.get<double>(), 4) : std::string();
  if (t.kind == "fcf") return characteristic_glyph(d.value("characteristic", "")) + " " + (d.value("zone", "") == "diameter" ? "⌀" : "") + value;
  if (t.kind == "surface") return value;
  return "";
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
  for (const char* k : {"sheet", "view", "parent", "list"})
    if (op.contains(k) && !(op[k].is_string() && is_uuid(op[k].get<std::string>()))) fail(std::string("'") + k + "' must be an op id");
  for (const char* k : {"name", "drawing", "kind", "type", "standard", "projection", "units", "side", "scale", "text", "prefix", "suffix", "rev", "date",
                        "description", "approved", "pdf", "pdf_sha256", "tag", "grow", "letter"})
    if (op.contains(k) && !op[k].is_string()) fail(std::string("'") + k + "' must be text");
  for (const char* k : {"size", "template", "values", "source", "orient", "style", "label", "place", "result", "tol", "frozen", "bom", "headers",
                        "fingerprints", "frames"})
    if (op.contains(k) && !op[k].is_object()) fail(std::string("'") + k + "' must be an object");
  for (const char* k : {"gap", "height", "precision", "width", "diameter", "angle"})
    if (op.contains(k) && !finite(op[k])) fail(std::string("'") + k + "' must be a number");
  // Section, detail, auxiliary, cropped and broken views (UI-82).
  if (op.contains("cut") && !(op["cut"].is_array() && op["cut"].size() >= 2 && std::all_of(op["cut"].begin(), op["cut"].end(), point2)))
    fail("a cutting line is [[u, v], [u, v], ...]: two or more points");
  if (op.contains("center") && !point2(op["center"])) fail("'center' must be [u, v]");
  if (op.contains("radius") && !(finite(op["radius"]) && op["radius"].get<double>() > 0)) fail("'radius' must be a positive number");
  if (op.contains("flip") && !op["flip"].is_boolean()) fail("'flip' must be true or false");
  if (op.contains("aligned") && !op["aligned"].is_boolean()) fail("'aligned' must be true or false");
  for (const char* k : {"whole", "sectioned"})
    if (op.contains(k) && !(op[k].is_array() && std::all_of(op[k].begin(), op[k].end(), [](const json& n) { return n.is_string(); })))
      fail(std::string("'") + k + "' must be node ids");
  if (op.contains("breakouts") && !(op["breakouts"].is_array() && std::all_of(op["breakouts"].begin(), op["breakouts"].end(), [](const json& b) {
                                      const json o = b.is_object() ? b.value("outline", json()) : json();
                                      return o.is_array() && o.size() >= 3 && std::all_of(o.begin(), o.end(), point2) && finite(b.value("depth", json()));
                                    })))
    fail("'breakouts' is a list of {outline [[u, v], ...] (three or more points), depth}");
  if (op.contains("hatch")) {
    const auto lining = [](const json& o) {
      return o.is_object() && (!o.contains("pattern") || o["pattern"].is_string()) && (!o.contains("angle") || finite(o["angle"])) &&
             (!o.contains("spacing") || (finite(o["spacing"]) && o["spacing"].get<double>() > 0));
    };
    const json& h = op["hatch"];
    if (!lining(h) || (h.contains("thin") && !h["thin"].is_string()) ||
        (h.contains("bodies") && !(h["bodies"].is_object() && std::all_of(h["bodies"].begin(), h["bodies"].end(), lining))))
      fail("'hatch' is {pattern, angle, spacing (paper mm), thin, bodies: {node: {pattern, angle, spacing}}}");
  }
  if (op.contains("crop")) {
    const json& c = op["crop"];
    if (!(c.is_array() && c.size() == 4 && std::all_of(c.begin(), c.end(), finite) && c[0].get<double>() < c[2].get<double>() && c[1].get<double>() < c[3].get<double>()))
      fail("'crop' must be [x0, y0, x1, y1] with x0 < x1 and y0 < y1");
  }
  if (op.contains("breaks")) {
    const json& b = op["breaks"];
    if (!b.is_array()) fail("'breaks' must be a list of {axis x|y, from, to, gap}");
    for (const auto& e : b)
      if (!e.is_object() || (e.value("axis", "") != "x" && e.value("axis", "") != "y") || !finite(e.value("from", json())) || !finite(e.value("to", json())) ||
          e["from"].get<double>() >= e["to"].get<double>() || (e.contains("gap") && !(finite(e["gap"]) && e["gap"].get<double>() >= 0)))
        fail("a break is {axis x|y, from, to (from < to), gap (paper mm)}");
  }
  if (op.contains("sheets") && !(op["sheets"].is_array() && std::all_of(op["sheets"].begin(), op["sheets"].end(), [](const json& s) {
                                   return s.is_string() && is_uuid(s.get<std::string>());
                                 })))
    fail("'sheets' must be op ids");
  if (op.contains("columns") && !(op["columns"].is_array() && std::all_of(op["columns"].begin(), op["columns"].end(), [](const json& c) { return c.is_string(); })))
    fail("'columns' must be column names");
  if (op.contains("numbers") && !(op["numbers"].is_array() && std::all_of(op["numbers"].begin(), op["numbers"].end(), [](const json& e) {
                                    return e.is_object() && e.value("n", json()).is_number_integer() && e["n"].get<long long>() > 0;
                                  })))
    fail("item numbers are [{n, identity, node}] with n from 1");
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
    if (op.contains("template") && op["template"].contains("fields")) {
      const json& fields = op["template"]["fields"];
      if (!fields.is_array()) fail("a template's fields are a list");
      for (const auto& f : fields) {
        const json rect = f.is_object() ? f.value("rect", json()) : json();
        const bool cell = rect.is_array() && rect.size() == 4 && std::all_of(rect.begin(), rect.end(), [](const json& v) { return finite(v); });
        if (!f.is_object() || !f.value("key", json()).is_string() || !(cell || point2(f.value("at", json()))))
          fail("a template field is {key, rect [x, y, w, h] or at [x, y], ...}");
        for (const char* k : {"height", "w", "angle"})
          if (f.contains(k) && !finite(f[k])) fail(std::string("a template field's ") + k + " is a number");
        for (const char* k : {"label", "align", "valign"})
          if (f.contains(k) && !f[k].is_string()) fail(std::string("a template field's ") + k + " is text");
      }
    }
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
      std::string name = item_name(*t);
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
  const Vec2 l = local(view(p));
  return {at[0] + l[0], at[1] + l[1]};
}

Vec2 ViewFrame::fold(Vec2 v) const {
  Vec2 out = v;
  for (const auto& b : breaks) {
    const double r = v[static_cast<size_t>(b.axis)];
    double& o = out[static_cast<size_t>(b.axis)];
    if (r >= b.to) o -= b.to - b.from - b.gap;
    else if (r > b.from) o -= (r - b.from) * (1 - b.gap / (b.to - b.from));
  }
  return out;
}

Vec2 ViewFrame::unfold(Vec2 v) const {
  Vec2 out = v;
  for (int axis = 0; axis < 2; ++axis) {
    double shift = 0;
    const double x = v[static_cast<size_t>(axis)];
    for (const auto& b : breaks) {
      if (b.axis != axis) continue;
      const double at = b.from - shift;
      if (x <= at) break;
      if (x < at + b.gap) {
        shift = b.from + (x - at) * (b.to - b.from) / std::max(b.gap, 1e-12) - x;
        break;
      }
      shift += b.to - b.from - b.gap;
    }
    out[static_cast<size_t>(axis)] = x + shift;
  }
  return out;
}

Vec2 ViewFrame::local(Vec2 v) const {
  const Vec2 f = fold(v);
  return {scale * (f[0] - centre[0]), scale * (f[1] - centre[1])};
}

json ViewFrame::to_json() const {
  json j = {{"id", id}, {"scale", scale_text(scale)}, {"at", at}, {"centre", centre}, {"box", box}, {"x", x}, {"y", y}, {"dir", dir}};
  if (crop[2] > crop[0]) j["crop"] = crop;
  if (radius > 0) j["circle"] = {{"center", circle}, {"radius", radius}};
  for (const auto& b : breaks) j["breaks"].push_back({{"axis", b.axis ? "y" : "x"}, {"from", b.from}, {"to", b.to}, {"gap", b.gap}});
  if (!error.empty()) j["error"] = error;
  return j;
}

std::array<double, 4> view_extent(const Document& doc, const Scene& scene, const ViewSpec& spec) {
  // Bodies square to the view: the corners of their tight boxes (moved by their explode offsets), which is exact. Bodies
  // seen turned (pictorial views, turned parts) of a part the exact tier draws: measured in the view's axes, where the
  // turned corners of their own boxes would come out up to a quarter too big (cached by key and turn); of a bigger model
  // the corners still (a measure per turn of every body would cost as much as the projection). An aligned section: its
  // pieces revolved.
  if (spec.aligned && spec.cut.size() >= 3) return detail::aligned_extent(doc, scene, spec);
  Vec3 x, y, z;
  view_axes(spec, x, y, z);
  std::array<double, 4> e{1e300, 1e300, -1e300, -1e300};
  const auto bodies = view_bodies(scene, spec);
  std::vector<std::string> keys;  // measured side by side first (the Engine's boxes one by one: minutes)
  for (const auto& [node, world] : bodies)
    if (const Node* n = scene.node(node)) keys.push_back(n->body_key);
  warm_tight_bboxes(doc, keys);
  std::vector<std::string> turned(bodies.size());  // body -> its key and turn, when measured turned
  std::vector<std::array<double, 9>> turns(bodies.size());
  bool small = false;
  {
    ViewSpec probe = spec;
    probe.quality = Quality::Auto;
    try {
      small = choose_tier(doc, scene, probe) == Quality::Exact;
    } catch (const std::exception&) {
    }
  }
  std::vector<size_t> missing;
  for (size_t i = 0; small && i < bodies.size(); ++i) {
    const Node* n = scene.node(bodies[i].first);
    if (!n) continue;
    const Mat4& w = bodies[i].second;
    const Vec3 rows[3] = {x, y, z};
    bool square = true;
    std::string id = n->body_key;
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) {
        const double v = rows[r][0] * w.at(0, c) + rows[r][1] * w.at(1, c) + rows[r][2] * w.at(2, c);
        turns[i][static_cast<size_t>(3 * r + c)] = v;
        square = square && (std::fabs(v) < 1e-12 || std::fabs(std::fabs(v) - 1) < 1e-12);
        char text[32];
        std::snprintf(text, sizeof text, "|%.9f", std::fabs(v) < 5e-10 ? 0.0 : v);
        id += text;
      }
    if (square) continue;
    turned[i] = id;
    std::lock_guard<std::mutex> lock(g_turned_mu);
    if (!g_turned.count(id)) missing.push_back(i);
  }
  if (!missing.empty()) {
    std::vector<std::array<double, 4>> found(missing.size(), {1, 1, -1, -1});
    OSD_Parallel::For(0, static_cast<int>(missing.size()), [&](int k) {
      const size_t i = missing[static_cast<size_t>(k)];
      const auto& r = turns[i];
      try {
        gp_Trsf t;
        t.SetValues(r[0], r[1], r[2], 0, r[3], r[4], r[5], 0, r[6], r[7], r[8], 0);
        const Bnd_Box b = tight_bbox(body_shape(doc, scene.node(bodies[i].first)->body_key).Moved(TopLoc_Location(t)));
        if (b.IsVoid()) return;
        double c[6];
        b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
        found[static_cast<size_t>(k)] = {c[0], c[1], c[3], c[4]};
      } catch (const std::exception&) {
      } catch (const Standard_Failure&) {  // not a rigid turn (a scaled node): its corners
      }
    });
    std::lock_guard<std::mutex> lock(g_turned_mu);
    if (g_turned.size() > 20000) g_turned.clear();
    for (size_t k = 0; k < missing.size(); ++k) g_turned[turned[missing[k]]] = found[k];
  }
  for (size_t i = 0; i < bodies.size(); ++i) {
    const auto& [node, world] = bodies[i];
    if (!turned[i].empty()) {
      std::array<double, 4> b;
      {
        std::lock_guard<std::mutex> lock(g_turned_mu);
        b = g_turned[turned[i]];
      }
      if (b[0] <= b[2]) {  // its turned extent, moved where the node is
        const Vec3 shift{world.at(0, 3), world.at(1, 3), world.at(2, 3)};
        const double u = dot3(shift, x), v = dot3(shift, y);
        e = {std::min(e[0], b[0] + u), std::min(e[1], b[1] + v), std::max(e[2], b[2] + u), std::max(e[3], b[3] + v)};
        continue;
      }
    }
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

std::array<double, 2> view_depth(const Document& doc, const Scene& scene, const ViewSpec& spec) {
  Vec3 x, y, z;
  view_axes(spec, x, y, z);
  const auto bodies = view_bodies(scene, spec);
  std::vector<std::string> keys;
  for (const auto& [node, world] : bodies)
    if (const Node* n = scene.node(node)) keys.push_back(n->body_key);
  warm_tight_bboxes(doc, keys);
  std::array<double, 2> e{1e300, -1e300};
  for (const auto& [node, world] : bodies) {
    const Bnd_Box b = node_tight_bbox(doc, scene, node, false);
    if (b.IsVoid()) continue;
    const Mat4 placed = scene.world(node);
    const Vec3 shift{world.at(0, 3) - placed.at(0, 3), world.at(1, 3) - placed.at(1, 3), world.at(2, 3) - placed.at(2, 3)};
    double c[6];
    b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
    for (int k = 0; k < 8; ++k) {
      const double w = dot3(plus3({c[(k & 1) ? 3 : 0], c[(k & 2) ? 4 : 1], c[(k & 4) ? 5 : 2]}, shift), z);
      e = {std::min(e[0], w), std::max(e[1], w)};
    }
  }
  if (e[0] > e[1]) e = {0, 0};
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
      const json& d = v->def;
      std::array<double, 4> e;
      const ViewFrame* parent = nullptr;
      if (!v->parent.empty()) {
        parent = &place(v->parent, depth + 1);
        if (!parent->error.empty()) throw Error("its parent view cannot be drawn: " + parent->error);
      }
      if (v->kind == "detail") {  // its circle, at its own scale (twice its parent's unless given)
        f.circle = {d["center"][0].get<double>(), d["center"][1].get<double>()};
        f.radius = d["radius"].get<double>();
        e = {f.circle[0] - f.radius, f.circle[1] - f.radius, f.circle[0] + f.radius, f.circle[1] + f.radius};
        f.scale = d.value("scale", "") != "" && d["scale"] != "sheet" ? parse_scale(d["scale"].get<std::string>()) : 2 * parent->scale;
      } else {
        e = view_extent(doc, scene, spec);
        if (parent) f.scale = parent->scale;
        else if (const std::string s = d.value("scale", "sheet"); s != "sheet") f.scale = parse_scale(s);
      }
      if (const json c = d.value("crop", json()); c.is_array() && c.size() == 4) {
        f.crop = {c[0].get<double>(), c[1].get<double>(), c[2].get<double>(), c[3].get<double>()};
        for (int k = 0; k < 4; ++k)
          if (k < 2 ? f.crop[static_cast<size_t>(k)] > e[static_cast<size_t>(k)] : f.crop[static_cast<size_t>(k)] < e[static_cast<size_t>(k)]) f.crop_cuts |= 1 << k;
        e = {std::max(e[0], f.crop[0]), std::max(e[1], f.crop[1]), std::min(e[2], f.crop[2]), std::min(e[3], f.crop[3])};
        if (e[0] > e[2] || e[1] > e[3]) throw Error("its crop box holds nothing of it");
      }
      // Breaks: its own, and those of the view it lines up with along the axis they share.
      for (const auto& b : d.value("breaks", json::array()))
        f.breaks.push_back({b.value("axis", "x") == "y" ? 1 : 0, b["from"].get<double>(), b["to"].get<double>(), b.value("gap", 6.0) / f.scale});
      Vec2 toward{0, 0};
      if (parent && v->kind != "detail" && view_direction(*v, toward))
        for (const auto& b : parent->breaks)
          if (std::fabs(toward[b.axis ? 0 : 1]) > 1 - 1e-9) f.breaks.push_back(b);
      std::sort(f.breaks.begin(), f.breaks.end(), [](const ViewFrame::Break& a, const ViewFrame::Break& b) { return std::tie(a.axis, a.from) < std::tie(b.axis, b.from); });
      for (size_t k = 1; k < f.breaks.size(); ++k)
        if (f.breaks[k].axis == f.breaks[k - 1].axis && f.breaks[k].from < f.breaks[k - 1].to) throw Error("its breaks overlap");
      const Vec2 flo = f.fold({e[0], e[1]}), fhi = f.fold({e[2], e[3]});
      const double lo[2] = {flo[0], flo[1]}, hi[2] = {fhi[0], fhi[1]};
      f.centre = v->kind == "detail" ? f.circle : Vec2{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2};
      if ((v->kind == "section" || v->kind == "auxiliary") && (!d.value("align", true) && d.contains("at"))) {
        f.at = {d["at"][0].get<double>(), d["at"][1].get<double>()};
      } else if (v->kind == "section" || v->kind == "auxiliary") {
        // Lined up with its parent across `toward` (a point keeps its place along that axis), `gap` past its frame.
        const ViewFrame& p = *parent;
        const Vec2 a{-toward[1], toward[0]};
        const auto along = [](Vec2 u, Vec2 w) { return u[0] * w[0] + u[1] * w[1]; };
        double reach = 0, back = 0;
        for (int k = 0; k < 4; ++k) {
          reach = std::max(reach, along({(k & 1 ? p.box[2] : p.box[0]) - p.at[0], (k & 2 ? p.box[3] : p.box[1]) - p.at[1]}, toward));
          back = std::max(back, -along({((k & 1 ? hi[0] : lo[0]) - f.centre[0]) * f.scale, ((k & 2 ? hi[1] : lo[1]) - f.centre[1]) * f.scale}, toward));
        }
        const double s = along(p.at, a) + f.scale * along({f.centre[0] - p.centre[0], f.centre[1] - p.centre[1]}, a);
        const double t = along(p.at, toward) + reach + d.value("gap", 20.0) + back;
        f.at = {a[0] * s + toward[0] * t, a[1] * s + toward[1] * t};
      } else if (v->kind == "detail") {
        f.at = d.contains("at") ? Vec2{d["at"][0].get<double>(), d["at"][1].get<double>()}
                                : Vec2{parent->box[2] + 20 + f.radius * f.scale, (parent->box[1] + parent->box[3]) / 2};
      } else if (v->kind == "projected") {
        // Aligned with its parent: the coordinate they share is the parent's, the other puts `gap` between the frames.
        const ViewFrame& p = *parent;
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
      if (style.is_object() && style.value("centermarks", false)) record["style"] = {{"centermarks", true}};  // drawn by each view itself
    }
    out.push_back({{"view", p.view}, {"record", record}});
  }
  return {{"scale", scale_text(s)}, {"views", out}};
}

}  // namespace opad::drawing
