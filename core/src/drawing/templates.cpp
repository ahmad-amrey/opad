// Drawing templates (TODO 11 UI-78): frames and title blocks drawn from scratch from general drafting practice (an ISO 5457
// border with centring marks and grid zones and an ISO 7200 style title block; an ASME style border and title block with
// a general tolerance note in our own words), never copied from a vendor's template; the title block's values filled in
// from the sheet and the document; a company's own frame and title block read from a DXF or DWG file.
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "opad/drawing/sheet.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/materials.hpp"

namespace opad::drawing {
namespace {

struct Cell {
  const char* key;
  const char* label;
  double x, y, w, h;  // in the block, mm from its bottom-left corner
  double height;      // the value's text height
  const char* align;  // left | center
  const char* valign; // bottom | middle | top
};

// ISO 7200 style: 180 mm wide (the width of an A4 sheet's frame), the mandatory fields (legal owner, identification number,
// date of issue, sheet, title, approval person, creator, document type) and what a part drawing adds (scale, projection
// symbol, size, material, mass, revision, status).
const std::vector<Cell>& iso_cells() {
  static const std::vector<Cell> cells = {
      {"owner", "Legal owner", 0, 30, 90, 10, 3.5, "left", "bottom"},
      {"doctype", "Document type", 90, 30, 45, 10, 2.5, "left", "bottom"},
      {"status", "Document status", 135, 30, 45, 10, 2.5, "left", "bottom"},
      {"author", "Created by", 0, 20, 45, 10, 2.5, "left", "bottom"},
      {"approved", "Approved by", 45, 20, 45, 10, 2.5, "left", "bottom"},
      {"title", "Title", 90, 10, 90, 20, 5, "center", "middle"},
      {"scale", "Scale", 0, 10, 20, 10, 2.5, "left", "bottom"},
      {"projection", "Projection", 20, 10, 22, 10, 0, "center", "middle"},
      {"size", "Size", 42, 10, 16, 10, 2.5, "left", "bottom"},
      {"date", "Date of issue", 58, 10, 32, 10, 2.5, "left", "bottom"},
      {"material", "Material", 0, 0, 50, 10, 2.5, "left", "bottom"},
      {"mass", "Mass", 50, 0, 40, 10, 2.5, "left", "bottom"},
      {"number", "Identification number", 90, 0, 50, 10, 3.5, "left", "bottom"},
      {"revision", "Rev.", 140, 0, 14, 10, 2.5, "left", "bottom"},
      {"sheet", "Sheet", 154, 0, 26, 10, 2.5, "left", "bottom"}};
  return cells;
}

// ASME style: a general tolerance note, the people who drew, checked and approved it with their dates, then company,
// title, size, drawing number, revision, scale, projection, weight and sheet.
const std::vector<Cell>& ansi_cells() {
  static const std::vector<Cell> cells = {
      {"tolerance", "Unless otherwise specified", 0, 0, 50, 40, 1.8, "left", "top"},
      {"author", "Drawn", 50, 30, 32, 10, 2.5, "left", "bottom"},
      {"date", "Date", 82, 30, 18, 10, 2, "left", "bottom"},
      {"checked", "Checked", 50, 20, 32, 10, 2.5, "left", "bottom"},
      {"checked_date", "Date", 82, 20, 18, 10, 2, "left", "bottom"},
      {"approved", "Approved", 50, 10, 32, 10, 2.5, "left", "bottom"},
      {"approved_date", "Date", 82, 10, 18, 10, 2, "left", "bottom"},
      {"material", "Material", 50, 0, 50, 10, 2.5, "left", "bottom"},
      {"owner", "Company", 100, 30, 100, 10, 3.5, "left", "bottom"},
      {"title", "Title", 100, 15, 100, 15, 5, "center", "middle"},
      {"size", "Size", 100, 8, 14, 7, 2.5, "left", "bottom"},
      {"number", "Drawing no.", 114, 8, 66, 7, 3.5, "left", "bottom"},
      {"revision", "Rev", 180, 8, 20, 7, 2.5, "left", "bottom"},
      {"scale", "Scale", 100, 0, 25, 8, 2.5, "left", "bottom"},
      {"projection", "Projection", 125, 0, 25, 8, 0, "center", "middle"},
      {"mass", "Weight", 150, 0, 25, 8, 2.5, "left", "bottom"},
      {"sheet", "Sheet", 175, 0, 25, 8, 2.5, "left", "bottom"}};
  return cells;
}

double rounded(double v) { return std::round(v * 1e6) / 1e6; }

std::string utf8(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}

// The block's lines: every cell's outline, merged where cells share an edge; on the block's border 0.7 mm, inside 0.35.
json grid_lines(const std::vector<Cell>& cells, double bw, double bh, double k) {
  std::map<long, std::vector<std::pair<double, double>>> across, up;  // y -> x spans; x -> y spans (µm keys)
  const auto key = [](double v) { return std::lround(v * 1000); };
  for (const auto& c : cells) {
    across[key(c.y)].push_back({c.x, c.x + c.w});
    across[key(c.y + c.h)].push_back({c.x, c.x + c.w});
    up[key(c.x)].push_back({c.y, c.y + c.h});
    up[key(c.x + c.w)].push_back({c.y, c.y + c.h});
  }
  json out = json::array();
  const auto emit = [&](std::map<long, std::vector<std::pair<double, double>>>& spans, bool horizontal, double border) {
    for (auto& [at, list] : spans) {
      std::sort(list.begin(), list.end());
      std::vector<std::pair<double, double>> merged;
      for (const auto& s : list)
        if (!merged.empty() && s.first <= merged.back().second + 1e-6) merged.back().second = std::max(merged.back().second, s.second);
        else merged.push_back(s);
      const double v = at / 1000.0;
      const double width = std::fabs(v) < 1e-6 || std::fabs(v - border) < 1e-6 ? 0.7 : 0.35;
      for (const auto& [a, b] : merged)
        out.push_back(horizontal ? json{rounded(a * k), rounded(v * k), rounded(b * k), rounded(v * k), width}
                                 : json{rounded(v * k), rounded(a * k), rounded(v * k), rounded(b * k), width});
    }
  };
  emit(across, true, bh);
  emit(up, false, bw);
  return out;
}

int zone_count(double length) { return std::max(2, 2 * static_cast<int>(std::lround(length / 100))); }  // fields of about 50 mm, an even number

std::string number(double v, int decimals) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", std::clamp(decimals, 0, 12), v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s;
}

// The part a sheet draws, for the title block: the one node of its first base view's source, or the document's one root.
// whole: the sheet draws the whole document (no part chosen), so the document's own properties name it.
std::string part_node(const Scene& scene, const Sheet& sheet, bool& whole) {
  whole = true;
  for (const auto& id : sheet.views) {
    const SheetView* v = scene.sheet_view(id);
    if (!v || v->kind != "base") continue;
    const json nodes = v->def.value("source", json::object()).value("nodes", json::array());
    whole = nodes.empty();
    if (nodes.size() == 1 && nodes[0].is_string() && scene.node(nodes[0].get<std::string>())) return nodes[0].get<std::string>();
    if (nodes.empty() && scene.roots.size() == 1) return scene.roots[0];
    return "";
  }
  return "";
}

std::string text_of(const json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number()) return number(v.get<double>(), 4);
  return "";
}

