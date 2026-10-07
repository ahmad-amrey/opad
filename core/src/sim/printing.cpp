#include "opad/sim/printing.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepOffsetAPI_MakeOffsetShape.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>

#include "../zip.hpp"

namespace opad::sim {

namespace {
constexpr double kPi = 3.14159265358979323846;

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

Vec3 normalized(Vec3 v) {
  const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (n < 1e-12) throw Error("print: the build direction must not be zero");
  return {v[0] / n, v[1] / n, v[2] / n};
}

// The first number of a slicer value: "0.45", "20%", "[45,135]", "0.4,0.4", ["PLA"]. Percent stays as written.
std::string first_value(const json& v) {
  if (v.is_array()) return v.empty() ? std::string() : first_value(v[0]);
  if (v.is_number()) return v.dump();
  if (!v.is_string()) return {};
  std::string s = trim(v.get<std::string>());
  if (!s.empty() && (s.front() == '[' || s.front() == '"')) s.erase(0, 1);
  const size_t cut = s.find_first_of(",;]\"");
  if (cut != std::string::npos) s = s.substr(0, cut);
  return trim(s);
}

bool number(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end != s.c_str();
}
}  // namespace

// ---------------------------------------------------------------- filaments
// Typical values of printed specimens (along-road and between-layer tensile tests in manufacturers' data sheets and the
// literature); a printer's own differ, especially between layers: override them from your test bars.
const std::vector<Filament>& filaments() {
  static const std::vector<Filament> list = {
      {"pla", "PLA", 3000, 0.35, 0.90, 0.85, 55, 42, 32, 30, 1.24, ""},
      {"petg", "PETG", 2000, 0.38, 0.90, 0.85, 45, 35, 25, 25, 1.27, ""},
      {"abs", "ABS", 2100, 0.35, 0.85, 0.80, 38, 30, 20, 22, 1.04, "bonds between layers weakly when cooled fast"},
      {"asa", "ASA", 2000, 0.35, 0.85, 0.80, 40, 32, 22, 24, 1.07, ""},
      {"pc", "PC", 2300, 0.37, 0.90, 0.85, 60, 45, 35, 32, 1.20, ""},
      {"pa", "PA (nylon, dry)", 1600, 0.40, 0.90, 0.85, 50, 40, 30, 28, 1.12, "absorbs moisture: wet nylon is up to half as stiff"},
      {"pla-cf", "PLA-CF", 5000, 0.33, 0.60, 0.50, 60, 38, 25, 26, 1.29, "fibres lie along the roads"},
      {"petg-cf", "PETG-CF", 4500, 0.35, 0.60, 0.50, 55, 35, 22, 25, 1.30, "fibres lie along the roads"},
      {"pa-cf", "PA-CF", 7000, 0.35, 0.45, 0.35, 95, 45, 30, 30, 1.18, "fibres lie along the roads; dry"},
      {"tpu", "TPU 95A", 26, 0.45, 0.95, 0.90, 30, 25, 20, 15, 1.21, "rubbery: linear only for small strains"},
  };
  return list;
}

const Filament* filament(const std::string& key) {
  std::string k = lower(trim(key));
  for (char& c : k)
    if (c == '_' || c == ' ') c = '-';
  static const std::vector<std::pair<std::string, std::string>> aliases = {
      {"pla+", "pla"}, {"pla-plus", "pla"}, {"pla-silk", "pla"}, {"pet", "petg"}, {"pa6", "pa"}, {"pa12", "pa"}, {"nylon", "pa"},
      {"pa6-cf", "pa-cf"}, {"paht-cf", "pa-cf"}, {"pa12-cf", "pa-cf"}, {"pet-cf", "petg-cf"}, {"flex", "tpu"}, {"tpu-95a", "tpu"}};
  for (const auto& [a, id] : aliases)
    if (k == a) k = id;
  for (const auto& f : filaments())
    if (f.id == k || lower(f.name) == k) return &f;
  return nullptr;
}

// ---------------------------------------------------------------- settings
double PrintSettings::fill() const { return std::min(1.0, flow * (1 - (1 - kPi / 4) * std::min(layer, line) / line)); }

json PrintSettings::to_json() const {
  json j = {{"material", material.name}, {"build_direction", up}, {"layer_height", layer}, {"line_width", line}, {"flow", flow},
            {"walls", walls}, {"top_layers", top}, {"bottom_layers", bottom}, {"infill", density * 100}, {"pattern", pattern},
            {"model", family}, {"infill_angle", angle}, {"road_fill", fill()}};
  if (!source.empty()) j["profile"] = source;
  return j;
}

std::string infill_family(const std::string& pattern) {
  std::string p = lower(trim(pattern));
  p.erase(std::remove_if(p.begin(), p.end(), [](char c) { return c == '_' || c == '-' || c == ' '; }), p.end());
  static const std::vector<std::pair<std::string, std::vector<std::string>>> table = {
      {"lines", {"rectilinear", "alignedrectilinear", "monotonic", "monotonicline", "line", "lines", "zigzag", "hilbertcurve", "archimedeanchords", "octagramspiral"}},
      {"grid", {"grid", "crosshatch", "cross"}},
      {"triangles", {"triangles", "trihexagon", "stars"}},
      {"honeycomb", {"honeycomb"}},
      {"cubic", {"cubic", "adaptivecubic", "quartercubic", "cubicsubdiv", "tetrahedral", "3dhoneycomb", "cross3d", "octet"}},
      {"gyroid", {"gyroid"}},
      {"lightning", {"lightning", "supportcubic"}},
      {"concentric", {"concentric"}},
  };
  for (const auto& [family, names] : table)
    if (std::find(names.begin(), names.end(), p) != names.end()) return family;
  return {};
}

// ---------------------------------------------------------------- profiles
namespace {
// key = value lines (PrusaSlicer .ini and the settings a slicer appends to its G-code as "; key = value"); Cura's
// [values] section the same way.
void ini_pairs(const std::string& text, json& raw) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::string l = trim(line);
    while (!l.empty() && l.front() == ';') l = trim(l.substr(1));
    if (l.empty() || l.front() == '[' || l.front() == '#') continue;
    const size_t eq = l.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(l.substr(0, eq));
    if (key.empty() || key.find(' ') != std::string::npos) continue;
    raw[key] = trim(l.substr(eq + 1));
  }
}

