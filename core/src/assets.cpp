// Linked assets (assets.hpp): imports by reference, read from their files on open, synced by an edit of their import op.
#include "opad/assets.hpp"

#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <map>
#include <set>

#include "import_common.hpp"
#include "opad/cache.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/kicad_pcb.hpp"
#include "opad/scene.hpp"

namespace opad {
namespace {

namespace fs = std::filesystem;
constexpr const char* kKeyDomain = "opad-asset/1|";  // a derived key is never the hash of a body's BREP text

std::string utf8(const fs::path& p) {
  const auto u = p.generic_u8string();
  return std::string(u.begin(), u.end());
}

std::string lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string kind_of(const fs::path& file) {
  const std::string ext = lower(file.extension().string());
  if (ext == ".step" || ext == ".stp") return "step";
  if (ext == ".iges" || ext == ".igs") return "iges";
  if (ext == ".brep" || ext == ".brp") return "brep";
  if (ext == ".dxf" || ext == ".dwg" || ext == ".svg") return "drawing";
  if (ext == ".kicad_pcb") return "kicad_pcb";
  return "mesh";
}

// A UNC path (\\server\share): opening it can hand the user's credentials to that server, so a document never makes OPAD
// look at one by itself.
bool network(const fs::path& p) {
#ifdef _WIN32
  const std::wstring& s = p.native();
  auto sep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
  if (s.size() < 2 || !sep(s[0]) || !sep(s[1])) return false;
  if (s.size() >= 4 && s[2] == L'?' && sep(s[3]))  // \\?\C:\ is a local path written long; \\?\UNC\ is not
    return s.size() >= 8 && std::towupper(s[4]) == L'U' && std::towupper(s[5]) == L'N' && std::towupper(s[6]) == L'C' && sep(s[7]);
  return true;
#else
  (void)p;
  return false;
#endif
}

// Whether `file` lies in `root`: as written when `lexical` (nothing on disk is touched), else with links resolved.
bool inside(const fs::path& file, const fs::path& root, bool lexical) {
  if (root.empty()) return false;
  auto norm = [lexical](const fs::path& p) {
    std::error_code ec;
    const fs::path a = fs::absolute(p, ec).lexically_normal();
    if (lexical) return a;
    const fs::path c = fs::weakly_canonical(a, ec);
    return ec ? a : c;
  };
  const fs::path f = norm(file), r = norm(root);
  auto fi = f.begin();
  for (auto ri = r.begin(); ri != r.end(); ++ri) {
    if (ri->empty()) continue;  // a trailing separator
    if (fi == f.end()) return false;
#ifdef _WIN32
    if (lower(utf8(*fi)) != lower(utf8(*ri))) return false;
#else
    if (*fi != *ri) return false;
#endif
    ++fi;
  }
  return true;
}

// The git work tree holding `dir`, else `dir`: a board in ../hw beside the design is part of the project.
fs::path project_root(const fs::path& dir) {
  std::error_code ec;
  for (fs::path p = dir; !p.empty(); p = p.parent_path()) {
    if (fs::exists(p / ".git", ec)) return p;
    if (p == p.parent_path()) break;
  }
  return dir;
}

fs::path doc_dir(const Document& doc) { return doc.path.empty() ? fs::path() : fs::absolute(doc.path).parent_path(); }

// `file` relative to `base` with forward slashes; empty when there is no such path (another drive).
std::string relative_to(fs::path file, const fs::path& base) {
  if (base.empty()) return {};
  if (lower(utf8(file.root_name())) != lower(utf8(base.root_name()))) return {};
  fs::path f = base.root_name();  // the drive letter as the base spells it
  f += file.root_directory();
  f += file.relative_path();
  const fs::path rel = f.lexically_normal().lexically_relative(base.lexically_normal());
  return rel.empty() ? std::string() : utf8(rel);
}

std::string place_id(const std::string& op, const std::string& place) {
  std::string h = sha256_hex("opad-asset-node|" + op + "|" + place);
  h[12] = '5';
  h[16] = "89ab"[std::stoi(h.substr(16, 1), nullptr, 16) & 3];
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

// Each sibling's place: a root by position (the file's own name may change), a KiCad footprint by its uuid (else its
// reference), anything else by type and name, numbered among siblings of the same name.
std::vector<std::string> places(const json& nodes, bool roots) {
  std::vector<std::string> out;
  std::map<std::string, int> seen;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const json& n = nodes[i];
    const json kicad = n.value("kicad", json());
    std::string part;
    if (roots) part = "#" + std::to_string(i);
    else if (kicad.is_object() && !kicad.value("uuid", "").empty()) part = "fp:" + kicad.value("uuid", "");
    else if (kicad.is_object() && !kicad.value("ref", "").empty()) part = "ref:" + kicad.value("ref", "");
    else part = n.value("type", "") + ":" + n.value("name", "");
    if (const int k = seen[part]++; k) part += "~" + std::to_string(k);
    out.push_back(part);
  }
  return out;
}

// Node ids for a read of the file, from the import op and each node's place, so a part keeps its id (and every rename,
// colour, placement and reference made to it) when the file is read again. With `was`, the import's nodes as last synced,
// a node takes the id of the one at its place; siblings whose names all changed (a writer numbering its parts anew) pair
// up in order when as many of a type are left on both sides.
void relabel(json& nodes, const std::string& op, const std::string& parent, const json* was = nullptr) {
  const bool roots = parent.empty();
  const std::vector<std::string> now = places(nodes, roots);
  std::vector<int> match(nodes.size(), -1);
  if (was && was->is_array()) {
    const std::vector<std::string> then = places(*was, roots);
    std::vector<bool> taken(then.size());
    for (size_t i = 0; i < now.size(); ++i)
      if (const auto it = std::find(then.begin(), then.end(), now[i]); it != then.end()) {
        match[i] = static_cast<int>(it - then.begin());
        taken[size_t(match[i])] = true;
      }
    // Only parts known by name: a KiCad footprint gone and another come are two footprints.
    auto named = [](const std::string& place) { return place.rfind("fp:", 0) != 0 && place.rfind("ref:", 0) != 0; };
    for (const char* type : {"body", "component"}) {
      std::vector<size_t> left_now, left_then;
      for (size_t i = 0; i < now.size(); ++i)
        if (match[i] < 0 && named(now[i]) && nodes[i].value("type", "") == type) left_now.push_back(i);
      for (size_t i = 0; i < then.size(); ++i)
        if (!taken[i] && named(then[i]) && (*was)[i].value("type", "") == type) left_then.push_back(i);
      if (!left_now.empty() && left_now.size() == left_then.size())
        for (size_t k = 0; k < left_now.size(); ++k) match[left_now[k]] = static_cast<int>(left_then[k]);
    }
  }
  for (size_t i = 0; i < nodes.size(); ++i) {
    json& n = nodes[i];
    const json* before = match[i] >= 0 ? &(*was)[size_t(match[i])] : nullptr;
    n["id"] = before && before->contains("id") ? (*before)["id"] : json(place_id(op, parent + "/" + now[i]));
    if (n.contains("children")) relabel(n["children"], op, n["id"].get<std::string>(), before && before->contains("children") ? &(*before)["children"] : nullptr);
  }
}

template <class J, class F>
void each_node(J& nodes, const F& fn) {
  if (!nodes.is_array()) return;
  for (auto& n : nodes) {
    fn(n);
    if (n.contains("children")) each_node(n["children"], fn);
  }
}
template <class J, class F>
void each_body(J& nodes, const F& fn) {
  each_node(nodes, [&](auto& n) {
    if (n.value("type", "") == "body") fn(n);
  });
}

const json& nodes_of(const json& data) {
  static const json none = json::array();
  return data.contains("nodes") ? data["nodes"] : none;
}

std::vector<EffectiveOp> asset_imports(const Document& doc) {
  std::vector<EffectiveOp> out;
  for (auto& e : effective_ops(doc))
    if (e.op->type == "import" && e.data().contains("asset") && e.data()["asset"].is_object()) out.push_back(std::move(e));
  return out;
}

EffectiveOp find_asset(const Document& doc, const std::string& id) {
  std::vector<EffectiveOp> all = asset_imports(doc);
  if (id.empty()) {
    if (all.size() != 1) throw Error(all.empty() ? "the document links no file" : "the document links several files: name the import");
    return all[0];
  }
  for (auto& e : all)
    if (e.op->id == id) return e;
  throw Error("no linked file is imported by " + id);
}

// How the file was read, as the asset records it (a KiCad board's options and frame, a drawing's placement).
json builder_options(const std::string& kind, const ImportOptions& opt) {
  json o = json::object();
  if (kind == "kicad_pcb")
    o = {{"components", opt.kicad.components}, {"dnp", opt.kicad.dnp}, {"vias", opt.kicad.vias},
         {"placeholder_height", opt.kicad.placeholder_height}, {"origin", opt.kicad.origin}};
  if (opt.center_drawing) o["center"] = true;
  if (!opt.placement.is_identity()) o["placement"] = opt.placement.to_json();
  return o;
}

ImportOptions read_options(const json& asset, const AssetOptions& opt) {
  ImportOptions o;
  o.viewer = true;  // as the viewer reads files: nothing is prepared for the body store
  o.heal = false;
  o.progress = opt.progress;
  o.kicad = opt.kicad;
  const json b = asset.value("builder", json::object()).value("options", json::object());
  o.kicad.components = b.value("components", true);
  o.kicad.dnp = b.value("dnp", true);
  o.kicad.vias = b.value("vias", false);
  o.kicad.placeholder_height = b.value("placeholder_height", 1.0);
  o.kicad.origin = b.value("origin", std::string("auto"));
  if (b.contains("origin_at") && b["origin_at"].is_array() && b["origin_at"].size() == 2)
    o.kicad.origin_at = {b["origin_at"][0].get<double>(), b["origin_at"][1].get<double>()};
  o.center_drawing = b.value("center", false);
  if (b.contains("placement")) o.placement = Mat4::from_json(b["placement"]);
  return o;
}

bool cacheable(const json& asset, const AssetOptions& opt) { return opt.cache && asset.value("kind", "") != "kicad_pcb"; }  // a board's models change without it
std::string content_of(const std::string& sha, const json& asset) { return sha + "|" + asset.value("builder", json::object()).dump(); }

// The file read into a document of its own (one import op, live bodies); a slow read is remembered by the file's content
// and how it was read, so a clone, a branch or a moved folder finds it again.
void read_file(Document& scratch, const fs::path& file, const json& asset, const std::string& sha, const AssetOptions& opt) {
  const ImportOptions o = read_options(asset, opt);
  const std::string content = content_of(sha, asset);
  if (cacheable(asset, opt) && detail::asset_cache_load(scratch, content, o)) return;
  const auto start = std::chrono::steady_clock::now();
  import_file(scratch, file, o);
  if (cacheable(asset, opt) && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(800)) detail::asset_cache_store(scratch, content);
}

json read_op(const Document& scratch) {
  for (auto it = scratch.ops.rbegin(); it != scratch.ops.rend(); ++it)
    if (it->type == "import") {
      json data = it->data;
      for (const char* k : {"op", "id", "ts", "by", "parent"}) data.erase(k);
      return data;
    }
  throw Error("the file holds nothing to show");
}

json body_meta(const Document& scratch, const std::string& from, const std::string& import_id) {
  const BodyEntry* e = scratch.body(from);
  json meta = e ? e->meta : json::object();
  meta["asset"] = import_id;
  return meta;
}

// `stale`: read from a file that is not the one synced, so the shape is not what the key stood for when the document's
// features were computed (a sync must not keep such a key: what depends on it has to be computed again).
void bind_body(Document& doc, const Document& scratch, const std::string& from, const std::string& key, const std::string& import_id, bool stale = false) {
  json meta = body_meta(scratch, from, import_id);
  if (stale) meta["stale"] = true;
  doc.add_external_body(key, meta);
  cache_shape(doc, key, body_shape(scratch, from));
}

bool cancelled_error(const std::exception& e) { return std::string(e.what()).find("cancelled") != std::string::npos; }

// Where an asset may be: beside the document as recorded, the project's copy, the absolute path.
std::vector<fs::path> candidates(const Document& doc, const json& asset) {
  std::vector<fs::path> out;
  if (!asset.is_object()) return out;
  const std::string rel = asset.value("path", ""), abs = asset.value("abs", "");
  if (const fs::path dir = doc_dir(doc); !dir.empty()) {
    if (!rel.empty()) out.push_back((dir / path_from_utf8(rel)).lexically_normal());
    if (const fs::path name = path_from_utf8(rel.empty() ? abs : rel).filename(); !name.empty()) out.push_back(dir / "assets" / name);
  }
  if (!abs.empty()) out.push_back(path_from_utf8(abs));
  return out;
}

AssetState status_of(const Document& doc, const EffectiveOp& e, const AssetOptions& opt) {
  const json& d = e.data();
  const json& a = d["asset"];
  AssetState st;
  st.import_id = e.op->id;
  st.name = d.value("source", "");
  st.kind = a.value("kind", "");
  st.storage = a.value("storage", "linked");
  st.path = a.value("path", a.value("abs", std::string()));
  each_body(nodes_of(d), [&](const json& n) {
    ++st.bodies;
    st.unbound += !doc.has_body(n.value("key", ""));
  });
  if (st.storage == "embedded") {
    st.state = "embedded";
    return st;
  }
  st.file = locate_asset(doc, a, opt);
  if (st.file.empty()) {
    st.state = "missing";
    st.reason = "not found: " + st.path;
    for (const auto& c : candidates(doc, a))
      if (network(c) && !asset_trusted(doc, c, opt)) {  // on a share nobody trusted: never looked at
        st.file = c;
        st.state = "untrusted";
        st.reason = "on a network share: " + utf8(c);
        break;
      }
  } else if (!asset_trusted(doc, st.file, opt)) {
    st.state = "untrusted";
    st.reason = "outside the document's project: " + utf8(st.file);
  } else {
    try {
      st.sha256 = file_sha256(st.file);
      st.state = st.sha256 == a.value("sha256", "") ? "ok" : "changed";
    } catch (const std::exception& ex) {
      st.state = "error";
      st.reason = ex.what();
    }
  }
  return st;
}

}  // namespace

json AssetState::to_json() const {
  json j = {{"import", import_id}, {"name", name}, {"kind", kind}, {"storage", storage}, {"path", path}, {"state", state}, {"bodies", bodies}};
  if (!file.empty()) j["file"] = utf8(file);
  if (!reason.empty()) j["reason"] = reason;
  if (!sha256.empty()) j["sha256"] = sha256;
  if (unbound) j["unbound"] = unbound;
  return j;
}

std::string file_sha256(const fs::path& file) {
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  if (ec) throw Error("cannot read " + utf8(file));
  const auto time = fs::last_write_time(file, ec);
  std::string where = utf8(fs::absolute(file).lexically_normal());
#ifdef _WIN32
  where = lower(where);
#endif
  const std::string stat = sha256_hex(where + "|" + std::to_string(size) + "|" + std::to_string(time.time_since_epoch().count()));
  if (const auto known = cache_get("asset-sha", stat); known && known->size() == 64) return *known;
  const std::string sha = sha256_file(file);
  // A file written moments ago can be written again within the clock's step with the same size: remember it once it is
  // older than that (git's "racy" entries).
  if (fs::file_time_type::clock::now() - time > std::chrono::seconds(3)) cache_put("asset-sha", stat, sha);
  return sha;
}

bool has_assets(const Document& doc) {
  for (const auto& o : doc.ops)
    if (o.type == "import" && o.data.contains("asset")) return true;
  return false;
}

json asset_of(const Document& doc, const std::string& import_id) {
  // The import and the edits of its asset only: cheap enough for a click (no effective log of every sketch and feature).
  const Op* op = doc.find_op(import_id);
  if (!op || op->type != "import" || !op->data.contains("asset")) return nullptr;
  std::set<std::string> deleted;
  for (const auto& o : doc.ops)
    if (o.type == "delete") deleted.insert(o.data.value("target", ""));
  json asset = op->data["asset"];
  for (const auto& o : doc.ops)
    if (o.type == "edit" && !deleted.count(o.id) && o.data.value("target", "") == import_id && o.data["set"].contains("asset")) asset = o.data["set"]["asset"];
  return asset;
}

bool asset_trusted(const Document& doc, const fs::path& file, const AssetOptions& opt) {
  if (opt.trust_all) return true;
  const bool remote = network(file);
  for (const auto& t : opt.trusted)
    if (inside(file, t, remote)) return true;
  if (doc.path.empty()) return !remote;
  const fs::path dir = doc_dir(doc);
  if (inside(file, dir, true)) return true;
  if (remote) return false;
  return inside(file, dir, false) || inside(file, project_root(dir), false);
}

fs::path locate_asset(const Document& doc, const json& asset, const AssetOptions& opt) {
  for (const auto& c : candidates(doc, asset)) {
    if (network(c) && !asset_trusted(doc, c, opt)) continue;  // not even looked at
    std::error_code ec;
    if (fs::is_regular_file(c, ec)) return c;
  }
  return {};
}

std::vector<AssetState> asset_status(const Document& doc, const AssetOptions& opt) {
  std::vector<AssetState> out;
  for (const auto& e : asset_imports(doc)) out.push_back(status_of(doc, e, opt));
  return out;
}

std::vector<AssetState> load_assets(Document& doc, const AssetOptions& opt) {
  std::vector<AssetState> out;
  const std::vector<EffectiveOp> imports = asset_imports(doc);  // registering bodies leaves the ops alone
  for (const auto& e : imports) {
    AssetState st = status_of(doc, e, opt);
    if ((st.state == "ok" || st.state == "changed") && st.unbound > 0) {
      try {
        // A changed file: the version synced when it is remembered, so the model stays as its features were computed;
        // else the file as it is now, its shapes marked stale.
        const json& asset = e.data()["asset"];
        Document scratch = Document::create();
        bool stale = st.state == "changed";
        if (stale && cacheable(asset, opt) && detail::asset_cache_load(scratch, content_of(asset.value("sha256", ""), asset), read_options(asset, opt))) {
          stale = false;
          st.reason = "the version synced is shown (remembered); sync to take the file as it is now";
        } else {
          read_file(scratch, st.file, asset, st.sha256, opt);
        }
        json nodes = read_op(scratch)["nodes"];
        relabel(nodes, e.op->id, "", &nodes_of(e.data()));
        std::map<std::string, std::string> from;  // node id -> body in the read
        each_body(nodes, [&](const json& n) { from[n.value("id", "")] = n.value("key", ""); });
        st.unbound = 0;
        // Bound by place, under the keys the import names.
        each_body(nodes_of(e.data()), [&](const json& n) {
          const std::string key = n.value("key", "");
          if (doc.has_body(key)) return;
          const auto it = from.find(n.value("id", ""));
          if (it == from.end()) ++st.unbound;
          else bind_body(doc, scratch, it->second, key, e.op->id, stale);
        });
        if (st.unbound) st.reason = std::to_string(st.unbound) + " parts are no longer in the file";
      } catch (const Standard_Failure& ex) {
        st.state = "error";
        st.reason = ex.GetMessageString();
      } catch (const std::exception& ex) {
        if (cancelled_error(ex)) throw;
        st.state = "error";
        st.reason = ex.what();
      }
    }
    out.push_back(std::move(st));
  }
  return out;
}

ImportResult link_file(Document& doc, const fs::path& file, const ImportOptions& opt) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) throw Error("file not found: " + utf8(file.filename()));
  const fs::path abs = fs::absolute(file).lexically_normal();
  if (opt.progress && !opt.progress(-1, "reading")) throw Error("import cancelled");
  const std::string sha = file_sha256(abs);
  const std::string kind = kind_of(abs);
  json asset = {{"v", 1}, {"kind", kind}};
  if (const std::string rel = relative_to(abs, doc_dir(doc)); !rel.empty()) asset["path"] = rel;
  asset["abs"] = utf8(abs);
  asset["sha256"] = sha;
  asset["size"] = fs::file_size(abs);
  asset["storage"] = "linked";
  asset["builder"] = {{"name", "opad"}, {"version", 1}, {"options", builder_options(kind, opt)}};
  AssetOptions read;
  read.kicad = opt.kicad;
  read.progress = opt.progress;
  Document scratch = Document::create();
  read_file(scratch, abs, asset, sha, read);
  json data = read_op(scratch);
  if (kind == "kicad_pcb" && data.contains("kicad") && data["kicad"].contains("origin"))
    asset["builder"]["options"]["origin_at"] = data["kicad"]["origin"];  // every later read keeps this frame
  asset["synced"] = now_iso8601();
  const std::string id = new_uuid();
  relabel(data["nodes"], id, "");
  ImportResult res;
  std::map<std::string, std::string> keys;  // body in the read -> derived key
  each_node(data["nodes"], [&](json& n) {
    if (n.value("type", "") != "body") return void(++res.components);
    ++res.bodies;
    const std::string from = n.value("key", "");
    auto it = keys.find(from);
    if (it == keys.end()) {
      const size_t ordinal = keys.size();
      it = keys.emplace(from, sha256_hex(kKeyDomain + sha + "|" + std::to_string(ordinal))).first;
    }
    n["key"] = it->second;
  });
  for (const auto& [from, key] : keys) bind_body(doc, scratch, from, key, id);
  data["op"] = "import";
  data["id"] = id;
  data["asset"] = asset;
  if (!opt.parent.empty()) data["parent"] = opt.parent;
  res.op_id = doc.append(data, opt.author).id;
  res.info = {{"linked", true}, {"sha256", sha}, {"kind", kind}};
  return res;
}

