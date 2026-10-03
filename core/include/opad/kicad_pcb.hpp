#pragma once
#include "step_io.hpp"

namespace opad {

// KiCad boards (.kicad_pcb), read from the published s-expression format (no KiCad code): the board as one solid of its
// Edge.Cuts outline (lines, arcs, circles, rectangles, polygons, curves, also inside footprints) with the plated and
// unplated drills as holes, its thickness and solder-mask colour; every footprint with a 3D model as a component placed
// the way KiCad places models (position, rotation, bottom-side flip, then the model's offset, rotation and scale); each
// model is read once and shared by every footprint using it; a translucent box stands in for a model that is not found.
// Hidden 2D layers (Edge.Cuts with the drills, courtyards) are there for sketches. `opt.viewer` keeps the shapes live.
ImportResult import_kicad_pcb(Document& doc, const std::filesystem::path& file, const ImportOptions& opt = {});

// The file a footprint's 3D model names on `board` (the .kicad_pcb), or empty: ${KIPRJMOD} is the board's folder;
// KICAD<n>_3DMODEL_DIR, KISYS3DMOD and other variables come from the project's text variables (<board>.kicad_pro), the
// environment, KiCad's configuration (kicad_common.json), then its install folders; then `model_dirs` (by the path below
// the variable, then by the file name), then kicad_download_dir(). A STEP is preferred anywhere (a VRML name finds the
// STEP beside it) before a VRML file itself, which is read in KiCad's units.
std::filesystem::path kicad_model_file(const std::string& name, const std::filesystem::path& board,
                                       const std::vector<std::filesystem::path>& model_dirs = {});

// Where models of KiCad's library are downloaded to (<user cache>/kicad-models, laid out as the library is).
std::filesystem::path kicad_download_dir();

}  // namespace opad
