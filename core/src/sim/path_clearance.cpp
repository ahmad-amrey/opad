// A motion study's path clearance (MCP stress test #21): how close the moving parts' bodies come to each other at every
// frame, the first collision and the closest approach, without posing each frame and running `clearance` by hand.
//
// Per frame, the pairs are taken nearest boxes first (each body's box moved with its part: a lower bound of its distance)
// until a box is farther than the nearest distance found or the clearance asked for. A distance is between meshes: each
// body entry meshed once with its triangle tree, two trees walked against each other through the parts' relative
// placement (nothing rebuilt per frame, no exact extrema), the walk stopped once the pair cannot be the frame's nearest,
// once per standing of the two parts (a part held still repeats its frame). Runs of frames go side by side on worker
// threads, each frame starting from the pair nearest in the frame before. Exact distances (and a Boolean for a
// collision's volume) are asked only to confirm: the closest approach, and the frames where two meshes meet.
// OPAD_CLEARANCE_TRACE=1 prints each phase's time and the slow pairs.
#include "opad/sim/study.hpp"
#include "opad/sim/joints.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/mass.hpp"

namespace opad::sim {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kTouch = 1e-6;  // mm: an exact distance at or under this is contact (as the clearance command)
// Mesh distances over 1 mm (and the clearance asked for) may come out this fraction high: two faces far apart that run
// parallel or concentric (a yoke around a cup) tie over their whole area, and an exact minimum visits every pair of them.
constexpr double kRelative = 0.05;

struct Box {
  double lo[3] = {kInf, kInf, kInf}, hi[3] = {-kInf, -kInf, -kInf};
};

struct Item {
  std::string id, name, key;
  int group = -1;            // index into the run's parts; -1: it does not move
  Mat4 world;                // where the scene has it
  bool rigid = true;         // world is a placement (else its shape is a scaled copy, meshed apart)
  Box box;                   // its box there
  std::vector<Vec3> samples; // points on it there: the vertices nearest its box's corners and middle
  TopoDS_Shape exact;          // at `world`
};

double box_gap(const Box& a, const Box& b) {
  double s = 0;
  for (int k = 0; k < 3; ++k) {
    const double g = std::max({0.0, a.lo[k] - b.hi[k], b.lo[k] - a.hi[k]});
    s += g * g;
  }
  return std::sqrt(s);
}

bool box_inside(const Box& in, const Box& out) {
  for (int k = 0; k < 3; ++k)
    if (in.lo[k] < out.lo[k] || in.hi[k] > out.hi[k]) return false;
  return true;
}

Box moved_box(const Box& b, const Mat4& d) {
  Box out;
  for (int c = 0; c < 8; ++c) {
    const Vec3 p = d.apply({c & 1 ? b.hi[0] : b.lo[0], c & 2 ? b.hi[1] : b.lo[1], c & 4 ? b.hi[2] : b.lo[2]});
    for (int k = 0; k < 3; ++k) out.lo[k] = std::min(out.lo[k], p[k]), out.hi[k] = std::max(out.hi[k], p[k]);
  }
  return out;
}

double dist(const Vec3& a, const Vec3& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

bool same(const Mat4& a, const Mat4& b) {
  for (size_t i = 0; i < 16; ++i)
    if (std::fabs(a.m[i] - b.m[i]) > 1e-9 * (i % 4 == 3 ? 1000 : 1)) return false;
  return true;
}

// Runs task(i) for i < n on the hardware's threads; the calling thread takes part and alone reports through `tick`
// (a false answer stops the rest).
template <class Task, class Tick>
void side_by_side(size_t n, Task&& task, Tick&& tick) {
  std::atomic<size_t> next{0};
  std::atomic<bool> stop{false};
  std::mutex mu;
  std::exception_ptr failed;
  auto work = [&](bool main) {
    for (size_t i = next++; i < n && !stop; i = next++) {
      try {
        task(i);
      } catch (...) {
        std::lock_guard<std::mutex> lock(mu);
        if (!failed) failed = std::current_exception();
        stop = true;
      }
      if (main && !tick(std::min(next.load(), n), n)) stop = true;
    }
  };
  std::vector<std::thread> pool;
  const size_t threads = std::min<size_t>(n, std::max(2u, std::thread::hardware_concurrency()));
  for (size_t i = 1; i < threads; ++i) pool.emplace_back(work, false);
  work(true);
  for (auto& t : pool) t.join();
  if (failed) std::rethrow_exception(failed);
  if (stop) throw Error("cancelled");
}

std::vector<std::string> solid_bodies_under(const Scene& scene, const json& ids, const char* key) {
  const json list = ids.is_string() ? json::array({ids}) : ids;
  if (!list.is_array() || list.empty()) throw Error(std::string("clearance: ") + key + " takes body or component ids");
  std::vector<std::string> out;
  for (const auto& id : list) {
    if (!id.is_string() || !scene.node(id.get<std::string>())) throw Error("clearance: unknown node " + id.dump());
    for (const auto& b : scene.bodies_under(id.get<std::string>())) {
      const Node* n = scene.node(b);
      if (n && n->kind == Node::Kind::Body && !n->body_missing && n->representation == "solid" && std::find(out.begin(), out.end(), b) == out.end())
        out.push_back(b);
    }
  }
  return out;
}

}  // namespace

json path_clearance(const Document& doc, const Scene& scene, StudyRun& run, const json& given, const Progress& progress) {
  const json spec = given.is_object() ? given : json::object();
  const double clearance = spec.value("clearance_mm", 0.0);
  if (!(clearance >= 0) || !std::isfinite(clearance)) throw Error("clearance: clearance_mm is zero or more");
  // Faces that touch, or overlap no deeper than this (a designed stop meshed and cut a hair into its seat), are a contact,
  // not a collision. 0 (the default): every meet is a collision, as before.
  const double contact_tolerance = spec.value("contact_tolerance_mm", 0.0);
  if (!(contact_tolerance >= 0) || !std::isfinite(contact_tolerance)) throw Error("clearance: contact_tolerance_mm is zero or more");
  const size_t frames = run.poses.size();
  if (!frames) return json{{"frames", 0}, {"status", "clear"}};
  // Which part moves each body: the nearest of its ancestors (or itself) the mechanism places.
  std::map<std::string, int> part_index;
  for (size_t i = 0; i < run.parts.size(); ++i) part_index[run.parts[i]] = int(i);
  auto group_of = [&](const std::string& id) {
    for (std::string at = id; !at.empty();) {
      if (const auto it = part_index.find(at); it != part_index.end()) return it->second;
      const Node* n = scene.node(at);
      at = n ? n->parent : std::string();
    }
    return -1;
  };
  // The two sides: a and b (bodies or components, hidden ones too); a alone: against the mechanism's other bodies; neither:
  // every pair of the mechanism's visible solids in parts that move apart.
  std::vector<std::string> mech;
  for (const auto& p : run.parts)
    for (const auto& b : solid_bodies_under(scene, json(p), "parts"))
      if (scene.effectively_visible(b) && std::find(mech.begin(), mech.end(), b) == mech.end()) mech.push_back(b);
  const bool named = spec.contains("a");
  if (spec.contains("b") && !named) throw Error("clearance: b needs a (the other side)");
  std::vector<std::string> side_a = named ? solid_bodies_under(scene, spec["a"], "a") : mech, side_b;
  if (spec.contains("b")) side_b = solid_bodies_under(scene, spec["b"], "b");
  else if (named)
    for (const auto& b : mech)
      if (std::find(side_a.begin(), side_a.end(), b) == side_a.end()) side_b.push_back(b);
  std::set<std::pair<std::string, std::string>> ignored;
  for (const auto& pair : spec.value("ignore", json::array())) {
    if (!pair.is_array() || pair.size() != 2) throw Error("clearance: ignore takes pairs [[a, b], ...]");
    for (const auto& x : solid_bodies_under(scene, pair[0], "ignore"))
      for (const auto& y : solid_bodies_under(scene, pair[1], "ignore")) ignored.insert(std::minmax(x, y));
  }
  std::vector<Item> items;
  std::map<std::string, size_t> index;
  auto item = [&](const std::string& id) {
    if (const auto it = index.find(id); it != index.end()) return it->second;
    const Node* n = scene.node(id);
    Item it;
    it.id = id;
    it.name = n->name;
    it.key = n->body_key;
    it.group = group_of(id);
    it.world = scene.world(id);
    it.rigid = mat_is_rigid(it.world);
    index[id] = items.size();
    items.push_back(std::move(it));
    return items.size() - 1;
  };
  struct Pair {
    size_t a, b;
    bool constant = false;  // the two never move against each other: one distance serves every frame
    bool followed = true;
  };
  std::vector<Pair> pairs;
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& x : side_a)
    for (const auto& y : named ? side_b : side_a) {
      if (x == y || ignored.count(std::minmax(x, y)) || !seen.insert(std::minmax(x, y)).second) continue;
      if (!named && group_of(x) == group_of(y)) continue;  // one rigid part: its bodies keep their distance
      pairs.push_back({item(x), item(y)});
      if (pairs.size() > 200000) throw Error("clearance: more than 200000 pairs of bodies: name smaller groups with a and b");
    }
  json out = {{"frames", frames}, {"bodies", items.size()}, {"pairs", pairs.size()}, {"clearance_mm", clearance}};
  if (contact_tolerance > 0) out["contact_tolerance_mm"] = contact_tolerance;
  if (pairs.empty()) {
    out["status"] = "clear";
    out["note"] = "no pair of bodies in parts that move apart";
    return out;
  }
  auto tick = [&](double f) { return !progress || progress(f, "Checking clearance"); };
  // OPAD_CLEARANCE_TRACE=1: each phase's time on stderr.
  const bool tracing = std::getenv("OPAD_CLEARANCE_TRACE") != nullptr;
  const auto began = std::chrono::steady_clock::now();
  auto trace = [&](const char* phase) {
    if (!tracing) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    std::fprintf(stderr, "clearance: %s %lld ms\n", phase, static_cast<long long>(ms));
  };
  // Each body where the scene has it: shape, box, sample points (on the calling thread: the shape cache's parse).
  for (auto& it : items) {
    it.exact = node_world_shape(doc, scene, it.id);
    const Bnd_Box b = node_world_bbox(doc, scene, it.id);
    if (!b.IsVoid()) b.Get(it.box.lo[0], it.box.lo[1], it.box.lo[2], it.box.hi[0], it.box.hi[1], it.box.hi[2]);
    Vec3 target[9];
    double best[9];
    for (int c = 0; c < 9; ++c) {
      for (int k = 0; k < 3; ++k) target[c][k] = c == 8 ? (it.box.lo[k] + it.box.hi[k]) / 2 : ((c >> k) & 1 ? it.box.hi[k] : it.box.lo[k]);
      best[c] = kInf;
    }
    Vec3 pick[9];
    for (TopExp_Explorer v(it.exact, TopAbs_VERTEX); v.More(); v.Next()) {
      const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
      const Vec3 q{p.X(), p.Y(), p.Z()};
      for (int c = 0; c < 9; ++c)
        if (const double d = dist(q, target[c]); d < best[c]) best[c] = d, pick[c] = q;
    }
    for (int c = 0; c < 9; ++c)
      if (best[c] < kInf && std::find(it.samples.begin(), it.samples.end(), pick[c]) == it.samples.end()) it.samples.push_back(pick[c]);
  }
  trace("shapes");
  // Each part's motion per frame from where the scene has it (group -1: none), and whether two parts move apart.
  const size_t groups = run.parts.size() + 1;
  std::vector<std::vector<Mat4>> delta(groups, std::vector<Mat4>(frames));
  std::vector<std::vector<TopLoc_Location>> moved(groups, std::vector<TopLoc_Location>(frames));
  for (size_t g = 1; g < groups; ++g) {
    const Mat4 back = scene.world(run.parts[g - 1]).inverse();
    for (size_t f = 0; f < frames; ++f) {
      delta[g][f] = run.poses[f][g - 1] * back;
      moved[g][f] = TopLoc_Location(trsf_from_mat(delta[g][f]));
    }
  }
  std::map<std::pair<int, int>, bool> constant;
  for (auto& p : pairs) {
    const int ga = items[p.a].group + 1, gb = items[p.b].group + 1;
    const auto key = std::minmax(ga, gb);
    auto it = constant.find(key);
    if (it == constant.end()) {
      bool c = true;
      const Mat4 rel0 = delta[size_t(key.first)][0].inverse() * delta[size_t(key.second)][0];
      for (size_t f = 1; f < frames && c; ++f) c = same(delta[size_t(key.first)][f].inverse() * delta[size_t(key.second)][f], rel0);
      it = constant.emplace(key, c).first;
    }
    p.constant = it->second;
  }
  // The bodies' boxes per frame.
  auto frame_box = [&](size_t i, size_t f) { return moved_box(items[i].box, delta[size_t(items[i].group + 1)][f]); };
  // A frame where a part stands where it stood at an earlier one (a table driver's hold) is that frame for it: a pair whose
  // two parts both repeat an earlier frame repeats its distance.
  std::vector<std::vector<size_t>> canon(groups, std::vector<size_t>(frames));
  for (size_t g = 0; g < groups; ++g) {
    std::vector<size_t> seen_frames;
    for (size_t f = 0; f < frames; ++f) {
      canon[g][f] = f;
      for (size_t s : seen_frames)
        if (same(delta[g][s], delta[g][f])) {
          canon[g][f] = s;
          break;
        }
      if (canon[g][f] == f) seen_frames.push_back(f);
    }
  }
  // Meshes: a copy of each body entry meshed once, fine for its size, with its tree (rigid instances share it; a scaled
  // body gets its own), made by the first worker that needs it.
  struct Mesh {
    std::once_flag once;
    std::shared_ptr<const MeshSet> set;  // null: no face could be meshed (exact distances then)
    double deflection = 0;
  };
  std::map<std::string, Mesh> meshes;
  std::vector<Mesh*> mesh_of(items.size());
  for (size_t i = 0; i < items.size(); ++i) mesh_of[i] = &meshes[items[i].rigid ? items[i].key : "#" + items[i].id];
  auto mesh = [&](size_t i) -> const Mesh& {
    Mesh& m = *mesh_of[i];
    std::call_once(m.once, [&] {
      const Item& it = items[i];
      const auto t0 = std::chrono::steady_clock::now();
      const TopoDS_Shape source = it.rigid ? body_shape(doc, it.key) : it.exact;
      const double size = dist({it.box.lo[0], it.box.lo[1], it.box.lo[2]}, {it.box.hi[0], it.box.hi[1], it.box.hi[2]});
      const double deflection = std::clamp(size * 2e-4, 2e-3, 0.1);
      TopoDS_Shape copy = BRepBuilderAPI_Copy(source, Standard_False, Standard_False).Shape();
      BRepMesh_IncrementalMesh(copy, deflection, Standard_False, 0.3, Standard_False);
      m.set = mesh_set(copy);
      m.deflection = deflection;  // asked of the mesher: a face it could not hold to that (a bad face) is not the body's rule
      if (tracing)
        std::fprintf(stderr, "clearance: meshed %s (%.3f mm) %lld ms\n", it.name.c_str(), deflection,
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    });
    return m;
  };
  auto mesh_frame = [&](size_t i) { return items[i].rigid ? items[i].world : Mat4(); };  // the mesh's coordinates in the world
  auto tol_of = [&](const Pair& p) { return 2 * std::max(mesh(p.a).deflection, mesh(p.b).deflection) + kTouch; };
  auto place = [&](const TopoDS_Shape& s, size_t i, size_t f) {
    const size_t g = size_t(items[i].group + 1);
    return g == 0 ? s : s.Moved(moved[g][f]);
  };
  // The distances taken: a pair at a frame (and every frame it stands the same; a constant pair at every frame).
  struct Task {
    size_t f, p;
    std::vector<size_t> frames;
    double value = kInf;
    Vec3 pa{}, pb{};
    bool inside = false;
    bool above = false;  // walked only far enough to know it is value or more (not its frame's nearest)
  };
  std::atomic<size_t> mesh_checks{0};
  auto measure = [&](Task& t, double stop) {
    const Pair& p = pairs[t.p];
    const Item &ia = items[p.a], &ib = items[p.b];
    const Mesh &ma = mesh(p.a), &mb = mesh(p.b);
    if (ma.set && mb.set) {
      // b's mesh into a's coordinates: (Da Fa)^-1 (Db Fb), both rigid; the points back into the world at this frame.
      const Mat4 wa = delta[size_t(ia.group + 1)][t.f] * mesh_frame(p.a), wb = delta[size_t(ib.group + 1)][t.f] * mesh_frame(p.b);
      const auto t0 = std::chrono::steady_clock::now();
      const MeshGap gap = mesh_distance(*ma.set, *mb.set, wa.inverse() * wb, kRelative, std::max(clearance, 1.0), stop,
                                        std::max(ma.deflection, mb.deflection));
      if (tracing) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (us > 20000) std::fprintf(stderr, "clearance: slow %s / %s %.3f mm %lld us\n", ia.name.c_str(), ib.name.c_str(), gap.value, static_cast<long long>(us));
      }
      ++mesh_checks;
      t.value = gap.value, t.pa = wa.apply(gap.a), t.pb = wa.apply(gap.b), t.above = gap.above;
    } else {  // nothing meshed: the exact distance
      if (tracing) std::fprintf(stderr, "clearance: exact %s / %s\n", ia.name.c_str(), ib.name.c_str());
      const json d = shape_distance(place(ia.exact, p.a, t.f), place(ib.exact, p.b, t.f));
      t.value = d.value("value", kInf);
      t.pa = d.value("point_a", Vec3{}), t.pb = d.value("point_b", Vec3{});
    }
    // A body wholly inside another leaves their surfaces apart: one of its points inside the other is a collision.
    if (!t.above && t.value > tol_of(p) && !ia.samples.empty() && !ib.samples.empty()) {
      const Box ba = frame_box(p.a, t.f), bb = frame_box(p.b, t.f);
      for (const auto& [in, around] : {std::make_pair(p.a, p.b), std::make_pair(p.b, p.a)}) {
        if (!box_inside(in == p.a ? ba : bb, in == p.a ? bb : ba)) continue;
        const Vec3 q = delta[size_t(items[in].group + 1)][t.f].apply(items[in].samples.front());
        BRepClass3d_SolidClassifier cls(place(items[around].exact, around, t.f), gp_Pnt(q[0], q[1], q[2]), 1e-6);
        if (cls.State() == TopAbs_IN) {
          t.value = 0, t.inside = true, t.pa = t.pb = q;
          break;
        }
      }
    }
  };
  // A frame's followed pairs, nearest boxes first.
  auto order_at = [&](size_t f) {
    std::vector<Box> boxes(items.size());
    std::vector<char> boxed(items.size(), 0);
    auto box = [&](size_t i) -> const Box& {
      if (!boxed[i]) boxes[i] = frame_box(i, f), boxed[i] = 1;
      return boxes[i];
    };
    std::vector<std::pair<double, size_t>> order;
    for (size_t k = 0; k < pairs.size(); ++k)
      if (pairs[k].followed) order.push_back({box_gap(box(pairs[k].a), box(pairs[k].b)), k});
    std::sort(order.begin(), order.end());
    return order;
  };
  auto throttled = [&](double from, double span) {
    return [&, from, span, last = std::chrono::steady_clock::now()](size_t done, size_t n) mutable {
      const auto now = std::chrono::steady_clock::now();
      if (now - last < std::chrono::milliseconds(100)) return true;
      last = now;
      return tick(from + span * double(done) / double(std::max<size_t>(n, 1)));
    };
  };
  auto exact_at = [&](const Pair& p, size_t f) {
    return shape_distance(place(items[p.a].exact, p.a, f), place(items[p.b].exact, p.b, f));
  };
  size_t exact_checks = 0;
  auto names = [&](const Pair& p) {
    return json{{"a", items[p.a].id}, {"b", items[p.b].id}, {"a_name", items[p.a].name}, {"b_name", items[p.b].name}};
  };
  // The start: pairs already in contact (a pin in its bore: boxes that meet, then meshes within their tolerance) are listed
  // and not followed: joined parts touch by design (name a pair in ignore to leave it out of this list too).
  json at_start = json::array();
  size_t at_start_count = 0;
  {
    std::vector<Task> start;
    for (const auto& [gap, k] : order_at(0)) {
      if (gap > kTouch) break;
      start.push_back({0, k, {0}});
    }
    side_by_side(start.size(), [&](size_t k) { measure(start[k], 2 * tol_of(pairs[start[k].p])); }, throttled(0, 0.1));
    for (const auto& t : start) {
      Pair& p = pairs[t.p];
      if (t.value > tol_of(p)) continue;
      p.followed = false;
      ++at_start_count;
      if (at_start.size() < 20) {
        json e = names(p);
        e["mesh_distance_mm"] = t.value;
        at_start.push_back(e);
      }
    }
  }
  trace("start");
  // Every frame: its pairs nearest boxes first until a box is farther than the nearest distance found (or the clearance),
  // each pair once per standing of its two parts (a constant pair once), and a walk stopped as soon as it cannot come
  // nearer than that. Frames go in runs side by side, each frame starting from the pair nearest in the frame before it
  // (the distance that bounds the rest at once).
  std::mutex done_mu;
  std::map<std::tuple<size_t, size_t, size_t>, std::shared_ptr<Task>> done;
  std::vector<std::vector<std::shared_ptr<Task>>> at_frame(frames);
  constexpr size_t kAny = std::numeric_limits<size_t>::max();
  auto distance_at = [&](size_t f, size_t k, double bound) {
    const Pair& p = pairs[k];
    const auto key = p.constant ? std::make_tuple(k, kAny, kAny)
                                : std::make_tuple(k, canon[size_t(items[p.a].group + 1)][f], canon[size_t(items[p.b].group + 1)][f]);
    {
      std::lock_guard<std::mutex> lock(done_mu);
      // Taken again unless only walked to a bound this frame could still beat.
      if (const auto it = done.find(key); it != done.end() && (!it->second->above || it->second->value >= bound)) return it->second;
    }
    auto made = std::make_shared<Task>(Task{f, k, {}});
    measure(*made, bound);
    std::lock_guard<std::mutex> lock(done_mu);
    auto& slot = done[key];
    if (!slot || (slot->above && !made->above)) slot = made;
    return made;
  };
  const size_t runs = std::min<size_t>(frames, std::max(2u, std::thread::hardware_concurrency()) * 2);
  side_by_side(
      runs,
      [&](size_t r) {
        size_t seed = kAny;  // the nearest pair of the frame before
        for (size_t f = frames * r / runs; f < frames * (r + 1) / runs; ++f) {
          double best = kInf;
          size_t nearest_pair = kAny;
          auto take = [&](size_t k) {
            const auto t = distance_at(f, k, std::max(best, clearance) + 1e-9);
            at_frame[f].push_back(t);
            if (!t->above && t->value < best) best = t->value, nearest_pair = k;
          };
          const auto order = order_at(f);
          if (seed != kAny && pairs[seed].followed) take(seed);
          for (const auto& [gap, k] : order) {
            if (gap > std::max(best, clearance) + 1e-9) break;
            if (k != seed) take(k);
          }
          seed = nearest_pair;
          if (tracing) std::fprintf(stderr, "clearance: frame %zu: %zu pairs, nearest %.4f mm\n", f, at_frame[f].size(), best);
        }
      },
      throttled(0.1, 0.8));
  // One task per distance, with the frames it stands for (in time order: its first is when it is first seen).
  std::vector<Task> tasks;
  std::map<const Task*, size_t> task_index;
  for (size_t f = 0; f < frames; ++f)
    for (const auto& t : at_frame[f]) {
      const auto [it, fresh] = task_index.emplace(t.get(), tasks.size());
      if (fresh) tasks.push_back(*t), tasks.back().frames.clear();
      tasks[it->second].frames.push_back(f);
    }
  for (auto& t : tasks) t.f = t.frames.front();
  trace("measured");
  // Per frame: the smallest of its pairs.
  std::vector<std::vector<const Task*>> by_frame(frames);
  for (const auto& t : tasks)
    for (size_t f : t.frames) by_frame[f].push_back(&t);
  std::vector<double> per_frame(frames, kInf);
  for (size_t f = 0; f < frames; ++f)
    for (const Task* t : by_frame[f]) per_frame[f] = std::min(per_frame[f], t->value);
  // Collisions: frames where two meshes meet (or one body is inside another), confirmed exactly in time order, at most
  // four frames per pair (its first, last and two between) and 48 in all (or 15 s); the first confirmed is the first collision.
  std::map<size_t, std::vector<size_t>> meet_frames;  // pair -> frames (a constant pair: frame 0)
  for (const auto& t : tasks)
    if (!t.above && (t.inside || t.value <= tol_of(pairs[t.p]))) meet_frames[t.p].push_back(t.f);
  std::vector<std::pair<size_t, size_t>> checks;  // (frame, pair)

