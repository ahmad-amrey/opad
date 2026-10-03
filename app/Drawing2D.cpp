#include "Drawing2D.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepGProp.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>

namespace drawing2d {
double luminance(const Rgb& c) {
  auto linear = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
  return 0.2126 * linear(c[0]) + 0.7152 * linear(c[1]) + 0.0722 * linear(c[2]);
}

double contrast(const Rgb& a, const Rgb& b) {
  const double la = luminance(a), lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

Rgb ink(const Rgb& background) { return contrast(kInkOnDark, background) >= contrast(kInkOnLight, background) ? kInkOnDark : kInkOnLight; }

// ---------------------------------------------------------------- layers
namespace {
using opad::json;
const json& fields(const opad::Node& n) {
  static const json none = json::object();
  return n.layer.is_object() ? n.layer : none;
}
std::string upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}
json nullIf(bool empty, json value) { return empty ? json(nullptr) : std::move(value); }
std::vector<double> patternOf(const json& fields) {
  std::vector<double> out;
  if (fields.contains("pattern") && fields["pattern"].is_array())
    for (const auto& d : fields["pattern"])
      if (d.is_number()) out.push_back(d.get<double>());
  return out;
}
// The on/frozen pair as fields and the visible it leaves.
void shown(json& op, bool on, bool frozen) {
  op["visible"] = on && !frozen;
  op["layer"]["off"] = nullIf(on, true);
  op["layer"]["frozen"] = nullIf(!frozen, true);
}
}  // namespace

bool isLayer(const opad::Scene& scene, const std::string& id) {
  const opad::Node* n = scene.node(id);
  if (!n || n->kind != opad::Node::Kind::Component) return false;
  return std::any_of(n->children.begin(), n->children.end(), [&](const std::string& child) {
    const opad::Node* c = scene.node(child);
    return c && c->kind == opad::Node::Kind::Body && c->representation == "drawing2d";
  });
}

namespace {
Layer make(const opad::Scene& scene, const opad::Node& n, const std::string& drawing) {
  const json& f = fields(n);
  Layer l;
  l.id = n.id;
  l.name = n.name;
  l.drawing = drawing;
  const bool frozenField = f.value("frozen", false);
  l.frozen = !n.visible && frozenField;
  l.on = n.visible || (l.frozen && !f.value("off", false));
  l.locked = n.locked;
  l.plot = f.value("plot", true);
  if (f.contains("linetype") && f["linetype"].is_string()) l.linetype = f["linetype"].get<std::string>();
  l.pattern = patternOf(f);
  if (f.contains("lineweight") && f["lineweight"].is_number()) l.lineweight = f["lineweight"].get<double>();
  for (const auto& child : n.children)
    if (const opad::Node* b = scene.node(child); b && b->kind == opad::Node::Kind::Body && b->representation == "drawing2d") {
      l.bodies.push_back(child);
      if (b->by_layer) l.byLayer.push_back(child);
    }
  if (l.byLayer.empty()) l.byLayer = l.bodies;
  l.own = int(l.bodies.size() - l.byLayer.size());
  bool first = true, uncolored = false;
  for (const auto& child : l.byLayer) {
    const opad::Node* b = scene.node(child);
    if (!b->has_color) uncolored = true;
    else if (first || b->color == l.color) l.color = b->color, l.colored = true;
    else l.mixed = true;
    first = false;
  }
  if (l.colored && uncolored) l.mixed = true;
  if (n.has_color && !l.mixed) l.color = n.color, l.colored = true;
  return l;
}
}  // namespace

std::vector<Layer> layers(const opad::Scene& scene) {
  std::vector<Layer> out;
  std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& id, const std::string& drawing) {
    const opad::Node* n = scene.node(id);
    if (!n || n->kind != opad::Node::Kind::Component) return;
    if (isLayer(scene, id)) out.push_back(make(scene, *n, drawing));
    for (const auto& child : n->children) walk(child, drawing);
  };
  for (const auto& root : scene.roots)
    if (const opad::Node* n = scene.node(root)) walk(root, n->name);
  return out;
}

