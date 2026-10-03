// Readers for the formats beyond STEP and the 2D drawings: STL, PLY and 3MF are parsed here directly (they are plain
// triangle lists, and a dedicated parser is several times faster than a generic one); IGES, glTF/GLB, OBJ and VRML go
// through OCCT's readers into an XCAF document and the shared assembly walk; BREP is OCCT's own text.
#include <BRep_Builder.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Poly_Triangulation.hxx>
#include <RWGltf_CafReader.hxx>
#include <RWObj_CafReader.hxx>
#include <Standard_Failure.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Iterator.hxx>
#include <VrmlAPI_CafReader.hxx>
#include <XCAFApp_Application.hxx>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "import_common.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"

namespace opad::detail {
namespace {

std::string lower_extension(const std::filesystem::path& file) {
  std::string e = file.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return e;
}

std::string read_binary(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw Error("cannot open " + file.string());
  in.seekg(0, std::ios::end);
  const auto size = static_cast<size_t>(in.tellg());
  in.seekg(0);
  std::string data(size, '\0');
  if (size && !in.read(data.data(), static_cast<std::streamsize>(size))) throw Error("cannot read " + file.string());
  return data;
}

// Paths for OCCT's readers: UTF-8, which they widen on Windows (a file named in Arabic did not open from the ANSI form).
std::string utf8_path(const std::filesystem::path& file) {
  const auto text = file.u8string();
  return std::string(text.begin(), text.end());
}

// The next "vertex" keyword of an ASCII STL (not part of a longer word such as a solid's name).
const char* find_vertex(const char* p, const char* end) {
  const std::string_view text(p, static_cast<size_t>(end - p));
  for (size_t at = text.find("vertex"); at != std::string_view::npos; at = text.find("vertex", at + 6)) {
    const bool before = at == 0 || std::isspace(static_cast<unsigned char>(text[at - 1]));
    const bool after = at + 6 < text.size() && std::isspace(static_cast<unsigned char>(text[at + 6]));
    if (before && after) return p + at;
  }
  return nullptr;
}

void report(const ImportOptions& opt, double fraction, const std::string& what) {
  if (opt.progress && !opt.progress(fraction, what)) throw Error("import cancelled");
}

// OCCT readers report through a progress indicator; it forwards the overall position and polls for cancellation.
class ReaderProgress : public Message_ProgressIndicator {
 public:
  ReaderProgress(const ImportOptions& opt, std::string what) : m_opt(opt), m_what(std::move(what)) {}
  bool cancelled = false;
  void Show(const Message_ProgressScope&, const Standard_Boolean) override {
    if (m_opt.progress && !m_opt.progress(std::min(1.0, GetPosition()), m_what)) cancelled = true;
  }
  Standard_Boolean UserBreak() override { return cancelled; }

 private:
  const ImportOptions& m_opt;
  std::string m_what;
};

// Skips spaces and parses one number; false at the end of the text or on something that is not a number.
bool next_number(const char*& p, const char* end, double& out) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) ++p;
  if (p >= end) return false;
  if (*p == '+') ++p;
  const auto r = std::from_chars(p, end, out);
  if (r.ec != std::errc()) return false;
  p = r.ptr;
  return std::isfinite(out) && std::abs(out) < 1e12;
}

// The import op for a file whose bodies sit under one component named after the file.
ImportResult append_import(Document& doc, const std::filesystem::path& file, const ImportOptions& opt, json children, ImportResult result) {
  json root = {{"type", "component"}, {"id", new_uuid()}, {"name", file.stem().string()}, {"children", std::move(children)}};
  json op = {{"op", "import"}, {"source", file.filename().string()}, {"units", "mm"}, {"nodes", json::array({root})}};
  if (!opt.parent.empty()) op["parent"] = opt.parent;
  result.op_id = doc.append(op, opt.author).id;
  ++result.components;
  return result;
}

json mesh_body(Document& doc, const TopoDS_Shape& face, const std::string& name, const std::filesystem::path& file, const ImportOptions& opt,
               ImportResult& result, const std::array<double, 3>* color = nullptr, const json& faceColors = {}) {
  json meta = {{"name", name}, {"units", "mm"}, {"source", file.filename().string()}, {"representation", "mesh"}};
  if (color) meta["color"] = {(*color)[0], (*color)[1], (*color)[2]};
  if (!faceColors.is_null()) meta["face_colors"] = faceColors;
  json body = {{"type", "body"}, {"id", new_uuid()}, {"name", name}, {"representation", "mesh"}};
  body["key"] = store_body(doc, face, meta, opt, true, &result);
  if (color) body["color"] = meta["color"];
  ++result.bodies;
  return body;
}

// ---------------------------------------------------------------- 3MF: a zip of XML parts
class Zip {
 public:
  explicit Zip(std::string data) : m_data(std::move(data)) {
    const size_t n = m_data.size();
    if (n < 22) throw Error("not a 3MF file (no zip directory)");
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > (n > 65557 ? n - 65557 : 0);)
      if (u32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) throw Error("not a 3MF file (no zip directory)");
    uint64_t count = u16(eocd + 10), offset = u32(eocd + 16);
    if ((count == 0xFFFF || offset == 0xFFFFFFFF) && eocd >= 20 && u32(eocd - 20) == 0x07064b50) {  // zip64
      const uint64_t z = u64(eocd - 20 + 8);
      if (z + 56 > n || u32(z) != 0x06064b50) throw Error("damaged 3MF zip directory");
      count = u64(z + 32);
      offset = u64(z + 48);
    }
    size_t at = static_cast<size_t>(offset);
    for (uint64_t i = 0; i < count; ++i) {
      if (at + 46 > n || u32(at) != 0x02014b50) throw Error("damaged 3MF zip directory");
      Entry e;
      e.method = u16(at + 10);
      e.csize = u32(at + 20);
      e.usize = u32(at + 24);
      const size_t nameLen = u16(at + 28), extraLen = u16(at + 30), commentLen = u16(at + 32);
      e.local = u32(at + 42);
      if (at + 46 + nameLen + extraLen > n) throw Error("damaged 3MF zip directory");
      std::string name = m_data.substr(at + 46, nameLen);
      for (size_t x = at + 46 + nameLen, xe = x + extraLen; x + 4 <= xe;) {  // zip64 sizes and offset
        const size_t id = u16(x), len = u16(x + 2);
        if (id == 0x0001) {
          size_t f = x + 4;
          if (e.usize == 0xFFFFFFFF && f + 8 <= xe) { e.usize = u64(f); f += 8; }
          if (e.csize == 0xFFFFFFFF && f + 8 <= xe) { e.csize = u64(f); f += 8; }
          if (e.local == 0xFFFFFFFF && f + 8 <= xe) e.local = u64(f);
        }
        x += 4 + len;
      }
      if (!name.empty() && name.front() == '/') name.erase(0, 1);
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      m_entries[name] = e;
      at += 46 + nameLen + extraLen + commentLen;
    }
  }
  bool has(std::string name) const { return m_entries.count(normal(std::move(name))) > 0; }
  std::string read(std::string name) const {
    auto it = m_entries.find(normal(name));
    if (it == m_entries.end()) throw Error("3MF part missing: " + name);
    const Entry& e = it->second;
    const size_t at = static_cast<size_t>(e.local);
    if (at + 30 > m_data.size() || u32(at) != 0x04034b50) throw Error("damaged 3MF part: " + name);
    const size_t start = at + 30 + u16(at + 26) + u16(at + 28);
    if (start + e.csize > m_data.size() || e.usize > (uint64_t(1) << 33)) throw Error("damaged 3MF part: " + name);
    if (e.method == 0) return m_data.substr(start, static_cast<size_t>(e.csize));
    if (e.method != 8) throw Error("3MF part uses an unsupported compression: " + name);
    std::string out(static_cast<size_t>(e.usize), '\0');
    z_stream s{};
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) throw Error("cannot decompress 3MF part: " + name);
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(m_data.data() + start));
    s.avail_in = static_cast<uInt>(e.csize);
    s.next_out = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    const int r = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    if (r != Z_STREAM_END) throw Error("cannot decompress 3MF part: " + name);
    out.resize(s.total_out);
    return out;
  }

 private:
  struct Entry { uint64_t csize = 0, usize = 0, local = 0; int method = 0; };
  static std::string normal(std::string name) {
    if (!name.empty() && name.front() == '/') name.erase(0, 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name;
  }
  uint32_t u16(size_t at) const { return uint8_t(m_data[at]) | uint32_t(uint8_t(m_data[at + 1])) << 8; }
  uint32_t u32(size_t at) const { return u16(at) | u16(at + 2) << 16; }
  uint64_t u64(size_t at) const { return u32(at) | uint64_t(u32(at + 4)) << 32; }
  std::string m_data;
  std::map<std::string, Entry> m_entries;
};

