#include "opad/design/sketch.hpp"
#include "opad/design/sketch_pattern.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace opad::design {
void shift_sketch_origin(Sketch& sk,double u,double v) {
  if(!std::isfinite(u)||!std::isfinite(v))throw Error("invalid sketch origin offset");
  for(auto& p:sk.points){p.x-=u;p.y-=v;}
  for(auto& c:sk.constraints){c.pos[0]-=u;c.pos[1]-=v;}
  for(auto& image:sk.images){image["position"][0]=image["position"][0].get<double>()-u;image["position"][1]=image["position"][1].get<double>()-v;}
  for(auto& pattern:sk.patterns)for(const auto& [key,offset]:std::vector<std::pair<std::string,double>>{{"cx",u},{"cy",v}}) {
    auto& input=pattern["inputs"][key];
    if(input.is_string())input="("+input.get<std::string>()+")-("+json(offset).dump()+" mm)";
    else input=(input.is_number()?input.get<double>():0)-offset;
    pattern["values"][key]=pattern["values"].value(key,0.0)-offset;
  }
  sk.validate();
}

namespace {

using EType = SkEntity::Type;
using CType = SkConstraint::Type;

const char* const kEntityNames[] = {"point", "line", "circle", "arc", "ellipse", "spline"};
const char* const kConstraintNames[] = {"coincident", "horizontal", "vertical", "parallel", "perpendicular", "collinear",
                                        "tangent",    "equal",      "concentric", "midpoint", "symmetric",   "fix", "smooth", "curvature",
                                        "distance",   "hdistance",  "vdistance",  "radius",   "diameter",    "angle", "arc_length"};

bool has_radius(EType t) { return t == EType::Circle || t == EType::Ellipse; }

// What a constraint reference resolves to. A Point *entity* counts as its point, so a sketch point drawn by
// the user and a bare end point can be constrained the same way.
enum class Kind { None, Point, Line, Circle, Arc, Ellipse, Spline, Constraint };

Kind entity_kind(EType t) {
  switch (t) {
    case EType::Point: return Kind::Point;
    case EType::Line: return Kind::Line;
    case EType::Circle: return Kind::Circle;
    case EType::Arc: return Kind::Arc;
    case EType::Ellipse: return Kind::Ellipse;
    case EType::Spline: return Kind::Spline;
  }
  return Kind::None;
}

// validate's lookups (UI-29): what each id is, which ids are points (not point entities) and the entities by id, one hash
// lookup each instead of a scan of the lists per reference (a converted drawing of 100k curves took minutes).
struct Index {
  std::unordered_map<int, Kind> kinds;
  std::unordered_set<int> points;
  std::unordered_map<int, const SkEntity*> entities;
};

Kind kind_of(const Sketch& sk, int id, const Index* index = nullptr) {
  if (index) {
    const auto found = index->kinds.find(id);
    return found == index->kinds.end() ? Kind::None : found->second;
  }
  if (sk.point(id)) return Kind::Point;
  if (const SkEntity* e = sk.entity(id)) return entity_kind(e->type);
  for (const auto& c : sk.constraints)
    if (c.id == id) return Kind::Constraint;
  return Kind::None;
}
bool has_point(const Sketch& sk, int id, const Index* index) { return index ? index->points.count(id) > 0 : sk.point(id) != nullptr; }
const SkEntity* entity_of(const Sketch& sk, int id, const Index* index) {
  if (!index) return sk.entity(id);
  const auto found = index->entities.find(id);
  return found == index->entities.end() ? nullptr : found->second;
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
    case CType::Tangent: return rr || (n == 2 && ((is(0, Kind::Line) && (is_round(k[1]) || is(1,Kind::Spline))) || ((is_round(k[0]) || is(0,Kind::Spline)) && is(1, Kind::Line)) || (is(0,Kind::Spline)&&is(1,Kind::Spline))));
    case CType::Smooth:
    case CType::Curvature: return n==2 && is(0,Kind::Spline) && is(1,Kind::Spline);
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
    case CType::VDistance: return pp || (n == 1 && is(0, Kind::Point));  // one point: its coordinate (gap log #11)
    case CType::Radius:
    case CType::Diameter: return n == 1 && is_round(k[0]);
    case CType::ArcLength: return n == 1 && is(0, Kind::Arc);
  }
  return false;
}

