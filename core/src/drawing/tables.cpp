// Parts lists, balloons, revision tables and issued revisions of drawing sheets (TODO 11 UI-84): rows from the bill of
// materials with stable item numbers, auto-balloon, the drawing's issues and what an issue keeps.
#include "opad/drawing/tables.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <list>
#include <mutex>
#include <set>

#include "opad/design/feature.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/bom.hpp"
#include "opad/drawing/symbols.hpp"
#include "opad/geometry.hpp"
#include "projection_internal.hpp"

namespace opad::drawing {
namespace {

Vec2 vec2(const json& j, Vec2 fallback = {0, 0}) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) return fallback;
  return {j[0].get<double>(), j[1].get<double>()};
}
double r2(double v) { return std::round(v * 100) / 100; }
json js(Vec2 v) { return json::array({r2(v[0]), r2(v[1])}); }

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

const Sheet& need_sheet(const Scene& scene, const json& args) {
  const std::string id = args.value("sheet", "");
  const Sheet* s = scene.sheet(id);
  if (!s) throw Error("sheet " + id + " does not exist (sheet_info lists the sheets)");
  return *s;
}

std::vector<const Sheet*> drawing_sheets(const Scene& scene, const Sheet& sheet) {
  std::vector<const Sheet*> out;
  for (const auto& s : scene.sheets)
    if (s.id == sheet.id || (!sheet.drawing.empty() && s.drawing == sheet.drawing)) out.push_back(&s);
  return out;
}

// What a sheet's parts list lists by default: the first level of the one node its first base view draws, else the document.
std::string default_root(const Scene& scene, const Sheet& sheet) {
  for (const auto& id : sheet.views) {
    const SheetView* v = scene.sheet_view(id);
    if (!v || v->kind != "base") continue;
    const json nodes = v->def.value("source", json::object()).value("nodes", json::array());
    return nodes.size() == 1 && nodes[0].is_string() && scene.node(nodes[0].get<std::string>()) ? nodes[0].get<std::string>() : "";
  }
  return "";
}

const json& default_columns() {
  static const json c = {"item", "qty", "name", "part_number", "material"};
  return c;
}

std::string header_of(const std::string& column, const std::string& mass_unit) {
  if (column == "item") return "ITEM";
  if (column == "qty") return "QTY";
  if (column == "name") return "NAME";
  if (column == "part_number") return "PART NUMBER";
  if (column == "description") return "DESCRIPTION";
  if (column == "material") return "MATERIAL";
  if (column == "mass") return "MASS (" + mass_unit + ")";
  if (column == "vendor") return "VENDOR";
  std::string up = column;
  for (char& c : up) c = c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return up;
}

// The BoM of the document's state, kept for a few states: every balloon of a sheet looks its number up in it.
std::mutex g_bom_mu;
std::list<std::pair<std::string, json>> g_boms;  // newest first
json cached_bom(const Document& doc, const Scene& scene, const BomOptions& o) {
  std::string key = std::to_string(reinterpret_cast<uintptr_t>(&doc)) + "|" + std::to_string(doc.ops.size()) + "|" +
                    (doc.ops.empty() ? std::string() : doc.ops.back().id) + "|" + std::to_string(doc.body_count()) + "|" + o.mode + "|" + o.root + "|" +
                    (o.mass ? o.mass_unit : std::string("-"));
  {
    std::lock_guard<std::mutex> lock(g_bom_mu);
    for (auto it = g_boms.begin(); it != g_boms.end(); ++it)
      if (it->first == key) {
        g_boms.splice(g_boms.begin(), g_boms, it);
        return it->second;
      }
  }
  json b = bom(doc, scene, o);
  std::lock_guard<std::mutex> lock(g_bom_mu);
  g_boms.emplace_front(key, b);
  if (g_boms.size() > 6) g_boms.pop_back();
  return b;
}

// A table: a header and rows of text, columns as wide as their text (min 8 mm), `stretch` taking what `width` leaves;
// at: its right end, on the header's side away from the rows (up: rows above the header).
struct Table {
  std::vector<std::string> head;
  std::vector<std::vector<std::string>> rows;
  std::vector<bool> centred;
  int stretch = -1;
  double width = 0;
};

void draw_table(Display& d, int layer, Vec2 at, bool up, const Table& t, const DimStyle& s) {
  const double h = 2.5 * s.scale, pad = 1.5 * s.scale, row = 6 * s.scale;
  const size_t n = t.head.size();
  std::vector<double> w(n, 8 * s.scale);
  for (size_t i = 0; i < n; ++i) {
    w[i] = std::max(w[i], rich_width(t.head[i], h) + 2 * pad);
    for (const auto& r : t.rows)
      if (i < r.size()) w[i] = std::max(w[i], rich_width(r[i], h) + 2 * pad);
  }
  double total = 0;
  for (double x : w) total += x;
  if (t.stretch >= 0 && static_cast<size_t>(t.stretch) < n && t.width * s.scale > total) {
    w[static_cast<size_t>(t.stretch)] += t.width * s.scale - total;
    total = t.width * s.scale;
  }
  const double x0 = at[0] - total, tall = row * static_cast<double>(t.rows.size() + 1);
  const double y0 = up ? at[1] : at[1] - tall, y1 = y0 + tall;
  d.polyline(layer, {{x0, y0}, {at[0], y0}, {at[0], y1}, {x0, y1}}, true);
  for (size_t k = 1; k <= t.rows.size(); ++k) {
    const double y = up ? y0 + row * static_cast<double>(k) : y1 - row * static_cast<double>(k);
    d.line(layer, {x0, y}, {at[0], y});
  }
  double x = x0;
  for (size_t i = 0; i < n; ++i) {
    if (i) d.line(layer, {x, y0}, {x, y1});
    for (size_t k = 0; k <= t.rows.size(); ++k) {  // k = 0: the header
      const std::string& text = k == 0 ? t.head[i] : i < t.rows[k - 1].size() ? t.rows[k - 1][i] : std::string();
      const double y = up ? y0 + row * (static_cast<double>(k) + 0.5) : y1 - row * (static_cast<double>(k) + 0.5);
      if (k == 0 || t.centred[i]) rich_text(d, layer, text, {x + w[i] / 2, y}, h, 0, 1, 2);
      else rich_text(d, layer, text, {x + pad, y}, h, 0, 0, 2);
    }
    x += w[i];
  }
}

