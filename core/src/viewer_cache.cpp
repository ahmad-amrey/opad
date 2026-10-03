// Viewer mode remembers slow reads: the shapes a STEP or IGES translation produced (with the display meshes made since),
// in OCCT's binary format under the user cache, keyed by the file's path, size and time. Opening the unchanged file
// again reads them back instead of translating it again (the Engine: 71 s of translation).
#include <BRep_Builder.hxx>
#include <BinTools.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include "opad/cache.hpp"
#include "opad/geometry.hpp"
#include "opad/step_io.hpp"

namespace opad {
namespace {

constexpr const char* kMagic = "OPADVC1";
constexpr std::uintmax_t kBudget = 2ull << 30;  // all viewer entries together; the oldest go first
// Bumped when a reader writes something new into its import op, so entries cached before it are read again (2: the
// DXF layer table's off / frozen / locked / plot / linetype / lineweight, TODO 11 UI-37; 3: by_layer, 4: a layer's linetype pattern, UI-89;
// 5: a far drawing's drawing_origin, UI-90; 6: shaped text, 7: a body's own linetype and lineweight, 8: shape fonts, 9: MTEXT formatting, 10: bold and italic styles, 11: MTEXT parts and obliquing, 12: a body's own linetype scale, 13: explicit bidi embeddings and isolates, UI-92).
constexpr const char* kReaders = "13";

std::filesystem::path folder() { return cache_dir() / "viewer"; }

std::filesystem::path entry_for(const std::filesystem::path& file, const ImportOptions& opt) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(file, error);
  const auto size = std::filesystem::file_size(file, error);
  if (error) return {};
  const auto time = std::filesystem::last_write_time(file, error);
  if (error) return {};
  // One spelling per file: separators, "..", and on Windows letter case do not make another entry.
  const auto u8 = absolute.lexically_normal().generic_u8string();
  std::string identity(u8.begin(), u8.end());
#ifdef _WIN32
  std::transform(identity.begin(), identity.end(), identity.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
#endif
  identity += "|" + std::to_string(size) + "|" + std::to_string(time.time_since_epoch().count()) + "|" + version_string() + "|" + kReaders +
              "|" + (opt.center_drawing ? "c" : "") + "|" + opt.placement.to_json().dump();
  return folder() / (sha256_hex(identity).substr(0, 40) + ".bin");
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

}  // namespace

bool viewer_cache_load(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) {
  const auto entry = entry_for(file, opt);
  std::error_code error;
  if (entry.empty() || !std::filesystem::exists(entry, error)) return false;
  try {
    std::ifstream in(entry, std::ios::binary);
    std::string magic;
    size_t length = 0;
    if (!std::getline(in, magic) || magic != kMagic || !(in >> length) || in.get() != '\n' || length > (size_t(1) << 31)) return false;
    std::string text(length, '\0');
    if (!in.read(text.data(), static_cast<std::streamsize>(length))) return false;
    const json head = json::parse(text);
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
    std::filesystem::last_write_time(entry, std::filesystem::file_time_type::clock::now(), error);  // recently used: kept longest
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

void viewer_cache_store(const Document& doc, const std::filesystem::path& file, const ImportOptions& opt, const std::function<bool()>& cancelled) {
  const Op* op = import_op(doc);
  const auto entry = entry_for(file, opt);
  if (!op || entry.empty()) return;
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
    if (!body) return;
    bodies.push_back({{"key", key}, {"meta", body->meta}});
    builder.Add(all, body_shape(doc, key));
  }
  if (cancelled && cancelled()) return;
  std::error_code error;
  std::filesystem::create_directories(folder(), error);
  const auto partial = entry.string() + ".part";
  {
    std::ofstream out(partial, std::ios::binary | std::ios::trunc);
    if (!out) return;
    const std::string head = json{{"op", data}, {"bodies", bodies}}.dump();
    out << kMagic << '\n' << head.size() << '\n' << head;
    try {
      BinTools::Write(all, out, Standard_True, Standard_True, BinTools_FormatVersion_CURRENT);
    } catch (const Standard_Failure&) {
      out.close();
      std::filesystem::remove(partial, error);
      return;
    }
    if (!out) {
      out.close();
      std::filesystem::remove(partial, error);
      return;
    }
  }
  if (cancelled && cancelled()) { std::filesystem::remove(partial, error); return; }
  std::filesystem::rename(partial, entry, error);
  if (error) std::filesystem::remove(partial, error);
  evict();
}

}  // namespace opad