const Layer* find(const std::vector<Layer>& all, const std::string& id) {
  const auto it = std::find_if(all.begin(), all.end(), [&](const Layer& l) { return l.id == id; });
  return it == all.end() ? nullptr : &*it;
}

std::string layerOf(const opad::Scene& scene, const std::string& node) {
  for (const opad::Node* n = scene.node(node); n; n = n->parent.empty() ? nullptr : scene.node(n->parent))
    if (isLayer(scene, n->id)) return n->id;
  return {};
}

std::optional<Layer> layerAt(const opad::Scene& scene, const std::string& node) {
  const opad::Node* n = scene.node(layerOf(scene, node));
  if (!n) return std::nullopt;
  const opad::Node* root = n;
  while (!root->parent.empty() && scene.node(root->parent)) root = scene.node(root->parent);
  return make(scene, *n, root->name);
}

json setOn(const Layer& layer, bool on) {
  json op = {{"target", layer.id}};
  shown(op, on, layer.frozen);
  return op;
}

json setFrozen(const Layer& layer, bool frozen) {
  json op = {{"target", layer.id}};
  shown(op, layer.on, frozen);
  return op;
}

json setLocked(const Layer& layer, bool locked) { return {{"target", layer.id}, {"locked", locked}}; }

json setColor(const Layer& layer, const Rgb& color) {
  json targets = json::array({layer.id});
  for (const auto& b : layer.byLayer) targets.push_back(b);
  return {{"targets", targets}, {"color", {color[0], color[1], color[2]}}};
}

json setDefaultColor(const Layer& layer) {
  json targets = json::array({layer.id});
  for (const auto& b : layer.byLayer) targets.push_back(b);
  return {{"targets", targets}, {"default_color", true}};
}

json setLinetype(const Layer& layer, const std::string& linetype, const std::vector<double>& pattern) {
  const bool continuous = linetype.empty() || upper(linetype) == "CONTINUOUS";
  return {{"target", layer.id}, {"visible", layer.on && !layer.frozen},
          {"layer", {{"linetype", nullIf(continuous, linetype)}, {"pattern", nullIf(continuous || pattern.empty(), pattern)}}}};
}

json setLineweight(const Layer& layer, double mm) {
  return {{"target", layer.id}, {"visible", layer.on && !layer.frozen}, {"layer", {{"lineweight", nullIf(mm < 0, std::round(mm * 100) / 100)}}}};
}

json setPlot(const Layer& layer, bool plot) {
  return {{"target", layer.id}, {"visible", layer.on && !layer.frozen}, {"layer", {{"plot", nullIf(plot, false)}}}};
}

const std::vector<std::string>& linetypes() {
  static const std::vector<std::string> list = {"Continuous", "Dashed", "Hidden", "Center", "Phantom", "Dot", "DashDot", "Border", "Divide"};
  return list;
}

const std::vector<double>& lineweights() {
  static const std::vector<double> list = {0.0, 0.05, 0.09, 0.13, 0.15, 0.18, 0.2, 0.25, 0.3, 0.35, 0.4, 0.5, 0.53,
                                           0.6, 0.7, 0.8, 0.9, 1.0, 1.06, 1.2, 1.4, 1.58, 2.0, 2.11};
  return list;
}

