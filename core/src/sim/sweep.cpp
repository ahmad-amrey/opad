// Design sweeps (study kind sweep): another study run again over values of the design's parameters, to find the design that
// does best (sim/study.hpp). Each point sets the parameters on a copy of the document, which regenerates what uses them (a
// vent moved, a fin added), and runs the study there; nothing is written to the document itself.
//
//   settings.study      the study to run at each point (id or name): thermal, static or modal
//   settings.params     [{name, values: [...]} | {name, from, to, steps}]: numbers in the parameter's own unit (mm, deg or
//                       plain) or expressions ("12 mm", "width / 3"); several parameters make a grid of every combination
//   settings.objective  {of: a summary value (max_temperature_C, max_von_mises_MPa, min_safety_factor, ...), bodies: [ids]
//                       (the worst of theirs), goal: min | max}; by default the hottest part (thermal), the highest stress
//                       (static), the first frequency, highest (modal)
//   settings.refine     with one parameter over a range: that many more points by golden section between the best grid
//                       point's neighbours
//   settings.screening  settings merged into the study's for the sweep's points (coarser cells: quicker), and
//   settings.confirm    then the best point run again with the study's own settings (default true when screening)
//   settings.max_points the grid's limit (default 60)
//
// Results: every point (its parameter values, the objective, the study's warnings or the reason it failed), the best, the
// objective against the parameter as a series (one parameter) or against the point's number, and the best point's result
// map. Apply the best with the param command.
#include <algorithm>
#include <cmath>
#include <sstream>

#include "opad/design/feature.hpp"
#include "opad/sim/fea.hpp"
#include "opad/sim/study.hpp"

