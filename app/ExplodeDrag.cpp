// Exploded views (UI-36): moving a part by hand. The selected part's DimensionHandle arrow is its first axis (its own
// direction, or X/Y/Z as the panel says) and takes typed values; this triad adds the two axes square to it and a square
// in the middle that moves the part in the view's plane, and a press on the part itself drags it the same way (a press
// that does not move is a click, handed to the view). Every move only changes the spec's manual offset of the part's
// unit (nothing is written); the part follows at once through the Explode look layer.
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <SelectMgr_Selection.hxx>
#include <QApplication>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "ExplodeArea.hpp"
#include "Theme.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
gp_Pnt point(const opad::Vec3& v) { return gp_Pnt(v[0], v[1], v[2]); }
double dot(const opad::Vec3& a, const opad::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
opad::Vec3 cross(const opad::Vec3& a, const opad::Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
opad::Vec3 scaled(const opad::Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
opad::Vec3 plus(const opad::Vec3& a, const opad::Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
double distance2(const QPointF& p, const QPointF& a, const QPointF& b) {
  const QPointF ab = b - a;
  const double size = QPointF::dotProduct(ab, ab);
  const double t = size > 1e-12 ? std::clamp(QPointF::dotProduct(p - a, ab) / size, 0.0, 1.0) : 0.0;
  const QPointF d = p - a - ab * t;
  return QPointF::dotProduct(d, d);
}

// Two flat arrows facing the camera (as the DimensionHandle's) and a square between them, in widget pixels; the part
// under the mouse in the selection colour.
class Triad : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(Triad, AIS_InteractiveObject)
 public:
  static constexpr double kLength = 40, kHead = 14, kShaft = 2.5, kWing = 7.5, kSquare = 6;
  gp_Pnt origin;
  gp_Dir view{0, 0, -1};
  std::array<gp_Dir, 2> axes{gp_Dir(1, 0, 0), gp_Dir(0, 1, 0)};
  double pixel = 1;
  int hover = -1;  // 0 the square, 1-2 an arrow
  QColor fill, rim, active;

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    const gp_Vec d(view);
    const gp_Vec up = gp_Vec(d).Crossed(std::abs(view.Z()) < 0.9 ? gp_Vec(0, 0, 1) : gp_Vec(1, 0, 0)).Normalized(), side = d.Crossed(up);
    auto part = [&](int index, const std::vector<gp_Pnt>& triangles, const std::vector<gp_Pnt>& outline) {
      Handle(Graphic3d_AspectFillArea3d) style = new Graphic3d_AspectFillArea3d;
      style->SetInteriorStyle(Aspect_IS_SOLID);
      style->SetInteriorColor(occ(hover == index ? active : fill));
      style->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
      style->SetSuppressBackFaces(false);
      const Handle(Graphic3d_Group) fills = prs->NewGroup();
      fills->SetGroupPrimitivesAspect(style);
      Handle(Graphic3d_ArrayOfTriangles) body = new Graphic3d_ArrayOfTriangles(static_cast<int>(triangles.size()));
      for (const auto& p : triangles) body->AddVertex(p);
      fills->AddPrimitiveArray(body);
      const Handle(Graphic3d_Group) lines = prs->NewGroup();
      lines->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(occ(rim), Aspect_TOL_SOLID, 1.5));
      Handle(Graphic3d_ArrayOfSegments) rims = new Graphic3d_ArrayOfSegments(static_cast<int>(2 * outline.size()));
      for (size_t i = 0; i < outline.size(); ++i) {
        rims->AddVertex(outline[i]);
        rims->AddVertex(outline[(i + 1) % outline.size()]);
      }
      lines->AddPrimitiveArray(rims);
    };
    for (int k = 0; k < 2; ++k) {
      gp_Vec along = gp_Vec(axes[static_cast<size_t>(k)]) - d * gp_Vec(axes[static_cast<size_t>(k)]).Dot(d);
      if (along.Magnitude() < 0.25) continue;  // seen end on: no arrow to pull
      along.Normalize();
      const gp_Vec across = d.Crossed(along).Normalized();
      auto at = [&](double u, double v) { return origin.Translated(along * (u * pixel) + across * (v * pixel)); };
      const double from = kSquare + 3;
      const gp_Pnt s0 = at(from, -kShaft), s1 = at(kLength - kHead, -kShaft), s2 = at(kLength - kHead, kShaft), s3 = at(from, kShaft);
      const gp_Pnt h0 = at(kLength - kHead, -kWing), h1 = at(kLength, 0), h2 = at(kLength - kHead, kWing);
      part(k + 1, {s0, s1, s2, s0, s2, s3, h0, h1, h2}, {s0, s1, h0, h1, h2, s2, s3});
    }
    auto corner = [&](double u, double v) { return origin.Translated(up * (u * pixel) + side * (v * pixel)); };
    const gp_Pnt c0 = corner(-kSquare, -kSquare), c1 = corner(-kSquare, kSquare), c2 = corner(kSquare, kSquare), c3 = corner(kSquare, -kSquare);
    part(0, {c0, c1, c2, c0, c2, c3}, {c0, c1, c2, c3});
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}
};
}  // namespace

