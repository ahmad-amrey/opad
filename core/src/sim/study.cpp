#include "opad/sim/study.hpp"
#include "opad/sim/cfd.hpp"

#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>
#include <numeric>

#include "opad/design/expr.hpp"
#include "opad/sim/joints.hpp"
#include "opad/sim/kinematics.hpp"

namespace opad::sim {

namespace {

constexpr double kPi = 3.14159265358979323846;

design::ParamTable params_of(const Scene& s) {
  std::vector<design::ParamDef> defs;
  for (const auto& p : s.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return design::ParamTable(defs);
}

// A driven coordinate over time.
struct Driver {
  std::string joint;
  size_t coord = 0;
  bool angle = true;
  std::function<double(double)> value;  // t (s) -> deg / mm
};

const Coord& coord_of(const Joint& j, size_t c) { return joint_kind(j.kind)->coords.at(c); }

size_t coord_named(const Joint& j, const json& d) {
  const JointKind* k = joint_kind(j.kind);
  if (!k || k->coords.empty()) throw Error("\"" + j.name + "\" (" + j.kind + ") has no coordinate to drive");
  if (!d.contains("coordinate")) return 0;
  const int c = coord_index(*k, d["coordinate"].get<std::string>());
  if (c < 0) throw Error("\"" + j.name + "\" has no coordinate " + d["coordinate"].dump());
  return size_t(c);
}

// A driver from {"joint", "coordinate"?, and one of "to" (+ "from"), "speed", "expr", "table"}; `duration` in s.
Driver make_driver(const Scene& s, const design::ParamTable& params, const json& d, double duration) {
  if (!d.is_object() || !d.contains("joint")) throw Error("a driver is {\"joint\": id, \"to\" | \"speed\" | \"expr\" | \"table\": ...}");
  const Joint* j = s.joint(d["joint"].get<std::string>());
  if (!j) throw Error("a driver names no joint: " + d["joint"].dump());
  if (is_relation(j->kind)) throw Error("\"" + j->name + "\" is a relation: drive one of the joints it couples");
  Driver out;
  out.joint = j->id;
  out.coord = coord_named(*j, d);
  out.angle = coord_of(*j, out.coord).angle;
  const double start = j->values.at(out.coord);
  const design::Dim dim = out.angle ? design::Dim::Angle : design::Dim::Length;
  const double unit = out.angle ? 180 / kPi : 1.0;  // the expression's radians back to degrees
  if (d.contains("expr")) {
    const std::string e = d["expr"].get<std::string>();
    params.as_with(dim, e, "t", design::Quantity{0});  // fails now, not at frame 40
    out.value = [params, e, dim, unit](double t) { return params.as_with(dim, e, "t", design::Quantity{t}) * unit; };
  } else if (d.contains("table")) {
    std::vector<std::pair<double, double>> pts;
    for (const auto& p : d["table"]) pts.push_back({p.at(0).get<double>(), p.at(1).get<double>()});
    if (pts.empty()) throw Error("a driver's table is [[t, value], ...]");
    std::sort(pts.begin(), pts.end());
    out.value = [pts](double t) {
      if (t <= pts.front().first) return pts.front().second;
      if (t >= pts.back().first) return pts.back().second;
      for (size_t i = 1; i < pts.size(); ++i)
        if (t <= pts[i].first) {
          const double f = (t - pts[i - 1].first) / std::max(1e-12, pts[i].first - pts[i - 1].first);
          return pts[i - 1].second + f * (pts[i].second - pts[i - 1].second);
        }
      return pts.back().second;
    };
  } else if (d.contains("speed")) {
    const double v = d["speed"].get<double>();
    out.value = [start, v](double t) { return start + v * t; };
  } else if (d.contains("to")) {
    const double from = d.value("from", start), to = d["to"].get<double>();
    const double T = std::max(duration, 1e-9);
    const std::string profile = d.value("profile", "linear");
    out.value = [from, to, T, profile](double t) {
      double f = std::clamp(t / T, 0.0, 1.0);
      if (profile == "smooth") f = f * f * (3 - 2 * f);  // starts and stops gently (cubic)
      else if (profile == "cycloidal") f = f - std::sin(2 * kPi * f) / (2 * kPi);
      return from + (to - from) * f;
    };
  } else {
    throw Error("a driver needs to (with from), speed, expr or table");
  }
  return out;
}

// Drivers from the joints' own drives (position or speed) when the study names none.
std::vector<json> joint_drivers(const Scene& s) {
  std::vector<json> out;
  for (const auto& j : s.joints) {
    const json dr = j.def.value("drive", json());
    if (!dr.is_object()) continue;
    const std::string mode = dr.value("mode", "position");
    json d = {{"joint", j.id}};
    if (dr.contains("coordinate")) d["coordinate"] = dr["coordinate"];
    if (mode == "speed" && dr.contains("value")) d["speed"] = dr["value"];
    else if (mode == "speed" && dr.contains("expr")) d["expr"] = dr["expr"];  // speeds as an expression are taken as values here
    else if (mode == "position" && dr.contains("expr")) d["expr"] = dr["expr"];
    else if (mode == "position" && dr.contains("value")) d["to"] = dr["value"];
    else continue;
    out.push_back(d);
  }
  return out;
}

std::vector<double> diff(const std::vector<double>& v, const std::vector<double>& t) {
  std::vector<double> out(v.size(), 0.0);
  const size_t n = v.size();
  if (n < 2) return out;
  for (size_t i = 0; i < n; ++i) {
    const size_t a = i == 0 ? 0 : i - 1, b = i + 1 == n ? n - 1 : i + 1;
    out[i] = (v[b] - v[a]) / std::max(1e-15, t[b] - t[a]);
  }
  return out;
}

Vec3 apply_point(const Mat4& m, const Vec3& p) { return m.apply(p); }

}  // namespace

Scene posed_at(const Scene& scene, const StudyRun& run, size_t frame) {
  Scene out = scene;
  if (frame >= run.poses.size()) return out;
  std::vector<size_t> order(run.parts.size());
  std::iota(order.begin(), order.end(), 0);
  std::vector<size_t> depth(run.parts.size());
  for (size_t i = 0; i < run.parts.size(); ++i) depth[i] = scene.path_to(run.parts[i]).size();
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return depth[a] < depth[b]; });
  for (size_t i : order) {
    auto it = out.nodes.find(run.parts[i]);
    if (it == out.nodes.end()) continue;
    const Mat4 parent = it->second.parent.empty() ? Mat4() : out.world(it->second.parent);
    it->second.local = parent.inverse() * run.poses[frame][i];
  }
  return out;
}

