#include <QSettings>
#include <SelectMgr_Filter.hxx>
#include <TopExp_Explorer.hxx>
#include <Prs3d_PointAspect.hxx>
#include "Viewport.hpp"
#include "Drawing2D.hpp"
#include "I18n.hpp"
#include "Motion.hpp"
#include "Highlight.hpp"
#include "Units.hpp"
#include "opad/mesh.hpp"
#include <V3d.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_AmbientLight.hxx>
#include "DepthBias.hpp"
#include "CurveSamples.hpp"
#include "CursorWrap.hpp"
#include "SketchSnap.hpp"
#include <QScreen>
#include <QApplication>
#include <QMenu>

#include <functional>

#include <QElapsedTimer>
#include <QScopedValueRollback>
#include <QStyleHints>
#include <QThread>
#include <QTimer>
#include <QWindow>

#include <Aspect_Grid.hxx>
#include <AIS_AnimationCamera.hxx>
#include <AIS_TexturedShape.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_Group.hxx>
#include <PrsMgr_Presentation.hxx>
#include <QPainter>
#include <QPointer>
#include <cstring>
#include <AIS_ViewCube.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_ScrollDelta.hxx>
#include <Aspect_VKeyFlags.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box2d.hxx>
#include <gp_Pnt2d.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Polygon3D.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangulation.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_Structure.hxx>
#include <Graphic3d_StructureManager.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Quantity_Color.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TopExp.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <V3d_ImageDumpOptions.hxx>
#include <gp_Pln.hxx>
#if defined(_WIN32)
#include <WNT_Window.hxx>
#elif defined(__APPLE__)
Handle(Aspect_Window) opad_make_cocoa_window(void* nsview);
#else
#include <Xw_Window.hxx>
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QToolTip>
#include <QInputDevice>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QWheelEvent>
#include <cmath>
#include <thread>

#include "opad/canvas.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "Jobs.hpp"
#include "NavCube.hpp"
#include "SketchBackdrop.hpp"
#include <TColStd_ListOfInteger.hxx>
#include <Prs3d_DatumAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Prs3d_TextAspect.hxx>

namespace {
class OwnerFilter : public SelectMgr_Filter {
 public:
  explicit OwnerFilter(std::function<bool(const Handle(SelectMgr_EntityOwner)&)> accepts):m_accepts(std::move(accepts)) {}
  Standard_Boolean IsOk(const Handle(SelectMgr_EntityOwner)& owner) const override {return m_accepts(owner);}
 private:
  std::function<bool(const Handle(SelectMgr_EntityOwner)&)> m_accepts;
};
constexpr int kCubeOffsetX = 100, kCubeOffsetY = 104;  // view cube centre from the top-right corner, in Qt points
}  // namespace

namespace {

Aspect_VKeyMouse qt_buttons(Qt::MouseButtons b) {
  unsigned int r = Aspect_VKeyMouse_NONE;
  if (b & Qt::LeftButton) r |= Aspect_VKeyMouse_LeftButton;
  if (b & Qt::MiddleButton) r |= Aspect_VKeyMouse_MiddleButton;
  if (b & Qt::RightButton) r |= Aspect_VKeyMouse_RightButton;
  return static_cast<Aspect_VKeyMouse>(r);
}

Aspect_VKeyFlags qt_flags(Qt::KeyboardModifiers m) {
  unsigned int r = Aspect_VKeyFlags_NONE;
  if (m & Qt::ShiftModifier) r |= Aspect_VKeyFlags_SHIFT;
  if (m & Qt::ControlModifier) r |= Aspect_VKeyFlags_CTRL;
  if (m & Qt::AltModifier) r |= Aspect_VKeyFlags_ALT;
  return static_cast<Aspect_VKeyFlags>(r);
}

Quantity_Color qcolor(const std::array<double, 3>& c) {
  return Quantity_Color(std::clamp(c[0], 0.0, 1.0), std::clamp(c[1], 0.0, 1.0), std::clamp(c[2], 0.0, 1.0), Quantity_TOC_sRGB);
}

Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }

}  // namespace

Viewport::Viewport(AppDocument* doc, QWidget* parent)
    : QWidget(parent), m_doc(doc), m_tokens(theme::current()), m_alive(std::make_shared<std::atomic<bool>>(true)) {
  setAttribute(Qt::WA_PaintOnScreen);
  setAttribute(Qt::WA_NoSystemBackground);
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  QCoreApplication::instance()->installEventFilter(this);
  setMinimumSize(200, 150);
  connect(doc, &AppDocument::changed, this, &Viewport::resetHoverFade);
  connect(doc, &AppDocument::changed, this, &Viewport::sync);
  const QSettings settings;
  m_gridSpacing = std::max(0.0, settings.value("view/gridSpacing", 0.0).toDouble());
  m_gridExtentSetting = std::max(1.0, settings.value("view/gridExtent", 100.0).toDouble());
  connect(units::notifier(), &units::Notifier::changed, this, [this] { refreshMeasurement(true); });  // labels in the shown unit
  m_syncTimer.setSingleShot(true);
  m_syncTimer.setInterval(50);
  connect(&m_syncTimer, &QTimer::timeout, this, &Viewport::sync);
  m_settleTimer.setSingleShot(true);
  m_settleTimer.setInterval(500);
  connect(&m_settleTimer, &QTimer::timeout, this, &Viewport::settleView);
  connect(doc, &AppDocument::aboutToReplace, this, [this] { m_history.clear(); });  // its views were of that document
  m_animateViews = settings.value("view/animate", true).toBool();
  m_refineTimer.setSingleShot(true);
  m_refineTimer.setInterval(350);
  connect(&m_refineTimer, &QTimer::timeout, this, &Viewport::refineVisible);
  m_adaptive = settings.value("view/adaptive", true).toBool();
  m_qualityTimer.setSingleShot(true);
  m_qualityTimer.setInterval(350);
  connect(&m_qualityTimer, &QTimer::timeout, this, &Viewport::restoreQuality);
  m_edgeTimer.setSingleShot(true);
  m_edgeTimer.setInterval(100);
  connect(&m_edgeTimer, &QTimer::timeout, this, &Viewport::buildEdgeOverlay);
  m_timer.setInterval(16);
  connect(&m_timer, &QTimer::timeout, this, [this] {
    if (!m_initialised) return;
    if (m_glide && !myViewAnimation.IsNull() && !myViewAnimation->IsStopped()) {  // on time also where no frame is drawn
      myViewAnimation->UpdateTimer();
      m_view->Invalidate();
    }
    m_glide = m_glide && !myViewAnimation.IsNull() && !myViewAnimation->IsStopped();
    if (!myViewAnimation.IsNull() && !myViewAnimation->IsStopped()) requestRedraw();
    else if (toAskNextFrame()) requestRedraw();
  });
  m_timer.start();
  m_trackpadEndTimer.setSingleShot(true);
  m_trackpadEndTimer.setInterval(180);  // platforms without ScrollEnd still need to release the virtual drag
  connect(&m_trackpadEndTimer, &QTimer::timeout, this, &Viewport::finishTrackpadScroll);
  m_dwellTimer.setSingleShot(true);  // the pointer rests: no event would run the tracker again
  connect(&m_dwellTimer, &QTimer::timeout, this, [this] { m_trackingDirty = true; requestRedraw(); });
  m_holdTimer.setSingleShot(true);
  connect(&m_holdTimer, &QTimer::timeout, this, &Viewport::pressHeld);
  m_selectOtherTimer.setSingleShot(true);
  connect(&m_selectOtherTimer, &QTimer::timeout, this, [this] {
    if (QMenu* menu = selectOtherMenu(m_selectOtherAt)) menu->popup(m_selectOtherGlobal);
  });
#if !defined(__APPLE__)
  grabGesture(Qt::PinchGesture);  // fallback for touch devices without native pinch events
#endif
}

Viewport::~Viewport() {
  if(m_bodyGlowJob) m_bodyGlowJob->cancel(); if(m_lookJob) m_lookJob->cancel(); *m_alive = false;
  if (m_side) { m_sideView->Remove(); delete sideWidget(); }  // the view goes before its window
}

void Viewport::setBlocked(bool on) {
  if (on) {
    finishTrackpadScroll();
    m_nativePinching = false;
  }
  m_blocked = on;
  applyOwnCursor();
}

void Viewport::benchShot(const QString& path) {
  if (!m_initialised) return;
  if (const QByteArray sel = qgetenv("OPAD_BENCH_SELECT"); !sel.isEmpty()) {  // select bodies by node name, synchronously
    m_ctx->ClearSelected(Standard_False);
    for (const auto& [id, it] : m_items)
      if (m_doc->nodeName(id) == QString::fromUtf8(sel)) m_ctx->AddOrRemoveSelected(it.ais, Standard_False);
  }
  applySelectionLayers();
  if (trace::enabled()) trace::log(QStringLiteral("bench: shot with %1 selected, layer of selected style %2").arg(m_ctx->NbSelected()).arg(m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->ZLayer()));
  if (const QByteArray view = qgetenv("OPAD_BENCH_VIEW"); !view.isEmpty()) {  // an unanimated camera for the frame dump
    m_view->SetProj(view == "bottom" ? V3d_Zneg : view == "top" ? V3d_Zpos : V3d_XposYnegZpos);
    m_view->FitAll(fitBounds(), 0.02, Standard_False);
    m_view->Redraw();
    emit notesMoved();  // the note cards follow the camera through a queued signal; the dump wants them placed now
  }
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  // Hover point relative to the cube centre (default: the TOP face); OPAD_BENCH_HOVER="dx,dy" overrides it.
  int dx = 0, dy = -36;
  const QByteArray hoverEnv = qgetenv("OPAD_BENCH_HOVER");
  if (hoverEnv.contains(',')) { dx = hoverEnv.split(',')[0].toInt(); dy = hoverEnv.split(',')[1].toInt(); }
  const int cubeX = w - qRound(kCubeOffsetX * m_cubeScale) + qRound(dx * m_cubeScale);
  const int cubeY = qRound((kCubeOffsetY + dy) * m_cubeScale);
  m_ctx->MoveTo(cubeX, cubeY, m_view, Standard_False);
  TColStd_ListOfInteger cubeModes;
  m_ctx->ActivatedModes(m_cube, cubeModes);
  trace::log(QStringLiteral("bench: cube hover at (%1,%2): detected=%3 isCube=%4 cubeModes=%5 cubeHasSel0=%6").arg(cubeX).arg(cubeY).arg(m_ctx->HasDetected()).arg(m_ctx->HasDetected() && m_ctx->DetectedInteractive() == m_cube).arg(cubeModes.Size()).arg(m_cube->HasSelection(0)));
  m_view->Redraw();
  m_view->RedrawImmediate();
  grabImage().save(path);
}

QString Viewport::benchCubePart(int dx, int dy) {
  if (!m_initialised) return {};
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  m_view->Redraw();  // the picker's camera range
  m_ctx->MoveTo(w - qRound(kCubeOffsetX * m_cubeScale) + qRound(dx * m_cubeScale), qRound((kCubeOffsetY + dy) * m_cubeScale), m_view, Standard_False);
  const auto* owner = m_ctx->HasDetected() ? dynamic_cast<const AIS_ViewCubeOwner*>(m_ctx->DetectedOwner().get()) : nullptr;
  const QString part = !owner ? QString() : AIS_ViewCube::IsBoxCorner(owner->MainOrientation()) ? "corner" : AIS_ViewCube::IsBoxEdge(owner->MainOrientation()) ? "edge" : "side";
  m_ctx->ClearDetected(Standard_False);
  return part;
}

void Viewport::setCubeEdgesCorners(bool on) {
  const Handle(NavCube) cube = Handle(NavCube)::DownCast(m_cube);
  if (cube.IsNull() || cube->edgesAndCorners() == on) return;
  cube->setEdgesAndCorners(on);
  m_ctx->ClearDetected(Standard_False);
  m_ctx->RecomputeSelectionOnly(m_cube);
  redrawScene();
}

// The displayed body with the most faces: where sub-shape picking is at its most expensive.
std::string Viewport::benchHeaviest() const {
  std::string best;
  int most = -1;
  for (const auto& [id, it] : m_items) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(it.located, TopAbs_FACE, faces);
    if (faces.Extent() > most) { most = faces.Extent(); best = id; }
  }
  if (trace::enabled()) trace::log(QStringLiteral("bench: heaviest body %1 (%2 faces)").arg(m_doc->nodeName(best)).arg(most));
  return best;
}

// A rubber band over the whole view in the current mode: the mass sub-shape selection case.
void Viewport::benchBand() {
  if (!m_initialised) return;
  m_view->FitAll(fitBounds(), 0.02, Standard_False);
  m_view->Redraw();
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  QElapsedTimer clock;
  clock.start();
  m_ctx->SelectRectangle(Graphic3d_Vec2i(0, 0), Graphic3d_Vec2i(w, h), m_view);
  const qint64 pickMs = clock.restart();
  OnSelectionChanged(m_ctx, m_view);
  const qint64 notifyMs = clock.restart();
  trace::log(QStringLiteral("bench: band selected %1 (OCCT %2 ms, handlers %3 ms)").arg(m_ctx->NbSelected()).arg(pickMs).arg(notifyMs));
}

// The picked sub-shape seen from the opposite side, where the rest of the model is in front of it.
void Viewport::benchSubShot(const QString& path) {
  if (!m_initialised) return;
  Standard_Real x = 0, y = 0, z = 0;
  m_view->Proj(x, y, z);
  m_view->SetProj(-x, -y, -z);
  m_view->FitAll(fitBounds(), 0.02, Standard_False);
  m_view->Redraw();
  grabImage().save(path);
}

void Viewport::initViewer() {
  if (m_initialised) return;
  trace::Scope scope("Viewport::initViewer");
  Handle(Aspect_DisplayConnection) disp = new Aspect_DisplayConnection();
  Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(disp, Standard_False);
  driver->ChangeOptions().buffersNoSwap = Standard_False;
  driver->ChangeOptions().ffpEnable = Standard_False;
  m_viewer = new V3d_Viewer(driver);
  // The overhead light first (UI-45): it alone casts shadows, and OCCT's shaders draw nothing in Studio when a light that
  // casts none comes before the one that does. Then the defaults (a headlight, the ambient light).
  Handle(V3d_DirectionalLight) overhead=new V3d_DirectionalLight(gp_Dir(0,0,-1),Quantity_NOC_WHITE,false);
  overhead->SetIntensity(0.75f);
  Handle(V3d_DirectionalLight) headlight=new V3d_DirectionalLight(V3d_Zneg,Quantity_NOC_WHITE,true);
  headlight->SetName("headlight");
  Handle(V3d_AmbientLight) ambient=new V3d_AmbientLight(Quantity_NOC_WHITE);
  for(const Handle(V3d_Light)& light:{Handle(V3d_Light)(overhead),Handle(V3d_Light)(headlight),Handle(V3d_Light)(ambient)}) {m_viewer->AddLight(light);m_viewer->SetLightOn(light);}
  m_ctx = new AIS_InteractiveContext(m_viewer);
  // Drawings are highlighted as lines, not tinted: shared drawers whose colours follow the background (updateDrawingHighlights).
  m_drawingSelected = new Prs3d_Drawer();
  m_drawingSelected->SetLink(m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected));
  m_drawingSelected->SetDisplayMode(AIS_WireFrame);
  m_drawingHover = new Prs3d_Drawer();
  m_drawingHover->SetLink(m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic));
  m_drawingHover->SetDisplayMode(AIS_WireFrame);
  m_viewer->SetGridEcho(Standard_False);  // OCCT's star on the grid node nearest the pointer: no click takes that node
  {  // TopOSD (notes, the drawing being made, measurement labels) has no depth test, but it kept the depth, so what is
     // translucent in Topmost (a note target's tint) was drawn after it, over it: red strokes came out pink. Clearing
     // the depth draws what is pending first.
    Graphic3d_ZLayerSettings osd = m_viewer->ZLayerSettings(Graphic3d_ZLayerId_TopOSD);
    osd.SetClearDepth(Standard_True);
    m_viewer->SetZLayerSettings(Graphic3d_ZLayerId_TopOSD, osd);
  }
  {  // A canvas shown through the model: drawn after it with no depth test and writing no depth, so the model never hides
     // it and it hides nothing drawn later (hover in Top, a selected body's X-ray in Topmost, which owns its depth buffer
     // and where the canvas itself went before and covered selected bodies behind it).
    Graphic3d_ZLayerSettings through;
    through.SetName("canvas through");
    through.SetEnableDepthTest(Standard_False);
    through.SetEnableDepthWrite(Standard_False);
    through.SetClearDepth(Standard_False);
    if (!m_viewer->InsertLayerBefore(m_throughLayer, through, Graphic3d_ZLayerId_Top)) m_throughLayer = Graphic3d_ZLayerId_Topmost;
  }
  m_hoverFadeEnabled=QSettings().value("view/hoverFade",true).toBool();
  m_hoverFadeSeconds=std::clamp(QSettings().value("view/hoverFadeSeconds",5.0).toDouble(),.1,60.0);
  m_hoverFadeTimer.setSingleShot(true);connect(&m_hoverFadeTimer,&QTimer::timeout,this,&Viewport::requestRedraw);
  m_ctx->SetPixelTolerance(4);
  m_ctx->AddFilter(new OwnerFilter([this](const Handle(SelectMgr_EntityOwner)& owner) {
    if(!Handle(CircleOwner)::DownCast(owner).IsNull()) return m_ctrlCenterPick;
    if(!Handle(OccluderOwner)::DownCast(owner).IsNull()) {  // selecting through objects, or a ghost's faces: what is behind is reached
      if(m_selectThrough) return false;
      const auto node=m_nodeOf.find(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()).get());
      const auto item=node==m_nodeOf.end()?m_items.end():m_items.find(node->second);
      return item==m_items.end() || !item->second.look.ghost;
    }
    const auto center=m_centerObjects.find(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()).get());
    return center==m_centerObjects.end() || m_centers.at(center->second).ref.kind!=opad::Ref::Kind::Center || m_ctrlCenterPick;
  }));
  m_ctx->SetAutoActivateSelection(Standard_False);  // displayBody activates the current filter itself
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetDisplayMode(AIS_Shaded);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetDisplayMode(AIS_Shaded);
  // A face/edge highlight is its own presentation, built with the highlight drawer rather than the body's.
  // Same rule as for bodies (displayBody): OCCT must never be able to mesh on the UI thread, so these drawers
  // get auto-triangulation off too and highlight the worker's mesh. Precaution: no stall was measured from it.
  m_ctx->DefaultDrawer()->SetAutoTriangulation(Standard_False);
  for (const Prs3d_TypeOfHighlight h : {Prs3d_TypeOfHighlight_Selected, Prs3d_TypeOfHighlight_Dynamic, Prs3d_TypeOfHighlight_LocalSelected, Prs3d_TypeOfHighlight_LocalDynamic})
    m_ctx->HighlightStyle(h)->SetAutoTriangulation(Standard_False);

  m_view = m_viewer->CreateView();
#if defined(_WIN32)
  Handle(WNT_Window) win = new WNT_Window(reinterpret_cast<Aspect_Handle>(winId()));
#elif defined(__APPLE__)
  Handle(Aspect_Window) win = opad_make_cocoa_window(reinterpret_cast<void*>(winId()));
#else
  Handle(Xw_Window) win = new Xw_Window(disp, static_cast<Aspect_Drawable>(winId()));
#endif
  m_view->SetWindow(win);
  if (!win->IsMapped()) win->Map();
  m_view->ChangeRenderingParams().NbMsaaSamples = 4;
  m_view->ChangeRenderingParams().RenderResolutionScale = 1.0f;
  m_view->SetImmediateUpdate(Standard_False);
  m_view->Camera()->SetProjectionType(QSettings().value("view/orthographic",true).toBool()?Graphic3d_Camera::Projection_Orthographic:Graphic3d_Camera::Projection_Perspective);
  m_view->SetProj(V3d_XposYnegZpos);

  m_navSelector = new SelectMgr_ViewerSelector();
  m_navSelector->SetPixelTolerance(1);
  m_navSelector->SetPickClosest(true);
  m_navSelector->SetDepthTolerance(SelectMgr_TypeOfDepthTolerance_Uniform, 0.0);
  m_navSelection = new SelectMgr_SelectionManager(m_navSelector);
  Handle(NavCube) cube = new NavCube();  // a plain cube whose edges and corners are still hover/click targets
  cube->setEdgesAndCorners(QSettings().value("view/cubeEdgesCorners", true).toBool());
  m_cube = cube;
  m_cubeScale = viewScale().x();
  m_cube->SetSize(58 * m_cubeScale);
  m_cube->SetFontHeight(11 * m_cubeScale);
  m_cube->SetAxesLabels("X", "Y", "Z");
  m_cube->SetBoxSideLabel(V3d_Zpos, "TOP");
  m_cube->SetBoxSideLabel(V3d_Zneg, "BOTTOM");
  m_cube->SetBoxSideLabel(V3d_Yneg, "FRONT");
  m_cube->SetBoxSideLabel(V3d_Ypos, "BACK");
  m_cube->SetBoxSideLabel(V3d_Xpos, "RIGHT");
  m_cube->SetBoxSideLabel(V3d_Xneg, "LEFT");
  m_cube->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
      Graphic3d_Vec2i(qRound(kCubeOffsetX * m_cubeScale), qRound(kCubeOffsetY * m_cubeScale))));
  SetViewAnimation(new OrbitCameraAnimation(m_view));
  myViewAnimation->SetOwnDuration(motion::seconds(0.5));
  m_cube->SetViewAnimation(myViewAnimation);
  m_cube->SetFixedAnimationLoop(Standard_False);
  m_cube->SetAutoStartAnimation(Standard_True);
  m_ctx->Display(m_cube, Standard_False);
  m_ctx->Load(m_cube, -1);  // register with the selection manager: Display() with auto-activation off does not
  m_ctx->Activate(m_cube, 0);

  SetRotationMode(AIS_RotationMode_BndBoxActive);
  SetLockOrbitZUp(Standard_True);  // keep the world horizon fixed during orbit
  SetAllowRotation(Standard_True);
  SetAllowPanning(Standard_True);
  SetAllowZooming(Standard_True);
  SetAllowZFocus(Standard_False);
  SetAllowDragging(Standard_False);
  setNavPreset(m_preset);
  // OCCT counts a press/release pair as a click only within 3 device pixels; a click that drifts more becomes
  // a rubber band that selects nothing. Allow a little hand jitter, scaled for high-DPI screens.
  myMouseClickThreshold = 5.0 * viewScale().x();
  m_initialised = true;
  m_sceneBackground = savedSceneBackground();
  applyTokens();
  setRenderQuality(savedRenderQuality());
  setSceneBackground(m_sceneBackground);
  showGrid();
  setTwoDimensional(m_twoDimensional);
  sync();
  if (std::exchange(m_originGuide, false)) setOriginGuide(true);  // asked for before the viewer existed
}

