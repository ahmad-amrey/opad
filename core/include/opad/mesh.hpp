#pragma once
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

Mesh tessellate(const TopoDS_Shape& s, double linear_tol, double angular_deg = 20.0);
// Tessellates a body-store entry through the user cache (F10).
Mesh tessellate_body(const Document& doc, const std::string& key, double linear_tol);

}  // namespace opad