json normalise(const json& raw) {
  auto get = [&](std::initializer_list<const char*> keys) -> std::string {
    for (const char* k : keys)
      if (raw.contains(k)) {
        const std::string v = first_value(raw[k]);
        if (!v.empty()) return v;
      }
    return {};
  };
  json out = json::object();
  double v = 0;
  double nozzle = 0.4;
  if (number(get({"nozzle_diameter", "machine_nozzle_size"}), v) && v > 0) nozzle = v;
  if (number(get({"perimeters", "wall_loops", "wall_line_count"}), v)) out["walls"] = int(std::lround(v));
  if (number(get({"top_solid_layers", "top_shell_layers", "top_layers"}), v)) out["top_layers"] = int(std::lround(v));
  if (number(get({"bottom_solid_layers", "bottom_shell_layers", "bottom_layers"}), v)) out["bottom_layers"] = int(std::lround(v));
  if (number(get({"layer_height"}), v) && v > 0) out["layer_height"] = v;
  const std::string width = get({"perimeter_extrusion_width", "inner_wall_line_width", "wall_line_width", "line_width", "extrusion_width"});
  if (number(width, v)) {
    if (width.find('%') != std::string::npos) v = v / 100 * nozzle;  // a share of the nozzle
    if (v <= 0) v = 1.125 * nozzle;                                   // 0: the slicer's automatic width
    out["line_width"] = v;
  }
  const std::string fill = get({"fill_density", "sparse_infill_density", "infill_sparse_density"});
  if (number(fill, v)) out["infill"] = (fill.find('%') == std::string::npos && v <= 1 && fill.find('.') != std::string::npos) ? v * 100 : v;
  const std::string pattern = get({"fill_pattern", "sparse_infill_pattern", "infill_pattern"});
  if (!pattern.empty()) out["pattern"] = pattern;
  if (number(get({"fill_angle", "infill_direction", "infill_angles"}), v)) out["infill_angle"] = v;
  const std::string flow = get({"extrusion_multiplier", "filament_flow_ratio", "material_flow"});
  if (number(flow, v) && v > 0) out["flow"] = (v > 5 ? v / 100 : v);  // Cura gives a percentage
  const std::string type = get({"filament_type", "material_type"});
  if (!type.empty() && filament(type)) out["material"] = type;
  return out;
}

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw Error("print: cannot read the profile " + path_to_utf8(p));
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}
}  // namespace

