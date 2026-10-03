// Face provenance (TODO 11 UI-94): see provenance.hpp. The body key chain is read by one replay of the log; each step
// is matched geometrically (memoised by the keys it compares, so a regeneration that keeps keys keeps the work).
#include "opad/design/provenance.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepTools.hxx>
#include <BRepTopAdaptor_FClass2d.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <ElSLib.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <Geom_Surface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp.hxx>
#include <gp_Lin.hxx>

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

#include "opad/geometry.hpp"
#include "opad/recognize.hpp"

namespace opad::design {

namespace {

// ---------------------------------------------------------------- one step: which faces of the result lie on earlier faces
struct Sample {
  gp_Pnt p;
  gp_Vec n;  // outward; zero where the surface has none (a cone's apex)
};

// Up to `want` points inside the face, spread over it (a UV grid kept where the face is; finer for slivers and rings).
std::vector<Sample> samples_of(const TopoDS_Face& f, size_t want) {
  double u0, u1, v0, v1;
  BRepTools::UVBounds(f, u0, u1, v0, v1);
  BRepTopAdaptor_FClass2d inside(f, 1e-9);
  BRepAdaptor_Surface s(f);
  std::vector<Sample> out;
  for (int n : {5, 15, 41}) {
    std::vector<gp_Pnt2d> in;
    for (int i = 0; i < n; ++i)
      for (int k = 0; k < n; ++k) {
        const gp_Pnt2d uv(u0 + (u1 - u0) * (i + 0.5) / n, v0 + (v1 - v0) * (k + 0.5) / n);
        if (inside.Perform(uv) == TopAbs_IN) in.push_back(uv);
      }
    if (in.empty()) continue;
    const size_t take = std::min(want, in.size());
    for (size_t j = 0; j < take; ++j) {
      const gp_Pnt2d& uv = in[take == 1 ? in.size() / 2 : j * (in.size() - 1) / (take - 1)];
      gp_Pnt p;
      gp_Vec du, dv;
      s.D1(uv.X(), uv.Y(), p, du, dv);
      gp_Vec normal = du.Crossed(dv);
      if (normal.Magnitude() > 1e-12) {
        normal.Normalize();
        if (f.Orientation() == TopAbs_REVERSED) normal.Reverse();
      } else {
        normal = gp_Vec();
      }
      out.push_back({p, normal});
    }
    break;
  }
  return out;
}

// A face a step left as it was is the same elementary surface, facing the same way, with as many edges and the same
// box (the step copied it, bit for bit): matched by that alone, so the samples are spent on the faces that changed.
// Empty for other surfaces.
std::string identity(const TopoDS_Face& f, const BRepAdaptor_Surface& s, const Bnd_Box& box, double tol) {
  std::vector<double> v;
  auto ax = [&](const gp_Ax3& a) {
    for (const gp_XYZ& x : {a.Location().XYZ(), a.Direction().XYZ(), a.XDirection().XYZ()}) v.insert(v.end(), {x.X(), x.Y(), x.Z()});
  };
  switch (s.GetType()) {
    case GeomAbs_Plane: ax(s.Plane().Position()); break;
    case GeomAbs_Cylinder: ax(s.Cylinder().Position()); v.push_back(s.Cylinder().Radius()); break;
    case GeomAbs_Cone: ax(s.Cone().Position()); v.insert(v.end(), {s.Cone().RefRadius(), s.Cone().SemiAngle()}); break;
    case GeomAbs_Sphere: ax(s.Sphere().Position()); v.push_back(s.Sphere().Radius()); break;
    case GeomAbs_Torus: ax(s.Torus().Position()); v.insert(v.end(), {s.Torus().MajorRadius(), s.Torus().MinorRadius()}); break;
    default: return {};
  }
  if (box.IsVoid()) return {};
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  v.insert(v.end(), {x0, y0, z0, x1, y1, z1});
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(f, TopAbs_EDGE, edges);
  std::string out = std::to_string(static_cast<int>(s.GetType())) + (f.Orientation() == TopAbs_REVERSED ? "r" : "f") + std::to_string(edges.Extent());
  for (double x : v) out += "," + std::to_string(std::llround(x / tol));
  return out;
}

struct Candidate {
  TopoDS_Face face;
  int source = 0, index = 0;
  Bnd_Box box;
  BRepAdaptor_Surface surf;
  double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  Handle(Geom_Surface) geom;                        // other than the elementary surfaces: made on first use
  std::unique_ptr<BRepTopAdaptor_FClass2d> inside;  // made on first use
};

double into(double x, double from, double period) { return x - std::floor((x - from + 1e-9) / period) * period; }

// Whether the sample lies on the face: on its surface, facing along it (or exactly against it: `against`), inside it.
bool on_face(Candidate& g, const Sample& s, double tol, bool& against) {
  double u = 0, v = 0;
  switch (g.surf.GetType()) {
    case GeomAbs_Plane: ElSLib::Parameters(g.surf.Plane(), s.p, u, v); break;
    case GeomAbs_Cylinder: ElSLib::Parameters(g.surf.Cylinder(), s.p, u, v); break;
    case GeomAbs_Cone: ElSLib::Parameters(g.surf.Cone(), s.p, u, v); break;
    case GeomAbs_Sphere: ElSLib::Parameters(g.surf.Sphere(), s.p, u, v); break;
    case GeomAbs_Torus: ElSLib::Parameters(g.surf.Torus(), s.p, u, v); break;
    default: {
      if (g.geom.IsNull()) g.geom = BRep_Tool::Surface(g.face);
      GeomAPI_ProjectPointOnSurf project(s.p, g.geom, g.u0, g.u1, g.v0, g.v1);
      if (project.NbPoints() == 0) return false;
      project.LowerDistanceParameters(u, v);
    }
  }
  if (g.surf.IsUPeriodic()) u = into(u, g.u0, g.surf.UPeriod());
  if (g.surf.IsVPeriodic()) v = into(v, g.v0, g.surf.VPeriod());
  gp_Pnt q;
  gp_Vec du, dv;
  g.surf.D1(u, v, q, du, dv);
  if (q.Distance(s.p) > tol) return false;
  gp_Vec n = du.Crossed(dv);
  against = false;
  if (n.Magnitude() > 1e-12 && s.n.Magnitude() > 0.5) {
    n.Normalize();
    if (g.face.Orientation() == TopAbs_REVERSED) n.Reverse();
    const double c = n.Dot(s.n);
    if (std::fabs(c) < 1 - 1e-6) return false;
    against = c < 0;
  }
  if (!g.inside) g.inside = std::make_unique<BRepTopAdaptor_FClass2d>(g.face, 1e-9);
  const TopAbs_State state = g.inside->Perform(gp_Pnt2d(u, v));
  return state == TopAbs_IN || state == TopAbs_ON;
}

struct Hit {
  int source = 0, index = 0;
  bool against = false;
};
struct FaceMatch {
  std::vector<Hit> hits;   // the earlier faces its samples lie on
  bool uncovered = false;  // some sample lies on none of them
};

// The faces of `after` against the faces of `sources` (the body before the step, then the bodies it took in), all in
// after's frame.
std::vector<FaceMatch> match_faces(const TopoDS_Shape& after, const std::vector<TopoDS_Shape>& sources, const Cancel& cancel) {
  Bnd_Box all;
  BRepBndLib::Add(after, all, Standard_False);
  const double tol = std::max(1e-5, 1e-7 * (all.IsVoid() ? 0.0 : std::sqrt(all.SquareExtent())));
  std::deque<Candidate> candidates;
  std::map<int, std::vector<Candidate*>> by_type;
  std::unordered_map<std::string, std::vector<const Candidate*>> unchanged;
  for (size_t k = 0; k < sources.size(); ++k) {
    if (sources[k].IsNull()) continue;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(sources[k], TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      Candidate& c = candidates.emplace_back();
      c.face = TopoDS::Face(faces(i));
      c.source = static_cast<int>(k);
      c.index = i - 1;
      c.surf.Initialize(c.face);
      BRepTools::UVBounds(c.face, c.u0, c.u1, c.v0, c.v1);
      BRepBndLib::Add(c.face, c.box, Standard_False);
      if (std::string id = identity(c.face, c.surf, c.box, tol); !id.empty()) unchanged[id].push_back(&c);
      c.box.Enlarge(tol);
      by_type[c.surf.GetType()].push_back(&c);
    }
  }
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(after, TopAbs_FACE, faces);
  std::vector<FaceMatch> out(static_cast<size_t>(faces.Extent()));
  for (int i = 1; i <= faces.Extent(); ++i) {
    if (cancel && cancel()) throw Error("cancelled");
    const TopoDS_Face f = TopoDS::Face(faces(i));
    FaceMatch& m = out[static_cast<size_t>(i - 1)];
    const BRepAdaptor_Surface surface(f);
    Bnd_Box box;
    BRepBndLib::Add(f, box, Standard_False);
    if (auto same = unchanged.find(identity(f, surface, box, tol)); same != unchanged.end()) {
      for (const Candidate* g : same->second) m.hits.push_back({g->source, g->index, false});
      continue;
    }
    const auto samples = samples_of(f, 7);
    const auto bucket = by_type.find(surface.GetType());
    m.uncovered = samples.empty();
    for (const auto& s : samples) {
      bool covered = false;
      if (bucket != by_type.end())
        for (Candidate* g : bucket->second) {
          if (g->box.IsOut(s.p)) continue;
          bool against = false;
          if (!on_face(*g, s, tol, against)) continue;
          covered = true;
          if (std::none_of(m.hits.begin(), m.hits.end(), [&](const Hit& h) { return h.source == g->source && h.index == g->index; }))
            m.hits.push_back({g->source, g->index, against});
        }
      m.uncovered |= !covered;
    }
  }
  return out;
}

struct Known {
  std::string key;  // the body's current key
  std::vector<FaceOwner> faces;
};

// Steps compare stored bodies, so their matches hold for any document with those keys (a regeneration that keeps keys,
// an undo, a snapshot of the same state).
std::mutex cache_mu;
std::map<std::string, std::shared_ptr<const std::vector<FaceMatch>>> match_cache;
std::map<std::string, Known> owner_cache;  // by document state + node

std::string place_text(const gp_Trsf& t) {
  std::string s;
  for (int r = 1; r <= 3; ++r)
    for (int c = 1; c <= 4; ++c) s += json(std::round(t.Value(r, c) * 1e9) / 1e9).dump() + ",";
  return s;
}

// The surface types of two bodies face by face: a copy or a moved body has its source's faces in the same order.
bool same_faces(const TopoDS_Shape& a, const TopoDS_Shape& b) {
  TopTools_IndexedMapOfShape fa, fb;
  TopExp::MapShapes(a, TopAbs_FACE, fa);
  TopExp::MapShapes(b, TopAbs_FACE, fb);
  if (fa.Extent() != fb.Extent()) return false;
  for (int i = 1; i <= fa.Extent(); ++i)
    if (BRepAdaptor_Surface(TopoDS::Face(fa(i)), Standard_False).GetType() != BRepAdaptor_Surface(TopoDS::Face(fb(i)), Standard_False).GetType()) return false;
  return true;
}

int face_count(const TopoDS_Shape& s) {
  TopTools_IndexedMapOfShape m;
  TopExp::MapShapes(s, TopAbs_FACE, m);
  return m.Extent();
}

// Body references of an input -> body nodes (a component stands for the bodies under it), as the features read them.
std::vector<std::string> bodies_of(const Scene& s, const json& refs) {
  std::vector<std::string> out;
  for (const auto& r : refs.is_array() ? refs : json::array({refs})) {
    try {
      const std::string id = Ref::from_json(r).body;
      const Node* n = s.node(id);
      if (!n) continue;
      for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : s.bodies_under(id))
        if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
    } catch (const std::exception&) {
    }
  }
  return out;
}

bool copies(const std::string& kind, const json& in) {
  if (kind == "mirror" || kind == "pattern_rect" || kind == "pattern_circ") return in.value("operation", "new") == "new";
  return kind == "move" && in.value("copy", false);
}

gp_Trsf place_in(const Mat4& from, const Mat4& to) {  // from's frame -> to's frame
  if (from.m == to.m) return gp_Trsf();
  return trsf_from_mat(to).Inverted() * trsf_from_mat(from);
}

TopoDS_Shape placed(const TopoDS_Shape& s, const gp_Trsf& t) { return t.Form() == gp_Identity ? s : s.Moved(TopLoc_Location(t)); }

}  // namespace