// ---------------------------------------------------------------- tokens
void Viewport::setTokens(const Tokens& t) {
  m_tokens = t;
  if (m_initialised) applyTokens();
  if (layered()) scheduleLooks();  // ghosts take the theme's ghost colour and alpha
}

void Viewport::applyTokens() {
  const Tokens& t = m_tokens;
  refreshMeasurement(true);
  updateSectionGizmo();  // its colours are baked in
  m_view->SetBackgroundColor(occ(t.vp));
  m_view->SetBgGradientStyle(Aspect_GradientFillMethod_None);
  setSceneBackground(m_sceneBackground);
  // The roles (UI-38, Highlight.hpp): the hover glows white, the selection is hued. OCCT's selected styles draw only
  // what has no glow of ours (a body shown through the stock path, a drawing layer's lines); ours are SubHighlights.
  const Quantity_Color hover = occ(t.hover), selected = occ(t.selected3d);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetColor(hover);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic)->SetColor(hover);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetTransparency(0.35f);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic)->SetTransparency(0.35f);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic)->SetDisplayMode(AIS_Shaded);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic)->SetFaceBoundaryDraw(false);
  m_ctx->SetToHilightSelected(true);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetColor(selected);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalSelected)->SetColor(selected);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetTransparency(0.6f);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalSelected)->SetTransparency(0.6f);
  // X-ray selection: the highlight is drawn in the Topmost layer, which has its own depth buffer,
  // so a selected object shows through whatever is in front of it.
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetZLayer(Graphic3d_ZLayerId_Topmost);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalSelected)->SetZLayer(Graphic3d_ZLayerId_Topmost);
  const double edge = highlight::kHoverEdgeWidth;
  for(auto kind:{Prs3d_TypeOfHighlight_Dynamic,Prs3d_TypeOfHighlight_LocalDynamic}) {
    auto drawer=m_ctx->HighlightStyle(kind);
    drawer->SetZLayer(Graphic3d_ZLayerId_Topmost);
    drawer->SetShadingAspect(new Prs3d_ShadingAspect());
    drawer->ShadingAspect()->SetColor(hover);
    drawer->ShadingAspect()->SetTransparency(0.55f);
    drawer->ShadingAspect()->Aspect()->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);
    drawer->SetFaceBoundaryDraw(true);
    drawer->SetFaceBoundaryAspect(new Prs3d_LineAspect(hover,Aspect_TOL_SOLID,edge));
    drawer->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_BALL,hover,5));
    drawer->PointAspect()->Aspect()->SetInteriorColor(Quantity_ColorRGBA(hover,0.65f));
    drawer->PointAspect()->Aspect()->SetAlphaMode(Graphic3d_AlphaMode_Blend);
    drawer->SetLineAspect(new Prs3d_LineAspect(hover,Aspect_TOL_SOLID,edge));
    drawer->SetWireAspect(new Prs3d_LineAspect(hover,Aspect_TOL_SOLID,edge));
  }
  // A curve body hovered whole: lines as wide as a hovered edge; on the light theme a darker rim under hovered lines.
  HoverLines& lines = HoverLines::current();
  lines.coreWidth = float(edge);
  lines.rim.Nullify();
  if (const QColor rim = highlight::hoverRim(t); rim.isValid()) {
    lines.rim = new Prs3d_Drawer();
    lines.rim->SetLink(m_ctx->DefaultDrawer());  // the rest (deflection, the other aspects) as everything else
    lines.rim->SetColor(occ(rim));
    lines.rim->SetZLayer(Graphic3d_ZLayerId_Topmost);
    lines.rim->SetDisplayMode(0);
    lines.rim->SetWireAspect(new Prs3d_LineAspect(occ(rim), Aspect_TOL_SOLID, highlight::kHoverRimWidth));
    lines.rim->SetLineAspect(new Prs3d_LineAspect(occ(rim), Aspect_TOL_SOLID, highlight::kHoverRimWidth));
  }
  if (!m_subHl.IsNull()) refreshSubHighlight();  // drawn by us in the selection colour
  applyGridColors();
  for (const auto& [ais, glow] : m_bodyGlows) m_ctx->Remove(glow, Standard_False);  // made again in the new colours
  if (!m_bodyGlows.empty()) { m_bodyGlows.clear(); applySelectionLayers(); }
  // View cube per the design: flat three-tone box with dark labels, thin X/Y/Z axes in red/green/blue along
  // the lower edges, and the hovered face/edge/corner filled with the hover accent to show where a click goes.
  m_cube->SetBoxColor(occ(t.mtop));
  m_cube->BoxSideStyle()->SetColor(occ(t.mtop));
  m_cube->BoxEdgeStyle()->SetColor(occ(t.mtop));    // edge and corner bands are drawn on the faces: same colour = invisible
  m_cube->BoxCornerStyle()->SetColor(occ(t.mtop));
  m_cube->SetTextColor(occ(t.medge));
  m_cube->SetInnerColor(occ(t.mleft));
  m_cube->SetBoxTransparency(0.0);
  m_cube->SetSize(64 * m_cubeScale);
  m_cube->SetRoundRadius(0.0);  // a plain cube: no bevelled edges or corners
  m_cube->SetBoxFacetExtension(0.0);
  m_cube->SetBoxEdgeGap(0.0);
  m_cube->SetBoxEdgeMinSize(0.0);
  m_cube->SetBoxCornerMinSize(0.0);
  m_cube->Attributes()->SetFaceBoundaryDraw(Standard_True);  // crisp edges between the faces, as in the design
  m_cube->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(t.medge), Aspect_TOL_SOLID, 1.0));
  m_cube->SetDrawAxes(Standard_True);
  m_cube->SetAxesPadding(6 * m_cubeScale);
  m_cube->SetAxesRadius(0.6 * m_cubeScale);
  m_cube->SetAxesConeRadius(1.2 * m_cubeScale);
  m_cube->SetAxesSphereRadius(1.0 * m_cubeScale);
  const QColor axisColor[3] = {t.dark ? QColor("#e05a52") : QColor("#c62828"), t.dark ? QColor("#4fc46a") : QColor("#2e7d32"), t.dark ? QColor("#5b95f5") : QColor("#1e5fd1")};
  const Prs3d_DatumParts axisPart[3] = {Prs3d_DatumParts_XAxis, Prs3d_DatumParts_YAxis, Prs3d_DatumParts_ZAxis};
  Handle(Prs3d_DatumAspect) axes = m_cube->Attributes()->DatumAspect();
  for (int i = 0; i < 3; ++i) {
    axes->ShadingAspect(axisPart[i])->SetColor(occ(axisColor[i]));
    axes->LineAspect(axisPart[i])->SetColor(occ(axisColor[i]));
    axes->TextAspect(axisPart[i])->SetColor(occ(axisColor[i]));
    axes->TextAspect(axisPart[i])->SetHeight(11);
  }
  // The cube draws its hover fill with the dynamic-highlight drawer's shading aspect (not its colour), so
  // recolour the aspect OCCT set up rather than replacing the drawer.
  m_cube->DynamicHilightAttributes()->ShadingAspect()->SetColor(occ(t.hover));  // the hover's white glow (UI-38)
  m_cube->DynamicHilightAttributes()->ShadingAspect()->SetTransparency(0.25f);
  Handle(NavCube)::DownCast(m_cube)->setCurrentColor(occ(t.selected3d));  // the side looked at: the selection's role
  // Edge and corner fills lie in the face planes (NavCube); pull the fill a hair towards the eye so it wins
  // the depth test instead of fighting the face.
  m_cube->DynamicHilightAttributes()->ShadingAspect()->Aspect()->SetPolygonOffsets(Aspect_POM_Fill, -1.0f, -1.0f);
  m_ctx->Redisplay(m_cube, Standard_False);
  setStyle(m_style);
  updateAnnotations();
  updateClipPlanes();
  redrawScene();
}

// ---------------------------------------------------------------- navigation presets (F16)
void Viewport::setNavPreset(NavPreset p) {
  finishTrackpadScroll();
  m_preset = p;
  AIS_MouseGestureMap& map = ChangeMouseGestureMap();
  map.Clear();
  const unsigned L = Aspect_VKeyMouse_LeftButton, M = Aspect_VKeyMouse_MiddleButton, R = Aspect_VKeyMouse_RightButton;
  const unsigned SHIFT = Aspect_VKeyFlags_SHIFT, CTRL = Aspect_VKeyFlags_CTRL;
  map.Bind(L, AIS_MouseGesture_SelectRectangle);
  map.Bind(L | CTRL, AIS_MouseGesture_SelectRectangle);
  map.Bind(L | SHIFT, AIS_MouseGesture_SelectRectangle);
  // Ctrl+click adds to the selection or takes a picked item out, as Shift+click does (UI-09: it selected nothing); a
  // circle centre under Ctrl+hover is still taken first (mousePressEvent, m_ctrlCenterPick).
  ChangeMouseSelectionSchemes().Bind(L | CTRL, AIS_SelectionScheme_XOR);
  switch (p) {
    case NavPreset::Fusion:
      map.Bind(M, AIS_MouseGesture_Pan);
      map.Bind(M | SHIFT, AIS_MouseGesture_RotateOrbit);
      map.Bind(M | CTRL, AIS_MouseGesture_Zoom);
      break;
    case NavPreset::SolidWorks:
      map.Bind(M, AIS_MouseGesture_RotateOrbit);
      map.Bind(M | CTRL, AIS_MouseGesture_Pan);
      map.Bind(M | SHIFT, AIS_MouseGesture_Zoom);
      break;
    case NavPreset::Onshape:
      map.Bind(R, AIS_MouseGesture_RotateOrbit);
      map.Bind(M, AIS_MouseGesture_Pan);
      map.Bind(R | CTRL, AIS_MouseGesture_Pan);
      break;
    case NavPreset::Blender:
      map.Bind(M, AIS_MouseGesture_RotateOrbit);
      map.Bind(M | SHIFT, AIS_MouseGesture_Pan);
      map.Bind(M | CTRL, AIS_MouseGesture_Zoom);
      break;
    case NavPreset::Cad2D:  // drafting: the middle button pans, the wheel (or Ctrl+middle) zooms, nothing orbits
      map.Bind(M, AIS_MouseGesture_Pan);
      map.Bind(M | SHIFT, AIS_MouseGesture_Pan);
      map.Bind(M | CTRL, AIS_MouseGesture_Zoom);
      break;
  }
  // Meta is reserved for synthetic trackpad drags; qt_flags() does not pass it from physical mouse events.
  map.Bind(M | Aspect_VKeyFlags_META, AIS_MouseGesture_Pan);
  map.Bind(M | Aspect_VKeyFlags_META | SHIFT, p == NavPreset::Cad2D ? AIS_MouseGesture_Pan : AIS_MouseGesture_RotateOrbit);
  if (m_twoDimensional)
    for (AIS_MouseGestureMap::Iterator it(map); it.More(); it.Next())
      if (it.Value() == AIS_MouseGesture_RotateOrbit || it.Value() == AIS_MouseGesture_RotateView)
        it.ChangeValue() = AIS_MouseGesture_Pan;
}

bool Viewport::orbitGesture(unsigned gesture) const {
  const unsigned L = Aspect_VKeyMouse_LeftButton, M = Aspect_VKeyMouse_MiddleButton, R = Aspect_VKeyMouse_RightButton;
  const unsigned SHIFT = Aspect_VKeyFlags_SHIFT;
  (void)L;
  if (gesture == (M | Aspect_VKeyFlags_META | SHIFT)) return m_preset != NavPreset::Cad2D;  // Shift + two-finger drag
  switch (m_preset) {
    case NavPreset::Fusion: return gesture == (M | SHIFT);
    case NavPreset::SolidWorks: return gesture == M;
    case NavPreset::Onshape: return gesture == R;
    case NavPreset::Blender: return gesture == M;
    case NavPreset::Cad2D: return false;
  }
  return false;
}

void Viewport::twoDimensionalHint(const QPoint& global) {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - m_twoDHintShown < 1500) return;  // once per attempt, not per trackpad event
  m_twoDHintShown = now;
  const QString text = tr("2D mode is on: turn it off (Shift+2) to orbit");
  QToolTip::showText(global + QPoint(14, 18), text, this, QRect(), 2500);
  emit hoverChanged(text);
}

// ---------------------------------------------------------------- display styles (F19)
namespace {
// Silhouettes (UI-48): a closed body outlined in its edges' colour where it turns away from the eye, where no edge lies
// (OCCT's outline pass: its back faces drawn again a pixel further out). The flag is read as each frame is drawn, so turning
// it on or off recomputes nothing. Translucent and clipped bodies are drawn without culling, so never outlined.
bool outline(const Handle(AIS_Shape)& ais, bool on, const Quantity_Color* edge) {
  const Handle(Prs3d_Drawer)& d = ais->Attributes();
  if (!d->HasOwnShadingAspect()) return false;  // the context's default, shared by every body that has none
  const Handle(Graphic3d_AspectFillArea3d)& aspect = d->ShadingAspect()->Aspect();
  const bool changed = bool(aspect->ToDrawSilhouette()) != on || (edge && !aspect->EdgeColor().IsEqual(*edge));
  aspect->SetDrawSilhouette(on);
  if (edge) {
    aspect->SetEdgeColor(*edge);
    aspect->SetEdgeWidth(d->FaceBoundaryAspect()->Aspect()->Width());  // as wide as the edges
  }
  return changed;
}
bool outline(const Handle(AIS_Shape)& ais, bool on, const Quantity_Color& edge) { return outline(ais, on, &edge); }
}  // namespace

void Viewport::outlineBodies() {
  const bool on = m_style == Style::ShadedEdges && !m_degraded;
  for (const auto& [id, item] : m_items) outline(item.ais, on, nullptr);
}

int Viewport::outlinedBodies() const {
  return int(std::count_if(m_items.begin(), m_items.end(), [](const auto& item) {
    const Handle(Prs3d_Drawer)& d = item.second.ais->Attributes();
    return d->HasOwnShadingAspect() && d->ShadingAspect()->Aspect()->ToDrawSilhouette();
  }));
}

bool Viewport::applyStyle(const Handle(AIS_Shape)& ais, const BodyLook* look) {
  Handle(Prs3d_Drawer) d = ais->Attributes();
  // A drawing has nothing behind its lines. In Hidden edges visible the edges are the overlay's (ViewportEdges.cpp), save a
  // body drawn without the worker's arrays.
  const bool drawing = drawingLayer(ais), hidden = (m_style == Style::HiddenLine || m_style == Style::HiddenEdges) && !drawing;
  const auto shaped = Handle(BodyShape)::DownCast(ais);
  const bool overlaid = m_style == Style::HiddenEdges && !drawing && !shaped.IsNull() && shaped->prs() && !shaped->prs()->triangles.IsNull();
  const bool edges = m_style == Style::ShadedEdges || m_style == Style::HiddenLine || (m_style == Style::HiddenEdges && !overlaid);
  bool changed = bool(d->FaceBoundaryDraw()) != edges;
  d->SetFaceBoundaryDraw(edges);
  // Line aspects ignore alpha here: a ghost's edges are blended towards the background instead. The body's own aspect
  // is changed in place, so the drawn groups (which share it) follow SynchronizeAspects as well as a recompute.
  QColor edge = hidden ? m_tokens.fg : m_tokens.medge;
  if (look && look->ghost) {
    const double t = 1 - look->opacity;
    edge = QColor::fromRgbF(edge.redF() + (m_tokens.vp.redF() - edge.redF()) * t, edge.greenF() + (m_tokens.vp.greenF() - edge.greenF()) * t,
                            edge.blueF() + (m_tokens.vp.blueF() - edge.blueF()) * t);
  }
  if (look && look->lineWidth > 0) edge = QColor::fromRgbF(look->color[0], look->color[1], look->color[2]);  // a drawing's glyph and hatch outlines
  if (d->HasOwnFaceBoundaryAspect()) d->FaceBoundaryAspect()->SetColor(occ(edge));
  else d->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(edge), Aspect_TOL_SOLID, 1.0));
  if (look && look->lineWidth > 0) {  // a drawing's lines: hairlines times the display scale (UI-10), its layer's weight and type
    for (const auto& [own, line] : {std::pair{d->HasOwnWireAspect(), d->WireAspect()}, {d->HasOwnLineAspect(), d->LineAspect()},
                                    {d->HasOwnFreeBoundaryAspect(), d->FreeBoundaryAspect()}})
      if (own) {
        line->SetWidth(look->lineWidth);
        line->Aspect()->SetLinePattern(look->linePattern);
        line->Aspect()->SetLineStippleFactor(look->lineFactor);
      }
    d->FaceBoundaryAspect()->SetWidth(lineWidth());  // outlines of fills and text stay hairlines
  }
  if (const auto body = Handle(BodyShape)::DownCast(ais); !body.IsNull()) changed = body->setHiddenLine(hidden, occ(sceneBackgroundColor()), occ(edge)) || changed;
  if (outline(ais, m_style == Style::ShadedEdges && !m_degraded, occ(edge))) ais->SynchronizeAspects();
  const int shadedMode = Handle(AIS_TexturedShape)::DownCast(ais).IsNull() ? AIS_Shaded : 3;  // an SVG's image: textured
  if (changed) ais->SetToUpdate(shadedMode);
  m_ctx->SetDisplayMode(ais, m_style == Style::Wireframe ? AIS_WireFrame : shadedMode, Standard_False);
  return changed;
}

void Viewport::setStyle(Style s) {
  m_style = s;
  if (!m_initialised) return;
  if (m_styleJob) m_styleJob->cancel();
  auto bodies = std::make_shared<std::vector<std::string>>();
  for (const auto& [id, it] : m_items) bodies->push_back(id);
  auto next = std::make_shared<size_t>(0);
  auto selected = std::make_shared<bool>(false);
  auto step = [this, bodies, next, selected] {
    if (*next >= bodies->size()) return false;
    if (const auto it = m_items.find((*bodies)[(*next)++]); it != m_items.end()) {
      // Not Redisplay: that dropped the body from the selection. The mode shown only, and only when it changed.
      if (applyStyle(it->second.ais, &it->second.look) && it->second.ais->DisplayMode() != AIS_WireFrame)
        m_ctx->RecomputePrsOnly(it->second.ais, Standard_False, Standard_False);
      *selected = *selected || m_ctx->IsSelected(it->second.ais);
    }
    return *next < bodies->size();
  };
  auto done = [this, selected](bool) {
    m_styleJob = nullptr;
    if (*selected) m_ctx->HilightSelected(Standard_False);
    scheduleEdgeOverlay();
    redrawScene();
  };
  if (!m_jobs || bodies->size() <= 16) {
    while (step()) {}
    return done(true);
  }
  m_styleJob = m_jobs->sliced(tr("Changing the display style"), [step](Job&) { return step(); }, done, JobKind::Background);
}

void Viewport::setGrid(bool on) {
  (m_sketchInput ? m_sketchGrid : m_grid) = on;
  showGrid();
}

void Viewport::showGrid() {
  if (!m_initialised) return;
  updateGridExtent();
  applyGridColors();
  if (gridDrawn()) m_viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
  else m_viewer->DeactivateGrid();
  redrawScene();
  emit gridShownChanged(gridShown());
}

// In a sketch every snap step is a line (24 to 60 px apart), so they are faint and every tenth one a little less: the
// theme's viewport colour with a share of its text colour. In 3D and 2D mode (a tenth of the view apart), OCCT's greys.
void Viewport::applyGridColors() {
  if (!m_initialised) return;
  const Handle(Aspect_Grid) grid = m_viewer->Grid(Aspect_GT_Rectangular);
  if (grid.IsNull()) return;
  if (!m_sketchInput) return grid->SetColors(Quantity_NOC_GRAY50, Quantity_NOC_GRAY70);
  auto mix = [&](double share) {
    const QColor& a = m_tokens.vp;
    const QColor& b = m_tokens.fg;
    return Quantity_Color(a.redF() + (b.redF() - a.redF()) * share, a.greenF() + (b.greenF() - a.greenF()) * share, a.blueF() + (b.blueF() - a.blueF()) * share, Quantity_TOC_sRGB);
  };
  grid->SetColors(mix(m_tokens.dark ? 0.09 : 0.11), mix(m_tokens.dark ? 0.22 : 0.26));
}

void Viewport::setGridSnap(bool on) {
  if (m_gridSnap == on) return;
  m_gridSnap = on;
  emit gridSnapChanged(on);
}

// What updateInfiniteGrid lays out for the current zoom, without waiting for the redraw that lays it out.
double Viewport::gridStep() const {
  if (!m_initialised || !(m_twoDimensional || m_sketchInput)) return m_gridStep;
  const double step = layoutStep();
  return step > 0 ? step : m_gridStep;
}

// The step the sketch or 2D grid takes at this zoom (0: none). In a sketch every line is a snap step: sketchsnap::gridStep,
// 24 to 60 px at the plane's own scale, in the shown length unit; a set spacing up to a quarter of the view (then its
// fractions). In 2D mode (the drawing viewer, its placer) a tenth of the view rounded down to a power of ten, or the set
// spacing while that draws at most 400 lines.
double Viewport::layoutStep() const {
  if (m_sketchInput) {
    const double pixel = planePixel(), unit = 1 / units::toDisplay(units::Kind::Length, 1.0);
    return pixel > 0 && std::isfinite(pixel) ? sketchsnap::gridStep(pixel, m_gridSpacing, std::isfinite(unit) ? unit : 1, std::min(width(), height()) / 4.0) : 0;
  }
  const gp_XYZ size = m_view->Camera()->ViewDimensions();
  const double span = std::max(size.X(), size.Y());
  if (!(span > 1e-9) || !std::isfinite(span)) return 0;
  return m_gridSpacing > 0 && span / m_gridSpacing <= 400 ? m_gridSpacing : std::pow(10.0, std::floor(std::log10(span / 10.0)));
}

// World units per widget pixel on the sketch's plane at the view's centre, along the screen direction where they are
// longest: on a plane tilted away from the screen a step is shorter on the screen along the tilt (by the tilt's cosine),
// so the grid is laid out for that direction (up to ten times the flat size: a plane seen nearly edge on).
double Viewport::planePixel() const {
  const double flat = pixelSize();
  if (!m_sketchInput || !m_initialised) return flat;
  const QPointF c(width() / 2.0, height() / 2.0);
  double u0, v0, u1, v1, u2, v2;
  if (!planePoint(c, m_sketchFrame, u0, v0) || !planePoint(c + QPointF(100, 0), m_sketchFrame, u1, v1) || !planePoint(c + QPointF(0, 100), m_sketchFrame, u2, v2)) return flat;
  const double along = std::max(std::hypot(u1 - u0, v1 - v0), std::hypot(u2 - u0, v2 - v0)) / 100;
  if (!std::isfinite(along) || along < flat * 1.02) return flat;  // facing the screen (to rounding)
  return std::min(along, 10 * flat);
}