void check_entity(const Sketch& sk, const SkEntity& e, const Index* index = nullptr) {
  const std::string who = std::string("sketch: ") + SkEntity::type_name(e.type) + " " + std::to_string(e.id);
  const size_t n = e.p.size();
  bool ok = false;
  switch (e.type) {
    case EType::Point:
    case EType::Circle: ok = n == 1; break;
    case EType::Line:
    case EType::Ellipse: ok = n == 2; break;
    case EType::Arc: ok = n == 3; break;
    case EType::Spline: ok = n >= 2 || (n == 0 && !e.equation.is_null()); break;  // an equation's points come when computed
  }
  if (e.degree) {
    if (e.type!=EType::Spline || e.degree<1 || e.degree>25) throw Error(who+": invalid spline basis: degree 1 to 25");
    if (e.weights.size()!=n) throw Error(who+": invalid spline basis: one weight per control point (or none, all 1)");
    if (e.knots.size()<2 || e.knots.size()!=e.multiplicities.size())
      throw Error(who+": invalid spline basis: knots and multiplicities of the same length, at least two (or neither: uniform, clamped or periodic)");
    int total=0;
    for(size_t i=0;i<e.knots.size();++i) {
      if(!std::isfinite(e.knots[i]) || (i && e.knots[i]<=e.knots[i-1]) || e.multiplicities[i]<1 || e.multiplicities[i]>e.degree+1) throw Error(who+": invalid spline knots");
      total+=e.multiplicities[i];
    }
    if(!e.periodic && total!=int(n)+e.degree+1) throw Error(who+": inconsistent spline basis: the multiplicities of an open spline add up to control points + degree + 1 ("+std::to_string(n+size_t(e.degree)+1)+"), clamped ends taking degree + 1 each");
    for(double w:e.weights) if(!(w>0) || !std::isfinite(w)) throw Error(who+": invalid spline weight");
  }
  if (!ok) throw Error(who + ": wrong number of points (" + std::to_string(n) + ")");
  for (const auto* t : {&e.start_tangent, &e.end_tangent})
    if (!t->empty() && (e.type != EType::Spline || e.degree || e.periodic || t->size() != 2 || !(std::hypot((*t)[0], (*t)[1]) > 1e-12)))
      throw Error(who + ": end tangents are [dx, dy], not zero, on an open fit spline");
  if (!e.equation.is_null()) {
    const json& q = e.equation;
    auto expression = [&](const char* key, bool needed) {
      if (!q.contains(key)) { if (needed) throw Error(who + ": an equation needs " + key); return; }
      if (!q[key].is_string() && !(q[key].is_number() && std::string(key).rfind("t", 0) == 0)) throw Error(who + ": an equation's " + key + " is an expression");
    };
    if (!q.is_object() || e.type != EType::Spline || e.degree) throw Error(who + ": an equation belongs on a fit spline: {x, y, t0, t1, tolerance}");
    expression("x", true);
    expression("y", true);
    expression("t0", false);
    expression("t1", false);
    if (q.contains("tolerance") && !(q["tolerance"].is_number() && q["tolerance"].get<double>() > 0)) throw Error(who + ": an equation's tolerance is a positive number of mm");
    if (q.contains("min_points") && !q["min_points"].is_number_integer()) throw Error(who + ": min_points is a whole number");
  }
  if (e.type == EType::Spline && !e.degree && e.periodic && std::set<int>(e.p.begin(), e.p.end()).size() < 3)
    throw Error(who + ": a closed fit spline needs three points or more");
  for (int pid : e.p)
    if (!has_point(sk, pid, index)) throw Error(who + ": point " + std::to_string(pid) + " does not exist");
  if(!e.source.is_null() && (!e.source.is_object() || !e.source.contains("ref") || !e.source.at("ref").is_object() || e.source.value("slot",-1)<0 || e.source.value("count",0)<=e.source.value("slot",-1)))throw Error(who+": invalid projection source");
  if (has_radius(e.type) && !(e.r > 0 && std::isfinite(e.r))) throw Error(who + ": radius must be positive");
}

