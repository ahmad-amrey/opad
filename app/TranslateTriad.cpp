#include "TranslateTriad.hpp"

#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_Group.hxx>
#include <Prs3d_Presentation.hxx>
#include <PrsMgr_PresentationManager.hxx>
#include <SelectMgr_Selection.hxx>

#include <algorithm>
#include <array>
#include <cmath>

#include "Theme.hpp"
#include "Viewport.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
double dot(const opad::Vec3& a, const opad::Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
opad::Vec3 scaled(const opad::Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
opad::Vec3 plus(const opad::Vec3& a, const opad::Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
double distance2(const QPointF& p, const QPointF& a, const QPointF& b) {
  const QPointF ab = b - a;
  const double size = QPointF::dotProduct(ab, ab);
  const double t = size > 1e-12 ? std::clamp(QPointF::dotProduct(p - a, ab) / size, 0.0, 1.0) : 0.0;
  const QPointF d = p - a - ab * t;
  return QPointF::dotProduct(d, d);
}

// Flat arrows facing the camera (as the DimensionHandle's) and a square between them, in widget pixels; the lit part in
// the selection colour (an arrow with a colour of its own: that colour, lighter).
class TriadObject : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(TriadObject, AIS_InteractiveObject)
 public:
  gp_Pnt origin;
  gp_Dir view{0, 0, -1};
  std::vector<std::pair<gp_Dir, QColor>> axes;
  double pixel = 1;
  int lit = -1;  // 0 the square, k arrow k
  QColor fill, rim, active;

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, Standard_Integer) override {
    using T = TranslateTriad;
    const gp_Vec d(view);
    const gp_Vec up = gp_Vec(d).Crossed(std::abs(view.Z()) < 0.9 ? gp_Vec(0, 0, 1) : gp_Vec(1, 0, 0)).Normalized(), side = d.Crossed(up);
    auto part = [&](const QColor& colour, const std::vector<gp_Pnt>& triangles, const std::vector<gp_Pnt>& outline) {
      Handle(Graphic3d_AspectFillArea3d) style = new Graphic3d_AspectFillArea3d;
      style->SetInteriorStyle(Aspect_IS_SOLID);
      style->SetInteriorColor(occ(colour));
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
    for (size_t k = 0; k < axes.size(); ++k) {
      gp_Vec along = gp_Vec(axes[k].first) - d * gp_Vec(axes[k].first).Dot(d);
      if (along.Magnitude() < 0.25) continue;  // seen end on: no arrow to pull
      along.Normalize();
      const gp_Vec across = d.Crossed(along).Normalized();
      auto at = [&](double u, double v) { return origin.Translated(along * (u * pixel) + across * (v * pixel)); };
      const double from = T::kSquare + 3;
      const gp_Pnt s0 = at(from, -T::kShaft), s1 = at(T::kLength - T::kHead, -T::kShaft), s2 = at(T::kLength - T::kHead, T::kShaft), s3 = at(from, T::kShaft);
      const gp_Pnt h0 = at(T::kLength - T::kHead, -T::kWing), h1 = at(T::kLength, 0), h2 = at(T::kLength - T::kHead, T::kWing);
      const QColor own = axes[k].second;
      const bool on = lit == static_cast<int>(k) + 1;
      part(own.isValid() ? (on ? own.lighter(150) : own) : (on ? active : fill), {s0, s1, s2, s0, s2, s3, h0, h1, h2}, {s0, s1, h0, h1, h2, s2, s3});
    }
    auto corner = [&](double u, double v) { return origin.Translated(up * (u * pixel) + side * (v * pixel)); };
    const gp_Pnt c0 = corner(-T::kSquare, -T::kSquare), c1 = corner(-T::kSquare, T::kSquare), c2 = corner(T::kSquare, T::kSquare), c3 = corner(T::kSquare, -T::kSquare);
    part(lit == 0 ? active : fill, {c0, c1, c2, c0, c2, c3}, {c0, c1, c2, c3});
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, Standard_Integer) override {}
};
}  // namespace

TranslateTriad::TranslateTriad(Viewport* view) : m_view(view) {}

TranslateTriad::~TranslateTriad() {
  // Not removed here: the viewport may be gone first (the window closing); its owner hides it while the view lives.
}

void TranslateTriad::place(const opad::Vec3& at, const std::vector<Arrow>& arrows) {
  m_at = at;
  m_arrows = arrows;
  draw();
}

void TranslateTriad::hide() {
  if (m_drag >= 0) return;  // never dropped half way through a pull
  if (m_shown) m_view->removeOverlay(m_object);
  m_shown = false;
  m_hover = -1;
}

void TranslateTriad::setHover(int part) {
  if (part == m_hover) return;
  m_hover = part;
  if (m_shown) draw();
}

void TranslateTriad::draw() {
  if (m_object.IsNull()) {
    m_object = new TriadObject;
    m_object->SetInfiniteState(true);  // not part of what Fit frames
  }
  auto* triad = static_cast<TriadObject*>(m_object.get());
  const Tokens& t = m_view->tokens();
  const double pixel = std::max(1e-9, m_view->pixelSize());
  const opad::Vec3 d = m_view->viewDirection();
  triad->origin = gp_Pnt(m_at[0], m_at[1], m_at[2]);
  triad->view = gp_Dir(d[0], d[1], d[2]);
  triad->axes.clear();
  for (const Arrow& a : m_arrows) triad->axes.push_back({gp_Dir(a.dir[0], a.dir[1], a.dir[2]), a.colour});
  triad->pixel = pixel;
  triad->lit = m_drag >= 0 ? m_drag : m_hover;
  triad->fill = t.fg2;
  triad->rim = t.dark ? QColor("#0b0d10") : QColor("#ffffff");
  triad->active = t.sel;
  // Where the parts are on screen, for presses and hovering.
  m_centre = m_view->widgetPoint(m_at);
  m_screen.clear();
  for (const Arrow& a : m_arrows) {
    const opad::Vec3 along = plus(a.dir, scaled(d, -dot(a.dir, d)));
    const double len = std::sqrt(dot(along, along));
    if (len < 0.25) {
      m_screen.push_back({m_centre, m_centre});
      continue;
    }
    m_screen.push_back({QPointF(m_view->widgetPoint(plus(m_at, scaled(along, (kSquare + 3) * pixel / len)))), QPointF(m_view->widgetPoint(plus(m_at, scaled(along, kLength * pixel / len))))});
  }
  m_object->SetToUpdate();
  if (!m_shown) {
    m_view->showOverlay(m_object);
    m_object->SetZLayer(Graphic3d_ZLayerId_TopOSD);  // over the bodies, a selected one (drawn in Topmost) too
  } else {
    m_view->updateOverlay(m_object);
  }
  m_shown = true;
}

int TranslateTriad::partAt(const QPointF& at) const {
  if (!m_shown) return -1;
  const QPointF c = at - m_centre;
  if (std::abs(c.x()) <= kSquare + 3 && std::abs(c.y()) <= kSquare + 3) return 0;
  int best = -1;
  double nearest = 100;  // within 10 px of a shaft
  for (size_t k = 0; k < m_screen.size(); ++k) {
    if (m_screen[k].first == m_screen[k].second) continue;
    const double d = distance2(at, m_screen[k].first, m_screen[k].second);
    if (d <= nearest) nearest = d, best = static_cast<int>(k) + 1;
  }
  return best;
}

QPointF TranslateTriad::partPoint(int part) const {
  if (part <= 0 || part > static_cast<int>(m_screen.size())) return m_centre;
  const auto& [a, b] = m_screen[static_cast<size_t>(part - 1)];
  return (a + b) / 2;
}

QPointF TranslateTriad::arrowTip(int part) const {
  if (part <= 0 || part > static_cast<int>(m_screen.size())) return m_centre;
  return m_screen[static_cast<size_t>(part - 1)].second;
}

void TranslateTriad::begin(int part, const QPointF& from) {
  m_drag = part;
  m_from = from;
  if (part > 0 && part <= static_cast<int>(m_arrows.size())) {  // the pointer's move projected on the arrow's direction on screen
    m_axis = m_arrows[static_cast<size_t>(part - 1)].dir;
    const double step = std::max(1e-9, m_view->pixelSize()) * 50;
    m_screenAxis = (QPointF(m_view->widgetPoint(plus(m_at, scaled(m_axis, step)))) - QPointF(m_view->widgetPoint(m_at))) / step;
  } else {  // in the view's plane through the triad
    m_plane = m_view->annotationCameraPlane(m_at);
  }
  if (m_shown) draw();
}

double TranslateTriad::along(const QPointF& to) const {
  const double size = QPointF::dotProduct(m_screenAxis, m_screenAxis);
  if (m_drag <= 0 || size < 1e-12) return 0;
  return QPointF::dotProduct(to - m_from, m_screenAxis) / size;
}

opad::Vec3 TranslateTriad::inPlane(const QPointF& to) const {
  double u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  if (m_drag != 0 || !m_view->planePoint(m_from, m_plane, u0, v0) || !m_view->planePoint(to, m_plane, u1, v1)) return {0, 0, 0};
  return plus(scaled(m_plane.x, u1 - u0), scaled(m_plane.y, v1 - v0));
}

void TranslateTriad::end() {
  m_drag = -1;
  if (m_shown) draw();
}
