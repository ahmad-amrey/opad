#include "opad/sim/cfd.hpp"

#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "../import_common.hpp"
#include "opad/geometry.hpp"
#include "opad/materials.hpp"
#include "opad/sim/airflow.hpp"
#include "opad/sim/fea.hpp"

namespace opad::sim {

namespace fs = std::filesystem;

namespace {

using V = Vec3;
constexpr double kPi = 3.14159265358979323846;
V sub(const V& a, const V& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V add(const V& a, const V& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V mul(const V& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const V& a, const V& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V cross(const V& a, const V& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double norm(const V& a) { return std::sqrt(dot(a, a)); }
V unit(const V& a) {
  const double n = norm(a);
  return n > 1e-12 ? mul(a, 1 / n) : V{0, 0, 0};
}

std::string num(double v) {
  std::ostringstream s;
  s.precision(10);
  s << v;
  return s.str();
}
std::string vec(const V& v) { return "(" + num(v[0]) + " " + num(v[1]) + " " + num(v[2]) + ")"; }

// ---------------------------------------------------------------- OpenFOAM's files
std::string header(const std::string& cls, const std::string& object, const std::string& location = {}) {
  return "FoamFile\n{\n    version 2.0;\n    format ascii;\n    class " + cls + ";\n" + (location.empty() ? "" : "    location \"" + location + "\";\n") +
         "    object " + object + ";\n}\n";
}
void put(const fs::path& file, const std::string& cls, const std::string& object, const std::string& body) {
  fs::create_directories(file.parent_path());
  write_text_file(file, header(cls, object) + body);
}

// The list after "N\n(" in an OpenFOAM file (points, faces, labels, a field), as text from its opening bracket.
std::string_view list_after(std::string_view t, size_t from = 0) {
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
struct Cells {
  std::vector<V> centre;
  std::vector<double> volume;
};
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

// Nearest of a set of points: a uniform grid of buckets.
class Nearest {
 public:
  Nearest(const std::vector<V>& pts, double cell) : m_pts(pts), m_cell(std::max(cell, 1e-6)) {
    for (size_t i = 0; i < pts.size(); ++i) m_grid[key(pts[i])].push_back(i);
  }
  size_t operator()(const V& p) const {
    size_t best = 0;
    double bd = 1e300;
    for (int r = 0; r < 64 && (bd == 1e300 || double(r - 1) * m_cell < std::sqrt(bd)); ++r) {
      const auto k = key(p);
      for (int i = -r; i <= r; ++i)
        for (int j = -r; j <= r; ++j)
          for (int l = -r; l <= r; ++l) {
            if (std::max({std::abs(i), std::abs(j), std::abs(l)}) != r) continue;
            const auto it = m_grid.find({k[0] + i, k[1] + j, k[2] + l});
            if (it == m_grid.end()) continue;
            for (size_t n : it->second)
              if (const double d = dot(sub(m_pts[n], p), sub(m_pts[n], p)); d < bd) bd = d, best = n;
          }
    }
    return best;
  }

 private:
  std::array<long, 3> key(const V& p) const { return {long(std::floor(p[0] / m_cell)), long(std::floor(p[1] / m_cell)), long(std::floor(p[2] / m_cell))}; }
  const std::vector<V>& m_pts;
  double m_cell;
  std::map<std::array<long, 3>, std::vector<size_t>> m_grid;
};

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
struct Patch {
  std::string name, type;
  size_t faces = 0, start = 0;
};
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

}  // namespace

// ---------------------------------------------------------------- where OpenFOAM is
OpenFoam openfoam() {
  OpenFoam f;
  auto has_bin = [](const fs::path& dir) {
    std::error_code ec;
    return fs::is_regular_file(dir / "simpleFoam", ec) || fs::is_regular_file(dir / "simpleFoam.exe", ec);
  };
  if (const char* env = std::getenv("OPAD_OPENFOAM"); env && *env) {
    const fs::path p = path_from_utf8(env);
    std::error_code ec;
    if (fs::is_regular_file(p / "etc" / "openfoam", ec) && fs::is_regular_file(p / "bin" / "foamEtcFile", ec)) f.wrapper = p / "etc" / "openfoam";
    if (has_bin(p / "bin")) f.bin = p / "bin";
    f.project = p;
    return f;
  }
  // On the PATH, with the project directory from the environment or where packages put it.
  if (const char* path = std::getenv("PATH")) {
    std::string s = path;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    for (size_t a = 0; a <= s.size();) {
      const size_t b = std::min(s.find(sep, a), s.size());
      const fs::path dir = s.substr(a, b - a);
      if (!dir.empty() && has_bin(dir)) {
        f.bin = dir;
        break;
      }
      a = b + 1;
    }
  }
  if (f.bin.empty()) {
    // An OpenCFD package's own wrapper (openfoam2306, ...), which sets everything up.
    std::error_code ec;
    for (const char* root : {"/usr/lib/openfoam", "/opt"})
      for (const auto& e : fs::directory_iterator(root, ec))
        if (e.path().filename().string().rfind("openfoam", 0) == 0 && fs::is_regular_file(e.path() / "etc" / "openfoam", ec) &&
            fs::is_regular_file(e.path() / "bin" / "foamEtcFile", ec)) {
          f.wrapper = e.path() / "etc" / "openfoam", f.project = e.path();
          return f;
        }
    return f;
  }
  if (const char* wm = std::getenv("WM_PROJECT_DIR"); wm && *wm) f.project = path_from_utf8(wm);
  else
    for (const char* guess : {"/usr/share/openfoam", "/usr/lib/openfoam"})
      if (std::error_code ec; fs::is_regular_file(fs::path(guess) / "etc" / "controlDict", ec)) f.project = guess;
  return f;
}

Enclosure find_enclosure(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, const std::string& named) {
  auto box_of = [&](const std::string& id) {
    Bnd_Box b;
    BRepBndLib::Add(node_world_shape(doc, scene, id), b);
    return b;
  };
  auto holds = [](const Bnd_Box& outer, const Bnd_Box& inner) {
    double a[6], b[6];
    outer.Get(a[0], a[1], a[2], a[3], a[4], a[5]);
    inner.Get(b[0], b[1], b[2], b[3], b[4], b[5]);
    const double tol = 1e-6 * std::sqrt(outer.SquareExtent());
    return b[0] >= a[0] - tol && b[1] >= a[1] - tol && b[2] >= a[2] - tol && b[3] <= a[3] + tol && b[4] <= a[4] + tol && b[5] <= a[5] + tol;
  };
  auto solid_shown = [&](const std::string& id) {
    const Node* n = scene.node(id);
    return n && n->kind == Node::Kind::Body && !n->body_missing && n->representation == "solid" && scene.effectively_visible(id);
  };
  Enclosure out;
  if (!named.empty()) {
    if (!solid_shown(named)) throw Error("cfd.enclosure: " + named + " is not a shown solid body");
    out.enclosure = named;
  } else if (!bodies.empty()) {
    Bnd_Box want;
    for (const auto& b : bodies) want.Add(box_of(b));
    double best = 1e300;
    for (const auto& id : scene.all_bodies()) {
      if (std::find(bodies.begin(), bodies.end(), id) != bodies.end() || !solid_shown(id)) continue;
      const Bnd_Box e = box_of(id);
      if (holds(e, want) && e.SquareExtent() < best) best = e.SquareExtent(), out.enclosure = id;
    }
  }
  if (out.enclosure.empty()) return out;
  const Bnd_Box e = box_of(out.enclosure);
  for (const auto& id : scene.all_bodies())
    if (id != out.enclosure && solid_shown(id) && holds(e, box_of(id))) out.inside.push_back(id);
  return out;
}

StudyRun run_cfd(const Document& doc, const Scene& scene, const json& st, const Progress& progress) {
  const auto t_start = std::chrono::steady_clock::now();
  auto seconds_since = [](std::chrono::steady_clock::time_point t) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); };
  json timing = json::object();
  StudyRun run;
  run.kind = "thermal";
  auto report = [&](double f, const std::string& phase) {
    if (progress && !progress(f, phase)) throw Error("cancelled");
  };
  const OpenFoam foam = openfoam();
  if (!foam.found())
    throw Error("the CFD air needs OpenFOAM: install it (Ubuntu: apt install openfoam; or OpenCFD's packages) or set OPAD_OPENFOAM to its directory");
  const double ambient = st.value("ambient", 25.0);
  const double T0 = ambient + 273.15;
  // quality: quick (screening a sweep), normal, fine: the box's cells, the flow's iterations, how settled the heat must be;
  // each given on its own wins.
  json cfd = st.value("cfd", json::object());
  {
    const std::string q = cfd.value("quality", std::string("normal"));
    if (q != "quick" && q != "normal" && q != "fine") throw Error("cfd.quality is quick, normal or fine");
    const json preset = q == "quick" ? json{{"divisions", 20}, {"flow_iterations", 400}, {"settle", 0.3}}
                        : q == "fine" ? json{{"divisions", 45}, {"flow_iterations", 1500}, {"settle", 0.05}}
                                      : json{{"divisions", 30}, {"flow_iterations", 1000}, {"settle", 0.1}};
    for (const auto& [k, v] : preset.items())
      if (!cfd.contains(k)) cfd[k] = v;
  }

  // How the air carries heat between cells: upwind (first order, bounded) unless asked (e.g. "limitedLinear 1").
  const std::string heat_scheme = cfd.value("heat_scheme", std::string("upwind"));
  for (char c : heat_scheme)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != ' ' && c != '.') throw Error("cfd.heat_scheme: an OpenFOAM scheme such as upwind or \"limitedLinear 1\"");

  // ---- the case's thermal loads: heat sources in bodies; fans (one at a duct's inlet, any number placed in an enclosure) or
  // one forced stream
  std::string load_case = st.value("case", std::string());
  if (load_case.empty())
    for (const auto& l : scene.loads)
      if (l.kind == "heat") {
        load_case = l.load_case;
        break;
      }
  std::vector<const Load*> heats, fan_loads, held;  // held: fixed temperatures
  const Load* stream = nullptr;
  for (const auto& l : scene.loads) {
    if (l.load_case != load_case) continue;
    if (!l.error.empty()) throw Error("load \"" + l.name + "\": " + l.error);
    if (l.kind == "heat") heats.push_back(&l);
    else if (l.kind == "temperature") held.push_back(&l);
    else if (l.kind == "fan") fan_loads.push_back(&l);
    else if (l.kind == "convection" && l.def.value("h", json()) == "forced") stream = &l;
    else if (l.kind == "convection" || l.kind == "radiation")
      run.warnings.push_back("\"" + l.name + "\" is left out: with the CFD air, the parts lose heat only to the air the fan or the stream moves");
  }
  if (heats.empty() && held.empty()) throw Error("load case \"" + load_case + "\" has no heat source: add one (load kind heat, W, on a body)");
  std::vector<std::string> bodies;
  auto add_body = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (!n) throw Error("no body " + id);
    for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id))
      if (std::find(bodies.begin(), bodies.end(), b) == bodies.end()) bodies.push_back(b);
  };
  for (const auto& b : st.value("bodies", json::array())) add_body(b.get<std::string>());
  const bool listed = !bodies.empty();
  if (!listed) {
    for (const Load* l : heats)
      for (const auto& r : l->refs) add_body(r.body);
    for (const Load* l : held)
      for (const auto& r : l->refs) add_body(r.body);
    for (const Load* l : fan_loads)
      for (const auto& r : l->refs) add_body(r.body);
    if (stream)
      for (const auto& r : stream->refs) add_body(r.body);
  }