namespace opad::sim {

namespace {

std::string unit_of(const Param& p) { return p.angle ? "deg" : p.len == 1 ? "mm" : ""; }

// A parameter's value as the expression the param op takes, and as the number shown and plotted (its own unit).
struct Value {
  std::string expr;
  double number = NAN;
};

Value value_of(const json& v, const Param& p) {
  if (v.is_number()) {
    std::ostringstream s;
    s.precision(10);
    s << v.get<double>();
    const std::string u = unit_of(p);
    return {u.empty() ? s.str() : s.str() + " " + u, v.get<double>()};
  }
  if (v.is_string()) return {v.get<std::string>(), NAN};
  throw Error("sweep: a parameter's value is a number or an expression");
}

struct Point {
  std::vector<Value> values;
  double objective = NAN;
  json summary;
  std::shared_ptr<const FeaResult> fea;
  std::string error;
};

}  // namespace

StudyRun run_sweep(const Document& doc, const Scene& scene, const json& st, const Progress& progress) {
  StudyRun run;
  run.kind = "sweep";
  auto report = [&](double f, const std::string& phase) {
    if (progress && !progress(f, phase)) throw Error("cancelled");
  };
  // ---- the study to repeat
  const std::string which = st.value("study", std::string());
  const Study* inner = scene.study(which);
  if (!inner)
    for (const auto& s : scene.studies)
      if (s.name == which) inner = &s;
  if (!inner) throw Error("sweep: settings.study is the study to run at each point (its id or name)");
  if (inner->kind != "thermal" && inner->kind != "static" && inner->kind != "modal")
    throw Error("sweep: \"" + inner->name + "\" is a " + inner->kind + " study; a sweep repeats a thermal, static or modal one");

  // ---- the parameters and their values
  struct Swept {
    const Param* param;
    std::vector<Value> values;
    double from = NAN, to = NAN;  // a range, for refining
  };
  std::vector<Swept> swept;
  for (const auto& p : st.value("params", json::array())) {
    const std::string name = p.value("name", std::string());
    const Param* par = scene.param(name);
    if (!par) throw Error("sweep: no parameter \"" + name + "\" (the params command lists them)");
    Swept s{par, {}};
    if (p.contains("values")) {
      for (const auto& v : p["values"]) s.values.push_back(value_of(v, *par));
    } else if (p.contains("from") && p.contains("to")) {
      const int n = std::max(2, p.value("steps", 5));
      s.from = p["from"].get<double>(), s.to = p["to"].get<double>();
      for (int i = 0; i < n; ++i) s.values.push_back(value_of(json(s.from + (s.to - s.from) * i / (n - 1)), *par));
    } else {
      throw Error("sweep: parameter \"" + name + "\" needs values, or from, to and steps");
    }
    if (s.values.empty()) throw Error("sweep: parameter \"" + name + "\" has no values");
    swept.push_back(std::move(s));
  }
  if (swept.empty()) throw Error("sweep: settings.params names the parameters to vary: [{\"name\": ..., \"values\": [...]}]");
  size_t grid = 1;
  for (const auto& s : swept) grid *= s.values.size();
  if (grid > size_t(st.value("max_points", 60)))
    throw Error("sweep: " + std::to_string(grid) + " points is more than max_points (" + std::to_string(st.value("max_points", 60)) + ")");

  // ---- the objective
  const json obj = st.value("objective", json::object());
  const std::string of = obj.value("of", inner->kind == "thermal" ? "max_temperature_C" : inner->kind == "static" ? "max_von_mises_MPa" : "frequency_1_Hz");
  const bool maximise = obj.value("goal", of == "min_safety_factor" || of == "safety_factor" || of.rfind("frequency", 0) == 0 ? "max" : "min") == "max";
  std::vector<std::string> obj_bodies;
  for (const auto& b : obj.value("bodies", json::array())) {
    const Node* n = scene.node(b.get<std::string>());
    if (!n) throw Error("sweep: objective.bodies: no body " + b.get<std::string>());
    obj_bodies.push_back(n->name);
  }
  auto objective_of = [&](const json& summary) {
    if (of == "frequency_1_Hz") {
      const json f = summary.value("frequencies_Hz", json());
      if (f.is_array() && !f.empty()) return f[0].get<double>();
      throw Error("the study gave no frequencies");
    }
    if (obj_bodies.empty()) {
      if (summary.contains(of) && summary[of].is_number()) return summary[of].get<double>();
      throw Error("the study's summary has no " + of);
    }
    // The worst of the bodies': the highest when it is to be made low, the lowest when it is to be made high.
    double worst = maximise ? 1e300 : -1e300;
    for (const auto& name : obj_bodies) {
      const json b = summary.value("bodies", json::object()).value(name, json());
      if (!b.is_object() || !b.contains(of) || !b[of].is_number()) throw Error("the study's summary has no " + of + " for \"" + name + "\"");
      worst = maximise ? std::min(worst, b[of].get<double>()) : std::max(worst, b[of].get<double>());
    }
    return worst;
  };
  auto better = [&](double a, double b) { return std::isnan(b) || (!std::isnan(a) && (maximise ? a > b : a < b)); };

  // ---- one point: the parameters set on a copy, the study run there
  const json screening = st.value("screening", json::object());
  std::vector<Point> points;
  int evaluations = 0;
  auto evaluate = [&](const std::vector<Value>& values, bool screened, double f0, double f1) {
    Point pt;
    pt.values = values;
    ++evaluations;
    try {
      Document d = doc;
      std::vector<json> ops;
      for (size_t k = 0; k < swept.size(); ++k) ops.push_back(design::make_edit_op(swept[k].param->id, {{"expr", values[k].expr}}));
      design::apply_ops(d, std::move(ops), "sweep");
      const Scene s = resolve(d);
      for (size_t k = 0; k < swept.size(); ++k)
        if (const Param* p = s.param(swept[k].param->name); p && std::isnan(pt.values[k].number))
          pt.values[k].number = p->angle ? p->value * 180 / 3.14159265358979323846 : p->value;
      json def = inner->def;
      json settings = def.value("settings", json::object());
      if (screened && !screening.empty()) settings.merge_patch(screening);
      def["settings"] = settings;
      const Progress inner_progress = [&](double f, const std::string& phase) {
        std::ostringstream label;
        label << "Point " << evaluations << ": " << phase;
        return !progress || progress(f0 + (f1 - f0) * f, label.str());
      };
      const StudyRun r = run_study(d, s, def, inner_progress);
      pt.summary = r.summary;
      pt.fea = r.fea;
      pt.objective = objective_of(r.summary);
    } catch (const Error& e) {
      if (std::string(e.what()) == "cancelled") throw;
      pt.error = e.what();
    }
    return pt;
  };
  const bool screened = !screening.empty();
  const int refine = swept.size() == 1 && !std::isnan(swept[0].from) ? std::max(0, st.value("refine", 0)) : 0;
  const bool confirm = st.value("confirm", screened);
  const double total = double(grid + size_t(refine) + (confirm ? 1 : 0));
  // The grid: every combination, the first parameter changing slowest.
  for (size_t i = 0; i < grid; ++i) {
    std::vector<Value> values;
    size_t rest = i;
    for (size_t k = swept.size(); k-- > 0;) {
      values.insert(values.begin(), swept[k].values[rest % swept[k].values.size()]);
      rest /= swept[k].values.size();
    }
    report(double(i) / total, "Point " + std::to_string(i + 1) + " of " + std::to_string(grid));
    points.push_back(evaluate(values, screened, double(i) / total, double(i + 1) / total));
  }
  auto best_of = [&] {
    size_t b = points.size();
    for (size_t i = 0; i < points.size(); ++i)
      if (points[i].error.empty() && (b == points.size() || better(points[i].objective, points[b].objective))) b = i;
    return b;
  };
  // Refining one parameter: golden section between the best grid point's neighbours.
  if (refine > 0 && best_of() < points.size()) {
    const Param& p = *swept[0].param;
    std::vector<double> xs;
    for (const auto& v : swept[0].values) xs.push_back(v.number);
    const size_t b = best_of();
    double lo = xs[b > 0 ? b - 1 : 0], hi = xs[std::min(b + 1, xs.size() - 1)];
    auto f = [&](double x, int k) {
      points.push_back(evaluate({value_of(json(x), p)}, screened, double(grid + size_t(k)) / total, double(grid + size_t(k) + 1) / total));
      const double v = points.back().objective;
      return std::isnan(v) ? (maximise ? -1e300 : 1e300) : (maximise ? -v : v);
    };
    const double g = (std::sqrt(5.0) - 1) / 2;
    double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
    double f1 = f(x1, 0), f2 = refine > 1 ? f(x2, 1) : 1e300;
    for (int k = 2; k < refine; ++k) {
      if (f1 < f2) hi = x2, x2 = x1, f2 = f1, x1 = hi - g * (hi - lo), f1 = f(x1, k);
      else lo = x1, x1 = x2, f1 = f2, x2 = lo + g * (hi - lo), f2 = f(x2, k);
    }
  }
  const size_t best = best_of();
  if (best == points.size()) throw Error("sweep: every point failed; the first: " + points.front().error);
  json confirmed;
  std::shared_ptr<const FeaResult> best_fea = points[best].fea;
  if (confirm) {
    report(double(total - 1) / total, "The best point with the study's own settings");
    Point c = evaluate(points[best].values, false, (total - 1) / total, 1.0);
    if (c.error.empty()) {
      confirmed = {{"objective", c.objective}, {"summary", c.summary}};
      best_fea = c.fea;
    } else {
      run.warnings.push_back("the best point failed with the study's own settings: " + c.error);
    }
  }

  // ---- results
  auto params_json = [&](const Point& p) {
    json o = json::object();
    for (size_t k = 0; k < swept.size(); ++k)
      o[swept[k].param->name] = std::isnan(p.values[k].number) ? json(p.values[k].expr) : json(p.values[k].number);
    return o;
  };
  json list = json::array();
  for (const auto& p : points) {
    json e = {{"params", params_json(p)}};
    if (p.error.empty()) {
      e["objective"] = p.objective;
      if (p.summary.contains("warnings") && !p.summary["warnings"].empty()) e["warnings"] = p.summary["warnings"];
      if (p.summary.contains("max_temperature_C")) e["max_temperature_C"] = p.summary["max_temperature_C"];
    } else {
      e["error"] = p.error;
    }
    list.push_back(e);
  }
  json params_info = json::array();
  for (const auto& s : swept) params_info.push_back({{"name", s.param->name}, {"unit", unit_of(*s.param)}, {"now", s.param->shown}});
  run.summary = {{"study", inner->name},
                 {"study_kind", inner->kind},
                 {"objective", {{"of", of}, {"goal", maximise ? "max" : "min"}, {"bodies", obj_bodies}}},
                 {"params", params_info},
                 {"points", list},
                 {"evaluations", evaluations},
                 {"best", {{"params", params_json(points[best])}, {"objective", points[best].objective}}}};
  if (!confirmed.is_null()) run.summary["best"]["confirmed"] = confirmed["objective"], run.summary["best"]["summary"] = confirmed["summary"];
  else run.summary["best"]["summary"] = points[best].summary;
  size_t failed = 0;
  for (const auto& p : points) failed += p.error.empty() ? 0 : 1;
  if (failed) run.warnings.push_back(std::to_string(failed) + " of " + std::to_string(points.size()) + " points failed (see points)");
  if (!run.warnings.empty()) run.summary["warnings"] = run.warnings;
  // The objective against the parameter (one), or the point's number.
  std::vector<size_t> order;
  for (size_t i = 0; i < points.size(); ++i)
    if (points[i].error.empty()) order.push_back(i);
  const bool along = swept.size() == 1 && std::all_of(order.begin(), order.end(), [&](size_t i) { return !std::isnan(points[i].values[0].number); });
  if (along) std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return points[a].values[0].number < points[b].values[0].number; });
  Series s{"objective", of, of.find("_C") != std::string::npos ? "degC" : of.find("MPa") != std::string::npos ? "MPa" : "", "value", {}};
  for (size_t i : order) {
    run.t.push_back(along ? points[i].values[0].number : double(i + 1));
    s.v.push_back(points[i].objective);
  }
  run.series.push_back(s);
  run.summary["axis"] = along ? swept[0].param->name + (unit_of(*swept[0].param).empty() ? "" : " (" + unit_of(*swept[0].param) + ")") : "point";
  run.fea = best_fea;
  return run;
}

}  // namespace opad::sim
