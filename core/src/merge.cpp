#include "opad/merge.hpp"

#include <OSD_Parallel.hxx>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace opad {
namespace {
size_t record_hash(const Op& o) {
  return o.raw.empty() ? std::hash<std::string>{}(o.data.dump()) : std::hash<std::string_view>{}(o.raw);
}
bool design_type(const std::string& type) { return type == "param" || type == "sketch" || type == "feature" || type == "regen"; }
}  // namespace

Manifest Manifest::of(const Document& d) {
  Manifest m;
  m.uuid = d.header.uuid;
  m.header = d.header.to_json().dump();
  m.ops.reserve(d.ops.size());
  for (const auto& o : d.ops) m.ops.emplace_back(o.id, record_hash(o));
  for (const auto& b : d.bodies()) m.bodies.insert(b.key);
  return m;
}

const char* relation_name(Relation r) {
  switch (r) {
    case Relation::same: return "same";
    case Relation::extends: return "extends";
    case Relation::rewritten: return "rewritten";
    default: return "other";
  }
}

Relation relation(const Manifest& base, const Document& version) {
  if (version.header.uuid != base.uuid) return Relation::other;
  if (version.header.to_json().dump() != base.header) return Relation::rewritten;
  std::unordered_set<std::string_view> ids;
  for (const auto& [id, hash] : base.ops) ids.insert(id);
  size_t next = 0;
  for (const auto& o : version.ops) {
    if (!ids.count(o.id)) continue;  // new
    if (next >= base.ops.size() || o.id != base.ops[next].first || record_hash(o) != base.ops[next].second) return Relation::rewritten;
    ++next;
  }
  if (next < base.ops.size()) return Relation::rewritten;
  return version.ops.size() == base.ops.size() ? Relation::same : Relation::extends;
}

json MergeConflict::to_json() const { return {{"target", target}, {"field", field}, {"ours", ours}, {"theirs", theirs}}; }

std::vector<std::pair<std::string, std::string>> op_effects(const json& op) {
  std::vector<std::pair<std::string, std::string>> out;
  const std::string kind = op.contains("op") && op["op"].is_string() ? op["op"].get<std::string>() : std::string();
  const std::string target = op.contains("target") && op["target"].is_string() ? op["target"].get<std::string>() : std::string();
  if (kind == "regen") {
    if (op.contains("results") && op["results"].is_object())
      for (const auto& [key, value] : op["results"].items()) out.emplace_back(key, "geometry");
  } else if (kind == "param") {
    out.emplace_back("parameter:" + op.value("name", ""), "*");
  } else if (kind == "edit") {
    if (op.contains("set") && op["set"].is_object())
      for (const auto& [key, value] : op["set"].items())
        out.emplace_back(target, key == "geometry_delta" || key == "geometry" || key == "result" || key == "inputs" || key == "plane" ? "geometry" : key);
  } else if (kind == "delete") {
    out.emplace_back(target, "*");
  } else if (!target.empty()) {
    for (const auto& [key, value] : op.items())
      if (key != "op" && key != "id" && key != "ts" && key != "by" && key != "target") out.emplace_back(target, key);
  }
  return out;
}

json op_effect(const json& op, const std::string& target, const std::string& field) {
  const json kind = op.contains("op") ? op["op"] : json();
  const std::string type = kind.is_string() ? kind.get<std::string>() : std::string();
  if (type == "delete") return json::array({kind, nullptr});
  if (type == "param") {
    json rest = op;
    for (const char* k : {"id", "ts", "by"}) rest.erase(k);
    return json::array({kind, rest});
  }
  if (type == "regen") return json::array({kind, op.contains("results") && op["results"].is_object() ? op["results"].value(target, json()) : json()});
  if (type == "edit") {
    json values = json::object();
    if (op.contains("set") && op["set"].is_object())
      for (const auto& [key, value] : op["set"].items())
        if ((key == "geometry_delta" || key == "geometry" || key == "result" || key == "inputs" || key == "plane" ? "geometry" : key) == field) values[key] = value;
    return json::array({kind, values});
  }
  return json::array({kind, op.contains(field) ? op[field] : json()});
}

