#include "opad/util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#include "opad/version.hpp"

namespace opad {

// ---------------------------------------------------------------- uuid / time
namespace {
struct IdContext {
  std::mutex mu;
  std::string seed, context;
  unsigned long long counter = 0;
  bool on = false;
  IdContext() {
    if (const char* e = std::getenv("OPAD_DETERMINISTIC"); e && *e) {
      on = true;
      seed = e;
    }
  }
};
IdContext& id_context() {
  static IdContext c;
  return c;
}
}  // namespace

bool deterministic_ids() { return id_context().on; }

void set_id_context(const std::string& context) {
  IdContext& c = id_context();
  std::lock_guard<std::mutex> lock(c.mu);
  c.context = context;
  c.counter = 0;
}

std::string new_uuid() {
  if (IdContext& c = id_context(); c.on) {  // a UUID (version 5 layout) from the seed, the context and a counter
    std::string h;
    {
      std::lock_guard<std::mutex> lock(c.mu);
      // Only inside a command, which names the context: anything else (a person's edit) keeps random ids.
      if (!c.context.empty()) h = sha256_hex(c.seed + "|" + c.context + "|" + std::to_string(c.counter++));
    }
    if (!h.empty()) {
      h[12] = '5';
      h[16] = "89ab"[std::stoi(h.substr(16, 1), nullptr, 16) & 3];
      return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
    }
  }
  static std::mutex mu;
  static std::mt19937_64 rng([] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(),
                      static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};
    return std::mt19937_64(seq);
  }());
  std::lock_guard<std::mutex> lock(mu);
  uint64_t a = rng(), b = rng();
  a = (a & 0xffffffffffff0fffULL) | 0x0000000000004000ULL;  // version 4
  b = (b & 0x3fffffffffffffffULL) | 0x8000000000000000ULL;  // variant 10
  char buf[37];
  std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(a >> 32),
                static_cast<unsigned>((a >> 16) & 0xffff), static_cast<unsigned>(a & 0xffff),
                static_cast<unsigned>(b >> 48), static_cast<unsigned long long>(b & 0xffffffffffffULL));
  return buf;
}

bool is_uuid(std::string_view s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    char c = s[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}

std::string now_iso8601() {
  if (deterministic_ids()) return "2000-01-01T00:00:00Z";
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

std::string default_author() {
  if (const char* e = std::getenv("OPAD_AUTHOR"); e && *e) return e;
  if (const char* e = std::getenv("GIT_AUTHOR_NAME"); e && *e) return e;
  if (const char* e = std::getenv("USER"); e && *e) return e;
  if (const char* e = std::getenv("USERNAME"); e && *e) return e;
  return "unknown";
}

std::string version_string() { return OPAD_VERSION; }

// ---------------------------------------------------------------- sha256
namespace {
struct Sha256 {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf[64];
  size_t buf_len = 0;
  uint64_t total = 0;
  static constexpr uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
      0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
      0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
      0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
      0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
      0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2};
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) |
             p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const uint8_t* p, size_t n) {
    total += n;
    while (n > 0) {
      size_t take = std::min(n, 64 - buf_len);
      std::memcpy(buf + buf_len, p, take);
      buf_len += take;
      p += take;
      n -= take;
      if (buf_len == 64) {
        block(buf);
        buf_len = 0;
      }
    }
  }
  std::string finish() {
    uint64_t bits = total * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint32_t v : h)
      for (int i = 7; i >= 0; --i) out.push_back(hex[(v >> (i * 4)) & 0xf]);
    return out;
  }
};
constexpr uint32_t Sha256::K[64];
}  // namespace

std::string sha256_hex(std::string_view data) {
  Sha256 s;
  s.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  return s.finish();
}

// ---------------------------------------------------------------- files
std::filesystem::path path_from_utf8(std::string_view utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string read_text_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw Error("cannot open file: " + p.string());
  // In one read at its size (a stream copied through a string stream took 0.6 s for the 334 MB Engine), then whatever
  // it grew by meanwhile.
  std::error_code ec;
  const auto size = std::filesystem::file_size(p, ec);
  std::string text(ec ? 0 : size_t(size), '\0');
  in.read(text.data(), std::streamsize(text.size()));
  text.resize(size_t(in.gcount()));
  char buf[1 << 16];
  while (in && (in.read(buf, sizeof buf) || in.gcount() > 0)) text.append(buf, size_t(in.gcount()));
  return text;
}

void write_text_file(const std::filesystem::path& p, std::string_view text) {
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
  std::filesystem::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cannot write file: " + p.string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
  }
  std::error_code ec;
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    std::filesystem::remove(p, ec);
    std::filesystem::rename(tmp, p, ec);
    if (ec) throw Error("cannot replace file: " + p.string() + ": " + ec.message());
  }
}

// ---------------------------------------------------------------- Mat4
Mat4 Mat4::translation(double x, double y, double z) {
  Mat4 r;
  r.at(0, 3) = x;
  r.at(1, 3) = y;
  r.at(2, 3) = z;
  return r;
}

bool Mat4::is_identity(double eps) const {
  Mat4 id;
  for (int i = 0; i < 16; ++i)
    if (std::fabs(m[i] - id.m[i]) > eps) return false;
  return true;
}

Mat4 Mat4::operator*(const Mat4& o) const {
  Mat4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += at(i, k) * o.at(k, j);
      r.at(i, j) = s;
    }
  return r;
}