// The frame and title block of a sheet record's template: frame right, bottom, top and the block's width and height.
struct Margins {
  double right = 10, bottom = 10, top = 10, block_w = 0, block_h = 0;
};
Margins margins(const json& sheet) {
  Margins m;
  const json t = sheet.value("template", json());
  if (!t.is_object()) return m;
  if (t.contains("frame") && t["frame"].is_object()) {
    m.right = t["frame"].value("right", 10.0), m.bottom = t["frame"].value("bottom", 10.0), m.top = t["frame"].value("top", 10.0);
  }
  if (t.contains("title_block") && t["title_block"].is_object()) m.block_w = t["title_block"].value("w", 0.0), m.block_h = t["title_block"].value("h", 0.0);
  return m;
}

int numbers_max(const json& numbers) {
  int top = 0;
  for (const auto& e : numbers)
    if (e.is_object()) top = std::max(top, e.value("n", 0));
  return top;
}

// Today in the local calendar, YYYY-MM-DD (a date of issue is the issuer's day, not UTC's).
std::string today() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char buf[16];
  std::strftime(buf, sizeof buf, "%Y-%m-%d", &local);
  return buf;
}

// A revision after `r`: digits count up, letters skip I, O, Q, S, X and Z (they read as digits or each other).
std::string revision_after(const std::string& r) {
  if (r.empty()) return "A";
  if (std::all_of(r.begin(), r.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return std::to_string(std::stoll(r) + 1);
  static const std::string letters = "ABCDEFGHJKLMNPRTUVWY";
  std::string up = r;
  for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (!std::all_of(up.begin(), up.end(), [](char c) { return letters.find(c) != std::string::npos; })) return r + ".1";
  for (size_t i = up.size(); i-- > 0;) {  // like counting: the last letter steps, a Y rolls over to A
    const size_t at = letters.find(up[i]);
    if (at + 1 < letters.size()) {
      up[i] = letters[at + 1];
      return up;
    }
    up[i] = letters[0];
  }
  return letters.substr(0, 1) + up;
}

TopoDS_Edge edge_of(const Curve& c) {
  const auto P = [](Vec2 v) { return gp_Pnt(v[0], v[1], 0); };
  gp_Ax2 ax(gp_Pnt(c.c[0], c.c[1], 0), gp::DZ(), gp_Dir(std::cos(c.rot), std::sin(c.rot), 0));
  switch (c.type) {
    case Curve::Type::Line: return BRepBuilderAPI_MakeEdge(P(c.pts[0]), P(c.pts[1])).Edge();
    case Curve::Type::Arc: return BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(c.c[0], c.c[1], 0), gp::DZ()), c.r1), c.a0, c.a1).Edge();
    case Curve::Type::Ellipse: return BRepBuilderAPI_MakeEdge(gp_Elips(ax, std::max(c.r1, c.r2), std::min(c.r1, c.r2)), c.a0, c.a1).Edge();
    case Curve::Type::Spline: {
      std::vector<double> knots;
      std::vector<int> mults;
      for (double k : c.knots) {
        if (!knots.empty() && std::fabs(k - knots.back()) < 1e-12) ++mults.back();
        else knots.push_back(k), mults.push_back(1);
      }
      TColgp_Array1OfPnt poles(1, static_cast<int>(c.pts.size()));
      for (size_t i = 0; i < c.pts.size(); ++i) poles.SetValue(static_cast<int>(i) + 1, P(c.pts[i]));
      TColStd_Array1OfReal k(1, static_cast<int>(knots.size()));
      TColStd_Array1OfInteger m(1, static_cast<int>(mults.size()));
      for (size_t i = 0; i < knots.size(); ++i) k.SetValue(static_cast<int>(i) + 1, knots[i]), m.SetValue(static_cast<int>(i) + 1, mults[i]);
      Handle(Geom_BSplineCurve) curve;
      if (c.weights.size() == c.pts.size()) {
        TColStd_Array1OfReal wts(1, static_cast<int>(c.weights.size()));
        for (size_t i = 0; i < c.weights.size(); ++i) wts.SetValue(static_cast<int>(i) + 1, c.weights[i]);
        curve = new Geom_BSplineCurve(poles, wts, k, m, c.degree);
      } else {
        curve = new Geom_BSplineCurve(poles, k, m, c.degree);
      }
      return BRepBuilderAPI_MakeEdge(curve).Edge();
    }
    default: return {};
  }
}

}  // namespace

const json& parts_list_columns() { return default_columns(); }

