// Exact circle-center discovery and navigation picking independent of the selection filter.
#include "Viewport.hpp"
#include "NavCube.hpp"

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
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
  const QColor color = QColor(Qt::white);
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

void Viewport::setCenterPicking(bool on,const QPointF& position) {
  if(!m_initialised || m_ctrlCenterPick==on) return;
  m_ctrlCenterPick=on;
  if(!on) {
    m_activeCenter.clear();
    for(auto it=m_centers.begin();it!=m_centers.end();) {
      if(it->second.ref.kind!=opad::Ref::Kind::Center || m_ctx->IsSelected(it->second.ais)) {++it;continue;}
      m_centerObjects.erase(it->second.ais.get());m_ctx->Remove(it->second.ais,false);it=m_centers.erase(it);
    }
  }
  ResetPreviousMoveTo();m_hoverOwner=nullptr;
  const auto at=devicePos(position);m_ctx->MoveTo(at.x(),at.y(),m_view,false);
  discoverCenter();redrawScene();
}

void Viewport::discoverCenter() {
  if (!m_ctrlCenterPick || m_filter != SelFilter::Vertex || !m_bodiesPickable || m_sketchInput || m_centerLocked || !m_ctx->HasDetected()) return;
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
  centerMarker(ref, circle->center.Transformed(m_ctx->DetectedInteractive()->Transformation()))->GlobalSelOwner()->SetPriority(10);
  m_activeCenter = ref.str();
  m_inferenceChoice = 0; refreshCenterStyles();
}

void Viewport::refreshCenterStyles() {
  if (!m_initialised) return;
  for (const auto& [key, marker] : m_centers) {
    const bool selected=m_ctx->IsSelected(marker.ais);
    const bool center=key==m_activeCenter, tracking=key==m_trackingMarker;
    const bool candidate=tracking;
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
    const int count=int(m_trackingCandidates.size());
    if (!count) return false;
    m_inferenceChoice=std::clamp(m_inferenceChoice,0,count-1);
    m_shiftHeld=true; m_shiftClock.start();
    m_centerLocked=false;
    m_trackingLocked=true;
    m_lockedTracking=m_trackingCandidates[m_inferenceChoice];
  } else {
    if (!m_shiftHeld) return false;
    const bool tap=m_shiftClock.elapsed()<250;
    m_shiftHeld=m_centerLocked=m_trackingLocked=false;
    const int count=int(m_trackingCandidates.size());
    if(tap && count>1) m_inferenceChoice=(m_inferenceChoice+1)%count;
  }
  m_trackingDirty=true; m_hoverOwner=nullptr;
  refreshCenterStyles();
  emit hoverChanged(m_trackingLocked ? tr("Tracking locked - release Shift to unlock") : tr("Tap Shift to cycle tracking points; hold Shift to lock"));
  redrawScene(); return true;
}

