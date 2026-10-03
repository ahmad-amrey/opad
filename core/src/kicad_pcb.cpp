// KiCad boards from the published s-expression format (dev-docs.kicad.org, "File formats"); no KiCad code is used.
// The page has Y down and OPAD has Y up: a page point (x, y) lands at (x - ox, oy - y) for the chosen origin. Angles are
// degrees, counter-clockwise as seen on the page. A footprint's pads and graphics are stored in its own frame (pads'
// angles include the footprint's), already mirrored when it sits on the bottom. The board is one prism of its Edge.Cuts
// outline with the drills as holes (no booleans unless a hole crosses an edge or another hole); every footprint with a
// 3D model is a component; each model file is read once (through import_file) and its bodies are shared by every
// footprint using it.
#include "opad/kicad_pcb.hpp"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRep_Builder.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <Geom_BezierCurve.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Circ.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#ifdef OPAD_HAVE_ZSTD
#include <zstd.h>
#endif

#include "import_common.hpp"
#include "opad/cache.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/scene.hpp"

namespace opad {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kLift = 0.01;
constexpr size_t kMaxCut = 200;  // drills cut by a boolean (across an edge or another drill)

// ---------------------------------------------------------------- s-expressions
struct Sx {
  std::string atom;
  std::vector<Sx> items;  // a list: items[0] names it
  bool list = false;
  const std::string& name() const {
    static const std::string none;
    return list && !items.empty() && !items[0].list ? items[0].atom : none;
  }
  const Sx* child(std::string_view n) const {
    for (const auto& i : items)
      if (i.list && i.name() == n) return &i;
    return nullptr;
  }
  template <class F>
  void each(std::string_view n, F&& f) const {
    for (const auto& i : items)
      if (i.list && i.name() == n) f(i);
  }
  bool has(std::string_view a) const {
    for (size_t k = 1; k < items.size(); ++k)
      if (!items[k].list && items[k].atom == a) return true;
    return false;
  }
  const std::string& text(size_t k) const {
    static const std::string none;
    return k < items.size() && !items[k].list ? items[k].atom : none;
  }
  double num(size_t k, double fallback = 0) const {
    const std::string& t = text(k);
    double v = 0;
    const char* first = t.data() + (!t.empty() && t[0] == '+');
    const auto r = std::from_chars(first, t.data() + t.size(), v);
    return r.ec == std::errc() && std::isfinite(v) ? v : fallback;
  }
};

// Top-level records nothing here needs (copper, zones, text, embedded fonts and files): skipped unparsed.
const std::set<std::string, std::less<>> kSkipped = {"net",      "segment",        "arc",   "zone",   "gr_text", "gr_text_box", "dimension", "group",
                                                     "generated", "embedded_fonts", "embedded_files", "image", "target", "title_block", "property", "layers"};

class Parser {
 public:
  explicit Parser(std::string_view text) : s(text) {}
  size_t embedded = std::string::npos;  // where the board's (skipped) embedded files start
  Sx board() {
    space();
    if (at >= s.size() || s[at] != '(') throw Error("not a KiCad board (no s-expression)");
    Sx root = list(0);
    if (root.name() != "kicad_pcb") throw Error("not a KiCad board: the file holds a '" + root.name() + "'");
    return root;
  }
  Sx record(size_t from) {  // one list, read in full
    at = from;
    return list(1);
  }

