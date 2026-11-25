#include "foam_case.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "../import_common.hpp"

namespace opad::sim::foam {

namespace {
V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V add(const V& a, const V& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V mul(const V& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double norm(const V& a) { return std::sqrt(dot(a, a)); }
}  // namespace

std::string num(double v) {
  std::ostringstream s;
  s.precision(10);
  s << v;
  return s.str();
}
std::string vec(const V& v) { return "(" + num(v[0]) + " " + num(v[1]) + " " + num(v[2]) + ")"; }

// ---------------------------------------------------------------- OpenFOAM's files
std::string header(const std::string& cls, const std::string& object, const std::string& location) {
  return "FoamFile\n{\n    version 2.0;\n    format ascii;\n    class " + cls + ";\n" + (location.empty() ? "" : "    location \"" + location + "\";\n") +
         "    object " + object + ";\n}\n";
}
void put(const fs::path& file, const std::string& cls, const std::string& object, const std::string& body) {
  fs::create_directories(file.parent_path());
  write_text_file(file, header(cls, object) + body);
}

// The list after "N\n(" in an OpenFOAM file (points, faces, labels, a field), as text from its opening bracket.
std::string_view list_after(std::string_view t, size_t from) {
  for (size_t i = from; i < t.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(t[i]))) continue;
    size_t j = i;
    while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) ++j;
    size_t k = j;
    while (k < t.size() && std::isspace(static_cast<unsigned char>(t[k]))) ++k;
    if (k < t.size() && t[k] == '(' && (i == 0 || std::isspace(static_cast<unsigned char>(t[i - 1])))) return t.substr(k);
    i = j;
  }
  throw Error("cfd: an OpenFOAM list is missing");
}
size_t count_before(std::string_view list_from_bracket, std::string_view whole) {
  // the number written before the list's bracket
  const size_t at = size_t(list_from_bracket.data() - whole.data());
  size_t e = at;
  while (e > 0 && std::isspace(static_cast<unsigned char>(whole[e - 1]))) --e;
  size_t b = e;
  while (b > 0 && std::isdigit(static_cast<unsigned char>(whole[b - 1]))) --b;
  return size_t(std::stoull(std::string(whole.substr(b, e - b))));
}
std::vector<V> read_points(const std::string& t) {
  const auto l = list_after(t, t.find("FoamFile") == std::string::npos ? 0 : t.find('}'));
  const size_t n = count_before(l, t);
  std::vector<V> out;
  out.reserve(n);
  const char* p = l.data() + 1;
  const char* end = t.data() + t.size();
  while (out.size() < n && p < end) {
    while (p < end && *p != '(') ++p;
    ++p;
    char* q;
    V v;
    v[0] = std::strtod(p, &q), v[1] = std::strtod(q, &q), v[2] = std::strtod(q, &q);
    out.push_back(v);
    p = q;
  }
  return out;
}
std::vector<std::vector<int>> read_faces(const std::string& t) {
  const auto l = list_after(t, t.find('}'));
  const size_t n = count_before(l, t);
  std::vector<std::vector<int>> out;
  out.reserve(n);
  const char* p = l.data() + 1;
  const char* end = t.data() + t.size();
  while (out.size() < n && p < end) {
    char* q;
    const long m = std::strtol(p, &q, 10);
    p = q;
    while (p < end && *p != '(') ++p;
    ++p;
    std::vector<int> f(size_t(std::max(0L, m)));
    for (auto& x : f) x = int(std::strtol(p, &q, 10)), p = q;
    while (p < end && *p != ')') ++p;
    ++p;
    out.push_back(std::move(f));
  }
  return out;
}
std::vector<double> read_numbers(std::string_view whole, std::string_view l) {
  const size_t n = count_before(l, whole);
  std::vector<double> out;
  out.reserve(n);
  const char* p = l.data() + 1;
  const char* end = whole.data() + whole.size();
  while (out.size() < n && p < end) {
    while (p < end && (std::isspace(static_cast<unsigned char>(*p)) || *p == '(' || *p == ')')) ++p;
    char* q;
    out.push_back(std::strtod(p, &q));
    if (q == p) break;
    p = q;
  }
  return out;
}
std::vector<int> read_labels(const std::string& t) {
  const auto v = read_numbers(t, list_after(t, t.find('}')));
  return std::vector<int>(v.begin(), v.end());
}
// A field's internal values: scalars, or vectors flattened (3 per cell); a uniform field gives its one value n times.
std::vector<double> read_internal(const std::string& t, size_t cells, int width) {
  const size_t at = t.find("internalField");
  if (at == std::string::npos) throw Error("cfd: a field without internalField");
  const size_t semi = t.find(';', at);
  const std::string head = t.substr(at, std::min(semi, t.find('(', at)) - at);
  if (head.find("nonuniform") == std::string::npos) {
    std::istringstream s(t.substr(at + 13, semi - at - 13));
    std::string word;
    s >> word;
    std::vector<double> one(size_t(width), 0.0);
    if (width == 1) s >> one[0];
    else {
      char c;
      s >> c >> one[0] >> one[1] >> one[2];
    }
    std::vector<double> out;
    for (size_t i = 0; i < cells; ++i) out.insert(out.end(), one.begin(), one.end());
    return out;
  }
  const auto l = list_after(t, at);
  if (width == 1) return read_numbers(t, l);
  const size_t n = count_before(l, t);
  std::vector<double> out;
  out.reserve(n * 3);
  const char* p = l.data() + 1;
  const char* end = t.data() + t.size();
  for (size_t i = 0; i < n && p < end; ++i) {
    while (p < end && *p != '(') ++p;
    ++p;
    char* q;
    for (int k = 0; k < 3; ++k) out.push_back(std::strtod(p, &q)), p = q;
  }
  return out;
}
// A patch's values in a field file's boundaryField (scalar), empty when the patch has none written.
std::vector<double> read_patch(const std::string& t, const std::string& patch) {
  const size_t bf = t.find("boundaryField");
  if (bf == std::string::npos) return {};
  size_t at = bf;
  for (;;) {
    at = t.find(patch, at + 1);
    if (at == std::string::npos) return {};
    const char before = t[at - 1], after = at + patch.size() < t.size() ? t[at + patch.size()] : ' ';
    if (std::isspace(static_cast<unsigned char>(before)) && std::isspace(static_cast<unsigned char>(after))) break;
  }
  const size_t open = t.find('{', at), close = t.find('}', open);
  const std::string blk = t.substr(open, close - open);
  const size_t v = blk.find("value");
  if (v == std::string::npos) return {};
  if (blk.find("nonuniform", v) != std::string::npos) return read_numbers(blk, list_after(blk, v));
  std::istringstream s(blk.substr(v + 5));
  std::string word;
  double x = 0;
  s >> word >> x;
  return {x};
}

