#include "opad/explode.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <cmath>

#include "opad/geometry.hpp"

namespace opad {
namespace {

Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 mul(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 unit(const Vec3& a) { const double n = norm(a); return n > 0 ? mul(a, 1 / n) : Vec3{0, 0, 1}; }

[[noreturn]] void bad(const std::string& why) { throw Error("explode: " + why); }

Vec3 vec_of(const json& j, const std::string& what) {
  if (!j.is_array() || j.size() != 3) bad(what + " must be [x, y, z]");
  Vec3 v{};
  for (size_t i = 0; i < 3; ++i) {
    if (!j[i].is_number() || !std::isfinite(j[i].get<double>())) bad(what + " must be three finite numbers");
    v[i] = j[i].get<double>();
  }
  return v;
}

std::vector<std::string> ids_of(const json& j, const std::string& what) {
  if (!j.is_array()) bad(what + " must be a list of node ids");
  std::vector<std::string> out;
  for (const auto& v : j) {
    if (!v.is_string()) bad(what + " must be a list of node ids");
    out.push_back(v.get<std::string>());
  }
  return out;
}

double number_of(const json& j, const char* key, double def, double lo, double hi) {
  if (!j.contains(key)) return def;
  const json& v = j[key];
  if (!v.is_number() || !std::isfinite(v.get<double>()) || v.get<double>() < lo || v.get<double>() > hi)
    bad(std::string(key) + " must be a number from " + json(lo).dump() + " to " + json(hi).dump());
  return v.get<double>();
}

struct Box {
  Vec3 lo{0, 0, 0}, hi{0, 0, 0};
  bool empty = true;
  void add(const Box& b) {
    if (b.empty) return;
    for (int i = 0; i < 3; ++i) {
      lo[i] = empty ? b.lo[i] : std::min(lo[i], b.lo[i]);
      hi[i] = empty ? b.hi[i] : std::max(hi[i], b.hi[i]);
    }
    empty = false;
  }
  static Box of(const Bnd_Box& b) {
    Box out;
    if (b.IsVoid()) return out;
    b.Get(out.lo[0], out.lo[1], out.lo[2], out.hi[0], out.hi[1], out.hi[2]);
    out.empty = false;
    return out;
  }
  Vec3 centre() const { return mul(::opad::add(lo, hi), 0.5); }
  double diag() const { return empty ? 0 : norm(sub(hi, lo)); }
  double volume() const { return empty ? 0 : (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]); }
  double extent(const Vec3& a) const { return std::abs(a[0]) * (hi[0] - lo[0]) + std::abs(a[1]) * (hi[1] - lo[1]) + std::abs(a[2]) * (hi[2] - lo[2]); }
  void span(const Vec3& a, double& from, double& to) const {  // the box projected on a
    from = to = dot(lo, a);
    for (int c = 1; c < 8; ++c) {
      const double p = dot(Vec3{(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]}, a);
      from = std::min(from, p);
      to = std::max(to, p);
    }
  }
  bool touches(const Box& b, double tol) const {
    for (int i = 0; i < 3; ++i)
      if (lo[i] > b.hi[i] + tol || b.lo[i] > hi[i] + tol) return false;
    return true;
  }
  bool inside(const Box& b, double tol) const {  // clear of b's walls all round: b's box holds it, b does not touch it
    for (int i = 0; i < 3; ++i)
      if (lo[i] <= b.lo[i] + tol || hi[i] >= b.hi[i] - tol) return false;
    return true;
  }
  bool over(const Box& b, size_t i, double tol) const {  // three quarters of it lie in b's span along axis i
    const double len = hi[i] - lo[i];
    return len < 1e-9 ? lo[i] >= b.lo[i] - tol && hi[i] <= b.hi[i] + tol : std::min(hi[i], b.hi[i]) - std::max(lo[i], b.lo[i]) >= 0.75 * len;
  }
  bool within(const Box& b, double tol) const { return over(b, 0, tol) && over(b, 1, tol) && over(b, 2, tol); }
  // +1 / -1 when it rests on b's upper / lower face along an axis (sunk in by a quarter at most) and lies over that face.
  int rests_on(const Box& b, double tol, size_t& axis) const {
    for (size_t i = 0; i < 3; ++i) {
      if (!over(b, (i + 1) % 3, tol) || !over(b, (i + 2) % 3, tol)) continue;
      const double dip = std::max(tol, 0.25 * (hi[i] - lo[i]));
      axis = i;
      if (lo[i] >= b.hi[i] - dip && lo[i] <= b.hi[i] + tol && hi[i] > b.hi[i]) return 1;
      if (hi[i] <= b.lo[i] + dip && hi[i] >= b.lo[i] - tol && lo[i] < b.lo[i]) return -1;
    }
    return 0;
  }
};

bool shown(const Node* n) { return n && n->visible && !(n->kind == Node::Kind::Body && n->body_missing); }

bool grouped(const ExplodeSpec& spec, const std::string& id) {
  for (const auto& g : spec.groups)
    if (std::find(g.begin(), g.end(), id) != g.end()) return true;
  return false;
}

const std::vector<std::string>& children_of(const Scene& scene, const std::string& id) {
  return id.empty() ? scene.roots : scene.node(id)->children;
}

// The unit tree under construction. Index 0 is the explode root; a unit always comes after its parent.
struct Builder {
  struct Work {
    ExplodeUnit u;
    std::vector<int> kids;
    bool chosen = false;  // keep, split or a group: the user picked it, so attach_small leaves it alone
    bool dead = false;
    Box box;
    double solid = 0;  // the volumes of its bodies' boxes: a unit of four corner screws is mostly air
    Box main;          // its largest body's box: a board with the capacitors that ride on it is still the board
    bool axial = false;  // fasteners: every body under it a fastener along `axis`
    Vec3 axis{0, 0, 1};
  };
  const Scene& scene;
  const ExplodeSpec& spec;
  std::vector<Work> w;
  std::unordered_map<std::string, size_t> group_of;
  std::vector<int> group_unit;

