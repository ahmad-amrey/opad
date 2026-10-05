#include "opad/document.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>

#include "opad/drawing/sheet.hpp"
#include "opad/assets.hpp"
#include "opad/scene.hpp"

namespace opad {
namespace {
bool sketch_records(const json& j);
std::string record_text(const json& j, int depth = 0);
}

// ---------------------------------------------------------------- Header
json Header::to_json() const {
  json j;
  j["uuid"] = uuid;
  j["units"] = units;
  j["created"] = created;
  j["generator"] = generator;
  return j;
}

Header Header::from_json(const json& j) {
  Header h;
  h.uuid = j.value("uuid", "");
  h.units = j.value("units", "mm");
  h.created = j.value("created", "");
  h.generator = j.value("generator", "");
  if (!is_uuid(h.uuid)) throw Error("header: missing or invalid document uuid");
  return h;
}

// ---------------------------------------------------------------- BodyEntry
std::string_view BodyEntry::checked_text() const {
  if (indexed.empty()) return brep;
  unsigned char s = check.state.load(std::memory_order_acquire);
  if (s == 0) check.state.store(s = sha256_hex(indexed) == key ? 1 : 2, std::memory_order_release);
  if (s == 2) throw Error("body entry " + key.substr(0, 12) + ": content does not match its key (corrupted or edited)");
  return indexed;
}

// ---------------------------------------------------------------- Document
Document::Document() : shape_cache(make_shape_cache()) {}

Document Document::create(const std::string& units) {
  Document d;
  d.header.format = kFormatVersion;
  d.header.uuid = new_uuid();
  d.header.units = units;
  d.header.created = now_iso8601();
  d.header.generator = "opad/" + version_string();
  d.dirty = true;
  return d;
}

const std::vector<std::string>& Document::op_types() {
  static const std::vector<std::string> t = {"import",     "reparent",    "transform", "appearance", "rename", "annotation",
                                             "measurement", "section",    "view",      "delete",     "param",  "sketch",
                                             "feature",    "edit",        "regen", "units",      "sheet",  "sheet_view",
                                             "sheet_item", "properties"};
  return t;
}

bool Document::known_type(const std::string& type) {
  const auto& types = op_types();
  return std::find(types.begin(), types.end(), type) != types.end();
}

static void require(const json& op, const char* key, const char* type) {
  if (!op.contains(key)) throw Error(std::string("op '") + op.value("op", "?") + "' is missing field '" + key + "'");
  const json& v = op[key];
  std::string t = type;
  bool ok = (t == "string" && v.is_string()) || (t == "array" && v.is_array()) ||
            (t == "object" && (v.is_object() || v.is_string())) || (t == "number" && v.is_number()) ||
            (t == "uuid" && v.is_string() && is_uuid(v.get<std::string>())) ||
            (t == "uuid?" && (v.is_null() || (v.is_string() && is_uuid(v.get<std::string>())))) ||
            (t == "matrix" && v.is_array() && (v.size() == 12 || v.size() == 16));
  if (!ok) throw Error(std::string("op '") + op.value("op", "?") + "': field '" + key + "' must be " + type);
}

static void validate_nodes(const json& nodes, int depth = 0) {
  if (!nodes.is_array()) throw Error("import: 'nodes' must be an array");
  if (depth > 64) throw Error("import: hierarchy too deep");
  for (const auto& n : nodes) {
    if (!n.is_object()) throw Error("import: node must be an object");
    std::string type = n.value("type", "");
    if (type != "component" && type != "body") throw Error("import: node type must be 'component' or 'body'");
    require(n, "id", "uuid");
    if (n.contains("transform") && !n["transform"].is_null()) require(n, "transform", "matrix");
    if (type == "body") {
      require(n, "key", "string");
      if (n["key"].get<std::string>().size() != 64) throw Error("import: body key must be a sha-256 hex digest");
    } else if (n.contains("children")) {
      validate_nodes(n["children"], depth + 1);
    }
  }
}

void Document::validate_op(const json& op) {
  if (!op.is_object()) throw Error("op must be a JSON object");
  if (!op.contains("op") || !op["op"].is_string()) throw Error("op is missing the 'op' type field");
  std::string type = op["op"].get<std::string>();
  if (!known_type(type)) throw Error("unknown op type: " + type);
  if (op.contains("id")) require(op, "id", "uuid");
  if(type=="units") {
    require(op,"length","string");const std::set<std::string> units={"mm","cm","m","um","in","ft"};
    if(!units.count(op.at("length").get<std::string>()))throw Error("unsupported document length unit");
  } else if (type == "import") {
    validate_nodes(op.value("nodes", json::array()));
    if (op.contains("parent") && !op["parent"].is_null()) require(op, "parent", "uuid");
  } else if (type == "reparent") {
    require(op, "target", "uuid");
    require(op, "parent", "uuid?");
  } else if (type == "transform") {
    require(op, "target", "uuid");
    require(op, "matrix", "matrix");
  } else if (type == "appearance") {
    require(op, "target", "uuid");
    if (!op.contains("color") && !op.contains("opacity") && !op.contains("visible") && !op.contains("locked"))
      throw Error("appearance: needs at least one of color, opacity, visible, locked");
    if (op.contains("color") && !(op["color"].is_array() && op["color"].size() == 3))
      throw Error("appearance: color must be [r,g,b] in 0..1");
    if (op.contains("layer") && !op["layer"].is_object()) throw Error("appearance: layer must be an object");
    if (op.contains("default_color") && !op["default_color"].is_boolean()) throw Error("appearance: default_color must be true or false");
  } else if (type == "rename") {
    require(op, "target", "uuid");
    require(op, "name", "string");
  } else if (type == "annotation") {
    if (op.contains("reply_to")) require(op, "reply_to", "uuid");
    require(op, "anchor", "object");
    require(op, "text", "string");
    Ref::from_json(op["anchor"]);
    if (op.contains("style")) {
      const auto& styles = annotation_styles();
      if (!op["style"].is_string() || std::find(styles.begin(), styles.end(), op["style"].get<std::string>()) == styles.end())
        throw Error("annotation: style must be one of ok, warning, issue, note, ai_agent");
    }
    if (op.contains("drawing")) {
      const auto& drawing = op.at("drawing");
      if (!drawing.is_object() || !drawing.contains("plane") || !drawing.at("plane").is_object())
        throw Error("annotation drawing: plane requires origin, x and y vectors in world mm");
      auto validatePlane = [](const json& plane) {
        if(!plane.is_object()) throw Error("annotation drawing: plane must be an object");
        for (const char* key : {"origin", "x", "y"}) {
          if (!plane.contains(key) || !plane.at(key).is_array() || plane.at(key).size() != 3)
            throw Error("annotation drawing: plane vectors must have three finite numbers");
          for (const auto& v : plane.at(key)) if (!v.is_number() || !std::isfinite(v.get<double>()))
            throw Error("annotation drawing: non-finite plane coordinate");
        }
        double xx=0, yy=0, xy=0;
        for (int i=0;i<3;++i) {double x=plane.at("x")[i], y=plane.at("y")[i];xx+=x*x;yy+=y*y;xy+=x*y;}
        if (std::abs(xx-1)>1e-6 || std::abs(yy-1)>1e-6 || std::abs(xy)>1e-6)
          throw Error("annotation drawing: plane axes must be orthonormal");
      };
      validatePlane(drawing.at("plane"));
      if (!drawing.contains("strokes") || !drawing.at("strokes").is_array() || drawing.at("strokes").empty() || drawing.at("strokes").size()>128)
        throw Error("annotation drawing: requires 1..128 strokes");
      size_t count=0;
      for (const auto& stroke : drawing.at("strokes")) {
        if (!stroke.is_object() || !stroke.contains("color") || !stroke.at("color").is_string()
            || (stroke.at("color")!="red" && stroke.at("color")!="green" && stroke.at("color")!="blue" && stroke.at("color")!="white")) throw Error("annotation drawing: color must be red, green, blue or white");
        if (!stroke.contains("width") || !stroke.at("width").is_number()
            || (stroke.at("width")!=1 && stroke.at("width")!=2 && stroke.at("width")!=4 && stroke.at("width")!=6 && stroke.at("width")!=8)) throw Error("annotation drawing: width must be 1, 2, 4 or 8 pixels (legacy 6 also accepted)");
        if (!stroke.contains("points") || !stroke.at("points").is_array() || stroke.at("points").size()<2)
          throw Error("annotation drawing: a stroke needs at least two points");
        if(stroke.contains("plane")) validatePlane(stroke.at("plane"));
        count+=stroke.at("points").size();
        if (count>8192) throw Error("annotation drawing: maximum 8192 points per annotation");
        for (const auto& point : stroke.at("points")) {
          if (!point.is_array() || point.size()!=2) throw Error("annotation drawing: points must be [u,v] in plane mm");
          for (const auto& v : point) if (!v.is_number() || !std::isfinite(v.get<double>())) throw Error("annotation drawing: non-finite point");
        }
      }
    }
  } else if (type == "measurement") {
    require(op, "kind", "string");
    require(op, "refs", "array");
    for (const auto& r : op["refs"]) Ref::from_json(r);
  } else if (type == "section") {
    require(op, "name", "string");
    require(op, "origin", "array");
    require(op, "normal", "array");
  } else if (type == "view") {
    require(op, "name", "string");
    require(op, "camera", "object");
    if (op.contains("display") && !op["display"].is_object()) throw Error("view: display must be an object");
  } else if (type == "delete") {
    require(op, "target", "uuid");
  } else if (type == "param") {
    require(op, "name", "string");
    require(op, "expr", "string");
  } else if (type == "sketch") {
    require(op, "name", "string");
    if (!op.contains("plane") || !op["plane"].is_object()) throw Error("sketch: 'plane' must be an object");
    if (!op.contains("geometry") || !op["geometry"].is_object()) throw Error("sketch: 'geometry' must be an object");
    if (op.contains("component")) require(op, "component", "uuid?");
  } else if (type == "feature") {
    require(op, "kind", "string");
    require(op, "name", "string");
    if (!op.contains("inputs") || !op["inputs"].is_object()) throw Error("feature: 'inputs' must be an object");
    if (op.contains("result") && !op["result"].is_object()) throw Error("feature: 'result' must be an object");
    if (op.contains("component")) require(op, "component", "uuid?");
  } else if (type == "edit") {
    require(op, "target", "uuid");
    if (!op.contains("set") || !op["set"].is_object()) throw Error("edit: 'set' must be an object");
  } else if (type == "regen") {
    if (!op.contains("results") || !op["results"].is_object()) throw Error("regen: 'results' must be an object");
  } else if (drawing::is_sheet_record(type)) {
    drawing::validate_record(op);
  } else if (type == "properties") {
    require(op, "target", "uuid");
    if (!op.contains("set") || !op["set"].is_object() || op["set"].empty()) throw Error("properties: 'set' must be a non-empty object");
    for (const auto& [k, v] : op["set"].items())
      if (k.empty() || !(v.is_string() || v.is_number() || v.is_boolean() || v.is_null()))
        throw Error("properties: '" + k + "' must be text, a number, true, false or null");
  }
}

const Op& Document::append(json op, const std::string& author) {
  validate_op(op);
  // Canonical key order: op, id, ts, by, then everything else in the order given.
  json out;
  out["op"] = op["op"];
  out["id"] = op.contains("id") ? op["id"] : json(new_uuid());
  out["ts"] = op.contains("ts") ? op["ts"] : json(now_iso8601());
  out["by"] = op.contains("by") ? op["by"] : json(author.empty() ? default_author() : author);
  for (auto it = op.begin(); it != op.end(); ++it) {
    if (it.key() == "op" || it.key() == "id" || it.key() == "ts" || it.key() == "by") continue;
    out[it.key()] = std::move(it.value());
  }
  std::string id = out["id"].get<std::string>();
  if (find_op(id)) throw Error("duplicate op id: " + id);
  if ((out["op"] == "delete" || out["op"] == "edit") && !find_op(out["target"].get<std::string>()))
    throw Error(out["op"].get<std::string>() + ": target op not found: " + out["target"].get<std::string>());
  if (out["op"] == "edit") {
    const auto* target = find_op(out["target"].get<std::string>());
    if (target && !known_type(target->type)) throw Error("edit: op '" + target->type + "' needs a newer OPAD; this build cannot edit it");
    if (target && (target->type == "annotation" || target->type == "import" || drawing::is_sheet_record(target->type))) {  // an asset's sync rewrites the nodes
      for (const char* k : {"op", "id", "ts", "by"})
        if (drawing::is_sheet_record(target->type) && out["set"].contains(k)) throw Error(std::string("edit: '") + k + "' cannot be changed");
      json effective = target->data;
      for (const auto& e : effective_ops(*this)) if (e.op->id == target->id) effective = e.data();
      for (const auto& [key,value] : out["set"].items()) {if(value.is_null()) effective.erase(key); else effective[key]=value;}
      validate_op(effective);
    }
  }
  Op o;
  o.id = id;
  o.type = out["op"].get<std::string>();
  o.raw = sketch_records(out) ? record_text(out) : out.dump();
  o.data = std::move(out);
  ops.push_back(std::move(o));
  dirty = true;
  return ops.back();
}

void Document::rewrite_op(size_t index, json data) {
  if (index < persisted_ops_ || index >= ops.size()) throw Error("only an op not saved yet can be rewritten");
  Op& o = ops[index];
  if (data.value("id", "") != o.id || data.value("op", "") != o.type) throw Error("a rewritten op keeps its id and type");
  validate_op(data);
  o.raw = sketch_records(data) ? record_text(data) : data.dump();
  o.data = std::move(data);
  dirty = true;
}

std::string Document::add_body(const std::string& brep, json meta) {
  if (brep.empty() || brep.back() != '\n') throw Error("body BREP text must end with a newline");
  if (brep.find('\r') != std::string::npos) throw Error("body BREP text must use LF line endings");
  std::string key = sha256_hex(brep);
  if (bodies_index_.count(key)) return key;
  BodyEntry e;
  e.key = key;
  e.meta = std::move(meta);
  e.brep = brep;
  bodies_index_[key] = bodies_.size();
  bodies_.push_back(std::move(e));
  dirty = true;
  return key;
}

std::string Document::add_body(const std::string& key, std::string&& brep, json meta) {
  if (brep.empty() || brep.back() != '\n') throw Error("body BREP text must end with a newline");
  if (key.size() != 64) throw Error("body key must be the SHA-256 of its BREP text");
  if (bodies_index_.count(key)) return key;
  BodyEntry e;
  e.key = key;
  e.meta = std::move(meta);
  e.brep = std::move(brep);
  bodies_index_[key] = bodies_.size();
  bodies_.push_back(std::move(e));
  dirty = true;
  return key;
}

std::string Document::add_live_body(const std::string& key, json meta) {
  if (bodies_index_.count(key)) return key;
  BodyEntry e;
  e.key = key;
  e.meta = std::move(meta);
  bodies_index_[key] = bodies_.size();
  bodies_.push_back(std::move(e));
  dirty = true;
  return key;
}

bool Document::has_live_bodies() const {
  for (const auto& b : bodies_)
    if (b.brep.empty() && b.indexed.empty() && !b.external) return true;
  return false;
}

std::string Document::add_external_body(const std::string& key, json meta) {
  if (auto it = bodies_index_.find(key); it != bodies_index_.end()) {
    BodyEntry& e = bodies_[it->second];
    if (e.brep.empty()) e.external = true, e.meta = std::move(meta);  // read again (a live body, a stale shape): as read now
    return key;
  }
  BodyEntry e;
  e.key = key;
  e.meta = std::move(meta);
  e.external = true;
  bodies_index_[key] = bodies_.size();
  bodies_.push_back(std::move(e));
  return key;
}

std::vector<Op> Document::truncate_ops(size_t count) {
  if (count >= ops.size()) return {};
  std::vector<Op> removed(ops.begin() + static_cast<std::ptrdiff_t>(count), ops.end());
  ops.resize(count);
  dirty = true;
  return removed;
}

void Document::restore_ops(std::vector<Op> removed) {
  for (auto& o : removed) {
    if (find_op(o.id)) throw Error("restore: duplicate op id: " + o.id);
    ops.push_back(std::move(o));
  }
  dirty = true;
}

const BodyEntry* Document::body(const std::string& key) const {
  auto it = bodies_index_.find(key);
  return it == bodies_index_.end() ? nullptr : &bodies_[it->second];
}

void Document::arrange_bodies(const std::vector<std::string>& keys, Document& from, bool keep_others) {
  for (const auto& key : keys) {
    if (bodies_index_.count(key)) continue;
    const auto it = from.bodies_index_.find(key);
    if (it == from.bodies_index_.end()) throw Error("body entry missing: " + key);
    if (from.source_ != source_) from.bodies_[it->second].checked_text();  // it leaves `from`'s text as a verified copy
  }
  std::vector<BodyEntry> out;
  out.reserve(keys.size() + (keep_others ? bodies_.size() : 0));
  std::vector<bool> taken(bodies_.size());
  std::set<std::string> placed;
  for (const auto& key : keys) {
    if (!placed.insert(key).second) continue;
    if (auto it = bodies_index_.find(key); it != bodies_index_.end()) {
      out.push_back(std::move(bodies_[it->second]));
      taken[it->second] = true;
    } else {
      auto& entry = from.bodies_[from.bodies_index_.at(key)];
      if (!entry.indexed.empty() && from.source_ != source_) entry.brep.assign(entry.indexed), entry.indexed = {};  // `from`'s text goes with it
      out.push_back(std::move(entry));
      entry.key.clear();
    }
  }
  if (keep_others)
    for (size_t i = 0; i < bodies_.size(); ++i)
      if (!taken[i]) out.push_back(std::move(bodies_[i]));
  bodies_ = std::move(out);
  bodies_index_.clear();
  for (size_t i = 0; i < bodies_.size(); ++i) bodies_index_[bodies_[i].key] = i;
  // `from` keeps what was not moved out
  from.bodies_.erase(std::remove_if(from.bodies_.begin(), from.bodies_.end(), [](const BodyEntry& b) { return b.key.empty(); }), from.bodies_.end());
  from.bodies_index_.clear();
  for (size_t i = 0; i < from.bodies_.size(); ++i) from.bodies_index_[from.bodies_[i].key] = i;
  dirty = true;
}

std::vector<std::string> Document::body_keys() const {
  std::vector<std::string> keys;
  keys.reserve(bodies_.size());
  for (const auto& b : bodies_) keys.push_back(b.key);
  return keys;
}

const Op* Document::find_op(const std::string& id) const {
  for (const auto& o : ops)
    if (o.id == id) return &o;
  return nullptr;
}

bool Document::is_deleted(const std::string& op_id) const {
  Scene s = resolve(*this);
  return std::find(s.deleted_ops.begin(), s.deleted_ops.end(), op_id) != s.deleted_ops.end();
}

static void collect_keys(const json& nodes, std::set<std::string>& keys) {
  for (const auto& n : nodes) {
    if (n.value("type", "") == "body") keys.insert(n.value("key", ""));
    if (n.contains("children")) collect_keys(n["children"], keys);
  }
}

// An op of a newer build: any string that is, or starts with, a body key of the store keeps that entry ("<key>", a
// "<key>/edge/3" token), wherever the record holds it.
static void collect_mentioned_keys(const json& j, const std::unordered_map<std::string, size_t>& store, std::set<std::string>& keys) {
  if (j.is_string()) {
    const std::string& s = j.get_ref<const std::string&>();
    if (s.size() >= 64 && (s.size() == 64 || !std::isxdigit(static_cast<unsigned char>(s[64]))) && store.count(s.substr(0, 64))) keys.insert(s.substr(0, 64));
  } else if (j.is_structured()) {
    for (const auto& v : j) collect_mentioned_keys(v, store, keys);
  }
}

std::vector<std::string> Document::gc() {
  std::set<std::string> live;
  // Feature results keep every intermediate state alive (the body before a fillet is what the fillet is
  // recomputed from); results an edit has superseded are not part of the effective log and go.
  const auto keep = [&](const std::vector<EffectiveOp>& log) {
    for (const auto& o : log) {
      if (o.op->type == "import") collect_keys(o.data().value("nodes", json::array()), live);
      else if (o.op->type == "feature" && o.data().contains("result"))
        for (const auto& b : o.data()["result"].value("bodies", json::array())) live.insert(b.value("key", ""));
      else if (drawing::is_sheet_record(o.op->type)) {  // a sheet's template geometry, an issue's frozen linework
        std::vector<std::string> keys;
        drawing::record_body_keys(o.data(), keys);
        live.insert(keys.begin(), keys.end());
      }
      else if (!known_type(o.op->type)) collect_mentioned_keys(o.data(), bodies_index_, live);
    }
  };
  const auto log = effective_ops(*this);
  keep(log);
  // An issued drawing revision keeps the model it showed (drawing::issued_scene: the log up to the issue).
  for (const auto& o : log)
    if (o.op->type == "sheet_item" && o.data().value("kind", "") == "issue") {
      std::vector<const Op*> upto;
      for (const auto& op : ops) {
        upto.push_back(&op);
        if (&op == o.op) break;
      }
      keep(effective_ops(upto));
    }
  std::vector<std::string> removed;
  for (const auto& b : bodies_)
    if (!live.count(b.key)) removed.push_back(b.key);
  if (!removed.empty()) {  // only then are the entries moved: moved-from ones kept in place had lost their BREP text
    std::vector<BodyEntry> kept;
    for (auto& b : bodies_)
      if (live.count(b.key)) kept.push_back(std::move(b));
    bodies_ = std::move(kept);
    bodies_index_.clear();
    for (size_t i = 0; i < bodies_.size(); ++i) bodies_index_[bodies_[i].key] = i;
    dirty = true;
  }
  return removed;
}

// ---------------------------------------------------------------- format
// Layout (all LF):
//   #opad <format>
//   <header json>
//   #ops
//   <one JSON object per line>
//   #bodies
//   #body <sha256> <line-count> <meta json>
//   <line-count lines of ASCII BREP>
//   ... repeated
namespace {
// Expand containers leading to sketch/drawing records. Each point/curve/constraint/stroke stays on one line.
bool sketch_records(const json& j) {
  if (!j.is_object()) return false;
  if (j.contains("points") || j.contains("entities") || j.contains("constraints") || j.contains("more_constraints") || j.contains("patterns") || j.contains("images") || j.contains("strokes")) return true;
  for (const auto& v : j) if (sketch_records(v)) return true;
  return false;
}
std::string record_text(const json& j, int depth) {
  if (!sketch_records(j)) return j.dump();
  const std::string indent(size_t(depth + 1) * 2, ' ');
  std::string out = "{";
  bool first = true;
  for (const auto& [k,v] : j.items()) {
    if (!first) out += ',';
    first = false;
    out += '\n' + indent + json(k).dump() + ": ";
    if ((k == "points" || k == "entities" || k == "constraints" || k == "more_constraints" || k == "patterns" || k == "images" || k == "strokes") && v.is_array()) {
      out += '[';
      for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ',';
        out += '\n' + indent + "  " + v[i].dump();
      }
      if (!v.empty()) out += '\n' + indent;
      out += ']';
    } else out += record_text(v, depth + 1);
  }
  return out + '\n' + std::string(size_t(depth) * 2, ' ') + '}';
}
}