void check_constraint(const Sketch& sk, const SkConstraint& c, const Index* index = nullptr) {
  const std::string who = std::string("sketch: ") + SkConstraint::type_name(c.type) + " constraint " + std::to_string(c.id);
  std::vector<Kind> kinds;
  for (int ref : c.refs) {
    const Kind k = ref == c.id ? Kind::Constraint : kind_of(sk, ref, index);
    if (k == Kind::None) throw Error(who + ": reference " + std::to_string(ref) + " does not exist");
    if (k == Kind::Constraint) throw Error(who + ": reference " + std::to_string(ref) + " is a constraint");
    kinds.push_back(k);
  }
  if (!refs_fit(c.type, kinds)) throw Error(who + ": references do not fit the constraint type");
  for(int id:c.anchors)if(!has_point(sk,id,index))throw Error(who+": missing endpoint anchor");
  if(c.type==CType::Smooth || c.type==CType::Curvature || (c.type==CType::Tangent && std::find(kinds.begin(),kinds.end(),Kind::Spline)!=kinds.end())) {
    size_t splines=0;
    for(int ref:c.refs)if(const auto* e=entity_of(sk,ref,index);e && e->type==EType::Spline) {
      if(e->degree<2 || e->periodic || e->multiplicities.front()!=e->degree+1 || e->multiplicities.back()!=e->degree+1)throw Error(who+": use an open control-point spline of degree two or higher");
      if(splines>=c.anchors.size() || (c.anchors[splines]!=e->p.front() && c.anchors[splines]!=e->p.back()))throw Error(who+": anchor is not a spline endpoint");
      ++splines;
    }
    if(c.anchors.size()!=splines)throw Error(who+": missing spline endpoint anchors");
  }
  else if(!c.anchors.empty())throw Error(who+": endpoint anchors only belong to spline continuity constraints");
  if (c.is_dimension() && !std::isfinite(c.value)) throw Error(who + ": value is not a number");
}

// Ids are handed out in increasing order, so a list is nearly always sorted by them: a binary search finds the item, a scan
// only what it misses (a list edited out of order). Scanning per lookup made a converted drawing's edges n² (UI-29).
template <class V>
auto find_id(V& v, int id) -> decltype(v.data()) {
  size_t lo = 0, hi = v.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (v[mid].id < id) lo = mid + 1; else hi = mid;
  }
  if (lo < v.size() && v[lo].id == id) return &v[lo];
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

namespace {
int item_id(const json& item) { return item.at("id").get<int>(); }
template <class T> int item_id(const T& item) { return item.id; }
}  // namespace

