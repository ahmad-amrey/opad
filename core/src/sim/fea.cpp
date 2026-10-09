#include "opad/sim/fea.hpp"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepAlgoAPI_BuilderAlgo.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <IntCurvesFace_ShapeIntersector.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <sstream>
#include <thread>

#include "../design/engine.hpp"
#include "../import_common.hpp"
#include "fea_mesh.hpp"
#include "radiation.hpp"
#include "opad/geometry.hpp"
#include "opad/materials.hpp"
#include "opad/sim/joints.hpp"
#include "opad/sim/airflow.hpp"
#include "opad/sim/cfd.hpp"
#include "opad/sim/printing.hpp"

namespace opad::sim {

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
  return n > 0 ? mul(a, 1 / n) : a;
}

std::filesystem::path exe_dir() {
#ifdef _WIN32
  return {};
#else
  std::error_code e;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", e);
  return e ? std::filesystem::path() : self.parent_path();
#endif
}

double von_mises(const std::array<double, 6>& s) {
  const double a = s[0] - s[1], b = s[1] - s[2], c = s[2] - s[0];
  return std::sqrt(0.5 * (a * a + b * b + c * c) + 3 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5]));
}

struct Mat {
  double E = 210000, nu = 0.3, rho = 7.85, yield = 250;  // MPa, -, g/cm3, MPa
  double k = 50, cp = 490, emissivity = 0.3;              // W/m.K, J/kg.K: steel's
  // A board: k along it, k_through across it (0: the same every way), across along normal (zero: its thinnest way).
  double k_through = 0;
  Vec3 normal{0, 0, 0};
  bool thermal_assumed = true;
  std::string name = "Steel";
  bool assumed = true;
};

Mat material_for(const Document& doc, const Scene& scene, const std::string& body, const json& overrides) {
  Mat m;
  const MaterialChoice c = material_of(doc, scene, body);
  if (!c.id.empty())
    if (const Mechanical* mech = mechanical(c.id)) {
      m.E = mech->youngs, m.nu = mech->poisson, m.yield = mech->yield, m.assumed = false;
      m.name = c.shown();
    }
  if (!c.id.empty())
    if (const Thermal* th = thermal(c.id)) {
      m.k = th->conductivity, m.cp = th->specific_heat, m.emissivity = th->emissivity, m.thermal_assumed = false;
      m.name = c.shown();
    }
  if (c.density > 0) m.rho = c.density;
  if (overrides.is_object()) {
    const json o = overrides.contains(body) ? overrides[body] : overrides.value("all", json());
    // "material": a library material for this study (a heatsink of several bodies made one metal), before the numbers given.
    if (const std::string id = o.is_object() ? o.value("material", std::string()) : std::string(); !id.empty()) {
      const Material* lib = material(id);
      if (!lib) throw Error("study materials: no library material \"" + id + "\"");
      if (const Mechanical* mech = mechanical(id)) m.E = mech->youngs, m.nu = mech->poisson, m.yield = mech->yield;
      if (const Thermal* th = thermal(id)) m.k = th->conductivity, m.cp = th->specific_heat, m.emissivity = th->emissivity, m.thermal_assumed = false;
      m.rho = lib->density, m.name = lib->name;
    }
    if (o.is_object()) {
      m.E = o.value("E", m.E), m.nu = o.value("nu", m.nu), m.rho = o.value("density", m.rho), m.yield = o.value("yield", m.yield);
      m.k = o.value("k", m.k), m.cp = o.value("cp", m.cp), m.emissivity = o.value("emissivity", m.emissivity);
      m.k_through = o.value("k_through", m.k_through);
      if (o.contains("normal")) m.normal = o["normal"].get<Vec3>();
      // A printed circuit board from its make-up: copper layers (35 um an ounce, covering part of each layer) in FR-4,
      // the copper in parallel along it and in series across it.
      if (o.contains("pcb")) {
        const json b = o["pcb"].is_object() ? o["pcb"] : json::object();
        const double t = b.value("thickness", 1.6), layers = b.value("layers", 4), oz = b.value("copper_oz", 1.0), cover = b.value("coverage", 0.7);
        const double cu = std::min(0.9 * t, layers * 0.035 * oz * cover), k_cu = 390, k_fr4 = 0.3;
        m.k = (cu * k_cu + (t - cu) * k_fr4) / t;
        m.k_through = t / (cu / k_cu + (t - cu) / k_fr4);
        m.cp = o.value("cp", 1100.0), m.rho = o.value("density", 1.9);
        if (!o.contains("name")) m.name = "PCB, " + std::to_string(int(layers)) + " layers";
      }
      m.assumed = false;
      m.thermal_assumed = m.thermal_assumed && !o.contains("k") && !o.contains("pcb");
      if (o.contains("name")) m.name = o["name"].get<std::string>();
    }
  }
  return m;
}

// The faces a shape's face became through a history of operations (itself when untouched, none when gone).
template <class H>
std::vector<TopoDS_Shape> after(H& history, const TopoDS_Shape& f) {
  std::vector<TopoDS_Shape> out;
  const TopTools_ListOfShape& m = history.Modified(f);
  if (!m.IsEmpty()) {
    for (const auto& x : m) out.push_back(x);
  } else if (!history.IsDeleted(f)) {
    out.push_back(f);
  }
  return out;
}

// A bolt: the body, its axis and the plane across the middle of its shank.
struct Bolt {
  std::string body;
  gp_Ax1 axis;
  gp_Pnt at;
  double preload = 0;
  std::string load;
};

// The axis of a bolt body: its largest cylindrical face's, cut at the middle of that face.
Bolt find_bolt(const TopoDS_Shape& shape, const std::string& body) {
  Bolt b;
  b.body = body;
  double best = 0;
  for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
    BRepAdaptor_Surface s(TopoDS::Face(e.Current()));
    if (s.GetType() != GeomAbs_Cylinder) continue;
    GProp_GProps g;
    BRepGProp::SurfaceProperties(e.Current(), g);
    if (g.Mass() <= best) continue;
    best = g.Mass();
    b.axis = s.Cylinder().Axis();
    // The middle of the face along the axis: its centre projected.
    const gp_Pnt c = g.CentreOfMass();
    const gp_Vec v(b.axis.Location(), c);
    b.at = b.axis.Location().Translated(gp_Vec(b.axis.Direction()) * v.Dot(gp_Vec(b.axis.Direction())));
  }
  if (best == 0) throw Error("a bolt preload needs a bolt with a cylindrical shank (or give its axis)");
  return b;
}

// ---------------------------------------------------------------- .frd
struct Frd {
  struct Block {
    std::string name;  // DISP, STRESS
    int step = 0;
    bool modal = false;
    double value = 0;  // the step's time or frequency
    std::map<int, std::vector<double>> values;
  };
  std::vector<Block> blocks;
};

Frd read_frd(const std::filesystem::path& p) {
  Frd out;
  std::ifstream in(p);
  if (!in) throw Error("CalculiX wrote no results (" + p.filename().string() + ")");
  std::string line;
  Frd::Block cur;
  bool in_block = false;
  double step_value = 0;
  bool step_modal = false;
  int step = 0;
  while (std::getline(in, line)) {
    if (line.size() >= 6 && line.compare(0, 6, "  100C") == 0) {
      // "  100CL  101 1.000000000          10                     0    1           1"
      step_value = std::strtod(line.substr(12, 13).c_str(), nullptr);
      step_modal = line.find("MODAL") != std::string::npos;
      ++step;
      continue;
    }
    if (line.size() >= 3 && line.compare(0, 3, " -4") == 0) {
      cur = Frd::Block();
      std::istringstream ss(line.substr(3));
      ss >> cur.name;
      cur.step = step;
      cur.modal = step_modal;
      cur.value = step_value;
      in_block = true;
      continue;
    }
    if (!in_block) continue;
    if (line.size() >= 3 && line.compare(0, 3, " -3") == 0) {
      out.blocks.push_back(std::move(cur));
      in_block = false;
      continue;
    }
    if (line.size() >= 13 && line.compare(0, 3, " -1") == 0) {
      const int node = std::atoi(line.substr(3, 10).c_str());
      std::vector<double> v;
      for (size_t at = 13; at + 12 <= line.size(); at += 12) v.push_back(std::strtod(line.substr(at, 12).c_str(), nullptr));
      cur.values[node] = std::move(v);
    }
  }
  return out;
}

std::vector<double> read_frequencies(const std::filesystem::path& dat) {
  std::ifstream in(dat);
  std::vector<double> out;
  std::string line;
  bool table = false;
  while (std::getline(in, line)) {
    if (line.find("E I G E N V A L U E   O U T P U T") != std::string::npos) {
      table = true;
      continue;
    }
    if (!table) continue;
    if (line.find("P A R T I C I P A T I O N") != std::string::npos) break;
    std::istringstream ss(line);
    int mode;
    double eig, rad, cyc;
    if (ss >> mode >> eig >> rad >> cyc) out.push_back(cyc);
  }
  return out;
}

// "total force (fx,fy,fz) for set FIX1 and time ..." then a line with the three numbers.
std::map<std::string, V> read_reactions(const std::filesystem::path& dat) {
  std::ifstream in(dat);
  std::map<std::string, V> out;
  std::string line, set;
  while (std::getline(in, line)) {
    const auto at = line.find("total force (fx,fy,fz) for set ");
    if (at != std::string::npos) {
      std::istringstream ss(line.substr(at + 31));
      ss >> set;
      continue;
    }
    if (!set.empty()) {
      std::istringstream ss(line);
      V v;
      if (ss >> v[0] >> v[1] >> v[2]) {
        out[set] = v;
        set.clear();
      }
    }
  }
  return out;
}

// Stresses at each element's integration points (*EL PRINT, S): element number -> xx yy zz xy yz zx per point, in the
// element's *ORIENTATION axes when it has one. The .dat lists sxx syy szz sxy sxz syz.
std::unordered_map<int, std::vector<std::array<double, 6>>> read_point_stresses(const std::filesystem::path& dat) {
  std::ifstream in(dat);
  std::unordered_map<int, std::vector<std::array<double, 6>>> out;
  std::string line;
  bool in_block = false;
  while (std::getline(in, line)) {
    if (line.find("stresses (elem, integ.pnt.") != std::string::npos) {
      in_block = true;
      continue;
    }
    if (!in_block) continue;
    std::istringstream ss(line);
    int e = 0, ip = 0;
    double v[6];
    if (ss >> e >> ip >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5]) {
      out[e].push_back({v[0], v[1], v[2], v[3], v[5], v[4]});
    } else if (line.find_first_not_of(" \t\r") != std::string::npos) {
      in_block = false;  // the next block's heading
    }
  }
  return out;
}

// A printed body's outer surface as triangles, to place each element in its skin: the distance straight up and straight
// down (along the build direction) to the surface, and the nearest side face (its normal sets the walls' road
// direction). Two hashed grids keep both to a few triangles per query.
class PrintSurface {
 public:
  struct T {
    V a, b, c, n;  // corners, outward unit normal
  };
  PrintSurface(std::vector<T> tris, const V& up, const V& ex, const V& ey, double cell) : m_tris(std::move(tris)), m_up(up), m_ex(ex), m_ey(ey), m_cell(cell) {
    for (size_t t = 0; t < m_tris.size(); ++t) {
      const T& x = m_tris[t];
      double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
      for (const V* p : {&x.a, &x.b, &x.c}) {
        const double q[3] = {dot(*p, m_ex), dot(*p, m_ey), dot(*p, m_up)};
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
      }
      for (long i = cellOf(lo[0]); i <= cellOf(hi[0]); ++i)
        for (long j = cellOf(lo[1]); j <= cellOf(hi[1]); ++j) {
          m_columns[key(i, j, 0)].push_back(int(t));
          if (std::fabs(dot(x.n, m_up)) < 0.9)
            for (long k = cellOf(lo[2]); k <= cellOf(hi[2]); ++k) m_sides[key(i, j, k)].push_back(int(t));
        }
    }
  }
  // Distance from p along +up (dir 1) or -up (dir -1) to the first surface triangle; 1e300 when none.
  double ray(const V& p, double dir) const {
    const auto it = m_columns.find(key(cellOf(dot(p, m_ex)), cellOf(dot(p, m_ey)), 0));
    if (it == m_columns.end()) return 1e300;
    const V d = mul(m_up, dir);
    double best = 1e300;
    for (int t : it->second) {  // Moller-Trumbore
      const T& x = m_tris[size_t(t)];
      const V e1 = sub(x.b, x.a), e2 = sub(x.c, x.a), h = cross(d, e2);
      const double det = dot(e1, h);
      if (std::fabs(det) < 1e-14) continue;
      const V s = sub(p, x.a);
      const double u = dot(s, h) / det;
      if (u < -1e-9 || u > 1 + 1e-9) continue;
      const V q = cross(s, e1);
      const double v = dot(d, q) / det;
      if (v < -1e-9 || u + v > 1 + 1e-9) continue;
      const double tt = dot(e2, q) / det;
      if (tt > 1e-9) best = std::min(best, tt);
    }
    return best;
  }
  // The nearest side triangle (not facing up or down): null when none within reach.
  const T* nearestSide(const V& p) const {
    const long ci = cellOf(dot(p, m_ex)), cj = cellOf(dot(p, m_ey)), ck = cellOf(dot(p, m_up));
    const T* best = nullptr;
    double bestD = 1e300;
    for (long r = 0; r < 64; ++r) {
      for (long i = ci - r; i <= ci + r; ++i)
        for (long j = cj - r; j <= cj + r; ++j)
          for (long k = ck - r; k <= ck + r; ++k) {
            if (std::max({std::labs(i - ci), std::labs(j - cj), std::labs(k - ck)}) != r) continue;  // the shell at r only
            const auto it = m_sides.find(key(i, j, k));
            if (it == m_sides.end()) continue;
            for (int t : it->second) {
              const double dd = distance(p, m_tris[size_t(t)]);
              if (dd < bestD) bestD = dd, best = &m_tris[size_t(t)];
            }
          }
      if (best && bestD <= r * m_cell) break;
    }
    return best;
  }

