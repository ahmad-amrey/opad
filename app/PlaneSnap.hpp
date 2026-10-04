#pragma once
// Where a point put on a plane by the pointer lands, as a sketch snaps the points it places (TODO 11 P1): shared by the
// primitive placer (its position, the footprint's corner or rim) and the sketch plane's origin. Within a few pixels of the
// pointer the best kind wins, the nearest of that kind:
//   1. an end (a face's corner, a sketch's or a drawing's end point), a centre (a round edge's, a circle's), the plane's origin;
//   2. a midpoint (a face's straight edge, a sketch line), a quadrant, an intersection;
//   3. the nearest point on a sketch's or a drawing's curve.
// The sketches' and drawings' points are the object snaps (Viewport::snapAt: the sketch's switch per kind), while object snap
// is on; the face's are the hovered face's, and the last one's after the pointer left it (a corner reached from outside the
// body). Nothing in reach: a grid node while grid snapping is on. Alt held: none of them, the point is where the pointer meets
// the plane. A point off the plane goes onto it square to it (seen where it is, as a sketch projects).
#include <QPointF>
#include <QString>

#include <TopoDS_Face.hxx>

#include <vector>

#include "opad/scene.hpp"

class Viewport;

class PlaneSnap {
 public:
  struct Options {
    bool free = false;    // Alt held: nothing snaps
    bool origin = true;   // the plane's origin is a point to snap to
    bool grid = true;     // grid nodes while grid snapping is on
    const opad::Vec3* skip = nullptr;  // not what lands on this point of the plane (the centre being sized from)
  };
  struct Result {
    bool ok = false;            // the pointer meets the plane
    QString kind;               // "endpoint", "midpoint", "center", "quadrant", "intersection", "nearest", "origin"; "grid"; "": none
    double u = 0, v = 0;        // on the plane, in its frame
    opad::Vec3 at{0, 0, 0};     // there, in the world
    opad::Vec3 seen{0, 0, 0};   // the point snapped to, where it is (off the plane for a face's corner above it)
    bool exact() const { return !kind.isEmpty() && kind != "grid"; }  // a point of the model: its place exactly, not rounded
  };
  explicit PlaneSnap(Viewport* view) : m_view(view) {}
  // `face`: the face under the pointer (null: none; the last face's points stay in reach).
  Result at(const QPointF& pos, const opad::Frame& frame, const TopoDS_Face& face, const Options& options);
  void forget();  // the last face's points go out of reach (a new pick)
  static int rank(const QString& kind);  // 0 ends, centres and the origin, 1 midpoints and the like, 2 nearest points

 private:
  struct Target {
    opad::Vec3 at;
    const char* kind;
  };
  const std::vector<Target>& faceTargets(const TopoDS_Face& face);
  Viewport* m_view;
  TopoDS_Face m_face;  // whose points m_targets are
  std::vector<Target> m_targets;
};
