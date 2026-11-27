#pragma once
// Flow studies (study kind "flow"): steady, turbulent, isothermal flow of air or water through a part (a duct fitting, an
// adapter, a nozzle) or around it, solved with OpenFOAM (simpleFoam, k-omega SST, wall functions) as separate programs.
//
//   inside   the fluid is the inside of the walls between an inlet and an outlet. An opening is the loop of edges round it
//            (closed and flat: the disc it bounds is the opening; a flat face's whole boundary is the end of a body that is
//            the fluid itself), or, as older studies give it, a flat face: the rim of a wall (one hole: the hole is the
//            opening) or the end of a body that is the fluid (no holes). Straight stubs of pipe go before the inlet and after the outlet (2 and 12
//            diameters by default, `extend`): the flow comes in developed and an expansion's jet mixes out before the
//            outlet, as in a real duct. Measured as a handbook measures a fitting: the total pressure at the inlet face
//            less the total pressure far downstream, the straight stub's own friction (smooth pipe) taken back out.
//   around   a box around the parts (bodies), the fluid coming at a speed along a direction; the parts' drag.
//
// A fitting whose walls are round about the line between the openings' centres (sampled by rays from that axis at several
// angles all along it) is meshed as a 5 degree wedge of its section (blockMesh), which takes seconds where the full part
// takes minutes; a sudden step in it (a wall steeper than 60 degrees) is a step of the wedge's blocks. Any other is meshed
// as the full part (blockMesh, then snappyHexMesh with layers along the walls). The solve stops once the pressure drop has
// settled (probes across the openings, every 10 iterations).
//
// Results: the summary (pressure_drop_pa, flow_m3s, max_speed_ms, recirculation), the series along the flow (static and total
// pressure, mean and max speed against the distance from the inlet face, in run.t), the walls' surface with the pressure and
// speed beside them, a cut through the middle of the fluid coloured the same way, and streamlines.
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "airflow.hpp"
#include "study.hpp"

namespace opad::sim {

// The settings a flow study takes, read and checked (flow_settings throws Error saying what is wrong).
//   {inlet: <edge refs of one loop> | <one face ref>, outlet: the same, flow: {value, unit: cfm | m3/h | l/s} | fan: {curve: [[q, p], ...] | free_flow,
//    shutoff_pressure, unit (of q and free_flow, default m3/h), pressure_unit (Pa | inH2O | mmH2O), speed %} | {model: id},
//    fluid: air | water, ambient: degC (20), quality: quick | normal | fine,
//    domain: inside | around (direction [x y z], speed {value, unit: m/s | km/h | mph} or m/s, bodies),
//    mesh: auto | axisymmetric | 3d, extend: {inlet, outlet} (diameters; 0: none), iterations, turbulence: auto | sst | laminar}
struct FlowSettings {
  std::string domain = "inside";
  std::vector<Ref> inlet, outlet;   // the edges of a loop, or one face
  std::vector<std::string> bodies;  // around: the parts (default: every shown solid); inside: more walls
  enum class Drive { Flow, Fan, Speed } drive = Drive::Flow;
  double flow_m3s = 0;              // Drive::Flow
  air::Fan fan;                     // Drive::Fan
  Vec3 direction{1, 0, 0};          // Drive::Speed (around)
  double speed_ms = 0;
  std::string fluid = "air";
  double ambient = 20;              // degC
  double rho = 0, nu = 0;           // kg/m3, m2/s at the ambient temperature
  std::string quality = "normal";
  std::string mesh = "auto";
  double extend_inlet = 2, extend_outlet = 12;  // diameters of straight pipe before the inlet and after the outlet
  int iterations = 0;               // 0: the quality's
  std::string turbulence = "auto";  // auto: laminar below Re 2000, else k-omega SST
};
FlowSettings flow_settings(const json& settings);

// How the fluid is meshed, decided from the geometry before anything is written (no OpenFOAM needed).
struct FlowPlan {
  std::string mesh;     // "axisymmetric" | "3d"
  std::string why_3d;   // with mesh "3d" and an inside domain: why it is not round (empty when asked for)
  std::string mode;     // inside: "wall" (the openings are holes in the walls' end faces) | "volume" (a body is the fluid)
  std::vector<std::string> walls;  // the bodies whose faces bound the fluid (around: the parts)
  struct Opening {
    Vec3 centre{0, 0, 0}, normal{0, 0, 1};  // normal: out of the fluid
    double area_mm2 = 0, diameter_mm = 0;  // diameter: hydraulic, 4 A / perimeter
    double radius_mm = 0;                  // round openings: their radius
    std::vector<Vec3> outline;             // the opening's edge, sampled (world mm)
  } inlet, outlet;
  Vec3 axis{0, 0, 1};    // inside: the flow's way at the inlet (into the fluid)
  double length_mm = 0;  // inlet face to outlet face along the axis (axisymmetric), or between their centres
  double inlet_extension_mm = 0, outlet_extension_mm = 0;
  std::vector<std::array<double, 2>> profile;  // axisymmetric: (distance along the axis, radius of the wall) mm, inlet to outlet
  double steepest_deg = 0;  // axisymmetric: the wall's steepest angle to the axis (above 30: the wedge corrects for skew), steps left out
  std::vector<size_t> steps;  // axisymmetric: the profile's sudden steps (walls steeper than 60 degrees): stretch i to i + 1
  double step_deg = 0;        // the steepest of them
  double cell_mm = 0;    // the mesh's cells along the flow (axisymmetric) or the background cells (3d)
  int radial_cells = 0;  // axisymmetric
  size_t cells = 0;      // an estimate of the mesh's size
  int iterations = 0;    // at most
  double seconds = 0;    // about how long the run takes on this computer's kind of system (estimate_seconds)
  double solve_seconds = 0, per_cell_iteration = 0;  // of which the solve, at this cost per cell and iteration (s)
  json to_json() const;
};
FlowPlan plan_flow(const Document& doc, const Scene& scene, const FlowSettings& settings);

// The OpenFOAM case for the plan written into `dir` (meshing dictionaries, fields, schemes; no program is run).
void write_flow_case(const Document& doc, const Scene& scene, const FlowSettings& settings, const FlowPlan& plan, const std::filesystem::path& dir);

StudyRun run_flow(const Document& doc, const Scene& scene, const json& settings, const Progress& progress);

// The loop of edges round an opening that `edge` is on (a click on one edge of it takes the whole loop): the edges of a hole
// in a flat face first, else a flat face's whole boundary, else a closed flat loop of a curved face's edges (a chamfer's or
// a bore's end); the smallest of a kind. Throws when no closed flat loop runs through it.
std::vector<Ref> opening_loop(const Document& doc, const Scene& scene, const Ref& edge);

// A part's two openings as OPAD proposes them, from those bodies only: each rim's hole, taken in past a chamfer or a rounded
// edge to the bore; else the flat faces' boundaries. The two farthest apart, the wider first (the air goes in there):
// [{edges: [edge refs], centre, size (mm across), body}, ...], empty when none.
json propose_openings(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies);

// Water's density (kg/m3) and kinematic viscosity (m2/s) at T degC (1 to 99).
void water_properties(double T, double& rho, double& nu);

}  // namespace opad::sim
