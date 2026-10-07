#include "opad/sim/joints.hpp"

#include <algorithm>
#include <cmath>

#include "opad/util.hpp"

namespace opad::sim {

const std::vector<JointKind>& joint_kinds() {
  static const std::vector<JointKind> kinds = {
      {"rigid", {}, 6, false},
      {"ground", {}, 6, false},  // rigid to the world: the part stays where it is
      {"revolute", {{"rotation", true}}, 5, false},
      {"slider", {{"translation", false}}, 5, false},
      {"cylindrical", {{"rotation", true}, {"translation", false}}, 4, false},
      {"pin_slot", {{"rotation", true}, {"translation", false}}, 4, false},  // turns about z, slides along x
      {"planar", {{"x", false}, {"y", false}, {"rotation", true}}, 3, false},
      {"ball", {}, 3, false},
      {"screw", {{"rotation", true}}, 5, false},  // advances `pitch` mm along z per turn
      {"gear", {}, 1, true},
      {"rack_pinion", {}, 1, true},
      {"lead_screw", {}, 1, true},
  };
  return kinds;
}

const JointKind* joint_kind(const std::string& kind) {
  for (const auto& k : joint_kinds())
    if (k.kind == kind) return &k;
  return nullptr;
}

bool is_relation(const std::string& kind) {
  const JointKind* k = joint_kind(kind);
  return k && k->relation;
}

int coord_index(const JointKind& kind, const std::string& coord) {
  for (size_t i = 0; i < kind.coords.size(); ++i)
    if (kind.coords[i].name == coord) return int(i);
  return -1;
}

std::pair<std::string, std::string> relation_coords(const std::string& kind) {
  if (kind == "gear") return {"rotation", "rotation"};
  return {"rotation", "translation"};
}

const std::vector<std::string>& load_kinds() {
  static const std::vector<std::string> kinds = {"fixed", "force", "pressure", "moment", "bolt_preload", "gravity", "displacement",
                                                 "heat", "temperature", "convection", "radiation", "fan"};
  return kinds;
}

const std::vector<std::string>& study_kinds() {
  static const std::vector<std::string> kinds = {"motion", "dynamic", "static", "modal", "thermal"};
  return kinds;
}

namespace {

void need_string(const json& op, const char* key, const char* what) {
  if (!op.contains(key) || !op[key].is_string() || op[key].get<std::string>().empty())
    throw Error(std::string(what) + ": '" + key + "' must be a non-empty string");
}

bool numbers(const json& v, size_t n) {
  return v.is_array() && (n == 0 || v.size() == n) &&
         std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number() && std::isfinite(x.get<double>()); });
}

void check_frame(const json& f, const char* what) {
  if (!f.is_object() || !numbers(f.value("origin", json()), 3) || !numbers(f.value("x", json()), 3) || !numbers(f.value("y", json()), 3))
    throw Error(std::string(what) + ": a frame is {\"origin\": [x, y, z], \"x\": [..], \"y\": [..]}");
  double xx = 0, yy = 0, xy = 0;
  for (int i = 0; i < 3; ++i) {
    const double x = f["x"][i].get<double>(), y = f["y"][i].get<double>();
    xx += x * x, yy += y * y, xy += x * y;
  }
  if (std::fabs(xx - 1) > 1e-6 || std::fabs(yy - 1) > 1e-6 || std::fabs(xy) > 1e-6)
    throw Error(std::string(what) + ": a frame's x and y must be unit vectors at right angles");
}

}  // namespace

