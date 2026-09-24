#include "opad/document.hpp"

#include <algorithm>
#include <set>
#include <sstream>

#include "opad/scene.hpp"

namespace opad {

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
                                             "feature",    "edit",        "regen"};
  return t;
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
  const auto& types = op_types();
  if (std::find(types.begin(), types.end(), type) == types.end()) throw Error("unknown op type: " + type);
  if (op.contains("id")) require(op, "id", "uuid");
  if (type == "import") {
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
        throw Error("annotation: style must be one of ok, warning, issue, note");
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
  } else if (type == "delete") {
    require(op, "target", "uuid");
  } else if (type == "param") {
    require(op, "name", "string");
    require(op, "expr", "string");
  } else if (type == "sketch") {
    require(op, "name", "string");
    if (!op.contains("plane") || !op["plane"].is_object()) throw Error("sketch: 'plane' must be an object");
    if (!op.contains("geometry") || !op["geometry"].is_object()) throw Error("sketch: 'geometry' must be an object");
  } else if (type == "feature") {
    require(op, "kind", "string");
    require(op, "name", "string");
    if (!op.contains("inputs") || !op["inputs"].is_object()) throw Error("feature: 'inputs' must be an object");
    if (op.contains("result") && !op["result"].is_object()) throw Error("feature: 'result' must be an object");
  } else if (type == "edit") {
    require(op, "target", "uuid");
    if (!op.contains("set") || !op["set"].is_object()) throw Error("edit: 'set' must be an object");
  } else if (type == "regen") {
    if (!op.contains("results") || !op["results"].is_object()) throw Error("regen: 'results' must be an object");
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
    out[it.key()] = it.value();
  }
  std::string id = out["id"].get<std::string>();
  if (find_op(id)) throw Error("duplicate op id: " + id);
  if ((out["op"] == "delete" || out["op"] == "edit") && !find_op(out["target"].get<std::string>()))
    throw Error(out["op"].get<std::string>() + ": target op not found: " + out["target"].get<std::string>());
  Op o;
  o.id = id;
  o.type = out["op"].get<std::string>();
  o.raw = out.dump();
  o.data = std::move(out);
  ops.push_back(std::move(o));
  dirty = true;
  return ops.back();
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
    if (b.brep.empty()) return true;
  return false;
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