// A minimal XML tag scanner: 3MF needs element names and attributes only, and its meshes run to millions of tags, which a
// DOM parser would hold as objects.
struct XmlTag {
  std::string_view name;  // without a namespace prefix
  bool closing = false, empty = false;
  std::string_view attrs;
  std::string_view attr(std::string_view key) const {
    size_t i = 0;
    while (i < attrs.size()) {
      while (i < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[i]))) ++i;
      const size_t eq = attrs.find('=', i);
      if (eq == std::string_view::npos) return {};
      std::string_view k = attrs.substr(i, eq - i);
      while (!k.empty() && std::isspace(static_cast<unsigned char>(k.back()))) k.remove_suffix(1);
      if (const size_t colon = k.find(':'); colon != std::string_view::npos) k.remove_prefix(colon + 1);
      size_t q = eq + 1;
      while (q < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[q]))) ++q;
      if (q >= attrs.size() || (attrs[q] != '"' && attrs[q] != '\'')) return {};
      const size_t close = attrs.find(attrs[q], q + 1);
      if (close == std::string_view::npos) return {};
      if (k == key) return attrs.substr(q + 1, close - q - 1);
      i = close + 1;
    }
    return {};
  }
};

class XmlScanner {
 public:
  explicit XmlScanner(std::string_view text) : m_text(text) {}
  bool next(XmlTag& tag) {
    while (true) {
      const size_t lt = m_text.find('<', m_at);
      if (lt == std::string_view::npos) return false;
      if (m_text.compare(lt, 4, "<!--") == 0) {
        const size_t e = m_text.find("-->", lt);
        m_at = e == std::string_view::npos ? m_text.size() : e + 3;
        continue;
      }
      const size_t gt = m_text.find('>', lt);
      if (gt == std::string_view::npos) return false;
      m_at = gt + 1;
      if (m_text[lt + 1] == '?' || m_text[lt + 1] == '!') continue;
      std::string_view body = m_text.substr(lt + 1, gt - lt - 1);
      tag = XmlTag{};
      if (!body.empty() && body.front() == '/') { tag.closing = true; body.remove_prefix(1); }
      if (!body.empty() && body.back() == '/') { tag.empty = true; body.remove_suffix(1); }
      size_t n = 0;
      while (n < body.size() && !std::isspace(static_cast<unsigned char>(body[n]))) ++n;
      tag.name = body.substr(0, n);
      if (const size_t colon = tag.name.find(':'); colon != std::string_view::npos) tag.name.remove_prefix(colon + 1);
      tag.attrs = body.substr(n);
      return true;
    }
  }

 private:
  std::string_view m_text;
  size_t m_at = 0;
};

double attr_number(const XmlTag& tag, std::string_view key, double fallback = 0) {
  const std::string_view v = tag.attr(key);
  if (v.empty()) return fallback;
  double out = fallback;
  const char* p = v.data();
  if (!next_number(p, v.data() + v.size(), out)) return fallback;
  return out;
}

std::string xml_unescape(std::string_view v) {
  std::string out;
  out.reserve(v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] != '&') { out += v[i]; continue; }
    const size_t semi = v.find(';', i);
    if (semi == std::string_view::npos) { out += v[i]; continue; }
    const std::string_view e = v.substr(i + 1, semi - i - 1);
    if (e == "amp") out += '&';
    else if (e == "lt") out += '<';
    else if (e == "gt") out += '>';
    else if (e == "quot") out += '"';
    else if (e == "apos") out += '\'';
    else { out += v.substr(i, semi - i + 1); }
    i = semi;
  }
  return out;
}

bool parse_hex_color(std::string_view v, std::array<double, 3>& rgb, double* alpha = nullptr) {
  if (v.size() < 7 || v[0] != '#') return false;
  auto byte = [&](size_t at) { unsigned x = 0; std::from_chars(v.data() + at, v.data() + at + 2, x, 16); return x / 255.0; };
  rgb = {byte(1), byte(3), byte(5)};
  if (alpha) *alpha = v.size() >= 9 ? byte(7) : 1.0;
  return true;
}

// A 3MF transform ("m00 m01 m02 m10 m11 m12 m20 m21 m22 m30 m31 m32", row vectors) as OPAD's column form, translation
// in millimetres.
Mat4 parse_3mf_transform(std::string_view v, double scale) {
  Mat4 m;
  if (v.empty()) return m;
  double a[12];
  const char* p = v.data();
  for (double& x : a)
    if (!next_number(p, v.data() + v.size(), x)) return Mat4{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) m.at(r, c) = a[c * 3 + r];
    m.at(r, 3) = a[9 + r] * scale;
  }
  return m;
}

