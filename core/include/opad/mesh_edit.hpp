#pragma once
// Mesh editing for 3D-print prep (core/src/mesh): mesh bodies (STL, OBJ, 3MF, PLY: a face whose triangulation is the
// geometry, `is_mesh_shape`) scaled, mirrored, joined, cut, split, repaired and simplified as meshes, never turned into
// solids. Booleans and plane cuts with capped faces are Manifold's (Apache-2.0, cmake/mesh_libs.cmake), built serial so a
// result is the same on every system; simplifying is meshoptimizer's (MIT). Everything here walks triangles: workers only.
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_GTrsf.hxx>

#include "document.hpp"
#include "json.hpp"
#include "scene.hpp"

namespace opad::meshops {

using Cancel = std::function<bool()>;

// A triangle mesh in world coordinates: 3 doubles a vertex, 3 indices a triangle (counter-clockwise seen from outside).
struct Mesh {
  std::vector<double> xyz;
  std::vector<uint32_t> tri;
  size_t vertices() const { return xyz.size() / 3; }
  size_t triangles() const { return tri.size() / 3; }
  bool empty() const { return tri.empty(); }
  std::array<double, 3> vertex(size_t v) const { return {xyz[v * 3], xyz[v * 3 + 1], xyz[v * 3 + 2]}; }
};

// Every triangulation of the shape with its locations applied, vertices at the same position merged (exactly; `weld` > 0
// merges those closer than that too). Throws when it has no triangles.
Mesh from_shape(const TopoDS_Shape& shape, double weld = 0);
// Vertices within `tol` of each other merged into one (0: exactly coincident only); `merged` counts them.
Mesh weld(const Mesh& m, double tol, size_t* merged = nullptr);
// A solid (or any B-rep) meshed on a copy: chord `deflection` (<= 0: 1/2000 of its box diagonal), 0.2 rad angle.
Mesh mesh_solid_copy(const TopoDS_Shape& shape, double deflection = 0);
// The mesh as a mesh body's shape: one face, triangulation only (what import stores).
TopoDS_Shape to_shape(const Mesh& m);
// A mesh body's text for the body store: BREP with the triangulation (store_body's mesh form), and its key (sha256).
std::string to_brep(const TopoDS_Shape& meshShape);

// Affine map of the vertices (scale per axis, mirror, any gp_Trsf/GTrsf); a map that turns the mesh inside out (negative
// determinant) reverses every triangle, so outside stays outside.
void transform(Mesh& m, const gp_GTrsf& t);
// A mesh body's shape moved by a rigid or uniformly scaled map, as a mesh (BRepBuilderAPI_Transform would copy a face
// without a surface badly).
TopoDS_Shape transformed(const TopoDS_Shape& meshShape, const gp_GTrsf& t);

Bnd_Box bounds(const Mesh& m);
// A shape's tight box: a mesh's vertices, else BRepBndLib's optimal box (no tolerance padding). Its lowest Z is where a
// body stands on the bed (drop_to_bed; print export's settle_meshes can take it too).
Bnd_Box tight_box(const TopoDS_Shape& shape);
double volume(const Mesh& m);                 // signed: positive for an outward closed mesh
std::array<double, 3> centroid(const Mesh& m);  // of the enclosed volume when closed, else of the vertices' box

// Shells: the triangles joined through shared vertices, each as its own mesh (largest first).
std::vector<Mesh> components(const Mesh& m);

// ---------------------------------------------------------------- Booleans and cuts (Manifold)
enum class Op { Union, Subtract, Intersect };
// a op b. Both must be closed, oriented 2-manifolds (else Error naming the side and its open/non-manifold edges: repair it
// first). An empty result is an empty mesh.
Mesh boolean(const Mesh& a, const Mesh& b, Op op, const Cancel& cancel = {});
// Several bodies joined at once (BatchBoolean): faster than one by one.
Mesh union_all(const std::vector<Mesh>& meshes, const Cancel& cancel = {});
// The parts on the plane's positive side (along `normal`) and negative side. A closed mesh is cut by Manifold, its cut
// faces capped; an open one (a scan) is clipped triangle by triangle, uncapped (`capped` false).
struct Cut {
  Mesh above, below;
  bool capped = true;
};
Cut split_by_plane(const Mesh& m, const std::array<double, 3>& origin, const std::array<double, 3>& normal, const Cancel& cancel = {});

// ---------------------------------------------------------------- checks and repair
struct Report {
  size_t vertices = 0, triangles = 0;
  size_t shells = 0;
  size_t open_edges = 0;          // sides with one triangle
  size_t holes = 0;               // loops of open edges
  size_t non_manifold_edges = 0;  // sides shared by more than two triangles
  size_t flipped_edges = 0;       // sides two triangles run the same way (inconsistent orientation)
  size_t degenerate = 0;          // zero-area triangles (or with a repeated vertex)
  size_t duplicates = 0;          // triangles over the same three vertices as another
  size_t self_intersections = 0;  // pairs of triangles crossing (not sharing a vertex); counting stops at `limit`
  bool self_intersections_capped = false;
  bool closed() const { return open_edges == 0 && non_manifold_edges == 0; }
  bool printable() const { return closed() && flipped_edges == 0 && self_intersections == 0; }
  double volume = 0, area = 0;
  json to_json() const;
};
struct CheckOptions {
  bool self_intersections = true;
  size_t limit = 1000;
};
Report check(const Mesh& m, const CheckOptions& opt = {}, const Cancel& cancel = {});

struct RepairOptions {
  double weld = 0;            // merge vertices closer than this (0: exact duplicates only)
  bool remove_degenerate = true;
  bool remove_duplicates = true;
  bool orient = true;         // one way round per shell, outward (positive volume)
  bool fill_holes = true;
  size_t max_hole_edges = 0;  // 0: any size
  bool count_crossings = true;  // the report after: self-intersections counted too (a box tree over the triangles)
};
struct RepairResult {
  Mesh mesh;
  Report before, after;
  size_t welded = 0, removed_degenerate = 0, removed_duplicates = 0, flipped = 0, holes_filled = 0, fill_triangles = 0;
  json to_json() const;
};
RepairResult repair(const Mesh& m, const RepairOptions& opt = {}, const Cancel& cancel = {});

// The mesh_check command: {bodies?: id | [ids] (all mesh bodies when left out), self_intersections?, limit?} ->
// {bodies: [{id, name, ...Report, next?}], printable, checked}.
json check_command(const Document& doc, const Scene& scene, const json& args, const Cancel& cancel = {});

// ---------------------------------------------------------------- simplify (meshoptimizer)
struct SimplifyOptions {
  size_t target_triangles = 0;  // 0: as few as the tolerance allows
  double tolerance = 0;         // mm the surface may move (0: 1% of the box diagonal when no target is given)
  bool keep_borders = true;     // open edges stay where they are
};
Mesh simplify(const Mesh& m, const SimplifyOptions& opt, double* error_mm = nullptr, const Cancel& cancel = {});

}  // namespace opad::meshops