 private:
  std::string_view s;
  size_t at = 0;
  void space() {
    while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n')) ++at;
  }
  std::string atom() {
    std::string out;
    if (s[at] == '"') {
      for (++at; at < s.size() && s[at] != '"'; ++at) {
        if (s[at] == '\\' && at + 1 < s.size()) {
          ++at;
          out += s[at] == 'n' ? '\n' : s[at];
        } else {
          out += s[at];
        }
      }
      if (at >= s.size()) throw Error("KiCad board: a quoted text does not end");
      ++at;
      return out;
    }
    const size_t from = at;
    while (at < s.size() && s[at] != ' ' && s[at] != '\t' && s[at] != '\r' && s[at] != '\n' && s[at] != '(' && s[at] != ')') ++at;
    return std::string(s.substr(from, at - from));
  }
  void skip() {  // past the ')' that closes the list whose name was just read
    for (int depth = 1; at < s.size() && depth > 0;) {
      const char c = s[at++];
      if (c == '"') {
        while (at < s.size() && s[at] != '"') at += s[at] == '\\' ? 2 : 1;
        ++at;
      } else if (c == '(') {
        ++depth;
      } else if (c == ')') {
        --depth;
      }
    }
  }
  Sx list(int depth) {
    if (depth > 200) throw Error("KiCad board: lists nest too deeply");
    ++at;
    Sx node;
    node.list = true;
    for (;;) {
      space();
      if (at >= s.size()) throw Error("KiCad board: the file ends inside a list");
      if (s[at] == ')') {
        ++at;
        return node;
      }
      if (s[at] != '(') {
        node.items.emplace_back().atom = atom();
        continue;
      }
      if (depth == 0) {  // a record of the board: read its name first, skip what is not needed
        const size_t mark = at++;
        space();
        if (at < s.size() && s[at] != '(' && s[at] != ')') {
          const std::string name = atom();
          if (name == "embedded_files" && embedded == std::string::npos) embedded = mark;
          if (kSkipped.count(name)) {
            skip();
            continue;
          }
        }
        at = mark;
      }
      node.items.push_back(list(depth + 1));
    }
  }
};

// ---------------------------------------------------------------- 2D pieces
using P2 = std::array<double, 2>;

double dist(P2 a, P2 b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }
P2 at2(const Sx* n) { return n ? P2{n->num(1), n->num(2)} : P2{0, 0}; }

// A footprint's place on the page: maps its own (unturned) coordinates to the page's.
struct Place {
  double x = 0, y = 0, angle = 0;
  P2 operator()(P2 p) const {
    const double a = angle * kPi / 180, c = std::cos(a), s = std::sin(a);
    return {x + p[0] * c + p[1] * s, y - p[0] * s + p[1] * c};
  }
};
P2 turn(P2 p, P2 about, double angle) { return Place{about[0], about[1], angle}({p[0] - about[0], p[1] - about[1]}); }

// A line (2 points), an arc (start, mid, end), a cubic curve (4 poles) or a circle (centre, a point on it).
struct Seg {
  enum Kind { Line, Arc, Curve, Circle } kind = Line;
  std::vector<P2> p;
};
using Loop = std::vector<Seg>;  // closed: each piece starts where the one before ends

struct Shapes {
  std::vector<Seg> open;   // lines, arcs and curves, chained into loops later
  std::vector<Loop> loops; // circles, rectangles, polygons
};

bool on_layer(const Sx& n, std::string_view layer) {
  const Sx* l = n.child("layer");
  return l && l->text(1) == layer;
}

// A graphic item (gr_* on the board, fp_* in a footprint and then mapped through `place`).
void shape_item(const Sx& n, const Place* place, Shapes& out) {
  const std::string kind = n.name().substr(3);
  auto pt = [&](P2 p) { return place ? (*place)(p) : p; };
  auto point = [&](const char* key) { return pt(at2(n.child(key))); };
  if (kind == "line") {
    const P2 a = point("start"), b = point("end");
    if (dist(a, b) > 1e-6) out.open.push_back({Seg::Line, {a, b}});
  } else if (kind == "arc") {
    if (n.child("mid")) {
      out.open.push_back({Seg::Arc, {point("start"), point("mid"), point("end")}});
    } else {  // before KiCad 6: the centre, where the arc starts and its angle, clockwise on the page
      const P2 c = at2(n.child("start")), s = at2(n.child("end"));
      const double a = n.child("angle") ? n.child("angle")->num(1) : 0;
      if (std::abs(a) >= 359.999) out.loops.push_back({{Seg::Circle, {pt(c), pt(s)}}});
      else if (std::abs(a) > 1e-9) out.open.push_back({Seg::Arc, {pt(s), pt(turn(s, c, -a / 2)), pt(turn(s, c, -a))}});
    }
  } else if (kind == "circle") {
    const P2 c = point("center"), e = point("end");
    if (dist(c, e) > 1e-6) out.loops.push_back({{Seg::Circle, {c, e}}});
  } else if (kind == "rect") {
    const P2 a = at2(n.child("start")), b = at2(n.child("end"));
    if (std::abs(a[0] - b[0]) < 1e-6 || std::abs(a[1] - b[1]) < 1e-6) return;
    const P2 c[4] = {pt(a), pt({b[0], a[1]}), pt(b), pt({a[0], b[1]})};
    Loop l;
    for (int i = 0; i < 4; ++i) l.push_back({Seg::Line, {c[i], c[(i + 1) % 4]}});
    out.loops.push_back(std::move(l));
  } else if (kind == "poly") {
    const Sx* pts = n.child("pts");
    if (!pts) return;
    Loop l;
    std::vector<P2> ends;  // first, last
    auto to = [&](P2 p) {
      if (!ends.empty() && dist(ends.back(), p) > 1e-6) l.push_back({Seg::Line, {ends.back(), p}});
      if (ends.empty()) ends.push_back(p);
      if (ends.size() < 2) ends.push_back(p);
      ends.back() = p;
    };
    for (size_t k = 1; k < pts->items.size(); ++k) {
      const Sx& e = pts->items[k];
      if (e.name() == "xy") {
        to(pt(at2(&e)));
      } else if (e.name() == "arc") {
        const P2 s = pt(at2(e.child("start"))), m = pt(at2(e.child("mid"))), en = pt(at2(e.child("end")));
        to(s);
        l.push_back({Seg::Arc, {s, m, en}});
        ends.back() = en;
      }
    }
    if (ends.size() == 2 && dist(ends.back(), ends.front()) > 1e-6) l.push_back({Seg::Line, {ends.back(), ends.front()}});
    if (l.size() >= 2) out.loops.push_back(std::move(l));
  } else if (kind == "curve") {
    std::vector<P2> poles;
    if (const Sx* pts = n.child("pts")) pts->each("xy", [&](const Sx& xy) { poles.push_back(pt(at2(&xy))); });
    if (poles.size() == 4 && dist(poles[0], poles[3]) > 1e-6) out.open.push_back({Seg::Curve, poles});
  }
}

// Open pieces chained into closed loops: an end within 10 um of the next piece's start or end is joined to it (exactly,
// at the earlier piece's end). Pieces that never close are counted in `unclosed`.
std::vector<Loop> chain(const std::vector<Seg>& open, int& unclosed) {
  constexpr double tol = 0.01;
  struct End {
    double x, y;
    size_t piece;
    bool last;
  };
  std::vector<End> index;
  for (size_t i = 0; i < open.size(); ++i) {
    index.push_back({open[i].p.front()[0], open[i].p.front()[1], i, false});
    index.push_back({open[i].p.back()[0], open[i].p.back()[1], i, true});
  }
  std::sort(index.begin(), index.end(), [](const End& a, const End& b) { return a.x < b.x; });
  std::vector<bool> used(open.size());
  auto nearest = [&](P2 q) -> const End* {
    const End* best = nullptr;
    double bestDistance = tol;
    for (auto it = std::lower_bound(index.begin(), index.end(), q[0] - tol, [](const End& e, double x) { return e.x < x; });
         it != index.end() && it->x <= q[0] + tol; ++it)
      if (!used[it->piece]) {
        const double d = std::hypot(it->x - q[0], it->y - q[1]);
        if (d <= bestDistance) bestDistance = d, best = &*it;
      }
    return best;
  };
  std::vector<Loop> loops;
  for (size_t i = 0; i < open.size(); ++i) {
    if (used[i]) continue;
    used[i] = true;
    Loop loop{open[i]};
    bool closed = false;
    for (;;) {
      const P2 end = loop.back().p.back();
      if (loop.size() > 1 && dist(end, loop.front().p.front()) <= tol) {
        loop.back().p.back() = loop.front().p.front();
        closed = true;
        break;
      }
      const End* next = nearest(end);
      if (!next) break;
      used[next->piece] = true;
      Seg s = open[next->piece];
      if (next->last) std::reverse(s.p.begin(), s.p.end());
      s.p.front() = end;
      if (s.kind != Seg::Line || dist(s.p.front(), s.p.back()) > 1e-6) loop.push_back(std::move(s));
    }
    if (closed && loop.size() >= 2) loops.push_back(std::move(loop));
    else ++unclosed;
  }
  return loops;
}

// The circle through three points; false when they lie on a line.
bool circle3(P2 a, P2 b, P2 c, P2& centre, double& r) {
  const double d = 2 * (a[0] * (b[1] - c[1]) + b[0] * (c[1] - a[1]) + c[0] * (a[1] - b[1]));
  if (std::abs(d) <= 1e-9 * dist(a, b) * dist(b, c)) return false;
  const double a2 = a[0] * a[0] + a[1] * a[1], b2 = b[0] * b[0] + b[1] * b[1], c2 = c[0] * c[0] + c[1] * c[1];
  centre = {(a2 * (b[1] - c[1]) + b2 * (c[1] - a[1]) + c2 * (a[1] - b[1])) / d, (a2 * (c[0] - b[0]) + b2 * (a[0] - c[0]) + c2 * (b[0] - a[0])) / d};
  r = dist(centre, a);
  return true;
}

// Points along a loop (arcs every 2 degrees, curves in 24 steps), for areas, nesting and distances.
std::vector<P2> polygon(const Loop& loop) {
  std::vector<P2> out;
  for (const Seg& s : loop) {
    if (s.kind == Seg::Circle) {
      const double r = dist(s.p[0], s.p[1]);
      for (int i = 0; i < 180; ++i) out.push_back({s.p[0][0] + r * std::cos(i * kPi / 90), s.p[0][1] + r * std::sin(i * kPi / 90)});
      continue;
    }
    out.push_back(s.p.front());
    P2 c;
    double r;
    if (s.kind == Seg::Arc && circle3(s.p[0], s.p[1], s.p[2], c, r)) {
      auto angle = [&](P2 p) { return std::atan2(p[1] - c[1], p[0] - c[0]); };
      auto wrap = [](double a) { return a < 0 ? a + 2 * kPi : a; };
      const double a0 = angle(s.p[0]), m = wrap(angle(s.p[1]) - a0), e = wrap(angle(s.p[2]) - a0), sweep = m < e ? e : e - 2 * kPi;
      const int n = std::max(2, static_cast<int>(std::ceil(std::abs(sweep) / (kPi / 90))));
      for (int i = 1; i < n; ++i) out.push_back({c[0] + r * std::cos(a0 + sweep * i / n), c[1] + r * std::sin(a0 + sweep * i / n)});
    } else if (s.kind == Seg::Curve) {
      for (int i = 1; i < 24; ++i) {
        const double t = i / 24.0, u = 1 - t, w[4] = {u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t};
        out.push_back({w[0] * s.p[0][0] + w[1] * s.p[1][0] + w[2] * s.p[2][0] + w[3] * s.p[3][0], w[0] * s.p[0][1] + w[1] * s.p[1][1] + w[2] * s.p[2][1] + w[3] * s.p[3][1]});
      }
    }
  }
  return out;
}

double area(const std::vector<P2>& poly) {  // counter-clockwise positive (Y up)
  double a = 0;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) a += poly[j][0] * poly[i][1] - poly[i][0] * poly[j][1];
  return a / 2;
}
bool inside(const std::vector<P2>& poly, P2 q) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    if ((poly[i][1] > q[1]) != (poly[j][1] > q[1]) && q[0] < (poly[j][0] - poly[i][0]) * (q[1] - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0])
      in = !in;
  return in;
}
double to_edges(const std::vector<P2>& poly, P2 q) {
  double best = 1e300;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    const double dx = poly[i][0] - poly[j][0], dy = poly[i][1] - poly[j][1], len2 = dx * dx + dy * dy;
    const double t = len2 > 0 ? std::clamp(((q[0] - poly[j][0]) * dx + (q[1] - poly[j][1]) * dy) / len2, 0.0, 1.0) : 0.0;
    best = std::min(best, std::hypot(poly[j][0] + t * dx - q[0], poly[j][1] + t * dy - q[1]));
  }
  return best;
}

// A drill: centre, size along its own x and y, and its angle.
struct Hole {
  P2 c;
  double w, h, angle;
  double reach() const { return std::max(w, h) / 2; }
};

Loop hole_loop(const Hole& h) {  // counter-clockwise
  if (std::abs(h.w - h.h) < 1e-6) return {{Seg::Circle, {h.c, {h.c[0] + h.w / 2, h.c[1]}}}};
  const double r = std::min(h.w, h.h) / 2, l = std::abs(h.w - h.h) / 2, a = (h.angle + (h.w > h.h ? 0 : 90)) * kPi / 180;
  const double c = std::cos(a), s = std::sin(a);
  auto p = [&](double x, double y) { return P2{h.c[0] + x * c - y * s, h.c[1] + x * s + y * c}; };
  return {{Seg::Line, {p(-l, -r), p(l, -r)}}, {Seg::Arc, {p(l, -r), p(l + r, 0), p(l, r)}}, {Seg::Line, {p(l, r), p(-l, r)}}, {Seg::Arc, {p(-l, r), p(-l - r, 0), p(-l, -r)}}};
}

// ---------------------------------------------------------------- OCCT
gp_Pnt pnt(P2 p, double z = 0) { return gp_Pnt(p[0], p[1], z); }

TopoDS_Edge edge(const Seg& s, const TopoDS_Vertex& a, const TopoDS_Vertex& b) {
  if (s.kind == Seg::Arc) {
    GC_MakeArcOfCircle arc(pnt(s.p[0]), pnt(s.p[1]), pnt(s.p[2]));
    if (arc.IsDone()) return BRepBuilderAPI_MakeEdge(arc.Value(), a, b);
  } else if (s.kind == Seg::Curve) {
    TColgp_Array1OfPnt poles(1, 4);
    for (int i = 0; i < 4; ++i) poles.SetValue(i + 1, pnt(s.p[i]));
    const Handle(Geom_Curve) curve = new Geom_BezierCurve(poles);
    return BRepBuilderAPI_MakeEdge(curve, a, b);
  }
  return BRepBuilderAPI_MakeEdge(a, b);
}
TopoDS_Edge circle_edge(const Seg& s, double z) { return BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(pnt(s.p[0], z), gp::DZ()), dist(s.p[0], s.p[1]))); }