QPointF Viewport::pixelAlign(const opad::Vec3& world, int width) const {
  if (!m_initialised || m_view->Window().IsNull()) return {};
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  const gp_Pnt ndc = m_view->Camera()->Project(gp_Pnt(world[0], world[1], world[2]));
  const double x = (ndc.X() + 1) * 0.5 * w, y = (1 - ndc.Y()) * 0.5 * h;  // device pixels from the top left, continuous
  auto onPixels = [&](double c) { return width % 2 ? std::floor(c) + 0.5 : std::round(c); };
  const QPointF scale = viewScale();
  if (!std::isfinite(x) || !std::isfinite(y) || !(scale.x() > 0) || !(scale.y() > 0)) return {};
  return {(onPixels(x) - x) / scale.x(), (onPixels(y) - y) / scale.y()};
}

// ---------------------------------------------------------------- the sketch's own cursor
void Viewport::setOwnCursor(bool on) {
  m_ownCursorWanted = on;
  applyOwnCursor();
}

void Viewport::applyOwnCursor() {
  const bool hide = m_ownCursorWanted && m_sketchInput && m_initialised && !m_blocked && !m_ownCursorAside && !QApplication::activePopupWidget();
  // Asked for, it is blank again also when another part of the view set or reset the widget's cursor meanwhile (the
  // section plane's handle, a measurement anchor, clearing a dimension).
  const bool blank = testAttribute(Qt::WA_SetCursor) && cursor().shape() == Qt::BlankCursor;
  if (hide == m_ownCursorShown && (!hide || blank)) return;
  const bool changed = hide != m_ownCursorShown;
  m_ownCursorShown = hide;
  if (hide) {
    // The overlays on the view (value boxes, prompt, chips) would inherit the blank cursor: they keep the arrow (one made
    // later gets it when it is polished, Viewport::event).
    for (QWidget* child : findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
      if (!child->testAttribute(Qt::WA_SetCursor)) child->setCursor(Qt::ArrowCursor);
    setCursor(Qt::BlankCursor);
  } else unsetCursor();
  if (changed) emit ownCursorChanged(hide);
}

void Viewport::updateGridExtent() {
  if (!m_initialised) return;
  m_gridSpacing=QSettings().value("view/gridSpacing",0.0).toDouble();
  if (m_twoDimensional || m_sketchInput) { updateInfiniteGrid(true); return; }
  // In 3D the grid is a patch under the model, in the grid plane: around the plane's origin while that square would be
  // at most four times the model's own (most parts), else around the model's footprint. A house 600 m out (an OBJ keeping
  // its site coordinates) got a 1.4 km sheet around (0,0,0), which also pulled box-less fits to the origin.
  const gp_Ax3 plane=m_viewer->PrivilegedPlane();
  const Bnd_Box bounds=fitBounds(false);
  const double minimum=std::max(100.0,m_gridExtentSetting);
  double cx=0,cy=0,half=0;
  if (!bounds.IsVoid()) {
    const auto lo=bounds.CornerMin(), hi=bounds.CornerMax();
    Bnd_Box2d footprint;
    for (int i=0;i<8;++i) {
      const gp_Vec rel(plane.Location(),gp_Pnt(i&1?hi.X():lo.X(),i&2?hi.Y():lo.Y(),i&4?hi.Z():lo.Z()));
      footprint.Add(gp_Pnt2d(rel.Dot(gp_Vec(plane.XDirection())),rel.Dot(gp_Vec(plane.YDirection()))));
    }
    double u0,v0,u1,v1;
    footprint.Get(u0,v0,u1,v1);
    half=std::max(u1-u0,v1-v0)/2*1.1;
    const double around=std::max({-u0,u1,-v0,v1})*1.1;  // the square around the origin that holds the model
    if (around>std::max(minimum,4*half)) cx=(u0+u1)/2,cy=(v0+v1)/2;
    else half=around;
  }
  double extent=std::max(half,minimum);
  const double step=m_gridSpacing>0?m_gridSpacing:std::pow(10.0,std::floor(std::log10(extent/10.0)));
  // Every tenth line stays on a world multiple of ten steps (and snapping on multiples of the step).
  const double major=10*step,ox=std::round(cx/major)*major,oy=std::round(cy/major)*major;
  extent+=std::max(std::abs(ox-cx),std::abs(oy-cy));
  m_gridStep=step;
  m_gridShownStep=0;
  placeGrid(ox,oy,step,extent);
  if (trace::enabled()) trace::log(QStringLiteral("3D grid: spacing %1 around (%2, %3), %4 each way").arg(step).arg(ox).arg(oy).arg(extent));
}

// The grid centred on (u, v) of the privileged plane. V3d_RectangularGrid draws its lines around (-XOrigin, -YOrigin)
// (UpdateDisplay translates by minus the origin) while Aspect_RectangularGrid::Compute snaps around (+XOrigin, +YOrigin);
// OPAD snaps by itself, so the origin is handed over negated and the lines lie where they are meant to.
void Viewport::placeGrid(double u, double v, double step, double extent) {
  m_viewer->SetRectangularGridValues(-u,-v,step,step,0);
  m_viewer->SetRectangularGridGraphicValues(extent,extent,0);
}

bool Viewport::benchGridEcho() const { return m_initialised && m_viewer->GridEcho(); }

Bnd_Box Viewport::benchGridBox() const {
  Bnd_Box box;
  if (!m_initialised) return box;
  Graphic3d_MapOfStructure displayed;
  m_viewer->StructureManager()->DisplayedStructures(displayed);
  for (Graphic3d_MapOfStructure::Iterator it(displayed); it.More(); it.Next())
    if (it.Key()->IsInfinite() && it.Key()->TransformPersistence().IsNull()) box.Add(it.Key()->MinMaxValues(Standard_True));
  return box;
}

// OCCT's grid is a finite patch. In 2D mode and while sketching (in the sketch's plane) it is laid out again around
// what the view shows whenever the view gets near its edge or the zoom asks for another spacing (layoutStep: in a sketch
// 1, 2 or 5 times a power of ten, 24 to 60 px apart, every line a snap step, so grid snapping lands on what is drawn).
// Its origin is on a multiple of ten steps: OCCT draws every tenth line from it darker, so those stay on round values
// (50, 100 mm) through the sketch's axes and do not move with a pan. Called from every redraw, so the test whether
// anything changed comes first.
void Viewport::updateInfiniteGrid(bool force) {
  if (!m_initialised || !gridDrawn()) return;
  const auto camera = m_view->Camera();
  const gp_XYZ size = camera->ViewDimensions();
  const double pixel = pixelSize(), step = layoutStep();
  // What the view shows of the plane: longer along a tilt (planePixel), as the step is.
  const double span = std::max(size.X(), size.Y()) * (m_sketchInput && pixel > 0 ? planePixel() / pixel : 1.0);
  if (!(span > 1e-9) || !std::isfinite(span) || !(step > 0) || !std::isfinite(step)) return;
  const gp_Ax3 plane = m_viewer->PrivilegedPlane();
  const gp_Vec rel(plane.Location(), camera->Center());
  const double cx = rel.Dot(gp_Vec(plane.XDirection())), cy = rel.Dot(gp_Vec(plane.YDirection()));
  const double off = std::hypot(cx - m_gridShownX, cy - m_gridShownY);
  if (!force && step == m_gridShownStep && off + span / 2 <= m_gridShownExtent * 0.9 && m_gridShownExtent <= span * 3) return;
  const double major = 10 * step, ox = std::round(cx / major) * major, oy = std::round(cy / major) * major;
  const double extent = std::ceil((span * 1.5 + std::max(std::abs(ox - cx), std::abs(oy - cy))) / step) * step;
  m_gridStep = m_gridShownStep = step;
  m_gridShownX = ox;
  m_gridShownY = oy;
  m_gridShownExtent = extent;
  placeGrid(ox, oy, step, extent);
  if (trace::enabled()) trace::log(QStringLiteral("2D grid: spacing %1 around (%2, %3), %4 each way").arg(step).arg(ox).arg(oy).arg(extent));
}

void Viewport::setShadows(bool on) {
  if (!m_initialised) return;
  // The overhead light only (UI-45): the headlight's shadows fall behind what casts them, out of sight, and cost a pass.
  for (V3d_ListOfLightIterator it = m_viewer->ActiveLightIterator(); it.More(); it.Next())
    if (it.Value()->Type() == Graphic3d_TypeOfLightSource_Directional) it.Value()->SetCastShadows(on && !it.Value()->IsHeadlight());
  m_view->ChangeRenderingParams().ShadowMapResolution = on ? 2048 : 1024;
  redrawScene();
}

void Viewport::setOrthographic(bool ortho) {
  if (!m_initialised) return;
  if (m_twoDimensional) ortho=true;
  m_view->Camera()->SetProjectionType(ortho ? Graphic3d_Camera::Projection_Orthographic : Graphic3d_Camera::Projection_Perspective);
  m_view->Invalidate();
  redrawScene();
}

bool Viewport::isOrthographic() const {
  return !m_initialised || m_view->Camera()->ProjectionType() == Graphic3d_Camera::Projection_Orthographic;
}

// ---------------------------------------------------------------- selection (F22)
void Viewport::activateSelection(const Handle(AIS_Shape)& ais) {
  m_ctx->Load(ais, -1);  // register with the selection manager (picking BVH); Display() with mode -1 does not
  m_ctx->Deactivate(ais);
  if (!m_bodiesPickable) return;  // sketching, or a feature input that only takes sketch regions / planes
  if (const auto node = m_nodeOf.find(ais.get()); node != m_nodeOf.end()) {  // a ghost, a locked or a hidden body (its look)
    if (const auto item = m_items.find(node->second); item != m_items.end() && !item->second.look.shownPickable()) return;
    if (const auto wire = m_sketchWires.find(node->second); wire != m_sketchWires.end() && !wire->second.look.shownPickable()) return;
  }
  TopAbs_ShapeEnum t = TopAbs_SHAPE;
  switch (m_filter) {
    case SelFilter::Body: t = TopAbs_SHAPE; break;
    case SelFilter::Face: t = TopAbs_FACE; break;
    case SelFilter::Edge: t = TopAbs_EDGE; break;
    case SelFilter::Vertex: t = TopAbs_VERTEX; break;
  }
  // A drawing has no faces to pick (UI-42): its text and hatches are a layer's, which the Face filter picks whole. OCCT
  // built a sensitive per glyph and hatch face on the UI thread there (a DWG's text layer: 0.1-0.2 s, 2.3-2.7 s in all).
  if (t == TopAbs_FACE && drawingLayer(ais)) t = TopAbs_SHAPE;
  m_ctx->Activate(ais, AIS_Shape::SelectionMode(t));
}

bool Viewport::drawingLayer(const Handle(AIS_InteractiveObject)& ais) const {
  const auto node = m_nodeOf.find(ais.get());
  const opad::Node* n = node == m_nodeOf.end() ? nullptr : m_doc->scene.node(node->second);
  return n && n->representation == "drawing2d";
}

void Viewport::setSelectionFilter(SelFilter f) {
  // The same filter again changes nothing: every body was activated in it as it was displayed (re-activating a big
  // drawing layer's thousands of edges costs OCCT a few hundred ms per layer). Who waits for filterApplied still gets it.
  if (f == m_filter && !m_filterJob && m_initialised) {
    QTimer::singleShot(0, this, [this] { emit filterApplied(); });
    return;
  }
  applySelectionFilter(f);
}

void Viewport::applySelectionFilter(SelFilter f) {
  resetHoverFade();
  m_filter = f;
  m_hoverOwner = nullptr;  // owners are rebuilt per mode; an address may be reused
  if (!m_initialised) return;
  clearSelection();
  // Activating a selection mode builds that mode's sensitive entities per object (faces/edges), which is
  // slow across a big assembly, so it runs as a sliced job.
  if (m_filterJob) m_filterJob->cancel();
  auto items = std::make_shared<std::vector<Handle(AIS_Shape)>>();
  for (const auto& [id, it] : m_items) items->push_back(it.ais);
  for (const auto& [id, it] : m_sketchWires) items->push_back(it.ais);
  auto i = std::make_shared<size_t>(0);
  m_filterJob = m_jobs->sliced(tr("Switching selection mode"), [this, items, i](Job&) {
    if (*i >= items->size()) return false;
    activateSelection((*items)[(*i)++]);
    return *i < items->size();
  }, [this](bool completed) {
    m_filterJob = nullptr;
    applySelectionLayers();
    redrawScene();
    if (completed) emit filterApplied();
  });
}

std::vector<opad::Ref> Viewport::selection() const {
  std::vector<opad::Ref> out;
  if (!m_initialised) return out;
  for (const auto& b : m_shadeBodies) {  // a selection too large to highlight per object (see selectNodes)
    opad::Ref r;
    r.body = b;
    out.push_back(r);
  }
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) {
    Handle(AIS_InteractiveObject) obj = m_ctx->SelectedInteractive();
    auto center = m_centerObjects.find(obj.get());
    if (center != m_centerObjects.end()) { out.push_back(m_centers.at(center->second).ref); continue; }
    if (!Handle(CircleOwner)::DownCast(m_ctx->SelectedOwner()).IsNull() || !Handle(OccluderOwner)::DownCast(m_ctx->SelectedOwner()).IsNull()) continue;
    auto it = m_nodeOf.find(obj.get());
    if (it == m_nodeOf.end()) continue;
    opad::Ref r;
    r.body = it->second;
    Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->SelectedOwner());
    const auto mine=Handle(SubShapeOwner)::DownCast(owner);
    if(!mine.IsNull() && m_filter!=SelFilter::Body) {r.kind=mine->kind();r.index=mine->index();}
    else if (!owner.IsNull() && owner->HasShape() && m_filter != SelFilter::Body) {
      const TopoDS_Shape& sub = owner->Shape();
      const auto shape=Handle(AIS_Shape)::DownCast(obj)->Shape();
      switch (sub.ShapeType()) {
        case TopAbs_FACE: r.kind = opad::Ref::Kind::Face; break;
        case TopAbs_EDGE: r.kind = opad::Ref::Kind::Edge; break;
        case TopAbs_VERTEX: r.kind = opad::Ref::Kind::Vertex; break;
        default: break;
      }
      if (r.kind != opad::Ref::Kind::Body) {
        Handle(SubShapeOwner) mine = Handle(SubShapeOwner)::DownCast(owner);  // knows its ordinal: no walk over the body
        r.index = mine.IsNull() ? opad::subshape_index(shape, sub) : mine->index();
      }
    }
    out.push_back(r);
  }
  return out;
}

// ---------------------------------------------------------------- selection
// Highlighting goes through the context one object at a time as a sliced job (Jobs.hpp), so the UI never
// blocks. Once the first slice shows that highlighting the whole set would take longer than kShadeAfterMs,
// the job stops and shades the selected region with translucent boxes instead (one per selected node);
// selection() then reports the shaded bodies exactly as if they had been highlighted.
namespace {
constexpr int kShadeAfterMs = 500;  // per-object highlight is allowed this long in total, else shade
constexpr int kMeasureMs = 10;      // how much highlighting to do before projecting the total
}  // namespace

void Viewport::setJobs(JobRunner* jobs) { m_jobs = jobs; }

void Viewport::selectNodes(const std::vector<std::string>& ids) {
  resetHoverFade();
  if (!m_initialised) {
    emit selectionApplied();
    return;
  }
  if (m_selJob) m_selJob->cancel();  // its done(false) runs now and leaves m_selJob null
  clearShade();
  struct State {
    std::vector<Handle(AIS_Shape)> undo, targets, applied;
    size_t u = 0, i = 0;
    bool measured = false, shade = false, cleared = false;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  st->undo = std::move(m_selApplied);
  m_selApplied.clear();
  for (const auto& id : ids)
    for (const auto& body : m_doc->scene.bodies_under(id)) {
      auto it = m_items.find(body);
      if (it != m_items.end()) st->targets.push_back(it->second.ais);
    }
  const size_t total = st->targets.size();
  auto step = [this, st, total](Job& j) -> bool {
    // Phase 1: drop the previous highlight one object at a time (a bulk ClearSelected would block).
    if (st->u < st->undo.size()) {
      const auto& h = st->undo[st->u++];
      if (m_ctx->IsSelected(h)) m_ctx->AddOrRemoveSelected(h, Standard_False);
      return true;
    }
    if (!st->cleared) {  // whatever else is selected came from a click: a handful, cheap to clear in bulk
      st->cleared = true;
      m_ctx->ClearSelected(Standard_False);
      st->clock.start();
    }
    // Phase 2: highlight the new set.
    if (st->i >= total) return false;
    const auto& h = st->targets[st->i++];
    m_ctx->AddOrRemoveSelected(h, Standard_False);
    st->applied.push_back(h);
    if (!st->measured && st->clock.elapsed() >= kMeasureMs) {
      st->measured = true;
      const double projected = static_cast<double>(st->clock.elapsed()) * static_cast<double>(total) / static_cast<double>(st->i);
      if (projected > kShadeAfterMs) {
        st->shade = true;
        for (const auto& a : st->applied)  // undo the few we did (bounded by kMeasureMs of work)
          if (m_ctx->IsSelected(a)) m_ctx->AddOrRemoveSelected(a, Standard_False);
        st->applied.clear();
        st->i = total;
        return false;
      }
    }
    if ((st->i & 63) == 0) j.setPhase(tr("Selecting %1 objects").arg(total), static_cast<int>(st->i * 100 / std::max<size_t>(1, total)));
    return st->i < total;
  };
  auto done = [this, st, ids](bool completed) {
    m_selJob = nullptr;
    m_selApplied = std::move(st->applied);
    applySelectionLayers();
    refreshSubHighlight();
    redrawScene();
    if (!completed) return;  // superseded or cancelled: the next job (or the click) owns the state now
    if (st->shade) showShade(ids);
    const bool notify = m_notifyWhenApplied;
    m_notifyWhenApplied = false;
    emit selectionApplied();
    if (notify) emit selectionChanged();
  };
  Q_ASSERT(m_jobs);  // wired by MainWindow before anything can be selected
  m_selJob = m_jobs->sliced(tr("Selecting %1 objects").arg(total), step, done);
}

// The selected faces, edges and vertices, drawn as one object (SubHighlight) in the Topmost layer, so they
// show through like selected bodies. OCCT would build one presentation per selected sub-shape inside the click
// (seconds to minutes for a rubber band over thousands); here their geometry is copied out of the bodies'
// existing meshes, one sub-shape per step of a sliced job, into a few large primitive arrays.
void Viewport::refreshSubHighlight() {
  if (m_subJob) m_subJob->cancel();
  if (!m_subHl.IsNull()) {
    m_ctx->Remove(m_subHl, Standard_False);
    m_subHl.Nullify();
  }
  struct State {
    std::vector<Handle(SubShapeOwner)> owners;
    size_t i = 0;
    std::vector<gp_Pnt> tv, sv, pv;  // the chunk being filled: triangle nodes, segment ends, points
    std::vector<int> ti;             // triangle indices into tv, 1-based
    Handle(SubHighlight) hl;
  };
  auto st = std::make_shared<State>();
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) {
    Handle(SubShapeOwner) o = Handle(SubShapeOwner)::DownCast(m_ctx->SelectedOwner());
    if (!o.IsNull() && Handle(CircleOwner)::DownCast(o).IsNull()) st->owners.push_back(o);
  }
  if (st->owners.empty()) return;
  QColor body;  // the selection's look over the first body close to its colour, else any (UI-38: an outline on a blue part)
  std::unordered_set<const void*> bodies;  // the colour is the body's: each one looked at once, not per selected face
  for (const auto& o : st->owners) {
    if (!bodies.insert(o->Selectable().get()).second) continue;
    if (const auto node = m_nodeOf.find(Handle(AIS_InteractiveObject)::DownCast(o->Selectable()).get()); node != m_nodeOf.end()) {
      const QColor colour = shownColor(node->second);
      if (!body.isValid() || highlight::closeToSelection(m_tokens, colour)) body = colour;
      if (highlight::closeToSelection(m_tokens, body)) break;
    }
  }
  st->hl = new SubHighlight(glowStyle(body, false));
  constexpr size_t kChunk = 200000;  // nodes per primitive array: turning a chunk into an array stays a small step
  auto flush = [st](bool all) {
    if (!st->tv.empty() && (all || st->tv.size() >= kChunk)) {
      Handle(Graphic3d_ArrayOfTriangles) a = new Graphic3d_ArrayOfTriangles(static_cast<int>(st->tv.size()), static_cast<int>(st->ti.size()));
      for (const gp_Pnt& p : st->tv) a->AddVertex(p);
      for (const int k : st->ti) a->AddEdge(k);
      st->hl->m_triangles.push_back(a);
      st->tv.clear();
      st->ti.clear();
    }
    if (!st->sv.empty() && (all || st->sv.size() >= kChunk)) {
      Handle(Graphic3d_ArrayOfSegments) a = new Graphic3d_ArrayOfSegments(static_cast<int>(st->sv.size()));
      for (const gp_Pnt& p : st->sv) a->AddVertex(p);
      st->hl->m_segments.push_back(a);
      st->sv.clear();
    }
    if (!st->pv.empty() && (all || st->pv.size() >= kChunk)) {
      Handle(Graphic3d_ArrayOfPoints) a = new Graphic3d_ArrayOfPoints(static_cast<int>(st->pv.size()));
      for (const gp_Pnt& p : st->pv) a->AddVertex(p);
      st->hl->m_points.push_back(a);
      st->pv.clear();
    }
  };
  auto step = [st, flush]() -> bool {
    if (st->i >= st->owners.size()) return false;
    const Handle(SubShapeOwner)& o = st->owners[st->i++];
    o->prepare();
    const TopoDS_Shape& sub = o->Shape();
    gp_Trsf body;  // rigid placements live on the object, not in the shape (displayBody)
    if (Handle(AIS_InteractiveObject) obj = Handle(AIS_InteractiveObject)::DownCast(o->Selectable()); !obj.IsNull()) body = obj->LocalTransformation();
    if(o->curve) {
      for(size_t i=1;i<o->curve->size();++i) {st->sv.push_back((*o->curve)[i-1].Transformed(body));st->sv.push_back((*o->curve)[i].Transformed(body));}
      flush(false);return true;
    }
    auto appendEdge=[&](const TopoDS_Edge& e) {
      const std::vector<gp_Pnt> line = edgePolyline(e);
      for (size_t n = 1; n < line.size(); ++n) {
        st->sv.push_back(line[n - 1].Transformed(body));
        st->sv.push_back(line[n].Transformed(body));
      }
    };
    TopLoc_Location loc;
    if (sub.ShapeType() == TopAbs_FACE) {
      Handle(Poly_Triangulation) t = BRep_Tool::Triangulation(TopoDS::Face(sub), loc);
      if (!t.IsNull()) {
        const gp_Trsf w = body * loc.Transformation();
        const int base = static_cast<int>(st->tv.size());
        for (int n = 1; n <= t->NbNodes(); ++n) st->tv.push_back(t->Node(n).Transformed(w));
        for (int k = 1; k <= t->NbTriangles(); ++k) {
          int a, b, c;
          t->Triangle(k).Get(a, b, c);
          st->ti.insert(st->ti.end(), {base + a, base + b, base + c});
        }
      }
      for(TopExp_Explorer edge(sub,TopAbs_EDGE);edge.More();edge.Next()) appendEdge(TopoDS::Edge(edge.Current()));
    } else if (sub.ShapeType() == TopAbs_EDGE) {
      appendEdge(TopoDS::Edge(sub));
    } else if (sub.ShapeType() == TopAbs_VERTEX) {
      st->pv.push_back(BRep_Tool::Pnt(TopoDS::Vertex(sub)).Transformed(body));
    }
    flush(false);
    return st->i < st->owners.size();
  };
  auto done = [this, st, flush](bool completed) {
    m_subJob = nullptr;
    if (!completed) return;  // superseded by a newer selection
    flush(true);
    m_subHl = st->hl;
    m_subHl->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(m_subHl, 0, -1, Standard_False);  // selection mode -1: never pickable
    redrawScene();
    emit subHighlightApplied();
  };
  if (st->owners.size() <= 64) {  // a click: no reason to wait for the next event-loop turn
    while (step()) {}
    done(true);
    return;
  }
  m_subJob = m_jobs->sliced(tr("Highlighting %1 selected").arg(st->owners.size()), [step](Job&) { return step(); }, done);
}

