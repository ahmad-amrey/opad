// E57 (ASTM E2807), the scanner exchange format, read and written without a library: the file is pages of 1020 bytes and
// a CRC-32C, an XML section describing the scans, and per scan a compressed-vector section of data packets whose byte
// streams hold one field each (floats, or integers bit-packed from their minimum). Read: every scan of data3D, Cartesian
// or spherical coordinates, colours, intensity and the surface-normals extension (nor:normalX...), invalid points left out,
// each scan placed by its pose. Written: one scan, Cartesian doubles in metres, 8-bit colours, normals and intensity.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string_view>

#include "opad/point_cloud.hpp"

namespace opad::cloud {
namespace {

void report(const ReadOptions& o, double f, const std::string& what) {
  if (o.progress && !o.progress(f, what)) throw Error("import cancelled");
}

// ---------------------------------------------------------------- a small XML tree
struct Node {
  std::string name;
  std::map<std::string, std::string> attr;
  std::string text;
  std::vector<std::unique_ptr<Node>> children;
  const Node* child(std::string_view n) const {
    for (const auto& c : children)
      if (c->name == n) return c.get();
    return nullptr;
  }
  std::string get(const std::string& k, const std::string& def = {}) const {
    auto it = attr.find(k);
    return it == attr.end() ? def : it->second;
  }
};

std::string unescape(std::string_view v) {
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
    else if (!e.empty() && e[0] == '#') {
      const long c = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? std::strtol(std::string(e.substr(2)).c_str(), nullptr, 16)
                                                                   : std::strtol(std::string(e.substr(1)).c_str(), nullptr, 10);
      if (c > 0 && c < 128) out += char(c);
    } else {
      out.append(v.substr(i, semi - i + 1));
    }
    i = semi;
  }
  return out;
}

std::unique_ptr<Node> parse_xml(std::string_view s) {
  auto root = std::make_unique<Node>();
  std::vector<Node*> stack{root.get()};
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '<') {
      const size_t lt = s.find('<', i);
      const size_t e = lt == std::string_view::npos ? s.size() : lt;
      stack.back()->text += unescape(s.substr(i, e - i));
      i = e;
      continue;
    }
    if (s.compare(i, 4, "<!--") == 0) {
      const size_t e = s.find("-->", i);
      i = e == std::string_view::npos ? s.size() : e + 3;
    } else if (s.compare(i, 9, "<![CDATA[") == 0) {
      const size_t e = s.find("]]>", i);
      if (e == std::string_view::npos) throw Error("the E57 XML section is cut off");
      stack.back()->text += std::string(s.substr(i + 9, e - i - 9));
      i = e + 3;
    } else if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) {
      const size_t e = s.find('>', i);
      i = e == std::string_view::npos ? s.size() : e + 1;
    } else if (s.compare(i, 2, "</") == 0) {
      const size_t e = s.find('>', i);
      if (stack.size() > 1) stack.pop_back();
      i = e == std::string_view::npos ? s.size() : e + 1;
    } else {
      size_t j = i + 1;
      while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '>' && s[j] != '/') ++j;
      auto node = std::make_unique<Node>();
      node->name = std::string(s.substr(i + 1, j - i - 1));
      while (j < s.size() && s[j] != '>' && !(s[j] == '/' && j + 1 < s.size() && s[j + 1] == '>')) {
        if (std::isspace(static_cast<unsigned char>(s[j]))) { ++j; continue; }
        size_t k = j;
        while (k < s.size() && s[k] != '=' && !std::isspace(static_cast<unsigned char>(s[k])) && s[k] != '>') ++k;
        const std::string key(s.substr(j, k - j));
        while (k < s.size() && (s[k] == '=' || std::isspace(static_cast<unsigned char>(s[k])))) ++k;
        if (k < s.size() && (s[k] == '"' || s[k] == '\'')) {
          const char q = s[k];
          const size_t e = s.find(q, k + 1);
          if (e == std::string_view::npos) throw Error("the E57 XML section is cut off");
          node->attr[key] = unescape(s.substr(k + 1, e - k - 1));
          j = e + 1;
        } else {
          j = k + 1;
        }
      }
      const bool selfClosing = j < s.size() && s[j] == '/';
      Node* raw = node.get();
      stack.back()->children.push_back(std::move(node));
      if (!selfClosing) stack.push_back(raw);
      i = s.find('>', j);
      i = i == std::string_view::npos ? s.size() : i + 1;
    }
  }
  return root;
}