// ---------------------------------------------------------------- the chain of keys each body went through
struct Provenance::Impl {
  struct Tool {
    std::string node, key;
    int event = -1;
    gp_Trsf place;  // the tool's frame -> the changed body's frame
  };
  struct Event {
    enum class Type { Made, Changed, Moved, Copied } type = Type::Made;
    std::string op, key;
    int prev = -1;           // Changed, Moved: this body's event before; Copied or a piece (split): the source's
    std::string source;      // Copied, or Made from a source body (a split's piece)
    gp_Trsf source_place;    // the source's frame -> this body's frame
    std::vector<Tool> tools;
  };

  const Document& doc;
  Cancel cancel;
  bool ordered = false, replayed = false;
  std::vector<EffectiveOp> ops;
  std::vector<std::string> deleted;
  std::unordered_map<std::string, int> orders;
  std::unordered_map<std::string, std::vector<Event>> events;
  std::map<std::pair<std::string, int>, std::vector<FaceOwner>> memo;
  std::map<std::string, Known> now;
  Scene final;

  Impl(const Document& d, Cancel c) : doc(d), cancel(std::move(c)) {}

  std::string state() const {
    return doc.header.uuid + ":" + std::to_string(doc.ops.size()) + ":" + (doc.ops.empty() ? std::string() : doc.ops.back().id) + ":" + std::to_string(doc.body_count());
  }