// ---------------------------------------------------------------- parts lists
const SheetItem* parts_list_of(const Scene& scene, const Sheet& sheet, const std::string& list) {
  if (!list.empty())
    if (const SheetItem* t = scene.sheet_item(list); t && t->kind == "parts_list") return t;
  for (const Sheet* s : drawing_sheets(scene, sheet)) {  // the sheet's own first
    if (s->id != sheet.id) continue;
    for (const auto& id : s->items)
      if (const SheetItem* t = scene.sheet_item(id); t && t->kind == "parts_list") return t;
  }
  for (const Sheet* s : drawing_sheets(scene, sheet))
    for (const auto& id : s->items)
      if (const SheetItem* t = scene.sheet_item(id); t && t->kind == "parts_list") return t;
  return nullptr;
}

json parts_rows(const Document& doc, const Scene& scene, const Sheet& sheet, const json& def, bool renumber) {
  const json b = def.value("bom", json::object());
  BomOptions o;
  o.mode = b.is_object() ? b.value("mode", "top") : "top";
  if (o.mode != "top" && o.mode != "parts") throw Error("a parts list lists the top level or the parts, not '" + o.mode + "'");
  o.root = b.is_object() && b.contains("root") ? b["root"].get<std::string>() : default_root(scene, sheet);
  if (!o.root.empty() && !scene.node(o.root)) throw Error("what it lists (" + o.root + ") is gone");
  const json columns = def.contains("columns") && def["columns"].is_array() && !def["columns"].empty() ? def["columns"] : default_columns();
  o.mass = std::find(columns.begin(), columns.end(), json("mass")) != columns.end();
  o.mass_unit = sheet.def.value("units", "mm") == "in" ? "lb" : "g";
  const json list = cached_bom(doc, scene, o);
  json rows = list.value("rows", json::array());
  // Numbers: a row's settled one (by its identity, else by a node of it), else one after the highest ever given.
  struct Entry {
    int n;
    std::string identity, node;
    bool used = false;
  };
  std::vector<Entry> stored;
  const json was = def.value("numbers", json::array());
  for (const auto& e : was)
    if (e.is_object() && e.value("n", 0) > 0) stored.push_back({e["n"].get<int>(), e.value("identity", ""), e.value("node", "")});
  std::vector<int> n(rows.size(), 0);
  std::vector<bool> settled(rows.size(), false);
  if (!renumber) {
    for (size_t i = 0; i < rows.size(); ++i)
      for (auto& e : stored)
        if (!e.used && !e.identity.empty() && e.identity == rows[i].value("identity", "")) {
          n[i] = e.n, settled[i] = e.used = true;
          break;
        }
    for (size_t i = 0; i < rows.size(); ++i) {
      if (n[i]) continue;
      const json nodes = rows[i].value("nodes", json::array());
      for (auto& e : stored)
        if (!e.used && !e.node.empty() && std::find(nodes.begin(), nodes.end(), json(e.node)) != nodes.end()) {
          n[i] = e.n, settled[i] = e.used = true;
          break;
        }
    }
  }
  int next = renumber ? 0 : numbers_max(was);
  for (size_t i = 0; i < rows.size(); ++i)
    if (!n[i]) n[i] = ++next;
  json numbers = json::array();
  for (size_t i = 0; i < rows.size(); ++i) {
    rows[i]["number"] = n[i];
    rows[i]["settled"] = settled[i];
    const json nodes = rows[i].value("nodes", json::array());
    numbers.push_back({{"n", n[i]}, {"identity", rows[i].value("identity", "")}, {"node", nodes.empty() ? json("") : nodes[0]}});
  }
  if (!renumber)
    for (const auto& e : stored)  // parts that are gone keep their numbers: never given to another
      if (!e.used) numbers.push_back({{"n", e.n}, {"identity", e.identity}, {"node", e.node}});
  const auto by_n = [](const json& a, const json& b) { return a.value("n", 0) < b.value("n", 0); };
  std::stable_sort(numbers.begin(), numbers.end(), by_n);
  std::stable_sort(rows.begin(), rows.end(), [](const json& a, const json& b) { return a.value("number", 0) < b.value("number", 0); });
  json headers = json::object();
  for (const auto& c : columns) {
    const std::string id = c.get<std::string>();
    const json given = def.value("headers", json::object());
    headers[id] = given.is_object() && given.contains(id) && given[id].is_string() ? given[id].get<std::string>() : header_of(id, o.mass_unit);
  }
  return {{"rows", rows}, {"columns", columns}, {"headers", headers}, {"numbers", numbers}, {"changed", numbers != was}, {"mass_unit", o.mass_unit},
          {"mode", o.mode}};
}

const json* row_of(const Scene& scene, const json& rows, const std::string& node) {
  const auto path = scene.path_to(node);  // the node and the components it is in, nearest first
  for (auto p = path.rbegin(); p != path.rend(); ++p)
    for (const auto& r : rows) {
      const json& nodes = r["nodes"];
      if (std::find(nodes.begin(), nodes.end(), json(*p)) != nodes.end()) return &r;
    }
  return nullptr;
}

