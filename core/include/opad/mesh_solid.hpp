#pragma once
// Mesh to solid (design.mesh_solid): a B-rep rebuilt from a triangle mesh (STL, OBJ, 3MF, PLY ...), and how close the
// result follows the mesh. The design layer (design/mesh_solid.hpp) first tries a sketch and an extrusion or revolution
// (editable history); what is neither is rebuilt here: the mesh is split into regions that one plane, cylinder, cone,
// sphere or torus fits within the tolerance, their shared borders become the edges (intersection curves where the two
// surfaces give one, else lines, arcs or splines fitted to the border), and the faces are sewn into a solid. Triangles no
// surface fits stay planar facets (coplanar ones merged), so a closed mesh always gives a closed solid.
// Everything here walks geometry: workers only.
#include <array>
#include <functional>
#include <string>
#include <vector>

#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>

#include "json.hpp"

namespace opad {

// A triangle mesh with shared vertices, as the conversion reads one.
struct TriMesh {
  std::vector<gp_Pnt> points;
  std::vector<std::array<int, 3>> triangles;
  bool empty() const { return triangles.empty(); }
  double area() const;
  double volume() const;    // enclosed volume (absolute; meaningful for a closed mesh)
  double diagonal() const;  // of the bounding box
  int open_edges() const;   // edges with one triangle (0: closed)
};

// The triangles of a triangulation-only shape (a mesh body) with its location applied, vertices closer than `weld` merged
// (<= 0: 1e-7 of the diagonal). Throws when there are none.
TriMesh mesh_of_shape(const TopoDS_Shape& shape, double weld = 0);

// How closely a rebuilt shape follows its source mesh.
struct MeshDeviation {
  double tolerance = 0;
  int samples = 0;          // mesh vertices measured
  double max = 0;           // mesh vertices to the shape's surface: the largest distance (a mesh's vertices lie on the true surface)
  double mean = 0, rms = 0;
  double within = 0;        // the fraction of mesh vertices within the tolerance (0..1)
  double back_max = 0;      // the shape's surface to the mesh: largest distance (on curved faces at most the facets' sagitta)
  double back_mean = 0;
  double sagitta = 0;       // the mesh's own chord error estimated from its facets: what back_max may reach
  double mesh_volume = 0, shape_volume = 0, mesh_area = 0, shape_area = 0;
  int mesh_triangles = 0, shape_faces = 0;
  double score() const;     // 0..100: the share of the mesh within tolerance, less any back deviation past what its facets explain
  json to_json() const;
};
MeshDeviation mesh_deviation(const TriMesh& mesh, const TopoDS_Shape& shape, double tolerance, const std::function<bool()>& cancelled = {});

struct MeshSolidOptions {
  double tolerance = 0;  // mm a rebuilt surface may be off the mesh's vertices; <= 0: mesh_tolerance()
  double angle = 35;     // degrees: the largest angle between neighbouring facets of one curved surface (more is a sharp edge)
};
// The default tolerance for a mesh: 1/2000 of its diagonal, at least 0.001 mm.
double mesh_tolerance(const TriMesh& mesh);

struct MeshBrep {
  TopoDS_Shape shape;   // a solid (a shell or faces when the mesh is open)
  json surfaces;        // how many faces of each kind: {"plane": 6, "cylinder": 2, ..., "facet": 0}
  bool faceted = false; // the fitted faces did not make a valid solid: planar facets only (coplanar triangles merged)
};
// The general path; throws on an empty mesh or when nothing valid can be built.
MeshBrep mesh_to_brep(const TriMesh& mesh, const MeshSolidOptions& options, const std::function<bool()>& cancelled = {});

}  // namespace opad