int Sketch::next_id() const {
  IdScan& s = id_scan;
  auto unchanged = [&](const auto& list, size_t k) { return list.size() >= s.n[k] && (!s.n[k] || item_id(list[s.n[k] - 1]) == s.last[k]); };
  if (!(unchanged(points, 0) && unchanged(entities, 1) && unchanged(constraints, 2) && unchanged(images, 3) && unchanged(patterns, 4))) s = IdScan{};
  auto read = [&](const auto& list, size_t k) {
    for (size_t i = s.n[k]; i < list.size(); ++i) s.top = std::max(s.top, s.last[k] = item_id(list[i]));
    s.n[k] = list.size();
    if (s.n[k]) s.last[k] = item_id(list[s.n[k] - 1]);
  };
  read(points, 0); read(entities, 1); read(constraints, 2); read(images, 3); read(patterns, 4);
  return std::max(s.top, id_watermark) + 1;
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
  if(t==CType::Smooth || t==CType::Curvature || t==CType::Tangent) {
    std::vector<const SkEntity*> splines;
    const SkEntity* line=nullptr;
    for(int ref:c.refs)if(const auto* e=entity(ref)){if(e->type==EType::Spline)splines.push_back(e);if(e->type==EType::Line)line=e;}
    if(splines.size()==2) {
      double best=INFINITY;
      for(int a:{splines[0]->p.front(),splines[0]->p.back()})for(int b:{splines[1]->p.front(),splines[1]->p.back()}) {
        double d=std::hypot(point(a)->x-point(b)->x,point(a)->y-point(b)->y);if(d<best){best=d;c.anchors={a,b};}
      }
    } else if(splines.size()==1 && line) {
      const auto a=*point(line->p[0]),b=*point(line->p[1]);double best=INFINITY;
      for(int id:{splines[0]->p.front(),splines[0]->p.back()}){const auto p=*point(id);const double distance=std::fabs((b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x));if(distance<best){best=distance;c.anchors={id};}}
    }
  }
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
  id_watermark = next_id() - 1;
  while(const int pattern=pattern_of(*this,id,true)) {
    const bool instance=pattern_of(*this,id,false)!=0;
    remove_pattern(*this,pattern);
    if(instance || pattern==id)return;
  }
  auto drop_constraints_on = [&](int ref) {
    std::erase_if(constraints, [&](const SkConstraint& c) { return std::find(c.refs.begin(), c.refs.end(), ref) != c.refs.end() || std::find(c.anchors.begin(),c.anchors.end(),ref)!=c.anchors.end(); });
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
  // Points, entities, constraints, images and patterns share one id space (a constraint's refs can name either).
  std::map<int, const char*> ids;
  auto claim = [&](int id, const char* what) {
    if (id <= 0) throw Error(std::string("sketch: ") + what + " id " + std::to_string(id) + " is not positive");
    if (const auto [at, added] = ids.emplace(id, what); !added)
      throw Error("sketch: " + std::string(at->second) + " " + std::to_string(id) + " and " + what + " " + std::to_string(id) +
                  " have the same id; points, entities, constraints, images and patterns share one id space, so every id must be "
                  "unique across the whole sketch");
  };
  for (const auto& p : points) {
    claim(p.id, "point");
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) throw Error("sketch: point " + std::to_string(p.id) + " has no valid coordinates");
  }
  for (const auto& e : entities) claim(e.id, "entity");
  for (const auto& c : constraints) claim(c.id, "constraint");
  Index index;
  index.kinds.reserve(ids.size());
  for (const auto& p : points) index.kinds[p.id] = Kind::Point, index.points.insert(p.id);
  for (const auto& e : entities) index.kinds[e.id] = entity_kind(e.type), index.entities[e.id] = &e;
  for (const auto& c : constraints) index.kinds[c.id] = Kind::Constraint;
  if(!images.is_array())throw Error("sketch images must be an array");
  for(const auto& image:images) {
    claim(image.at("id").get<int>(),"image");if(!image.at("data").is_string()||!image.at("position").is_array()||image.at("position").size()!=2)throw Error("invalid sketch image");
    const double w=image.at("width").get<double>(),h=image.at("height").get<double>(),a=image.value("angle",0.0),opacity=image.value("opacity",.5),x=image.at("position")[0].get<double>(),y=image.at("position")[1].get<double>();
    if(!std::isfinite(w)||!std::isfinite(h)||!std::isfinite(a)||!std::isfinite(opacity)||!std::isfinite(x)||!std::isfinite(y)||w<=0||h<=0||opacity<0||opacity>1)throw Error("invalid sketch image placement");
  }
  if(!patterns.is_array())throw Error("sketch patterns must be an array");
  for(const auto& p:patterns) {
    // What a pattern needs, said plainly (gap log #14: the keys could only be guessed from error messages).
    static const std::string form="a sketch pattern is {\"id\", \"seeds\": [entity ids], \"inputs\": {\"count\", \"rows\", \"dx\", \"dy\"} in rows and columns, or {\"polar\": true, \"count\", \"angle\", \"cx\", \"cy\"} about a centre; values may be expressions}";
    if(!p.is_object() || !p.contains("id") || !p["id"].is_number_integer())throw Error("sketch pattern without an id: "+form);
    if(!p.contains("seeds") || !p["seeds"].is_array() || p["seeds"].empty())throw Error("sketch pattern "+std::to_string(p["id"].get<int>())+" has no seeds: "+form);
    if(!p.contains("inputs") || !p["inputs"].is_object())throw Error("sketch pattern "+std::to_string(p["id"].get<int>())+" has no inputs: "+form);
    claim(p.at("id").get<int>(),"pattern");
    for(int id:p.at("seeds").get<std::vector<int>>())if(!index.entities.count(id))throw Error("pattern seed no longer exists");
    for(const auto& instance:p.value("instances",json::array()))for(const auto& pair:instance.at("map")) {
      if(!pair.is_array()||pair.size()!=2)throw Error("invalid pattern ID map");
      for(int id:pair.get<std::vector<int>>())if(!index.points.count(id)&&!index.entities.count(id))throw Error("pattern refers to missing geometry");
    }
  }
  for (const auto& e : entities) check_entity(*this, e, &index);
  for (const auto& c : constraints) check_constraint(*this, c, &index);
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
    if (e.degree) { o["degree"]=e.degree; o["knots"]=e.knots; o["multiplicities"]=e.multiplicities; o["weights"]=e.weights; o["periodic"]=e.periodic; }
    else if (e.periodic) o["periodic"]=true;
    if (!e.equation.is_null()) o["equation"]=e.equation;
    if (!e.start_tangent.empty()) o["start_tangent"]=e.start_tangent;
    if (!e.end_tangent.empty()) o["end_tangent"]=e.end_tangent;
    if (e.construction) o["construction"] = true;
    if (e.fixed) o["fixed"] = true;
    if(!e.source.is_null())o["source"]=e.source;
    je.push_back(std::move(o));
  }
  for (const auto& c : constraints) {
    json o = {{"id", c.id}, {"type", SkConstraint::type_name(c.type)}, {"refs", c.refs}};
    if(!c.anchors.empty())o["anchors"]=c.anchors;
    if (c.is_dimension()) {
      o["value"] = c.value;
      if (c.reference) o["reference"] = true;
      if (c.is_signed) o["signed"] = true;
      if (!c.expr.empty()) o["expr"] = c.expr;
      o["pos"] = json::array({c.pos[0], c.pos[1]});
    }
    jc.push_back(std::move(o));
  }
  auto ordered = [](json& a) { std::sort(a.begin(), a.end(), [](const json& x, const json& y) { return x.at("id").get<int>() < y.at("id").get<int>(); }); };
  ordered(jp); ordered(je); ordered(jc);
  json out{{"points", std::move(jp)}, {"entities", std::move(je)}, {"constraints", std::move(jc)}};
  if (id_watermark) out["id_watermark"] = id_watermark;
  if(!patterns.empty())out["patterns"]=patterns;
  if(!images.empty()){out["images"]=images;ordered(out["images"]);}
  return out;
}