  void order_ops() {
    if (ordered) return;
    ordered = true;
    ops = effective_ops(doc, &deleted);
    for (size_t i = 0; i < ops.size(); ++i) orders[ops[i].op->id] = static_cast<int>(i);
  }

  int order_of(const std::string& op) {
    order_ops();
    auto it = orders.find(op);
    return it == orders.end() ? INT_MAX : it->second;
  }

  void replay() {
    if (replayed) return;
    replayed = true;
    order_ops();
    SceneBuilder b(doc);
    b.scene().deleted_ops = deleted;
    auto apply = [&](const EffectiveOp& e) {
      try {
        b.apply(e.op->id, e.op->type, e.data());
      } catch (const std::exception&) {  // resolve() reports it
      }
    };
    for (const EffectiveOp& e : ops) {
      const std::string& id = e.op->id;
      const json& d = e.data();
      if (e.op->type == "import") {
        apply(e);
        std::function<void(const json&)> walk = [&](const json& nodes) {
          for (const auto& n : nodes.is_array() ? nodes : json::array()) {
            const std::string nid = n.value("id", "");
            if (n.value("type", "") != "body") walk(n.value("children", json::array()));
            else if (const Node* x = b.scene().node(nid); x && x->source_op == id && x->kind == Node::Kind::Body) events[nid].push_back({Event::Type::Made, id, x->body_key});
          }
        };
        walk(d.value("nodes", json::array()));
        continue;
      }
      if (e.op->type != "feature") {
        apply(e);
        continue;
      }
      const Scene& s = b.scene();
      const std::string kind = d.value("kind", "");
      const json in = d.value("inputs", json::object());
      const json result = d.value("result", json::object());
      std::vector<std::string> entries;
      for (const auto& r : result.value("bodies", json::array())) entries.push_back(r.value("id", ""));
      struct Was {
        std::string key;
        int event = -1;
        Mat4 world;
      };
      std::map<std::string, Was> was;
      auto remember = [&](const std::string& n) {
        const Node* x = s.node(n);
        if (x && x->kind == Node::Kind::Body && !was.count(n)) was[n] = {x->body_key, static_cast<int>(events[n].size()) - 1, s.world(n)};
      };
      for (const auto& n : entries) remember(n);
      // What it took in (combine tools, joined bodies, a pattern's sources), as they were.
      std::vector<std::string> taken;
      if (const FeatureSpec* spec = feature_spec(kind))
        for (const auto& input : spec->inputs)
          if (input.type == "bodies" && input.name != "extent_body" && in.contains(input.name) && input_active(input, in))
            for (const auto& n : bodies_of(s, in[input.name]))
              if (std::find(taken.begin(), taken.end(), n) == taken.end()) taken.push_back(n);
      for (const auto& r : result.value("removed", json::array()))
        if (r.is_string() && std::find(taken.begin(), taken.end(), r.get<std::string>()) == taken.end()) taken.push_back(r.get<std::string>());
      for (const auto& n : taken) remember(n);
      const bool copying = copies(kind, in);
      const std::vector<std::string> sources = copying ? bodies_of(s, in.value("bodies", json::array())) : std::vector<std::string>();
      apply(e);
      const Scene& t = b.scene();
      std::string last_kept;
      size_t made = 0;
      for (const auto& n : entries) {
        const Node* x = t.node(n);
        if (!x || x->kind != Node::Kind::Body) continue;
        if (auto it = was.find(n); it != was.end() && it->second.event >= 0) {  // an existing body: changed (or left as it was)
          last_kept = n;
          if (it->second.key == x->body_key) continue;
          Event ev{kind == "move" || kind == "scale" ? Event::Type::Moved : Event::Type::Changed, id, x->body_key, it->second.event};
          if (ev.type == Event::Type::Changed)
            for (const auto& tool : taken) {
              const auto w = was.find(tool);
              if (tool == n || w == was.end() || w->second.event < 0 || std::find(entries.begin(), entries.end(), tool) != entries.end()) continue;
              try {
                ev.tools.push_back({tool, w->second.key, w->second.event, place_in(w->second.world, it->second.world)});
              } catch (const std::exception&) {  // a scaled component: not compared
              }
            }
          events[n].push_back(std::move(ev));
          continue;
        }
        if (x->source_op != id) continue;
        Event ev{Event::Type::Made, id, x->body_key};
        const std::string source = copying ? (sources.empty() ? std::string() : sources[made % sources.size()]) : kind == "split" ? last_kept : std::string();
        ++made;
        if (auto w = was.find(source); !source.empty() && w != was.end() && w->second.event >= 0) {
          try {
            ev.source_place = place_in(w->second.world, t.world(n));
            ev.type = copying ? Event::Type::Copied : Event::Type::Made;
            ev.source = source;
            ev.prev = w->second.event;
          } catch (const std::exception&) {
          }
        }
        events[n].push_back(std::move(ev));
      }
    }
    b.finish();
    final = b.take();
  }