size_t characters(const std::string& s) {
  size_t n = 0, longest = 0;
  for (char ch : s) {
    if (ch == '\n') longest = std::max(longest, n), n = 0;
    else n += (static_cast<unsigned char>(ch) & 0xC0) != 0x80;
  }
  return std::max(longest, n);
}

// ISO 5456-2: a truncated cone's front view (narrow end left) and its end view, the circles right of it in first angle and
// left of it in third; centred on c, its large diameter d.
void projection_symbol(Display& d, int layer, Vec2 c, double D, bool third) {
  const double len = D, small = D / 2, gap = 0.5 * D, total = len + gap + D;
  const double x0 = c[0] - total / 2;
  const double cone = third ? x0 + D + gap : x0, circles = third ? x0 + D / 2 : x0 + len + gap + D / 2;
  d.polyline(layer, {{cone, c[1] - small / 2}, {cone + len, c[1] - D / 2}, {cone + len, c[1] + D / 2}, {cone, c[1] + small / 2}}, true);
  d.circle(layer, {circles, c[1]}, D / 2);
  d.circle(layer, {circles, c[1]}, small / 2);
}

}  // namespace

// ---------------------------------------------------------------- templates
json make_template(const std::string& standard, double w, double h) {
  const bool ansi = standard == "ansi" || standard == "asme";
  if (!ansi && standard != "iso") throw Error("template is iso, ansi or none, not '" + standard + "'");
  if (!(w > 60 && h > 60)) throw Error("the paper is too small for a frame and a title block");
  const double m = ansi ? (std::max(w, h) > 440 ? 12.7 : 10) : 10, left = ansi ? m : 20;
  json t = {{"id", ansi ? "ansi" : "iso"},
            {"name", ansi ? "ASME frame and title block" : "ISO 5457 frame, ISO 7200 title block"},
            {"standard", ansi ? "asme" : "iso"},
            {"frame", {{"left", left}, {"right", m}, {"top", m}, {"bottom", m}, {"width", 0.7}}},
            {"marks", true},
            {"zones", {{"x", zone_count(w)}, {"y", zone_count(h)}, {"from", ansi ? "bottom-right" : "top-left"}}}};
  const std::vector<Cell>& cells = ansi ? ansi_cells() : iso_cells();
  const double bw = ansi ? 200 : 180, bh = 40;
  const double k = std::min(1.0, (w - left - m) / bw);  // a narrow sheet (ANSI A upright) takes the block smaller
  json fields = json::array();
  for (const auto& c : cells)
    fields.push_back({{"key", c.key}, {"label", c.label}, {"rect", {rounded(c.x * k), rounded(c.y * k), rounded(c.w * k), rounded(c.h * k)}},
                      {"height", rounded(c.height * k)}, {"align", c.align}, {"valign", c.valign}});
  t["title_block"] = {{"w", rounded(bw * k)}, {"h", rounded(bh * k)}, {"label_height", rounded(1.8 * k)}, {"lines", grid_lines(cells, bw, bh, k)}, {"fields", fields}};
  return t;
}

