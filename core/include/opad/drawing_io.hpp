#pragma once
#include "step_io.hpp"

namespace opad {
// Import STEP, planar DXF/SVG, or STL/OBJ mesh geometry into the same append-only document.
ImportResult import_file(Document& doc, const std::filesystem::path& file, const ImportOptions& options = {});
ExportResult export_drawing(const Document& doc, const Scene& scene, const std::filesystem::path& file, const ExportOptions& options);
}