std::vector<double> dashes(const std::string& linetype, const std::vector<double>& pattern) {
  std::string name = upper(linetype);
  if (name.empty() || name == "CONTINUOUS" || name == "BYLAYER" || name == "BYBLOCK") return {};
  if (!pattern.empty())  // the file's own: drawn when it has a gap
    return std::any_of(pattern.begin(), pattern.end(), [](double d) { return d < 0; }) ? pattern : std::vector<double>{};
  static const std::map<int, std::vector<double>> iso = {  // acadiso.lin, ACAD_ISOnnW100
      {2, {12, -3}}, {3, {12, -18}}, {4, {24, -3, 0.5, -3}}, {5, {24, -3, 0.5, -3, 0.5, -3}}, {6, {24, -3, 0.5, -3, 0.5, -3, 0.5, -3}}, {7, {0.5, -3}},
      {8, {24, -3, 6, -3}}, {9, {24, -3, 6, -3, 6, -3}}, {10, {12, -3, 0.5, -3}}, {11, {12, -3, 12, -3, 0.5, -3}}, {12, {12, -3, 0.5, -3, 0.5, -3}},
      {13, {12, -3, 12, -3, 0.5, -3, 0.5, -3}}, {14, {12, -3, 0.5, -3, 0.5, -3, 0.5, -3}}, {15, {12, -3, 12, -3, 0.5, -3, 0.5, -3, 0.5, -3}}};
  if (name.rfind("ACAD_ISO", 0) == 0)
    if (const auto it = iso.find(std::atoi(name.c_str() + 8)); it != iso.end()) return it->second;
  static const std::vector<std::pair<std::string, std::vector<double>>> acad = {  // acad.lin in mm, the names a guess looks for in this order
      {"DASHDOT", {12.7, -6.35, 0, -6.35}},  {"CENTER", {31.75, -6.35, 6.35, -6.35}}, {"PHANTOM", {31.75, -6.35, 6.35, -6.35, 6.35, -6.35}},
      {"BORDER", {12.7, -6.35, 12.7, -6.35, 0, -6.35}}, {"DIVIDE", {12.7, -6.35, 0, -6.35, 0, -6.35}}, {"HIDDEN", {6.35, -3.175}},
      {"DASHED", {12.7, -6.35}}, {"DOT", {0, -6.35}}};
  double size = 1;  // DASHED2 half, DASHEDX2 twice
  if (name.size() > 2 && name.compare(name.size() - 2, 2, "X2") == 0) size = 2, name.resize(name.size() - 2);
  else if (name.size() > 1 && name.back() == '2') size = 0.5, name.pop_back();
  auto sized = [size](std::vector<double> d) {
    for (double& v : d) v *= size;
    return d;
  };
  for (const auto& [base, d] : acad)
    if (name == base) return sized(d);
  for (const auto& [base, d] : acad)  // other names, as they read: "CENTERLINE", "HIDDEN_LINE", "DOTTED"
    if (name.find(base) != std::string::npos || (base == "DASHDOT" && name.find("DASH") != std::string::npos && name.find("DOT") != std::string::npos) ||
        (base == "DASHED" && name.find("DASH") != std::string::npos))
      return d;
  return acad[6].second;  // dashed
}

LinePattern linePattern(const std::vector<double>& dashes, double pixelsPerMm) {
  std::vector<std::pair<bool, double>> e;  // on or off, pixels; neighbours of a kind merged, around the end too
  for (double d : dashes) {
    const bool on = d >= 0;
    const double px = std::abs(d) * pixelsPerMm;
    if (!e.empty() && e.back().first == on) e.back().second += px;
    else e.push_back({on, px});
  }
  if (e.size() > 1 && e.front().first == e.back().first) e.front().second += e.back().second, e.pop_back();
  if (e.size() < 2) return {};
  double period = 0;
  for (const auto& x : e) period += x.second;
  // Each dash and gap a whole number of bits (dots one) of f pixels, the period repeated k times in 16 bits: the k and f
  // whose lengths come closest.
  LinePattern best{0xFFC0, 1};  // more dashes than 16 bits hold: OCCT's dash
  double bestError = 1e300;
  for (int k : {1, 2, 4, 8}) {
    const int bits = 16 / k;
    if (bits < int(e.size())) break;
    for (int f = 1; f <= 256 && f <= period + 1; ++f) {
      std::vector<int> b(e.size());
      int sum = 0;
      for (size_t i = 0; i < e.size(); ++i) sum += b[i] = std::max(1, int(std::lround(e[i].second / f)));
      while (sum != bits) {  // shave the one most over its length (keeping one bit), or lengthen the one most short of it
        int at = -1;
        double worst = -1e300;
        for (size_t i = 0; i < e.size(); ++i) {
          const double off = sum > bits ? b[i] * f - e[i].second : e[i].second - b[i] * f;
          if ((sum < bits || b[i] > 1) && off > worst) worst = off, at = int(i);
        }
        if (at < 0) break;
        b[size_t(at)] += sum > bits ? -1 : 1;
        sum += sum > bits ? -1 : 1;
      }
      if (sum != bits) continue;
      double error = 0;
      for (size_t i = 0; i < e.size(); ++i) error += std::abs(b[i] * f - e[i].second);
      if (error >= bestError - 1e-9) continue;
      bestError = error;
      uint16_t pattern = 0;
      int bit = 15;
      for (int r = 0; r < k; ++r)
        for (size_t i = 0; i < e.size(); ++i)
          for (int n = 0; n < b[i]; ++n, --bit)
            if (e[i].first) pattern |= uint16_t(1u << bit);
      best = {pattern, uint16_t(f)};
    }
  }
  return best;
}