  Builder(const Scene& s, const ExplodeSpec& sp) : scene(s), spec(sp), group_unit(sp.groups.size(), -1) {
    for (size_t g = 0; g < sp.groups.size(); ++g)
      for (const auto& id : sp.groups[g]) group_of.emplace(id, g);
    w.emplace_back();
    w[0].u.level = 0;
  }

  int add(int parent, const std::string& id, const std::string& name, bool chosen) {
    Work x;
    x.u.id = id;
    x.u.name = name;
    x.u.parent = parent;
    x.u.level = w[static_cast<size_t>(parent)].u.level + 1;
    x.chosen = chosen;
    w.push_back(std::move(x));
    const int i = static_cast<int>(w.size()) - 1;
    w[static_cast<size_t>(parent)].kids.push_back(i);
    return i;
  }

  int group(size_t g, int parent) {
    if (group_unit[g] < 0) {
      const auto& members = spec.groups[g];
      std::string name = "Group";
      for (const auto& m : members)
        if (const Node* n = scene.node(m)) { name = n->name; break; }
      if (members.size() > 1) name += " +" + std::to_string(members.size() - 1);
      group_unit[g] = add(parent, members.front(), name, true);
    }
    return group_unit[g];
  }

  // A child of `parent`'s node, `depth` below the root. closed: past the levels (only an explicit split opens).
  void place(int parent, const std::string& id, int depth, bool closed) {
    const Node* n = scene.node(id);
    if (!shown(n)) return;
    if (auto g = group_of.find(id); g != group_of.end()) return absorb(group(g->second, parent), id, true);
    if (n->kind == Node::Kind::Body) {
      w[static_cast<size_t>(add(parent, id, n->name, false))].u.bodies.push_back(id);
      return;
    }
    const bool kept = spec.keep.count(id) > 0, split = spec.split.count(id) > 0;
    const int u = add(parent, id, n->name, kept || split);
    const bool opens = split || (!kept && !closed && (spec.levels == 0 || depth < spec.levels));
    for (const auto& c : n->children) {
      if (opens) place(u, c, depth + 1, closed);
      else absorb(u, c, false);
    }
  }

  // Everything under node `id` moves with unit u, except explicitly split components and group members inside it.
  void absorb(int u, const std::string& id, bool in_group) {
    const Node* n = scene.node(id);
    if (!shown(n)) return;
    if (!in_group)
      if (auto g = group_of.find(id); g != group_of.end()) return absorb(group(g->second, u), id, true);
    if (n->kind == Node::Kind::Body) {
      w[static_cast<size_t>(u)].u.bodies.push_back(id);
      return;
    }
    if (spec.split.count(id)) {
      const int s = add(u, id, n->name, true);
      for (const auto& c : n->children) place(s, c, 0, true);
      return;
    }
    for (const auto& c : n->children) absorb(u, c, false);
  }

  void bodies_under(int u, std::vector<std::string>& out) const {
    const Work& x = w[static_cast<size_t>(u)];
    out.insert(out.end(), x.u.bodies.begin(), x.u.bodies.end());
    for (int k : x.kids) bodies_under(k, out);
  }

  void kill(int u) {
    w[static_cast<size_t>(u)].dead = true;
    for (int k : w[static_cast<size_t>(u)].kids) kill(k);
  }

  // Small children of p ride with the larger sibling they touch: the smallest one whose box they sit on rather than in
  // (a capacitor joins the board, not the enclosure whose box holds both). Chains end at a part that is not small.
  void attach(int p) {
    auto& kids = w[static_cast<size_t>(p)].kids;
    if (kids.size() < 2) return;
    const double limit = spec.small_size > 0 ? spec.small_size : spec.small_ratio * w[static_cast<size_t>(p)].box.diag();
    std::vector<int> target(kids.size(), -1);
    for (size_t i = 0; i < kids.size(); ++i) {
      const Work& a = w[static_cast<size_t>(kids[i])];
      if (a.chosen || a.box.empty || a.box.diag() >= limit) continue;
      bool best_inside = true;
      double best_diag = 0;
      for (size_t j = 0; j < kids.size(); ++j) {
        const Work& c = w[static_cast<size_t>(kids[j])];
        if (j == i || c.box.empty || c.box.diag() <= a.box.diag() || !a.box.touches(c.box, spec.touch)) continue;
        const bool inside = a.box.inside(c.box, spec.touch);
        if (target[i] < 0 || (best_inside && !inside) || (inside == best_inside && c.box.diag() < best_diag)) {
          target[i] = static_cast<int>(j);
          best_inside = inside;
          best_diag = c.box.diag();
        }
      }
    }
    std::vector<int> keep_kids;
    for (size_t i = 0; i < kids.size(); ++i) {
      if (target[i] < 0) {
        keep_kids.push_back(kids[i]);
        continue;
      }
      int f = target[i];
      while (target[static_cast<size_t>(f)] >= 0) f = target[static_cast<size_t>(f)];
      Work& to = w[static_cast<size_t>(kids[static_cast<size_t>(f)])];
      bodies_under(kids[i], to.u.bodies);
      to.box.add(w[static_cast<size_t>(kids[i])].box);
      to.solid += w[static_cast<size_t>(kids[i])].solid;
      if (w[static_cast<size_t>(kids[i])].main.diag() > to.main.diag()) to.main = w[static_cast<size_t>(kids[i])].main;
      kill(kids[i]);
    }
    kids = keep_kids;
  }

