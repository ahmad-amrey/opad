#pragma once
// Netgen behind a plain interface (its headers stay in fea_mesh.cpp): a volume mesh of a shape's solids.
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>

#include <array>
#include <functional>
#include <vector>

#include "opad/util.hpp"

namespace opad::sim {

struct VolumeMesh {
  std::vector<Vec3> nodes;
  int tet_nodes = 10;                       // 4 or 10, in CalculiX's (Abaqus) order
  std::vector<std::array<int, 10>> tets;    // node indices (0-based)
  std::vector<int> tet_solid;               // the solid each is in: index into `solids` (0-based)
  int tri_nodes = 6;                        // 3 or 6: corners first, then the mid-side nodes
  std::vector<std::array<int, 6>> tris;     // surface elements
  std::vector<int> tri_face;                // the face each is on: index into `faces` (0-based)
  TopTools_IndexedMapOfShape faces, solids;  // the shape's, as the mesh numbers them
};

// maxh: the largest element edge (mm); grading 0..1; second_order: curved quadratic elements. Throws Error when Netgen
// fails or is not in this build.
VolumeMesh mesh_solids(const TopoDS_Shape& shape, double maxh, double grading, bool second_order, const std::function<bool()>& cancelled);

bool netgen_available();

}  // namespace opad::sim
