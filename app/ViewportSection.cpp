// Section plane gizmo: the clip plane's outline drawn over the model (edges only, sized to the model's extent
// in the plane), and a strip inside each side that is a drag handle. Hovering a strip shows a two-headed arrow
// along the section axis at the mouse; dragging moves the plane along its normal. Hit tests are done in widget
// space by the viewport (like the measurement anchors), nothing here is pickable through OCCT.
#include "Viewport.hpp"

#include <Graphic3d_ArrayOfPolylines.hxx>
#include <Graphic3d_SequenceOfHClipPlane.hxx>
#include <Prs3d_Arrow.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>

#include <gp.hxx>

#include <QMouseEvent>
#include <algorithm>
#include <array>
#include <cmath>

#include "opad/inspect.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
gp_Pnt pnt(const opad::Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }

constexpr double kBandIn = 14.0;   // px: the handle strip reaches this far inside the outline
constexpr double kBandOut = 4.0;   // px: and this far outside, so the line itself is hit too
constexpr double kArrowPx = 34.0;  // px: each head of the drag arrow reaches this far from the side

class SectionGizmo : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(SectionGizmo, AIS_InteractiveObject)
 public:
  std::array<gp_Pnt, 4> corners;
  int hovered = -1;      // side index (corner i -> i+1), -1 = none
  gp_Pnt anchor;         // where the arrow sits on the hovered side
  gp_Dir normal{0, 0, 1};
  double pixel = 1.0;    // world units per widget pixel
  QColor lineColor, hoverColor;
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    {
      auto group = prs->NewGroup();
      Handle(Prs3d_LineAspect) style = new Prs3d_LineAspect(occ(lineColor), Aspect_TOL_SOLID, 1.5);
      group->SetGroupPrimitivesAspect(style->Aspect());
      Handle(Graphic3d_ArrayOfPolylines) outline = new Graphic3d_ArrayOfPolylines(5);
      for (int i = 0; i < 5; ++i) outline->AddVertex(corners[i % 4]);
      group->AddPrimitiveArray(outline);
    }
    if (hovered < 0) return;
    {
      auto group = prs->NewGroup();
      Handle(Prs3d_LineAspect) style = new Prs3d_LineAspect(occ(hoverColor), Aspect_TOL_SOLID, 3.0);
      group->SetGroupPrimitivesAspect(style->Aspect());
      Handle(Graphic3d_ArrayOfPolylines) side = new Graphic3d_ArrayOfPolylines(2);
      side->AddVertex(corners[hovered]);
      side->AddVertex(corners[(hovered + 1) % 4]);
      group->AddPrimitiveArray(side);
    }
    auto group = prs->NewGroup();
    Handle(Prs3d_ShadingAspect) style = new Prs3d_ShadingAspect();
    group->SetClosed(true);
    style->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
    style->SetColor(occ(hoverColor));
    style->Aspect()->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);  // flat, the same amber as the side
    group->SetGroupPrimitivesAspect(style->Aspect());
    const double reach = kArrowPx * pixel, head = 13.0 * pixel, radius = 2.4 * pixel;
    for (int sign : {1, -1})
      group->AddPrimitiveArray(Prs3d_Arrow::DrawShaded(gp_Ax1(anchor, sign > 0 ? normal : normal.Reversed()), radius, reach, head * 0.45, head, 20));
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}
};
}  // namespace

void Viewport::updateSectionGizmo() {
  if (!m_initialised) return;
  auto drop = [&] {
    if (!m_sectionGizmo.IsNull()) m_ctx->Remove(m_sectionGizmo, Standard_False);
    m_sectionGizmo.Nullify();
    m_sectionHasPlane = false;
    if (m_sectionHover >= 0) { unsetCursor(); setToolTip(QString()); }
    m_sectionHover = -1;
    m_sectionDrag = false;
  };
  opad::Vec3 lo, hi;
  const double len = std::sqrt(m_sectionNormal[0] * m_sectionNormal[0] + m_sectionNormal[1] * m_sectionNormal[1] + m_sectionNormal[2] * m_sectionNormal[2]);
  if (!m_sectionEnabled || len < 1e-9 || !opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi)) return drop();
  const gp_Dir n(m_sectionNormal[0], m_sectionNormal[1], m_sectionNormal[2]);
  // In-plane axes: cross the normal with the world axis it is least aligned with.
  const double ax = std::fabs(n.X()), ay = std::fabs(n.Y()), az = std::fabs(n.Z());
  const gp_Dir seed = ax <= ay && ax <= az ? gp::DX() : ay <= az ? gp::DY() : gp::DZ();
  const gp_Dir u = n.Crossed(seed), v = n.Crossed(u);
  const gp_Pnt o = pnt(m_sectionOrigin);
  // Extent: the model's bounding-box corners projected into the plane, plus a margin of the box diagonal.
  double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300;
  for (int i = 0; i < 8; ++i) {
    const gp_Vec d(gp_Pnt((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2]).XYZ() - o.XYZ());
    const double du = d.Dot(gp_Vec(u)), dv = d.Dot(gp_Vec(v));
    umin = std::min(umin, du); umax = std::max(umax, du);
    vmin = std::min(vmin, dv); vmax = std::max(vmax, dv);
  }
  const double margin = 0.06 * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
  if (!(margin > 0)) return drop();
  umin -= margin; umax += margin; vmin -= margin; vmax += margin;
  auto corner = [&](double a, double b) { return o.Translated(gp_Vec(u) * a + gp_Vec(v) * b); };
  const std::array<gp_Pnt, 4> corners{corner(umin, vmin), corner(umax, vmin), corner(umax, vmax), corner(umin, vmax)};
  for (int i = 0; i < 4; ++i) m_sectionCorners[i] = {corners[i].X(), corners[i].Y(), corners[i].Z()};
  m_sectionHasPlane = true;
  refreshSectionGizmo();
}