  // Directions and distances of p's children, from p's assembled box.
  void spread(int p) {
    const auto& kids = w[static_cast<size_t>(p)].kids;
    const Box& P = w[static_cast<size_t>(p)].box;
    const Vec3 pc = P.centre();
    const double pd = P.diag();
    for (int k : kids) {
      Work& x = w[static_cast<size_t>(k)];
      x.u.lo = x.box.lo;
      x.u.hi = x.box.hi;
      x.u.centre = x.box.empty ? pc : x.box.centre();
    }
    if (kids.size() < 2) return;  // nothing to move away from
    const Vec3 a = unit(spec.axis);
    if (spec.mode == "stack") return stack(kids, a, spec.spacing * 0.1 * pd);
    const int largest = by_size(w, kids).front();
    const int base = w[static_cast<size_t>(largest)].box.diag() >= 0.85 * pd ? largest : -1;  // nearly the whole: it stays
    for (int k : kids) {
      Work& x = w[static_cast<size_t>(k)];
      if (x.box.empty || k == base) continue;
      const Vec3 v = sub(x.u.centre, pc);
      if (x.axial) {  // out along its axis, the short way out of the parent (a tie: the parent's way)
        double from = 0, to = 0;
        P.span(x.axis, from, to);
        const double c = dot(x.u.centre, x.axis), tol = 1e-6 * std::max(1.0, pd);
        const double sign = to - c < c - from - tol ? 1 : c - from < to - c - tol ? -1 : dot(w[static_cast<size_t>(p)].u.dir, x.axis) < 0 ? -1 : 1;
        x.u.dir = mul(x.axis, sign);
        x.u.distance = spec.spacing * (std::abs(dot(v, x.axis)) + 0.5 * x.box.extent(x.axis) + 0.25 * P.extent(x.axis));
        continue;
      }
      if (spec.mode == "axis") {
        const double along = dot(v, a);
        x.u.dir = along < -1e-9 * std::max(1.0, pd) ? mul(a, -1) : a;
        x.u.distance = spec.spacing * (std::abs(along) + 0.5 * x.box.extent(a) + 0.25 * P.extent(a));
        continue;
      }
      const double len = norm(v);
      if (len > 0.01 * pd && len > 1e-9) {
        x.u.dir = mul(v, 1 / len);
      } else {  // on the parent's centre (coaxial parts): along the parent's longest side
        const Vec3 size = sub(P.hi, P.lo);
        const int ax = size[0] >= size[1] && size[0] >= size[2] ? 0 : size[1] >= size[2] ? 1 : 2;
        x.u.dir = {0, 0, 0};
        x.u.dir[static_cast<size_t>(ax)] = v[static_cast<size_t>(ax)] < 0 ? -1 : 1;
      }
      x.u.distance = spec.spacing * (0.5 * x.box.diag() + 0.25 * pd);
    }
    if (spec.mode == "radial") rest(kids);
    if (spec.spacing > 0) {
      contain(kids, base, pc, a);
      clear(kids, spec.spacing * 0.05 * pd);
      keep_order(kids, spec.spacing * 0.05 * pd);
    }
  }

  // Parts leaving the same way keep their order along it where one is in the other's path (screws, board, lid out of a
  // shell): none passes through another.
  void keep_order(const std::vector<int>& kids, double gap) {
    std::vector<int> moving;
    for (int k : kids)
      if (!w[static_cast<size_t>(k)].box.empty && w[static_cast<size_t>(k)].u.distance > 0) moving.push_back(k);
    std::vector<char> done(moving.size(), 0);
    for (size_t g = 0; g < moving.size(); ++g) {
      if (done[g]) continue;
      const Vec3 d = w[static_cast<size_t>(moving[g])].u.dir;
      std::vector<int> same;
      for (size_t h = g; h < moving.size(); ++h)
        if (!done[h] && dot(w[static_cast<size_t>(moving[h])].u.dir, d) > 0.999) {
          same.push_back(moving[h]);
          done[h] = 1;
        }
      if (same.size() < 2) continue;
      const Vec3 u = unit(cross(std::abs(d[2]) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0}, d)), v = cross(d, u);
      auto from = [&](int k, const Vec3& axis) { double a0, a1; w[static_cast<size_t>(k)].box.span(axis, a0, a1); return std::pair{a0, a1}; };
      std::stable_sort(same.begin(), same.end(), [&](int x, int y) { return from(x, d).first < from(y, d).first; });
      for (size_t n = 1; n < same.size(); ++n) {
        Work& x = w[static_cast<size_t>(same[n])];
        const double x0 = from(same[n], d).first;
        const auto [xu0, xu1] = from(same[n], u);
        const auto [xv0, xv1] = from(same[n], v);
        for (size_t m = 0; m < n; ++m) {
          const Work& y = w[static_cast<size_t>(same[m])];
          const auto [yu0, yu1] = from(same[m], u);
          const auto [yv0, yv1] = from(same[m], v);
          if (std::min(xu1, yu1) - std::max(xu0, yu0) <= spec.touch || std::min(xv1, yv1) - std::max(xv0, yv0) <= spec.touch) continue;
          x.u.distance = std::max(x.u.distance, from(same[m], d).second + y.u.distance + gap - x0);
        }
      }
    }
  }

  // A part resting on a larger sibling's face (a chip on its board, a lid on its shell) lifts straight off that face.
  void rest(const std::vector<int>& kids) {
    for (int k : kids) {
      Work& x = w[static_cast<size_t>(k)];
      if (x.box.empty || x.u.distance == 0 || x.axial) continue;
      double under = 0;
      for (int j : kids) {
        const Box& y = w[static_cast<size_t>(j)].main;
        size_t axis = 0;
        if (j == k || y.empty || y.diag() <= std::max(under, x.box.diag())) continue;
        if (const int side = x.box.rests_on(y, spec.touch, axis)) {
          x.u.dir = {0, 0, 0};
          x.u.dir[axis] = side;
          under = y.diag();
        }
      }
    }
  }

