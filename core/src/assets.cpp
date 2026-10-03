// Linked assets (assets.hpp): imports by reference, read from their files on open, synced by an edit of their import op.
#include "opad/assets.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <OSD_Parallel.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
constexpr const char* kKeyDomain = "opad-asset/2|";  // a derived key is never the hash of a body's BREP text

// A body's geometry in a few bytes: its topology counts, its vertices, the type and middle point of every edge and face
// (a mesh face: its nodes), each list sorted, to about 1e-6 mm. The same part in another version of the file gives the same
// digest, so its key survives a sync (nothing that uses it is computed again) without the version synced at hand.
std::string shape_digest(const TopoDS_Shape& s) {
  using Rec = std::array<std::int64_t, 5>;
  auto q = [](double v) { return std::isfinite(v) ? std::int64_t(std::llround(v * 1e6)) : std::int64_t(0); };
  std::vector<std::int64_t> head;
  std::vector<Rec> vertices, edges, faces;
  TopTools_IndexedMapOfShape map;
  for (TopAbs_ShapeEnum type : {TopAbs_SOLID, TopAbs_SHELL, TopAbs_WIRE}) {
    map.Clear();
    TopExp::MapShapes(s, type, map);
    head.push_back(map.Extent());
  }
  map.Clear();
  TopExp::MapShapes(s, TopAbs_VERTEX, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(map(i)));
    vertices.push_back({q(p.X()), q(p.Y()), q(p.Z()), 0, 0});
  }
  map.Clear();
  TopExp::MapShapes(s, TopAbs_EDGE, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    const TopoDS_Edge& e = TopoDS::Edge(map(i));
    Rec r{-1, 0, 0, 0, BRep_Tool::Degenerated(e) ? 1 : 0};
    double a = 0, b = 0;
    try {
      if (!r[4] && !BRep_Tool::Curve(e, a, b).IsNull()) {
        const BRepAdaptor_Curve c(e);
        const gp_Pnt p = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
        r = {c.GetType(), q(p.X()), q(p.Y()), q(p.Z()), 0};
      }
    } catch (const Standard_Failure&) {
      r[0] = -2;
    }
    edges.push_back(r);
  }
  map.Clear();
  TopExp::MapShapes(s, TopAbs_FACE, map);
  for (int i = 1; i <= map.Extent(); ++i) {
    const TopoDS_Face& f = TopoDS::Face(map(i));
    TopLoc_Location loc;
    Rec r{-1, 0, 0, 0, f.Orientation()};
    try {
      if (BRep_Tool::Surface(f, loc).IsNull()) {
        if (const Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(f, loc); !t.IsNull()) {
          std::uint64_t h = 1469598103934665603ull;  // FNV-1a over the nodes in their order
          for (int k = 1; k <= t->NbNodes(); ++k) {
            const gp_Pnt p = t->Node(k).Transformed(loc.Transformation());
            for (const std::int64_t v : {q(p.X()), q(p.Y()), q(p.Z())}) h = (h ^ std::uint64_t(v)) * 1099511628211ull;
          }
          r = {-3, t->NbNodes(), t->NbTriangles(), std::int64_t(h), f.Orientation()};
        }
      } else {
        double u0 = 0, u1 = 0, v0 = 0, v1 = 0;
        BRepTools::UVBounds(f, u0, u1, v0, v1);
        const BRepAdaptor_Surface a(f, false);
        const gp_Pnt p = a.Value((u0 + u1) / 2, (v0 + v1) / 2);
        r = {a.GetType(), q(p.X()), q(p.Y()), q(p.Z()), f.Orientation()};
      }
    } catch (const Standard_Failure&) {
      r[0] = -2;
    }
    faces.push_back(r);
  }
  std::string bytes(reinterpret_cast<const char*>(head.data()), head.size() * sizeof(std::int64_t));
  for (auto* list : {&vertices, &edges, &faces}) {
    std::sort(list->begin(), list->end());
    const std::int64_t n = std::int64_t(list->size());
    bytes.append(reinterpret_cast<const char*>(&n), sizeof n);
    bytes.append(reinterpret_cast<const char*>(list->data()), list->size() * sizeof(Rec));
  }
  return sha256_hex(bytes);
}

