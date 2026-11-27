#pragma once
// Deviation (surface comparison): how far body A ("from", typically a 3D scan) lies from body B ("to", typically the CAD
// rebuilt from it), point by point and signed, so material missing (A inside B) is told apart from material added (A
// outside B). Worker-only: B's triangles go into a BVH (a mesh as stored, a solid meshed finely on a copy), A is sampled
// (a mesh's vertices, a solid's faces by area; at most `sample` points, the rest skipped and said so), and every sample
// asks the BVH for its nearest point. The sign comes from B's angle-weighted pseudo-normals; where the nearest point lies on
// an open border of B (a scan's hole, an open shell) it is undefined and counted apart. Small sets on a solid B are
// measured exactly on its faces (BRepExtrema) after the BVH found the face.
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "document.hpp"
#include "scene.hpp"

namespace opad::deviation {

// World triangles of one side: welded vertices (float, mm), three indices a triangle, the B-rep face ordinal of each
// triangle (a solid's TopExp::MapShapes order; for a mesh the facet ordinal, i.e. the triangle's own index).
struct Triangles {
  std::vector<float> xyz;
  std::vector<uint32_t> tri;
  std::vector<int> face;
  bool cloud = false;  // a point cloud: xyz holds its points in node order (no triangles, no weld)
  size_t triangle_count() const { return tri.size() / 3; }
  size_t vertex_count() const { return xyz.size() / 3; }
  void append(const Triangles& other);  // the other's faces are offset past this one's highest
};

// The triangles of a world shape: a mesh as stored, a solid meshed on a copy at `deflection` (the cached shape keeps its
// own triangulation; a point cloud, nodes without triangles, gives its points only: `cloud`). Vertices that coincide (shared
// face borders) are welded.
Triangles triangles_of(const TopoDS_Shape& world, double deflection);

// Nearest-point index over a triangle set.
class Surface {
 public:
  struct Hit {
    double distance = std::numeric_limits<double>::infinity();  // signed: > 0 outside (along the normals), < 0 inside
    bool sign_defined = true;  // false: the nearest point lies on an open border, the sign is a guess (|distance| holds)
    gp_Pnt point;              // nearest point on the surface
    gp_Dir normal;             // the pseudo-normal there (face, edge or vertex)
    int triangle = -1;         // -1: nothing within reach
    bool found() const { return triangle >= 0; }
  };
  static std::shared_ptr<const Surface> build(Triangles triangles, const std::function<bool()>& cancelled = {});
  // Nearest point within `reach` (infinity: anywhere). Thread-safe (const).
  Hit nearest(const gp_Pnt& p, double reach = std::numeric_limits<double>::infinity()) const;
  size_t triangle_count() const { return m_tri.size() / 3; }
  size_t border_edges() const { return m_border; }  // triangle sides with no neighbour: 0 for a closed surface
  bool closed() const { return m_border == 0; }
  int face_of(int triangle) const { return triangle >= 0 && size_t(triangle) < m_face.size() ? m_face[size_t(triangle)] : -1; }
  const Bnd_Box& box() const { return m_box; }
  double area() const { return m_area; }
  bool flipped() const { return m_flipped; }  // its triangles faced inwards (a negative volume): turned round

