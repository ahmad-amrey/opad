// Exact circle-center discovery and navigation picking independent of the selection filter.
#include "Viewport.hpp"
#include "NavCube.hpp"

#include <BRepBuilderAPI_MakeVertex.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <Graphic3d_Camera.hxx>
#include <TopoDS_Vertex.hxx>
#include <Prs3d_PointAspect.hxx>
#include <QApplication>
#include <QKeyEvent>
#include <QWindow>
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
  m_ctx->SetSelectionSensitivity(marker, 0, static_cast<int>(std::lround(12 * displayScale())));  // a 24 px target (UI-124)
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
  moveTo(devicePos(position));
  discoverCenter();redrawScene();
}

// UI-31. First one ray from the point towards the eye (nothing behind the point or behind the eye counts), then what is
// drawn over its pixel, as box selection tests it: a part thinner than a pixel, or a gap between facets that the exact
// ray slips through, still covers the point on screen. The pixel's pick tolerance reaches faces beside the point, far
// nearer when seen edge-on (a cylinder's silhouette): its margin is wider and the point's own body is left to the ray.
bool Viewport::pointVisible(const gp_Pnt& p, const std::string& own, double slackPx) const {
  if (!m_initialised || m_selectThrough || m_navSelector.IsNull()) return true;
  const auto camera = m_view->Camera();
  const bool ortho = camera->IsOrthographic();
  gp_Vec back = ortho ? gp_Vec(camera->Direction()).Reversed() : gp_Vec(p, camera->Eye());
  const double reach = ortho ? RealLast() : back.Magnitude();
  if (back.SquareMagnitude() < 1e-24) return true;
  back.Normalize();
  const gp_Vec ahead(camera->Direction());
  const double scale = ortho ? 1.0 : std::max(1e-6, gp_Vec(camera->Eye(), p).Dot(ahead) / camera->Distance());
  const double pixel = pixelSize() * scale, slack = slackPx * pixel;  // at the point's depth
  auto occludes = [&](int i, bool others) {
    const auto node = m_navNodes.find(m_navSelector->Picked(i)->Selectable().get());
    if (node == m_navNodes.end() || (others && node->second == own)) return false;
    const auto item = m_items.find(node->second);
    return item != m_items.end() && !item->second.look.ghost && m_ctx->IsDisplayed(item->second.ais);  // a ghost is seen through
  };
  const gp_Pnt from = p.Translated(back * slack);
  m_navSelector->Pick(gp_Ax1(from, gp_Dir(back)), m_view);
  for (int i = 1; i <= m_navSelector->NbPicked(); ++i)
    if (occludes(i, false) && m_navSelector->PickedPoint(i).Distance(from) < reach - slack) return false;
  Standard_Integer x = 0, y = 0;
  m_view->Convert(p.X(), p.Y(), p.Z(), x, y);
  m_navSelector->Pick(x, y, m_view);
  for (int i = 1; i <= m_navSelector->NbPicked(); ++i)
    if (occludes(i, true) && gp_Vec(m_navSelector->PickedPoint(i), p).Dot(ahead) > std::max(slack, 8 * pixel)) return false;
  return true;
}

bool Viewport::detectedPoint(gp_Pnt& p) const {
  if (!m_initialised || !m_ctx->HasDetected()) return false;
  const auto& selector = m_ctx->MainSelector();
  const auto owner = m_ctx->DetectedOwner();
  for (int i = 1; i <= selector->NbPicked(); ++i)
    if (selector->Picked(i) == owner) { p = selector->PickedPoint(i); return true; }
  return false;
}