double linePoints(double lineweight) { return lineweight < 0 ? 1.0 : std::max(1.0, std::ceil(lineweight * 96 / 25.4 - 0.05)); }

// ---------------------------------------------------------------- layer states
json captureState(const opad::Scene& scene) {
  json all = json::object();
  for (const Layer& l : layers(scene)) {
    json s = {{"name", l.name}, {"on", l.on}, {"frozen", l.frozen}, {"locked", l.locked}, {"plot", l.plot}};
    if (l.colored && !l.mixed) s["color"] = {l.color[0], l.color[1], l.color[2]};
    if (!l.linetype.empty()) s["linetype"] = l.linetype;
    if (!l.pattern.empty()) s["pattern"] = l.pattern;
    if (l.lineweight >= 0) s["lineweight"] = l.lineweight;
    all[l.id] = std::move(s);
  }
  return {{"layers", all}};
}

json entityInfo(const TopoDS_Shape& sub) try {
  if (sub.IsNull()) return json();
  if (sub.ShapeType() == TopAbs_VERTEX) return {{"type", "point"}};
  if (sub.ShapeType() == TopAbs_FACE) {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(sub, props);
    return {{"type", "fill"}, {"area", props.Mass()}};
  }
  if (sub.ShapeType() != TopAbs_EDGE) return json();
  const BRepAdaptor_Curve curve(TopoDS::Edge(sub));
  json info = {{"length", GCPnts_AbscissaPoint::Length(curve)}};
  const bool closed = std::abs(curve.LastParameter() - curve.FirstParameter()) >= 2 * M_PI - 1e-8;
  switch (curve.GetType()) {
    case GeomAbs_Line: info["type"] = "line"; break;
    case GeomAbs_Circle:
      info["type"] = closed ? "circle" : "arc";
      info["radius"] = curve.Circle().Radius();
      break;
    case GeomAbs_Ellipse: info["type"] = "ellipse"; break;
    case GeomAbs_BSplineCurve:
    case GeomAbs_BezierCurve: info["type"] = "spline"; break;
    default: info["type"] = "curve";
  }
  return info;
} catch (const Standard_Failure&) {  // a curve that cannot be measured is still a curve
  return {{"type", sub.ShapeType() == TopAbs_FACE ? "fill" : "curve"}};
}

std::string entityType(const json& inspected) {
  const std::string type = inspected.value("type", ""), curve = inspected.value("curve", "");
  if (type == "vertex" || type == "point") return "point";
  if (type == "face") return "fill";
  if (type != "edge") return {};
  if (curve == "line") return "line";
  if (curve == "ellipse") return "ellipse";
  if (curve == "bspline" || curve == "bezier") return "spline";
  if (curve != "circle") return "curve";
  const json &a = inspected.value("start", json()), &b = inspected.value("end", json());
  if (!a.is_array() || !b.is_array() || a.size() != 3 || b.size() != 3) return "arc";
  double gap = 0;
  for (int i = 0; i < 3; ++i) gap = std::max(gap, std::abs(a[i].get<double>() - b[i].get<double>()));
  return gap <= 1e-7 * std::max(1.0, inspected.value("radius", 1.0)) ? "circle" : "arc";
}

const char* kindWord(opad::Ref::Kind kind) {
  switch (kind) {
    case opad::Ref::Kind::Body: return "group";
    case opad::Ref::Kind::Vertex:
    case opad::Ref::Kind::Point: return "point";
    case opad::Ref::Kind::Face: return "fill";
    case opad::Ref::Kind::Center: return "center";
    default: return "object";
  }
}