std::vector<std::string> Document::gc() {
  std::set<std::string> live;
  // Feature results keep every intermediate state alive (the body before a fillet is what the fillet is
  // recomputed from); results an edit has superseded are not part of the effective log and go.
  for (const auto& o : effective_ops(*this)) {
    if (o.op->type == "import") collect_keys(o.data().value("nodes", json::array()), live);
    else if (o.op->type == "feature" && o.data().contains("result"))
      for (const auto& b : o.data()["result"].value("bodies", json::array())) live.insert(b.value("key", ""));
  }
  std::vector<std::string> removed;
  std::vector<BodyEntry> kept;
  for (auto& b : bodies_) {
    if (live.count(b.key)) kept.push_back(std::move(b));
    else removed.push_back(b.key);
  }
  if (!removed.empty()) {
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
// Expand only containers leading to sketch records. Each point/curve/constraint stays on one line.
bool sketch_records(const json& j) {
  if (!j.is_object()) return false;
  if (j.contains("points") || j.contains("entities") || j.contains("constraints") || j.contains("patterns")) return true;
  for (const auto& v : j) if (sketch_records(v)) return true;
  return false;
}
std::string record_text(const json& j, int depth = 0) {
  if (!sketch_records(j)) return j.dump();
  const std::string indent(size_t(depth + 1) * 2, ' ');
  std::string out = "{";
  bool first = true;
  for (const auto& [k,v] : j.items()) {
    if (!first) out += ',';
    first = false;
    out += '\n' + indent + json(k).dump() + ": ";
    if ((k == "points" || k == "entities" || k == "constraints" || k == "patterns") && v.is_array()) {
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
  for (const auto& b : bodies_) reserve += b.brep.size() + 128;
  for (const auto& o : ops) reserve += o.raw.size() + 1;
  out.reserve(reserve);
  out += "#opad ";
  out += std::to_string(kFormatVersion);
  out += '\n';
  out += header.to_json().dump();
  out += '\n';
  out += "#ops\n";
  for (const auto& o : ops) {
    out += sketch_records(o.data) ? record_text(o.data) : o.raw.empty() ? o.data.dump() : o.raw;
    out += '\n';
  }
  out += "#bodies\n";
  for (const auto& b : bodies_) {
    size_t lines = static_cast<size_t>(std::count(b.brep.begin(), b.brep.end(), '\n'));
    out += "#body ";
    out += b.key;
    out += ' ';
    out += std::to_string(lines);
    out += ' ';
    out += b.meta.dump();
    out += '\n';
    out += b.brep;
  }
  return out;
}

Document Document::parse(const std::string& text, const std::filesystem::path& origin) {
  Document d;
  d.path = origin;
  std::string where = origin.empty() ? std::string("<memory>") : origin.string();
  auto fail = [&](size_t line, const std::string& msg) {
    throw Error(where + ":" + std::to_string(line + 1) + ": " + msg);
  };

  // Split into lines without copying the body text more than once.
  std::vector<std::string_view> lines;
  {
    size_t start = 0;
    while (start <= text.size()) {
      size_t nl = text.find('\n', start);
      if (nl == std::string::npos) {
        if (start < text.size()) lines.emplace_back(text.data() + start, text.size() - start);
        break;
      }
      size_t len = nl - start;
      if (len > 0 && text[nl - 1] == '\r') --len;
      lines.emplace_back(text.data() + start, len);
      start = nl + 1;
    }
  }
  if (lines.size() < 2 || lines[0].rfind("#opad ", 0) != 0) fail(0, "not an OPAD document (expected '#opad <version>')");
  int fmt = std::atoi(std::string(lines[0].substr(6)).c_str());
  if (fmt < 1 || fmt > kFormatVersion)
    fail(0, "unsupported format version " + std::to_string(fmt) + " (this build reads up to " +
                std::to_string(kFormatVersion) + ")");
  try {
    d.header = Header::from_json(json::parse(lines[1]));
  } catch (const json::exception& e) {
    fail(1, std::string("bad header: ") + e.what());
  }
  d.header.format = fmt;

  size_t i = 2;
  if (i >= lines.size() || lines[i] != "#ops") fail(i, "expected '#ops'");
  ++i;
  for (; i < lines.size() && lines[i] != "#bodies"; ++i) {
    std::string_view l = lines[i];
    if (l.empty()) continue;
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
      if (++i >= lines.size() || lines[i] == "#bodies") fail(i, "truncated op record");
      scan(lines[i]);
      record += '\n'; record += lines[i];
    }
    try {
      o.data = json::parse(record);
      validate_op(o.data);
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
  if (i < lines.size() && lines[i] == "#bodies") {
    ++i;
    while (i < lines.size()) {
      std::string_view l = lines[i];
      if (l.empty()) { ++i; continue; }
      if (l.rfind("#body ", 0) != 0) fail(i, "expected '#body <key> <lines> <meta>'");
      std::istringstream hs{std::string(l.substr(6))};
      std::string key;
      size_t n = 0;
      hs >> key >> n;
      std::string meta_text;
      std::getline(hs, meta_text);
      if (!hs || key.size() != 64 || n == 0 || key.find_first_not_of("0123456789abcdef") != std::string::npos)
        fail(i, "bad body header");
      // Subtract before comparing: hostile counts must not overflow or trigger huge allocations.
      if (n > lines.size() - i - 1) fail(i, "truncated body entry");
      BodyEntry e;
      e.key = key;
      try {
        e.meta = meta_text.empty() ? json::object() : json::parse(meta_text);
      } catch (const json::exception& ex) {
        fail(i, std::string("bad body meta: ") + ex.what());
      }
      if (!e.meta.is_object()) fail(i, "body metadata must be an object");
      for (size_t k = 1; k <= n; ++k) {
        if (i + k >= lines.size()) fail(i, "truncated body entry");
        e.brep.append(lines[i + k]);
        e.brep.push_back('\n');
      }
      if (sha256_hex(e.brep) != key) fail(i, "body entry content does not match its key (corrupted or edited)");
      if (!d.bodies_index_.count(key)) {
        d.bodies_index_[key] = d.bodies_.size();
        d.bodies_.push_back(std::move(e));
      }
      i += n + 1;
    }
  }
  d.persisted_ops_ = d.ops.size();
  d.dirty = false;
  return d;
}

Document Document::load(const std::filesystem::path& p) {
  Document d = parse(read_text_file(p), p);
  d.path = p;
  return d;
}

void Document::save() {
  if (path.empty()) throw Error("document has no path; use save_as");
  save_as(path);
}

void Document::save_as(const std::filesystem::path& p) {
  header.format = kFormatVersion;  // migrate on save (F9)
  write_text_file(p, serialize());
  path = p;
  persisted_ops_ = ops.size();
  dirty = false;
}

}  // namespace opad