std::string Document::serialize() const {
  if (has_live_bodies()) throw Error("viewer-mode document: its bodies have no BREP text; export it to an .opad document first");
  std::string out;
  size_t reserve = 256;
  for (const auto& b : bodies_) reserve += b.text().size() + 128;
  for (const auto& o : ops) reserve += o.raw.size() + 1;
  out.reserve(reserve);
  out += "#opad ";
  out += std::to_string(kFormatVersion);
  out += '\n';
  out += header.to_json().dump();
  out += '\n';
  out += "#ops\n";
  for (const auto& o : ops) {
    out += o.raw.empty() ? record_text(o.data) : o.raw;
    out += '\n';
  }
  out += "#bodies\n";
  for (const auto& b : bodies_) {
    if (b.external) continue;  // a linked asset's: read from its file
    const std::string_view brep = b.text();
    size_t lines = static_cast<size_t>(std::count(brep.begin(), brep.end(), '\n'));
    out += "#body ";
    out += b.key;
    out += ' ';
    out += std::to_string(lines);
    out += ' ';
    out += b.meta.dump();
    out += '\n';
    out += brep;
  }
  return out;
}

Document Document::parse(const std::string& text, const std::filesystem::path& origin, const BodyFilter& skip_body,
                         const Progress& progress) {
  return parse_text(text, origin, skip_body, false, progress);
}