bool same_effect(const json& a, const std::string& fieldA, const json& b, const std::string& fieldB, const std::string& target) {
  return fieldA == fieldB && op_effect(a, target, fieldA) == op_effect(b, target, fieldB);
}

MergePlan plan_merge(const Manifest& base, const Document& ours, const Document& theirs) {
  MergePlan p;
  p.theirs = relation(base, theirs);
  if (p.theirs == Relation::other) { p.error = "the file is another document"; return p; }
  if (p.theirs == Relation::rewritten) { p.error = "the history in the file was rewritten"; return p; }
  // A session only appends to what it read or wrote, or undoes: undone past that, its log no longer continues the base.
  const size_t n = base.ops.size();
  bool continues = ours.header.uuid == base.uuid && ours.ops.size() >= n;
  for (size_t i = 0; continues && i < n; ++i) continues = ours.ops[i].id == base.ops[i].first;
  if (!continues) { p.error = "changes saved before were undone"; return p; }
  p.appended = theirs.ops.size() >= n;
  for (size_t i = 0; p.appended && i < n; ++i) p.appended = theirs.ops[i].id == base.ops[i].first;
  std::unordered_map<std::string_view, size_t> fresh;  // theirs' ops the base lacks
  std::unordered_set<std::string_view> baseIds;
  for (const auto& [id, hash] : base.ops) baseIds.insert(id);
  for (size_t i = 0; i < theirs.ops.size(); ++i)
    if (!baseIds.count(theirs.ops[i].id)) fresh.emplace(theirs.ops[i].id, i);
  std::unordered_set<size_t> both;
  for (size_t i = n; i < ours.ops.size(); ++i) {
    const auto it = fresh.find(ours.ops[i].id);
    if (it == fresh.end()) { p.mine.push_back(i); continue; }
    if (theirs.ops[it->second].raw != ours.ops[i].raw) { p.error = "both versions hold a different operation " + ours.ops[i].id; return p; }
    p.shared = true;
    both.insert(it->second);
  }
  p.incoming = fresh.size() - both.size();
  // Conflicts: what ours' unsaved ops and theirs' new ones both change.
  std::map<std::string, std::vector<std::pair<std::string, const Op*>>> changed;  // target -> (field, theirs op)
  std::unordered_map<std::string_view, const std::string*> types;
  for (const auto& o : theirs.ops) types.emplace(o.id, &o.type);
  for (size_t i : p.mine) types.emplace(ours.ops[i].id, &ours.ops[i].type);
  auto design = [&](const Op& o) {
    if (design_type(o.type)) return true;
    if (o.type != "edit" && o.type != "delete") return false;
    const auto it = types.find(o.data.value("target", ""));
    return it != types.end() && design_type(*it->second);
  };
  bool theirsDesign = false, oursDesign = false;
  for (size_t i = 0; i < theirs.ops.size(); ++i) {
    if (baseIds.count(theirs.ops[i].id) || both.count(i)) continue;
    theirsDesign = theirsDesign || design(theirs.ops[i]);
    for (auto& [target, field] : op_effects(theirs.ops[i].data)) changed[target].emplace_back(field, &theirs.ops[i]);
  }
  std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
  for (size_t i : p.mine) {
    oursDesign = oursDesign || design(ours.ops[i]);
    for (const auto& [target, field] : op_effects(ours.ops[i].data)) {
      const auto it = changed.find(target);
      if (it == changed.end()) continue;
      for (const auto& [other, op] : it->second)
        if ((field == other || field == "*" || other == "*") && !same_effect(ours.ops[i].data, field, op->data, other, target)) {
          MergeConflict c{target, field == "*" ? other : field, ours.ops[i].id, op->id};
          if (seen.emplace(c.target, c.field, c.ours, c.theirs).second) p.conflicts.push_back(std::move(c));
        }
    }
  }
  p.design = theirsDesign && oursDesign;
  return p;
}

