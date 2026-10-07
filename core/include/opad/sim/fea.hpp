#pragma once
// Structural studies (static and modal): the bodies a load case acts on, meshed by Netgen into quadratic tetrahedra
// (C3D10), solved by CalculiX's ccx, read back as nodal displacements and stresses or natural frequencies and mode shapes.
//
// Units: mm, N, MPa (N/mm2), tonnes/mm3 for density, so frequencies come out in Hz.
//
// Bodies that touch along faces are joined where they touch (bonded): the general fuse of their solids shares those faces,
// so the mesh is continuous through them. A bolt with a preload is cut across the middle of its shank and the cut is a
// CalculiX pre-tension section carrying the preload; a case with no support gets the least it needs to stay put (three
// nodes fixed 3-2-1), which carries no load when the loads balance (a preload).
//
// Loads (load ops, sim/joints.hpp): fixed (faces held), displacement (faces moved by a vector), force (N, spread over the
// faces by area), pressure (MPa, into the faces), moment (N.mm about the faces' centre, as a linear force field), gravity
// (mm/s2 on every body's mass) and bolt_preload (N on a bolt body). Materials come from the bodies (materials.hpp
// mechanical()), steel when none is set.
//
// Thermal studies mesh the same way and solve heat transfer: heat (W into bodies or through faces), temperature (faces
// held at degC), convection (a film coefficient, or one from the air: sim/airflow.hpp), radiation and fans through
// plate-fin heatsinks; steady, or over time from the ambient temperature. Conductivity, specific heat and emissivity
// come from the bodies (materials.hpp thermal()). Units inside: mm, s, t, mW; W/m.K conductivity is the same number.
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "study.hpp"

namespace opad::sim {

struct FeaResult {
  std::string kind;                            // static | modal | thermal
  std::vector<Vec3> nodes;                     // mm, world
  std::vector<std::array<int, 3>> skin;        // the outer surface as triangles (node indices), to draw results on
  std::vector<int> skin_body;                  // the body each triangle is on (index into bodies)
  std::vector<std::string> bodies;             // body node ids
  std::vector<std::array<int, 4>> tets;        // corner nodes of every element (probing)
  // static
  std::vector<Vec3> displacement;              // mm
  std::vector<std::array<double, 6>> stress;   // MPa: xx yy zz xy yz zx
  std::vector<double> von_mises;               // MPa
  std::vector<double> failure_index;           // printed bodies (sim/printing.hpp): their failure index at each node (>= 1 fails), 0 elsewhere
  // thermal
  std::vector<double> temperature;             // degC, steady or the last frame
  std::vector<std::vector<double>> temperature_frames;  // [frame][node] over time (StudyRun::t)
  // the CFD air (sim/cfd.hpp): the air's way through the parts from points across the inlet, with its speed (m/s) and
  // temperature (degC) at each point
  std::vector<std::vector<Vec3>> streamlines;
  std::vector<std::vector<double>> streamline_speed, streamline_temperature;
  // modal
  std::vector<double> frequencies;             // Hz
  std::vector<std::vector<Vec3>> modes;        // shape of each mode at each node (mass normalised, as ccx gives them)
  double mesh_size = 0;
  size_t elements = 0;
};

// The air around a thermal study's parts, worked out elsewhere (sim/cfd.cpp, the air solved around them): every outer face of
// the parts takes its film from it, given again after each solve from the temperatures, until they settle.
struct AirFace {
  Vec3 centre, normal;     // mm, outward
  double area = 0;         // mm2
  int body = 0;            // index into the study's bodies
  double T = 0;            // degC, as last solved
  double h = 0, sink = 0;  // to give: W/m2K, degC
};
using AirFilms = std::function<void(std::vector<AirFace>& faces, int pass)>;

// Where CalculiX's solver is: OPAD_CCX, beside the program, else ccx (or ccx_2.21, ...) on the PATH. Empty when none.
std::filesystem::path ccx_program();

// The value of a result field at a world point: the nearest node's (field: von_mises, displacement, ...).
double probe(const FeaResult& r, const Vec3& at, const std::string& field, int mode = 0);

}  // namespace opad::sim