double number(const Node* n, double def = 0) {
  if (!n) return def;
  std::string t = n->text;
  t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) { return std::isspace(c); }), t.end());
  if (t.empty()) return def;
  double v = def;
  if (!parse_double(t.data(), t.data() + t.size(), v)) return def;
  return v;
}
double attr_number(const Node& n, const std::string& k, double def) {
  const std::string t = n.get(k);
  if (t.empty()) return def;
  double v = def;
  if (!parse_double(t.data(), t.data() + t.size(), v)) return def;
  return v;
}

// ---------------------------------------------------------------- pages
struct Paged {
  const std::string& d;
  uint64_t page = 1024;
  // `n` logical bytes from physical offset `at`, skipping each page's CRC; `at` moves past them.
  std::string read(uint64_t& at, size_t n) const {
    std::string out;
    out.reserve(n);
    const uint64_t payload = page - 4;
    while (n > 0) {
      const uint64_t off = at % page;
      if (off >= payload) { at += page - off; continue; }
      const size_t take = size_t(std::min<uint64_t>(payload - off, n));
      if (at + take > d.size()) throw Error("the E57 file is cut off");
      out.append(d, size_t(at), take);
      at += take;
      n -= take;
      if (at % page == payload) at += 4;
    }
    return out;
  }
};

template <class T>
T le(const std::string& s, size_t at) {
  T v;
  std::memcpy(&v, s.data() + at, sizeof(T));
  return v;
}

// One prototype field's stream.
struct Field {
  std::string name, type;
  bool single = false;
  int64_t minimum = 0, maximum = 0;
  double scale = 1, offset = 0;
  int bits = 0;
  std::string bytes;  // the whole stream, joined across packets
  size_t bit = 0;     // read position
  double next() {
    if (type == "Float") {
      const size_t n = single ? 4 : 8;
      const size_t at = bit / 8;
      bit += n * 8;
      if (at + n > bytes.size()) throw Error("an E57 scan has fewer values than records");
      if (single) return le<float>(bytes, at);
      return le<double>(bytes, at);
    }
    uint64_t raw = 0;
    if (bits > 0) {
      size_t at = bit / 8;
      const int shift = int(bit % 8);
      if ((bit + size_t(bits) + 7) / 8 > bytes.size()) throw Error("an E57 scan has fewer values than records");
      unsigned char buf[16] = {};
      std::memcpy(buf, bytes.data() + at, std::min<size_t>(16, bytes.size() - at));
      // Little-endian LSB-first: gather up to 72 bits.
      unsigned __int128 acc = 0;
      for (int k = 15; k >= 0; --k) acc = (acc << 8) | buf[k];
      raw = uint64_t((acc >> shift) & ((bits >= 64) ? ~uint64_t(0) : ((uint64_t(1) << bits) - 1)));
      bit += size_t(bits);
    }
    const int64_t v = minimum + int64_t(raw);
    return type == "ScaledInteger" ? double(v) * scale + offset : double(v);
  }
};

}  // namespace

