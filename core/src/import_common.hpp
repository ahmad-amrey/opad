#pragma once
// Shared by the format readers (step_io.cpp, drawing_io.cpp, formats.cpp); not a public header: it carries OCCT types.
#include <TDocStd_Document.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "opad/step_io.hpp"

namespace opad::detail {

// Stores one imported body. Viewer mode registers the reader's shape as a live body (no healing, BREP text or
// hashing); otherwise the BREP text goes into the body store, with the triangulation for a mesh (it has no surfaces).
std::string store_body(Document& doc, TopoDS_Shape shape, json meta, const ImportOptions& opt, bool mesh, ImportResult* res = nullptr);

// A live shape (viewer mode, a linked asset) as body-store text, by its meta's representation: a mesh with its
// triangulation; a solid checked and healed like a full import first (`healed` set when that changed it, the meta's face
// colours moved to the faces healing made); a drawing as it is.
std::string persist_body(TopoDS_Shape& shape, json& meta, bool& healed);

// Walks an XCAF document (assemblies, instances, names, colours, materials) into one import op. `mesh`: the shapes are
// triangulations (glTF, OBJ, VRML), not B-reps. For readers that do not convert units or axes themselves, `scale` takes
// file units to mm and `root` turns the file's axes to OPAD's.
ImportResult import_xcaf(Document& doc, const Handle(TDocStd_Document)& xdoc, const std::filesystem::path& file, const ImportOptions& opt, bool mesh,
                         double scale = 1.0, const Mat4& root = {});

// A triangle mesh as one face: `xyz` holds 3 floats per vertex, `triangles` 3 zero-based indices per triangle. Vertices at
// the same position are merged first when `weld` is set (STL repeats them per facet). Normals are smoothed across edges
// flatter than 30 degrees and kept sharp across steeper ones, so curved parts look round and machined edges stay crisp.
TopoDS_Face mesh_face(const std::vector<float>& xyz, const std::vector<uint32_t>& triangles, bool weld);
// The same mesh coloured triangle by triangle (3MF materials and painting, PLY colours): `colour[t]` indexes `colours`, -1
// the body's own colour. One face per colour, the body's first (a compound when there are several, its "face_colors" put
// into `meta`), shaded as one mesh.
TopoDS_Shape mesh_faces(const std::vector<float>& xyz, const std::vector<uint32_t>& triangles, bool weld, const std::vector<int>& colour,
                        const std::vector<std::array<double, 3>>& colours, json* meta = nullptr);

// Runs a program and waits for it (two minutes at most); its exit status, or -1 when it did not start. Arguments go as
// wide strings on Windows, so a file named in Arabic reaches the program intact; its output is dropped (drawing_io.cpp).
int run_program(const std::filesystem::path& program, const std::vector<std::filesystem::path>& args, const std::filesystem::path& cwd = {});

// The viewer cache keyed by content (viewer_cache.cpp): a linked asset's read, found again by its file's hash and how it was
// read (`content`) wherever the file now is (a clone, a branch, a renamed folder). Same contract as viewer_cache_load/store.
bool asset_cache_load(Document& doc, const std::string& content, const ImportOptions& opt);
void asset_cache_store(const Document& doc, const std::string& content, const std::function<bool()>& cancelled = {});
// A DWG's conversion (viewer_cache.cpp): the DXF text a converter (`converter` names it) made, kept by the DWG's content,
// so the drawing opens again without converting. find: the kept DXF, or empty. keep: only when converting took at least as
// long as reading the DXF (opening it again is then at least twice as fast).
std::filesystem::path dwg_cache_find(const std::filesystem::path& dwg, const std::string& converter);
bool dwg_cache_keep(const std::filesystem::path& dwg, const std::string& converter, const std::filesystem::path& dxf, double convert_ms, double read_ms);

// The readers behind import_file (formats.cpp).
ImportResult import_iges(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
// glTF, GLB, OBJ, VRML; `kicad_vrml`: a KiCad footprint model, in 2.54 mm units and Z up (VRML is otherwise metres, Y up).
ImportResult import_mesh_scene(Document& doc, const std::filesystem::path& file, const ImportOptions& opt, bool kicad_vrml = false);
ImportResult import_brep_file(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
// A picture (.png .jpg .bmp .gif .webp) as a canvas: a flat rectangle at the picture's resolution with its bytes as a raster
// (image_io.cpp).
ImportResult import_image(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
ImportResult import_stl(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
ImportResult import_ply(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);
ImportResult import_3mf(Document& doc, const std::filesystem::path& file, const ImportOptions& opt);

}  // namespace opad::detail
