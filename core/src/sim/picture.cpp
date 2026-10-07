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
      std::string field = st.value("field", r.kind == "modal" ? "mode" : r.kind == "thermal" ? "temperature" : "von_mises");
      // The CFD air's streamlines (sim/cfd.hpp): coloured on the temperature scale with the parts, or by the air's speed
      // (field air_speed: the parts plain); streamlines false hides them.
      const bool air_speed = field == "air_speed";
      if (air_speed && r.streamlines.empty()) throw Error("render: air_speed is for a thermal study with the CFD air (settings.air cfd)");
      if (air_speed) field = "temperature";
      const bool lines = !r.streamlines.empty() && st.value("streamlines", true) && (field == "temperature");
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
      if (field == "temperature" && lines && !air_speed)
        for (const auto& l : r.streamline_temperature)
          for (double v : l) lo = std::min(lo, v), hi = std::max(hi, v);
      if (air_speed) {
        lo = 0, hi = -1e300;
        for (const auto& l : r.streamline_speed)
          for (double v : l) hi = std::max(hi, v);
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
      // hide: bodies left out of the map; ghost: drawn see-through (an enclosure over what it holds).
      auto listed = [&](const char* key, size_t body) {
        for (const auto& id : st.value(key, json::array()))
          if (id.is_string() && body < r.bodies.size() && r.bodies[body] == id.get<std::string>()) return true;
        return false;
      };
      out.meshes = std::make_shared<std::vector<Mesh>>(2);
      Mesh& m = out.meshes->front();
      Mesh& gm = out.meshes->back();
      std::vector<int> index(r.nodes.size(), -1), gindex(r.nodes.size(), -1);
      std::vector<float> colors, gcolors;
      for (size_t ti = 0; ti < r.skin.size(); ++ti) {
        const size_t body = ti < r.skin_body.size() ? size_t(r.skin_body[ti]) : 0;
        if (listed("hide", body)) continue;
        if (listed("ghost", body)) {
          for (int k : r.skin[ti]) {
            if (gindex[size_t(k)] < 0) {
              gindex[size_t(k)] = int(gm.positions.size() / 3);
              for (int c = 0; c < 3; ++c) gm.positions.push_back(float(r.nodes[size_t(k)][size_t(c)]));
              const auto col = result_color(std::clamp((value[size_t(k)] - lo) / (hi - lo), 0.0, 1.0));
              gcolors.insert(gcolors.end(), col.begin(), col.end());
            }
            gm.indices.push_back(uint32_t(gindex[size_t(k)]));
          }
          continue;
        }
        for (int k : r.skin[ti]) {
          if (index[size_t(k)] < 0) {
            index[size_t(k)] = int(m.positions.size() / 3);
            for (int c = 0; c < 3; ++c) m.positions.push_back(float(r.nodes[size_t(k)][size_t(c)] + scale * (*disp)[size_t(k)][size_t(c)]));
            const auto col = result_color(std::clamp((value[size_t(k)] - lo) / (hi - lo), 0.0, 1.0));
            if (air_speed) colors.insert(colors.end(), {0.72f, 0.72f, 0.75f});
            else colors.insert(colors.end(), col.begin(), col.end());
          }
          m.indices.push_back(uint32_t(index[size_t(k)]));
        }
      }
      if (!gm.indices.empty()) {
        RenderItem ghost;
        ghost.mesh = &gm;
        ghost.vertex_colors = std::move(gcolors);
        ghost.opacity = 0.25f;
        o.extra.push_back(std::move(ghost));
      }
      RenderItem item;
      item.mesh = &m;
      item.vertex_colors = std::move(colors);
      if (lines)
        for (size_t l = 0; l < r.streamlines.size(); ++l) {
          std::vector<std::array<float, 3>> poly, col;
          for (size_t k = 0; k < r.streamlines[l].size(); ++k) {
            const auto& q = r.streamlines[l][k];
            poly.push_back({float(q[0]), float(q[1]), float(q[2])});
            const double v = air_speed ? r.streamline_speed[l][k] : r.streamline_temperature[l][k];
            const auto c = result_color(std::clamp((v - lo) / (hi - lo), 0.0, 1.0));
            col.push_back({c[0], c[1], c[2]});
          }
          item.lines.push_back(std::move(poly));
          item.line_colors.push_back(std::move(col));
        }
      o.extra.push_back(std::move(item));
      for (const auto& body : r.bodies) o.hide.push_back(body);
      out.legend = true;
      out.title = air_speed ? "AIR SPEED" : field == "von_mises" ? "VON MISES" : field == "failure_index" ? "FAILURE INDEX" : field == "temperature" ? "TEMPERATURE"
                  : r.kind == "modal" ? "MODE " + std::to_string(mode + 1) : "DISPLACEMENT";
      out.unit = air_speed ? "m/s" : field == "von_mises" ? "MPa" : field == "failure_index" ? "" : field == "temperature" ? "C" : "mm";
      if (r.kind == "modal") out.unit = "", out.title += " " + std::to_string(int(std::lround(r.frequencies[size_t(mode)]))) + " HZ";
      if (capped) {
        std::ostringstream pk;
        pk.precision(peak >= 10 ? 0 : 2);
        pk << std::fixed << peak;
        out.title += " (PEAK " + pk.str() + " ABOVE)";
      }
      out.lo = lo, out.hi = hi;
      out.info["field"] = air_speed ? "air_speed" : field;
      if (lines) out.info["streamlines"] = r.streamlines.size();
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