void apply_merge(Document& ours, Document& theirs, const MergePlan& plan, const std::vector<std::string>& theirs_bodies) {
  if (!plan.error.empty()) throw Error("cannot merge: " + plan.error);
  for (size_t i : plan.mine)
    if (i >= ours.ops.size()) throw Error("cannot merge: the plan is out of date");
  ours.arrange_bodies(theirs_bodies, theirs, true);
  std::vector<Op> log = std::move(theirs.ops);
  theirs.ops.clear();
  log.reserve(log.size() + plan.mine.size());
  for (size_t i : plan.mine) log.push_back(std::move(ours.ops[i]));
  ours.ops = std::move(log);
  ours.dirty = true;
}

bool changes_design(const Document& base, const Document& version) {
  std::unordered_set<std::string_view> old;
  for (const auto& o : base.ops) old.insert(o.id);
  std::unordered_map<std::string_view, const std::string*> types;
  for (const auto& o : version.ops) types.emplace(o.id, &o.type);
  for (const auto& o : version.ops) {
    if (old.count(o.id)) continue;
    if (design_type(o.type)) return true;
    if (o.type != "edit" && o.type != "delete") continue;
    const auto it = types.find(o.data.value("target", ""));
    if (it != types.end() && design_type(*it->second)) return true;
  }
  return false;
}

RestorePlan plan_restore(const Document& current, const Document& version) {
  RestorePlan p;
  if (version.header.uuid != current.header.uuid) {
    p.problem = RestorePlan::Problem::other_document;
    return p;
  }
  std::unordered_map<std::string_view, const Op*> at;
  for (const auto& o : current.ops) at.emplace(o.id, &o);
  std::unordered_set<std::string_view> kept;
  for (const auto& o : version.ops) {
    const auto it = at.find(o.id);
    const bool same = it != at.end() && (!o.raw.empty() && !it->second->raw.empty() ? o.raw == it->second->raw : o.data == it->second->data);
    if (!same) {
      p.problem = RestorePlan::Problem::not_ancestor;
      p.op = o.id;
      return p;
    }
    kept.insert(o.id);
  }
  std::unordered_set<std::string_view> later;
  for (const auto& o : current.ops)
    if (!kept.count(o.id)) later.insert(o.id);
  p.later = later.size();
  if (!p.later) {
    p.problem = RestorePlan::Problem::current;
    return p;
  }
  for (const auto& o : current.ops) {
    if (!later.count(o.id)) continue;
    if ((o.type == "delete" || o.type == "edit") && later.count(o.data.value("target", ""))) continue;  // gone with its target
    p.tombstones.push_back(o.id);
  }
  for (const auto& b : version.bodies())
    if (!current.has_body(b.key)) p.bodies.push_back(b.key);
  return p;
}

void apply_restore(Document& current, Document& version, const RestorePlan& plan, const std::string& author) {
  if (plan.problem != RestorePlan::Problem::none) throw Error("cannot restore this version as new changes");
  if (!plan.bodies.empty()) {
    std::vector<std::string> keys = current.body_keys();
    keys.insert(keys.end(), plan.bodies.begin(), plan.bodies.end());
    current.arrange_bodies(keys, version, true);
  }
  for (const auto& id : plan.tombstones) current.append(json{{"op", "delete"}, {"target", id}}, author);
  current.dirty = true;
}

