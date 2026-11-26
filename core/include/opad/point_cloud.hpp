#pragma once
// Point clouds (raw scans) as a body type of their own. For reverse engineering the measured points are the truth: a
// scanning app's mesh export rounds edges, fills small holes and smooths detail away.
//
// A cloud body is a mesh body with nodes and no triangles: one face without a surface whose Poly_Triangulation holds the
// points (single precision), their normals when known, and in its UV nodes the colour (U = 0xRRGGBB, exact in a float) and
// one scalar per point (V: a scanner's intensity, a deviation). It rides the shape cache, the BREP and binary stores, the
// viewer cache and make_editable as any mesh does, and a point is a mesh node: Ref "uuid/vertex/<i>" is point i. The body's
// meta says "representation": "points" with "points" (count) and "attributes".
//
// Region references (a lasso, a brush, a box of points) name point ordinals as ranges, as a fitted reference names
// triangles: {"body": <node>, "kind": "region", "vertices": [[first, last], ...]} (inclusive). Everything that walks a
// whole cloud is worker work.
#include <TopoDS_Shape.hxx>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "json.hpp"
#include "document.hpp"
#include "scene.hpp"
#include "util.hpp"

namespace opad::cloud {

using Cancel = std::function<bool()>;  // true: stop (the function throws Error("cancelled"))

struct Points {
  std::vector<float> xyz;      // 3 per point, mm
  std::vector<uint8_t> rgb;    // 3 per point (0-255), or empty
  std::vector<float> normals;  // 3 per point (unit), or empty
  std::vector<float> scalar;   // 1 per point, or empty
  std::string scalar_name;     // what the scalar is: "intensity", "deviation", ...
  // Where the points are: a point's place is xyz + offset (mm). Readers keep far coordinates (survey data, a scanner's
  // world frame) near zero this way, since a float holds a millimetre only within 16 km of its zero; the import puts the
  // offset into the body's placement.
  Vec3 offset{0, 0, 0};

  size_t size() const { return xyz.size() / 3; }
  bool empty() const { return xyz.empty(); }
  bool colored() const { return !rgb.empty() && rgb.size() == xyz.size(); }
  bool has_normals() const { return !normals.empty() && normals.size() == xyz.size(); }
  bool has_scalar() const { return !scalar.empty() && scalar.size() == size(); }
  Vec3 point(size_t i) const { return {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]}; }
  // Drops attributes whose size does not match the points (a reader that found them on some points only).
  void normalise();
  // Moves every point (and turns the normals) by a rigid or scaling matrix.
  void transform(const Mat4& m);
};

// ---------------------------------------------------------------- the body
TopoDS_Shape to_shape(const Points& points);
bool is_cloud(const TopoDS_Shape& shape);  // a mesh shape whose triangulations have nodes and no triangles
size_t point_count(const TopoDS_Shape& shape);
// The points of a cloud shape in the shape's own frame (its faces' locations applied). `attributes` false: positions only.
Points from_shape(const TopoDS_Shape& shape, bool attributes = true);
// What a cloud body's meta says about it: {"representation": "points", "points": N, "attributes": [...], "scalar": name}.
json meta_of(const Points& points);
json meta_of_shape(const TopoDS_Shape& shape);  // the same from a cloud shape, without copying its points
// A cloud shape as body-store text: BREP format 3 with the triangulation (normals kept), floats exact.
std::string to_brep(const TopoDS_Shape& shape);
// The axis-aligned box of the points: {min, max} (both zero for an empty cloud).
std::array<Vec3, 2> bounds(const Points& points);

// ---------------------------------------------------------------- files
// Clouds read in file units scaled to mm and turned to Z up: `scale` (0: the format's own, PLY/XYZ/PTS/CSV mm, E57 and
// LAS metres) and `up` ("z", "y"; empty: the format's own, Z).
struct ReadOptions {
  double scale = 0;
  std::string up;
  std::function<bool(double, const std::string&)> progress;  // false: cancel
};
// Extensions read as clouds (".xyz", ".pts", ".csv", ".e57", ".las"; ".laz" is named and refused). PLY files are clouds
// when they have no faces (ply_is_cloud).
const std::vector<std::string>& cloud_extensions();
bool is_cloud_extension(const std::filesystem::path& file);
bool ply_is_cloud(const std::filesystem::path& file);  // a PLY without faces (reads the header only)
Points read_file(const std::filesystem::path& file, const ReadOptions& options = {});
// The PLY parser shared with the mesh reader (formats.cpp): vertices with their attributes; `triangles` (may be null) get
// the faces fanned into triangles.
Points read_ply_data(const std::string& data, const ReadOptions& options, std::vector<uint32_t>* triangles = nullptr,
                     std::vector<uint32_t>* triangle_rgb = nullptr);
Points read_text_data(const std::string& data, const std::string& extension, const ReadOptions& options);
Points read_las_data(const std::string& data, const ReadOptions& options);
Points read_e57_data(const std::string& data, const ReadOptions& options);
// Written in mm by extension: .ply (binary little-endian, every attribute), .xyz (x y z [r g b] [nx ny nz]), .pts
// (count line, then x y z intensity r g b), .csv (a header row), .e57 (metres, one scan).
const std::vector<std::string>& writable_extensions();
void write_file(const std::filesystem::path& file, const Points& points);
std::string write_ply_data(const Points& points, bool binary = true);
std::string write_e57_data(const Points& points);

