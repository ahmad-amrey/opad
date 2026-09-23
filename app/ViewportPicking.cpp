// Exact circle-center discovery and navigation picking independent of the selection filter.
#include "Viewport.hpp"
#include "NavCube.hpp"

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopoDS_Vertex.hxx>
#include <Prs3d_PointAspect.hxx>
#include <QApplication>
#include <QKeyEvent>
#include "Jobs.hpp"
#include <cmath>
#include <queue>
#include <algorithm>

Handle(AIS_Shape) Viewport::centerMarker(const opad::Ref& ref, const gp_Pnt& point) {
  const std::string key = ref.str();
  auto found = m_centers.find(key);
  if (found != m_centers.end()) return found->second.ais;
  Handle(AIS_Shape) marker = new AIS_Shape(BRepBuilderAPI_MakeVertex(point).Vertex());
  const QColor color = m_tokens.sel;
  marker->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_O,
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
  m_inferenceChoice = 0; refreshCenterStyles();
}

void Viewport::refreshCenterStyles() {
  if (!m_initialised) return;
  for (const auto& [key, marker] : m_centers) {
    const bool selected=m_ctx->IsSelected(marker.ais);
    const bool center=key==m_activeCenter, tracking=key==m_trackingMarker;
    const bool candidate=(center && m_inferenceChoice==0)
        || (tracking && (m_activeCenter.empty() || m_inferenceChoice>0));
    const bool locked=(center && m_centerLocked) || (tracking && m_trackingLocked);
    auto aspect=marker.ais->Attributes()->PointAspect();
    aspect->SetTypeOfMarker(selected ? Aspect_TOM_O_PLUS : Aspect_TOM_O);
    aspect->SetScale(selected ? 4.0 : locked ? 7.0 : candidate ? 5.0 : 3.0);
    marker.ais->SynchronizeAspects();
  }
}

bool Viewport::inferenceKey(QKeyEvent* key) {
  if (key->key()!=Qt::Key_Shift || key->isAutoRepeat() || m_sketchInput || m_blocked || !m_initialised) return false;
  if (key->type()==QEvent::KeyPress) {
    if (m_shiftHeld || QApplication::mouseButtons()!=Qt::NoButton) return false;
    const bool center=!m_activeCenter.empty();
    const int count=int(m_trackingCandidates.size())+int(center);
    if (!count) return false;
    m_inferenceChoice=std::clamp(m_inferenceChoice,0,count-1);
    m_shiftHeld=true; m_shiftClock.start();
    m_centerLocked=center && m_inferenceChoice==0;
    m_trackingLocked=!m_centerLocked;
    if(m_trackingLocked) m_lockedTracking=m_trackingCandidates[m_inferenceChoice-int(center)];
  } else {
    if (!m_shiftHeld) return false;
    const bool tap=m_shiftClock.elapsed()<250;
    m_shiftHeld=m_centerLocked=m_trackingLocked=false;
    const int count=int(m_trackingCandidates.size())+int(!m_activeCenter.empty());
    if(tap && count>1) m_inferenceChoice=(m_inferenceChoice+1)%count;
  }
  m_trackingDirty=true; m_hoverOwner=nullptr;
  refreshCenterStyles();
  emit hoverChanged(m_centerLocked ? tr("Center locked - release Shift to unlock") : tr("Tap Shift to cycle enlarged targets; hold Shift to lock"));
  redrawScene(); return true;
}

bool Viewport::eventFilter(QObject* object, QEvent* e) {
  if ((e->type()==QEvent::KeyPress || e->type()==QEvent::KeyRelease)
      && (object==this || underMouse() || m_shiftHeld) && window()->isActiveWindow())
    if(inferenceKey(static_cast<QKeyEvent*>(e))) return true;
  if (e->type()==QEvent::WindowDeactivate && object==window()) {
    m_shiftHeld=m_centerLocked=m_trackingLocked=false; refreshCenterStyles();
  }
  return QWidget::eventFilter(object,e);
}