// ---------------------------------------------------------------- the git driver (tools/opad_merge.py, rule for rule)
namespace {
// Python's str.strip(): ASCII whitespace, the separators 0x1c-0x1f and the Unicode spaces (the text is valid UTF-8).
std::string_view py_strip(std::string_view s) {
  static constexpr std::string_view wide[] = {
      "\xc2\x85", "\xc2\xa0", "\xe1\x9a\x80", "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83",
      "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87", "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a",
      "\xe2\x80\xa8", "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80"};
  auto space = [](std::string_view t, bool front) -> size_t {
    if (t.empty()) return 0;
    const unsigned char c = static_cast<unsigned char>(front ? t.front() : t.back());
    if (c == ' ' || (c >= '\t' && c <= '\r') || (c >= 0x1c && c <= 0x1f)) return 1;
    for (std::string_view w : wide)
      if (t.size() >= w.size() && (front ? t.substr(0, w.size()) : t.substr(t.size() - w.size())) == w) return w.size();
    return 0;
  };
  while (const size_t n = space(s, true)) s.remove_prefix(n);
  while (const size_t n = space(s, false)) s.remove_suffix(n);
  return s;
}

// Python's int() of a body's line count: spaces around, a sign, digits with single underscores between them.
bool py_int(std::string_view s, long long& value) {
  s = py_strip(s);
  const bool negative = !s.empty() && s[0] == '-';
  if (!s.empty() && (s[0] == '-' || s[0] == '+')) s.remove_prefix(1);
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  if (s.empty() || !digit(s.front()) || !digit(s.back())) return false;
  value = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '_' && digit(s[i + 1])) continue;
    if (!digit(s[i])) return false;
    if (value < 1LL << 56) value = value * 10 + (s[i] - '0');  // past any file's line count: stays too long
  }
  if (negative) value = -value;
  return true;
}