// ---------------------------------------------------------------- regions
struct Region {
  std::vector<std::array<uint32_t, 2>> ranges;  // inclusive, sorted, disjoint, not touching
  size_t count() const;
  bool empty() const { return ranges.empty(); }
  bool contains(uint32_t i) const;
  std::vector<uint32_t> indices() const;
  static Region of(std::vector<uint32_t> indices);  // any order, repeats allowed
  Region unite(const Region& o) const;
  Region subtract(const Region& o) const;
  json to_json() const;                   // [[first, last], ...]
  static Region from_json(const json& j); // the same, or plain ordinals; throws on anything else
};
// A region reference ({"body", "kind": "region", "vertices"}) and back; `points` (when given) checks the ordinals.
json region_ref(const std::string& body, const Region& region);
Region region_of_ref(const json& ref, size_t points = 0);
// The points of a region: inside an axis-aligned box, a sphere, or a band about a plane.
Region in_box(const Points& p, const Vec3& lo, const Vec3& hi);
Region in_sphere(const Points& p, const Vec3& centre, double radius);
Region in_band(const Points& p, const Vec3& origin, const Vec3& normal, double half_band);
// Inside a screen polygon (a lasso): each point taken through `view` (row-major 4x4, world -> normalised device
// coordinates, perspective divided) into x, y, and kept when inside `polygon` (NDC). Multi-threaded.
Region in_polygon(const Points& p, const std::array<double, 16>& view, const std::vector<std::array<double, 2>>& polygon,
                  const Cancel& cancel = {});
Points subset(const Points& p, const Region& keep);
Points without(const Points& p, const Region& drop);

// ---------------------------------------------------------------- neighbours
// A uniform grid over the points (cells about `cell` mm, or chosen from the density when 0), for radius and k-nearest
// queries. Holds a copy of the positions in cell order. Thread-safe to query.
class Grid {
 public:
  explicit Grid(const Points& p, double cell = 0);
  double cell() const { return m_cell; }
  // Up to k nearest other points of point i (or of a position), nearest first; their ordinals and squared distances.
  void nearest(const float* q, size_t k, std::vector<uint32_t>& out, std::vector<float>* d2 = nullptr, int64_t self = -1) const;
  void within(const float* q, double radius, std::vector<uint32_t>& out) const;
  size_t size() const { return m_n; }

 private:
  int64_t key(int64_t x, int64_t y, int64_t z) const { return (x * m_dim[1] + y) * m_dim[2] + z; }
  size_t m_n = 0;
  double m_cell = 1;
  double m_lo[3] = {0, 0, 0};
  int64_t m_dim[3] = {1, 1, 1};
  std::vector<uint32_t> m_start;  // per cell (dense when it fits, else sorted keys below)
  std::vector<int64_t> m_keys;    // sparse cells: sorted keys, m_start per key + one
  bool m_dense = true;
  std::vector<uint32_t> m_order;  // point ordinals in cell order
  std::vector<float> m_xyz;       // positions in cell order
  bool cellRange(int64_t x, int64_t y, int64_t z, uint32_t& a, uint32_t& b) const;
  void cellOf(const float* q, int64_t c[3]) const;
};
// The typical distance between neighbouring points (median nearest-neighbour distance of a sample).
double spacing(const Points& p);

// ---------------------------------------------------------------- tools (workers)
struct Slice {
  Frame frame;                  // origin on the cutting plane, x/y in it
  std::vector<uint32_t> indices;  // the points within the band
  std::vector<float> uv;        // their places on the plane (2 per point, frame coordinates)
};
// The points within ±half_band of the plane through `origin` square to `normal`, projected onto it. `x` (optional) the
// frame's X direction in the plane.
Slice slice(const Points& p, const Vec3& origin, const Vec3& normal, double half_band, const Vec3& x = {0, 0, 0});
// A slice as polylines: points chained to their nearest neighbours (a jump over `gap` starts a new chain; 0: 4 x the
// spacing), thinned to within `tolerance` of the chain (Douglas-Peucker; 0: twice the spacing, at least 1/2000 of its size). Closed loops end on their first
// point. Chains of fewer than three points are left out.
std::vector<std::vector<std::array<double, 2>>> profile(const std::vector<float>& uv, double gap = 0, double tolerance = 0);
// Statistical outliers: points whose mean distance to their k nearest neighbours is over the cloud's mean + std_ratio x its
// standard deviation.
Region outliers(const Points& p, int k = 16, double std_ratio = 2.0, const Cancel& cancel = {});
// One point per voxel of `size` mm: the point nearest the voxel's centroid (an actual measured point, attributes kept).
Points voxel_downsample(const Points& p, double size, const Cancel& cancel = {});
// Normals by principal component analysis of the k nearest neighbours, turned consistently: across neighbours by a
// spanning walk from the point farthest out, which faces away from the centre.
void estimate_normals(Points& p, int k = 16, const Cancel& cancel = {});
// ---------------------------------------------------------------- reports (commands, the live MCP)
// A cloud body's points turned to world orientation, its placement's translation kept as their offset (exact far out).
Points world_points(const Document& doc, const Scene& scene, const std::string& node, bool attributes = true);
// The cloud bodies selected (or all of them) written to one file by its extension (write_file), in world mm.
json export_points(const Document& doc, const Scene& scene, const std::filesystem::path& out, const std::vector<std::string>& select);
// {points, bounds {min, max}, size, attributes, scalar, spacing (when `measure`)}
json describe(const Points& p, bool measure = false);
// Up to `limit` points from `offset` of a region (null: all) as {count, offset, points: [[x,y,z],...], colors?, normals?}.
json points_json(const Points& p, const Region* region, size_t offset, size_t limit);

}  // namespace opad::cloud
