// Annotations of drawing sheets (TODO 11 UI-79, UI-80, UI-81): references picked on a view, items measured from the
// model and drawn from their measure, the records the sheet_item command appends, smart dimension readings and dimension
// sets from datums.
#include "opad/drawing/annotate.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <regex>

#include "../design/engine.hpp"
#include "opad/design/expr.hpp"
#include "opad/design/feature.hpp"
#include "opad/drawing/holes.hpp"
#include "opad/drawing/symbols.hpp"
#include "opad/geometry.hpp"

namespace opad::drawing {
namespace {

double dot3(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 scaled(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 plus3(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 minus3(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 unit3(const Vec3& a) {
  const double l = std::sqrt(dot3(a, a));
  return l > 0 ? scaled(a, 1 / l) : a;
}
Vec3 of(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }
Vec3 of(const gp_Dir& d) { return {d.X(), d.Y(), d.Z()}; }

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
Vec2 vec2(const json& j, Vec2 fallback = {0, 0}) {
  if (!j.is_array() || j.size() != 2 || !j[0].is_number() || !j[1].is_number()) return fallback;
  return {j[0].get<double>(), j[1].get<double>()};
}
json js(Vec2 v) { return json::array({v[0], v[1]}); }
double r3(double v) { return std::round(v * 1000) / 1000; }

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

// References resolved as the design engine resolves a feature's (hint-aware), in the final scene.
struct Resolver {
  static std::vector<design::ParamDef> defs_of(const Scene& scene) {
    std::vector<design::ParamDef> defs;
    for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
    return defs;
  }
  design::ParamTable params;
  std::map<std::string, TopoDS_Shape> fresh;
  json notes = json::object();
  design::Ctx ctx;
  Resolver(const Document& doc, const Scene& scene) : params(defs_of(scene)), ctx{doc, params, scene, fresh, {}, &notes} {}
};

// What a reference gives an annotation: a point (its aspect), and the line, circle, cylinder or plane it lies on.
struct Pick {
  Vec3 p{0, 0, 0}, a{0, 0, 0}, b{0, 0, 0}, centre{0, 0, 0}, axis{0, 0, 1};
  bool edge = false, line = false, circle = false, full = false, cylinder = false, plane = false, point = false;
  double r = 0;
  std::string node;
  TopoDS_Shape sub;
};

Pick pick_of(const Resolver& R, json r) {
  Pick k;
  if (r.is_string()) r = Ref::parse(r.get<std::string>()).to_json();
  std::string aspect = r.value("aspect", "");
  if (r.value("kind", "") == "center") r["kind"] = "edge", aspect = "center";
  if (r.value("kind", "") == "point") {
    k.p = Ref::from_json(r).point;
    k.point = true;
    return k;
  }
  const design::ResolvedRef res = R.ctx.resolve(r);
  k.node = res.node;
  k.sub = res.sub;
  const TopoDS_Shape& s = res.sub;
  if (s.ShapeType() == TopAbs_VERTEX) {
    k.p = of(BRep_Tool::Pnt(TopoDS::Vertex(s)));
    k.point = true;
    return k;
  }
  if (s.ShapeType() == TopAbs_FACE) {
    const TopoDS_Face face = TopoDS::Face(s);
    const BRepAdaptor_Surface f(face);
    double u0, u1, v0, v1;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    if (f.GetType() == GeomAbs_Cylinder) {
      const gp_Cylinder c = f.Cylinder();
      const gp_Pnt o = c.Location();
      const gp_Dir z = c.Axis().Direction();
      k.cylinder = true, k.r = c.Radius(), k.axis = of(z);
      k.a = of(o.Translated(gp_Vec(z) * v0)), k.b = of(o.Translated(gp_Vec(z) * v1));
      k.p = k.centre = scaled(plus3(k.a, k.b), 0.5);
      return k;
    }
    if (f.GetType() == GeomAbs_Plane) {
      k.plane = true, k.axis = of(f.Plane().Axis().Direction());
      k.p = k.centre = of(f.Value((u0 + u1) / 2, (v0 + v1) / 2));
      return k;
    }
    throw Error("pick an edge, a vertex, a cylinder or a flat face");
  }
  if (s.ShapeType() != TopAbs_EDGE) throw Error("pick an edge or a vertex");
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
    k.full = std::fabs(t1 - t0) >= 2 * M_PI - 1e-6;
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
  if (!aspect.empty()) k.point = true;
  return k;
}

// Paper mm from a view's centre.
struct Paper {
  const ViewFrame& f;
  Vec2 operator()(const Vec3& p) const {
    const Vec2 v = f.view(p);
    return {f.scale * (v[0] - f.centre[0]), f.scale * (v[1] - f.centre[1])};
  }
  bool along(const Vec3& axis) const { return std::fabs(dot3(unit3(axis), f.dir)) >= 0.9999; }
  bool across(const Vec3& axis) const { return std::fabs(dot3(unit3(axis), f.dir)) <= 1e-4; }
};

double per_mm(const Sheet& sheet) { return sheet.def.value("units", "mm") == "in" ? 1 / 25.4 : 1; }

const Sheet* sheet_of(const Scene& scene, const std::string& id) { return scene.sheet(id); }

// A reference as a sheet item keeps it: with the hint that lets it follow a topology change, and its aspect. A centre
// reference is its circle's edge with the aspect "center".
json reference(const Document& doc, const Scene& scene, const json& r, std::string aspect) {
  Ref ref = Ref::from_json(r);
  if (aspect.empty() && r.is_object()) aspect = r.value("aspect", "");
  if (ref.kind == Ref::Kind::Center) {
    ref.kind = Ref::Kind::Edge;
    if (aspect.empty()) aspect = "center";
  }
  if (ref.kind != Ref::Kind::Point && !scene.node(ref.body)) throw Error("reference body " + ref.body + " does not exist");
  json j = r.is_object() && r.contains("hint") ? r : design::make_ref(doc, scene, ref);  // picked on a worker: hinted there
  if (ref.kind != Ref::Kind::Point) j["kind"] = Ref::kind_name(ref.kind);
  if (!aspect.empty()) {
    static const std::set<std::string> aspects = {"start", "end", "mid", "center"};
    if (!aspects.count(aspect)) throw Error("aspect is start, end, mid or center, not '" + aspect + "'");
    j["aspect"] = aspect;
  } else if (j.is_object()) {
    j.erase("aspect");
  }
  return j;
}

json references(const Document& doc, const Scene& scene, const json& refs, const json& aspects) {
  if (!refs.is_array() || refs.empty()) throw Error("refs: a list of references");
  json out = json::array();
  for (size_t i = 0; i < refs.size(); ++i)
    out.push_back(reference(doc, scene, refs[i], aspects.is_array() && i < aspects.size() ? aspects[i].get<std::string>() : std::string()));
  return out;
}

const ViewFrame& frame_of(const std::vector<ViewFrame>& frames, const std::string& id) {
  for (const auto& f : frames)
    if (f.id == id) return f;
  throw Error("view " + id + " is not on its sheet");
}

std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// "B(M)" -> "BⓂ": a datum or tolerance with its circled modifier.
std::string with_modifier(const std::string& s) {
  const size_t open = s.find('(');
  if (open == std::string::npos || s.back() != ')') return s;
  return s.substr(0, open) + modifier_glyph(upper(s.substr(open + 1, s.size() - open - 2)));
}

FrameCells frame_cells(const json& d) {
  FrameCells c;
  c.characteristic = characteristic_glyph(d.value("characteristic", ""));
  const json& v = d.contains("value") ? d["value"] : json(0);
  c.tolerance = (d.value("zone", "") == "diameter" ? "⌀" : "") + (v.is_number() ? number(v.get<double>(), 4) : v.is_string() ? v.get<std::string>() : std::string());
  if (const std::string m = d.value("material", ""); !m.empty()) c.tolerance += modifier_glyph(upper(m));
  for (const auto& x : d.value("datums", json::array()))
    if (x.is_string() && !x.get<std::string>().empty()) c.datums.push_back(with_modifier(x.get<std::string>()));
  return c;
}

// A hole's values from the hole feature that drilled it, where one drilled a hole of this size into the body.
void feature_values(const Resolver& R, const std::string& node, Hole& h) {
  const auto length = [&](const json& in, const char* k, double fallback) { return in.contains(k) ? design::eval_input(R.params, in[k], design::Dim::Length) : fallback; };
  const auto angle = [&](const json& in, const char* k, double fallback) {
    return in.contains(k) ? design::eval_input(R.params, in[k], design::Dim::Angle) * 180 / M_PI : fallback;
  };
  for (const auto& f : R.ctx.scene.features) {
    if (f.kind != "hole" || f.suppressed || !f.error.empty()) continue;
    bool drilled = false;
    for (const auto& b : f.result.value("bodies", json::array())) drilled = drilled || b.value("id", "") == node;
    if (!drilled) continue;
    try {
      const json& in = f.inputs;
      if (std::fabs(length(in, "diameter", 5) - h.diameter) > 1e-6 || in.value("type", "simple") != h.type) continue;
      const bool all = in.value("extent", "distance") == "all";
      if (!h.through && (all || std::fabs(length(in, "depth", 10) - h.depth) > 1e-6)) continue;
      h.feature = f.id;
      if (h.type == "counterbore") h.cb_diameter = length(in, "cb_diameter", h.cb_diameter), h.cb_depth = length(in, "cb_depth", h.cb_depth);
      if (h.type == "countersink") h.cs_diameter = length(in, "cs_diameter", h.cs_diameter), h.cs_angle = angle(in, "cs_angle", h.cs_angle);
      if (!h.through && in.value("tip", "flat") == "angled") h.tip_angle = angle(in, "tip_angle", h.tip_angle);
      return;
    } catch (const std::exception&) {
    }
  }
}

}  // namespace

json item_references(const Document& doc, const Scene& scene, const json& refs, const json& aspects) { return references(doc, scene, refs, aspects); }

// ---------------------------------------------------------------- values
std::string format_number(double value, int precision, const Sheet* sheet) {
  precision = std::clamp(precision, 0, 8);
  if (sheet && sheet->standard == "asme" && sheet->def.value("units", "mm") == "in") {  // .500: trailing zeros, no leading zero
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", precision, value);
    std::string s = buf;
    if (s.rfind("0.", 0) == 0) s.erase(0, 1);
    else if (s.rfind("-0.", 0) == 0) s.erase(1, 1);
    return std::all_of(s.begin(), s.end(), [](char c) { return c == '0' || c == '.' || c == '-'; }) ? (precision ? "." + std::string(static_cast<size_t>(precision), '0') : "0") : s;
  }
  return number(value, precision);
}

std::string format_value(double value, const json& item, const Sheet* sheet) {
  const int precision = std::clamp(item.value("precision", 2), 0, 8);
  const std::string type = item.value("type", "");
  const std::string shown = format_number(value, precision, sheet);
  if (item.contains("text") && item["text"].is_string()) {  // "<>" stands for the value, as drafting tools write it
    std::string text = item["text"].get<std::string>();
    if (const size_t at = text.find("<>"); at != std::string::npos) text.replace(at, 2, shown);
    return text;
  }
  const std::string symbol = type == "diameter" ? "⌀" : type == "radius" ? "R" : "", degrees = type == "angle" ? "°" : "";
  std::string out = symbol + shown + degrees;
  if (item.contains("tol") && item["tol"].is_object()) {
    const json& t = item["tol"];
    const double plus = t.value("plus", 0.0), minus = t.value("minus", -plus);
    const int decimals = std::max(precision, 3);
    const std::string kind = t.value("type", "sym");
    const auto sign = [&](double x) {  // a zero deviation is a plain 0
      const std::string n = format_number(std::fabs(x), decimals, sheet);
      return std::all_of(n.begin(), n.end(), [](char c) { return c == '0' || c == '.'; }) ? std::string("0") : (x < 0 ? "-" : "+") + n;
    };
    if (kind == "limits") out = symbol + format_number(value + plus, decimals, sheet) + degrees + "\n" + symbol + format_number(value + minus, decimals, sheet) + degrees;
    else if (kind == "sym") out += " ±" + format_number(std::fabs(plus), decimals, sheet);
    else if (kind == "fit") out += " " + t.value("fit", "") + (t.contains("plus") || t.contains("minus") ? " (" + sign(t.value("plus", 0.0)) + "/" + sign(t.value("minus", 0.0)) + ")" : "");
    else out += " " + sign(plus) + "/" + sign(minus);
  }
  return item.value("prefix", "") + out + item.value("suffix", "");
}

void check_tolerance(const json& t) {
  if (t.is_null()) return;
  if (!t.is_object()) throw Error("tolerance: an object {type, plus, minus} or {type: fit, fit}");
  const std::string type = t.value("type", "sym");
  if (type != "sym" && type != "dev" && type != "limits" && type != "fit") throw Error("tolerance: type is sym, dev, limits or fit");
  for (const char* k : {"plus", "minus"})
    if (t.contains(k) && !t[k].is_number()) throw Error(std::string("tolerance: ") + k + " is a number in the sheet's units");
  if (type == "fit") {
    static const std::regex designation("[A-Za-z]{1,2}[0-9]{1,2}(/[A-Za-z]{1,2}[0-9]{1,2})?");
    if (!t.contains("fit") || !t["fit"].is_string() || !std::regex_match(t["fit"].get<std::string>(), designation))
      throw Error("tolerance: a fit is a designation such as H7, g6 or H7/g6");
  }
}

json evaluate_item(const Document& doc, const Scene& scene, const Sheet& sheet, const SheetItem& item, const ViewFrame& frame) {
  if (item.kind != "dimension") throw Error("only dimensions have a value");
  if (!frame.error.empty()) throw Error("its view cannot be drawn: " + frame.error);
  const Resolver R(doc, scene);
  std::vector<Pick> picks;
  for (const auto& r : item.def.value("refs", json::array())) picks.push_back(pick_of(R, r));
  const std::string& type = item.type;
  const double units = per_mm(sheet);
  double value = 0;
  Vec3 anchor{0, 0, 0};
  // What it measures on paper, for drawing it (paper mm from the view's centre, as the anchor).
  json geometry = json::object();
  const Paper paper{frame};
  const auto paper_of = [&](const Vec2& v) { return json::array({frame.scale * (v[0] - frame.centre[0]), frame.scale * (v[1] - frame.centre[1])}); };
  const auto lineish = [](const Pick& k) { return k.line && !k.point; };
  if (type == "horizontal" || type == "vertical" || type == "aligned") {
    Vec2 a, b;
    if (picks.size() == 1 && picks[0].edge && !picks[0].point) {
      a = frame.view(picks[0].a), b = frame.view(picks[0].b);
      anchor = scaled(plus3(picks[0].a, picks[0].b), 0.5);
    } else if (picks.size() == 1 && picks[0].cylinder && paper.across(picks[0].axis)) {  // a cylinder from the side: across it
      Vec2 n = frame.view(picks[0].axis);
      n = unit(left(n));
      const Vec2 c = frame.view(picks[0].centre);
      a = sub(c, mul(n, picks[0].r)), b = add(c, mul(n, picks[0].r));
      anchor = picks[0].centre;
    } else if (picks.size() == 2) {
      a = frame.view(picks[0].p), b = frame.view(picks[1].p);
      anchor = scaled(plus3(picks[0].p, picks[1].p), 0.5);
    } else {
      throw Error("a " + type + " dimension takes two references or one edge");
    }
    value = type == "horizontal" ? std::fabs(b[0] - a[0]) : type == "vertical" ? std::fabs(b[1] - a[1]) : std::hypot(b[0] - a[0], b[1] - a[1]);
    geometry = {{"from", paper_of(a)}, {"to", paper_of(b)}};
    if (type == "aligned" && picks.size() == 2 && (lineish(picks[0]) || lineish(picks[1]))) {
      // Across two parallel lines, or from a point square to a line.
      const int li = lineish(picks[0]) ? 0 : 1;
      const Pick& L = picks[size_t(li)];
      const Pick& O = picks[size_t(1 - li)];
      const Vec2 a0 = frame.view(L.a), a1 = frame.view(L.b);
      const double la = std::hypot(a1[0] - a0[0], a1[1] - a0[1]);
      if (la > 1e-9) {
        const Vec2 u{(a1[0] - a0[0]) / la, (a1[1] - a0[1]) / la};
        if (lineish(O)) {
          const Vec2 b0 = frame.view(O.a), b1 = frame.view(O.b);
          const double lb = std::hypot(b1[0] - b0[0], b1[1] - b0[1]);
          if (lb > 1e-9) {
            const Vec2 w{(b1[0] - b0[0]) / lb, (b1[1] - b0[1]) / lb};
            if (std::fabs(u[0] * w[1] - u[1] * w[0]) < 1e-6) {
              value = std::fabs(u[0] * (b0[1] - a0[1]) - u[1] * (b0[0] - a0[0]));
              const Vec2 m{(a0[0] + a1[0]) / 2, (a0[1] + a1[1]) / 2};  // from the middle of the first line straight across
              const double t = (m[0] - b0[0]) * w[0] + (m[1] - b0[1]) * w[1];
              geometry = {{"from", paper_of(m)}, {"to", paper_of({b0[0] + w[0] * t, b0[1] + w[1] * t})}};
            }
          }
        } else {
          const Vec2 q = frame.view(O.p);
          const double t = (q[0] - a0[0]) * u[0] + (q[1] - a0[1]) * u[1];
          const Vec2 foot{a0[0] + u[0] * t, a0[1] + u[1] * t};
          value = std::hypot(q[0] - foot[0], q[1] - foot[1]);
          geometry = {{"from", paper_of(foot)}, {"to", paper_of(q)}};
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
      const Vec3 e = unit3(minus3(picks[size_t(i)].b, picks[size_t(i)].a));
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
  json out = {{"value", value}, {"shown", format_value(value, item.def, &sheet)}, {"anchor", {at[0] - frame.at[0], at[1] - frame.at[1]}}, {"geometry", geometry}};
  if (R.notes.contains("rehinted")) out["rehinted"] = R.notes["rehinted"];
  return out;
}

bool known_item(const std::string& kind, const std::string& type) {
  static const std::set<std::string> kinds = {"dimension", "note", "centermark", "centerline", "hole_callout", "hole_table", "datum", "fcf", "surface", "dimension_set"};
  static const std::set<std::string> dimensions = {"horizontal", "vertical", "aligned", "radius", "diameter", "angle"};
  static const std::set<std::string> sets = {"ordinate", "baseline", "chain"};
  if (!kinds.count(kind)) return false;
  if (kind == "dimension") return dimensions.count(type) > 0;
  if (kind == "dimension_set") return sets.count(type) > 0;
  return true;
}

// ---------------------------------------------------------------- picks
json pick_reference(const Document& doc, const Scene& scene, const ViewFrame& frame, const json& pick) {
  const std::string node = pick.value("node", "");
  const Node* n = scene.node(node);
  if (!n || n->kind != Node::Kind::Body) throw Error("that is not a body of the model");
  const int edge = pick.value("edge", -1), face = pick.value("face", -1);
  if (edge < 0 && face < 0) throw Error("that line is not an edge of the model");
  const std::string snap = pick.value("snap", "nearest");
  const Vec2 at = sub(vec2(pick.value("at", json())), frame.at);
  json ref = design::make_ref(doc, scene, Ref{node, edge >= 0 ? Ref::Kind::Edge : Ref::Kind::Face, edge >= 0 ? edge : face});
  const Resolver R(doc, scene);
  const Paper paper{frame};
  Pick k = pick_of(R, ref);
  std::string what;
  Vec2 point = paper(k.p);
  if (k.cylinder) what = "cylinder";
  else if (k.plane) what = "plane";
  else if (!k.edge) what = "face";
  else if (k.line) what = "line";
  else if (k.circle) what = k.full ? "circle" : "arc";
  else what = "edge";
  if (k.edge) {
    const Vec2 a = paper(k.a), b = paper(k.b);
    if (snap == "centre" && (k.circle || k.centre != Vec3{0, 0, 0})) {
      ref["aspect"] = "center", what = "center", point = paper(k.centre);
    } else if (snap == "end") {  // the end of the edge that shows there (a hidden-line piece may end elsewhere)
      const double da = len(sub(a, at)), db = len(sub(b, at)), tol = std::max(0.05, 0.02 * len(sub(a, b)));
      if (std::min(da, db) <= tol) {
        ref["aspect"] = da <= db ? "start" : "end", what = "vertex", point = da <= db ? a : b;
      }
    } else if (snap == "mid" && k.line) {
      const Vec2 m = mul(add(a, b), 0.5);
      if (len(sub(m, at)) <= std::max(0.05, 0.02 * len(sub(a, b)))) ref["aspect"] = "mid", what = "midpoint", point = m;
    }
  }
  return {{"ref", ref}, {"what", what}, {"at", js(point)}};
}

// ---------------------------------------------------------------- measure
json measure_item(const Document& doc, const Scene& scene, const Sheet& sheet, const SheetItem& item, const ViewFrame* frame) {
  const json& d = item.def;
  const std::string& kind = item.kind;
  if (kind == "dimension") {
    if (!frame) throw Error("a dimension needs its view");
    return evaluate_item(doc, scene, sheet, item, *frame);
  }
  const json refs = d.value("refs", json::array());
  if (kind == "note") {
    json m = json::object();
    if (!refs.empty()) {
      if (!frame || !frame->error.empty()) throw Error("its view cannot be drawn");
      const Resolver R(doc, scene);
      const Pick k = pick_of(R, refs[0]);
      const Paper paper{*frame};
      Vec2 tip = paper(k.p);
      if (k.circle && !k.point && paper.along(k.axis)) {  // onto the circle, on the text's side
        const Vec2 c = paper(k.centre);
        tip = add(c, mul(unit(sub(vec2(d.value("at", json()), c), c)), k.r * frame->scale));
      }
      m["tip"] = js(tip);
    } else if (d.contains("to")) {
      m["tip"] = d["to"];
    }
    return m;
  }
  if (!known_item(kind, item.type)) throw Error("needs a newer OPAD (sheet_item kind '" + kind + "')");
  if (!frame) throw Error("a " + kind + " needs its view");
  if (!frame->error.empty()) throw Error("its view cannot be drawn: " + frame->error);
  const Resolver R(doc, scene);
  const Paper paper{*frame};
  const double units = per_mm(sheet);
  const Vec2 place = vec2(d.value("place", json::object()).value("text", json()));
  const auto need = [&](size_t lo, size_t hi, const char* what) {
    if (refs.size() < lo || refs.size() > hi) throw Error(std::string("a ") + kind + " takes " + what);
  };
  if (kind == "centermark") {
    need(1, 1, "one circle");
    const Pick k = pick_of(R, refs[0]);
    if (!(k.circle || k.cylinder)) throw Error("a centre mark takes a circle or its cylinder");
    if (!paper.along(k.axis)) throw Error("the circle is not seen along its axis in this view");
    return {{"centre", js(paper(k.centre))}, {"r", k.r * frame->scale}};
  }
  if (kind == "centerline") {
    need(1, 2, "two circles, a cylinder seen from the side or two parallel lines");
    std::vector<Pick> k;
    for (const auto& r : refs) k.push_back(pick_of(R, r));
    if (k.size() == 1) {
      if (!k[0].cylinder || !paper.across(k[0].axis)) throw Error("a centre line takes a cylinder seen from the side, two circles or two parallel lines");
      return {{"from", js(paper(k[0].a))}, {"to", js(paper(k[0].b))}, {"r", {0, 0}}};
    }
    const auto lineish = [](const Pick& p) { return p.line && !p.point; };
    if (lineish(k[0]) && lineish(k[1])) {
      const Vec2 a0 = paper(k[0].a), a1 = paper(k[0].b), b0 = paper(k[1].a), b1 = paper(k[1].b);
      const Vec2 u = unit(sub(a1, a0)), w = unit(sub(b1, b0));
      if (std::fabs(u[0] * w[1] - u[1] * w[0]) > 1e-6) throw Error("the lines are not parallel in this view");
      const Vec2 n = left(u), mid = mul(add(a0, b0), 0.5);
      const double off = dot(sub(mid, a0), n);
      double lo = 1e300, hi = -1e300;
      for (const Vec2 p : {a0, a1, b0, b1}) lo = std::min(lo, dot(sub(p, a0), u)), hi = std::max(hi, dot(sub(p, a0), u));
      const Vec2 base = add(a0, mul(n, off));
      return {{"from", js(add(base, mul(u, lo)))}, {"to", js(add(base, mul(u, hi)))}, {"r", {0, 0}}};
    }
    double r[2] = {0, 0};
    Vec2 c[2];
    for (int i = 0; i < 2; ++i) {
      const Pick& p = k[size_t(i)];
      if (!(p.circle || p.cylinder || p.point)) throw Error("a centre line takes the centres of two circles");
      c[i] = paper(p.circle || p.cylinder ? p.centre : p.p);
      if ((p.circle || p.cylinder) && paper.along(p.axis)) r[i] = p.r * frame->scale;
    }
    if (len(sub(c[1], c[0])) < 1e-9) throw Error("the two centres are the same point in this view");
    return {{"from", js(c[0])}, {"to", js(c[1])}, {"r", {r[0], r[1]}}};
  }
  if (kind == "hole_callout") {
    need(1, 1, "one hole");
    const design::ResolvedRef rr = R.ctx.resolve(refs[0].is_string() ? Ref::parse(refs[0].get<std::string>()).to_json() : refs[0]);
    const TopoDS_Shape body = R.ctx.node_shape(rr.node);
    const auto holes = find_holes(body);
    const int i = hole_of(holes, body, rr.sub);
    if (i < 0) throw Error("that is not a hole: pick its wall or one of its circles");
    Hole h = holes[static_cast<size_t>(i)];
    int count = 0;
    for (const auto& o : holes) count += o.same_size(h);
    feature_values(R, rr.node, h);
    const int precision = d.value("precision", 2);
    const std::string shown = hole_callout(h, count, sheet.standard, units, [&](double v) { return format_number(v, precision, &sheet); });
    const Vec2 c = paper(h.entry);
    json m = {{"shown", shown}, {"hole", h.to_json()}, {"count", count}, {"centre", js(c)}, {"r", 0}};
    if (paper.along(h.dir)) {
      m["r"] = h.outer() / 2 * frame->scale;
    } else if (paper.across(h.dir)) {  // seen from the side: the ends of its opening, where the wall's outlines meet the surface
      const Vec2 n = mul(left(unit(frame->view(h.dir))), h.outer() / 2 * frame->scale);
      m["ends"] = {js(sub(c, n)), js(add(c, n))};
    }
    if (R.notes.contains("rehinted")) m["rehinted"] = R.notes["rehinted"];
    return m;
  }
  if (kind == "hole_table") {
    need(0, 1, "at most an origin");
    const SheetView* v = scene.sheet_view(item.view);
    if (!v) throw Error("its view does not exist");
    const ViewSpec spec = view_spec(scene, *v);
    Vec2 origin;  // view coordinates
    if (!refs.empty()) {
      origin = frame->view(pick_of(R, refs[0]).p);
    } else {
      const auto e = view_extent(doc, scene, spec);
      origin = {e[0], e[1]};
    }
    struct Found {
      Hole h;
      Vec2 at;
      int group = -1;
    };
    std::vector<Found> found;
    for (const auto& [node, world] : view_bodies(scene, spec)) {
      const TopoDS_Shape body = R.ctx.node_shape(node);
      for (auto& h : find_holes(body)) {
        if (!paper.along(h.dir)) continue;
        feature_values(R, node, h);
        found.push_back({h, frame->view(h.entry)});
      }
    }
    // A letter per size (smallest first), a number per hole (top to bottom, left to right).
    std::vector<size_t> groups;  // a representative hole per size
    for (size_t i = 0; i < found.size(); ++i) {
      for (size_t g = 0; g < groups.size() && found[i].group < 0; ++g) {
        Hole a = found[groups[g]].h, b = found[i].h;
        b.dir = a.dir;  // seen along the view either way
        if (a.same_size(b)) found[i].group = static_cast<int>(g);
      }
      if (found[i].group < 0) found[i].group = static_cast<int>(groups.size()), groups.push_back(i);
    }
    std::vector<size_t> rank(groups.size());
    std::iota(rank.begin(), rank.end(), 0);
    std::stable_sort(rank.begin(), rank.end(), [&](size_t a, size_t b) { return found[groups[a]].h.outer() < found[groups[b]].h.outer() - 1e-9; });
    std::vector<int> letter(groups.size());
    for (size_t i = 0; i < rank.size(); ++i) letter[rank[i]] = static_cast<int>(i);
    std::vector<size_t> order(found.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      const Found &x = found[a], &y = found[b];
      if (letter[size_t(x.group)] != letter[size_t(y.group)]) return letter[size_t(x.group)] < letter[size_t(y.group)];
      if (std::fabs(x.at[1] - y.at[1]) > 1e-6) return x.at[1] > y.at[1];
      return x.at[0] < y.at[0];
    });
    const int precision = d.value("precision", 2);
    const auto num = [&](double x) { return format_number(x, precision, &sheet); };
    json rows = json::array();
    std::map<int, int> numbers;
    for (size_t i : order) {
      const Found& f = found[i];
      const int l = letter[size_t(f.group)];
      std::string tag = l < 26 ? std::string(1, static_cast<char>('A' + l)) : "A" + std::to_string(l);
      tag += std::to_string(++numbers[l]);
      std::string size = hole_callout(f.h, 1, sheet.standard, units, num);
      std::replace(size.begin(), size.end(), '\n', ' ');
      const Vec2 rel{frame->scale * (f.at[0] - frame->centre[0]), frame->scale * (f.at[1] - frame->centre[1])};
      rows.push_back({{"tag", tag}, {"x", (f.at[0] - origin[0]) * units}, {"y", (f.at[1] - origin[1]) * units}, {"x_shown", num((f.at[0] - origin[0]) * units)},
                      {"y_shown", num((f.at[1] - origin[1]) * units)}, {"size", size}, {"centre", js(rel)}, {"r", f.h.outer() / 2 * frame->scale}});
    }
    return {{"rows", rows}, {"origin", js({frame->scale * (origin[0] - frame->centre[0]), frame->scale * (origin[1] - frame->centre[1])})}};
  }
  if (kind == "datum" || kind == "surface") {
    need(1, 1, "one edge");
    const Pick k = pick_of(R, refs[0]);
    Vec2 a, b;
    if (k.line) {  // the edge (a point picked on it names it too)
      a = paper(k.a), b = paper(k.b);
    } else if (k.cylinder && paper.across(k.axis)) {  // the side of it towards the place
      const Vec2 c0 = paper(k.a), c1 = paper(k.b), n = left(unit(sub(c1, c0)));
      const double side = dot(sub(place, c0), n) >= 0 ? 1 : -1;
      a = add(c0, mul(n, side * k.r * frame->scale)), b = add(c1, mul(n, side * k.r * frame->scale));
    } else if (k.circle && paper.along(k.axis)) {
      const Vec2 c = paper(k.centre), out = unit(sub(place, c), {0, 1});
      return {{"foot", js(add(c, mul(out, k.r * frame->scale)))}, {"out", js(out)}};
    } else if (k.plane && paper.across(k.axis)) {  // a flat face seen edge on: through its middle, along it
      const Vec2 c = paper(k.centre), along = left(unit(frame->view(k.axis), {1, 0}));
      a = sub(c, mul(along, 5)), b = add(c, mul(along, 5));
    } else {
      throw Error("pick a straight edge (or a circle seen along its axis)");
    }
    const Vec2 u = unit(sub(b, a));
    const double l = len(sub(b, a)), t = std::clamp(dot(sub(place, a), u), 0.1 * l, 0.9 * l);
    const Vec2 foot = add(a, mul(u, t));
    Vec2 out = left(u);
    if (dot(sub(place, foot), out) < 0 || (len(sub(place, foot)) < 1e-9 && dot(foot, out) < 0)) out = mul(out, -1);
    return {{"foot", js(foot)}, {"out", js(out)}, {"line", {js(a), js(b)}}};
  }
  if (kind == "fcf") {
    need(0, 1, "at most one feature");
    if (refs.empty()) return json::object();
    const Pick k = pick_of(R, refs[0]);
    Vec2 tip = paper(k.p);
    if (k.line) {  // the edge's point nearest the frame
      const Vec2 a = paper(k.a), b = paper(k.b), u = unit(sub(b, a));
      tip = add(a, mul(u, std::clamp(dot(sub(place, a), u), 0.0, len(sub(b, a)))));
    } else if ((k.circle || k.cylinder) && !k.point && paper.along(k.axis)) {  // onto the circle, towards the frame's corner
      const Vec2 c = paper(k.centre);
      tip = add(c, mul(unit(sub(place, c), {0, 1}), k.r * frame->scale));
    }
    return {{"tip", js(tip)}};
  }
  if (kind == "dimension_set") {
    if (refs.size() < 2) throw Error("a dimension set takes an origin and at least one feature");
    const std::string axis = d.value("axis", "horizontal");
    if (axis != "horizontal" && axis != "vertical") throw Error("axis is horizontal or vertical");
    const Vec2 u = axis == "horizontal" ? Vec2{1, 0} : Vec2{0, 1};
    std::vector<Vec2> view, pts;
    for (const auto& r : refs) {
      const Pick k = pick_of(R, r);
      view.push_back(frame->view(k.circle || k.cylinder ? k.centre : k.p));
      pts.push_back(paper(k.circle || k.cylinder ? k.centre : k.p));
    }
    const std::string type = item.type;
    const int precision = d.value("precision", 2);
    json values = json::array(), shown = json::array(), order = json::array();
    if (type == "chain") {  // along the axis from the origin, each from the one before
      std::vector<size_t> idx(view.size());
      std::iota(idx.begin(), idx.end(), 0);
      const double o = dot(view[0], u);
      std::stable_sort(idx.begin() + 1, idx.end(), [&](size_t a, size_t b) { return std::fabs(dot(view[a], u) - o) < std::fabs(dot(view[b], u) - o); });
      for (size_t i = 0; i < idx.size(); ++i) order.push_back(idx[i]);
      for (size_t i = 1; i < idx.size(); ++i) values.push_back(r3(std::fabs(dot(sub(view[idx[i]], view[idx[i - 1]]), u)) * units * 1000) / 1000);
    } else {
      for (size_t i = 1; i < view.size(); ++i) {
        const double v = dot(sub(view[i], view[0]), u) * units;
        values.push_back(type == "baseline" ? std::fabs(v) : v);
      }
    }
    for (const auto& v : values) shown.push_back(format_number(v.get<double>(), precision, &sheet));
    json points = json::array();
    for (const auto& p : pts) points.push_back(js(p));
    json m = {{"origin", points[0]}, {"points", points}, {"values", values}, {"shown", shown}};
    if (!order.empty()) m["order"] = order;
    return m;
  }
  throw Error("needs a newer OPAD (sheet_item kind '" + kind + "')");
}

json item_result(const json& def, const json& m) {
  const std::string kind = def.value("kind", "");
  if (kind == "dimension") return {{"value", m.value("value", 0.0)}, {"shown", m.value("shown", "")}};
  if (kind == "hole_callout") return {{"shown", m.value("shown", "")}, {"count", m.value("count", 1)}};
  if (kind == "dimension_set") return {{"values", m.value("values", json::array())}, {"shown", m.value("shown", json::array())}};
  if (kind == "hole_table") {
    json rows = json::array();
    for (const auto& r : m.value("rows", json::array())) rows.push_back({{"tag", r["tag"]}, {"x", r3(r["x"].get<double>())}, {"y", r3(r["y"].get<double>())}, {"size", r["size"]}});
    return {{"rows", rows}};
  }
  return nullptr;
}

// ---------------------------------------------------------------- draw
namespace {

void hole_table(Display& d, int layer, Vec2 at, const json& rows, const DimStyle& s) {
  const double h = 2.5 * s.scale, pad = 1.2 * s.scale, row = 2.2 * h;
  std::vector<std::array<std::string, 4>> cells = {{"TAG", "X", "Y", "SIZE"}};
  for (const auto& r : rows) cells.push_back({r.value("tag", ""), r.value("x_shown", ""), r.value("y_shown", ""), r.value("size", "")});
  std::array<double, 4> w{};
  for (const auto& c : cells)
    for (size_t i = 0; i < 4; ++i) w[i] = std::max(w[i], text_width(c[i], h) + 2 * pad);
  const double total = w[0] + w[1] + w[2] + w[3], tall = row * static_cast<double>(cells.size());
  d.polyline(layer, {at, {at[0] + total, at[1]}, {at[0] + total, at[1] - tall}, {at[0], at[1] - tall}}, true);
  for (size_t k = 1; k < cells.size(); ++k) d.line(layer, {at[0], at[1] - row * static_cast<double>(k)}, {at[0] + total, at[1] - row * static_cast<double>(k)});
  double x = at[0];
  for (size_t i = 0; i < 4; ++i) {
    if (i) d.line(layer, {x, at[1]}, {x, at[1] - tall});
    for (size_t k = 0; k < cells.size(); ++k) {
      const double y = at[1] - row * (static_cast<double>(k) + 0.5);
      if (i == 3 && k > 0) rich_text(d, layer, cells[k][i], {x + pad, y}, h, 0, 0, 2);
      else rich_text(d, layer, cells[k][i], {x + w[i] / 2, y}, h, 0, 1, 2);
    }
    x += w[i];
  }
}

}  // namespace

void draw_item(Display& d, const Sheet& sheet, const json& def, const json& m, Vec2 origin, const DimStyle& s) {
  const int dims = d.layer({"Dimensions", kInk, LineType::Continuous, 0.25});
  const std::string kind = def.value("kind", "");
  const auto P = [&](const json& j) { return add(origin, vec2(j)); };
  const Vec2 place = P(def.value("place", json::object()).value("text", json()));
  if (kind == "dimension") {
    const json& g = m["geometry"];
    const std::string text = m["shown"].get<std::string>(), type = def.value("type", "");
    if (g.contains("lines")) {
      const json& l = g["lines"];
      angular_dimension(d, dims, {P(l[0][0]), P(l[0][1])}, {P(l[1][0]), P(l[1][1])}, place, text, s);
    } else if (g.contains("centre")) {
      radial_dimension(d, dims, P(g["centre"]), g["r"].get<double>(), place, text, type == "diameter", s);
    } else {
      const Vec2 a = P(g["from"]), b = P(g["to"]);
      const Vec2 axis = type == "horizontal" ? Vec2{1, 0} : type == "vertical" ? Vec2{0, 1} : Vec2{b[0] - a[0], b[1] - a[1]};
      linear_dimension(d, dims, a, b, axis, place, text, s);
    }
    return;
  }
  if (kind == "note") {
    const int notes = d.layer({"Text", kInk, LineType::Continuous, 0.25});
    const std::string text = def.value("text", "");
    const Vec2 at = P(def.value("at", json()));
    if (m.contains("tip")) {
      DimStyle t = s;
      t.text = def.value("height", 3.5);
      leader(d, notes, P(m["tip"]), at, text, t);
    } else {
      rich_text(d, notes, text, at, def.value("height", 3.5));
    }
    return;
  }
  const int center = d.layer({"Center", kInk, LineType::Continuous, 0.25});
  if (kind == "centermark") {
    centre_mark(d, center, P(m["centre"]), m.value("r", 0.0), def.value("extend", 2.0) * s.scale, {1, 0}, s);
  } else if (kind == "centerline") {
    const Vec2 a = P(m["from"]), b = P(m["to"]), u = unit(sub(b, a));
    const double ext = def.value("extend", 3.0) * s.scale;
    const json r = m.value("r", json::array({0, 0}));
    centre_line(d, center, sub(a, mul(u, r[0].get<double>() + ext)), add(b, mul(u, r[1].get<double>() + ext)), s);
  } else if (kind == "hole_callout") {
    const Vec2 c = P(m["centre"]);
    const double r = m.value("r", 0.0);
    Vec2 tip = r > 0 ? add(c, mul(unit(sub(place, c), {1, 1}), r)) : c;
    if (const json ends = m.value("ends", json()); ends.is_array() && ends.size() == 2) {  // the end of the opening nearer the text
      const Vec2 a = P(ends[0]), b = P(ends[1]);
      tip = len(sub(place, a)) <= len(sub(place, b)) ? a : b;
    }
    leader(d, dims, tip, place, m.value("shown", ""), s);
  } else if (kind == "hole_table") {
    const double h = 2.5 * s.scale;
    for (const auto& row : m.value("rows", json::array())) {
      const double r = row.value("r", 0.0);
      d.text(dims, row.value("tag", ""), add(P(row["centre"]), {0.72 * r + 0.5 * s.scale, 0.72 * r + 0.5 * s.scale}), h, 0, 0, 1);
    }
    hole_table(d, dims, vec2(def.value("at", json())), m.value("rows", json::array()), s);
  } else if (kind == "datum") {
    datum_symbol(d, dims, P(m["foot"]), vec2(m["out"], {0, 1}), place, def.value("letter", ""), s);
  } else if (kind == "surface") {
    surface_symbol(d, dims, P(m["foot"]), vec2(m["out"], {0, 1}), def.value("process", "removal"), def.value("value", ""), s);
  } else if (kind == "fcf") {
    const FrameCells cells = frame_cells(def);
    const double w = feature_frame(d, dims, place, cells, s);
    if (m.contains("tip")) {  // from the frame's nearer end to the feature, an arrow at the feature
      const Vec2 tip = P(m["tip"]), from = std::fabs(tip[0] - place[0]) <= std::fabs(tip[0] - place[0] - w) ? place : Vec2{place[0] + w, place[1]};
      const Vec2 knee{tip[0], from[1]};
      if (std::fabs(knee[0] - from[0]) > 1e-9 && std::fabs(knee[1] - tip[1]) > 1e-9) {
        d.line(dims, from, knee);
        d.line(dims, knee, tip);
      } else {
        d.line(dims, from, tip);
      }
      const Vec2 dir = unit(sub(tip, std::fabs(knee[1] - tip[1]) > 1e-9 ? knee : from)), across = mul(left(dir), s.arrow * s.scale / 6);
      const Vec2 back = sub(tip, mul(dir, s.arrow * s.scale));
      d.fill(dims, {{tip, add(back, across), sub(back, across)}});
    }
  } else if (kind == "dimension_set") {
    const std::string type = def.value("type", "ordinate");
    const Vec2 u = def.value("axis", "horizontal") == "vertical" ? Vec2{0, 1} : Vec2{1, 0}, n = left(u);
    std::vector<Vec2> pts;
    for (const auto& p : m.value("points", json::array())) pts.push_back(P(p));
    std::vector<std::string> shown;
    for (const auto& t : m.value("shown", json::array())) shown.push_back(t.get<std::string>());
    if (pts.empty()) return;
    if (type == "ordinate") {
      std::vector<std::string> texts{"0"};
      texts.insert(texts.end(), shown.begin(), shown.end());
      ordinate_dimensions(d, dims, pts, texts, u, dot(place, n), s);
    } else if (type == "baseline") {
      const double step = def.value("spacing", 7.0) * s.scale;
      std::vector<size_t> idx(pts.size() - 1);
      std::iota(idx.begin(), idx.end(), 1);
      std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return std::fabs(dot(sub(pts[a], pts[0]), u)) < std::fabs(dot(sub(pts[b], pts[0]), u)); });
      const double base = dot(sub(place, pts[0]), n), side = base >= 0 ? 1 : -1;
      for (size_t k = 0; k < idx.size(); ++k) {
        const Vec2 at = add(place, mul(n, side * step * static_cast<double>(k)));
        linear_dimension(d, dims, pts[0], pts[idx[k]], u, at, idx[k] - 1 < shown.size() ? shown[idx[k] - 1] : std::string(), s);
      }
    } else {
      std::vector<size_t> order;
      for (const auto& o : m.value("order", json::array())) order.push_back(o.get<size_t>());
      for (size_t k = 1; k < order.size() && order[k] < pts.size() && order[k - 1] < pts.size(); ++k)
        linear_dimension(d, dims, pts[order[k - 1]], pts[order[k]], u, place, k - 1 < shown.size() ? shown[k - 1] : std::string(), s);
    }
  }
  (void)sheet;
}

// ---------------------------------------------------------------- plan
json plan_item(const Document& doc, const Scene& scene, const json& args, json* measured_out) {
  const std::string sheetId = args.at("sheet").get<std::string>();
  const Sheet* sheet = sheet_of(scene, sheetId);
  if (!sheet) throw Error("sheet " + sheetId + " does not exist (sheet_info lists the sheets)");
  const std::string kind = args.value("kind", args.contains("refs") || args.contains("picks") ? "dimension" : "note");
  json op = {{"op", "sheet_item"}, {"sheet", sheet->id}};
  std::vector<ViewFrame> frames;
  const ViewFrame* frame = nullptr;
  if (args.contains("view") && !args["view"].is_null()) {
    const SheetView* view = scene.sheet_view(args["view"].get<std::string>());
    if (!view || view->sheet != sheet->id) throw Error("sheet_item: view " + args["view"].get<std::string>() + " is not on this sheet");
    op["view"] = view->id;
    if (!view->error.empty()) throw Error("sheet_item: its view cannot be drawn: " + view->error);
    frames = layout(doc, scene, *sheet);
    frame = &frame_of(frames, view->id);
  }
  op["kind"] = kind;
  if (kind != "note" && !known_item(kind, kind == "dimension" || kind == "dimension_set" ? args.value("type", kind == "dimension" ? "aligned" : "ordinate") : "")) {
    if (kind == "dimension") throw Error("sheet_item: type is horizontal, vertical, aligned, radius, diameter or angle");
    if (kind == "dimension_set") throw Error("sheet_item: a dimension set's type is ordinate, baseline or chain");
    throw Error("sheet_item: kind is dimension, note, centermark, centerline, hole_callout, hole_table, datum, fcf, surface or dimension_set");
  }
  // References: given, or picked on the view (the app's snaps).
  json refs = json::array();
  if (args.contains("picks")) {
    if (!frame) throw Error("sheet_item: picks are made on a view");
    // A point on an edge matters to dimensions, sets, leaders and centre lines; the others take the edge itself.
    const bool points = kind == "dimension" || kind == "dimension_set" || kind == "note" || kind == "centerline";
    for (const auto& p : args["picks"]) {
      json r = pick_reference(doc, scene, *frame, p)["ref"];
      if (!points) r.erase("aspect");
      refs.push_back(r);
    }
  } else if (args.contains("refs") && !(args["refs"].is_array() && args["refs"].empty())) {
    refs = references(doc, scene, args["refs"], args.value("aspects", json()));
  }
  if (!refs.empty()) op["refs"] = refs;
  if (kind != "note" && kind != "hole_table" && kind != "fcf" && refs.empty()) throw Error("sheet_item: a " + kind + " needs refs");
  if (kind != "note" && !frame) throw Error("sheet_item: a " + kind + " needs its view");
  if (args.contains("place")) op["place"] = {{"text", args["place"]}};
  for (const char* k : {"precision", "text", "prefix", "suffix"})
    if (args.contains(k)) op[k] = args[k];
  if (args.contains("tolerance")) {
    check_tolerance(args["tolerance"]);
    op["tol"] = args["tolerance"];
  }
  if (kind == "dimension" || kind == "dimension_set") op["type"] = args.value("type", kind == "dimension" ? "aligned" : "ordinate");
  if (kind == "note") {
    const std::string text = args.value("text", "");
    if (text.empty()) throw Error("sheet_item: a note needs its text");
    if (args.contains("height")) op["height"] = args["height"];
    if (args.contains("to")) op["to"] = args["to"];
    if (refs.size() > 1) throw Error("sheet_item: a note's leader points at one reference");
    if (!refs.empty() && !frame) throw Error("sheet_item: a note with a leader needs its view");
  } else if (kind == "centermark" || kind == "centerline") {
    if (args.contains("extend")) op["extend"] = args["extend"];
  } else if (kind == "datum") {
    const std::string letter = upper(args.value("letter", ""));
    if (letter.empty() || letter.size() > 2 || !std::all_of(letter.begin(), letter.end(), [](char c) { return c >= 'A' && c <= 'Z'; }))
      throw Error("sheet_item: a datum's letter is A to Z");
    op["letter"] = letter;
  } else if (kind == "fcf") {
    const std::string c = args.value("characteristic", "");
    if (characteristic_glyph(c).empty()) throw Error("sheet_item: characteristic is one of straightness, flatness, circularity, cylindricity, line_profile, surface_profile, angularity, perpendicularity, parallelism, position, concentricity, symmetry, circular_runout, total_runout");
    op["characteristic"] = c;
    if (!args.contains("value") || !(args["value"].is_number() || args["value"].is_string())) throw Error("sheet_item: a feature control frame needs its tolerance value");
    op["value"] = args["value"];
    if (const std::string z = args.value("zone", ""); !z.empty()) {
      if (z != "diameter") throw Error("sheet_item: zone is diameter or empty");
      op["zone"] = z;
    }
    if (const std::string mc = upper(args.value("material", "")); !mc.empty()) {
      if (mc != "M" && mc != "L" && mc != "S") throw Error("sheet_item: material is M, L or S");
      op["material"] = mc;
    }
    if (args.contains("datums")) {
      json ds = json::array();
      for (const auto& x : args["datums"]) {
        if (!x.is_string()) throw Error("sheet_item: datums are letters");
        if (!x.get<std::string>().empty()) ds.push_back(upper(x.get<std::string>()));
      }
      if (ds.size() > 3) throw Error("sheet_item: at most three datums");
      if (!ds.empty()) op["datums"] = ds;
    }
    if (refs.size() > 1) throw Error("sheet_item: a feature control frame points at one feature");
    if (refs.empty() && !args.contains("place")) throw Error("sheet_item: place the frame or give the feature it points at");
  } else if (kind == "surface") {
    const std::string p = args.value("process", "removal");
    if (p != "any" && p != "removal" && p != "no_removal") throw Error("sheet_item: process is any, removal or no_removal");
    op["process"] = p;
    if (args.contains("value")) op["value"] = args["value"].is_string() ? args["value"] : json(args["value"].dump());
  } else if (kind == "dimension_set") {
    const std::string axis = args.value("axis", "horizontal");
    if (axis != "horizontal" && axis != "vertical") throw Error("sheet_item: axis is horizontal or vertical");
    op["axis"] = axis;
    if (args.contains("spacing")) op["spacing"] = args["spacing"];
  } else if (kind == "hole_table") {
    const auto room = drawing_room(sheet->def);
    op["at"] = args.contains("at") ? args["at"] : json::array({room[0] + 5, room[3] - 5});
  }
  if (kind == "note") {
    op["at"] = args.contains("at") ? args["at"] : op.contains("view") ? json::array({0, 0}) : json::array({sheet->width / 2, sheet->height / 2});
  }
  // Measured as the drawing will measure it; placed beside what it measures unless placed.
  SheetItem t;
  t.sheet = sheet->id;
  t.view = op.value("view", "");
  t.kind = kind;
  t.type = op.value("type", "");
  t.def = op;
  if (kind == "datum" || kind == "surface") {  // the side away from the view's middle unless placed
    if (!op.contains("place")) {
      t.def["place"] = {{"text", {0, 0}}};
      const json m = measure_item(doc, scene, *sheet, t, frame);
      const Vec2 foot = vec2(m["foot"]), out = vec2(m["out"], {0, 1});
      const Vec2 away = dot(foot, out) < 0 ? mul(out, -1) : out;
      op["place"] = {{"text", js(add(foot, mul(away, kind == "datum" ? 12 : 1)))}};
      t.def = op;
    }
  }
  if (kind == "note" && !refs.empty() && !args.contains("at")) {
    t.def["at"] = {0, 0};
    const json m = measure_item(doc, scene, *sheet, t, frame);
    op["at"] = js(add(vec2(m["tip"]), {10, 8}));
    t.def = op;
  }
  if (kind == "fcf" && !op.contains("place")) {
    t.def["place"] = {{"text", {0, 0}}};
    const json m = measure_item(doc, scene, *sheet, t, frame);
    op["place"] = {{"text", js(add(vec2(m["tip"]), {10, 10}))}};
    t.def = op;
  }
  const json m = measure_item(doc, scene, *sheet, t, frame);
  if (!op.contains("place")) {
    if (kind == "dimension") {
      const Vec2 a = vec2(m["anchor"]);
      const std::string type = op["type"];
      op["place"] = {{"text", type == "vertical" ? js(add(a, {8, 0})) : type == "radius" || type == "diameter" ? js(add(a, {8, 8})) : js(add(a, {0, 8}))}};
    } else if (kind == "hole_callout") {
      const Vec2 c = vec2(m["centre"]);
      const double r = m.value("r", 0.0);
      op["place"] = {{"text", js(add(c, {0.72 * r + 8, 0.72 * r + 8}))}};
    } else if (kind == "dimension_set") {  // below the view (left of it for upright values), past the datum symbols
      const Vec2 o = vec2(m["origin"]);
      op["place"] = {{"text", op["axis"] == "vertical" ? js({frame->box[0] - frame->at[0] - 20, o[1]}) : js({o[0], frame->box[1] - frame->at[1] - 20})}};
    }
  }
  if (const json r = item_result(op, m); !r.is_null()) op["result"] = r;
  if (measured_out) *measured_out = m;
  return op;
}

json plan_dimension(const Document& doc, const Scene& scene, const json& args) {
  const std::string sheetId = args.at("sheet").get<std::string>();
  const Sheet* sheet = sheet_of(scene, sheetId);
  if (!sheet) throw Error("sheet " + sheetId + " does not exist");
  const SheetView* view = scene.sheet_view(args.value("view", ""));
  if (!view || view->sheet != sheet->id) throw Error("a dimension needs a view on its sheet");
  const auto frames = layout(doc, scene, *sheet);
  const ViewFrame& frame = frame_of(frames, view->id);
  json picks = json::array(), refs = json::array();
  std::vector<std::string> whats;
  if (args.contains("picks")) {
    for (const auto& p : args["picks"]) {
      const json r = pick_reference(doc, scene, frame, p);
      picks.push_back(r);
      refs.push_back(r["ref"]);
      whats.push_back(r["what"]);
    }
  } else {
    refs = references(doc, scene, args.at("refs"), args.value("aspects", json()));
    const Resolver R(doc, scene);
    for (const auto& r : refs) {
      const Pick k = pick_of(R, r);
      whats.push_back(k.point ? "vertex" : k.cylinder ? "cylinder" : k.line ? "line" : k.circle ? (k.full ? "circle" : "arc") : k.edge ? "edge" : "face");
    }
  }
  std::vector<std::string> types;
  const auto pointish = [](const std::string& w) { return w == "vertex" || w == "midpoint" || w == "center"; };
  if (whats.size() == 1) {
    if (whats[0] == "circle" || whats[0] == "cylinder") types = {"diameter"};
    else if (whats[0] == "arc") types = {"radius", "diameter"};
    else if (whats[0] == "line" || whats[0] == "edge") types = {"horizontal", "vertical", "aligned"};
    else throw Error("pick a second point to dimension to");
  } else if (whats.size() == 2) {
    if (whats[0] == "line" && whats[1] == "line") types = {"angle", "aligned", "horizontal", "vertical"};
    else if (pointish(whats[0]) || pointish(whats[1]) || whats[0] == "circle" || whats[1] == "circle" || whats[0] == "line" || whats[1] == "line")
      types = {"horizontal", "vertical", "aligned"};
    else throw Error("these two cannot be dimensioned to each other");
  } else {
    throw Error("a dimension takes one or two picks");
  }
  json choices = json::array();
  std::string first;
  for (const auto& type : types) {
    json a = args;
    a.erase("picks");
    a["refs"] = refs;
    a.erase("aspects");
    a["kind"] = "dimension";
    a["type"] = type;
    try {
      json m;
      json op = plan_item(doc, scene, a, &m);
      if (type == "angle" && m.value("geometry", json::object()).contains("lines") == false) continue;
      choices.push_back({{"type", type}, {"op", op}, {"measured", m}});
    } catch (const std::exception& e) {
      if (first.empty()) first = e.what();
    }
  }
  // Two parallel lines have no angle: the angle reading fails and the distance readings stay.
  if (choices.empty()) throw Error(first.empty() ? "nothing to dimension" : first);
  return {{"picks", picks}, {"choices", choices}};
}

json datum_dimensions(const Document& doc, const Scene& scene, const json& args) {
  const std::string sheetId = args.at("sheet").get<std::string>();
  const Sheet* sheet = sheet_of(scene, sheetId);
  if (!sheet) throw Error("sheet " + sheetId + " does not exist");
  const std::string viewId = args.at("view").get<std::string>();
  const SheetView* view = scene.sheet_view(viewId);
  if (!view || view->sheet != sheet->id) throw Error("the view is not on this sheet");
  const std::string type = args.value("type", "ordinate");
  if (type != "ordinate" && type != "baseline" && type != "chain") throw Error("type is ordinate, baseline or chain");
  const auto frames = layout(doc, scene, *sheet);
  const ViewFrame& frame = frame_of(frames, viewId);
  const Resolver R(doc, scene);
  const Paper paper{frame};
  // The view's datums: an upright edge measures across (horizontal values), a level one up and down.
  std::vector<std::string> wanted;
  for (const auto& l : args.value("datums", json::array())) wanted.push_back(upper(l.get<std::string>()));
  json across, upright;  // the reference of the datum that is an upright line, and of the level one
  std::string acrossLetter, uprightLetter;
  std::set<std::string> parts;  // the bodies the datums stand on
  for (const auto& id : sheet->items) {
    const SheetItem* t = scene.sheet_item(id);
    if (!t || t->kind != "datum" || t->view != viewId || !t->error.empty()) continue;
    const std::string letter = t->def.value("letter", "");
    if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), letter) == wanted.end()) continue;
    const json ref = t->def["refs"][0];
    Pick k;
    try {
      k = pick_of(R, ref);
    } catch (const std::exception&) {
      continue;
    }
    if (!k.line) continue;
    const Vec2 u = unit(sub(paper(k.b), paper(k.a)));
    json r = ref;
    r["aspect"] = "mid";
    if (std::fabs(u[0]) < 1e-6 && across.is_null()) across = r, acrossLetter = letter, parts.insert(k.node);
    else if (std::fabs(u[1]) < 1e-6 && upright.is_null()) upright = r, uprightLetter = letter, parts.insert(k.node);
  }
  if (across.is_null() && upright.is_null()) throw Error("the view has no datum on an upright or level straight edge: place datum symbols first");
  // The features: given, else every circle of the view seen along its axis on the part the datums stand on (its holes and
  // bosses).
  json features = json::array();
  if (args.contains("refs")) {
    features = references(doc, scene, args["refs"], args.value("aspects", json()));
  } else {
    const auto g = project(doc, scene, view_spec(scene, *view));
    std::vector<std::pair<Vec2, json>> found;
    for (const auto& c : g->curves) {
      if (c.type != Curve::Type::Arc || c.a1 - c.a0 < 2 * M_PI - 1e-6 || c.edge < 0 || c.body < 0) continue;
      if (!parts.empty() && !parts.count(g->bodies[size_t(c.body)].node)) continue;
      const Vec2 at = c.c;
      if (std::any_of(found.begin(), found.end(), [&](const auto& f) { return len(sub(f.first, at)) < 1e-6; })) continue;  // concentric: one
      json r = design::make_ref(doc, scene, Ref{g->bodies[size_t(c.body)].node, Ref::Kind::Edge, c.edge});
      r["aspect"] = "center";
      found.push_back({at, r});
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first[0] < b.first[0] - 1e-9 || (std::fabs(a.first[0] - b.first[0]) <= 1e-9 && a.first[1] > b.first[1]); });
    for (auto& f : found) features.push_back(f.second);
  }
  if (features.empty()) throw Error("no features to dimension: the view shows no circles along their axes; pick features");
  json ops = json::array();
  for (const auto& [origin, axis, letter] : {std::tuple{across, "horizontal", acrossLetter}, std::tuple{upright, "vertical", uprightLetter}}) {
    if (origin.is_null()) continue;
    json refs = json::array({origin});
    for (const auto& f : features) refs.push_back(f);
    json a = {{"sheet", sheet->id}, {"view", viewId}, {"kind", "dimension_set"}, {"type", type}, {"axis", axis}, {"refs", refs}};
    if (args.contains("precision")) a["precision"] = args["precision"];
    json op = plan_item(doc, scene, a);
    op["datum"] = letter;
    ops.push_back(op);
  }
  return {{"ops", ops}};
}

std::vector<std::array<Vec2, 2>> cylinder_axes(const Document& doc, const Scene& scene, const ViewFrame& f, const ViewSpec& spec) {
  struct Axis {
    Vec3 p, d;
    double r, lo, hi, turn;
  };
  std::vector<std::array<Vec2, 2>> out;
  for (const auto& [node, world] : view_bodies(scene, spec)) {
    TopoDS_Shape shape;
    try {
      shape = node_world_shape(doc, scene, node);
    } catch (const std::exception&) {
      continue;
    }
    const Mat4 placed = scene.world(node);
    const Vec3 shift{world.at(0, 3) - placed.at(0, 3), world.at(1, 3) - placed.at(1, 3), world.at(2, 3) - placed.at(2, 3)};
    std::vector<Axis> axes;
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face face = TopoDS::Face(e.Current());
      try {
        const BRepAdaptor_Surface s(face);
        if (s.GetType() != GeomAbs_Cylinder) continue;
        const gp_Cylinder c = s.Cylinder();
        const Vec3 dir = of(c.Axis().Direction());
        if (std::fabs(dot3(dir, f.dir)) > 1e-4) continue;
        double u0, u1, v0, v1;
        BRepTools::UVBounds(face, u0, u1, v0, v1);
        const Vec3 loc = plus3(of(c.Location()), shift);
        bool merged = false;
        for (auto& a : axes) {  // the same cylinder in pieces
          const Vec3 off = minus3(loc, a.p);
          const double t = dot3(off, a.d);
          if (std::fabs(std::fabs(dot3(dir, a.d)) - 1) > 1e-9 || std::fabs(c.Radius() - a.r) > 1e-6 || std::sqrt(std::max(0.0, dot3(off, off) - t * t)) > 1e-6) continue;
          const double sign = dot3(dir, a.d) > 0 ? 1 : -1, b0 = t + sign * v0, b1 = t + sign * v1;
          if (std::max(b0, b1) < a.lo - 1e-6 || std::min(b0, b1) > a.hi + 1e-6) continue;
          a.lo = std::min({a.lo, b0, b1}), a.hi = std::max({a.hi, b0, b1}), a.turn += u1 - u0;
          merged = true;
          break;
        }
        if (!merged) axes.push_back({loc, dir, c.Radius(), std::min(v0, v1), std::max(v0, v1), u1 - u0});
      } catch (const Standard_Failure&) {
      }
    }
    for (const auto& a : axes)
      if (a.turn >= 2 * M_PI - 1e-6) out.push_back({f.paper(plus3(a.p, scaled(a.d, a.lo))), f.paper(plus3(a.p, scaled(a.d, a.hi)))});
  }
  // Collinear pieces that touch (a counterbore and its hole) make one line.
  for (bool joined = true; joined;) {
    joined = false;
    for (size_t i = 0; i < out.size() && !joined; ++i)
      for (size_t j = i + 1; j < out.size() && !joined; ++j) {
        const Vec2 u = unit(sub(out[i][1], out[i][0]));
        const Vec2 n = left(u);
        if (std::fabs(dot(sub(out[j][0], out[i][0]), n)) > 1e-6 || std::fabs(dot(sub(out[j][1], out[i][0]), n)) > 1e-6) continue;
        double lo = 0, hi = len(sub(out[i][1], out[i][0])), b0 = dot(sub(out[j][0], out[i][0]), u), b1 = dot(sub(out[j][1], out[i][0]), u);
        if (std::max(b0, b1) < lo - 1e-6 || std::min(b0, b1) > hi + 1e-6) continue;
        lo = std::min({lo, b0, b1}), hi = std::max({hi, b0, b1});
        out[i] = {add(out[i][0], mul(u, lo)), add(out[i][0], mul(u, hi))};
        out.erase(out.begin() + static_cast<long>(j));
        joined = true;
      }
  }
  return out;
}

// Centre marks and centre lines a view draws itself (style centermarks): a mark on every whole circle seen along its axis
// (one for concentric ones, the widest), a centre line between the two outlines of every cylinder seen from the side.
void view_centre_marks(Display& d, const ViewFrame& f, const ViewGeometry& g, const DimStyle& s, const std::vector<std::array<Vec2, 2>>* axes) {
  const int center = d.layer({"Center", kInk, LineType::Continuous, 0.25});
  const auto place = [&](Vec2 p) { return Vec2{f.at[0] + f.scale * (p[0] - f.centre[0]), f.at[1] + f.scale * (p[1] - f.centre[1])}; };
  std::vector<std::pair<Vec2, double>> marks;
  std::map<std::pair<int, int>, std::vector<const Curve*>> outlines;
  for (const auto& c : g.curves) {
    if (c.type == Curve::Type::Arc && c.a1 - c.a0 >= 2 * M_PI - 1e-6 && c.r1 * f.scale > 0.3) {
      const Vec2 at = place(c.c);
      auto it = std::find_if(marks.begin(), marks.end(), [&](const auto& m) { return len(sub(m.first, at)) < 1e-6; });
      if (it == marks.end()) marks.push_back({at, c.r1 * f.scale});
      else it->second = std::max(it->second, c.r1 * f.scale);
    } else if (c.type == Curve::Type::Line && c.kind == Curve::Kind::Silhouette && c.face >= 0 && c.pts.size() == 2) {
      outlines[{c.body, c.face}].push_back(&c);
    }
  }
  for (const auto& [at, r] : marks) centre_mark(d, center, at, r, 2 * s.scale, {1, 0}, s);
  if (axes) {
    for (const auto& [a, b] : *axes) {
      const Vec2 u = unit(sub(b, a));
      centre_line(d, center, sub(a, mul(u, 2 * s.scale)), add(b, mul(u, 2 * s.scale)), s);
    }
    return;
  }
  for (const auto& [key, lines] : outlines) {
    // The two farthest-apart parallel outlines of the face: the middle line between them, as long as both together.
    const Curve *a = nullptr, *b = nullptr;
    double best = 0;
    for (size_t i = 0; i < lines.size(); ++i)
      for (size_t j = i + 1; j < lines.size(); ++j) {
        const Vec2 u = unit(sub(lines[i]->pts[1], lines[i]->pts[0])), w = unit(sub(lines[j]->pts[1], lines[j]->pts[0]));
        if (std::fabs(u[0] * w[1] - u[1] * w[0]) > 1e-6) continue;
        const double apart = std::fabs(dot(sub(lines[j]->pts[0], lines[i]->pts[0]), left(u)));
        if (apart > best) best = apart, a = lines[i], b = lines[j];
      }
    if (!a || best * f.scale < 0.3) continue;
    const Vec2 a0 = place(a->pts[0]), a1 = place(a->pts[1]), b0 = place(b->pts[0]);
    const Vec2 u = unit(sub(a1, a0)), n = left(u);
    const Vec2 base = add(a0, mul(n, dot(sub(b0, a0), n) / 2));
    double lo = 1e300, hi = -1e300;
    for (const Vec2 p : {a0, a1, b0, place(b->pts[1])}) lo = std::min(lo, dot(sub(p, a0), u)), hi = std::max(hi, dot(sub(p, a0), u));
    centre_line(d, center, add(base, mul(u, lo - 2 * s.scale)), add(base, mul(u, hi + 2 * s.scale)), s);
  }
}

}  // namespace opad::drawing
