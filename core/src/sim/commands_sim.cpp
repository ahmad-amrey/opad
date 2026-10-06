// Commands for joints, poses, loads and studies (sim/*.hpp): what the CLI, Python, MCP and the app run.
#include <cmath>
#include <functional>
#include <set>

#include "opad/commands.hpp"
#include "opad/design/feature.hpp"
#include "opad/scene.hpp"
#include "opad/sim/frames.hpp"
#include "opad/sim/joints.hpp"
#include "opad/sim/kinematics.hpp"
#include "opad/sim/study.hpp"

namespace opad::commands {

void register_study_commands(const std::function<void(const CommandInfo&, Handler)>& add);  // sim/commands_study.cpp

namespace {

constexpr double kPi = 3.14159265358979323846;

Document& need_doc(Document* d) {
  if (!d) throw Error("this command needs a document: pass \"doc\"");
  return *d;
}

std::string node_name(const Scene& s, const std::string& id) {
  if (id.empty()) return "the world";
  const Node* n = s.node(id);
  return n ? n->name : id;
}

// A node id from a node id or a reference to something on it ("<body>/face/2").
std::string node_of(const Scene& s, const json& v, const char* what) {
  if (v.is_null()) return {};
  std::string id;
  if (v.is_string()) {
    id = v.get<std::string>();
    if (!s.node(id)) id = sim::frame_node(v);
  } else {
    id = sim::frame_node(v);
  }
  if (id.empty() || !s.node(id)) throw Error(std::string(what) + ": no body or component " + (v.is_string() ? v.get<std::string>() : v.dump()));
  return id;
}

void refuse_moving_locked(const Scene& s, const std::vector<std::string>& nodes) {
  for (const auto& id : nodes)
    if (const Node* holder = s.lock_holder(id)) throw LockedError(node_name(s, id), holder->name, "moving");
}

// Rotation about z by a and translation along z by d of a frame (in its own axes).
Frame frame_moved(const Frame& f, double angle_rad, double along_x, double along_y, double along_z) {
  const Vec3 z = f.normal();
  Frame o = f;
  const double c = std::cos(angle_rad), s = std::sin(angle_rad);
  for (int i = 0; i < 3; ++i) {
    o.origin[size_t(i)] += f.x[size_t(i)] * along_x + f.y[size_t(i)] * along_y + z[size_t(i)] * along_z;
    o.x[size_t(i)] = c * f.x[size_t(i)] + s * f.y[size_t(i)];
    o.y[size_t(i)] = -s * f.x[size_t(i)] + c * f.y[size_t(i)];
  }
  return o;
}

Mat4 mat_of_frame(const Frame& f) {
  const Vec3 z = f.normal();
  Mat4 m;
  for (int i = 0; i < 3; ++i) {
    m.at(i, 0) = f.x[size_t(i)];
    m.at(i, 1) = f.y[size_t(i)];
    m.at(i, 2) = z[size_t(i)];
    m.at(i, 3) = f.origin[size_t(i)];
  }
  return m;
}

// The joint's frame where its coordinates are `values` (deg, mm), relative to the frame at zero.
Frame at_values(const std::string& kind, const Frame& zero, const std::vector<double>& v, double pitch) {
  auto at = [&](size_t i) { return i < v.size() ? v[i] : 0.0; };
  const double deg = kPi / 180;
  if (kind == "revolute") return frame_moved(zero, at(0) * deg, 0, 0, 0);
  if (kind == "slider") return frame_moved(zero, 0, 0, 0, at(0));
  if (kind == "cylindrical") return frame_moved(zero, at(0) * deg, 0, 0, at(1));
  if (kind == "pin_slot") return frame_moved(frame_moved(zero, 0, at(1), 0, 0), at(0) * deg, 0, 0, 0);
  if (kind == "planar") return frame_moved(frame_moved(zero, 0, at(0), at(1), 0), at(2) * deg, 0, 0, 0);
  if (kind == "screw") return frame_moved(zero, at(0) * deg, 0, 0, pitch * at(0) / 360);
  return zero;
}

json values_json(const std::vector<double>& v) {
  json a = json::array();
  for (double x : v) a.push_back(std::round(x * 1e9) / 1e9);
  return a;
}

json moved_json(const Scene& s, const json& pose) {
  json out = json::array();
  if (pose.is_object())
    for (const auto& p : pose.value("placements", json::array())) out.push_back({{"id", p["target"]}, {"name", node_name(s, p["target"].get<std::string>())}});
  return out;
}

// A joint coordinate's value from a number or [numbers / null].
std::vector<double> coordinate_values(const json& v, const std::string& joint) {
  std::vector<double> out;
  if (v.is_number()) return {v.get<double>()};
  if (!v.is_array()) throw Error("the value of joint " + joint + " is a number or a list of numbers (null leaves one free)");
  for (const auto& x : v) out.push_back(x.is_null() ? NAN : x.get<double>());
  return out;
}

json joint_summary(const Scene& s, const Joint& j) {
  json e = {{"id", j.id}, {"name", j.name}, {"kind", j.kind}};
  if (sim::is_relation(j.kind)) {
    e["joints"] = j.joints;
    for (const char* k : {"ratio", "radius", "lead", "reverse"})
      if (j.def.contains(k)) e[k] = j.def[k];
  } else {
    e["base"] = j.base.empty() ? json(nullptr) : json(j.base);
    e["base_name"] = node_name(s, j.base);
    e["part"] = j.part;
    e["part_name"] = node_name(s, j.part);
    e["values"] = values_json(j.values);
    json coords = json::array();
    if (const auto* k = sim::joint_kind(j.kind))
      for (const auto& c : k->coords) coords.push_back(c.name + (c.angle ? " (deg)" : " (mm)"));
    e["coordinates"] = coords;
    for (const char* k : {"limits", "locked", "pitch", "drive", "spring", "friction"})
      if (j.def.contains(k)) e[k] = j.def[k];
  }
  if (!j.error.empty()) e["error"] = j.error;
  return e;
}

// The joint command's arguments as JSON Schema: frames and parts may be references (strings) or objects.
json joint_args() {
  const json frame = {{"anyOf", {{{"type", "string"}}, {{"type", "object"}}}},
                      {"description", "face/edge/vertex ref or rule, axis ({base} | {direction, origin}), {point} or {origin, z, x}; flip, offset, x"}};
  const json node = {{"type", {"string", "null"}}};
  const json coord = {{"type", "string"}, {"enum", {"rotation", "translation", "x", "y"}}};
  json drive = {{"type", "object"},
                {"properties", {{"coordinate", coord}, {"mode", {{"type", "string"}, {"enum", {"position", "speed", "torque", "force"}}}}, {"value", {{"type", "number"}}},
                                {"expr", {{"type", "string"}}}, {"to", {{"type", "number"}}}, {"table", {{"type", "array"}}}}},
                {"description", "position deg|mm, speed deg/s|mm/s, torque N.mm, force N; expr of t"}};
  json spring = {{"type", "object"},
                 {"properties", {{"coordinate", coord}, {"stiffness", {{"type", "number"}}}, {"damping", {{"type", "number"}}}, {"rest", {{"type", "number"}}}}},
                 {"description", "N/mm, N.s/mm or N.mm/deg, N.mm.s/deg"}};
  return {{"doc", "path"},
          {"kind", {{"type", "string"}, {"enum", {"revolute", "slider", "cylindrical", "pin_slot", "planar", "ball", "screw", "rigid", "ground", "gear", "rack_pinion", "lead_screw"}}}},
          {"name", "string"},
          {"part", node},
          {"base", node},
          {"at", frame},
          {"at_part", frame},
          {"mate", "bool - snap faces against each other (default true)"},
          {"angle", "number - deg, snapping"},
          {"offset", "number - mm along z, snapping"},
          {"limits", {{"type", "object"},
                      {"additionalProperties", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}}},
                      {"description", "{coordinate: [min, max]} deg / mm"}}},
          {"locked", "bool - held at its values"},
          {"pitch", "number - screw: mm per turn (negative: left hand)"},
          {"joints", {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", "relations: the two joints"}}},
          {"ratio", "number - gear: 2nd turns per 1st turn (signed)"},
          {"teeth", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", "gear: [1st, 2nd]; ratio = -t1/t2"}}},
          {"internal", "bool - gear: ring pair (+t1/t2)"},
          {"radius", "number - rack_pinion: pitch radius mm"},
          {"lead", "number - lead_screw: mm per turn"},
          {"reverse", "bool - rack_pinion: other way"},
          {"drive", drive},
          {"spring", spring},
          {"friction", "number - N.mm or N"},
          {"id", "string - a joint to change"},
          {"by", "string"}};
}

}  // namespace

void register_sim_commands(const std::function<void(const CommandInfo&, Handler)>& add) {
  auto reg = [&](const char* name, const char* desc, json args, bool mutates, Handler h) { add({name, desc, std::move(args), mutates}, std::move(h)); };

  reg("joint",
      "Join two parts as a joint allows, or couple two joints (gear, rack_pinion, lead_screw); at_part snaps the part onto it; id changes one",
      joint_args(),
      true, [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene s = resolve(doc);
        const std::string by = a.value("by", "");
        // ---- change an existing joint
        if (a.contains("id")) {
          const Joint* j = s.joint(a["id"].get<std::string>());
          if (!j) throw Error("no joint " + a["id"].get<std::string>());
          json set = json::object();
          for (const char* k : {"name", "limits", "locked", "pitch", "drive", "spring", "friction", "ratio", "radius", "lead", "reverse"})
            if (a.contains(k)) set[k] = a[k];
          if (set.empty()) throw Error("joint: nothing to change (name, limits, locked, pitch, drive, spring, friction, ratio, radius, lead, reverse)");
          json effective = j->def;
          for (const auto& [k, v] : set.items()) {
            if (v.is_null()) effective.erase(k);
            else effective[k] = v;
          }
          Document::validate_op(effective);
          const std::string id = doc.append({{"op", "edit"}, {"target", j->id}, {"set", set}}, by).id;
          return json{{"id", j->id}, {"edit", id}, {"changed", set}};
        }
        const std::string kind = a.at("kind").get<std::string>();
        const sim::JointKind* k = sim::joint_kind(kind);
        if (!k) throw Error("joint: unknown kind \"" + kind + "\" (revolute, slider, cylindrical, pin_slot, planar, ball, screw, rigid, ground, gear, rack_pinion, lead_screw)");
        auto default_name = [&](const std::string& stem) {
          std::set<std::string> taken;
          for (const auto& j : s.joints) taken.insert(j.name);
          for (int n = 1;; ++n)
            if (!taken.count(stem + std::to_string(n))) return stem + std::to_string(n);
        };
        std::string label = kind;
        label[0] = char(std::toupper(label[0]));
        for (auto& c : label)
          if (c == '_') c = ' ';
        const std::string name = a.value("name", default_name(label + " "));
        // ---- relations
        if (k->relation) {
          const json js = a.value("joints", json());
          if (!js.is_array() || js.size() != 2) throw Error("joint: a " + kind + " couples two joints: \"joints\": [first, second]");
          const auto [c1, c2] = sim::relation_coords(kind);
          std::vector<double> at;
          for (size_t i = 0; i < 2; ++i) {
            const Joint* j = s.joint(js[i].get<std::string>());
            if (!j) throw Error("joint: no joint " + js[i].dump());
            const auto* jk = sim::joint_kind(j->kind);
            const int c = jk ? sim::coord_index(*jk, i == 0 ? c1 : c2) : -1;
            if (c < 0) throw Error("joint: a " + kind + " needs a " + (i == 0 ? c1 : c2) + " on \"" + j->name + "\" (a " + j->kind + " joint has none)");
            at.push_back(j->values[size_t(c)]);
          }
          json op = {{"op", "joint"}, {"name", name}, {"kind", kind}, {"joints", js}, {"values", values_json(at)}};
          if (kind == "gear") {
            double ratio = a.value("ratio", 0.0);
            if (a.contains("teeth")) {
              const json t = a["teeth"];
              if (!t.is_array() || t.size() != 2 || t[0].get<double>() <= 0 || t[1].get<double>() <= 0) throw Error("joint: teeth is [teeth on the 1st, teeth on the 2nd]");
              ratio = t[0].get<double>() / t[1].get<double>() * (a.value("internal", false) ? 1 : -1);
            }
            if (ratio == 0) throw Error("joint: a gear needs its ratio or teeth");
            op["ratio"] = ratio;
          } else if (kind == "rack_pinion") {
            if (!a.contains("radius") || a["radius"].get<double>() <= 0) throw Error("joint: a rack_pinion needs the pinion's pitch radius (mm)");
            op["radius"] = a["radius"];
            if (a.value("reverse", false)) op["reverse"] = true;
          } else {
            if (!a.contains("lead") || a["lead"].get<double>() == 0) throw Error("joint: a lead_screw needs its lead (mm per turn)");
            op["lead"] = a["lead"];
          }
          const std::string id = doc.append(op, by).id;
          const Scene after = resolve(doc);
          sim::Mechanism mech(after);
          const sim::Mechanism::Result r = mech.settle();
          json out = {{"id", id}, {"name", name}, {"kind", kind}};
          if (!r.ok) {
            doc.truncate_ops(doc.ops.size() - 1);
            throw Error("joint: " + r.error);
          }
          if (json pose = sim::pose_op(after, mech, "Pose after " + name); !pose.is_null()) {
            doc.append(pose, by);
            out["moved"] = moved_json(after, pose);
          }
          out["dof"] = mech.analysis()["dof"];
          return out;
        }
        // ---- joints between parts
        const bool snap = a.contains("at_part") && !a["at_part"].is_null();
        std::string part = a.contains("part") && !a["part"].is_null() ? node_of(s, a["part"], "joint part")
                           : snap ? node_of(s, a["at_part"], "joint part (from at_part)")
                                  : std::string();
        if (part.empty()) throw Error("joint: which part moves? give part (or at_part on it)");
        std::string base;
        if (kind != "ground") {
          if (a.contains("base")) base = a["base"].is_null() ? std::string() : node_of(s, a["base"], "joint base");
          else if (a.contains("at")) {
            const std::string on = sim::frame_node(a["at"]);
            if (!on.empty() && on != part && s.node(on)) base = on;
          }
        }
        if (base == part) throw Error("joint: the base and the part are the same node");
        if (!base.empty()) {
          for (const auto& p : s.path_to(part))
            if (p == base) throw Error("joint: \"" + node_name(s, part) + "\" is inside \"" + node_name(s, base) + "\": it already moves with it");
          for (const auto& p : s.path_to(base))
            if (p == part) throw Error("joint: \"" + node_name(s, base) + "\" is inside \"" + node_name(s, part) + "\"");
        }
        const Frame on_base = a.contains("at") ? sim::joint_frame(doc, s, a["at"]) : snap ? sim::joint_frame(doc, s, a["at_part"]) : Frame();
        if (!a.contains("at") && !snap) throw Error("joint: where is it? give at (a face, an edge, a vertex or an axis)");
        Frame on_part = snap ? sim::joint_frame(doc, s, a["at_part"]) : on_base;
        // Snapping mates: the part's frame is turned about its x so its z runs against the base's (faces meet).
        if (snap && a.value("mate", true)) {
          for (auto& c : on_part.y) c = -c;
        }
        // Coordinates it starts at: angle and offset when snapping; what has no coordinate is set into the base frame.
        std::vector<double> values(k->coords.size(), 0.0);
        Frame zero = on_base;
        double angle = a.value("angle", 0.0), offset = a.value("offset", 0.0);
        if ((angle != 0 || offset != 0) && !snap) throw Error("joint: angle and offset place the part when it snaps (with at_part); to move a joint later use joint_set");
        const int rot = sim::coord_index(*k, "rotation");
        const bool slides_z = kind == "slider" || kind == "cylindrical";
        if (rot >= 0) values[size_t(rot)] = angle;
        else zero = frame_moved(zero, angle * kPi / 180, 0, 0, 0);
        if (slides_z) values[size_t(sim::coord_index(*k, "translation"))] = offset;
        else zero = frame_moved(zero, 0, 0, 0, offset);
        const double pitch = a.value("pitch", 0.0);
        const Mat4 base_world = base.empty() ? Mat4() : s.world(base);
        const Mat4 part_world = s.world(part);
        json op = {{"op", "joint"}, {"name", name}, {"kind", kind}, {"base", base.empty() ? json(nullptr) : json(base)}, {"part", part}};
        // At zero the part's frame lies on the base's: both stored in their node's own coordinates.
        op["frames"] = {zero.transformed(base_world.inverse()).to_json(), on_part.transformed(part_world.inverse()).to_json()};
        if (!values.empty()) op["values"] = values_json(values);
        for (const char* key : {"limits", "locked", "pitch", "drive", "spring", "friction"})
          if (a.contains(key)) op[key] = a[key];
        if (a.contains("at")) op["refs"] = json{{"at", a["at"]}};
        if (snap) op["refs"]["at_part"] = a["at_part"];
        if (snap) refuse_moving_locked(s, {part});
        const size_t mark = doc.ops.size();
        const std::string id = doc.append(op, by).id;
        const Scene after = resolve(doc);
        sim::Mechanism mech(after);
        sim::Mechanism::Result r;
        if (snap) {
          // The part goes where its frame meets the base's at those values; whatever is joined to it follows.
          const Frame target = at_values(kind, zero, values, pitch);
          const Mat4 world = mat_of_frame(target) * mat_of_frame(on_part).inverse() * part_world;
          r = mech.place(part, world, base);
        } else {
          r = mech.settle(base);
        }
        if (!r.ok) {
          doc.truncate_ops(mark);
          throw Error("joint: " + r.error);
        }
        json out = {{"id", id}, {"name", name}, {"kind", kind}, {"base", base.empty() ? json(nullptr) : json(base)}, {"part", part}};
        if (json pose = sim::pose_op(after, mech, "Snap " + name); !pose.is_null()) {
          refuse_moving_locked(after, [&] {
            std::vector<std::string> ids;
            for (const auto& p : pose["placements"]) ids.push_back(p["target"].get<std::string>());
            return ids;
          }());
          doc.append(pose, by);
          out["moved"] = moved_json(after, pose);
        }
        out["values"] = values_json(mech.values()[id]);
        const auto [fa, fb] = mech.joint_frames(id);
        out["frame"] = design::frame_result(fa);
        out["dof"] = mech.analysis()["dof"];
        if (!r.notes.empty()) out["notes"] = r.notes;
        return out;
      });

  reg("joint_set",
      "Drive joints to values (deg / mm) or place a part; the rest follow through the joints. Writes a pose; preview only reports",
      {{"doc", "path"},
       {"values", {{"type", "object"}, {"additionalProperties", {{"anyOf", {{{"type", "number"}}, {{"type", "array"}, {"items", {{"type", {"number", "null"}}}}}}}}},
                   {"description", "{joint id: value (deg or mm) or [values, null leaves one free]}"}}},
       {"part", "string - a part to place instead (drag)"},
       {"matrix", "[16] row-major world placement for part"},
       {"anchor", "string - the part to keep still when nothing else holds the mechanism"},
       {"name", "string - the pose's name"},
       {"preview", "bool - solve and report without writing"},
       {"by", "string"}},
      true, [](Document* d, const json& a) {
        Document& doc = need_doc(d);
        const Scene s = resolve(doc);
        sim::Mechanism mech(s);
        sim::Mechanism::Result r;
        if (a.contains("part")) {
          const std::string part = node_of(s, a["part"], "joint_set part");
          r = mech.place(part, Mat4::from_json(a.at("matrix")), a.value("anchor", ""));
        } else {
          sim::Values want;
          const json v = a.value("values", json::object());
          if (!v.is_object() || v.empty()) throw Error("joint_set: give values {joint id: value} or a part with its matrix");
          for (const auto& [id, x] : v.items()) want[id] = coordinate_values(x, id);
          r = mech.drive(want, a.value("anchor", ""));
        }
        if (!r.ok) throw Error("joint_set: " + r.error + (r.reached > 0 ? " (it gets " + std::to_string(int(std::round(r.reached * 100))) + "% of the way)" : ""));
        json out;
        json values = json::object();
        for (const auto& [id, v] : mech.values()) values[id] = values_json(v);
        out["values"] = values;
        const json pose = sim::pose_op(s, mech, a.value("name", std::string("Pose")));
        out["moved"] = moved_json(s, pose);
        if (!r.notes.empty()) out["notes"] = r.notes;
        if (!a.value("preview", false) && !pose.is_null()) {
          std::vector<std::string> ids;
          for (const auto& p : pose["placements"]) ids.push_back(p["target"].get<std::string>());
          refuse_moving_locked(s, ids);
          out["id"] = doc.append(pose, a.value("by", "")).id;
        }
        return out;
      });

  reg("mechanism", "The joints, relations and parts of the document's mechanism: values, limits, drives, degrees of freedom and which joints can move",
      {{"doc", "path"}}, false, [](Document* d, const json&) {
        const Scene s = resolve(need_doc(d));
        sim::Mechanism mech(s);
        json out = mech.analysis();
        json joints = json::array();
        for (const auto& j : s.joints) {
          json e = joint_summary(s, j);
          for (const auto& x : out["joints"])
            if (x["id"] == j.id && x.contains("free")) e["free"] = x["free"];
          if (!sim::is_relation(j.kind) && j.error.empty() && mech.has_part(j.part)) {
            try {
              e["frame"] = design::frame_result(mech.joint_frames(j.id).first);
            } catch (const std::exception&) {
            }
          }
          joints.push_back(e);
        }
        out["joints"] = joints;
        json parts = json::array();
        for (const auto& p : mech.parts()) parts.push_back({{"id", p}, {"name", node_name(s, p)}});
        out["parts"] = parts;
        json studies = json::array();
        for (const auto& st : s.studies) studies.push_back({{"id", st.id}, {"name", st.name}, {"kind", st.kind}});
        if (!studies.empty()) out["studies"] = studies;
        json loads = json::array();
        for (const auto& l : s.loads) loads.push_back({{"id", l.id}, {"name", l.name}, {"kind", l.kind}, {"case", l.load_case}});
        if (!loads.empty()) out["loads"] = loads;
        out["engines"] = sim::engines();
        return out;
      });

  register_study_commands(add);
}

}  // namespace opad::commands