void validate_joint_op(const json& op) {
  need_string(op, "name", "joint");
  need_string(op, "kind", "joint");
  const std::string kind = op["kind"].get<std::string>();
  const JointKind* k = joint_kind(kind);
  if (!k) return;  // a kind of a newer build: kept as written, reported when replayed
  if (op.contains("values") && !numbers(op["values"], 0)) throw Error("joint: 'values' must be a list of numbers");
  if (k->relation) {
    const json& j = op.value("joints", json());
    if (!j.is_array() || j.size() != 2 || !j[0].is_string() || !j[1].is_string() || !is_uuid(j[0].get<std::string>()) ||
        !is_uuid(j[1].get<std::string>()) || j[0] == j[1])
      throw Error("joint: a " + kind + " couples two different joints: 'joints': [id, id]");
    for (const char* key : {"ratio", "radius", "lead"})
      if (op.contains(key) && !(op[key].is_number() && std::isfinite(op[key].get<double>())))
        throw Error(std::string("joint: '") + key + "' must be a number");
    if (op.contains("carrier")) {
      if (!op["carrier"].is_string() || !is_uuid(op["carrier"].get<std::string>())) throw Error("joint: 'carrier' must be a node id");
      const json& cf = op.value("carrier_frames", json());
      if (!cf.is_array() || cf.size() != 2) throw Error("joint: a relation with a carrier keeps 'carrier_frames': the two joints' frames in it");
      check_frame(cf[0], "joint");
      check_frame(cf[1], "joint");
    }
    return;
  }
  if (!op.contains("part") || !op["part"].is_string() || !is_uuid(op["part"].get<std::string>()))
    throw Error("joint: 'part' must be the id of the part it moves");
  if (op.contains("base") && !op["base"].is_null() && !(op["base"].is_string() && is_uuid(op["base"].get<std::string>())))
    throw Error("joint: 'base' must be a node id or null (the world)");
  if (op.contains("base") && op["base"] == op["part"]) throw Error("joint: a part cannot be joined to itself");
  const json& frames = op.value("frames", json());
  if (!frames.is_array() || frames.size() != 2) throw Error("joint: 'frames' must hold the joint's frame on the base and on the part");
  check_frame(frames[0], "joint");
  check_frame(frames[1], "joint");
  if (op.contains("limits")) {
    if (!op["limits"].is_object()) throw Error("joint: 'limits' must be {coordinate: [min, max]}");
    for (const auto& [c, v] : op["limits"].items()) {
      if (coord_index(*k, c) < 0) throw Error("joint: a " + kind + " joint has no coordinate '" + c + "' to limit");
      if (!v.is_null() && (!numbers(v, 2) || v[0].get<double>() > v[1].get<double>()))
        throw Error("joint: the limits of '" + c + "' are [min, max] with min <= max");
    }
  }
  for (const char* key : {"locked"})
    if (op.contains(key) && !op[key].is_boolean()) throw Error(std::string("joint: '") + key + "' must be true or false");
  for (const char* key : {"pitch", "friction"})
    if (op.contains(key) && !(op[key].is_number() && std::isfinite(op[key].get<double>())))
      throw Error(std::string("joint: '") + key + "' must be a number");
  if (kind == "screw" && (!op.contains("pitch") || std::fabs(op["pitch"].get<double>()) < 1e-12))
    throw Error("joint: a screw joint needs its 'pitch' (mm it advances per turn, negative for a left-hand thread)");
  for (const char* key : {"drive", "spring"})
    if (op.contains(key) && !op[key].is_null() && !op[key].is_object()) throw Error(std::string("joint: '") + key + "' must be an object");
}

void validate_pose_op(const json& op) {
  const json& p = op.value("placements", json());
  if (!p.is_array()) throw Error("pose: 'placements' must be a list of {target, matrix}");
  for (const auto& e : p) {
    if (!e.is_object() || !e.contains("target") || !e["target"].is_string() || !is_uuid(e["target"].get<std::string>()))
      throw Error("pose: each placement names its 'target' node");
    if (!numbers(e.value("matrix", json()), 16) && !numbers(e.value("matrix", json()), 12))
      throw Error("pose: each placement has its 'matrix' (16 numbers, row-major)");
  }
  if (op.contains("values")) {
    if (!op["values"].is_object()) throw Error("pose: 'values' must be {joint: [coordinates]}");
    for (const auto& [j, v] : op["values"].items())
      if (!is_uuid(j) || !numbers(v, 0)) throw Error("pose: 'values' must be {joint id: [numbers]}");
  }
}

void validate_load_op(const json& op) {
  need_string(op, "name", "load");
  need_string(op, "kind", "load");
  if (op.contains("case") && !op["case"].is_string()) throw Error("load: 'case' must be text");
  if (op.contains("refs") && !op["refs"].is_array()) throw Error("load: 'refs' must be a list of references");
  for (const char* key : {"vector", "direction"})
    if (op.contains(key) && !numbers(op[key], 3)) throw Error(std::string("load: '") + key + "' must be [x, y, z]");
  for (const char* key : {"value", "magnitude", "ambient", "velocity"})
    if (op.contains(key) && !(op[key].is_number() && std::isfinite(op[key].get<double>())))
      throw Error(std::string("load: '") + key + "' must be a number");
  // Thermal loads (sim/fea.hpp): a film coefficient or how to find it, an emissivity, fans.
  if (op.contains("h")) {
    const json& h = op["h"];
    const bool ok = (h.is_number() && h.get<double>() > 0) || (h.is_string() && (h == "natural" || h == "forced"));
    if (!ok) throw Error("load: 'h' is a film coefficient in W/m2K, \"natural\" or \"forced\"");
  }
  if (op.contains("emissivity") && !(op["emissivity"].is_number() && op["emissivity"].get<double>() >= 0 && op["emissivity"].get<double>() <= 1))
    throw Error("load: 'emissivity' is 0 to 1");
  if (op.contains("count") && !(op["count"].is_number_integer() && op["count"].get<int>() >= 1)) throw Error("load: 'count' is a whole number, 1 or more");
}

void validate_study_op(const json& op) {
  need_string(op, "name", "study");
  need_string(op, "kind", "study");
  if (op.contains("settings") && !op["settings"].is_object()) throw Error("study: 'settings' must be an object");
  if (op.contains("result") && !op["result"].is_object()) throw Error("study: 'result' must be an object");
}

}  // namespace opad::sim