QColor Viewport::shownColor(const std::string& node) const {
  std::array<double, 3> c;
  if (const auto item = m_items.find(node); item != m_items.end()) c = item->second.look.color;
  else if (const auto wire = m_sketchWires.find(node); wire != m_sketchWires.end()) c = wire->second.look.color;
  else return {};
  return QColor::fromRgbF(std::clamp(c[0], 0.0, 1.0), std::clamp(c[1], 0.0, 1.0), std::clamp(c[2], 0.0, 1.0));
}

GlowStyle Viewport::glowStyle(const QColor& body, bool wholeBody) const {
  const highlight::Selection s = highlight::selection(m_tokens, body, wholeBody);
  const float scale = float(viewScale().x());  // device pixels: the same width at 100 % and 150 %
  GlowStyle g;
  g.fill = occ(s.fill);
  g.edge = occ(s.edge);
  g.halo = occ(s.halo);
  g.fillAlpha = float(s.fillAlpha);
  g.haloAlpha = float(s.haloAlpha);
  g.edgeWidth = float(s.edgeWidth) * scale;
  g.haloWidth = float(s.haloWidth) * scale;
  g.point = float(s.point) * scale;
  g.pointHalo = float(s.pointHalo) * scale;
  return g;
}

// Selected bodies over their own materials: a tint of the selection colour and its edges in a halo (UI-38), from the
// worker-built arrays, large selections sliced.
void Viewport::applySelectionLayers() {
  if(m_bodyGlowJob) m_bodyGlowJob->cancel();
  markPickedPoints();
  struct State {
    std::vector<std::pair<Handle(AIS_Shape),std::shared_ptr<BodyPrs>>> targets;
    std::vector<const AIS_InteractiveObject*> stale;
    std::set<const AIS_InteractiveObject*> keep;
    size_t i=0,removed=0;
  };
  auto state=std::make_shared<State>();
  // Only what has to change (UI-40): selected objects (Topmost, their glow) and those still in a layer they no longer
  // belong to. With nothing selected that is nothing, and no job: a load made one per batch of bodies, 70 on the Engine.
  auto target=[&](const Handle(AIS_Shape)& ais,const std::shared_ptr<BodyPrs>& prs,Graphic3d_ZLayerId rest) {
    const bool selected=m_ctx->IsSelected(ais);
    if(!selected && ais->ZLayer()==rest) return;
    state->targets.push_back({ais,prs});
    if(selected && prs) state->keep.insert(ais.get());
  };
  // A stretched canvas: no arrays of that aspect.
  for(auto& [id,it]:m_items) {auto prs=m_prs.find(it.key);target(it.ais,prs==m_prs.end()||it.stretch!=1?nullptr:prs->second,it.look.layer);}
  for(auto& [id,wire]:m_sketchWires) target(wire.ais,wire.prs,Graphic3d_ZLayerId_Default);
  for(const auto& [ais,glow]:m_bodyGlows) if(!state->keep.count(ais)) state->stale.push_back(ais);
  if(state->targets.empty() && state->stale.empty()) return;
  auto update=[this](const Handle(AIS_Shape)& ais,const std::shared_ptr<BodyPrs>& prs) {
    const bool selected=m_ctx->IsSelected(ais);
    Graphic3d_ZLayerId rest=Graphic3d_ZLayerId_Default;  // where its look puts it (UI-121); selected: Topmost, the X-ray, last
    if(const auto node=m_nodeOf.find(ais.get());node!=m_nodeOf.end()) if(const auto item=m_items.find(node->second);item!=m_items.end()) rest=item->second.look.layer;
    const auto want=selected?Graphic3d_ZLayerId_Topmost:rest;
    if(ais->ZLayer()!=want) m_ctx->SetZLayer(ais,want);
    if(!selected || !prs) return;
    // A body a feature preview stands in for (moved, joined, cut) shows no glow where it was: it read as a copy left behind.
    if(const auto node=m_nodeOf.find(ais.get());node!=m_nodeOf.end() && m_previewHidden.count(node->second)) return;
    auto& glow=m_bodyGlows[ais.get()];
    if(glow.IsNull()) {
      // The arrays the body is drawn with: a zoom-refined body with a glow on its coarser base mesh z-fought with it
      // on curved faces (dark blotches all over a selected loft).
      std::shared_ptr<const BodyPrs> shown=prs;
      if(const auto body=Handle(BodyShape)::DownCast(ais);!body.IsNull() && body->displayPrs() && !body->displayPrs()->triangles.IsNull()) shown=body->displayPrs();
      // Faces: a tint and thin boundaries, outlined when the body is the selection's colour. Only curves (a sketch, a drawing
      // layer): drawn as picked edges, thick in a halo; outlined they would read white, the hover's colour. A picture: its
      // outline only, never a tint over it.
      const bool faces=!shown->triangles.IsNull();
      const auto node=m_nodeOf.find(ais.get());
      const opad::Node* body=node==m_nodeOf.end()?nullptr:m_doc->scene.node(node->second);
      glow=new SubHighlight(glowStyle(faces?shownColor(node==m_nodeOf.end()?std::string():node->second):QColor(),faces));
      if(faces && !(body && opad::is_canvas(*body))) glow->m_triangles.push_back(shown->triangles);
      if(!shown->boundaries.IsNull()) glow->m_segments.push_back(shown->boundaries);
      if(!shown->loosePoints.IsNull()) glow->m_points.push_back(shown->loosePoints);
      glow->SetZLayer(Graphic3d_ZLayerId_Topmost);
      glow->SetClipPlanes(ais->ClipPlanes());
      m_ctx->Display(glow,0,-1,false);
      if(m_side) if(const auto node=m_nodeOf.find(ais.get());node!=m_nodeOf.end()) maskSide(node->second,glow);
    }
    glow->SetLocalTransformation(ais->Transformation());
  };
  auto step=[this,state,update](Job*) {
    if(state->removed<state->stale.size()) {
      auto it=m_bodyGlows.find(state->stale[state->removed++]);
      if(it!=m_bodyGlows.end()) {m_ctx->Remove(it->second,false);m_bodyGlows.erase(it);}
      return true;
    }
    if(state->i>=state->targets.size()) return false;
    const auto& [ais,prs]=state->targets[state->i++];update(ais,prs);return true;
  };
  if(state->targets.size()+state->stale.size()<=64) {while(step(nullptr)) {} return;}
  m_bodyGlowJob=m_jobs->sliced(tr("Highlighting %1 selected").arg(state->keep.size()),[step](Job& job){return step(&job);},[this](bool){m_bodyGlowJob=nullptr;redrawScene();},JobKind::Background);
}

// One translucent box per selected node, covering its bodies; a stand-in for per-object highlighting.
void Viewport::showShade(const std::vector<std::string>& ids) {
  clearShade();
  for (const auto& id : ids) {
    Bnd_Box box;
    for (const auto& b : m_doc->scene.bodies_under(id)) {
      if (!m_items.count(b)) continue;
      m_shadeBodies.push_back(b);
      try {
        box.Add(opad::node_world_bbox(m_doc->doc, m_doc->scene, b));
      } catch (const std::exception&) {
      }
    }
    if (box.IsVoid()) continue;
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    const double diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
    const double pad = std::max(1e-3, diag * 0.002);
    Handle(AIS_Shape) s = new AIS_Shape(BRepPrimAPI_MakeBox(gp_Pnt(x0 - pad, y0 - pad, z0 - pad), gp_Pnt(x1 + pad, y1 + pad, z1 + pad)).Shape());
    s->SetColor(occ(m_tokens.selected3d));
    s->SetTransparency(0.7f);
    s->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
    s->Attributes()->SetFaceBoundaryDraw(Standard_True);
    s->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.selected3d), Aspect_TOL_SOLID, 2.5));
    s->SetZLayer(Graphic3d_ZLayerId_Topmost);  // same X-ray treatment as per-object highlights
    m_ctx->Display(s, AIS_Shaded, -1, Standard_False);  // selection mode -1: never pickable
    m_shade.push_back(s);
  }
}

void Viewport::clearShade() {
  for (const auto& s : m_shade) m_ctx->Remove(s, Standard_False);
  m_shade.clear();
  m_shadeBodies.clear();
}

void Viewport::clearSelection() {
  if (!m_initialised) return;
  clearCenters();
  m_notifyWhenApplied = true;  // selectionChanged fires once the (possibly sliced) un-highlight has settled
  selectNodes({});
}

// A click in the 3D view: OCCT has already changed the context selection; drop any stand-ins and in-flight job.
void Viewport::handleMoveTo(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) {
  QElapsedTimer clock;
  clock.start();
  AIS_ViewController::handleMoveTo(ctx, view);
  if (trace::enabled() && clock.elapsed() > 50) trace::log(QStringLiteral("slow frame part: picking/hover highlight %1 ms").arg(clock.elapsed()));
}

void Viewport::handleViewRedraw(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) {
  QElapsedTimer clock;
  clock.start();
  refreshMeasurement();
  noteCameraMoved();
  pruneTracking();
  scheduleRefinement();
  smallPartsCameraMoved();  // the small-part filter
  trackHoverFade();
  if (m_twoDimensional || m_sketchInput) updateInfiniteGrid(false);
  updateCubeSide();
  if (const auto camera = m_view->Camera()->WorldViewProjState(); camera != m_settleCamera) {  // moved: recorded once it rests
    m_settleCamera = camera;
    m_settleTimer.start();
  }
  degradeWhileNavigating();
  // Side by side: A's view is drawn after this one with its camera (the controller redraws it only when it is invalid).
  const bool full = m_side && m_view->IsInvalidated() && !m_sideView->IsInvalidated();
  QElapsedTimer draw;
  draw.start();
  AIS_ViewController::handleViewRedraw(ctx, view);
  if (m_side) drawSide(full);
  if (!m_degraded && draw.elapsed() >= 2) m_fullFrameMs = draw.elapsed();  // a hover alone redraws in under 2 ms
  if (trace::enabled() && clock.elapsed() > 50) trace::log(QStringLiteral("slow frame part: redraw %1 ms").arg(clock.elapsed()));
}

// The cube side the view looks straight at, drawn as selected (UI-38): only when it changes, a recompute of the cube alone.
void Viewport::updateCubeSide() {
  if (m_twoDimensional) return;
  const gp_Dir toEye = m_view->Camera()->Direction().Reversed();
  int side = -1;
  for (const V3d_TypeOfOrientation o : {V3d_Xpos, V3d_Ypos, V3d_Zpos, V3d_Xneg, V3d_Yneg, V3d_Zneg})
    if (V3d::GetProjAxis(o).IsEqual(toEye, 1e-4)) side = o;
  if (Handle(NavCube)::DownCast(m_cube)->setCurrentSide(side)) m_ctx->RecomputePrsOnly(m_cube, Standard_False);
}

void Viewport::OnSelectionChanged(const Handle(AIS_InteractiveContext)&, const Handle(V3d_View)&) {
  resetHoverFade();
  // The controller reports every click, also one on the view cube: that turns the camera and selects nothing,
  // and telling the listeners would run a guided tool's measure again on the same picks.
  if (m_cubeClick) {
    m_cubeClick = false;
    if (trace::enabled()) trace::log(QStringLiteral("3D click: on the view cube, %1 stay selected").arg(m_ctx->NbSelected()));
    return;
  }
  // Arc discovery targets must never become edge picks through a rubber band, and a face standing in front (UI-31) is
  // never a pick at all.
  std::vector<Handle(SelectMgr_EntityOwner)> discovery, occluders;
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected())
    if (!Handle(CircleOwner)::DownCast(m_ctx->SelectedOwner()).IsNull()) discovery.push_back(m_ctx->SelectedOwner());
    else if (!Handle(OccluderOwner)::DownCast(m_ctx->SelectedOwner()).IsNull()) occluders.push_back(m_ctx->SelectedOwner());
  for (const auto& owner : occluders) m_ctx->AddOrRemoveSelected(owner, false);
  for (const auto& owner : discovery) {
    const auto circle=Handle(CircleOwner)::DownCast(owner);
    auto node=m_nodeOf.find(Handle(AIS_InteractiveObject)::DownCast(owner->Selectable()).get());
    m_ctx->AddOrRemoveSelected(owner,false);
    if(node!=m_nodeOf.end()) {
      opad::Ref ref; ref.kind=opad::Ref::Kind::Center; ref.body=node->second; ref.index=circle->index();
      const gp_Pnt at=circle->center.Transformed(owner->Selectable()->Transformation());
      m_ctx->AddOrRemoveSelected(centerMarker(ref,at),false);
    }
  }
  if (m_selJob) m_selJob->cancel();
  gp_Pnt p;
  m_hasLastPick = detectedPoint(p);  // guided tools mark where the click landed
  if (m_hasLastPick) {
    const auto circle=Handle(CircleOwner)::DownCast(m_ctx->DetectedOwner());
    if(!circle.IsNull()) p=circle->center.Transformed(m_ctx->DetectedInteractive()->Transformation());
    m_lastPick = {p.X(), p.Y(), p.Z()};
    auto center = m_centerObjects.find(m_ctx->DetectedInteractive().get());
    if (center != m_centerObjects.end()) {
      const gp_Pnt& exact = m_centers.at(center->second).point;
      m_lastPick = {exact.X(), exact.Y(), exact.Z()};
      m_centers.at(center->second).ais->GlobalSelOwner()->SetPriority(5);
      m_centerLocked = false;  // the selected marker persists; discover the next circle
      m_activeCenter.clear();
    }
  }
  for (auto it = m_centers.begin(); it != m_centers.end();) {
    if (it->first == m_activeCenter || m_ctx->IsSelected(it->second.ais)) { ++it; continue; }
    m_centerObjects.erase(it->second.ais.get());
    m_ctx->Remove(it->second.ais, false);
    it = m_centers.erase(it);
  }
  if(!m_snapClick.empty() && m_centers.count(m_snapClick)) { m_lastPick=m_centers.at(m_snapClick).ref.point; m_hasLastPick=true; }
  refreshCenterStyles();
  clearShade();
  m_needFit = false;
  applySelectionLayers();
  refreshSubHighlight();
  redrawScene();
  if (trace::enabled()) trace::log(QStringLiteral("3D click: %1 selected in context").arg(m_ctx->NbSelected()));
  emit selectionChanged();
}

void Viewport::isolate(const std::vector<std::string>& ids, bool fit) {
  if (ids.empty() && m_isolated.empty()) return;  // not isolated (every document replacement asks): no full sync
  m_isolated.clear();
  for (const auto& id : ids)
    {if(m_doc->scene.sketch(id))m_isolated.insert(id);for (const auto& b : m_doc->scene.bodies_under(id)) m_isolated.insert(b);}
  m_needFit = fit && !m_isolated.empty();
  sync();
  if (fit && !m_isolated.empty()) fitAll();
  emit isolationChanged();
}

// ---------------------------------------------------------------- camera (F17/F18)
// What Fit frames: the model as drawn (bodies, sketches and their images, a feature preview, finite overlays such as a
// drawing being placed), never the grid, gizmos or annotations (infinite). V3d_View::FitAll(margin) boxes every
// structure in the view, and OCCT 7.9 adds the centre of any *infinite* structure more than 500 m across
// (Graphic3d_Layer::BoundingBox, centerOfinfiniteBndBox). The grid is one, and it used to reach from the world origin to
// the farthest coordinate: a house 600 m out (an OBJ keeping its site coordinates) was framed together with (0,0,0), 9x
// too small, in the corner under the cube.
Bnd_Box Viewport::fitBounds(bool fallback) const {
  Bnd_Box bounds;
  auto add = [&](const Handle(AIS_InteractiveObject)& object) {
    if (object.IsNull() || !m_ctx->IsDisplayed(object)) return;  // erased under a feature preview: the preview counts
    Bnd_Box box;
    object->BoundingBox(box);
    bounds.Add(box);
  };
  for (const auto& [id, item] : m_items) add(item.ais);
  for (const auto& [id, wire] : m_sketchWires) {
    add(wire.ais);
    for (const auto& image : wire.backdrops) add(image);
  }
  for (const auto& preview : m_previewBodies) add(preview);
  for (const auto& [id, part] : m_compareParts) add(part);  // null: not drawable
  for (const auto& overlay : m_overlays) if (!overlay->IsInfinite() && overlay->TransformPersistence().IsNull()) add(overlay);
  if (fallback && bounds.IsVoid()) {  // nothing to frame: the default grid, as Home does
    const double extent = std::max(1.0, m_gridExtentSetting);
    bounds.Add(gp_Pnt(-extent, -extent, 0));
    bounds.Add(gp_Pnt(extent, extent, 0));
  }
  return bounds;
}

void Viewport::fitAll(bool animate) {
  if (!m_initialised) return;
  moveCamera(animate, 0.35, [this] {
    m_view->FitAll(fitBounds(), 0.02, Standard_False);
    // A flat wire can make OCCT put an orthographic eye exactly on its target.
    // Keep a usable picking ray without changing the fitted on-screen scale.
    const auto camera=m_view->Camera();
    if(camera->IsOrthographic() && camera->Distance()<1.0){camera->SetDistance(std::max(1.0,camera->Scale()));m_view->ZFitAll();}
  });
}

void Viewport::animateFitAll(double seconds) {
  if (!m_initialised) return;
  myViewAnimation->Stop();
  m_needFit = false;
  Handle(Graphic3d_Camera) start = new Graphic3d_Camera(*m_view->Camera());
  fitAll();
  Handle(Graphic3d_Camera) end = new Graphic3d_Camera(*m_view->Camera());
  m_view->Camera()->Copy(start);
  myViewAnimation->SetView(m_view);
  myViewAnimation->SetCameraStart(start);
  myViewAnimation->SetCameraEnd(end);
  myViewAnimation->SetOwnDuration(seconds);
  myViewAnimation->StartTimer(0.0, 1.0, Standard_True);
  m_glide = true;  // the 16 ms timer moves the camera
  requestRedraw();
}

bool Viewport::cameraMoving() const { return !myViewAnimation.IsNull() && !myViewAnimation->IsStopped(); }

bool Viewport::showsAll() const {
  const Bnd_Box box = fitBounds(false);
  if (!m_initialised || box.IsVoid()) return true;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  for (int c = 0; c < 8; ++c)
    if (!rect().contains(widgetPoint({(c & 1) ? x1 : x0, (c & 2) ? y1 : y0, (c & 4) ? z1 : z0}))) return false;
  return true;
}

void Viewport::fitWhenReady() {
  m_fitNodesOnSync.clear();
  m_needFit = true;
  if (!m_items.empty()) fitAll();
}

void Viewport::fitNodesWhenReady(std::vector<std::string> ids) {
  m_fitNodesOnSync=std::move(ids);m_needFit=true;requestSync();
}

void Viewport::cancelMeshing() {
  *m_meshCancel = true;
  m_meshCancel = std::make_shared<std::atomic<bool>>(false);
  // What was still to come stays out (UI-40), until resetMeshing: the bodies waiting for a mesh and those queued for display.
  std::lock_guard<std::mutex> lock(m_meshMu);
  for (const auto& [key, nodes] : m_waiting) m_meshSkipped.insert(key);
  for (const auto& id : m_displayQueue)
    if (const opad::Node* n = m_doc->scene.node(id)) m_meshSkipped.insert(n->body_key);
  m_waiting.clear();
  m_waitingNodes = 0;
  m_displayQueue.clear();
}

void Viewport::resetMeshing() {
  std::lock_guard<std::mutex> lock(m_meshMu);
  m_meshSkipped.clear();
}

void Viewport::fitNodes(const std::vector<std::string>& ids, bool animate) {
  if (!m_initialised) return;
  m_needFit = false;
  Bnd_Box box;
  for (const auto& id : ids) {
    if(m_doc->scene.sketch(id)) box.Add(opad::node_world_bbox(m_doc->doc,m_doc->scene,id));
    for (const auto& b : m_doc->scene.bodies_under(id))
      if (m_items.count(b)) box.Add(opad::node_world_bbox(m_doc->doc, m_doc->scene, b));
  }
  if (trace::enabled()) { double a, b, c, d, e, f; if (!box.IsVoid()) box.Get(a, b, c, d, e, f); trace::log(QStringLiteral("fitNodes: box void=%1 [%2 %3 %4]-[%5 %6 %7]").arg(box.IsVoid()).arg(a).arg(b).arg(c).arg(d).arg(e).arg(f)); }
  if (box.IsVoid()) return fitAll(animate);
  moveCamera(animate, 0.35, [this, &box] { m_view->FitAll(box, 0.02, Standard_False); });
}