struct Model3mf {
  struct Component { std::string object, path; Mat4 transform; };
  struct Object {
    std::string name;
    std::vector<float> xyz;
    std::vector<uint32_t> triangles;
    std::vector<Component> components;
    bool has_color = false;  // the object's own material (pid, pindex)
    std::array<double, 3> color{};
    double opacity = 1;
    std::vector<int> paint;  // per triangle: one of `colors` (its material, a slicer's painting), -1 none; empty when none has one
  };
  std::map<std::string, Object> objects;
  std::vector<Component> build;
  std::vector<std::pair<std::array<double, 3>, double>> colors;  // colour, opacity
};

// The filament a slicer painted a triangle with, by area: PrusaSlicer's mmu_segmentation and Bambu Studio's paint_color are
// the same bitstream (TriangleSelector), in hex, last digit first. A digit's low two bits count the sides a triangle was split
// at (its children follow), else its high two bits are the state (3: the next digit + 3). State n > 0 is extruder n.
int painted_state(std::string_view hex) {
  std::vector<int> digits;
  for (auto it = hex.rbegin(); it != hex.rend(); ++it) {
    const char c = *it;
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) return 0;
    digits.push_back(d);
  }
  std::map<int, double> area;
  std::vector<double> open{1.0};  // shares of the triangles still to read, depth first (siblings share alike)
  for (size_t at = 0; at < digits.size() && !open.empty();) {
    const double share = open.back();
    open.pop_back();
    const int code = digits[at++], split = code & 3;
    if (split) {
      open.insert(open.end(), size_t(split + 1), share / (split + 1));
      continue;
    }
    int state = code >> 2;
    if (state == 3 && at < digits.size()) state = 3 + digits[at++];
    area[state] += share;
  }
  int best = 0;
  double most = 0;
  for (const auto& [state, share] : area)
    if (share > most + 1e-12) { best = state; most = share; }
  return best;
}

Model3mf parse_3mf_model(std::string_view text, const ImportOptions& opt, double& scale, const std::vector<std::array<double, 3>>& filaments) {
  Model3mf model;
  std::map<std::string, std::vector<std::pair<std::array<double, 3>, double>>> palettes;  // basematerials / colorgroup id -> colours
  std::string palette;
  Model3mf::Object* object = nullptr;
  std::string objectPid, objectIndex;
  auto intern = [&](const std::array<double, 3>& rgb, double opacity) {
    for (size_t i = 0; i < model.colors.size(); ++i)
      if (model.colors[i].first == rgb && model.colors[i].second == opacity) return static_cast<int>(i);
    model.colors.push_back({rgb, opacity});
    return static_cast<int>(model.colors.size() - 1);
  };
  auto material = [&](std::string_view pid, std::string_view index) -> const std::pair<std::array<double, 3>, double>* {
    auto it = palettes.find(std::string(pid));
    size_t i = 0;
    if (!index.empty()) std::from_chars(index.data(), index.data() + index.size(), i);
    return it != palettes.end() && i < it->second.size() ? &it->second[i] : nullptr;
  };
  auto finishObject = [&] {
    if (!object) return;
    if (const auto* own = objectPid.empty() ? nullptr : material(objectPid, objectIndex)) {
      object->has_color = true;
      object->color = own->first;
      object->opacity = own->second;
    }
    object = nullptr;
  };
  XmlScanner scan(text);
  XmlTag tag;
  size_t tags = 0;
  while (scan.next(tag)) {
    if ((++tags & 0x3FFFF) == 0) report(opt, -1, "translating meshes");
    const std::string_view name = tag.name;
    if (tag.closing) {
      if (name == "object") finishObject();
      if (name == "basematerials" || name == "colorgroup") palette.clear();
      continue;
    }
    if (name == "vertex" && object) {
      object->xyz.push_back(static_cast<float>(attr_number(tag, "x") * scale));
      object->xyz.push_back(static_cast<float>(attr_number(tag, "y") * scale));
      object->xyz.push_back(static_cast<float>(attr_number(tag, "z") * scale));
    } else if (name == "triangle" && object) {
      const double nv = static_cast<double>(object->xyz.size() / 3);
      const double a = attr_number(tag, "v1", -1), b = attr_number(tag, "v2", -1), c = attr_number(tag, "v3", -1);
      if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv) throw Error("3MF triangle refers to a missing vertex");
      object->triangles.insert(object->triangles.end(), {static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c)});
      // Its own material (pid, p1; either falls back to the object's), or the filament a slicer painted it with.
      int colour = -1;
      if (std::string_view painted = tag.attr("paint_color").empty() ? tag.attr("mmu_segmentation") : tag.attr("paint_color"); !painted.empty()) {
        if (const int state = painted_state(painted); state > 0 && size_t(state) <= filaments.size()) colour = intern(filaments[size_t(state - 1)], 1);
      } else if (!tag.attr("pid").empty() || !tag.attr("p1").empty()) {
        const std::string_view pid = tag.attr("pid").empty() ? std::string_view(objectPid) : tag.attr("pid");
        if (const auto* m = material(pid, tag.attr("p1").empty() ? std::string_view(objectIndex) : tag.attr("p1"))) colour = intern(m->first, m->second);
      }
      if (colour >= 0 || !object->paint.empty()) {
        object->paint.resize(object->triangles.size() / 3 - 1, -1);
        object->paint.push_back(colour);
      }
    } else if (name == "model") {
      const std::string_view unit = tag.attr("unit");
      scale = unit == "micron" ? 0.001 : unit == "centimeter" ? 10.0 : unit == "inch" ? 25.4 : unit == "foot" ? 304.8 : unit == "meter" ? 1000.0 : 1.0;
    } else if (name == "basematerials" || name == "colorgroup") {
      palette = std::string(tag.attr("id"));
      palettes[palette];
    } else if ((name == "base" || name == "color") && !palette.empty()) {
      std::array<double, 3> rgb{0.75, 0.75, 0.78};
      double alpha = 1;
      parse_hex_color(tag.attr(name == "base" ? "displaycolor" : "color"), rgb, &alpha);
      palettes[palette].push_back({rgb, std::clamp(alpha, 0.05, 1.0)});
    } else if (name == "object") {
      finishObject();
      object = &model.objects[std::string(tag.attr("id"))];
      object->name = xml_unescape(tag.attr("name"));
      objectPid = std::string(tag.attr("pid"));
      objectIndex = std::string(tag.attr("pindex"));
      if (tag.empty) finishObject();
    } else if (name == "component" && object) {
      object->components.push_back({std::string(tag.attr("objectid")), std::string(tag.attr("path")), parse_3mf_transform(tag.attr("transform"), scale)});
    } else if (name == "item") {
      model.build.push_back({std::string(tag.attr("objectid")), std::string(tag.attr("path")), parse_3mf_transform(tag.attr("transform"), scale)});
    }
  }
  finishObject();
  return model;
}

}  // namespace

// ---------------------------------------------------------------- triangle meshes
TopoDS_Face mesh_face(const std::vector<float>& xyz, const std::vector<uint32_t>& triangles, bool weld) {
  return TopoDS::Face(mesh_faces(xyz, triangles, weld, {}, {}));
}

