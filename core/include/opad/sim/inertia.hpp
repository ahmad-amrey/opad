#pragma once
// Mass properties of a part (a body, or a component and everything under it) for dynamics: mass, centre of mass and the
// inertia tensor about it, from each body's solids and density (materials.hpp; a body without one is taken as steel and
// said so). Walks the geometry: workers only.
#include <array>
#include <string>
#include <vector>

#include "../document.hpp"
#include "../scene.hpp"

namespace opad::sim {

struct PartMass {
  double mass = 0;                   // kg
  Vec3 centre{0, 0, 0};              // mm, world
  std::array<double, 9> inertia{};   // kg.mm2 about the centre, world axes, row-major
  int bodies = 0;
  std::vector<std::string> notes;    // bodies without a density (steel assumed), without solids
};

PartMass part_mass(const Document& doc, const Scene& scene, const std::string& node, double default_density = 7.85);

}  // namespace opad::sim
