// High-level shapes in sketch geometry (TODO 10 B4), expanded into points, entities and constraints.
#include "opad/design/sketch_shapes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "opad/design/sketch_create.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch_modify.hpp"
#include "opad/design/sketch_text.hpp"

namespace opad::design {
namespace {

// The size of what a list of entities spans (curves by their centres and radii): enough to tell an offset that
// grew a closed chain from one that shrank it.
double span(const Sketch& sk, const std::vector<int>& ids) {
  double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
  for (int id : ids) {
    const SkEntity* e = sk.entity(id);
    if (!e) continue;
    for (int p : e->p)
      if (const SkPoint* q = sk.point(p)) {
        const double r = e->type == SkEntity::Type::Circle || e->type == SkEntity::Type::Arc ? (e->type == SkEntity::Type::Circle ? e->r : 0) : 0;
        x0 = std::min(x0, q->x - r), y0 = std::min(y0, q->y - r), x1 = std::max(x1, q->x + r), y1 = std::max(y1, q->y + r);
      }
  }
  return x1 >= x0 ? std::hypot(x1 - x0, y1 - y0) : 0;
}

void offset_shape(Sketch& sk, const json& options, const json& earlier) {
  std::vector<int> seeds;
  if (options.contains("shape")) {  // an earlier shape of the same list: its curves
    const int index = options["shape"].get<int>();
    if (index < 0 || index >= static_cast<int>(earlier.size())) throw Error("options.shape must name an earlier shape, 0 to " + std::to_string(static_cast<int>(earlier.size()) - 1));
    for (const auto& id : earlier[static_cast<size_t>(index)]["entities"])
      if (const SkEntity* e = sk.entity(id.get<int>()); e && !e->construction) seeds.push_back(e->id);
  }
  if (options.contains("entity")) seeds.push_back(options.at("entity").get<int>());
  for (const auto& id : options.value("entities", json::array())) seeds.push_back(id.get<int>());
  if (seeds.empty()) throw Error("an offset needs options.shape (an earlier shape's index), options.entity (one curve of the chain) or options.entities");
  for (int id : seeds)
    if (!sk.entity(id)) throw Error("there is no entity " + std::to_string(id) + " to offset");
  const double distance = options.value("distance", 0.0);
  if (!std::isfinite(distance) || std::fabs(distance) < 1e-9) throw Error("an offset needs a nonzero options.distance (+ outward, - inward)");
  const bool round = options.value("round", true);
  const std::vector<int> chain = connected_entities(sk, seeds);
  // The kernel's side depends on the chain's direction: offset, and if it went the other way, offset the other way.
  auto attempt = [&](double d) {
    Sketch trial = sk;
    std::set<int> before;
    for (const auto& e : trial.entities) before.insert(e.id);
    offset_entities(trial, chain, d, round);
    std::vector<int> made;
    for (const auto& e : trial.entities)
      if (!before.count(e.id)) made.push_back(e.id);
    return std::make_pair(trial, span(trial, made) > span(sk, chain));
  };
  auto [result, grew] = attempt(distance);
  if (grew != (distance > 0)) result = attempt(-distance).first;
  sk = std::move(result);
}

std::pair<double, double> pick(const json& p, size_t shape, size_t i) {
  if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
    throw Error("shapes[" + std::to_string(shape) + "].picks[" + std::to_string(i) + "] must be [u, v] in sketch mm");
  return {p[0].get<double>(), p[1].get<double>()};
}

}  // namespace

const std::vector<std::string>& sketch_shape_kinds() {
  static const std::vector<std::string> kinds = {"point", "line", "circle", "rect2", "rect_center", "rect3", "rounded_rect", "arc3", "arc_radius",
                                                 "tangent_arc", "slot", "cslot", "arcslot", "path", "polygon_outer", "circle2", "conic",
                                                 "control_spline", "tangent_circle", "offset", "text"};
  return kinds;
}

json expand_sketch_shapes(const json& geometry_in, json* id_map) {
  json geometry = geometry_in.is_object() ? geometry_in : json::object();
  const json shapes = geometry.value("shapes", json::array());
  geometry.erase("shapes");
  // Ids the caller left out take the next free ones, points first.
  int top = geometry.value("id_watermark", 0);
  for (const char* list : {"points", "entities", "constraints", "more_constraints", "images", "patterns"})
    for (const auto& item : geometry.value(list, json::array()))
      if (item.contains("id") && item["id"].is_number_integer()) top = std::max(top, item["id"].get<int>());
  for (const char* list : {"points", "entities", "constraints"})
    if (geometry.contains(list))
      for (auto& item : geometry[list])
        if (item.is_object() && !item.contains("id")) item["id"] = ++top;
  Sketch sk = Sketch::from_json(geometry);
  sk.validate();
  json map = json::array();
  for (size_t i = 0; i < shapes.size(); ++i) {
    const json& shape = shapes[i];
    const std::string kind = shape.value("kind", "");
    const std::string where = "shapes[" + std::to_string(i) + "] (" + (kind.empty() ? "no kind" : kind) + ")";
    try {
      const auto& kinds = sketch_shape_kinds();
      if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) {
        std::string list;
        for (const auto& k : kinds) list += (list.empty() ? "" : ", ") + k;
        throw Error("unknown kind; the kinds are " + list);
      }
      if (shape.contains("first_id")) {
        const int first = shape["first_id"].get<int>();
        const int next = sk.next_id();
        if (first < next) throw Error("first_id " + std::to_string(first) + " is already taken; the next free id is " + std::to_string(next));
        sk.id_watermark = first - 1;
      }
      std::set<int> points, entities, constraints;
      for (const auto& p : sk.points) points.insert(p.id);
      for (const auto& e : sk.entities) entities.insert(e.id);
      for (const auto& c : sk.constraints) constraints.insert(c.id);
      const json options = shape.value("options", json::object());
      if (kind == "offset") {
        offset_shape(sk, options, map);
      } else {
        std::vector<std::pair<double, double>> picks;
        const json list = shape.value("picks", json::array());
        for (size_t k = 0; k < list.size(); ++k) picks.push_back(pick(list[k], i, k));
        create_primitive(sk, kind, picks, options);
      }
      json entry = {{"shape", i}, {"kind", kind}, {"points", json::array()}, {"entities", json::array()}, {"constraints", json::array()}};
      for (const auto& p : sk.points)
        if (!points.count(p.id)) entry["points"].push_back(p.id);
      for (const auto& e : sk.entities)
        if (!entities.count(e.id)) entry["entities"].push_back(e.id);
      for (const auto& c : sk.constraints)
        if (!constraints.count(c.id)) entry["constraints"].push_back(c.id);
      // Outline text: one point inside each letter, to extrude them as profiles ({"sketch": id, "at": point}).
      if (kind == "text" && options.value("style", "outline") == "outline") {
        Sketch letters;
        for (const auto& id : entry["entities"]) {
          const SkEntity& e = *sk.entity(id.get<int>());
          for (int p : e.p)
            if (!letters.point(p)) letters.points.push_back(*sk.point(p));
          letters.entities.push_back(e);
        }
        const auto regions = sketch_regions(letters, Frame{});
        TextOptions t;
        t.height = options.value("height", 10.0);
        t.align = options.value("align", "left");
        const auto origin = pick(shape.value("picks", json::array()).at(0), i, 0);
        std::set<int> seen;
        entry["profiles"] = json::array();
        for (const auto& [x, y] : text_stroke_points(options.value("text", std::string()), origin.first, origin.second, t))
          if (const int r = region_at(regions, Frame{}, x, y); r >= 0 && seen.insert(r).second) entry["profiles"].push_back({{"at", {x, y}}});
      }
      map.push_back(entry);
    } catch (const std::exception& e) {
      throw Error(where + ": " + e.what());
    }
  }
  if (id_map) *id_map = map;
  return sk.to_json();
}

}  // namespace opad::design
