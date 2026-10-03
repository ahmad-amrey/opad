#pragma once
// Snaps on a 2D drawing (TODO 11 UI-78; the 2D mode can share it): the points a pointer locks onto in a display list -
// ends, midpoints, centres, quadrants, intersections and the nearest point on a curve - kept in a uniform grid, so a
// query near the pointer looks at a few cells, never at every primitive. Qt-free. Built once (a worker), queried as often
// as the pointer moves (const, any thread).
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "opad/drawing/display.hpp"

namespace opad::drawing {

enum class SnapKind : uint8_t { End, Mid, Centre, Quadrant, Intersection, Nearest };
constexpr unsigned kAllSnaps = 0x3F;
constexpr unsigned snap_bit(SnapKind k) { return 1u << static_cast<unsigned>(k); }
const char* snap_kind_name(SnapKind k);  // end, mid, centre, quadrant, intersection, nearest

struct Snap {
  SnapKind kind = SnapKind::Nearest;
  Vec2 at{0, 0};
  double distance = 0;
  std::string source;  // the record that drew the curve (Prim::source)
};

class SnapIndex {
 public:
  SnapIndex() = default;
  // The curves of `d` (text, fills and images give none), followed within `tol` (drawing units). Lines give their ends
  // and middle; arcs their ends, middle, centre and the quadrants they pass; circles and ellipses their centre and
  // quadrants; polylines their ends (every corner when they have few) and middle; splines their ends and middle.
  explicit SnapIndex(const Display& d, double tol = 0.01);
  // Within `radius` of p, of `kinds`: the closest of the points (ends, middles, centres, quadrants, the crossings of two
  // curves), else the closest point on a curve. Null when nothing is that close.
  std::optional<Snap> find(Vec2 p, double radius, unsigned kinds = kAllSnaps) const;
  size_t points() const { return m_points.size(); }
  size_t segments() const { return m_segments.size(); }
  bool empty() const { return m_points.empty() && m_segments.empty(); }

 private:
  struct Point {
    Vec2 at;
    SnapKind kind;
    int source;
  };
  struct Segment {
    Vec2 a, b;
    int curve, source;
  };
  long long key(long ix, long iy) const { return (static_cast<long long>(ix) << 32) ^ static_cast<unsigned long>(iy); }
  long cell(double v) const;
  void put(std::unordered_map<long long, std::vector<int>>& grid, Vec2 p, int i);
  std::vector<std::string> m_sources;
  std::vector<Point> m_points;
  std::vector<Segment> m_segments;
  double m_cell = 1;
  std::unordered_map<long long, std::vector<int>> m_pointGrid, m_segmentGrid;
};

}  // namespace opad::drawing