  for (auto& [p, fs] : meet_frames) {
    std::sort(fs.begin(), fs.end());

    const size_t picks = std::min<size_t>(4, fs.size());
    for (size_t k = 0; k < picks; ++k) checks.push_back({fs[picks == 1 ? 0 : k * (fs.size() - 1) / (picks - 1)], p});
  }
  std::sort(checks.begin(), checks.end());
  checks.erase(std::unique(checks.begin(), checks.end()), checks.end());
  // The joints held at a limit at a frame (deg / mm, as their limits are written), joining one of the pair's parts: a
  // contact there is the designed stop.
  struct Limited {
    std::string joint, name, coord, side, base, part;
    double limit;
    std::vector<double> values;
  };
  std::vector<Limited> limited;
  for (const auto& j : scene.joints) {
    const JointKind* k = joint_kind(j.kind);
    const auto l = j.def.find("limits");
    if (!k || l == j.def.end() || !l->is_object()) continue;
    std::vector<const Series*> values;  // value series in coordinate order (each coordinate pushes value, speed, acceleration)
    for (const auto& s : run.series)
      if (s.id == j.id && s.group == "value") values.push_back(&s);
    for (size_t c = 0; c < k->coords.size() && c < values.size(); ++c) {
      const auto lim = l->find(k->coords[c].name);
      if (lim == l->end() || !lim->is_array() || lim->size() != 2) continue;
      for (int side = 0; side < 2; ++side)
        if ((*lim)[size_t(side)].is_number())
          limited.push_back({j.id, j.name, k->coords[c].name, side ? "upper" : "lower", j.base, j.part, (*lim)[size_t(side)].get<double>(), values[c]->v});
    }
  }
  auto part_of = [&](size_t item) { return items[item].group < 0 ? std::string() : run.parts[size_t(items[item].group)]; };
  auto at_limit = [&](const Pair& p, size_t f) {
    json out = json::array();
    const std::string a = part_of(p.a), b = part_of(p.b);
    for (const auto& l : limited) {
      if (f >= l.values.size() || std::fabs(l.values[f] - l.limit) > 1e-6 * std::max(1.0, std::fabs(l.limit))) continue;
      const bool joins = (!a.empty() && (l.part == a || l.base == a)) || (!b.empty() && (l.part == b || l.base == b));
      if (joins) out.push_back({{"joint", l.joint}, {"name", l.name}, {"coordinate", l.coord}, {"limit", l.side}, {"value", l.limit}});
    }
    return out;
  };
  json first = nullptr, contacts = json::array();
  size_t first_frame = frames, confirmed = 0, contact_count = 0;
  const auto confirming = std::chrono::steady_clock::now();
  std::set<size_t> collided, contacted;
  for (const auto& [f, k] : checks) {
    if (f > first_frame || confirmed >= 48 || std::chrono::steady_clock::now() - confirming > std::chrono::seconds(15)) break;
    ++confirmed;
    const Pair& p = pairs[k];
    const Task* t = nullptr;
    for (const auto& x : tasks)
      if (x.p == k && x.f == f) t = &x;
    std::string kind = "inside";
    double volume = 0, depth = kInf;
    if (!t || !t->inside) {
      ++exact_checks;
      if (exact_at(p, f).value("value", kInf) > kTouch) continue;
      kind = "touching", depth = 0;
      try {  // touching or crossing: the common volume says
        const TopoDS_Shape sa = place(items[p.a].exact, p.a, f), sb = place(items[p.b].exact, p.b, f);
        BRepAlgoAPI_Common common(sa, sb);
        if (common.IsDone() && TopExp_Explorer(common.Shape(), TopAbs_SOLID).More()) {
          const double v = volume_properties(common.Shape()).mass;
          if (v > std::max(1e-9, 1e-6 * std::min(volume_properties(sa).mass, volume_properties(sb).mass))) {
            kind = "intersecting", volume = v;
            // How deep they overlap: a thin overlap's area is about twice its contact patch, so depth ~ 2 V / A.
            const double area = area_properties(common.Shape()).mass;
            depth = area > 0 ? 2 * v / area : kInf;
          }
        }
      } catch (const Standard_Failure&) {
      }
    }
    const json limits = at_limit(p, f);
    if (contact_tolerance > 0 && depth <= contact_tolerance) {  // a contact: listed, the pair followed no further
      contacted.insert(k);
      ++contact_count;
      if (contacts.size() < 20) {
        json e = names(p);
        e["t"] = run.t[f];
        e["frame"] = f;
        e["kind"] = kind;
        e["depth_mm"] = depth;
        if (volume > 0) e["volume_mm3"] = volume;
        if (!limits.empty()) e["at_limit"] = limits;
        e["expected"] = !limits.empty();
        contacts.push_back(e);
      }
      continue;
    }
    collided.insert(k);
    if (f < first_frame) {
      first_frame = f;
      first = names(p);
      first["t"] = run.t[f];
      first["frame"] = f;
      first["kind"] = kind;
      if (volume > 0) first["volume_mm3"] = volume;
      if (std::isfinite(depth) && depth > 0) first["depth_mm"] = depth;
      if (!limits.empty()) first["at_limit"] = limits;  // a joint at its stop: contact_tolerance_mm may call it a contact
    }
  }
  // Meets earlier than the first collision (or any, without one) left unconfirmed.
  size_t unconfirmed = 0;
  for (const auto& [p, fs] : meet_frames)
    for (size_t f : fs)
      if (f < first_frame && !std::binary_search(checks.begin(), checks.begin() + std::ptrdiff_t(confirmed), std::make_pair(f, p))) ++unconfirmed;

