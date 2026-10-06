// Material library (TODO 11 UI-140): built-in materials, the names they go by, what a node is made of and its mass.
#include "opad/materials.hpp"

#include <BRep_Builder.hxx>
#include <OSD_Parallel.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>

#include "opad/geometry.hpp"
#include "opad/mass.hpp"

namespace opad {
namespace {

// Lower case, every run of punctuation one space; bytes of other scripts are kept as letters.
std::string normalised(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c >= 0x80 || std::isalnum(c)) out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
    else if (!out.empty() && out.back() != ' ') out += ' ';
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// One name of a material in a normalised text: "#code" anywhere in the text with the spaces taken out, but not inside a
// longer number ("6061" in "al6061t6", "14301" in "1.4301"); words as whole words, and a plain word also glued to a grade
// ("pa12", "ss304", "almg3").
bool names(const std::string& norm, const std::string& squashed, const std::string& name) {
  if (name[0] == '#') {
    const std::string code = name.substr(1);
    for (size_t at = squashed.find(code); at != std::string::npos; at = squashed.find(code, at + 1)) {
      const size_t end = at + code.size();
      if ((at == 0 || !std::isdigit(static_cast<unsigned char>(squashed[at - 1]))) &&
          (end == squashed.size() || !std::isdigit(static_cast<unsigned char>(squashed[end]))))
        return true;
    }
    return false;
  }
  if ((" " + norm + " ").find(" " + name + " ") != std::string::npos) return true;
  for (char c : name)
    if (!std::isalpha(static_cast<unsigned char>(c))) return false;
  for (size_t start = 0; start < norm.size();) {
    const size_t end = std::min(norm.find(' ', start), norm.size());
    if (end - start > name.size() && norm.compare(start, name.size(), name) == 0 && std::isdigit(static_cast<unsigned char>(norm[start + name.size()])))
      return true;
    start = end + 1;
  }
  return false;
}

}  // namespace

double property_number(const json& v) {
  double d = 0;
  if (v.is_number()) d = v.get<double>();
  else if (v.is_string()) d = std::strtod(v.get_ref<const std::string&>().c_str(), nullptr);
  return std::isfinite(d) && d > 0 ? d : 0;
}

json Material::to_json() const { return {{"id", id}, {"name", name}, {"density", density}, {"color", color}, {"opacity", opacity}}; }

// Densities are typical values for the grade named (g/cm3); colours are display presets, not measured.
const std::vector<Material>& materials() {
  static const std::vector<Material> list = {
      {"steel", "Steel", 7.85, {0.62, 0.63, 0.65}, 1,
       {"steel", "carbon steel", "mild steel", "crs", "spcc", "#s235", "#s275", "#s355", "#c45", "#ck45", "#42crmo4", "#1018", "#1020",
        "#1045", "#4140", "#st37", "#st52", "#dc01"}},
      {"stainless", "Stainless steel", 8.00, {0.75, 0.76, 0.78}, 1,
       {"stainless", "inox", "ss", "sus", "a2", "a4", "#304", "#316", "#14301", "#14401", "#14404", "#x5crni1810"}},
      {"aluminium-6061", "Aluminium 6061", 2.70, {0.82, 0.83, 0.85}, 1,
       {"aluminium", "aluminum", "alu", "al", "almg", "alsi", "almgsi", "#6061", "#6063", "#6082", "#5052", "#5083", "#7075", "#2024"}},
      {"brass", "Brass", 8.50, {0.80, 0.66, 0.33}, 1, {"brass", "cuzn", "#c360", "#c26000", "#ms58"}},
      {"copper", "Copper", 8.96, {0.80, 0.47, 0.33}, 1, {"copper", "cu", "#c101", "#c110"}},
      {"titanium", "Titanium Ti-6Al-4V", 4.43, {0.58, 0.57, 0.55}, 1, {"titanium", "ti", "#ti6al4v"}},
      {"abs", "ABS", 1.04, {0.22, 0.22, 0.23}, 1, {"abs", "acrylonitrile butadiene styrene"}},
      {"pla", "PLA", 1.24, {0.93, 0.93, 0.91}, 1, {"pla", "polylactic acid", "polylactide"}},
      {"petg", "PETG", 1.27, {0.75, 0.85, 0.90}, 0.75, {"petg", "pet g", "polyethylene terephthalate glycol"}},
      {"nylon", "Nylon (PA6)", 1.14, {0.93, 0.92, 0.88}, 1, {"nylon", "polyamide", "pa"}},
      {"nylon-12", "Nylon 12 (PA12)", 1.01, {0.45, 0.46, 0.48}, 1, {"nylon 12", "polyamide 12", "pa 12", "pa 11", "#pa12", "#pa11"}},
      {"polycarbonate", "Polycarbonate", 1.20, {0.88, 0.92, 0.95}, 0.55, {"polycarbonate", "pc"}},
      {"pom", "POM (acetal)", 1.41, {0.96, 0.96, 0.94}, 1, {"pom", "acetal", "polyacetal", "polyoxymethylene"}},
      {"fr4", "FR-4", 1.85, {0.12, 0.42, 0.22}, 1, {"fr4", "fr 4", "pcb", "glass epoxy", "epoxy glass"}},
      {"glass", "Glass", 2.50, {0.82, 0.91, 0.95}, 0.3, {"glass", "soda lime", "borosilicate"}}};
  return list;
}

const Mechanical* mechanical(const std::string& id) {
  // Typical room-temperature values (MatWeb, manufacturers' sheets): steel S235-S355, 304 stainless, 6061-T6, CW614N brass,
  // annealed copper, Ti-6Al-4V, injection-moulded or printed polymers along the print; FR-4 in its plane.
  static const std::map<std::string, Mechanical> table = {
      {"steel", {210000, 0.30, 250}},        {"stainless", {193000, 0.29, 215}}, {"aluminium-6061", {69000, 0.33, 276}},
      {"brass", {100000, 0.34, 200}},        {"copper", {117000, 0.34, 70}},     {"titanium", {114000, 0.34, 880}},
      {"abs", {2300, 0.35, 40}},             {"pla", {3500, 0.36, 50}},          {"petg", {2100, 0.38, 50}},
      {"nylon", {2700, 0.39, 70}},           {"nylon-12", {1600, 0.40, 45}},     {"polycarbonate", {2400, 0.37, 62}},
      {"pom", {2900, 0.35, 65}},             {"fr4", {22000, 0.12, 300}},        {"glass", {70000, 0.22, 33}}};
  const auto it = table.find(id);
  return it == table.end() ? nullptr : &it->second;
}

const Material* material(const std::string& id) {
  for (const auto& m : materials())
    if (m.id == id) return &m;
  return nullptr;
}

const Material* find_material(const std::string& text) {
  const std::string norm = normalised(text);
  if (norm.empty()) return nullptr;
  for (const auto& m : materials())
    if (norm == normalised(m.id) || norm == normalised(m.name)) return &m;
  std::string squashed;
  for (char c : norm)
    if (c != ' ') squashed += c;
  // The more specific first: "FR-4" before glass, "PETG" before PLA, nylon before glass ("glass-filled nylon"), PA12 before
  // PA, stainless before steel.
  static const char* order[] = {"fr4", "petg", "pla", "abs", "nylon-12", "nylon", "polycarbonate", "pom", "stainless", "aluminium-6061",
                                "brass", "copper", "titanium", "steel", "glass"};
  for (const char* id : order) {
    const Material* m = material(id);
    for (const auto& name : m->names)
      if (names(norm, squashed, name)) return m;
  }
  return nullptr;
}

std::string MaterialChoice::shown() const {
  if (!id.empty() && (text.empty() || normalised(text) == normalised(id)))
    if (const Material* m = material(id)) return m->name;
  return text;
}

MaterialChoice material_of(const Document& doc, const Scene& scene, const std::string& node) {
  MaterialChoice c;
  for (const Node* n = scene.node(node); n; n = n->parent.empty() ? nullptr : scene.node(n->parent)) {
    const json& p = n->properties;
    const bool text = p.contains("material") && p["material"].is_string() && !p["material"].get_ref<const std::string&>().empty();
    const double density = property_number(p.value("density", json()));
    if (!text && density <= 0) continue;
    c.from = n->id;
    if (text) c.text = p["material"].get<std::string>();
    c.density = density;
    break;
  }
  if (c.from.empty())
    if (const Node* n = scene.node(node); n && n->kind == Node::Kind::Body)
      if (const BodyEntry* b = doc.body(n->body_key); b && b->meta.contains("material") && b->meta["material"].is_string())
        c.text = b->meta["material"].get<std::string>();
  if (const Material* m = find_material(c.text)) {
    c.id = m->id;
    if (c.density <= 0) c.density = m->density;
  }
  return c;
}

namespace {
struct Volumes {
  std::mutex mu;
  std::unordered_map<std::string, double> by_key;
};
Volumes& volumes() {
  static Volumes v;
  return v;
}
}  // namespace

double body_volume(const Document& doc, const std::string& key) {
  Volumes& cache = volumes();
  {
    std::lock_guard<std::mutex> lock(cache.mu);
    if (const auto it = cache.by_key.find(key); it != cache.by_key.end()) return it->second;
  }
  const TopoDS_Shape shape = body_shape(doc, key);
  TopoDS_Compound solids;
  BRep_Builder builder;
  builder.MakeCompound(solids);
  int count = 0;
  for (TopExp_Explorer e(shape, TopAbs_SOLID); e.More(); e.Next(), ++count) builder.Add(solids, e.Current());
  const double v = count ? std::fabs(volume_properties(solids).mass) : 0;
  std::lock_guard<std::mutex> lock(cache.mu);
  cache.by_key[key] = v;
  return v;
}

void warm_volumes(const Document& doc, const std::vector<std::string>& keys, const std::function<bool()>& cancelled) {
  std::vector<std::string> missing;
  {
    Volumes& cache = volumes();
    std::lock_guard<std::mutex> lock(cache.mu);
    for (const auto& k : keys)
      if (!cache.by_key.count(k) && doc.has_body(k)) missing.push_back(k);
  }
  std::sort(missing.begin(), missing.end());
  missing.erase(std::unique(missing.begin(), missing.end()), missing.end());
  OSD_Parallel::For(0, static_cast<int>(missing.size()), [&](int i) {
    if (cancelled && cancelled()) return;
    try {
      body_volume(doc, missing[static_cast<size_t>(i)]);
    } catch (const std::exception&) {
    } catch (const Standard_Failure&) {
    }
  });
  if (cancelled && cancelled()) throw Error("cancelled");
}

std::optional<double> body_mass(const Document& doc, const Scene& scene, const std::string& node, std::string* why) {
  const auto fail = [&](const std::string& reason) {
    if (why) *why = reason;
    return std::optional<double>();
  };
  const Node* n = scene.node(node);
  if (!n || n->kind != Node::Kind::Body) return fail("not a body");
  if (const double m = property_number(n->properties.value("mass", json())); m > 0) return m;
  if (n->body_missing) return fail("its geometry is missing");
  const MaterialChoice c = material_of(doc, scene, node);
  if (c.density <= 0) return fail(c.text.empty() ? "no material" : "no density for the material '" + c.text + "'");
  const double v = body_volume(doc, n->body_key);
  if (v <= 0) return fail("not a solid");
  const Mat4 w = scene.world(node);
  const double det = w.at(0, 0) * (w.at(1, 1) * w.at(2, 2) - w.at(1, 2) * w.at(2, 1)) - w.at(0, 1) * (w.at(1, 0) * w.at(2, 2) - w.at(1, 2) * w.at(2, 0)) +
                     w.at(0, 2) * (w.at(1, 0) * w.at(2, 1) - w.at(1, 1) * w.at(2, 0));
  return v * std::fabs(det) * c.density / 1000;  // mm3 x g/cm3
}

double mass_unit(const std::string& unit) {
  if (unit == "g") return 1;
  if (unit == "kg") return 1000;
  if (unit == "lb") return 453.59237;
  throw Error("mass unit is g, kg or lb, not '" + unit + "'");
}

}  // namespace opad