TopoDS_Shape mesh_faces(const std::vector<float>& xyz, const std::vector<uint32_t>& triangles, bool weld, const std::vector<int>& colour,
                        const std::vector<std::array<double, 3>>& colours, json* meta) {
  const size_t nt = triangles.size() / 3;
  std::vector<float> welded;
  std::vector<uint32_t> weldedTriangles;
  const std::vector<float>* points = &xyz;
  const std::vector<uint32_t>* tris = &triangles;
  if (weld) {
    struct Key {
      uint32_t x, y, z;
      bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct Hash {
      size_t operator()(const Key& k) const { return (size_t(k.x) * 73856093u) ^ (size_t(k.y) * 19349663u) ^ (size_t(k.z) * 83492791u); }
    };
    auto bits = [](float f) { if (f == 0.0f) f = 0.0f; uint32_t b; std::memcpy(&b, &f, 4); return b; };  // -0 == +0
    std::unordered_map<Key, uint32_t, Hash> index;
    index.reserve(xyz.size() / 9 + 16);
    std::vector<uint32_t> remap(xyz.size() / 3);
    welded.reserve(xyz.size() / 3);
    for (size_t v = 0; v < remap.size(); ++v) {
      const Key k{bits(xyz[v * 3]), bits(xyz[v * 3 + 1]), bits(xyz[v * 3 + 2])};
      auto [it, added] = index.emplace(k, static_cast<uint32_t>(welded.size() / 3));
      if (added) welded.insert(welded.end(), {xyz[v * 3], xyz[v * 3 + 1], xyz[v * 3 + 2]});
      remap[v] = it->second;
    }
    weldedTriangles.resize(triangles.size());
    for (size_t i = 0; i < triangles.size(); ++i) weldedTriangles[i] = remap[triangles[i]];
    points = &welded;
    tris = &weldedTriangles;
  }
  const size_t nv = points->size() / 3;
  const auto& P = *points;
  const auto& T = *tris;
  // Face normals, area weighted.
  std::vector<double> fn(nt * 3);
  for (size_t t = 0; t < nt; ++t) {
    const float* a = &P[T[t * 3] * 3];
    const float* b = &P[T[t * 3 + 1] * 3];
    const float* c = &P[T[t * 3 + 2] * 3];
    const double u[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
    const double w[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
    fn[t * 3] = u[1] * w[2] - u[2] * w[1];
    fn[t * 3 + 1] = u[2] * w[0] - u[0] * w[2];
    fn[t * 3 + 2] = u[0] * w[1] - u[1] * w[0];
  }
  // Corners per vertex, then each vertex's corners grouped by direction: one output node (with its own normal) per group.
  std::vector<uint32_t> start(nv + 1, 0), corners(nt * 3);
  for (size_t i = 0; i < nt * 3; ++i) ++start[T[i] + 1];
  for (size_t v = 0; v < nv; ++v) start[v + 1] += start[v];
  {
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (size_t i = 0; i < nt * 3; ++i) corners[fill[T[i]]++] = static_cast<uint32_t>(i);
  }
  const double crease = std::cos(30.0 * M_PI / 180.0);
  std::vector<uint32_t> cornerNode(nt * 3);
  std::vector<float> nodes, normals;
  nodes.reserve(nv * 3);
  normals.reserve(nv * 3);
  struct Group { double rep[3]; double sum[3]; };
  std::vector<Group> groups;
  for (size_t v = 0; v < nv; ++v) {
    groups.clear();
    const size_t base = nodes.size() / 3;
    for (uint32_t k = start[v]; k < start[v + 1]; ++k) {
      const uint32_t c = corners[k], t = c / 3;
      const double* n = &fn[t * 3];
      const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      size_t g = groups.size();
      if (len <= 0) g = groups.empty() ? groups.size() : 0;  // a degenerate facet joins any group
      else
        for (size_t i = 0; i < groups.size(); ++i)
          if ((n[0] * groups[i].rep[0] + n[1] * groups[i].rep[1] + n[2] * groups[i].rep[2]) / len >= crease) { g = i; break; }
      if (g == groups.size()) {
        Group fresh{{0, 0, 1}, {0, 0, 0}};
        if (len > 0) for (int i = 0; i < 3; ++i) fresh.rep[i] = n[i] / len;
        groups.push_back(fresh);
      }
      for (int i = 0; i < 3; ++i) groups[g].sum[i] += n[i];
      cornerNode[c] = static_cast<uint32_t>(base + g);
    }
    for (const Group& g : groups) {
      nodes.insert(nodes.end(), {P[v * 3], P[v * 3 + 1], P[v * 3 + 2]});
      double len = std::sqrt(g.sum[0] * g.sum[0] + g.sum[1] * g.sum[1] + g.sum[2] * g.sum[2]);
      const double* d = len > 0 ? g.sum : g.rep;
      if (len <= 0) len = 1;
      normals.insert(normals.end(), {static_cast<float>(d[0] / len), static_cast<float>(d[1] / len), static_cast<float>(d[2] / len)});
    }
  }
  const int nodeCount = static_cast<int>(nodes.size() / 3);
  if (nodeCount == 0 || nt == 0) throw Error("the mesh has no triangles");
  // One face per colour, the body's own first, shaded as one mesh (the normals above saw every triangle), so no seam shows
  // where the colour changes.
  std::vector<std::vector<uint32_t>> parts;  // triangles by colour + 1
  if (colour.size() == nt) {
    parts.resize(colours.size() + 1);
    for (size_t t = 0; t < nt; ++t) parts[colour[t] >= 0 && size_t(colour[t]) < colours.size() ? size_t(colour[t]) + 1 : 0].push_back(uint32_t(t));
    if (parts[0].size() == nt) parts.clear();
  }
  BRep_Builder builder;
  auto face = [&](const std::vector<uint32_t>* only) {  // every triangle when null
    const int count = only ? static_cast<int>(only->size()) : static_cast<int>(nt);
    std::vector<int> local;
    if (only) {
      local.assign(size_t(nodeCount), 0);
      for (uint32_t t : *only)
        for (int k = 0; k < 3; ++k) local[cornerNode[t * 3 + k]] = 1;
      int next = 0;
      for (int& l : local) l = l ? ++next : 0;
    }
    const int used = only ? (local.empty() ? 0 : *std::max_element(local.begin(), local.end())) : nodeCount;
    Handle(Poly_Triangulation) mesh = new Poly_Triangulation(used, count, Standard_False, Standard_True);
    for (int i = 0; i < nodeCount; ++i) {
      const int at = only ? local[size_t(i)] : i + 1;
      if (at == 0) continue;
      mesh->SetNode(at, gp_Pnt(nodes[i * 3], nodes[i * 3 + 1], nodes[i * 3 + 2]));
      mesh->SetNormal(at, gp_Vec3f(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]));
    }
    auto node = [&](uint32_t corner) { return only ? local[cornerNode[corner]] : static_cast<int>(cornerNode[corner]) + 1; };
    for (int i = 0; i < count; ++i) {
      const uint32_t t = only ? (*only)[size_t(i)] : uint32_t(i);
      mesh->SetTriangle(i + 1, Poly_Triangle(node(t * 3), node(t * 3 + 1), node(t * 3 + 2)));
    }
    TopoDS_Face f;
    builder.MakeFace(f, mesh);
    return f;
  };
  if (parts.empty()) return face(nullptr);
  TopoDS_Compound all;
  builder.MakeCompound(all);
  FaceColors painted;
  painted.colors = colours;
  for (size_t c = 0; c < parts.size(); ++c) {
    if (parts[c].empty()) continue;
    builder.Add(all, face(&parts[c]));
    painted.face.push_back(static_cast<int>(c) - 1);
  }
  if (meta)
    if (json j = painted.to_json(); !j.is_null()) (*meta)["face_colors"] = std::move(j);
  return painted.face.size() == 1 ? TopoDS_Iterator(all).Value() : TopoDS_Shape(all);
}

ImportResult import_stl(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) {
  report(opt, -1, "reading");
  const std::string data = read_binary(file);
  std::vector<float> xyz;
  std::vector<uint32_t> triangles;
  const size_t size = data.size();
  uint32_t count = 0;
  if (size >= 84) std::memcpy(&count, data.data() + 80, 4);
  const size_t expected = 84 + size_t(count) * 50;
  // Binary when the facet count matches the size; some writers pad the end, and some put "solid" in a binary header.
  const bool ascii = data.compare(0, 5, "solid") == 0 && data.find("facet", 0) != std::string::npos && data.find("facet") < 4096;
  const bool binary = count > 0 && (size == expected || (size > expected && size - expected < 4096 && !ascii));
  if (binary) {
    if (count > 50000000) throw Error("the STL has too many triangles (" + std::to_string(count) + ")");
    xyz.resize(size_t(count) * 9);
    triangles.resize(size_t(count) * 3);
    for (uint32_t i = 0; i < count; ++i) {
      std::memcpy(&xyz[size_t(i) * 9], data.data() + 84 + size_t(i) * 50 + 12, 36);
      for (int k = 0; k < 3; ++k) triangles[size_t(i) * 3 + k] = i * 3 + k;
      if ((i & 0xFFFFF) == 0xFFFFF) report(opt, double(i) / count, "translating triangles");
    }
    for (float f : xyz)
      if (!std::isfinite(f) || std::abs(f) > 1e12f) throw Error("invalid mesh vertex in " + file.filename().string());
  } else {
    if (data.find("vertex") == std::string::npos) throw Error("not an STL file, or an empty one: " + file.filename().string());
    const char* p = data.data();
    const char* end = p + size;
    while (true) {
      const char* hit = find_vertex(p, end);
      if (!hit) break;
      p = hit + 6;
      double v[3];
      for (double& x : v)
        if (!next_number(p, end, x)) throw Error("invalid STL vertex in " + file.filename().string());
      xyz.insert(xyz.end(), {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])});
      if ((xyz.size() & 0x3FFFF) == 0) report(opt, double(p - data.data()) / size, "translating triangles");
    }
    if (xyz.empty() || xyz.size() % 9) throw Error("invalid or truncated STL: " + file.filename().string());
    triangles.resize(xyz.size() / 3);
    for (size_t i = 0; i < triangles.size(); ++i) triangles[i] = static_cast<uint32_t>(i);
  }
  report(opt, 0.0, "building");
  ImportResult result;
  json children = json::array({mesh_body(doc, mesh_face(xyz, triangles, true), "Mesh", file, opt, result)});
  return append_import(doc, file, opt, std::move(children), result);
}

