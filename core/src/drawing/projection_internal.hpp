#pragma once
// Shared by the projection tiers (projection.cpp: exact, draft, the cache; hybrid.cpp); not a public header.
#include <Adaptor3d_Curve.hxx>
#include <Geom2d_BSplineCurve.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

#include "opad/drawing/projection.hpp"
#include "opad/mesh.hpp"

namespace opad::drawing::detail {

// One body node of the view: the body-store shape (shared with other threads, so never meshed or changed here) and
// where it is.
struct Source {
  std::string node, key;
  TopoDS_Shape proto, placed;  // placed: proto moved to its world placement (a transformed copy when not rigid)
  Mat4 world;
  bool rigid = true;
  gp_Trsf trsf;                // the world placement when rigid
  bool mesh = false;           // triangulation only (STL, 3MF, OBJ...)
  bool whole = false;          // a section leaves it uncut (ViewSpec::whole)
  // What a section's cut left of a body (UI-82): key is the cut's own (meshes are cached under it), base the body's;
  // edges and faces map the cut shape's ordinals to the body's (-1: made by the cut). proto is placed in the world.
  std::string base;
  std::shared_ptr<const std::vector<int>> edges, faces;
  const std::string& body_key() const { return base.empty() ? key : base; }
};

struct View {
  gp_Dir x, y, z;  // z points towards the viewer
  Vec2 at(const gp_Pnt& p) const { return {p.XYZ().Dot(x.XYZ()), p.XYZ().Dot(y.XYZ())}; }
  Vec2 along(const gp_XYZ& v) const { return {v.Dot(x.XYZ()), v.Dot(y.XYZ())}; }
  double depth(const gp_Pnt& p) const { return p.XYZ().Dot(z.XYZ()); }
};

// Progress and cancel of one projection; safe from parallel workers (reports are throttled, never two at once).
class Run {
 public:
  explicit Run(const ProjectionProgress& callback) : m_callback(callback) {}
  void report(double fraction, const std::string& phase, bool force = false);
  bool cancelled() const { return m_stop.load(); }
  void check() const {
    if (m_stop.load()) throw Error("cancelled");
  }

 private:
  const ProjectionProgress& m_callback;
  std::mutex m_mu;
  std::atomic<bool> m_stop{false};
  std::chrono::steady_clock::time_point m_last{};
};

// The projection of the curve between t0 and t1, typed: lines, circles and ellipses stay analytic (an arc, an ellipse,
// or a segment when seen edge-on), B-splines keep their poles (an orthographic projection is affine), anything else is
// approximated within tol. Appended to `out` with the class and source of `like` and the depth of its middle (Curve::z).
void emit(const Adaptor3d_Curve& c, double t0, double t1, const View& v, const Curve& like, double tol, std::vector<Curve>& out);

// A spline curve as OCCT's (null when its poles and knots do not make one).
Handle(Geom2d_BSplineCurve) curve2d(const Curve& k);

// Display-like deflection of a body for its meshes (from its box) and the body's mesh, made on a copy of the shared
// shape and cached (memory and the user cache's "mesh" bucket, where tessellate_body() keeps its meshes too).
double deflection_for(const Document& doc, const std::string& key);
std::shared_ptr<const Mesh> body_mesh(const Document& doc, const Source& s, double deflection);

void hybrid(const Document& doc, const std::vector<Source>& sources, const ViewSpec& spec, const View& view, Run& run,
            ViewGeometry& out);

// Section views (section.cpp): the sources the cut crosses replaced by what is left of them (cached by body key,
// placement and cut), those it takes away entirely dropped, the others kept; then the faces the cut left facing the
// viewer as regions in view coordinates, by source index. Parallel; cancellable between bodies.
void cut_sources(const Document& doc, const ViewSpec& spec, const View& view, std::vector<Source>& sources, Run& run,
                 std::vector<ViewGeometry::Region>& regions);
// A cut body's curves named after the body's own edges and faces (Source::edges, faces).
void name_cut_curves(const std::vector<Source>& sources, std::vector<Curve>& curves);

}  // namespace opad::drawing::detail