// The two axes square to the first one: the world axes least along it, made square (X and Y for a part going up), each
// pointing the positive way of its largest component.
std::array<opad::Vec3, 2> Explode::crossAxes(const opad::Vec3& axis) {
  const double n = std::sqrt(dot(axis, axis));
  const opad::Vec3 a = n > 0 ? scaled(axis, 1 / n) : opad::Vec3{0, 0, 1};
  size_t k = 0;
  for (size_t i = 1; i < 3; ++i)
    if (std::abs(a[i]) < std::abs(a[k]) - 1e-9) k = i;
  opad::Vec3 e{0, 0, 0};
  e[k] = 1;
  opad::Vec3 u = plus(e, scaled(a, -dot(e, a)));
  u = scaled(u, 1 / std::sqrt(dot(u, u)));
  opad::Vec3 v = cross(a, u);
  size_t big = 0;
  for (size_t i = 1; i < 3; ++i)
    if (std::abs(v[i]) > std::abs(v[big]) + 1e-9) big = i;
  if (v[big] < 0) v = scaled(v, -1);
  return {u, v};
}

void Explode::placeTriad(bool shown, const opad::Vec3& at, const opad::Vec3& axis) {
  Viewport* view = services().viewport();
  if (!shown) {
    if (m_drag.part >= 0) return;  // the part being dragged is never dropped mid-way
    if (m_triadShown) view->removeOverlay(m_triad);
    m_triadShown = false;
    m_triadHover = -1;
    return;
  }
  if (m_triad.IsNull()) {
    m_triad = new Triad;
    m_triad->SetInfiniteState(true);  // not part of what Fit frames
  }
  auto* triad = static_cast<Triad*>(m_triad.get());
  m_triadAt = at;
  m_triadAxes = crossAxes(axis);
  const Tokens& t = view->tokens();
  const double pixel = std::max(1e-9, view->pixelSize());
  const opad::Vec3 d = view->viewDirection();
  triad->origin = point(at);
  triad->view = gp_Dir(d[0], d[1], d[2]);
  for (size_t k = 0; k < 2; ++k) triad->axes[k] = gp_Dir(m_triadAxes[k][0], m_triadAxes[k][1], m_triadAxes[k][2]);
  triad->pixel = pixel;
  triad->hover = m_drag.part >= 0 ? m_drag.part : m_triadHover;
  triad->fill = t.fg2;
  triad->rim = t.dark ? QColor("#0b0d10") : QColor("#ffffff");
  triad->active = t.sel;
  // Where the parts are on screen, for presses and hovering.
  m_triadCentre = view->widgetPoint(at);
  for (size_t k = 0; k < 2; ++k) {
    const opad::Vec3 along = plus(m_triadAxes[k], scaled(d, -dot(m_triadAxes[k], d)));
    const double len = std::sqrt(dot(along, along));
    if (len < 0.25) {
      m_triadArrows[k] = {m_triadCentre, m_triadCentre};
      continue;
    }
    const opad::Vec3 tip = plus(at, scaled(along, Triad::kLength * pixel / len));
    m_triadArrows[k] = {QPointF(view->widgetPoint(plus(at, scaled(along, (Triad::kSquare + 3) * pixel / len)))), QPointF(view->widgetPoint(tip))};
  }
  triad->SetToUpdate();
  if (!m_triadShown) {
    view->showOverlay(m_triad);
    m_triad->SetZLayer(Graphic3d_ZLayerId_TopOSD);  // over the part, which is selected and so drawn in Topmost too
  } else {
    view->updateOverlay(m_triad);
  }
  m_triadShown = true;
}

int Explode::triadPart(const QPointF& at) const {
  if (!m_triadShown) return -1;
  const QPointF c = at - m_triadCentre;
  if (std::abs(c.x()) <= Triad::kSquare + 3 && std::abs(c.y()) <= Triad::kSquare + 3) return 0;
  for (size_t k = 0; k < 2; ++k)
    if (m_triadArrows[k].first != m_triadArrows[k].second && distance2(at, m_triadArrows[k].first, m_triadArrows[k].second) <= 100) return static_cast<int>(k) + 1;
  return -1;
}

QPointF Explode::triadPoint(int part) const {
  if (part <= 0) return m_triadCentre;
  const auto& [a, b] = m_triadArrows[static_cast<size_t>(part - 1)];
  return (a + b) / 2;
}

