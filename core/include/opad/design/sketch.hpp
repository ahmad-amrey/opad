#pragma once
// 2D sketch model: points, curves on them, geometric constraints and driving dimensions, plus the numeric
// solver. Pure maths (no OCCT): the wire/profile builders that need the kernel live in sketch_geom.hpp.
//
// Everything has an integer id, unique inside the sketch across points, entities and constraints, so a
// reference is one number. Coordinates are sketch-plane (u, v) in mm; angles in radians.
#include <map>
#include <string>
#include <vector>

#include "../json.hpp"
#include "../util.hpp"
#include "expr.hpp"

namespace opad::design {

struct SkPoint {
  int id = 0;
  double x = 0, y = 0;
  bool fixed = false;  // projected/reference geometry: the solver never moves it
};

struct SkEntity {
  enum class Type { Point, Line, Circle, Arc, Ellipse, Spline };
  int id = 0;
  Type type = Type::Line;
  // Point: p[0]. Line: p[0] -> p[1]. Circle: p[0] centre + r. Arc: p[0] centre, counter-clockwise from p[1] to
  // p[2] (the solver keeps both ends at one radius). Ellipse: p[0] centre, p[1] end of the major axis + r =
  // minor radius. Spline: p = fit points, interpolated in order; closed and periodic (smooth through the seam) when
  // `periodic` is set or the first point id is repeated at the end.
  std::vector<int> p;
  // Nonzero degree stores an exact rational B-spline: p are control poles.
  int degree = 0;
  std::vector<double> knots, weights;
  std::vector<int> multiplicities;
  bool periodic = false;
  // An open fit spline's end directions, [dx, dy] in the sketch (gap log #5); empty = free.
  std::vector<double> start_tangent, end_tangent;
  // A fit spline through samples of x(t), y(t) (gap log #2): {"x", "y", "t0", "t1", "tolerance", "min_points"}; the
  // sketch computation rewrites its (fixed) points. Older builds read it as the fit spline it stores.
  json equation;
  json source; // optional associative projection descriptor
  double r = 0;
  bool construction = false;  // guide geometry: never part of a profile
  bool fixed = false;         // projected/reference: r is not a solver variable either
  static const char* type_name(Type t);
  static Type type_from_name(const std::string& s);
};

struct SkConstraint {
  enum class Type {
    // geometric
    Coincident,     // [point, point] or [point, line|circle|arc]: point on the (infinite) curve
    Horizontal,     // [line] or [point, point]
    Vertical,       // [line] or [point, point]
    Parallel,       // [line, line]
    Perpendicular,  // [line, line]
    Collinear,      // [line, line]
    Tangent,        // [line, circle|arc] or [circle|arc, circle|arc]
    Equal,          // [line, line] length, or [circle|arc, circle|arc] radius
    Concentric,     // [circle|arc, circle|arc]
    Midpoint,       // [point, line]
    Symmetric,      // [point, point, line]: mirror images about the line
    Fix,            // [point] or [entity]: stays where it is now
    Smooth,         // [spline,spline]: coincident endpoints, opposite tangents and equal curvature (G2)
    Curvature,      // [spline,spline]: equal signed endpoint curvature along the joined path
    // driving dimensions (value in mm or radians)
    Distance,       // [point, point], [point, line], [line] = its length, or [line, line] (parallel lines)
    HDistance,      // [point, point] along u (with `is_signed`, q.u - p.u); [point]: its u from the sketch origin
    VDistance,      // [point, point] along v (with `is_signed`, q.v - p.v); [point]: its v from the sketch origin
    Radius,         // [circle|arc]
    Diameter,       // [circle|arc]
    Angle,          // [line, line], between their directions p0->p1, 0..pi
    ArcLength       // [arc], counter-clockwise length
  };
  int id = 0;
  Type type = Type::Coincident;
  std::vector<int> refs;
  std::vector<int> anchors; // stable endpoint point IDs for spline continuity constraints
  double value = 0;       // dimensions: the evaluated value the solver drives to
  std::string expr;       // dimensions: the expression as typed ("width / 2", "12 mm"); empty = plain value
  bool reference = false; // measured after solving; never removes a degree of freedom
  bool is_signed = false; // hdistance/vdistance between points: the signed q - p, not its size (gap log #11)
  double pos[2] = {0, 0}; // dimensions: where the label sits (display only)
  bool is_dimension() const { return type >= Type::Distance; }
  static const char* type_name(Type t);
  static Type type_from_name(const std::string& s);
};

struct Sketch {
  std::vector<SkPoint> points;
  std::vector<SkEntity> entities;
  std::vector<SkConstraint> constraints;
  json images = json::array(); // embedded raster backdrops, each with a stable ID
  json patterns = json::array(); // associative patterns, maps refer to stable point/entity IDs
  mutable int id_watermark = 0; // never recycle a deleted ID
  // next_id's running maximum (TODO 11 UI-29: it read every list on every call, so building n items took n² steps): how
  // much of each list (points, entities, constraints, images, patterns) it has read, the id it read last in each, and
  // the largest. Only what was appended since is read; a list that shrank, or whose last read item is no longer the
  // same, is read again. (An id changed in place before that item could only leave a gap; nothing raises one.)
  struct IdScan { size_t n[5] = {}; int last[5] = {}; int top = 0; };
  mutable IdScan id_scan;