  // An enclosure (cfd.enclosure, or the smallest shown body whose box holds every body the loads name): then the air inside
  // it and a margin of the room around it are solved, its vents open to the room, its fans placed inside, and every shown
  // body within it (the board, connectors) takes part.
  const json named = cfd.value("enclosure", json());
  std::string enclosure;
  if (named.is_string() || (!listed && named != json(false))) {
    const Enclosure found = find_enclosure(doc, scene, bodies, named.is_string() ? named.get<std::string>() : std::string());
    enclosure = found.enclosure;
    if (!enclosure.empty()) {
      add_body(enclosure);
      if (!listed)
        for (const auto& id : found.inside) add_body(id);
    }
  }
  if (!enclosure.empty()) {
    if (fan_loads.empty() && !cfd.value("buoyancy", true) && !cfd.value("sealed", false))
      throw Error("the CFD air in an enclosure moves by fans (load kind fan) or by warm air rising (cfd.buoyancy): with neither it stands still "
                  "(only a sealed box's air may: cfd.sealed)");
    if (stream) run.warnings.push_back("\"" + stream->name + "\" is left out: in an enclosure the fans move the air");
    stream = nullptr;
  } else {
    if (fan_loads.size() > 1) throw Error("the CFD air in a duct takes one fan (a fan's count puts several side by side; in an enclosure, any number)");
    if (fan_loads.empty() && !stream)
      throw Error("the CFD air needs a fan, or a convection with h forced (its velocity and way): still air stays with the correlations");
  }
  // In a duct the parts' heat is OpenFOAM's (whole bodies); in an enclosure CalculiX's, which takes heat on faces too.
  if (enclosure.empty())
    for (const Load* l : heats)
      for (const auto& r : l->refs)
        if (r.kind != Ref::Kind::Body) throw Error("heat \"" + l->name + "\": the CFD air in a duct takes heat sources in whole bodies, not on faces");
  if (enclosure.empty() && !held.empty()) {
    if (heats.empty()) throw Error("the CFD air in a duct takes heat sources (load kind heat); fixed temperatures only in an enclosure");
    for (const Load* l : held) run.warnings.push_back("\"" + l->name + "\" is left out: fixed temperatures are taken in an enclosure, not in a duct");
  }
  const Load* fan_load = enclosure.empty() && !fan_loads.empty() ? fan_loads.front() : nullptr;
  // The frame: along the duct's air; an enclosure's is the world's.
  const V way = !enclosure.empty() ? V{1, 0, 0} : unit((fan_load ? fan_load : stream)->def.value("vector", V{1, 0, 0}));
  if (norm(way) < 0.5) throw Error("the air's way (vector) has no length");

  // ---- the parts: world solids, triangulated for the mesher and for showing the results
  std::vector<TopoDS_Shape> world;
  Bnd_Box all;
  for (const auto& b : bodies) {
    const Node* n = scene.node(b);
    if (n->body_missing || n->representation != "solid") throw Error("\"" + n->name + "\" is not a solid");
    world.push_back(node_world_shape(doc, scene, b));
    BRepBndLib::Add(world.back(), all);
  }
  auto index_of = [&](const std::string& id) { return size_t(std::find(bodies.begin(), bodies.end(), id) - bodies.begin()); };
  double x0, y0, z0, x1, y1, z1;
  all.Get(x0, y0, z0, x1, y1, z1);
  // A frame along the air: the world axes least along it.
  V axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  std::sort(std::begin(axes), std::end(axes), [&](const V& a, const V& b) { return std::fabs(dot(a, way)) < std::fabs(dot(b, way)); });
  const V e1 = enclosure.empty() ? unit(sub(axes[0], mul(way, dot(axes[0], way)))) : V{0, 1, 0};
  const V e2 = unit(cross(way, e1));
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (double x : {x0, x1})
    for (double y : {y0, y1})
      for (double z : {z0, z1}) {
        const V p{x, y, z};
        const double c[3] = {dot(p, way), dot(p, e1), dot(p, e2)};
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
      }
  // A body's extent along a direction.
  auto extent = [&](size_t i, const V& d, double& from, double& to) {
    Bnd_Box b;
    BRepBndLib::AddOptimal(world[i], b, false, false);
    double c[6];
    b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
    from = 1e300, to = -1e300;
    for (double x : {c[0], c[3]})
      for (double y : {c[1], c[4]})
        for (double z : {c[2], c[5]}) from = std::min(from, dot({x, y, z}, d)), to = std::max(to, dot({x, y, z}, d));
  };
  // The finest cell: two thirds of a fin's thickness and a sixth of the gap between fins (the gap's boundary layers want
  // six cells across: on six 3 mm fins 8.4 mm apart, cells of 2, 1.5 and 1 mm gave the parts' rise as 129, 106 and 89 %
  // of the correlations' and the flow as 135, 120 and 103 %), else a 40th of the parts' size.
  // In an enclosure the box's cells are a 30th of its size (quick: a 20th, fine: a 45th; cfd.cell_size: twice that), and each body's surface is refined
  // down to what its fins, walls and vents need (below).
  const double given = cfd.value("cell_size", 0.0);
  const double size = std::sqrt(std::pow(hi[0] - lo[0], 2) + std::pow(hi[1] - lo[1], 2) + std::pow(hi[2] - lo[2], 2));
  double fine = given;
  json fins_json;
  std::vector<bool> finned(bodies.size(), false);
  std::vector<double> fin_want(bodies.size(), 0.0);
  for (const Load* l : fan_loads)
    for (const auto& r : l->refs)
      if (const size_t i = index_of(r.body); i < bodies.size())
        if (const auto f = air::fin_array(world[i], unit(l->def.value("vector", V{1, 0, 0})))) {
          finned[i] = true;
          fin_want[i] = std::max(0.2, std::min(f->t / 1.5, f->gap / 6) * 1e3);
          if (l == fan_load) fins_json = f->to_json();
          if (given <= 0 && enclosure.empty()) fine = std::min(fine > 0 ? fine : 1e300, fin_want[i]);
        }
  if (fine <= 0) fine = std::max(0.25, size / 40);
  const double coarse = enclosure.empty() ? 2 * fine : given > 0 ? 2 * given : std::max(0.5, size / cfd.value("divisions", 30.0));

  // Cells: each body's surface refined until its thinnest wall has a cell and a quarter across and its narrowest gap three
  // (an enclosure's vents two: refining all of its surface for them took a small box to a million cells), a finned body's
  // to its fins' cells, at most four levels below the box's cells.
  std::vector<int> level(bodies.size(), 1);
  int deepest = 1;
  json refinement = json::object();  // per body: its level and what set it (summary "refinement")
  // In an enclosure a gap's cells are refined only in a box around it (the faces that look across it): refining all of a box's
  // surface, outside too, for its 3 mm slots made three quarters of a fan-cooled box's 320k cells.
  struct GapBox {
    V lo, hi;
    int level;
  };
  std::vector<GapBox> gap_boxes;
  auto level_for = [&](double want) { return std::clamp(int(std::ceil(std::log2(coarse / want) - 0.2)), 1, 4); };
  for (size_t i = 0; i < bodies.size(); ++i) {
    const air::Thinness t = air::thinness(world[i]);
    double want = enclosure.empty() ? 1e300 : given > 0 ? given : (fin_want[i] > 0 ? fin_want[i] : 1e300);
    if (t.wall > 0) want = std::min(want, t.wall / cfd.value("wall_cells", 1.25));
    const double across = bodies[i] == enclosure ? 2 : cfd.value("gap_cells", 3.0);
    if (enclosure.empty() && t.gap > 0 && t.gap < 0.5 * size) want = std::min(want, t.gap / across);
    if (want < 1e300) level[i] = level_for(want);
    int gap_level = 0;
    if (!enclosure.empty() && given <= 0)
      for (const auto& g : t.gaps) {
        if (g.width >= 0.5 * size) continue;
        const int L = level_for(g.width / across);
        if (L <= level[i]) continue;
        const double pad = coarse / std::pow(2.0, L);
        gap_boxes.push_back({{g.lo[0] - pad, g.lo[1] - pad, g.lo[2] - pad}, {g.hi[0] + pad, g.hi[1] + pad, g.hi[2] + pad}, L});
        gap_level = std::max(gap_level, L);
      }
    deepest = std::max({deepest, level[i], gap_level});
    refinement[scene.node(bodies[i])->name] = {{"level", level[i]}, {"cell_mm", coarse / std::pow(2.0, level[i])}, {"wall_mm", t.wall}, {"gap_mm", t.gap}};
    if (gap_level > 0) refinement[scene.node(bodies[i])->name]["gap_level"] = gap_level;
  }
  // Boxes of one level that overlap made one.
  for (bool merged = true; merged;) {
    merged = false;
    for (size_t a = 0; a < gap_boxes.size() && !merged; ++a)
      for (size_t b = a + 1; b < gap_boxes.size() && !merged; ++b) {
        GapBox &x = gap_boxes[a], &y = gap_boxes[b];
        if (x.level != y.level) continue;
        bool overlap = true;
        for (int k = 0; k < 3; ++k) overlap = overlap && x.lo[k] <= y.hi[k] && y.lo[k] <= x.hi[k];
        if (!overlap) continue;
        for (int k = 0; k < 3; ++k) x.lo[k] = std::min(x.lo[k], y.lo[k]), x.hi[k] = std::max(x.hi[k], y.hi[k]);
        gap_boxes.erase(gap_boxes.begin() + long(b));
        merged = true;
      }
  }
  fine = coarse / std::pow(2.0, deepest);  // the finest cell
  // Fans in an enclosure: each a disk of air whose two sides the fan's curve sets apart in pressure. On a body that makes
  // heat or has fins (a heatsink), the disk lies against the side the air comes from, the fan's size across; on any other
  // body (one that stands for the fan), the disk is its middle across the air's way, as wide as it, and the body is air.
  struct Disk {
    const Load* load;
    air::Fan fan;
    V centre, normal;
    double radius;
  };
  std::vector<Disk> disks;
  std::vector<bool> is_air(bodies.size(), false);
  if (!enclosure.empty())
    for (const Load* l : fan_loads) {
      if (l->refs.empty()) throw Error("fan \"" + l->name + "\" is on nothing");
      const size_t i = index_of(l->refs.front().body);
      Disk d{l, air::fan_from(l->def.value("fan", json("80x25"))), {0, 0, 0}, unit(l->def.value("vector", V{1, 0, 0})), 0};
      if (norm(d.normal) < 0.5) throw Error("fan \"" + l->name + "\": its vector has no length");
      if (l->def.value("count", 1) > 1) run.warnings.push_back("fan \"" + l->name + "\": count is for a duct; in an enclosure, add a fan load per fan");
      V u1 = unit(cross(d.normal, std::fabs(d.normal[0]) < 0.9 ? V{1, 0, 0} : V{0, 1, 0})), u2 = unit(cross(d.normal, u1));
      double n0, n1, p0, p1, q0, q1;
      extent(i, d.normal, n0, n1);
      extent(i, u1, p0, p1);
      extent(i, u2, q0, q1);
      bool heated = false;
      for (const Load* h : heats)
        for (const auto& r : h->refs) heated = heated || r.body == bodies[i];
      const bool model = !heated && !finned[i] && bodies[i] != enclosure;
      const double along = model ? 0.5 * (n0 + n1) : n0 - std::max(1.0, 1.5 * coarse / std::pow(2.0, level[i]));
      d.centre = add(add(mul(d.normal, along), mul(u1, 0.5 * (p0 + p1))), mul(u2, 0.5 * (q0 + q1)));
      d.radius = model || d.fan.size <= 0 ? 0.47 * std::min(p1 - p0, q1 - q0) : 0.47 * d.fan.size;
      if (model) is_air[i] = true, level[i] = 0;
      disks.push_back(d);
    }

