// The study and load commands (sim/study.hpp, sim/fea.hpp).
#include <cmath>
#include <functional>
#include <set>

#include "opad/commands.hpp"
#include "opad/scene.hpp"
#include "opad/sim/joints.hpp"
#include "opad/sim/kinematics.hpp"
#include "opad/sim/study.hpp"

namespace opad::commands {

namespace {

std::string free_name(const std::vector<std::string>& taken, const std::string& stem) {
  for (int n = 1;; ++n)
    if (std::find(taken.begin(), taken.end(), stem + std::to_string(n)) == taken.end()) return stem + std::to_string(n);
}

std::string capital(std::string s) {
  if (!s.empty()) s[0] = char(std::toupper(s[0]));
  for (auto& c : s)
    if (c == '_') c = ' ';
  return s;
}

// The study's result as the op keeps it: the summary, numbers rounded so a file diff stays readable.
json rounded(const json& j) {
  if (j.is_number_float()) {
    const double v = j.get<double>();
    if (!std::isfinite(v)) return nullptr;
    const double mag = std::fabs(v);
    if (mag == 0) return 0.0;
    const double scale = std::pow(10.0, 6 - int(std::ceil(std::log10(mag))));
    return std::round(v * scale) / scale;
  }
  if (j.is_object()) {
    json o = json::object();
    for (const auto& [k, v] : j.items()) o[k] = rounded(v);
    return o;
  }
  if (j.is_array()) {
    json a = json::array();
    for (const auto& v : j) a.push_back(rounded(v));
    return a;
  }
  return j;
}

}  // namespace

void register_study_commands(const std::function<void(const CommandInfo&, Handler)>& add) {
  auto reg = [&](const char* name, const char* desc, json args, bool mutates, Handler h) { add({name, desc, std::move(args), mutates}, std::move(h)); };

  reg("study",
      "Define and run a study: motion (joints driven over time), dynamic (Chrono: gravity, contacts, motors, springs), static "
      "or modal (Netgen + CalculiX on a load case). Returns its summary (and sampled series); id: run or change one",
      {{"doc", "path"},
       {"kind", "motion|dynamic|static|modal"},
       {"name", "string"},
       {"settings", "object - see the agent guide (Motion and simulation)"},
       {"id", "string - an existing study: run it again (with settings: changed first)"},
       {"run", "bool - run it now (default true)"},
       {"series", "bool - also return series (or a list of names / groups)"},
       {"samples", "int - points per series (default 21)"},
       {"pose_at", "number - s: also write a pose of the parts at that time (motion, dynamic)"},
       {"by", "string"}},
      true, [](Document* d, const json& a) {
        if (!d) throw Error("this command needs a document: pass \"doc\"");
        Document& doc = *d;
        const std::string by = a.value("by", "");
        Scene s = resolve(doc);
        std::string id;
        json def;
        if (a.contains("id")) {
          const Study* st = s.study(a["id"].get<std::string>());
          if (!st) throw Error("no study " + a["id"].get<std::string>());
          id = st->id;
          def = st->def;
          json set = json::object();
          for (const char* k : {"name", "settings"})
            if (a.contains(k)) set[k] = a[k];
          if (!set.empty()) {
            for (const auto& [k, v] : set.items()) def[k] = v;
            Document::validate_op(def);
            doc.append({{"op", "edit"}, {"target", id}, {"set", set}}, by);
            s = resolve(doc);
          }
        } else {
          const std::string kind = a.at("kind").get<std::string>();
          const auto& kinds = sim::study_kinds();
          if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) throw Error("study: kind is motion, dynamic, static or modal");
          std::vector<std::string> taken;
          for (const auto& st : s.studies) taken.push_back(st.name);
          def = {{"op", "study"}, {"name", a.value("name", free_name(taken, capital(kind) + " study "))}, {"kind", kind},
                 {"settings", a.value("settings", json::object())}};
          Document::validate_op(def);
        }
        json out = {{"name", def["name"]}, {"kind", def["kind"]}};
        if (!a.value("run", true)) {
          if (id.empty()) id = doc.append(def, by).id;
          out["id"] = id;
          return out;
        }
        const auto run = sim::run_study_cached(doc, s, def);
        const json summary = rounded(run->summary);
        if (id.empty()) {
          def["result"] = summary;
          id = doc.append(def, by).id;
        } else if (s.study(id)->result != summary) {
          doc.append({{"op", "edit"}, {"target", id}, {"set", {{"result", summary}}}}, by);
        }
        out["id"] = id;
        json report = sim::study_report(*run, a);
        for (const auto& [k, v] : report.items()) out[k] = v;
        if (a.contains("pose_at") && !run->t.empty()) {
          const double t = a["pose_at"].get<double>();
          size_t best = 0;
          for (size_t i = 0; i < run->t.size(); ++i)
            if (std::fabs(run->t[i] - t) < std::fabs(run->t[best] - t)) best = i;
          const Scene now = resolve(doc);
          const Scene posed = sim::posed_at(now, *run, best);
          json placements = json::array();
          for (const auto& p : run->parts) {
            const Node* before = now.node(p);
            const Node* after = posed.node(p);
            if (before && after && before->local.m != after->local.m) placements.push_back({{"target", p}, {"matrix", after->local.to_json()}});
          }
          if (!placements.empty()) {
            json pose = {{"op", "pose"}, {"name", def["name"].get<std::string>() + " at " + std::to_string(run->t[best]) + " s"}, {"placements", placements}};
            // Joint values at that frame, from the run's value series.
            json values = json::object();
            for (const auto& j : now.joints) {
              const auto* k = sim::joint_kind(j.kind);
              if (!k || k->relation) continue;
              json v = json::array();
              for (const auto& c : k->coords)
                for (const auto& ser : run->series)
                  if (ser.id == j.id && ser.group == "value" && ser.name == j.name + " " + c.name && best < ser.v.size()) v.push_back(ser.v[best]);
              if (v.size() == k->coords.size()) values[j.id] = v;
            }
            if (!values.empty()) pose["values"] = values;
            out["pose"] = doc.append(pose, by).id;
          }
        }
        return out;
      });