bool Viewport::eventFilter(QObject* object, QEvent* e) {
  if(e->type()==QEvent::MouseButtonPress || e->type()==QEvent::MouseButtonDblClick) {
    const auto widget=qobject_cast<QWidget*>(object);
    if(widget && (widget==window() || window()->isAncestorOf(widget)))resetHoverFade();
  }
  if((e->type()==QEvent::KeyPress || e->type()==QEvent::KeyRelease) && static_cast<QKeyEvent*>(e)->key()==Qt::Key_Control
      && (object==this || underMouse() || m_ctrlCenterPick))
    setCenterPicking(e->type()==QEvent::KeyPress,m_trackingCursor);
  if ((e->type()==QEvent::KeyPress || e->type()==QEvent::KeyRelease)
      && (object==this || underMouse() || m_shiftHeld)
      && (window()->isActiveWindow() || m_shiftHeld
          || (QApplication::activeWindow() && window()->isAncestorOf(QApplication::activeWindow()))))
    if(inferenceKey(static_cast<QKeyEvent*>(e))) return true;
  if (e->type()==QEvent::ApplicationDeactivate) {
    setCenterPicking(false,m_trackingCursor);
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

// Orbit pivot for a press at `cursor`: the surface under it, a drawing's plane when it is inside the drawing, else the
// geometry nearest the pointer on screen, surface or curve (TODO 10 A7: 08d0841 had turned empty space to the geometry
// nearest the view's centre, which the view cube still uses). Only an empty view keeps the camera's focus.
gp_Pnt Viewport::orbitPoint(const Graphic3d_Vec2i& cursor) {
  gp_Pnt point;
  if (navigationPoint(cursor, point)) return point;
  const QPointF at(cursor.x()/devicePixelRatioF(),cursor.y()/devicePixelRatioF());bool inside=false;
  point=drawingPlanePoint(at,inside);if(inside)return point;
  gp_Pnt surface;double surfaceDistance=1e300;
  if(nearestSurface(cursor.x(),cursor.y(),surface)) {
    Standard_Integer px=0,py=0;m_view->Convert(surface.X(),surface.Y(),surface.Z(),px,py);
    surfaceDistance=std::hypot(double(px-cursor.x()),double(py-cursor.y()));
  }
  bool curve=false;double curveDistance=1e300;
  const gp_Pnt onCurve=nearestCurvePoint(at,curve,curveDistance);
  if(curve && std::sqrt(curveDistance)*devicePixelRatioF()<surfaceDistance)return onCurve;
  if(surfaceDistance<1e300)return surface;
  return m_view->Camera()->Center();
}

gp_Pnt Viewport::centralOrbitPoint() {
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  gp_Pnt point;
  if (nearestSurface(w / 2, h / 2, point)) return point;
  // No visible geometry (empty document or model entirely outside the view).
  return drawingOrbitPoint();
}

// The frontmost visible, unclipped surface at the screen position nearest (cx, cy) in backing pixels.
bool Viewport::nearestSurface(int cx, int cy, gp_Pnt& point) {
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  // Expanded triangle picks can return a vertex far from the requested ray near
  // a silhouette. Reject those candidates instead of orbiting about a distant corner.
  auto pick = [&](int x, int y) {
    if (!navigationPoint(Graphic3d_Vec2i(x, y), point)) return false;
    Standard_Integer px, py;
    m_view->Convert(point.X(), point.Y(), point.Z(), px, py);
    return std::abs(px - x) <= 2 && std::abs(py - y) <= 2;
  };
  if (m_navSelector.IsNull() || w <= 0 || h <= 0) return false;
  if (cx >= 0 && cy >= 0 && cx < w && cy < h && pick(cx, cy)) return true;

  // Search screen regions nearest the point first. Rectangle picks use the existing
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
  add(0, 0, w - 1, h - 1);
  m_navSelector->AllowOverlapDetection(true);
  while (!regions.empty()) {
    const Region r = regions.top(); regions.pop();
    if (r.x1 - r.x0 <= 1 && r.y1 - r.y0 <= 1) {
      // Point picking resolves the frontmost non-clipped surface, regardless of
      // selection filter. Accuracy is within the existing one-pixel pick tolerance.
      if (pick((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2)) return true;
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
  return false;
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


// Inside a drawing's bounds under the pointer: the point of its plane there (curves enclose usable drawing space; the
// orbit must not jump to the outline of a large drawing).
gp_Pnt Viewport::drawingPlanePoint(const QPointF& cursor,bool& found) {
  found=false;
  gp_Pnt best=m_view->Camera()->Center(); double distance=1e100;
  if(m_sketchInput){double u,v;if(planePoint(cursor,m_sketchFrame,u,v)){const auto p=m_sketchFrame.to_world(u,v);found=true;return gp_Pnt(p[0],p[1],p[2]);}}
  for(const auto& [id,item]:m_items){
    const auto* node=m_doc->scene.node(id);if(!node || node->representation!="drawing2d" || !m_ctx->IsDisplayed(item.ais))continue;
    const auto cached=m_prs.find(item.key);if(cached==m_prs.end() || cached->second->box.IsVoid())continue;
    const auto box=cached->second->box;const auto lo=box.CornerMin(),hi=box.CornerMax();const auto transform=item.ais->Transformation();
    const auto origin=gp_Pnt(0,0,(lo.Z()+hi.Z())*.5).Transformed(transform);const auto x=gp_Dir(1,0,0).Transformed(transform),y=gp_Dir(0,1,0).Transformed(transform);
    opad::Frame frame;frame.origin={origin.X(),origin.Y(),origin.Z()};frame.x={x.X(),x.Y(),x.Z()};frame.y={y.X(),y.Y(),y.Z()};double u,v;
    if(!planePoint(cursor,frame,u,v))continue;const auto point=frame.to_world(u,v);const gp_Pnt world(point[0],point[1],point[2]);const auto local=world.Transformed(transform.Inverted());
    if(local.X()<lo.X() || local.X()>hi.X() || local.Y()<lo.Y() || local.Y()>hi.Y())continue;
    const double depth=gp_Vec(m_view->Camera()->Eye(),world).Dot(gp_Vec(m_view->Camera()->Direction()));if(depth<0 || depth>=distance)continue;
    distance=depth;best=world;found=true;
  }
  return best;
}

// The point of a drawing's or sketch's curves nearest `cursor` on screen (squared distance in widget pixels).
gp_Pnt Viewport::nearestCurvePoint(const QPointF& cursor,bool& found,double& distance) {
  found=false;distance=1e100;
  gp_Pnt best=m_view->Camera()->Center();
  auto segment=[&](gp_Pnt a,gp_Pnt b) {
    const QPointF pa=widgetPoint({a.X(),a.Y(),a.Z()}), pb=widgetPoint({b.X(),b.Y(),b.Z()}), d=pb-pa;
    const double len=QPointF::dotProduct(d,d);
    const double t=len>0?std::clamp(QPointF::dotProduct(cursor-pa,d)/len,0.0,1.0):0;
    const auto delta=pa+d*t-cursor; const double sq=QPointF::dotProduct(delta,delta);
    if (sq<distance) { found=true;distance=sq; best=a.Translated(gp_Vec(a,b)*t); }
  };
  for (const auto& [id,item]:m_items) {
    if(!m_ctx->IsDisplayed(item.ais)) continue;
    auto p=m_prs.find(item.key); if(p==m_prs.end()) continue;
    const auto& points=p->second->drawingSegments;
    for(size_t i=0;i+1<points.size();i+=2) segment(points[i].Transformed(item.ais->Transformation()),points[i+1].Transformed(item.ais->Transformation()));
  }
  for(const auto& [id,wire]:m_sketchWires) {
    if(!m_ctx->IsDisplayed(wire.ais) || !wire.prs) continue;
    const auto& points=wire.prs->drawingSegments;
    for(size_t i=0;i+1<points.size();i+=2) segment(points[i],points[i+1]);
  }
  return best;
}

gp_Pnt Viewport::drawingOrbitPoint(const QPointF* cursor,bool* found) {
  bool inside=false,onCurve=false;double distance=0;
  if(cursor){const gp_Pnt p=drawingPlanePoint(*cursor,inside);if(inside){if(found)*found=true;return p;}}
  const gp_Pnt p=nearestCurvePoint(cursor?*cursor:QPointF(width()/2.0,height()/2.0),onCurve,distance);
  if(found)*found=onCurve;
  return p;
}


opad::json Viewport::circleInfo(const opad::Ref& ref) const {
  std::shared_ptr<BodyPrs> prs; double scale=1;
  if(auto sk=m_sketchWires.find(ref.body);sk!=m_sketchWires.end()) prs=sk->second.prs;
  else { auto body=m_items.find(ref.body); if(body==m_items.end()) return {};
    auto found=m_prs.find(body->second.key); if(found==m_prs.end()) return {}; prs=found->second;
    scale=std::abs(body->second.ais->Transformation().ScaleFactor());
  }
  for(const auto& [id,c]:prs->circles) {
    if(id!=ref.index && std::find(c.meshEdges.begin(),c.meshEdges.end(),ref.index)==c.meshEdges.end()) continue;
    if(ref.kind!=opad::Ref::Kind::Center && ref.kind!=opad::Ref::Kind::Edge) return {};
    opad::json info={{"diameter",2*c.radius*scale}};
    if(c.segments) info["segments"]=c.segments;
    return info;
  }
  return {};
}