  // The air's box: a duct along the air, a little upstream and more downstream, a cell clear of the parts across; or an
  // enclosure in a margin of the room, open on every side. Off the parts' faces by a fraction of a cell, so that no face of
  // theirs lies on a grid plane.
  const double len = hi[0] - lo[0];
  const double big = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
  const double margin = std::max(4 * coarse, cfd.value("padding", 0.15 * big));
  const double up = enclosure.empty() ? std::max(4 * coarse, cfd.value("upstream", 0.25 * len)) : margin;
  const double down = enclosure.empty() ? std::max(4 * coarse, cfd.value("downstream", 0.5 * len)) : margin;
  const double pad = enclosure.empty() ? cfd.value("padding", fine) : margin;
  const double off = 0.37 * coarse;
  const double d0 = lo[0] - up - off, d1 = hi[0] + down + off;
  const double a0 = lo[1] - pad - off, a1 = hi[1] + pad + off, b0 = lo[2] - pad - off, b1 = hi[2] + pad + off;
  const int nx = std::max(4, int(std::ceil((d1 - d0) / coarse))), ny = std::max(2, int(std::ceil((a1 - a0) / coarse))),
            nz = std::max(2, int(std::ceil((b1 - b0) / coarse)));
  if (double(nx) * ny * nz > cfd.value("max_cells", 3e6)) throw Error("cfd: the air's box would take too many cells at this cell size: set cfd.cell_size larger");
  auto corner = [&](double d, double a, double b) { return add(add(mul(way, d), mul(e1, a)), mul(e2, b)); };

  // ---- the case
  struct Dir {
    fs::path p;
    bool keep = false;
    ~Dir() {
      std::error_code e;
      if (!keep && !p.empty()) fs::remove_all(p, e);
    }
  } dir;
  dir.p = fs::temp_directory_path() / ("opad-cfd-" + new_uuid().substr(0, 12));
  fs::create_directories(dir.p);
  if (const char* k = std::getenv("OPAD_KEEP_CFD"); k && *k) dir.keep = true;
  const fs::path cas = dir.p / "cht", flow = dir.p / "flow";
  run.summary["case_dir"] = dir.keep ? json(path_to_utf8(dir.p)) : json(nullptr);
  // Programs: run with the project directory known (its etc files), or through a full install's wrapper.
  const int timeout = int(st.value("timeout", 7200.0) * 1000);
  auto foam_run = [&](const std::string& app, const std::vector<std::string>& args, const fs::path& where, double at) {
    report(at, "OpenFOAM: " + app);
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
    ro.timeout_ms = timeout;
    ro.cancelled = [&] { return progress && !progress(at, "OpenFOAM: " + app); };
    std::vector<fs::path> a;
    fs::path program;
    if (!foam.wrapper.empty()) program = foam.wrapper, a.push_back(app);
    else program = foam.bin / app;
    for (const auto& s : args) a.push_back(s);
    a.push_back("-case");
    a.push_back(where);
    const int status = detail::run_program(program, a, where, ro);
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
  };