json plan_parts_list(const Document& doc, const Scene& scene, const json& args) {
  const Sheet& sheet = need_sheet(scene, args);
  json op = {{"op", "sheet_item"}, {"sheet", sheet.id}, {"kind", "parts_list"}};
  for (const char* k : {"bom", "columns", "headers"})
    if (args.contains(k)) op[k] = args[k];
  const Margins m = margins(sheet.def);
  const bool down = args.value("grow", "up") == "down";
  if (down) op["grow"] = "down";
  op["at"] = args.contains("at") ? args["at"] : js({sheet.width - m.right, down ? sheet.height - m.top : m.bottom + m.block_h});
  op["width"] = args.contains("width") ? args["width"] : json(m.block_w > 0 ? m.block_w : 120.0);
  const json r = parts_rows(doc, scene, sheet, op);
  if (r["rows"].empty()) throw Error("there is nothing to list: no parts in what the sheet draws");
  op["numbers"] = r["numbers"];
  return op;
}

json plan_revision_table(const Scene& scene, const json& args) {
  const Sheet& sheet = need_sheet(scene, args);
  const Margins m = margins(sheet.def);
  const bool up = args.value("grow", "down") == "up";
  json op = {{"op", "sheet_item"}, {"sheet", sheet.id}, {"kind", "revision_table"}};
  if (up) op["grow"] = "up";
  op["at"] = args.contains("at") ? args["at"] : js({sheet.width - m.right, up ? m.bottom + m.block_h : sheet.height - m.top});
  op["width"] = args.value("width", 120.0);
  return op;
}

// ---------------------------------------------------------------- balloons
void balloon(Display& d, int layer, Vec2 centre, double diameter, const std::string& text, Vec2 tip, bool dot, const std::string& qty, const DimStyle& s) {
  const double r = diameter / 2 * s.scale;
  d.circle(layer, centre, r);
  const double h = (text.size() > 2 ? 2.5 : 3.5) * s.scale;
  rich_text(d, layer, text, centre, h, 0, 1, 2);
  if (!qty.empty()) rich_text(d, layer, qty, {centre[0] + r + 1 * s.scale, centre[1]}, 2.5 * s.scale, 0, 0, 2);
  const double l = std::hypot(tip[0] - centre[0], tip[1] - centre[1]);
  if (l <= r + 0.5 * s.scale) return;  // the tip inside the circle: no leader
  const Vec2 u{(tip[0] - centre[0]) / l, (tip[1] - centre[1]) / l};
  d.line(layer, {centre[0] + u[0] * r, centre[1] + u[1] * r}, tip);
  if (dot) {  // on a surface
    std::vector<Vec2> ring;
    for (int k = 0; k < 16; ++k) ring.push_back({tip[0] + 0.5 * s.scale * std::cos(k * M_PI / 8), tip[1] + 0.5 * s.scale * std::sin(k * M_PI / 8)});
    d.fill(layer, {ring});
  } else {  // on an edge
    const Vec2 back{tip[0] - u[0] * s.arrow * s.scale, tip[1] - u[1] * s.arrow * s.scale}, across{-u[1] * s.arrow * s.scale / 6, u[0] * s.arrow * s.scale / 6};
    d.fill(layer, {{tip, {back[0] + across[0], back[1] + across[1]}, {back[0] - across[0], back[1] - across[1]}}});
  }
}