StudyRun run_motion(const Document& doc, const Scene& scene, const json& st, const Progress& progress) {
  StudyRun run;
  run.kind = "motion";
  const double duration = st.value("duration", 1.0);
  if (!(duration > 0)) throw Error("a motion study's duration is a positive number of seconds");
  int frames = st.value("frames", 0);
  if (frames <= 0) frames = st.contains("step") ? int(std::round(duration / st["step"].get<double>())) + 1 : 101;
  frames = std::clamp(frames, 2, 20001);
  const design::ParamTable params = params_of(scene);
  std::vector<json> specs;
  for (const auto& d : st.value("drivers", json::array())) specs.push_back(d);
  if (specs.empty()) specs = joint_drivers(scene);
  if (specs.empty()) throw Error("a motion study needs drivers: [{\"joint\": id, \"to\": 360}] (or joints with a position or speed drive)");
  std::vector<Driver> drivers;
  for (const auto& d : specs) drivers.push_back(make_driver(scene, params, d, duration));
  Mechanism mech(scene);
  for (const auto& p : mech.problems()) run.warnings.push_back(p);
  run.parts = mech.parts();
  // Traced points: where a point of a part goes. {"part", "point": [x, y, z] in the world at the start, "name"}.
  struct Trace {
    std::string name;
    size_t part;
    Vec3 local;
    std::vector<double> x, y, z;
  };
  std::vector<Trace> traces;
  for (const auto& tr : st.value("traces", json::array())) {
    const std::string part = tr.at("part").get<std::string>();
    const auto it = std::find(run.parts.begin(), run.parts.end(), part);
    if (it == run.parts.end()) throw Error("a trace's part " + part + " is not a part of the mechanism");
    const Vec3 p = tr.at("point").get<Vec3>();
    const Mat4 w = mech.part_world(part);
    const Node* n = scene.node(part);
    traces.push_back({tr.value("name", (n ? n->name : part) + " point"), size_t(it - run.parts.begin()), w.inverse().apply(p), {}, {}, {}});
  }
  std::map<std::string, std::vector<std::vector<double>>> values;  // joint -> coordinate -> frame
  bool stopped = false;
  for (int f = 0; f < frames; ++f) {
    const double t = duration * f / (frames - 1);
    Values want;
    for (const auto& d : drivers) {
      auto& v = want[d.joint];
      const Joint* j = scene.joint(d.joint);
      if (v.empty()) v.assign(j->values.size(), NAN);
      v[d.coord] = d.value(t);
    }
    const Mechanism::Result r = mech.drive(want);
    if (!r.ok) {
      run.warnings.push_back("at t = " + std::to_string(t) + " s: " + r.error);
      run.summary["stopped_at"] = t;
      stopped = true;
      break;
    }
    for (const auto& n : r.notes)
      if (std::find(run.warnings.begin(), run.warnings.end(), n) == run.warnings.end()) run.warnings.push_back(n);
    run.t.push_back(t);
    std::vector<Mat4> pose;
    for (const auto& p : run.parts) pose.push_back(mech.part_world(p));
    for (auto& tr : traces) {
      const Vec3 w = apply_point(pose[tr.part], tr.local);
      tr.x.push_back(w[0]), tr.y.push_back(w[1]), tr.z.push_back(w[2]);
    }
    run.poses.push_back(std::move(pose));
    for (const auto& [id, v] : mech.values()) {
      auto& slot = values[id];
      slot.resize(v.size());
      for (size_t c = 0; c < v.size(); ++c) slot[c].push_back(v[c]);
    }
    if (progress && !progress(double(f + 1) / frames, "Moving")) throw Error("cancelled");
  }
  json joints = json::object();
  for (const auto& j : scene.joints) {
    const auto it = values.find(j.id);
    if (it == values.end()) continue;
    const JointKind* k = joint_kind(j.kind);
    json e = json::object();
    for (size_t c = 0; c < it->second.size(); ++c) {
      const Coord& co = k->coords[c];
      const std::string unit = co.angle ? "deg" : "mm";
      const std::string label = j.name + " " + co.name;
      const auto& v = it->second[c];
      const auto sp = diff(v, run.t), ac = diff(sp, run.t);
      run.series.push_back({j.id, label, unit, "value", v});
      run.series.push_back({j.id, label + " speed", unit + "/s", "speed", sp});
      run.series.push_back({j.id, label + " acceleration", unit + "/s2", "acceleration", ac});
      const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
      double vmax = 0, amax = 0;
      for (double x : sp) vmax = std::max(vmax, std::fabs(x));
      for (double x : ac) amax = std::max(amax, std::fabs(x));
      e[co.name] = {{"min", *lo}, {"max", *hi}, {"max_speed", vmax}, {"max_acceleration", amax}, {"unit", unit}};
    }
    joints[j.name] = e;
  }
  json tsum = json::object();
  for (const auto& tr : traces) {
    std::vector<double> speed(tr.x.size(), 0.0);
    const auto vx = diff(tr.x, run.t), vy = diff(tr.y, run.t), vz = diff(tr.z, run.t);
    double length = 0, vmax = 0;
    for (size_t i = 0; i < tr.x.size(); ++i) {
      speed[i] = std::sqrt(vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
      vmax = std::max(vmax, speed[i]);
      if (i) length += std::sqrt(std::pow(tr.x[i] - tr.x[i - 1], 2) + std::pow(tr.y[i] - tr.y[i - 1], 2) + std::pow(tr.z[i] - tr.z[i - 1], 2));
    }
    run.series.push_back({run.parts[tr.part], tr.name + " x", "mm", "trace", tr.x});
    run.series.push_back({run.parts[tr.part], tr.name + " y", "mm", "trace", tr.y});
    run.series.push_back({run.parts[tr.part], tr.name + " z", "mm", "trace", tr.z});
    run.series.push_back({run.parts[tr.part], tr.name + " speed", "mm/s", "speed", speed});
    auto range = [](const std::vector<double>& v) {
      const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
      return json::array({*lo, *hi});
    };
    tsum[tr.name] = {{"path_length", length}, {"max_speed", vmax}, {"x", range(tr.x)}, {"y", range(tr.y)}, {"z", range(tr.z)}};
  }
  run.summary["frames"] = run.t.size();
  run.summary["duration"] = duration;
  run.summary["completed"] = !stopped;
  run.summary["joints"] = joints;
  if (!tsum.empty()) run.summary["traces"] = tsum;
  if (!run.warnings.empty()) run.summary["warnings"] = run.warnings;
  return run;
}

StudyRun run_study(const Document& doc, const Scene& scene, const json& study, const Progress& progress) {
  const std::string kind = study.value("kind", "");
  const json settings = study.value("settings", json::object());
  if (kind == "motion") return run_motion(doc, scene, settings, progress);
  if (kind == "dynamic") return run_dynamic(doc, scene, settings, progress);
  if (kind == "sweep") return run_sweep(doc, scene, settings, progress);
  if (kind == "thermal" && settings.value("air", std::string()) == "cfd") return run_cfd(doc, scene, settings, progress);
  if (kind == "static" || kind == "modal" || kind == "thermal") return run_structural(doc, scene, kind, settings, progress);
  throw Error("unknown study kind \"" + kind + "\" (motion, dynamic, static, modal, thermal, sweep)");
}

std::shared_ptr<const StudyRun> run_study_cached(const Document& doc, const Scene& scene, const json& study, const Progress& progress) {
  static std::mutex mu;
  static std::list<std::pair<std::string, std::shared_ptr<const StudyRun>>> cache;
  json key_json = {{"kind", study.value("kind", "")}, {"settings", study.value("settings", json::object())}};
  const std::string key = doc.header.uuid + "|" + scene.state + "|" + sha256_hex(key_json.dump());
  {
    std::lock_guard<std::mutex> lock(mu);
    for (auto it = cache.begin(); it != cache.end(); ++it)
      if (it->first == key) {
        cache.splice(cache.begin(), cache, it);
        return cache.front().second;
      }
  }
  auto run = std::make_shared<const StudyRun>(run_study(doc, scene, study, progress));
  if (!scene.state.empty()) {
    std::lock_guard<std::mutex> lock(mu);
    cache.push_front({key, run});
    while (cache.size() > 6) cache.pop_back();
  }
  return run;
}

json study_report(const StudyRun& run, const json& options) {
  json out = run.summary;
  const json want = options.value("series", json());
  if (want.is_null() || want == false) return out;
  const int samples = std::clamp(options.value("samples", 21), 2, 2001);
  std::vector<size_t> at;
  const size_t n = run.t.size();
  for (int i = 0; i < samples && n; ++i) {
    const size_t k = size_t(std::round(double(i) * double(n - 1) / double(samples - 1)));
    if (at.empty() || at.back() != k) at.push_back(k);
  }
  auto picked = [&](const Series& s) {
    if (want == true)
      return s.group == "value" || s.group == "trace" || s.group == "motor" || s.group == "energy" || s.group == "reaction" || s.group == "temperature";
    for (const auto& w : want)
      if (w.is_string() && (w.get<std::string>() == s.name || w.get<std::string>() == s.group || w.get<std::string>() == s.id)) return true;
    return false;
  };
  json t = json::array();
  for (size_t k : at) t.push_back(std::round(run.t[k] * 1e6) / 1e6);
  json series = json::array();
  for (const auto& s : run.series) {
    if (!picked(s)) continue;
    json v = json::array();
    for (size_t k : at) v.push_back(k < s.v.size() ? std::round(s.v[k] * 1e6) / 1e6 : 0.0);
    series.push_back({{"name", s.name}, {"unit", s.unit}, {"group", s.group}, {"v", v}});
  }
  out["t"] = t;
  out["series"] = series;
  return out;
}

}  // namespace opad::sim