bool valid_utf8(std::string_view s) {  // as Python decodes it: no overlong forms, surrogates or code points past U+10FFFF
  const auto* p = reinterpret_cast<const unsigned char*>(s.data());
  const auto* const e = p + s.size();
  while (p < e) {
    if (e - p >= 8) {
      uint64_t w;
      std::memcpy(&w, p, 8);
      if (!(w & 0x8080808080808080ull)) { p += 8; continue; }
    }
    const unsigned c = *p;
    if (c < 0x80) { ++p; continue; }
    const int n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
    if (!n || e - p <= n) return false;
    unsigned cp = c & (0x3fu >> n);
    for (int k = 1; k <= n; ++k) {
      if ((p[k] & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (p[k] & 0x3fu);
    }
    if ((n == 2 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) || (n == 3 && (cp < 0x10000 || cp > 0x10ffff))) return false;
    p += n + 1;
  }
  return true;
}

void universal_newlines(std::string& text) {  // Python's text mode: CRLF and CR read as LF
  if (text.find('\r') == std::string::npos) return;
  size_t w = 0;
  for (size_t r = 0; r < text.size(); ++r) {
    if (text[r] != '\r') text[w++] = text[r];
    else if (r + 1 >= text.size() || text[r + 1] != '\n') text[w++] = '\n';
  }
  text.resize(w);
}

json py_json(std::string_view s) {  // json.loads: also refuses a byte order mark, which nlohmann skips
  if (s.substr(0, 3) == "\xef\xbb\xbf") throw Error("unexpected UTF-8 BOM");
  return json::parse(s.begin(), s.end());
}

// Python's == on decoded JSON: objects ignore key order, numbers compare by value, true == 1 and false == 0.
bool py_equal(const json& a, const json& b) {
  if ((a.is_number() || a.is_boolean()) && (b.is_number() || b.is_boolean())) {
    if (a.is_boolean() == b.is_boolean()) return a == b;
    const json& number = a.is_boolean() ? b : a;
    return number.get<double>() == ((a.is_boolean() ? a : b).get<bool>() ? 1.0 : 0.0);
  }
  if (a.type() != b.type()) return false;
  if (a.is_object()) {
    if (a.size() != b.size()) return false;
    for (auto it = a.begin(); it != a.end(); ++it) {
      const auto other = b.find(it.key());
      if (other == b.end() || !py_equal(it.value(), *other)) return false;
    }
    return true;
  }
  if (a.is_array()) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (!py_equal(a[i], b[i])) return false;
    return true;
  }
  return a == b;
}

struct Lines {  // a cursor over the text's lines, each with its '\n' (the last may have none)
  std::string_view text;
  size_t pos = 0;
  bool more() const { return pos < text.size(); }
  std::string_view next() {
    const size_t nl = text.find('\n', pos);
    const size_t end = nl == std::string_view::npos ? text.size() : nl + 1;
    const std::string_view line = text.substr(pos, end - pos);
    pos = end;
    return line;
  }
};
std::string_view chomp(std::string_view s) {
  while (!s.empty() && s.back() == '\n') s.remove_suffix(1);
  return s;
}

struct Record {
  std::string id;
  std::string_view raw;  // as written, without its last '\n'
  json data;
};
bool same(const Record& a, const Record& b) { return a.raw == b.raw || py_equal(a.data, b.data); }

struct Version {
  int format = 0;
  json header;
  std::string_view head;                       // the header and '#ops' lines as written
  std::vector<Record> ops;                     // each id once, where it first appears
  std::unordered_map<std::string, size_t> at;  // id -> index in ops
  std::vector<std::pair<std::string_view, std::string_view>> bodies;  // key, entry ('#body' line and text), each key once
  std::unordered_set<std::string_view> keys;
};

// Body texts hashed so far, by key: the same text met again in another version is not hashed again.
using Verified = std::unordered_map<std::string_view, std::string_view>;

// One version as the driver reads it, refusing what it refuses in the order it does.
Version read_version(std::string_view text, Verified& verified) {
  if (!valid_utf8(text)) throw Error("not UTF-8 text");
  Version v;
  Lines lines{text};
  const std::string_view first = lines.next(), header = lines.next(), ops = lines.next(), format = py_strip(first);
  if (!lines.more() || (format != "#opad 1" && format != "#opad 2") || py_strip(ops) != "#ops") throw Error("unsupported OPAD header");
  v.format = format.back() - '0';
  v.header = py_json(header);
  v.head = text.substr(first.size(), header.size() + ops.size());
  for (;;) {
    if (!lines.more()) throw Error("missing body store");
    const size_t start = lines.pos;
    std::string_view l = lines.next();
    const std::string_view s = py_strip(l);
    if (s == "#bodies") break;
    if (s.empty() || l[0] == '#') throw Error("unexpected metadata in operation log");  // never dropped silently
    int depth = 0;
    bool quoted = false, escaped = false;
    for (;;) {
      for (const char c : l) {
        if (escaped) escaped = false;
        else if (quoted && c == '\\') escaped = true;
        else if (c == '"') quoted = !quoted;
        else if (!quoted) depth += (c == '{' || c == '[') - (c == '}' || c == ']');
      }
      if (depth <= 0 || !lines.more()) break;
      l = lines.next();
    }
    Record r;
    r.raw = chomp(text.substr(start, lines.pos - start));
    r.data = py_json(r.raw);
    const auto id = r.data.is_object() ? r.data.find("id") : r.data.end();
    if (id == r.data.end() || !id->is_string()) throw Error("operation requires an ID");
    r.id = id->get<std::string>();
    if (const auto [it, fresh] = v.at.emplace(r.id, v.ops.size()); !fresh) {
      if (!same(v.ops[it->second], r)) throw Error("conflicting operation ID " + r.id);
      continue;
    }
    v.ops.push_back(std::move(r));
  }
  // Bodies: read up to the first malformed entry, then the hashes of those read (in parallel) before that error.
  std::vector<std::pair<std::string_view, std::string_view>> hash;  // key, text
  Verified pending;
  std::string failure;
  while (lines.more()) {
    const size_t start = lines.pos;
    const std::string_view l = lines.next();
    if (py_strip(l).empty()) continue;
    std::string_view rest = chomp(l), fields[4];
    size_t n = 0;
    for (size_t sp; n < 3 && (sp = rest.find(' ')) != std::string_view::npos; rest.remove_prefix(sp + 1)) fields[n++] = rest.substr(0, sp);
    fields[n++] = rest;
    if (n != 4 || fields[0] != "#body") { failure = "invalid body header"; break; }
    long long count = 0;
    if (!py_int(fields[2], count)) { failure = "invalid body length"; break; }
    Lines end = lines;
    long long got = 0;
    for (; got < count && end.more(); ++got) end.next();
    if (count < 1 || got < count) { failure = "invalid body length"; break; }
    try {
      py_json(fields[3]);
    } catch (const std::exception& e) {
      failure = e.what();
      break;
    }
    const std::string_view key = fields[1], body = text.substr(lines.pos, end.pos - lines.pos);
    const auto known = verified.find(key);
    if (known == verified.end() || known->second != body) {
      const auto [it, fresh] = pending.emplace(key, body);
      if (fresh || it->second != body) hash.emplace_back(key, body);
    }
    if (v.keys.insert(key).second) v.bodies.emplace_back(key, text.substr(start, end.pos - start));
    lines = end;
  }
  std::vector<char> ok(hash.size());
  OSD_Parallel::For(0, int(hash.size()), [&](int i) { ok[size_t(i)] = sha256_hex(hash[size_t(i)].second) == hash[size_t(i)].first; });
  if (std::find(ok.begin(), ok.end(), 0) != ok.end()) throw Error("body hash mismatch");
  for (const auto& [key, body] : hash) verified.emplace(key, body);
  if (!failure.empty()) throw Error(failure);
  return v;
}

void write_merge(const std::filesystem::path& p, const FileMerge& m) {  // as write_text_file: a whole new file or none
  std::filesystem::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    m.write(out);
    if (!out.flush()) throw Error("cannot write file: " + tmp.string());
  }
  std::error_code ec;
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    std::filesystem::remove(p, ec);
    std::filesystem::rename(tmp, p, ec);
    if (ec) throw Error("cannot replace file: " + p.string() + ": " + ec.message());
  }
}
}  // namespace