json plan_balloons(const Document& doc, const Scene& scene, const json& args) {
  const Sheet& sheet = need_sheet(scene, args);
  const std::string view = args.value("view", "");
  const SheetView* v = scene.sheet_view(view);
  if (!v || v->sheet != sheet.id) throw Error("view " + view + " is not on this sheet");
  if (!v->error.empty()) throw Error("the view cannot be drawn: " + v->error);
  const auto frames = layout(doc, scene, sheet);
  const auto fit = std::find_if(frames.begin(), frames.end(), [&](const ViewFrame& f) { return f.id == view; });
  if (fit == frames.end() || !fit->error.empty()) throw Error("the view cannot be drawn");
  const ViewFrame& f = *fit;
  const SheetItem* list = parts_list_of(scene, sheet, args.value("list", ""));
  json out = {{"ops", json::array()}, {"list", list ? list->id : ""}};
  json def;
  if (list) {
    def = list->def;
  } else {
    def = plan_parts_list(doc, scene, {{"sheet", sheet.id}});
    out["create"] = def;
  }
  const json rows = parts_rows(doc, scene, list ? *scene.sheet(list->sheet) : sheet, def);
  if (list && rows.value("changed", false)) out["numbers"] = rows["numbers"];
  const json& listed = rows["rows"];
  // Rows that have a balloon in this view already.
  std::set<int> done;
  if (!args.value("all", false))
    for (const auto& id : sheet.items) {
      const SheetItem* t = scene.sheet_item(id);
      if (!t || t->kind != "balloon" || t->view != view || t->refs.empty()) continue;
      if (const json* r = row_of(scene, listed, t->refs[0].body)) done.insert(r->value("number", 0));
    }
  // The longest edge of each row's parts that the view shows whole (a piece of an edge partly hidden counts a quarter).
  const ViewSpec spec = view_spec(scene, *v);
  const auto g = shape_linework(project(doc, scene, spec), f);  // what the view shows of them
  std::map<std::pair<int, int>, int> pieces;
  std::set<std::pair<int, int>> partly;
  for (const auto& c : g->curves) {
    if (c.edge < 0 || c.body < 0) continue;
    ++pieces[{c.body, c.edge}];
    if (c.hidden) partly.insert({c.body, c.edge});
  }
  struct Best {
    double score = -1;
    const Curve* curve = nullptr;
    std::string node;
  };
  std::map<int, Best> best;
  for (const auto& c : g->curves) {
    if (c.hidden || c.edge < 0 || c.body < 0 || static_cast<size_t>(c.body) >= g->bodies.size()) continue;
    const std::string& node = g->bodies[static_cast<size_t>(c.body)].node;
    const json* r = row_of(scene, listed, node);
    if (!r) continue;
    const int n = r->value("number", 0);
    if (done.count(n)) continue;
    const bool whole = !partly.count({c.body, c.edge}) && pieces[{c.body, c.edge}] == 1;
    const double score = c.length() * (whole ? 1 : 0.25) * (c.kind == Curve::Kind::Sharp ? 1 : 0.5);
    Best& b = best[n];
    if (score > b.score) b = {score, &c, node};
  }
  // Balloons around the frame on the side each tip is nearest, spread along it so that none overlap.
  const double diameter = args.value("diameter", 10.0), r = diameter / 2, gap = 8, pitch = diameter + 3;
  struct Placed {
    int n;
    std::string node;
    const Curve* curve;
    Vec2 tip, at;
    int side;
  };
  std::vector<Placed> placed;
  const double cx = (f.box[0] + f.box[2]) / 2, cy = (f.box[1] + f.box[3]) / 2;
  const double hw = std::max(1e-6, (f.box[2] - f.box[0]) / 2), hh = std::max(1e-6, (f.box[3] - f.box[1]) / 2);
  for (const auto& [n, b] : best) {
    const Curve& c = *b.curve;
    Vec2 mid;
    if (c.type == Curve::Type::Line) mid = {(c.pts[0][0] + c.pts[1][0]) / 2, (c.pts[0][1] + c.pts[1][1]) / 2};
    else if (c.type == Curve::Type::Arc) mid = {c.c[0] + c.r1 * std::cos((c.a0 + c.a1) / 2), c.c[1] + c.r1 * std::sin((c.a0 + c.a1) / 2)};
    else {
      const auto pts = c.sample(0.05);
      mid = pts[pts.size() / 2];
    }
    const Vec2 tip{f.at[0] + f.scale * (mid[0] - f.centre[0]), f.at[1] + f.scale * (mid[1] - f.centre[1])};
    const double dx = (tip[0] - cx) / hw, dy = (tip[1] - cy) / hh;
    const int side = std::fabs(dx) >= std::fabs(dy) ? (dx >= 0 ? 0 : 1) : (dy >= 0 ? 2 : 3);  // right, left, top, bottom
    placed.push_back({n, b.node, &c, tip, tip, side});
  }
  for (int side = 0; side < 4; ++side) {
    std::vector<Placed*> on;
    for (auto& p : placed)
      if (p.side == side) on.push_back(&p);
    const int along = side < 2 ? 1 : 0;  // the coordinate that varies along the side
    std::stable_sort(on.begin(), on.end(), [&](const Placed* a, const Placed* b) { return a->tip[along] < b->tip[along]; });
    std::vector<double> pos;
    double want = 0, have = 0;
    for (size_t k = 0; k < on.size(); ++k) {
      pos.push_back(k ? std::max(on[k]->tip[along], pos[k - 1] + pitch) : on[k]->tip[along]);
      want += on[k]->tip[along], have += pos[k];
    }
    const double shift = on.empty() ? 0 : (want - have) / static_cast<double>(on.size());
    for (size_t k = 0; k < on.size(); ++k) {
      Vec2 at;
      at[along] = pos[k] + shift;
      at[1 - along] = side == 0 ? f.box[2] + gap + r : side == 1 ? f.box[0] - gap - r : side == 2 ? f.box[3] + gap + r : f.box[1] - gap - r;
      on[k]->at = at;
    }
  }
  std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) { return a.n < b.n; });
  for (const auto& p : placed) {
    json op = {{"op", "sheet_item"}, {"sheet", sheet.id}, {"view", view}, {"kind", "balloon"},
               {"refs", {design::make_ref(doc, scene, Ref{p.node, Ref::Kind::Edge, p.curve->edge})}},
               {"place", {{"text", js({p.at[0] - f.at[0], p.at[1] - f.at[1]})}}}};
    if (list) op["list"] = list->id;
    if (args.value("qty", false)) op["qty"] = true;
    if (diameter != 10.0) op["diameter"] = diameter;
    op["result"] = {{"shown", std::to_string(p.n)}};
    out["ops"].push_back(op);
  }
  return out;
}

// ---------------------------------------------------------------- revisions
std::vector<const SheetItem*> drawing_issues(const Scene& scene, const Sheet& sheet) {
  std::set<std::string> sheets;
  for (const Sheet* s : drawing_sheets(scene, sheet)) sheets.insert(s->id);
  std::vector<const SheetItem*> out;
  for (const auto& t : scene.sheet_items)
    if (t.kind == "issue" && sheets.count(t.sheet)) out.push_back(&t);
  return out;
}

std::string next_revision(const Scene& scene, const Sheet& sheet) {
  const auto issues = drawing_issues(scene, sheet);
  return issues.empty() ? "A" : revision_after(issues.back()->def.value("rev", ""));
}