// ---------------------------------------------------------------- PLY
ImportResult import_ply(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) {
  report(opt, -1, "reading");
  const std::string data = read_binary(file);
  const size_t headerEnd = data.find("end_header");
  if (data.compare(0, 3, "ply") != 0 || headerEnd == std::string::npos) throw Error("not a PLY file: " + file.filename().string());
  size_t body = data.find('\n', headerEnd);
  if (body == std::string::npos) throw Error("truncated PLY header");
  ++body;
  enum class Format { Ascii, Little, Big } format = Format::Ascii;
  struct Prop { std::string name; std::string type, countType; bool list = false; };
  struct Element { std::string name; size_t count = 0; std::vector<Prop> props; };
  std::vector<Element> elements;
  {
    std::istringstream header(data.substr(0, headerEnd));
    std::string line;
    while (std::getline(header, line)) {
      std::istringstream ls(line);
      std::string word;
      ls >> word;
      if (word == "format") {
        std::string f;
        ls >> f;
        format = f == "binary_little_endian" ? Format::Little : f == "binary_big_endian" ? Format::Big : Format::Ascii;
      } else if (word == "element") {
        Element e;
        ls >> e.name >> e.count;
        elements.push_back(e);
      } else if (word == "property" && !elements.empty()) {
        Prop p;
        ls >> p.type;
        if (p.type == "list") { p.list = true; ls >> p.countType >> p.type; }
        ls >> p.name;
        elements.back().props.push_back(p);
      }
    }
  }
  auto size_of = [](const std::string& t) -> int {
    if (t == "char" || t == "uchar" || t == "int8" || t == "uint8") return 1;
    if (t == "short" || t == "ushort" || t == "int16" || t == "uint16") return 2;
    if (t == "int" || t == "uint" || t == "int32" || t == "uint32" || t == "float" || t == "float32") return 4;
    if (t == "double" || t == "float64") return 8;
    throw Error("unsupported PLY property type: " + t);
  };
  const char* p = data.data() + body;
  const char* end = data.data() + data.size();
  // One value of a binary property, as a double.
  auto binary = [&](const std::string& t) -> double {
    const int n = size_of(t);
    if (p + n > end) throw Error("truncated PLY data");
    unsigned char b[8];
    std::memcpy(b, p, n);
    p += n;
    if (format == Format::Big) std::reverse(b, b + n);
    if (t == "char" || t == "int8") return static_cast<int8_t>(b[0]);
    if (t == "uchar" || t == "uint8") return b[0];
    if (t == "short" || t == "int16") { int16_t v; std::memcpy(&v, b, 2); return v; }
    if (t == "ushort" || t == "uint16") { uint16_t v; std::memcpy(&v, b, 2); return v; }
    if (t == "int" || t == "int32") { int32_t v; std::memcpy(&v, b, 4); return v; }
    if (t == "uint" || t == "uint32") { uint32_t v; std::memcpy(&v, b, 4); return v; }
    if (t == "float" || t == "float32") { float v; std::memcpy(&v, b, 4); return v; }
    double v;
    std::memcpy(&v, b, 8);
    return v;
  };
  auto value = [&](const std::string& t) -> double {
    if (format != Format::Ascii) return binary(t);
    double v;
    if (!next_number(p, end, v)) throw Error("invalid or truncated PLY data");
    return v;
  };
  std::vector<float> xyz;
  std::vector<uint32_t> triangles;
  constexpr uint32_t kNone = 0xFFFFFFFFu;
  std::vector<uint32_t> vertexRgb, triangleRgb;  // 0xRRGGBB (0-255 per channel) or kNone
  auto packed = [](const double* rgb) {
    if (rgb[0] < 0 || rgb[1] < 0 || rgb[2] < 0) return kNone;
    auto byte = [](double x) { return uint32_t(std::clamp(std::lround(x), 0l, 255l)); };
    return byte(rgb[0]) << 16 | byte(rgb[1]) << 8 | byte(rgb[2]);
  };
  for (const Element& e : elements) {
    const bool vertex = e.name == "vertex", face = e.name == "face";
    for (size_t i = 0; i < e.count; ++i) {
      double v[3] = {0, 0, 0}, rgb[3] = {-1, -1, -1};
      const size_t firstTriangle = triangles.size() / 3;
      for (const Prop& prop : e.props) {
        if (prop.list) {
          const size_t n = static_cast<size_t>(value(prop.countType));
          if (n > 1000) throw Error("invalid PLY face");
          uint32_t first = 0, previous = 0;
          for (size_t k = 0; k < n; ++k) {
            const double x = value(prop.type);
            if (!face || (prop.name != "vertex_indices" && prop.name != "vertex_index")) continue;
            if (x < 0 || x >= static_cast<double>(xyz.size() / 3)) throw Error("PLY face refers to a missing vertex");
            const auto index = static_cast<uint32_t>(x);
            if (k == 0) first = index;
            else if (k >= 2) triangles.insert(triangles.end(), {first, previous, index});  // a fan over the polygon
            previous = index;
          }
          continue;
        }
        const double x = value(prop.type);
        if (!vertex && !face) continue;
        // Colours as bytes; float ones (0-1) are scaled.
        const double c = prop.type == "float" || prop.type == "float32" || prop.type == "double" || prop.type == "float64" ? x * 255 : x;
        if (prop.name == "x") v[0] = x;
        else if (prop.name == "y") v[1] = x;
        else if (prop.name == "z") v[2] = x;
        else if (prop.name == "red" || prop.name == "diffuse_red") rgb[0] = c;
        else if (prop.name == "green" || prop.name == "diffuse_green") rgb[1] = c;
        else if (prop.name == "blue" || prop.name == "diffuse_blue") rgb[2] = c;
      }
      if (vertex) {
        if (!std::isfinite(v[0] + v[1] + v[2]) || std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])}) > 1e12) throw Error("invalid PLY vertex");
        xyz.insert(xyz.end(), {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])});
        vertexRgb.push_back(packed(rgb));
      } else if (face) {
        triangleRgb.resize(triangles.size() / 3, kNone);
        if (const uint32_t own = packed(rgb); own != kNone) std::fill(triangleRgb.begin() + std::ptrdiff_t(firstTriangle), triangleRgb.end(), own);
      }
      if ((i & 0x3FFFF) == 0x3FFFF) report(opt, double(p - data.data()) / data.size(), "translating " + e.name + "s");
    }
  }
  if (triangles.empty()) throw Error("the PLY file has no faces (point clouds are not shown)");
  report(opt, 0.0, "building");
  // A triangle's colour: its face's, else the one most of its corners have. A few colours (a part coloured by region) become
  // the body's (the commonest) and faces of their own; a scan's colours, which vary point by point, their average.
  const size_t nt = triangles.size() / 3;
  triangleRgb.resize(nt, kNone);
  std::map<uint32_t, size_t> counts;
  for (size_t t = 0; t < nt; ++t) {
    uint32_t& own = triangleRgb[t];
    if (own == kNone) {
      const uint32_t a = vertexRgb[triangles[t * 3]], b = vertexRgb[triangles[t * 3 + 1]], c = vertexRgb[triangles[t * 3 + 2]];
      own = b == c ? b : a;
    }
    if (own != kNone) ++counts[own];
  }
  auto unpack = [](uint32_t c) { return std::array<double, 3>{((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0}; };
  ImportResult result;
  std::array<double, 3> color{};
  bool colored = !counts.empty();
  std::vector<int> colour;
  std::vector<std::array<double, 3>> colours;
  json meta = json::object();
  if (counts.size() > 64) {
    double sum[3] = {0, 0, 0};
    size_t n = 0;
    for (const auto& [c, count] : counts) {
      const auto rgb = unpack(c);
      for (int k = 0; k < 3; ++k) sum[k] += rgb[size_t(k)] * double(count);
      n += count;
    }
    for (int k = 0; k < 3; ++k) color[size_t(k)] = sum[k] / double(n);
  } else if (colored) {
    const uint32_t common = std::max_element(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    color = unpack(common);
    std::map<uint32_t, int> index;
    for (const auto& [c, count] : counts)
      if (c != common) { index[c] = static_cast<int>(colours.size()); colours.push_back(unpack(c)); }
    colour.resize(nt, -1);
    for (size_t t = 0; t < nt; ++t)
      if (auto it = index.find(triangleRgb[t]); it != index.end()) colour[t] = it->second;
  }
  const TopoDS_Shape shape = mesh_faces(xyz, triangles, false, colour, colours, &meta);
  json mesh = mesh_body(doc, shape, "Mesh", file, opt, result, colored ? &color : nullptr, meta.value("face_colors", json()));
  return append_import(doc, file, opt, json::array({mesh}), result);
}

// ---------------------------------------------------------------- 3MF
ImportResult import_3mf(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) {
  report(opt, -1, "reading");
  const Zip zip(read_binary(file));
  std::string root = "3D/3dmodel.model";
  if (zip.has("_rels/.rels")) {
    const std::string rels = zip.read("_rels/.rels");
    XmlScanner scan(rels);
    XmlTag tag;
    while (scan.next(tag))
      if (tag.name == "Relationship" && tag.attr("Type").size() >= 8 && tag.attr("Type").substr(tag.attr("Type").size() - 8) == "3dmodel")
        root = std::string(tag.attr("Target"));
  }
  if (!root.empty() && root.front() == '/') root.erase(0, 1);
  // What slicers keep apart from the model (Bambu Studio, PrusaSlicer, OrcaSlicer): object names, the extruder of each object,
  // of a Bambu object's parts (its components) and of a Prusa object's volumes (triangle ranges of its mesh), and the
  // project's filament colours, which show each part and painted triangle as the slicer does.
  std::map<std::string, std::string> settingNames;
  struct Extruders { int own = 0; std::map<std::string, int> parts; std::vector<std::array<int, 3>> volumes; };  // volumes: first, last, extruder
  std::map<std::string, Extruders> extruders;  // by root object id
  for (const char* config : {"Metadata/model_settings.config", "Metadata/Slic3r_PE_model.config"}) {
    if (!zip.has(config)) continue;
    const std::string text = zip.read(config);
    XmlScanner scan(text);
    XmlTag tag;
    std::string object, part;
    int depth = 0;
    while (scan.next(tag)) {
      if (tag.name == "object") {
        if (tag.closing) { object.clear(); depth = 0; }
        else { object = std::string(tag.attr("id")); depth = 0; }
      } else if (tag.name == "part" || tag.name == "volume") {
        depth += tag.closing ? -1 : (tag.empty ? 0 : 1);
        if (tag.closing || object.empty()) continue;
        part = std::string(tag.attr("id"));
        if (tag.name == "volume") extruders[object].volumes.push_back({int(attr_number(tag, "firstid", -1)), int(attr_number(tag, "lastid", -2)), 0});
      } else if (tag.name == "metadata" && !object.empty() && tag.attr("key") == "extruder") {
        const int extruder = int(attr_number(tag, "value"));
        auto& e = extruders[object];
        if (depth == 0) e.own = extruder;
        else if (tag.attr("type") == "volume" && !e.volumes.empty()) e.volumes.back()[2] = extruder;
        else if (!part.empty()) e.parts[part] = extruder;
      } else if (tag.name == "metadata" && !object.empty() && depth == 0 && tag.attr("key") == "name" && !settingNames.count(object)) {
        settingNames[object] = xml_unescape(tag.attr("value"));
      }
    }
  }
  std::vector<std::array<double, 3>> filaments;
  if (zip.has("Metadata/project_settings.config")) {  // Bambu Studio, OrcaSlicer: JSON
    try {
      const json settings = json::parse(zip.read("Metadata/project_settings.config"));
      for (const auto& c : settings.value("filament_colour", json::array())) {
        std::array<double, 3> rgb{0.75, 0.75, 0.78};
        if (c.is_string()) parse_hex_color(c.get<std::string>(), rgb);
        filaments.push_back(rgb);
      }
    } catch (const std::exception&) {
    }
  } else if (zip.has("Metadata/Slic3r_PE.config")) {  // PrusaSlicer: "; key = value" lines, an extruder's colour before its filament's
    std::map<std::string, std::vector<std::string>> lists;
    std::istringstream lines(zip.read("Metadata/Slic3r_PE.config"));
    for (std::string line; std::getline(lines, line);)
      for (const char* key : {"extruder_colour", "filament_colour"})
        if (line.rfind(std::string("; ") + key + " = ", 0) == 0) {
          std::istringstream values(line.substr(std::strlen(key) + 5));
          for (std::string v; std::getline(values, v, ';');) {
            v.erase(std::remove_if(v.begin(), v.end(), [](char ch) { return ch == '"' || std::isspace(static_cast<unsigned char>(ch)); }), v.end());
            lists[key].push_back(v);
          }
        }
    for (size_t i = 0; i < std::max(lists["extruder_colour"].size(), lists["filament_colour"].size()); ++i) {
      std::array<double, 3> rgb{0.75, 0.75, 0.78};
      if (i >= lists["extruder_colour"].size() || !parse_hex_color(lists["extruder_colour"][i], rgb))
        if (i < lists["filament_colour"].size()) parse_hex_color(lists["filament_colour"][i], rgb);
      filaments.push_back(rgb);
    }
  }
  std::map<std::string, Model3mf> models;
  std::map<std::string, double> scales;
  auto model = [&](std::string path) -> const Model3mf& {
    if (path.empty()) path = root;
    if (path.front() == '/') path.erase(0, 1);
    auto it = models.find(path);
    if (it != models.end()) return it->second;
    double scale = scales.count(root) ? scales[root] : 1.0;
    Model3mf parsed = parse_3mf_model(zip.read(path), opt, scale, filaments);
    scales[path] = scale;
    return models.emplace(path, std::move(parsed)).first->second;
  };
  model(root);
  ImportResult result;
  // (part, object id, extruder) -> body key, with its colour: shared meshes are instances.
  std::map<std::tuple<std::string, std::string, int>, std::pair<std::string, json>> keys;
  int depthGuard = 0;
  // `extruder`: the slicer's for this object or part (0: none); `owner`: the root object it belongs to.
  std::function<json(const std::string&, const std::string&, const Mat4&, const std::string&, int, const std::string&)> node =
      [&](const std::string& path, const std::string& id, const Mat4& transform, const std::string& fallback, int extruder, const std::string& owner) -> json {
    const Model3mf& m = model(path);
    auto it = m.objects.find(id);
    if (it == m.objects.end()) throw Error("3MF build refers to a missing object " + id);
    const Model3mf::Object& o = it->second;
    std::string name = o.name;
    if (name.empty() && (path.empty() || path == root || path == "/" + root) && settingNames.count(id)) name = settingNames[id];
    if (name.empty()) name = fallback;
    json n;
    if (!o.triangles.empty()) {
      auto& [key, look] = keys[{path, id, extruder}];
      if (key.empty()) {
        report(opt, -1, "building");
        // The body's colour: its own material, else its filament, else the commonest of its triangles'. The triangles
        // coloured otherwise are faces of their own.
        const size_t nt = o.triangles.size() / 3;
        std::vector<std::pair<std::array<double, 3>, double>> colours;
        std::vector<int> colour(nt, -1);
        auto local = [&](const std::pair<std::array<double, 3>, double>& c) {
          const auto at = std::find(colours.begin(), colours.end(), c);
          if (at != colours.end()) return static_cast<int>(at - colours.begin());
          colours.push_back(c);
          return static_cast<int>(colours.size() - 1);
        };
        for (size_t t = 0; t < o.paint.size() && t < nt; ++t)
          if (o.paint[t] >= 0) colour[t] = local(m.colors[size_t(o.paint[t])]);
        if (owner == id && extruders.count(id))
          for (const auto& [first, last, e] : extruders[id].volumes)
            for (int t = std::max(first, 0); e > 0 && size_t(e) <= filaments.size() && t <= last && size_t(t) < nt; ++t)
              if (colour[size_t(t)] < 0) colour[size_t(t)] = local({filaments[size_t(e - 1)], 1.0});
        std::optional<std::pair<std::array<double, 3>, double>> own;
        if (o.has_color) own = {o.color, o.opacity};
        else if (extruder > 0 && size_t(extruder) <= filaments.size()) own = {filaments[size_t(extruder - 1)], 1.0};
        else if (!colours.empty()) {
          std::vector<size_t> count(colours.size() + 1, 0);
          for (int c : colour) ++count[size_t(c + 1)];
          const size_t best = size_t(std::max_element(count.begin() + 1, count.end()) - count.begin());
          if (count[best] >= count[0]) own = colours[best - 1];
        }
        std::vector<std::array<double, 3>> rgb;
        for (const auto& c : colours) rgb.push_back(c.first);
        for (int& c : colour)
          if (c >= 0 && own && colours[size_t(c)].first == own->first) c = -1;
        json meta = {{"name", name}, {"units", "mm"}, {"source", file.filename().string()}, {"representation", "mesh"}};
        look = json::object();
        if (own) meta["color"] = look["color"] = {own->first[0], own->first[1], own->first[2]};
        if (own && own->second < 1.0) look["opacity"] = own->second;
        const TopoDS_Shape shape = mesh_faces(o.xyz, o.triangles, false, colour, rgb, &meta);
        key = store_body(doc, shape, meta, opt, true, &result);
      }
      n = {{"type", "body"}, {"id", new_uuid()}, {"name", name}, {"key", key}, {"representation", "mesh"}};
      for (const auto& [field, value] : look.items()) n[field] = value;
      ++result.bodies;
    } else {
      if (++depthGuard > 64) throw Error("3MF components nest too deeply");
      json children = json::array();
      int i = 0;
      for (const auto& c : o.components) {
        ++i;
        int part = extruder;  // a Bambu part's own filament (its id is the component's object id)
        if (auto e = extruders.find(owner); e != extruders.end())
          if (auto p = e->second.parts.find(c.object); p != e->second.parts.end() && p->second > 0) part = p->second;
        children.push_back(node(c.path.empty() ? path : c.path, c.object, c.transform, name + "." + std::to_string(i), part, owner));
      }
      --depthGuard;
      n = {{"type", "component"}, {"id", new_uuid()}, {"name", name}, {"children", children}};
      ++result.components;
    }
    if (!transform.is_identity()) n["transform"] = transform.to_json();
    return n;
  };
  json children = json::array();
  int item = 0;
  for (const auto& b : model(root).build) {
    int extruder = 0;  // a slicer project's object prints in its extruder's filament, the first unless it says
    if (!filaments.empty()) extruder = extruders.count(b.object) && extruders[b.object].own > 0 ? extruders[b.object].own : 1;
    children.push_back(node(b.path.empty() ? root : b.path, b.object, b.transform, "Object " + std::to_string(++item), extruder, b.object));
  }
  if (children.empty()) throw Error("the 3MF file has nothing to build");
  return append_import(doc, file, opt, std::move(children), result);
}

// ---------------------------------------------------------------- through OCCT's readers
ImportResult import_iges(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) {
  report(opt, -1, "reading");
  IGESCAFControl_Reader reader;
  reader.SetColorMode(Standard_True);
  reader.SetNameMode(Standard_True);
  reader.SetLayerMode(Standard_False);
  Interface_Static::SetCVal("xstep.cascade.unit", "MM");
  try {
    if (reader.ReadFile(utf8_path(file).c_str()) != IFSelect_RetDone) throw Error("IGES read failed (not an IGES file?): " + file.filename().string());
  } catch (const Standard_Failure& e) {
    throw Error(std::string("IGES read failed: ") + e.GetMessageString());
  }
  Handle(TDocStd_Document) xdoc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", xdoc);
  Handle(ReaderProgress) progress = new ReaderProgress(opt, "translating geometry");
  bool ok = false;
  try {
    ok = reader.Transfer(xdoc, progress->Start());
  } catch (const Standard_Failure& e) {
    throw Error(std::string("IGES transfer failed: ") + e.GetMessageString());
  }
  if (progress->cancelled) throw Error("import cancelled");
  if (!ok) throw Error("IGES transfer produced no shapes: " + file.filename().string());
  return import_xcaf(doc, xdoc, file, opt, false);
}

ImportResult import_mesh_scene(Document& doc, const std::filesystem::path& file, const ImportOptions& opt, bool kicad_vrml) {
  report(opt, -1, "reading");
  const std::string ext = lower_extension(file);
  Handle(RWMesh_CafReader) reader;
  if (ext == ".gltf" || ext == ".glb") {
    Handle(RWGltf_CafReader) gltf = new RWGltf_CafReader();
    gltf->SetParallel(Standard_True);
    gltf->SetToSkipLateDataLoading(false);
    reader = gltf;  // glTF is Y-up in metres; the reader knows
  } else if (ext == ".obj") {
    Handle(RWObj_CafReader) obj = new RWObj_CafReader();
    obj->SetSinglePrecision(Standard_True);
    // OBJ has no units or axes. Most writers (Blender, SketchUp, Maya) are Y-up, but OPAD's own export keeps Z up.
    std::ifstream head(file, std::ios::binary);
    std::string first(256, '\0');
    head.read(first.data(), 256);
    obj->SetFileLengthUnit(0.001);
    obj->SetFileCoordinateSystem(first.find("# OPAD") == 0 ? RWMesh_CoordinateSystem_Zup : RWMesh_CoordinateSystem_Yup);
    reader = obj;
  } else {
    reader = new VrmlAPI_CafReader();  // VRML is in metres, Y up; this reader converts neither (see below)
    reader->SetFileLengthUnit(1.0);    // it scales points by this, which is -1 (unknown) unless set: everything came mirrored
  }
  reader->SetSystemLengthUnit(0.001);
  reader->SetSystemCoordinateSystem(RWMesh_CoordinateSystem_Zup);
  Handle(TDocStd_Document) xdoc;
  XCAFApp_Application::GetApplication()->NewDocument("MDTV-XCAF", xdoc);
  reader->SetDocument(xdoc);
  Handle(ReaderProgress) progress = new ReaderProgress(opt, "translating meshes");
  bool ok = false;
  try {
    ok = reader->Perform(TCollection_AsciiString(utf8_path(file).c_str()), progress->Start());
  } catch (const Standard_Failure& e) {
    throw Error("cannot read " + file.filename().string() + ": " + e.GetMessageString());
  }
  if (progress->cancelled) throw Error("import cancelled");
  if (!ok) throw Error("cannot read " + file.filename().string() + " (damaged, or a variant OPAD does not read)");
  if (kicad_vrml) return import_xcaf(doc, xdoc, file, opt, true, 2.54);
  if (ext == ".wrl" || ext == ".vrml") {
    Mat4 yUp;  // file Y -> Z, file Z -> -Y
    yUp.at(1, 1) = 0; yUp.at(1, 2) = -1; yUp.at(2, 1) = 1; yUp.at(2, 2) = 0;
    return import_xcaf(doc, xdoc, file, opt, true, 1000.0, yUp);
  }
  return import_xcaf(doc, xdoc, file, opt, true);
}

ImportResult import_brep_file(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) {
  report(opt, -1, "reading");
  return import_brep(doc, read_text_file(file), file.stem().string(), opt);
}

}  // namespace opad::detail