std::array<double, 4> drawing_room(const json& sheet) {
  const json size = sheet.value("size", json::object());
  const double w = size.value("w", 0.0), h = size.value("h", 0.0);
  const json t = sheet.value("template", json());
  if (!t.is_object() || !t.contains("frame")) return {20, 10, w - 10, h - 10};
  const json f = t["frame"];
  std::array<double, 4> r{f.value("left", 10.0), f.value("bottom", 10.0), w - f.value("right", 10.0), h - f.value("top", 10.0)};
  if (t.contains("title_block") && t["title_block"].is_object()) r[1] += t["title_block"].value("h", 0.0) + 5;  // above the title block
  return r;
}

json title_values(const Document& doc, const Scene& scene, const Sheet& sheet, bool measure) {
  const json values = sheet.def.value("values", json::object());
  const json t = sheet.def.value("template", json::object());
  bool whole = true;
  const std::string node = part_node(scene, sheet, whole);
  const Node* part = node.empty() ? nullptr : scene.node(node);
  const json props = part ? part->properties : json::object();
  const bool inches = sheet.def.value("units", "mm") == "in";
  // The document's own properties: what every drawing of it says (owner, project, approvals); for a drawing of the whole
  // document also its title, number and description, before the one root part's.
  const auto own = [&](const std::string& k) { return text_of(scene.properties.value(k == "owner" && !scene.properties.contains("owner") ? "company" : k, json())); };
  const auto lookup = [&](const std::string& what) -> std::string {
    if (what == "title") {
      if (whole && !own("title").empty()) return own("title");
      if (part) return part->name;
      if (!doc.path.empty()) return utf8(doc.path.stem());
      return sheet.drawing;
    }
    if (what == "number" || what == "description") {
      const std::string mine = text_of(props.value(what == "number" ? "part_number" : what, json()));
      return !mine.empty() || !whole ? mine : own(what);
    }
    if (what == "author") return !own("author").empty() ? own("author") : sheet.def.value("by", "");
    if (what == "date") return sheet.def.value("ts", "").substr(0, 10);
    if (what == "scale") return scale_text(sheet.scale);
    if (what == "units") return inches ? "in" : "mm";
    if (what == "drawing") return sheet.drawing;
    if (what == "name") return sheet.name;
    if (what == "file") return utf8(doc.path.filename());
    if (what == "size") {
      const json size = sheet.def.value("size", json::object());
      if (!size.contains("preset")) return number(sheet.width, 1) + " × " + number(sheet.height, 1);
      const std::string preset = size["preset"].get<std::string>();
      return preset.rfind("ANSI-", 0) == 0 ? preset.substr(5) : preset;  // ASME names the size by its letter
    }
    if (what == "sheet") {
      int n = 0, at = 0;
      for (const auto& s : scene.sheets)
        if (s.id == sheet.id || (!sheet.drawing.empty() && s.drawing == sheet.drawing)) {
          ++n;
          if (s.id == sheet.id) at = n;
        }
      return std::to_string(std::max(at, 1)) + " / " + std::to_string(std::max(n, 1));
    }
    if (what == "doctype") return !part ? "Drawing" : part->kind == Node::Kind::Body ? "Part drawing" : "Assembly drawing";
    if (what == "material") return part ? material_of(doc, scene, node).shown() : "";
    if (what == "mass") {
      if (!part || !measure) return "";
      try {
        const json p = node_properties(doc, scene, node, true);
        if (!p.contains("mass") || !p["mass"].is_number()) return "";
        const double g = p["mass"].get<double>();
        if (inches) return number(g / 453.59237, 3) + " lb";
        return g < 1000 ? number(g, 1) + " g" : number(g / 1000, 3) + " kg";
      } catch (const std::exception&) {
        return "";
      }
    }
    if (what == "tolerance")  // a general note, in our own words
      return inches ? "DIMENSIONS IN INCHES\nTOLERANCES:\n.XX ±.01   .XXX ±.005\nANGLES ±0.5°\nBREAK SHARP EDGES\nDO NOT SCALE DRAWING"
                    : "DIMENSIONS IN MM\nTOLERANCES:\nX.X ±0.2   X.XX ±0.1\nANGLES ±0.5°\nBREAK SHARP EDGES\nDO NOT SCALE DRAWING";
    if (what.rfind("prop:", 0) == 0) {  // the part's, else the document's
      const std::string mine = text_of(props.value(what.substr(5), json()));
      return !mine.empty() ? mine : own(what.substr(5));
    }
    if (what.rfind("doc:", 0) == 0) return own(what.substr(4));
    return own(what);  // owner, project, checked, approved, status, revision, ...: the document's
  };
  std::set<std::string> keys;
  if (t.contains("title_block") && t["title_block"].is_object())
    for (const auto& f : t["title_block"].value("fields", json::array())) keys.insert(f.value("key", ""));
  for (const auto& [k, v] : values.items()) keys.insert(k);
  keys.erase("");
  json out = json::object();
  for (const auto& k : keys) {
    const json v = values.value(k, json());
    if (v.is_string()) {
      const std::string s = v.get<std::string>();
      out[k] = s.size() > 1 && s[0] == '=' ? lookup(s.substr(1)) : s;
    } else {
      out[k] = lookup(k);
    }
  }
  return out;
}