Points read_e57_data(const std::string& d, const ReadOptions& o) {
  if (d.size() < 48 || d.compare(0, 8, "ASTM-E57") != 0) throw Error("not an E57 file");
  Paged paged{d, le<uint64_t>(d, 40)};
  if (paged.page < 64 || paged.page > (1u << 20)) throw Error("the E57 file has an invalid page size");
  uint64_t xmlAt = le<uint64_t>(d, 24);
  const uint64_t xmlLength = le<uint64_t>(d, 32);
  if (xmlLength > d.size()) throw Error("the E57 file's XML section is out of the file");
  const auto xml = parse_xml(paged.read(xmlAt, size_t(xmlLength)));
  const Node* root = xml->child("e57Root");
  if (!root) throw Error("the E57 file has no e57Root");
  const Node* data3D = root->child("data3D");
  Points out;
  out.scalar_name = "intensity";
  std::vector<double> world;  // positions as doubles until the offset is known
  bool anyColour = false, anyNormals = false, anyIntensity = false;
  std::vector<std::vector<uint8_t>> colours;
  size_t total = 0;
  struct Scan { std::vector<double> xyz; std::vector<uint8_t> rgb; std::vector<float> n, s; bool colour = false, normals = false, intensity = false; };
  std::vector<Scan> scans;
  if (data3D)
    for (const auto& child : data3D->children) {
      const Node* points = child->child("points");
      if (!points || points->get("type") != "CompressedVector") continue;
      const Node* proto = points->child("prototype");
      if (!proto) continue;
      const uint64_t records = uint64_t(attr_number(*points, "recordCount", 0));
      uint64_t at = uint64_t(attr_number(*points, "fileOffset", 0));
      if (records == 0) continue;
      std::vector<Field> fields;
      for (const auto& f : proto->children) {
        Field fl;
        fl.name = f->name;
        fl.type = f->get("type");
        if (fl.type == "Float") fl.single = f->get("precision") == "single";
        else if (fl.type == "Integer" || fl.type == "ScaledInteger") {
          fl.minimum = int64_t(attr_number(*f, "minimum", fl.type == "Integer" ? -9.2e18 : -2147483648.0));
          fl.maximum = int64_t(attr_number(*f, "maximum", fl.type == "Integer" ? 9.2e18 : 2147483647.0));
          fl.scale = attr_number(*f, "scale", 1);
          fl.offset = attr_number(*f, "offset", 0);
          const uint64_t span = uint64_t(fl.maximum) - uint64_t(fl.minimum);
          while (fl.bits < 64 && (fl.bits == 0 ? 0 : (uint64_t(1) << fl.bits)) <= span) ++fl.bits;
          if (span == 0) fl.bits = 0;
        } else {
          throw Error("the E57 scan's field " + fl.name + " is of type " + fl.type + ", which is not read");
        }
        fields.push_back(fl);
      }
      // The section: its header, then data packets (index and empty packets skipped).
      const std::string head = paged.read(at, 32);
      if (uint8_t(head[0]) != 1) throw Error("an E57 compressed vector section is missing");
      const uint64_t sectionLength = le<uint64_t>(head, 8);
      uint64_t pos = le<uint64_t>(head, 16);
      uint64_t consumed = 32;
      // Packets follow the header in the logical stream; the data offset is where the first one is.
      while (consumed < sectionLength) {
        uint64_t p = pos;
        const std::string ph = paged.read(p, 4);
        const uint8_t type = uint8_t(ph[0]);
        const size_t length = size_t(le<uint16_t>(ph, 2)) + 1;
        uint64_t q = pos;
        const std::string packet = paged.read(q, length);
        pos = q;
        consumed += length;
        if (type == 1) {
          const size_t streams = le<uint16_t>(packet, 4);
          if (streams != fields.size()) throw Error("an E57 data packet has " + std::to_string(streams) + " streams for " + std::to_string(fields.size()) + " fields");
          size_t data = 6 + 2 * streams;
          for (size_t k = 0; k < streams; ++k) {
            const size_t n = le<uint16_t>(packet, 6 + 2 * k);
            if (data + n > packet.size()) throw Error("an E57 data packet is cut off");
            fields[k].bytes.append(packet, data, n);
            data += n;
          }
        } else if (type != 0 && type != 2) {
          throw Error("an E57 packet of unknown type");
        }
        if (consumed >= sectionLength) break;
        report(o, std::min(1.0, double(consumed) / double(sectionLength)), "reading scan " + std::to_string(scans.size() + 1));
      }
      auto find = [&](const char* n) -> Field* {
        for (auto& f : fields)
          if (f.name == n) return &f;
        return nullptr;
      };
      Field *cx = find("cartesianX"), *cy = find("cartesianY"), *cz = find("cartesianZ");
      Field *sr = find("sphericalRange"), *sa = find("sphericalAzimuth"), *se = find("sphericalElevation");
      Field *cr = find("colorRed"), *cg = find("colorGreen"), *cb = find("colorBlue"), *in = find("intensity");
      Field *nx = find("nor:normalX"), *ny = find("nor:normalY"), *nz = find("nor:normalZ");
      Field *ci = find("cartesianInvalidState"), *si = find("sphericalInvalidState");
      const bool cart = cx && cy && cz, sph = sr && sa && se;
      if (!cart && !sph) throw Error("an E57 scan has neither Cartesian nor spherical coordinates");
      Scan scan;
      scan.colour = cr && cg && cb;
      scan.normals = nx && ny && nz;
      scan.intensity = in != nullptr;
      // Colour and intensity limits: the scan's, else the fields' own range.
      auto limits = [&](const char* group, const char* lo, const char* hi, Field* f, double& a, double& b) {
        a = f && f->type != "Float" ? double(f->minimum) : 0;
        b = f && f->type != "Float" ? double(f->maximum) : 1;
        if (f && f->type == "ScaledInteger") a = a * f->scale + f->offset, b = b * f->scale + f->offset;
        if (const Node* l = child->child(group)) {
          a = number(l->child(lo), a);
          b = number(l->child(hi), b);
        }
        if (b <= a) b = a + 1;
      };
      double cLo[3], cHi[3], iLo, iHi;
      if (scan.colour) {
        limits("colorLimits", "colorRedMinimum", "colorRedMaximum", cr, cLo[0], cHi[0]);
        limits("colorLimits", "colorGreenMinimum", "colorGreenMaximum", cg, cLo[1], cHi[1]);
        limits("colorLimits", "colorBlueMinimum", "colorBlueMaximum", cb, cLo[2], cHi[2]);
      }
      if (in) limits("intensityLimits", "intensityMinimum", "intensityMaximum", in, iLo, iHi);
      // The pose: a unit quaternion and a translation, scan frame -> file frame.
      double qw = 1, qx = 0, qy = 0, qz = 0, tx = 0, ty = 0, tz = 0;
      if (const Node* pose = child->child("pose")) {
        if (const Node* r = pose->child("rotation")) qw = number(r->child("w"), 1), qx = number(r->child("x")), qy = number(r->child("y")), qz = number(r->child("z"));
        if (const Node* t = pose->child("translation")) tx = number(t->child("x")), ty = number(t->child("y")), tz = number(t->child("z"));
      }
      const double ql = std::sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
      if (ql > 0) qw /= ql, qx /= ql, qy /= ql, qz /= ql;
      const double R[9] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw),
                           2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw),
                           2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)};
      scan.xyz.reserve(size_t(records) * 3);
      for (uint64_t r = 0; r < records; ++r) {
        double x, y, z;
        if (cart) x = cx->next(), y = cy->next(), z = cz->next();
        if (sph) {
          const double range = sr->next(), az = sa->next(), el = se->next();
          if (!cart) x = range * std::cos(el) * std::cos(az), y = range * std::cos(el) * std::sin(az), z = range * std::sin(el);
        }
        double c[3] = {0, 0, 0}, n[3] = {0, 0, 0}, s = 0;
        if (scan.colour) c[0] = cr->next(), c[1] = cg->next(), c[2] = cb->next();
        if (in) s = in->next();
        if (scan.normals) n[0] = nx->next(), n[1] = ny->next(), n[2] = nz->next();
        bool invalid = false;
        if (ci) invalid = invalid || (cart && ci->next() != 0);
        if (si) invalid = invalid || (!cart && si->next() != 0);
        // Fields the prototype has that are read here only to keep the streams in step.
        for (auto& f : fields)
          if (&f != cx && &f != cy && &f != cz && &f != sr && &f != sa && &f != se && &f != cr && &f != cg && &f != cb && &f != in && &f != nx &&
              &f != ny && &f != nz && &f != ci && &f != si)
            f.next();
        if (invalid || !std::isfinite(x + y + z)) continue;
        scan.xyz.insert(scan.xyz.end(), {R[0] * x + R[1] * y + R[2] * z + tx, R[3] * x + R[4] * y + R[5] * z + ty, R[6] * x + R[7] * y + R[8] * z + tz});
        if (scan.colour)
          for (int k = 0; k < 3; ++k) scan.rgb.push_back(uint8_t(std::clamp(std::lround((c[k] - cLo[k]) / (cHi[k] - cLo[k]) * 255), 0l, 255l)));
        if (scan.normals) {
          const double m[3] = {R[0] * n[0] + R[1] * n[1] + R[2] * n[2], R[3] * n[0] + R[4] * n[1] + R[5] * n[2], R[6] * n[0] + R[7] * n[1] + R[8] * n[2]};
          const double l = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
          for (double v : m) scan.n.push_back(float(l > 0 ? v / l : 0));
        }
        if (in) scan.s.push_back(float((s - iLo) / (iHi - iLo)));
        if ((r & 0xFFFFF) == 0xFFFFF) report(o, double(r) / double(records), "translating points");
      }
      total += scan.xyz.size() / 3;
      anyColour = anyColour || scan.colour, anyNormals = anyNormals || scan.normals, anyIntensity = anyIntensity || scan.intensity;
      scans.push_back(std::move(scan));
    }
  (void)world, (void)colours;
  if (scans.empty()) throw Error("the E57 file has no 3D scans");
  // Joined in metres; an attribute some scans lack is dropped unless every scan has it.
  bool allColour = true, allNormals = true, allIntensity = true;
  for (const auto& s : scans) allColour = allColour && s.colour, allNormals = allNormals && s.normals, allIntensity = allIntensity && s.intensity;
  out.xyz.reserve(total * 3);
  bool started = false;
  for (auto& s : scans) {
    for (size_t i = 0; i < s.xyz.size(); i += 3) {
      if (!started) {
        started = true;
        // Kept near zero in floats: the offset in metres here, a metre-rounded first point when it lies over 10 m out.
        if (std::max({std::abs(s.xyz[i]), std::abs(s.xyz[i + 1]), std::abs(s.xyz[i + 2])}) > 10)
          out.offset = {std::round(s.xyz[i]), std::round(s.xyz[i + 1]), std::round(s.xyz[i + 2])};
      }
      for (size_t k = 0; k < 3; ++k) out.xyz.push_back(float(s.xyz[i + k] - out.offset[k]));
    }
    if (allColour) out.rgb.insert(out.rgb.end(), s.rgb.begin(), s.rgb.end());
    if (allNormals) out.normals.insert(out.normals.end(), s.n.begin(), s.n.end());
    if (allIntensity) out.scalar.insert(out.scalar.end(), s.s.begin(), s.s.end());
    s = Scan();
  }
  (void)anyColour, (void)anyNormals, (void)anyIntensity;
  // Metres to mm (or the scale asked for), Z up unless told otherwise.
  out.normalise();
  {
    const double s = o.scale > 0 ? o.scale : 1000.0;
    const bool yUp = o.up == "y" || o.up == "Y";
    Mat4 m;
    if (yUp) m.at(1, 1) = 0, m.at(1, 2) = -1, m.at(2, 1) = 1, m.at(2, 2) = 0;
    Mat4 sc;
    sc.at(0, 0) = sc.at(1, 1) = sc.at(2, 2) = s;
    const Mat4 all = m * sc;
    out.transform(all);
    out.offset = all.apply(out.offset);
  }
  return out;
}