TopoDS_Wire wire(const Loop& loop) {
  BRepBuilderAPI_MakeWire w;
  if (loop.size() == 1 && loop[0].kind == Seg::Circle) {
    w.Add(circle_edge(loop[0], 0));
    return w.Wire();
  }
  std::vector<TopoDS_Vertex> v;
  for (const auto& s : loop) v.push_back(BRepBuilderAPI_MakeVertex(pnt(s.p.front())));
  for (size_t i = 0; i < loop.size(); ++i) w.Add(edge(loop[i], v[i], v[(i + 1) % loop.size()]));
  if (!w.IsDone()) throw Error("the board outline does not form a wire");
  return w.Wire();
}

// Edges of loose pieces, for the 2D layers.
void add_edges(BRep_Builder& b, TopoDS_Compound& c, const std::vector<Seg>& pieces) {
  for (const Seg& s : pieces) {
    try {
      if (s.kind == Seg::Circle) b.Add(c, circle_edge(s, 0));
      else b.Add(c, edge(s, BRepBuilderAPI_MakeVertex(pnt(s.p.front())), BRepBuilderAPI_MakeVertex(pnt(s.p.back()))));
    } catch (const Standard_Failure&) {
    }
  }
}

Mat4 rotation(int axis, double degrees) {
  const double a = degrees * kPi / 180;
  auto snap = [](double v) { return std::abs(v) < 1e-12 ? 0.0 : std::abs(v - 1) < 1e-12 ? 1.0 : std::abs(v + 1) < 1e-12 ? -1.0 : v; };
  const double c = snap(std::cos(a)), s = snap(std::sin(a));
  Mat4 m;
  const int i = (axis + 1) % 3, j = (axis + 2) % 3;
  m.at(i, i) = c, m.at(i, j) = -s, m.at(j, i) = s, m.at(j, j) = c;
  return m;
}

std::string stable_id(const std::string& op, const std::string& what) {
  std::string h = sha256_hex("opad-kicad|" + op + "|" + what);
  h[12] = '5';
  h[16] = "89ab"[std::stoi(h.substr(16, 1), nullptr, 16) & 3];
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

std::string utf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// "R2" before "R10".
bool natural_less(const std::string& a, const std::string& b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
      size_t i2 = i, j2 = j;
      while (i2 < a.size() && std::isdigit(static_cast<unsigned char>(a[i2]))) ++i2;
      while (j2 < b.size() && std::isdigit(static_cast<unsigned char>(b[j2]))) ++j2;
      const auto x = a.substr(i, i2 - i), y = b.substr(j, j2 - j);
      const auto xs = x.find_first_not_of('0'), ys = y.find_first_not_of('0');
      const std::string xn = xs == std::string::npos ? "" : x.substr(xs), yn = ys == std::string::npos ? "" : y.substr(ys);
      if (xn.size() != yn.size()) return xn.size() < yn.size();
      if (xn != yn) return xn < yn;
      i = i2, j = j2;
    } else {
      if (a[i] != b[j]) return a[i] < b[j];
      ++i, ++j;
    }
  }
  return a.size() - i < b.size() - j;
}

// The solder mask's colour by its name in the stackup ("Green", "Matte Black", "#1A5C2BFF").
std::array<double, 3> mask_color(const std::string& name) {
  if (name.size() >= 7 && name[0] == '#') {
    std::array<double, 3> rgb{};
    for (int i = 0; i < 3; ++i) rgb[static_cast<size_t>(i)] = std::strtol(name.substr(1 + 2 * i, 2).c_str(), nullptr, 16) / 255.0;
    return rgb;
  }
  static const std::pair<const char*, std::array<double, 3>> table[] = {
      {"green", {0.07, 0.30, 0.15}}, {"red", {0.60, 0.08, 0.08}},   {"blue", {0.06, 0.20, 0.52}},  {"black", {0.07, 0.07, 0.07}},
      {"white", {0.90, 0.90, 0.88}}, {"yellow", {0.78, 0.72, 0.12}}, {"purple", {0.30, 0.10, 0.40}}, {"orange", {0.85, 0.42, 0.10}}};
  const std::string n = lower(name);
  for (const auto& [key, rgb] : table)
    if (n.find(key) != std::string::npos) return rgb;
  return table[0].second;
}

// ---------------------------------------------------------------- model lookup
std::string env(const std::string& name) {
#ifdef _WIN32
  const std::wstring w(name.begin(), name.end());
  const wchar_t* v = _wgetenv(w.c_str());
  return v && *v ? utf8(std::filesystem::path(v)) : std::string();
#else
  const char* v = std::getenv(name.c_str());
  return v ? v : "";
#endif
}

// KiCad's configuration folders, newest version first.
std::vector<std::filesystem::path> config_dirs() {
  std::filesystem::path root;
#ifdef _WIN32
  if (const auto a = env("APPDATA"); !a.empty()) root = path_from_utf8(a) / "kicad";
#elif defined(__APPLE__)
  if (const auto h = env("HOME"); !h.empty()) root = path_from_utf8(h) / "Library/Preferences/kicad";
#else
  if (const auto x = env("XDG_CONFIG_HOME"); !x.empty()) root = path_from_utf8(x) / "kicad";
  else if (const auto h = env("HOME"); !h.empty()) root = path_from_utf8(h) / ".config/kicad";
#endif
  std::vector<std::pair<double, std::filesystem::path>> found;
  std::error_code e;
  if (!root.empty())
    for (const auto& d : std::filesystem::directory_iterator(root, e))
      if (d.is_directory(e)) {
        const double v = std::atof(d.path().filename().string().c_str());
        if (v > 0) found.push_back({v, d.path()});
      }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<std::filesystem::path> out;
  for (const auto& f : found) out.push_back(f.second);
  return out;
}

// KiCad's own 3D model folders as installed, the given major version first.
std::vector<std::filesystem::path> install_dirs(int major) {
  std::vector<std::filesystem::path> out;
#ifdef _WIN32
  std::vector<std::filesystem::path> bases;
  if (const auto p = env("ProgramFiles"); !p.empty()) bases.push_back(path_from_utf8(p) / "KiCad");
  bases.push_back("C:/Program Files/KiCad");
  std::vector<std::pair<double, std::filesystem::path>> found;
  std::error_code e;
  for (const auto& base : bases)
    for (const auto& d : std::filesystem::directory_iterator(base, e))
      if (d.is_directory(e)) {
        double v = std::atof(d.path().filename().string().c_str());
        if (static_cast<int>(v) == major) v += 1000;
        found.push_back({v, d.path() / "share/kicad/3dmodels"});
      }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& f : found) out.push_back(f.second);
  for (const auto& base : bases) out.push_back(base / "share/kicad/modules/packages3d");  // KiCad 5
#elif defined(__APPLE__)
  (void)major;
  out = {"/Applications/KiCad/KiCad.app/Contents/SharedSupport/3dmodels", "/Library/Application Support/kicad/3dmodels"};
#else
  (void)major;
  out = {"/usr/share/kicad/3dmodels", "/usr/local/share/kicad/3dmodels", "/usr/share/kicad/modules/packages3d"};
#endif
  return out;
}

// The KiCad version a 3D model variable belongs to (KICAD9_3DMODEL_DIR: 9, KISYS3DMOD: 5), else 0.
int variable_version(const std::string& var) {
  if (var == "KISYS3DMOD") return 5;
  if (var.rfind("KICAD", 0) == 0 && var.size() > 17 && var.compare(var.size() - 12, 12, "_3DMODEL_DIR") == 0) return std::atoi(var.c_str() + 5);
  return 0;
}

// Where a path variable may point, in the order KiCad itself takes them: the project's text variables, the environment,
// KiCad's configuration, then (3D model variables) its install folders.
std::vector<std::filesystem::path> variable_dirs(const std::string& var, const std::map<std::string, std::string>& project) {
  std::vector<std::filesystem::path> out;
  if (const auto it = project.find(var); it != project.end()) out.push_back(path_from_utf8(it->second));
  if (const auto v = env(var); !v.empty()) out.push_back(path_from_utf8(v));
  for (const auto& dir : config_dirs()) {
    std::error_code e;
    const auto file = dir / "kicad_common.json";
    if (!std::filesystem::is_regular_file(file, e)) continue;
    try {
      const json j = json::parse(read_text_file(file), nullptr, false);
      if (j.is_discarded() || !j.contains("environment") || !j["environment"].is_object()) continue;
      const json& vars = j["environment"].value("vars", json());
      if (vars.is_object() && vars.contains(var) && vars[var].is_string() && !vars[var].get<std::string>().empty())
        out.push_back(path_from_utf8(vars[var].get<std::string>()));
    } catch (const std::exception&) {
    }
  }
  if (const int major = variable_version(var))
    for (const auto& d : install_dirs(major)) out.push_back(d);
  return out;
}