Document Document::parse_index(std::string text, const std::filesystem::path& origin, const BodyFilter& skip_body) {
  // The entries point into the text, which therefore is what a verifying read would make of it: LF only, and a last line
  // that ends.
  if (text.find('\r') != std::string::npos) {
    size_t w = 0;
    for (size_t r = 0; r < text.size(); ++r)
      if (text[r] != '\r' || r + 1 >= text.size() || text[r + 1] != '\n') text[w++] = text[r];
    text.resize(w);
  }
  if (!text.empty() && text.back() != '\n') text += '\n';
  auto source = std::make_shared<const std::string>(std::move(text));
  Document d = parse_text(*source, origin, skip_body, true);
  d.source_ = std::move(source);
  return d;
}

Document Document::parse_text(std::string_view text, const std::filesystem::path& origin, const BodyFilter& skip_body, bool index,
                               const Progress& progress) {
  Document d;
  double reported = 0;
  auto report = [&](std::string_view at) {  // how far through the text `at` begins, every half per cent
    if (!progress || text.empty()) return;
    const double f = static_cast<double>(at.data() - text.data()) / static_cast<double>(text.size());
    if (f - reported < 0.005) return;
    reported = f;
    if (!progress(f)) throw Error("cancelled");
  };
  d.path = origin;
  std::string where = origin.empty() ? std::string("<memory>") : origin.string();
  auto fail = [&](size_t line, const std::string& msg) {
    throw Error(where + ":" + std::to_string(line + 1) + ": " + msg);
  };

  // Line by line from `pos`; `i` is the number of the line next() returned last. Body text is only scanned for its
  // line ends, then copied (or, in index mode, pointed at) in one piece.
  size_t pos = 0, i = size_t(-1);
  std::string_view l;
  auto next = [&]() {
    if (pos >= text.size()) return false;
    ++i;
    const size_t nl = text.find('\n', pos);
    size_t len = (nl == std::string_view::npos ? text.size() : nl) - pos;
    if (nl != std::string_view::npos && len > 0 && text[nl - 1] == '\r') --len;
    l = text.substr(pos, len);
    pos = nl == std::string_view::npos ? text.size() : nl + 1;
    return true;
  };
  if (!next() || l.rfind("#opad ", 0) != 0 || pos >= text.size()) fail(0, "not an OPAD document (expected '#opad <version>')");
  int fmt = std::atoi(std::string(l.substr(6)).c_str());
  if (fmt < 1 || fmt > kFormatVersion)
    fail(0, "unsupported format version " + std::to_string(fmt) + " (this build reads up to " +
                std::to_string(kFormatVersion) + ")");
  next();
  try {
    d.header = Header::from_json(json::parse(l));
  } catch (const json::exception& e) {
    fail(1, std::string("bad header: ") + e.what());
  }
  d.header.format = fmt;

  if (!next() || l != "#ops") fail(2, "expected '#ops'");
  bool bodies = false;
  while (next()) {
    if (l == "#bodies") { bodies = true; break; }
    if (l.empty()) continue;
    report(l);
    if (l.rfind("<<<<<<<", 0) == 0 || l.rfind("=======", 0) == 0 || l.rfind(">>>>>>>", 0) == 0)
      fail(i, "unresolved git conflict marker");
    if (l[0] == '#') continue;  // reserved for future section-level metadata; ignored
    Op o;
    // Count structural brackets outside strings once, rather than repeatedly parsing a growing record.
    std::string record(l);
    int depth = 0;
    bool quoted = false, escaped = false;
    auto scan = [&](std::string_view part) {
      for (char c : part) {
        if (escaped) { escaped = false; continue; }
        if (quoted && c == '\\') { escaped = true; continue; }
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted) {
          if (c == '{' || c == '[') ++depth;
          if (c == '}' || c == ']') --depth;
        }
      }
    };
    scan(l);
    while (depth > 0) {
      if (!next() || l == "#bodies") fail(i, "truncated op record");
      scan(l);
      record += '\n'; record += l;
    }
    try {
      o.data = json::parse(record);
      // An op type of a newer build is kept as it is (an opaque record: written back byte for byte, never applied, not
      // editable here); only its envelope is checked.
      if (o.data.is_object() && o.data.contains("op") && o.data["op"].is_string() && !known_type(o.data["op"].get<std::string>())) {
        if (o.data.contains("id")) require(o.data, "id", "uuid");
      } else {
        validate_op(o.data);
      }
    } catch (const json::exception& e) {
      fail(i, std::string("bad op: ") + e.what());
    } catch (const Error& e) {
      fail(i, std::string("bad op: ") + e.what());
    }
    if (!o.data.contains("id")) fail(i, "op has no id");
    o.id = o.data["id"].get<std::string>();
    o.type = o.data["op"].get<std::string>();
    o.raw = std::move(record);
    d.ops.push_back(std::move(o));
  }
  while (bodies && next()) {
    if (l.empty()) continue;
    report(l);
    if (l.rfind("#body ", 0) != 0) fail(i, "expected '#body <key> <lines> <meta>'");
    std::istringstream hs{std::string(l.substr(6))};
    std::string key;
    size_t n = 0;
    hs >> key >> n;
    std::string meta_text;
    std::getline(hs, meta_text);
    const size_t at = i;
    if (!hs || key.size() != 64 || n == 0 || key.find_first_not_of("0123456789abcdef") != std::string::npos)
      fail(at, "bad body header");
    // Its lines are only counted here (a hostile count runs into the end of the text, it allocates nothing).
    const size_t start = pos;
    for (size_t k = 0; k < n; ++k) {
      if (pos >= text.size()) fail(at, "truncated body entry");
      const size_t nl = text.find('\n', pos);
      pos = nl == std::string_view::npos ? text.size() : nl + 1;
    }
    i += n;
    if (skip_body && skip_body(key)) continue;
    BodyEntry e;
    e.key = key;
    try {
      e.meta = meta_text.empty() ? json::object() : json::parse(meta_text);
    } catch (const json::exception& ex) {
      fail(at, std::string("bad body meta: ") + ex.what());
    }
    if (!e.meta.is_object()) fail(at, "body metadata must be an object");
    const std::string_view brep = text.substr(start, pos - start);
    if (index) {
      e.indexed = brep;
    } else {
      if (brep.find('\r') == std::string_view::npos) e.brep.assign(brep);
      else
        for (size_t k = 0; k < brep.size(); ++k)
          if (brep[k] != '\r' || k + 1 >= brep.size() || brep[k + 1] != '\n') e.brep.push_back(brep[k]);
      if (e.brep.back() != '\n') e.brep.push_back('\n');
      if (sha256_hex(e.brep) != key) fail(at, "body entry content does not match its key (corrupted or edited)");
    }
    if (!d.bodies_index_.count(key)) {
      d.bodies_index_[key] = d.bodies_.size();
      d.bodies_.push_back(std::move(e));
    }
  }
  d.persisted_ops_ = d.ops.size();
  d.dirty = false;
  return d;
}