  std::vector<FaceOwner> by_ordinal(const std::vector<FaceOwner>& was, const TopoDS_Shape& before, const TopoDS_Shape& after) {
    if (was.size() != static_cast<size_t>(face_count(after)) || !same_faces(before, after)) return {};
    return was;
  }

  std::vector<FaceOwner> compose(const std::vector<FaceMatch>& matches, const std::vector<const std::vector<FaceOwner>*>& from, const std::string& op) {
    std::vector<FaceOwner> out;
    out.reserve(matches.size());
    for (const auto& m : matches) {
      FaceOwner best{op};
      bool have = false, merged = false;
      std::set<std::string> owners;
      for (const auto& h : m.hits) {
        const auto& list = *from[static_cast<size_t>(h.source)];
        if (h.index >= static_cast<int>(list.size())) continue;
        FaceOwner o = list[static_cast<size_t>(h.index)];
        // A face turned inside out (the wall a cut tool leaves) is this step's, made with that tool.
        if (h.against) o = {op, o.via.empty() ? o.op : o.via, false};
        owners.insert(o.op);
        merged |= o.merged;
        if (!have || order_of(o.op) < order_of(best.op)) best = o;
        have = true;
      }
      if (have) best.merged = merged || owners.size() > 1 || m.uncovered;
      out.push_back(best);
    }
    return out;
  }

