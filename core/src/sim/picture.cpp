#include "opad/sim/picture.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "opad/sim/fea.hpp"
#include "opad/sim/kinematics.hpp"
#include "opad/sim/study.hpp"

namespace opad::sim {

Picture picture(const Document& doc, Scene scene, const json& args, RenderOptions& o) {
  Picture out;
  if (args.contains("joints")) {
    Mechanism mech(scene);
    Values want;
    for (const auto& [id, v] : args["joints"].items()) {
      std::vector<double> x;
      if (v.is_number()) x.push_back(v.get<double>());
      else
        for (const auto& e : v) x.push_back(e.is_null() ? NAN : e.get<double>());
      want[id] = x;
    }
    const auto r = mech.drive(want);
    if (!r.ok) throw Error("render: " + r.error);
    scene = mech.posed(scene);
  }
  if (args.contains("study")) {
    const json st = args["study"];
    const Study* s = scene.study(st.value("id", std::string()));
    if (!s) throw Error("render: no study " + st.value("id", std::string()));
    const auto run = run_study_cached(doc, scene, s->def);
    out.info["study"] = s->name;
    if (run->fea) {
      const FeaResult& r = *run->fea;
      const std::string field = st.value("field", r.kind == "modal" ? "mode" : r.kind == "thermal" ? "temperature" : "von_mises");
      // A thermal study over time: the frame asked for (t or frame), else its last.
      const std::vector<double>* temps = &r.temperature;
      if (r.kind == "thermal" && !r.temperature_frames.empty() && (st.contains("t") || st.contains("frame"))) {
        size_t f = st.contains("frame") ? size_t(std::max(0, st["frame"].get<int>())) : 0;
        if (st.contains("t"))
          for (size_t i = 0; i < run->t.size(); ++i)
            if (std::fabs(run->t[i] - st["t"].get<double>()) < std::fabs(run->t[f] - st["t"].get<double>())) f = i;
        temps = &r.temperature_frames[std::min(f, r.temperature_frames.size() - 1)];
      }
      const int mode = st.value("mode", 1) - 1;
      if (r.kind == "modal" && (mode < 0 || size_t(mode) >= r.modes.size())) throw Error("render: mode is 1 to " + std::to_string(r.modes.size()));
      if (field == "temperature" && r.temperature.empty()) throw Error("render: temperature is for a thermal study");
      if (field == "failure_index" && r.failure_index.empty()) throw Error("render: failure_index is for a static study of printed bodies (settings.print)");
      const std::vector<Vec3>* disp = r.kind == "modal" ? &r.modes[size_t(mode)] : &r.displacement;
      std::vector<double> value(r.nodes.size(), 0.0);
      for (size_t i = 0; i < r.nodes.size(); ++i) {
        const Vec3& d = (*disp)[i];
        value[i] = field == "von_mises" && !r.von_mises.empty()          ? r.von_mises[i]
                   : field == "failure_index" && !r.failure_index.empty() ? r.failure_index[i]
                   : field == "temperature" && !temps->empty()           ? (*temps)[i]
                                                                           : std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      }
      // The range over the nodes the skin shows.
      double lo = 1e300, hi = -1e300, dmax = 0;
      Vec3 a{1e300, 1e300, 1e300}, b{-1e300, -1e300, -1e300};
      for (const auto& t : r.skin)
        for (int k : t) {
          lo = std::min(lo, value[size_t(k)]), hi = std::max(hi, value[size_t(k)]);
          const Vec3& d = (*disp)[size_t(k)];
          dmax = std::max(dmax, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
          for (int c = 0; c < 3; ++c) a[size_t(c)] = std::min(a[size_t(c)], r.nodes[size_t(k)][size_t(c)]), b[size_t(c)] = std::max(b[size_t(c)], r.nodes[size_t(k)][size_t(c)]);
        }
      if (hi <= lo) hi = lo + 1e-12;
      // A stress singularity (a re-entrant corner where bonded parts meet, a point load) would paint the whole part in the
      // scale's bottom colour: the scale stops at the 99.5th percentile when the peak is far above it (or at `max`), and
      // the legend says so; the peak is reported.
      const double peak = hi;
      bool capped = false;
      if (st.contains("max")) {
        hi = std::max(lo + 1e-12, st["max"].get<double>());
        capped = peak > hi;
      } else if (field == "von_mises" || field == "failure_index") {
        std::vector<double> shown;
        for (const auto& t : r.skin)
          for (int k : t) shown.push_back(value[size_t(k)]);
        std::nth_element(shown.begin(), shown.begin() + long(0.995 * double(shown.size() - 1)), shown.end());
        const double p995 = shown[size_t(0.995 * double(shown.size() - 1))];
        if (peak > 1.5 * p995 && p995 > lo) hi = p995, capped = true;
      }
      const double size = std::sqrt(std::pow(b[0] - a[0], 2) + std::pow(b[1] - a[1], 2) + std::pow(b[2] - a[2], 2));
      const double scale = st.contains("scale") ? st["scale"].get<double>() : dmax > 0 ? 0.05 * size / dmax : 0.0;
      out.meshes = std::make_shared<std::vector<Mesh>>(1);
      Mesh& m = out.meshes->front();
      std::vector<int> index(r.nodes.size(), -1);
      std::vector<float> colors;
      for (const auto& t : r.skin)
        for (int k : t) {
          if (index[size_t(k)] < 0) {
            index[size_t(k)] = int(m.positions.size() / 3);
            for (int c = 0; c < 3; ++c) m.positions.push_back(float(r.nodes[size_t(k)][size_t(c)] + scale * (*disp)[size_t(k)][size_t(c)]));
            const auto col = result_color(std::clamp((value[size_t(k)] - lo) / (hi - lo), 0.0, 1.0));
            colors.insert(colors.end(), col.begin(), col.end());
          }
          m.indices.push_back(uint32_t(index[size_t(k)]));
        }
      RenderItem item;
      item.mesh = &m;
      item.vertex_colors = std::move(colors);
      o.extra.push_back(std::move(item));
      for (const auto& body : r.bodies) o.hide.push_back(body);
      out.legend = true;
      out.title = field == "von_mises" ? "VON MISES" : field == "failure_index" ? "FAILURE INDEX" : field == "temperature" ? "TEMPERATURE"
                  : r.kind == "modal" ? "MODE " + std::to_string(mode + 1) : "DISPLACEMENT";
      out.unit = field == "von_mises" ? "MPa" : field == "failure_index" ? "" : field == "temperature" ? "C" : "mm";
      if (r.kind == "modal") out.unit = "", out.title += " " + std::to_string(int(std::lround(r.frequencies[size_t(mode)]))) + " HZ";
      if (capped) {
        std::ostringstream pk;
        pk.precision(peak >= 10 ? 0 : 2);
        pk << std::fixed << peak;
        out.title += " (PEAK " + pk.str() + " ABOVE)";
      }
      out.lo = lo, out.hi = hi;
      out.info["field"] = field;
      out.info["range"] = {lo, hi};
      if (capped) out.info["peak"] = peak, out.info["scale_capped"] = true;
      out.info["deformation_scale"] = scale;
      if (r.kind == "modal") out.info["frequency_Hz"] = r.frequencies[size_t(mode)];
    } else {
      if (run->t.empty()) throw Error("render: the study has no frames");
      size_t f = st.contains("frame") ? size_t(std::max(0, st["frame"].get<int>())) : run->t.size() - 1;
      if (st.contains("t")) {
        const double t = st["t"].get<double>();
        f = 0;
        for (size_t i = 0; i < run->t.size(); ++i)
          if (std::fabs(run->t[i] - t) < std::fabs(run->t[f] - t)) f = i;
      }
      f = std::min(f, run->t.size() - 1);
      scene = posed_at(scene, *run, f);
      out.info["frame"] = f;
      out.info["t"] = run->t[f];
    }
  }
  out.scene = std::move(scene);
  return out;
}

}  // namespace opad::sim