  reg("load",
      "Add a structural load or support to a load case (static and modal studies): fixed, force, pressure, moment, "
      "bolt_preload, gravity, displacement. With id: change one",
      {{"doc", "path"},
       {"kind", "fixed|force|pressure|moment|bolt_preload|gravity|displacement"},
       {"name", "string"},
       {"case", "string - load case (default \"Load case 1\")"},
       {"refs", "array - faces (fixed, force, pressure, moment, displacement) or the bolt body (bolt_preload)"},
       {"vector", "[x,y,z] - force N, moment N.mm, gravity mm/s2, displacement mm"},
       {"value", "number - pressure MPa (positive pushes into the face), preload N"},
       {"axis", "object - bolt_preload: the bolt's axis (default: its longest cylinder)"},
       {"id", "string - an existing load to change"},
       {"by", "string"}},
      true, [](Document* d, const json& a) {
        if (!d) throw Error("this command needs a document: pass \"doc\"");
        Document& doc = *d;
        const std::string by = a.value("by", "");
        const Scene s = resolve(doc);
        if (a.contains("id")) {
          const Load* l = s.load(a["id"].get<std::string>());
          if (!l) throw Error("no load " + a["id"].get<std::string>());
          json set = json::object();
          for (const char* k : {"name", "case", "refs", "vector", "value", "axis"})
            if (a.contains(k)) set[k] = a[k];
          if (set.empty()) throw Error("load: nothing to change");
          json effective = l->def;
          for (const auto& [k, v] : set.items()) effective[k] = v;
          Document::validate_op(effective);
          doc.append({{"op", "edit"}, {"target", l->id}, {"set", set}}, by);
          return json{{"id", l->id}, {"changed", set}};
        }
        const std::string kind = a.at("kind").get<std::string>();
        const auto& kinds = sim::load_kinds();
        if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
          throw Error("load: kind is fixed, force, pressure, moment, bolt_preload, gravity or displacement");
        json refs = a.value("refs", json::array());
        if (!refs.is_array()) refs = json::array({refs});
        for (const auto& r : refs) Ref::from_json(r.is_object() && r.contains("select") ? json(r["body"]) : r);
        if (kind != "gravity" && refs.empty()) throw Error("load: a " + kind + " acts on faces (or a bolt body): give refs");
        if ((kind == "force" || kind == "moment" || kind == "gravity" || kind == "displacement") && !a.contains("vector"))
          throw Error("load: a " + kind + " needs its vector [x, y, z]");
        if ((kind == "pressure" || kind == "bolt_preload") && !a.contains("value"))
          throw Error(std::string("load: a ") + kind + " needs its value (" + (kind == "pressure" ? "MPa" : "N") + ")");
        std::vector<std::string> taken;
        for (const auto& l : s.loads) taken.push_back(l.name);
        json op = {{"op", "load"}, {"name", a.value("name", free_name(taken, capital(kind) + " "))}, {"kind", kind},
                   {"case", a.value("case", std::string("Load case 1"))}, {"refs", refs}};
        for (const char* k : {"vector", "value", "axis"})
          if (a.contains(k)) op[k] = a[k];
        const std::string id = doc.append(op, by).id;
        return json{{"id", id}, {"name", op["name"]}, {"kind", kind}, {"case", op["case"]}};
      });

  reg("sim_engines", "Which simulation engines this build and machine have: Chrono (dynamic), Netgen and CalculiX (static, modal)", json::object(), false,
      [](Document*, const json&) { return sim::engines(); });
}

}  // namespace opad::commands
