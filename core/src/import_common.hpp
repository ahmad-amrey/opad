#pragma once
// Shared by the format readers (step_io.cpp, drawing_io.cpp, formats.cpp); not a public header: it carries OCCT types.
#include <TDocStd_Document.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "opad/step_io.hpp"

namespace opad::detail {

// Stores one imported body. Viewer mode registers the reader's shape as a live body (no healing, BREP text or
// hashing); otherwise the BREP text goes into the body store, with the triangulation for a mesh (it has no surfaces).
std::string store_body(Document& doc, TopoDS_Shape shape, json meta, const ImportOptions& opt, bool mesh, ImportResult* res = nullptr);

// Walks an XCAF document (assemblies, instances, names, colours, materials) into one import op. `mesh`: the shapes are
// triangulations (glTF, OBJ, VRML), not B-reps. For readers that do not convert units or axes themselves, `scale` takes
// file units to mm and `root` turns the file's axes to OPAD's.
ImportResult import_xcaf(Document& doc, const Handle(TDocStd_Document)& xdoc, const std::filesystem::path& file, const ImportOptions& opt, bool mesh,
                         double scale = 1.0, const Mat4& root = {});

// A triangle mesh as one face: `xyz` holds 3 floats per vertex, `triangles` 3 zero-based indices per triangle. Vertices at
// the same position are merged first when `weld` is set (STL repeats them per facet). Normals are smoothed across edges
// flatter than 30 degrees and kept sharp across steeper ones, so curved parts look round and machined edges stay crisp.
TopoDS_Face mesh_face(const std::vector<float>& xyz, const std::vector<uint32_t>& triangles, bool weld);

// Where the source file is, on its import op (UI-07): "source_path" (absolute) and "source_repo" (relative to the git
// work tree it lies in; none outside one), both UTF-8 with '/'; of opt.source_file when set, else of `file`.
void stamp_source(json& op, const std::filesystem::path& file, const ImportOptions& opt);

// The readers behind import_file (formats.cpp).
ImportResult import_iges(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
ImportResult import_mesh_scene(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);  // glTF, GLB, OBJ, VRML
ImportResult import_brep_file(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
ImportResult import_stl(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
ImportResult import_ply(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
ImportResult import_3mf(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);

}  // namespace opad::detail