void Viewport::fitSelection(bool animate) {
  if (!m_initialised) return;
  m_needFit = false;
  Bnd_Box box;
  for (const auto& b : m_shadeBodies) box.Add(opad::node_world_bbox(m_doc->doc, m_doc->scene, b));
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) {
    Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->SelectedOwner());
    auto it = m_nodeOf.find(m_ctx->SelectedInteractive().get());
    if(const auto sub=Handle(SubShapeOwner)::DownCast(owner);!sub.IsNull()) sub->prepare();
    if (!owner.IsNull() && owner->HasShape() && owner->ComesFromDecomposition() && m_filter != SelFilter::Body) {  // not a drawing layer picked whole
      TopoDS_Shape sub = owner->Shape();
      Handle(AIS_InteractiveObject) obj = m_ctx->SelectedInteractive();
      if (!obj.IsNull() && obj->HasTransformation()) sub = sub.Moved(TopLoc_Location(obj->LocalTransformation()));
      BRepBndLib::Add(sub, box, Standard_True);
    }
    else if (it != m_nodeOf.end()) box.Add(opad::node_world_bbox(m_doc->doc, m_doc->scene, it->second));
  }
  if (trace::enabled()) { double a = 0, b = 0, c = 0, d = 0, e = 0, f = 0; if (!box.IsVoid()) box.Get(a, b, c, d, e, f); trace::log(QStringLiteral("fitSelection: box void=%1 [%2 %3 %4]-[%5 %6 %7]").arg(box.IsVoid()).arg(a).arg(b).arg(c).arg(d).arg(e).arg(f)); }
  if (box.IsVoid()) return fitAll(animate);
  moveCamera(animate, 0.35, [this, &box] { m_view->FitAll(box, 0.02, Standard_False); });
}

void Viewport::standardView(const QString& name, bool animate) {
  if (m_twoDimensional && name.startsWith("iso")) return;
  if (!m_initialised) return;
  m_needFit = false;
  myViewAnimation->Stop();
  ResetViewInput();myUI.Reset();myGL.Reset();
  V3d_TypeOfOrientation o = V3d_XposYnegZpos;
  if (name == "top") o = V3d_Zpos;
  else if (name == "bottom") o = V3d_Zneg;
  else if (name == "front") o = V3d_Yneg;
  else if (name == "back") o = V3d_Ypos;
  else if (name == "right") o = V3d_Xpos;
  else if (name == "left") o = V3d_Xneg;
  else if (name == "iso-back") o = V3d_XnegYposZpos;
  moveCamera(animate, 0.4, [this, o] {
    m_view->SetProj(o);
    fitAll();
  });
}

void Viewport::home(bool animate) {
  if(!m_initialised) return;
  myViewAnimation->Stop();m_needFit=false;
  moveCamera(animate,0.4,[this] {
    if(applyHomeCamera()) return;  // the document's own Home (UI-47), as it was set
    if(!m_twoDimensional) m_view->SetProj(V3d_XposYnegZpos);  // else the iso view, fitted; 2D mode keeps its plane
    const Bnd_Box bounds=fitBounds();
    const gp_Pnt center((bounds.CornerMin().XYZ()+bounds.CornerMax().XYZ())*.5);
    const auto camera=m_view->Camera();const gp_Vec shift(camera->Center(),center);
    camera->SetEyeAndCenter(camera->Eye().Translated(shift),center);
    m_view->FitAll(bounds,0.02,Standard_False);
  });
}
void Viewport::configureGrid(double spacing,double extent) {
  QSettings().setValue("view/gridSpacing",m_gridSpacing=std::max(0.0,spacing));QSettings().setValue("view/gridExtent",m_gridExtentSetting=std::max(1.0,extent));
  updateGridExtent();redrawScene();
}

void Viewport::rollView(double degrees) {
  if (!m_initialised) return;
  finishAnimation();  // a turn while one runs adds to where that one ends
  m_needFit = false;
  Handle(Graphic3d_Camera) cam = m_view->Camera();
  Handle(Graphic3d_Camera) end = new Graphic3d_Camera(*cam);
  gp_Dir up = cam->Up();
  up.Rotate(gp_Ax1(gp::Origin(), cam->Direction()), degrees * M_PI / 180.0);  // about the axis into the screen
  end->SetUp(up);
  animateCamera(end, 0.25);  // the 16 ms timer redraws while it runs; reduced motion (UI-124): the camera jumps
}

opad::json Viewport::cameraJson() const {
  opad::json j;
  if (!m_initialised) return j;
  const Handle(Graphic3d_Camera)& c = m_view->Camera();
  j["eye"] = {c->Eye().X(), c->Eye().Y(), c->Eye().Z()};
  j["target"] = {c->Center().X(), c->Center().Y(), c->Center().Z()};
  j["up"] = {c->Up().X(), c->Up().Y(), c->Up().Z()};
  j["absolute"] = true;
  j["projection"] = c->IsOrthographic() ? "orthographic" : "perspective";
  j["scale"] = c->Scale();
  j["fov_deg"] = c->FOVy();
  return j;
}

void Viewport::setCameraJson(const opad::json& j) {
  if (!m_initialised) return;
  myViewAnimation->Stop();
  m_needFit = false;
  opad::Camera cam = opad::Camera::from_json(j);
  Handle(Graphic3d_Camera) c = m_view->Camera();
  c->SetFOVy(cam.fov_deg);
  c->SetProjectionType(cam.perspective ? Graphic3d_Camera::Projection_Perspective : Graphic3d_Camera::Projection_Orthographic);
  if (cam.absolute) {
    c->SetEye(gp_Pnt(cam.eye[0], cam.eye[1], cam.eye[2]));
    c->SetCenter(gp_Pnt(cam.target[0], cam.target[1], cam.target[2]));
    c->SetUp(gp_Dir(cam.up[0], cam.up[1], cam.up[2]));
    if (cam.scale > 0) c->SetScale(cam.scale);
    m_view->Invalidate();
    requestRedraw();
  } else {
    gp_Dir d(cam.eye[0], cam.eye[1], cam.eye[2]);
    m_view->SetProj(d.X(), d.Y(), d.Z());
    m_view->SetUp(cam.up[0], cam.up[1], cam.up[2]);
    fitAll();
  }
}

QImage Viewport::grabImage() {
  if (!m_initialised) return QImage();
  if (m_twoDimensional || m_sketchInput) updateInfiniteGrid(false);  // the grid the next redraw lays out (a hidden view has none)
  Image_PixMap pix;
  V3d_ImageDumpOptions o;
  o.Width = static_cast<int>(width() * devicePixelRatioF());
  o.Height = static_cast<int>(height() * devicePixelRatioF());
  o.BufferType = Graphic3d_BT_RGB;
  o.ToAdjustAspect = Standard_True;
  if (!m_view->ToPixMap(pix, o)) return QImage();
  QImage img(static_cast<int>(pix.Width()), static_cast<int>(pix.Height()), QImage::Format_RGB888);
  for (int y = 0; y < img.height(); ++y) {
    uchar* row = img.scanLine(y);
    for (int x = 0; x < img.width(); ++x) {
      Quantity_ColorRGBA c = pix.PixelColor(x, y);
      row[x * 3] = static_cast<uchar>(c.GetRGB().Red() * 255);
      row[x * 3 + 1] = static_cast<uchar>(c.GetRGB().Green() * 255);
      row[x * 3 + 2] = static_cast<uchar>(c.GetRGB().Blue() * 255);
    }
  }
  return img;
}

std::vector<std::array<double, 3>> Viewport::drawnColors(const std::string& nodeId) const {
  std::vector<std::array<double, 3>> out;
  const auto it = m_items.find(nodeId);
  if (it == m_items.end()) return out;
  for (const auto& p : it->second.ais->Presentations()) {
    if (p->Mode() != AIS_Shaded) continue;
    for (const auto& g : p->Groups())
      if (const auto fill = Handle(Graphic3d_AspectFillArea3d)::DownCast(g->Aspects()); !fill.IsNull()) {
        double r, gr, b;
        fill->InteriorColor().Values(r, gr, b, Quantity_TOC_sRGB);
        out.push_back({r, gr, b});
      }
  }
  return out;
}

std::vector<double> Viewport::drawnTransparencies(const std::string& nodeId) const {
  std::vector<double> out;
  if (const auto it = m_items.find(nodeId); it != m_items.end())
    for (const auto& p : it->second.ais->Presentations())
      if (p->Mode() == AIS_Shaded)
        for (const auto& g : p->Groups())
          if (const auto fill = Handle(Graphic3d_AspectFillArea3d)::DownCast(g->Aspects()); !fill.IsNull()) out.push_back(fill->FrontMaterial().Transparency());
  return out;
}

// ---------------------------------------------------------------- section (F20)
void Viewport::setSection(bool enabled, const opad::Vec3& origin, const opad::Vec3& normal, bool caps) {
  m_sectionEnabled = enabled;
  m_sectionOrigin = origin;
  m_sectionNormal = normal;
  m_sectionCaps = caps;
  updateClipPlanes();
  updateSectionGizmo();
  redrawScene();
}

void Viewport::updateClipPlanes() {
  if (!m_initialised) return;
  if (!m_sectionPlane.IsNull()) m_view->RemoveClipPlane(m_sectionPlane);
  m_sectionPlane.Nullify();
  if (!m_sectionEnabled) {
    m_view->Invalidate();
    return;
  }
  double len = std::sqrt(m_sectionNormal[0] * m_sectionNormal[0] + m_sectionNormal[1] * m_sectionNormal[1] + m_sectionNormal[2] * m_sectionNormal[2]);
  if (len < 1e-9) return;
  m_sectionPlane = new Graphic3d_ClipPlane(gp_Pln(gp_Pnt(m_sectionOrigin[0], m_sectionOrigin[1], m_sectionOrigin[2]),
                                                  gp_Dir(m_sectionNormal[0], m_sectionNormal[1], m_sectionNormal[2])));
  m_sectionPlane->SetCapping(m_sectionCaps);
  m_sectionPlane->SetUseObjectMaterial(Standard_False);
  m_sectionPlane->SetCappingColor(occ(m_tokens.cap));
  m_sectionPlane->SetCappingHatchOff();
  m_view->AddClipPlane(m_sectionPlane);
  m_view->Invalidate();
}

// ---------------------------------------------------------------- dimension (F23)
// ---------------------------------------------------------------- guided-tool picking
void Viewport::setPickAccumulate(bool on, bool retainPicks) {
  if(m_pickAccumulate!=on || m_retainToolPicks!=(on && retainPicks))resetHoverFade();
  const bool references = ghostsPickable();
  m_pickAccumulate = on;
  referencesChanged(references);  // a tool's or a feature input's picks may be on ghosts (UI-33)
  m_retainToolPicks = on && retainPicks;
  ChangeMouseSelectionSchemes().Bind(Aspect_VKeyMouse_LeftButton, on ? AIS_SelectionScheme_XOR : AIS_SelectionScheme_Replace);
  if (!on) {
    m_measureSelectionLocked = false;
    showPickMarkers({});
    clearPreview();
  }
}

void Viewport::deselectLast() {
  if (!m_initialised) return;
  Handle(SelectMgr_EntityOwner) last;
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) last = m_ctx->SelectedOwner();
  if (last.IsNull()) return;
  m_ctx->AddOrRemoveSelected(last, Standard_False);
  OnSelectionChanged(m_ctx, m_view);
}

void Viewport::keepLastSelected() {
  if (!m_initialised) return;
  std::vector<Handle(SelectMgr_EntityOwner)> owners;
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) owners.push_back(m_ctx->SelectedOwner());
  if (owners.size() < 2) return;
  owners.pop_back();
  for (const auto& o : owners) m_ctx->AddOrRemoveSelected(o, Standard_False);
  OnSelectionChanged(m_ctx, m_view);
}

bool Viewport::lastPickPoint(opad::Vec3& p) const {
  p = m_lastPick;
  return m_hasLastPick;
}

void Viewport::showPickMarkers(const std::vector<opad::Vec3>& points) {
  if (!m_initialised) return;
  for (const auto& o : m_pickMarkers) m_ctx->Remove(o, Standard_False);
  m_pickMarkers.clear();
  int n = 0;
  for (const opad::Vec3& p : points) {
    Handle(AIS_TextLabel) mark = new AIS_TextLabel();
    mark->SetText(TCollection_ExtendedString(QString(" %1 ").arg(++n).toStdString().c_str(), Standard_True));
    mark->SetPosition(gp_Pnt(p[0], p[1], p[2]));
    mark->SetHeight(11);
    mark->SetColor(occ(m_tokens.onsel));
    mark->SetDisplayType(Aspect_TODT_SUBTITLE);
    mark->SetColorSubTitle(occ(m_tokens.sel));
    mark->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(mark, Standard_False);
    m_ctx->Deactivate(mark);
    m_pickMarkers.push_back(mark);
  }
  redrawScene();
}

void Viewport::clearPreview() {
  if (!m_initialised || m_preview.empty()) return;
  for (const auto& o : m_preview) m_ctx->Remove(o, Standard_False);
  m_preview.clear();
  redrawScene();
}

void Viewport::showPreview(const opad::Vec3& a, const opad::Vec3& b, const QString& label) {
  if (!m_initialised) return;
  for (const auto& o : m_preview) m_ctx->Remove(o, Standard_False);
  m_preview.clear();
  gp_Pnt pa(a[0], a[1], a[2]), pb(b[0], b[1], b[2]);
  if (pa.Distance(pb) > 1e-9) {
    Handle(AIS_Shape) line = new AIS_Shape(BRepBuilderAPI_MakeEdge(pa, pb).Edge());
    line->Attributes()->SetWireAspect(new Prs3d_LineAspect(occ(m_tokens.hov), Aspect_TOL_DASH, 1.5));
    line->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(line, Standard_False);
    m_ctx->Deactivate(line);
    m_preview.push_back(line);
    Handle(AIS_TextLabel) text = new AIS_TextLabel();
    text->SetText(TCollection_ExtendedString(label.toStdString().c_str(), Standard_True));
    text->SetPosition(gp_Pnt((pa.X() + pb.X()) / 2, (pa.Y() + pb.Y()) / 2, (pa.Z() + pb.Z()) / 2));
    text->SetHeight(12);
    text->SetColor(occ(m_tokens.hov));
    text->SetDisplayType(Aspect_TODT_SUBTITLE);
    text->SetColorSubTitle(occ(m_tokens.bg2));
    text->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(text, Standard_False);
    m_ctx->Deactivate(text);
    m_preview.push_back(text);
  }
  redrawScene();
}

// ---------------------------------------------------------------- scene sync
namespace {
double deflectionForBox(const Bnd_Box& box) {
  double d = 0.1;
  if (!box.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    double diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
    d = std::clamp(diag * 0.0008, 0.005, 1.0);
  }
  return d;
}

// A raster node's picture as a base64 data: URI, or null.
const std::string* rasterHref(const opad::Node& n) {
  if (!n.raster.is_object()) return nullptr;
  const auto it = n.raster.find("href");
  if (it == n.raster.end() || !it->is_string()) return nullptr;
  const std::string& href = it->get_ref<const std::string&>();
  const size_t comma = href.find(',');
  return href.rfind("data:image/", 0) == 0 && comma != std::string::npos && href.substr(0, comma).find(";base64") != std::string::npos ? &href : nullptr;
}

// A canvas's picture mirrored (its flags, opad/canvas.hpp): [left-right, upside down].
std::array<bool, 2> rasterFlip(const opad::Node& n) { return n.canvas.is_null() ? std::array<bool, 2>{false, false} : opad::CanvasFlags::of(n.canvas).flip; }

// Which picture a raster node shows and how it is fitted, without reading all of it (the scene is synced often and a
// picture is megabytes): its length, its first and last 4 KB, the corners, the fitting and a canvas's mirroring.
std::string rasterKey(const opad::Node& n) {
  const std::string* href = rasterHref(n);
  if (!href) return {};
  const std::string_view v(*href);
  const size_t edge = std::min<size_t>(v.size(), 4096);
  const auto flip = rasterFlip(n);
  return std::to_string(v.size()) + ":" + std::to_string(std::hash<std::string_view>{}(v.substr(0, edge))) + ":" +
         std::to_string(std::hash<std::string_view>{}(v.substr(v.size() - edge))) + "|" + n.raster.value("corners", opad::json()).dump() + "|" +
         n.raster.value("preserveAspectRatio", "") + (flip[0] ? "|h" : "") + (flip[1] ? "|v" : "");
}

// Worker: the texture of a raster node, fitted into its corners' proportions as SVG's preserveAspectRatio says (at most
// 4096 x 2048), else at most 8192 on its longer side; null when the picture cannot be decoded.
Handle(Image_PixMap) rasterPixels(const std::string& base64, const opad::json& raster) {
  const std::string aspect = raster.value("preserveAspectRatio", "");
  QImage image = decodePicture(QByteArray::fromBase64(QByteArray::fromStdString(base64)), aspect != "none" ? 4096 : 8192);
  if (image.isNull()) return {};
  if (const opad::json& flip = raster.value("flip", opad::json()); flip.is_array() && (flip[0] == true || flip[1] == true))
    image = image.flipped((flip[0] == true ? Qt::Horizontal : Qt::Orientations()) | (flip[1] == true ? Qt::Vertical : Qt::Orientations()));
  const auto& corners = raster.at("corners");
  auto point = [&](int i) { return gp_Pnt(corners[i][0].get<double>(), corners[i][1].get<double>(), corners[i][2].get<double>()); };
  const double ratio = point(0).Distance(point(1)) / std::max(1e-12, point(0).Distance(point(2)));
  if (aspect != "none") {
    const int h = int(std::clamp(std::max(double(image.height()), image.width() / ratio), 1.0, 2048.0));
    const int w = int(std::clamp(h * ratio, 1.0, 4096.0));
    QImage canvas(w, h, QImage::Format_RGBA8888);
    canvas.fill(Qt::transparent);
    const auto scaled = image.scaled(w, h, aspect.find("slice") != std::string::npos ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&canvas);
    painter.drawImage((w - scaled.width()) / 2, (h - scaled.height()) / 2, scaled);
    painter.end();
    image = canvas;
  }
  return texturePixels(image);
}
}  // namespace

void Viewport::decodeRaster(const opad::Node& n, const std::string& key) {
  if (!m_rasterDecoding.insert(key).second) return;
  const std::string& href = *rasterHref(n);
  auto data = std::make_shared<const std::string>(href.substr(href.find(',') + 1));  // the scene moves on meanwhile
  const auto flip = rasterFlip(n);
  auto fit = std::make_shared<const opad::json>(opad::json{{"corners", n.raster.at("corners")}, {"preserveAspectRatio", n.raster.value("preserveAspectRatio", "")}, {"flip", {flip[0], flip[1]}}});
  auto made = std::make_shared<Handle(Image_PixMap)>();
  QPointer<Viewport> guard(this);
  m_jobs->async(tr("Decoding pictures"), [data, fit, made](Progress progress) {
    if (!progress.cancelled()) *made = rasterPixels(*data, *fit);
  }, [this, guard, key, made](bool, const QString&) {
    if (!guard) return;
    m_rasterDecoding.erase(key);
    m_rasters[key] = *made;  // null: the frame is shown
    ++m_rastersDecoded;
    if (trace::enabled() && !made->IsNull()) trace::log(QStringLiteral("picture decoded on a worker: %1 x %2").arg((*made)->SizeX()).arg((*made)->SizeY()));
    requestSync();
  });
}

bool Viewport::showsPicture(const std::string& nodeId) const {
  const auto it = m_items.find(nodeId);
  return it != m_items.end() && !Handle(AIS_TexturedShape)::DownCast(it->second.ais).IsNull();
}

double Viewport::deflectionFor(const std::string& key) { return deflectionForBox(opad::body_bbox(m_doc->doc, key)); }

// Tessellation runs off the UI thread (F21); bodies appear once their mesh is ready.
void Viewport::startMeshing(std::vector<std::string> keys) {
  struct MeshJob { TopoDS_Shape shape; std::string key; std::shared_ptr<const opad::FaceColors> colors; bool drawing; };
  std::vector<MeshJob> jobs;
  {
    std::lock_guard<std::mutex> lock(m_meshMu);
    for (const auto& k : keys) {
      if (m_meshed.count(k) || m_meshing.count(k) || m_meshSkipped.count(k)) continue;
      m_meshing.insert(k);
      auto colors = std::make_shared<const opad::FaceColors>(opad::face_colors(m_doc->doc, k));
      const auto waiting = m_waiting.find(k);  // a drawing layer's edges are picked in groups (UI-42)
      const opad::Node* n = waiting == m_waiting.end() || waiting->second.empty() ? nullptr : m_doc->scene.node(waiting->second.front());
      jobs.push_back({opad::body_shape(m_doc->doc, k), k, colors->empty() ? nullptr : colors, n && n->representation == "drawing2d"});
    }
  }
  if (jobs.empty()) return;
  auto alive = m_alive;
  auto cancel = m_meshCancel;
  auto cache = m_doc->doc.shape_cache;  // the worker fills the bbox cache too, so later UI queries are O(1)
  // Bodies side by side on several workers (each also meshes its own faces in parallel): one after another, an
  // assembly of many small parts took seconds to appear while most cores idled. Bodies share no sub-shapes, so
  // meshing them at once writes to separate topology.
  auto queue = std::make_shared<const std::vector<MeshJob>>(std::move(jobs));
  auto next = std::make_shared<std::atomic<size_t>>(0);
  const int workers = std::min(static_cast<int>(queue->size()), std::clamp(QThread::idealThreadCount() - 1, 1, 8));
  for (int w = 0; w < workers; ++w) {
   QThread* worker = QThread::create([this, alive, cancel, cache, queue, next]() {
    for (size_t i; (i = (*next)++) < queue->size();) {
      const auto& j = (*queue)[i];
      if (*cancel) {
        if (!*alive) return;
        std::lock_guard<std::mutex> lock(m_meshMu);
        m_meshing.erase(j.key);
        if (m_activeCache == cache.get()) {
          m_meshSkipped.insert(j.key);
          handOver(j.key);
        }
        continue;
      }
      std::shared_ptr<BodyPrs> prs;
      try {
        const Bnd_Box box = opad::body_bbox(*cache, j.key, j.shape);
        const auto mesh = BodyPrs::meshForDisplay(j.shape, deflectionForBox(box));
        if (mesh.status || mesh.recovered_faces || mesh.incomplete_cones)
          trace::log(QString("mesh %1: status=%2 recovered=%3 incomplete cones=%4").arg(QString::fromStdString(j.key)).arg(mesh.status).arg(mesh.recovered_faces).arg(mesh.incomplete_cones));
        // The box from before the mesh is only good for the deflection: it follows the surfaces' poles, and one
        // small body with a 10 m box zoomed Fit All out of the whole Engine. The presentation gets the mesh's box.
        prs = BodyPrs::build(j.shape, opad::refine_body_bbox(*cache, j.key, j.shape), false, j.drawing, j.colors);  // so Display() on the UI thread is cheap
        prs->deflection = deflectionForBox(box);
      } catch (...) {
      }
      if (!*alive) return;
      std::lock_guard<std::mutex> lock(m_meshMu);
      m_meshing.erase(j.key);
      if (m_activeCache == cache.get()) {  // a newer document owns the bookkeeping otherwise
        ++m_meshCount;
        m_meshed.insert(j.key);
        if (prs) m_prs[j.key] = std::move(prs);
        handOver(j.key);
      }
    }
   });
   connect(worker, &QThread::finished, worker, &QObject::deleteLater);
   worker->start(QThread::LowPriority);  // the UI thread stays first in line
  }
}

