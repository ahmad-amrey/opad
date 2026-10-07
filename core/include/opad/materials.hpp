#pragma once
// Material library (TODO 11 UI-140): the built-in materials with their densities and appearance, the names STEP files
// and other tools give them, and what a body is made of. A `properties` op sets `material` (a library id or any text)
// and optionally `density` (g/cm3) on a body or a component; the nearest node upwards that sets either decides, else the
// material the body's file named (STEP, glTF, OBJ). Mass = the enclosed volume (mass.cpp) x density.
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "opad/document.hpp"
#include "opad/scene.hpp"

namespace opad {

struct Material {
  std::string id, name;            // "aluminium-6061", "Aluminium 6061"
  double density = 0;              // g/cm3
  std::array<double, 3> color{};   // appearance preset
  double opacity = 1;
  std::vector<std::string> names;  // what it is called: words ("stainless", "aisi 304") and "#" codes ("#6061")
  json to_json() const;            // id, name, density, color, opacity
};
const std::vector<Material>& materials();
// Elastic properties for structural studies (sim/fea.hpp): typical values for the library grade; null for an id it has none for.
struct Mechanical {
  double youngs = 0;   // MPa
  double poisson = 0;
  double yield = 0;    // MPa (tensile strength for brittle materials)
};
const Mechanical* mechanical(const std::string& material_id);
// Thermal properties for thermal studies: typical room-temperature values; null for an id it has none for.
struct Thermal {
  double conductivity = 0;   // W/m.K
  double specific_heat = 0;  // J/kg.K
  double emissivity = 0;     // of a plain surface (bare metal low, painted or anodised high: set it on the radiation load)
};
const Thermal* thermal(const std::string& material_id);
const Material* material(const std::string& id);
// The library material a name stands for: an id, a library name, or what files and tools write ("Aluminum 6061-T6",
// "SS304", "1.4301", "PA12", "Glass-filled nylon", "Plastic - ABS"). Null when nothing fits.
const Material* find_material(const std::string& text);

// What a node is made of. text: as written (a property, or the file's material name); id: the library material it
// stands for (empty when none); from: the node whose properties decided (empty: the body's file); density in g/cm3, 0
// when unknown (a `density` property beats the library's).
struct MaterialChoice {
  std::string text, id, from;
  double density = 0;
  std::string shown() const;  // the library's name when the text is its id (or empty), else the text
};
MaterialChoice material_of(const Document& doc, const Scene& scene, const std::string& node);

// The volume the solids of a body-store entry enclose (mm3; 0 when it has none), cached by key for the process (keys are
// content hashes). Walks the geometry the first time: workers only.
double body_volume(const Document& doc, const std::string& key);
// Measures the volumes of these keys that are not cached yet, side by side; a key that fails is left for body_volume to
// report. Throws Error("cancelled") when `cancelled` says so.
void warm_volumes(const Document& doc, const std::vector<std::string>& keys, const std::function<bool()>& cancelled = {});
// A body node's mass in grams: its `mass` property, else volume x the scale of its placement x density. Empty, with the
// reason in `why`, when there is no density, no solid or no geometry.
std::optional<double> body_mass(const Document& doc, const Scene& scene, const std::string& node, std::string* why = nullptr);
// Grams per unit: g, kg, lb; throws Error for another.
double mass_unit(const std::string& unit);
// A positive number property (density, mass) as a number or as text; 0 when it is neither.
double property_number(const json& value);

}  // namespace opad
