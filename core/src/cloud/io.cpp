// Point cloud files (opad/point_cloud.hpp): PLY (ASCII and binary, also the mesh reader's parser), XYZ / PTS / CSV text,
// LAS (uncompressed) and the writers. E57 is in e57.cpp.
#include <OSD_Parallel.hxx>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>

#include "opad/point_cloud.hpp"

namespace opad::cloud {
namespace {

std::string lower_ext(const std::filesystem::path& file) {
  std::string e = file.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return e;
}

std::string read_all(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw Error("cannot open " + path_to_utf8(file.filename()));
  in.seekg(0, std::ios::end);
  const auto size = size_t(in.tellg());
  in.seekg(0);
  std::string data(size, '\0');
  if (size && !in.read(data.data(), std::streamsize(size))) throw Error("cannot read " + path_to_utf8(file.filename()));
  return data;
}

void report(const ReadOptions& o, double f, const std::string& what) {
  if (o.progress && !o.progress(f, what)) throw Error("import cancelled");
}

// Points read as doubles are kept as floats about an offset: the first point, rounded to a metre, when it is far out.
struct Builder {
  Points p;
  bool started = false;
  void add(double x, double y, double z) {
    if (!started) {
      started = true;
      if (std::max({std::abs(x), std::abs(y), std::abs(z)}) > 1e4)
        p.offset = {std::round(x / 1000) * 1000, std::round(y / 1000) * 1000, std::round(z / 1000) * 1000};
    }
    p.xyz.push_back(float(x - p.offset[0]));
    p.xyz.push_back(float(y - p.offset[1]));
    p.xyz.push_back(float(z - p.offset[2]));
  }
};

// The scale to mm and the turn to Z up the options ask for, on top of the format's own `unit` (mm per file unit).
void finish(Points& p, const ReadOptions& o, double unit) {
  p.normalise();
  const double s = o.scale > 0 ? o.scale : unit;
  const bool yUp = o.up == "y" || o.up == "Y";
  if (!o.up.empty() && !yUp && o.up != "z" && o.up != "Z") throw Error("the up axis is \"z\" or \"y\"");
  if (s == 1 && !yUp) return;
  Mat4 m;
  if (yUp) m.at(1, 1) = 0, m.at(1, 2) = -1, m.at(2, 1) = 1, m.at(2, 2) = 0;  // file Y -> Z, file Z -> -Y
  Mat4 scale;
  scale.at(0, 0) = scale.at(1, 1) = scale.at(2, 2) = s;
  const Mat4 all = m * scale;
  p.transform(all);
  p.offset = all.apply(p.offset);
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// ---------------------------------------------------------------- fixed-point text, locale free
void put_fixed(std::string& out, double v, int decimals = 4) {
  if (!std::isfinite(v)) v = 0;
  static const double kScale[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
  const double scaled = std::round(std::abs(v) * kScale[decimals]);
  uint64_t whole = uint64_t(scaled);
  if (v < 0 && whole != 0) out += '-';
  const uint64_t div = uint64_t(kScale[decimals]);
  char buf[32];
  int n = 0;
  uint64_t ip = whole / div, fp = whole % div;
  do { buf[n++] = char('0' + ip % 10); ip /= 10; } while (ip);
  while (n) out += buf[--n];
  if (decimals) {
    out += '.';
    char f[8];
    for (int i = decimals - 1; i >= 0; --i) f[i] = char('0' + fp % 10), fp /= 10;
    out.append(f, size_t(decimals));
  }
}
void put_int(std::string& out, int v) { out += std::to_string(v); }

}  // namespace

// ---------------------------------------------------------------- extensions
const std::vector<std::string>& cloud_extensions() {
  static const std::vector<std::string> list = {".xyz", ".asc", ".pts", ".csv", ".e57", ".las", ".laz"};
  return list;
}

bool is_cloud_extension(const std::filesystem::path& file) {
  const auto e = lower_ext(file);
  const auto& l = cloud_extensions();
  return std::find(l.begin(), l.end(), e) != l.end();
}

bool ply_is_cloud(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::string head(65536, '\0');
  in.read(head.data(), std::streamsize(head.size()));
  head.resize(size_t(in.gcount()));
  const size_t end = head.find("end_header");
  if (head.compare(0, 3, "ply") != 0 || end == std::string::npos) return false;
  std::istringstream lines(head.substr(0, end));
  std::string line;
  bool vertices = false;
  while (std::getline(lines, line)) {
    std::istringstream ls(line);
    std::string word, name;
    size_t count = 0;
    ls >> word >> name >> count;
    if (word != "element") continue;
    if (name == "face" && count > 0) return false;
    if (name == "vertex" && count > 0) vertices = true;
  }
  return vertices;
}

Points read_file(const std::filesystem::path& file, const ReadOptions& o) {
  const auto e = lower_ext(file);
  if (e == ".laz") throw Error("LAZ (compressed LAS) is not read: convert it to LAS or E57 first (LASzip, PDAL or CloudCompare)");
  report(o, -1, "reading");
  const std::string data = read_all(file);
  Points p;
  if (e == ".ply") p = read_ply_data(data, o);
  else if (e == ".e57") p = read_e57_data(data, o);
  else if (e == ".las") p = read_las_data(data, o);
  else if (e == ".xyz" || e == ".pts" || e == ".csv" || e == ".txt" || e == ".asc") p = read_text_data(data, e, o);
  else throw Error("not a point cloud format: " + e);
  if (p.empty()) throw Error("the file has no points: " + path_to_utf8(file.filename()));
  return p;
}

// ---------------------------------------------------------------- PLY
Points read_ply_data(const std::string& data, const ReadOptions& o, std::vector<uint32_t>* triangles, std::vector<uint32_t>* triangleRgb) {
  const size_t headerEnd = data.find("end_header");
  if (data.compare(0, 3, "ply") != 0 || headerEnd == std::string::npos) throw Error("not a PLY file");
  size_t body = data.find('\n', headerEnd);
  if (body == std::string::npos) throw Error("truncated PLY header");
  ++body;
  enum class Format { Ascii, Little, Big } format = Format::Ascii;
  enum class T { I8, U8, I16, U16, I32, U32, F32, F64 };
  enum class Role { None, X, Y, Z, R, G, B, NX, NY, NZ, Scalar };
  struct Prop {
    std::string name;
    T type = T::F32, countType = T::U8;
    bool list = false;
    Role role = Role::None;
  };
  struct Element { std::string name; size_t count = 0; std::vector<Prop> props; };
  std::vector<Element> elements;
  std::string scalarName;
  auto type_of = [](const std::string& t) {
    if (t == "char" || t == "int8") return T::I8;
    if (t == "uchar" || t == "uint8") return T::U8;
    if (t == "short" || t == "int16") return T::I16;
    if (t == "ushort" || t == "uint16") return T::U16;
    if (t == "int" || t == "int32") return T::I32;
    if (t == "uint" || t == "uint32") return T::U32;
    if (t == "float" || t == "float32") return T::F32;
    if (t == "double" || t == "float64") return T::F64;
    throw Error("unsupported PLY property type: " + t);
  };
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
        std::string t;
        ls >> t;
        if (t == "list") {
          std::string ct;
          p.list = true;
          ls >> ct >> t;
          p.countType = type_of(ct);
        }
        p.type = type_of(t);
        ls >> p.name;
        std::string n = p.name;
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (elements.back().name == "vertex" && !p.list) {
          if (n == "x") p.role = Role::X;
          else if (n == "y") p.role = Role::Y;
          else if (n == "z") p.role = Role::Z;
          else if (n == "red" || n == "diffuse_red" || n == "r") p.role = Role::R;
          else if (n == "green" || n == "diffuse_green" || n == "g") p.role = Role::G;
          else if (n == "blue" || n == "diffuse_blue" || n == "b") p.role = Role::B;
          else if (n == "nx" || n == "normal_x") p.role = Role::NX;
          else if (n == "ny" || n == "normal_y") p.role = Role::NY;
          else if (n == "nz" || n == "normal_z") p.role = Role::NZ;
          else if (scalarName.empty() && (n == "intensity" || n == "scalar_intensity" || n.rfind("scalar_", 0) == 0 || n == "quality" || n == "confidence")) {
            p.role = Role::Scalar;
            scalarName = n.rfind("scalar_", 0) == 0 ? n.substr(7) : n;
          }
        }
        elements.back().props.push_back(p);
      }
    }
  }
  const char* p = data.data() + body;
  const char* end = data.data() + data.size();
  const bool big = format == Format::Big;
  auto binary = [&](T t) -> double {
    static const int sizes[] = {1, 1, 2, 2, 4, 4, 4, 8};
    const int n = sizes[int(t)];
    if (p + n > end) throw Error("truncated PLY data");
    unsigned char b[8];
    std::memcpy(b, p, size_t(n));
    p += n;
    if (big) std::reverse(b, b + n);
    switch (t) {
      case T::I8: return int8_t(b[0]);
      case T::U8: return b[0];
      case T::I16: { int16_t v; std::memcpy(&v, b, 2); return v; }
      case T::U16: { uint16_t v; std::memcpy(&v, b, 2); return v; }
      case T::I32: { int32_t v; std::memcpy(&v, b, 4); return v; }
      case T::U32: { uint32_t v; std::memcpy(&v, b, 4); return v; }
      case T::F32: { float v; std::memcpy(&v, b, 4); return v; }
      case T::F64: { double v; std::memcpy(&v, b, 8); return v; }
    }
    return 0;
  };
  auto value = [&](T t) -> double {
    if (format != Format::Ascii) return binary(t);
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end) throw Error("invalid or truncated PLY data");
    if (*p == '+') ++p;
    double v;
    const char* after = parse_double(p, end, v);
    if (!after) throw Error("invalid or truncated PLY data");
    p = after;
    return v;
  };
  Builder out;
  out.p.scalar_name = scalarName;
  bool colour = false, normals = false, scalar = false;
  for (const Element& e : elements)
    if (e.name == "vertex")
      for (const Prop& pr : e.props) {
        colour = colour || pr.role == Role::R;
        normals = normals || pr.role == Role::NX;
        scalar = scalar || pr.role == Role::Scalar;
      }
  constexpr uint32_t kNone = 0xFFFFFFFFu;
  for (const Element& e : elements) {
    const bool vertex = e.name == "vertex", face = e.name == "face";
    if (vertex) {
      if (e.count > 400'000'000) throw Error("the PLY file has too many points");
      out.p.xyz.reserve(e.count * 3);
      if (colour) out.p.rgb.reserve(e.count * 3);
      if (normals) out.p.normals.reserve(e.count * 3);
      if (scalar) out.p.scalar.reserve(e.count);
    }
    // A float colour property (0-1) is scaled to bytes.
    for (size_t i = 0; i < e.count; ++i) {
      double v[3] = {0, 0, 0}, rgb[3] = {-1, -1, -1}, n[3] = {0, 0, 0}, s = 0;
      const size_t firstTriangle = triangles ? triangles->size() / 3 : 0;
      for (const Prop& prop : e.props) {
        if (prop.list) {
          const double count = value(prop.countType);
          if (count < 0 || count > 1000) throw Error("invalid PLY face");
          uint32_t first = 0, previous = 0;
          const bool indices = face && (prop.name == "vertex_indices" || prop.name == "vertex_index");
          for (size_t k = 0; k < size_t(count); ++k) {
            const double x = value(prop.type);
            if (!indices || !triangles) continue;
            if (x < 0 || x >= double(out.p.size())) throw Error("PLY face refers to a missing vertex");
            const auto index = uint32_t(x);
            if (k == 0) first = index;
            else if (k >= 2) triangles->insert(triangles->end(), {first, previous, index});  // a fan over the polygon
            previous = index;
          }
          continue;
        }
        const double x = value(prop.type);
        const bool flt = prop.type == T::F32 || prop.type == T::F64;
        if (face) {
          std::string nm = prop.name;
          if (nm == "red" || nm == "diffuse_red") rgb[0] = flt ? x * 255 : x;
          else if (nm == "green" || nm == "diffuse_green") rgb[1] = flt ? x * 255 : x;
          else if (nm == "blue" || nm == "diffuse_blue") rgb[2] = flt ? x * 255 : x;
          continue;
        }
        if (!vertex) continue;
        switch (prop.role) {
          case Role::X: v[0] = x; break;
          case Role::Y: v[1] = x; break;
          case Role::Z: v[2] = x; break;
          case Role::R: rgb[0] = flt ? x * 255 : prop.type == T::U16 ? x / 257 : x; break;
          case Role::G: rgb[1] = flt ? x * 255 : prop.type == T::U16 ? x / 257 : x; break;
          case Role::B: rgb[2] = flt ? x * 255 : prop.type == T::U16 ? x / 257 : x; break;
          case Role::NX: n[0] = x; break;
          case Role::NY: n[1] = x; break;
          case Role::NZ: n[2] = x; break;
          case Role::Scalar: s = x; break;
          default: break;
        }
      }
      if (vertex) {
        if (!std::isfinite(v[0] + v[1] + v[2]) || std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2])}) > 1e12) throw Error("invalid PLY vertex");
        out.add(v[0], v[1], v[2]);
        if (colour)
          for (double c : rgb) out.p.rgb.push_back(uint8_t(std::clamp(std::lround(c < 0 ? 0 : c), 0l, 255l)));
        if (normals) {
          const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          for (double c : n) out.p.normals.push_back(float(l > 0 && std::isfinite(l) ? c / l : 0));
        }
        if (scalar) out.p.scalar.push_back(float(s));
      } else if (face && triangleRgb && triangles) {
        triangleRgb->resize(triangles->size() / 3, kNone);
        if (rgb[0] >= 0 && rgb[1] >= 0 && rgb[2] >= 0) {
          auto byte = [](double c) { return uint32_t(std::clamp(std::lround(c), 0l, 255l)); };
          const uint32_t own = byte(rgb[0]) << 16 | byte(rgb[1]) << 8 | byte(rgb[2]);
          std::fill(triangleRgb->begin() + std::ptrdiff_t(firstTriangle), triangleRgb->end(), own);
        }
      }
      if ((i & 0x3FFFF) == 0x3FFFF) report(o, double(p - data.data()) / double(data.size()), "translating " + e.name + "s");
    }
  }
  if (!triangles || triangles->empty()) finish(out.p, o, 1.0);
  else out.p.normalise();
  return std::move(out.p);
}