Document Document::load(const std::filesystem::path& p, const BodyFilter& skip_body, const Progress& progress) {
  if (!progress) {
    Document d = parse(read_text_file(p), p, skip_body);
    d.path = p;
    return d;
  }
  // Read in blocks into a string of the file's size (the first fifth of the progress), then parsed (the rest).
  std::ifstream in(p, std::ios::binary | std::ios::ate);
  if (!in) throw Error("cannot open file: " + path_to_utf8(p));
  const auto size = static_cast<size_t>(in.tellg());
  in.seekg(0);
  std::string text(size, '\0');
  constexpr size_t kBlock = 8u << 20;
  for (size_t at = 0; at < size; at += kBlock) {
    const size_t n = std::min(kBlock, size - at);
    if (!in.read(text.data() + at, static_cast<std::streamsize>(n))) throw Error("cannot read file: " + path_to_utf8(p));
    if (!progress(0.2 * static_cast<double>(at + n) / static_cast<double>(size))) throw Error("cancelled");
  }
  Document d = parse(text, p, skip_body, [&](double f) { return progress(0.2 + 0.8 * f); });
  d.path = p;
  return d;
}

Document Document::load_index(const std::filesystem::path& p, const BodyFilter& skip_body) {
  Document d = parse_index(read_text_file(p), p, skip_body);
  d.path = p;
  return d;
}

void Document::save() {
  if (path.empty()) throw Error("document has no path; use save_as");
  save_as(path);
}

void Document::save_as(const std::filesystem::path& p) {
  header.format = kFormatVersion;  // migrate on save (F9)
  for (auto& edit : asset_path_edits(*this, p.parent_path())) append(std::move(edit));  // saved elsewhere: linked paths follow
  rebase_asset_paths(*this, p.parent_path());
  write_text_file(p, serialize());
  path = p;
  persisted_ops_ = ops.size();
  dirty = false;
}

}  // namespace opad