// The keys of a read's bodies, from their geometry (in parallel: a file of a thousand parts).
std::vector<std::string> asset_keys(const std::vector<TopoDS_Shape>& shapes) {
  std::vector<std::string> keys(shapes.size());
  OSD_Parallel::For(0, static_cast<int>(shapes.size()), [&](int i) {
    try {
      keys[size_t(i)] = sha256_hex(kKeyDomain + shape_digest(shapes[size_t(i)]));
    } catch (const Standard_Failure&) {
    }
  });
  for (size_t i = 0; i < keys.size(); ++i)  // a shape the kernel cannot walk: a key of its own, never another's
    if (keys[i].empty()) keys[i] = sha256_hex(kKeyDomain + new_uuid());
  return keys;
}

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
  if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif" || ext == ".webp") return "image";
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
// up in order when as many of a type are left on both sides. With `keys` (body in the read -> the key of its geometry), a
// body whose geometry is that of exactly one sibling as synced is that part first, wherever it now is and whatever its name.
void relabel(json& nodes, const std::string& op, const std::string& parent, const json* was = nullptr, const std::map<std::string, std::string>* keys = nullptr) {
  const bool roots = parent.empty();
  const std::vector<std::string> now = places(nodes, roots);
  std::vector<int> match(nodes.size(), -1);
  if (was && was->is_array()) {
    const std::vector<std::string> then = places(*was, roots);
    std::vector<bool> taken(then.size());
    auto take = [&](size_t i, size_t j) {
      match[i] = static_cast<int>(j);
      taken[j] = true;
    };
    if (keys) {
      std::map<std::string, std::vector<size_t>> mine, theirs;
      for (size_t i = 0; i < nodes.size(); ++i)
        if (const auto it = keys->find(nodes[i].value("key", "")); nodes[i].value("type", "") == "body" && it != keys->end()) mine[it->second].push_back(i);
      for (size_t j = 0; j < was->size(); ++j)
        if ((*was)[j].value("type", "") == "body") theirs[(*was)[j].value("key", "")].push_back(j);
      for (const auto& [key, here] : mine)
        if (const auto it = theirs.find(key); here.size() == 1 && it != theirs.end() && it->second.size() == 1) take(here[0], it->second[0]);
    }
    for (size_t i = 0; i < now.size(); ++i)
      if (const auto it = std::find(then.begin(), then.end(), now[i]); match[i] < 0 && it != then.end() && !taken[size_t(it - then.begin())])
        take(i, size_t(it - then.begin()));
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
    if (n.contains("children")) relabel(n["children"], op, n["id"].get<std::string>(), before && before->contains("children") ? &(*before)["children"] : nullptr, keys);
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

// The bodies a read's nodes name (`order`: each once, as the nodes list them), each with the key of its geometry (and of its
// picture: another picture of the same size is another body).
std::map<std::string, std::string> derived_keys(const Document& scratch, const json& nodes, std::vector<std::string>& order,
                                                const std::map<std::string, json>& pictures) {
  std::set<std::string> seen;
  each_body(nodes, [&](const json& n) {
    if (seen.insert(n.value("key", "")).second) order.push_back(n.value("key", ""));
  });
  std::vector<TopoDS_Shape> shapes;
  for (const auto& from : order) shapes.push_back(body_shape(scratch, from));
  const std::vector<std::string> keys = asset_keys(shapes);
  std::map<std::string, std::string> out;
  for (size_t i = 0; i < order.size(); ++i) {
    const auto it = pictures.find(order[i]);
    out[order[i]] = it == pictures.end() ? keys[i] : sha256_hex(keys[i] + "|" + sha256_hex(it->second.dump()));
  }
  return out;
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

// What a KiCad board shows depends on its 3D models too: their names and contents as this machine finds them (a model
// changes, is downloaded or goes missing without the board changing). Empty for any other kind.
std::string models_of(const fs::path& file, const json& asset, const AssetOptions& opt) {
  if (asset.value("kind", "") != "kicad_pcb") return {};
  std::vector<std::string> lines;
  for (const auto& m : kicad_models(file, read_options(asset, opt).kicad).value("models", json::array())) {
    std::string line = m.value("name", "") + "|";
    try {
      if (m.contains("file")) line += file_sha256(path_from_utf8(m["file"].get<std::string>()));
    } catch (const std::exception&) {
      line += "?";
    }
    lines.push_back(line);
  }
  std::sort(lines.begin(), lines.end());
  std::string all;
  for (const auto& l : lines) all += l + "\n";
  return sha256_hex(all);
}

// A version of the asset as the content cache knows it: the file's hash, how it is read and (a board) its models'.
std::string content_of(const json& asset, const std::string& sha, const std::string& models) {
  return sha + "|" + asset.value("builder", json::object()).dump() + (models.empty() ? "" : "|" + models);
}

// The file read into a document of its own (one import op, live bodies). Every read is remembered by its content (the
// viewer cache, LRU): reopening is fast, a clone, a branch or a moved folder finds it again, and a file changed since
// still shows the version synced, as the document's features were computed from it. True when the file itself was read.
// `source`: the file a derived one was made from; kicad-cli's STEP of a board gets its parts named after the footprints.
bool read_file(Document& scratch, const fs::path& file, const json& asset, const std::string& content, const AssetOptions& opt, bool store = true,
               const fs::path& source = {}) {
  const ImportOptions o = read_options(asset, opt);
  if (opt.cache && detail::asset_cache_load(scratch, content, o)) return false;
  import_file(scratch, file, o);
  if (const json b = asset.value("builder", json::object()); !source.empty() && b.value("name", "") == "kicad-cli")
    kicad_label_export(scratch.ops.back().data, scratch, source, b.value("options", json::object()));
  if (opt.cache && store) detail::asset_cache_store(scratch, content);
  return true;
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

// A linked picture's bytes stay in its file: the read's rasters give up their data (`href`), which goes with the body entry
// (never saved) and reaches the scene from there. Body in the read -> its picture.
std::map<std::string, json> take_pictures(json& nodes) {
  std::map<std::string, json> out;
  each_body(nodes, [&](json& n) {
    if (n.contains("raster") && n["raster"].is_object() && n["raster"].contains("href")) {
      out[n.value("key", "")] = n["raster"]["href"];
      n["raster"].erase("href");
    }
  });
  return out;
}

json body_meta(const Document& scratch, const std::string& from, const std::string& import_id, const std::map<std::string, json>& pictures = {}) {
  const BodyEntry* e = scratch.body(from);
  json meta = e ? e->meta : json::object();
  meta["asset"] = import_id;
  if (const auto it = pictures.find(from); it != pictures.end()) meta["href"] = it->second;
  return meta;
}

// `stale`: read from a file that is not the one synced, so the shape is not what the key stood for when the document's
// features were computed (a sync must not keep such a key: what depends on it has to be computed again).
void bind_body(Document& doc, const Document& scratch, const std::string& from, const std::string& key, const std::string& import_id,
               const std::map<std::string, json>& pictures, bool stale = false) {
  json meta = body_meta(scratch, from, import_id, pictures);
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

// An asset read through another file (asset.derived): a board exported to STEP by kicad-cli, a file a converter turns into
// one OPAD reads. The source is what is watched and synced; the shapes come from the derived file.
const json* derived_of(const json& asset) { return asset.contains("derived") && asset["derived"].is_object() ? &asset["derived"] : nullptr; }

std::string builder_name(const json& derived) { return derived.value("builder", json::object()).value("name", std::string("a converter")); }

// The derived file as this machine finds it: where recorded, if in the project or OPAD's own cache.
fs::path locate_derived(const Document& doc, const json& derived, const AssetOptions& opt) {
  const fs::path f = locate_asset(doc, derived, opt);
  return f.empty() || asset_trusted(doc, f, opt) || inside(f, cache_dir(), false) ? f : fs::path();
}

// A derived file made anew from the source (missing here, or the source changed): the hook runs the converter.
fs::path derive_again(const json& asset, const fs::path& source, const AssetOptions& opt) {
  const json& dv = *derived_of(asset);
  if (!opt.derive) throw Error("the file is read through " + builder_name(dv) + ", which is not available here: " + dv.value("path", dv.value("abs", std::string())));
  if (opt.progress && !opt.progress(-1, "converting")) throw Error("cancelled");
  fs::path made = opt.derive(asset, source, opt.progress);
  std::error_code ec;
  if (made.empty() || !fs::is_regular_file(made, ec)) throw Error(builder_name(dv) + " made nothing from " + utf8(source.filename()));
  return fs::absolute(made).lexically_normal();
}

void place_derived(json& dv, const fs::path& file, const std::string& sha, const fs::path& dir) {
  dv["abs"] = utf8(file);
  if (const std::string rel = relative_to(file, dir); !rel.empty()) dv["path"] = rel;
  else dv.erase("path");
  dv["sha256"] = sha;
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
      st.models = models_of(st.file, a, opt);
      st.state = st.sha256 == a.value("sha256", "") ? "ok" : "changed";
      if (st.state == "ok" && a.contains("models_sha256") && a.value("models_sha256", "") != st.models) {
        st.state = "changed";
        st.reason = "its 3D models changed";
      }
      if (const json* dv = derived_of(a)) {
        st.derived = locate_derived(doc, *dv, opt);
        if (!st.derived.empty()) st.derived_sha256 = file_sha256(st.derived);
        if (st.derived.empty() && !opt.derive) {
          st.state = "missing";
          st.reason = "the file read in its place is missing and " + builder_name(*dv) + " is not available here: " + dv->value("path", dv->value("abs", std::string()));
        } else if (st.state == "ok" && st.derived.empty() && st.unbound == 0) {
          st.reason = "the file read in its place is missing: it is made again when it is read next";  // its shapes are here: nothing to sync
        } else if (st.state == "ok" && st.derived_sha256 != dv->value("sha256", "")) {
          st.state = "changed";
          st.reason = st.derived.empty() ? "the file read in its place is missing: it is made again" : "the file read in its place changed";
        }
      }
    } catch (const Standard_Failure& ex) {
      st.state = "error";
      st.reason = ex.GetMessageString();
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
  if (!derived.empty()) j["derived"] = utf8(derived);
  if (unbound) j["unbound"] = unbound;
  return j;
}

std::string file_sha256(const fs::path& file, bool compute) {
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
  if (!compute) return {};
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
        // else the file as it is now, its shapes marked stale. An asset read through a derived file reads that one: as
        // long as it is the one synced, it is the version synced whatever the source became.
        const json& asset = e.data()["asset"];
        const json* dv = derived_of(asset);
        const json& view = dv ? *dv : asset;
        Document scratch = Document::create();
        bool stale = dv ? st.derived_sha256 != dv->value("sha256", "") : st.state == "changed";
        const std::string synced = dv ? content_of(view, dv->value("sha256", ""), "") : content_of(asset, asset.value("sha256", ""), asset.value("models_sha256", ""));
        if (stale && opt.cache && detail::asset_cache_load(scratch, synced, read_options(view, opt))) {
          stale = false;
          st.reason = "the version synced is shown (remembered); sync to take the file as it is now";
        } else if (dv) {
          if (st.derived.empty()) {
            st.derived = derive_again(asset, st.file, opt);
            st.derived_sha256 = file_sha256(st.derived);
          }
          stale = st.derived_sha256 != dv->value("sha256", "");
          read_file(scratch, st.derived, view, content_of(view, st.derived_sha256, ""), opt, true, st.file);
        } else {
          read_file(scratch, st.file, asset, content_of(asset, st.sha256, st.models), opt);
        }
        json nodes = read_op(scratch)["nodes"];
        const std::map<std::string, json> pictures = take_pictures(nodes);
        // In a changed file, a part whose geometry gives the key it had is the part synced, not stale.
        std::vector<std::string> order;
        std::map<std::string, std::string> derived;  // body in the read -> its key
        if (stale) derived = derived_keys(scratch, nodes, order, pictures);
        relabel(nodes, e.op->id, "", &nodes_of(e.data()), stale ? &derived : nullptr);
        std::map<std::string, std::string> from;  // node id -> body in the read
        each_body(nodes, [&](const json& n) { from[n.value("id", "")] = n.value("key", ""); });
        st.unbound = 0;
        int changed = 0;
        // Bound by place, under the keys the import names.
        each_body(nodes_of(e.data()), [&](const json& n) {
          const std::string key = n.value("key", "");
          if (doc.has_body(key)) return;
          const auto it = from.find(n.value("id", ""));
          if (it == from.end()) return void(++st.unbound);
          const bool differs = stale && derived[it->second] != key;
          changed += differs;
          bind_body(doc, scratch, it->second, key, e.op->id, pictures, differs);
        });
        if (stale) st.reason = changed ? std::to_string(changed) + " parts differ from the version synced" : "the parts are as synced";
        if (st.unbound) st.reason = std::to_string(st.unbound) + " parts are no longer in the file" + (changed ? ", " + std::to_string(changed) + " differ" : "");
        // Made again from the file synced (a clone; kicad-cli writes the time into its STEP): the same parts, nothing to sync.
        if (dv && !changed && !st.unbound && st.sha256 == asset.value("sha256", "") && st.models == asset.value("models_sha256", std::string())) {
          st.state = "ok";
          st.reason = "made again from the file synced";
        }
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

namespace {

// link_file and link_derived: `derived` (with its `builder`) is the file read in the source's place, or empty.
ImportResult link(Document& doc, const fs::path& file, const fs::path& derived, const json& builder, const ImportOptions& opt) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) throw Error("file not found: " + utf8(file.filename()));
  if (!derived.empty() && !fs::is_regular_file(derived, ec)) throw Error("file not found: " + utf8(derived.filename()));
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
  const std::string models = models_of(abs, asset, read);
  if (!models.empty()) asset["models_sha256"] = models;
  json data;
  if (!derived.empty()) {
    const fs::path made = fs::absolute(derived).lexically_normal();
    json dv = {{"kind", kind_of(made)}, {"builder", builder.is_object() ? builder : json{{"name", "a converter"}}}};
    place_derived(dv, made, file_sha256(made), doc_dir(doc));
    read_file(scratch, made, dv, content_of(dv, dv["sha256"], ""), read, true, abs);
    asset["derived"] = dv;
    data = read_op(scratch);
  } else {
    const std::string first = content_of(asset, sha, models);
    const bool fresh = read_file(scratch, abs, asset, first, read, false);
    data = read_op(scratch);
    if (kind == "kicad_pcb" && data.contains("kicad") && data["kicad"].contains("origin"))
      asset["builder"]["options"]["origin_at"] = data["kicad"]["origin"];  // every later read keeps this frame
    if (const std::string content = content_of(asset, sha, models); fresh || content != first) detail::asset_cache_store(scratch, content);
  }
  asset["synced"] = now_iso8601();
  const std::string id = new_uuid();
  relabel(data["nodes"], id, "");
  ImportResult res;
  const std::map<std::string, json> pictures = take_pictures(data["nodes"]);
  std::vector<std::string> order;
  std::map<std::string, std::string> keys = derived_keys(scratch, data["nodes"], order, pictures);  // body in the read -> its key
  each_node(data["nodes"], [&](json& n) {
    if (n.value("type", "") != "body") return void(++res.components);
    ++res.bodies;
    n["key"] = keys[n.value("key", "")];
  });
  for (const auto& from : order) bind_body(doc, scratch, from, keys[from], id, pictures);
  data["op"] = "import";
  data["id"] = id;
  data["asset"] = asset;
  if (!opt.parent.empty()) data["parent"] = opt.parent;
  res.op_id = doc.append(data, opt.author).id;
  res.info = {{"linked", true}, {"sha256", sha}, {"kind", kind}};
  return res;
}

}  // namespace

ImportResult link_file(Document& doc, const fs::path& file, const ImportOptions& opt) {
  if (!opt.kicad.kicad_cli || kind_of(file) != "kicad_pcb") return link(doc, file, {}, json(), opt);
  // KiCad's own export of the board, linked: the board is watched and synced, the STEP made from it read (UI-73).
  const json options = kicad_export_options(file, opt.kicad);
  const fs::path step = kicad_cli_export(file, options, opt.progress);
  ImportOptions o = opt;
  o.kicad.kicad_cli = false;
  return link(doc, file, step, {{"name", "kicad-cli"}, {"version", 1}, {"kicad", kicad_cli(true).version}, {"options", options}}, o);
}

fs::path derive_asset(const json& asset, const fs::path& source, const std::function<bool(double, const std::string&)>& progress) {
  const json builder = asset.value("derived", json::object()).value("builder", json::object());
  if (builder.value("name", "") == "kicad-cli") return kicad_cli_export(source, builder.value("options", json::object()), progress);
  throw Error("the file is read through " + builder.value("name", std::string("a converter")) + ", which OPAD cannot run");
}

ImportResult link_derived(Document& doc, const fs::path& file, const fs::path& derived, const json& builder, const ImportOptions& opt) {
  return link(doc, file, derived, builder, opt);
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
  if (!file.empty() && !derived_of(asset))  // Replace may bring another kind of file (a drawing for a STEP): read as that
    if (const std::string kind = kind_of(where); kind != asset.value("kind", "")) {
      asset["kind"] = kind;
      asset.erase("models_sha256");
    }
  auto place = [&](json& a) {
    a["abs"] = utf8(where);
    if (const std::string rel = relative_to(where, doc_dir(doc)); !rel.empty()) a["path"] = rel;
    else a.erase("path");
  };
  const std::string models = models_of(where, asset, opt);
  const bool same = sha == asset.value("sha256", "") && (!asset.contains("models_sha256") || models == asset.value("models_sha256", ""));
  const json* dv = derived_of(asset);
  fs::path made = dv ? locate_derived(doc, *dv, opt) : fs::path();
  design::Plan plan;
  if (same && (!dv || (!made.empty() && file_sha256(made) == dv->value("sha256", "")))) {
    plan.report = {{"import", id}, {"sha256", sha}, {"up_to_date", true}};  // the file synced last: at most it moved
    if (!file.empty() && utf8(where) != asset.value("abs", "")) {
      place(asset);
      plan.ops.push_back(design::make_edit_op(id, {{"asset", asset}}));
    }
    return plan;
  }
  Document scratch = Document::create();
  if (dv) {
    // Made again from the source by the converter when it is here; without it only a derived file changed by hand is read.
    const json view = *dv;
    if (opt.derive || made.empty() || !same) made = derive_again(asset, where, opt);
    const std::string derived_sha = file_sha256(made);
    read_file(scratch, made, view, content_of(view, derived_sha, ""), opt, true, where);
    place_derived(asset["derived"], made, derived_sha, doc_dir(doc));
  } else {
    read_file(scratch, where, asset, content_of(asset, sha, models), opt);
  }
  json fresh = read_op(scratch);
  const std::map<std::string, json> pictures = take_pictures(fresh["nodes"]);
  std::vector<std::string> order;
  std::map<std::string, std::string> keys = derived_keys(scratch, fresh["nodes"], order, pictures);
  relabel(fresh["nodes"], id, "", &nodes_of(data), &keys);
  // A body whose geometry did not change keeps its key (no needless regeneration downstream): the key of its geometry is
  // the one its place had, or the shape its place had is loaded, not stale and the same to the kernel's noise.
  std::map<std::string, std::string> was;  // node id -> key
  std::map<std::string, std::string> names;
  each_body(nodes_of(data), [&](const json& n) { was[n.value("id", "")] = n.value("key", ""); names[n.value("id", "")] = n.value("name", ""); });
  std::map<std::string, std::set<std::string>> before;  // body in the read -> the keys its places had
  each_body(fresh["nodes"], [&](const json& n) {
    if (const auto it = was.find(n.value("id", "")); it != was.end()) before[n.value("key", "")].insert(it->second);
  });
  int kept = 0;
  for (size_t i = 0; i < order.size(); ++i) {
    if (opt.progress && !opt.progress(double(i + 1) / double(order.size()), "comparing")) throw Error("cancelled");
    const std::string& from = order[i];
    std::string& key = keys[from];
    const auto it = before.find(from);
    if (it == before.end()) continue;
    if (it->second.count(key)) {
      ++kept;
      continue;
    }
    const std::string& old = *it->second.begin();
    if (it->second.size() == 1 && !pictures.count(from) && doc.has_body(old) && !doc.body(old)->meta.value("stale", false)) {
      try {
        if (design::same_shapes(body_shape(scratch, from), body_shape(doc, old))) key = old, ++kept;
      } catch (const Standard_Failure&) {
      } catch (const std::exception&) {
      }
    }
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
  if (!models.empty()) asset["models_sha256"] = models;
  asset["synced"] = now_iso8601();
  place(asset);
  fresh["asset"] = asset;
  for (const auto& [k, v] : data.items())  // what the reader no longer says (warnings) goes
    if (!fresh.contains(k) && k != "op" && k != "id" && k != "ts" && k != "by" && k != "parent") fresh[k] = nullptr;
  Document staged = doc;  // the plan's walk reads the new bodies from it
  std::vector<design::NewBody> bodies;
  std::set<std::string> staging;
  for (const auto& from : order) {
    const std::string& key = keys[from];
    // A key shown stale holds another version's shape (the shared cache gets the right one now: it is that key's geometry).
    if ((doc.has_body(key) && !doc.body(key)->meta.value("stale", false)) || !staging.insert(key).second) continue;
    const TopoDS_Shape shape = body_shape(scratch, from);
    const json meta = body_meta(scratch, from, id, pictures);
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
  struct Work { TopoDS_Shape shape; json meta; std::string brep, error; bool healed = false; };
  std::vector<Work> work(keys.size());
  std::atomic<bool> stop{false};
  OSD_Parallel::For(0, static_cast<int>(keys.size()), [&](int i) {
    if (stop || (cancel && cancel())) { stop = true; return; }
    Work& w = work[static_cast<size_t>(i)];
    try {
      w.meta = doc.body(keys[static_cast<size_t>(i)])->meta;
      w.shape = body_shape(doc, keys[static_cast<size_t>(i)]);
      w.brep = detail::persist_body(w.shape, w.meta, w.healed);
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
    json meta = work[i].meta;
    meta.erase("asset");
    meta.erase("href");
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
  each_body(nodes, [&](json& n) {
    if (const json& meta = doc.body(n.value("key", ""))->meta; meta.contains("href") && n.contains("raster") && n["raster"].is_object())
      n["raster"]["href"] = meta["href"];  // a picture's bytes come into the document with it
    n["key"] = renamed[n.value("key", "")];
  });
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
  if (const json* dv = derived_of(asset)) {  // the file read in its place too: a clone opens without the converter
    const fs::path made = locate_asset(doc, *dv, any);
    if (made.empty()) throw Error("the file read in its place is missing: sync it first");
    const fs::path to = inside(made, folder, false) ? made : folder / ".opad" / made.filename();
    copy(made, to);
    place_derived(asset["derived"], to, dv->value("sha256", ""), dir);
  }
  const std::string id = e.op->id;  // appending may move the log's ops
  doc.append(design::make_edit_op(id, {{"asset", asset}}), author);
  return {{"import", id}, {"path", asset["path"]}, {"copied", copied}};
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