std::string write_ply_data(const Points& p, bool binary) {
  std::string out;
  out += "ply\nformat ";
  out += binary ? "binary_little_endian 1.0\n" : "ascii 1.0\n";
  out += "comment written by OPAD\n";
  if (p.offset != Vec3{0, 0, 0}) {
    out += "comment offset ";
    for (double v : p.offset) put_fixed(out, v), out += ' ';
    out += "(mm, added to every point)\n";
  }
  const bool distant = p.offset != Vec3{0, 0, 0};
  out += "element vertex " + std::to_string(p.size()) + "\n";
  const char* coord = distant ? "double" : "float";
  out += std::string("property ") + coord + " x\nproperty " + coord + " y\nproperty " + coord + " z\n";
  if (p.colored()) out += "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  if (p.has_normals()) out += "property float nx\nproperty float ny\nproperty float nz\n";
  const std::string sname = p.scalar_name.empty() ? "scalar" : p.scalar_name;
  if (p.has_scalar()) out += "property float scalar_" + sname + "\n";
  out += "end_header\n";
  const size_t n = p.size();
  if (binary) {
    const size_t rec = (distant ? 24 : 12) + (p.colored() ? 3 : 0) + (p.has_normals() ? 12 : 0) + (p.has_scalar() ? 4 : 0);
    const size_t at = out.size();
    out.resize(at + rec * n);
    char* w = out.data() + at;
    for (size_t i = 0; i < n; ++i) {
      if (distant) {
        for (size_t k = 0; k < 3; ++k) {
          const double v = double(p.xyz[3 * i + k]) + p.offset[k];
          std::memcpy(w, &v, 8), w += 8;
        }
      } else {
        std::memcpy(w, &p.xyz[3 * i], 12), w += 12;
      }
      if (p.colored()) std::memcpy(w, &p.rgb[3 * i], 3), w += 3;
      if (p.has_normals()) std::memcpy(w, &p.normals[3 * i], 12), w += 12;
      if (p.has_scalar()) std::memcpy(w, &p.scalar[i], 4), w += 4;
    }
    return out;
  }
  for (size_t i = 0; i < n; ++i) {
    for (size_t k = 0; k < 3; ++k) put_fixed(out, double(p.xyz[3 * i + k]) + p.offset[k]), out += k < 2 ? ' ' : '\0';
    out.pop_back();
    if (p.colored())
      for (size_t k = 0; k < 3; ++k) out += ' ', put_int(out, p.rgb[3 * i + k]);
    if (p.has_normals())
      for (size_t k = 0; k < 3; ++k) out += ' ', put_fixed(out, p.normals[3 * i + k], 5);
    if (p.has_scalar()) out += ' ', put_fixed(out, p.scalar[i], 5);
    out += '\n';
  }
  return out;
}