Sketch Sketch::from_json(const json& j) {
  Sketch sk;
  try {
    if (!j.is_object()) throw Error("sketch: not a JSON object");
    sk.id_watermark = j.value("id_watermark", 0);
    sk.patterns = j.value("patterns",json::array());
    sk.images=j.value("images",json::array());
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
      e.p = o.value("p", std::vector<int>{});  // an equation curve's come when it is computed; the rest are checked
      if (has_radius(e.type)) e.r = o.at("r").get<double>();
      e.degree=o.value("degree",0); e.knots=o.value("knots",std::vector<double>{});
      e.weights=o.value("weights",std::vector<double>{}); e.multiplicities=o.value("multiplicities",std::vector<int>{}); e.periodic=o.value("periodic",false);
      e.start_tangent=o.value("start_tangent",std::vector<double>{}); e.end_tangent=o.value("end_tangent",std::vector<double>{});
      e.equation=o.value("equation",json());
      // A control-point spline given only its degree: uniform knots, clamped at both ends or periodic, and unit
      // weights (gap log #5: every form without weights was refused as "invalid spline basis").
      if (e.degree>0 && e.type==SkEntity::Type::Spline) {
        const int n=int(e.p.size());
        if (e.weights.empty()) e.weights.assign(size_t(n),1.0);
        if (e.knots.empty() && e.multiplicities.empty() && n>e.degree) {
          if (e.periodic) for (int i=0;i<=n;++i) { e.knots.push_back(i); e.multiplicities.push_back(1); }
          else for (int i=0;i<=n-e.degree;++i) { e.knots.push_back(i); e.multiplicities.push_back(i==0 || i==n-e.degree ? e.degree+1 : 1); }
        }
      }
      e.construction = o.value("construction", false);
      e.fixed = o.value("fixed", false);e.source=o.value("source",json());
      sk.entities.push_back(std::move(e));
    }
    for (const auto& o : list("constraints")) {
      SkConstraint c;
      c.id = o.at("id").get<int>();
      c.type = SkConstraint::type_from_name(o.at("type").get<std::string>());
      c.refs = o.at("refs").get<std::vector<int>>();
      c.anchors=o.value("anchors",std::vector<int>{});
      if (c.is_dimension()) {
        c.value = o.at("value").get<double>();
        c.reference = o.value("reference", false);
        c.is_signed = o.value("signed", false);
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