void Viewport::benchClick(double fx, double fy) {
  if (!m_initialised) return;
  m_view->Redraw();  // see benchPick: the picker needs a frame after a camera change
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  // Spiral out from the asked-for spot to the nearest one over a body entity that is not picked yet: a big
  // assembly has holes, and a second click on the same face would un-pick it.
  Graphic3d_Vec2i pt(static_cast<int>(w * fx), static_cast<int>(h * fy));
  for (int ring = 0, found = 0; ring < 30 && !found; ++ring)
    for (int k = 0; k < (ring == 0 ? 1 : 16) && !found; ++k) {
      const double ang = k * 0.39269908169872414;
      const Graphic3d_Vec2i q(pt.x() + static_cast<int>(ring * 0.02 * w * std::cos(ang)), pt.y() + static_cast<int>(ring * 0.02 * h * std::sin(ang)));
      if (q.x() < 0 || q.y() < 0 || q.x() >= w || q.y() >= h) continue;
      m_ctx->MoveTo(q.x(), q.y(), m_view, Standard_False);
      if (m_ctx->HasDetected() && m_nodeOf.count(m_ctx->DetectedInteractive().get()) && !m_ctx->IsSelected(m_ctx->DetectedOwner())) {
        pt = q;
        found = 1;
      }
    }
  // Through the widget's own mouse handlers and paint path, as real input arrives: move, press, release, then
  // the pointer wanders over neighbouring entities (hover labels, the tool's preview line).
  const QPointF scale = viewScale();
  auto send = [this, scale](QEvent::Type type, const Graphic3d_Vec2i& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
    const QPointF local(at.x() / scale.x(), at.y() / scale.y());
    QCoreApplication::postEvent(this, new QMouseEvent(type, local, mapToGlobal(local), button, buttons, Qt::NoModifier));
  };
  send(QEvent::MouseMove, pt, Qt::NoButton, Qt::NoButton);
  send(QEvent::MouseButtonPress, pt, Qt::LeftButton, Qt::LeftButton);
  send(QEvent::MouseButtonRelease, pt, Qt::LeftButton, Qt::NoButton);
  for (int i = 1; i <= 160; ++i)  // ~8 s of hovering, back and forth across the neighbourhood: past a long body-to-body measure
    QTimer::singleShot(50 * i, this, [send, pt, i, w] { send(QEvent::MouseMove, Graphic3d_Vec2i(pt.x() + ((i % 20) - 10) * w / 50, pt.y() + (i % 5) * 9), Qt::NoButton, Qt::NoButton); });
  trace::log(QStringLiteral("bench: mouse click posted at %1,%2").arg(pt.x()).arg(pt.y()));
}

void Viewport::benchFlush() {
  if (!m_initialised || m_flushingViewEvents) return;
  m_view->Redraw();  // see benchPick: the picker needs a frame after a camera change
  QScopedValueRollback<bool> flushing(m_flushingViewEvents, true);
  FlushViewEvents(m_ctx, m_view, Standard_True);
}

void Viewport::benchDoubleClickAt(const QPointF& at, Qt::KeyboardModifiers modifiers) {
  if (!m_initialised) return;
  m_view->Redraw();
  using Step = std::pair<QEvent::Type, Qt::MouseButtons>;
  for (const auto& [type, buttons] : {Step{QEvent::MouseButtonPress, Qt::LeftButton}, Step{QEvent::MouseButtonRelease, Qt::NoButton},
                                      Step{QEvent::MouseButtonDblClick, Qt::LeftButton}, Step{QEvent::MouseButtonRelease, Qt::NoButton}}) {
    QMouseEvent e(type, at, mapToGlobal(at), Qt::LeftButton, buttons, modifiers);
    QCoreApplication::sendEvent(this, &e);
    paintEvent(nullptr);
  }
}

void Viewport::benchClickAt(const QPointF& at, Qt::KeyboardModifiers modifiers) {
  if (!m_initialised) return;
  m_view->Redraw();  // the picker needs a frame after a camera change
  auto send = [&](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers held) {
    QMouseEvent e(type, at, mapToGlobal(at), button, buttons, held);
    QCoreApplication::sendEvent(this, &e);
    paintEvent(nullptr);
  };
  send(QEvent::MouseMove, Qt::NoButton, Qt::NoButton, modifiers);
  send(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, modifiers);
  send(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, modifiers);
  send(QEvent::MouseMove, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
}

void Viewport::clearHover() {
  m_hoverOwner = nullptr;  // the next frame labels what is under the pointer again
  if (m_hover.isEmpty()) return;
  m_hover.clear();
  emit hoverChanged(m_hover);
}

void Viewport::benchPick() {
  if (!m_initialised) return;
  m_view->Redraw();  // a frame first: the picker clips to the camera z range, which only Redraw (AutoZFit) updates
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  QElapsedTimer clock;
  clock.start();
  m_ctx->MoveTo(w / 2, h / 2, m_view, Standard_False);
  const qint64 firstMs = clock.restart();  // the first pick after a mode switch also builds the picking BVHs
  m_ctx->MoveTo(w / 2 + 3, h / 2 + 3, m_view, Standard_False);
  const qint64 nextMs = clock.restart();
  QString hit = QStringLiteral("nothing");
  if (m_ctx->HasDetected()) {
    auto it = m_nodeOf.find(m_ctx->DetectedInteractive().get());
    hit = it == m_nodeOf.end() ? QStringLiteral("non-body") : m_doc->nodeName(it->second);
  }
  m_ctx->ClearSelected(Standard_False);
  const Graphic3d_Vec2i pt(w / 2, h / 2);  // a click as the mouse handlers deliver it
  UpdateMousePosition(pt, Aspect_VKeyMouse_NONE, Aspect_VKeyFlags_NONE, false);
  UpdateMouseButtons(pt, Aspect_VKeyMouse_LeftButton, Aspect_VKeyFlags_NONE, false);
  UpdateMouseButtons(pt, Aspect_VKeyMouse_NONE, Aspect_VKeyFlags_NONE, false);
  FlushViewEvents(m_ctx, m_view, Standard_True);
  trace::log(QStringLiteral("bench: pick at view centre detected %1; click selected %2 (hover %3 ms, next hover %4 ms, click %5 ms)").arg(hit).arg(m_ctx->NbSelected()).arg(firstMs).arg(nextMs).arg(clock.elapsed()));
}

// Mesh worker, m_meshMu held: the key goes to the display pump, with one queued pumpMeshed() per batch.
void Viewport::handOver(const std::string& key) {
  m_newlyMeshed.push_back(key);
  if (std::exchange(m_pumpPosted, true)) return;
  QMetaObject::invokeMethod(this, [this] { pumpMeshed(); }, Qt::QueuedConnection);
}

// The keys the workers finished (or skipped, cancelled): the bodies that waited for them join the display queue.
void Viewport::pumpMeshed() {
  std::vector<std::string> keys;
  {
    std::lock_guard<std::mutex> lock(m_meshMu);
    keys.swap(m_newlyMeshed);
    m_pumpPosted = false;
    bool moved = false;
    for (const auto& key : keys) {
      auto it = m_waiting.find(key);
      if (it == m_waiting.end()) continue;
      m_waitingNodes -= it->second.size();
      if (m_meshed.count(key))
        for (auto& id : it->second) m_displayQueue.push_back(std::move(id));
      m_waiting.erase(it);
      moved = true;
    }
    if (!moved) return;
  }
  if (!m_displayQueue.empty()) emit meshingProgress(remainingBodies());
  runPump();
}

// One long-lived sliced job displays the queued bodies (UI-40). While the queue is dry and meshes are still on their way
// it pauses, and the next batch resumes it; it ends once nothing is queued or waiting. It is a child of the job that
// reports the stream (a load, Displaying bodies), else Background. Neither a job nor a queue: the stream has settled.
void Viewport::runPump() {
  if (m_displayJob) return m_jobs->resume(m_displayJob);
  if (m_displayQueue.empty() || !m_jobs) return streamSettled();
  ++m_pumpRuns;
  m_displayJob = m_jobs->sliced(tr("Displaying bodies"), [this](Job& job) {
    if (m_doc->loading) return false;
    if (m_displayQueue.empty()) {
      if (m_waitingNodes == 0) return false;
      streamSettled();
      job.pause();
      return true;
    }
    const std::string id = std::move(m_displayQueue.front());
    m_displayQueue.pop_front();
    const size_t before = m_items.size();
    displayBody(id);
    m_streamAdded = m_streamAdded || m_items.size() > before;
    if ((++m_pumpSteps & 15) == 0) {
      emit meshingProgress(remainingBodies());
      m_view->Invalidate();
      requestRedraw();  // bodies appear as they are added
    }
    return true;
  }, [this](bool completed) {
    m_displayJob = nullptr;
    if (!completed) m_displayQueue.clear();  // cancelled with the stream: what is meshed shows on the next change
    streamSettled();
  }, m_streamJob ? JobKind::Child : JobKind::Background, m_streamJob);
}

// The display queue ran dry: progress, the view fitted while a load streams in (at most every 250 ms), and once no body
// waits for its mesh either, what depends on the whole scene (finishSync: depth bias, grid, refinement, a fit of nodes).
void Viewport::streamSettled() {
  emit meshingProgress(remainingBodies());
  if (m_waitingNodes == 0) return finishSync(0, std::exchange(m_streamAdded, false));
  if (m_streamAdded && m_needFit && m_fitNodesOnSync.empty() && (!m_streamFit.isValid() || m_streamFit.elapsed() > 250)) {
    m_view->FitAll(fitBounds(), 0.02, Standard_False);
    m_streamFit.start();
  }
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::setStreamJob(Job* job) { m_streamJob = job; }

// Creates the OpenGL viewer ahead of the first document (0.2-0.8 s) so that opening a file does not pay for it. The native
// child window exists while hidden, which is all OCCT needs.
void Viewport::warmUp() {
  m_warmed = true;
  if (m_initialised) return;
  try {
    initViewer();
  } catch (const Standard_Failure&) {  // no context yet: paintEvent will try again once visible
  }
}

void Viewport::firstFrame() {
  if (m_initialised) m_view->Redraw();  // compiles the shaders (0.2-0.4 s); better here than when the document appears
}

void Viewport::renameBodyKeys(const std::map<std::string, std::string>& keys) {
  {
    std::lock_guard<std::mutex> lock(m_meshMu);
    for (auto& key : m_newlyMeshed)
      if (auto it = keys.find(key); it != keys.end()) key = it->second;
    for (const auto& [from, to] : keys) {
      if (auto it = m_waiting.find(from); it != m_waiting.end()) {
        m_waiting[to] = std::move(it->second);
        m_waiting.erase(from);
      }
      if (m_meshed.erase(from)) m_meshed.insert(to);
      if (m_meshSkipped.erase(from)) m_meshSkipped.insert(to);
      if (auto it = m_prs.find(from); it != m_prs.end()) {
        m_prs[to] = it->second;
        m_prs.erase(it);
      }
      if (auto it = m_refined.find(from); it != m_refined.end()) {
        m_refined[to] = it->second;
        m_refined.erase(it);
      }
    }
  }
  for (auto& [id, item] : m_items)
    if (auto it = keys.find(item.key); it != keys.end()) item.key = it->second;
}

void Viewport::requestSync() {
  if (!m_syncTimer.isActive()) m_syncTimer.start();
}

// Reconciles the context with the scene, on document changes only. Removals and attribute changes are applied at once
// (cheap); the bodies to display go to the display pump's queue (runPump: computing a body's presentation and selection
// entities is the expensive part and must not block the UI, see Jobs.hpp), those without a mesh wait for it (m_waiting)
// and join the queue as the mesh workers finish them (pumpMeshed), with no sync per batch (UI-40). A change that only
// re-applied some nodes' appearance, placement, name or parent (AppDocument::lastChange: a hide, its undo and redo) looks
// at the bodies under them alone; anything else, or a sync for another reason (isolation, picking), at every body.
void Viewport::sync() {
  if (!m_initialised || m_doc->loading) return;
  trace::Scope scope("Viewport::sync");
  QElapsedTimer clock;
  clock.start();
  const qint64 cpu = trace::threadCpuMs();
  const opad::Scene& scene = m_doc->scene;
  bool fresh = false;
  {
    // Mesh bookkeeping is per shape cache: a new document means new TopoDS_Shapes without triangulation.
    std::lock_guard<std::mutex> lock(m_meshMu);
    const void* cache = m_doc->doc.shape_cache.get();
    if (cache != m_activeCache) {
      fresh = true;
      m_activeCache = cache;
      m_meshed.clear();
      m_meshSkipped.clear();
      m_newlyMeshed.clear();
      // The last document's display arrays are freed on a worker (UI-41): the Engine's are hundreds of MB in many blocks.
      disposeLater(std::make_shared<std::pair<decltype(m_prs), decltype(m_refined)>>(std::move(m_prs), std::move(m_refined)));
      m_prs.clear();
      m_refined.clear();
      m_partSizes.clear();
    }
  }
  pruneSnapIndexes();
  const AppDocument::Change& change = m_doc->lastChange();
  const bool partial = !fresh && !change.whole && m_doc->revision == m_syncedRevision + 1;
  m_syncedRevision = m_doc->revision;
  if (!partial && !m_isolated.empty()) {  // the mode ends by itself once every isolated object is gone (deleted)
    bool any = false;
    for (const auto& id : m_isolated) {
      const opad::Node* n = scene.node(id);
      if ((n && !n->body_missing) || scene.sketch(id)) { any = true; break; }
    }
    if (!any) {
      m_isolated.clear();
      emit isolationChanged();
    }
  }
  std::vector<std::string> bodies, pending;
  if (partial) {
    std::unordered_set<std::string> seen;
    for (const auto& id : change.nodes)
      for (auto& body : scene.bodies_under(id))
        if (seen.insert(body).second) bodies.push_back(std::move(body));
    // What waited or was queued for these is decided again below.
    m_displayQueue.erase(std::remove_if(m_displayQueue.begin(), m_displayQueue.end(), [&seen](const std::string& id) { return seen.count(id) > 0; }),
                         m_displayQueue.end());
    for (const auto& id : bodies)
      if (const opad::Node* n = scene.node(id))
        if (auto w = m_waiting.find(n->body_key); w != m_waiting.end()) {
          auto& nodes = w->second;
          const size_t before = nodes.size();
          nodes.erase(std::remove(nodes.begin(), nodes.end(), id), nodes.end());
          m_waitingNodes -= before - nodes.size();
        }
  } else {
    bodies = scene.all_bodies();
    m_waiting.clear();
    m_waitingNodes = 0;
    m_displayQueue.clear();
  }
  std::set<std::string> rasters;  // the pictures the bodies looked at show (a whole sync forgets the others)
  bool recoloredSelected = false, moved = false;
  // One body: true when its displayed object stays as it is (attributes applied in place, or moved where it is); otherwise
  // it is queued for display, waits for its mesh or its picture, or is not shown, and an object it had goes.
  auto place = [&](const std::string& id) {
    const opad::Node* n = scene.node(id);
    if (!n || n->body_missing) return false;
    // Isolate mode shows exactly the isolated set and ignores visibility flags; otherwise the flags rule.
    if (!m_isolated.empty() ? !m_isolated.count(id) : !scene.effectively_visible(id)) return false;
    auto it = m_items.find(id);
    const std::string raster = rasterKey(*n);
    if (!raster.empty()) rasters.insert(raster);
    if (const opad::Mat4 world = scene.world(id); it != m_items.end() && it->second.key == n->body_key && it->second.raster == raster && it->second.rigid &&
                                                  it->second.world.m != world.m && relocate(it->second, *n, world)) {
      // Only moved (a transform op: a canvas let go, a part placed): its location, as a look's offset is; nothing is
      // displayed again, so it never blinks out for a frame.
      it->second.world = world;
      moved = true;
      ++m_relocateCount;
    }
    if (it != m_items.end() && it->second.key == n->body_key && it->second.world.m == scene.world(id).m && it->second.raster == raster) {
      Item& item = it->second;
      if (item.color != n->color || item.opacity != n->opacity || (!n->canvas.is_null() && !(composeLook(*n) == item.look))) {  // a canvas's flags too
        item.color = n->color;
        item.opacity = n->opacity;
        applyLook(id, item, composeLook(*n));  // the new appearance under the layers of looks (UI-121)
        // The presentation only: Redisplay also rebuilt the selection owners, which dropped the body from the selection
        // (a colour picked for the selection left it unselected, though the status bar still counted it).
        m_ctx->RecomputePrsOnly(item.ais, Standard_False);
        recoloredSelected = recoloredSelected || m_ctx->IsSelected(item.ais);
      } else if (n->representation == "drawing2d") {  // its layer's line weight or type may have changed (UI-89)
        if (const BodyLook look = composeLook(*n); !(look == item.look)) {
          applyLook(id, item, look);
          recoloredSelected = recoloredSelected || m_ctx->IsSelected(item.ais);
        }
      }
      return true;
    }
    bool meshed, skipped;
    {
      std::lock_guard<std::mutex> lock(m_meshMu);
      meshed = m_meshed.count(n->body_key) > 0;
      skipped = m_meshSkipped.count(n->body_key) > 0;
    }
    if (skipped) return false;  // its meshing (or display) was cancelled: reopening the file shows it
    if (!meshed) {
      auto& nodes = m_waiting[n->body_key];
      if (nodes.empty()) pending.push_back(n->body_key);
      nodes.push_back(id);
      ++m_waitingNodes;
      return false;
    }
    if (!raster.empty() && !m_rasters.count(raster)) {  // shown once its picture is decoded; what is shown until then stays
      decodeRaster(*n, raster);
      return it != m_items.end();
    }
    m_displayQueue.push_back(id);
    return false;
  };
  bool removed = false;
  auto drop = [&](std::map<std::string, Item>::iterator it) {
    if (!std::exchange(removed, true)) clearCenters();  // topology, placement or visibility changed: no stale source circles
    retire(it->second);
    m_nodeOf.erase(it->second.ais.get());
    return m_items.erase(it);
  };
  if (partial) {
    for (const auto& id : bodies)
      if (!place(id))
        if (auto it = m_items.find(id); it != m_items.end()) drop(it);
  } else {
    std::unordered_set<std::string> kept;
    for (const auto& id : bodies)
      if (place(id)) kept.insert(id);
    for (auto it = m_items.begin(); it != m_items.end();) it = kept.count(it->first) ? std::next(it) : drop(it);
  }
  if (!partial)
    for (auto it = m_rasters.begin(); it != m_rasters.end();) it = rasters.count(it->first) ? std::next(it) : m_rasters.erase(it);
  if (removed) removeRetired();
  if (moved) {
    clearCenters();  // found where the bodies were
    if (!m_subHl.IsNull() || m_subJob) refreshSubHighlight();
    if (!m_notes.empty()) {  // notes pinned to them follow
      m_noteCamera.Reset();
      QMetaObject::invokeMethod(this, [this] { emit notesMoved(); }, Qt::QueuedConnection);
    }
    recoloredSelected = true;  // the highlight follows the new location
  }
  if (recoloredSelected) m_ctx->HilightSelected(Standard_False);  // its highlight was on the old presentation
  if (removed && (!m_subHl.IsNull() || m_subJob)) refreshSubHighlight();  // the retired bodies' selected sub-shapes went with them
  if (!pending.empty()) startMeshing(pending);
  if (layered()) scheduleLooks();  // the hierarchy under a layer's components may have changed
  syncSketches(partial);
  applySelectionLayers();
  if (m_notesRevision != m_doc->revision) updateAnnotations();  // a document change, not a batch of meshes
  updateClipPlanes();
  ++(partial ? m_partialSyncs : m_syncs);
  m_syncMs += clock.elapsed();
  m_syncCpuMs += trace::threadCpuMs() - cpu;
  if (!m_displayQueue.empty()) emit meshingProgress(remainingBodies());  // first: the window may make a job to report the stream (the pump's parent)
  runPump();
  if (m_style == Style::HiddenEdges) scheduleEdgeOverlay();
}

// A displayed body that goes (UI-41): erased at once, which only hides it and turns its picking off, and removed from the
// context later by removeRetired. Removing frees its presentation and selection structures, ~2 ms a body on the Engine:
// one by one in sync, a new document waited 2.6 s for the old one's 1,295 bodies.
void Viewport::retire(const Item& item) {
  m_ctx->Erase(item.ais, Standard_False);
  if (!item.navigation.IsNull()) {
    m_navNodes.erase(item.navigation.get());
    m_navSelection->Deactivate(item.navigation);
  }
  m_retired.push_back({item.ais, item.navigation});
}

void Viewport::removeRetired() {
  if (m_retireJob || m_retired.empty() || !m_jobs) return;
  // What the objects alone still hold (their shapes with the meshes, their arrays and picking structures) is freed on a
  // worker, a batch at a time: freeing one big body took up to 400 ms here.
  struct Batch { std::vector<TopoDS_Shape> shapes; std::vector<std::shared_ptr<const BodyPrs>> prs; };
  auto batch = std::make_shared<std::shared_ptr<Batch>>(std::make_shared<Batch>());
  auto flush = [batch] {
    if ((*batch)->shapes.empty()) return;
    disposeLater(std::move(*batch));
    *batch = std::make_shared<Batch>();
  };
  m_retireJob = m_jobs->sliced(tr("Clearing the view"), [this, batch, flush](Job&) {
    if (m_retired.empty()) return false;
    Retired r = std::move(m_retired.front());
    m_retired.pop_front();
    if (!r.navigation.IsNull()) m_navSelection->Remove(r.navigation);
    m_ctx->Remove(r.ais, Standard_False);
    (*batch)->shapes.push_back(r.ais->Shape());
    if (const auto body = Handle(BodyShape)::DownCast(r.ais); !body.IsNull()) {
      (*batch)->prs.push_back(body->prs());
      (*batch)->prs.push_back(body->displayPrs());
    }
    r = {};
    if ((*batch)->shapes.size() >= 64) flush();
    return !m_retired.empty();
  }, [this, flush](bool) {
    flush();
    m_retireJob = nullptr;
    if (!m_retired.empty()) QTimer::singleShot(0, this, &Viewport::removeRetired);  // cancelled: the rest later
  }, JobKind::Background);
}

namespace {
// A canvas's picture rectangle `stretch` times as tall (free aspect), as image_io builds it (the texture spans its UV range):
// one face, meshed here (two triangles).
TopoDS_Shape stretchedCanvas(const opad::Node& n, double stretch) {
  double w = 0, h = 0;
  opad::canvas_body_size(n, w, h);
  const TopoDS_Face face = BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 0, w, 0, h * stretch).Face();
  BRepMesh_IncrementalMesh(face, std::max(w, h * stretch) / 50);
  return face;
}
}  // namespace

bool Viewport::rigidPlacement(const opad::Node& n, const opad::Mat4& world, gp_Trsf& placement, double& stretch) {
  placement = gp_Trsf();
  stretch = 1;
  if (world.is_identity()) return true;
  if (opad::mat_is_rigid(world)) return placement = opad::trsf_from_mat(world), true;
  if (!opad::is_canvas(n)) return false;
  // A stretched canvas: its axes square to each other, y (and z) scaled apart from x. Its similarity has them all as x.
  double len[3];
  for (int c = 0; c < 3; ++c) len[c] = std::sqrt(world.at(0, c) * world.at(0, c) + world.at(1, c) * world.at(1, c) + world.at(2, c) * world.at(2, c));
  if (!(len[0] > 1e-12) || !(len[1] > 1e-12) || !(len[2] > 1e-12)) return false;
  opad::Mat4 similar = world;
  for (int r = 0; r < 3; ++r) similar.at(r, 1) *= len[0] / len[1], similar.at(r, 2) *= len[0] / len[2];
  if (!opad::mat_is_rigid(similar)) return false;
  placement = opad::trsf_from_mat(similar);
  stretch = len[1] / len[0];
  return true;
}

bool Viewport::relocate(Item& item, const opad::Node& n, const opad::Mat4& world) {
  gp_Trsf placement;
  double stretch = 1;
  if (!item.rigid || !rigidPlacement(n, world, placement, stretch)) return false;
  if (std::fabs(stretch - item.stretch) > 1e-12 * stretch) {  // a canvas stretched otherwise: its rectangle at the new aspect, in place
    const auto textured = Handle(AIS_TexturedShape)::DownCast(item.ais);
    if (textured.IsNull()) return false;  // its picture is not shown (yet): displayed again
    item.located = stretch == 1 ? opad::body_shape(m_doc->doc, item.key) : stretchedCanvas(n, stretch);
    item.stretch = stretch;
    textured->SetShape(item.located);
    m_ctx->RecomputePrsOnly(textured, Standard_False);
    m_ctx->RecomputeSelectionOnly(textured);
    if (!item.navigation.IsNull()) {  // the orbit pivot's copy of the picture's own rectangle no longer fits it
      m_navNodes.erase(item.navigation.get());
      m_navSelection->Remove(item.navigation);
      item.navigation.Nullify();
    }
    if (const auto glow = m_bodyGlows.find(item.ais.get()); glow != m_bodyGlows.end()) {  // its outline: the next selection pass, at this aspect
      m_ctx->Remove(glow->second, Standard_False);
      m_bodyGlows.erase(glow);
    }
  }
  item.placement = placement;
  gp_Trsf placed;
  if (item.look.offset != std::array<double, 3>{0, 0, 0}) placed.SetTranslation(gp_Vec(item.look.offset[0], item.look.offset[1], item.look.offset[2]));
  placed.Multiply(placement);
  m_ctx->SetLocation(item.ais, placed.Form() == gp_Identity ? TopLoc_Location() : TopLoc_Location(placed));
  if (!item.navigation.IsNull()) {
    item.navigation->SetLocalTransformation(placed);
    m_navSelection->Update(item.navigation, Standard_False);
  }
  if (const auto glow = m_bodyGlows.find(item.ais.get()); glow != m_bodyGlows.end()) glow->second->SetLocalTransformation(item.ais->Transformation());
  return true;
}

// Adds one body to the context: presentation + selection entities are computed here.
void Viewport::displayBody(const std::string& id) {
  const opad::Scene& scene = m_doc->scene;
  const opad::Node* n = scene.node(id);
  if (!n || n->body_missing || m_items.count(id)) return;  // the scene moved on since this was queued
  QElapsedTimer t;
  t.start();
  const qint64 cpu = trace::threadCpuMs();
  opad::Mat4 world = scene.world(id);
  TopoDS_Shape proto = opad::body_shape(m_doc->doc, n->body_key);
  std::shared_ptr<BodyPrs> prs;
  {
    std::lock_guard<std::mutex> lock(m_meshMu);
    auto p = m_prs.find(n->body_key);
    if (p != m_prs.end()) prs = p->second;
  }
  const BodyLook look = composeLook(*n);  // the document's appearance under the layers of looks (UI-121)
  // Rigid placements go on the object as a local transformation, so the prototype's precomputed arrays
  // (and sub-shape ordinals) are shared by every instance; anything else gets a transformed copy, but for a canvas stretched
  // out of its proportions, which is its own rectangle at that aspect.
  TopoDS_Shape located = proto;
  gp_Trsf placement;
  double stretch = 1;
  const bool rigid = rigidPlacement(*n, world, placement, stretch);
  if (!rigid) {
    located = opad::node_world_shape(m_doc->doc, scene, id);
    prs.reset();
  } else if (stretch != 1) {
    located = stretchedCanvas(*n, stretch);
    prs.reset();
  }
  Handle(AIS_Shape) ais = new BodyShape(located, prs);
  if (!rigid)
    if (auto colors = std::make_shared<const opad::FaceColors>(opad::face_colors(m_doc->doc, n->body_key)); !colors->empty())
      Handle(BodyShape)::DownCast(ais)->setFaceColors(colors);
  if (auto refined = m_refined.find(n->body_key); rigid && stretch == 1 && refined != m_refined.end())
    Handle(BodyShape)::DownCast(ais)->setDisplayPrs(refined->second.prs);  // zoomed in before: draw it fine at once
  const std::string raster = rasterKey(*n);
  if (const auto r = m_rasters.find(raster); r != m_rasters.end() && !r->second.IsNull()) {  // decoded on a worker (decodeRaster)
    Handle(AIS_TexturedShape) textured = new AIS_TexturedShape(located);
    textured->SetTexturePixMap(r->second);
    textured->SetTextureMapOn();
    textured->DisableTextureModulate();
    textured->SetTextureRepeat(false);
    ais = textured;
  } else if (r != m_rasters.end()) {
    emit hoverChanged(tr("Embedded image could not be decoded; showing its frame"));
  }
  gp_Trsf placed;
  if (look.offset != std::array<double, 3>{0, 0, 0}) placed.SetTranslation(gp_Vec(look.offset[0], look.offset[1], look.offset[2]));  // an explode offset, after the placement
  if (rigid) placed.Multiply(placement);
  if (placed.Form() != gp_Identity) ais->SetLocalTransformation(placed);
  ais->Attributes()->SetTypeOfDeflection(Aspect_TOD_ABSOLUTE);
  ais->Attributes()->SetMaximalChordialDeviation(deflectionFor(n->body_key));
  ais->Attributes()->SetDeviationAngle(20.0 * M_PI / 180.0);
  // Never let OCCT (re)mesh on the UI thread: with auto-triangulation on, building the selection entities
  // re-runs BRepMesh for any face whose mesh is missing or coarser than asked, which froze the app for
  // minutes on big bodies. Meshing happens once, on the worker; unmeshed faces get box sensitives.
  ais->Attributes()->SetAutoTriangulation(Standard_False);
  ais->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
  ais->SetColor(qcolor(look.color));
  if (look.opacity < 1.0) ais->SetTransparency(1.0 - look.opacity);
  if (look.layer != Graphic3d_ZLayerId_Default) ais->SetZLayer(look.layer);
  m_nodeOf[ais.get()] = id;  // applyStyle asks whether it is a drawing's
  if((n->representation=="drawing2d" && n->raster.is_null()) || opad::is_canvas(*n)) {  // outlined when selected: never a tint over a picture
    if(!opad::is_canvas(*n)) ais->Attributes()->ShadingAspect()->Aspect()->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);  // fills and text in their colour, unlit
    ais->SetHilightAttributes(m_drawingSelected);  // shared: their colours follow the background (updateDrawingHighlights)
    ais->SetDynamicHilightAttributes(m_drawingHover);
  }
  applyStyle(ais, &look);
  m_ctx->Display(ais, m_style == Style::Wireframe ? AIS_WireFrame : !Handle(AIS_TexturedShape)::DownCast(ais).IsNull() ? 3 : AIS_Shaded, -1, Standard_False);  // selection activated below, once
  if (!look.visible) m_ctx->Erase(ais, Standard_False);
  if (m_side) maskSide(id, ais);  // side by side: one of B's changes stays out of A's view
  const qint64 displayMs = t.elapsed();
  m_items[id] = Item{ais, n->body_key, world, n->color, n->opacity, located, {}, raster, look, rigid, stretch, placement};
  m_nodeOf[ais.get()] = id;
  ++m_displayCount;
  activateSelection(ais);  // after m_items: its look may say not pickable
  if (trace::enabled() && t.elapsed() > 50) trace::log(QStringLiteral("displayBody %1: display %2 ms, selection %3 ms").arg(QString::fromStdString(n->name)).arg(displayMs).arg(t.elapsed() - displayMs));
  m_longestDisplay = std::max(m_longestDisplay, t.elapsed());
  m_longestDisplayCpu = std::max(m_longestDisplayCpu, trace::threadCpuMs() - cpu);
  if (prs && !prs->navigation.IsNull()) {
    Handle(NavigationShape) nav = new NavigationShape(prs->navigation);
    if (placed.Form() != gp_Identity) nav->SetLocalTransformation(placed);
    m_navSelection->Load(nav, -1);
    if (look.visible) m_navSelection->Activate(nav, 0);
    m_items[id].navigation = nav;
    m_navNodes[nav.get()] = id;
  }
}

