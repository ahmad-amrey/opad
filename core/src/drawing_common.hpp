#pragma once
// Shared by the 2D drawing readers (drawing_io.cpp: SVG and the import; dxf_reader.cpp: DXF); not a public header.
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_XYZ.hxx>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "opad/geometry.hpp"
#include "opad/step_io.hpp"

namespace opad::detail {

// A drawing read from a file: per layer, its geometry grouped by colour (edges for curves, faces for fills and text).
struct Drawing {
  static constexpr uint32_t kNoColor = 0xFF000000u;  // drawn in the viewer's own drawing colour
  std::map<std::string, std::map<uint32_t, TopoDS_Compound>> layers;  // layer -> 0xRRGGBB or kNoColor -> geometry
  std::map<std::string, bool> visible;
  std::map<std::string, json> layer_info;  // DXF: the layer table's entry (Node::layer); "locked" also locks the layer's node
  std::map<std::string, uint32_t> by_layer;  // DXF: the colour group of a layer's BYLAYER entities (Node::by_layer)
  std::map<std::string, json> images;
  std::vector<std::string> warnings;
  // Where the geometry's own origin lies in the file's coordinates (mm). A drawing far from its origin (survey or
  // map coordinates) is read near (0,0) instead, where 32-bit display coordinates still resolve; its import op puts it back.
  gp_XYZ origin{0, 0, 0};
  BRep_Builder builder;
  Mat4 transform;  // SVG: the current element's transform, applied by add()
  void add(const std::string& layer, const TopoDS_Shape& shape, uint32_t color = kNoColor);
  void line(const std::string& layer, double x, double y, double u, double v);
  void circle(const std::string& layer, double x, double y, double r);
  bool empty() const { return layers.empty(); }
};

// DXF as AutoCAD, LibreDWG's dwg2dxf and the ODA converter write it: everything model space shows.
Drawing read_dxf(const std::filesystem::path& file, const ImportOptions& options);

}  // namespace opad::detail