  // Each part: an STL in metres (snappyHexMesh), its triangles kept for showing the results.
  std::vector<std::string> region;
  std::vector<std::vector<V>> tri_pts(bodies.size());  // three points per triangle, mm
  for (size_t i = 0; i < bodies.size(); ++i) {
    region.push_back(is_air[i] ? std::string() : "s" + std::to_string(i));
    if (is_air[i]) continue;
    BRepMesh_IncrementalMesh(world[i], std::max(0.02, coarse / std::pow(2.0, level[i]) / 4), false, 0.3, true);
    std::ostringstream stl;
    stl.precision(10);
    stl << "solid " << region[i] << "\n";
    for (TopExp_Explorer e(world[i], TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face face = TopoDS::Face(e.Current());
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
      if (tri.IsNull()) continue;
      const bool rev = face.Orientation() == TopAbs_REVERSED;
      for (int t = 1; t <= tri->NbTriangles(); ++t) {
        int n[3];
        tri->Triangle(t).Get(n[0], n[1], n[2]);
        if (rev) std::swap(n[1], n[2]);
        V p[3];
        for (int k = 0; k < 3; ++k) {
          const gp_Pnt q = tri->Node(n[k]).Transformed(loc.Transformation());
          p[k] = {q.X(), q.Y(), q.Z()};
          tri_pts[i].push_back(p[k]);
        }
        const V nn = unit(cross(sub(p[1], p[0]), sub(p[2], p[0])));
        stl << " facet normal " << nn[0] << " " << nn[1] << " " << nn[2] << "\n  outer loop\n";
        for (const auto& q : p) stl << "   vertex " << q[0] * 1e-3 << " " << q[1] * 1e-3 << " " << q[2] * 1e-3 << "\n";
        stl << "  endloop\n endfacet\n";
      }
    }
    stl << "endsolid " << region[i] << "\n";
    fs::create_directories(cas / "constant" / "triSurface");
    write_text_file(cas / "constant" / "triSurface" / (region[i] + ".stl"), stl.str());
  }

  // Warm air rising (cfd.buoyancy; on by default in an enclosure without fans): the flow and the air's temperature solved
  // together at each pass between the air and the parts, the walls at the parts' temperatures. Its air is meshed without
  // snapping to the parts (their faces stepped to the cells): on snapped cells the solvers under gravity (Boussinesq or
  // compressible, steady or not) blew up from rounding alone, still air at one temperature included.
  const bool buoyant = !enclosure.empty() && cfd.value("buoyancy", disks.empty());
  // Sealed (cfd.sealed): only the air inside the enclosure, none of the room's; nothing goes in or out.
  const bool sealed = !enclosure.empty() && cfd.value("sealed", false);

  // ---- mesh: the duct, the parts cut out of it as cell zones, then one region each
  report(0.03, "Writing the OpenFOAM case");
  {
    std::ostringstream bm;
    bm << "convertToMeters 0.001;\nvertices (\n";
    for (const V& v : {corner(d0, a0, b0), corner(d1, a0, b0), corner(d1, a1, b0), corner(d0, a1, b0), corner(d0, a0, b1), corner(d1, a0, b1),
                       corner(d1, a1, b1), corner(d0, a1, b1)})
      bm << "  " << vec(v) << "\n";
    bm << ");\nblocks ( hex (0 1 2 3 4 5 6 7) (" << nx << " " << ny << " " << nz << ") simpleGrading (1 1 1) );\nboundary (\n"
       << (enclosure.empty() ? "  inlet { type patch; faces ((0 4 7 3)); }\n  outlet { type patch; faces ((1 2 6 5)); }\n"
                               "  walls { type wall; faces ((0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7)); }\n);\n"
                             : "  open { type patch; faces ((0 4 7 3) (1 2 6 5) (0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7)); }\n);\n");
    put(cas / "system" / "blockMeshDict", "dictionary", "blockMeshDict", bm.str());
    std::ostringstream sh;
    sh << "castellatedMesh true; snap " << (buoyant ? "false" : "true") << "; addLayers false;\ngeometry {\n";
    for (const auto& r : region)
      if (!r.empty()) sh << "  " << r << " { type triSurfaceMesh; file \"" << r << ".stl\"; }\n";
    for (size_t g = 0; g < gap_boxes.size(); ++g) sh << "  gap" << g << " { type searchableBox; min " << vec(gap_boxes[g].lo) << "; max " << vec(gap_boxes[g].hi) << "; }\n";
    sh << "}\ncastellatedMeshControls {\n  maxLocalCells 6000000; maxGlobalCells 12000000; minRefinementCells 0; maxLoadUnbalance 0.1; nCellsBetweenLevels 2;\n"
       << "  features ();\n  refinementSurfaces {\n";
    // In a duct, each part a region of the mesh (its cells kept, conjugate heat in OpenFOAM); in an enclosure only the air is
    // meshed, the parts its walls (their heat in CalculiX, below).
    for (size_t i = 0; i < region.size(); ++i)
      if (!region[i].empty()) {
        sh << "    " << region[i] << " { level (" << level[i] << " " << level[i] << ");";
        if (enclosure.empty()) sh << " faceZone " << region[i] << "; cellZone " << region[i] << "; cellZoneInside inside;";
        sh << " }\n";
      }
    // A point in the air: upstream in a duct, in the room's margin by a corner of an enclosure; in a sealed one, in its air.
    V seed = enclosure.empty() ? corner(d0 + 0.5 * (up + off), 0.5 * (a0 + a1), 0.5 * (b0 + b1)) : corner(d0 + 0.5 * margin, a0 + 0.5 * margin, b0 + 0.5 * margin);
    if (sealed) {
      // The first point of a grid over the enclosure's box that no part holds, a cell clear of their faces.
      Bnd_Box eb;
      BRepBndLib::Add(world[index_of(enclosure)], eb);
      double c[6];
      eb.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
      bool found = false;
      for (int i = 1; i < 12 && !found; ++i)
        for (int j = 1; j < 12 && !found; ++j)
          for (int k = 1; k < 12 && !found; ++k) {
            const gp_Pnt q(c[0] + (c[3] - c[0]) * (i + 0.13) / 12, c[1] + (c[4] - c[1]) * (j + 0.17) / 12, c[2] + (c[5] - c[2]) * (k + 0.19) / 12);
            bool free = true;
            for (size_t b = 0; b < world.size() && free; ++b) {
              if (is_air[b]) continue;
              BRepClass3d_SolidClassifier in(world[b], q, 1e-6);
              if (in.State() != TopAbs_OUT) free = false;
              else if (BRepExtrema_DistShapeShape(world[b], BRepBuilderAPI_MakeVertex(q).Vertex()).Value() < coarse) free = false;
            }
            if (free) seed = {q.X(), q.Y(), q.Z()}, found = true;
          }
      if (!found) throw Error("cfd.sealed: no air found inside the enclosure (is it hollow?)");
    }
    const V inside_air = mul(seed, 1e-3);
    sh << "  }\n  resolveFeatureAngle 30;\n  refinementRegions {";
    for (size_t g = 0; g < gap_boxes.size(); ++g) sh << " gap" << g << " { mode inside; levels ((1e15 " << gap_boxes[g].level << ")); }";
    sh << " }\n  locationInMesh " << vec(inside_air) << ";\n  allowFreeStandingZoneFaces false;\n}\n"
       << "snapControls { nSmoothPatch 3; tolerance 2.0; nSolveIter 50; nRelaxIter 5; nFeatureSnapIter 10; implicitFeatureSnap true; explicitFeatureSnap false; "
          "multiRegionFeatureSnap true; }\n"
       << "addLayersControls { relativeSizes true; layers {} expansionRatio 1.0; finalLayerThickness 0.3; minThickness 0.1; nGrow 0; featureAngle 60; "
          "nRelaxIter 3; nSmoothSurfaceNormals 1; nSmoothNormals 3; nSmoothThickness 10; maxFaceThicknessRatio 0.5; maxThicknessToMedialRatio 0.3; "
          "minMedialAxisAngle 90; nBufferCellsNoExtrude 0; nLayerIter 50; }\n"
       << "meshQualityControls { maxNonOrtho 65; maxBoundarySkewness 20; maxInternalSkewness 4; maxConcave 80; minVol 1e-18; minTetQuality 1e-15; "
          "minArea -1; minTwist 0.02; minDeterminant 0.001; minFaceWeight 0.05; minVolRatio 0.01; minTriangleTwist -1; nSmoothScale 4; "
          "errorReduction 0.75; relaxed { maxNonOrtho 75; } }\nmergeTolerance 1e-6;\n";
    put(cas / "system" / "snappyHexMeshDict", "dictionary", "snappyHexMeshDict", sh.str());
    put(cas / "system" / "controlDict", "dictionary", "controlDict",
        "application chtMultiRegionSimpleFoam; startFrom latestTime; startTime 0; stopAt endTime; endTime 1; deltaT 1; writeControl timeStep; "
        "writeInterval 1; purgeWrite 1; writeFormat ascii; writePrecision 10; writeCompression off; timeFormat general; timePrecision 8; "
        "runTimeModifiable false;\n");
    put(cas / "system" / "fvSchemes", "dictionary", "fvSchemes",
        "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; } laplacianSchemes { default Gauss "
        "linear corrected; } interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
    put(cas / "system" / "fvSolution", "dictionary", "fvSolution", "\n");
  }
  const auto t_mesh = std::chrono::steady_clock::now();
  foam_run("blockMesh", {}, cas, 0.05);
  report(0.08, "Meshing the air around the parts (snappyHexMesh)");
  foam_run("snappyHexMesh", {"-overwrite"}, cas, 0.08);
  // The air: in a duct, the cells in no part's zone, which splitMeshRegions names domain0; in an enclosure, the whole mesh.
  const std::string fluid = "domain0";
  std::vector<std::string> solids;
  if (enclosure.empty()) {
    foam_run("splitMeshRegions", {"-cellZones", "-overwrite"}, cas, 0.3);
    if (!fs::is_directory(cas / "constant" / fluid / "polyMesh")) throw Error("cfd: the mesh has no air around the parts");
    for (size_t i = 0; i < region.size(); ++i)
      if (region[i].empty()) continue;
      else if (fs::is_directory(cas / "constant" / region[i] / "polyMesh")) solids.push_back(region[i]);
      else run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" was too thin for the cell size and is left out: set cfd.cell_size smaller");
  }
  const fs::path air_mesh = enclosure.empty() ? cas / "constant" / fluid / "polyMesh" : cas / "constant" / "polyMesh";
  timing["mesh"] = seconds_since(t_mesh);
  // The fans: a face zone of the air's faces each disk crosses, made a pair of baffles the fan's pressure jumps across.
  std::vector<size_t> disk_faces;
  if (!disks.empty()) {
    std::ostringstream ts, cb;
    ts << "actions (\n";
    cb << "internalFacesOnly true;\nbaffles {\n";
    for (size_t k = 0; k < disks.size(); ++k) {
      const std::string f = "fan" + std::to_string(k);
      ts << " { name " << f << "; type faceZoneSet; action new; source searchableSurfaceToFaceZone; surface searchableDisk; origin "
         << vec(mul(disks[k].centre, 1e-3)) << "; normal " << vec(disks[k].normal) << "; radius " << num(disks[k].radius * 1e-3) << "; }\n";
      cb << "  " << f << " { type faceZone; zoneName " << f << "; patches { master { name " << f << "_in; type cyclic; neighbourPatch " << f
         << "_out; } slave { name " << f << "_out; type cyclic; neighbourPatch " << f << "_in; } } }\n";
    }
    put(cas / "system" / "topoSetDict", "dictionary", "topoSetDict", ts.str() + ");\n");
    put(cas / "system" / "createBafflesDict", "dictionary", "createBafflesDict", cb.str() + "}\n");
    const std::string log = foam_run("topoSet", {}, cas, 0.31);
    for (size_t k = 0; k < disks.size(); ++k) {
      // "faceZoneSet fan0 now size N"
      const std::string key = "fan" + std::to_string(k) + " now size ";
      const size_t at = log.rfind(key);
      const size_t n = at == std::string::npos ? 0 : size_t(std::strtoul(log.c_str() + at + key.size(), nullptr, 10));
      if (n == 0) throw Error("fan \"" + disks[k].load->name + "\": its disk crosses no air (is it inside a part?)");
      disk_faces.push_back(n);
    }
    foam_run("createBaffles", {"-overwrite"}, cas, 0.315);
  }
  // Each fan's disk as the mesh has it: the area of the faces it crosses (the solver's mean speed through the fan is over
  // these, not the circle's area: a few percent apart).
  std::vector<double> disk_area(disks.size(), 0.0);
  if (!disks.empty()) {
    const auto P = read_points(read_text_file(air_mesh / "points"));
    const auto F = read_faces(read_text_file(air_mesh / "faces"));
    for (const auto& pt : read_boundary(read_text_file(air_mesh / "boundary")))
      for (size_t k = 0; k < disks.size(); ++k)
        if (pt.name == "fan" + std::to_string(k) + "_in")
          for (size_t f = pt.start; f < pt.start + pt.faces; ++f) {
            V c{0, 0, 0}, area{0, 0, 0};
            for (int q : F[f]) c = add(c, P[size_t(q)]);
            c = mul(c, 1.0 / double(F[f].size()));
            for (size_t q = 0; q < F[f].size(); ++q) area = add(area, mul(cross(sub(P[size_t(F[f][q])], c), sub(P[size_t(F[f][(q + 1) % F[f].size()])], c)), 0.5));
            disk_area[k] += norm(area);
          }
  }

  // Streamlines over the air's cells, with its velocity (m/s) and temperature (K) in each.
  auto trace = [&](FeaResult& res, const Cells& air_cells, const std::vector<double>& Uc, const std::vector<double>& Tc) {
    // Streamlines: from points across the inlet in front of the parts, along the air's velocity (the nearest cell's, in
    // steps of half the finest cell, the midpoint rule) until the outlet, the duct's side, still air or a part.
    std::vector<V> at_mm;
    std::vector<double> cell_mm;  // each air cell's size
    for (size_t c = 0; c < air_cells.centre.size(); ++c)
      at_mm.push_back(mul(air_cells.centre[c], 1e3)), cell_mm.push_back(std::cbrt(std::max(0.0, air_cells.volume[c])) * 1e3);
    const Nearest near(at_mm, coarse);
    double mean_speed = 0;
    for (size_t c = 0; c < at_mm.size(); ++c) mean_speed += norm({Uc[3 * c], Uc[3 * c + 1], Uc[3 * c + 2]});
    mean_speed /= std::max<size_t>(1, at_mm.size());
    auto velocity = [&](const V& p, size_t& cell) {
      cell = near(p);
      if (norm(sub(at_mm[cell], p)) > 1.2 * cell_mm[cell]) return V{0, 0, 0};  // inside a part
      return V{Uc[3 * cell], Uc[3 * cell + 1], Uc[3 * cell + 2]};
    };
    const int across = cfd.value("streamlines", 10);
    // Seeds: across the inlet in front of the parts; in an enclosure, across each fan's disk just before it.
    std::vector<V> seeds;
    if (enclosure.empty()) {
      const double wa = hi[1] - lo[1], wb = hi[2] - lo[2];
      const int na = std::max(1, int(std::lround(across * std::sqrt(wa / std::max(1e-9, wb))))),
                nb = std::max(1, int(std::lround(across * std::sqrt(wb / std::max(1e-9, wa)))));
      for (int i = 0; i < na; ++i)
        for (int j = 0; j < nb; ++j) seeds.push_back(corner(d0 + 2 * fine, lo[1] + wa * (i + 0.5) / na, lo[2] + wb * (j + 0.5) / nb));
    } else {
      for (const Disk& d : disks) {
        const V u1 = unit(cross(d.normal, std::fabs(d.normal[0]) < 0.9 ? V{1, 0, 0} : V{0, 1, 0})), u2 = unit(cross(d.normal, u1));
        for (int i = 0; i < across; ++i)
          for (int j = 0; j < across; ++j) {
            const double x = -1 + 2 * (i + 0.5) / across, y = -1 + 2 * (j + 0.5) / across;
            if (x * x + y * y <= 1) seeds.push_back(add(add(sub(d.centre, mul(d.normal, fine)), mul(u1, x * d.radius)), mul(u2, y * d.radius)));
          }
      }
    }
    const double reach = norm({d1 - d0, a1 - a0, b1 - b0});
    for (V p : seeds) {
      std::vector<Vec3> line;
      std::vector<double> speed, temp;
      size_t cell = 0;
      for (int step = 0; step < int(16 * reach / fine); ++step) {
        const V u = velocity(p, cell);
        const double sp = norm(u), h = 0.5 * cell_mm[cell];  // half the cell it is in
        if (sp < 1e-3 * mean_speed) break;
        if (step % 2 == 0 || line.empty()) {
          line.push_back(p);
          speed.push_back(sp);
          temp.push_back(Tc[cell] - 273.15);
        }
        size_t c2 = 0;
        const V um = velocity(add(p, mul(u, 0.5 * h / sp)), c2);
        if (norm(um) < 1e-3 * mean_speed) break;
        p = add(p, mul(um, h / norm(um)));
        const double d = dot(p, way), ea = dot(p, e1), eb = dot(p, e2);
        if (d > d1 - 0.5 * fine || d < d0 + (enclosure.empty() ? 0 : 0.5 * fine) || ea < a0 + 0.5 * fine || ea > a1 - 0.5 * fine ||
            eb < b0 + 0.5 * fine || eb > b1 - 0.5 * fine)
          break;
      }
      if (line.size() < 3) continue;
      res.streamlines.push_back(std::move(line));
      res.streamline_speed.push_back(std::move(speed));
      res.streamline_temperature.push_back(std::move(temp));
    }
  };

  // ---- the flow alone (simpleFoam, laminar), on the air's mesh
  const air::Air a = air::properties(ambient);
  json fan_json;
  {
    fs::create_directories(flow / "constant");
    fs::copy(air_mesh, flow / "constant" / "polyMesh", fs::copy_options::recursive);
    const int iters = cfd.value("flow_iterations", 1000);
    put(flow / "system" / "controlDict", "dictionary", "controlDict",
        "application simpleFoam; startFrom startTime; startTime 0; stopAt endTime; endTime " + std::to_string(iters) +
            "; deltaT 1; writeControl timeStep; writeInterval " + std::to_string(iters) +
            "; writeFormat ascii; writePrecision 10; timeFormat general; runTimeModifiable false;\n");
    put(flow / "system" / "fvSchemes", "dictionary", "fvSchemes",
        "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; div(phi,U) bounded Gauss "
        "upwind; div((nuEff*dev2(T(grad(U))))) Gauss linear; } laplacianSchemes { default Gauss linear corrected; } "
        "interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
    put(flow / "system" / "fvSolution", "dictionary", "fvSolution",
        "solvers { p { solver GAMG; smoother GaussSeidel; tolerance 1e-8; relTol 0.01; } U { solver smoothSolver; smoother GaussSeidel; "
        "tolerance 1e-8; relTol 0.1; } }\nSIMPLE { nNonOrthogonalCorrectors 1; consistent no; residualControl { p 1e-4; U 1e-5; } }\n"
        "relaxationFactors { fields { p 0.5; } equations { U 0.7; } }\n");
    put(flow / "constant" / "transportProperties", "dictionary", "transportProperties", "transportModel Newtonian; nu " + num(a.nu) + ";\n");
    put(flow / "constant" / "turbulenceProperties", "dictionary", "turbulenceProperties", "simulationType laminar;\n");
    std::string u_bc, p_bc;
    if (!enclosure.empty()) {
      // Open to the room on every side (its total pressure the room's), each fan a jump in pressure across its disk: its
      // curve against the mean speed through the disk, kinematic (Pa / rho).
      u_bc = "open { type pressureInletOutletVelocity; value uniform (0 0 0); }\n  \"fan[0-9]+_(in|out)\" { type cyclic; }";
      p_bc = "open { type totalPressure; p0 uniform 0; value uniform 0; }";
      for (size_t k = 0; k < disks.size(); ++k) {
        const air::Fan& fan = disks[k].fan;
        const double A = disk_area[k] > 0 ? disk_area[k] : kPi * std::pow(disks[k].radius * 1e-3, 2);
        std::ostringstream t;
        const int n = fan.curve.size() >= 2 ? int(fan.curve.size()) : 2;
        for (int i = 0; i < n; ++i) {
          const double Q = fan.curve.size() >= 2 ? fan.curve[size_t(i)][0] : fan.Qmax * i / (n - 1);
          t << " (" << num(Q / A) << " " << num(fan.pressure(Q) / a.rho) << ")";
        }
        const std::string f = "fan" + std::to_string(k);
        p_bc += "\n  " + f + "_in { type fan; patchType cyclic; uniformJump true; jumpTable table (" + t.str() + " ); value uniform 0; }\n  " + f +
                "_out { type fan; patchType cyclic; value uniform 0; }";
      }
    } else if (fan_load) {
      const air::Fan fan = air::fan_from(fan_load->def.value("fan", json("80x25")));
      const int count = fan_load->def.value("count", 1);
      // The fan's curve in kinematic pressure (Pa / rho) against the duct's flow, count fans side by side.
      std::ostringstream curve;
      curve << "(";
      const int n = fan.curve.size() >= 2 ? int(fan.curve.size()) : 2;
      for (int i = 0; i < n; ++i) {
        const double Q = fan.curve.size() >= 2 ? fan.curve[size_t(i)][0] : fan.Qmax * i / (n - 1);
        curve << " (" << num(Q * count) << " " << num(fan.pressure(Q) / a.rho) << ")";
      }
      curve << " )\n";
      write_text_file(flow / "constant" / "fanCurve", curve.str());
      u_bc = "inlet { type pressureInletOutletVelocity; value uniform (0 0 0); }\n  outlet { type inletOutlet; inletValue uniform (0 0 0); value uniform (0 0 0); }";
      p_bc = "inlet { type fanPressure; file \"$FOAM_CASE/constant/fanCurve\"; outOfBounds clamp; direction in; p0 uniform 0; value uniform 0; }\n"
             "  outlet { type fixedValue; value uniform 0; }";
      fan_json = {{"name", fan_load->name}, {"fan", fan.to_json()}, {"fans", count}};
    } else {
      const double U = stream->def.value("velocity", 1.0);
      u_bc = "inlet { type fixedValue; value uniform " + vec(mul(way, U)) + "; }\n  outlet { type inletOutlet; inletValue uniform (0 0 0); value uniform (0 0 0); }";
      p_bc = "inlet { type zeroGradient; }\n  outlet { type fixedValue; value uniform 0; }";
    }
    put(flow / "0" / "U", "volVectorField", "U",
        "dimensions [0 1 -1 0 0 0 0];\ninternalField uniform (0 0 0);\nboundaryField\n{\n  \".*\" { type noSlip; }\n  " + u_bc + "\n}\n");
    put(flow / "0" / "p", "volScalarField", "p",
        "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type zeroGradient; }\n  " + p_bc + "\n}\n");
    auto mean = [](const std::vector<double>& v) {
      double m = 0;
      for (double x : v) m += x;
      return v.empty() ? 0.0 : m / double(v.size());
    };
    // What the flow did: each fan's flow and pressure and the air through the box (an enclosure), or the duct's flow.
    auto flow_report = [&](const std::string& phi, const std::string& p, int iterations, bool converged) {
      if (!enclosure.empty()) {
        json list = json::array();
        for (size_t k = 0; k < disks.size(); ++k) {
          const std::string f = "fan" + std::to_string(k);
          double Q = 0;
          for (double v : read_patch(phi, f + "_in")) Q += v;  // along the disk's normal: the way the fan blows
          const double jump = (mean(read_patch(p, f + "_out")) - mean(read_patch(p, f + "_in"))) * a.rho;
          if (Q < 0) run.warnings.push_back("fan \"" + disks[k].load->name + "\": the air went through it backwards (" + num(-Q * 3600) + " m3/h)");
          list.push_back({{"name", disks[k].load->name}, {"fan", disks[k].fan.to_json()}, {"flow_m3h", Q * 3600}, {"flow_cfm", Q / 4.719474e-4},
                          {"pressure_Pa", jump}, {"disk", {{"centre", disks[k].centre}, {"normal", disks[k].normal}, {"radius_mm", disks[k].radius},
                                                           {"faces", disk_faces[k]}}}});
        }
        double in = 0, out = 0;
        for (double v : read_patch(phi, "open")) (v > 0 ? out : in) += std::fabs(v);
        fan_json = {{"fans", list}, {"vents", {{"air_in_m3h", in * 3600}, {"air_out_m3h", out * 3600}}}, {"iterations", iterations}, {"converged", converged}};
      } else {
        double Q = 0;
        for (double v : read_patch(phi, "inlet")) Q -= v;
        const double pmean = mean(read_patch(p, "inlet"));
        json flow_json = {{"flow_m3h", Q * 3600}, {"flow_cfm", Q / 4.719474e-4}, {"inlet_static_Pa", pmean * a.rho}, {"pressure_Pa", pmean * a.rho},
                          {"iterations", iterations}, {"converged", converged}, {"inlet_velocity_m_s", Q / ((a1 - a0) * (b1 - b0) * 1e-6)}};
        if (!fan_json.is_null())
          for (const auto& [k, v] : flow_json.items()) fan_json[k] = v;
        else
          fan_json = flow_json;
      }
    };
    std::string last, phi, p;
    const auto t_flow = std::chrono::steady_clock::now();
    // A sealed box with no fans and no gravity: its air stands still and only conducts.
    const bool still = sealed && disks.empty() && !buoyant;
    if (still) {
      phi = "FoamFile { version 2.0; format ascii; class surfaceScalarField; object phi; }\ndimensions [0 3 -1 0 0 0 0];\n"
            "internalField uniform 0;\nboundaryField\n{\n  \".*\" { type calculated; value uniform 0; }\n}\n";
    } else if (!buoyant) {
      report(0.32, "Solving the air's flow (simpleFoam)");
      const std::string log = foam_run("simpleFoam", {}, flow, 0.32);
      last = latest_time(flow);
      if (last.empty() || last == "0") throw Error("cfd: simpleFoam wrote no flow");
      const bool converged = log.find("SIMPLE solution converged") != std::string::npos;
      if (!converged) run.warnings.push_back("the air's flow had not settled after " + std::to_string(iters) + " iterations (cfd.flow_iterations)");
      phi = read_text_file(flow / last / "phi"), p = read_text_file(flow / last / "p");
      flow_report(phi, p, std::stoi(last), converged);
    }
    timing["flow"] = seconds_since(t_flow);
    if (!enclosure.empty()) {
      // ---- the heat in an enclosure: the parts in CalculiX (sim/fea.cpp, one bonded mesh: touching parts conduct as one),
      // the air's temperature in OpenFOAM on the frozen flow (scalarTransportFoam, one linear solve), back and forth: the
      // parts' surfaces fix the air's walls; the heat each face then gives the air is its film, against the room's
      // temperature (or, where the air is the warmer, against the air next to it), until the temperatures settle.
      const fs::path heat = buoyant ? flow : dir.p / "heat";  // where the air's temperature is solved
      std::string U;
      if (!buoyant) {
        fs::create_directories(heat / "constant");
        fs::copy(air_mesh, heat / "constant" / "polyMesh", fs::copy_options::recursive);
        U = still ? "FoamFile { version 2.0; format ascii; class volVectorField; object U; }\ndimensions [0 1 -1 0 0 0 0];\n"
                    "internalField uniform (0 0 0);\nboundaryField\n{\n  \".*\" { type fixedValue; value uniform (0 0 0); }\n}\n"
                  : read_text_file(flow / last / "U");
        for (const char* from : {"pressureInletOutletVelocity"})
          for (size_t at = U.find(from); at != std::string::npos; at = U.find(from)) U.replace(at, std::strlen(from), "fixedValue");
        fs::create_directories(heat / "0");
        write_text_file(heat / "0" / "U", U);
        write_text_file(heat / "0" / "phi", phi);
        put(heat / "constant" / "transportProperties", "dictionary", "transportProperties", "DT DT [0 2 -1 0 0 0 0] " + num(a.alpha) + ";\n");
        put(heat / "system" / "controlDict", "dictionary", "controlDict",
            "application scalarTransportFoam; startFrom startTime; startTime 0; stopAt endTime; endTime 1; deltaT 1; writeControl timeStep; "
            "writeInterval 1; writeFormat ascii; writePrecision 10; timeFormat general; runTimeModifiable false;\n");
        put(heat / "system" / "fvSchemes", "dictionary", "fvSchemes",
            "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; div(phi,T) bounded Gauss " + heat_scheme + "; } "
            "laplacianSchemes { default Gauss linear corrected; } interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
        put(heat / "system" / "fvSolution", "dictionary", "fvSolution",
            "solvers { T { solver PBiCGStab; preconditioner DILU; tolerance 1e-10; relTol 0; } }\nSIMPLE { nNonOrthogonalCorrectors 1; }\n");
      } else {
        // Boussinesq: the air's density falls with its temperature (beta = 1 / T) under gravity (settings.gravity, down).
        const V down = unit(st.value("gravity", V{0, 0, -1}));
        put(flow / "constant" / "g", "uniformDimensionedVectorField", "g", "dimensions [0 1 -2 0 0 0 0];\nvalue " + vec(mul(down, 9.80665)) + ";\n");
        put(flow / "constant" / "transportProperties", "dictionary", "transportProperties",
            "transportModel Newtonian; nu " + num(a.nu) + "; beta " + num(a.beta) + "; TRef " + num(T0) + "; Pr " + num(a.Pr) + "; Prt 0.85;\n");
        put(flow / "system" / "fvSchemes", "dictionary", "fvSchemes",
            "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; div(phi,U) bounded Gauss upwind; "
            "div(phi,T) bounded Gauss " + heat_scheme + "; div((nuEff*dev2(T(grad(U))))) Gauss linear; } laplacianSchemes { default Gauss linear corrected; } "
            "interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
        put(flow / "system" / "fvSolution", "dictionary", "fvSolution",
            "solvers { p_rgh { solver GAMG; smoother GaussSeidel; tolerance 1e-8; relTol 0.01; } \"(U|T)\" { solver PBiCGStab; preconditioner DILU; "
            "tolerance 1e-8; relTol 0.1; } }\nSIMPLE { momentumPredictor yes; nNonOrthogonalCorrectors 0; pRefCell 0; pRefValue 0; }\n"
            "relaxationFactors { fields { p_rgh 0.7; } equations { U 0.3; T 0.5; } }\n");
        std::string pfan;  // the fans' jumps, on p_rgh
        for (size_t k = 0; k < disks.size(); ++k) {
          const air::Fan& fan = disks[k].fan;
          const double A = disk_area[k] > 0 ? disk_area[k] : kPi * std::pow(disks[k].radius * 1e-3, 2);
          std::ostringstream t;
          const int n = fan.curve.size() >= 2 ? int(fan.curve.size()) : 2;
          for (int i = 0; i < n; ++i) {
            const double Q = fan.curve.size() >= 2 ? fan.curve[size_t(i)][0] : fan.Qmax * i / (n - 1);
            t << " (" << num(Q / A) << " " << num(fan.pressure(Q) / a.rho) << ")";
          }
          const std::string f = "fan" + std::to_string(k);
          pfan += "  " + f + "_in { type fan; patchType cyclic; uniformJump true; jumpTable table (" + t.str() + " ); value uniform 0; }\n  " + f +
                  "_out { type fan; patchType cyclic; value uniform 0; }\n";
        }
        const std::string cyc = "  \"fan[0-9]+_(in|out)\" { type cyclic; }\n";
        put(flow / "0" / "U", "volVectorField", "U",
            "dimensions [0 1 -1 0 0 0 0];\ninternalField uniform (0 0 0);\nboundaryField\n{\n  \".*\" { type noSlip; }\n  open { type "
            "pressureInletOutletVelocity; value uniform (0 0 0); }\n" + cyc + "}\n");
        put(flow / "0" / "p_rgh", "volScalarField", "p_rgh",
            "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type fixedFluxPressure; value uniform 0; }\n  open { type "
            "fixedValue; value uniform 0; }\n" + pfan + "}\n");
        put(flow / "0" / "p", "volScalarField", "p",
            "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type calculated; value uniform 0; }\n" + cyc + "}\n");
        put(flow / "0" / "alphat", "volScalarField", "alphat",
            "dimensions [0 2 -1 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type calculated; value uniform 0; }\n" + cyc + "}\n");
        fs::remove(flow / "0" / "U.orig");
      }
      // The parts' walls: each face's centre, area, normal, the cell behind it and its distance to the face.
      const auto P = read_points(read_text_file(air_mesh / "points"));
      const auto F = read_faces(read_text_file(air_mesh / "faces"));
      const auto own = read_labels(read_text_file(air_mesh / "owner"));
      const auto patches = read_boundary(read_text_file(air_mesh / "boundary"));
      const Cells air_cells = read_cells(air_mesh);
      std::vector<int> fea_body(bodies.size(), -1);  // the study's body index in CalculiX
      json fea_bodies = json::array();
      for (size_t i = 0; i < bodies.size(); ++i)
        if (!is_air[i]) fea_body[i] = int(fea_bodies.size()), fea_bodies.push_back(bodies[i]);
      struct Wall {
        int body;
        V centre;  // mm
        double area, k_d;  // m2, W/m2K: the air's conductance from the face to the cell behind it
        size_t cell;
      };
      std::vector<std::pair<std::string, std::vector<size_t>>> wall_patches;  // patch -> indices into walls
      std::vector<Wall> walls;
      for (const auto& pt : patches) {
        int body = -1;
        for (size_t i = 0; i < region.size(); ++i)
          if (!region[i].empty() && (pt.name == region[i] || pt.name.rfind(region[i] + "_", 0) == 0)) body = fea_body[i];
        if (body < 0) continue;
        wall_patches.push_back({pt.name, {}});
        for (size_t f = pt.start; f < pt.start + pt.faces; ++f) {
          V c{0, 0, 0}, area{0, 0, 0};
          for (int k : F[f]) c = add(c, P[size_t(k)]);
          c = mul(c, 1.0 / double(F[f].size()));
          for (size_t k = 0; k < F[f].size(); ++k)
            area = add(area, mul(cross(sub(P[size_t(F[f][k])], c), sub(P[size_t(F[f][(k + 1) % F[f].size()])], c)), 0.5));
          const double A = norm(area);
          const size_t cell = size_t(own[f]);
          const double d = A > 0 ? std::fabs(dot(sub(c, air_cells.centre[cell]), mul(area, 1 / A))) : 0;
          wall_patches.back().second.push_back(walls.size());
          walls.push_back({body, mul(c, 1e3), A, a.k / std::max(d, 1e-6), cell});
        }
      }
      if (walls.empty()) throw Error("cfd: the air's mesh has no walls on the parts");
      std::string T_other = "  \".*\" { type zeroGradient; }\n  open { type inletOutlet; inletValue uniform " + num(T0) + "; value uniform " + num(T0) +
                            "; }\n  \"fan[0-9]+_(in|out)\" { type cyclic; }\n";
      std::vector<double> Tair;               // K, each air cell, the last pass
      std::vector<double> Twall(walls.size());  // degC
      std::vector<std::unique_ptr<Nearest>> near_face;
      std::vector<std::vector<size_t>> face_ids;  // per body: indices into the faces CalculiX gives
      std::vector<std::vector<V>> face_centres;
      std::vector<std::unique_ptr<Nearest>> near_wall;
      std::vector<std::vector<V>> wall_centres;
      std::vector<std::vector<size_t>> wall_ids;
      int passes = 0, buoyant_done = 0;
      double air_seconds = 0;
      // The heat the air carries out of the room's margin (W): out where it flows out, at the temperature of the cell it
      // leaves (upwind; where it flows in it is the room's).
      std::vector<size_t> open_cell;
      for (const auto& pt : patches)
        if (pt.name == "open")
          for (size_t f = pt.start; f < pt.start + pt.faces; ++f) open_cell.push_back(size_t(own[f]));
      auto air_leaving = [&](const std::string& phi_text, const std::vector<double>& T) {
        const auto flux = read_patch(phi_text, "open");
        double q = 0;
        for (size_t k = 0; k < flux.size() && k < open_cell.size(); ++k)
          if (flux[k] > 0) q += a.rho * a.cp * flux[k] * (T[open_cell[k]] - T0);
        return q;
      };
      std::vector<double> h0;  // each face's film once settled on (W/m2K)
      std::vector<double> target, last_r;  // the temperature each film should work against; the last pass's residual
      double omega = 0.7;
      const AirFilms films = [&](std::vector<AirFace>& faces, int pass) {
        if (pass == 0) {
          h0.assign(faces.size(), 0.0), target.assign(faces.size(), 0.0);
          face_ids.assign(fea_bodies.size(), {}), face_centres.assign(fea_bodies.size(), {});
          wall_ids.assign(fea_bodies.size(), {}), wall_centres.assign(fea_bodies.size(), {});
          for (size_t k = 0; k < faces.size(); ++k) face_ids[size_t(faces[k].body)].push_back(k), face_centres[size_t(faces[k].body)].push_back(faces[k].centre);
          for (size_t w = 0; w < walls.size(); ++w) wall_ids[size_t(walls[w].body)].push_back(w), wall_centres[size_t(walls[w].body)].push_back(walls[w].centre);
          for (size_t b = 0; b < fea_bodies.size(); ++b) {
            near_face.push_back(std::make_unique<Nearest>(face_centres[b], coarse));
            near_wall.push_back(std::make_unique<Nearest>(wall_centres[b], coarse));
          }
        }
        ++passes;
        const auto t_pass = std::chrono::steady_clock::now();
        struct Clock {
          std::chrono::steady_clock::time_point t;
          double& sum;
          ~Clock() { sum += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }
        } clock{t_pass, air_seconds};
        report(std::min(0.9, 0.45 + 0.02 * pass), "The air's temperature, pass " + std::to_string(pass + 1));
        // The walls at the parts' temperatures: each wall face the nearest face CalculiX solved.
        for (size_t w = 0; w < walls.size(); ++w) {
          const size_t b = size_t(walls[w].body);
          Twall[w] = face_ids[b].empty() ? ambient : faces[face_ids[b][(*near_face[b])(walls[w].centre)]].T;
        }
        std::ostringstream bc;
        bc.precision(10);
        bc << T_other;
        for (const auto& [name, ids] : wall_patches) {
          bc << "  " << name << " { type fixedValue; value nonuniform List<scalar> " << ids.size() << " (";
          for (size_t w : ids) bc << " " << Twall[w] + 273.15;
          bc << " ); }\n";
        }
        if (!buoyant) {
          put(heat / "0" / "T", "volScalarField", "T", "dimensions [0 0 0 1 0 0 0];\ninternalField uniform " + num(T0) + ";\nboundaryField\n{\n" + bc.str() + "}\n");
          foam_run("scalarTransportFoam", {}, heat, std::min(0.9, 0.45 + 0.02 * pass));
          Tair = read_internal(read_text_file(heat / "1" / "T"), air_cells.centre.size(), 1);
        } else {
          // Going on from the last pass's flow and temperatures, the walls at the parts' new ones.
          const std::string now = latest_time(flow);
          std::ostringstream internal;
          internal.precision(10);
          if (Tair.empty()) {
            internal << "uniform " << T0;
          } else {
            internal << "nonuniform List<scalar> " << Tair.size() << " (";
            for (double v : Tair) internal << " " << v;
            internal << " )";
          }
          put(flow / now / "T", "volScalarField", "T", "dimensions [0 0 0 1 0 0 0];\ninternalField " + internal.str() + ";\nboundaryField\n{\n" + bc.str() + "}\n");
          // In chunks until the heat the walls give the air leaves the room's margin (within 5 %): before that, the warm air
          // is still filling the box and the films would be the box's warming up, not its steady state.
          for (int chunk = 0; chunk < (sealed ? 1 : cfd.value("buoyant_chunks", 12)); ++chunk) {
            const int more = pass == 0 && chunk == 0 ? cfd.value("buoyant_first", 600) : cfd.value("buoyant_pass", 200);
            buoyant_done += more;
            put(flow / "system" / "controlDict", "dictionary", "controlDict",
                "application buoyantBoussinesqSimpleFoam; startFrom latestTime; startTime 0; stopAt endTime; endTime " + std::to_string(buoyant_done) +
                    "; deltaT 1; writeControl timeStep; writeInterval " + std::to_string(more) +
                    "; purgeWrite 1; writeFormat ascii; writePrecision 10; timeFormat general; runTimeModifiable false;\n");
            foam_run("buoyantBoussinesqSimpleFoam", {}, flow, std::min(0.9, 0.45 + 0.02 * pass));
            const std::string now = latest_time(flow);
            Tair = read_internal(read_text_file(flow / now / "T"), air_cells.centre.size(), 1);
            double given = 0;
            for (size_t w = 0; w < walls.size(); ++w) given += walls[w].k_d * (Twall[w] + 273.15 - Tair[walls[w].cell]) * walls[w].area;
            const double carried = air_leaving(read_text_file(flow / now / "phi"), Tair);
            if (std::fabs(carried - given) <= 0.05 * std::fabs(given) + 1e-3) break;
          }
        }
        // Each wall face's heat into the air, gathered onto the faces CalculiX has.
        std::vector<double> qA(faces.size(), 0), A(faces.size(), 0), kdA(faces.size(), 0), TcA(faces.size(), 0);
        for (size_t w = 0; w < walls.size(); ++w) {
          const size_t b = size_t(walls[w].body);
          if (face_ids[b].empty()) continue;
          const size_t k = face_ids[b][(*near_face[b])(walls[w].centre)];
          const double Tc = Tair[walls[w].cell] - 273.15;
          qA[k] += walls[w].k_d * (Twall[w] - Tc) * walls[w].area;
          A[k] += walls[w].area, kdA[k] += walls[w].k_d * walls[w].area, TcA[k] += Tc * walls[w].area;
        }
        for (size_t k = 0; k < faces.size(); ++k) {
          AirFace& f = faces[k];
          double q, kd, Tc;
          if (A[k] > 0) {
            q = qA[k] / A[k], kd = kdA[k] / A[k], Tc = TcA[k] / A[k];
          } else {  // smaller than the air's cells there: the nearest wall face's, if the air is there at all
            const size_t b = size_t(f.body);
            const size_t w = wall_ids[b].empty() ? 0 : wall_ids[b][(*near_wall[b])(f.centre)];
            if (wall_ids[b].empty() || norm(sub(walls[w].centre, f.centre)) > 1.5 * coarse) {
              // No air beside it (a sealed box's outside, a gap too narrow for the cells): insulated.
              f.h = 0, f.sink = ambient;
              continue;
            }
            Tc = Tair[walls[w].cell] - 273.15, kd = walls[w].k_d, q = kd * (Twall[w] - Tc);
          }
          // The first passes: the face's film against the room's temperature (against the air next to it where that is the
          // warmer). Then that film stays and only the temperature it works against moves (relaxed), so that the film gives
          // the heat the air took: switching faces between the two, or the films chasing the temperatures, kept the
          // passes from settling (a degree up and down after thirty).
          if (pass < 2 || h0[k] <= 0) {
            if (f.T - ambient > 0.2 && q > 0) f.h = std::clamp(q / (f.T - ambient), 1.0, 1e4), f.sink = ambient;
            else f.h = std::clamp(kd, 1.0, 1e4), f.sink = Tc;
            if (pass == 1) h0[k] = f.h;
          } else {
            f.h = h0[k];
            target[k] = f.T - q / h0[k];
          }
        }
        // The temperatures the films work against, relaxed by Aitken's factor from the last two passes' residuals.
        if (pass >= 2) {
          std::vector<double> r(faces.size());
          for (size_t k = 0; k < faces.size(); ++k) r[k] = h0[k] > 0 ? target[k] - faces[k].sink : 0.0;
          if (!last_r.empty()) {
            double num = 0, den = 0;
            for (size_t k = 0; k < r.size(); ++k) num += last_r[k] * (r[k] - last_r[k]), den += (r[k] - last_r[k]) * (r[k] - last_r[k]);
            if (den > 0) omega = std::clamp(-omega * num / den, 0.1, 1.5);
          }
          for (size_t k = 0; k < faces.size(); ++k)
            if (h0[k] > 0) faces[k].sink += omega * r[k];
          last_r = std::move(r);
        }
      };
      json fst = st;
      fst.erase("air");
      fst["cfd"] = cfd;
      fst["bodies"] = fea_bodies;
      const Progress inner = [&](double f, const std::string& phase) { return !progress || progress(0.4 + 0.55 * f, phase); };
      StudyRun solid = run_structural(doc, scene, "thermal", fst, inner, &films);
      // The air: what leaves the box's margin and the heat it carries off; streamlines with its temperatures.
      if (buoyant) {
        const std::string now = latest_time(flow);
        phi = read_text_file(flow / now / "phi");
        U = read_text_file(flow / now / "U");
        flow_report(phi, read_text_file(flow / now / "p_rgh"), buoyant_done, true);
      }
      const auto mass = read_patch(phi, "open");
      double mdot = 0, mt = 0;
      for (size_t k = 0; k < mass.size() && k < open_cell.size(); ++k)
        if (mass[k] > 0) mdot += mass[k], mt += mass[k] * Tair[open_cell[k]];
      const double carried = air_leaving(phi, Tair);
      // How well the air's heat balances: what the walls give it against what leaves the room's margin.
      double given = 0;
      for (size_t w = 0; w < walls.size(); ++w) given += walls[w].k_d * (Twall[w] + 273.15 - Tair[walls[w].cell]) * walls[w].area;
      const double imbalance = std::fabs(given) > 1e-6 ? std::fabs(carried - given) / std::fabs(given) : 0.0;
      if (imbalance > 0.1)
        run.warnings.push_back("the air's heat balance closes to " + std::to_string(int(std::lround(100 * imbalance))) + " %" +
                               (buoyant ? " (warm air rising does not settle fully: a Fine run checks it)" : ""));
      fan_json["vents"]["outlet_air_C"] = mdot > 0 ? mt / mdot - 273.15 : ambient;
      fan_json["vents"]["air_rise_C"] = (mdot > 0 ? mt / mdot - 273.15 : ambient) - ambient;
      fan_json["vents"]["heat_to_air_W"] = carried;
      auto with_air = std::make_shared<FeaResult>(*solid.fea);
      trace(*with_air, air_cells, read_internal(U, air_cells.centre.size(), 3), Tair);
      solid.fea = with_air;
      json summary = solid.summary;
      summary["air"] = "cfd";
      summary["engine"] = "OpenFOAM (snappyHexMesh, simpleFoam, scalarTransportFoam) and CalculiX";
      summary["enclosure"] = scene.node(enclosure)->name;
      summary["fans"] = fan_json["fans"];
      summary["vents"] = fan_json["vents"];
      summary["flow_iterations"] = fan_json["iterations"];
      summary["air_passes"] = passes;
      timing["air_passes"] = air_seconds;
      timing["calculix"] = solid.summary.value("ccx_seconds", 0.0);
      timing["total"] = seconds_since(t_start);
      summary["seconds"] = timing;
      summary["air_balance"] = {{"from_parts_W", given}, {"leaving_W", carried}};
      summary["air_cells"] = air_cells.centre.size();
      summary["cells"] = air_cells.centre.size();
      summary["cell_size_mm"] = fine;
      summary["refinement"] = refinement;
      summary["gap_boxes"] = gap_boxes.size();
      summary["streamlines"] = solid.fea->streamlines.size();
      summary["room_mm"] = {{"size", {d1 - d0, a1 - a0, b1 - b0}}, {"margin", margin}};
      summary["case_dir"] = run.summary["case_dir"];
      json warnings = solid.summary.value("warnings", json::array());
      for (const auto& w : run.warnings) warnings.push_back(w);
      for (const auto& w : solid.warnings) run.warnings.push_back(w);
      summary["warnings"] = run.warnings;
      solid.summary = summary;
      solid.warnings = run.warnings;
      return solid;
    }
    // The frozen flow for the heat: U as solved, its face fluxes as mass fluxes.
    fs::create_directories(cas / "0" / fluid);
    std::string U = read_text_file(flow / last / "U");
    for (const char* from : {"pressureInletOutletVelocity", "fanPressure"}) {
      for (size_t at = U.find(from); at != std::string::npos; at = U.find(from)) U.replace(at, std::strlen(from), "fixedValue");
    }
    write_text_file(cas / "0" / fluid / "U", U);
    std::string mass = phi;
    {
      const size_t dims = mass.find("[0 3 -1 0 0 0 0]");
      if (dims != std::string::npos) mass.replace(dims, 16, "[1 0 -1 0 0 0 0]");
      // Every real number after the header scaled by the density (counts and brackets stay).
      const size_t body = mass.find("dimensions");
      std::string out = mass.substr(0, body);
      out.reserve(mass.size() + mass.size() / 4);
      bool in_dims = false;  // inside [..]: the dimensions, never scaled
      for (size_t i = body; i < mass.size();) {
        const char c = mass[i];
        if (c == '[') in_dims = true;
        else if (c == ']') in_dims = false;
        const bool start = (std::isdigit(static_cast<unsigned char>(c)) || ((c == '-' || c == '.') && i + 1 < mass.size() && std::isdigit(static_cast<unsigned char>(mass[i + 1])))) &&
                           (i == 0 || std::isspace(static_cast<unsigned char>(mass[i - 1])) || mass[i - 1] == '(');
        if (!start) {
          out += c, ++i;
          continue;
        }
        size_t j = i + 1;
        while (j < mass.size() && (std::isdigit(static_cast<unsigned char>(mass[j])) || mass[j] == '.' || mass[j] == 'e' || mass[j] == 'E' || mass[j] == '-' || mass[j] == '+')) ++j;
        const std::string tok = mass.substr(i, j - i);
        const bool real = tok.find_first_of(".eE") != std::string::npos;
        out += real && !in_dims ? num(std::strtod(tok.c_str(), nullptr) * a.rho) : tok;
        i = j;
      }
      mass = out;
    }
    write_text_file(cas / "0" / fluid / "phi", mass);
  }

  // ---- the heat: conjugate, the flow frozen
  std::map<std::string, Thermal> props;  // conductivity, specific heat; density in rho below
  std::map<std::string, double> rho, power;
  for (size_t i = 0; i < bodies.size(); ++i) {
    Thermal th{50, 490, 0.3};
    double r = 7.85;
    const MaterialChoice c = material_of(doc, scene, bodies[i]);
    bool assumed = true;
    if (!c.id.empty())
      if (const Thermal* t = thermal(c.id)) th = *t, assumed = false;
    if (c.density > 0) r = c.density;
    const json o = st.value("materials", json::object()).value(bodies[i], st.value("materials", json::object()).value("all", json()));
    if (o.is_object()) {
      th.conductivity = o.value("k", th.conductivity), th.specific_heat = o.value("cp", th.specific_heat), r = o.value("density", r);
      assumed = assumed && !o.contains("k");
    }
    if (assumed) run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" has no material with thermal properties: steel assumed");
    props[region[i]] = th, rho[region[i]] = r * 1000;  // kg/m3
  }
  double heat_in = 0;
  for (const Load* l : heats) {
    // Spread over the bodies it names by their volumes.
    std::vector<size_t> on;
    for (const auto& r : l->refs)
      if (const size_t i = size_t(std::find(bodies.begin(), bodies.end(), r.body) - bodies.begin()); i < bodies.size()) on.push_back(i);
    const double P = l->def.value("value", 0.0);
    heat_in += P;
    for (size_t i : on) power[region[i]] += P / double(on.size());
  }
  put(cas / "constant" / "regionProperties", "dictionary", "regionProperties", [&] {
    std::string s = "regions ( fluid (" + fluid + ") solid (";
    for (const auto& r : solids) s += " " + r;
    return s + " ) );\n";
  }());
  put(cas / "constant" / "g", "uniformDimensionedVectorField", "g", "dimensions [0 1 -2 0 0 0 0];\nvalue (0 0 0);\n");
  put(cas / "constant" / fluid / "thermophysicalProperties", "dictionary", "thermophysicalProperties",
      "thermoType { type heRhoThermo; mixture pureMixture; transport const; thermo hConst; equationOfState perfectGas; specie specie; energy "
      "sensibleEnthalpy; }\nmixture { specie { molWeight 28.96; } thermodynamics { Cp " + num(a.cp) + "; Hf 0; } transport { mu " + num(a.mu) +
          "; Pr " + num(a.Pr) + "; } }\n");
  put(cas / "constant" / fluid / "turbulenceProperties", "dictionary", "turbulenceProperties", "simulationType laminar;\n");
  put(cas / "constant" / fluid / "radiationProperties", "dictionary", "radiationProperties", "radiation off; radiationModel none;\n");
  put(cas / "system" / fluid / "fvSchemes", "dictionary", "fvSchemes",
      "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; div(phi,U) bounded Gauss upwind; "
      "div(phi,K) bounded Gauss upwind; div(phi,h) bounded Gauss " + heat_scheme + "; div(((rho*nuEff)*dev2(T(grad(U))))) Gauss linear; } "
      "laplacianSchemes { default Gauss linear corrected; } interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
  put(cas / "system" / fluid / "fvSolution", "dictionary", "fvSolution",
      "solvers { \"rho.*\" { solver PCG; preconditioner DIC; tolerance 0; relTol 0; } p_rgh { solver GAMG; smoother GaussSeidel; tolerance 1e-7; "
      "relTol 0.01; } \"(U|h)\" { solver PBiCGStab; preconditioner DILU; tolerance 1e-9; relTol 0.1; } }\n"
      "SIMPLE { frozenFlow yes; momentumPredictor no; nNonOrthogonalCorrectors 0; pRefCell 0; pRefValue 100000; rhoMin 0.2; rhoMax 2; }\n"
      "relaxationFactors { fields { rho 1; p_rgh 0.7; } equations { U 0.3; h 0.9; } }\n");
  const std::string coupled = " { type compressible::turbulentTemperatureCoupledBaffleMixed; Tnbr T; kappaMethod %K; value uniform " + num(T0) + "; }";
  auto with = [&](std::string s, const char* k) { return s.replace(s.find("%K"), 2, k); };
  auto field = [&](const std::string& reg, const std::string& name, const std::string& cls, const std::string& dims, const std::string& internal,
                   const std::string& bf) {
    write_text_file(cas / "0" / reg / name, header(cls, name, "0/" + reg) + "dimensions " + dims + ";\ninternalField " + internal + ";\nboundaryField\n{\n" + bf + "}\n");
  };
  // The room's air comes in at the ambient temperature: a duct's inlet, or wherever it flows into an enclosure's box.
  const std::string T_in = "inletOutlet; inletValue uniform " + num(T0) + "; value uniform " + num(T0) + "; }\n";
  const std::string fans_cyclic = "  \"fan[0-9]+_(in|out)\" { type cyclic; }\n";
  field(fluid, "T", "volScalarField", "[0 0 0 1 0 0 0]", "uniform " + num(T0),
        "  \".*\" { type zeroGradient; }\n" +
            (enclosure.empty() ? "  inlet { type fixedValue; value uniform " + num(T0) + "; }\n  outlet { type " + T_in : "  open { type " + T_in) +
            "  \"" + fluid + "_to_.*\"" + with(coupled, "fluidThermo") + "\n" + fans_cyclic);
  field(fluid, "p", "volScalarField", "[1 -1 -2 0 0 0 0]", "uniform 100000", "  \".*\" { type calculated; value uniform 100000; }\n" + fans_cyclic);
  field(fluid, "p_rgh", "volScalarField", "[1 -1 -2 0 0 0 0]", "uniform 100000",
        "  \".*\" { type fixedFluxPressure; value uniform 100000; }\n" +
            std::string(enclosure.empty() ? "  outlet { type fixedValue; value uniform 100000; }\n  inlet { type fixedValue; value uniform 100000; }\n"
                                          : "  open { type fixedValue; value uniform 100000; }\n") +
            fans_cyclic);
  for (const auto& r : solids) {
    const Thermal& th = props[r];
    put(cas / "constant" / r / "thermophysicalProperties", "dictionary", "thermophysicalProperties",
        "thermoType { type heSolidThermo; mixture pureMixture; transport constIso; thermo hConst; equationOfState rhoConst; specie specie; energy "
        "sensibleEnthalpy; }\nmixture { specie { molWeight 50; } transport { kappa " + num(th.conductivity) + "; } thermodynamics { Hf 0; Cp " +
            num(th.specific_heat) + "; } equationOfState { rho " + num(rho[r]) + "; } }\n");
    put(cas / "constant" / r / "radiationProperties", "dictionary", "radiationProperties", "radiation off; radiationModel none;\n");
    put(cas / "system" / r / "fvSchemes", "dictionary", "fvSchemes",
        "ddtSchemes { default steadyState; } gradSchemes { default Gauss linear; } divSchemes { default none; } laplacianSchemes { default Gauss "
        "linear corrected; } interpolationSchemes { default linear; } snGradSchemes { default corrected; }\n");
    put(cas / "system" / r / "fvSolution", "dictionary", "fvSolution",
        "solvers { \"h.*\" { solver PCG; preconditioner DIC; tolerance 1e-10; relTol 0; } }\nSIMPLE { nNonOrthogonalCorrectors 0; }\n"
        "relaxationFactors { equations { h 1; } }\n");
    if (power[r] > 0)
      put(cas / "system" / r / "fvOptions", "dictionary", "fvOptions",
          "heat { type scalarSemiImplicitSource; active yes; selectionMode all; volumeMode absolute; injectionRateSuSp { h (" + num(power[r]) + " 0); } }\n");
    field(r, "T", "volScalarField", "[0 0 0 1 0 0 0]", "uniform " + num(T0), "  \".*\" { type zeroGradient; }\n  \"" + r + "_to_.*\"" + with(coupled, "solidThermo") + "\n");
    field(r, "p", "volScalarField", "[1 -1 -2 0 0 0 0]", "uniform 100000", "  \".*\" { type calculated; value uniform 100000; }\n");
  }
  // In chunks until the parts' temperatures stop moving (a hundredth of a degree over a chunk).
  const int chunk = 150, most = cfd.value("heat_iterations", 3000);
  double before = -1, change = 1e9;
  int done = 0;
  report(0.45, "Solving the heat (chtMultiRegionSimpleFoam)");
  while (done < most) {
    done += chunk;
    put(cas / "system" / "controlDict", "dictionary", "controlDict",
        "application chtMultiRegionSimpleFoam; startFrom latestTime; startTime 0; stopAt endTime; endTime " + std::to_string(done) +
            "; deltaT 1; writeControl timeStep; writeInterval " + std::to_string(chunk) +
            "; purgeWrite 1; writeFormat ascii; writePrecision 10; writeCompression off; timeFormat general; timePrecision 8; runTimeModifiable false;\n");
    const std::string log = foam_run("chtMultiRegionSimpleFoam", {}, cas, 0.45 + 0.5 * double(done) / most);
    // The hottest the parts got in the chunk's last iteration: each solid region's "Min/max T".
    double hot = -1e300;
    const size_t last_it = log.rfind("\nTime = ");
    for (size_t at = log.find("Solving for solid region", last_it == std::string::npos ? 0 : last_it); at != std::string::npos;
         at = log.find("Solving for solid region", at + 1))
      if (const size_t mm = log.find("Min/max T:", at); mm != std::string::npos) {
        std::istringstream s(log.substr(mm + 10, 80));
        double lo_t = 0, hi_t = 0;
        s >> lo_t >> hi_t;
        hot = std::max(hot, hi_t);
      }
    change = before < 0 ? 1e9 : std::fabs(hot - before);
    before = hot;
    if (change < 0.01) break;
  }
  if (change >= 0.01) run.warnings.push_back("the temperatures were still moving by " + num(change) + " degC over the last " + std::to_string(chunk) + " iterations");

  // ---- results: the parts' temperatures on their surfaces, the air's at the outlet
  report(0.97, "Reading the results");
  const std::string last = latest_time(cas);
  auto res = std::make_shared<FeaResult>();
  res->kind = "thermal";
  res->bodies = bodies;
  res->mesh_size = fine;
  json per_body = json::object();
  double hottest = -1e300;
  V hottest_at{0, 0, 0};
  size_t cells_total = 0;
  for (size_t i = 0; i < bodies.size(); ++i) {
    const std::string& r = region[i];
    if (std::find(solids.begin(), solids.end(), r) == solids.end()) continue;
    const Cells cells = read_cells(cas / "constant" / r / "polyMesh");
    cells_total += cells.centre.size();
    const auto T = read_internal(read_text_file(cas / last / r / "T"), cells.centre.size(), 1);
    std::vector<V> centres_mm;
    for (const auto& c : cells.centre) centres_mm.push_back(mul(c, 1e3));
    const Nearest near(centres_mm, 2 * fine);
    // The surface, one node per triangle corner.
    const int base = int(res->nodes.size());
    for (size_t k = 0; k < tri_pts[i].size(); ++k) {
      res->nodes.push_back(tri_pts[i][k]);
      res->temperature.push_back(T[near(tri_pts[i][k])] - 273.15);
    }
    for (size_t k = 0; k + 2 < tri_pts[i].size(); k += 3) {
      res->skin.push_back({base + int(k), base + int(k + 1), base + int(k + 2)});
      res->skin_body.push_back(int(i));
    }
    double peak = -1e300, mean = 0, vol = 0;
    size_t at = 0;
    for (size_t c = 0; c < T.size() && c < cells.volume.size(); ++c) {
      if (T[c] > peak) peak = T[c], at = c;
      mean += T[c] * cells.volume[c], vol += cells.volume[c];
    }
    per_body[scene.node(bodies[i])->name] = {{"max_temperature_C", peak - 273.15}, {"mean_temperature_C", vol > 0 ? mean / vol - 273.15 : 0.0},
                                             {"at", centres_mm[at]}, {"conductivity_W_mK", props[r].conductivity}, {"cells", T.size()}};
    if (peak - 273.15 > hottest) hottest = peak - 273.15, hottest_at = centres_mm[at];
  }
  res->displacement.assign(res->nodes.size(), V{0, 0, 0});
  // The air leaving: mass-weighted temperature where it leaves (a duct's outlet, an enclosure's box wherever air flows out),
  // and the heat it carries off.
  {
    const std::string phi = read_text_file(cas / last / fluid / "phi"), T = read_text_file(cas / last / fluid / "T");
    const std::string leave = enclosure.empty() ? "outlet" : "open";
    const auto m = read_patch(phi, leave), t = read_patch(T, leave);
    double mdot = 0, mt = 0, carried = 0;
    for (size_t k = 0; k < m.size() && k < t.size(); ++k) {
      if (m[k] > 0) mdot += m[k], mt += m[k] * t[k];
      carried += m[k] * a.cp * (t[k] - T0);
    }
    const double out_T = mdot > 0 ? mt / mdot - 273.15 : ambient;
    json& to = enclosure.empty() ? fan_json : fan_json["vents"];
    to["outlet_air_C"] = out_T;
    to["air_rise_C"] = out_T - ambient;
    to["heat_to_air_W"] = carried;
    run.summary["to_air_W"] = carried;
    const Cells air_cells = read_cells(cas / "constant" / fluid / "polyMesh");
    cells_total += air_cells.centre.size();
    run.summary["air_cells"] = air_cells.centre.size();

    trace(*res, air_cells, read_internal(read_text_file(cas / "0" / fluid / "U"), air_cells.centre.size(), 3),
          read_internal(read_text_file(cas / last / fluid / "T"), air_cells.centre.size(), 1));
    run.summary["streamlines"] = res->streamlines.size();
  }
  json summary = run.summary;
  summary["air"] = "cfd";
  summary["engine"] = "OpenFOAM (snappyHexMesh, simpleFoam, chtMultiRegionSimpleFoam)";
  summary["case"] = load_case;
  summary["ambient_C"] = ambient;
  summary["max_temperature_C"] = hottest;
  summary["max_temperature_at"] = hottest_at;
  summary["bodies"] = per_body;
  summary["heat_W"] = heat_in;
  summary["cell_size_mm"] = fine;
  summary["cells"] = cells_total;
  summary["heat_iterations"] = done;
  if (!fins_json.is_null()) fan_json["fins"] = fins_json;
  if (!enclosure.empty()) {
    summary["enclosure"] = scene.node(enclosure)->name;
    summary["fans"] = fan_json["fans"];
    summary["vents"] = fan_json["vents"];
    summary["flow_iterations"] = fan_json["iterations"];
    summary["room_mm"] = {{"size", {d1 - d0, a1 - a0, b1 - b0}}, {"margin", margin}};
  } else if (fan_load) {
    if (const auto it = per_body.find(scene.node(fan_load->refs.front().body)->name); it != per_body.end())
      fan_json["thermal_resistance_C_W"] = (it->at("max_temperature_C").get<double>() - ambient) / std::max(1e-9, heat_in);
    summary["fans"] = json::array({fan_json});
  } else {
    summary["stream"] = fan_json;
  }
  if (enclosure.empty()) summary["duct_mm"] = {{"length", d1 - d0}, {"across", {a1 - a0, b1 - b0}}, {"upstream", up}, {"downstream", down}};
  summary["warnings"] = run.warnings;
  run.summary = summary;
  run.t = {0.0};
  run.fea = res;
  return run;
}

}  // namespace opad::sim