// A region's mesh: its cells' centres and volumes (from its faces), mm.
Cells read_cells(const fs::path& poly) {
  const auto P = read_points(read_text_file(poly / "points"));
  const auto F = read_faces(read_text_file(poly / "faces"));
  const auto own = read_labels(read_text_file(poly / "owner"));
  const auto nei = read_labels(read_text_file(poly / "neighbour"));
  // A cell may own no face (the last, when all its faces are inner ones with lower neighbours): count both lists.
  int nc = 0;
  for (int o : own) nc = std::max(nc, o + 1);
  for (int n : nei) nc = std::max(nc, n + 1);
  // Face centres and area vectors from a fan of triangles about the points' mean; a cell's volume by the divergence
  // theorem, its centre as the volume-weighted centre of the pyramids on its faces.
  std::vector<V> fc(F.size()), fa(F.size());
  for (size_t f = 0; f < F.size(); ++f) {
    V m{0, 0, 0};
    for (int k : F[f]) m = add(m, P[size_t(k)]);
    m = mul(m, 1.0 / double(F[f].size()));
    V area{0, 0, 0}, c{0, 0, 0};
    double total = 0;
    for (size_t k = 0; k < F[f].size(); ++k) {
      const V& a = P[size_t(F[f][k])];
      const V& b = P[size_t(F[f][(k + 1) % F[f].size()])];
      const V s = mul(cross(sub(a, m), sub(b, m)), 0.5);
      const double w = norm(s);
      area = add(area, s);
      c = add(c, mul(add(add(a, b), m), w / 3));
      total += w;
    }
    fc[f] = total > 0 ? mul(c, 1 / total) : m;
    fa[f] = area;
  }
  // Mean of each cell's face centres: a point inside it for the pyramids.
  std::vector<V> guess(size_t(nc), V{0, 0, 0});
  std::vector<int> nf(size_t(nc), 0);
  for (size_t f = 0; f < F.size(); ++f) {
    guess[size_t(own[f])] = add(guess[size_t(own[f])], fc[f]), ++nf[size_t(own[f])];
    if (f < nei.size()) guess[size_t(nei[f])] = add(guess[size_t(nei[f])], fc[f]), ++nf[size_t(nei[f])];
  }
  for (int c = 0; c < nc; ++c) guess[size_t(c)] = mul(guess[size_t(c)], 1.0 / std::max(1, nf[size_t(c)]));
  Cells out;
  out.centre.assign(size_t(nc), V{0, 0, 0});
  out.volume.assign(size_t(nc), 0.0);
  std::vector<V> moment(size_t(nc), V{0, 0, 0});
  auto pyramid = [&](int c, size_t f, double sign) {
    const double v = sign * dot(fa[f], sub(fc[f], guess[size_t(c)])) / 3;
    out.volume[size_t(c)] += v;
    moment[size_t(c)] = add(moment[size_t(c)], mul(add(mul(fc[f], 0.75), mul(guess[size_t(c)], 0.25)), v));
  };
  for (size_t f = 0; f < F.size(); ++f) {
    pyramid(own[f], f, 1);
    if (f < nei.size()) pyramid(nei[f], f, -1);
  }
  for (int c = 0; c < nc; ++c)
    out.centre[size_t(c)] = out.volume[size_t(c)] > 0 ? mul(moment[size_t(c)], 1 / out.volume[size_t(c)]) : guess[size_t(c)];
  return out;
}


