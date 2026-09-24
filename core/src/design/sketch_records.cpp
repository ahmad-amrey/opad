#include "opad/design/sketch.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace opad::design {

json sketch_delta(const json& before, const json& after) {
  json delta = json::object();
  for (const char* key : {"points", "entities", "constraints", "patterns"}) {
    std::map<int, json> old, now;
    for (const auto& v : before.value(key, json::array())) old[v.at("id").get<int>()] = v;
    for (const auto& v : after.value(key, json::array())) now[v.at("id").get<int>()] = v;
    json changes = json::array();
    for (const auto& [id, v] : old) if (!now.count(id)) changes.push_back({{"id", id}, {"deleted", true}});
    for (const auto& [id, v] : now) if (!old.count(id) || old.at(id) != v) changes.push_back(v);
    std::sort(changes.begin(), changes.end(), [](const json& a, const json& b) { return a.at("id").get<int>() < b.at("id").get<int>(); });
    if (!changes.empty()) delta[key] = changes;
  }
  for (const auto& [key, value] : after.items())
    if (key != "points" && key != "entities" && key != "constraints" && key != "patterns" && (!before.contains(key) || before.at(key) != value)) delta[key] = value;
  for (const auto& [key, value] : before.items())
    if (key != "points" && key != "entities" && key != "constraints" && key != "patterns" && !after.contains(key)) delta[key] = nullptr;
  return delta;
}

json apply_sketch_delta(const json& before, const json& delta) {
  if (!delta.is_object()) throw Error("sketch edit: delta must be an object");
  json out = before;
  for (const auto& [key, changes] : delta.items()) {
    if (key != "points" && key != "entities" && key != "constraints" && key != "patterns") {
      if (changes.is_null()) out.erase(key); else out[key] = changes;
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
  Sketch::from_json(out); // reject dangling references atomically
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
  if (c.type == T::HDistance) return std::fabs(a.x-b.x);
  if (c.type == T::VDistance) return std::fabs(a.y-b.y);
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
  return ParamTable(std::move(defs));
}

void evaluate_dimensions(Sketch& sk, const ParamTable& params) {
  const auto table = sketch_parameters(sk,params);
  std::vector<double> values;
  for (const auto& c : sk.constraints) {
    if (!c.is_dimension() || c.reference || c.expr.empty()) { values.push_back(c.value); continue; }
    // A driven value may be inspected, but cannot drive its own source geometry through a hidden cycle.
    for (const auto& name : expr_identifiers(c.expr))
      for (const auto& ref : sk.constraints)
        if (ref.reference && name == "d" + std::to_string(ref.id)) throw Error("reference dimensions cannot drive geometry");
    values.push_back(table.as(c.type == SkConstraint::Type::Angle ? Dim::Angle : Dim::Length, c.expr));
  }
  for (size_t i=0; i<values.size(); ++i) sk.constraints[i].value=values[i];
}

} // namespace opad::design
