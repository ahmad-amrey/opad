#pragma once
#include <Geom_Curve.hxx>
#include <array>
#include <functional>
#include "sketch_geom.hpp"
namespace opad::design {
std::vector<int> connected_entities(const Sketch& sketch,const std::vector<int>& seeds);
// The chain through each seed curve, as other CAD programs select it: curve ends are linked where only two curves of a kind
// (construction or not) meet, at any corner, and where several branch only between two that each take the other for the
// smooth way on (turning at most 15 degrees, clearly the nearest); the chain is the path or loop those links make, the same
// whichever of its curves is the seed or which way they were drawn. A path whose loose end comes back to a point it runs
// through loses that end's curve (never the seed's side), so no three of its curves meet at a point. connected_entities takes
// everything connected instead (the whole shape: what Move, Align and a rigid group want). Each seed, then its chain.
std::vector<int> chain_entities(const Sketch& sketch,const std::vector<int>& seeds);
struct SketchTransform {
  double x=0,y=0,angle=0,scale=1,cx=0,cy=0;
  bool mirror=false; // reflect about the local x axis through (cx,cy), then rotate/scale/translate
};
// Moved in place (not a copy), the curves take the whole of every rigid group they belong to (rigid_closure).
std::vector<int> transform_entities(Sketch& sk,const std::vector<int>& ids,const SketchTransform& transform,bool copy);
// The ids with every point and curve of each Rigid constraint that names one of them, until nothing more joins.
std::vector<int> rigid_closure(const Sketch& sk,const std::vector<int>& ids);
// Align (the sketch's Modify > Align): turns the curves `ids` (and their rigid groups) as one about `pivot`, an end of the
// curve `reference`, so that the straight line between its two ends (align_ends: a line, or an arc's or open spline's
// chord: a DXF's nearly straight segment is often a flat arc) runs along `to`: Vertical or Horizontal (the sketch's axes),
// Parallel to `target`'s ends, or Collinear with them (then moved across onto it too). The smaller turn of the two that
// do it. With `hold` the alignment is kept by a constraint (vertical or horizontal: on the line, or on an arc's or
// spline's two ends; parallel or collinear: between two lines only). The reference must be one of the curves moved (or of
// their groups). Returns the curves moved.
bool align_ends(const Sketch& sk,int curve,int& a,int& b);  // a line's, an arc's or an open spline's two ends
enum class AlignTo { Vertical, Horizontal, Parallel, Collinear };
struct SketchAlign { int reference=0,pivot=0,target=0; AlignTo to=AlignTo::Vertical; bool hold=false; };
SketchTransform align_transform(const Sketch& sk,const SketchAlign& align);
std::vector<int> align_entities(Sketch& sk,const std::vector<int>& ids,const SketchAlign& align);
std::vector<int> append_sketch_shape(Sketch& sk,const TopoDS_Shape& shape,const Frame& frame={},bool construction=false,bool fixed=false);
// The curves `ids` as the chains an offset makes one copy of each: a chain runs on through the points where exactly two of
// them meet (by point, or apart in one place) and stops at a free end or where several branch; a circle, an ellipse or a
// closed spline is a chain of its own. `closed`: it comes back to where it started (it grows outward or inward, else it
// offsets to one side). Points are left out; the order follows `ids` (each chain's curves in their order along it).
struct OffsetGroup { std::vector<int> ids; bool closed=false; };
std::vector<OffsetGroup> offset_groups(const Sketch& sk,const std::vector<int>& ids);
// The kernel's offset of the connected chain `ids` (lines, arcs, splines) by `distance` (the sign picks the side), as new
// plain curves; returns them. Several chains: one offset_groups chain at a time.
std::vector<int> offset_entities(Sketch& sk,const std::vector<int>& ids,double distance,bool round);
// An open chain offsets to one side only, a parallel copy (no end caps, never closed): the signed distance (|distance|
// with the kernel's sign) whose copy lies nearer (tx, ty), the side the pointer is on. A closed chain: its kernel sign as
// given is kept by the caller (positive grows outward).
double offset_side(const Sketch& sk,const std::vector<int>& ids,double distance,bool round,double tx,double ty);
// The signed distance one chain of offset_groups is offset by, as if it were picked alone with the pointer at (tx, ty): a
// closed chain `distance` (positive grows outward); an open one its copy on the side of the chain the pointer is on for a
// positive distance, on the other side for a negative one (so a negative distance turns every chain round).
double offset_toward(const Sketch& sk,const OffsetGroup& chain,double distance,bool round,double tx,double ty);
// The same, held by an offset relationship (SkConstraint::Type::Offset, `expr` kept as typed): the chain and its offset keep
// their shape and move and turn together, and a new value of the relationship (double-click its label, or an expression's
// parameter) makes the offset again at that distance (refresh_offsets). Returns the relationship.
int offset_relationship(Sketch& sk,const std::vector<int>& ids,double distance,bool round,const std::string& expr={});
int heal_endpoints(Sketch& sk,double tolerance);
int heal_to_curves(Sketch& sk,double tolerance);
void split_entity(Sketch& sk,int entity,double x,double y);
void break_intersections(Sketch& sk,const std::vector<int>& entities);
void extend_entity(Sketch& sk,int entity,int boundary,double x,double y);
// One-click extend (TODO 11 UI-28): the end of the line or arc nearer (x, y) to the nearest curve it meets on its way,
// whichever that is. Returns the boundary reached.
int extend_entity(Sketch& sk,int entity,double x,double y);
// A fillet at a point where two lines or arcs end (TODO 11 UI-28; construction ones ending there too are left out when two
// others do: a fillet's whole side, a diagonal): its centre, where it touches the first and the second curve (along each
// from the corner, the nearest that fits) and the curves; false when none fits.
struct FilletCorner { double cx=0,cy=0,ax=0,ay=0,bx=0,by=0,r=0; int first=0,second=0; bool ccw=true; };  // ccw: from a to b
bool fillet_geometry(const Sketch& sk,int point,double r,FilletCorner& out);
// Why no fillet of radius r fits at `point`, for the tool to say (kind None when one fits): TooFew = fewer than two lines
// or arcs end at that point (the other passes through it, or ends at a point of its own: a drawing's coincident ends),
// TooMany = more than two (`curves` how many), Spline = a spline ends there, Tangent = the two meet tangent (no corner),
// TooBig = the curves are too short or too curved for r (`largest`: about the largest radius that fits, 0 when none).
struct FilletProblem { enum Kind { None, TooFew, TooMany, Spline, Tangent, TooBig } kind=None; int curves=0; double largest=0; };
FilletProblem fillet_problem(const Sketch& sk,int point,double r);
// Makes it: each curve ends where the arc touches it, the arc is tangent to both with its radius (`expr` kept as typed);
// what measured a trimmed line's whole length moves to a construction line along the old side, an arc's length goes; the
// old corner stays as a virtual sharp on both curves when something still refers to it. Returns the arc. Throws (the
// sketch as it was) when no fillet fits.
int fillet_corner(Sketch& sk,int point,double r,const std::string& expr={});
// Merges point `from` into `into` (a dragged point dropped on another, UI-28): every curve and constraint on it now holds
// `into`; constraints that become empty go. Throws (the sketch as it was) when that would collapse a curve or a dimension
// keeps the two apart.
void merge_points(Sketch& sk,int from,int into);
// Trim for splines and ellipses (TODO 11 UI-28; the editor trims lines, circles and arcs exactly itself). curve_cuts: where
// the other curves cross curve `id` (any kind: the kernel intersects them), as parameters along it, sorted, each with the
// curve crossing there; an open curve touched at an end is not cut there.
struct CurveCuts {
  Handle(Geom_Curve) curve;  // in sketch coordinates (z = 0)
  double first=0,last=0;
  bool closed=false;
  std::vector<std::pair<double,int>> at;
};
// `filter` (optional): false when a curve cannot reach curve `id` (an editor's samples tell), so the kernel skips it.
using CurveFilter=std::function<bool(const SkEntity&)>;
CurveCuts curve_cuts(const Sketch& sk,int id,const CurveFilter& filter={});
// Where two curves cross, by the kernel (nothing for a point, or when boxes from their points are apart): what cuts a line,
// a circle or an arc that the editor's exact trim does not intersect itself (a spline, an ellipse).
std::vector<std::array<double,2>> curve_crossings(const Sketch& sk,const SkEntity& a,const SkEntity& b);
// A stretch of that curve between two parameters (`to` past `last` crosses a periodic curve's seam), with the curve that
// cuts it at each end (0: an end of the curve, or the seam).
struct TrimPiece { double from=0,to=0; int from_by=0,to_by=0; };
TopoDS_Edge piece_edge(const CurveCuts& cuts,const TrimPiece& piece);
// What a trim at (x, y) keeps (empty: nothing, the whole curve goes) and takes (the span between the cuts on either side of
// it, two stretches when it crosses a closed curve's seam that is not periodic); false: nothing to cut between (a closed
// curve crossed once).
bool trim_pieces(const CurveCuts& cuts,double x,double y,std::vector<TrimPiece>& keep,std::vector<TrimPiece>& gone);
// Makes it: the pieces kept become exact B-splines whose ends are the curve's old end points or new points on the crossing
// curves (coincident with a line, circle or arc); the first keeps the id. Returns the pieces. Throws (the sketch as it was)
// when the curve is not an editable spline or ellipse or is crossed once and closed.
std::vector<int> trim_curve(Sketch& sk,int id,double x,double y);
// The exact trim of a line, a circle or an arc (UI-28, the editor's click and fence). A circle's or an arc's round: centre,
// radius, counter-clockwise from a0 by sweep (2 pi: a whole circle); holds: (x, y), on its circle, lies on it.
struct TrimRound {
  double cx=0,cy=0,r=0,a0=0,sweep=0;
  bool holds(double x,double y) const;
};
TrimRound trim_round(const Sketch& sk,const SkEntity& circleOrArc);
// Where the other curves cross `target` (a line, a circle or an arc), sorted along it: t is 0..1 along a line, the angle
// from a0 round a circle or an arc; `other` the curve crossing there. `samples` (optional): a spline's or an ellipse's
// polyline (a preview's), else the kernel crosses them.
struct TrimCut { double t=0; int other=0; };
struct TrimCrossings {
  bool line=true;
  double ax=0,ay=0,bx=0,by=0;  // a line's ends
  TrimRound self;              // a circle's or an arc's
  std::vector<TrimCut> cuts;
};
using TrimSamples=std::function<const std::vector<std::pair<double,double>>*(const SkEntity&)>;
TrimCrossings trim_crossings(const Sketch& sk,const SkEntity& target,const TrimSamples& samples={});
// Takes the piece of curve `id` about (u, v) between the curves crossing it (all of it when none does). Each new end is a
// point held on the curve that cuts there; a line cut in its middle leaves two collinear lines, an arc two equal arcs, a
// circle the arc round the other way. Constraints on the curve stay where they still hold on what is left (its direction,
// a radius, a tangency, a point on it, a distance to it); what measured its whole extent goes (drop_extent_constraints), so
// the trim is never refused for a dimension on the curve. NotACurve: not a line, a circle or an arc; CrossedOnce: a circle
// crossed once (nothing to cut between). Either way nothing changed.
enum class TrimOutcome { Trimmed, NotACurve, CrossedOnce };
TrimOutcome trim_entity(Sketch& sk,int id,double u,double v);
// What measures a curve's whole extent and means something else once it is trimmed shorter: on a line its length (a length
// dimension, an "equal" with another line) and a point held at its middle; on an arc its length.
void drop_extent_constraints(Sketch& sk,int id);
// The two lines ending at `point` (construction ones ending there too left out when two others do, as for a fillet), first
// to last in the sketch; empty when there are not two.
std::vector<int> corner_lines(const Sketch& sk,int point);
// Cuts the corner `point` of its two lines (corner_lines) `first` along the first and `second` along the second; what
// measured a side and the old corner are kept as a fillet keeps them (a construction line along the old side, a virtual
// sharp). Throws (the sketch as it was) when the corner is not two lines or a distance reaches past a line's end.
void chamfer_corner(Sketch& sk,int point,double first,double second);
void delete_curve_node(Sketch& sk,int point);
void boolean_regions(Sketch& sk,double ax,double ay,double bx,double by,const std::string& operation);
void identify_regions(const Sketch& sk,std::vector<Region>& regions,const Frame& frame={});
// Signed source entity IDs of a region's oriented boundary, stable when its geometry moves.
std::vector<int> region_sources(const Sketch& sk,const Region& region,const Frame& frame={});
}