json read_profile(const std::filesystem::path& file) {
  std::string ext = lower(file.extension().string());
  const std::string data = read_file(file);
  json raw = json::object();
  if (ext == ".3mf") {
    try {
      const detail::Zip zip(data);
      if (zip.has("Metadata/project_settings.config")) {  // OrcaSlicer, Bambu Studio: JSON
        raw = json::parse(zip.read("Metadata/project_settings.config"));
      } else if (zip.has("Metadata/Slic3r_PE.config")) {  // PrusaSlicer: "; key = value"
        ini_pairs(zip.read("Metadata/Slic3r_PE.config"), raw);
      } else {
        throw Error("print: " + path_to_utf8(file.filename()) + " holds no PrusaSlicer, OrcaSlicer or Bambu Studio settings");
      }
    } catch (const json::exception& e) {
      throw Error(std::string("print: the 3MF's settings are not valid JSON: ") + e.what());
    }
  } else if (ext == ".json") {
    try {
      raw = json::parse(data);
    } catch (const json::exception& e) {
      throw Error(std::string("print: ") + path_to_utf8(file.filename()) + " is not valid JSON: " + e.what());
    }
    if (!raw.is_object()) throw Error("print: " + path_to_utf8(file.filename()) + " is not a slicer profile");
  } else {
    ini_pairs(data, raw);  // .ini, .cfg, .inst.cfg, .gcode
  }
  json out = normalise(raw);
  if (out.empty()) throw Error("print: no slicer settings found in " + path_to_utf8(file.filename()));
  return out;
}

