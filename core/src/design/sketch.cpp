#include "opad/design/sketch.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace opad::design {

namespace {

using EType = SkEntity::Type;
using CType = SkConstraint::Type;

const char* const kEntityNames[] = {"point", "line", "circle", "arc", "ellipse", "spline"};
const char* const kConstraintNames[] = {"coincident", "horizontal", "vertical", "parallel", "perpendicular", "collinear",
                                        "tangent",    "equal",      "concentric", "midpoint", "symmetric",   "fix",
                                        "distance",   "hdistance",  "vdistance",  "radius",   "diameter",    "angle"};

bool has_radius(EType t) { return t == EType::Circle || t == EType::Ellipse; }

// What a constraint reference resolves to. A Point *entity* counts as its point, so a sketch point drawn by
// the user and a bare end point can be constrained the same way.
enum class Kind { None, Point, Line, Circle, Arc, Ellipse, Spline, Constraint };

Kind kind_of(const Sketch& sk, int id) {
  if (sk.point(id)) return Kind::Point;
  if (const SkEntity* e = sk.entity(id)) {
    switch (e->type) {
      case EType::Point: return Kind::Point;
      case EType::Line: return Kind::Line;
      case EType::Circle: return Kind::Circle;
      case EType::Arc: return Kind::Arc;
      case EType::Ellipse: return Kind::Ellipse;
      case EType::Spline: return Kind::Spline;
    }
  }
  for (const auto& c : sk.constraints)
    if (c.id == id) return Kind::Constraint;
  return Kind::None;
}

bool is_round(Kind k) { return k == Kind::Circle || k == Kind::Arc; }

// The reference patterns of the header, one place for add_constraint, validate and from_json.
bool refs_fit(CType t, const std::vector<Kind>& k) {
  const size_t n = k.size();
  auto is = [&](size_t i, Kind want) { return i < n && k[i] == want; };
  const bool pp = n == 2 && is(0, Kind::Point) && is(1, Kind::Point);
  const bool ll = n == 2 && is(0, Kind::Line) && is(1, Kind::Line);
  const bool rr = n == 2 && is_round(k[0]) && is_round(k[1]);
  switch (t) {
    case CType::Coincident: return pp || (n == 2 && is(0, Kind::Point) && (is(1, Kind::Line) || is_round(k[1])));
    case CType::Horizontal:
    case CType::Vertical: return pp || (n == 1 && is(0, Kind::Line));
    case CType::Parallel:
    case CType::Perpendicular:
    case CType::Collinear:
    case CType::Angle: return ll;
    case CType::Tangent: return rr || (n == 2 && ((is(0, Kind::Line) && is_round(k[1])) || (is_round(k[0]) && is(1, Kind::Line))));
    case CType::Equal: return ll || rr;
    case CType::Concentric: {
      auto centred = [](Kind x) { return is_round(x) || x == Kind::Ellipse; };
      return n == 2 && centred(k[0]) && centred(k[1]);
    }
    case CType::Midpoint: return n == 2 && is(0, Kind::Point) && is(1, Kind::Line);
    case CType::Symmetric: return n == 3 && is(0, Kind::Point) && is(1, Kind::Point) && is(2, Kind::Line);
    case CType::Fix: return n == 1;  // any point or entity
    case CType::Distance: return pp || ll || (n == 1 && is(0, Kind::Line)) || (n == 2 && is(0, Kind::Point) && is(1, Kind::Line));
    case CType::HDistance:
    case CType::VDistance: return pp;
    case CType::Radius:
    case CType::Diameter: return n == 1 && is_round(k[0]);
  }
  return false;
}

void check_entity(const Sketch& sk, const SkEntity& e) {
  const std::string who = std::string("sketch: ") + SkEntity::type_name(e.type) + " " + std::to_string(e.id);
  const size_t n = e.p.size();
  bool ok = false;
  switch (e.type) {
    case EType::Point:
    case EType::Circle: ok = n == 1; break;
    case EType::Line:
    case EType::Ellipse: ok = n == 2; break;
    case EType::Arc: ok = n == 3; break;
    case EType::Spline: ok = n >= 2; break;
  }
  if (!ok) throw Error(who + ": wrong number of points (" + std::to_string(n) + ")");
  for (int pid : e.p)
    if (!sk.point(pid)) throw Error(who + ": point " + std::to_string(pid) + " does not exist");
  if (has_radius(e.type) && !(e.r > 0 && std::isfinite(e.r))) throw Error(who + ": radius must be positive");
}

void check_constraint(const Sketch& sk, const SkConstraint& c) {
  const std::string who = std::string("sketch: ") + SkConstraint::type_name(c.type) + " constraint " + std::to_string(c.id);
  std::vector<Kind> kinds;
  for (int ref : c.refs) {
    const Kind k = ref == c.id ? Kind::Constraint : kind_of(sk, ref);
    if (k == Kind::None) throw Error(who + ": reference " + std::to_string(ref) + " does not exist");
    if (k == Kind::Constraint) throw Error(who + ": reference " + std::to_string(ref) + " is a constraint");
    kinds.push_back(k);
  }
  if (!refs_fit(c.type, kinds)) throw Error(who + ": references do not fit the constraint type");
  if (c.is_dimension() && !std::isfinite(c.value)) throw Error(who + ": value is not a number");
}

template <class V>
auto find_id(V& v, int id) -> decltype(v.data()) {
  for (auto& it : v)
    if (it.id == id) return &it;
  return nullptr;
}

template <size_t N>
int name_index(const char* const (&names)[N], const std::string& s, const char* what) {
  for (size_t i = 0; i < N; ++i)
    if (s == names[i]) return int(i);
  throw Error(std::string("sketch: unknown ") + what + " type '" + s + "'");
}

}  // namespace

