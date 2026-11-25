#pragma once
// OpenFOAM's files and programs, shared by the thermal study's air (sim/cfd.cpp) and the flow study (sim/flow.cpp): a
// case's dictionaries written, its mesh and fields read back, its programs run with their logs followed. Internal to the core.
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "opad/sim/cfd.hpp"

namespace opad::sim::foam {

namespace fs = std::filesystem;
using V = Vec3;

std::string num(double v);       // 10 significant digits
std::string vec(const V& v);     // "(x y z)"
std::string header(const std::string& cls, const std::string& object, const std::string& location = {});
// A dictionary or field file: its FoamFile header, then body.
void put(const fs::path& file, const std::string& cls, const std::string& object, const std::string& body);

// The list after "N (" in an OpenFOAM file (points, faces, labels, a field), as text from its opening bracket.
std::string_view list_after(std::string_view t, size_t from = 0);
size_t count_before(std::string_view list_from_bracket, std::string_view whole);
std::vector<V> read_points(const std::string& t);
std::vector<std::vector<int>> read_faces(const std::string& t);
std::vector<double> read_numbers(std::string_view whole, std::string_view l);
std::vector<int> read_labels(const std::string& t);
// A field's internal values: scalars, or vectors flattened (3 per cell); a uniform field gives its one value n times.
std::vector<double> read_internal(const std::string& t, size_t cells, int width);
// A patch's values in a field file's boundaryField (scalar), empty when the patch has none written.
std::vector<double> read_patch(const std::string& t, const std::string& patch);

// A region's mesh: its cells' centres and volumes (from its faces), in the mesh's units.
struct Cells {
  std::vector<V> centre;
  std::vector<double> volume;
};
Cells read_cells(const fs::path& poly);

// Nearest of a set of points: a uniform grid of buckets.
class Nearest {
 public:
  Nearest(const std::vector<V>& pts, double cell) : m_pts(pts), m_cell(std::max(cell, 1e-6)) {
    for (size_t i = 0; i < pts.size(); ++i) m_grid[key(pts[i])].push_back(i);
  }
  size_t operator()(const V& p) const { return (*this)(p, nullptr); }
  // The nearest that `ok` accepts (npos when none is within 64 buckets).
  static constexpr size_t npos = size_t(-1);
  size_t operator()(const V& p, const std::function<bool(size_t)>& ok) const {
    size_t best = ok ? npos : 0;
    double bd = 1e300;
    for (int r = 0; r < 64 && (bd == 1e300 || double(r - 1) * m_cell < std::sqrt(bd)); ++r) {
      const auto k = key(p);
      for (int i = -r; i <= r; ++i)
        for (int j = -r; j <= r; ++j)
          for (int l = -r; l <= r; ++l) {
            if (std::max({std::abs(i), std::abs(j), std::abs(l)}) != r) continue;
            const auto it = m_grid.find({k[0] + i, k[1] + j, k[2] + l});
            if (it == m_grid.end()) continue;
            for (size_t n : it->second)
              if (const double d = dist2(m_pts[n], p); d < bd && (!ok || ok(n))) bd = d, best = n;
          }
    }
    return best;
  }

 private:
  static double dist2(const V& a, const V& b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]); }
  std::array<long, 3> key(const V& p) const { return {long(std::floor(p[0] / m_cell)), long(std::floor(p[1] / m_cell)), long(std::floor(p[2] / m_cell))}; }
  const std::vector<V>& m_pts;
  double m_cell;
  std::map<std::array<long, 3>, std::vector<size_t>> m_grid;
};

// The newest time directory of a case ("" when none).
std::string latest_time(const fs::path& dir);

// A mesh's patches (constant/polyMesh/boundary): name, type, faces.
struct Patch {
  std::string name, type;
  size_t faces = 0, start = 0;
};
std::vector<Patch> read_boundary(const std::string& t);

// Runs one of OpenFOAM's programs on the case `where` (-case where), its log in where/log.<app>. `at`: where the study is
// when the program starts; `to`: where it is once it is done (its share of the bar, followed through its log: the mesher's
// phases, a solver's iterations against its controlDict's endTime); a program of seconds keeps `at`. Throws Error with the
// log's FOAM FATAL text when it fails, "cancelled" when progress says stop. Returns the log.
std::string run(const OpenFoam& foam, const std::string& app, const std::vector<std::string>& args, const fs::path& where, double at, double to,
                int timeout_ms, const Progress& progress);

}  // namespace opad::sim::foam