// The pick tolerance reaches past what is drawn both ways: a vertex's through a thin wall, and a face's to the face of a
// silhouette edge seen edge-on, nearer than that edge or vertex though both are in sight. So the pointer takes the first
// owner in pick order that is not an occluder and whose point is in sight, else nothing.
bool Viewport::dropOccluded() {
  if (!m_initialised || !m_ctx->HasDetected() || m_hoverCycled) return false;
  const bool subShapes = m_filter == SelFilter::Edge || m_filter == SelFilter::Vertex;
  const auto& selector = m_ctx->MainSelector();
  auto hidden = [&](const Handle(SelectMgr_EntityOwner)& owner) {
    if (!Handle(OccluderOwner)::DownCast(owner).IsNull()) return true;
    const auto body = subShapes && !Handle(StdSelect_BRepOwner)::DownCast(owner).IsNull()
        ? m_nodeOf.find(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()).get()) : m_nodeOf.end();
    if (body == m_nodeOf.end()) return false;
    for (int i = 1; i <= selector->NbPicked(); ++i)
      if (selector->Picked(i) == owner) return !pointVisible(selector->PickedPoint(i), body->second);
    return false;
  };
  const auto first = m_ctx->DetectedOwner();
  if (!hidden(first)) return false;
  Handle(SelectMgr_EntityOwner) take;
  int rank = 0;
  for (m_ctx->InitDetected(); m_ctx->MoreDetected() && rank < 16 && take.IsNull(); m_ctx->NextDetected(), ++rank)
    if (const auto owner = m_ctx->DetectedCurrentOwner(); owner != first && !hidden(owner)) take = owner;
  for (int i = 0; !take.IsNull() && i < 64 && m_ctx->DetectedOwner() != take; ++i) m_ctx->HilightNextDetected(m_view, Standard_False);
  if (take.IsNull() || m_ctx->DetectedOwner() != take) m_ctx->ClearDetected(Standard_False);
  m_view->InvalidateImmediate();
  return !m_ctx->HasDetected();
}

void Viewport::moveTo(const Graphic3d_Vec2i& at) {
  m_ctx->MoveTo(at.x(), at.y(), m_view, Standard_False);
  dropOccluded();
}

void Viewport::contextLazyMoveTo(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view, const Graphic3d_Vec2i& point) {
  // The hover chosen there (select other, UI-128) stays what the pointer is on, also for the click (the controller picks
  // again for a click when its last pick point was reset by the press).
  if (m_hoverCycled && point == m_cycledAt) {
    myPrevMoveTo = point;
    return;
  }
  AIS_ViewController::contextLazyMoveTo(ctx, view, point);
  dropOccluded();
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
    const bool locked=(center && m_centerLocked) || (tracking && m_shift.locked());
    auto aspect=marker.ais->Attributes()->PointAspect();
    aspect->SetTypeOfMarker(selected ? Aspect_TOM_O_PLUS : tracking && m_trackingCross ? Aspect_TOM_X : Aspect_TOM_O);
    aspect->SetScale(selected ? 4.0 : locked ? 7.0 : candidate ? 5.0 : 3.0);
    marker.ais->SynchronizeAspects();
  }
}

bool Viewport::inferenceKey(QKeyEvent* key) {
  if (key->key()!=Qt::Key_Shift || key->isAutoRepeat() || m_sketchInput || m_blocked || !m_initialised) return false;
  if (!m_shiftClock.isValid()) m_shiftClock.start();
  using R=tracking::ShiftLock::Result;
  const int count=int(m_trackingCandidates.size());
  if (key->type()==QEvent::KeyPress) {
    if (QApplication::mouseButtons()!=Qt::NoButton) return false;
    const R r=m_shift.press(m_shiftClock.elapsed(),count);
    if (r==R::Ignored) return false;
    m_centerLocked=false;
    if (r==R::Locked) m_lockedTracking=m_trackingCandidates[m_inferenceChoice=std::clamp(m_inferenceChoice,0,count-1)];
  } else {
    const auto held=m_lockedTracking;
    const R r=m_shift.release(m_shiftClock.elapsed(),count,m_crossings);
    if (r==R::Ignored) return false;
    m_centerLocked=false;
    if (r==R::StuckPrevious) m_lockedTracking=m_tapLock;  // the guide shown before the double tap's first tap
    else if (r==R::NextCrossing) ++m_crossChoice;
    else if (r==R::Cycled || r==R::Unlocked) {
      m_tapLock=held; m_crossChoice=0;
      if (r==R::Cycled) m_inferenceChoice=(m_inferenceChoice+1)%count;
    }
  }
  m_trackingDirty=true; m_hoverOwner=nullptr;
  refreshCenterStyles();
  redrawScene(); return true;
}

