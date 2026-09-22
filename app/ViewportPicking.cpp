// Exact circle-center discovery and navigation picking independent of the selection filter.
#include "Viewport.hpp"

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopoDS_Vertex.hxx>
#include <Prs3d_PointAspect.hxx>
#include <QApplication>
#include <QKeyEvent>
#include "Jobs.hpp"
#include <cmath>

Handle(AIS_Shape) Viewport::centerMarker(const opad::Ref& ref, const gp_Pnt& point) {
  const std::string key = ref.str();
  auto found = m_centers.find(key);
  if (found != m_centers.end()) return found->second.ais;
  Handle(AIS_Shape) marker = new AIS_Shape(BRepBuilderAPI_MakeVertex(point).Vertex());
  const QColor color = m_tokens.sel;
  marker->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_O_PLUS,
      Quantity_Color(color.redF(), color.greenF(), color.blueF(), Quantity_TOC_sRGB), 3.0));
  marker->SetZLayer(Graphic3d_ZLayerId_Topmost);
  m_ctx->Display(marker, 0, -1, false);
  m_ctx->Load(marker, -1);
  m_ctx->Activate(marker, 0);
  m_ctx->SetSelectionSensitivity(marker, 0, 8);
  marker->GlobalSelOwner()->SetPriority(10);
  m_centers.emplace(key, CenterMarker{ref, point, marker});
  m_centerObjects[marker.get()] = key;
  redrawScene();
  return marker;
}

void Viewport::discoverCenter() {
  if (m_filter != SelFilter::Vertex || !m_bodiesPickable || m_sketchInput || m_centerLocked || !m_ctx->HasDetected()) return;
  Handle(CircleOwner) circle = Handle(CircleOwner)::DownCast(m_ctx->DetectedOwner());
  if (circle.IsNull()) return;
  auto body = m_nodeOf.find(m_ctx->DetectedInteractive().get());
  if (body == m_nodeOf.end()) return;
  opad::Ref ref;
  ref.body = body->second;
  ref.kind = opad::Ref::Kind::Center;
  ref.index = circle->index();
  if (ref.str() == m_activeCenter) return;
  // An unlocked marker remains reachable across empty space; another arc replaces it. Selected
  // centers persist independently, so discovering the second circle cannot discard the first pick.
  auto previous = m_centers.find(m_activeCenter);
  if (previous != m_centers.end() && !m_ctx->IsSelected(previous->second.ais)) {
    m_centerObjects.erase(previous->second.ais.get());
    m_ctx->Remove(previous->second.ais, false);
    m_centers.erase(previous);
  }
  for (const auto& [key, marker] : m_centers) marker.ais->GlobalSelOwner()->SetPriority(5);
  centerMarker(ref, circle->center.Transformed(m_items.at(ref.body).ais->Transformation()))->GlobalSelOwner()->SetPriority(10);
  m_activeCenter = ref.str();
}

bool Viewport::eventFilter(QObject* object, QEvent* e) {
  if (object != this && e->type() == QEvent::KeyPress && underMouse() && window()->isActiveWindow() && !m_sketchInput) {
    auto* key = static_cast<QKeyEvent*>(e);
    if (key->key() == Qt::Key_Shift && !key->isAutoRepeat() && toggleCenterLock()) return true;
  }
  return QWidget::eventFilter(object, e);
}

bool Viewport::toggleCenterLock() {
  if (!m_initialised || m_blocked || m_filter != SelFilter::Vertex || m_activeCenter.empty() || QApplication::mouseButtons() != Qt::NoButton) return false;
  m_centerLocked = !m_centerLocked;
  m_hoverOwner = nullptr;
  const QString hint = m_centerLocked ? tr("Center locked · click center · Shift unlock") : tr("Circle center · Shift lock · click center");
  m_hover = hint;
  emit hoverChanged(hint);
  redrawScene();
  return true;
}

void Viewport::clearCenters() {
  if (!m_initialised) return;
  for (const auto& [key, center] : m_centers) m_ctx->Remove(center.ais, false);
  m_centers.clear();
  m_centerObjects.clear();
  m_activeCenter.clear();
  m_centerLocked = false;
  m_hoverOwner = nullptr;
}

bool Viewport::navigationPoint(const Graphic3d_Vec2i& cursor, gp_Pnt& point) {
  if (m_navSelector.IsNull()) return false;
  m_navSelector->Pick(cursor.x(), cursor.y(), m_view);
  for (int i = 1; i <= m_navSelector->NbPicked(); ++i) {
    auto node = m_navNodes.find(m_navSelector->Picked(i)->Selectable().get());
    if (node == m_navNodes.end()) continue;
    auto item = m_items.find(node->second);
    if (item == m_items.end() || !m_ctx->IsDisplayed(item->second.ais)) continue;
    point = m_navSelector->PickedPoint(i);
    return true;
  }
  return false;
}

gp_Pnt Viewport::orbitPoint(const Graphic3d_Vec2i& cursor) {
  gp_Pnt point;
  if (navigationPoint(cursor, point)) return point;
  // Empty space: intersect the cursor ray with the current camera's focus plane. This pivot is
  // under the cursor even after panning/zooming far away from the model's bounding-box center.
  double x, y, z, dx, dy, dz;
  m_view->ConvertWithProj(cursor.x(), cursor.y(), x, y, z, dx, dy, dz);
  const gp_Vec ray(dx, dy, dz), normal(m_view->Camera()->Direction());
  const double denominator = ray.Dot(normal);
  if (std::abs(denominator) < 1e-12) return m_view->Camera()->Center();
  return gp_Pnt(x, y, z).Translated(ray * (gp_Vec(gp_Pnt(x, y, z), m_view->Camera()->Center()).Dot(normal) / denominator));
}

gp_Pnt Viewport::GravityPoint(const Handle(AIS_InteractiveContext)&, const Handle(V3d_View)&) {
  const Graphic3d_Vec2i cursor = m_cubeGesture
      ? devicePos(QPointF(width() * 0.5, height() * 0.5))
      : Graphic3d_Vec2i(int(myGL.OrbitRotation.PointStart.x()), int(myGL.OrbitRotation.PointStart.y()));
  return orbitPoint(cursor);
}

void Viewport::focusCube() {
  const Handle(Graphic3d_Camera)& camera = m_view->Camera();
  const gp_Pnt surface = orbitPoint(devicePos(QPointF(width() * 0.5, height() * 0.5)));
  // Change only depth on the central view ray: preserve direction, image position and zoom.
  const gp_Vec direction(camera->Direction());
  const double depth = gp_Vec(camera->Eye(), surface).Dot(direction);
  if (depth > 1e-7) camera->SetCenter(camera->Eye().Translated(direction * depth));
}