PrintSettings print_settings(const json& print) {
  if (!print.is_object()) throw Error("print: the settings are an object (see the agent guide, Printed parts)");
  json j = json::object();
  if (print.contains("profile")) {
    j = read_profile(path_from_utf8(print["profile"].get<std::string>()));
    j["source"] = print["profile"];
  }
  for (const auto& [k, v] : print.items())
    if (k != "profile" && k != "bodies") j[k] = v;
  PrintSettings s;
  if (j.contains("source")) s.source = j["source"].get<std::string>();
  // The filament, then any of its values overridden (from your own test bars).
  const json m = j.value("material", json("pla"));
  const std::string base = m.is_string() ? m.get<std::string>() : m.value("base", std::string("pla"));
  const Filament* f = filament(base);
  if (!f) {
    std::string names;
    for (const auto& x : filaments()) names += (names.empty() ? "" : ", ") + x.id;
    throw Error("print: no printing material \"" + base + "\" (" + names + "; or {\"base\": ..., \"E\": ..., \"Z\": ...})");
  }
  s.material = *f;
  if (m.is_object()) {
    Filament& x = s.material;
    for (auto [key, field] : std::initializer_list<std::pair<const char*, double*>>{
             {"E", &x.E}, {"nu", &x.nu}, {"kt", &x.kt}, {"kz", &x.kz}, {"X", &x.X}, {"Y", &x.Y}, {"Z", &x.Z}, {"S", &x.S}, {"density", &x.density}})
      if (m.contains(key)) *field = m[key].get<double>();
    if (m.contains("name")) x.name = m["name"].get<std::string>();
    else x.name += " (adjusted)";
  }
  if (j.contains("build_direction")) s.up = normalized(j["build_direction"].get<Vec3>());
  s.layer = j.value("layer_height", s.layer);
  s.line = j.value("line_width", s.line);
  s.flow = j.value("flow", s.flow);
  s.walls = j.value("walls", s.walls);
  s.top = j.value("top_layers", s.top);
  s.bottom = j.value("bottom_layers", s.bottom);
  const json infill = j.contains("infill") ? j["infill"] : j.value("infill_density", json(s.density * 100));
  double pct = 0;
  if (infill.is_number()) pct = infill.get<double>();
  else if (!number(first_value(infill), pct)) throw Error("print: infill is a percentage (0 to 100)");
  s.density = std::clamp(pct / 100, 0.0, 1.0);
  s.pattern = j.value("pattern", s.pattern);
  s.angle = j.value("infill_angle", s.angle);
  s.family = s.density >= 0.99 ? "solid" : infill_family(s.pattern);
  if (s.family.empty())
    throw Error("print: unknown infill pattern \"" + s.pattern + "\" (grid, rectilinear, triangles, honeycomb, cubic, gyroid, lightning, concentric, "
                "or a slicer's name for one of them)");
  if (!(s.layer > 0) || !(s.line > 0) || s.layer > s.line * 1.5) throw Error("print: layer_height and line_width must be positive, the layer no thicker than 1.5 lines");
  if (s.walls < 0 || s.top < 0 || s.bottom < 0) throw Error("print: walls, top_layers and bottom_layers cannot be negative");
  if (!(s.flow > 0)) throw Error("print: flow must be positive");
  return s;
}

const char* region_name(Region r) { return r == Region::Wall ? "wall" : r == Region::TopBottom ? "top/bottom" : "infill"; }