void draw_paper(Display& d, const Document& doc, const Scene& scene, const Sheet& sheet) {
  const double w = sheet.width, h = sheet.height;
  const int frame = d.layer({"Frame", kInk, LineType::Continuous, 0.7});
  const json t = sheet.def.value("template", json());
  if (!t.is_object()) {  // sheets made before templates: the ISO 5457 frame and its centring marks alone
    if (w > 60 && h > 40) {
      d.polyline(frame, {{20, 10}, {w - 10, 10}, {w - 10, h - 10}, {20, h - 10}}, true);
      const double cx = (20 + w - 10) / 2, cy = h / 2;
      d.line(frame, {cx, 5}, {cx, 15});
      d.line(frame, {cx, h - 5}, {cx, h - 15});
      d.line(frame, {15, cy}, {25, cy});
      d.line(frame, {w - 5, cy}, {w - 15, cy});
    }
    return;
  }
  const int thin = d.layer({"Title block", kInk, LineType::Continuous, 0.35});
  if (t.contains("geometry") && t["geometry"].is_string() && doc.has_body(t["geometry"].get<std::string>())) {
    const int own = d.layer({"Template", kInk, LineType::Continuous, 0.35});
    TopoDS_Shape shape;
    try {
      shape = body_shape(doc, t["geometry"].get<std::string>());
    } catch (const std::exception&) {  // unreadable: the frame and the block are still drawn
    }
    const json at = t.value("at", json::array({0, 0}));
    if (!shape.IsNull() && at.is_array() && at.size() == 2 && at[0].is_number() && at[1].is_number() && (at[0] != 0 || at[1] != 0)) {
      gp_Trsf move;
      move.SetTranslation(gp_Vec(at[0].get<double>(), at[1].get<double>(), 0));
      shape = shape.Moved(TopLoc_Location(move));
    }
    add_shape(d, own, shape, 0.01);
  }
  if (t.contains("frame") && t["frame"].is_object()) {
    const json f = t["frame"];
    const double l = f.value("left", 20.0), r = w - f.value("right", 10.0), b = f.value("bottom", 10.0), tp = h - f.value("top", 10.0);
    d.polyline(frame, {{l, b}, {r, b}, {r, tp}, {l, tp}}, true);
    const double cx = (l + r) / 2, cy = (b + tp) / 2;
    if (t.value("marks", false)) {  // centring marks: from the middle of the margin to 5 mm inside the frame
      d.line(frame, {cx, b / 2}, {cx, b + 5});
      d.line(frame, {cx, (h + tp) / 2}, {cx, tp - 5});
      d.line(frame, {l / 2, cy}, {l + 5, cy});
      d.line(frame, {(w + r) / 2, cy}, {r - 5, cy});
    }
    if (t.contains("zones") && t["zones"].is_object()) {  // grid reference fields in the margins: numbers along, letters up
      const json z = t["zones"];
      const int nx = std::clamp(z.value("x", 0), 0, 60), ny = std::clamp(z.value("y", 0), 0, 26);
      const bool fromRight = z.value("from", "top-left") == "bottom-right";
      static const char* letters = "ABCDEFGHJKLMNPQRSTUVWXYZ";  // no I or O
      const double fx = nx ? (r - l) / nx : 0, fy = ny ? (tp - b) / ny : 0;
      for (int i = 0; i < nx; ++i) {
        const double x = l + fx * i;
        if (i > 0 && !(t.value("marks", false) && 2 * i == nx)) {
          d.line(thin, {x, b}, {x, b - std::min(5.0, b / 2)});
          d.line(thin, {x, tp}, {x, tp + std::min(5.0, (h - tp) / 2)});
        }
        const std::string label = std::to_string(fromRight ? nx - i : i + 1);
        d.text(thin, label, {x + fx / 2, b / 2}, std::min(3.5, b / 3), 0, 1, 2);
        d.text(thin, label, {x + fx / 2, (h + tp) / 2}, std::min(3.5, (h - tp) / 3), 0, 1, 2);
      }
      for (int j = 0; j < ny; ++j) {
        const double y = b + fy * j;
        if (j > 0 && !(t.value("marks", false) && 2 * j == ny)) {
          d.line(thin, {l, y}, {l - std::min(5.0, l / 2), y});
          d.line(thin, {r, y}, {r + std::min(5.0, (w - r) / 2), y});
        }
        const std::string label(1, letters[(fromRight ? j : ny - 1 - j) % 24]);
        d.text(thin, label, {l / 2, y + fy / 2}, std::min(3.5, (w - r) / 3), 0, 1, 2);
        d.text(thin, label, {(w + r) / 2, y + fy / 2}, std::min(3.5, (w - r) / 3), 0, 1, 2);
      }
    }
    if (t.contains("title_block") && t["title_block"].is_object()) {
      const json tb = t["title_block"];
      const Vec2 o{r - tb.value("w", 0.0), b};
      for (const auto& line : tb.value("lines", json::array())) {
        if (!line.is_array() || line.size() < 4) continue;
        const double width = line.size() > 4 ? line[4].get<double>() : 0.35;
        d.line(width >= 0.5 ? frame : thin, {o[0] + line[0].get<double>(), o[1] + line[1].get<double>()}, {o[0] + line[2].get<double>(), o[1] + line[3].get<double>()});
      }
      const json values = title_values(doc, scene, sheet);
      const double lh = tb.value("label_height", 1.8);
      for (const auto& f : tb.value("fields", json::array())) {
        const json rect = f.value("rect", json());
        if (!rect.is_array() || rect.size() != 4) continue;
        const double x = o[0] + rect[0].get<double>(), y = o[1] + rect[1].get<double>(), fw = rect[2].get<double>(), fh = rect[3].get<double>();
        const std::string key = f.value("key", "");
        d.text(thin, f.value("label", ""), {x + 0.8, y + fh - 0.7}, lh, 0, 0, 3);
        const double room = fh - lh - 1.4, under = fh - lh - 2.3;  // under the label; for a line from 1 mm up to 0.6 mm below it
        if (key == "projection") {
          projection_symbol(d, thin, {x + fw / 2, y + room / 2 + 0.6}, std::min(room - 1.2, fw / 2.8), sheet.projection == "third");
          continue;
        }
        const std::string text = values.value(key, "");
        if (text.empty()) continue;
        const size_t lines = static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
        const std::string align = f.value("align", "left"), valign = f.value("valign", "bottom");
        double th = f.value("height", 2.5);
        th = std::min({th, (fw - 2) / (0.62 * static_cast<double>(std::max<size_t>(characters(text), 1))), room / (1 + 1.6 * static_cast<double>(lines - 1)),
                       lines == 1 && valign == "bottom" ? under / 1.3 : th});
        th = std::max(th, 1.0);
        const double tx = align == "center" ? x + fw / 2 : x + 1.2;
        const double ty = valign == "middle" ? y + room / 2 + 0.4 : valign == "top" ? y + fh - lh - 1.6 : y + 1.0;
        d.text(thin, text, {tx, ty}, th, 0, align == "center" ? 1 : 0, valign == "middle" ? 2 : valign == "top" ? 3 : 1);
      }
    }
  }
}