std::string FileMerge::text() const {
  std::string out;
  size_t size = 0;
  for (const auto& p : pieces) size += p.size();
  out.reserve(size);
  for (const auto& p : pieces) out += p;
  return out;
}

void FileMerge::write(std::ostream& out) const {
  for (const auto& p : pieces) out.write(p.data(), std::streamsize(p.size()));
}

FileMerge merge_files(std::string base, std::string ours, std::string theirs, bool keep_conflicts) {
  FileMerge m;
  for (std::string* text : {&base, &ours, &theirs}) universal_newlines(*text);
  const auto o = std::make_shared<const std::string>(std::move(ours)), t = std::make_shared<const std::string>(std::move(theirs));
  try {
    Verified verified;
    const Version vb = read_version(base, verified), vo = read_version(*o, verified), vt = read_version(*t, verified);
    if (!py_equal(vb.header, vo.header) || !py_equal(vb.header, vt.header)) throw Error("document header changed; manual review required");
    for (const Version* v : {&vo, &vt}) {
      for (const auto& r : vb.ops)
        if (const auto it = v->at.find(r.id); it == v->at.end() || !same(r, v->ops[it->second]))
          throw Error("history was rewritten; manual review required");
      for (const auto& key : vb.keys)
        if (!v->keys.count(key)) throw Error("body store was pruned; manual review required");
      size_t next = 0;  // operation order is what regeneration and tombstones depend on
      for (const auto& r : v->ops)
        if (vb.at.count(r.id) && vb.ops[next++].id != r.id) throw Error("operation history was reordered");
    }
    for (const auto& r : vo.ops)
      if (const auto it = vt.at.find(r.id); it != vt.at.end() && !vb.at.count(r.id) && !same(r, vt.ops[it->second]))
        throw Error("conflicting operation ID " + r.id);
    // What the new ops of one side only change, against the other's.
    std::map<std::string, std::vector<std::pair<std::string, const Record*>>> changed;  // target -> field, theirs' op
    for (const auto& r : vt.ops)
      if (!vb.at.count(r.id) && !vo.at.count(r.id))
        for (auto& [target, field] : op_effects(r.data)) changed[target].emplace_back(std::move(field), &r);
    std::string concurrent;
    std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
    for (const auto& r : vo.ops) {
      if (vb.at.count(r.id) || vt.at.count(r.id)) continue;
      for (const auto& [target, field] : op_effects(r.data)) {
        const auto it = changed.find(target);
        if (it == changed.end()) continue;
        for (const auto& [other, op] : it->second)
          if ((field == other || field == "*" || other == "*") && !same_effect(r.data, field, op->data, other, target)) {
            if (concurrent.empty()) concurrent = "concurrent changes to " + target + "/" + field + "; manual review required";
            MergeConflict c{target, field == "*" ? other : field, r.id, op->id};
            if (seen.emplace(c.target, c.field, c.ours, c.theirs).second) m.conflicts.push_back(std::move(c));
          }
      }
    }
    if (!concurrent.empty() && !keep_conflicts) throw Error(concurrent);
    std::vector<const Record*> merged;
    merged.reserve(vo.ops.size() + vt.ops.size());
    for (const auto& r : vo.ops) merged.push_back(&r);
    for (const auto& r : vt.ops) {
      const auto it = vo.at.find(r.id);
      if (it == vo.at.end()) merged.push_back(&r);
      else if (!same(vo.ops[it->second], r)) throw Error("conflicting operation ID " + r.id);
    }
    size_t next = 0;
    for (const Record* r : merged)
      if (vt.at.count(r->id) && vt.ops[next++].id != r->id) throw Error("branch operation order conflicts; manual review required");
    static constexpr std::string_view format[] = {"#opad 1\n", "#opad 2\n"};
    m.pieces.push_back(format[std::max({vb.format, vo.format, vt.format}) - 1]);
    m.pieces.push_back(vo.head);
    for (const Record* r : merged) {
      m.pieces.push_back(r->raw);
      m.pieces.push_back("\n");
    }
    m.pieces.push_back("#bodies\n");
    for (const auto& [key, entry] : vo.bodies) m.pieces.push_back(entry);
    for (const auto& [key, entry] : vt.bodies)
      if (!vo.keys.count(key)) m.pieces.push_back(entry);
    m.texts = {o, t};
  } catch (const std::exception& e) {
    m.error = e.what();
    m.pieces.clear();
  }
  return m;
}