  static std::vector<int> by_size(const std::vector<Work>& w, const std::vector<int>& kids) {
    std::vector<int> order = kids;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return w[static_cast<size_t>(a)].box.diag() > w[static_cast<size_t>(b)].box.diag(); });
    return order;
  }

  // A part inside a larger sibling's box (a board in its shell) leaves it on the side away from where that one goes (out
  // of a base that stays: towards the rest of the model), and only as far as it must (clear). It does not dive through
  // the shell's floor because its centre is below the model's.
  void contain(const std::vector<int>& kids, int base, const Vec3& pc, const Vec3& a) {
    const std::vector<int> order = by_size(w, kids);
    for (size_t n = 0; n < order.size(); ++n) {
      Work& x = w[static_cast<size_t>(order[n])];
      if (x.box.empty || x.axial) continue;  // a fastener leaves its hole along its axis
      for (size_t m = n; m-- > 0;) {  // the smallest larger one first
        const Work& c = w[static_cast<size_t>(order[m])];
        if (c.box.empty || c.box.diag() <= x.box.diag() || c.solid < 0.2 * c.box.volume() || !x.box.within(c.box, spec.touch)) continue;
        Vec3 out = mul(c.u.dir, -1);
        if (order[m] == base) {
          const Vec3 rest = sub(pc, c.box.centre());
          if (spec.mode == "axis") out = dot(rest, a) < 0 ? mul(a, -1) : a;
          else out = norm(rest) > 1e-6 * std::max(1.0, c.box.diag()) ? unit(rest) : x.u.dir;
        }
        x.u.dir = out;
        x.u.distance = 0;
        break;
      }
    }
  }

  // No part stays buried in a larger sibling (a board in its shell): larger ones first, each one goes on along its
  // direction until its box is a gap clear of every one placed before it.
  void clear(const std::vector<int>& kids, double gap) {
    const std::vector<int> order = by_size(w, kids);
    auto moved = [](const Work& x) {
      Box b = x.box;
      b.lo = opad::add(b.lo, mul(x.u.dir, x.u.distance));
      b.hi = opad::add(b.hi, mul(x.u.dir, x.u.distance));
      return b;
    };
    std::vector<int> placed;
    for (int k : order) {
      Work& x = w[static_cast<size_t>(k)];
      if (x.box.empty) continue;
      for (size_t round = 0; round <= placed.size(); ++round) {
        bool pushed = false;
        for (int j : placed) {
          const Box a = moved(x), b = moved(w[static_cast<size_t>(j)]);
          bool overlap = true;
          for (size_t i = 0; i < 3; ++i) overlap = overlap && std::min(a.hi[i], b.hi[i]) - std::max(a.lo[i], b.lo[i]) > spec.touch;
          if (!overlap) continue;
          double need = HUGE_VAL;  // the least distance that parts the two along one axis
          for (size_t i = 0; i < 3; ++i) {
            if (x.u.dir[i] > 1e-9) need = std::min(need, (b.hi[i] + gap - x.box.lo[i]) / x.u.dir[i]);
            else if (x.u.dir[i] < -1e-9) need = std::min(need, (x.box.hi[i] - b.lo[i] + gap) / -x.u.dir[i]);
          }
          if (need > x.u.distance && need < HUGE_VAL) {
            x.u.distance = need;
            pushed = true;
          }
        }
        if (!pushed) break;
      }
      placed.push_back(k);
    }
  }

  // Piles the children up along a, lowest first: each one starts a gap above every earlier one it overlaps across a
  // (bottom shell, board, top shell); side-by-side parts keep their height.
  void stack(const std::vector<int>& kids, const Vec3& a, double gap) {
    const Vec3 u = unit(cross(std::abs(a[2]) < 0.9 ? Vec3{0, 0, 1} : Vec3{1, 0, 0}, a)), v = cross(a, u);
    struct Span { double a0, a1, u0, u1, v0, v1; };
    std::vector<Span> s(kids.size());
    std::vector<size_t> order;
    for (size_t i = 0; i < kids.size(); ++i) {
      const Box& b = w[static_cast<size_t>(kids[i])].box;
      if (b.empty) continue;
      b.span(a, s[i].a0, s[i].a1);
      b.span(u, s[i].u0, s[i].u1);
      b.span(v, s[i].v0, s[i].v1);
      order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
      return s[x].a0 != s[y].a0 ? s[x].a0 < s[y].a0 : s[x].a0 + s[x].a1 < s[y].a0 + s[y].a1;
    });
    std::vector<double> top(kids.size(), 0);
    for (size_t n = 0; n < order.size(); ++n) {
      const size_t i = order[n];
      double start = s[i].a0;
      for (size_t m = 0; m < n; ++m) {
        const size_t j = order[m];
        const bool over = std::min(s[i].u1, s[j].u1) - std::max(s[i].u0, s[j].u0) > spec.touch &&
                          std::min(s[i].v1, s[j].v1) - std::max(s[i].v0, s[j].v0) > spec.touch;
        if (over) start = std::max(start, top[j] + gap);
      }
      Work& x = w[static_cast<size_t>(kids[i])];
      x.u.dir = a;
      x.u.distance = start - s[i].a0;
      top[i] = s[i].a1 + x.u.distance;
    }
  }

  void flatten(int u, int parent, std::vector<ExplodeUnit>& out) const {
    const Work& x = w[static_cast<size_t>(u)];
    int self = parent;
    if (u != 0) {
      out.push_back(x.u);
      out.back().parent = parent;
      out.back().level = parent < 0 ? 1 : out[static_cast<size_t>(parent)].level + 1;
      self = static_cast<int>(out.size()) - 1;
    }
    for (int k : x.kids)
      if (!w[static_cast<size_t>(k)].dead) flatten(k, self, out);
  }
};