 private:
  long cellOf(double x) const { return long(std::floor(x / m_cell)); }
  static long long key(long i, long j, long k) { return ((long long)(i + (1 << 20)) << 42) | ((long long)(j + (1 << 20)) << 21) | (long long)(k + (1 << 20)); }
  // Distance from p to triangle x (Ericson's closest point).
  static double distance(const V& p, const T& x) {
    const V ab = sub(x.b, x.a), ac = sub(x.c, x.a), ap = sub(p, x.a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return norm(ap);
    const V bp = sub(p, x.b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return norm(bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return norm(sub(p, add(x.a, mul(ab, d1 / (d1 - d3)))));
    const V cp = sub(p, x.c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return norm(cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return norm(sub(p, add(x.a, mul(ac, d2 / (d2 - d6)))));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
      return norm(sub(p, add(x.b, mul(sub(x.c, x.b), (d4 - d3) / ((d4 - d3) + (d5 - d6))))));
    const double den = 1 / (va + vb + vc);
    return norm(sub(p, add(x.a, add(mul(ab, vb * den), mul(ac, vc * den)))));
  }
  std::vector<T> m_tris;
  V m_up, m_ex, m_ey;
  double m_cell;
  std::unordered_map<long long, std::vector<int>> m_columns, m_sides;
};

}  // namespace

std::filesystem::path ccx_program() {
  if (const char* e = std::getenv("OPAD_CCX"); e && *e) return path_from_utf8(e);
  std::error_code err;
  for (const char* name : {"ccx", "ccx_2.22", "ccx_2.21", "ccx_2.20", "ccx_2.19"}) {
#ifdef _WIN32
    const auto beside = exe_dir() / (std::string(name) + ".exe");
#else
    const auto beside = exe_dir() / name;
#endif
    if (!exe_dir().empty() && std::filesystem::exists(beside, err)) return beside;
  }
  const char* path = std::getenv("PATH");
  if (!path) return {};
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  std::string p = path;
  size_t start = 0;
  while (start <= p.size()) {
    const size_t end = std::min(p.find(sep, start), p.size());
    const std::filesystem::path dir = p.substr(start, end - start);
    for (const char* name : {"ccx", "ccx_2.22", "ccx_2.21", "ccx_2.20", "ccx_2.19"}) {
#ifdef _WIN32
      const auto f = dir / (std::string(name) + ".exe");
#else
      const auto f = dir / name;
#endif
      if (!dir.empty() && std::filesystem::is_regular_file(f, err)) return f;
    }
    start = end + 1;
  }
  return {};
}

double probe(const FeaResult& r, const Vec3& at, const std::string& field, int mode) {
  if (r.nodes.empty()) throw Error("no results to probe");
  auto value = [&](size_t i) -> double {
    if (field == "von_mises") return r.von_mises.at(i);
    if (field == "displacement") return norm(r.displacement.at(i));
    if (field == "dx" || field == "dy" || field == "dz") return r.displacement.at(i)[size_t(field[1] - 'x')];
    if (field == "sxx" || field == "syy" || field == "szz") return r.stress.at(i)[size_t(field[1] - 'x')];
    if (field == "sxy") return r.stress.at(i)[3];
    if (field == "syz") return r.stress.at(i)[4];
    if (field == "szx") return r.stress.at(i)[5];
    if (field == "mode") return norm(r.modes.at(size_t(mode)).at(i));
    if (field == "temperature") {
      if (r.temperature.empty()) throw Error("probe: temperature is for thermal studies");
      return r.temperature.at(i);
    }
    if (field == "failure_index") {
      if (r.failure_index.empty()) throw Error("probe: failure_index is for printed bodies in a static study");
      return r.failure_index.at(i);
    }
    throw Error("probe: unknown field " + field + " (von_mises, displacement, dx, dy, dz, sxx, syy, szz, sxy, syz, szx, mode, failure_index, temperature)");
  };
  // Inside an element: its corner values weighed by the point's barycentric coordinates (the best element when the point
  // is on the surface or a hair outside it).
  if (r.tets.empty()) {  // a surface only (the CFD air's results): the nearest node
    size_t best = 0;
    for (size_t i = 0; i < r.nodes.size(); ++i)
      if (norm(sub(r.nodes[i], at)) < norm(sub(r.nodes[best], at))) best = i;
    return value(best);
  }
  double best_out = 1e300;
  double best_value = 0;
  for (const auto& t : r.tets) {
    const V a = r.nodes[size_t(t[0])], b = r.nodes[size_t(t[1])], c = r.nodes[size_t(t[2])], d = r.nodes[size_t(t[3])];
    const V ab = sub(b, a), ac = sub(c, a), ad = sub(d, a), ap = sub(at, a);
    const double vol = dot(ab, cross(ac, ad));
    if (std::fabs(vol) < 1e-300) continue;
    const double l1 = dot(ap, cross(ac, ad)) / vol, l2 = dot(ab, cross(ap, ad)) / vol, l3 = dot(ab, cross(ac, ap)) / vol, l0 = 1 - l1 - l2 - l3;
    const double outside = std::max({0.0, -l0, -l1, -l2, -l3});
    if (outside < best_out) {
      best_out = outside;
      best_value = l0 * value(size_t(t[0])) + l1 * value(size_t(t[1])) + l2 * value(size_t(t[2])) + l3 * value(size_t(t[3]));
      if (outside == 0) break;
    }
  }
  if (best_out < 0.05) return best_value;
  size_t best = 0;
  double dist = 1e300;
  for (size_t i = 0; i < r.nodes.size(); ++i) {
    const double e = norm(sub(r.nodes[i], at));
    if (e < dist) dist = e, best = i;
  }
  return value(best);
}

json engines() {
#ifdef OPAD_HAVE_CHRONO
  const bool chrono = true;
#else
  const bool chrono = false;
#endif
  const auto ccx = ccx_program();
  const bool fea = netgen_available() && !ccx.empty();
  json out = {{"motion", true}, {"dynamic", chrono}, {"static", fea}, {"modal", fea}, {"thermal", fea}, {"netgen", netgen_available()}};
  const OpenFoam foam = openfoam();
  out["cfd"] = foam.found();  // thermal studies with the air solved (settings.air = "cfd")
  if (foam.found()) out["openfoam"] = path_to_utf8(foam.wrapper.empty() ? foam.bin : foam.wrapper);
  out["ccx"] = ccx.empty() ? json(nullptr) : json(path_to_utf8(ccx));
  if (!fea)
    out["note"] = ccx.empty() ? "static, modal and thermal studies need CalculiX's ccx: install it (Ubuntu: apt install calculix-ccx; Windows: put ccx.exe beside OPAD) or set OPAD_CCX"
                              : "this build has no Netgen";
  return out;
}

StudyRun run_structural(const Document& doc, const Scene& scene, const std::string& kind, const json& st, const Progress& progress, const AirFilms* air_films) {
  StudyRun run;
  run.kind = kind;
  auto report = [&](double f, const std::string& phase) {
    if (progress && !progress(f, phase)) throw Error("cancelled");
  };
  if (!netgen_available()) throw Error("static and modal studies need Netgen: this build has none");
  const std::filesystem::path ccx = ccx_program();
  if (ccx.empty()) throw Error(engines()["note"].get<std::string>());

  // ---- the case and its loads
  std::string load_case = st.value("case", std::string());
  if (load_case.empty()) load_case = scene.loads.empty() ? std::string("Load case 1") : scene.loads.front().load_case;
  // Thermal studies take the case's thermal loads, static and modal ones its structural loads.
  auto thermal_load = [](const std::string& k) { return k == "heat" || k == "temperature" || k == "convection" || k == "radiation" || k == "fan"; };
  const bool thermal_study = kind == "thermal";
  std::vector<const Load*> loads;
  for (const auto& l : scene.loads)
    if (l.load_case == load_case && thermal_load(l.kind) == thermal_study && !(air_films && (l.kind == "convection" || l.kind == "radiation" || l.kind == "fan"))) {
      if (!l.error.empty()) throw Error("load \"" + l.name + "\": " + l.error);
      loads.push_back(&l);
    }
  if (kind == "static" && loads.empty()) throw Error("load case \"" + load_case + "\" has no loads: add some with the load command");
  if (thermal_study && std::none_of(loads.begin(), loads.end(), [](const Load* l) { return l->kind == "heat" || l->kind == "temperature"; }))
    throw Error("load case \"" + load_case + "\" has no heat: add a heat source (load kind heat, W) or a fixed temperature");

  // ---- bodies: as given, else every body a load names
  std::vector<std::string> bodies;
  auto add_body = [&](const std::string& id) {
    const Node* n = scene.node(id);
    if (!n) throw Error("no body " + id);
    for (const auto& b : n->kind == Node::Kind::Body ? std::vector<std::string>{id} : scene.bodies_under(id))
      if (std::find(bodies.begin(), bodies.end(), b) == bodies.end()) bodies.push_back(b);
  };
  for (const auto& b : st.value("bodies", json::array())) add_body(b.get<std::string>());
  if (bodies.empty())
    for (const Load* l : loads)
      for (const auto& r : l->refs) add_body(r.body);
  if (bodies.empty()) throw Error("a structural study needs bodies: give settings.bodies or loads on their faces");
  std::vector<TopoDS_Shape> world;  // each body's solids, world coordinates
  for (const auto& b : bodies) {
    const Node* n = scene.node(b);
    if (n->body_missing || n->representation != "solid") throw Error("\"" + n->name + "\" is not a solid");
    TopoDS_Compound solids;
    BRep_Builder bb;
    bb.MakeCompound(solids);
    int count = 0;
    const TopoDS_Shape shape = node_world_shape(doc, scene, b);
    for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next(), ++count) bb.Add(solids, e.Current());
    if (!count) throw Error("\"" + n->name + "\" has no solid to analyse");
    world.push_back(count == 1 ? TopoDS_Shape(TopExp_Explorer(shape, TopAbs_SOLID).Current()) : TopoDS_Shape(solids));
  }
  run.summary["case"] = load_case;

  // ---- bolts: cut across the shank
  const std::map<std::string, TopoDS_Shape> fresh;
  const design::ParamTable params;
  const design::Ctx ctx{doc, params, scene, fresh, {}};
  std::vector<Bolt> bolts;
  for (const Load* l : loads) {
    if (l->kind != "bolt_preload") continue;
    if (l->refs.empty()) throw Error("bolt preload \"" + l->name + "\" names no bolt body");
    const std::string body = l->refs.front().body;
    const size_t i = size_t(std::find(bodies.begin(), bodies.end(), body) - bodies.begin());
    if (i >= bodies.size()) throw Error("bolt preload \"" + l->name + "\": its bolt is not among the study's bodies");
    Bolt b = find_bolt(world[i], body);
    if (l->def.contains("axis")) {
      b.axis = ctx.axis(l->def["axis"]);
      b.at = b.axis.Location();
    }
    b.preload = l->def.value("value", 0.0);
    b.load = l->name;
    bolts.push_back(b);
  }
  std::vector<TopoDS_Shape> cut = world;  // bodies after their bolt cuts
  std::vector<std::unique_ptr<BRepAlgoAPI_Splitter>> splits(world.size());
  for (const auto& b : bolts) {
    const size_t i = size_t(std::find(bodies.begin(), bodies.end(), b.body) - bodies.begin());
    const Bnd_Box box = tight_bbox(world[i]);
    const double reach = std::sqrt(box.SquareExtent()) * 2;
    const TopoDS_Face plane = BRepBuilderAPI_MakeFace(gp_Pln(b.at, b.axis.Direction()), -reach, reach, -reach, reach).Face();
    auto sp = std::make_unique<BRepAlgoAPI_Splitter>();
    TopTools_ListOfShape args, tools;
    args.Append(cut[i]);
    tools.Append(plane);
    sp->SetArguments(args);
    sp->SetTools(tools);
    sp->Build();
    if (!sp->IsDone()) throw Error("the bolt \"" + scene.node(b.body)->name + "\" could not be cut across its shank");
    cut[i] = sp->Shape();
    splits[i] = std::move(sp);
  }
  // ---- printed bodies (sim/printing.hpp): each one's settings, and its infill split off from its walls and skins so that
  // the mesh follows the boundary between them. settings.print applies to every body; print.bodies: {id: {...}} adds to
  // it per body, {id: false} leaves a body solid; with print.bodies alone only the bodies it names are printed.
  std::vector<std::optional<PrintSettings>> printed(bodies.size());
  if (const json pj = st.value("print", json()); pj.is_object() && !thermal_study) {
    const json per = pj.value("bodies", json::object());
    bool defaults = false;
    for (const auto& [k, v] : pj.items()) defaults = defaults || k != "bodies";
    for (size_t i = 0; i < bodies.size(); ++i) {
      const json o = per.is_object() && per.contains(bodies[i]) ? per[bodies[i]] : json();
      if (o.is_boolean() && !o.get<bool>()) continue;
      if (!defaults && !o.is_object()) continue;
      json b = pj;
      b.erase("bodies");
      if (o.is_object())
        for (const auto& [k, v] : o.items()) b[k] = v;
      printed[i] = print_settings(b);
    }
  }
  std::vector<std::unique_ptr<BRepAlgoAPI_BuilderAlgo>> parts(bodies.size());
  // Each printed body split into its infill (core_pieces), the walls between its skins (band_pieces) and, when it has
  // skins (split_skins), the top and bottom skins: the rest.
  std::vector<std::vector<TopoDS_Shape>> core_pieces(bodies.size()), band_pieces(bodies.size());
  std::vector<char> split_skins(bodies.size(), 0);
  for (size_t i = 0; i < bodies.size(); ++i) {
    if (!printed[i]) continue;
    report(0.04, "Finding the infill of \"" + scene.node(bodies[i])->name + "\"");
    if (printed[i]->wall_thickness() <= 0 && printed[i]->top_thickness() <= 0 && printed[i]->bottom_thickness() <= 0) {
      for (TopExp_Explorer e(cut[i], TopAbs_SOLID); e.More(); e.Next()) core_pieces[i].push_back(e.Current());  // all infill
      continue;
    }
    const PrintRegions pr = print_regions(world[i], *printed[i]);
    if (pr.core.IsNull()) run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" is no thicker than its walls and skins: it prints solid");
    split_skins[i] = pr.skins;
    if (pr.core.IsNull() && pr.middle.IsNull()) continue;  // all walls, or all skin
    auto part = std::make_unique<BRepAlgoAPI_BuilderAlgo>();
    TopTools_ListOfShape args;
    args.Append(cut[i]);
    if (!pr.middle.IsNull()) args.Append(pr.middle);
    if (!pr.core.IsNull()) args.Append(pr.core);
    part->SetArguments(args);
    part->SetNonDestructive(true);
    part->Build();
    if (!part->IsDone()) throw Error("the infill of \"" + scene.node(bodies[i])->name + "\" could not be split from its walls");
    for (TopExp_Explorer e(pr.core, TopAbs_SOLID); !pr.core.IsNull() && e.More(); e.Next())
      for (const auto& f : after(*part, e.Current())) core_pieces[i].push_back(f);
    for (TopExp_Explorer e(pr.middle, TopAbs_SOLID); !pr.middle.IsNull() && e.More(); e.Next())
      for (const auto& f : after(*part, e.Current()))
        if (std::none_of(core_pieces[i].begin(), core_pieces[i].end(), [&](const TopoDS_Shape& c) { return c.IsSame(f); })) band_pieces[i].push_back(f);
    cut[i] = part->Shape();
    parts[i] = std::move(part);
  }
  report(0.05, "Joining the bodies");
  // ---- join: one shape whose touching faces are shared
  // A single body needs no joining: its shapes stand for themselves.
  struct Join {
    BRepAlgoAPI_BuilderAlgo algo;
    bool used = false;
    const TopTools_ListOfShape& Modified(const TopoDS_Shape& f) { return used ? algo.Modified(f) : empty; }
    bool IsDeleted(const TopoDS_Shape& f) { return used && algo.IsDeleted(f); }
    TopTools_ListOfShape empty;
  } fuse;
  TopoDS_Shape joined = cut.front();
  if (cut.size() > 1) {
    TopTools_ListOfShape args;
    for (const auto& s : cut) args.Append(s);
    fuse.algo.SetArguments(args);
    fuse.algo.SetNonDestructive(true);
    fuse.algo.Build();
    if (!fuse.algo.IsDone()) throw Error("the bodies could not be joined for meshing");
    fuse.used = true;
    joined = fuse.algo.Shape();
  }
  TopoDS_Compound solids;
  BRep_Builder bb;
  bb.MakeCompound(solids);
  for (TopExp_Explorer e(joined, TopAbs_SOLID); e.More(); e.Next()) bb.Add(solids, e.Current());
  // Which body each final solid belongs to.
  std::vector<std::vector<TopoDS_Shape>> body_solids(bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i)
    for (TopExp_Explorer e(cut[i], TopAbs_SOLID); e.More(); e.Next())
      for (const auto& f : after(fuse, e.Current())) body_solids[i].push_back(f);

  // ---- mesh
  const Bnd_Box all = tight_bbox(solids);
  const double size = std::sqrt(all.SquareExtent());
  double maxh = st.value("mesh_size", 0.0);
  if (maxh <= 0) maxh = std::max(size / 25, 0.5);
  run.summary["mesh_size"] = maxh;
  report(0.1, "Meshing");
  const VolumeMesh mesh = mesh_solids(solids, maxh, st.value("grading", 0.3), st.value("second_order", true), [&] { return progress && !progress(0.1, "Meshing"); });
  if (mesh.straightened)
    run.warnings.push_back(std::to_string(mesh.straightened) + " element(s) curved inside out on a tight curve were made straight-sided (a finer mesh_size there keeps them curved)");
  const int tn = mesh.tet_nodes;
  // Solids of the mesh -> bodies.
  std::vector<int> solid_body(size_t(mesh.solids.Extent()), -1);
  for (int k = 1; k <= mesh.solids.Extent(); ++k)
    for (size_t i = 0; i < bodies.size(); ++i)
      for (const auto& s : body_solids[i])
        if (s.IsSame(mesh.solids(k))) solid_body[size_t(k - 1)] = int(i);
  for (size_t k = 0; k < solid_body.size(); ++k)
    if (solid_body[k] < 0) throw Error("a meshed solid belongs to no body (the join changed it)");
  std::vector<char> solid_region(size_t(mesh.solids.Extent()), 0);  // a printed body's infill (1), walls between its skins (2)
  for (size_t i = 0; i < bodies.size(); ++i)
    for (const auto* pieces : {&core_pieces[i], &band_pieces[i]})
      for (const auto& piece : *pieces)
        for (const auto& f : after(fuse, piece))
          for (int k = 1; k <= mesh.solids.Extent(); ++k)
            if (f.IsSame(mesh.solids(k))) solid_region[size_t(k - 1)] = pieces == &core_pieces[i] ? 1 : 2;

  // Tet faces by their sorted corners, to find the element under a surface triangle (CalculiX face numbers S1..S4 of
  // C3D10/C3D4 in Abaqus order: 1-2-3, 1-4-2, 2-4-3, 3-4-1).
  static const int faces_of[4][3] = {{0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {2, 3, 0}};
  std::map<std::array<int, 3>, std::vector<std::pair<int, int>>> tet_face;  // corners -> (element, face 1..4)
  for (size_t e = 0; e < mesh.tets.size(); ++e)
    for (int f = 0; f < 4; ++f) {
      std::array<int, 3> k = {mesh.tets[e][size_t(faces_of[f][0])], mesh.tets[e][size_t(faces_of[f][1])], mesh.tets[e][size_t(faces_of[f][2])]};
      std::sort(k.begin(), k.end());
      tet_face[k].push_back({int(e), f + 1});
    }
  auto corners = [&](size_t t) {
    std::array<int, 3> k = {mesh.tris[t][0], mesh.tris[t][1], mesh.tris[t][2]};
    std::sort(k.begin(), k.end());
    return k;
  };
  auto centroid = [&](int e) {
    V c{0, 0, 0};
    for (int k = 0; k < 4; ++k) c = add(c, mesh.nodes[size_t(mesh.tets[size_t(e)][size_t(k)])]);
    return mul(c, 0.25);
  };

  // Load faces -> mesh triangles. A reference is resolved on the body as it is, then followed through the cut and the join.
  auto triangles_of = [&](const Load& l) {
    std::set<int> faces;  // mesh face indices (0-based)
    for (const auto& rj : l.def.value("refs", json::array())) {
      for (const auto& rr : ctx.resolve_all(rj)) {
        const size_t i = size_t(std::find(bodies.begin(), bodies.end(), rr.node) - bodies.begin());
        if (i >= bodies.size()) throw Error("load \"" + l.name + "\" acts on a body outside the study");
        if (rr.sub.ShapeType() != TopAbs_FACE) throw Error("load \"" + l.name + "\" needs faces");
        TopTools_IndexedMapOfShape map;
        TopExp::MapShapes(world[i], TopAbs_FACE, map);
        const TopoDS_Shape original = map(rr.index + 1);
        std::vector<TopoDS_Shape> now = {original};
        if (splits[i]) {
          std::vector<TopoDS_Shape> next;
          for (const auto& f : now)
            for (const auto& g : after(*splits[i], f)) next.push_back(g);
          now = next;
        }
        if (parts[i]) {  // a printed body's split into walls and infill
          std::vector<TopoDS_Shape> next;
          for (const auto& f : now)
            for (const auto& g : after(*parts[i], f)) next.push_back(g);
          now = next;
        }
        std::vector<TopoDS_Shape> fin;
        for (const auto& f : now)
          for (const auto& g : after(fuse, f)) fin.push_back(g);
        for (const auto& f : fin)
          if (const int k = mesh.faces.FindIndex(f); k > 0) faces.insert(k - 1);
      }
    }
    std::vector<size_t> tris;
    for (size_t t = 0; t < mesh.tris.size(); ++t)
      if (faces.count(mesh.tri_face[t])) tris.push_back(t);
    if (tris.empty()) throw Error("load \"" + l.name + "\": its faces are not on the meshed bodies' surface");
    return tris;
  };
  // A triangle's area, outward unit normal (away from its element) and consistent nodal weights (sum 1).
  struct Tri {
    double area;
    V normal;
    std::vector<std::pair<int, double>> weights;
  };
  auto tri_info = [&](size_t t) {
    Tri out;
    const V a = mesh.nodes[size_t(mesh.tris[t][0])], b = mesh.nodes[size_t(mesh.tris[t][1])], c = mesh.nodes[size_t(mesh.tris[t][2])];
    V n = cross(sub(b, a), sub(c, a));
    out.area = norm(n) / 2;
    n = unit(n);
    const auto it = tet_face.find(corners(t));
    if (it != tet_face.end() && !it->second.empty()) {
      const V inside = centroid(it->second.front().first);
      if (dot(sub(inside, a), n) > 0) n = mul(n, -1);
    }
    out.normal = n;
    if (mesh.tri_nodes == 6) {
      for (int k = 3; k < 6; ++k) out.weights.push_back({mesh.tris[t][size_t(k)], 1.0 / 3});  // a quadratic triangle's uniform load: all at mid-sides
    } else {
      for (int k = 0; k < 3; ++k) out.weights.push_back({mesh.tris[t][size_t(k)], 1.0 / 3});
    }
    return out;
  };
  // A surface triangle's area, its mid-side nodes counted (four flat parts of a curved quadratic triangle).
  auto tri_area = [&](size_t t) {
    const auto& tri = mesh.tris[t];
    auto area3 = [&](int i, int j, int k) {
      return norm(cross(sub(mesh.nodes[size_t(i)], mesh.nodes[size_t(k)]), sub(mesh.nodes[size_t(j)], mesh.nodes[size_t(k)]))) / 2;
    };
    if (mesh.tri_nodes != 6) return area3(tri[0], tri[1], tri[2]);
    // Mid-side node of each edge: the one nearest the edge's middle.
    int mid[3];
    for (int e = 0; e < 3; ++e) {
      const V m = mul(add(mesh.nodes[size_t(tri[size_t(e)])], mesh.nodes[size_t(tri[size_t((e + 1) % 3)])]), 0.5);
      double best = 1e300;
      for (int k = 3; k < 6; ++k)
        if (norm(sub(mesh.nodes[size_t(tri[size_t(k)])], m)) < best) best = norm(sub(mesh.nodes[size_t(tri[size_t(k)])], m)), mid[e] = tri[size_t(k)];
    }
    return area3(tri[0], mid[0], mid[2]) + area3(mid[0], tri[1], mid[1]) + area3(mid[2], mid[1], tri[2]) + area3(mid[0], mid[1], mid[2]);
  };
  auto nodes_of = [&](const std::vector<size_t>& tris) {
    std::set<int> out;
    for (size_t t : tris)
      for (int k = 0; k < mesh.tri_nodes; ++k) out.insert(mesh.tris[t][size_t(k)]);
    return out;
  };

  std::ostringstream cload;
  // Bolt pre-tension sections, made here rather than by CalculiX's *PRE-TENSION SECTION (whose cut stays bridged by elements
  // that touch it only at an edge): the cut's nodes are doubled for every element of the bolt below the cut, the two copies
  // tied across (no sliding) by equations, and their overlap along the axis is a reference node's displacement, which the
  // preload pushes: u_ref = (u_below - u_above) . axis, so the preload shortens the bolt's grip as tightening a nut does.
  std::vector<Vec3> nodes_out = mesh.nodes;
  std::vector<std::array<int, 10>> tets_out = mesh.tets;
  std::ostringstream equations;
  json bolt_info = json::array();
  std::vector<int> ref_nodes;
  for (size_t b = 0; b < bolts.size(); ++b) {
    const Bolt& bo = bolts[b];
    const V n{bo.axis.Direction().X(), bo.axis.Direction().Y(), bo.axis.Direction().Z()};
    const V at{bo.at.X(), bo.at.Y(), bo.at.Z()};
    const size_t bi = size_t(std::find(bodies.begin(), bodies.end(), bo.body) - bodies.begin());
    const double tol = 1e-6 * size;
    double area = 0;
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
      bool on = true;
      for (int k = 0; k < 3 && on; ++k) on = std::fabs(dot(sub(mesh.nodes[size_t(mesh.tris[t][size_t(k)])], at), n)) < tol;
      if (!on) continue;
      const auto it = tet_face.find(corners(t));
      if (it == tet_face.end()) continue;
      bool bolt_face = false;
      for (const auto& [e, f] : it->second) bolt_face = bolt_face || solid_body[size_t(mesh.tet_solid[size_t(e)])] == int(bi);
      if (bolt_face) area += tri_area(t);
    }
    if (area <= 0) throw Error("bolt \"" + scene.node(bo.body)->name + "\": no elements on its cut (is the axis right?)");
    std::map<int, int> twin;  // node on the cut -> its copy below
    for (size_t e = 0; e < tets_out.size(); ++e) {
      if (solid_body[size_t(mesh.tet_solid[e])] != int(bi) || dot(sub(centroid(int(e)), at), n) >= 0) continue;
      for (int k = 0; k < tn; ++k) {
        const int node = tets_out[e][size_t(k)];
        if (std::fabs(dot(sub(mesh.nodes[size_t(node)], at), n)) >= tol) continue;
        auto [it, fresh] = twin.try_emplace(node, int(nodes_out.size()));
        if (fresh) nodes_out.push_back(mesh.nodes[size_t(node)]);
        tets_out[e][size_t(k)] = it->second;
      }
    }
    if (twin.empty()) throw Error("bolt \"" + scene.node(bo.body)->name + "\": nothing of it lies on the cut");
    ref_nodes.push_back(int(b));  // numbered after every node, below
    // Two directions across the axis.
    const V t1 = unit(std::fabs(n[0]) < 0.9 ? cross(n, V{1, 0, 0}) : cross(n, V{0, 1, 0})), t2 = cross(n, t1);
    const std::array<V, 3> dirs = {n, t1, t2};
    // Which of the copy's degrees of freedom each equation eliminates: the largest coefficients, each used once.
    std::array<int, 3> dep{};
    std::array<bool, 3> used{};
    for (int q = 0; q < 3; ++q) {
      int best = -1;
      for (int k = 0; k < 3; ++k)
        if (!used[size_t(k)] && (best < 0 || std::fabs(dirs[size_t(q)][size_t(k)]) > std::fabs(dirs[size_t(q)][size_t(best)]))) best = k;
      dep[size_t(q)] = best, used[size_t(best)] = true;
    }
    for (const auto& [above, below] : twin)
      for (int q = 0; q < 3; ++q) {
        const V& d = dirs[size_t(q)];
        std::vector<std::tuple<int, int, double>> terms;  // node (1-based), dof, coefficient
        terms.push_back({below + 1, dep[size_t(q)] + 1, -d[size_t(dep[size_t(q)])]});
        for (int k = 0; k < 3; ++k)
          if (k != dep[size_t(q)] && std::fabs(d[size_t(k)]) > 1e-12) terms.push_back({below + 1, k + 1, -d[size_t(k)]});
        for (int k = 0; k < 3; ++k)
          if (std::fabs(d[size_t(k)]) > 1e-12) terms.push_back({above + 1, k + 1, d[size_t(k)]});
        if (q == 0) terms.push_back({-1 - int(b), 1, 1.0});  // the reference node, numbered below
        equations << "*EQUATION\n" << terms.size() << "\n";
        for (size_t i = 0; i < terms.size(); ++i) {
          const auto& [node, dof, c] = terms[i];
          equations << "@" << node << "@, " << dof << ", " << c << ((i + 1) % 4 == 0 || i + 1 == terms.size() ? "\n" : ", ");
        }
      }
    bolt_info.push_back({{"name", bo.load}, {"bolt", scene.node(bo.body)->name}, {"preload_N", bo.preload}, {"section_area_mm2", area},
                         {"nominal_stress_MPa", bo.preload / area}, {"at", at}, {"axis", n}, {"section_nodes", twin.size()}});
  }
  const int bolt_node0 = int(nodes_out.size()) + 1;
  {
    // Reference nodes get their numbers now that every node is in.
    std::string text = equations.str(), out;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '@') {
        out += text[i];
        continue;
      }
      const size_t j = text.find('@', i + 1);
      int node = std::stoi(text.substr(i + 1, j - i - 1));
      if (node < 0) node = bolt_node0 + (-node - 1);
      out += std::to_string(node);
      i = j;
    }
    equations.str(out);
  }
  for (size_t b = 0; b < bolts.size(); ++b) cload << bolt_node0 + int(b) << ", 1, " << bolts[b].preload << "\n";