std::string latest_time(const fs::path& dir) {
  double best = -1;
  std::string name;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_directory()) continue;
    const std::string s = e.path().filename().string();
    char* end = nullptr;
    const double t = std::strtod(s.c_str(), &end);
    if (end && *end == 0 && !s.empty() && t > best) best = t, name = s;
  }
  return name;
}

// A mesh's patches (constant/polyMesh/boundary): name, type, faces.
std::vector<Patch> read_boundary(const std::string& t) {
  std::vector<Patch> out;
  const std::string_view body = list_after(t);
  size_t at = 0;
  while (at < body.size()) {
    const size_t open = body.find('{', at);
    if (open == std::string_view::npos) break;
    const size_t close = body.find('}', open);
    size_t n0 = body.find_last_not_of(" \t\r\n", open - 1);
    size_t n1 = body.find_last_of(" \t\r\n(", n0);
    Patch p;
    p.name = std::string(body.substr(n1 + 1, n0 - n1));
    const std::string_view e = body.substr(open, close - open);
    auto value = [&](const char* key) {
      const size_t k = e.find(key);
      if (k == std::string_view::npos) return std::string();
      size_t v = k + std::strlen(key);
      while (v < e.size() && std::isspace(static_cast<unsigned char>(e[v]))) ++v;
      const size_t w = e.find(';', v);
      return std::string(e.substr(v, w - v));
    };
    p.type = value("type");
    p.faces = size_t(std::strtoull(value("nFaces").c_str(), nullptr, 10));
    p.start = size_t(std::strtoull(value("startFace").c_str(), nullptr, 10));
    out.push_back(p);
    at = close + 1;
  }
  return out;
}