bool Viewport::trackingEscape(QEvent* e) {
  if (e->type()!=QEvent::ShortcutOverride && e->type()!=QEvent::KeyPress) return false;
  auto* key=static_cast<QKeyEvent*>(e);
  if (key->key()!=Qt::Key_Escape || (key->modifiers() & ~Qt::KeypadModifier)) return false;
  if (e->type()==QEvent::KeyPress && std::exchange(m_eatEscape,false)) return true;  // the press after its override
  if (!m_shift.escape()) return false;
  m_eatEscape=e->type()==QEvent::ShortcutOverride; e->accept();
  m_trackingDirty=true; m_hoverOwner=nullptr;
  refreshCenterStyles(); redrawScene();
  return true;
}

bool Viewport::eventFilter(QObject* object, QEvent* e) {
  // The window's surface back after it was minimised (or covered while the screen was locked): the frame it lost is drawn
  // again whole; nothing in the scene changed, so the next frame alone would draw nothing and the view stayed black.
  if (e->type() == QEvent::Expose && object == window()->windowHandle()) {
    const bool exposed = window()->windowHandle()->isExposed();
    if (exposed && !std::exchange(m_topExposed, exposed)) exposedAgain();
    m_topExposed = exposed;
  }
  // A native overlay over the view moved, changed its size or went (ViewOverlay.hpp), or the system uncovered part of the
  // view's own window: that part is shown again, or it keeps the overlay's last image (trails of the value boxes).
  if ((e->type() == QEvent::Move || e->type() == QEvent::Resize || e->type() == QEvent::Hide) && object != this && object->isWidgetType()) {
    auto* overlay = static_cast<QWidget*>(object);
    if (overlay->testAttribute(Qt::WA_NativeWindow) && !overlay->isWindow() && isAncestorOf(overlay)) overlayUncovered();
  }
  if (e->type() == QEvent::Expose && windowHandle() && object == windowHandle()) overlayUncovered();
  if (zoomWindowKey(object, e)) return true;
  if(e->type()==QEvent::MouseButtonPress || e->type()==QEvent::MouseButtonDblClick) {
    const auto widget=qobject_cast<QWidget*>(object);
    if(widget && (widget==window() || window()->isAncestorOf(widget)))resetHoverFade();
  }
  if((e->type()==QEvent::KeyPress || e->type()==QEvent::KeyRelease) && static_cast<QKeyEvent*>(e)->key()==Qt::Key_Control
      && (object==this || underMouse() || m_ctrlCenterPick))
    setCenterPicking(e->type()==QEvent::KeyPress,m_trackingCursor);
  if ((e->type()==QEvent::KeyPress || e->type()==QEvent::KeyRelease)
      && (object==this || underMouse() || m_shift.held())
      && (window()->isActiveWindow() || m_shift.held()
          || (QApplication::activeWindow() && window()->isAncestorOf(QApplication::activeWindow()))))
    if(inferenceKey(static_cast<QKeyEvent*>(e))) return true;
  // Esc on a tracking lock beats the window's Esc (the guided tool's step back): at the override stage, its press eaten.
  if (((e->type()==QEvent::ShortcutOverride && (object==this || underMouse())) || (e->type()==QEvent::KeyPress && m_eatEscape)) && trackingEscape(e))
    return true;
  // A popup menu takes the mouse: the system pointer is back over the view while it is open (looked at once it is in).
  if ((e->type()==QEvent::Show || e->type()==QEvent::Hide) && m_ownCursorWanted)
    if (const auto* popup=qobject_cast<QWidget*>(object); popup && popup->windowType()==Qt::Popup)
      QTimer::singleShot(0,this,[this]{applyOwnCursor();});
  // Onto an overlay on the view (the prompt, the chips, a value box): Qt sends the view no Leave (it is still under the
  // pointer, as the overlay's parent), but the sketch is not under the pointer any more, nor is the drawing cursor.
  if (e->type()==QEvent::Enter && m_sketchInput && object!=this)
    if (const auto* widget=qobject_cast<QWidget*>(object); widget && !widget->isWindow() && isAncestorOf(widget)) m_sketchInput->sketchLeave();
  if (e->type()==QEvent::ApplicationDeactivate) {
    setCenterPicking(false,m_trackingCursor);
    m_centerLocked=false; m_shift.deactivate(); refreshCenterStyles();
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

// The point of a drawing's or sketch's curves nearest `cursor` on screen (squared distance in widget pixels). Run by run
// (BodyPrs::segmentRuns), nearest box on screen first, until a box is farther than the best point found: a press over a
// big drawing projected its every segment (UI-51).
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
  struct Run { double bound; const BodyPrs* prs; gp_Trsf trsf; size_t first, count; bool ordered; };
  std::vector<Run> runs;
  const auto camera=m_view->Camera();
  // The squared distance from the cursor to a box's outline on screen: 0 inside it, or when a corner is behind the eye.
  auto bound=[&](const Bnd_Box& box,const gp_Trsf& trsf) {
    if(box.IsVoid()) return 0.0;
    const gp_Pnt lo=box.CornerMin(),hi=box.CornerMax();
    double x0=1e300,y0=1e300,x1=-1e300,y1=-1e300;
    for(int c=0;c<8;++c) {
      const gp_Pnt p=gp_Pnt(c&1?hi.X():lo.X(),c&2?hi.Y():lo.Y(),c&4?hi.Z():lo.Z()).Transformed(trsf);
      if(!camera->IsOrthographic() && gp_Vec(camera->Eye(),p).Dot(gp_Vec(camera->Direction()))<=0) return 0.0;
      const QPointF s=widgetPoint({p.X(),p.Y(),p.Z()});
      x0=std::min(x0,s.x());y0=std::min(y0,s.y());x1=std::max(x1,s.x());y1=std::max(y1,s.y());
    }
    const double dx=cursor.x()<x0?x0-cursor.x():cursor.x()>x1?cursor.x()-x1:0, dy=cursor.y()<y0?y0-cursor.y():cursor.y()>y1?cursor.y()-y1:0;
    return dx*dx+dy*dy;
  };
  auto add=[&](const BodyPrs* prs,const gp_Trsf& trsf) {
    if(prs->drawingSegments.size()<2) return;
    if(prs->segmentRuns.empty()) { runs.push_back({0.0,prs,trsf,0,prs->drawingSegments.size()/2,false}); return; }
    for(const auto& run:prs->segmentRuns) runs.push_back({bound(run.box,trsf),prs,trsf,run.first,run.count,true});
  };
  for (const auto& [id,item]:m_items) {
    if(!m_ctx->IsDisplayed(item.ais)) continue;
    auto p=m_prs.find(item.key); if(p==m_prs.end()) continue;
    add(p->second.get(),item.ais->Transformation());
  }
  for(const auto& [id,wire]:m_sketchWires)
    if(m_ctx->IsDisplayed(wire.ais) && wire.prs) add(wire.prs.get(),gp_Trsf());
  std::sort(runs.begin(),runs.end(),[](const Run& a,const Run& b){return a.bound<b.bound;});
  for(const auto& run:runs) {
    if(run.bound>=distance) break;
    const auto& points=run.prs->drawingSegments;
    for(size_t k=run.first;k<run.first+run.count;++k) {
      const size_t i=run.ordered?run.prs->segmentOrder[k]:k;
      segment(points[2*i].Transformed(run.trsf),points[2*i+1].Transformed(run.trsf));
    }
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
