#pragma once
// Hidden-line projection of bodies into a drawing view (TODO 11 UI-77): typed 2D curves in view coordinates, each with
// its source (body + edge ordinal, or face ordinal for a silhouette inside a face) and its class. Three tiers:
//   exact  - HLRBRep_Algo on copies of the bodies; up to about 2,000 faces (Auto), monolithic and not interruptible
//   draft  - HLRBRep_PolyAlgo on meshed copies; polylines only, for a first paint
//   hybrid - the exact B-rep curves, their visibility read from a depth buffer of the bodies' meshes (big assemblies:
//            the Hydrostatic took 94 s exact against about 1 s); analytic silhouettes of quadrics, elsewhere found on the
//            mesh and settled onto the surface (splines on the true contour)
// Where pieces lie on one another (lines, arcs, ellipses, identical splines) only the nearest visible one is kept, and a
// hidden one only where no visible one lies. Results are a pure function of body keys, world placements, the view and
// the tier, cached under that fingerprint in memory and in the user cache. Workers only: every tier walks or meshes
// geometry.
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "opad/document.hpp"
#include "opad/render.hpp"
#include "opad/scene.hpp"

namespace opad::drawing {

using Vec2 = std::array<double, 2>;

enum class Quality { Auto, Exact, Draft, Hybrid };
const char* quality_name(Quality q);
Quality quality_from_name(const std::string& name);  // auto | exact | draft | hybrid; throws Error

struct ViewSpec {
  std::vector<std::string> nodes;  // bodies or components to draw; empty = every body of the scene
  std::vector<std::string> hide;   // nodes left out, with everything under them
  bool visible_only = false;       // also leave out bodies hidden in the 3D view
  Vec3 dir{0, -1, 0};              // from the model towards the viewer (Camera::preset's eye); default front
  Vec3 up{0, 0, 1};
  bool hidden = true;              // return hidden lines as well
  bool tangent = true;             // edges between tangent faces
  bool silhouettes = true;
  bool seams = false;              // seams and splits of one smooth surface (never drawn on sheets)
  Quality quality = Quality::Auto;
  int exact_faces = 2000;          // Auto: exact up to this many faces, hybrid above
  int resolution = 4096;           // hybrid: depth-buffer pixels along the long side of the view
  double tolerance = 0.01;         // mm: approximated curves stay this close to the projection
  std::map<std::string, Vec3> offsets;  // node -> world translation added to it and its children (exploded views)
  static ViewSpec preset(const std::string& view);  // the Camera::preset names: front, top, right, iso, ...
  json to_json() const;
  static ViewSpec from_json(const json& j);  // {"view":"front"} or {"dir":[..],"up":[..]}, plus the fields above
};

struct Curve {
  enum class Type : uint8_t { Line, Arc, Ellipse, Spline, Polyline };
  enum class Kind : uint8_t { Sharp, Tangent, Seam, Silhouette };
  Type type = Type::Line;
  Kind kind = Kind::Sharp;
  bool hidden = false;
  std::vector<Vec2> pts;  // Line: its two ends; Polyline: its points; Spline: its poles
  // Arc (r1) and Ellipse (r1, r2 the major and minor semi-axes, rot the major axis' angle): centre and the angles from
  // a0 to a1, counter-clockwise (a1 - a0 = 2 pi: closed). An arc's angles are absolute; an ellipse's are its parameter.
  Vec2 c{0, 0};
  double r1 = 0, r2 = 0, rot = 0, a0 = 0, a1 = 0;
  int degree = 0;                       // Spline: degree, flat knot vector, weights (empty: polynomial)
  std::vector<double> knots, weights;
  int body = -1;  // index into ViewGeometry::bodies
  int edge = -1;  // edge ordinal in that body (also for a silhouette that runs along an edge)
  int face = -1;  // face ordinal: a silhouette inside a face
  double z = 0;   // depth of its middle towards the viewer: of coincident pieces the nearest one is kept
  std::vector<Vec2> sample(double tol) const;  // a polyline within tol of the curve
  // Cubic Béziers (start, two controls, end) within tol of the curve, for writers without splines or ellipses (SVG
  // paths, PDF): lines and polylines exactly, arcs and ellipses in equal pieces of at most a quarter turn, splines split
  // at their knots (first approximated by a cubic one when rational or of a higher degree). Sheets pass 0.01 mm / scale.
  std::vector<std::array<Vec2, 4>> beziers(double tol) const;
  double length() const;
  json to_json(double bezier_tol = 0) const;  // bezier_tol > 0: arcs, ellipses and splines also as "bezier"
  static const char* type_name(Type t);
  static const char* kind_name(Kind k);
};

struct ViewGeometry {
  struct Body {
    std::string node, key;
  };
  std::vector<Body> bodies;
  std::vector<Curve> curves;
  Quality tier = Quality::Exact;
  std::string fingerprint;
  Vec3 x{1, 0, 0}, y{0, 0, 1}, dir{0, -1, 0};  // view x and y in world coordinates, and towards the viewer
  std::array<double, 4> bounds{0, 0, 0, 0};    // xmin, ymin, xmax, ymax of every curve
  json stats = json::object();                  // faces, edges, timings (ms) of the run that made it
  json counts() const;                          // curves by visibility, kind and type
  json to_json(bool with_curves = true, double bezier_tol = 0) const;
  std::string serialize() const;
  static ViewGeometry deserialize(const std::string& blob);  // throws Error
};

// Reports (fraction 0..1 or -1, phase); returning false cancels and project() throws Error("cancelled"). Called from
// the calling thread or from its parallel workers, never two at a time.
using ProjectionProgress = std::function<bool(double, const std::string&)>;

// The view's axes in world coordinates (x right, y up, dir towards the viewer), as every tier projects with them.
void view_axes(const ViewSpec& spec, Vec3& x, Vec3& y, Vec3& dir);
// The body nodes a view draws, with their world placements (the explode offsets added). Throws for an unknown node.
std::vector<std::pair<std::string, Mat4>> view_bodies(const Scene& scene, const ViewSpec& spec);
// The tier Auto takes for this view (counts faces: workers only).
Quality choose_tier(const Document& doc, const Scene& scene, const ViewSpec& spec);
std::string projection_fingerprint(const Document& doc, const Scene& scene, const ViewSpec& spec, Quality tier);
// Cached by fingerprint (memory, then the user cache's "projection" bucket); use_cache=false always computes.
std::shared_ptr<const ViewGeometry> project(const Document& doc, const Scene& scene, const ViewSpec& spec,
                                            const ProjectionProgress& progress = {}, bool use_cache = true);
void clear_projection_memory();

// A quick look at a projection: visible lines black, tangent ones grey, hidden ones dashed light grey; the view is fitted.
Image preview_image(const ViewGeometry& g, int width, int height);

}  // namespace opad::drawing