// ---------------------------------------------------------------- XYZ / PTS / CSV
Points read_text_data(const std::string& data, const std::string& extension, const ReadOptions& o) {
  const char* begin = data.data();
  const char* end = begin + data.size();
  // The lines to look at first: the header (names, or a PTS count) and how many numbers a line has.
  enum Role { None, X, Y, Z, R, G, B, NX, NY, NZ, S };
  std::vector<int> roles;
  std::string scalarName;
  const char* first = begin;  // first data line
  auto line_end = [&](const char* q) {
    const void* nl = std::memchr(q, '\n', size_t(end - q));
    return nl ? static_cast<const char*>(nl) : end;
  };
  auto split = [](std::string_view line) {
    std::vector<std::string_view> f;
    size_t i = 0;
    while (i < line.size()) {
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == ',' || line[i] == ';' || line[i] == '\r')) ++i;
      size_t j = i;
      while (j < line.size() && !(line[j] == ' ' || line[j] == '\t' || line[j] == ',' || line[j] == ';' || line[j] == '\r')) ++j;
      if (j > i) f.push_back(line.substr(i, j - i));
      i = j;
    }
    return f;
  };
  auto number = [](std::string_view s, double& v) {
    const char* a = s.data();
    if (*a == '+') ++a;
    const char* e = parse_double(a, s.data() + s.size(), v);
    return e == s.data() + s.size();
  };
  std::vector<std::vector<double>> sample;
  size_t columns = 0;
  for (const char* q = begin; q < end && sample.size() < 200;) {
    const char* e = line_end(q);
    std::string_view line(q, size_t(e - q));
    const auto f = split(line);
    const char* next = e < end ? e + 1 : end;
    if (f.empty() || line.find('#') == 0 || line.rfind("//", 0) == 0) {
      if (sample.empty()) first = next;
      q = next;
      continue;
    }
    std::vector<double> nums;
    bool all = true;
    for (auto s : f) {
      double v;
      if (!number(s, v)) { all = false; break; }
      nums.push_back(v);
    }
    if (!all) {
      if (!sample.empty()) throw Error("a line of the point file is not numbers: " + std::string(line.substr(0, 80)));
      // A header row: column names.
      roles.clear();
      for (auto s : f) {
        std::string n(s);
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        n.erase(std::remove(n.begin(), n.end(), '"'), n.end());
        int r = None;
        if (n == "x" || n == "//x") r = X;
        else if (n == "y") r = Y;
        else if (n == "z") r = Z;
        else if (n == "r" || n == "red") r = R;
        else if (n == "g" || n == "green") r = G;
        else if (n == "b" || n == "blue") r = B;
        else if (n == "nx" || n == "normal_x" || n == "normalx") r = NX;
        else if (n == "ny" || n == "normal_y" || n == "normaly") r = NY;
        else if (n == "nz" || n == "normal_z" || n == "normalz") r = NZ;
        else if (scalarName.empty() && n != "") r = S, scalarName = n == "i" ? "intensity" : n;
        roles.push_back(r);
      }
      first = next;
      q = next;
      continue;
    }
    // A PTS count line (or any lone whole number before the points).
    if (sample.empty() && nums.size() == 1 && nums[0] == std::floor(nums[0])) {
      first = next;
      q = next;
      continue;
    }
    if (!columns) columns = nums.size();
    sample.push_back(std::move(nums));
    q = next;
  }
  if (sample.empty()) throw Error("the file has no points");
  if (roles.empty()) {
    if (columns < 3) throw Error("a point line needs at least x, y and z");
    auto bytes = [&](size_t c) {  // whole numbers 0-255 in this column on every sampled line
      for (const auto& s : sample)
        if (c >= s.size() || s[c] < 0 || s[c] > 255 || s[c] != std::floor(s[c])) return false;
      return true;
    };
    auto unit = [&](size_t c) {
      for (const auto& s : sample)
        if (c >= s.size() || std::abs(s[c]) > 1.0001) return false;
      return true;
    };
    roles = {X, Y, Z};
    if (columns == 4) roles.push_back(S), scalarName = "intensity";
    else if (columns == 6 && bytes(3) && bytes(4) && bytes(5) && !(unit(3) && unit(4) && unit(5))) roles.insert(roles.end(), {R, G, B});
    else if (columns == 6) roles.insert(roles.end(), {NX, NY, NZ});
    else if (columns == 7) roles.insert(roles.end(), {S, R, G, B}), scalarName = "intensity";  // PTS
    else if (columns >= 9) {
      if (bytes(3) && bytes(4) && bytes(5) && unit(6) && unit(7) && unit(8)) roles.insert(roles.end(), {R, G, B, NX, NY, NZ});
      else if (unit(3) && unit(4) && unit(5) && bytes(6) && bytes(7) && bytes(8)) roles.insert(roles.end(), {NX, NY, NZ, R, G, B});
      else if (columns >= 10 && bytes(4) && bytes(5) && bytes(6)) roles.insert(roles.end(), {S, R, G, B, NX, NY, NZ}), scalarName = "intensity";
    }
  }
  auto has = [&](int r) { return std::find(roles.begin(), roles.end(), r) != roles.end(); };
  if (!has(X) || !has(Y) || !has(Z)) throw Error("the point file's header names no x, y and z columns");
  const bool colour = has(R) && has(G) && has(B), normals = has(NX) && has(NY) && has(NZ), scalar = has(S);
  // Colours given as 0-1 fractions (no channel above 1 and some not whole) are scaled to bytes.
  bool fraction = colour;
  if (colour) {
    bool whole = true;
    for (const auto& s : sample)
      for (size_t k = 0; k < roles.size() && k < s.size(); ++k)
        if (roles[k] == R || roles[k] == G || roles[k] == B) {
          if (s[k] > 1) fraction = false;
          if (s[k] != std::floor(s[k])) whole = false;
        }
    fraction = fraction && !whole;
  }
  // Parsed in parallel: the text cut into blocks at line ends, each block into its own arrays, joined in order.
  const size_t bytes = size_t(end - first);
  const int blocks = int(std::clamp<size_t>(bytes / (4u << 20), 1, 256));
  std::vector<const char*> cuts{first};
  for (int b = 1; b < blocks; ++b) {
    const char* c = first + bytes * size_t(b) / size_t(blocks);
    c = std::max(c, cuts.back());
    c = line_end(c);
    cuts.push_back(c < end ? c + 1 : end);
  }
  cuts.push_back(end);
  struct Part { std::vector<double> xyz; std::vector<uint8_t> rgb; std::vector<float> n, s; std::string error; };
  std::vector<Part> parts(cuts.size() - 1);
  std::atomic<size_t> done{0};
  std::mutex progressLock;
  std::atomic<bool> cancelled{false};
  OSD_Parallel::For(0, int(parts.size()), [&](int b) {
    Part& part = parts[size_t(b)];
    std::vector<double> row;
    try {
      for (const char* q = cuts[size_t(b)]; q < cuts[size_t(b) + 1] && !cancelled;) {
        const char* e = line_end(q);
        row.clear();
        const char* c = q;
        bool comment = false;
        while (c < e) {
          while (c < e && (*c == ' ' || *c == '\t' || *c == ',' || *c == ';' || *c == '\r')) ++c;
          if (c >= e) break;
          if (*c == '#' || (*c == '/' && c + 1 < e && c[1] == '/')) { comment = row.empty(); break; }
          if (*c == '+') ++c;
          double v;
          const char* a = parse_double(c, e, v);
          if (!a) { row.clear(); comment = true; break; }  // a stray text line (a second header): skipped
          row.push_back(v);
          c = a;
        }
        q = e < end ? e + 1 : end;
        if (comment || row.size() < roles.size() || row.size() < 3) {
          if (!comment && row.size() == 1) continue;  // a PTS scan's count line
          if (!comment && !row.empty()) throw Error("a point line has " + std::to_string(row.size()) + " numbers, " + std::to_string(roles.size()) + " expected");
          continue;
        }
        double xyz[3] = {0, 0, 0}, rgb[3] = {0, 0, 0}, n[3] = {0, 0, 0}, s = 0;
        for (size_t k = 0; k < roles.size(); ++k) {
          const double v = row[k];
          switch (roles[k]) {
            case X: xyz[0] = v; break;
            case Y: xyz[1] = v; break;
            case Z: xyz[2] = v; break;
            case R: rgb[0] = v; break;
            case G: rgb[1] = v; break;
            case B: rgb[2] = v; break;
            case NX: n[0] = v; break;
            case NY: n[1] = v; break;
            case NZ: n[2] = v; break;
            case S: s = v; break;
            default: break;
          }
        }
        if (!std::isfinite(xyz[0] + xyz[1] + xyz[2]) || std::max({std::abs(xyz[0]), std::abs(xyz[1]), std::abs(xyz[2])}) > 1e12) continue;
        part.xyz.insert(part.xyz.end(), xyz, xyz + 3);
        if (colour) {
          for (double c2 : rgb) part.rgb.push_back(uint8_t(std::clamp(std::lround(fraction ? c2 * 255 : c2), 0l, 255l)));
        }
        if (normals) {
          const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          for (double c2 : n) part.n.push_back(float(l > 0 ? c2 / l : 0));
        }
        if (scalar) part.s.push_back(float(s));
      }
    } catch (const std::exception& e) {
      part.error = e.what();
    }
    const size_t d = ++done;
    std::lock_guard<std::mutex> lock(progressLock);
    try {
      report(o, double(d) / double(parts.size()), "translating points");
    } catch (const Error&) {
      cancelled = true;
    }
  });
  if (cancelled) throw Error("import cancelled");
  Builder out;
  size_t total = 0;
  for (const auto& part : parts) {
    if (!part.error.empty()) throw Error(part.error);
    total += part.xyz.size() / 3;
  }
  out.p.xyz.reserve(total * 3);
  if (colour) out.p.rgb.reserve(total * 3);
  if (normals) out.p.normals.reserve(total * 3);
  if (scalar) out.p.scalar.reserve(total), out.p.scalar_name = scalarName;
  for (auto& part : parts) {
    for (size_t i = 0; i < part.xyz.size(); i += 3) out.add(part.xyz[i], part.xyz[i + 1], part.xyz[i + 2]);
    out.p.rgb.insert(out.p.rgb.end(), part.rgb.begin(), part.rgb.end());
    out.p.normals.insert(out.p.normals.end(), part.n.begin(), part.n.end());
    out.p.scalar.insert(out.p.scalar.end(), part.s.begin(), part.s.end());
    part = Part();
  }
  (void)extension;
  finish(out.p, o, 1.0);
  return std::move(out.p);
}