  // ---- printed bodies: each element's region (wall, top/bottom skin, infill) and its axes (1 along the roads or the
  // infill's direction, 2 across in the layer, 3 the build direction). A skin element is in a top or bottom skin when the
  // surface straight above it is within top_thickness (below: bottom_thickness), else in a wall, whose roads follow the
  // nearest side face. Walls (and concentric infill) are grouped by road direction in 5 degree steps.
  struct PrintedElement {
    int region = -1;  // Region, -1: not printed
    int bin = 0;      // road direction step (walls, concentric infill)
  };
  std::vector<PrintedElement> pel(mesh.tets.size());
  std::vector<std::array<V, 3>> paxes(mesh.tets.size());
  constexpr int kBins = 36;
  auto layer_axes = [](const V& up) {  // the layer plane's x (the world X, else Y, as the slicer's bed has it) and y
    V ex = sub(V{1, 0, 0}, mul(up, up[0]));
    if (norm(ex) < 0.3) ex = sub(V{0, 1, 0}, mul(up, up[1]));
    ex = mul(ex, 1 / norm(ex));
    return std::array<V, 2>{ex, cross(up, ex)};
  };
  auto in_plane = [](const std::array<V, 2>& xy, double deg) {
    const double a = deg * 3.14159265358979323846 / 180;
    return add(mul(xy[0], std::cos(a)), mul(xy[1], std::sin(a)));
  };
  for (size_t i = 0; i < bodies.size(); ++i) {
    if (!printed[i]) continue;
    const PrintSettings& ps = *printed[i];
    const V up = ps.up;
    const auto xy = layer_axes(up);
    // The body's outer surface: faces of its elements that no other of its elements shares, facing out.
    std::map<std::array<int, 3>, std::pair<int, int>> faces;  // sorted corners -> (count, element * 4 + face)
    static const int fc[4][4] = {{0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 3, 1}, {1, 2, 3, 0}};  // three corners, then the fourth
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
      if (solid_body[size_t(mesh.tet_solid[e])] != int(i)) continue;
      for (int f = 0; f < 4; ++f) {
        std::array<int, 3> k = {mesh.tets[e][size_t(fc[f][0])], mesh.tets[e][size_t(fc[f][1])], mesh.tets[e][size_t(fc[f][2])]};
        std::sort(k.begin(), k.end());
        auto& slot = faces[k];
        ++slot.first;
        slot.second = int(e) * 4 + f;
      }
    }
    std::vector<PrintSurface::T> tris;
    for (const auto& [k, slot] : faces) {
      if (slot.first != 1) continue;
      const auto& t = mesh.tets[size_t(slot.second / 4)];
      const int* c = fc[slot.second % 4];
      const V a = mesh.nodes[size_t(t[size_t(c[0])])], b = mesh.nodes[size_t(t[size_t(c[1])])], cc = mesh.nodes[size_t(t[size_t(c[2])])];
      const V d = mesh.nodes[size_t(t[size_t(c[3])])];
      V n = cross(sub(b, a), sub(cc, a));
      if (dot(n, sub(d, a)) > 0) n = mul(n, -1);
      const double ln = norm(n);
      if (ln < 1e-15) continue;
      tris.push_back({a, b, cc, mul(n, 1 / ln)});
    }
    const PrintSurface surf(std::move(tris), up, xy[0], xy[1], std::max(maxh, 2 * ps.wall_thickness()));
    const double tt = ps.top_thickness(), tb = ps.bottom_thickness();
    auto road_bin = [&](const V& p) {
      const PrintSurface::T* side = surf.nearestSide(p);
      if (!side) return 0;
      const V t = cross(up, side->n);  // along the contour
      double a = std::atan2(dot(t, xy[1]), dot(t, xy[0]));
      if (a < 0) a += 3.14159265358979323846;
      return std::min(kBins - 1, int(a / 3.14159265358979323846 * kBins));
    };
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
      if (solid_body[size_t(mesh.tet_solid[e])] != int(i)) continue;
      V p{0, 0, 0};
      for (int k = 0; k < 4; ++k) p = add(p, mesh.nodes[size_t(mesh.tets[e][size_t(k)])]);
      p = mul(p, 0.25);
      PrintedElement& pe = pel[e];
      const char sr = solid_region[size_t(mesh.tet_solid[e])];
      if (sr == 1) {
        pe.region = int(Region::Core);
        if (ps.family == "concentric") pe.bin = road_bin(p);
      } else if (sr != 2 && (split_skins[i] || (tt > 0 && surf.ray(p, 1) <= tt) || (tb > 0 && surf.ray(p, -1) <= tb))) {
        pe.region = int(Region::TopBottom);
      } else {
        pe.region = int(Region::Wall);
        pe.bin = road_bin(p);
      }
      const bool contour = pe.region == int(Region::Wall) || (pe.region == int(Region::Core) && ps.family == "concentric");
      const double deg = contour ? (pe.bin + 0.5) * 180.0 / kBins : pe.region == int(Region::Core) ? ps.angle : 0.0;
      const V a1 = in_plane(xy, deg);
      paxes[e] = {a1, cross(up, a1), up};
    }
  }

  // The outer skin: triangles with one element behind them, quadratic ones split in four.
  auto build_skin = [&](FeaResult& out) {
    for (size_t t = 0; t < mesh.tris.size(); ++t) {
      const auto it = tet_face.find(corners(t));
      if (it == tet_face.end() || it->second.size() != 1) continue;
      const int body = solid_body[size_t(mesh.tet_solid[size_t(it->second.front().first)])];
      const auto& tri = mesh.tris[t];
      if (mesh.tri_nodes == 6) {
        // Which mid-side node sits on which edge: the nearest edge middle.
        int mid[3] = {-1, -1, -1};  // edge 01, 12, 20
        for (int k = 3; k < 6; ++k) {
          const V p = mesh.nodes[size_t(tri[size_t(k)])];
          double best = 1e300;
          int e = 0;
          for (int j = 0; j < 3; ++j) {
            const V m = mul(add(mesh.nodes[size_t(tri[size_t(j)])], mesh.nodes[size_t(tri[size_t((j + 1) % 3)])]), 0.5);
            if (norm(sub(m, p)) < best) best = norm(sub(m, p)), e = j;
          }
          mid[e] = tri[size_t(k)];
        }
        if (mid[0] >= 0 && mid[1] >= 0 && mid[2] >= 0) {
          for (const auto& s : {std::array<int, 3>{tri[0], mid[0], mid[2]}, {mid[0], tri[1], mid[1]}, {mid[2], mid[1], tri[2]}, {mid[0], mid[1], mid[2]}}) {
            out.skin.push_back(s);
            out.skin_body.push_back(body);
          }
          continue;
        }
      }
      out.skin.push_back({tri[0], tri[1], tri[2]});
      out.skin_body.push_back(body);
    }
  };

  // ---- CalculiX: one input in the job folder, its results read back (thermal studies run it more than once).
  struct Dir {
    std::filesystem::path p;
    bool keep = false;
    ~Dir() {
      std::error_code e;
      if (!keep && !p.empty()) std::filesystem::remove_all(p, e);
    }
  } dir;
  dir.p = std::filesystem::temp_directory_path() / ("opad-ccx-" + new_uuid().substr(0, 12));
  std::filesystem::create_directories(dir.p);
  if (const char* k = std::getenv("OPAD_KEEP_CCX"); k && *k) dir.keep = true;
  double ccx_seconds = 0;
  auto run_ccx = [&](const std::string& input, double at) {
    report(at, "Solving (CalculiX)");
    const auto started = std::chrono::steady_clock::now();
    struct Clock {
      std::chrono::steady_clock::time_point t;
      double& sum;
      ~Clock() { sum += std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count(); }
    } clock{started, ccx_seconds};
    write_text_file(dir.p / "job.inp", input);
    // One thread for the solver unless asked: CalculiX 2.21's threaded SPOOLES factorisation (Ubuntu's ccx) races and now and
    // then returns wrong displacements for the same input (seen on a cantilever: 3.04 mm three runs out of five, 5.44 mm or
    // 1.82 mm the others); single-threaded it is exact and repeatable. OPAD_CCX_THREADS=n for a ccx known to be safe.
    {
      const char* asked = std::getenv("OPAD_CCX_THREADS");
      const std::string threads = asked && *asked ? asked : "1";
      // Cavity radiation's view factors (cfd.radiation "calculix") have no such race: every core.
      const std::string view_threads = std::to_string(std::max(1u, std::thread::hardware_concurrency()));
#ifdef _WIN32
      _putenv_s("OMP_NUM_THREADS", threads.c_str());
      _putenv_s("CCX_NPROC_EQUATION_SOLVER", threads.c_str());
      _putenv_s("CCX_NPROC_VIEWFACTOR", view_threads.c_str());
#else
      // Only when it changes: sweep points run at once (sim/sweep.cpp), and setenv beside getenv is a race.
      for (const char* name : {"OMP_NUM_THREADS", "CCX_NPROC_EQUATION_SOLVER"})
        if (const char* now = std::getenv(name); !now || threads != now) setenv(name, threads.c_str(), 1);
      if (const char* now = std::getenv("CCX_NPROC_VIEWFACTOR"); !now || view_threads != now) setenv("CCX_NPROC_VIEWFACTOR", view_threads.c_str(), 1);
#endif
    }
    detail::RunOptions ro;
    ro.output = dir.p / "ccx.log";
    ro.timeout_ms = int(st.value("timeout", 1800.0) * 1000);
    ro.cancelled = [&] { return progress && !progress(at, "Solving (CalculiX)"); };
    const int status = detail::run_program(ccx, {"-i", "job"}, dir.p, ro);
    std::string log;
    try {
      log = read_text_file(dir.p / "ccx.log");
    } catch (...) {
    }
    if (status != 0 || log.find("*ERROR") != std::string::npos) {
      const auto pos = log.find("*ERROR");
      std::string why = pos == std::string::npos ? "it stopped with status " + std::to_string(status) : log.substr(pos, std::min<size_t>(400, log.size() - pos));
      throw Error("CalculiX failed: " + why);
    }
    return read_frd(dir.p / "job.frd");
  };

  // ---- thermal: heat sources, fixed temperatures, convection (given, or from the air: sim/airflow.hpp), radiation and
  // fans through heatsinks; steady, or over time from the ambient temperature. Convection that depends on the temperatures
  // (natural, a fan's air warming along the fins) is found by solving again until the temperatures settle.
  if (thermal_study) {
    namespace air = sim::air;
    const double ambient = st.value("ambient", 25.0);
    V up = unit(mul(st.value("gravity", V{0, 0, -1}), -1));
    if (norm(up) < 0.5) up = {0, 0, 1};
    const bool transient = st.contains("duration");
    std::vector<Mat> mats;
    for (size_t i = 0; i < bodies.size(); ++i) {
      mats.push_back(material_for(doc, scene, bodies[i], st.value("materials", json())));
      if (mats.back().thermal_assumed) run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" has no material with thermal properties: steel assumed");
      // A board's across: its thinnest way of the world's axes when not given.
      if (mats.back().k_through > 0 && norm(mats.back().normal) < 1e-9) {
        Bnd_Box b;
        BRepBndLib::Add(world[i], b);
        double c[6];
        b.Get(c[0], c[1], c[2], c[3], c[4], c[5]);
        const double ext[3] = {c[3] - c[0], c[4] - c[1], c[5] - c[2]};
        const int thin = int(std::min_element(ext, ext + 3) - ext);
        mats.back().normal = {thin == 0 ? 1.0 : 0.0, thin == 1 ? 1.0 : 0.0, thin == 2 ? 1.0 : 0.0};
      }
    }
    // Elements and the skin: each outer triangle's element face, area, outward normal and centre.
    auto tet_volume = [&](size_t e) {
      const V a = mesh.nodes[size_t(mesh.tets[e][0])], b = mesh.nodes[size_t(mesh.tets[e][1])], c = mesh.nodes[size_t(mesh.tets[e][2])],
              d = mesh.nodes[size_t(mesh.tets[e][3])];
      return std::fabs(dot(sub(b, a), cross(sub(c, a), sub(d, a)))) / 6;
    };
    std::vector<double> body_volume(bodies.size(), 0.0);
    for (size_t e = 0; e < mesh.tets.size(); ++e) body_volume[size_t(solid_body[size_t(mesh.tet_solid[e])])] += tet_volume(e);
    struct Face {
      size_t tri;
      int elem, face, body;
      double area;
      V normal, centre;
    };
    auto skin_face = [&](size_t t) -> std::optional<Face> {
      const auto it = tet_face.find(corners(t));
      if (it == tet_face.end() || it->second.size() != 1) return std::nullopt;  // inside: two bodies bonded there
      Face f;
      f.tri = t;
      f.elem = it->second.front().first;
      f.face = it->second.front().second;
      f.body = solid_body[size_t(mesh.tet_solid[size_t(f.elem)])];
      f.area = tri_area(t);
      f.normal = tri_info(t).normal;
      V c{0, 0, 0};
      for (int k = 0; k < 3; ++k) c = add(c, mesh.nodes[size_t(mesh.tris[t][size_t(k)])]);
      f.centre = mul(c, 1.0 / 3);
      return f;
    };
    // A load's outer triangles: its faces', or every outer triangle of the bodies it names.
    auto load_faces = [&](const Load& l, bool bodies_ok) {
      std::vector<Face> out;
      const bool on_bodies = !l.refs.empty() && std::all_of(l.refs.begin(), l.refs.end(), [](const Ref& r) { return r.kind == Ref::Kind::Body; });
      if (on_bodies) {
        if (!bodies_ok) throw Error("load \"" + l.name + "\" acts on faces: pick faces, not bodies");
        std::set<int> wanted;
        for (const auto& r : l.refs) wanted.insert(int(std::find(bodies.begin(), bodies.end(), r.body) - bodies.begin()));
        for (size_t t = 0; t < mesh.tris.size(); ++t)
          if (const auto f = skin_face(t); f && wanted.count(f->body)) out.push_back(*f);
        return out;
      }
      for (size_t t : triangles_of(l))
        if (const auto f = skin_face(t)) out.push_back(*f);
      if (out.empty()) throw Error("load \"" + l.name + "\": its faces are inside, where bodies touch: pick outer faces");
      return out;
    };
    auto body_of = [&](const Load& l) {
      if (l.refs.empty()) throw Error("load \"" + l.name + "\" names no body");
      const size_t i = size_t(std::find(bodies.begin(), bodies.end(), l.refs.front().body) - bodies.begin());
      if (i >= bodies.size()) throw Error("load \"" + l.name + "\" acts on a body outside the study");
      return i;
    };
    // Something to look at along a face's normal: the gap to the nearest solid (mm), 0 when nothing is there.
    IntCurvesFace_ShapeIntersector rays;
    rays.Load(solids, 1e-7);
    auto gap_along = [&](const V& from, const V& n, double reach) {
      const gp_Lin line(gp_Pnt(from[0], from[1], from[2]), gp_Dir(n[0], n[1], n[2]));
      rays.Perform(line, 1e-3 * reach, reach);
      double best = 0;
      for (int i = 1; rays.IsDone() && i <= rays.NbPnt(); ++i)
        if (const double w = rays.WParameter(i); w > 1e-3 && (best == 0 || w < best)) best = w;
      return best;
    };

    // Films (convection) and radiation on element faces; their h and sink temperature are set per solve.
    struct Film {
      Face f;
      double h = 0, sink = 0;  // W/m2K, degC
      int load = 0;            // index into loads
    };
    struct Rad {
      Face f;
      double eps = 0, sink = 0;
      bool cavity = false;  // the faces see each other (CalculiX's view factors), the rest of their view the room
    };
    std::vector<Film> films;
    std::vector<Rad> rads;
    std::ostringstream fixed_temps, fluxes;
    double heat_in = 0;
    json applied = json::array();
    // Natural convection: each OCC face of the load as one plate, its triangles grouped.
    struct Plate {
      std::vector<size_t> films;  // indices into films
      double up = 0, L = 0, gap = 0;
    };
    std::vector<Plate> plates;
    std::vector<size_t> plate_load;  // the load of each plate
    // Fans: a heatsink each, its fins, the fan, the films it sets.
    struct FanRun {
      size_t load;
      air::Fan fan;
      int count = 1;
      air::FinArray fins;
      double inlet = 0, Q = 0;
      air::Channel ch{};
      std::vector<size_t> films;
      double heat = 0, rise = 0;
    };
    std::vector<FanRun> fan_runs;
    for (size_t li = 0; li < loads.size(); ++li) {
      const Load& l = *loads[li];
      const json& d = l.def;
      if (l.kind == "heat") {
        const double P = d.value("value", 0.0);
        heat_in += P;
        const bool on_bodies = !l.refs.empty() && std::all_of(l.refs.begin(), l.refs.end(), [](const Ref& r) { return r.kind == Ref::Kind::Body; });
        if (on_bodies) {
          double V_total = 0;
          std::set<int> wanted;
          for (const auto& r : l.refs) wanted.insert(int(std::find(bodies.begin(), bodies.end(), r.body) - bodies.begin()));
          for (int b : wanted) V_total += body_volume[size_t(b)];
          const double q = 1000 * P / V_total;  // mW/mm3
          for (size_t e = 0; e < mesh.tets.size(); ++e)
            if (wanted.count(solid_body[size_t(mesh.tet_solid[e])])) fluxes << e + 1 << ", BF, " << q << "\n";
          applied.push_back({{"name", l.name}, {"kind", "heat"}, {"W", P}, {"in", "volume"}, {"volume_mm3", V_total}});
        } else {
          const auto fs = load_faces(l, false);
          double A = 0;
          for (const auto& f : fs) A += f.area;
          const double q = 1000 * P / A;  // mW/mm2
          for (const auto& f : fs) fluxes << f.elem + 1 << ", S" << f.face << ", " << q << "\n";
          applied.push_back({{"name", l.name}, {"kind", "heat"}, {"W", P}, {"in", "faces"}, {"area_mm2", A}});
        }
      } else if (l.kind == "temperature") {
        const auto fs = load_faces(l, false);
        std::set<int> nodes;
        for (const auto& f : fs)
          for (int k = 0; k < mesh.tri_nodes; ++k) nodes.insert(mesh.tris[f.tri][size_t(k)]);
        for (int n : nodes) fixed_temps << n + 1 << ", 11, 11, " << d.value("value", ambient) << "\n";
        applied.push_back({{"name", l.name}, {"kind", "temperature"}, {"C", d.value("value", ambient)}, {"nodes", nodes.size()}});
      } else if (l.kind == "convection") {
        const auto fs = load_faces(l, true);
        const double sink = d.value("ambient", ambient);
        const json h = d.value("h", json(10.0));
        if (h.is_number()) {
          for (const auto& f : fs) films.push_back({f, h.get<double>(), sink, int(li)});
        } else if (h == "natural") {
          std::map<int, std::vector<Face>> by_face;  // OCC face -> its triangles
          for (const auto& f : fs) by_face[mesh.tri_face[f.tri]].push_back(f);
          for (const auto& [face, tris] : by_face) {
            Plate p;
            double A = 0;
            V n{0, 0, 0}, c{0, 0, 0};
            for (const auto& f : tris) A += f.area, n = add(n, mul(f.normal, f.area)), c = add(c, mul(f.centre, f.area));
            c = mul(c, 1 / A);
            const bool flat = norm(n) > 0.5 * A;  // a curved face (a cylinder all round) has no one way it looks
            n = flat ? unit(n) : up;
            // Its height along up (a tilted face), or its area over its perimeter (a level one).
            V u1 = sub(up, mul(n, dot(up, n)));
            if (norm(u1) < 1e-6) u1 = std::fabs(n[0]) < 0.9 ? cross(n, V{1, 0, 0}) : cross(n, V{0, 1, 0});
            u1 = unit(u1);
            const V u2 = cross(n, u1);
            double lo1 = 1e300, hi1 = -1e300, lo2 = 1e300, hi2 = -1e300, loz = 1e300, hiz = -1e300;
            for (const auto& f : tris)
              for (int k = 0; k < 3; ++k) {
                const V q = mesh.nodes[size_t(mesh.tris[f.tri][size_t(k)])];
                lo1 = std::min(lo1, dot(q, u1)), hi1 = std::max(hi1, dot(q, u1));
                lo2 = std::min(lo2, dot(q, u2)), hi2 = std::max(hi2, dot(q, u2));
                loz = std::min(loz, dot(q, up)), hiz = std::max(hiz, dot(q, up));
              }
            p.up = flat ? dot(n, up) : 0;
            const bool level = std::fabs(p.up) > 0.7072;
            p.L = level ? A / std::max(1e-9, 2 * ((hi1 - lo1) + (hi2 - lo2))) : std::max(hiz - loz, 1e-3);
            if (!level && flat) {
              const double g = gap_along(add(c, mul(n, 1e-3)), n, std::max(p.L, 1.0));
              if (g > 0 && g < p.L) p.gap = g;
            }
            for (const auto& f : tris) {
              p.films.push_back(films.size());
              films.push_back({f, 5.0, sink, int(li)});
            }
            plates.push_back(p);
            plate_load.push_back(li);
          }
        } else {  // forced: a stream along the faces at velocity, the way of vector
          const double U = d.value("velocity", 0.0);
          const V way = unit(d.value("vector", V{1, 0, 0}));
          std::map<int, std::vector<Face>> by_face;
          for (const auto& f : fs) by_face[mesh.tri_face[f.tri]].push_back(f);
          for (const auto& [face, tris] : by_face) {
            double lo = 1e300, hi = -1e300;
            for (const auto& f : tris)
              for (int k = 0; k < 3; ++k) {
                const double x = dot(mesh.nodes[size_t(mesh.tris[f.tri][size_t(k)])], way);
                lo = std::min(lo, x), hi = std::max(hi, x);
              }
            const double hf = air::forced_plate_h(U, std::max(hi - lo, 1.0) * 1e-3, sink);
            for (const auto& f : tris) films.push_back({f, hf, sink, int(li)});
          }
        }
        applied.push_back({{"name", l.name}, {"kind", "convection"}, {"h", h}, {"ambient_C", sink}, {"faces_area_mm2", [&] {
                             double A = 0;
                             for (const auto& f : fs) A += f.area;
                             return A;
                           }()}});
      } else if (l.kind == "radiation") {
        const auto fs = load_faces(l, true);
        for (const auto& f : fs) rads.push_back({f, d.value("emissivity", mats[size_t(f.body)].emissivity), d.value("ambient", ambient)});
        applied.push_back({{"name", l.name}, {"kind", "radiation"}});
      } else if (l.kind == "fan") {
        FanRun fr;
        fr.load = li;
        fr.fan = air::fan_from(d.value("fan", json("80x25")));
        fr.count = d.value("count", 1);
        fr.inlet = d.value("ambient", ambient);
        const size_t b = body_of(l);
        const V way = unit(d.value("vector", V{1, 0, 0}));
        const auto fins = air::fin_array(world[b], way);
        if (!fins)
          throw Error("fan \"" + l.name + "\": \"" + scene.node(bodies[b])->name +
                      "\" has no plate fins along the air's way (vector): give a convection with h forced and the air's velocity instead");
        fr.fins = *fins;
        const air::FinArray fa = fr.fins;
        fr.Q = air::operating_point(fr.fan, [&](double Q) { return air::channel(fa, Q, fr.inlet).dp; }, fr.count);
        if (fr.Q <= 0) throw Error("fan \"" + l.name + "\" cannot push air through the heatsink's fins");
        // The heatsink's outer faces the air washes: all but those facing back from the fins (the base's underside).
        for (size_t t = 0; t < mesh.tris.size(); ++t)
          if (const auto f = skin_face(t); f && f->body == int(b) && dot(f->normal, fa.up) > -0.5) {
            fr.films.push_back(films.size());
            films.push_back({*f, 10.0, fr.inlet, int(li)});
          }
        fan_runs.push_back(fr);
      }
    }
    // The air solved around the parts: a film on every outer face, from it.
    // With the air solved, radiation (cfd.radiation, default on) between every outer face and to the room: by rays
    // (sim/radiation.hpp, each face's sink from the others' temperatures, per solve), or "calculix" for CalculiX's own cavity
    // radiation (its view factors and the dense system they make solved inside every solve: minutes for a few thousand faces).
    const json rad_mode = air_films ? st.value("cfd", json::object()).value("radiation", json(true)) : json(false);
    const bool cavity = rad_mode.is_string() && rad_mode.get<std::string>() == "calculix";
    const bool by_rays = !cavity && (rad_mode.is_boolean() ? rad_mode.get<bool>() : rad_mode.is_string() && rad_mode.get<std::string>() == "rays");
    if (rad_mode.is_string() && !cavity && !by_rays) throw Error("cfd.radiation: true, false, \"rays\" or \"calculix\"");
    std::vector<radiation::Surface> surfaces;  // by rays: one per entry of rads, in its order
    if (air_films)
      for (size_t t = 0; t < mesh.tris.size(); ++t)
        if (const auto f = skin_face(t)) {
          films.push_back({*f, 10.0, ambient, -1});
          if (cavity || by_rays) rads.push_back({*f, mats[size_t(f->body)].emissivity, ambient, cavity});
          if (by_rays) {
            radiation::Surface s;
            for (int k = 0; k < 3; ++k) s.corners[size_t(k)] = mesh.nodes[size_t(mesh.tris[t][size_t(k)])];
            // The corners counter-clockwise seen along the outward normal.
            if (dot(cross(sub(s.corners[1], s.corners[0]), sub(s.corners[2], s.corners[0])), f->normal) < 0) std::swap(s.corners[1], s.corners[2]);
            s.normal = f->normal;
            s.area = f->area;
            s.emissivity = mats[size_t(f->body)].emissivity;
            surfaces.push_back(s);
          }
        }
    radiation::ViewFactors views;
    json radiation_info;
    if (by_rays) {
      report(0.28, "Radiation: view factors");
      const auto started = std::chrono::steady_clock::now();
      views = radiation::view_factors(surfaces, st.value("cfd", json::object()).value("radiation_rays", 512));
      radiation_info = {{"model", "rays"}, {"surfaces", surfaces.size()}, {"rays", views.rays},
                              {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
    } else if (cavity) {
      radiation_info = {{"model", "calculix"}, {"surfaces", rads.size()}};
    }
    if (films.empty() && rads.empty() && fixed_temps.str().empty())
      throw Error("the heat has nowhere to go: add a convection, a radiation, a fan or a fixed temperature");

    // One solve: the input with the films as they are now, the nodal temperatures back (the last frame).
    auto input = [&](bool steady) {
      std::ostringstream in;
      in.precision(10);
      in << "*HEADING\nOPAD thermal study, case " << load_case << "\n*NODE, NSET=NALL\n";
      for (size_t i = 0; i < mesh.nodes.size(); ++i) in << i + 1 << ", " << mesh.nodes[i][0] << ", " << mesh.nodes[i][1] << ", " << mesh.nodes[i][2] << "\n";
      for (size_t i = 0; i < bodies.size(); ++i) {
        in << "*ELEMENT, TYPE=" << (tn == 10 ? "C3D10" : "C3D4") << ", ELSET=B" << i + 1 << "\n";
        for (size_t e = 0; e < mesh.tets.size(); ++e) {
          if (solid_body[size_t(mesh.tet_solid[e])] != int(i)) continue;
          in << e + 1;
          for (int k = 0; k < tn; ++k) in << ", " << mesh.tets[e][size_t(k)] + 1;
          in << "\n";
        }
      }
      // mm, s, t, N: conductivity W/m.K as it is, specific heat J/kg.K x 1e6, density t/mm3.
      for (size_t i = 0; i < bodies.size(); ++i) {
        in << "*MATERIAL, NAME=M" << i + 1;
        if (mats[i].k_through > 0) {
          // Along the board in its local x and y, across it in z (the orientation below).
          in << "\n*CONDUCTIVITY, TYPE=ORTHO\n" << mats[i].k << ", " << mats[i].k << ", " << mats[i].k_through << "\n";
        } else {
          in << "\n*CONDUCTIVITY\n" << mats[i].k << "\n";
        }
        in << "*SPECIFIC HEAT\n" << mats[i].cp * 1e6 << "\n*DENSITY\n" << mats[i].rho * 1e-9 << "\n";
        if (mats[i].k_through > 0) {
          const V n = unit(mats[i].normal);
          const V a = unit(cross(n, std::fabs(n[0]) < 0.9 ? V{1, 0, 0} : V{0, 1, 0})), b = cross(n, a);
          in << "*ORIENTATION, NAME=OB" << i + 1 << ", SYSTEM=RECTANGULAR\n" << a[0] << ", " << a[1] << ", " << a[2] << ", " << b[0] << ", " << b[1] << ", " << b[2] << "\n";
          in << "*SOLID SECTION, ELSET=B" << i + 1 << ", MATERIAL=M" << i + 1 << ", ORIENTATION=OB" << i + 1 << "\n";
        } else {
          in << "*SOLID SECTION, ELSET=B" << i + 1 << ", MATERIAL=M" << i + 1 << "\n";
        }
      }
      if (!rads.empty()) in << "*PHYSICAL CONSTANTS, ABSOLUTE ZERO=-273.15, STEFAN BOLTZMANN=5.670E-11\n";
      in << "*INITIAL CONDITIONS, TYPE=TEMPERATURE\nNALL, " << ambient << "\n";
      if (steady) {
        in << "*STEP, INC=1000\n*HEAT TRANSFER, STEADY STATE\n1., 1.\n";
      } else {
        const double duration = st.value("duration", 60.0);
        const int frames = std::clamp(st.value("frames", 61), 2, 2000) - 1;
        in << "*STEP, INC=100000\n*HEAT TRANSFER, DIRECT\n" << duration / frames << ", " << duration << "\n";
      }
      if (!fixed_temps.str().empty()) in << "*BOUNDARY\n" << fixed_temps.str();
      if (!fluxes.str().empty()) in << "*DFLUX\n" << fluxes.str();
      if (!films.empty()) {
        in << "*FILM\n";
        for (const auto& f : films) in << f.f.elem + 1 << ", F" << f.f.face << ", " << f.sink << ", " << f.h * 1e-3 << "\n";  // W/m2K -> mW/mm2K
      }
      if (!rads.empty()) {
        in << "*RADIATE\n";
        for (const auto& r : rads) in << r.f.elem + 1 << ", R" << r.f.face << (r.cavity ? "CR" : "") << ", " << r.sink << ", " << r.eps << "\n";
      }
      in << "*NODE FILE\nNT\n*END STEP\n";
      return in.str();
    };
    auto temps_of = [&](const Frd& frd, std::vector<std::vector<double>>* frames, std::vector<double>* times) {
      std::vector<double> T(mesh.nodes.size(), ambient);
      for (const auto& b : frd.blocks) {
        if (b.name != "NDTEMP") continue;
        std::vector<double> t(mesh.nodes.size(), ambient);
        for (const auto& [node, v] : b.values)
          if (node >= 1 && size_t(node) <= t.size() && !v.empty()) t[size_t(node - 1)] = v[0];
        if (frames) frames->push_back(t), times->push_back(b.value);
        T = std::move(t);
      }
      return T;
    };
    // A face's mean temperature: on a quadratic (6-node) triangle the mean of its mid-side nodes, which is exact for the
    // quadratic field (the corners weigh nothing); the corners' mean put a box's 5 W at 5.5 W summed over its faces.
    auto face_temp = [&](const Face& f, const std::vector<double>& T) {
      double s = 0;
      const int first = mesh.tri_nodes == 6 ? 3 : 0;
      for (int k = first; k < first + 3; ++k) s += T[size_t(mesh.tris[f.tri][size_t(k)])];
      return s / 3;
    };
    // The films from the temperatures: natural convection per plate, each fan's operating point, channels and air.
    int air_pass = 0;
    double film_mismatch = 0;  // with the air solved: the films against what the air took, as a share of it
    auto update = [&](const std::vector<double>& T) {
      if (by_rays) {
        std::vector<double> Ts(rads.size());
        for (size_t i = 0; i < rads.size(); ++i) Ts[i] = face_temp(rads[i].f, T);
        const std::vector<double> sink = radiation::sinks(views, surfaces, Ts, ambient);
        for (size_t i = 0; i < rads.size(); ++i) rads[i].sink = sink[i];
      }
      if (air_films) {
        std::vector<AirFace> faces;
        faces.reserve(films.size());
        for (const auto& fl : films) faces.push_back({fl.f.centre, fl.f.normal, fl.f.area, fl.f.body, face_temp(fl.f, T), fl.h, fl.sink});
        (*air_films)(faces, air_pass++);
        // How far the films just solved with were from what the air took at those temperatures: the passes are done only
        // when they agree (walls held at a temperature never move, so the temperatures alone settled at once while the
        // films gave the air two and a half times what it took).
        double off = 0, took = 0;
        for (size_t i = 0; i < films.size(); ++i) {
          const double A = films[i].f.area * 1e-6;
          off += A * std::fabs(films[i].h * (faces[i].T - films[i].sink) - faces[i].q_air);
          took += A * std::fabs(faces[i].q_air);
        }
        film_mismatch = took > 0 ? off / took : 0;
        for (size_t i = 0; i < films.size(); ++i) films[i].h = faces[i].h, films[i].sink = faces[i].sink;
        return;
      }
      for (size_t p = 0; p < plates.size(); ++p) {
        const Plate& pl = plates[p];
        double A = 0, Ts = 0;
        for (size_t i : pl.films) A += films[i].f.area, Ts += films[i].f.area * face_temp(films[i].f, T);
        Ts /= A;
        const double sink = films[pl.films.front()].sink;
        const double h = air::natural_h(pl.up, pl.L * 1e-3, Ts, sink, pl.gap * 1e-3);
        for (size_t i : pl.films) films[i].h = h;
      }
      for (auto& fr : fan_runs) {
        // Heat the air took last time, upstream of each face, warms it on the way: the sink there.
        const V way = fr.fins.flow;
        const air::Air a0 = air::properties(fr.inlet);
        const double mdot_cp = a0.rho * fr.Q * a0.cp;
        double mean_air = fr.inlet + 0.5 * fr.rise;
        fr.ch = air::channel(fr.fins, fr.Q, mean_air);
        std::vector<std::pair<double, size_t>> order;
        for (size_t i : fr.films) order.push_back({dot(films[i].f.centre, way), i});
        std::sort(order.begin(), order.end());
        double upstream = 0;
        fr.heat = 0;
        for (const auto& [x, i] : order) {
          Film& f = films[i];
          const double q = f.h * f.f.area * 1e-6 * (face_temp(f.f, T) - f.sink);  // W, as last solved
          f.h = fr.ch.h;
          f.sink = fr.inlet + (upstream + 0.5 * std::max(0.0, q)) / mdot_cp;
          upstream += std::max(0.0, q);
        }
        fr.heat = upstream;
        fr.rise = upstream / mdot_cp;
      }
    };
    report(0.3, "Solving (CalculiX)");
    std::vector<double> T(mesh.nodes.size(), ambient + 20);  // a first guess for the films that depend on it
    const bool coupled = !plates.empty() || !fan_runs.empty() || air_films;
    int iterations = 0;
    double change = 0;
    json changes = json::array();
    if (air_films) update(T);
    else if (coupled) update(T), update(T);  // the second pass spreads the guessed heat along the fans' air
    const double settle = air_films ? st.value("cfd", json::object()).value("settle", 0.1) : 0.05;
    // With the air solved, the films must also give the air what it takes (cfd.agree, a share of the heat).
    const double agree = st.value("cfd", json::object()).value("agree", 0.02);
    json mismatches = json::array();
    double solved_films = 0, solved_rads = 0;
    bool settled = !coupled;
    for (int it = 0; it < (coupled ? (air_films ? st.value("cfd", json::object()).value("passes", 30) : 12) : 1); ++it) {
      const Frd frd = run_ccx(input(true), 0.3 + 0.05 * std::min(it, 10));
      const std::vector<double> next = temps_of(frd, nullptr, nullptr);
      change = 0;
      for (size_t i = 0; i < next.size(); ++i) change = std::max(change, std::fabs(next[i] - T[i]));
      T = next;
      ++iterations;
      changes.push_back(change);
      // The heat as this solve had it (its own films and sinks): what goes to the air and what is radiated.
      solved_films = solved_rads = 0;
      for (const auto& f : films) solved_films += f.h * f.f.area * 1e-6 * (face_temp(f.f, T) - f.sink);
      for (const auto& r : rads) {
        const double Ts = face_temp(r.f, T) + 273.15, Ta = r.sink + 273.15;
        solved_rads += r.eps * 5.670e-8 * r.f.area * 1e-6 * (Ts * Ts * Ts * Ts - Ta * Ta * Ta * Ta);
      }
      if (!coupled) break;
      if (!air_films && change < settle) {
        settled = true;
        break;
      }
      update(T);
      if (air_films) {
        mismatches.push_back(film_mismatch);
        if (change < settle && film_mismatch < agree) {
          settled = true;
          break;
        }
      }
    }
    if (!settled && change >= settle) run.warnings.push_back("the temperatures were still moving by " + std::to_string(change) + " degC after " + std::to_string(iterations) + " solves");
    else if (!settled) run.warnings.push_back("the parts and the air still disagreed by " + std::to_string(int(std::lround(100 * film_mismatch))) +
                                              " % of the heat after " + std::to_string(iterations) + " solves (cfd.passes)");
    // Over time: the films of the steady state, from the ambient temperature.
    std::vector<std::vector<double>> frames;
    std::vector<double> times;
    if (transient) {
      report(0.85, "Solving over time (CalculiX)");
      T = temps_of(run_ccx(input(false), 0.85), &frames, &times);
    }

    // ---- results
    report(0.95, "Reading the results");
    auto res = std::make_shared<FeaResult>();
    res->kind = kind;
    res->nodes = mesh.nodes;
    res->bodies = bodies;
    res->mesh_size = maxh;
    res->elements = mesh.tets.size();
    for (const auto& t : mesh.tets) res->tets.push_back({t[0], t[1], t[2], t[3]});
    build_skin(*res);
    res->temperature = T;
    res->temperature_frames = frames;
    res->displacement.assign(mesh.nodes.size(), V{0, 0, 0});
    json summary;
    summary["case"] = load_case;
    summary["mesh_size"] = maxh;
    summary["ambient_C"] = ambient;
    size_t hot = 0, cold = 0;
    for (size_t i = 0; i < T.size(); ++i) {
      if (T[i] > T[hot]) hot = i;
      if (T[i] < T[cold]) cold = i;
    }
    summary["max_temperature_C"] = T[hot];
    summary["max_temperature_at"] = mesh.nodes[hot];
    summary["min_temperature_C"] = T[cold];
    // Per body: its hottest point and its mean (by volume).
    json per_body = json::object();
    {
      std::vector<double> peak(bodies.size(), -1e300), sum(bodies.size(), 0.0);
      std::vector<size_t> at(bodies.size(), 0);
      for (size_t e = 0; e < mesh.tets.size(); ++e) {
        const size_t b = size_t(solid_body[size_t(mesh.tet_solid[e])]);
        double m = 0;
        for (int k = 0; k < 4; ++k) m += T[size_t(mesh.tets[e][size_t(k)])] / 4;
        sum[b] += m * tet_volume(e);
        for (int k = 0; k < tn; ++k)
          if (const size_t n = size_t(mesh.tets[e][size_t(k)]); T[n] > peak[b]) peak[b] = T[n], at[b] = n;
      }
      for (size_t b = 0; b < bodies.size(); ++b) {
        per_body[scene.node(bodies[b])->name] = {{"max_temperature_C", peak[b]}, {"mean_temperature_C", sum[b] / body_volume[b]}, {"at", mesh.nodes[at[b]]},
                                                 {"material", mats[b].name}, {"conductivity_W_mK", mats[b].k}};
        if (mats[b].k_through > 0) per_body[scene.node(bodies[b])->name]["conductivity_through_W_mK"] = mats[b].k_through;
      }
    }
    summary["bodies"] = per_body;
    // Where the heat goes: each convection and radiation, the rest into the fixed temperatures.
    double to_air = 0, radiated = 0;
    json by_load = json::object();
    for (const auto& f : films) {
      const double q = f.h * f.f.area * 1e-6 * (face_temp(f.f, T) - f.sink);
      to_air += q;
      const std::string by = f.load < 0 ? std::string("the air") : loads[size_t(f.load)]->name;
      by_load[by] = by_load.value(by, 0.0) + q;
      json& b = summary["bodies"][scene.node(bodies[size_t(f.f.body)])->name];  // each part's share
      b["to_air_W"] = b.value("to_air_W", 0.0) + q;
    }
    for (const auto& r : rads) {
      const double Ts = face_temp(r.f, T) + 273.15, Ta = r.sink + 273.15;
      radiated += r.eps * 5.670e-8 * r.f.area * 1e-6 * (Ts * Ts * Ts * Ts - Ta * Ta * Ta * Ta);
    }
    // Faces that see each other trade most of what they radiate: what leaves for the room is the rest of the heat (steady).
    if (cavity && fixed_temps.str().empty() && !transient) radiated = heat_in - to_air;
    summary["heat_W"] = heat_in;
    summary["to_air_W"] = to_air;
    if (!rads.empty()) summary["radiated_W"] = radiated;
    if (!radiation_info.is_null()) summary["radiation"] = radiation_info;
    if (air_films) summary["solved_balance_W"] = {{"heat", heat_in}, {"to_air", solved_films}, {"radiated", solved_rads}};
    if (!fixed_temps.str().empty() && !transient) summary["to_fixed_temperatures_W"] = heat_in - to_air - radiated;
    summary["loads_W"] = by_load;
    json conv = json::array();
    for (size_t p = 0; p < plates.size(); ++p) {
      const Plate& pl = plates[p];
      conv.push_back({{"load", loads[plate_load[p]]->name}, {"h_W_m2K", films[pl.films.front()].h}, {"length_mm", pl.L}, {"facing_up", pl.up},
                      {"gap_mm", pl.gap}});
    }
    if (!conv.empty()) summary["natural_convection"] = conv;
    json fans_json = json::array();
    for (const auto& fr : fan_runs) {
      const Load& l = *loads[fr.load];
      const double peak = per_body[scene.node(l.refs.front().body)->name]["max_temperature_C"].get<double>();
      fans_json.push_back({{"name", l.name},
                           {"fan", fr.fan.to_json()},
                           {"fans", fr.count},
                           {"heatsink", scene.node(l.refs.front().body)->name},
                           {"fins", fr.fins.to_json()},
                           {"flow_m3h", fr.Q * 3600},
                           {"flow_cfm", fr.Q / 4.719474e-4},
                           {"pressure_Pa", air::channel(fr.fins, fr.Q, fr.inlet).dp},
                           {"channel_velocity_m_s", fr.ch.V},
                           {"reynolds", fr.ch.Re},
                           {"h_W_m2K", fr.ch.h},
                           {"inlet_C", fr.inlet},
                           {"air_rise_C", fr.rise},
                           {"heat_W", fr.heat},
                           {"thermal_resistance_C_W", fr.heat > 0 ? (peak - fr.inlet) / fr.heat : 0.0}});
    }
    if (!fans_json.empty()) summary["fans"] = fans_json;
    summary["loads"] = applied;
    summary["solves"] = iterations;
    summary["ccx_seconds"] = ccx_seconds;
    if (changes.size() > 1) summary["solve_changes_C"] = changes;
    if (!mismatches.empty()) summary["air_disagreement"] = mismatches;  // per pass: the films against what the air took
    summary["nodes"] = mesh.nodes.size();
    summary["elements"] = mesh.tets.size();
    if (transient) {
      run.t = times;
      // Each body's hottest point over time.
      for (size_t b = 0; b < bodies.size(); ++b) {
        Series s{bodies[b], scene.node(bodies[b])->name + " max temperature", "degC", "temperature", {}};
        for (const auto& fr : frames) {
          double m = -1e300;
          for (size_t e = 0; e < mesh.tets.size(); ++e)
            if (solid_body[size_t(mesh.tet_solid[e])] == int(b))
              for (int k = 0; k < tn; ++k) m = std::max(m, fr[size_t(mesh.tets[e][size_t(k)])]);
          s.v.push_back(m);
        }
        run.series.push_back(std::move(s));
      }
      summary["duration_s"] = times.empty() ? 0.0 : times.back();
    } else {
      run.t = {0.0};
    }
    summary["warnings"] = run.warnings;
    run.summary = summary;
    run.fea = res;
    return run;
  }

  // ---- the input file
  report(0.3, "Writing the CalculiX input");
  std::ostringstream inp;
  inp.precision(10);
  inp << "*HEADING\nOPAD " << kind << " study, case " << load_case << "\n*NODE, NSET=NALL\n";
  for (size_t i = 0; i < nodes_out.size(); ++i) inp << i + 1 << ", " << nodes_out[i][0] << ", " << nodes_out[i][1] << ", " << nodes_out[i][2] << "\n";
  for (size_t b = 0; b < bolts.size(); ++b) inp << bolt_node0 + int(b) << ", " << bolts[b].at.X() << ", " << bolts[b].at.Y() << ", " << bolts[b].at.Z() << "\n";
  std::vector<Mat> mats;
  for (size_t i = 0; i < bodies.size(); ++i) {
    mats.push_back(material_for(doc, scene, bodies[i], st.value("materials", json())));
    if (printed[i]) {  // the filament's, printed (its regions' materials are written below)
      const Filament& f = printed[i]->material;
      mats.back().E = f.E, mats.back().nu = f.nu, mats.back().rho = f.density, mats.back().yield = f.X, mats.back().assumed = false;
    }
    if (mats.back().assumed) run.warnings.push_back("\"" + scene.node(bodies[i])->name + "\" has no material with elastic properties: steel assumed");
  }
  for (size_t i = 0; i < bodies.size(); ++i) {
    inp << "*ELEMENT, TYPE=" << (tn == 10 ? "C3D10" : "C3D4") << ", ELSET=B" << i + 1 << "\n";
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
      if (solid_body[size_t(mesh.tet_solid[e])] != int(i)) continue;
      inp << e + 1;
      for (int k = 0; k < tn; ++k) inp << ", " << tets_out[e][size_t(k)] + 1;
      inp << "\n";
    }
  }
  inp << "*ELSET, ELSET=EALL\n";
  for (size_t i = 0; i < bodies.size(); ++i) inp << "B" << i + 1 << "\n";
  // Printed bodies: an orthotropic material per region, a section per region and road direction with its axes.
  std::vector<std::array<Ortho, 3>> pmats(bodies.size());
  for (size_t i = 0; i < bodies.size(); ++i) {
    if (!printed[i]) {
      inp << "*MATERIAL, NAME=M" << i + 1 << "\n*ELASTIC\n" << mats[i].E << ", " << mats[i].nu << "\n*DENSITY\n" << mats[i].rho * 1e-9 << "\n";
      inp << "*SOLID SECTION, ELSET=B" << i + 1 << ", MATERIAL=M" << i + 1 << "\n";
      continue;
    }
    mats[i].name = printed[i]->material.name + " (printed)";
    mats[i].assumed = false;
    std::map<std::pair<int, int>, std::vector<size_t>> groups;  // (region, bin) -> elements
    for (size_t e = 0; e < mesh.tets.size(); ++e)
      if (solid_body[size_t(mesh.tet_solid[e])] == int(i)) groups[{pel[e].region, pel[e].bin}].push_back(e);
    for (int r = 0; r < 3; ++r) {
      const Ortho m = region_material(*printed[i], Region(r));
      pmats[i][size_t(r)] = m;
      inp << "*MATERIAL, NAME=P" << i + 1 << "R" << r << "\n*ELASTIC, TYPE=ENGINEERING CONSTANTS\n" << m.E1 << ", " << m.E2 << ", " << m.E3 << ", "
          << m.nu12 << ", " << m.nu13 << ", " << m.nu23 << ", " << m.G12 << ", " << m.G13 << "\n" << m.G23 << "\n*DENSITY\n" << m.density * 1e-9 << "\n";
    }
    for (const auto& [key, elems] : groups) {
      const std::string set = "P" + std::to_string(i + 1) + "R" + std::to_string(key.first) + "D" + std::to_string(key.second);
      inp << "*ELSET, ELSET=" << set << "\n";
      for (size_t n = 0; n < elems.size(); ++n) inp << elems[n] + 1 << ((n + 1) % 16 == 0 || n + 1 == elems.size() ? "\n" : ", ");
      const auto& ax = paxes[elems.front()];
      inp << "*ORIENTATION, NAME=O" << set << ", SYSTEM=RECTANGULAR\n" << ax[0][0] << ", " << ax[0][1] << ", " << ax[0][2] << ", " << ax[1][0] << ", "
          << ax[1][1] << ", " << ax[1][2] << "\n";
      inp << "*SOLID SECTION, ELSET=" << set << ", MATERIAL=P" << i + 1 << "R" << key.first << ", ORIENTATION=O" << set << "\n";
    }
  }
  // Supports.
  std::ostringstream boundary, dload;
  std::vector<std::string> fixed_sets;
  bool supported = false;
  std::map<int, V> forces;  // node -> force N
  json applied = json::array();
  int set_no = 0;
  for (const Load* l : loads) {
    const std::string set = "S" + std::to_string(++set_no);
    if (l->kind == "fixed" || l->kind == "displacement") {
      const auto tris = triangles_of(*l);
      const auto nodes = nodes_of(tris);
      inp << "*NSET, NSET=" << set << "\n";
      int n = 0;
      for (int k : nodes) inp << k + 1 << (++n % 12 ? ", " : ",\n");
      inp << "\n";
      if (l->kind == "fixed") {
        boundary << set << ", 1, 3\n";
        fixed_sets.push_back(set);
        supported = true;
      } else {
        const V d = l->def.value("vector", V{0, 0, 0});
        for (int k = 0; k < 3; ++k) boundary << set << ", " << k + 1 << ", " << k + 1 << ", " << d[size_t(k)] << "\n";
        fixed_sets.push_back(set);
        supported = true;
      }
      applied.push_back({{"name", l->name}, {"kind", l->kind}, {"nodes", nodes.size()}});
    } else if (l->kind == "force" || l->kind == "pressure" || l->kind == "moment") {
      const auto tris = triangles_of(*l);
      double area = 0;
      std::vector<Tri> info;
      for (size_t t : tris) info.push_back(tri_info(t)), area += info.back().area;
      if (l->kind == "force") {
        const V F = l->def.value("vector", V{0, 0, 0});
        for (const auto& ti : info)
          for (const auto& [node, w] : ti.weights) forces[node] = add(forces[node], mul(F, ti.area / area * w));
      } else if (l->kind == "pressure") {
        const double p = l->def.value("value", 0.0);
        for (const auto& ti : info)
          for (const auto& [node, w] : ti.weights) forces[node] = add(forces[node], mul(ti.normal, -p * ti.area * w));
      } else {
        // A moment as the linear force field about the faces' centre whose resultant moment is M.
        const V M = l->def.value("vector", V{0, 0, 0});
        const V axis = unit(M);
        std::map<int, double> weight;
        V c{0, 0, 0};
        for (const auto& ti : info)
          for (const auto& [node, w] : ti.weights) weight[node] += ti.area * w;
        double total = 0;
        for (const auto& [node, w] : weight) c = add(c, mul(mesh.nodes[size_t(node)], w)), total += w;
        c = mul(c, 1 / total);
        double J = 0;
        for (const auto& [node, w] : weight) {
          V r = sub(mesh.nodes[size_t(node)], c);
          r = sub(r, mul(axis, dot(r, axis)));
          J += w * dot(r, r);
        }
        if (J <= 0) throw Error("moment \"" + l->name + "\": its faces have no extent across the moment's axis");
        for (const auto& [node, w] : weight) {
          V r = sub(mesh.nodes[size_t(node)], c);
          r = sub(r, mul(axis, dot(r, axis)));
          forces[node] = add(forces[node], mul(cross(M, r), w / J));
        }
      }
      applied.push_back({{"name", l->name}, {"kind", l->kind}, {"faces_area_mm2", area}});
    } else if (l->kind == "gravity") {
      const V g = l->def.value("vector", V{0, 0, -9810});
      const double mag = norm(g);
      if (mag > 0) dload << "EALL, GRAV, " << mag << ", " << g[0] / mag << ", " << g[1] / mag << ", " << g[2] / mag << "\n";
      applied.push_back({{"name", l->name}, {"kind", l->kind}});
    }
  }
  // Nothing held: the least that keeps it still (3-2-1 on three far nodes), which carries no load when the loads balance.
  if (!supported) {
    size_t a = 0, b = 0, c = 0;
    double farthest = 0;
    for (size_t i = 0; i < mesh.nodes.size(); ++i)
      if (norm(sub(mesh.nodes[i], mesh.nodes[0])) > farthest) farthest = norm(sub(mesh.nodes[i], mesh.nodes[0])), a = i;
    farthest = 0;
    for (size_t i = 0; i < mesh.nodes.size(); ++i)
      if (norm(sub(mesh.nodes[i], mesh.nodes[a])) > farthest) farthest = norm(sub(mesh.nodes[i], mesh.nodes[a])), b = i;
    farthest = 0;
    const V ab = unit(sub(mesh.nodes[b], mesh.nodes[a]));
    for (size_t i = 0; i < mesh.nodes.size(); ++i) {
      V r = sub(mesh.nodes[i], mesh.nodes[a]);
      r = sub(r, mul(ab, dot(r, ab)));
      if (norm(r) > farthest) farthest = norm(r), c = i;
    }
    // a: all three; b: the two across ab; c: the one out of the abc plane.
    const V n = unit(cross(ab, sub(mesh.nodes[c], mesh.nodes[a])));
    int keep_b = std::fabs(ab[0]) > std::fabs(ab[1]) && std::fabs(ab[0]) > std::fabs(ab[2]) ? 1 : std::fabs(ab[1]) > std::fabs(ab[2]) ? 2 : 3;
    int keep_c = std::fabs(n[0]) > std::fabs(n[1]) && std::fabs(n[0]) > std::fabs(n[2]) ? 1 : std::fabs(n[1]) > std::fabs(n[2]) ? 2 : 3;
    boundary << a + 1 << ", 1, 3\n";
    for (int k = 1; k <= 3; ++k)
      if (k != keep_b) boundary << b + 1 << ", " << k << ", " << k << "\n";
    boundary << c + 1 << ", " << keep_c << ", " << keep_c << "\n";
    if (kind == "static") run.warnings.push_back("nothing holds the bodies: three nodes hold them still (3-2-1); if the loads do not balance, add a fixed support");
  }
  inp << equations.str();
  inp << "*BOUNDARY\n" << boundary.str();
  if (kind == "static") {
    inp << "*STEP\n*STATIC\n";
    if (!forces.empty() || !cload.str().empty()) {
      inp << "*CLOAD\n" << cload.str();
      for (const auto& [node, f] : forces)
        for (int k = 0; k < 3; ++k)
          if (f[size_t(k)] != 0) inp << node + 1 << ", " << k + 1 << ", " << f[size_t(k)] << "\n";
    }
    if (!dload.str().empty()) inp << "*DLOAD\n" << dload.str();
    inp << "*NODE FILE\nU\n*EL FILE\nS\n";
    for (const auto& s : fixed_sets) inp << "*NODE PRINT, NSET=" << s << ", TOTALS=ONLY\nRF\n";
    for (size_t i = 0; i < bodies.size(); ++i)  // printed bodies: each element's own stresses for its strength
      if (printed[i]) inp << "*EL PRINT, ELSET=B" << i + 1 << "\nS\n";
    inp << "*END STEP\n";
  } else {
    const int modes = std::clamp(st.value("modes", 6), 1, 100);
    inp << "*STEP\n*FREQUENCY\n" << modes << "\n*NODE FILE\nU\n*END STEP\n";
  }

  // ---- run
  const Frd frd = run_ccx(inp.str(), 0.35);

  // ---- results
  auto res = std::make_shared<FeaResult>();
  res->kind = kind;
  res->nodes = mesh.nodes;
  res->bodies = bodies;
  res->mesh_size = maxh;
  res->elements = mesh.tets.size();
  for (const auto& t : mesh.tets) res->tets.push_back({t[0], t[1], t[2], t[3]});
  build_skin(*res);
  const size_t nn = mesh.nodes.size();
  auto field = [&](const Frd::Block& blk, size_t width) {
    std::vector<std::vector<double>> out(nn, std::vector<double>(width, 0.0));
    for (const auto& [node, v] : blk.values)
      if (node >= 1 && size_t(node) <= nn)
        for (size_t k = 0; k < width && k < v.size(); ++k) out[size_t(node - 1)][k] = v[k];
    return out;
  };
  json summary;
  if (kind == "static") {
    const Frd::Block* disp = nullptr;
    const Frd::Block* stress = nullptr;
    for (const auto& b : frd.blocks) {
      if (b.name == "DISP" && !disp) disp = &b;
      if (b.name == "STRESS" && !stress) stress = &b;
    }
    if (!disp || !stress) throw Error("CalculiX wrote no displacements or stresses");
    const auto d = field(*disp, 3), s = field(*stress, 6);
    res->displacement.resize(nn);
    res->stress.resize(nn);
    res->von_mises.resize(nn);
    for (size_t i = 0; i < nn; ++i) {
      res->displacement[i] = {d[i][0], d[i][1], d[i][2]};
      for (size_t k = 0; k < 6; ++k) res->stress[i][k] = s[i][k];
      res->von_mises[i] = von_mises(res->stress[i]);
    }
    // Peaks, overall and per body.
    size_t vm_at = 0, d_at = 0;
    for (size_t i = 0; i < nn; ++i) {
      if (res->von_mises[i] > res->von_mises[vm_at]) vm_at = i;
      if (norm(res->displacement[i]) > norm(res->displacement[d_at])) d_at = i;
    }
    summary["max_von_mises_MPa"] = res->von_mises[vm_at];
    summary["max_von_mises_at"] = res->nodes[vm_at];
    summary["max_displacement_mm"] = norm(res->displacement[d_at]);
    summary["max_displacement_at"] = res->nodes[d_at];
    summary["max_displacement"] = res->displacement[d_at];
    std::vector<double> body_peak(bodies.size(), 0.0);
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
      const int b = solid_body[size_t(mesh.tet_solid[e])];
      for (int k = 0; k < tn; ++k) body_peak[size_t(b)] = std::max(body_peak[size_t(b)], res->von_mises[size_t(mesh.tets[e][size_t(k)])]);
    }
    // Printed bodies: each element's failure index in its road axes (failure()), the worst per body and what governs.
    std::vector<double> body_fi(bodies.size(), 0.0);
    std::vector<std::string> body_mode(bodies.size());
    std::vector<size_t> body_fi_at(bodies.size(), 0);
    if (std::any_of(printed.begin(), printed.end(), [](const auto& p) { return p.has_value(); })) {
      // Each element's index from its own stresses at its integration points (node-averaged stresses would mix a stiff
      // skin's into the infill next to it); at a node, the worst of the elements around it.
      res->failure_index.assign(nn, 0.0);
      const auto points = read_point_stresses(dir.p / "job.dat");
      for (size_t e = 0; e < mesh.tets.size(); ++e) {
        if (pel[e].region < 0) continue;
        const size_t b = size_t(solid_body[size_t(mesh.tet_solid[e])]);
        const Ortho& m = pmats[b][size_t(pel[e].region)];
        Failure worst;
        if (const auto it = points.find(int(e) + 1); it != points.end()) {
          for (const auto& st : it->second)  // in the element's orientation: its region's axes already
            if (const Failure f = failure(st, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, m); f.index > worst.index) worst = f;
        } else {
          for (int k = 0; k < tn; ++k)
            if (const Failure f = failure(res->stress[size_t(mesh.tets[e][size_t(k)])], paxes[e][0], paxes[e][1], paxes[e][2], m); f.index > worst.index) worst = f;
        }
        size_t at = size_t(mesh.tets[e][0]);
        for (int k = 0; k < tn; ++k) {
          const size_t node = size_t(mesh.tets[e][size_t(k)]);
          res->failure_index[node] = std::max(res->failure_index[node], worst.index);
          if (res->von_mises[node] > res->von_mises[at]) at = node;
        }
        if (worst.index > body_fi[b]) {
          body_fi[b] = worst.index, body_fi_at[b] = at;
          body_mode[b] = worst.mode + " (" + region_name(Region(pel[e].region)) + ")";
        }
      }
    }
    json per_body = json::object();
    double worst_sf = 1e300;
    for (size_t i = 0; i < bodies.size(); ++i) {
      if (printed[i]) {
        const double sf = body_fi[i] > 0 ? 1 / std::sqrt(body_fi[i]) : 1e300;
        per_body[scene.node(bodies[i])->name] = {{"max_von_mises_MPa", body_peak[i]}, {"material", mats[i].name}, {"criterion", "Tsai-Hill in the layer, quadratic between layers (printed)"},
                                                 {"safety_factor", sf < 1e299 ? json(sf) : json(nullptr)}, {"fails", body_mode[i]},
                                                 {"weakest_at", res->nodes[body_fi_at[i]]}};
        worst_sf = std::min(worst_sf, sf);
        continue;
      }
      const double sf = body_peak[i] > 0 ? mats[i].yield / body_peak[i] : 1e300;
      per_body[scene.node(bodies[i])->name] = {{"max_von_mises_MPa", body_peak[i]}, {"material", mats[i].name}, {"yield_MPa", mats[i].yield},
                                               {"safety_factor", sf < 1e299 ? json(sf) : json(nullptr)}};
      worst_sf = std::min(worst_sf, sf);
    }
    summary["bodies"] = per_body;
    if (worst_sf < 1e299) summary["min_safety_factor"] = worst_sf;
    const auto reactions = read_reactions(dir.p / "job.dat");
    json rj = json::object();
    for (size_t k = 0; k < fixed_sets.size(); ++k)
      if (const auto it = reactions.find(fixed_sets[k]); it != reactions.end()) {
        // the set's load name
        int counter = 0;
        for (const Load* l : loads)
          if ("S" + std::to_string(++counter) == fixed_sets[k]) rj[l->name] = it->second;
      }
    if (!rj.empty()) summary["reactions_N"] = rj;
    // Bolts: the axial stress across the shank half way between the cut and the bolt's ends is what the preload makes.
    for (auto& bj : bolt_info) {
      const V n = bj["axis"].get<V>(), at = bj["at"].get<V>();
      const std::string bolt = bj["bolt"].get<std::string>();
      double sum = 0, count = 0;
      for (size_t e = 0; e < mesh.tets.size(); ++e) {
        const int b = solid_body[size_t(mesh.tet_solid[e])];
        if (scene.node(bodies[size_t(b)])->name != bolt) continue;
        for (int k = 0; k < 4; ++k) {
          const size_t node = size_t(mesh.tets[e][size_t(k)]);
          const double along = dot(sub(res->nodes[node], at), n);
          if (std::fabs(along) < 1.5 * maxh || std::fabs(along) > 2.5 * maxh) continue;  // a slice two elements off the cut
          const auto& s = res->stress[node];
          const double axial = n[0] * n[0] * s[0] + n[1] * n[1] * s[1] + n[2] * n[2] * s[2] + 2 * (n[0] * n[1] * s[3] + n[1] * n[2] * s[4] + n[2] * n[0] * s[5]);
          sum += axial, count += 1;
        }
      }
      if (count > 0) bj["axial_stress_MPa"] = sum / count;
    }
    if (!bolt_info.empty()) summary["bolts"] = bolt_info;
  } else {
    res->frequencies = read_frequencies(dir.p / "job.dat");
    for (const auto& b : frd.blocks) {
      if (b.name != "DISP" || !b.modal) continue;
      const auto d = field(b, 3);
      std::vector<V> shape(nn);
      for (size_t i = 0; i < nn; ++i) shape[i] = {d[i][0], d[i][1], d[i][2]};
      res->modes.push_back(std::move(shape));
    }
    if (res->frequencies.empty()) throw Error("CalculiX found no natural frequencies");
    summary["frequencies_Hz"] = res->frequencies;
    if (!supported) summary["note"] = "free: the first six modes are the rigid-body motions (about 0 Hz)";
  }
  // Printed bodies: the settings used, each region's volume, material and the plastic it takes, and where it is weakest.
  json print_json = json::object();
  for (size_t i = 0; i < bodies.size(); ++i) {
    if (!printed[i]) continue;
    double vol[3] = {0, 0, 0}, lowest = 1e300, highest = -1e300;
    for (size_t e = 0; e < mesh.tets.size(); ++e) {
      if (solid_body[size_t(mesh.tet_solid[e])] != int(i) || pel[e].region < 0) continue;
      const V a = mesh.nodes[size_t(mesh.tets[e][0])], b = mesh.nodes[size_t(mesh.tets[e][1])], c = mesh.nodes[size_t(mesh.tets[e][2])],
              d = mesh.nodes[size_t(mesh.tets[e][3])];
      vol[pel[e].region] += std::fabs(dot(sub(b, a), cross(sub(c, a), sub(d, a)))) / 6;
      for (const V* q : {&a, &b, &c, &d}) lowest = std::min(lowest, dot(*q, printed[i]->up)), highest = std::max(highest, dot(*q, printed[i]->up));
    }
    json regions = json::object();
    double mass = 0;
    for (int r = 0; r < 3; ++r) {
      const Ortho& m = pmats[i][size_t(r)];
      mass += vol[r] * m.density / 1000;  // mm3 * g/cm3 -> g
      regions[region_name(Region(r))] = {{"volume_mm3", vol[r]}, {"E_MPa", {m.E1, m.E2, m.E3}}, {"strength_MPa", {m.X, m.Y, m.Z}}, {"density", m.density}};
    }
    json pj = printed[i]->to_json();
    pj["regions"] = regions;
    pj["printed_mass_g"] = mass;
    if (kind == "static" && !res->failure_index.empty()) {
      double fi = 0;
      size_t at = 0;
      for (size_t e = 0; e < mesh.tets.size(); ++e)
        if (solid_body[size_t(mesh.tet_solid[e])] == int(i))
          for (int k = 0; k < tn; ++k)
            if (res->failure_index[size_t(mesh.tets[e][size_t(k)])] > fi) fi = res->failure_index[size_t(mesh.tets[e][size_t(k)])], at = size_t(mesh.tets[e][size_t(k)]);
      if (fi > 0) {
        pj["min_safety_factor"] = 1 / std::sqrt(fi);
        pj["weakest_at"] = res->nodes[at];
        pj["weakest_height_mm"] = dot(res->nodes[at], printed[i]->up) - lowest;  // above the bed (the body's lowest point)
        const int layers = std::max(1, int(std::ceil((highest - lowest) / printed[i]->layer - 1e-6)));
        pj["weakest_layer"] = std::clamp(int((dot(res->nodes[at], printed[i]->up) - lowest) / printed[i]->layer) + 1, 1, layers);
        if (summary.contains("bodies") && summary["bodies"].contains(scene.node(bodies[i])->name)) pj["fails"] = summary["bodies"][scene.node(bodies[i])->name]["fails"];
      }
    }
    print_json[scene.node(bodies[i])->name] = pj;
  }
  if (!print_json.empty()) summary["print"] = print_json;
  summary["nodes"] = nn;
  summary["elements"] = mesh.tets.size();
  summary["element_type"] = tn == 10 ? "C3D10 (quadratic tetrahedra)" : "C3D4 (linear tetrahedra)";
  summary["mesh_size_mm"] = maxh;
  summary["loads"] = applied;
  json mat_json = json::object();
  for (size_t i = 0; i < bodies.size(); ++i)
    mat_json[scene.node(bodies[i])->name] = {{"material", mats[i].name}, {"E_MPa", mats[i].E}, {"nu", mats[i].nu}, {"density", mats[i].rho}};
  summary["materials"] = mat_json;
  if (!run.warnings.empty()) summary["warnings"] = run.warnings;
  if (dir.keep) summary["files"] = path_to_utf8(dir.p);
  for (const auto& [k, v] : summary.items()) run.summary[k] = v;
  // Frames: static has one (the loaded state), modal one per mode; the parts stay where they are.
  if (kind == "static") run.t = {0.0};
  else run.t.assign(res->frequencies.begin(), res->frequencies.end());
  run.fea = res;
  report(1, "Done");
  return run;
}

}  // namespace opad::sim