const ViewBookmark* find_view(const Scene& scene, const std::string& id) {
  for (const auto& v : scene.views)
    if (v.id == id) return &v;
  throw Error("explode: no view " + id);
}

}  // namespace

json ExplodeSpec::to_json() const {
  const ExplodeSpec d;
  json j = json::object();
  if (!root.empty()) j["root"] = root;
  j["levels"] = levels;
  j["mode"] = mode;
  if (mode != "radial") j["axis"] = axis;
  j["spacing"] = spacing;
  j["attach_small"] = attach_small;
  if (fasteners) j["fasteners"] = true;
  if (small_ratio != d.small_ratio) j["small_ratio"] = small_ratio;
  if (small_size != d.small_size) j["small_size"] = small_size;
  if (touch != d.touch) j["touch"] = touch;
  if (!keep.empty()) j["keep"] = keep;
  if (!split.empty()) j["split"] = split;
  if (!groups.empty()) j["groups"] = groups;
  if (!offsets.empty()) {
    json o = json::object();
    for (const auto& [id, v] : offsets) o[id] = v;
    j["offsets"] = o;
  }
  j["stages"] = stages;  // always: what a spec without it means has changed once (TODO 11 D4)
  if (duration != d.duration) j["duration"] = duration;
  j["t"] = t;
  return j;
}

ExplodeSpec ExplodeSpec::from_json(const json& j) {
  if (!j.is_object()) bad("must be an object");
  ExplodeSpec s;
  if (j.contains("root") && !j["root"].is_null()) {
    if (!j["root"].is_string()) bad("root must be a component id");
    s.root = j["root"].get<std::string>();
  }
  if (j.contains("levels")) {
    if (!j["levels"].is_number_integer() || j["levels"].get<int>() < 0) bad("levels must be 0 (every level) or more");
    s.levels = j["levels"].get<int>();
  }
  if (j.contains("mode")) {
    s.mode = j["mode"].is_string() ? j["mode"].get<std::string>() : "";
    if (s.mode != "radial" && s.mode != "axis" && s.mode != "stack") bad("mode is radial, axis or stack");
  }
  if (j.contains("axis")) {
    s.axis = vec_of(j["axis"], "axis");
    if (norm(s.axis) < 1e-12) bad("axis must not be zero");
  }
  s.spacing = number_of(j, "spacing", s.spacing, 0, 1000);
  if (j.contains("attach_small")) {
    if (!j["attach_small"].is_boolean()) bad("attach_small must be true or false");
    s.attach_small = j["attach_small"].get<bool>();
  }
  if (j.contains("fasteners")) {
    if (!j["fasteners"].is_boolean()) bad("fasteners must be true or false");
    s.fasteners = j["fasteners"].get<bool>();
  }
  s.small_ratio = number_of(j, "small_ratio", s.small_ratio, 0, 1);
  s.small_size = number_of(j, "small_size", s.small_size, 0, 1e9);
  s.touch = number_of(j, "touch", s.touch, 0, 1e9);
  if (j.contains("keep"))
    for (const auto& id : ids_of(j["keep"], "keep")) s.keep.insert(id);
  if (j.contains("split"))
    for (const auto& id : ids_of(j["split"], "split")) s.split.insert(id);
  if (j.contains("groups")) {
    if (!j["groups"].is_array()) bad("groups must be a list of node id lists");
    for (const auto& g : j["groups"]) {
      s.groups.push_back(ids_of(g, "a group"));
      if (s.groups.back().empty()) bad("a group needs at least one node");
    }
  }
  if (j.contains("offsets")) {
    if (!j["offsets"].is_object()) bad("offsets must be {unit id: [x, y, z]}");
    for (const auto& [id, v] : j["offsets"].items()) s.offsets[id] = vec_of(v, "offset of " + id);
  }
  if (j.contains("stages")) {
    s.stages = j["stages"].is_string() ? j["stages"].get<std::string>() : "";
    if (s.stages == "levels") s.stages = "together";  // no longer staged by level (TODO 11 D4)
    if (s.stages != "together" && s.stages != "units") bad("stages is together or units");
  }
  s.duration = number_of(j, "duration", s.duration, 0, 600);
  s.t = number_of(j, "t", s.t, 0, 1);
  return s;
}

std::string explode_root(const Scene& scene, const ExplodeSpec& spec) {
  std::string root = spec.root;
  const Node* r = root.empty() ? nullptr : scene.node(root);
  if (!r || r->kind != Node::Kind::Component) root.clear();  // gone since the view was saved: the whole model
  for (;;) {
    std::string only;
    int count = 0;
    for (const auto& c : children_of(scene, root))
      if (shown(scene.node(c))) {
        only = c;
        ++count;
      }
    if (count != 1 || scene.node(only)->kind != Node::Kind::Component || spec.keep.count(only) || grouped(spec, only)) return root;
    root = only;
  }
}

int explode_depth(const Scene& scene, const ExplodeSpec& spec) {
  std::function<int(const std::string&)> depth = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (!shown(n)) return 0;
    int d = 0;
    for (const auto& c : n->children) d = std::max(d, depth(c));
    return d + 1;
  };
  int out = 0;
  const std::string root = explode_root(scene, spec);
  for (const auto& c : children_of(scene, root)) out = std::max(out, depth(c));
  return out;
}