Mat4 Mat4::inverse() const {
  const double a = at(0, 0), b = at(0, 1), c = at(0, 2), d = at(1, 0), e = at(1, 1), f = at(1, 2), g = at(2, 0), h = at(2, 1), k = at(2, 2);
  const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-300) throw Error("the transform cannot be inverted");
  Mat4 r;
  const double l[3][3] = {{e * k - f * h, c * h - b * k, b * f - c * e}, {f * g - d * k, a * k - c * g, c * d - a * f}, {d * h - e * g, b * g - a * h, a * e - b * d}};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) r.at(i, j) = l[i][j] / det;
    r.at(i, 3) = -(r.at(i, 0) * at(0, 3) + r.at(i, 1) * at(1, 3) + r.at(i, 2) * at(2, 3));
  }
  return r;
}

Vec3 Mat4::apply(const Vec3& p) const {
  return {at(0, 0) * p[0] + at(0, 1) * p[1] + at(0, 2) * p[2] + at(0, 3),
          at(1, 0) * p[0] + at(1, 1) * p[1] + at(1, 2) * p[2] + at(1, 3),
          at(2, 0) * p[0] + at(2, 1) * p[1] + at(2, 2) * p[2] + at(2, 3)};
}

Vec3 Mat4::apply_dir(const Vec3& d) const {
  return {at(0, 0) * d[0] + at(0, 1) * d[1] + at(0, 2) * d[2],
          at(1, 0) * d[0] + at(1, 1) * d[1] + at(1, 2) * d[2],
          at(2, 0) * d[0] + at(2, 1) * d[1] + at(2, 2) * d[2]};
}

json Mat4::to_json() const {
  json a = json::array();
  for (double v : m) a.push_back(v == 0 ? 0.0 : v);  // normalise -0
  return a;
}

Mat4 Mat4::from_json(const json& j) {
  Mat4 r;
  if (j.is_null()) return r;
  if (!j.is_array()) throw Error("transform must be an array of 12 or 16 numbers");
  if (j.size() == 16) {
    for (size_t i = 0; i < 16; ++i) r.m[i] = j[i].get<double>();
  } else if (j.size() == 12) {
    for (size_t i = 0; i < 12; ++i) r.m[i] = j[i].get<double>();
    r.m[12] = 0;
    r.m[13] = 0;
    r.m[14] = 0;
    r.m[15] = 1;
  } else {
    throw Error("transform must be an array of 12 or 16 numbers");
  }
  return r;
}

// ---------------------------------------------------------------- Ref
const char* Ref::kind_name(Kind k) {
  switch (k) {
    case Kind::Body: return "body";
    case Kind::Face: return "face";
    case Kind::Edge: return "edge";
    case Kind::Vertex: return "vertex";
    case Kind::Center: return "center";
    case Kind::Point: return "point";
  }
  return "body";
}

static Ref::Kind kind_from_name(std::string_view s) {
  if (s == "body") return Ref::Kind::Body;
  if (s == "face") return Ref::Kind::Face;
  if (s == "edge") return Ref::Kind::Edge;
  if (s == "vertex") return Ref::Kind::Vertex;
  if (s == "center") return Ref::Kind::Center;
  if (s == "point") return Ref::Kind::Point;
  throw Error("unknown reference kind: " + std::string(s));
}

std::string Ref::str() const {
  std::ostringstream ss;
  ss.precision(12);
  if (kind == Kind::Point) {
    ss << "point/" << point[0] << "," << point[1] << "," << point[2];
    return ss.str();
  }
  ss << body;
  if (kind != Kind::Body) ss << "/" << kind_name(kind) << "/" << index;
  return ss.str();
}

json Ref::to_json() const {
  json j;
  if (kind == Kind::Point) {
    j["kind"] = "point";
    j["point"] = {point[0], point[1], point[2]};
    return j;
  }
  j["body"] = body;
  j["kind"] = kind_name(kind);
  if (kind != Kind::Body) j["index"] = index;
  return j;
}

Ref Ref::parse(std::string_view s) {
  Ref r;
  if (s.rfind("point/", 0) == 0) {
    r.kind = Kind::Point;
    std::string rest(s.substr(6));
    for (char& c : rest)
      if (c == ',') c = ' ';
    std::istringstream ss(rest);
    if (!(ss >> r.point[0] >> r.point[1] >> r.point[2])) throw Error("bad point reference: " + std::string(s));
    return r;
  }
  size_t slash = s.find('/');
  r.body = std::string(s.substr(0, slash));
  if (!is_uuid(r.body)) throw Error("bad reference (expected uuid[/face|edge|vertex/N]): " + std::string(s));
  if (slash == std::string_view::npos) return r;
  std::string_view rest = s.substr(slash + 1);
  size_t slash2 = rest.find('/');
  if (slash2 == std::string_view::npos) throw Error("bad reference (missing index): " + std::string(s));
  r.kind = kind_from_name(rest.substr(0, slash2));
  if (r.kind == Kind::Body || r.kind == Kind::Point) throw Error("bad reference kind: " + std::string(s));
  r.index = std::atoi(std::string(rest.substr(slash2 + 1)).c_str());
  return r;
}

Ref Ref::from_json(const json& j) {
  if (j.is_string()) return parse(j.get<std::string>());
  Ref r;
  std::string kind = j.value("kind", "body");
  r.kind = kind_from_name(kind);
  if (r.kind == Kind::Point) {
    const auto& p = j.at("point");
    r.point = {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
    return r;
  }
  r.body = j.at("body").get<std::string>();
  if (r.kind != Kind::Body) r.index = j.value("index", -1);
  if (j.contains("point") && j["point"].is_array()) {
    const auto& p = j["point"];
    r.point = {p[0].get<double>(), p[1].get<double>(), p[2].get<double>()};
  }
  return r;
}

}  // namespace opad
