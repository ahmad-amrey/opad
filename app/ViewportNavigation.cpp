// Navigation staples (UI-47): the zoom window, previous and next view, a Home of the user's own, the view twist and the
// animated camera moves the commands use. The CAD 2D preset is in setNavPreset, the middle double click in
// mouseDoubleClickEvent, the cube's menu in the mouse handlers (Viewport.cpp).
#include <QGuiApplication>
#include <QKeyEvent>
#include <QSettings>
#include <QWindow>

#include <AIS_AnimationCamera.hxx>
#include <AIS_RubberBand.hxx>

#include <algorithm>

#include "Viewport.hpp"

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }
QString triple(const gp_XYZ& v) { return QString("%1,%2,%3").arg(v.X(), 0, 'g', 17).arg(v.Y(), 0, 'g', 17).arg(v.Z(), 0, 'g', 17); }
}  // namespace

// ---------------------------------------------------------------- animated moves
bool Viewport::animationsShown() const {
  return m_initialised && m_animateViews && (m_forceAnimate || (windowHandle() && windowHandle()->isExposed()));
}

void Viewport::setAnimateViews(bool on) {
  m_animateViews = on;
  QSettings().setValue("view/animate", on);
}

void Viewport::finishAnimation() {
  if (myViewAnimation->IsStopped()) return;
  const Handle(Graphic3d_Camera) end = myViewAnimation->CameraEnd();
  myViewAnimation->Stop();
  if (!end.IsNull()) m_view->Camera()->Copy(end);
}

void Viewport::animateCamera(const Handle(Graphic3d_Camera)& end, double seconds) {
  myViewAnimation->Stop();
  m_needFit = false;
  if (!animationsShown()) {
    m_view->Camera()->Copy(end);
    m_view->Invalidate();
    requestRedraw();
    return;
  }
  myViewAnimation->SetView(m_view);
  myViewAnimation->SetCameraStart(new Graphic3d_Camera(*m_view->Camera()));
  myViewAnimation->SetCameraEnd(end);
  myViewAnimation->SetOwnDuration(seconds);
  myViewAnimation->StartTimer(0.0, 1.0, Standard_True);  // the 16 ms timer redraws while it runs
  requestRedraw();
}

void Viewport::moveCamera(bool animate, double seconds, const std::function<void()>& move) {
  myViewAnimation->Stop();
  if (!animate || !animationsShown()) {
    move();
    m_view->Invalidate();
    requestRedraw();
    return;
  }
  const Handle(Graphic3d_Camera) start = new Graphic3d_Camera(*m_view->Camera());
  move();
  const Handle(Graphic3d_Camera) end = new Graphic3d_Camera(*m_view->Camera());
  m_view->Camera()->Copy(start);
  animateCamera(end, seconds);
}

// ---------------------------------------------------------------- zoom window
void Viewport::startZoomWindow() {
  if (!m_initialised || m_blocked) return;
  m_zoomWindow = true;
  m_zoomDrag = false;
  setCursor(Qt::CrossCursor);
  emit hoverChanged(tr("Zoom window: drag around what to see, or click to zoom in there · Esc cancels"));
  emit zoomWindowChanged(true);
}

void Viewport::cancelZoomWindow() {
  if (!m_zoomWindow) return;
  m_zoomWindow = m_zoomDrag = false;
  if (m_ctx->IsDisplayed(myRubberBand)) {
    m_ctx->Remove(myRubberBand, Standard_False);
    redrawScene();
  }
  myRubberBand->ClearPoints();
  unsetCursor();
  emit hoverChanged(QString());
  emit zoomWindowChanged(false);
}

// The rectangle dragged so far: dashed, faintly filled, in the tool colour (the selection box keeps its own blue/green).
void Viewport::showZoomBand() {
  const Graphic3d_Vec2i a = devicePos(m_zoomFrom), b = devicePos(m_zoomTo);
  myRubberBand->SetLineColor(occ(m_tokens.hov));
  myRubberBand->SetLineType(Aspect_TOL_DASH);
  myRubberBand->SetFilling(occ(m_tokens.hov), 0.9);
  myRubberBand->SetRectangle(std::min(a.x(), b.x()), -std::max(a.y(), b.y()), std::max(a.x(), b.x()), -std::min(a.y(), b.y()));
  if (!m_ctx->IsDisplayed(myRubberBand)) m_ctx->Display(myRubberBand, 0, -1, Standard_False, AIS_DS_Displayed);
  else m_ctx->Redisplay(myRubberBand, Standard_False);
  redrawScene();
}