const char* SkEntity::type_name(Type t) { return kEntityNames[size_t(t)]; }
SkEntity::Type SkEntity::type_from_name(const std::string& s) { return Type(name_index(kEntityNames, s, "entity")); }
const char* SkConstraint::type_name(Type t) { return kConstraintNames[size_t(t)]; }
SkConstraint::Type SkConstraint::type_from_name(const std::string& s) { return Type(name_index(kConstraintNames, s, "constraint")); }

SkPoint* Sketch::point(int id) { return find_id(points, id); }
const SkPoint* Sketch::point(int id) const { return find_id(points, id); }
SkEntity* Sketch::entity(int id) { return find_id(entities, id); }
const SkEntity* Sketch::entity(int id) const { return find_id(entities, id); }
SkConstraint* Sketch::constraint(int id) { return find_id(constraints, id); }

int Sketch::next_id() const {
  int top = 0;
  for (const auto& p : points) top = std::max(top, p.id);
  for (const auto& e : entities) top = std::max(top, e.id);
  for (const auto& c : constraints) top = std::max(top, c.id);
  return top + 1;
}

int Sketch::add_point(double x, double y, bool fixed) {
  if (!std::isfinite(x) || !std::isfinite(y)) throw Error("sketch: point coordinates are not numbers");
  SkPoint p;
  p.id = next_id();
  p.x = x;
  p.y = y;
  p.fixed = fixed;
  points.push_back(p);
  return p.id;
}

// The builders check before they push, so a throwing call leaves the sketch as it was.
static int add_entity(Sketch& sk, SkEntity e) {
  e.id = sk.next_id();
  check_entity(sk, e);
  sk.entities.push_back(std::move(e));
  return sk.entities.back().id;
}

int Sketch::add_line(int p0, int p1, bool construction) {
  SkEntity e;
  e.type = EType::Line;
  e.p = {p0, p1};
  e.construction = construction;
  return add_entity(*this, std::move(e));
}

int Sketch::add_circle(int centre, double r, bool construction) {
  SkEntity e;
  e.type = EType::Circle;
  e.p = {centre};
  e.r = r;
  e.construction = construction;
  return add_entity(*this, std::move(e));
}

int Sketch::add_arc(int centre, int start, int end, bool construction) {
  SkEntity e;
  e.type = EType::Arc;
  e.p = {centre, start, end};
  e.construction = construction;
  return add_entity(*this, std::move(e));
}

int Sketch::add_constraint(SkConstraint::Type t, std::vector<int> refs, double value, const std::string& expr) {
  SkConstraint c;
  c.id = next_id();
  c.type = t;
  c.refs = std::move(refs);
  if (c.is_dimension()) {
    c.value = value;
    c.expr = expr;
  }
  check_constraint(*this, c);
  constraints.push_back(std::move(c));
  return constraints.back().id;
}