void Viewport::updateDepthBias() {
  std::vector<Bnd_Box> boxes;
  for (const auto& [id, item] : m_items) {
    Bnd_Box box;
    item.ais->BoundingBox(box);
    boxes.push_back(box);
  }
  const auto ranks = depthSlots(boxes);
  size_t i = 0;
  bool reselect = false;
  for (const auto& [id, item] : m_items) {
    // Whole depth units, with slope separation for oblique coplanar faces.
    // Fractional hash offsets used to quantize to the same depth and flicker.
    const int slot = ranks[i++];
    const auto body=Handle(BodyShape)::DownCast(item.ais);
    const double extent=boxes[i-1].IsVoid()?1:boxes[i-1].CornerMin().Distance(boxes[i-1].CornerMax());
    if (!body.IsNull() && body->setRayBias(m_renderQuality==2 ? -slot*std::max(1e-5,extent*2e-6) : 0)) {
      m_ctx->RecomputePrsOnly(body,false);  // keeps it selected (Redisplay did not)
      reselect = reselect || m_ctx->IsSelected(body);
    }
    item.ais->SetPolygonOffsets(Aspect_POM_Fill, 1.0f + 0.25f * slot, 1.0f + 4.0f * slot);
  }
  if (reselect) m_ctx->HilightSelected(Standard_False);
}

void Viewport::finishSync(int pendingCount, bool added) {
  if (added && m_style == Style::HiddenEdges) scheduleEdgeOverlay();
  if (added) updateDepthBias();
  if (added) m_refineTimer.start();  // bodies that arrived in a zoomed-in view
  updateGridExtent();
  emit meshingProgress(pendingCount);
  // Keep fitting while a load is still streaming bodies in, but only until the user moves the camera:
  // every fit, orbit or zoom of theirs clears m_needFit so a later batch never snaps the view back.
  if(m_needFit && !m_fitNodesOnSync.empty()) {
    if(pendingCount==0){auto ids=std::move(m_fitNodesOnSync);m_fitNodesOnSync.clear();fitNodes(ids);}
  }else if (added && m_needFit) m_view->FitAll(fitBounds(), 0.02, Standard_False);
  if(!m_needFit)m_fitNodesOnSync.clear();
  if (pendingCount == 0) m_needFit = false;
  if (m_sectionEnabled) updateSectionGizmo();  // the model's extent may have changed
  m_view->Invalidate();
  requestRedraw();
}

// ---------------------------------------------------------------- Qt events
QPointF Viewport::viewScale() const {
  Standard_Integer viewW = 0, viewH = 0;
  if (!m_view.IsNull() && !m_view->Window().IsNull()) m_view->Window()->Size(viewW, viewH);
  const qreal fallback = devicePixelRatioF();
  return {width() > 0 && viewW > 0 ? qreal(viewW) / width() : fallback,
          height() > 0 && viewH > 0 ? qreal(viewH) / height() : fallback};
}

Graphic3d_Vec2i Viewport::devicePos(const QPointF& p) const {
  const QPointF scale = viewScale();
  return Graphic3d_Vec2i(qRound(p.x() * scale.x()), qRound(p.y() * scale.y()));
}

void Viewport::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  if (!m_initialised) {
    m_needFit = true;
    if (m_warmed) initViewer();  // else the startup makes it once the window has been painted (warmUp)
  }
  // The stacked layout may resize us after the native window was created; re-check once shown.
  QTimer::singleShot(0, this, [this] { syncWindowSize(); requestRedraw(); });
}

// Keeps the OCCT window in step with the widget. A native child that was created while hidden can keep
// its initial size until the next real resize, which left the 3D view drawing in a corner (open from Recent).
void Viewport::syncWindowSize() {
  if (!m_initialised || m_view.IsNull() || m_view->Window().IsNull()) return;
  const qreal dpr = devicePixelRatioF();
  const int wantW = qRound(width() * dpr), wantH = qRound(height() * dpr);
  // Cache Qt's size and display scale so a hidden native child gets an OCCT resize when shown.
  // The OCCT Cocoa window can report logical points here even when Qt's display scale is 2.
  if (wantW == m_lastSyncedSize.first && wantH == m_lastSyncedSize.second) return;
  m_lastSyncedSize = {wantW, wantH};
  if (QWindow* native = windowHandle()) native->resize(size());
  m_view->MustBeResized();
  const qreal cubeScale = viewScale().x();
  if (!m_cube.IsNull() && !qFuzzyCompare(cubeScale, m_cubeScale)) {
    m_cubeScale = cubeScale;
    m_cube->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER,
        Graphic3d_Vec2i(qRound(kCubeOffsetX * m_cubeScale), qRound(kCubeOffsetY * m_cubeScale))));
    m_cube->SetSize(64 * m_cubeScale);
    m_cube->SetFontHeight(11 * m_cubeScale);
    m_cube->SetAxesPadding(6 * m_cubeScale);
    m_cube->SetAxesRadius(0.6 * m_cubeScale);
    m_cube->SetAxesConeRadius(1.2 * m_cubeScale);
    m_cube->SetAxesSphereRadius(1.0 * m_cubeScale);
    m_ctx->Redisplay(m_cube, Standard_False);
    scheduleLooks();  // drawings' hairlines follow the display scale
  }
  if (trace::enabled()) {
    Standard_Integer viewW = 0, viewH = 0;
    m_view->Window()->Size(viewW, viewH);
    trace::log(QStringLiteral("viewport coordinates: Qt %1x%2, OCCT %3x%4, display scale %5")
                   .arg(width()).arg(height()).arg(viewW).arg(viewH).arg(dpr));
  }
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::paintEvent(QPaintEvent*) {
  // A selection callback can open a modal dialog (for example, adding a note). Its nested Qt
  // event loop may deliver another paint before OCCT finishes iterating its pending click points.
  // A second FlushViewEvents() would clear that sequence and invalidate the outer iterator.
  if (m_flushingViewEvents) {
    m_repaintAfterFlush = true;
    return;
  }
  if (!m_initialised) {
    if (!m_warmed) return;  // the startup has not made the viewer yet (warmUp)
    initViewer();
  }
  syncWindowSize();
  QElapsedTimer frame;
  frame.start();
  {
    QScopedValueRollback<bool> flushing(m_flushingViewEvents, true);
    FlushViewEvents(m_ctx, m_view, Standard_True);
  }
  if (m_repaintAfterFlush) {
    m_repaintAfterFlush = false;
    requestRedraw();
  }
  if (trace::enabled() && frame.elapsed() > 100) trace::log(QStringLiteral("slow frame: %1 ms (%2 objects)").arg(frame.elapsed()).arg(m_items.size()));
  updateTracking();
  updateHover();
  updateObjectSnap();
}

// The status text, the hovered drawing entity and the point under the mouse, after a frame's detection.
void Viewport::updateHover() {
  // The label needs the sub-shape's ordinal, a walk over the whole body: only when the hovered owner changes.
  const Standard_Transient* hoverOwner = m_ctx->HasDetected() ? m_ctx->DetectedOwner().get() : nullptr;
  if (hoverOwner == m_hoverOwner) return;
  m_hoverOwner = hoverOwner;
  discoverCenter();
  QString hover;
  opad::json drawingInfo;
  if (m_ctx->HasDetected()) {
    Handle(AIS_InteractiveObject) obj = m_ctx->DetectedInteractive();
    auto it = m_nodeOf.find(obj.get());
    const opad::Node* node = it != m_nodeOf.end() ? m_doc->scene.node(it->second) : nullptr;
    if (node && m_drawingWords && node->representation == "drawing2d" && node->raster.is_null()) {  // 2D words (UI-118)
      Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
      Handle(SubShapeOwner) mine = Handle(SubShapeOwner)::DownCast(owner);
      const QString layer = m_doc->nodeName(node->parent.empty() ? node->id : node->parent);
      // The whole layer under its own owner (the Bodies filter, and the Face filter, which picks a drawing layer whole: UI-42)
      // is a group; an object of it is named by its kind.
      const bool whole = owner.IsNull() || !owner->HasShape() || !owner->ComesFromDecomposition() || m_filter == SelFilter::Body;
      drawingInfo = whole ? opad::json{{"type", "group"}} : drawing2d::entityInfo(owner->Shape());
      if (!drawingInfo.contains("type") || !drawingInfo["type"].is_string()) drawingInfo["type"] = "group";
      drawingInfo["body"] = node->id;
      if (!mine.IsNull()) drawingInfo["index"] = mine->index();
      hover = drawingInfo["type"] == "group" ? tr("Group on %1").arg(layer) : tr("%1 on %2").arg(drawingWord(drawingInfo["type"].get<std::string>()), layer);
      if (drawingInfo.contains("radius")) hover += QStringLiteral(" · R ") + units::format(units::Kind::Length, drawingInfo["radius"].get<double>());
      else if (drawingInfo.contains("length")) hover += QStringLiteral(" · ") + units::format(units::Kind::Length, drawingInfo["length"].get<double>());
      else if (drawingInfo.contains("area")) hover += QStringLiteral(" · ") + units::format(units::Kind::Area, drawingInfo["area"].get<double>());
    } else if (it != m_nodeOf.end()) {
      hover = hoverName(it->second);
      Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
      if (!owner.IsNull() && owner->HasShape() && owner->ComesFromDecomposition() && m_filter != SelFilter::Body) {
        const TopoDS_Shape& sub = owner->Shape();
        const char* kind = sub.ShapeType() == TopAbs_FACE ? "face" : sub.ShapeType() == TopAbs_EDGE ? "edge" : "vertex";
        Handle(SubShapeOwner) mine = Handle(SubShapeOwner)::DownCast(owner);
        hover += QString::fromUtf8(" › %1 %2").arg(i18n::t(kind)).arg(mine.IsNull() ? opad::subshape_index(Handle(AIS_Shape)::DownCast(obj)->Shape(), sub) : mine->index());
        if(!mine.IsNull()) {
          opad::Ref ref; ref.body=it->second; ref.kind=Handle(CircleOwner)::DownCast(mine).IsNull()?opad::Ref::Kind::Edge:opad::Ref::Kind::Center; ref.index=mine->index();
          if(sub.ShapeType()==TopAbs_EDGE || ref.kind==opad::Ref::Kind::Center) {
            const auto info=circleInfo(ref);
            if(info.contains("diameter")) hover+=tr(" | Diameter %1").arg(units::format(units::Kind::Length,info["diameter"].get<double>()));
            if(info.contains("segments")) hover+=tr(" | %1 segments (approximate)").arg(info["segments"].get<int>());
          }
        }
      }
    }
  }
  if (!m_activeCenter.empty()) {
    hover += hover.isEmpty() ? QString() : QStringLiteral(" · ");
    hover += m_centerLocked ? tr("Center locked · click center · Shift unlock") : tr("Circle center · Shift lock · click center");
  } else if (m_ctx->HasDetected() && m_centerObjects.count(m_ctx->DetectedInteractive().get())) {
    hover = tr("Circle center");
  }
  if (hover != m_hover) {
    m_hover = hover;
    emit hoverChanged(hover);
  }
  if (drawingInfo != m_hoverInfo) {
    m_hoverInfo = drawingInfo;
    emit hoverInfo(drawingInfo);
  }
  gp_Pnt hp;
  const bool onGeometry = detectedPoint(hp)
      && (m_nodeOf.count(m_ctx->DetectedInteractive().get()) || m_centerObjects.count(m_ctx->DetectedInteractive().get()));
  if (onGeometry) {
    auto center = m_centerObjects.find(m_ctx->DetectedInteractive().get());
    Handle(CircleOwner) circle = Handle(CircleOwner)::DownCast(m_ctx->DetectedOwner());
    if (center != m_centerObjects.end()) hp = m_centers.at(center->second).point;
    else if (!circle.IsNull()) hp = circle->center.Transformed(m_ctx->DetectedInteractive()->Transformation());
  }
  emit hoverPoint(onGeometry, opad::Vec3{hp.X(), hp.Y(), hp.Z()});
}

void Viewport::resizeEvent(QResizeEvent*) {
  if (!m_initialised) return;
  finishTrackpadScroll();
  m_view->MustBeResized();
  m_view->Invalidate();
  requestRedraw();
}

QPointF Viewport::cubeCentre() const { return QPointF(width() - kCubeOffsetX, kCubeOffsetY); }

// Same test as a press below: the hover is refreshed near the cube only, so a click on the model costs no extra pick.
bool Viewport::cubeAt(const QPointF& point) {
  if (!m_initialised || m_blocked || m_twoDimensional) return false;
  const QPointF cubeCenter(width() - kCubeOffsetX, kCubeOffsetY);
  if (qAbs(point.x() - cubeCenter.x()) > 96 || qAbs(point.y() - cubeCenter.y()) > 96) return false;
  const Graphic3d_Vec2i at = devicePos(point);
  m_ctx->MoveTo(at.x(), at.y(), m_view, Standard_False);
  return m_ctx->HasDetected() && m_ctx->DetectedInteractive() == m_cube;
}