// The leading variable of a model name ("${KICAD9_3DMODEL_DIR}/x.step" -> KICAD9_3DMODEL_DIR, "x.step"); empty without one.
std::string leading_variable(const std::string& name, std::string& rest) {
  if (name.size() < 4 || name[0] != '$' || (name[1] != '{' && name[1] != '(')) return {};
  const size_t close = name.find(name[1] == '{' ? '}' : ')');
  if (close == std::string::npos) return {};
  rest = name.substr(close + 1);
  while (!rest.empty() && rest.front() == '/') rest.erase(0, 1);
  return name.substr(2, close - 2);
}

bool is_vrml(const std::filesystem::path& p) {
  const std::string ext = lower(p.extension().string());
  return ext == ".wrl" || ext == ".vrml";
}

// The STEP file for a candidate (a VRML name finds the STEP beside it), or with `vrml` the VRML file itself.
std::filesystem::path existing(std::filesystem::path p, bool vrml) {
  std::error_code e;
  if (vrml || !is_vrml(p)) return (vrml == is_vrml(p)) && std::filesystem::is_regular_file(p, e) ? p : std::filesystem::path();
  for (const char* alt : {".step", ".stp", ".STEP", ".STP"}) {
    p.replace_extension(alt);
    if (std::filesystem::is_regular_file(p, e)) return p;
  }
  return {};
}

// A model of KiCad's own library: "<library>.3dshapes/<file>" right below a 3D model variable, plain names only, as the
// library publishes it (STEP).
std::string library_path(const std::string& var, const std::string& rest) {
  const size_t slash = rest.find('/');
  if (!variable_version(var) || slash == std::string::npos || rest.find('/', slash + 1) != std::string::npos) return {};
  const std::string lib = rest.substr(0, slash);
  std::string file = rest.substr(slash + 1);
  auto plain = [](const std::string& s, const char* extra) {
    return !s.empty() && s[0] != '.' && std::all_of(s.begin(), s.end(), [&](unsigned char c) { return std::isalnum(c) || (c && std::strchr(extra, c)); });
  };
  if (lib.size() <= 9 || lib.compare(lib.size() - 9, 9, ".3dshapes") != 0 || !plain(lib, "_.+-") || !plain(file, "_.+-,()")) return {};
  const std::string ext = lower(path_from_utf8(file).extension().string());
  if (ext == ".wrl" || ext == ".vrml") file = file.substr(0, file.size() - ext.size()) + ".step";
  else if (ext != ".step" && ext != ".stp") return {};
  return lib + "/" + file;
}

// The library's release for a KiCad version (its models move and get renamed between them).
std::string library_tag(int version) { return version <= 0 ? "master" : version == 5 ? "5.1.12" : std::to_string(version) + ".0.0"; }

// Footprints' 3D model files for one board (kicad_model_file).
class Resolver {
 public:
  struct Found {
    std::filesystem::path file;  // empty: not found
    std::string library;         // a model of KiCad's library: "<library>.3dshapes/<file>.step"
    int version = 0;             // the KiCad version its variable names
  };
  Resolver(const std::filesystem::path& board, std::vector<std::filesystem::path> dirs) : dir(board.parent_path()), user(std::move(dirs)) {
    std::filesystem::path pro = board;
    pro.replace_extension(".kicad_pro");
    std::error_code e;
    if (!std::filesystem::is_regular_file(pro, e)) return;
    try {  // the project's text variables, which may name ${KIPRJMOD}
      const json j = json::parse(read_text_file(pro), nullptr, false);
      if (!j.is_object() || !j.contains("text_variables") || !j["text_variables"].is_object()) return;
      for (const auto& [k, v] : j["text_variables"].items()) {
        if (!v.is_string() || v.get<std::string>().empty()) continue;
        std::string value = v.get<std::string>();
        for (size_t at; (at = value.find("${KIPRJMOD}")) != std::string::npos;) value.replace(at, 11, utf8(dir));
        project[k] = value;
      }
    } catch (const std::exception&) {
    }
  }
  Found find(const std::string& name) {
    Found out;
    std::string text = name;
    std::replace(text.begin(), text.end(), '\\', '/');
    if (text.empty() || text.rfind("kicad-embed://", 0) == 0) return out;
    std::vector<std::filesystem::path> candidates;
    std::string rest;
    const std::string var = leading_variable(text, rest);
    if (var == "KIPRJMOD") {
      candidates.push_back(dir / path_from_utf8(rest));
    } else if (!var.empty()) {
      auto it = vars.find(var);
      if (it == vars.end()) it = vars.emplace(var, variable_dirs(var, project)).first;
      for (const auto& d : it->second) candidates.push_back(d / path_from_utf8(rest));
      out.library = library_path(var, rest);
      out.version = variable_version(var);
    } else {
      rest = text;
      const auto p = path_from_utf8(text);
      candidates.push_back(p.is_absolute() ? p : dir / p);
    }
    for (const auto& d : user) {
      candidates.push_back(d / path_from_utf8(rest));
      candidates.push_back(d / path_from_utf8(rest).filename());
    }
    if (!out.library.empty()) candidates.push_back(kicad_download_dir() / path_from_utf8(out.library));
    for (const bool vrml : {false, true})  // a STEP anywhere before a VRML
      for (const auto& c : candidates)
        if (auto f = existing(c.lexically_normal(), vrml); !f.empty()) {
          out.file = f;
          return out;
        }
    return out;
  }

 private:
  std::filesystem::path dir;
  std::vector<std::filesystem::path> user;
  std::map<std::string, std::string> project;
  std::map<std::string, std::vector<std::filesystem::path>> vars;
};

}  // namespace

std::filesystem::path kicad_download_dir() { return cache_dir() / "kicad-models"; }

std::filesystem::path kicad_model_file(const std::string& name, const std::filesystem::path& board, const std::vector<std::filesystem::path>& model_dirs) {
  return Resolver(board, model_dirs).find(name).file;
}

namespace {

// ---------------------------------------------------------------- embedded files (kicad-embed://)
// An (embedded_files (file (name ..) (type ..) (data |base64...|) (checksum ..)) ..) record: name -> its data as written.
void embedded_files(const Sx& n, std::map<std::string, std::string>& out) {
  n.each("file", [&](const Sx& f) {
    const Sx* name = f.child("name");
    const Sx* data = f.child("data");
    if (!name || !data) return;
    std::string text;
    for (size_t k = 1; k < data->items.size(); ++k)
      if (!data->items[k].list)
        for (const char c : data->items[k].atom)
          if (c != '|') text += c;
    out[name->text(1)] = std::move(text);
  });
}

std::string base64(const std::string& text) {
  std::string out;
  out.reserve(text.size() * 3 / 4);
  unsigned bits = 0;
  int count = 0;
  for (const unsigned char c : text) {
    int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (v < 0) continue;  // padding, line breaks
    bits = (bits << 6) | static_cast<unsigned>(v);
    if ((count += 6) >= 8) out += static_cast<char>((bits >> (count -= 8)) & 0xFF);
  }
  return out;
}

// KiCad stores embedded files zstd-compressed, then base64: the file, written once into the user cache under its content
// hash (its own name kept, which says its format); empty with `error` set when it cannot be.
std::filesystem::path extract_embedded(const std::string& name, const std::string& data, const std::string& hash, std::string& error) {
  std::string leaf = utf8(path_from_utf8(name).filename());
  for (char& c : leaf)
    if (!std::isalnum(static_cast<unsigned char>(c)) && !std::strchr("._-+(),", c)) c = '_';
  if (leaf.empty() || leaf[0] == '.') leaf = "model" + leaf;
  const auto target = cache_dir() / "kicad-embed" / hash.substr(0, 24) / path_from_utf8(leaf);
  std::error_code e;
  if (std::filesystem::is_regular_file(target, e)) return target;
  std::string raw = base64(data);
  if (raw.size() >= 4 && static_cast<unsigned char>(raw[0]) == 0x28 && static_cast<unsigned char>(raw[1]) == 0xB5 && static_cast<unsigned char>(raw[2]) == 0x2F &&
      static_cast<unsigned char>(raw[3]) == 0xFD) {
#ifdef OPAD_HAVE_ZSTD
    ZSTD_DStream* stream = ZSTD_createDStream();
    ZSTD_initDStream(stream);
    ZSTD_inBuffer in{raw.data(), raw.size(), 0};
    std::string out;
    std::vector<char> buffer(ZSTD_DStreamOutSize());
    for (;;) {
      ZSTD_outBuffer o{buffer.data(), buffer.size(), 0};
      const size_t r = ZSTD_decompressStream(stream, &o, &in);
      if (ZSTD_isError(r)) {
        error = std::string("damaged (") + ZSTD_getErrorName(r) + ")";
        break;
      }
      out.append(buffer.data(), o.pos);
      if (out.size() > (size_t(1) << 30)) error = "larger than 1 GB";
      if (!error.empty() || (in.pos >= in.size && o.pos < o.size)) break;
    }
    ZSTD_freeDStream(stream);
    if (!error.empty()) return {};
    raw = std::move(out);
#else
    error = "this build reads no zstd-compressed data";
    return {};
#endif
  }
  if (raw.empty()) {
    error = "empty";
    return {};
  }
  write_text_file(target, raw);
  return target;
}

// ---------------------------------------------------------------- the board
struct Model {
  std::string name;
  Vec3 offset{0, 0, 0}, scale{1, 1, 1}, rotate{0, 0, 0};
  double opacity = 1.0;
};

struct Footprint {
  std::string ref, name, uuid;
  Place place;
  bool bottom = false, dnp = false;
  double height = 0;  // the footprint's "Height" property (mm), 0 when it has none
  std::vector<Model> models;
  std::map<std::string, std::string> embedded;  // its own embedded files (kicad-embed://name)
  std::array<std::vector<P2>, 3> outline;  // own coordinates: courtyard, fabrication outline, pads
  std::vector<Seg> courtyard;              // on the page
};

double height_property(const std::string& value) {
  const char* first = value.c_str();
  char* last = nullptr;
  const double v = std::strtod(first, &last);
  if (last == first || !std::isfinite(v) || v <= 0) return 0;
  const std::string unit = lower(std::string(last));
  return std::min(v * (unit.find("mil") != std::string::npos ? 0.0254 : unit.find("in") != std::string::npos ? 25.4 : 1.0), 200.0);
}

Vec3 xyz(const Sx* n, Vec3 fallback) {
  const Sx* v = n ? n->child("xyz") : nullptr;
  return v ? Vec3{v->num(1, fallback[0]), v->num(2, fallback[1]), v->num(3, fallback[2])} : fallback;
}

void points_of(const Shapes& s, std::vector<P2>& out) {
  for (const auto& seg : s.open) {
    const auto pts = polygon(Loop{seg});
    out.insert(out.end(), pts.begin(), pts.end());
    out.push_back(seg.p.back());
  }
  for (const auto& loop : s.loops) {
    const auto pts = polygon(loop);
    out.insert(out.end(), pts.begin(), pts.end());
  }
}

struct Part {
  std::string key, name, representation;
  Mat4 at;
  bool has_color = false;
  std::array<double, 3> color{};
  double opacity = 1.0;
};

class Builder {
 public:
  Builder(Document& d, const std::filesystem::path& f, const ImportOptions& o) : doc(d), file(f), opt(o), made(o), resolver(f, o.kicad.model_dirs) { made.heal = false; }

