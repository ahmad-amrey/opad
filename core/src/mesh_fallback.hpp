#pragma once
#include <TopoDS_Face.hxx>

namespace opad::detail {
// Worker-only: triangulates a face from its boundary alone (mesh_fallback.cpp), for faces BRepMesh left without
// triangles. Stores the triangulation on the face; false when the face has no usable boundary.
bool triangulate_from_boundary(TopoDS_Face face, double tolerance, double angle);
}  // namespace opad::detail
