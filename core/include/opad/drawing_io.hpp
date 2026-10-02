#pragma once
#include "step_io.hpp"

namespace opad {
// Imports any supported file into the same append-only document: B-reps (STEP, IGES, BREP), meshes (STL, OBJ, 3MF, PLY,
// glTF/GLB, VRML) and planar drawings (DXF, DWG, SVG). With `options.viewer` nothing is prepared for saving (see ImportOptions).
ImportResult import_file(Document& doc, const std::filesystem::path& file, const ImportOptions& options = {});
// The extensions import_file reads, lower case with the dot.
const std::vector<std::string>& importable_extensions();
ExportResult export_drawing(const Document& doc, const Scene& scene, const std::filesystem::path& file, const ExportOptions& options);
}
