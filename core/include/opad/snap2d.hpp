#pragma once
// Object snaps in a plane (UI-90): the sketch editor's snap set (endpoint, midpoint, centre, quadrant, intersection,
// perpendicular and tangent from the last point, nearest; its settings sketch/snap/<kind_name>) over plain 2D curves, so
// the Review picks on drawings and sketches and the plot window share one engine and one order: a point (an end, a centre)
// within the aperture wins, then the nearest midpoint, quadrant, intersection, perpendicular or tangent point, then the
// nearest point on a curve. Qt-free; an Index is built once per shape (on a worker) and asked on every mouse move: a grid
// of short pieces, so a query looks at the curves near the point only.
#include <memory>
#include <vector>

class TopoDS_Shape;

namespace opad::snap2d {
enum class Kind { None, Endpoint, Midpoint, Center, Quadrant, Intersection, Nearest, Perpendicular, Tangent };
const char* kind_name(Kind kind);  // "endpoint", "midpoint", "center", "quadrant", "intersection", "nearest", "perpendicular", "tangent"

struct P {
  double x = 0, y = 0;
};
struct Kinds {
  bool endpoint = true, midpoint = true, center = true, quadrant = true, intersection = true, nearest = true, perpendicular = true, tangent = true;
  bool on(Kind kind) const;
};
struct Snap {
  Kind kind = Kind::None;
  P at;
  int source = -1, curve = -1;            // what it lies on (an intersection: the first of the two curves)
  int otherSource = -1, otherCurve = -1;  // an intersection's second curve
  explicit operator bool() const { return kind != Kind::None; }
};

class Index {
 public:
  struct Point {
    P at;
    Kind kind;
    int curve;
  };
  struct Piece {  // a straight piece of a curve, no longer than a grid cell
    P a, b;
    int curve;
  };
  struct Round {  // what a curve is when it is a line or a circle (exact intersections and nearest points)
    bool round = false, line = false;
    P center;
    double radius = 0, from = 0, sweep = 0;
    P a, b;  // a line's ends
  };
  int add_line(P a, P b);
  // An arc of a circle: angles in radians from +x, sweep signed; |sweep| >= 2 pi is the whole circle (no ends, no midpoint).
  // tolerance: the chord's largest distance from the arc.
  int add_arc(P center, double radius, double from, double sweep, double tolerance);
  int add_polyline(const std::vector<P>& points, bool closed);  // a spline or any other curve, sampled; its ends when open
  void add_point(P at);                                         // a point of its own (an endpoint snap)
  void add_center(int curve, P at);                             // an ellipse's centre
  void finish();                                                // builds the grid; nothing may be added after it
  bool empty() const { return m_points.empty() && m_pieces.empty(); }
  size_t curves() const { return m_round.size(); }
  double z = 0;  // where the curves lie along the shape's z (a drawing's plane: 0)
  // What lies within r of `at` (each once).
  void around(P at, double r, std::vector<Point>& points, std::vector<Piece>& pieces) const;
  // The nearest point to `q` on a round curve (an arc or circle), or false for any other curve or outside the arc.
  bool onRound(int curve, P q, P& out) const;
  bool spans(int curve, P q) const;  // whether a round curve's arc spans the direction of `q` from its centre
  const Round* round(int curve) const;  // null for a loose point

 private:
  std::vector<Point> m_points;
  std::vector<Piece> m_pieces;
  std::vector<Round> m_round;  // per curve
  double m_x0 = 0, m_y0 = 0, m_cell = 1;
  int m_nx = 0, m_ny = 0;
  std::vector<std::vector<int>> m_cellPoints, m_cellPieces;
  bool m_finished = false;
};

// The edges and loose vertices of a shape, in its own x and y (a drawing body's plane), z their mean height.
std::shared_ptr<Index> index_shape(const TopoDS_Shape& shape);

// An index placed in a common plane: x' = a x + b y + c, y' = d x + e y + f (rotation and translation, maybe a uniform scale).
struct Placed {
  const Index* index = nullptr;
  double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
};
// The snap nearest `at` (common coordinates) within `aperture` over all the sources, in the order above; intersections
// between curves of different sources too. With `from` (the point picked before), the foot of the perpendicular from it
// on a line or a circle and where a line from it touches a circle, on the curves near `at`.
Snap snap(const std::vector<Placed>& sources, P at, double aperture, const Kinds& kinds = {}, const P* from = nullptr);
}  // namespace opad::snap2d