// ---------------------------------------------------------------- LAS (1.0 - 1.4, point formats 0-10, uncompressed)
Points read_las_data(const std::string& d, const ReadOptions& o) {
  if (d.size() < 227 || d.compare(0, 4, "LASF") != 0) throw Error("not a LAS file");
  auto u8 = [&](size_t at) { return uint8_t(d[at]); };
  auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, d.data() + at, 2); return v; };
  auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, d.data() + at, 4); return v; };
  auto u64 = [&](size_t at) { uint64_t v; std::memcpy(&v, d.data() + at, 8); return v; };
  auto f64 = [&](size_t at) { double v; std::memcpy(&v, d.data() + at, 8); return v; };
  const int minor = u8(25);
  const uint32_t offset = u32(96);
  const int format = u8(104);
  if (format & 0xC0) throw Error("this LAS file is compressed (LAZ): convert it to LAS or E57 first");
  const int fmt = format & 0x3F;
  if (fmt > 10) throw Error("unsupported LAS point format " + std::to_string(fmt));
  const size_t rec = u16(105);
  uint64_t count = u32(107);
  if (minor >= 4 && d.size() >= 255 && (count == 0 || fmt >= 6)) count = u64(247);
  const double scale[3] = {f64(131), f64(139), f64(147)}, add[3] = {f64(155), f64(163), f64(171)};
  if (offset > d.size() || rec < 20) throw Error("invalid LAS header");
  count = std::min<uint64_t>(count, (d.size() - offset) / rec);
  int rgbAt = -1;
  if (fmt == 2) rgbAt = 20;
  else if (fmt == 3 || fmt == 5) rgbAt = 28;
  else if (fmt == 7 || fmt == 8 || fmt == 10) rgbAt = 30;
  if (rgbAt >= 0 && size_t(rgbAt + 6) > rec) rgbAt = -1;
  // 16-bit colour channels; some writers put 8-bit values in them.
  bool wide = false;
  if (rgbAt >= 0)
    for (uint64_t i = 0; i < std::min<uint64_t>(count, 10000) && !wide; ++i)
      for (int k = 0; k < 3; ++k) wide = wide || u16(offset + size_t(i) * rec + size_t(rgbAt + 2 * k)) > 255;
  Builder out;
  out.p.scalar_name = "intensity";
  out.p.xyz.reserve(size_t(count) * 3);
  out.p.scalar.reserve(size_t(count));
  if (rgbAt >= 0) out.p.rgb.reserve(size_t(count) * 3);
  for (uint64_t i = 0; i < count; ++i) {
    const size_t at = offset + size_t(i) * rec;
    int32_t x, y, z;
    std::memcpy(&x, d.data() + at, 4), std::memcpy(&y, d.data() + at + 4, 4), std::memcpy(&z, d.data() + at + 8, 4);
    out.add(x * scale[0] + add[0], y * scale[1] + add[1], z * scale[2] + add[2]);
    out.p.scalar.push_back(float(u16(at + 12)));
    if (rgbAt >= 0)
      for (int k = 0; k < 3; ++k) {
        const uint16_t c = u16(at + size_t(rgbAt + 2 * k));
        out.p.rgb.push_back(uint8_t(wide ? c >> 8 : std::min<uint16_t>(c, 255)));
      }
    if ((i & 0xFFFFF) == 0xFFFFF) report(o, double(i) / double(count), "translating points");
  }
  finish(out.p, o, 1000.0);
  return std::move(out.p);
}