design::Plan plan_asset_sync(const Document& doc, const std::string& import_id, const AssetOptions& opt, const fs::path& file) {
  const EffectiveOp e = find_asset(doc, import_id);
  const std::string id = e.op->id;
  const json& data = e.data();
  json asset = data["asset"];
  if (asset.value("storage", "linked") == "embedded") throw Error("the asset is embedded: it is no longer read from a file");
  const fs::path where = file.empty() ? locate_asset(doc, asset, opt) : fs::absolute(file).lexically_normal();
  if (where.empty()) throw Error("the linked file is not found: " + asset.value("path", asset.value("abs", std::string())));
  if (file.empty() && !asset_trusted(doc, where, opt)) throw Error("the linked file is outside the document's project; trust it first: " + utf8(where));
  const std::string sha = file_sha256(where);
  auto place = [&](json& a) {
    a["abs"] = utf8(where);
    if (const std::string rel = relative_to(where, doc_dir(doc)); !rel.empty()) a["path"] = rel;
    else a.erase("path");
  };
  design::Plan plan;
  if (sha == asset.value("sha256", "")) {  // the file synced last: at most it moved
    plan.report = {{"import", id}, {"sha256", sha}, {"up_to_date", true}};
    if (!file.empty() && utf8(where) != asset.value("abs", "")) {
      place(asset);
      plan.ops.push_back(design::make_edit_op(id, {{"asset", asset}}));
    }
    return plan;
  }
  Document scratch = Document::create();
  read_file(scratch, where, asset, sha, opt);
  json fresh = read_op(scratch);
  relabel(fresh["nodes"], id, "", &nodes_of(data));
  // The keys the same places had, kept for a body whose geometry did not change (no needless regeneration downstream).
  std::map<std::string, std::string> was;  // node id -> key
  std::map<std::string, std::string> names;
  each_body(nodes_of(data), [&](const json& n) { was[n.value("id", "")] = n.value("key", ""); names[n.value("id", "")] = n.value("name", ""); });
  std::map<std::string, std::set<std::string>> before;  // body in the read -> the keys its places had
  std::vector<std::string> order;
  each_body(fresh["nodes"], [&](const json& n) {
    const std::string from = n.value("key", "");
    if (std::find(order.begin(), order.end(), from) == order.end()) order.push_back(from);
    if (const auto it = was.find(n.value("id", "")); it != was.end()) before[from].insert(it->second);
  });
  std::map<std::string, std::string> keys;
  int kept = 0;
  for (size_t i = 0; i < order.size(); ++i) {
    const std::string& from = order[i];
    std::string key;
    if (const auto it = before.find(from); it != before.end() && it->second.size() == 1 && doc.has_body(*it->second.begin()) &&
                                            !doc.body(*it->second.begin())->meta.value("stale", false)) {
      try {
        if (design::same_shapes(body_shape(scratch, from), body_shape(doc, *it->second.begin()))) key = *it->second.begin();
      } catch (const Standard_Failure&) {
      } catch (const std::exception&) {
      }
    }
    kept += !key.empty();
    if (key.empty()) key = sha256_hex(kKeyDomain + sha + "|" + std::to_string(i));
    keys[from] = key;
    if (opt.progress && !opt.progress(double(i + 1) / double(order.size()), "comparing")) throw Error("cancelled");
  }
  json added = json::array(), removed = json::array(), changed = json::array();
  std::set<std::string> now;
  each_body(fresh["nodes"], [&](json& n) {
    n["key"] = keys[n.value("key", "")];
    const std::string nid = n.value("id", "");
    now.insert(nid);
    const auto it = was.find(nid);
    if (it == was.end()) added.push_back(n.value("name", ""));
    else if (it->second != n["key"].get<std::string>()) changed.push_back(n.value("name", ""));
  });
  for (const auto& [nid, key] : was)
    if (!now.count(nid)) removed.push_back(names[nid]);
  asset["sha256"] = sha;
  asset["size"] = fs::file_size(where);
  asset["synced"] = now_iso8601();
  place(asset);
  fresh["asset"] = asset;
  for (const auto& [k, v] : data.items())  // what the reader no longer says (warnings) goes
    if (!fresh.contains(k) && k != "op" && k != "id" && k != "ts" && k != "by" && k != "parent") fresh[k] = nullptr;
  Document staged = doc;  // the plan's walk reads the new bodies from it
  std::vector<design::NewBody> bodies;
  for (const auto& from : order) {
    const std::string& key = keys[from];
    if (doc.has_body(key)) continue;
    const TopoDS_Shape shape = body_shape(scratch, from);
    const json meta = body_meta(scratch, from, id);
    staged.add_external_body(key, meta);
    cache_shape(staged, key, shape);
    bodies.push_back({key, "", meta, std::make_shared<TopoDS_Shape>(shape)});
  }
  plan = design::plan_ops(staged, {design::make_edit_op(id, fresh)}, false, [&opt] { return opt.progress && !opt.progress(-1, "regenerating"); });
  plan.bodies.insert(plan.bodies.begin(), bodies.begin(), bodies.end());
  plan.report["import"] = id;
  plan.report["sha256"] = sha;
  plan.report["up_to_date"] = false;
  plan.report["added"] = added;
  plan.report["removed"] = removed;
  plan.report["changed"] = changed;
  plan.report["kept"] = kept;
  return plan;
}