  ImportResult run() {
    report(-1, "reading");
    read(parse());
    report(0, "building");
    const std::string op_id = new_uuid();
    json children = json::array();
    const auto boards = board(op_id);
    for (const auto& b : boards) children.push_back(b);
    if (opt.kicad.components) components(op_id, children);
    if (json layers = layers2d(op_id); !layers.is_null()) children.push_back(layers);
    json root_node = {{"type", "component"}, {"id", stable_id(op_id, "root")}, {"name", utf8(file.stem())}, {"children", children}};
    if (!opt.placement.is_identity()) root_node["transform"] = opt.placement.to_json();
    json op = {{"op", "import"}, {"id", op_id}, {"source", utf8(file.filename())}, {"units", "mm"}, {"nodes", json::array({root_node})},
               {"kicad", {{"origin", {origin[0], origin[1]}}, {"thickness", thickness}}}};
    if (!opt.parent.empty()) op["parent"] = opt.parent;
    res.components += 1;
    res.op_id = doc.append(op, opt.author).id;
    if (!missing.empty()) {
      std::string list;
      for (size_t i = 0; i < missing.size() && i < 8; ++i) list += (i ? ", " : "") + missing[i];
      if (missing.size() > 8) list += ", ...";
      res.warnings.push_back("footprint 3D models not found, shown as boxes: " + list);
    }
    res.info = {{"footprints", footprints.size()}, {"components", placed}, {"models", models.size()}, {"placeholders", placeholders},
                {"missing_models", missing}, {"downloadable", downloadable.size()}, {"holes", holes.size()}, {"thickness", thickness},
                {"outlines", loops.size()}};
    return res;
  }

  // The models the footprints that would be placed show: where each was found, which footprints use it, and for one of
  // KiCad's library, its place there.
  json model_list() {
    read(parse());
    std::map<std::string, std::vector<std::string>> refs;
    std::vector<std::pair<std::string, Resolver::Found>> order;
    for (const auto& f : footprints)
      if (opt.kicad.dnp || !f.dnp)
        for (const auto& m : f.models) {
          auto& r = refs[m.name];
          if (r.empty()) order.push_back({m.name, locate(f, m)});
          r.push_back(f.ref);
        }
    json list = json::array();
    int present = 0, absent = 0, library = 0;
    for (const auto& [name, f] : order) {
      json m = {{"name", name}, {"refs", refs[name]}};
      if (!f.file.empty()) m["file"] = utf8(f.file);
      if (!f.library.empty()) m["library"] = f.library, m["tag"] = library_tag(f.version);
      present += !f.file.empty();
      absent += f.file.empty();
      library += f.file.empty() && !f.library.empty();
      list.push_back(m);
    }
    return {{"models", list}, {"found", present}, {"missing", absent}, {"downloadable", library}, {"download_dir", utf8(kicad_download_dir())}};
  }

 private:
  Document& doc;
  std::filesystem::path file;
  const ImportOptions& opt;
  ImportOptions made;  // for the shapes built here, valid by construction: checking a board with thousands of drills took minutes
  ImportResult res;
  double thickness = 1.6;
  std::array<double, 3> color{0.07, 0.30, 0.15};
  P2 origin{0, 0};
  Shapes edges;              // Edge.Cuts on the page
  std::vector<Loop> loops;   // closed outlines, board coordinates
  std::vector<Hole> holes;   // board coordinates
  std::vector<Footprint> footprints;
  std::map<std::string, std::vector<Part>> models;  // model file + scale -> its bodies (empty: could not be read)
  Resolver resolver;
  std::map<std::string, Resolver::Found> found;        // model name (or embedded content) -> its file (empty: not found)
  std::string text;                                    // the board file
  size_t embedded_at = std::string::npos;              // its embedded files, read when a model names one
  std::optional<std::map<std::string, std::string>> board_files;
  std::map<const std::string*, std::string> hashes;    // embedded data -> its hash
  std::map<std::string, std::string> boxes;            // placeholder size -> body key
  std::vector<std::string> missing;
  std::set<std::string> downloadable;  // missing models of KiCad's library
  int placed = 0, placeholders = 0;

  void report(double fraction, const std::string& what) {
    if (opt.progress && !opt.progress(fraction, what)) throw Error("import cancelled");
  }
  Sx parse() {
    text = read_text_file(file);
    Parser p(text);
    Sx root = p.board();
    embedded_at = p.embedded;
    return root;
  }

  // Where a footprint's model is: a file found as KiCad finds it, or an embedded one (the footprint's own, else the
  // board's) written out to the cache.
  const Resolver::Found& locate(const Footprint& f, const Model& m) {
    static const std::string scheme = "kicad-embed://";
    if (m.name.rfind(scheme, 0) != 0) {
      auto it = found.find(m.name);
      if (it == found.end()) it = found.emplace(m.name, resolver.find(m.name)).first;
      return it->second;
    }
    const std::string name = m.name.substr(scheme.size());
    const std::string* data = nullptr;
    if (const auto it = f.embedded.find(name); it != f.embedded.end()) data = &it->second;
    if (!data) {
      if (!board_files) {
        board_files.emplace();
        if (embedded_at != std::string::npos) embedded_files(Parser(text).record(embedded_at), *board_files);
      }
      if (const auto it = board_files->find(name); it != board_files->end()) data = &it->second;
    }
    std::string& hash = hashes[data];
    if (data && hash.empty()) hash = sha256_hex(*data);
    const std::string key = data ? "kicad-embed|" + hash + "|" + name : m.name;
    auto it = found.find(key);
    if (it != found.end()) return it->second;
    Resolver::Found where;
    std::string error = data ? "" : "not in the board";
    if (data) where.file = extract_embedded(name, *data, hash, error);
    if (!error.empty()) res.warnings.push_back("embedded 3D model " + name + ": " + error);
    return found.emplace(key, where).first->second;
  }
  P2 board_xy(P2 p) const { return {p[0] - origin[0], origin[1] - p[1]}; }
  void to_board(Seg& s) const {
    for (auto& p : s.p) p = board_xy(p);
  }