// The object from m_sectionCorners and the hover state: cheap enough for every mouse move over a handle.
void Viewport::refreshSectionGizmo() {
  if (!m_initialised || !m_sectionHasPlane) return;
  const bool fresh = m_sectionGizmo.IsNull();
  if (fresh) m_sectionGizmo = new SectionGizmo();
  auto* g = static_cast<SectionGizmo*>(m_sectionGizmo.get());
  std::array<gp_Pnt, 4> corners;
  for (int i = 0; i < 4; ++i) corners[i] = pnt(m_sectionCorners[i]);
  g->corners = corners;
  g->normal = gp_Dir(m_sectionNormal[0], m_sectionNormal[1], m_sectionNormal[2]);
  g->hovered = m_sectionHover;
  g->pixel = pixelSize();
  g->lineColor = m_tokens.sel;
  g->hoverColor = m_tokens.amber;  // a handle, not a hover or a selection: distinct from both on any model colour
  if (m_sectionHover >= 0) {
    const gp_Pnt a = corners[m_sectionHover], b = corners[(m_sectionHover + 1) % 4];
    g->anchor = a.Translated(gp_Vec(a, b) * m_sectionHoverT);
  }
  if (fresh) {
    Handle(Graphic3d_SequenceOfHClipPlane) noClip = new Graphic3d_SequenceOfHClipPlane();
    noClip->SetOverrideGlobal(Standard_True);  // the outline lies in the clip plane itself
    g->SetClipPlanes(noClip);
    g->SetZLayer(Graphic3d_ZLayerId_Topmost);
    g->SetInfiniteState(Standard_True);  // never part of Fit All
    m_ctx->Display(g, 0, -1, Standard_False);
  } else {
    m_ctx->Redisplay(m_sectionGizmo, Standard_False);
  }
  m_view->Invalidate();
}

// The side whose handle strip is under the mouse; `t` is where along that side (0..1) the mouse projects.
int Viewport::sectionHandleAt(const QPointF& pos, double& t) const {
  if (!m_initialised || !m_sectionHasPlane) return -1;
  std::array<QPointF, 4> p;
  QPointF centre;
  for (int i = 0; i < 4; ++i) {
    p[i] = widgetPoint(m_sectionCorners[i]);
    centre += p[i] / 4.0;
  }
  int best = -1;
  double bestDist = 1e300;
  for (int i = 0; i < 4; ++i) {
    const QPointF a = p[i], b = p[(i + 1) % 4], ab = b - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    if (len2 < 1) continue;
    const double s = std::clamp(QPointF::dotProduct(pos - a, ab) / len2, 0.0, 1.0);
    const QPointF foot = a + ab * s, off = pos - foot;
    QPointF inward(-ab.y(), ab.x());
    if (QPointF::dotProduct(inward, centre - a) < 0) inward = -inward;
    inward /= std::sqrt(len2);
    const double along = QPointF::dotProduct(off, inward);  // signed: positive = inside the outline
    const double across = std::sqrt(std::max(0.0, QPointF::dotProduct(off, off) - along * along));
    if (along < -kBandOut || along > kBandIn || across > 1.0) continue;
    const double dist = std::fabs(along);
    if (dist < bestDist) { bestDist = dist; best = i; t = s; }
  }
  return best;
}

// Mouse movement -> movement of the plane along its normal: the normal's screen direction at the plane origin
// (px per world unit); when the normal points at the camera the direction collapses and vertical drag is used.
bool Viewport::sectionDragDelta(const QPointF& from, const QPointF& to, double& along) const {
  if (!m_initialised) return false;
  const double L = std::max(pixelSize() * 50.0, 1e-9);
  const opad::Vec3 o = m_sectionOrigin, n = m_sectionNormal;
  const QPointF dir = QPointF(widgetPoint({o[0] + n[0] * L, o[1] + n[1] * L, o[2] + n[2] * L})) - QPointF(widgetPoint(o));
  const double len2 = QPointF::dotProduct(dir, dir);
  if (len2 < 25.0) {  // face-on: less than 5 px for 50 px worth of normal
    along = -(to.y() - from.y()) * pixelSize();
    return true;
  }
  along = QPointF::dotProduct(to - from, dir) / len2 * L;
  return true;
}

