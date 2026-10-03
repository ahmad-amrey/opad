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
#include <tuple>
#include <vector>

#include "opad/geometry.hpp"
#include "opad/step_io.hpp"

namespace opad::detail {

// A drawing read from a file: per layer, its geometry grouped by colour (edges for curves, faces for fills and text)
// and, for DXF entities that set their own, linetype and lineweight.
struct Drawing {
  static constexpr uint32_t kNoColor = 0xFF000000u;  // drawn in the viewer's own drawing colour
  struct Pen {
    uint32_t color = kNoColor;  // 0xRRGGBB or kNoColor
    std::string linetype;       // its own linetype ("" by layer): the body's `line` (Node::line)
    int lineweight = -1;        // its own in 1/100 mm, -3 the default; -1 by layer
    bool operator<(const Pen& o) const { return std::tie(color, linetype, lineweight) < std::tie(o.color, o.linetype, o.lineweight); }
  };
  std::map<std::string, std::map<Pen, TopoDS_Compound>> layers;  // layer -> how it is drawn -> geometry
  std::map<std::string, std::vector<double>> patterns;  // DXF: dashes of the linetypes bodies name, as Node::layer's pattern
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
  void add(const std::string& layer, const TopoDS_Shape& shape, uint32_t color = kNoColor) { add(layer, shape, Pen{color, {}, -1}); }
  void add(const std::string& layer, const TopoDS_Shape& shape, const Pen& pen);
  void line(const std::string& layer, double x, double y, double u, double v);
  void circle(const std::string& layer, double x, double y, double r);
  bool empty() const { return layers.empty(); }
};

// DXF as AutoCAD, LibreDWG's dwg2dxf and the ODA converter write it: everything model space shows.
Drawing read_dxf(const std::filesystem::path& file, const ImportOptions& options);

}  // namespace opad::detail