// ---------------------------------------------------------------- writers
const std::vector<std::string>& writable_extensions() {
  static const std::vector<std::string> list = {".ply", ".xyz", ".pts", ".csv", ".e57"};
  return list;
}

void write_file(const std::filesystem::path& file, const Points& p) {
  const auto e = lower_ext(file);
  std::string text;
  if (e == ".ply") text = write_ply_data(p, true);
  else if (e == ".e57") text = write_e57_data(p);
  else if (e == ".xyz" || e == ".pts" || e == ".csv") {
    const size_t n = p.size();
    const char sep = e == ".csv" ? ',' : ' ';
    text.reserve(n * 40);
    if (e == ".pts") text += std::to_string(n) + "\n";
    if (e == ".csv") {
      text += "x,y,z";
      if (p.colored()) text += ",r,g,b";
      if (p.has_normals()) text += ",nx,ny,nz";
      if (p.has_scalar()) text += "," + (p.scalar_name.empty() ? std::string("scalar") : p.scalar_name);
      text += "\n";
    }
    for (size_t i = 0; i < n; ++i) {
      for (size_t k = 0; k < 3; ++k) {
        if (k) text += sep;
        put_fixed(text, double(p.xyz[3 * i + k]) + p.offset[k]);
      }
      if (e == ".pts") {
        text += ' ';
        put_fixed(text, p.has_scalar() ? p.scalar[i] : 0, 3);
        for (size_t k = 0; k < 3; ++k) text += ' ', put_int(text, p.colored() ? p.rgb[3 * i + k] : 255);
      } else {
        if (p.colored())
          for (size_t k = 0; k < 3; ++k) text += sep, put_int(text, p.rgb[3 * i + k]);
        if (p.has_normals())
          for (size_t k = 0; k < 3; ++k) text += sep, put_fixed(text, p.normals[3 * i + k], 5);
        if (e == ".csv" && p.has_scalar()) text += sep, put_fixed(text, p.scalar[i], 5);
      }
      text += '\n';
    }
  } else {
    throw Error("a point cloud is written as .ply, .xyz, .pts, .csv or .e57, not " + e);
  }
  std::ofstream out(file, std::ios::binary);
  if (!out || !out.write(text.data(), std::streamsize(text.size()))) throw Error("cannot write " + path_to_utf8(file.filename()));
}

}  // namespace opad::cloud