  std::shared_ptr<const std::vector<FaceMatch>> matches(const std::string& key, const TopoDS_Shape& after, const std::vector<TopoDS_Shape>& sources) {
    {
      std::lock_guard<std::mutex> lock(cache_mu);
      if (auto it = match_cache.find(key); it != match_cache.end()) return it->second;
    }
    auto found = std::make_shared<const std::vector<FaceMatch>>(match_faces(after, sources, cancel));
    std::lock_guard<std::mutex> lock(cache_mu);
    if (match_cache.size() > 512) match_cache.clear();
    match_cache[key] = found;
    return found;
  }

  void compute(const std::string& node, int at) {
    const Event& ev = events.at(node)[static_cast<size_t>(at)];
    TopoDS_Shape after;
    try {
      after = body_shape(doc, ev.key);
    } catch (const std::exception&) {  // a superseded body that is not stored any more: nothing to say about it
      memo[{node, at}] = {};
      return;
    }
    std::vector<FaceOwner> out;
    try {
      if (ev.type == Event::Type::Moved || ev.type == Event::Type::Copied) {
        const std::string& from = ev.type == Event::Type::Moved ? node : ev.source;
        const std::vector<FaceOwner>& was = owners_at(from, ev.prev);
        out = by_ordinal(was, body_shape(doc, events.at(from)[static_cast<size_t>(ev.prev)].key), after);
        if (ev.type == Event::Type::Copied)
          for (auto& o : out) o = {ev.op, o.via.empty() ? o.op : o.via, false};
      } else if (ev.type == Event::Type::Changed || !ev.source.empty()) {
        // The body before (this one, or the one a piece was cut from), then what the step took in.
        const std::string& from = ev.source.empty() ? node : ev.source;
        const Event& before = events.at(from)[static_cast<size_t>(ev.prev)];
        std::vector<TopoDS_Shape> shapes = {placed(body_shape(doc, before.key), ev.source_place)};
        std::vector<const std::vector<FaceOwner>*> owners = {&owners_at(from, ev.prev)};
        std::string key = before.key + ">" + ev.key + "@" + place_text(ev.source_place);
        for (const auto& tool : ev.tools) {
          try {
            shapes.push_back(placed(body_shape(doc, tool.key), tool.place));
          } catch (const std::exception&) {
            continue;
          }
          owners.push_back(&owners_at(tool.node, tool.event));
          key += "|" + tool.key + "@" + place_text(tool.place);
        }
        out = compose(*matches(key, after, shapes), owners, ev.op);
      }
    } catch (const Standard_Failure&) {  // a body the kernel cannot walk: this step owns the faces
      out.clear();
    } catch (const std::exception& e) {  // an earlier body no longer in the store (collected): the same
      if (std::string(e.what()) == "cancelled") throw;
      out.clear();
    }
    if (out.empty()) out.assign(static_cast<size_t>(face_count(after)), FaceOwner{ev.op});
    memo[{node, at}] = std::move(out);
  }

  // Owners of the faces of `node` after its event `at`. The body's own chain is walked forward from the last step
  // already known (a long history is a loop, not a deep recursion); sources and tools recurse into their own chains.
  const std::vector<FaceOwner>& owners_at(const std::string& node, int at) {
    if (auto it = memo.find({node, at}); it != memo.end()) return it->second;
    int from = at;
    while (from > 0 && !memo.count({node, from - 1})) {
      const auto type = events.at(node)[static_cast<size_t>(from)].type;
      if (type != Event::Type::Changed && type != Event::Type::Moved) break;
      --from;
    }
    for (int e = from; e <= at; ++e)
      if (!memo.count({node, e})) compute(node, e);
    return memo[{node, at}];
  }

  // The node's faces now. Kept per document state, so asking again (another selection on the same body, an agent's
  // entity_details) costs no replay.
  const Known& current(const std::string& node) {
    if (auto it = now.find(node); it != now.end()) return it->second;
    const std::string key = state() + "|" + node;
    {
      std::lock_guard<std::mutex> lock(cache_mu);
      // (A viewer document made editable re-keys its bodies in place: a key no longer stored is asked again.)
      if (auto it = owner_cache.find(key); it != owner_cache.end() && (it->second.key.empty() || doc.has_body(it->second.key))) return now[node] = it->second;
    }
    replay();
    Known out;
    const Node* n = final.node(node);
    auto it = events.find(node);
    if (n && n->kind == Node::Kind::Body && n->representation == "solid" && !n->body_missing && it != events.end() && !it->second.empty() &&
        it->second.back().key == n->body_key) {
      out.key = n->body_key;
      out.faces = owners_at(node, static_cast<int>(it->second.size()) - 1);
    }
    std::lock_guard<std::mutex> lock(cache_mu);
    if (owner_cache.size() > 256) owner_cache.clear();
    owner_cache[key] = out;
    return now[node] = std::move(out);
  }
};

