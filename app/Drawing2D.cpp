#include "Drawing2D.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

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

std::vector<Layer> layers(const opad::Scene& scene) {
  std::vector<Layer> out;
  std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& id, const std::string& drawing) {
    const opad::Node* n = scene.node(id);
    if (!n || n->kind != opad::Node::Kind::Component) return;
    if (isLayer(scene, id)) {
      const json& f = fields(*n);
      Layer l;
      l.id = id;
      l.name = n->name;
      l.drawing = drawing;
      const bool frozenField = f.value("frozen", false);
      l.frozen = !n->visible && frozenField;
      l.on = n->visible || (l.frozen && !f.value("off", false));
      l.locked = n->locked;
      l.plot = f.value("plot", true);
      if (f.contains("linetype") && f["linetype"].is_string()) l.linetype = f["linetype"].get<std::string>();
      if (f.contains("lineweight") && f["lineweight"].is_number()) l.lineweight = f["lineweight"].get<double>();
      bool first = true, uncolored = false;
      for (const auto& child : n->children)
        if (const opad::Node* b = scene.node(child); b && b->kind == opad::Node::Kind::Body && b->representation == "drawing2d") {
          l.bodies.push_back(child);
          if (!b->has_color) uncolored = true;
          else if (first || b->color == l.color) l.color = b->color, l.colored = true;
          else l.mixed = true;
          first = false;
        }
      if (l.colored && uncolored) l.mixed = true;
      if (n->has_color && !l.mixed) l.color = n->color, l.colored = true;
      out.push_back(std::move(l));
    }
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
  for (const auto& b : layer.bodies) targets.push_back(b);
  return {{"targets", targets}, {"color", {color[0], color[1], color[2]}}};
}

json setDefaultColor(const Layer& layer) {
  json targets = json::array({layer.id});
  for (const auto& b : layer.bodies) targets.push_back(b);
  return {{"targets", targets}, {"default_color", true}};
}

json setLinetype(const Layer& layer, const std::string& linetype) {
  const bool continuous = linetype.empty() || upper(linetype) == "CONTINUOUS";
  return {{"target", layer.id}, {"visible", layer.on && !layer.frozen}, {"layer", {{"linetype", nullIf(continuous, linetype)}}}};
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

int lineType(const std::string& linetype) {
  const std::string name = upper(linetype);
  if (name.empty() || name == "CONTINUOUS" || name == "BYLAYER" || name == "BYBLOCK") return 0;
  for (const char* dotDash : {"DASHDOT", "CENTER", "PHANTOM", "BORDER", "DIVIDE"})
    if (name.find(dotDash) != std::string::npos) return 3;
  if (name.find("DOT") != std::string::npos) return 2;
  return 1;  // dashed, hidden, and any other named pattern
}

double linePoints(double lineweight) { return lineweight < 0 ? 1.0 : std::max(1.0, std::ceil(lineweight * 96 / 25.4 - 0.05)); }

// ---------------------------------------------------------------- layer states
json captureState(const opad::Scene& scene) {
  json all = json::object();
  for (const Layer& l : layers(scene)) {
    json s = {{"name", l.name}, {"on", l.on}, {"frozen", l.frozen}, {"locked", l.locked}, {"plot", l.plot}};
    if (l.colored && !l.mixed) s["color"] = {l.color[0], l.color[1], l.color[2]};
    if (!l.linetype.empty()) s["linetype"] = l.linetype;
    if (l.lineweight >= 0) s["lineweight"] = l.lineweight;
    all[l.id] = std::move(s);
  }
  return {{"layers", all}};
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
    if (upper(linetype) != upper(l.linetype)) patch["linetype"] = nullIf(linetype.empty(), linetype);
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