// ---------------------------------------------------------------- writing
namespace {
uint32_t crc32c(const unsigned char* p, size_t n) {
  static uint32_t table[256];
  static const bool ready = [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0x82F63B78u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    return true;
  }();
  (void)ready;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
template <class T>
void put(std::string& s, T v) {
  char b[sizeof(T)];
  std::memcpy(b, &v, sizeof(T));
  s.append(b, sizeof(T));
}
uint64_t physical(uint64_t logical) { return logical / 1020 * 1024 + logical % 1020; }
std::string xml_double(double v) {
  char b[64];
  std::snprintf(b, sizeof b, "%.17g", v);
  std::string s = b;
  std::replace(s.begin(), s.end(), ',', '.');  // a locale with a decimal comma
  return s;
}
}  // namespace

std::string write_e57_data(const Points& p) {
  const size_t n = p.size();
  if (n == 0) throw Error("the point cloud has no points");
  std::string L(48, '\0');  // the logical stream; the header is filled in last
  while (L.size() % 4) L += '\0';
  const uint64_t sectionAt = L.size();
  L.append(32, '\0');
  const uint64_t dataAt = L.size();
  // Streams: x, y, z (double, metres), then red, green, blue (8 bits), then intensity, then normals (single).
  const bool colour = p.colored(), normals = p.has_normals(), scalar = p.has_scalar();
  const size_t streams = 3 + (colour ? 3 : 0) + (scalar ? 1 : 0) + (normals ? 3 : 0);
  const size_t record = 24 + (colour ? 3 : 0) + (scalar ? 4 : 0) + (normals ? 12 : 0);
  const size_t perPacket = (65536 - 8 - 2 * streams - 8) / record;
  float sLo = 0, sHi = 1;
  if (scalar) {
    sLo = *std::min_element(p.scalar.begin(), p.scalar.end());
    sHi = *std::max_element(p.scalar.begin(), p.scalar.end());
  }
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (size_t first = 0; first < n; first += perPacket) {
    const size_t m = std::min(perPacket, n - first);
    std::vector<std::string> s(streams);
    for (size_t i = first; i < first + m; ++i) {
      for (size_t k = 0; k < 3; ++k) {
        const double v = (double(p.xyz[3 * i + k]) + p.offset[k]) / 1000.0;
        lo[k] = std::min(lo[k], v), hi[k] = std::max(hi[k], v);
        put(s[k], v);
      }
      size_t at = 3;
      if (colour)
        for (size_t k = 0; k < 3; ++k) s[at + k] += char(p.rgb[3 * i + k]);
      if (colour) at += 3;
      if (scalar) put(s[at++], p.scalar[i]);
      if (normals)
        for (size_t k = 0; k < 3; ++k) put(s[at + k], p.normals[3 * i + k]);
    }
    std::string packet;
    put<uint8_t>(packet, 1);
    put<uint8_t>(packet, 0);
    put<uint16_t>(packet, 0);  // length - 1, below
    put<uint16_t>(packet, uint16_t(streams));
    for (const auto& b : s) put<uint16_t>(packet, uint16_t(b.size()));
    for (const auto& b : s) packet += b;
    while (packet.size() % 4) packet += '\0';
    const uint16_t length = uint16_t(packet.size() - 1);
    std::memcpy(packet.data() + 2, &length, 2);
    L += packet;
  }
  const uint64_t sectionLength = L.size() - sectionAt;
  {
    std::string h;
    put<uint8_t>(h, 1);
    h.append(7, '\0');
    put<uint64_t>(h, sectionLength);
    put<uint64_t>(h, physical(dataAt));
    put<uint64_t>(h, 0);
    std::memcpy(L.data() + sectionAt, h.data(), 32);
  }
  while (L.size() % 4) L += '\0';
  const uint64_t xmlAt = L.size();
  std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  x += "<e57Root type=\"Structure\" xmlns=\"http://www.astm.org/COMMIT/E57/2010-e57-v1.0\"";
  if (normals) x += " xmlns:nor=\"http://www.libe57.org/E57_NOR_surface_normals.txt\"";
  x += ">\n";
  x += "<formatName type=\"String\"><![CDATA[ASTM E57 3D Imaging Data File]]></formatName>\n";
  x += "<guid type=\"String\"><![CDATA[{" + new_uuid() + "}]]></guid>\n";
  x += "<versionMajor type=\"Integer\">1</versionMajor>\n<versionMinor type=\"Integer\">0</versionMinor>\n";
  x += "<e57LibraryVersion type=\"String\"><![CDATA[OPAD " + version_string() + "]]></e57LibraryVersion>\n";
  x += "<coordinateMetadata type=\"String\"></coordinateMetadata>\n";
  x += "<data3D type=\"Vector\" allowHeterogeneousChildren=\"1\">\n<vectorChild type=\"Structure\">\n";
  x += "<guid type=\"String\"><![CDATA[{" + new_uuid() + "}]]></guid>\n<name type=\"String\"><![CDATA[OPAD point cloud]]></name>\n";
  x += "<cartesianBounds type=\"Structure\"><xMinimum type=\"Float\">" + xml_double(lo[0]) + "</xMinimum><xMaximum type=\"Float\">" + xml_double(hi[0]) +
       "</xMaximum><yMinimum type=\"Float\">" + xml_double(lo[1]) + "</yMinimum><yMaximum type=\"Float\">" + xml_double(hi[1]) +
       "</yMaximum><zMinimum type=\"Float\">" + xml_double(lo[2]) + "</zMinimum><zMaximum type=\"Float\">" + xml_double(hi[2]) + "</zMaximum></cartesianBounds>\n";
  if (colour)
    x += "<colorLimits type=\"Structure\"><colorRedMinimum type=\"Integer\">0</colorRedMinimum><colorRedMaximum type=\"Integer\">255</colorRedMaximum>"
         "<colorGreenMinimum type=\"Integer\">0</colorGreenMinimum><colorGreenMaximum type=\"Integer\">255</colorGreenMaximum>"
         "<colorBlueMinimum type=\"Integer\">0</colorBlueMinimum><colorBlueMaximum type=\"Integer\">255</colorBlueMaximum></colorLimits>\n";
  if (scalar)
    x += "<intensityLimits type=\"Structure\"><intensityMinimum type=\"Float\">" + xml_double(sLo) + "</intensityMinimum><intensityMaximum type=\"Float\">" +
         xml_double(sHi > sLo ? sHi : sLo + 1) + "</intensityMaximum></intensityLimits>\n";
  x += "<pose type=\"Structure\"><rotation type=\"Structure\"><w type=\"Float\">1</w><x type=\"Float\">0</x><y type=\"Float\">0</y><z type=\"Float\">0</z></rotation>"
       "<translation type=\"Structure\"><x type=\"Float\">0</x><y type=\"Float\">0</y><z type=\"Float\">0</z></translation></pose>\n";
  x += "<points type=\"CompressedVector\" fileOffset=\"" + std::to_string(physical(sectionAt)) + "\" recordCount=\"" + std::to_string(n) + "\">\n";
  x += "<prototype type=\"Structure\">\n";
  for (const char* c : {"cartesianX", "cartesianY", "cartesianZ"}) x += std::string("<") + c + " type=\"Float\" precision=\"double\"/>\n";
  if (colour)
    for (const char* c : {"colorRed", "colorGreen", "colorBlue"}) x += std::string("<") + c + " type=\"Integer\" minimum=\"0\" maximum=\"255\"/>\n";
  if (scalar) x += "<intensity type=\"Float\" precision=\"single\"/>\n";
  if (normals)
    for (const char* c : {"nor:normalX", "nor:normalY", "nor:normalZ"}) x += std::string("<") + c + " type=\"Float\" precision=\"single\"/>\n";
  x += "</prototype>\n<codecs type=\"Vector\" allowHeterogeneousChildren=\"1\"></codecs>\n</points>\n";
  x += "</vectorChild>\n</data3D>\n<images2D type=\"Vector\" allowHeterogeneousChildren=\"1\"></images2D>\n</e57Root>\n";
  L += x;
  // The header (physical offsets), then pages of 1020 bytes, each with its CRC-32C (big-endian), the last one padded.
  const uint64_t pages = (L.size() + 1019) / 1020;
  {
    std::string h = "ASTM-E57";
    put<uint32_t>(h, 1);
    put<uint32_t>(h, 0);
    put<uint64_t>(h, pages * 1024);
    put<uint64_t>(h, physical(xmlAt));
    put<uint64_t>(h, x.size());
    put<uint64_t>(h, 1024);
    std::memcpy(L.data(), h.data(), 48);
  }
  L.resize(size_t(pages * 1020), '\0');
  std::string out(size_t(pages * 1024), '\0');
  for (uint64_t k = 0; k < pages; ++k) {
    std::memcpy(out.data() + k * 1024, L.data() + k * 1020, 1020);
    const uint32_t c = crc32c(reinterpret_cast<const unsigned char*>(out.data() + k * 1024), 1020);
    const unsigned char b[4] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
    std::memcpy(out.data() + k * 1024 + 1020, b, 4);
  }
  return out;
}

}  // namespace opad::cloud