// ---------------------------------------------------------------- materials
namespace {
// Plane-stress reduced stiffness of a ply in its axes, rotated by theta (deg) into the laminate's: Qbar (11 22 12 16 26 66).
std::array<double, 6> qbar(double E1, double E2, double nu12, double G12, double theta) {
  const double nu21 = nu12 * E2 / E1, d = 1 - nu12 * nu21;
  const double Q11 = E1 / d, Q22 = E2 / d, Q12 = nu12 * E2 / d, Q66 = G12;
  const double c = std::cos(theta * kPi / 180), s = std::sin(theta * kPi / 180);
  const double c2 = c * c, s2 = s * s, cs = c * s;
  return {Q11 * c2 * c2 + 2 * (Q12 + 2 * Q66) * s2 * c2 + Q22 * s2 * s2,
          Q11 * s2 * s2 + 2 * (Q12 + 2 * Q66) * s2 * c2 + Q22 * c2 * c2,
          (Q11 + Q22 - 4 * Q66) * s2 * c2 + Q12 * (s2 * s2 + c2 * c2),
          (Q11 - Q12 - 2 * Q66) * c2 * cs + (Q12 - Q22 + 2 * Q66) * s2 * cs,
          (Q11 - Q12 - 2 * Q66) * s2 * cs + (Q12 - Q22 + 2 * Q66) * c2 * cs,
          (Q11 + Q22 - 2 * Q12 - 2 * Q66) * s2 * c2 + Q66 * (s2 * s2 + c2 * c2)};
}

using M3 = std::array<std::array<double, 3>, 3>;
M3 inverse(const M3& m) {
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  M3 r;
  r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
  r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
  r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
  r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
  r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
  r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
  r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
  r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
  r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
  return r;
}

struct Road {  // the deposited plastic in a road's axes (scaled by its fill)
  double E1, E2, E3, nu, G12, G13, G23, X, Y, Z, S;
};

Road road(const PrintSettings& s) {
  const Filament& f = s.material;
  const double phi = s.fill(), G = f.E / (2 * (1 + f.nu));
  return {phi * f.E, phi * f.kt * f.E, phi * f.kz * f.E, f.nu, phi * f.kt * G, phi * f.kz * G, phi * std::min(f.kt, f.kz) * G,
          phi * f.X, phi * f.Y, phi * f.Z, phi * f.S};
}

// Layers of roads alternating at theta and theta + 90 deg (top and bottom skins, solid infill): classical lamination
// theory for the in-plane stiffness, first-ply failure (Tsai-Hill in each ply) for its in-plane strengths.
Ortho laminate(const Road& r, double theta, double density) {
  const auto qa = qbar(r.E1, r.E2, r.nu, r.G12, theta), qb = qbar(r.E1, r.E2, r.nu, r.G12, theta + 90);
  std::array<double, 6> A;
  for (int i = 0; i < 6; ++i) A[size_t(i)] = 0.5 * (qa[size_t(i)] + qb[size_t(i)]);
  const M3 Am = {{{A[0], A[2], A[3]}, {A[2], A[1], A[4]}, {A[3], A[4], A[5]}}};
  const M3 a = inverse(Am);
  // The in-plane stress a ply takes under a laminate stress, and its Tsai-Hill index.
  auto strength = [&](const std::array<double, 3>& load) {
    std::array<double, 3> eps{};
    for (int i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k) eps[size_t(i)] += a[size_t(i)][size_t(k)] * load[size_t(k)];
    double worst = 0;
    for (const double th : {theta, theta + 90}) {
      const auto q = qbar(r.E1, r.E2, r.nu, r.G12, th);
      const double sx = q[0] * eps[0] + q[2] * eps[1] + q[3] * eps[2];
      const double sy = q[2] * eps[0] + q[1] * eps[1] + q[4] * eps[2];
      const double txy = q[3] * eps[0] + q[4] * eps[1] + q[5] * eps[2];
      const double c = std::cos(th * kPi / 180), s = std::sin(th * kPi / 180);
      const double s1 = c * c * sx + s * s * sy + 2 * c * s * txy, s2 = s * s * sx + c * c * sy - 2 * c * s * txy;
      const double t12 = -c * s * sx + c * s * sy + (c * c - s * s) * txy;
      worst = std::max(worst, s1 * s1 / (r.X * r.X) - s1 * s2 / (r.X * r.X) + s2 * s2 / (r.Y * r.Y) + t12 * t12 / (r.S * r.S));
    }
    return worst > 0 ? 1 / std::sqrt(worst) : 1e30;
  };
  Ortho o;
  o.E1 = 1 / a[0][0], o.E2 = 1 / a[1][1], o.E3 = r.E3;
  o.nu12 = -a[0][1] / a[0][0], o.nu13 = r.nu, o.nu23 = r.nu;
  o.G12 = 1 / a[2][2], o.G13 = o.G23 = 0.5 * (r.G13 + r.G23);
  o.X = strength({1, 0, 0}), o.Y = strength({0, 1, 0}), o.Z = r.Z;
  o.S12 = strength({0, 0, 1}), o.S13 = o.S23 = std::min(r.S, r.Z);
  o.density = density;
  return o;
}

// Poisson ratios kept inside what an orthotropic material allows (|nu_ij| < sqrt(E_i / E_j)), with room to spare.
void admissible(Ortho& o) {
  auto clamp = [](double& nu, double Ei, double Ej) { nu = std::min(nu, 0.4 * std::sqrt(Ei / Ej)); };
  clamp(o.nu12, o.E1, o.E2), clamp(o.nu13, o.E1, o.E3), clamp(o.nu23, o.E2, o.E3);
}
}  // namespace

