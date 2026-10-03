// Viewer mode remembers slow reads: the shapes a STEP or IGES translation produced (with the display meshes made since),
// in OCCT's binary format under the user cache, keyed by the file's content and how it is read. Opening the file again,
// wherever it now is (a copy, a clone, a checkout that touched it), reads them back instead of translating it again (the
// Engine: 71 s of translation). An entry is kept only when reading it back is at least twice as fast as reading the file
// (UI-75: a drawing's entry was 134 MB for a 3 MB DWG and no faster): drawings are never kept that way, and a DWG keeps
// the DXF text its conversion made instead (24 MB for that drawing, whose open went from 4.0-4.5 s to 2.4 s).
#include <BRep_Builder.hxx>
#include <BinTools.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

#include "import_common.hpp"
#include "opad/assets.hpp"
#include "opad/cache.hpp"
#include "opad/geometry.hpp"
#include "opad/step_io.hpp"

namespace opad {
namespace {

constexpr const char* kMagic = "OPADVC1";  // the head has "read_ms" since UI-75; older readers ignore it
constexpr std::uintmax_t kBudget = 2ull << 30;  // all viewer entries together; the oldest go first

using Clock = std::chrono::steady_clock;
double since(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

std::filesystem::path folder() { return cache_dir() / "viewer"; }

std::string extension(const std::filesystem::path& file) {
  std::string e = file.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

// A file's content hash (remembered per path, size and time, so an unchanged file is hashed once), or empty.
std::string content_hash(const std::filesystem::path& file) {
  try {
    return file_sha256(file);
  } catch (const std::exception&) {
    return {};
  }
}

std::filesystem::path entry_for(const std::filesystem::path& file, const ImportOptions& opt) {
  if (!viewer_cache_applies(file)) return {};
  const std::string sha = content_hash(file);
  if (sha.empty()) return {};
  const std::string identity = "viewer|" + sha + "|" + extension(file) + "|" + version_string() + "|" + (opt.center_drawing ? "c" : "") + "|" +
                               opt.placement.to_json().dump();
  return folder() / (sha256_hex(identity).substr(0, 40) + ".bin");
}

// Beside an entry that did not win: the file is not stored again until it changes.
std::filesystem::path skip_for(std::filesystem::path entry) { return entry.replace_extension(".skip"); }

void mark_slow(const std::filesystem::path& entry, double read_ms, double cached_ms) {
  std::error_code error;
  std::filesystem::remove(entry, error);
  try {
    write_text_file(skip_for(entry), json{{"read_ms", read_ms}, {"cached_ms", cached_ms}}.dump() + "\n");
  } catch (const std::exception&) {
  }
}

// The import op and its live bodies (a viewer document holds exactly one import of the file).
const Op* import_op(const Document& doc) {
  for (const auto& o : doc.ops)
    if (o.type == "import") return &o;
  return nullptr;
}

void collect_keys(const json& nodes, std::vector<std::string>& keys) {
  if (!nodes.is_array()) return;
  for (const auto& n : nodes) {
    if (n.contains("key") && n["key"].is_string() && std::find(keys.begin(), keys.end(), n["key"].get<std::string>()) == keys.end())
      keys.push_back(n["key"].get<std::string>());
    if (n.contains("children")) collect_keys(n["children"], keys);
  }
}

void evict() {
  std::error_code error;
  std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
  std::uintmax_t total = 0;
  for (const auto& e : std::filesystem::directory_iterator(folder(), error)) {
    if (!e.is_regular_file(error)) continue;
    total += e.file_size(error);
    files.push_back({e.last_write_time(error), e.path()});
  }
  std::sort(files.begin(), files.end());
  for (const auto& [time, path] : files) {
    if (total <= kBudget) break;
    const auto size = std::filesystem::file_size(path, error);
    if (std::filesystem::remove(path, error)) total -= size;
  }
}

// An entry keyed by content (a linked asset's file hash and how it is read) rather than by where the file is.
std::filesystem::path content_entry(const std::string& content) { return folder() / (sha256_hex("asset|" + content + "|" + version_string()).substr(0, 40) + ".bin"); }

// A DWG's converted DXF text, by the DWG's content and the converter that made it.
std::filesystem::path dwg_entry(const std::filesystem::path& dwg, const std::string& converter) {
  const std::string sha = content_hash(dwg);
  return sha.empty() ? std::filesystem::path() : folder() / (sha256_hex("dwg|" + sha + "|" + converter + "|" + version_string()).substr(0, 40) + ".dxf");
}

// The head of an entry (op, bodies, the read time it replaces), its stream left at the shapes.
bool read_head(std::ifstream& in, json& head) {
  std::string magic;
  size_t length = 0;
  if (!std::getline(in, magic) || magic != kMagic || !(in >> length) || in.get() != '\n' || length > (size_t(1) << 31)) return false;
  std::string text(length, '\0');
  if (!in.read(text.data(), static_cast<std::streamsize>(length))) return false;
  head = json::parse(text);
  return true;
}

// `timed`: a viewer entry, which must stay at least twice as fast as the read it replaces: one that is not (a cold disk,
// another machine's numbers) is dropped after this load and the file marked.
bool load_entry(Document& doc, const std::filesystem::path& entry, const ImportOptions& opt, bool timed) {
  std::error_code error;
  if (entry.empty() || !std::filesystem::exists(entry, error)) return false;
  const auto start = Clock::now();
  try {
    std::ifstream in(entry, std::ios::binary);
    json head;
    if (!read_head(in, head)) return false;
    if (opt.progress && !opt.progress(-1, "reading")) throw Error("import cancelled");
    TopoDS_Shape all;
    BinTools::Read(all, in);
    const json& bodies = head.at("bodies");
    std::vector<TopoDS_Shape> shapes;
    for (TopoDS_Iterator it(all); it.More(); it.Next()) shapes.push_back(it.Value());
    if (shapes.size() != bodies.size()) return false;
    Document staged = doc;
    for (size_t i = 0; i < shapes.size(); ++i) {
      const std::string key = bodies[i].at("key").get<std::string>();
      staged.add_live_body(key, bodies[i].at("meta"));
      cache_shape(staged, key, shapes[i]);
    }
    json op = head.at("op");
    if (!opt.parent.empty()) op["parent"] = opt.parent;
    staged.append(op, opt.author);
    doc = std::move(staged);
    in.close();
    const double read_ms = head.value("read_ms", -1.0), took = since(start);
    if (timed && read_ms > 0 && took * 2 > read_ms) mark_slow(entry, read_ms, took);
    else std::filesystem::last_write_time(entry, std::filesystem::file_time_type::clock::now(), error);  // recently used: kept longest
    return true;
  } catch (const Error& e) {
    if (std::string(e.what()) == "import cancelled") throw;
    return false;
  } catch (const Standard_Failure&) {
    return false;
  } catch (const std::exception&) {
    return false;
  }
}

// `read_ms` < 0: an asset entry, always kept (it also holds the version a linked file was synced at).
json store_entry(const Document& doc, const std::filesystem::path& entry, const std::function<bool()>& cancelled, double read_ms) {
  json report = {{"kept", false}};
  const Op* op = import_op(doc);
  std::error_code error;
  if (!op || entry.empty()) return report;
  if (read_ms >= 0 && std::filesystem::exists(skip_for(entry), error)) {
    report["reason"] = "slower";
    return report;
  }
  json data = op->data;
  for (const char* field : {"id", "ts", "by", "parent"}) data.erase(field);
  std::vector<std::string> keys;
  collect_keys(data["nodes"], keys);
  json bodies = json::array();
  BRep_Builder builder;
  TopoDS_Compound all;
  builder.MakeCompound(all);
  for (const auto& key : keys) {
    const BodyEntry* body = doc.body(key);
    if (!body) return report;
    bodies.push_back({{"key", key}, {"meta", body->meta}});
    builder.Add(all, body_shape(doc, key));
  }
  if (cancelled && cancelled()) return report;
  std::filesystem::create_directories(folder(), error);
  const auto partial = entry.string() + ".part";
  const auto start = Clock::now();
  {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) return report;
    json head = {{"op", data}, {"bodies", bodies}};
    if (read_ms >= 0) head["read_ms"] = read_ms;
    const std::string text = head.dump();
    out << kMagic << '\n' << text.size() << '\n' << text;
    try {
      BinTools::Write(all, out, Standard_True, Standard_True, BinTools_FormatVersion_CURRENT);
    } catch (const Standard_Failure&) {
      out.close();
      std::filesystem::remove(partial, error);
      return report;
    }
    if (!out) {
      out.close();
      std::filesystem::remove(partial, error);
      return report;
    }
  }
  const double write_ms = since(start);
  if (cancelled && cancelled()) { std::filesystem::remove(partial, error); return report; }
  std::filesystem::rename(partial, entry, error);
  if (error) {
    std::filesystem::remove(partial, error);
    return report;
  }
  report["write_ms"] = write_ms;
  report["bytes"] = std::filesystem::file_size(entry, error);
  // Reading an entry back costs about what writing it did (the Hydrostatic: 0.3 s to write, 0.4 s to read for 17 s of
  // translation): a read many times the write wins for sure; otherwise it is read back and timed.
  if (read_ms >= 0 && write_ms * 4 >= read_ms) {
    const auto back = Clock::now();
    try {
      std::ifstream in(entry, std::ios::binary);
      json head;
      TopoDS_Shape shapes;
      if (read_head(in, head)) BinTools::Read(shapes, in);
    } catch (const Standard_Failure&) {
    } catch (const std::exception&) {
    }
    report["cached_ms"] = since(back);
    if (report["cached_ms"].get<double>() * 2 > read_ms) {
      mark_slow(entry, read_ms, report["cached_ms"].get<double>());
      report["reason"] = "slower";
      return report;
    }
  }
  report["kept"] = true;
  evict();
  return report;
}

}  // namespace

bool viewer_cache_applies(const std::filesystem::path& file) {
  const std::string ext = extension(file);
  return ext != ".dxf" && ext != ".svg" && ext != ".dwg";
}
bool viewer_cache_load(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) { return load_entry(doc, entry_for(file, opt), opt, true); }
json viewer_cache_store(const Document& doc, const std::filesystem::path& file, const ImportOptions& opt, double read_ms, const std::function<bool()>& cancelled) {
  if (!viewer_cache_applies(file)) return {{"kept", false}, {"reason", "drawing"}};
  return store_entry(doc, entry_for(file, opt), cancelled, std::max(read_ms, 0.0));
}

namespace detail {
bool asset_cache_load(Document& doc, const std::string& content, const ImportOptions& opt) { return load_entry(doc, content_entry(content), opt, false); }
void asset_cache_store(const Document& doc, const std::string& content, const std::function<bool()>& cancelled) { store_entry(doc, content_entry(content), cancelled, -1); }

std::filesystem::path dwg_cache_find(const std::filesystem::path& dwg, const std::string& converter) {
  const auto entry = dwg_entry(dwg, converter);
  std::error_code error;
  if (entry.empty() || !std::filesystem::is_regular_file(entry, error)) return {};
  std::filesystem::last_write_time(entry, std::filesystem::file_time_type::clock::now(), error);
  return entry;
}

bool dwg_cache_keep(const std::filesystem::path& dwg, const std::string& converter, const std::filesystem::path& dxf, double convert_ms, double read_ms) {
  // Reading the DXF is itself most of a DWG's open (a 3 MB, 176-layer drawing: 1.2-1.7 s to convert, 2.4 s to read its 24 MB
  // DXF), so the DXF is kept when it opens the drawing at least 1.5 times as fast: twice would never keep that drawing's.
  if (convert_ms < 250 || convert_ms * 2 < read_ms) return false;
  const auto entry = dwg_entry(dwg, converter);
  std::error_code error;
  if (entry.empty()) return false;
  std::filesystem::create_directories(folder(), error);
  const auto partial = entry.string() + ".part";
  std::filesystem::copy_file(dxf, partial, std::filesystem::copy_options::overwrite_existing, error);
  if (!error) std::filesystem::rename(partial, entry, error);
  if (error) {
    std::filesystem::remove(partial, error);
    return false;
  }
  evict();
  return true;
}
}  // namespace detail

}  // namespace opad
