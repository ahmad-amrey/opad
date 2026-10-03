#pragma once
#include "step_io.hpp"

namespace opad {

// KiCad boards (.kicad_pcb), read from the published s-expression format (no KiCad code): the board as one solid of its
// Edge.Cuts outline (lines, arcs, circles, rectangles, polygons, curves, also inside footprints) with the plated and
// unplated drills as holes, its thickness and solder-mask colour; every footprint with a 3D model as a component placed
// the way KiCad places models (position, rotation, bottom-side flip, then the model's offset, rotation and scale); each
// model is read once and shared by every footprint using it; a translucent box stands in for a model that is not found.
// Models embedded in the footprint or the board (kicad-embed://, zstd + base64) are written once to the user cache and read.
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

// The 3D models shown by the footprints the reader would place with `opt`, each with the footprints using it, the file it
// resolves to and, for a model of KiCad's own library, its place and release there:
// {"models": [{"name", "refs": [...], "file"?, "library"?: "<lib>.3dshapes/<file>.step", "tag"?}], "found", "missing",
//  "downloadable" (missing library models), "download_dir"}.
json kicad_models(const std::filesystem::path& board, const KicadOptions& opt = {});

// Fetches the board's missing library models (kicad_models' "downloadable") with curl from KiCad's library repository
// (gitlab.com/kicad/libraries/kicad-packages3D, at the release of the KiCad version the board's variable names, else
// master; OPAD_KICAD_MODELS_URL replaces the address) into kicad_download_dir(), where the reader finds them next time.
// The models are CC-BY-SA 4.0 with KiCad's design exception: callers ask the user first, and OPAD never ships them.
// {"downloaded": [library paths], "failed": [{"model", "error"}], "dir"}; `progress` returning false cancels.
json kicad_download_models(const std::filesystem::path& board, const KicadOptions& opt = {},
                           const std::function<bool(double, const std::string&)>& progress = {});

// What reading the board again would change in a KiCad import of `doc` (`import_id`, or the only one), in the import's own
// frame and options, matched by footprint uuid, else reference: per reference designator "moved" (dx, dy mm, drot degrees),
// "flipped", "models_changed", "footprint_changed", "added", "removed", a count of "unchanged", and the board's
// "thickness", "holes" and "outline" (area, page box) before/after when they differ; "changed" when anything does.
// `board` defaults to the import's source beside the document.
json kicad_sync_preview(const Document& doc, const std::string& import_id = {}, const std::filesystem::path& board = {});

}  // namespace opad