Ortho region_material(const PrintSettings& s, Region region) {
  const Road r = road(s);
  const double rho_plastic = s.material.density;
  if (region == Region::Wall) {
    Ortho o{r.E1, r.E2, r.E3, r.nu, r.nu, r.nu, r.G12, r.G13, r.G23, r.X, r.Y, r.Z, r.S, std::min(r.S, r.Z), std::min(r.S, r.Z), s.fill() * rho_plastic};
    admissible(o);
    return o;
  }
  if (region == Region::TopBottom || s.family == "solid") {
    // Region axes: 1 along the infill angle's base (the region's own x), roads at theta and theta + 90 in it. The skins'
    // first layer runs at the infill angle, as the slicers lay them.
    Ortho o = laminate(r, region == Region::TopBottom ? s.angle : 0, s.fill() * rho_plastic);
    admissible(o);
    return o;
  }
  // The infill: relative density rho (its share of solid plastic) in the infill's axes (1 at the infill angle). E, G, X,
  // Y, Z, S: the filament's (a road's along and between layers). Walls that run along a load stretch (their share times the
  // plastic's stiffness and strength); walls loaded across bend (rho^3 stiffness, rho^2 strength); lines stacked
  // crosswise touch only where they cross (rho^2 through the layers).
  const Filament& f = s.material;
  const double rho = s.density * s.fill(), E = f.E, G = f.E / (2 * (1 + f.nu)), kz = f.kz, X = f.X, Z = f.Z, S = f.S, SZ = std::min(f.S, f.Z);
  const double r2 = rho * rho, r3 = r2 * rho, r15 = rho * std::sqrt(rho);
  Ortho o{};
  o.density = rho * rho_plastic;
  const std::string& fam = s.family;
  if (fam == "lines") {
    o = {rho / 2 * E, rho / 2 * E, r2 * kz * E, 0.05, 0.05, 0.05, r3 * E / 16, r2 * kz * G, r2 * kz * G, rho / 2 * X, rho / 2 * X, r2 * Z, 0.2 * r2 * S, r2 * SZ, r2 * SZ, o.density};
  } else if (fam == "grid") {
    // A square grid of walls t thick, a apart: rho = 2 t/a - (t/a)^2, and the walls along a load are t/a of it.
    const double w = 1 - std::sqrt(1 - rho);
    o = {w * E, w * E, rho * kz * E, 0.05, 0.05, 0.05, w * w * w * E / 2, rho / 2 * kz * G, rho / 2 * kz * G, w * X, w * X, rho * Z, 0.2 * r2 * S, rho / 2 * SZ, rho / 2 * SZ, o.density};
  } else if (fam == "triangles") {
    o = {rho / 3 * E, rho / 3 * E, rho * kz * E, 1.0 / 3, 0.1, 0.1, rho / 8 * E, rho / 2 * kz * G, rho / 2 * kz * G, rho / 3 * X, rho / 3 * X, rho * Z, rho / 4 * S, rho / 2 * SZ, rho / 2 * SZ, o.density};
  } else if (fam == "honeycomb") {
    o = {1.5 * r3 * E, 1.5 * r3 * E, rho * kz * E, 0.9, 0.1, 0.1, 0.37 * r3 * E, 0.5 * rho * kz * G, 0.5 * rho * kz * G, 0.5 * r2 * X, 0.5 * r2 * X, rho * Z, 0.3 * r2 * S, 0.5 * rho * SZ, 0.5 * rho * SZ, o.density};
  } else if (fam == "cubic") {
    const double e = 0.25 * rho * E;
    o = {e, e, e * std::sqrt(kz), 0.25, 0.25, 0.25, 0.4 * e, 0.4 * e * std::sqrt(kz), 0.4 * e * std::sqrt(kz), 0.25 * rho * X, 0.25 * rho * X, 0.25 * rho * Z, 0.2 * rho * S, 0.2 * rho * SZ, 0.2 * rho * SZ, o.density};
  } else if (fam == "gyroid") {
    const double e = 0.45 * r15 * E;
    o = {e, e, e * kz, 0.3, 0.3, 0.3, e / 2.6, e * kz / 2.6, e * kz / 2.6, 0.4 * r15 * X, 0.4 * r15 * X, 0.4 * r15 * Z, 0.3 * r15 * S, 0.3 * r15 * SZ, 0.3 * r15 * SZ, o.density};
  } else if (fam == "lightning") {
    const double e = 0.01 * rho * E;
    o = {e, e, e, 0.1, 0.1, 0.1, 0.4 * e, 0.4 * e, 0.4 * e, 0.01 * rho * X, 0.01 * rho * X, 0.01 * rho * Z, 0.01 * rho * S, 0.01 * rho * SZ, 0.01 * rho * SZ, o.density};
  } else if (fam == "concentric") {  // rings along the contour, stacked: 1 along the contour (oriented like the walls)
    o = {rho * E, r3 * E / 16, rho * kz * E, 0.05, 0.1, 0.1, r3 * E / 16, rho / 2 * kz * G, 0.2 * r2 * G, rho * X, 0.3 * r2 * f.Y, rho * Z, 0.2 * r2 * S, rho / 2 * SZ, 0.2 * r2 * SZ, o.density};
  } else {
    throw Error("print: no infill model for \"" + fam + "\"");
  }
  // Floors: an empty-ish infill keeps a stiffness the solver can take (a millionth of the plastic's).
  for (double* v : {&o.E1, &o.E2, &o.E3, &o.G12, &o.G13, &o.G23}) *v = std::max(*v, 1e-6 * E);
  for (double* v : {&o.X, &o.Y, &o.Z, &o.S12, &o.S13, &o.S23}) *v = std::max(*v, 1e-6 * X);
  admissible(o);
  return o;
}

