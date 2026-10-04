#include "PlaneSnap.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <QLineF>

#include <cmath>

#include "Viewport.hpp"

namespace {
constexpr double kReach = 10;      // widget points around the pointer (the object snap's aperture)
constexpr size_t kTargets = 4000;  // of one face: enough for any face a primitive is put on
}  // namespace

int PlaneSnap::rank(const QString& kind) {
  if (kind == "endpoint" || kind == "center" || kind == "origin") return 0;
  if (kind == "nearest") return 2;
  return 1;
}

void PlaneSnap::forget() {
  m_face.Nullify();
  m_targets.clear();
}

// A face's corners, its round edges' centres and its straight edges' midpoints (kept for the face until another is hovered).
const std::vector<PlaneSnap::Target>& PlaneSnap::faceTargets(const TopoDS_Face& face) {
  if (face.IsNull() || face.IsSame(m_face)) return m_targets;  // none hovered: the last one's
  m_face = face;
  m_targets.clear();
  for (TopExp_Explorer v(face, TopAbs_VERTEX); v.More() && m_targets.size() < kTargets; v.Next()) {
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
    m_targets.push_back({{p.X(), p.Y(), p.Z()}, "endpoint"});
  }
  for (TopExp_Explorer e(face, TopAbs_EDGE); e.More() && m_targets.size() < 2 * kTargets; e.Next()) {
    const TopoDS_Edge& edge = TopoDS::Edge(e.Current());
    if (BRep_Tool::Degenerated(edge)) continue;
    const BRepAdaptor_Curve c(edge);
    if (c.GetType() == GeomAbs_Circle) {
      const gp_Pnt p = c.Circle().Location();
      m_targets.push_back({{p.X(), p.Y(), p.Z()}, "center"});
    } else if (c.GetType() == GeomAbs_Line) {
      const gp_Pnt p = c.Value((c.FirstParameter() + c.LastParameter()) / 2);
      m_targets.push_back({{p.X(), p.Y(), p.Z()}, "midpoint"});
    }
  }
  return m_targets;
}

PlaneSnap::Result PlaneSnap::at(const QPointF& pos, const opad::Frame& frame, const TopoDS_Face& face, const Options& options) {
  Result r;
  if (!m_view->planePoint(pos, frame, r.u, r.v) || !std::isfinite(r.u) || !std::isfinite(r.v) || std::fabs(r.u) > 1e6 || std::fabs(r.v) > 1e6) return r;
  r.ok = true;
  r.at = r.seen = frame.to_world(r.u, r.v);
  const std::vector<Target>& targets = faceTargets(face);  // also when free: the face hovered is remembered
  if (options.free) return r;
  int best = 3;
  double nearest = kReach;
  opad::Vec3 seen{0, 0, 0};
  QString kind;
  double su = 0, sv = 0;
  if (options.skip) frame.to_local(*options.skip, su, sv);
  auto offer = [&](const opad::Vec3& p, const QString& k) {
    if (options.skip) {
      double pu = 0, pv = 0;
      frame.to_local(p, pu, pv);
      if (std::hypot(pu - su, pv - sv) < 1e-6 * (1 + std::fabs(su) + std::fabs(sv))) return;
    }
    const int cls = rank(k);
    const double d = QLineF(QPointF(m_view->widgetPoint(p)), pos).length();
    if (d > kReach || cls > best || (cls == best && d >= nearest)) return;
    best = cls, nearest = d, seen = p, kind = k;
  };
  // The sketches' and drawings' object snaps (the index of each is made on a worker the first time; none until then).
  if (opad::Vec3 p; m_view->objectSnap() && m_view->snapAt(pos, p, &kind)) {
    const QString k = kind;
    kind.clear();
    offer(p, k);
  }
  for (const Target& t : targets) offer(t.at, QString::fromLatin1(t.kind));
  if (options.origin) offer(frame.origin, QStringLiteral("origin"));
  if (!kind.isEmpty()) {
    frame.to_local(seen, r.u, r.v);
    r.kind = kind;
    r.seen = seen;
    r.at = frame.to_world(r.u, r.v);
    return r;
  }
  if (options.grid && m_view->gridSnap() && m_view->gridStep() > 0) {
    const double step = m_view->gridStep();
    r.u = std::round(r.u / step) * step;
    r.v = std::round(r.v / step) * step;
    r.kind = QStringLiteral("grid");
    r.at = r.seen = frame.to_world(r.u, r.v);
  }
  return r;
}