std::optional<Vec3> fastener_axis(const TopoDS_Shape& shape) {
  if (shape.IsNull()) return std::nullopt;
  struct Group {
    gp_Ax1 axis;
    double radius = 0, area = 0, lo = HUGE_VAL, hi = -HUGE_VAL;
  };
  std::vector<Group> groups;
  int faces = 0;
  for (TopExp_Explorer f(shape, TopAbs_FACE); f.More(); f.Next()) {
    if (++faces > 2000) return std::nullopt;  // no fastener; bounded cost
    const TopoDS_Face& face = TopoDS::Face(f.Current());
    const BRepAdaptor_Surface s(face, false);
    if (s.GetType() != GeomAbs_Cylinder) continue;
    const gp_Cylinder c = s.Cylinder();
    double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
    BRepTools::UVBounds(face, u0, u1, v0, v1);
    const double r = c.Radius(), tol = 1e-6 * std::max(1.0, r);
    auto g = std::find_if(groups.begin(), groups.end(), [&](const Group& x) {
      return x.axis.Direction().IsParallel(c.Axis().Direction(), 1e-4) && gp_Lin(x.axis).Distance(c.Location()) < 1e3 * tol;
    });
    if (g == groups.end()) g = groups.insert(groups.end(), Group{c.Axis()});
    const gp_Dir d = g->axis.Direction();
    const double base = gp_Vec(g->axis.Location(), c.Location()).Dot(gp_Vec(d)), turn = c.Axis().Direction().Dot(d);
    g->radius = std::max(g->radius, r);
    g->area += r * std::min(u1 - u0, 2 * M_PI) * (v1 - v0);
    g->lo = std::min({g->lo, base + v0 * turn, base + v1 * turn});
    g->hi = std::max({g->hi, base + v0 * turn, base + v1 * turn});
  }
  if (groups.empty()) return std::nullopt;
  const Group& best = *std::max_element(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.area < b.area; });
  // How far the part reaches along the axis and around it: its vertices (a cylinder's seam ends sit on its radius).
  const gp_Vec d(best.axis.Direction());
  double lo = best.lo, hi = best.hi, around = best.radius;
  for (TopExp_Explorer v(shape, TopAbs_VERTEX); v.More(); v.Next()) {
    const gp_Vec to(best.axis.Location(), BRep_Tool::Pnt(TopoDS::Vertex(v.Current())));
    const double along = to.Dot(d);
    lo = std::min(lo, along);
    hi = std::max(hi, along);
    around = std::max(around, (to - d * along).Magnitude());
  }
  if (best.radius < 0.6 * around || hi - lo < 3 * best.radius) return std::nullopt;  // a hole in a plate, a disc
  Vec3 a{d.X(), d.Y(), d.Z()};
  size_t big = 0;
  for (size_t i = 1; i < 3; ++i)
    if (std::abs(a[i]) > std::abs(a[big]) + 1e-9) big = i;
  return a[big] < 0 ? mul(a, -1) : a;
}

ExplodeAxisFn fastener_axes(const Document& doc, const Scene& scene, std::shared_ptr<FastenerAxes> cache) {
  if (!cache) cache = std::make_shared<FastenerAxes>();
  return [&doc, &scene, cache](const std::string& id) -> std::optional<Vec3> {
    const Node* n = scene.node(id);
    if (!n) return std::nullopt;
    std::optional<Vec3> a;
    bool known = false;
    {
      std::lock_guard<std::mutex> lock(cache->mu);
      if (const auto it = cache->by_key.find(n->body_key); it != cache->by_key.end()) {
        a = it->second;
        known = true;
      }
    }
    if (!known) {
      try {
        a = fastener_axis(body_shape(doc, n->body_key));
      } catch (const std::exception&) {
      }
      std::lock_guard<std::mutex> lock(cache->mu);
      cache->by_key.emplace(n->body_key, a);
    }
    if (!a) return std::nullopt;
    const Mat4 m = scene.world(id);
    Vec3 out{0, 0, 0};
    for (int r = 0; r < 3; ++r) out[static_cast<size_t>(r)] = m.at(r, 0) * (*a)[0] + m.at(r, 1) * (*a)[1] + m.at(r, 2) * (*a)[2];
    return unit(out);
  };
}

std::vector<ExplodeUnit> explode_units(const Document& doc, const Scene& scene, const ExplodeSpec& spec, const ExplodeBoxFn& box_of,
                                       const ExplodeAxisFn& axis_of) {
  Builder b(scene, spec);
  const std::string root = explode_root(scene, spec);
  for (const auto& c : children_of(scene, root)) b.place(0, c, 1, false);

  std::vector<std::string> bodies;
  for (const auto& x : b.w) bodies.insert(bodies.end(), x.u.bodies.begin(), x.u.bodies.end());
  ExplodeBoxFn box = box_of;
  if (!box) {
    std::vector<std::string> keys;
    for (const auto& id : bodies) keys.push_back(scene.node(id)->body_key);
    warm_tight_bboxes(doc, keys);
    box = [&](const std::string& id) {
      Bnd_Box out;
      try {
        out = node_tight_bbox(doc, scene, id, false);
        if (out.IsVoid()) out = node_world_bbox(doc, scene, id);  // a mesh: the box of its triangles
      } catch (const std::exception&) {
      }
      return out;
    };
  }
  std::unordered_map<std::string, Box> boxes;
  for (const auto& id : bodies) boxes.emplace(id, Box::of(box(id)));
  for (size_t i = b.w.size(); i-- > 0;) {  // children come after their parents: bottom-up
    auto& x = b.w[i];
    for (const auto& id : x.u.bodies) {
      x.box.add(boxes[id]);
      x.solid += boxes[id].volume();
      if (boxes[id].diag() > x.main.diag()) x.main = boxes[id];
    }
    std::vector<int> live;
    for (int k : x.kids) {
      const auto& kid = b.w[static_cast<size_t>(k)];
      if (kid.u.bodies.empty() && kid.kids.empty()) continue;  // only hidden parts under it
      x.box.add(kid.box);
      x.solid += kid.solid;
      if (kid.main.diag() > x.main.diag()) x.main = kid.main;
      live.push_back(k);
    }
    x.kids = live;
  }
  for (size_t i = 0; i < b.w.size(); ++i)
    if (!b.w[i].dead && spec.attach_small) b.attach(static_cast<int>(i));
  if (spec.fasteners && spec.mode == "radial") {  // units of fasteners that lie the same way
    const ExplodeAxisFn axis = axis_of ? axis_of : fastener_axes(doc, scene);
    for (size_t i = 1; i < b.w.size(); ++i) {
      auto& x = b.w[i];
      std::vector<std::string> under;
      if (!x.dead) b.bodies_under(static_cast<int>(i), under);
      if (under.empty() || under.size() > 64) continue;
      std::optional<Vec3> first;
      for (const auto& id : under) {
        const std::optional<Vec3> a = axis(id);
        if (!a || (first && std::abs(dot(*a, *first)) < 0.999)) {
          first.reset();
          break;
        }
        if (!first) first = a;
      }
      if (first) {
        x.axial = true;
        x.axis = *first;
      }
    }
  }
  for (size_t i = 0; i < b.w.size(); ++i)
    if (!b.w[i].dead) b.spread(static_cast<int>(i));

  std::vector<ExplodeUnit> units;
  b.flatten(0, -1, units);
  explode_stage(units, spec);
  return units;
}

