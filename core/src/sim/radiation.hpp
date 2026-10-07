#pragma once
// Radiation between surfaces that see each other (grey, diffuse), the rest of each one's view the room: the view factors cast
// once as rays from every surface, then per solve the radiosities from the surface temperatures and, from them, the light each
// surface gets as the temperature of a black surrounding giving it as much (its sink). A conduction solver then needs only
// radiation to a sink per face (CalculiX's plain R faces, no cavity): a dense view factor system solved inside it took ~4 min
// per solve for 6500 faces; this takes about a second once and milliseconds per solve.
#include <array>
#include <utility>
#include <vector>

#include "opad/util.hpp"

namespace opad::sim::radiation {

struct Surface {
  std::array<Vec3, 3> corners;  // counter-clockwise seen from outside (the normal's side)
  Vec3 normal;                  // outward, unit
  double area = 0;              // mm2 (any unit, as long as all are the same)
  double emissivity = 0.9;
};

struct ViewFactors {
  std::vector<std::vector<std::pair<int, float>>> to;  // per surface: the others it sees and the share of its view each takes
  std::vector<double> room;                            // per surface: the share of its view that leaves the scene
  long long rays = 0;
};

// rays: per surface (cosine-weighted, stratified, the same on every run); threads 0: as many as the machine has.
ViewFactors view_factors(const std::vector<Surface>& surfaces, int rays = 512, int threads = 0);

// The sink temperature (degC) of each surface: (G / sigma)^1/4 with G the light falling on it, from the surfaces at T (degC)
// and the room at room_C. Its net loss is then emissivity * sigma * (T^4 - sink^4) per area.
std::vector<double> sinks(const ViewFactors& vf, const std::vector<Surface>& surfaces, const std::vector<double>& T, double room_C);

}  // namespace opad::sim::radiation