void Explode::beginDrag(int part, const QPointF& at) {
  if (m_dragUnit < 0 || m_dragUnit >= static_cast<int>(m_units.size())) return;
  Viewport* view = services().viewport();
  const opad::ExplodeUnit& u = m_units[static_cast<size_t>(m_dragUnit)];
  m_drag = {};
  m_drag.part = part;
  m_drag.unit = u.id;
  m_drag.start = at;
  m_drag.scale = std::max(0.2, opad::explode_progress(u, m_t));  // the part moves this much of its offset at t
  if (const auto m = m_spec.offsets.find(u.id); m != m_spec.offsets.end()) m_drag.manual = m->second;
  if (part > 0) {  // along an arrow: the mouse's move projected on the arrow's direction on screen
    const opad::Vec3 axis = m_triadAxes[static_cast<size_t>(part - 1)];
    const double step = std::max(1e-9, view->pixelSize()) * 50;
    m_drag.axis = axis;
    m_drag.screenAxis = (QPointF(view->widgetPoint(plus(m_triadAt, scaled(axis, step)))) - QPointF(view->widgetPoint(m_triadAt))) / step;
  } else {  // in the view's plane through the part's middle
    m_drag.plane = view->annotationCameraPlane(m_triadAt);
  }
  hideHint(true);
  placeHandle();
}

void Explode::dragTo(const QPointF& at) {
  if (m_drag.part < 0) return;
  const auto unit = std::find_if(m_units.begin(), m_units.end(), [&](const opad::ExplodeUnit& u) { return u.id == m_drag.unit; });
  if (unit == m_units.end()) return endDrag();  // laid out again without it
  opad::Vec3 move{0, 0, 0};
  if (m_drag.part > 0) {
    const double size = QPointF::dotProduct(m_drag.screenAxis, m_drag.screenAxis);
    if (size < 1e-12) return;
    move = scaled(m_drag.axis, QPointF::dotProduct(at - m_drag.start, m_drag.screenAxis) / size);
  } else {
    double u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    Viewport* view = services().viewport();
    if (!view->planePoint(m_drag.start, m_drag.plane, u0, v0) || !view->planePoint(at, m_drag.plane, u1, v1)) return;
    move = plus(scaled(m_drag.plane.x, u1 - u0), scaled(m_drag.plane.y, v1 - v0));
  }
  const opad::Vec3 manual = plus(m_drag.manual, scaled(move, 1 / m_drag.scale));
  if (dot(manual, manual) < 1e-18) m_spec.offsets.erase(m_drag.unit);
  else m_spec.offsets[m_drag.unit] = manual;
  apply();
}

void Explode::endDrag() {
  if (m_drag.part < 0) return;
  m_drag = {};
  opad::explode_stage(m_units, m_spec);  // one after another: its turn now that it is let go
  apply();
  services().browser()->refreshDecorations();
}

// The viewport's mouse events, before the view and the DimensionHandle see them (an application filter installed after
// the handle's).
bool Explode::dragEvent(QEvent* event) {
  Viewport* view = services().viewport();
  if (m_replaying) return false;
  auto* mouse = static_cast<QMouseEvent*>(event);
  const QPointF at = mouse->position();
  switch (event->type()) {
    case QEvent::MouseButtonPress: {
      if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier || !m_triadShown || view->ghostsPickable()) return false;
      if (const int part = triadPart(at); part >= 0) {
        beginDrag(part, at);
        return true;
      }
      if (m_handle->grips(at)) return false;  // its arrow: the handle's
      // On the selected part itself: a drag moves it in the view's plane, a press that does not move is a click.
      opad::Ref ref;
      if (!view->referenceAt(at, ref) || ref.body.empty() || unitOf(ref.body) != m_dragUnit) return false;
      m_pressAt = at;
      m_pressGlobal = mouse->globalPosition();
      m_bodyArmed = true;
      return true;
    }
    case QEvent::MouseMove:
      if (m_drag.part >= 0) {
        dragTo(at);
        return true;
      }
      if (m_bodyArmed) {
        if ((at - m_pressAt).manhattanLength() >= QApplication::startDragDistance()) {
          m_bodyArmed = false;
          beginDrag(0, m_pressAt);
          dragTo(at);
        }
        return true;
      }
      if (mouse->buttons() == Qt::NoButton && m_triadShown) {
        const int hover = triadPart(at);
        if (hover != m_triadHover) {
          m_triadHover = hover;
          placeHandle();
        }
      }
      return false;
    case QEvent::MouseButtonRelease:
      if (mouse->button() != Qt::LeftButton) return false;
      if (m_drag.part >= 0) {
        endDrag();
        return true;
      }
      if (m_bodyArmed) {  // a click after all: the view gets it as it came
        m_bodyArmed = false;
        QMouseEvent press(QEvent::MouseButtonPress, m_pressAt, m_pressGlobal, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, m_pressAt, m_pressGlobal, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        m_replaying = true;
        QCoreApplication::sendEvent(view, &press);
        QCoreApplication::sendEvent(view, &release);
        m_replaying = false;
        return true;
      }
      return false;
    default: return false;
  }
}