void explode_stage(std::vector<ExplodeUnit>& units, const ExplodeSpec& spec) {
  for (auto& u : units) {
    u.t0 = 0;
    u.t1 = 1;
  }
  if (spec.stages != "units") return;
  // One after another: the farthest move first, a unit never before the moving unit that holds it (the screws leave the
  // lid once it is out). The order follows the moves, not the levels (TODO 11 D4).
  std::vector<double> travel(units.size(), 0);
  std::vector<char> moves(units.size(), 0);
  for (size_t i = 0; i < units.size(); ++i) {
    Vec3 own = mul(units[i].dir, units[i].distance);
    if (const auto m = spec.offsets.find(units[i].id); m != spec.offsets.end()) own = add(own, m->second);
    travel[i] = norm(own);
    moves[i] = units[i].distance > 0 || spec.offsets.count(units[i].id);
  }
  auto later = [&](size_t x, size_t y) { return travel[x] != travel[y] ? travel[x] < travel[y] : x > y; };  // heap: farthest, then tree order
  std::vector<size_t> ready;
  std::vector<std::vector<size_t>> after(units.size());
  for (size_t i = 0; i < units.size(); ++i) {
    if (!moves[i]) continue;
    int above = units[i].parent;
    while (above >= 0 && !moves[static_cast<size_t>(above)]) above = units[static_cast<size_t>(above)].parent;
    (above < 0 ? ready : after[static_cast<size_t>(above)]).push_back(i);
  }
  std::make_heap(ready.begin(), ready.end(), later);
  std::vector<size_t> order;
  while (!ready.empty()) {
    std::pop_heap(ready.begin(), ready.end(), later);
    const size_t i = ready.back();
    ready.pop_back();
    order.push_back(i);
    for (const size_t c : after[i]) {
      ready.push_back(c);
      std::push_heap(ready.begin(), ready.end(), later);
    }
  }
  for (size_t n = 0; n < order.size(); ++n) {
    units[order[n]].t0 = double(n) / order.size();
    units[order[n]].t1 = double(n + 1) / order.size();
  }
}

double explode_progress(const ExplodeUnit& u, double t) {
  const double x = std::clamp(u.t1 > u.t0 ? (t - u.t0) / (u.t1 - u.t0) : (t >= u.t1 ? 1 : 0), 0.0, 1.0);
  return x * x * (3 - 2 * x);
}

std::vector<Vec3> explode_unit_offsets(const std::vector<ExplodeUnit>& units, const ExplodeSpec& spec, double t) {
  std::vector<Vec3> out(units.size(), Vec3{0, 0, 0});
  for (size_t i = 0; i < units.size(); ++i) {
    const ExplodeUnit& u = units[i];
    Vec3 own = mul(u.dir, u.distance);
    if (auto m = spec.offsets.find(u.id); m != spec.offsets.end()) own = add(own, m->second);
    out[i] = add(u.parent >= 0 ? out[static_cast<size_t>(u.parent)] : Vec3{0, 0, 0}, mul(own, explode_progress(u, t)));
  }
  return out;
}

std::unordered_map<std::string, Vec3> explode_offsets(const std::vector<ExplodeUnit>& units, const ExplodeSpec& spec, double t) {
  const std::vector<Vec3> moves = explode_unit_offsets(units, spec, t);
  std::unordered_map<std::string, Vec3> out;
  for (size_t i = 0; i < units.size(); ++i)
    for (const auto& id : units[i].bodies) out[id] = moves[i];
  return out;
}

Scene exploded_scene(const Scene& scene, const std::unordered_map<std::string, Vec3>& offsets) {
  Scene out = scene;
  for (const auto& [id, v] : offsets) {
    auto it = out.nodes.find(id);
    if (it == out.nodes.end() || (v[0] == 0 && v[1] == 0 && v[2] == 0)) continue;
    // world' = T(v) * world takes local' = T(A^-1 v) * local, A the linear part of the parent's world placement.
    const Mat4 p = it->second.parent.empty() ? Mat4::identity() : scene.world(it->second.parent);
    const Vec3 c0{p.at(0, 0), p.at(1, 0), p.at(2, 0)}, c1{p.at(0, 1), p.at(1, 1), p.at(2, 1)}, c2{p.at(0, 2), p.at(1, 2), p.at(2, 2)};
    const double det = dot(c0, cross(c1, c2));
    if (std::abs(det) < 1e-15) continue;
    const Vec3 l{dot(v, cross(c1, c2)) / det, dot(c0, cross(v, c2)) / det, dot(c0, cross(c1, v)) / det};
    it->second.local = Mat4::translation(l[0], l[1], l[2]) * it->second.local;
  }
  return out;
}