void Viewport::finishZoomWindow() {
  const Graphic3d_Vec2i a = devicePos(m_zoomFrom), b = devicePos(m_zoomTo);
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  const auto r = viewnav::zoomRect(a.x(), a.y(), b.x(), b.y(), w, h, qRound(4 * viewScale().x()));
  cancelZoomWindow();
  m_needFit = false;
  moveCamera(true, 0.35, [this, r] { m_view->WindowFit(r[0], r[1], r[2], r[3]); });
}

// Esc leaves the zoom window at the shortcut stage, wherever the focus is in the window, so the tool's or window's Esc
// does not run too.
bool Viewport::zoomWindowKey(QObject* object, QEvent* e) {
  if (!m_zoomWindow || (e->type() != QEvent::ShortcutOverride && e->type() != QEvent::KeyPress)) return false;
  if (static_cast<QKeyEvent*>(e)->key() != Qt::Key_Escape) return false;
  const auto* widget = qobject_cast<QWidget*>(object);
  if (!widget || (widget != window() && !window()->isAncestorOf(widget))) return false;
  cancelZoomWindow();
  e->accept();
  return true;
}

// ---------------------------------------------------------------- previous / next view
ViewState Viewport::viewState() const {
  const Handle(Graphic3d_Camera)& c = m_view->Camera();
  return {{c->Eye().X(), c->Eye().Y(), c->Eye().Z()}, {c->Center().X(), c->Center().Y(), c->Center().Z()}, {c->Up().X(), c->Up().Y(), c->Up().Z()}, c->Scale()};
}

void Viewport::settleView() {
  if (!m_initialised) return;
  if (!myViewAnimation->IsStopped() || QGuiApplication::mouseButtons() != Qt::NoButton) {  // still moving
    m_settleTimer.start();
    return;
  }
  m_history.record(viewState());
}

void Viewport::goTo(const ViewState& s) {
  const Handle(Graphic3d_Camera) end = new Graphic3d_Camera(*m_view->Camera());
  end->SetEyeAndCenter(gp_Pnt(s.eye[0], s.eye[1], s.eye[2]), gp_Pnt(s.target[0], s.target[1], s.target[2]));
  end->SetUp(gp_Dir(s.up[0], s.up[1], s.up[2]));
  if (end->IsOrthographic()) end->SetScale(s.scale);  // a perspective camera's scale is its distance, set by the eye
  animateCamera(end, 0.35);
}

bool Viewport::previousView() {
  if (!m_initialised) return false;
  finishAnimation();
  m_history.record(viewState());  // here, also when it has not rested long enough to be recorded
  const ViewState* s = m_history.back();
  if (!s) {
    emit hoverChanged(tr("No earlier view"));
    return false;
  }
  goTo(*s);
  return true;
}

bool Viewport::nextView() {
  if (!m_initialised) return false;
  finishAnimation();
  m_history.record(viewState());
  const ViewState* s = m_history.forward();
  if (!s) {
    emit hoverChanged(tr("No later view"));
    return false;
  }
  goTo(*s);
  return true;
}

// ---------------------------------------------------------------- Home
void Viewport::setHomeView() {
  if (!m_initialised) return;
  const Handle(Graphic3d_Camera)& c = m_view->Camera();
  QSettings settings;
  settings.setValue("view/homeEye", triple(c->Direction().Reversed().XYZ()));
  settings.setValue("view/homeUp", triple(c->Up().XYZ()));
  emit hoverChanged(tr("Home looks this way now"));
}

void Viewport::resetHomeView() {
  QSettings settings;
  settings.remove("view/homeEye");
  settings.remove("view/homeUp");
  emit hoverChanged(tr("Home is the iso view again"));
}

bool Viewport::customHome() const { return QSettings().contains("view/homeEye"); }

// ---------------------------------------------------------------- twist
gp_Dir Viewport::naturalUp() const {
  const gp_Dir direction = m_view->Camera()->Direction();
  if (m_sketchInput) {  // a sketch reads with its own y up
    gp_Vec y(m_sketchFrame.y[0], m_sketchFrame.y[1], m_sketchFrame.y[2]);
    y -= gp_Vec(direction) * y.Dot(gp_Vec(direction));
    if (y.Magnitude() > 1e-9) return gp_Dir(y);
  }
  return viewnav::naturalUp(direction);
}

double Viewport::twistAngle() const {
  if (!m_initialised) return 0;
  return viewnav::twist(m_view->Camera()->Direction(), m_view->Camera()->Up(), naturalUp());
}

void Viewport::twistView(double degrees) {
  if (!m_initialised) return;
  finishAnimation();
  m_needFit = false;
  const Handle(Graphic3d_Camera) end = new Graphic3d_Camera(*m_view->Camera());
  end->SetUp(viewnav::twisted(end->Direction(), naturalUp(), degrees));
  animateCamera(end, 0.25);
}