std::string linework_brep(const ViewGeometry& g) {
  BRep_Builder b;
  TopoDS_Compound all, parts[3];
  b.MakeCompound(all);
  for (auto& p : parts) b.MakeCompound(p);
  for (const auto& c : g.curves) {
    const int at = c.hidden ? 2 : c.kind == Curve::Kind::Tangent || c.kind == Curve::Kind::Seam || c.kind == Curve::Kind::Break ? 1 : 0;
    try {
      if (c.type == Curve::Type::Polyline) {
        for (size_t i = 1; i < c.pts.size(); ++i)
          if (std::hypot(c.pts[i][0] - c.pts[i - 1][0], c.pts[i][1] - c.pts[i - 1][1]) > 1e-9)
            b.Add(parts[at], BRepBuilderAPI_MakeEdge(gp_Pnt(c.pts[i - 1][0], c.pts[i - 1][1], 0), gp_Pnt(c.pts[i][0], c.pts[i][1], 0)).Edge());
        continue;
      }
      const TopoDS_Edge e = edge_of(c);
      if (!e.IsNull()) b.Add(parts[at], e);
    } catch (const Standard_Failure&) {  // a degenerate piece: left out
    }
  }
  for (auto& p : parts) b.Add(all, p);
  if (!g.sections.empty()) {  // a section's cut faces (UI-82): a compound of closed outlines per body, after the lines
    TopoDS_Compound faces;
    b.MakeCompound(faces);
    for (const auto& r : g.sections) {
      TopoDS_Compound region;
      b.MakeCompound(region);
      for (const auto& l : r.loops) {
        BRepBuilderAPI_MakePolygon poly;
        for (const auto& q : l) poly.Add(gp_Pnt(q[0], q[1], 0));
        poly.Close();
        if (poly.IsDone()) b.Add(region, poly.Wire());
      }
      b.Add(faces, region);
    }
    b.Add(all, faces);
  }
  return brep_from_shape(all);
}

size_t frozen_bytes(const Document& doc, const Scene& scene, const Sheet& sheet, const ProjectionProgress& progress) {
  std::vector<const SheetView*> views;
  for (const Sheet* s : drawing_sheets(scene, sheet))
    for (const auto& f : layout(doc, scene, *s))
      if (const SheetView* v = scene.sheet_view(f.id); v && f.error.empty()) views.push_back(v);
  size_t bytes = 0;
  for (size_t i = 0; i < views.size(); ++i) {
    if (progress && !progress(static_cast<double>(i) / static_cast<double>(views.size()), "linework")) throw Error("cancelled");
    const auto g = project(doc, scene, view_spec(scene, *views[i]), [&](double t, const std::string& phase) {
      return !progress || progress(t < 0 ? -1 : (static_cast<double>(i) + t) / static_cast<double>(views.size()), phase);
    });
    const std::string brep = linework_brep(*g);
    if (!doc.has_body(sha256_hex(brep))) bytes += brep.size();
  }
  return bytes;
}

json plan_issue(const Document& doc, const Scene& scene, const json& args, std::map<std::string, std::string>* frozen) {
  const Sheet& sheet = need_sheet(scene, args);
  const auto sheets = drawing_sheets(scene, sheet);
  std::string rev = args.value("rev", "");
  rev.erase(0, rev.find_first_not_of(" \t"));
  rev.erase(rev.find_last_not_of(" \t") + 1);
  if (rev.empty()) rev = next_revision(scene, sheet);
  for (const SheetItem* t : drawing_issues(scene, sheet))
    if (t->def.value("rev", "") == rev) throw Error("revision " + rev + " was issued already");
  json op = {{"op", "sheet_item"}, {"sheet", sheets.front()->id}, {"kind", "issue"}, {"rev", rev}};
  op["date"] = args.value("date", today());
  op["by"] = args.value("by", default_author());
  for (const char* k : {"description", "approved", "tag"})
    if (args.contains(k) && args[k].is_string() && !args[k].get<std::string>().empty()) op[k] = args[k];
  json ids = json::array(), values = json::object(), fingerprints = json::object(), placed = json::object(), edits = json::array();
  const bool freeze = args.value("freeze", true);
  for (const Sheet* s : sheets) {
    ids.push_back(s->id);
    const auto frames = layout(doc, scene, *s);
    for (const auto& f : frames) {
      const SheetView* v = scene.sheet_view(f.id);
      if (!v || !f.error.empty()) continue;
      const auto g = project(doc, scene, view_spec(scene, *v));
      fingerprints[f.id] = g->fingerprint;
      placed[f.id] = {{"at", js(f.at)}, {"centre", {f.centre[0], f.centre[1]}}, {"scale", f.scale}};
      if (freeze && frozen) (*frozen)[f.id] = linework_brep(*g);
    }
    for (const auto& id : s->items) {
      const SheetItem* t = scene.sheet_item(id);
      if (!t || !t->error.empty()) continue;
      if (t->kind == "parts_list") {
        const json r = parts_rows(doc, scene, *s, t->def);
        if (r.value("changed", false)) edits.push_back({{"op", "edit"}, {"target", id}, {"set", {{"numbers", r["numbers"]}}}});
        continue;
      }
      if (t->kind != "dimension" && t->kind != "hole_callout" && t->kind != "dimension_set" && t->kind != "balloon") continue;
      try {
        const ViewFrame* f = nullptr;
        for (const auto& x : frames)
          if (x.id == t->view) f = &x;
        const json result = item_result(t->def, measure_item(doc, scene, *s, *t, f));
        if (result.is_object() && result.contains("shown")) values[id] = result["shown"];
      } catch (const std::exception&) {  // dangling: what it showed when it was made
        if (t->def.contains("result") && t->def["result"].contains("shown")) values[id] = t->def["result"]["shown"];
      }
    }
  }
  op["sheets"] = ids;
  if (!values.empty()) op["values"] = values;
  if (!fingerprints.empty()) op["fingerprints"] = fingerprints, op["frames"] = placed;
  return {{"op", op}, {"edits", edits}};
}

