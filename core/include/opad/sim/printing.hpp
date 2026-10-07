#pragma once
// Printed parts in structural studies: a body made by an FFF/FDM printer is not the solid its shape is but walls, top and
// bottom skins and a sparse infill, each laid as roads of plastic whose stiffness and strength differ along a road, across
// it and between layers. From the slicer's settings (walls, top and bottom layers, layer height, line width, infill
// pattern, density and angle, flow) and the filament, a study splits each printed body into its skin and its core
// (print_core) and gives every element the orthotropic material of its region, in that region's axes:
//
//   wall        roads along the part's contour: 1 along the road, 2 across it in the layer, 3 the build direction;
//   top/bottom  layers of roads alternating at the infill angle and 90 deg from it: a two-ply laminate (classical
//               lamination theory), in-plane in 1-2, layers along 3;
//   core        the infill: per-pattern stiffness and strength scaling with its density, from the mechanics of the
//               pattern (walls along the load stretch, others bend), in the infill's axes (1 at the infill angle);
//               100 % infill is the same laminate as the skins.
//
// Every road is a stadium (a rectangle with rounded sides) line_width wide and layer_height high: it fills
// 1 - (1 - pi/4) h / w of its box (times the flow), which scales stiffness, strength and mass, and its flat contact with
// the next layer is what the build-direction properties stand for. Failure (failure()) is Tsai-Hill in the layer and a
// quadratic criterion between layers, with the tensile strengths along the roads, across them and between layers and the
// compressive ones; it also says which governs.
//
// The filament values are typical printed-specimen data (along-road and between-layer tensile tests) and vary with the
// printer, temperature and cooling: `material` overrides any of them, from your own test bars.
#include <TopoDS_Shape.hxx>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "../util.hpp"

namespace opad::sim {

struct Filament {
  std::string id, name;
  double E = 3300;          // MPa: along a road
  double nu = 0.35;
  double kt = 0.9, kz = 0.85;  // stiffness across a road (in the layer) and between layers, as fractions of E
  double X = 55, Y = 40, Z = 30, S = 30;  // tensile strength along a road, across roads, between layers; shear (MPa)
  double C = 70;            // compressive strength (MPa): roads and layers pressed together do not come apart
  double density = 1.24;    // g/cm3, the solid plastic
  std::string note;
};
const std::vector<Filament>& filaments();
const Filament* filament(const std::string& id_or_name);  // by id ("pla"), name or the slicer's type ("PLA", "PA-CF")

struct PrintSettings {
  Filament material;
  Vec3 up{0, 0, 1};          // build direction
  double layer = 0.2, line = 0.45, flow = 1.0;  // mm, mm, extrusion multiplier
  int walls = 2, top = 4, bottom = 4;
  double density = 0.2;      // infill, 0..1
  std::string pattern = "grid";
  std::string family = "grid";  // lines | grid | triangles | honeycomb | cubic | gyroid | lightning | concentric | solid
  double angle = 45;         // deg: infill direction (top and bottom skins: this and 90 deg more)
  std::string source;        // the profile it was read from, if any

  double wall_thickness() const { return walls * line; }
  double top_thickness() const { return top * layer; }
  double bottom_thickness() const { return bottom * layer; }
  double fill() const;       // the fraction of a road's box its stadium section fills, times the flow (<= 1)
  json to_json() const;
};

// A slicer's settings as `print` keys: PrusaSlicer .ini, OrcaSlicer / Bambu Studio .json, Cura .cfg / .inst.cfg, a
// .3mf project (PrusaSlicer's or Orca's / Bambu's settings inside) or a G-code file (the settings its slicer appended).
json read_profile(const std::filesystem::path& file);
// The settings of `print` (a study's settings.print, or one body's override over it): `profile` read first, then the
// keys given. Throws Error for what it cannot use.
PrintSettings print_settings(const json& print);
std::string infill_family(const std::string& pattern);  // "" when unknown

enum class Region { Wall, TopBottom, Core };
const char* region_name(Region r);

// One region's orthotropic material in its axes (1, 2 in the layer, 3 build direction): engineering constants, tensile and
// shear strengths, and the density of what is printed there.
struct Ortho {
  double E1, E2, E3, nu12, nu13, nu23, G12, G13, G23;  // MPa
  double X, Y, Z, S12, S13, S23;                         // MPa: tensile and shear strengths
  double density;                                         // g/cm3
  double Xc = 0, Yc = 0, Zc = 0;                         // MPa: compressive strengths
};
Ortho region_material(const PrintSettings& s, Region r);

// Failure of a stress (xx yy zz xy yz zx, MPa, world axes) in a region whose axes are a1, a2, a3 (unit, world): in the
// layer, Tsai-Hill with each normal stress against its tensile or compressive strength; between layers, the stress
// across them and the shears along them, quadratically; the worse of the two. index >= 1 fails; the safety factor is
// 1 / sqrt(index). mode: what governs ("along the roads", "across the roads", "between layers" (pulled apart),
// "in compression", "shear").
struct Failure {
  double index = 0;
  std::string mode;
};
Failure failure(const std::array<double, 6>& stress, const Vec3& a1, const Vec3& a2, const Vec3& a3, const Ortho& m);

// The infill region of a printed body (world coordinates): what is wall_thickness inside its surface, top_thickness below
// its upward faces and bottom_thickness above its downward ones, as a slicer leaves it for infill. Null when nothing is
// left (a thin part prints solid). Throws Error when the walls cannot be offset.
TopoDS_Shape print_core(const TopoDS_Shape& body, const PrintSettings& s);
// The same with what lies between the top and bottom skins (the infill and the walls around it; null when the skins
// meet): splitting a body by both makes its mesh follow every region's boundary. skins: the settings have any.
struct PrintRegions {
  TopoDS_Shape core, middle;
  bool skins = false;
};
PrintRegions print_regions(const TopoDS_Shape& body, const PrintSettings& s);

}  // namespace opad::sim