  trace("collisions");
  // The closest approach of the pairs that never collided: the smallest mesh distances confirmed exactly (a few: the exact
  // minimum can sit at a neighbouring frame within the meshes' tolerance).
  std::vector<const Task*> nearest;
  for (const auto& t : tasks)
    if (!t.above && !collided.count(t.p) && !contacted.count(t.p) && t.value < kInf) nearest.push_back(&t);
  std::stable_sort(nearest.begin(), nearest.end(), [](const Task* x, const Task* y) { return x->value < y->value; });
  json closest = nullptr;
  double closest_value = kInf;
  double tolerance = 0;
  for (const auto& [key, m] : meshes)
    if (m.set) tolerance = std::max(tolerance, 2 * m.deflection);
  for (size_t k = 0, asked = 0; k < nearest.size() && asked < 4; ++k) {
    const Task* t = nearest[k];
    if (t->value > nearest.front()->value * (1 + kRelative) + tolerance + kTouch) break;
    const Pair& p = pairs[t->p];
    const size_t f = p.constant ? 0 : t->f;
    ++asked, ++exact_checks;
    const json d = exact_at(p, f);
    const double v = d.value("value", kInf);
    if (v < closest_value) {
      closest_value = v;
      closest = names(p);
      closest["t"] = run.t[f];
      closest["frame"] = f;
      closest["distance_mm"] = v;
      closest["point_a"] = d["point_a"];
      closest["point_b"] = d["point_b"];
      if (p.constant) closest["at_every_frame"] = true;
    }
  }
  trace("closest");
  // Pairs closer than the clearance at some frame, closest first.
  json close = json::array();
  size_t close_count = 0;
  if (clearance > 0) {
    std::map<size_t, const Task*> worst;
    for (const auto& t : tasks)
      if (!t.above && t.value < clearance && !contacted.count(t.p) && (!worst.count(t.p) || t.value < worst[t.p]->value)) worst[t.p] = &t;
    std::vector<const Task*> list;
    for (const auto& [p, t] : worst) list.push_back(t);
    std::stable_sort(list.begin(), list.end(), [](const Task* x, const Task* y) { return x->value < y->value; });
    close_count = list.size();
    for (size_t k = 0; k < list.size() && k < 20; ++k) {
      json e = names(pairs[list[k]->p]);
      e["min_distance_mm"] = list[k]->value;
      e["t"] = run.t[pairs[list[k]->p].constant ? 0 : list[k]->f];
      close.push_back(e);
    }
  }
  // What the run keeps: the minimum per frame as a series (meshes: within mesh_tolerance_mm).
  std::vector<double> series(frames);
  for (size_t f = 0; f < frames; ++f) series[f] = per_frame[f] < kInf ? per_frame[f] : NAN;
  run.series.push_back({"clearance", "Minimum distance", "mm", "clearance", series});
  size_t followed = 0;
  for (const auto& p : pairs) followed += p.followed;
  out["pairs"] = followed;
  out["status"] = !first.is_null() ? "collision" : close_count ? "too_close" : contact_count ? "contact" : "clear";
  if (contact_tolerance > 0) out["contacts"] = contacts, out["contact_pairs"] = contacted.size(), out["contact_frames"] = contact_count;
  out["first_collision"] = first;
  out["min_distance_mm"] = !first.is_null() ? json(0.0) : closest.is_null() ? json(nullptr) : json(closest_value);
  if (!closest.is_null()) out["closest"] = closest;
  if (clearance > 0) out["too_close"] = close, out["too_close_pairs"] = close_count;
  if (unconfirmed) out["unconfirmed_meets"] = unconfirmed;  // frames where meshes met within mesh_tolerance_mm, not checked exactly
  if (at_start_count) out["in_contact_at_start"] = at_start, out["in_contact_at_start_pairs"] = at_start_count;
  out["mesh_tolerance_mm"] = tolerance;
  out["checked"] = {{"mesh", mesh_checks.load()}, {"exact", exact_checks}};
  if (!tick(1)) throw Error("cancelled");
  return out;
}

}  // namespace opad::sim