Scene with_issue(const Scene& scene, const json& op) {
  Scene s = scene;
  SheetItem t;
  t.id = new_uuid();
  t.sheet = op.value("sheet", "");
  t.kind = "issue";
  t.def = op;
  for (auto& sh : s.sheets)
    if (sh.id == t.sheet) sh.items.push_back(t.id);
  s.sheet_items.push_back(std::move(t));
  return s;
}

json issue_changes(const Document& doc, const Scene& scene, const SheetItem& issue) {
  json views = json::array(), values = json::array(), gone = json::array();
  const json fingerprints = issue.def.value("fingerprints", json::object()), shown_then = issue.def.value("values", json::object());
  for (const auto& [id, fp] : fingerprints.items()) {
    const SheetView* v = scene.sheet_view(id);
    if (!v || !v->error.empty()) {
      gone.push_back(id);
      continue;
    }
    try {
      if (fp != projection_fingerprint(doc, scene, view_spec(scene, *v), Quality::Auto)) views.push_back(id);
    } catch (const std::exception&) {
      gone.push_back(id);
    }
  }
  for (const auto& [id, shown] : shown_then.items()) {
    const SheetItem* t = scene.sheet_item(id);
    const Sheet* s = t ? scene.sheet(t->sheet) : nullptr;
    if (!t || !s) {
      gone.push_back(id);
      continue;
    }
    try {
      const auto frames = t->view.empty() ? std::vector<ViewFrame>{} : layout(doc, scene, *s);
      const ViewFrame* f = nullptr;
      for (const auto& x : frames)
        if (x.id == t->view) f = &x;
      const json now = item_result(t->def, measure_item(doc, scene, *s, *t, f));
      if (!now.is_object() || now.value("shown", json()) != shown) values.push_back(id);
    } catch (const std::exception&) {
      gone.push_back(id);
    }
  }
  return {{"views", views}, {"values", values}, {"gone", gone}};
}

const SheetItem* find_issue(const Scene& scene, const Sheet& sheet, const std::string& rev) {
  for (const SheetItem* t : drawing_issues(scene, sheet))
    if (t->id == rev || t->def.value("rev", "") == rev) return t;
  return nullptr;
}

Scene issued_scene(const Document& doc, const SheetItem& issue) {
  std::vector<const Op*> upto;
  for (const auto& op : doc.ops) {
    upto.push_back(&op);
    if (op.id == issue.id) break;
  }
  if (upto.empty() || upto.back()->id != issue.id) throw Error("revision " + issue.def.value("rev", "") + " is not in the document's log");
  SceneBuilder b(doc);
  std::vector<std::string> deleted;
  const auto log = effective_ops(upto, &deleted);
  b.scene().deleted_ops = deleted;
  for (const auto& e : log) {
    try {
      b.apply(e.op->id, e.op->type, e.data());
    } catch (const std::exception& ex) {
      b.scene().unresolved.push_back({e.op->id, e.op->type, std::string("failed to apply: ") + ex.what()});
    }
  }
  b.finish();
  return b.take();
}

ViewGeometry frozen_geometry(const TopoDS_Shape& lines) {
  ViewGeometry g;
  const detail::View top{gp::DX(), gp::DY(), gp::DZ()};
  int k = 0;
  for (TopoDS_Iterator it(lines); it.More() && k < 3; it.Next(), ++k) {
    Curve like;
    like.kind = k == 1 ? Curve::Kind::Tangent : Curve::Kind::Sharp;
    like.hidden = k == 2;
    for (TopExp_Explorer e(it.Value(), TopAbs_EDGE); e.More(); e.Next()) {
      const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
      if (BRep_Tool::Degenerated(edge)) continue;
      const BRepAdaptor_Curve c(edge);
      detail::emit(c, c.FirstParameter(), c.LastParameter(), top, like, 1e-3, g.curves);
    }
  }
  for (auto& c : g.curves) c.z = 0;
  int part = 0;
  for (TopoDS_Iterator it(lines); it.More(); it.Next(), ++part) {
    if (part != 3) continue;
    for (TopoDS_Iterator r(it.Value()); r.More(); r.Next()) {
      ViewGeometry::Region region{static_cast<int>(g.sections.size()), {}};
      for (TopExp_Explorer w(r.Value(), TopAbs_WIRE); w.More(); w.Next()) {
        std::vector<Vec2> loop;
        for (BRepTools_WireExplorer e(TopoDS::Wire(w.Current())); e.More(); e.Next()) {
          const gp_Pnt q = BRep_Tool::Pnt(e.CurrentVertex());
          loop.push_back({q.X(), q.Y()});
        }
        if (loop.size() > 2) region.loops.push_back(std::move(loop));
      }
      g.sections.push_back(std::move(region));
    }
  }
  return g;
}

