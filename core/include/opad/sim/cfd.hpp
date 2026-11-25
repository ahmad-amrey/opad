#pragma once
// Thermal studies with the air solved (settings.air = "cfd"): OpenFOAM, run as separate programs (as CalculiX is), takes the
// parts and the air around them as one conjugate heat-transfer problem instead of handbook correlations.
//
//   the air        a duct around the parts along the fan's (or the stream's) way: a little upstream, more downstream,
//                  one cell clear of them across; blockMesh, then snappyHexMesh cuts the parts out of it (each a region
//                  of its own) and splitMeshRegions separates them;
//   the flow       simpleFoam, laminar and steady, first-order upwind (a fin's blunt end sheds a wake that a steady
//                  solution of higher order would not settle): a fan as the inlet's pressure from its curve (it finds its own
//                  operating point against the parts' resistance), or a stream at the velocity of a forced convection;
//   the heat       chtMultiRegionSimpleFoam on that flow, frozen: heat sources in the parts, conduction through them and
//                  into the air, the air carrying it out; the parts' faces on the duct are adiabatic. Upwind for the
//                  air's enthalpy too: linearUpwind left air colder than the inlet behind the fins (8 K on a 2 mm
//                  mesh) and the parts 4 K hotter; limitedLinear agrees with upwind to 0.2 K.
//
// An enclosure (cfd.enclosure, or the smallest shown body holding the loads' bodies): the air in it and a margin of the room
// around it, open on every side, every shown body inside taking part; its vents are its holes, each fan load a disk of
// baffles with the fan's curve as the pressure jump across it (on a heatsink: against its upstream side; on a body that
// stands for the fan: its middle, the body itself air). Only the air is meshed; the flow is solved once, then the heat goes
// back and forth: the parts in CalculiX as one bonded mesh (sim/fea.cpp, AirFilms), the air's temperature in OpenFOAM on
// the frozen flow (scalarTransportFoam) with the parts' surfaces as its walls, each face's heat into the air its film for
// the next solve. (OpenFOAM's own conjugate solver, region by region a step behind, crawled or diverged on touching chips,
// heatsinks and boards.)
//
// Results: the parts' temperatures on their surfaces (the solid cells nearest each point), the air's way through the parts
// as streamlines, and the summary (temperatures, the fan's flow and pressure, the air's temperature at the outlet).
// OpenFOAM: OPAD_OPENFOAM (its project directory), else its programs on the PATH with WM_PROJECT_DIR or a known install.
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "study.hpp"

namespace opad::sim {

// Where OpenFOAM is: the directory of its programs and its project directory; empty when none. `problem`: why an install
// that is there cannot run (OpenCFD's Windows build without the MS-MPI it links against); found() is false then.
// Windows: OpenCFD's installer (per user under %APPDATA%\ESI-OpenCFD\OpenFOAM\v<N>, or Program Files) is found by itself;
// OPAD_OPENFOAM may name its root, the OpenFOAM-v<N> folder inside it, or a folder with bin/simpleFoam.
struct OpenFoam {
  std::filesystem::path bin, project, wrapper;  // wrapper: a full install's etc/openfoam, which sets the environment itself
  std::string problem;
  bool found() const { return (!bin.empty() || !wrapper.empty()) && problem.empty(); }
};
OpenFoam openfoam();

// The enclosure a thermal study's air would be solved in: `named` (a body id), else the smallest shown solid whose box holds
// all of `bodies`; when none does, the smallest group of shown solids around them whose boxes together do (a base and its
// lid, a frame and its panels: each reaching out past the parts, none a part of them). `walls`: its bodies (`enclosure`
// the first); `inside`: every other shown solid within their box. Both empty when nothing encloses them.
struct Enclosure {
  std::string enclosure;
  std::vector<std::string> walls;
  std::vector<std::string> inside;
};
Enclosure find_enclosure(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, const std::string& named = {});
// The same with the enclosure's bodies named (cfd.enclosure as a list): every one a shown solid.
Enclosure find_enclosure(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, const std::vector<std::string>& named);

// Every shown solid's world box, measured once (in parallel): what the enclosure is found from. Measuring every body again
// for each candidate held the UI for a minute on a board with its parts; a caller that asks often keeps one (worker-built).
struct BodyBoxes {
  using Box = std::array<double, 6>;  // x0 y0 z0 x1 y1 z1
  std::vector<std::string> ids;
  std::vector<Box> boxes;
  std::unordered_map<std::string, size_t> at;
};
BodyBoxes body_boxes(const Document& doc, const Scene& scene);
// find_enclosure from the boxes: `named` (one body or several), else found around `bodies`; no geometry is read.
Enclosure find_enclosure(const BodyBoxes& boxes, const std::vector<std::string>& bodies, const std::vector<std::string>& named);
// The enclosure when nothing makes heat yet: the body that holds the most others, with what closes it. Empty: none.
std::vector<std::string> likely_enclosure(const BodyBoxes& boxes);
// A group of walls as the summary names it: "Base + Lid".
std::string enclosure_name(const Scene& scene, const std::vector<std::string>& walls);

// A fan's opening as drawn (a fan load's "outlet", a blower's "inlet"): a closed region of a sketch ({"sketch", "at" or
// "boundary"}, as extrude takes profiles) or a planar face (a reference). Its outline (closed loops, world mm), its normal (the
// sketch's, or the face's out of its body), its centre and area (mm2). Throws when the reference names no closed region.
struct FanProfile {
  std::vector<std::vector<std::array<double, 3>>> loops;
  std::array<double, 3> normal{0, 0, 1}, centre{0, 0, 0};
  double area = 0;
};
FanProfile fan_profile(const Document& doc, const Scene& scene, const json& ref);

// Whether the box's vents let its fans blow what they can, before a run. Its vents are the holes through its walls (a face's
// inner loops, on the faces that look out of the box); those ahead of the fans' air are exhausts, the others intakes, a hole a
// fan's own opening sits in is that fan's (it blows straight through it). Each side a sharp-edged orifice (discharge 0.6):
// dp = rho/2 (Q / (0.6 A))^2, both in series, met by the fans' curves together (their flows at that pressure added): the flow
// they give, against the free flow, which side holds them back, and the flow the heat needs for a 10 K rise of the air.
// fans: fan loads' definitions (fan, type, outlet, inlet, vector, refs); watts: the heat all told.
// progress(done, of): the holes examined so far; false stops it (Error "cancelled").
json vent_estimate(const Document& doc, const Scene& scene, const std::vector<std::string>& walls, const std::vector<json>& fans, double watts,
                   double ambient_C = 25, const std::function<bool(int, int)>& progress = {});

StudyRun run_cfd(const Document& doc, const Scene& scene, const json& settings, const Progress& progress);

}  // namespace opad::sim