  void read(const Sx& root) {
    if (const Sx* g = root.child("general"); g && g->child("thickness")) thickness = g->child("thickness")->num(1, 1.6);
    std::optional<P2> aux;
    if (const Sx* setup = root.child("setup")) {
      if (const Sx* a = setup->child("aux_axis_origin"); a && (a->num(1) != 0 || a->num(2) != 0)) aux = at2(a);
      if (const Sx* stack = setup->child("stackup")) {
        double sum = 0;
        std::string mask;
        stack->each("layer", [&](const Sx& l) {
          if (const Sx* t = l.child("thickness")) sum += t->num(1);
          if (const Sx* c = l.child("color"); c && (mask.empty() || l.text(1) == "F.Mask") && (l.text(1) == "F.Mask" || l.text(1) == "B.Mask")) mask = c->text(1);
        });
        if (!root.child("general") || !root.child("general")->child("thickness")) thickness = sum > 0 ? sum : thickness;
        if (!mask.empty()) color = mask_color(mask);
      }
    }
    if (thickness <= 0) thickness = 1.6;
    std::vector<Hole> page_holes;
    for (const auto& n : root.items) {
      if (!n.list) continue;
      const std::string& kind = n.name();
      if (kind.rfind("gr_", 0) == 0 && on_layer(n, "Edge.Cuts")) shape_item(n, nullptr, edges);
      else if (kind == "footprint" || kind == "module") footprint(n, page_holes);
      else if (kind == "via" && opt.kicad.vias && !n.has("blind") && !n.has("micro") && !(n.child("type") && n.child("type")->text(1) != "through") && n.child("drill"))
        page_holes.push_back({at2(n.child("at")), n.child("drill")->num(1), n.child("drill")->num(1), 0});
    }
    int unclosed = 0;
    std::vector<Loop> page_loops = chain(edges.open, unclosed);
    page_loops.insert(page_loops.end(), edges.loops.begin(), edges.loops.end());
    if (unclosed) res.warnings.push_back("Edge.Cuts pieces that do not close into an outline: " + std::to_string(unclosed) + "; the board may be incomplete");
    // The origin: the drill/place origin when the board sets one, else the outline's centre (else the footprints').
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    auto grow = [&](P2 p) { x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]); };
    for (const auto& l : page_loops)
      for (const auto& p : polygon(l)) grow(p);
    if (x0 > x1)
      for (const auto& f : footprints) grow({f.place.x, f.place.y});
    if (opt.kicad.origin == "page") origin = {0, 0};
    else if (aux && opt.kicad.origin != "center") origin = *aux;
    else if (x0 <= x1) origin = {(x0 + x1) / 2, (y0 + y1) / 2};
    for (auto& l : page_loops) {
      for (auto& s : l) to_board(s);
      loops.push_back(std::move(l));
    }
    for (const auto& h : page_holes)
      if (h.w > 0) holes.push_back({board_xy(h.c), h.w, h.h > 0 ? h.h : h.w, h.angle});
    if (loops.empty()) res.warnings.push_back("the board has no outline on Edge.Cuts; only its components are shown");
  }

  void footprint(const Sx& n, std::vector<Hole>& page_holes) {
    Footprint f;
    f.name = n.text(1);
    if (const Sx* at = n.child("at")) f.place = {at->num(1), at->num(2), at->num(3)};
    f.bottom = n.child("layer") && n.child("layer")->text(1).rfind("B.", 0) == 0;
    if (const Sx* u = n.child("uuid")) f.uuid = u->text(1);
    else if (const Sx* t = n.child("tstamp")) f.uuid = t->text(1);
    if (const Sx* a = n.child("attr"); a && a->has("dnp")) f.dnp = true;
    if (const Sx* d = n.child("dnp"); d && d->text(1) != "no") f.dnp = true;
    for (const auto& c : n.items) {
      if (!c.list) continue;
      const std::string& kind = c.name();
      if (kind == "property") {
        const std::string key = lower(c.text(1));
        if (key == "reference") f.ref = c.text(2);
        else if (key == "height") f.height = height_property(c.text(2));
      } else if (kind == "fp_text" && c.text(1) == "reference") {
        f.ref = c.text(2);
      } else if (kind == "model" && !c.has("hide") && !(c.child("hide") && c.child("hide")->text(1) != "no")) {
        Model m;
        m.name = c.text(1);
        if (const Sx* off = c.child("offset")) m.offset = xyz(off, m.offset);
        else if (const Sx* at = c.child("at")) {  // before KiCad 6: inches
          m.offset = xyz(at, m.offset);
          for (auto& v : m.offset) v *= 25.4;
        }
        m.scale = xyz(c.child("scale"), m.scale);
        m.rotate = xyz(c.child("rotate"), m.rotate);
        if (const Sx* o = c.child("opacity")) m.opacity = std::clamp(o->num(1, 1), 0.05, 1.0);
        f.models.push_back(m);
      } else if (kind == "embedded_files") {
        embedded_files(c, f.embedded);
      } else if (kind == "pad") {
        pad(c, f, page_holes);
      } else if (kind.rfind("fp_", 0) == 0 && kind != "fp_text") {
        const Sx* l = c.child("layer");
        const std::string layer = l ? l->text(1) : "";
        if (layer == "Edge.Cuts") {
          shape_item(c, &f.place, edges);
        } else if (layer == "F.CrtYd" || layer == "B.CrtYd" || layer == "F.Fab" || layer == "B.Fab") {
          Shapes local;
          shape_item(c, nullptr, local);
          points_of(local, f.outline[layer.find("CrtYd") != std::string::npos ? 0 : 1]);
          if (layer.find("CrtYd") != std::string::npos) {
            Shapes page;
            shape_item(c, &f.place, page);
            f.courtyard.insert(f.courtyard.end(), page.open.begin(), page.open.end());
            for (const auto& loop : page.loops) f.courtyard.insert(f.courtyard.end(), loop.begin(), loop.end());
          }
        }
      }
    }
    footprints.push_back(std::move(f));
  }

  void pad(const Sx& c, Footprint& f, std::vector<Hole>& page_holes) {
    const Sx* at = c.child("at");
    const P2 local = at2(at);
    const double angle = at ? at->num(3) : 0;  // the pad's own angle, the footprint's included
    if (const Sx* size = c.child("size")) {
      const double r = std::max(size->num(1), size->num(2, size->num(1))) / 2;
      for (const P2 d : {P2{-r, -r}, P2{r, r}}) f.outline[2].push_back({local[0] + d[0], local[1] + d[1]});
    }
    const std::string& type = c.text(2);
    const Sx* drill = c.child("drill");
    if ((type != "thru_hole" && type != "np_thru_hole") || !drill) return;
    const bool oval = drill->text(1) == "oval";
    const size_t k = oval ? 2 : 1;
    const double w = drill->num(k), h = oval && k + 1 < drill->items.size() && !drill->items[k + 1].list ? drill->num(k + 1, w) : w;
    if (w <= 0) return;
    const P2 centre = f.place(local);
    const P2 offset = drill->child("offset") ? at2(drill->child("offset")) : P2{0, 0};
    page_holes.push_back({Place{centre[0], centre[1], angle}(offset), w, h, angle});
  }

  // The board: one prism per outer outline, its cutouts and the drills inside it as holes. A drill that crosses an
  // outline or another drill is cut afterwards (one boolean), one outside every board is left out.
  std::vector<json> board(const std::string& op_id) {
    std::vector<json> out;
    if (loops.empty()) return out;
    std::vector<std::vector<P2>> polys;
    std::vector<double> areas;
    for (const auto& l : loops) polys.push_back(polygon(l)), areas.push_back(std::abs(area(polys.back())));
    std::vector<int> depth(loops.size(), 0), parent(loops.size(), -1);
    for (size_t i = 0; i < loops.size(); ++i)
      for (size_t j = 0; j < loops.size(); ++j)
        if (i != j && areas[j] > areas[i] && inside(polys[j], polys[i][0])) {
          ++depth[i];
          if (parent[i] < 0 || areas[j] < areas[static_cast<size_t>(parent[i])]) parent[i] = static_cast<int>(j);
        }
    struct Region {
      size_t outer;
      std::vector<size_t> cutouts;
      std::vector<Hole> holes;
    };
    std::vector<Region> regions;
    std::map<size_t, size_t> region_of;
    for (size_t i = 0; i < loops.size(); ++i)
      if (depth[i] % 2 == 0 && areas[i] > 1e-6) region_of[i] = regions.size(), regions.push_back({i, {}, {}});
    for (size_t i = 0; i < loops.size(); ++i)
      if (depth[i] % 2 == 1 && parent[i] >= 0 && region_of.count(static_cast<size_t>(parent[i]))) regions[region_of[static_cast<size_t>(parent[i])]].cutouts.push_back(i);
    // Drills: inside a region with room around them, else cut by a boolean when they touch material.
    std::vector<Hole> crossing;
    std::vector<std::pair<Hole, size_t>> simple;
    int outside = 0;
    for (const Hole& h : holes) {
      bool placed_in = false, touches = false;
      for (size_t r = 0; r < regions.size() && !placed_in; ++r) {
        const Region& g = regions[r];
        double room = to_edges(polys[g.outer], h.c);
        bool in = inside(polys[g.outer], h.c);
        for (size_t c : g.cutouts) {
          room = std::min(room, to_edges(polys[c], h.c));
          in = in && !inside(polys[c], h.c);
        }
        if (in && room > h.reach() + 0.01) simple.push_back({h, r}), placed_in = true;
        else if (room <= h.reach() + 0.01 || in) touches = true;
      }
      if (!placed_in) {
        if (touches) crossing.push_back(h);
        else ++outside;
      }
    }
    std::sort(simple.begin(), simple.end(), [](const auto& a, const auto& b) { return a.first.c[0] < b.first.c[0]; });
    double reach = 0;
    for (const auto& s : simple) reach = std::max(reach, s.first.reach());
    std::vector<int> state(simple.size(), 0);  // 0 simple, 1 crossing another, 2 a duplicate
    for (size_t i = 0; i < simple.size(); ++i) {
      if (state[i]) continue;
      const Hole& a = simple[i].first;
      for (size_t j = i + 1; j < simple.size() && simple[j].first.c[0] - a.c[0] <= a.reach() + reach + 0.01; ++j) {
        if (state[j]) continue;
        const Hole& b = simple[j].first;
        const double d = dist(a.c, b.c);
        if (d < 1e-6 && std::abs(a.w - b.w) < 1e-6 && std::abs(a.h - b.h) < 1e-6) state[j] = 2;
        else if (d < a.reach() + b.reach() + 0.01) state[j] = 1;
      }
    }
    for (size_t i = 0; i < simple.size(); ++i)
      if (state[i] == 0) regions[simple[i].second].holes.push_back(simple[i].first);
      else if (state[i] == 1) crossing.push_back(simple[i].first);
    if (outside) res.warnings.push_back("drills outside the board outline, left out: " + std::to_string(outside));
    if (crossing.size() > kMaxCut) {  // a board that breaks its own design rules; one cut of thousands took minutes
      res.warnings.push_back("drills overlapping other drills or the board's edge, left out: " + std::to_string(crossing.size() - kMaxCut));
      crossing.resize(kMaxCut);
    }
    BRep_Builder bb;
    TopoDS_Compound tools;
    bb.MakeCompound(tools);
    for (const Hole& h : crossing) {
      const TopoDS_Face face = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), wire(hole_loop(h)), Standard_True);
      gp_Trsf down;
      down.SetTranslation(gp_Vec(0, 0, -1));
      bb.Add(tools, BRepPrimAPI_MakePrism(face, gp_Vec(0, 0, thickness + 2)).Shape().Moved(TopLoc_Location(down)));
    }
    int index = 0;
    for (const Region& g : regions) {
      report(-1, "building");
      TopoDS_Wire outer = wire(loops[g.outer]);
      if (area(polys[g.outer]) < 0) outer.Reverse();
      BRepBuilderAPI_MakeFace mf(gp_Pln(gp::XOY()), outer, Standard_True);
      for (size_t c : g.cutouts) {
        TopoDS_Wire w = wire(loops[c]);
        if (area(polys[c]) > 0) w.Reverse();
        mf.Add(w);
      }
      for (const Hole& h : g.holes) {
        TopoDS_Wire w = wire(hole_loop(h));
        w.Reverse();
        mf.Add(w);
      }
      if (!mf.IsDone()) {
        res.warnings.push_back("a board outline could not be made into a face");
        continue;
      }
      TopoDS_Shape solid = BRepPrimAPI_MakePrism(mf.Face(), gp_Vec(0, 0, thickness)).Shape();
      if (!crossing.empty()) {
        BRepAlgoAPI_Cut cut(solid, tools);
        if (cut.IsDone() && !cut.Shape().IsNull()) solid = cut.Shape();
        else res.warnings.push_back("drills across the board's edge could not be cut");
      }
      const std::string name = regions.size() == 1 ? "Board" : "Board " + std::to_string(index + 1);
      json meta = {{"name", name}, {"units", "mm"}, {"source", utf8(file.filename())}, {"color", color}};
      json body = {{"type", "body"}, {"id", stable_id(op_id, "board/" + std::to_string(index))}, {"name", name}, {"color", color}};
      body["key"] = detail::store_body(doc, solid, meta, made, false, &res);
      ++res.bodies;
      ++index;
      out.push_back(body);
    }
    return out;
  }

  // A model file's bodies, read once per file and scale: the scale is built into copies of the shapes (KiCad applies
  // it last, inside the model's own frame), so the placements stay rigid.
  const std::vector<Part>& model(const std::filesystem::path& path, const Vec3& scale, size_t done, size_t total) {
    const std::string id = utf8(path) + "|" + json(scale).dump();
    if (auto it = models.find(id); it != models.end()) return it->second;
    auto& parts = models[id];
    const std::string phase = "translating 3D models " + std::to_string(done + 1) + "/" + std::to_string(total);
    report(total ? double(done) / double(total) : -1, phase);
    Document part_doc = Document::create();
    ImportOptions o;
    o.viewer = opt.viewer;
    o.heal = opt.heal;
    o.progress = [this, phase](double, const std::string&) { return !opt.progress || opt.progress(-1, phase); };
    try {
      if (is_vrml(path)) detail::import_mesh_scene(part_doc, path, o, true);  // KiCad's own VRML: 0.1 inch units, Z up
      else import_file(part_doc, path, o);
    } catch (const std::exception& e) {
      if (std::string(e.what()) == "import cancelled") throw;
      res.warnings.push_back("3D model " + utf8(path.filename()) + " could not be read: " + e.what());
      return parts;
    }
    const Scene s = resolve(part_doc);
    const bool scaled = std::abs(scale[0] - 1) > 1e-9 || std::abs(scale[1] - 1) > 1e-9 || std::abs(scale[2] - 1) > 1e-9;
    for (const auto& id_ : s.all_bodies()) {
      const Node* n = s.node(id_);
      const BodyEntry* entry = part_doc.body(n->body_key);
      if (!n->visible || n->body_missing || !entry) continue;
      Part p{n->body_key, n->name, n->representation, s.world(id_), n->has_color, n->color, n->opacity};
      TopoDS_Shape shape = body_shape(part_doc, n->body_key);
      if (scaled && n->representation != "mesh") {
        shape = shape.Moved(TopLoc_Location(trsf_from_mat(p.at)));
        if (std::abs(scale[0] - scale[1]) < 1e-9 && std::abs(scale[1] - scale[2]) < 1e-9) {
          gp_Trsf t;
          t.SetScale(gp::Origin(), scale[0]);
          shape = BRepBuilderAPI_Transform(shape, t, true).Shape();
        } else {
          gp_GTrsf g;
          g.SetValue(1, 1, scale[0]), g.SetValue(2, 2, scale[1]), g.SetValue(3, 3, scale[2]);
          shape = BRepBuilderAPI_GTransform(shape, g, true).Shape();
        }
        p.key = detail::store_body(doc, shape, entry->meta, opt, false, &res);
        p.at = Mat4{};
      } else {
        if (scaled) res.warnings.push_back("3D model " + utf8(path.filename()) + " is a mesh: its scale was left out");
        if (!doc.has_body(p.key)) {
          if (opt.viewer) doc.add_live_body(p.key, entry->meta);
          else doc.add_body(entry->brep, entry->meta);
          ++res.new_entries;
        }
        cache_shape(doc, p.key, shape);
      }
      parts.push_back(std::move(p));
    }
    return parts;
  }

  // Every footprint with a visible 3D model, placed as KiCad places models: the footprint's position and rotation, on
  // the bottom turned over (about X), then the model's offset (on top of the board, or under it), its rotation (Z, Y,
  // X, each negated) and scale. A footprint whose models are all missing gets a translucent box instead.
  void components(const std::string& op_id, json& children) {
    std::vector<const Footprint*> order;
    for (const auto& f : footprints)
      if (!f.models.empty() && (opt.kicad.dnp || !f.dnp)) order.push_back(&f);
    std::stable_sort(order.begin(), order.end(), [](const Footprint* a, const Footprint* b) { return natural_less(a->ref, b->ref); });
    std::set<std::string> files;
    for (const Footprint* f : order)
      for (const auto& m : f->models) {
        if (const auto& where = locate(*f, m); !where.file.empty()) files.insert(utf8(where.file) + "|" + json(m.scale).dump());
      }
    std::set<std::string> ids;
    auto unique_id = [&](const std::string& what) {
      std::string id = stable_id(op_id, what);
      if (!ids.insert(id).second) ids.insert(id = new_uuid());
      return id;
    };
    for (const Footprint* f : order) {
      const std::string key = f->uuid.empty() ? f->ref : f->uuid;
      const P2 at = board_xy({f->place.x, f->place.y});
      json bodies = json::array();
      std::vector<std::string> absent;
      for (size_t mi = 0; mi < f->models.size(); ++mi) {
        const Model& m = f->models[mi];
        const auto& where = locate(*f, m);
        const auto& path = where.file;
        if (path.empty()) {
          absent.push_back(m.name);
          if (!where.library.empty()) downloadable.insert(where.library);
          continue;
        }
        const size_t done = std::min(models.size(), files.size());
        const auto& parts = model(path, m.scale, done, files.size());
        if (parts.empty()) {
          absent.push_back(m.name);
          continue;
        }
        Mat4 place = f->bottom ? rotation(0, 180) * Mat4::translation(m.offset[0], m.offset[1], m.offset[2])
                               : Mat4::translation(m.offset[0], m.offset[1], m.offset[2] + thickness);
        place = place * rotation(2, -m.rotate[2]) * rotation(1, -m.rotate[1]) * rotation(0, -m.rotate[0]);
        for (size_t pi = 0; pi < parts.size(); ++pi) {
          const Part& p = parts[pi];
          const std::string stem = utf8(path_from_utf8(m.name).stem());
          json body = {{"type", "body"}, {"id", unique_id(key + "/" + std::to_string(mi) + "/" + std::to_string(pi))},
                       {"name", parts.size() == 1 || p.name.empty() ? stem : p.name}, {"key", p.key}};
          if (p.representation != "solid") body["representation"] = p.representation;
          if (const Mat4 t = place * p.at; !t.is_identity()) body["transform"] = t.to_json();
          if (p.has_color) body["color"] = p.color;
          if (const double o = std::min(p.opacity, m.opacity); o < 1) body["opacity"] = o;
          bodies.push_back(body);
          ++res.bodies;
        }
      }
      if (bodies.empty()) {
        bodies.push_back(placeholder(*f, unique_id(key + "/placeholder")));
        for (const auto& a : absent) missing.push_back((f->ref.empty() ? f->name : f->ref) + " (" + utf8(path_from_utf8(a).filename()) + ")");
      }
      const std::string short_name = f->name.substr(f->name.find(':') == std::string::npos ? 0 : f->name.find(':') + 1);
      json node = {{"type", "component"}, {"id", unique_id(key)}, {"name", f->ref.empty() ? short_name : f->ref + " " + short_name},
                   {"children", bodies}, {"kicad", {{"ref", f->ref}, {"footprint", f->name}, {"side", f->bottom ? "bottom" : "top"}}}};
      if (f->dnp) node["kicad"]["dnp"] = true;
      if (!absent.empty()) node["kicad"]["missing"] = absent;
      const Mat4 frame = Mat4::translation(at[0], at[1], 0) * rotation(2, f->place.angle);
      if (!frame.is_identity()) node["transform"] = frame.to_json();
      children.push_back(node);
      ++res.components;
      ++placed;
    }
  }

  // A box over the footprint's courtyard (else its fabrication outline, else its pads), as tall as its "Height" property
  // or the placeholder height, on its side of the board (10 um off it, so the faces do not fight). Boxes of one size
  // share their body.
  json placeholder(const Footprint& f, const std::string& id) {
    double x0 = -0.5, y0 = -0.5, x1 = 0.5, y1 = 0.5;
    for (const auto& pts : f.outline)
      if (!pts.empty()) {
        x0 = y0 = 1e300, x1 = y1 = -1e300;
        for (const auto& p : pts) x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y0 = std::min(y0, -p[1]), y1 = std::max(y1, -p[1]);
        break;
      }
    const double h = f.height > 0 ? f.height : std::max(opt.kicad.placeholder_height, 0.01), dx = std::max(x1 - x0, 0.01), dy = std::max(y1 - y0, 0.01);
    auto round = [](double v) { return std::to_string(std::llround(v * 1000)); };
    std::string& key = boxes[round(dx) + "x" + round(dy) + "x" + round(h) + (f.bottom ? "b" : "t")];
    const std::array<double, 3> amber{1.0, 0.68, 0.18};
    if (key.empty()) {
      const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(-dx / 2, -dy / 2, f.bottom ? -h : 0), dx, dy, h).Shape();
      key = detail::store_body(doc, box, {{"name", "Placeholder"}, {"units", "mm"}, {"source", utf8(file.filename())}, {"color", amber}}, made, false, &res);
    }
    ++placeholders;
    ++res.bodies;
    const std::string stem = f.models.empty() ? "Placeholder" : utf8(path_from_utf8(f.models.front().name).stem());
    return {{"type", "body"}, {"id", id}, {"name", stem}, {"key", key}, {"color", amber}, {"opacity", 0.55}, {"placeholder", true},
            {"transform", Mat4::translation((x0 + x1) / 2, (y0 + y1) / 2, f.bottom ? -kLift : thickness + kLift).to_json()}};
  }

  // Hidden 2D layers for sketches: the outline with the drills (at the board's bottom), the courtyards on each side.
  json layers2d(const std::string& op_id) {
    BRep_Builder b;
    json bodies = json::array();
    auto layer = [&](const std::string& name, const TopoDS_Compound& c, double z) {
      json meta = {{"representation", "drawing2d"}, {"layer", name}, {"source", utf8(file.filename())}};
      json body = {{"type", "body"}, {"id", stable_id(op_id, "layer/" + name)}, {"name", name}, {"representation", "drawing2d"},
                   {"key", detail::store_body(doc, c, meta, made, false, &res)}};
      if (z != 0) body["transform"] = Mat4::translation(0, 0, z).to_json();
      bodies.push_back(body);
      ++res.bodies;
    };
    if (!loops.empty() || !holes.empty()) {
      TopoDS_Compound cuts;
      b.MakeCompound(cuts);
      for (const auto& l : loops) add_edges(b, cuts, l);
      for (const auto& h : holes) add_edges(b, cuts, hole_loop(h));
      layer("Edge.Cuts", cuts, 0);
    }
    for (const bool bottom : {false, true}) {
      TopoDS_Compound c;
      b.MakeCompound(c);
      bool any = false;
      for (const auto& f : footprints) {
        if (f.bottom != bottom || f.courtyard.empty() || (!opt.kicad.dnp && f.dnp)) continue;
        std::vector<Seg> pieces = f.courtyard;
        for (auto& s : pieces) to_board(s);
        add_edges(b, c, pieces);
        any = true;
      }
      if (any) layer(bottom ? "B.Courtyard" : "F.Courtyard", c, bottom ? 0 : thickness);
    }
    if (bodies.empty()) return json();
    ++res.components;
    return {{"type", "component"}, {"id", stable_id(op_id, "layers")}, {"name", "Layers"}, {"visible", false}, {"children", bodies}};
  }
};

}  // namespace