 private:
  struct Node {
    float lo[3], hi[3];
    uint32_t first = 0, count = 0;  // leaf: triangles [first, first + count) in m_order; inner: count 0, children first, first + 1
  };
  void buildTree();
  std::vector<float> m_xyz;
  std::vector<uint32_t> m_tri;
  std::vector<int> m_face;
  std::vector<float> m_normal;     // unit normal per triangle
  std::vector<float> m_vnormal;    // angle-weighted pseudo-normal per vertex (unit)
  std::vector<int32_t> m_next;     // per triangle side (3t + k, side k = corners k, k+1): the triangle across, -1 none
  std::vector<uint8_t> m_vborder;  // per vertex: on an open border
  std::vector<uint32_t> m_order;
  std::vector<Node> m_nodes;
  size_t m_border = 0;
  Bnd_Box m_box;
  double m_area = 0;
  bool m_flipped = false;
};

// Points of side A to measure.
struct Samples {
  std::vector<gp_Pnt> points;
  std::vector<int> face;    // B-rep face / facet ordinal each point lies on (-1: unknown)
  std::string source;       // "mesh vertices" or "surface samples"
  size_t population = 0;    // vertices (mesh) or triangles meshed (solid) they were taken from
  size_t region_population = 0;  // of those, in the region (= population without one)
  bool subsampled = false;
};

// Which part of A to measure: faces / facets by ordinal (empty: all) and a world box (void: everywhere).
struct Region {
  std::vector<int> faces;
  Bnd_Box box;
  bool empty() const { return faces.empty() && box.IsVoid(); }
};

// `mesh`: A is a mesh (its welded vertices are the samples); else area-weighted points on its triangles. At most `limit`.
Samples sample(const Triangles& a, bool mesh, size_t limit, const Region& region = {});

struct Values {
  std::vector<float> value;      // signed mm per sample (NaN: ignored, farther than max_distance or nothing found)
  std::vector<gp_Pnt> nearest;   // on B
  std::vector<uint8_t> undefined;  // sign undefined
  size_t ignored = 0, undefined_count = 0;
  bool exact = false;  // measured on B's exact faces
};
// Every sample against `to`; `exact_faces` (B's faces, world; empty: none) refines each value on the face the BVH found.
Values measure(const Samples& from, const Surface& to, double max_distance, const std::vector<TopoDS_Shape>& exact_faces = {},
               const std::function<bool()>& cancelled = {});

struct Statistics {
  size_t count = 0;
  double mean = 0, mean_abs = 0, median = 0, median_abs = 0, rms = 0, std_dev = 0, min = 0, max = 0, max_abs = 0, p90_abs = 0, p95_abs = 0,
         p99_abs = 0;
  double within = 0, outside = 0, inside = 0;  // fractions: |d| <= tol, d > tol, d < -tol
};
Statistics statistics(const std::vector<float>& values, double tolerance);

struct Histogram {
  double from = 0, to = 0, bin = 0;
  std::vector<size_t> counts;
  size_t below = 0, above = 0;
};
// `range` the half width (<= 0: chosen from the values: the 99th percentile of |d|, at least twice the tolerance).
Histogram histogram(const std::vector<float>& values, double tolerance, int bins, double range = 0);

struct Spot {
  gp_Pnt at, nearest;
  double value = 0;
  size_t points = 0;  // samples beyond the tolerance within the cluster's radius
};
// The worst places beyond the tolerance, at most `count`, each at least `radius` from the others (so they are not all in
// one blob), worst first.
std::vector<Spot> worst_spots(const Samples& s, const Values& v, double tolerance, int count, double radius);

// Best fit (ICP, point-to-plane) of A's points onto B: the rigid motion of A that brings them nearest. Pairs farther than
// `reach` (infinity: none) are left out from the start, and later ones beyond three times the median distance.
struct Fit {
  gp_Trsf motion;
  double rms_before = 0, rms_after = 0;
  int iterations = 0, pairs = 0;
  bool converged = false;
};
Fit best_fit(const std::vector<gp_Pnt>& points, const Surface& to, double reach = std::numeric_limits<double>::infinity(), int max_iterations = 60,
             const std::function<bool()>& cancelled = {});

// What the command and the app's tool run: args from, to (body or component ids, or arrays of them), tolerance (mm, default
// 0.1), both_ways, max_distance (mm, 0: none), region ([refs of A's faces] or {"min":[x,y,z],"max":[x,y,z]}), sample (most
// points, default 500000), worst (spots, default 8), bins (default 40), range (histogram half width, mm), align (best fit
// first), exact ("auto" | true | false).
struct Study {
  json report;
  std::shared_ptr<const Surface> to, from;  // B's index; A's when both ways
  Samples samples;
  Values values;
  gp_Trsf alignment;  // applied to A before measuring (identity without align)
  std::vector<std::string> from_bodies, to_bodies;
  double size = 0;  // the from side's box diagonal (the worst spots are apart by 6% of it, at least ten tolerances)
  size_t limit = 0;
};
using ProgressFn = std::function<void(const std::string& phase, int percent)>;
Study run(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled = {}, const ProgressFn& progress = {});
json command(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled = {});
// The from side's stats, histogram and worst spots again for another tolerance or range (the values stay): cheap, no BVH.
json summary(const Study& study, double tolerance, int bins = 40, double range = 0, int worst = 8);

// The deflection a solid is meshed at for a tolerance and a size (its box's diagonal).
double deflection_for(double tolerance, double diagonal);

}  // namespace opad::deviation