ExplodeSpec view_explode(const Scene& scene, const std::string& view_id) {
  const ViewBookmark* v = find_view(scene, view_id);
  return v->explode.is_object() ? ExplodeSpec::from_json(v->explode) : ExplodeSpec{};
}

std::vector<ExplodeTrail> explode_trails(const std::vector<ExplodeUnit>& units, const ExplodeSpec& spec, double t) {
  const std::vector<Vec3> moves = explode_unit_offsets(units, spec, t);
  std::vector<ExplodeTrail> out;
  for (size_t i = 0; i < units.size(); ++i) {
    const Vec3 base = units[i].parent >= 0 ? moves[static_cast<size_t>(units[i].parent)] : Vec3{0, 0, 0};
    const Vec3 own = sub(moves[i], base);
    if (norm(own) <= 1e-6 * std::max(1.0, norm(sub(units[i].hi, units[i].lo)))) continue;
    out.push_back({i, add(units[i].centre, base), add(units[i].centre, moves[i])});
  }
  return out;
}

std::unordered_map<std::string, int> explode_body_units(const std::vector<ExplodeUnit>& units) {
  std::unordered_map<std::string, int> of;
  for (size_t i = 0; i < units.size(); ++i)
    for (const auto& b : units[i].bodies) of.emplace(b, static_cast<int>(i));
  return of;
}

int explode_unit_of(const Scene& scene, const std::vector<ExplodeUnit>& units, const std::string& id, const std::unordered_map<std::string, int>* body_units) {
  std::unordered_map<std::string, int> local;
  if (!body_units) body_units = &(local = explode_body_units(units));
  const Node* n = scene.node(id);
  if (n && n->kind == Node::Kind::Body) {
    const auto it = body_units->find(id);
    return it == body_units->end() ? -1 : it->second;
  }
  for (size_t i = 0; i < units.size(); ++i)
    if (units[i].id == id) return static_cast<int>(i);
  if (!n) return -1;
  int found = -1;
  for (const auto& b : scene.bodies_under(id)) {
    const auto it = body_units->find(b);
    if (it == body_units->end()) continue;  // hidden
    if (found >= 0 && it->second != found) return -1;
    found = it->second;
  }
  return found;
}

double explode_travel(const ExplodeUnit& u, const ExplodeSpec& spec, const Vec3& axis) {
  Vec3 own = mul(u.dir, u.distance);
  if (const auto m = spec.offsets.find(u.id); m != spec.offsets.end()) own = add(own, m->second);
  return dot(own, unit(axis));
}

void set_explode_travel(ExplodeSpec& spec, const ExplodeUnit& u, const Vec3& axis, double travel) {
  const Vec3 a = unit(axis);
  Vec3 manual = spec.offsets.count(u.id) ? spec.offsets[u.id] : Vec3{0, 0, 0};
  manual = add(manual, mul(a, travel - explode_travel(u, spec, a)));
  if (norm(manual) < 1e-9) spec.offsets.erase(u.id);
  else spec.offsets[u.id] = manual;
}

ExplodeRule explode_rule(const ExplodeSpec& spec, const std::string& component) {
  return spec.keep.count(component) ? ExplodeRule::Keep : spec.split.count(component) ? ExplodeRule::Split : ExplodeRule::Level;
}

void set_explode_rule(ExplodeSpec& spec, const std::string& component, ExplodeRule rule) {
  spec.keep.erase(component);
  spec.split.erase(component);
  if (rule == ExplodeRule::Keep) spec.keep.insert(component);
  else if (rule == ExplodeRule::Split) spec.split.insert(component);
}

int explode_group_of(const ExplodeSpec& spec, const std::string& id) {
  for (size_t g = 0; g < spec.groups.size(); ++g)
    if (std::find(spec.groups[g].begin(), spec.groups[g].end(), id) != spec.groups[g].end()) return static_cast<int>(g);
  return -1;
}

void explode_group(ExplodeSpec& spec, const std::vector<std::string>& ids) {
  std::vector<std::string> members;
  for (const auto& id : ids)
    if (std::find(members.begin(), members.end(), id) == members.end()) members.push_back(id);
  if (members.size() < 2) return;
  for (auto& g : spec.groups)
    g.erase(std::remove_if(g.begin(), g.end(), [&](const std::string& id) { return std::find(members.begin(), members.end(), id) != members.end(); }), g.end());
  spec.groups.erase(std::remove_if(spec.groups.begin(), spec.groups.end(), [](const std::vector<std::string>& g) { return g.size() < 2; }), spec.groups.end());
  for (size_t i = 1; i < members.size(); ++i) spec.offsets.erase(members[i]);
  spec.groups.push_back(members);
}

bool explode_ungroup(ExplodeSpec& spec, const std::string& id) {
  const int g = explode_group_of(spec, id);
  if (g < 0) return false;
  spec.offsets.erase(spec.groups[static_cast<size_t>(g)].front());
  spec.groups.erase(spec.groups.begin() + g);
  return true;
}

Scene exploded_scene(const Document& doc, const Scene& scene, const json& explode) {
  ExplodeSpec spec;
  if (explode.is_string()) {
    const ViewBookmark* v = find_view(scene, explode.get<std::string>());
    if (!v->explode.is_object()) throw Error("explode: view \"" + v->name + "\" is not an exploded view");
    spec = ExplodeSpec::from_json(v->explode);
  } else {
    spec = ExplodeSpec::from_json(explode);
  }
  return exploded_scene(scene, explode_offsets(explode_units(doc, scene, spec), spec, spec.t));
}

}  // namespace opad
