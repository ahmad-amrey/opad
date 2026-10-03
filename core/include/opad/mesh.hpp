#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "document.hpp"

class TopoDS_Shape;

namespace opad {

struct Mesh {
  std::vector<float> positions;   // xyz
  std::vector<float> normals;     // xyz per vertex
  std::vector<uint32_t> indices;  // triangles
  struct FaceRange { int face; uint32_t first; uint32_t count; };
  std::vector<FaceRange> faces;   // triangle ranges per B-rep face ordinal
  size_t triangle_count() const { return indices.size() / 3; }
  bool empty() const { return indices.empty(); }
  std::string serialize() const;
  static Mesh deserialize(const std::string& blob);
};

// The colours a file gave single faces of a body (body meta "face_colors", UI-74): {"colors": [[r,g,b] sRGB, ...],
// "runs": [count, colour, count, colour, ...]} over the body's faces in ordinal order (TopExp::MapShapes; a mesh's pieces,
// not its facets). Colour -1, and every face past the runs, is the body's own colour, so recolouring a body keeps the
// faces its file coloured otherwise (a connector's gold pins on a black housing).
struct FaceColors {
  std::vector<std::array<double, 3>> colors;
  std::vector<int> face;  // colour index per face ordinal, -1 = the body's colour
  bool empty() const;
  int at(int ordinal) const { return ordinal >= 0 && static_cast<size_t>(ordinal) < face.size() ? face[static_cast<size_t>(ordinal)] : -1; }
  json to_json() const;                       // null when no face has a colour of its own
  static FaceColors from_json(const json& j);  // anything malformed reads as no face colours
};
FaceColors face_colors(const Document& doc, const std::string& key);  // from the body entry's meta

struct MeshingReport {
  int status = 0;
  int recovered_faces = 0;
  int incomplete_cones = 0;
};
// Worker-only: prepare triangulations, recovering incomplete conical faces without
// changing the B-rep, subshape identities or analytic geometry.
MeshingReport mesh_shape(const TopoDS_Shape& s, double linear_tol, double angular_deg = 20.0);
// Worker-only, after mesh_shape: re-triangulates cylinders and extrusions of any curve as upright strips between their
// two rims, so seen along their direction they have no area (BRepMesh's leaning triangles showed as slivers past the
// rims of extruded profiles). Faces whose rims are not sampled at matching points keep their mesh. Returns how many
// faces changed. The display uses it; tessellate() does not, so headless renders and exports stay as they were.
int straighten_ruled_faces(const TopoDS_Shape& s);
Mesh tessellate(const TopoDS_Shape& s, double linear_tol, double angular_deg = 20.0);
// Tessellates a body-store entry through the user cache (F10).
Mesh tessellate_body(const Document& doc, const std::string& key, double linear_tol);

}  // namespace opad