Provenance::Provenance(const Document& doc, Cancel cancel) : m(std::make_unique<Impl>(doc, std::move(cancel))) {}
Provenance::~Provenance() = default;

std::vector<FaceOwner> Provenance::face_owners(const std::string& node) { return m->current(node).faces; }

std::vector<FaceOwner> Provenance::edge_owners(const std::string& node) {
  const Known& known = m->current(node);
  const std::vector<FaceOwner>& faces = known.faces;
  if (faces.empty()) return {};
  const TopoDS_Shape shape = body_shape(m->doc, known.key);
  TopTools_IndexedMapOfShape face_map, edges;
  TopExp::MapShapes(shape, TopAbs_FACE, face_map);
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  TopTools_IndexedDataMapOfShapeListOfShape ancestors;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, ancestors);
  std::vector<FaceOwner> out;
  out.reserve(static_cast<size_t>(edges.Extent()));
  for (int i = 1; i <= edges.Extent(); ++i) {
    FaceOwner best;
    bool have = false, differ = false;
    if (ancestors.Contains(edges(i)))
      for (TopTools_ListIteratorOfListOfShape it(ancestors.FindFromKey(edges(i))); it.More(); it.Next()) {
        const int f = face_map.FindIndex(it.Value());
        if (f < 1 || f > static_cast<int>(faces.size())) continue;
        const FaceOwner& o = faces[static_cast<size_t>(f - 1)];
        if (have && o.op != best.op) differ = true;
        if (!have || m->order_of(o.op) > m->order_of(best.op)) best = o;
        have = true;
      }
    if (!have) best = faces.front();
    if (differ) best.merged = false;
    out.push_back(best);
  }
  return out;
}

int Provenance::order(const std::string& op) {
  const int o = m->order_of(op);
  return o == INT_MAX ? -1 : o;
}

const Scene& Provenance::scene() {
  m->replay();
  return m->final;
}

json Provenance::op_info(const std::string& op) {
  const int i = order(op);
  if (i < 0) return nullptr;
  const EffectiveOp& e = m->ops[static_cast<size_t>(i)];
  const json& d = e.data();
  if (e.op->type == "import") {
    std::string name = d.value("source", "");
    name = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
    if (name.empty() && d.contains("nodes") && d["nodes"].is_array() && !d["nodes"].empty()) name = d["nodes"][0].value("name", "");
    return {{"op", op}, {"type", "import"}, {"kind", "import"}, {"name", name.empty() ? std::string("Import") : name}, {"category", "imported"}, {"icon", "import"}};
  }
  const std::string kind = d.value("kind", "");
  const FeatureSpec* spec = feature_spec(kind);
  return {{"op", op}, {"type", e.op->type}, {"kind", kind}, {"name", d.value("name", kind)}, {"category", feature_category(e.op->type, d)}, {"icon", spec ? spec->icon : kind}};
}

json Provenance::describe(const FaceOwner& o) {
  json j = op_info(o.op);
  if (j.is_null()) j = {{"op", o.op}};
  if (!o.via.empty()) j["via"] = op_info(o.via);
  if (o.merged) j["merged"] = true;
  return j;
}

std::string feature_category(const std::string& op_type, const json& data) {
  if (op_type == "import") return "imported";
  const std::string kind = data.value("kind", "");
  static const std::set<std::string> making = {"box", "cylinder", "sphere", "cone", "torus", "extrude", "revolve", "sweep", "loft", "pipe", "coil", "thicken"};
  if (making.count(kind)) {
    const std::string op = data.value("inputs", json::object()).value("operation", "new");
    return op == "join" ? "boss" : op == "cut" ? (kind == "revolve" ? "groove" : "pocket") : op == "intersect" ? "intersect" : "body";
  }
  static const std::map<std::string, std::string> named = {{"offset_face", "press_pull"}, {"move", "transform"}, {"scale", "transform"},
                                                           {"pattern_rect", "pattern"}, {"pattern_circ", "pattern"}};
  if (auto it = named.find(kind); it != named.end()) return it->second;
  return kind;
}