std::string resolve_merge(std::string merged, const std::vector<MergeConflict>& conflicts, const std::vector<bool>& mine, const std::string& author) {
  Document d = Document::parse_index(std::move(merged));
  std::set<std::string> added;
  auto tombstone = [&](std::string op) {
    if (added.insert("delete " + op).second) d.append(json{{"op", "delete"}, {"target", std::move(op)}}, author);
  };
  for (size_t i = 0; i < conflicts.size(); ++i) {
    const MergeConflict& c = conflicts[i];
    const Op* ours = d.find_op(c.ours);
    const Op* theirs = d.find_op(c.theirs);
    if (!ours || !theirs) throw Error("the merge holds no operation " + (ours ? c.theirs : c.ours));
    const bool oursDelete = ours->type == "delete", theirsDelete = theirs->type == "delete";
    if (i < mine.size() && mine[i]) {
      if (theirsDelete) tombstone(theirs->id);
      else if (!oursDelete && added.insert("copy " + ours->id).second) {
        json copy = ours->data;  // before append: it moves the log
        for (const char* k : {"id", "ts", "by"}) copy.erase(k);
        d.append(std::move(copy), author);
      }
    } else if (oursDelete && !theirsDelete) {
      tombstone(ours->id);
    }
  }
  return d.serialize();
}

int merge_driver(const std::vector<std::filesystem::path>& args) {
  try {
    if (args.size() != 3 && args.size() != 4) throw Error("usage: merge-driver <base> <ours> <theirs> [<name>] (git's %O %A %B %P)");
    std::string base = read_text_file(args[0]), ours = read_text_file(args[1]), theirs = read_text_file(args[2]);
    const FileMerge m = merge_files(std::move(base), std::move(ours), std::move(theirs));
    if (!m.error.empty()) throw Error(m.error);
    write_merge(args[1], m);
    return 0;
  } catch (const std::exception& e) {
    const auto name = args.size() == 4 ? args[3].u8string() : std::u8string();
    const std::string in = name.empty() ? std::string() : " in " + std::string(reinterpret_cast<const char*>(name.data()), name.size());
    std::fprintf(stderr, "OPAD merge conflict%s: %s\n", in.c_str(), e.what());
    return 1;
  }
}

}  // namespace opad
