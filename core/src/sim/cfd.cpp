#include "opad/sim/cfd.hpp"

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <algorithm>
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

StudyRun run_cfd(const Document& doc, const Scene& scene, const json& st, const Progress& progress) {
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
  const json cfd = st.value("cfd", json::object());

  // ---- the case's thermal loads: heat sources in bodies, one fan or one forced stream
  std::string load_case = st.value("case", std::string());
  if (load_case.empty())
    for (const auto& l : scene.loads)
      if (l.kind == "heat") {
        load_case = l.load_case;
        break;
      }
  std::vector<const Load*> heats;
  const Load* fan_load = nullptr;
  const Load* stream = nullptr;
  for (const auto& l : scene.loads) {
    if (l.load_case != load_case) continue;
    if (!l.error.empty()) throw Error("load \"" + l.name + "\": " + l.error);
    if (l.kind == "heat") heats.push_back(&l);
    else if (l.kind == "fan") {
      if (fan_load) throw Error("the CFD air takes one fan (a fan's count puts several side by side)");
      fan_load = &l;
    } else if (l.kind == "convection" && l.def.value("h", json()) == "forced") {
      stream = &l;
    } else if (l.kind == "convection" || l.kind == "radiation" || l.kind == "temperature") {
      run.warnings.push_back("\"" + l.name + "\" is left out: with the CFD air, the parts lose heat only to the air the fan or the stream moves");
    }
  }
  if (heats.empty()) throw Error("load case \"" + load_case + "\" has no heat source: add one (load kind heat, W, on a body)");
  if (!fan_load && !stream)
    throw Error("the CFD air needs a fan, or a convection with h forced (its velocity and way): still air stays with the correlations");
  std::vector<std::string> bodies;
  auto add_body = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (!n) throw Error("no body " + id);
    for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id))
      if (std::find(bodies.begin(), bodies.end(), b) == bodies.end()) bodies.push_back(b);
  };
  for (const auto& b : st.value("bodies", json::array())) add_body(b.get<std::string>());
  if (bodies.empty()) {
    for (const Load* l : heats)
      for (const auto& r : l->refs) add_body(r.body);
    for (const Load* l : {fan_load, stream})
      if (l)
        for (const auto& r : l->refs) add_body(r.body);
  }
  for (const Load* l : heats)
    for (const auto& r : l->refs)
      if (r.kind != Ref::Kind::Body) throw Error("heat \"" + l->name + "\": the CFD air takes heat sources in whole bodies, not on faces");
  const V way = unit((fan_load ? fan_load : stream)->def.value("vector", V{1, 0, 0}));
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
  double x0, y0, z0, x1, y1, z1;
  all.Get(x0, y0, z0, x1, y1, z1);
  // A frame along the air: the world axes least along it.
  V axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  std::sort(std::begin(axes), std::end(axes), [&](const V& a, const V& b) { return std::fabs(dot(a, way)) < std::fabs(dot(b, way)); });
  const V e1 = unit(sub(axes[0], mul(way, dot(axes[0], way)))), e2 = unit(cross(way, e1));
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  for (double x : {x0, x1})
    for (double y : {y0, y1})
      for (double z : {z0, z1}) {
        const V p{x, y, z};
        const double c[3] = {dot(p, way), dot(p, e1), dot(p, e2)};
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
      }
  // The finest cell: two thirds of a fin's thickness and a sixth of the gap between fins (the gap's boundary layers want
  // six cells across: on six 3 mm fins 8.4 mm apart, cells of 2, 1.5 and 1 mm gave the parts' rise as 129, 106 and 89 %
  // of the correlations' and the flow as 135, 120 and 103 %), else a 40th of the parts' size.
  double fine = cfd.value("cell_size", 0.0);
  json fins_json;
  if (fine <= 0) {
    const double size = std::sqrt(std::pow(hi[0] - lo[0], 2) + std::pow(hi[1] - lo[1], 2) + std::pow(hi[2] - lo[2], 2));
    fine = std::max(0.25, size / 40);
    if (fan_load && !fan_load->refs.empty()) {
      const size_t i = size_t(std::find(bodies.begin(), bodies.end(), fan_load->refs.front().body) - bodies.begin());
      if (i < world.size())
        if (const auto f = air::fin_array(world[i], way)) {
          fine = std::max(0.2, std::min(f->t / 1.5, f->gap / 6) * 1e3);
          fins_json = f->to_json();
        }
    }
  }
  const double coarse = 2 * fine;
  const double len = hi[0] - lo[0];
  const double up = std::max(4 * coarse, cfd.value("upstream", 0.25 * len)), down = std::max(4 * coarse, cfd.value("downstream", 0.5 * len));
  const double pad = cfd.value("padding", fine);
  // The duct, off the parts' faces by a fraction of a cell so that no face of theirs lies on a grid plane.
  const double off = 0.37 * coarse;
  const double d0 = lo[0] - up - off, d1 = hi[0] + down + off;
  const double a0 = lo[1] - pad - off, a1 = hi[1] + pad + off, b0 = lo[2] - pad - off, b1 = hi[2] + pad + off;
  const int nx = std::max(4, int(std::ceil((d1 - d0) / coarse))), ny = std::max(2, int(std::ceil((a1 - a0) / coarse))),
            nz = std::max(2, int(std::ceil((b1 - b0) / coarse)));
  if (double(nx) * ny * nz > cfd.value("max_cells", 3e6)) throw Error("cfd: the duct would take too many cells at this cell size: set cfd.cell_size larger");
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
      setenv("WM_PROJECT_DIR", path_to_utf8(foam.project).c_str(), 1);
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
    region.push_back("s" + std::to_string(i));
    BRepMesh_IncrementalMesh(world[i], std::max(0.02, fine / 4), false, 0.3, true);
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

  // ---- mesh: the duct, the parts cut out of it as cell zones, then one region each
  report(0.03, "Writing the OpenFOAM case");
  {
    std::ostringstream bm;
    bm << "convertToMeters 0.001;\nvertices (\n";
    for (const V& v : {corner(d0, a0, b0), corner(d1, a0, b0), corner(d1, a1, b0), corner(d0, a1, b0), corner(d0, a0, b1), corner(d1, a0, b1),
                       corner(d1, a1, b1), corner(d0, a1, b1)})
      bm << "  " << vec(v) << "\n";
    bm << ");\nblocks ( hex (0 1 2 3 4 5 6 7) (" << nx << " " << ny << " " << nz << ") simpleGrading (1 1 1) );\nboundary (\n"
       << "  inlet { type patch; faces ((0 4 7 3)); }\n  outlet { type patch; faces ((1 2 6 5)); }\n"
       << "  walls { type wall; faces ((0 1 5 4) (3 7 6 2) (0 3 2 1) (4 5 6 7)); }\n);\n";
    put(cas / "system" / "blockMeshDict", "dictionary", "blockMeshDict", bm.str());
    std::ostringstream sh;
    sh << "castellatedMesh true; snap true; addLayers false;\ngeometry {\n";
    for (const auto& r : region) sh << "  " << r << " { type triSurfaceMesh; file \"" << r << ".stl\"; }\n";
    sh << "}\ncastellatedMeshControls {\n  maxLocalCells 6000000; maxGlobalCells 12000000; minRefinementCells 0; maxLoadUnbalance 0.1; nCellsBetweenLevels 2;\n"
       << "  features ();\n  refinementSurfaces {\n";
    for (const auto& r : region) sh << "    " << r << " { level (1 1); faceZone " << r << "; cellZone " << r << "; cellZoneInside inside; }\n";
    const V inside_air = mul(corner(d0 + 0.5 * (up + off), 0.5 * (a0 + a1), 0.5 * (b0 + b1)), 1e-3);
    sh << "  }\n  resolveFeatureAngle 30;\n  refinementRegions {}\n  locationInMesh " << vec(inside_air) << ";\n  allowFreeStandingZoneFaces false;\n}\n"
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
  foam_run("blockMesh", {}, cas, 0.05);
  report(0.08, "Meshing the air around the parts (snappyHexMesh)");
  foam_run("snappyHexMesh", {"-overwrite"}, cas, 0.08);
  foam_run("splitMeshRegions", {"-cellZones", "-overwrite"}, cas, 0.3);
  // The air: the cells in no part's zone, which splitMeshRegions names domain0.
  const std::string fluid = "domain0";
  if (!fs::is_directory(cas / "constant" / fluid / "polyMesh")) throw Error("cfd: the mesh has no air around the parts");
  std::vector<std::string> solids;
  for (const auto& r : region)
    if (fs::is_directory(cas / "constant" / r / "polyMesh")) solids.push_back(r);
    else run.warnings.push_back("a part was too thin for the cell size and is left out of the air: set cfd.cell_size smaller");

  // ---- the flow alone (simpleFoam, laminar), on the air's mesh
  const air::Air a = air::properties(ambient);
  json fan_json;
  {
    fs::create_directories(flow / "constant");
    fs::copy(cas / "constant" / fluid / "polyMesh", flow / "constant" / "polyMesh", fs::copy_options::recursive);
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
    std::string u_in, p_in;
    if (fan_load) {
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
      u_in = "inlet { type pressureInletOutletVelocity; value uniform (0 0 0); }";
      p_in = "inlet { type fanPressure; file \"$FOAM_CASE/constant/fanCurve\"; outOfBounds clamp; direction in; p0 uniform 0; value uniform 0; }";
      fan_json = {{"name", fan_load->name}, {"fan", fan.to_json()}, {"fans", count}};
    } else {
      const double U = stream->def.value("velocity", 1.0);
      u_in = "inlet { type fixedValue; value uniform " + vec(mul(way, U)) + "; }";
      p_in = "inlet { type zeroGradient; }";
    }
    put(flow / "0" / "U", "volVectorField", "U",
        "dimensions [0 1 -1 0 0 0 0];\ninternalField uniform (0 0 0);\nboundaryField\n{\n  \".*\" { type noSlip; }\n  " + u_in +
            "\n  outlet { type inletOutlet; inletValue uniform (0 0 0); value uniform (0 0 0); }\n}\n");
    put(flow / "0" / "p", "volScalarField", "p",
        "dimensions [0 2 -2 0 0 0 0];\ninternalField uniform 0;\nboundaryField\n{\n  \".*\" { type zeroGradient; }\n  " + p_in +
            "\n  outlet { type fixedValue; value uniform 0; }\n}\n");
    report(0.32, "Solving the air's flow (simpleFoam)");
    const std::string log = foam_run("simpleFoam", {}, flow, 0.32);
    const std::string last = latest_time(flow);
    if (last.empty() || last == "0") throw Error("cfd: simpleFoam wrote no flow");
    const bool converged = log.find("SIMPLE solution converged") != std::string::npos;
    if (!converged) run.warnings.push_back("the air's flow had not settled after " + std::to_string(iters) + " iterations (cfd.flow_iterations)");
    const std::string phi = read_text_file(flow / last / "phi"), p = read_text_file(flow / last / "p");
    double Q = 0;
    for (double v : read_patch(phi, "inlet")) Q -= v;
    const auto pin = read_patch(p, "inlet");
    double pmean = 0;
    for (double v : pin) pmean += v;
    pmean = pin.empty() ? 0 : pmean / double(pin.size());
    json flow_json = {{"flow_m3h", Q * 3600}, {"flow_cfm", Q / 4.719474e-4}, {"inlet_static_Pa", pmean * a.rho}, {"pressure_Pa", pmean * a.rho}, {"iterations", std::stoi(last)},
                      {"converged", converged}, {"inlet_velocity_m_s", Q / ((a1 - a0) * (b1 - b0) * 1e-6)}};
    if (!fan_json.is_null())
      for (const auto& [k, v] : flow_json.items()) fan_json[k] = v;
    else
      fan_json = flow_json;
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
      "div(phi,K) bounded Gauss upwind; div(phi,h) bounded Gauss upwind; div(((rho*nuEff)*dev2(T(grad(U))))) Gauss linear; } "
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
  field(fluid, "T", "volScalarField", "[0 0 0 1 0 0 0]", "uniform " + num(T0),
        "  \".*\" { type zeroGradient; }\n  inlet { type fixedValue; value uniform " + num(T0) + "; }\n  outlet { type inletOutlet; inletValue uniform " +
            num(T0) + "; value uniform " + num(T0) + "; }\n  \"" + fluid + "_to_.*\"" + with(coupled, "fluidThermo") + "\n");
  field(fluid, "p", "volScalarField", "[1 -1 -2 0 0 0 0]", "uniform 100000", "  \".*\" { type calculated; value uniform 100000; }\n");
  field(fluid, "p_rgh", "volScalarField", "[1 -1 -2 0 0 0 0]", "uniform 100000", "  \".*\" { type fixedFluxPressure; value uniform 100000; }\n  outlet { type fixedValue; value uniform 100000; }\n  inlet { type fixedValue; value uniform 100000; }\n");
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
  // The air leaving: mass-weighted temperature at the outlet, and the heat it carries off.
  {
    const std::string phi = read_text_file(cas / last / fluid / "phi"), T = read_text_file(cas / last / fluid / "T");
    const auto m = read_patch(phi, "outlet"), t = read_patch(T, "outlet");
    double mdot = 0, mt = 0;
    for (size_t k = 0; k < m.size() && k < t.size(); ++k) mdot += m[k], mt += m[k] * t[k];
    const double out_T = mdot > 0 ? mt / mdot - 273.15 : ambient;
    fan_json["outlet_air_C"] = out_T;
    fan_json["air_rise_C"] = out_T - ambient;
    fan_json["heat_to_air_W"] = mdot * a.cp * (out_T - ambient);
    run.summary["to_air_W"] = mdot * a.cp * (out_T - ambient);
    const Cells air_cells = read_cells(cas / "constant" / fluid / "polyMesh");
    cells_total += air_cells.centre.size();
    run.summary["air_cells"] = air_cells.centre.size();

    // Streamlines: from points across the inlet in front of the parts, along the air's velocity (the nearest cell's, in
    // steps of half the finest cell, the midpoint rule) until the outlet, the duct's side, still air or a part.
    const auto Uc = read_internal(read_text_file(cas / "0" / fluid / "U"), air_cells.centre.size(), 3);
    const auto Tc = read_internal(read_text_file(cas / last / fluid / "T"), air_cells.centre.size(), 1);
    std::vector<V> at_mm;
    for (const auto& c : air_cells.centre) at_mm.push_back(mul(c, 1e3));
    const Nearest near(at_mm, 2 * fine);
    double mean_speed = 0;
    for (size_t c = 0; c < at_mm.size(); ++c) mean_speed += norm({Uc[3 * c], Uc[3 * c + 1], Uc[3 * c + 2]});
    mean_speed /= std::max<size_t>(1, at_mm.size());
    auto velocity = [&](const V& p, size_t& cell) {
      cell = near(p);
      if (norm(sub(at_mm[cell], p)) > 1.8 * fine) return V{0, 0, 0};  // inside a part
      return V{Uc[3 * cell], Uc[3 * cell + 1], Uc[3 * cell + 2]};
    };
    const double h = 0.5 * fine;
    const int across = cfd.value("streamlines", 10);
    const double wa = hi[1] - lo[1], wb = hi[2] - lo[2];
    const int na = std::max(1, int(std::lround(across * std::sqrt(wa / std::max(1e-9, wb))))), nb = std::max(1, int(std::lround(across * std::sqrt(wb / std::max(1e-9, wa)))));
    for (int i = 0; i < na; ++i)
      for (int j = 0; j < nb; ++j) {
        V p = corner(d0 + 2 * fine, lo[1] + wa * (i + 0.5) / na, lo[2] + wb * (j + 0.5) / nb);
        std::vector<Vec3> line;
        std::vector<double> speed, temp;
        size_t cell = 0;
        for (int step = 0; step < int(8 * (d1 - d0) / h); ++step) {
          const V u = velocity(p, cell);
          const double sp = norm(u);
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
          if (d > d1 - 0.5 * fine || d < d0 || ea < a0 || ea > a1 || eb < b0 || eb > b1) break;
        }
        if (line.size() < 3) continue;
        res->streamlines.push_back(std::move(line));
        res->streamline_speed.push_back(std::move(speed));
        res->streamline_temperature.push_back(std::move(temp));
      }
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
  if (fan_load) {
    if (const auto it = per_body.find(scene.node(fan_load->refs.front().body)->name); it != per_body.end())
      fan_json["thermal_resistance_C_W"] = (it->at("max_temperature_C").get<double>() - ambient) / std::max(1e-9, heat_in);
    summary["fans"] = json::array({fan_json});
  } else {
    summary["stream"] = fan_json;
  }
  summary["duct_mm"] = {{"length", d1 - d0}, {"across", {a1 - a0, b1 - b0}}, {"upstream", up}, {"downstream", down}};
  summary["warnings"] = run.warnings;
  run.summary = summary;
  run.t = {0.0};
  run.fea = res;
  return run;
}

}  // namespace opad::sim