std::string run(const OpenFoam& foam, const std::string& app, const std::vector<std::string>& args, const fs::path& where, double at, double to,
                int timeout_ms, const Progress& progress) {
  if (progress && !progress(at, "OpenFOAM: " + app)) throw Error("cancelled");
  if (!foam.project.empty()) {
#ifdef _WIN32
    _putenv_s("WM_PROJECT_DIR", path_to_utf8(foam.project).c_str());
#else
    const std::string project = path_to_utf8(foam.project);  // only when it changes: sweep points run at once
    if (const char* now = std::getenv("WM_PROJECT_DIR"); !now || project != now) setenv("WM_PROJECT_DIR", project.c_str(), 1);
#endif
  }
  detail::RunOptions ro;
  ro.output = where / ("log." + app);
  ro.timeout_ms = timeout_ms;
  // The program's log read as it grows (once a second): a run of minutes is seen to move. The mesher says its phase and the
  // iteration in it; a solver its time, against the end time of its controlDict.
  double end_time = 0;
  try {
    const std::string control = read_text_file(where / "system" / "controlDict");
    if (const size_t e = control.find("endTime "); e != std::string::npos) end_time = std::atof(control.c_str() + e + 8);
  } catch (...) {
  }
  double shown = at;
  std::string phase = "OpenFOAM: " + app;
  auto read_at = std::chrono::steady_clock::now();
  double first_time = -1;
  bool stopped = false;
  ro.cancelled = [&] {
    const auto now = std::chrono::steady_clock::now();
    if (to > at && now - read_at > std::chrono::seconds(1)) {
      read_at = now;
      std::string tail;
      if (std::ifstream in(ro.output, std::ios::binary | std::ios::ate); in) {
        const std::streamoff size = in.tellg(), take = std::min<std::streamoff>(size, 65536);
        in.seekg(size - take);
        tail.resize(size_t(take));
        in.read(tail.data(), take);
      }
      auto last = [&](const char* word) {
        const size_t i = tail.rfind(word);
        return i == std::string::npos ? -1.0 : std::atof(tail.c_str() + i + std::strlen(word));
      };
      if (app == "snappyHexMesh") {
        // three phases: castellating (refinement), snapping to the parts (morph iterations), adding layers
        const size_t refine = tail.rfind("Refinement phase"), snap = tail.rfind("Morphing phase"), layer = tail.rfind("layer addition phase");
        const double morph = last("Morph iteration "), layers = last("Layer addition iteration ");
        double sub = 0.05;
        if (layer != std::string::npos || layers >= 0) sub = 0.7 + 0.25 * std::min(1.0, (layers + 1) / 10.0), phase = "Meshing the air: adding layers along the parts";
        else if (snap != std::string::npos || morph >= 0) sub = 0.3 + 0.4 * std::min(1.0, (morph + 1) / 10.0), phase = "Meshing the air: fitting it to the parts";
        else if (refine != std::string::npos) sub = 0.15, phase = "Meshing the air: refining around the parts";
        shown = std::max(shown, at + (to - at) * sub);
      } else if (const double t = last("\nTime = "); t > 0 && end_time > 0) {
        if (first_time < 0) first_time = t - 1;
        const double sub = std::clamp((t - first_time) / std::max(1.0, end_time - first_time), 0.0, 1.0);
        shown = std::max(shown, at + (to - at) * sub);
        phase = "OpenFOAM: " + app + ", iteration " + std::to_string(int(t)) + " of " + std::to_string(int(end_time));
      }
    }
    stopped = progress && !progress(shown, phase);
    return stopped;
  };
  std::vector<fs::path> a;
  fs::path program;
  if (!foam.wrapper.empty()) program = foam.wrapper, a.push_back(app);
  else program = foam.bin / app;
  for (const auto& s : args) a.push_back(s);
  a.push_back("-case");
  a.push_back(where);
  const int status = detail::run_program(program, a, where, ro);
  if (stopped) throw Error("cancelled");
  std::string log;
  try {
    log = read_text_file(ro.output);
  } catch (...) {
  }
  if (status != 0 || log.find("FOAM FATAL") != std::string::npos) {
    const size_t at_err = log.find("FOAM FATAL");
    throw Error("OpenFOAM's " + app + " failed: " + (at_err == std::string::npos ? "status " + std::to_string(status) : log.substr(at_err, 500)));
  }
  return log;
}

}  // namespace opad::sim::foam
