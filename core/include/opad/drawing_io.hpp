#pragma once
#include "step_io.hpp"

namespace opad {
// Imports any supported file into the same append-only document: B-reps (STEP, IGES, BREP), meshes (STL, OBJ, 3MF, PLY,
// glTF/GLB, VRML) and planar drawings (DXF, DWG, SVG). With `options.viewer` nothing is prepared for saving (see ImportOptions).
ImportResult import_file(Document& doc, const std::filesystem::path& file, const ImportOptions& options = {});
// The extensions import_file reads, lower case with the dot.
const std::vector<std::string>& importable_extensions();
// The git work tree a path lies in: the nearest folder up holding a .git entry (folder or file); empty when none. No git.
std::filesystem::path repo_top(const std::filesystem::path& path);
// Where an import op's source file is now (UI-07). The op records it as "source_path" (absolute) and "source_repo"
// (relative to the work tree it was in, which survives a clone or a move of the repository): first that path in the
// document's work tree, then source_path, then "source" (the file name) beside the document. `exists` says whether the
// file is there; the path is the best guess either way (empty: the op names no file). A few stats, no reading.
std::filesystem::path import_source(const json& op, const std::filesystem::path& document, bool* exists = nullptr);
ExportResult export_drawing(const Document& doc, const Scene& scene, const std::filesystem::path& file, const ExportOptions& options);
// DWG goes through LibreDWG's converters unless the ODA File Converter is switched on: third-party software whose terms
// allow non-members non-commercial use only, so it is opt-in (the desktop's files/useOda, or OPAD_USE_ODA=1), never
// bundled. oda_file_converter: the installed one, empty when none.
void set_use_oda(bool on);
bool use_oda();
std::filesystem::path oda_file_converter();
// The converter a DWG is read through now: "oda", "libredwg" or "override:<OPAD_DWG2DXF>". The viewer cache keys DWG reads
// on it, so switching ODA on or off reads a remembered drawing again.
std::string dwg_reader();
// Whether a DWG converter is there for writing (toDwg) or reading DWG, found as the conversion looks for it: OPAD_DXF2DWG /
// OPAD_DWG2DXF when set (that file alone), the ODA File Converter while it is switched on (use_oda), LibreDWG's beside the program or on PATH. File checks only.
bool dwg_converter(bool toDwg);
}