const char* nodeWord(const opad::Scene& scene, const std::string& id) {
  const opad::Node* n = scene.node(id);
  if (!n) return "object";
  if (n->kind == opad::Node::Kind::Body) return n->representation == "drawing2d" ? "group" : "object";
  if (isLayer(scene, id)) return "layer";
  return std::any_of(n->children.begin(), n->children.end(), [&](const std::string& child) { return isLayer(scene, child); }) ? "drawing" : "object";
}

json properties(json props) {
  if (!props.is_object()) return props;
  for (const char* solidOnly : {"solid", "volume", "axis", "normal", "origin", "surface", "adjacent_faces"}) props.erase(solidOnly);
  for (const auto& [from, to] : {std::pair{"faces", "fills"}, {"edges", "objects"}, {"vertices", "points"}}) {
    if (!props.contains(from)) continue;
    if (props[from].is_number()) props[to] = props[from];
    props.erase(from);  // a face's bounding edges and an edge's vertices: their ends are start and end
  }
  return props;
}

bool drawingOnly(const opad::Scene& scene) {
  const auto bodies = scene.all_bodies();
  return !bodies.empty() && std::all_of(bodies.begin(), bodies.end(), [&](const std::string& id) { return scene.node(id)->representation == "drawing2d"; });
}

bool hasDrawings(const opad::Scene& scene) {
  return std::any_of(scene.nodes.begin(), scene.nodes.end(), [](const auto& entry) { return entry.second.representation == "drawing2d"; });
}

std::vector<json> restoreState(const opad::Scene& scene, const json& display) {
  std::vector<json> out;
  if (!display.is_object() || !display.contains("layers") || !display["layers"].is_object()) return out;
  const json& saved = display["layers"];
  for (const Layer& l : layers(scene)) {
    const json* s = saved.contains(l.id) ? &saved[l.id] : nullptr;
    if (!s)  // re-imported or another file: by name
      for (const auto& [id, state] : saved.items())
        if (state.is_object() && state.value("name", "") == l.name) { s = &state; break; }
    if (!s || !s->is_object()) continue;
    const bool on = s->value("on", true), frozen = s->value("frozen", false), locked = s->value("locked", false), plot = s->value("plot", true);
    const std::string linetype = s->value("linetype", "");
    const double lineweight = s->contains("lineweight") && (*s)["lineweight"].is_number() ? (*s)["lineweight"].get<double>() : -1;
    json op = {{"target", l.id}}, patch = json::object();
    if (on != l.on || frozen != l.frozen) shown(op, on, frozen);
    if (locked != l.locked) op["locked"] = locked;
    if (plot != l.plot) patch["plot"] = nullIf(plot, false);
    const std::vector<double> pattern = patternOf(*s);
    if (upper(linetype) != upper(l.linetype) || pattern != l.pattern) {
      patch["linetype"] = nullIf(linetype.empty(), linetype);
      patch["pattern"] = nullIf(linetype.empty() || pattern.empty(), pattern);
    }
    if (std::abs(lineweight - l.lineweight) > 1e-9) patch["lineweight"] = nullIf(lineweight < 0, lineweight);
    if (!patch.empty()) {
      for (const auto& [key, value] : patch.items()) op["layer"][key] = value;
      if (!op.contains("visible")) op["visible"] = l.on && !l.frozen;
    }
    if (op.size() > 1) out.push_back(std::move(op));
    const bool colored = s->contains("color") && (*s)["color"].is_array() && (*s)["color"].size() == 3;
    if (colored) {
      const Rgb c{(*s)["color"][0].get<double>(), (*s)["color"][1].get<double>(), (*s)["color"][2].get<double>()};
      if (!l.colored || l.mixed || c != l.color) out.push_back(setColor(l, c));
    } else if (l.colored && !l.mixed) {  // saved in the drawing's own colours: back to them
      out.push_back(setDefaultColor(l));
    }
  }
  return out;
}
}  // namespace drawing2d