json kicad_models(const std::filesystem::path& board, const KicadOptions& opt) {
  Document scratch = Document::create();
  ImportOptions o;
  o.kicad = opt;
  try {
    return Builder(scratch, board, o).model_list();
  } catch (const Standard_Failure& e) {
    throw Error("cannot read " + utf8(board.filename()) + ": " + e.GetMessageString());
  }
}

json kicad_download_models(const std::filesystem::path& board, const KicadOptions& opt, const std::function<bool(double, const std::string&)>& progress) {
  const json list = kicad_models(board, opt);
  std::vector<std::pair<std::string, std::string>> wanted;  // library path, tag
  std::set<std::string> seen;
  for (const auto& m : list["models"])
    if (!m.contains("file") && m.contains("library") && seen.insert(m["library"].get<std::string>()).second) wanted.push_back({m["library"], m["tag"]});
  json out = {{"downloaded", json::array()}, {"failed", json::array()}, {"dir", utf8(kicad_download_dir())}};
  if (wanted.empty()) return out;
  std::string base = env("OPAD_KICAD_MODELS_URL");
  if (base.empty()) base = "https://gitlab.com/kicad/libraries/kicad-packages3D/-/raw";
  while (!base.empty() && base.back() == '/') base.pop_back();
  std::filesystem::path curl = "curl";
#ifdef _WIN32
  curl = "curl.exe";
  std::error_code e;
  if (const auto root = env("SystemRoot"); !root.empty() && std::filesystem::is_regular_file(path_from_utf8(root) / "System32" / "curl.exe", e))
    curl = path_from_utf8(root) / "System32" / "curl.exe";
#endif
  const auto dir = kicad_download_dir();
  std::error_code e2;
  std::filesystem::create_directories(dir, e2);
  if (!std::filesystem::exists(dir / "README.txt", e2))
    write_text_file(dir / "README.txt",
                    "3D models from the KiCad library (https://gitlab.com/kicad/libraries/kicad-packages3D), downloaded by OPAD when asked to.\n"
                    "Licence: CC-BY-SA 4.0 with the KiCad libraries exception (https://www.kicad.org/libraries/license/): free to use in your\n"
                    "own designs; redistributing the models as a collection is bound by CC-BY-SA. OPAD does not ship them.\n");
  for (size_t i = 0; i < wanted.size(); ++i) {
    const auto& [library, tag] = wanted[i];
    if (progress && !progress(double(i) / double(wanted.size()), "downloading 3D models " + std::to_string(i + 1) + "/" + std::to_string(wanted.size())))
      throw Error("download cancelled");
    const auto target = dir / path_from_utf8(library);
    auto part = target;
    part += ".part";
    std::filesystem::create_directories(target.parent_path(), e2);
    std::string error = "not in the library";
    for (const std::string& release : {tag, std::string("master")}) {
      std::filesystem::remove(part, e2);
      const int status = detail::run_program(curl, {"-sS", "-f", "-L", "--max-time", "100", "-o", part, base + "/" + release + "/" + library});
      if (status < 0) {
        error = "curl could not be started";
        break;
      }
      std::string head(64, '\0');
      std::ifstream(part, std::ios::binary).read(head.data(), 64);
      if (status == 0 && head.rfind("ISO-10303-21", 0) == 0) {
        std::filesystem::rename(part, target, e2);
        error = e2 ? e2.message() : "";
        break;
      }
      if (status != 22) error = "curl failed (" + std::to_string(status) + ")";  // 22: the server said no (404)
      if (tag == "master") break;
    }
    std::filesystem::remove(part, e2);
    if (error.empty()) out["downloaded"].push_back(library);
    else out["failed"].push_back({{"model", library}, {"error", error}});
  }
  return out;
}

ImportResult import_kicad_pcb(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) {
  try {
    return Builder(doc, file, opt).run();
  } catch (const Standard_Failure& e) {
    throw Error("cannot build " + utf8(file.filename()) + ": " + e.GetMessageString());
  }
}

}  // namespace opad