Failure failure(const std::array<double, 6>& st, const Vec3& a1, const Vec3& a2, const Vec3& a3, const Ortho& m) {
  // The stress tensor in the region's axes: s_ij = a_i . S . a_j.
  const double S[3][3] = {{st[0], st[3], st[5]}, {st[3], st[1], st[4]}, {st[5], st[4], st[2]}};
  const Vec3* ax[3] = {&a1, &a2, &a3};
  double l[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double v = 0;
      for (int p = 0; p < 3; ++p)
        for (int q = 0; q < 3; ++q) v += (*ax[i])[size_t(p)] * S[p][q] * (*ax[j])[size_t(q)];
      l[i][j] = v;
    }
  const double s1 = l[0][0], s2 = l[1][1], s3 = l[2][2], t12 = l[0][1], t13 = l[0][2], t23 = l[1][2];
  const double iX = 1 / (m.X * m.X), iY = 1 / (m.Y * m.Y), iZ = 1 / (m.Z * m.Z);
  Failure f;
  f.index = s1 * s1 * iX + s2 * s2 * iY + s3 * s3 * iZ - s1 * s2 * (iX + iY - iZ) - s1 * s3 * (iX + iZ - iY) - s2 * s3 * (iY + iZ - iX) +
            t12 * t12 / (m.S12 * m.S12) + t13 * t13 / (m.S13 * m.S13) + t23 * t23 / (m.S23 * m.S23);
  f.index = std::max(f.index, 0.0);
  const double parts[4] = {std::fabs(s1) / m.X, std::fabs(s2) / m.Y, std::fabs(s3) / m.Z,
                           std::max({std::fabs(t12) / m.S12, std::fabs(t13) / m.S13, std::fabs(t23) / m.S23})};
  static const char* names[4] = {"along the roads", "across the roads", "between layers", "shear"};
  f.mode = names[std::max_element(parts, parts + 4) - parts];
  return f;
}

