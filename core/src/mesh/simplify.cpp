// Simplify a mesh to a triangle count or a tolerance (opad/mesh_edit.hpp) with meshoptimizer's edge-collapse simplifier
// (quadric error), positions handed over in floats about the box's middle, the error asked and reported in millimetres.
#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>

#include "opad/mesh_edit.hpp"
#include "opad/util.hpp"

namespace opad::meshops {

Mesh simplify(const Mesh& m, const SimplifyOptions& opt, double* error_mm, const Cancel& cancel) {
  if (m.empty()) throw Error("the mesh has no triangles");
  const Bnd_Box box = bounds(m);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, cz = (z0 + z1) / 2;
  const double diag = std::sqrt(box.SquareExtent());
  std::vector<float> pos(m.xyz.size());
  for (size_t v = 0; v < m.vertices(); ++v) {
    pos[v * 3] = static_cast<float>(m.xyz[v * 3] - cx);
    pos[v * 3 + 1] = static_cast<float>(m.xyz[v * 3 + 1] - cy);
    pos[v * 3 + 2] = static_cast<float>(m.xyz[v * 3 + 2] - cz);
  }
  if (cancel && cancel()) throw Error("cancelled");
  size_t target = opt.target_triangles * 3;
  double tolerance = opt.tolerance;
  if (target == 0 && tolerance <= 0) tolerance = diag * 0.01;
  if (target == 0) target = 3;            // as few as the tolerance allows
  if (tolerance <= 0) tolerance = diag;   // the count decides
  if (target >= m.tri.size()) throw Error("the mesh already has " + std::to_string(m.triangles()) + " triangles, no more than the target");
  std::vector<unsigned int> out(m.tri.size());
  unsigned int options = meshopt_SimplifyErrorAbsolute;
  if (opt.keep_borders) options |= meshopt_SimplifyLockBorder;
  float err = 0;
  const size_t n = meshopt_simplify(out.data(), m.tri.data(), m.tri.size(), pos.data(), m.vertices(), sizeof(float) * 3, target,
                                    static_cast<float>(tolerance), options, &err);
  if (cancel && cancel()) throw Error("cancelled");
  if (n == 0) throw Error("simplifying left no triangles: raise the target or lower the tolerance");
  out.resize(n);
  std::vector<int64_t> map(m.vertices(), -1);
  Mesh r;
  r.tri.reserve(n);
  for (unsigned int v : out) {
    if (map[v] < 0) {
      map[v] = static_cast<int64_t>(r.vertices());
      r.xyz.insert(r.xyz.end(), {m.xyz[size_t(v) * 3], m.xyz[size_t(v) * 3 + 1], m.xyz[size_t(v) * 3 + 2]});
    }
    r.tri.push_back(static_cast<uint32_t>(map[v]));
  }
  if (error_mm) *error_mm = err;
  // Edge collapses can leave a folded pair (two triangles over the same corners) where a thin part closed up: cleaned like
  // Repair mesh does, the small gaps that leaves closed, so a closed mesh stays closed.
  RepairOptions clean;
  clean.orient = true;
  clean.fill_holes = true;
  clean.max_hole_edges = 16;
  clean.count_crossings = false;
  return repair(r, clean, cancel).mesh;
}

}  // namespace opad::meshops