namespace {

// A pocket whose faces are coaxial cylinders and cones with flat steps square to them is a drilled hole.
bool hole_like(const std::vector<TopoDS_Face>& faces) {
  std::optional<gp_Ax1> axis;
  bool round = false;
  for (const auto& f : faces) {
    BRepAdaptor_Surface s(f, Standard_False);
    if (s.GetType() == GeomAbs_Cylinder || s.GetType() == GeomAbs_Cone) {
      const gp_Ax1 a = s.GetType() == GeomAbs_Cylinder ? s.Cylinder().Axis() : s.Cone().Axis();
      if (!axis) axis = a;
      else if (!axis->IsParallel(a, 1e-6) || gp_Lin(*axis).Distance(a.Location()) > 1e-6 * std::max(1.0, a.Location().Distance(gp::Origin()))) return false;
      round |= s.GetType() == GeomAbs_Cylinder;
    } else if (s.GetType() != GeomAbs_Plane) {
      return false;
    }
  }
  if (!round) return false;
  for (const auto& f : faces) {
    BRepAdaptor_Surface s(f, Standard_False);
    if (s.GetType() == GeomAbs_Plane && !s.Plane().Axis().IsParallel(*axis, 1e-6)) return false;
  }
  return true;
}

}  // namespace

json related(const Document& doc, const json& args, const Cancel& cancel) {
  json refs = args.contains("refs") ? args["refs"] : args.contains("ref") ? args["ref"] : json();
  if (refs.is_string() || refs.is_object()) refs = json::array({refs});
  if (!refs.is_array() || refs.empty()) throw Error("related needs refs: faces, edges or bodies, e.g. \"<body>/face/3\"");
  std::set<std::string> kinds = {"feature", "import", "body"}, known = kinds;
  known.insert(recognizer_kinds().begin(), recognizer_kinds().end());
  const bool asked = args.contains("kinds") && args["kinds"].is_array();
  if (asked) {
    kinds.clear();
    for (const auto& k : args["kinds"]) {
      if (!k.is_string() || !known.count(k.get<std::string>())) throw Error("related: unknown kind " + k.dump() + " (feature, import, body, hole, fillet, chamfer, boss, pocket, wall, tangent, loop, similar)");
      kinds.insert(k.get<std::string>());
    }
  } else {
    kinds = known;
  }
  const size_t limit = static_cast<size_t>(std::clamp(args.value("limit", 500), 1, 5000));
  Provenance p(doc, cancel);
  const Scene& scene = p.scene();
  std::vector<std::string> bodies;
  std::map<std::string, std::vector<std::string>> picked;  // body -> owners of its picked faces and edges
  std::map<std::string, std::pair<std::vector<int>, std::vector<int>>> subs;  // body -> picked face and edge ordinals
  json items = json::array();
  for (const auto& r : refs) {
    const Ref ref = Ref::from_json(r);
    if (ref.kind != Ref::Kind::Face && ref.kind != Ref::Kind::Edge && ref.kind != Ref::Kind::Body)
      throw Error(std::string("related takes faces, edges or bodies, not a ") + Ref::kind_name(ref.kind));
    const Node* n = scene.node(ref.body);
    if (!n || n->kind != Node::Kind::Body) throw Error("related: " + ref.body + " is not a body of the document");
    if (std::find(bodies.begin(), bodies.end(), ref.body) == bodies.end()) bodies.push_back(ref.body);
    if (ref.kind != Ref::Kind::Body) (ref.kind == Ref::Kind::Face ? subs[ref.body].first : subs[ref.body].second).push_back(ref.index);
    json item = {{"ref", ref.str()}};
    if (ref.kind != Ref::Kind::Body) {
      const auto owners = ref.kind == Ref::Kind::Face ? p.face_owners(ref.body) : p.edge_owners(ref.body);
      if (owners.empty()) {
        item["provenance"] = false;
      } else {
        if (ref.index < 0 || ref.index >= static_cast<int>(owners.size())) throw Error("related: " + ref.str() + " does not exist");
        item["owner"] = p.describe(owners[static_cast<size_t>(ref.index)]);
        picked[ref.body].push_back(owners[static_cast<size_t>(ref.index)].op);
      }
    }
    items.push_back(item);
  }
  // Per owner, the faces it made on the picked bodies: the owners of the picks, or every owner when only bodies were picked.
  struct Candidate {
    json refs = json::array();
    size_t count = 0, merged = 0;
    std::vector<std::string> bodies, via;
    std::vector<TopoDS_Face> faces;
  };
  std::map<std::string, Candidate> by_op;
  std::vector<std::string> listed;
  std::set<std::string> wanted;
  for (const auto& [b, ops] : picked) wanted.insert(ops.begin(), ops.end());
  std::map<std::string, size_t> face_total;
  for (const auto& b : bodies) {
    const auto owners = p.face_owners(b);
    face_total[b] = owners.size();
    if (owners.empty()) continue;
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(body_shape(doc, scene.node(b)->body_key), TopAbs_FACE, faces);
    for (size_t i = 0; i < owners.size(); ++i) {
      const FaceOwner& o = owners[i];
      if (!wanted.empty() && !wanted.count(o.op)) continue;
      if (!by_op.count(o.op)) listed.push_back(o.op);
      Candidate& c = by_op[o.op];
      if (c.refs.size() < limit) c.refs.push_back(b + "/face/" + std::to_string(i));
      ++c.count;
      c.merged += o.merged;
      if (std::find(c.bodies.begin(), c.bodies.end(), b) == c.bodies.end()) c.bodies.push_back(b);
      if (!o.via.empty() && std::find(c.via.begin(), c.via.end(), o.via) == c.via.end()) c.via.push_back(o.via);
      if (c.faces.size() < 64 && static_cast<int>(i) < faces.Extent()) c.faces.push_back(TopoDS::Face(faces(static_cast<int>(i) + 1)));
    }
  }
  json candidates = json::array();
  for (const auto& op : listed) {
    const Candidate& c = by_op[op];
    json info = p.op_info(op);
    if (info.is_null()) continue;
    const std::string type = info["type"] == "import" ? "import" : "feature";
    if (!kinds.count(type)) continue;
    bool all = !picked.empty();
    for (const auto& [b, ops] : picked)
      for (const auto& o : ops) all = all && o == op;
    json j = {{"kind", type}, {"op", op}, {"name", info["name"]}, {"feature_kind", info["kind"]}, {"category", info["category"]}, {"icon", info["icon"]},
              {"bodies", c.bodies}, {"refs", c.refs}, {"count", c.count}, {"contains_selection", all}};
    if (info["category"] == "pocket" && c.count <= c.faces.size() && hole_like(c.faces)) j["category"] = "hole";
    if (c.merged) j["merged"] = c.merged;
    if (c.refs.size() < c.count) j["truncated"] = true;
    if (!c.via.empty()) {
      j["via"] = json::array();
      for (const auto& v : c.via) j["via"].push_back(p.op_info(v));
    }
    if (type == "import") j["provenance"] = false;
    // A feature that made part of the body comes first; the one that made (nearly) all of it, or the import, after the
    // groups recognised on it.
    size_t total = 0;
    for (const auto& b : c.bodies) total += face_total[b];
    j["_tier"] = !all ? 5 : type == "feature" && c.count * 10 < total * 9 ? 0 : 2;
    candidates.push_back(j);
  }
  // TODO 11 UI-97: what the geometry itself shows (holes, fillets, chamfers, bosses, pockets, walls, chains, loops,
  // similar entities), on the picked solids; for bodies picked whole, every group of the kinds asked for.
  std::set<std::string> recognize;
  for (const auto& k : recognizer_kinds())
    if (kinds.count(k)) recognize.insert(k);
  for (const auto& b : recognize.empty() ? std::vector<std::string>() : bodies) {
    const Node* n = scene.node(b);
    const auto sub = subs.find(b);
    if (n->representation != "solid" || n->body_missing || (sub == subs.end() && !asked)) continue;
    Recognizer rec(node_world_shape(doc, scene, b), cancel);
    std::vector<Recognized> found;
    if (sub != subs.end()) {
      found = rec.around(sub->second.first, sub->second.second, recognize);
    } else {
      for (const char* k : {"hole", "fillet", "chamfer", "boss", "pocket", "wall"})
        if (recognize.count(k))
          for (auto& g : rec.all(k)) found.push_back(std::move(g));
    }
    const bool only = sub != subs.end() && bodies.size() == 1;
    for (const auto& g : found) {
      const bool edges = g.faces.empty();
      json list = json::array();
      for (int i : edges ? g.edges : g.faces) {
        if (list.size() >= limit) break;
        list.push_back(b + (edges ? "/edge/" : "/face/") + std::to_string(i));
      }
      const size_t count = edges ? g.edges.size() : g.faces.size();
      json j = {{"kind", g.kind}, {"label", g.label}, {"params", g.params}, {"bodies", {b}}, {"refs", list}, {"count", count}, {"contains_selection", only}};
      if (!g.rule.empty()) j["rule"] = g.rule;
      if (list.size() < count) j["truncated"] = true;
      j["_tier"] = !only ? 5 : g.kind == "similar" ? 4 : g.kind == "tangent" || g.kind == "loop" ? 3 : 1;
      candidates.push_back(j);
    }
  }
  auto rank = [](const json& c) {  // smaller first within a tier; similar rules keep their order
    const bool similar = c["kind"] == "similar";
    return std::make_tuple(c["_tier"].get<int>(), similar, similar ? size_t(0) : c["count"].get<size_t>());
  };
  std::stable_sort(candidates.begin(), candidates.end(), [&](const json& a, const json& b) { return rank(a) < rank(b); });
  for (auto& c : candidates) c.erase("_tier");
  if (kinds.count("body"))
    for (const auto& b : bodies) {
      const Node* n = scene.node(b);
      candidates.push_back({{"kind", "body"}, {"body", b}, {"name", n->name}, {"refs", json::array({b})}, {"count", 1}, {"contains_selection", true}});
    }
  return {{"refs", items}, {"candidates", candidates}};
}

}  // namespace opad::design