  SkPoint* point(int id);
  const SkPoint* point(int id) const;
  SkEntity* entity(int id);
  const SkEntity* entity(int id) const;
  SkConstraint* constraint(int id);
  int next_id() const;  // above both the live IDs and the deletion watermark; amortised O(1) while the lists grow

  // Convenience builders (ids are allocated here). They add no constraints.
  int add_point(double x, double y, bool fixed = false);
  int add_line(int p0, int p1, bool construction = false);
  int add_circle(int centre, double r, bool construction = false);
  int add_arc(int centre, int start, int end, bool construction = false);  // ccw from start to end
  int add_constraint(SkConstraint::Type t, std::vector<int> refs, double value = 0, const std::string& expr = {});
  // Removes an entity or constraint (or a point nobody uses), and every constraint that referenced it; points
  // left without any entity go with it.
  void remove(int id);

  json to_json() const;                   // {"points":[..],"entities":[..],"constraints":[..]}
  static Sketch from_json(const json& j); // throws Error on dangling references or unknown types
  void validate() const;
};

// Entity-ID deltas are append-only document edits, independent of array ordering. An image whose bytes did not change
// goes as "image_fields": [{"id", <only the fields that changed>, a field removed as null}], so moving, scaling or fading
// a backdrop never stores its picture again (an older build keeps that key as a sketch field and shows the image unmoved).
json sketch_delta(const json& before, const json& after);
json apply_sketch_delta(const json& before, const json& delta);
// A sketch op's geometry as last solved: the regeneration's result, else what was given. A result never repeats the
// images (the solver leaves them alone): they come from the given geometry.
json solved_geometry(const json& sketch_op_data);
// A sketch geometry as text to tell whether what it shows changed (the viewport compares it on every scene sync): each
// picture by its length and samples of its bytes, never the megabytes themselves, every other field as it is.
std::string geometry_stamp(const json& geometry);
double dimension_value(const Sketch& sk, const SkConstraint& c);
ParamTable sketch_parameters(const Sketch& sk, const ParamTable& params = {});
void evaluate_dimensions(Sketch& sk, const ParamTable& params = {});
// Change local origin without moving any geometry in the sketch plane.
void shift_sketch_origin(Sketch& sk,double u,double v);

// ---------------------------------------------------------------- solver
struct SolveOptions {
  // Dragging: these points are pulled towards a target with a weak spring while every constraint holds.
  struct Drag { int point; double x, y; };
  std::vector<Drag> drags;
  int max_iterations = 100;
  double tolerance = 1e-8;  // on each constraint residual (mm or rad)
};

struct SolveResult {
  bool converged = false;
  int dof = 0;                  // remaining degrees of freedom (0 = fully constrained)
  double residual = 0;          // largest constraint residual left
  std::vector<int> failed;      // constraints whose residual is above tolerance (conflicting / unreachable)
  std::vector<int> redundant;   // constraints that add no information (dependent rows), when fully analysed
  std::vector<int> free_points; // points that can still move (for colouring under-constrained geometry)
};

// Moves the points (and radii) so every constraint holds, staying as close to the current positions as
// possible (so the sketch does not flip or jump). On failure the sketch is left at the best attempt only when
// `keep_best` is set; otherwise it is restored.
SolveResult solve(Sketch& sk, const SolveOptions& opt = {}, bool keep_best = false);

}  // namespace opad::design