json read_template_file(const std::filesystem::path& file, std::string& brep) {
  Document scratch = Document::create();
  import_file(scratch, file);
  const Scene s = resolve(scratch);
  BRep_Builder builder;
  TopoDS_Compound all;
  builder.MakeCompound(all);
  int parts = 0;
  for (const auto& id : s.all_bodies()) {
    const Node* n = s.node(id);
    if (!n || n->representation != "drawing2d") continue;
    builder.Add(all, node_world_shape(scratch, s, id));
    ++parts;
  }
  if (!parts) throw Error(utf8(file.filename()) + " has no 2D geometry for a template");
  Bnd_Box box;
  BRepBndLib::Add(all, box);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  brep = brep_from_shape(all);
  // The paper: the smallest standard sheet that holds it, upright or lying as it is drawn; else its own size.
  const double w = x1 - x0, h = y1 - y0;
  json size = {{"w", rounded(w)}, {"h", rounded(h)}};
  for (const auto& p : paper_sizes()) {
    const bool landscape = w > h;
    const double pw = landscape ? p.h : p.w, ph = landscape ? p.w : p.h;
    if (w <= pw + 1 && h <= ph + 1) {
      size = {{"preset", p.name}, {"w", pw}, {"h", ph}};
      break;
    }
  }
  const double pw = size["w"].get<double>(), ph = size["h"].get<double>();
  // Drawn where the sheet is (its corner at the origin): kept; elsewhere: moved onto the paper.
  const bool onPaper = x0 > -1 && y0 > -1 && x1 < pw + 1 && y1 < ph + 1;
  json t = {{"id", "file"}, {"name", utf8(file.stem())}, {"source", utf8(file.filename())}, {"size", size}};
  if (!onPaper) t["at"] = {rounded(-x0 + std::max(0.0, (pw - w) / 2)), rounded(-y0 + std::max(0.0, (ph - h) / 2))};
  return t;
}

std::string store_template_geometry(Document& doc, const json& tmpl, const std::string& brep) {
  TopoDS_Shape shape;
  try {
    shape = shape_from_brep(brep);
  } catch (const std::exception&) {
  }
  if (shape.IsNull()) throw Error("a template's geometry is BREP text of a 2D drawing");
  return doc.add_body(brep, {{"name", tmpl.value("name", "Template")}, {"representation", "drawing2d"}, {"source", tmpl.value("source", "")}, {"template", true}});
}

json template_from_file(Document& doc, const std::filesystem::path& file) {
  std::string brep;
  json t = read_template_file(file, brep);
  t["geometry"] = store_template_geometry(doc, t, brep);
  return t;
}

}  // namespace opad::drawing