// Unknown ids and points still used by an entity are left alone: a caller deleting a selection may name a
// line and its end points in any order.
void Sketch::remove(int id) {
  auto drop_constraints_on = [&](int ref) {
    std::erase_if(constraints, [&](const SkConstraint& c) { return std::find(c.refs.begin(), c.refs.end(), ref) != c.refs.end(); });
  };
  auto used = [&](int pid) {
    for (const auto& e : entities)
      if (std::find(e.p.begin(), e.p.end(), pid) != e.p.end()) return true;
    return false;
  };
  auto drop_point = [&](int pid) {
    if (used(pid)) return;
    drop_constraints_on(pid);
    std::erase_if(points, [&](const SkPoint& p) { return p.id == pid; });
  };

  if (constraint(id)) {
    std::erase_if(constraints, [&](const SkConstraint& c) { return c.id == id; });
  } else if (const SkEntity* e = entity(id)) {
    const std::vector<int> pts = e->p;
    drop_constraints_on(id);
    std::erase_if(entities, [&](const SkEntity& x) { return x.id == id; });
    for (int pid : pts) drop_point(pid);
  } else if (point(id)) {
    drop_point(id);
  }
}

void Sketch::validate() const {
  std::set<int> ids;
  auto claim = [&](int id, const char* what) {
    if (id <= 0) throw Error(std::string("sketch: ") + what + " id " + std::to_string(id) + " is not positive");
    if (!ids.insert(id).second) throw Error("sketch: id " + std::to_string(id) + " is used twice");
  };
  for (const auto& p : points) {
    claim(p.id, "point");
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw Error("sketch: point " + std::to_string(p.id) + " has no valid coordinates");
  }
  for (const auto& e : entities) claim(e.id, "entity");
  for (const auto& c : constraints) claim(c.id, "constraint");
  for (const auto& e : entities) check_entity(*this, e);
  for (const auto& c : constraints) check_constraint(*this, c);
}

json Sketch::to_json() const {
  json jp = json::array(), je = json::array(), jc = json::array();
  for (const auto& p : points) {
    json o = {{"id", p.id}, {"x", p.x}, {"y", p.y}};
    if (p.fixed) o["fixed"] = true;
    jp.push_back(std::move(o));
  }
  for (const auto& e : entities) {
    json o = {{"id", e.id}, {"type", SkEntity::type_name(e.type)}, {"p", e.p}};
    if (has_radius(e.type)) o["r"] = e.r;
    if (e.construction) o["construction"] = true;
    if (e.fixed) o["fixed"] = true;
    je.push_back(std::move(o));
  }
  for (const auto& c : constraints) {
    json o = {{"id", c.id}, {"type", SkConstraint::type_name(c.type)}, {"refs", c.refs}};
    if (c.is_dimension()) {
      o["value"] = c.value;
      if (!c.expr.empty()) o["expr"] = c.expr;
      o["pos"] = json::array({c.pos[0], c.pos[1]});
    }
    jc.push_back(std::move(o));
  }
  return json{{"points", std::move(jp)}, {"entities", std::move(je)}, {"constraints", std::move(jc)}};
}

Sketch Sketch::from_json(const json& j) {
  Sketch sk;
  try {
    if (!j.is_object()) throw Error("sketch: not a JSON object");
    auto list = [&](const char* key) -> json {
      if (!j.contains(key)) return json::array();
      if (!j.at(key).is_array()) throw Error(std::string("sketch: '") + key + "' is not an array");
      return j.at(key);
    };
    for (const auto& o : list("points")) {
      SkPoint p;
      p.id = o.at("id").get<int>();
      p.x = o.at("x").get<double>();
      p.y = o.at("y").get<double>();
      p.fixed = o.value("fixed", false);
      sk.points.push_back(p);
    }
    for (const auto& o : list("entities")) {
      SkEntity e;
      e.id = o.at("id").get<int>();
      e.type = SkEntity::type_from_name(o.at("type").get<std::string>());
      e.p = o.at("p").get<std::vector<int>>();
      if (has_radius(e.type)) e.r = o.at("r").get<double>();
      e.construction = o.value("construction", false);
      e.fixed = o.value("fixed", false);
      sk.entities.push_back(std::move(e));
    }
    for (const auto& o : list("constraints")) {
      SkConstraint c;
      c.id = o.at("id").get<int>();
      c.type = SkConstraint::type_from_name(o.at("type").get<std::string>());
      c.refs = o.at("refs").get<std::vector<int>>();
      if (c.is_dimension()) {
        c.value = o.at("value").get<double>();
        c.expr = o.value("expr", std::string());
        if (o.contains("pos")) {
          const auto pos = o.at("pos").get<std::vector<double>>();
          if (pos.size() != 2) throw Error("sketch: constraint " + std::to_string(c.id) + ": 'pos' needs two numbers");
          c.pos[0] = pos[0];
          c.pos[1] = pos[1];
        }
      }
      sk.constraints.push_back(std::move(c));
    }
  } catch (const json::exception& e) {
    throw Error(std::string("sketch: malformed JSON: ") + e.what());
  }
  sk.validate();
  return sk;
}

}  // namespace opad::design