// ---------------------------------------------------------------- the infill region
PrintRegions print_regions(const TopoDS_Shape& body, const PrintSettings& s) {
  const double tw = s.wall_thickness(), tt = s.top_thickness(), tb = s.bottom_thickness();
  auto volume = [](const TopoDS_Shape& x) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(x, g);
    return g.Mass();
  };
  try {
    TopoDS_Shape inner = body;
    if (tw > 0) {
      // Walls are horizontal (a slicer offsets each layer's outline): faces looking up or down move in only as far as the
      // thinner skin, which the shifted copies below then set exactly. Falls back to an even offset where that fails.
      const double flat = std::max(0.0, std::min({tw, tt > 0 ? tt : tw, tb > 0 ? tb : tw}));
      bool done = false;
      if (flat < tw - 1e-9) {
        BRepOffset_MakeOffset off;
        off.Initialize(body, -tw, 1e-4, BRepOffset_Skin, false, false, GeomAbs_Intersection);
        for (TopExp_Explorer e(body, TopAbs_FACE); e.More(); e.Next()) {
          const TopoDS_Face face = TopoDS::Face(e.Current());
          BRepAdaptor_Surface sf(face);
          if (sf.GetType() != GeomAbs_Plane) continue;
          const gp_Dir n = sf.Plane().Axis().Direction();
          if (std::fabs(n.X() * s.up[0] + n.Y() * s.up[1] + n.Z() * s.up[2]) > 0.999) off.SetOffsetOnFace(face, -std::max(flat, 1e-3));
        }
        off.MakeOffsetShape();
        if (off.IsDone() && !off.Shape().IsNull() && TopExp_Explorer(off.Shape(), TopAbs_SOLID).More()) inner = off.Shape(), done = true;
      }
      if (!done) {
        BRepOffsetAPI_MakeOffsetShape off;
        off.PerformByJoin(body, -tw, 1e-4, BRepOffset_Skin, false, false, GeomAbs_Intersection);
        if (!off.IsDone()) {
          off.PerformBySimple(body, -tw);
          if (!off.IsDone()) throw Error("print: the walls (" + std::to_string(tw) + " mm) could not be offset into this body");
        }
        inner = off.Shape();
      }
    }
    auto shifted = [&](double d) {
      gp_Trsf t;
      t.SetTranslation(gp_Vec(s.up[0] * d, s.up[1] * d, s.up[2] * d));
      return BRepBuilderAPI_Transform(body, t, true).Shape();
    };
    // Infill: under top_thickness of material above it and over bottom_thickness below it, as a slicer finds per layer;
    // the same cut of the whole body is what is neither top nor bottom skin.
    const double whole = volume(body);
    auto solid = [&](const TopoDS_Shape& x) { return !x.IsNull() && TopExp_Explorer(x, TopAbs_SOLID).More() && volume(x) >= 1e-6 * whole; };
    PrintRegions out;
    TopoDS_Shape core = inner, middle = body;
    if (tt > 0) core = BRepAlgoAPI_Common(core, shifted(-tt)).Shape(), middle = BRepAlgoAPI_Common(middle, shifted(-tt)).Shape();
    if (tb > 0) core = BRepAlgoAPI_Common(core, shifted(tb)).Shape(), middle = solid(middle) ? BRepAlgoAPI_Common(middle, shifted(tb)).Shape() : middle;
    if (solid(core)) out.core = core;
    if (tt > 0 || tb > 0) {
      out.skins = true;
      if (solid(middle)) out.middle = middle;
    }
    return out;
  } catch (const Standard_Failure& e) {
    throw Error(std::string("print: the infill region could not be made: ") + e.GetMessageString());
  }
}

TopoDS_Shape print_core(const TopoDS_Shape& body, const PrintSettings& s) { return print_regions(body, s).core; }

}  // namespace opad::sim