Display issued_display(const Document& doc, const Scene& then, const Sheet& sheet, const SheetItem& issue, const ProjectionProgress& progress, json* report) {
  // Dimensions write the values they were issued with (the same as measured in `then`, unless measuring changed since).
  Scene scene = then;
  const json values = issue.def.value("values", json::object());
  for (auto& t : scene.sheet_items)
    if (t.kind == "dimension" && values.contains(t.id) && values[t.id].is_string()) t.def["text"] = values[t.id];
  const Sheet* found = scene.sheet(sheet.id);
  if (!found) throw Error("sheet " + sheet.name + " was not part of revision " + issue.def.value("rev", ""));
  const Sheet& s = *found;
  Display d;
  d.title = s.name;
  d.paper = {0, 0, s.width, s.height};
  for (const auto& l : std::vector<Layer>{{"Frame", kInk, LineType::Continuous, 0.7}, {"Visible", kInk, LineType::Continuous, 0.5}, {"Tangent", kInk, LineType::Continuous, 0.25},
                                          {"Hidden", kInk, LineType::Hidden, 0.25}, {"Dimensions", kInk, LineType::Continuous, 0.25}, {"Text", kInk, LineType::Continuous, 0.25}})
    d.layer(l);
  draw_paper(d, doc, scene, s);
  // The views where they stood, drawn from what was frozen (a view that froze none: projected in the scene as issued).
  auto frames = layout(doc, scene, s);
  const json placed = issue.def.value("frames", json::object()), frozen = issue.def.value("frozen", json::object());
  for (auto& f : frames) {
    if (!placed.contains(f.id)) continue;
    const json& p = placed[f.id];
    f.at = vec2(p.value("at", json()), f.at);
    f.centre = vec2(p.value("centre", json()), f.centre);
    f.scale = p.value("scale", f.scale);
    f.error.clear();
  }
  json skipped = json::array();
  int views = 0, items = 0;
  for (size_t i = 0; i < frames.size(); ++i) {
    const ViewFrame& f = frames[i];
    const SheetView* v = scene.sheet_view(f.id);
    if (!v) continue;
    const size_t from = d.prims.size();
    const std::string key = frozen.value(f.id, "");
    try {
      if (!key.empty() && doc.has_body(key)) {
        const auto g = shape_linework(std::make_shared<const ViewGeometry>(frozen_geometry(body_shape(doc, key))), f);
        try {
          draw_view(d, f, *v, *g, &doc, &scene);  // centre lines on the cylinders of the model as issued
        } catch (const std::exception&) {      // that model is gone (gc): the centre marks of its circles only
          d.prims.resize(from);
          draw_view(d, f, *v, *g, nullptr, &scene);
        }
      } else {
        if (!f.error.empty() || !v->error.empty()) throw Error(f.error.empty() ? v->error : f.error);
        const auto g = shape_linework(project(doc, scene, view_spec(scene, *v), [&](double t, const std::string& phase) {
          return !progress || progress(t < 0 ? -1 : (static_cast<double>(i) + t) / static_cast<double>(frames.size()), phase);
        }), f);
        draw_view(d, f, *v, *g, &doc, &scene);
      }
      ++views;
    } catch (const std::exception& e) {
      d.prims.resize(from);
      skipped.push_back({{"id", f.id}, {"error", e.what()}});
    }
  }
  std::vector<std::string> owners;
  for (const auto& id : s.items)
    if (const SheetItem* t = scene.sheet_item(id); t && std::find(owners.begin(), owners.end(), t->view) == owners.end()) owners.push_back(t->view);
  for (const auto& owner : owners) items += draw_items(d, doc, scene, s, frames, owner, skipped);
  if (report) *report = {{"views", views}, {"items", items}, {"bodies", 0}, {"skipped", skipped}};
  return d;
}

// ---------------------------------------------------------------- drawing
void draw_table_item(Display& d, const json& def, const json& m, const DimStyle& s) {
  const int layer = d.layer({"Tables", kInk, LineType::Continuous, 0.35});
  const std::string kind = def.value("kind", "");
  Table t;
  t.width = def.value("width", 0.0);
  if (kind == "parts_list") {
    const json columns = m.value("columns", default_columns()), headers = m.value("headers", json::object());
    const std::string unit = m.value("mass_unit", "g");
    for (const auto& c : columns) {
      const std::string id = c.get<std::string>();
      t.head.push_back(headers.value(id, header_of(id, unit)));
      t.centred.push_back(id == "item" || id == "qty" || id == "mass");
      if (id == "name" || (id == "description" && t.stretch < 0)) t.stretch = static_cast<int>(t.head.size() - 1);
    }
    for (const auto& r : m.value("rows", json::array())) {
      std::vector<std::string> cells;
      for (const auto& c : columns) {
        const std::string id = c.get<std::string>();
        const json v = id == "item" ? r.value("number", json()) : r.value(id, r.value("properties", json::object()).value(id, json()));
        if (id == "mass") cells.push_back(v.is_number() ? number(v.get<double>(), unit == "g" ? 1 : 3) : "");
        else cells.push_back(v.is_string() ? v.get<std::string>() : v.is_number() ? number(v.get<double>(), 4) : "");
      }
      t.rows.push_back(cells);
    }
    draw_table(d, layer, vec2(def.value("at", json())), def.value("grow", "up") != "down", t, s);  // item 1 next to the header
  } else if (kind == "revision_table") {
    t.head = {"REV", "DESCRIPTION", "DATE", "APPROVED"};
    t.centred = {true, false, true, false};
    t.stretch = 1;
    for (const auto& r : m.value("rows", json::array()))
      t.rows.push_back({r.value("rev", ""), r.value("description", ""), r.value("date", ""), r.value("approved", "")});
    draw_table(d, layer, vec2(def.value("at", json())), def.value("grow", "down") == "up", t, s);  // the first issue next to the header
  }
}

}  // namespace opad::drawing
