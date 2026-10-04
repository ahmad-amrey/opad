#include "opad/design/sketch.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <functional>
#include <string_view>

namespace opad::design {

namespace {
bool record_list(const std::string& key) { return key == "points" || key == "entities" || key == "constraints" || key == "patterns" || key == "images"; }

bool same_picture(const json& a, const json& b) {
  const auto x = a.find("data"), y = b.find("data");
  return (x == a.end()) == (y == b.end()) && (x == a.end() || *x == *y);
}
// An image whose bytes stayed: only the fields that changed, a field gone as null (UI-71: every nudge appended the whole
// picture again, +1.9 MB per move of a 1.4 MB PNG).
json image_fields(const json& was, const json& now) {
  json out = {{"id", now.at("id")}};
  for (const auto& [k, v] : now.items())
    if (k != "id" && (!was.contains(k) || was.at(k) != v)) out[k] = v;
  for (const auto& [k, v] : was.items())
    if (!now.contains(k)) out[k] = nullptr;
  return out;
}
}  // namespace

json sketch_delta(const json& before, const json& after) {
  json delta = json::object(), fields = json::array();
  for (const char* key : {"points", "entities", "constraints", "patterns", "images"}) {
    std::map<int, const json*> old, now;  // read in place: a picture is megabytes
    if (const auto it = before.find(key); it != before.end() && it->is_array()) for (const auto& v : *it) old[v.at("id").get<int>()] = &v;
    if (const auto it = after.find(key); it != after.end() && it->is_array()) for (const auto& v : *it) now[v.at("id").get<int>()] = &v;
    json changes = json::array();
    for (const auto& [id, v] : old) if (!now.count(id)) changes.push_back({{"id", id}, {"deleted", true}});
    for (const auto& [id, v] : now) {
      const auto was = old.find(id);
      if (was != old.end() && *was->second == *v) continue;
      if (was != old.end() && std::string(key) == "images" && same_picture(*was->second, *v)) fields.push_back(image_fields(*was->second, *v));
      else changes.push_back(*v);
    }
    std::sort(changes.begin(), changes.end(), [](const json& a, const json& b) { return a.at("id").get<int>() < b.at("id").get<int>(); });
    if (!changes.empty()) delta[key] = changes;
  }
  if (!fields.empty()) delta["image_fields"] = fields;
  for (const auto& [key, value] : after.items())
    if (!record_list(key) && key != "image_fields" && (!before.contains(key) || before.at(key) != value)) delta[key] = value;
  // The joins kept apart for older builds (Sketch::to_json) all gone: an empty list, never null. An older build's edit of
  // a sketch this build regenerated says null for them (its to_json does not write the key it skips), which must not drop
  // them (apply_sketch_delta).
  for (const auto& [key, value] : before.items())
    if (!record_list(key) && key != "image_fields" && !after.contains(key)) delta[key] = key == "more_constraints" ? json::array() : json(nullptr);
  return delta;
}

json apply_sketch_delta(const json& before, const json& delta) {
  if (!delta.is_object()) throw Error("sketch edit: delta must be an object");
  json out = before;
  out.erase("image_fields");  // what an older build kept of field changes it could not apply
  for (const auto& [key, changes] : delta.items()) {
    if (key == "image_fields") continue;
    if (!record_list(key)) {
      if (key == "more_constraints" && changes.is_null()) continue;  // an older build's edit: it never saw them (sketch_delta)
      if (changes.is_null() || (key == "more_constraints" && changes.is_array() && changes.empty())) out.erase(key); else out[key] = changes;
      continue;
    }
    if (!changes.is_array()) throw Error("sketch edit: records must be an array");
    std::map<int, json> records;
    for (const auto& v : out.value(key, json::array())) records[v.at("id").get<int>()] = v;
    std::set<int> seen;
    for (const auto& v : changes) {
      const int id = v.at("id").get<int>();
      if (id < 1 || !seen.insert(id).second) throw Error("sketch edit: invalid or duplicate record ID");
      if (v.value("deleted", false)) records.erase(id); else records[id] = v;
    }
    out[key] = json::array();
    for (const auto& [id, v] : records) out[key].push_back(v);
  }
  if (const auto f = delta.find("image_fields"); f != delta.end()) {
    if (!f->is_array()) throw Error("sketch edit: image fields must be an array");
    std::set<int> seen;
    for (const auto& v : *f) {
      const int id = v.at("id").get<int>();
      json* image = nullptr;
      if (out.contains("images") && out["images"].is_array())
        for (auto& i : out["images"]) if (i.at("id").get<int>() == id) image = &i;
      if (!image || !seen.insert(id).second) throw Error("sketch edit: image " + std::to_string(id) + " does not exist or is changed twice");
      for (const auto& [k, field] : v.items())
        if (k == "id") continue;
        else if (field.is_null()) image->erase(k);
        else (*image)[k] = field;
    }
  }
  Sketch::from_json(out); // reject dangling references atomically
  return out;
}

json solved_geometry(const json& data) {
  static const json none = json::object();
  const json& given = data.contains("geometry") ? data["geometry"] : none;
  if (!data.contains("result") || !data["result"].contains("geometry")) return given;
  json solved = data["result"]["geometry"];
  if (!solved.contains("images") && given.contains("images")) solved["images"] = given["images"];
  // A regeneration never changes the constraints, and this build's results repeat "more_constraints" (Sketch::to_json): a
  // result without them is an older build's, which could not read them. They still hold (from_json drops what does not).
  if (!solved.contains("more_constraints") && given.contains("more_constraints")) solved["more_constraints"] = given["more_constraints"];
  return solved;
}

std::string geometry_stamp(const json& geometry) {
  if (!geometry.is_object()) return geometry.dump();
  std::string out;
  for (const auto& [key, value] : geometry.items()) {
    out += key + ':';
    if (key != "images" || !value.is_array()) {
      out += value.dump() + ',';
      continue;
    }
    for (const auto& image : value) {
      if (!image.is_object()) {
        out += image.dump() + ',';
        continue;
      }
      out += '{';
      for (const auto& [field, v] : image.items()) {
        out += field + ':';
        if (field != "data" || !v.is_string()) {
          out += v.dump() + ',';
          continue;
        }
        const std::string_view bytes(v.get_ref<const std::string&>());  // its length, its first, middle and last 4 KB
        const size_t edge = std::min<size_t>(bytes.size(), 4096);
        out += std::to_string(bytes.size());
        for (const size_t at : {size_t(0), (bytes.size() - edge) / 2, bytes.size() - edge}) out += '/' + std::to_string(std::hash<std::string_view>{}(bytes.substr(at, edge)));
        out += ',';
      }
      out += "},";
    }
  }
  return out;
}

double dimension_value(const Sketch& sk, const SkConstraint& c) {
  using T = SkConstraint::Type;
  auto point = [&](int id) -> const SkPoint& {
    if (const auto* p = sk.point(id)) return *p;
    const auto* e = sk.entity(id);
    if (e && e->type == SkEntity::Type::Point) return *sk.point(e->p[0]);
    throw Error("dimension: expected a point");
  };
  auto dist = [](const SkPoint& a, const SkPoint& b) { return std::hypot(b.x-a.x,b.y-a.y); };
  auto line_dist = [&](const SkPoint& p, const SkEntity& e) {
    const auto& a = point(e.p[0]); const auto& b = point(e.p[1]);
    return std::fabs((b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x))/std::max(1e-12, dist(a,b));
  };
  if ((c.type == T::HDistance || c.type == T::VDistance) && c.refs.size() == 1 && sk.point(c.refs[0])) {  // a coordinate
    const auto& a = point(c.refs[0]);
    return c.type == T::HDistance ? a.x : a.y;
  }
  const auto* e = sk.entity(c.refs.at(0));
  if (c.type == T::Radius || c.type == T::Diameter || c.type == T::ArcLength) {
    const double r = e->type == SkEntity::Type::Arc ? dist(point(e->p[0]),point(e->p[1])) : e->r;
    if (c.type == T::Radius) return r;
    if (c.type == T::Diameter) return 2*r;
    const auto& o = point(e->p[0]); const auto& a = point(e->p[1]); const auto& b = point(e->p[2]);
    double t = std::atan2((a.x-o.x)*(b.y-o.y)-(a.y-o.y)*(b.x-o.x),(a.x-o.x)*(b.x-o.x)+(a.y-o.y)*(b.y-o.y));
    if (t < 0) t += 2*M_PI;
    return r*t;
  }
  if (c.type == T::Angle) {
    const auto* f = sk.entity(c.refs.at(1));
    const auto& a = point(e->p[0]); const auto& b = point(e->p[1]);
    const auto& p = point(f->p[0]); const auto& q = point(f->p[1]);
    return std::atan2(std::fabs((b.x-a.x)*(q.y-p.y)-(b.y-a.y)*(q.x-p.x)),(b.x-a.x)*(q.x-p.x)+(b.y-a.y)*(q.y-p.y));
  }
  if (c.refs.size() == 1) return dist(point(e->p[0]),point(e->p[1]));
  const auto* f = sk.entity(c.refs.at(1));
  if (f && f->type == SkEntity::Type::Line) return line_dist(e && e->type == SkEntity::Type::Line ? point(e->p[0]) : point(c.refs[0]), *f);
  const auto& a = point(c.refs[0]); const auto& b = point(c.refs[1]);
  if (c.type == T::HDistance) return c.is_signed ? b.x-a.x : std::fabs(a.x-b.x);
  if (c.type == T::VDistance) return c.is_signed ? b.y-a.y : std::fabs(a.y-b.y);
  return dist(a,b);
}

ParamTable sketch_parameters(const Sketch& sk, const ParamTable& params) {
  auto defs = params.defs();
  for (const auto& c : sk.constraints) if (c.is_dimension()) {
    const std::string name = "d" + std::to_string(c.id);
    if (params.find(name)) throw Error("sketch dimension name conflicts with parameter " + name);
    const std::string expr = c.reference || c.expr.empty()
        ? json(c.reference ? dimension_value(sk,c) : c.value).dump() + (c.type == SkConstraint::Type::Angle ? " rad" : " mm")
        : c.expr;
    defs.push_back({name, name, expr, {}});
  }
  return ParamTable(std::move(defs),params.unit());
}

void evaluate_dimensions(Sketch& sk, const ParamTable& params) {
  const auto table = sketch_parameters(sk,params);
  std::vector<double> values;
  // Only a sketch with reference dimensions needs the walk through every expression they could reach (gap log #3:
  // it re-read hundreds of parameter expressions per dimension on the arm's 1,282-dimension discs).
  const bool references = std::any_of(sk.constraints.begin(), sk.constraints.end(), [](const SkConstraint& c) { return c.reference; });
  for (const auto& c : sk.constraints) {
    if (!c.is_dimension() || c.reference || c.expr.empty()) { values.push_back(c.value); continue; }
    if (!references) { values.push_back(table.as(c.type == SkConstraint::Type::Angle ? Dim::Angle : Dim::Length, c.expr)); continue; }
    std::set<std::string> visited;
    std::function<void(const std::string&)> check=[&](const std::string& expr) {
      for(const auto& name:expr_identifiers(expr))if(visited.insert(name).second) {
        for(const auto& ref:sk.constraints)if(ref.reference && name=="d"+std::to_string(ref.id))throw Error("reference dimensions cannot drive geometry");
        if(const auto* def=table.find(name))check(def->expr);
      }
    };
    check(c.expr);
    values.push_back(table.as(c.type == SkConstraint::Type::Angle ? Dim::Angle : Dim::Length, c.expr));
  }
  for (size_t i=0; i<values.size(); ++i) sk.constraints[i].value=values[i];
}

} // namespace opad::design