design::Plan plan_asset_embed(const Document& doc, const std::string& import_id, const std::function<bool()>& cancel) {
  const EffectiveOp e = find_asset(doc, import_id);
  const json& data = e.data();
  json asset = data["asset"];
  if (asset.value("storage", "linked") == "embedded") throw Error("the asset is embedded already");
  std::vector<std::string> keys;
  each_body(nodes_of(data), [&](const json& n) {
    const std::string key = n.value("key", "");
    if (!doc.has_body(key)) throw Error("the linked file is not loaded: locate or sync it first");
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
  });
  // Each body as the store keeps it (healed like a full import), side by side: the kernel work is independent per shape.
  struct Work { TopoDS_Shape shape; std::string brep, error; bool healed = false; };
  std::vector<Work> work(keys.size());
  std::atomic<bool> stop{false};
  OSD_Parallel::For(0, static_cast<int>(keys.size()), [&](int i) {
    if (stop || (cancel && cancel())) { stop = true; return; }
    Work& w = work[static_cast<size_t>(i)];
    try {
      w.shape = body_shape(doc, keys[static_cast<size_t>(i)]);
      w.brep = detail::persist_body(w.shape, doc.body(keys[static_cast<size_t>(i)])->meta.value("representation", "solid"), w.healed);
    } catch (const Standard_Failure& ex) {
      w.error = ex.GetMessageString();
    } catch (const std::exception& ex) {
      w.error = ex.what();
    }
  });
  if (stop) throw Error("cancelled");
  Document staged = doc;
  std::map<std::string, std::string> renamed;
  std::vector<design::NewBody> bodies;
  int healed = 0;
  for (size_t i = 0; i < keys.size(); ++i) {
    json meta = doc.body(keys[i])->meta;
    meta.erase("asset");
    if (!work[i].error.empty()) throw Error("body '" + meta.value("name", keys[i].substr(0, 12)) + "' cannot be embedded: " + work[i].error);
    const std::string key = sha256_hex(work[i].brep);
    renamed[keys[i]] = key;
    healed += work[i].healed;
    if (staged.has_body(key)) continue;
    staged.add_body(work[i].brep, meta);
    cache_shape(staged, key, work[i].shape);
    bodies.push_back({key, std::move(work[i].brep), meta, std::make_shared<TopoDS_Shape>(work[i].shape)});
  }
  json nodes = nodes_of(data);
  each_body(nodes, [&](json& n) { n["key"] = renamed[n.value("key", "")]; });
  asset["storage"] = "embedded";
  design::Plan plan = design::plan_ops(staged, {design::make_edit_op(e.op->id, {{"nodes", nodes}, {"asset", asset}})}, false, cancel);
  plan.bodies.insert(plan.bodies.begin(), bodies.begin(), bodies.end());
  plan.report["import"] = e.op->id;
  plan.report["embedded"] = keys.size();
  plan.report["healed"] = healed;
  return plan;
}