void Viewport::setSectionHover(int side, double t) {
  const bool was = m_sectionHover >= 0;
  m_sectionHover = side;
  m_sectionHoverT = t;
  if (side >= 0) {
    // The cursor follows the normal's direction on screen.
    const double L = std::max(pixelSize() * 50.0, 1e-9);
    const opad::Vec3 o = m_sectionOrigin, n = m_sectionNormal;
    const QPointF dir = QPointF(widgetPoint({o[0] + n[0] * L, o[1] + n[1] * L, o[2] + n[2] * L})) - QPointF(widgetPoint(o));
    Qt::CursorShape shape = Qt::SizeAllCursor;
    if (QPointF::dotProduct(dir, dir) >= 25.0) {
      const double deg = std::fmod(std::fabs(std::atan2(dir.y(), dir.x()) * 180.0 / std::acos(-1.0)), 180.0);  // 0 = horizontal
      shape = deg < 22.5 || deg > 157.5 ? Qt::SizeHorCursor : deg < 67.5 ? Qt::SizeBDiagCursor : deg < 112.5 ? Qt::SizeVerCursor : Qt::SizeFDiagCursor;
    }
    setCursor(shape);
    if (!was) setToolTip(tr("Drag to move the section plane along its axis"));
  } else if (was) {
    unsetCursor();
    setToolTip(QString());
  }
  if (was || side >= 0) {
    refreshSectionGizmo();
    redrawScene();
  }
}

// ---- mouse, called first by the viewport's handlers; true = the gizmo took the event
bool Viewport::sectionMousePress(QMouseEvent* e) {
  if (e->button() != Qt::LeftButton || !m_sectionHasPlane || m_sketchInput) return false;
  double t = 0;
  const int side = sectionHandleAt(e->position(), t);
  if (side < 0) return false;
  setSectionHover(side, t);
  m_sectionDrag = true;
  m_sectionDragFrom = e->position();
  m_sectionDragOrigin = m_sectionOrigin;
  m_needFit = false;
  e->accept();
  return true;
}

bool Viewport::sectionMouseMove(QMouseEvent* e) {
  if (!m_sectionHasPlane || m_sketchInput) {
    if (m_sectionHover >= 0) setSectionHover(-1, 0);
    return false;
  }
  if (m_sectionDrag) {
    double d = 0;
    if (sectionDragDelta(m_sectionDragFrom, e->position(), d)) {
      const opad::Vec3 o = m_sectionDragOrigin, n = m_sectionNormal;
      emit sectionDragged({o[0] + n[0] * d, o[1] + n[1] * d, o[2] + n[2] * d});
    }
    e->accept();
    return true;
  }
  if (e->buttons() != Qt::NoButton) return false;  // a navigation drag keeps its cursor
  double t = 0;
  const int side = sectionHandleAt(e->position(), t);
  if (side != m_sectionHover || (side >= 0 && std::fabs(t - m_sectionHoverT) > 1e-3)) setSectionHover(side, t);
  return false;  // the model under the strip may still be hovered
}

// OPAD_BENCH_SECTION: a widget point inside the handle strip of the outline's longest side on screen.
bool Viewport::benchSectionHandle(QPointF& at) const {
  if (!m_sectionHasPlane) return false;
  std::array<QPointF, 4> p;
  QPointF centre;
  for (int i = 0; i < 4; ++i) {
    p[i] = widgetPoint(m_sectionCorners[i]);
    centre += p[i] / 4.0;
  }
  int best = -1;
  double bestLen = 0;
  for (int i = 0; i < 4; ++i) {
    const QPointF ab = p[(i + 1) % 4] - p[i];
    const double len = std::sqrt(QPointF::dotProduct(ab, ab));
    if (len > bestLen) { bestLen = len; best = i; }
  }
  if (best < 0 || bestLen < 1) return false;
  const QPointF a = p[best], ab = p[(best + 1) % 4] - a, mid = a + ab * 0.5;
  QPointF inward(-ab.y(), ab.x());
  if (QPointF::dotProduct(inward, centre - a) < 0) inward = -inward;
  at = mid + inward / bestLen * 6.0;
  return true;
}

bool Viewport::sectionMouseRelease(QMouseEvent* e) {
  if (!m_sectionDrag || e->button() != Qt::LeftButton) return false;
  m_sectionDrag = false;
  double t = 0;
  const int side = sectionHandleAt(e->position(), t);
  setSectionHover(side, t);
  e->accept();
  return true;
}