QRect Viewport::cubeRect() const {
  if (!m_initialised || m_twoDimensional) return {};
  return QRect(width() - kCubeOffsetX - 48, kCubeOffsetY - 48, 96, 96);
}

void Viewport::mousePressEvent(QMouseEvent* e) {
  if (m_blocked) return;
  finishTrackpadScroll();
  m_nativePinching = false;
  setFocus();
  if(m_boxJob) m_boxJob->cancel();
  if(e->buttons()==e->button()) { m_dragOffset = {}; m_warpGate.pending = false; }
  m_pressPos = (e->position()+m_dragOffset).toPoint();
  m_rightPress = e->button() == Qt::RightButton;
  m_cubeClick = false;
  if (m_sketchInput && e->button() != Qt::LeftButton && !m_ownCursorAside) {  // a camera gesture or the context menu: the pointer
    m_ownCursorAside = true;
    applyOwnCursor();
  }
  m_holdTimer.stop();
  m_holdPress = false;
  const bool listPending = m_selectOtherTimer.isActive();  // an Alt+click's list waits out the double-click time
  m_selectOtherTimer.stop();
  // The zoom window takes the left drag (UI-47); a right click leaves it.
  if (m_zoomWindow && e->button() == Qt::LeftButton) { m_zoomDrag = true; m_zoomFrom = m_zoomTo = e->position(); return; }
  if (m_zoomWindow && e->button() == Qt::RightButton) { m_rightPress = false; cancelZoomWindow(); return; }
  m_cubeMenu = m_rightPress && cubeAt(e->position());  // a right click on the cube opens its menu
  if (sectionMousePress(e)) return;  // a press on the section plane's handle strip starts a drag, never a selection
  // Alt+click lists everything under the pointer (UI-128): the press and its release are not the controller's. The second
  // click of an Alt+double-click is (a click, smart selection's tangent chain): it opens no list.
  if (m_initialised && e->button() == Qt::LeftButton && e->modifiers() == Qt::AltModifier && !m_sketchInput && !cubeAt(e->position())
      && !(listPending && e->type() == QEvent::MouseButtonDblClick)) {
    m_selectOtherPress = true;
    e->accept();
    return;
  }
  // A press can arrive without a preceding hover. Refresh only near the cube (or when the old hover was
  // the cube) so the gesture below uses this press's owner without an extra scene pick on every model click.
  const QPointF cubeCenter(width() - kCubeOffsetX, kCubeOffsetY);
  const bool nearCube = qAbs(e->position().x() - cubeCenter.x()) <= 96
      && qAbs(e->position().y() - cubeCenter.y()) <= 96;
  if (m_initialised && e->button() == Qt::LeftButton
      && (nearCube || (m_ctx->HasDetected() && m_ctx->DetectedInteractive() == m_cube)))
    moveTo(devicePos(e->position()));
  if(m_initialised && e->button()==Qt::LeftButton && !m_sketchInput)
    setCenterPicking(e->modifiers().testFlag(Qt::ControlModifier),e->position());
  if(m_ctrlCenterPick && e->button()==Qt::LeftButton && !m_sketchInput && m_filter==SelFilter::Vertex && !m_measureSelectionLocked) {
    moveTo(devicePos(e->position()));discoverCenter();
    if(m_ctx->HasDetected()) {
      const auto circle=Handle(CircleOwner)::DownCast(m_ctx->DetectedOwner());
      auto marker=m_centerObjects.find(m_ctx->DetectedInteractive().get());
      if(!circle.IsNull() && !m_activeCenter.empty()) m_snapClick=m_activeCenter;
      else if(marker!=m_centerObjects.end() && m_centers.at(marker->second).ref.kind==opad::Ref::Kind::Center) m_snapClick=marker->second;
      if(!m_snapClick.empty()) {e->accept();return;}
    }
  }
  // Sketching: the left button belongs to the sketch editor, except on the view cube.
  if (m_sketchInput && m_initialised && e->button() == Qt::LeftButton && !(m_ctx->HasDetected() && m_ctx->DetectedInteractive() == m_cube)) {
    double u, v;
    if (planePoint(e->position(), m_sketchFrame, u, v)) {
      m_sketchDrag = true;
      m_sketchInput->sketchPress(u, v, e->modifiers());
    }
    return;
  }
  // A left press on the view cube: dragging orbits the view (the cube turns with it); a click without movement
  // still goes through the controller's click path and snaps to the picked side.
  if (m_initialised && !m_twoDimensional && e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier && m_ctx->HasDetected() && m_ctx->DetectedInteractive() == m_cube) {
    ChangeMouseGestureMap().Bind(Aspect_VKeyMouse_LeftButton, AIS_MouseGesture_RotateOrbit);
    focusCube();
    m_cubeGesture = true;
    m_cubeClick = true;
    myViewAnimation->SetOwnDuration(motion::seconds(0.5));  // the turn to the clicked side (roll and align set their own)
    m_needFit = false;
    // Only the Replace scheme hands a click to the cube (HandleMouseClick); a guided tool's XOR would toggle it as a pick.
    ChangeMouseSelectionSchemes().Bind(Aspect_VKeyMouse_LeftButton, AIS_SelectionScheme_Replace);
  }
  if (m_initialised && !m_cubeGesture && objectSnapPress(e)) return;
  if (!m_cubeGesture && e->button()==Qt::LeftButton && m_initialised && m_pickAccumulate && !m_measureSelectionLocked) {
    // A locked guide's point is taken wherever the click lands (a cross lock's crossing can be far from the pointer).
    auto tracked=m_centers.find(m_trackingMarker);
    if(tracked!=m_centers.end() && (m_shift.locked() || ((!m_ctx->HasDetected() || m_ctx->DetectedInteractive()==tracked->second.ais) && (QPointF(widgetPoint(tracked->second.ref.point))-e->position()).manhattanLength()<16))) {
      m_snapClick=m_trackingMarker; e->accept(); return;
    }
  }
  // Capture the entire gesture before OCCT can toggle an edge or begin a selection rectangle.
  // Middle/right navigation and the view cube retain their usual controls.
  if (!m_cubeGesture && e->button() == Qt::LeftButton && m_initialised
      && (m_measureSelectionLocked || (m_retainToolPicks && m_ctx->HasDetected() && m_ctx->IsSelected(m_ctx->DetectedOwner())))) {
    m_measureAnchorPress = true;
    e->accept();
    return;
  }
  if (m_initialised && m_twoDimensional && orbitGesture(qt_buttons(e->buttons()) | qt_flags(e->modifiers())))
    twoDimensionalHint(e->globalPosition().toPoint());
  if(m_initialised && !m_twoDimensional) {
    const auto gesture=qt_buttons(e->buttons())|qt_flags(e->modifiers());
    if(ChangeMouseGestureMap().IsBound(gesture) && ChangeMouseGestureMap().Find(gesture)==AIS_MouseGesture_RotateOrbit) {
      const auto camera=m_view->Camera();const auto direction=camera->Direction();
      if(std::abs(direction.Z())>1-1e-8){const auto up=camera->Up();camera->SetDirection(gp_Dir(direction.X()+up.X()*1e-4,direction.Y()+up.Y()*1e-4,direction.Z()));}
    }
  }
  if (m_initialised && e->button() == Qt::LeftButton && e->buttons() == Qt::LeftButton && e->modifiers() == Qt::NoModifier && !m_cubeGesture) {
    m_holdAt = e->position();
    m_holdTimer.start(QGuiApplication::styleHints()->mousePressAndHoldInterval());
  }
  if (m_initialised && UpdateMouseButtons(devicePos(e->position()+m_dragOffset), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
  m_holdTimer.stop();
  if (m_blocked) return;
  if (std::exchange(m_holdPress, false) && e->button() == Qt::LeftButton) {  // the held press opened the list
    e->accept();
    return;
  }
  if (m_zoomDrag && e->button() == Qt::LeftButton) {
    m_zoomDrag = false;
    m_zoomTo = e->position();
    finishZoomWindow();
    return;
  }
  if (sectionMouseRelease(e)) return;
  if (m_selectOtherPress && e->button() == Qt::LeftButton) {
    m_selectOtherPress = false;
    if ((e->position() + m_dragOffset - m_pressPos).manhattanLength() < 4) {
      m_selectOtherAt = e->position();
      m_selectOtherGlobal = e->globalPosition().toPoint();
      m_selectOtherTimer.start(QGuiApplication::styleHints()->mouseDoubleClickInterval());
    }
    e->accept();
    return;
  }
  if (!m_snapClick.empty() && e->button()==Qt::LeftButton) {
    const auto key=m_snapClick;
    auto marker=m_centers.find(key);
    if(marker!=m_centers.end() && (e->position()-m_pressPos).manhattanLength()<4) {
      const auto ref=marker->second.ref; m_ctx->AddOrRemoveSelected(marker->second.ais,false);
      // Retain the exact acquired point rather than a stale selector hit.
      OnSelectionChanged(m_ctx,m_view);
      m_shift.clicked();  // the pick ends a sticky lock
      m_trackingDirty=true;
    }
    m_snapClick.clear(); e->accept(); return;
  }
  if (m_measureAnchorPress && e->button() == Qt::LeftButton) {
    m_measureAnchorPress = false;
    if ((e->position() + m_dragOffset - m_pressPos).manhattanLength() < 4) {
      const int index = measurementAnchorAt(e->position());
      if (index >= 0) {
        const auto anchor = m_measureAnchors[index];
        emit measurementAnchorPicked(anchor.side, anchor.point);
      }
    }
    e->accept();
    return;
  }
  if (m_sketchDrag && e->button() == Qt::LeftButton) {
    m_sketchDrag = false;
    double u, v;
    if (m_sketchInput && planePoint(e->position(), m_sketchFrame, u, v)) m_sketchInput->sketchRelease(u, v, e->modifiers());
    if (m_sketchInput && m_ownCursorAside && !rect().contains(e->position().toPoint())) {  // dropped off the view
      m_ownCursorAside = false;
      applyOwnCursor();
    }
    return;
  }
  QPointF releasePosition=e->position();
  if(m_warpGate.pending) {releasePosition=m_warpPosition-m_dragOffset;m_warpGate.pending=false;}
  if (m_initialised && UpdateMouseButtons(devicePos(releasePosition + m_dragOffset), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
  if (m_cubeGesture && e->button() == Qt::LeftButton) {
    m_cubeGesture = false;
    ChangeMouseGestureMap().Bind(Aspect_VKeyMouse_LeftButton, AIS_MouseGesture_SelectRectangle);
    ChangeMouseSelectionSchemes().Bind(Aspect_VKeyMouse_LeftButton, m_pickAccumulate ? AIS_SelectionScheme_XOR : AIS_SelectionScheme_Replace);
  }
  if (m_rightPress && e->button() == Qt::RightButton && (e->position() + m_dragOffset - m_pressPos).manhattanLength() < 4) {
    m_rightPress = false;
    if (std::exchange(m_cubeMenu, false)) emit cubeMenuRequested(e->globalPosition().toPoint());
    else {
      contextPick(e->position());
      m_contextAt = e->position();  // while the menu is open: where it was asked for (Select other, UI-128)
      m_inContextMenu = true;
      emit contextMenuRequested(e->globalPosition().toPoint());
      m_inContextMenu = false;
    }
  }
  // A camera gesture is over: the sketch's own cursor again at once (ownCursorChanged: the editor snaps where the pointer is
  // now), so a click right after it without a move goes where the cursor is drawn.
  if (m_sketchInput && m_ownCursorAside && e->buttons() == Qt::NoButton) {
    m_ownCursorAside = m_ownCursorWanted && rect().contains(e->position().toPoint()) && cubeAt(e->position());
    applyOwnCursor();
  }
  if (e->buttons() == Qt::NoButton) { m_dragOffset = {}; m_warpGate.pending=false; }
}

// Off the view (onto the ribbon, or out of the window): nothing is under the pointer any more. The controller would keep
// detecting at the last position on every redraw (after an orbit too), and the object there stayed highlighted.
void Viewport::leaveEvent(QEvent* e) {
  QWidget::leaveEvent(e);
  if (!m_initialised) return;
  if (m_sketchInput) m_sketchInput->sketchLeave();
  m_hoverCycled = false;
  ResetPreviousMoveTo();
  m_hoverFadeTimer.stop();
  if (m_ctx->HasDetected()) {
    m_ctx->ClearDetected(Standard_False);
    m_view->InvalidateImmediate();
  }
  requestRedraw();
}

bool Viewport::benchLeave() {
  if (!m_initialised || m_items.empty()) return false;
  fitAll();
  m_view->Redraw();  // the picker needs a frame after a camera change
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  m_ctx->MoveTo(w / 2, h / 2, m_view, Standard_False);
  const bool hovered = m_ctx->HasDetected();
  QEvent leave(QEvent::Leave);
  QCoreApplication::sendEvent(this, &leave);
  FlushViewEvents(m_ctx, m_view, Standard_True);  // what the next frame does
  const bool left = !m_ctx->HasDetected();
  trace::log(QStringLiteral("bench: leave: hovered %1, highlight after leaving %2 %3").arg(hovered).arg(left ? "cleared" : "KEPT").arg(hovered && left ? "PASS" : "FAIL"));
  return hovered && left;
}

void Viewport::mouseMoveEvent(QMouseEvent* e) {
  const bool awaitingWarp=m_warpGate.pending;
  if (e->buttons() != Qt::NoButton && !m_warpGate.accept(e->globalPosition().toPoint())) return;
  if(awaitingWarp && !m_warpGate.pending && e->buttons()!=Qt::NoButton) m_dragOffset=m_warpPosition-e->position();
  if(m_initialised && e->buttons()==Qt::NoButton) setCenterPicking(e->modifiers().testFlag(Qt::ControlModifier),e->position());
  if (m_hoverCycled && devicePos(e->position() + m_dragOffset) != m_cycledAt) m_hoverCycled = false;  // the pointer moved on: it picks again
  m_trackingCursor = e->position();
  m_trackingDirty = true;
  if (m_holdTimer.isActive() && (e->position() - m_holdAt).manhattanLength() >= 4) m_holdTimer.stop();  // a drag, not a hold
  if (m_blocked) return;
  if (m_holdPress) {  // the list is open over the held press, whose release the list may take instead of the view
    if (e->buttons() & Qt::LeftButton) return;
    m_holdPress = false;
  }
  if (m_zoomDrag) { m_zoomTo = e->position(); showZoomBand(); return; }
  if (m_zoomWindow && e->buttons() == Qt::NoButton) return;  // nothing hovered or glowing: the prompt stays in the status
  if (m_selectOtherPress) return;  // an Alt+press drags nothing
  if (m_trackpadMode != TrackpadMode::None && e->buttons() == Qt::NoButton) finishTrackpadScroll();
  if (m_measureAnchorPress) return;
  if (sectionMouseMove(e)) return;  // dragging the section plane
  if (m_measureSelectionLocked && e->buttons() == Qt::NoButton && m_sectionHover < 0) {
    const int index = measurementAnchorAt(e->position());
    if (index >= 0) {
      setCursor(Qt::PointingHandCursor);
      setToolTip(tr("Set measurement point %1 here").arg(m_measureAnchors[index].side + 1));
    } else {
      unsetCursor();
      setToolTip(QString());
    }
  }
  // The sketch's own cursor gives way to the pointer over the view cube, the section plane's handle and during a camera
  // gesture (it comes back with the first move after it), and while a drag goes off the view (the press holds the mouse,
  // so the view's blank cursor would stay over the ribbon and panels, and nothing would show where the pointer is).
  if (m_sketchInput) {
    const bool off = m_sketchDrag && !rect().contains(e->position().toPoint());
    const bool aside = (e->buttons() != Qt::NoButton && !m_sketchDrag) || off
        || (m_ownCursorWanted && e->buttons() == Qt::NoButton && (m_sectionHover >= 0 || cubeAt(e->position())));
    if (aside != m_ownCursorAside) {
      m_ownCursorAside = aside;
      applyOwnCursor();
    }
  }
  // Camera gestures do not need sketch hover, snapping, or geometry updates.
  if (m_sketchInput && (m_sketchDrag || e->buttons()==Qt::NoButton)) {
    double u, v;
    if (planePoint(e->position(), m_sketchFrame, u, v)) m_sketchInput->sketchMove(u, v, e->modifiers(), m_sketchDrag);
    if (m_sketchDrag) return;  // not a rubber band
  }
  if (e->buttons() != Qt::NoButton) m_needFit = false;  // a drag: the user owns the camera now
  if (m_initialised && UpdateMousePosition(devicePos(e->position() + m_dragOffset), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
  const bool navigation = myMouseActiveGesture == AIS_MouseGesture_Pan
      || myMouseActiveGesture == AIS_MouseGesture_RotateOrbit || myMouseActiveGesture == AIS_MouseGesture_RotateView
      || myMouseActiveGesture == AIS_MouseGesture_Zoom || myMouseActiveGesture == AIS_MouseGesture_ZoomVertical;
  if (navigation && e->buttons() != Qt::NoButton && e->spontaneous()
      && QGuiApplication::platformName() != "wayland") {
    const QPoint global = e->globalPosition().toPoint();
    if (QGuiApplication::screenAt(global)) {
      std::vector<QRect> screens;for(auto* display:QGuiApplication::screens()) screens.push_back(display->geometry());
      const QPoint target = wrappedDesktopCursor(global,screens);
      if (target != global) {
        // Keep controller coordinates continuous across the warp, including its
        // generated move event; the scene never sees a display-width jump.
        m_warpPosition=e->position()+m_dragOffset;
        m_dragOffset += global - target;
        m_warpGate.begin(global, target);
        QCursor::setPos(target);
        if (QCursor::pos() != target) { m_dragOffset -= global - target; m_warpGate.pending=false; }
      }
    }
  }
}

void Viewport::wheelEvent(QWheelEvent* e) {
  if (!m_initialised || m_blocked) return;
  const bool trackpad = (e->device() && e->device()->type() == QInputDevice::DeviceType::TouchPad)
      || (!e->pixelDelta().isNull() && e->phase() != Qt::NoScrollPhase);
  if (trackpad) {
    if (m_nativePinching) { e->accept(); return; }
    const QPointF delta = !e->pixelDelta().isNull() ? QPointF(e->pixelDelta()) : QPointF(e->angleDelta()) / 8.0;
    if (e->modifiers() & Qt::ControlModifier) {
      finishTrackpadScroll();
      if (delta.y() != 0.0) {
        m_needFit = false;
        UpdateZoom(Aspect_ScrollDelta(devicePos(e->position()), delta.y()));
        requestRedraw();
      }
    } else if (!delta.isNull()) {
      trackpadScroll(e->position(), delta, bool(e->modifiers() & Qt::ShiftModifier));
    }
    if (e->phase() == Qt::ScrollEnd) finishTrackpadScroll();
    e->accept();
    return;
  }
  finishTrackpadScroll();
  m_needFit = false;
  m_wheelClock.start();
  const double delta = e->angleDelta().y() / 8.0;
  if (UpdateZoom(Aspect_ScrollDelta(devicePos(e->position()), delta))) requestRedraw();
}

void Viewport::trackpadScroll(const QPointF& position, const QPointF& delta, bool orbit) {
  if (orbit && m_twoDimensional) twoDimensionalHint(mapToGlobal(position).toPoint());
  orbit = orbit && !m_twoDimensional && m_preset != NavPreset::Cad2D;
  if (delta.isNull()) return;
  const TrackpadMode mode = orbit ? TrackpadMode::Orbit : TrackpadMode::Pan;
  if (mode != m_trackpadMode) {
    finishTrackpadScroll();
    m_trackpadCursor = position;
    m_trackpadMode = mode;
    const Aspect_VKeyFlags flags = Aspect_VKeyFlags_META | (orbit ? Aspect_VKeyFlags_SHIFT : 0);
    if(orbit){const auto camera=m_view->Camera();const auto direction=camera->Direction();if(std::abs(direction.Z())>1-1e-8){const auto up=camera->Up();camera->SetDirection(gp_Dir(direction.X()+up.X()*1e-4,direction.Y()+up.Y()*1e-4,direction.Z()));}}
    UpdateMouseButtons(devicePos(m_trackpadCursor), Aspect_VKeyMouse_MiddleButton, flags, false);
  }
  m_trackpadAnchor = position;
  m_trackpadCursor += delta;
  const Aspect_VKeyFlags flags = Aspect_VKeyFlags_META | (orbit ? Aspect_VKeyFlags_SHIFT : 0);
  UpdateMousePosition(devicePos(m_trackpadCursor), Aspect_VKeyMouse_MiddleButton, flags, false);
  m_needFit = false;
  m_trackpadEndTimer.start();
  requestRedraw();
}

void Viewport::finishTrackpadScroll() {
  m_trackpadEndTimer.stop();
  if (m_trackpadMode == TrackpadMode::None) return;
  const Aspect_VKeyFlags flags = Aspect_VKeyFlags_META
      | (m_trackpadMode == TrackpadMode::Orbit ? Aspect_VKeyFlags_SHIFT : 0);
  UpdateMouseButtons(devicePos(m_trackpadCursor), Aspect_VKeyMouse_NONE, flags, false);
  m_trackpadMode = TrackpadMode::None;
  UpdateMousePosition(devicePos(m_trackpadAnchor), Aspect_VKeyMouse_NONE, Aspect_VKeyFlags_NONE, false);
  requestRedraw();
}

void Viewport::zoomAt(const QPointF& position, qreal scaleFactor) {
  if (scaleFactor <= 0.0 || scaleFactor == 1.0) return;
  // OCCT maps a positive scroll delta of 100 to a 2x zoom, and a negative delta of -100 to 0.5x.
  const double delta = scaleFactor > 1.0 ? 100.0 * (scaleFactor - 1.0)
                                         : -100.0 * (1.0 / scaleFactor - 1.0);
  m_needFit = false;
  UpdateZoom(Aspect_ScrollDelta(devicePos(position), delta));
  requestRedraw();
}

bool Viewport::handleNativeGesture(QNativeGestureEvent* e) {
  if (!m_initialised || m_blocked) return false;
  switch (e->gestureType()) {
    case Qt::BeginNativeGesture:
      m_nativePinching = false;
      e->accept();
      return true;
    case Qt::EndNativeGesture:
      m_nativePinching = false;
      e->accept();
      return true;
    case Qt::ZoomNativeGesture:
      finishTrackpadScroll();
      m_nativePinching = true;
      zoomAt(e->position(), std::clamp(1.0 + e->value(), 0.05, 20.0));
      e->accept();
      return true;
    case Qt::PanNativeGesture:
      if (!m_nativePinching) trackpadScroll(e->position(), e->delta(), false);
      e->accept();
      return true;
    default:
      return false;
  }
}
