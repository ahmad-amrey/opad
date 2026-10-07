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
//                  into the air, the air carrying it out; the parts' faces on the duct are adiabatic.
//
// Results: the parts' temperatures on their surfaces (the solid cells nearest each point), the air's way through the parts
// as streamlines, and the summary (temperatures, the fan's flow and pressure, the air's temperature at the outlet).
// OpenFOAM: OPAD_OPENFOAM (its project directory), else its programs on the PATH with WM_PROJECT_DIR or a known install.
#include <filesystem>
#include <string>

#include "study.hpp"

namespace opad::sim {

// Where OpenFOAM is: the directory of its programs and its project directory; empty when none.
struct OpenFoam {
  std::filesystem::path bin, project, wrapper;  // wrapper: a full install's etc/openfoam, which sets the environment itself
  bool found() const { return !bin.empty() || !wrapper.empty(); }
};
OpenFoam openfoam();

StudyRun run_cfd(const Document& doc, const Scene& scene, const json& settings, const Progress& progress);

}  // namespace opad::sim