void Viewport::clearCenters() {
  clearTracking();
  if (!m_initialised) return;
  for (const auto& [key, center] : m_centers) m_ctx->Remove(center.ais, false);
  m_centers.clear();
  m_centerObjects.clear();
  m_activeCenter.clear();
  m_centerLocked = false;
  m_shiftHeld = false;
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
  return centralOrbitPoint();
}

gp_Pnt Viewport::centralOrbitPoint() {
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  const int cx = w / 2, cy = h / 2;
  gp_Pnt point;
  // Expanded triangle picks can return a vertex far from the requested ray near
  // a silhouette. Reject those candidates instead of orbiting about a distant corner.
  auto pick = [&](int x, int y) {
    if (!navigationPoint(Graphic3d_Vec2i(x, y), point)) return false;
    Standard_Integer px, py;
    m_view->Convert(point.X(), point.Y(), point.Z(), px, py);
    return std::abs(px - x) <= 2 && std::abs(py - y) <= 2;
  };
  if (m_navSelector.IsNull()) return m_view->Camera()->Center();
  if (pick(cx, cy)) return point;

  // Search screen regions nearest the center first. Rectangle picks use the existing
  // mesh BVHs to discard empty regions, so holes and small/off-center parts are found
  // without walking every triangle or relying on a sparse grid of sample rays.
  struct Region {
    int x0, y0, x1, y1;
    double distance;
    bool operator<(const Region& other) const { return distance > other.distance; }
  };
  std::priority_queue<Region> regions;
  auto add = [&](int x0, int y0, int x1, int y1) {
    const double dx = cx - std::clamp(cx, x0, x1), dy = cy - std::clamp(cy, y0, y1);
    regions.push({x0, y0, x1, y1, dx * dx + dy * dy});
  };
  if (w <= 0 || h <= 0 || m_navSelector.IsNull()) return m_view->Camera()->Center();
  add(0, 0, w - 1, h - 1);
  m_navSelector->AllowOverlapDetection(true);
  while (!regions.empty()) {
    const Region r = regions.top(); regions.pop();
    if (r.x1 - r.x0 <= 1 && r.y1 - r.y0 <= 1) {
      // Point picking resolves the frontmost non-clipped surface, regardless of
      // selection filter. Accuracy is within the existing one-pixel pick tolerance.
      if (pick((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2)) return point;
      continue;
    }
    m_navSelector->Pick(r.x0, r.y0, r.x1, r.y1, m_view);
    bool occupied = false;
    for (int i = 1; i <= m_navSelector->NbPicked(); ++i) {
      auto node = m_navNodes.find(m_navSelector->Picked(i)->Selectable().get());
      if (node == m_navNodes.end()) continue;
      auto item = m_items.find(node->second);
      if (item != m_items.end() && m_ctx->IsDisplayed(item->second.ais)) { occupied = true; break; }
    }
    if (!occupied) continue;
    if (r.x1 - r.x0 >= r.y1 - r.y0) {
      const int mid = (r.x0 + r.x1) / 2;
      add(r.x0, r.y0, mid, r.y1); add(mid, r.y0, r.x1, r.y1);
    } else {
      const int mid = (r.y0 + r.y1) / 2;
      add(r.x0, r.y0, r.x1, mid); add(r.x0, mid, r.x1, r.y1);
    }
  }
  // No visible geometry (empty document or model entirely outside the view).
  return m_view->Camera()->Center();
}

gp_Pnt Viewport::GravityPoint(const Handle(AIS_InteractiveContext)&, const Handle(V3d_View)&) {
  // focusCube() acquired this point on press; don't repeat the search when the
  // same press becomes a drag.
  if (m_cubeGesture) return Handle(NavCube)::DownCast(m_cube)->orbitPoint();
  return orbitPoint(Graphic3d_Vec2i(int(myGL.OrbitRotation.PointStart.x()), int(myGL.OrbitRotation.PointStart.y())));
}

void Viewport::focusCube() {
  Handle(NavCube)::DownCast(m_cube)->setOrbitPoint(centralOrbitPoint());
}