json pack_asset(Document& doc, const std::string& import_id, const std::string& author) {
  if (doc.path.empty()) throw Error("save the document first: its project's assets folder is beside it");
  const EffectiveOp e = find_asset(doc, import_id);
  json asset = e.data()["asset"];
  if (asset.value("storage", "linked") == "embedded") throw Error("the asset is embedded: nothing to pack");
  AssetOptions any;
  any.trust_all = true;  // the user asks for this copy
  const fs::path source = locate_asset(doc, asset, any);
  if (source.empty()) throw Error("the linked file is not found: " + asset.value("path", asset.value("abs", std::string())));
  const fs::path dir = doc_dir(doc), folder = dir / "assets";
  int copied = 0;
  auto copy = [&](const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(to, ec) && fs::equivalent(from, to, ec)) return;
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
    ++copied;
  };
  fs::path target;
  if (inside(source, folder, false)) {
    target = source;  // the project's copy already
  } else if (asset.value("kind", "") == "kicad_pcb") {
    // The board with its project file (text variables) and the models below its folder, laid out as there so ${KIPRJMOD}
    // still finds them; models of KiCad's libraries stay where KiCad keeps them.
    const fs::path from = source.parent_path(), to = folder / source.stem();
    target = to / source.filename();
    copy(source, target);
    if (fs::path project = fs::path(source).replace_extension(".kicad_pro"); fs::exists(project)) copy(project, to / project.filename());
    for (const auto& m : kicad_models(source).value("models", json::array()))
      if (m.contains("file")) {
        const fs::path f = path_from_utf8(m["file"].get<std::string>());
        if (inside(f, from, false)) copy(f, to / fs::absolute(f).lexically_normal().lexically_relative(fs::absolute(from).lexically_normal()));
      }
  } else {
    target = folder / source.filename();
    const std::string sha = file_sha256(source);
    for (int i = 2; fs::exists(target) && file_sha256(target) != sha; ++i)  // another file of that name: keep both
      target = folder / path_from_utf8(utf8(source.stem()) + " (" + std::to_string(i) + ")" + utf8(source.extension()));
    copy(source, target);
  }
  asset["storage"] = "project";
  if (target != source) asset["from"] = utf8(source);
  asset["abs"] = utf8(target);
  asset["path"] = relative_to(target, dir);
  doc.append(design::make_edit_op(e.op->id, {{"asset", asset}}), author);
  return {{"import", e.op->id}, {"path", asset["path"]}, {"copied", copied}};
}

void rebase_asset_paths(Document& doc, const fs::path& dir) {
  if (dir.empty()) return;
  const fs::path base = fs::absolute(dir);
  for (size_t i = doc.persisted_ops(); i < doc.ops.size(); ++i) {
    const Op& o = doc.ops[i];
    const bool import = o.type == "import" && o.data.contains("asset");
    const bool edit = o.type == "edit" && o.data["set"].contains("asset");
    if (!import && !edit) continue;
    json data = o.data;
    json& asset = import ? data["asset"] : data["set"]["asset"];
    if (!asset.is_object() || !asset.contains("abs")) continue;
    const std::string rel = relative_to(path_from_utf8(asset["abs"].get<std::string>()), base);
    if (rel.empty() ? !asset.contains("path") : asset.value("path", "") == rel) continue;
    if (rel.empty()) asset.erase("path");
    else asset["path"] = rel;
    doc.rewrite_op(i, std::move(data));
  }
}

}  // namespace opad
