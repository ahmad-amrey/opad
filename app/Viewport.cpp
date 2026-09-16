#include "Viewport.hpp"

#include <functional>

#include <QElapsedTimer>
#include <QTimer>
#include <QWindow>

#include <AIS_AnimationCamera.hxx>
#include <AIS_ViewCube.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_ScrollDelta.hxx>
#include <Aspect_VKeyFlags.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <Quantity_Color.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <TopExp.hxx>
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

#include <QMouseEvent>
#include <QWheelEvent>
#include <cmath>
#include <thread>

#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "Jobs.hpp"
#include "NavCube.hpp"
#include <TColStd_ListOfInteger.hxx>
#include <Prs3d_DatumAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <Prs3d_TextAspect.hxx>

namespace {
constexpr int kCubeOffsetX = 100, kCubeOffsetY = 104;  // view cube centre from the top-right corner, in px
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
  setMinimumSize(200, 150);
  connect(doc, &AppDocument::changed, this, &Viewport::sync);
  m_syncTimer.setSingleShot(true);
  m_syncTimer.setInterval(50);
  connect(&m_syncTimer, &QTimer::timeout, this, &Viewport::sync);
  m_timer.setInterval(16);
  connect(&m_timer, &QTimer::timeout, this, [this] {
    if (!m_initialised) return;
    if (!myViewAnimation.IsNull() && !myViewAnimation->IsStopped()) requestRedraw();
    else if (toAskNextFrame()) requestRedraw();
  });
  m_timer.start();
}

Viewport::~Viewport() { *m_alive = false; }

void Viewport::benchShot(const QString& path) {
  if (!m_initialised) return;
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  // Hover point relative to the cube centre (default: the TOP face); OPAD_BENCH_HOVER="dx,dy" overrides it.
  int dx = 0, dy = -36;
  const QByteArray hoverEnv = qgetenv("OPAD_BENCH_HOVER");
  if (hoverEnv.contains(',')) { dx = hoverEnv.split(',')[0].toInt(); dy = hoverEnv.split(',')[1].toInt(); }
  m_ctx->MoveTo(w - kCubeOffsetX + dx, kCubeOffsetY + dy, m_view, Standard_False);
  TColStd_ListOfInteger cubeModes;
  m_ctx->ActivatedModes(m_cube, cubeModes);
  trace::log(QStringLiteral("bench: cube hover at (%1,%2): detected=%3 isCube=%4 cubeModes=%5 cubeHasSel0=%6").arg(w - kCubeOffsetX + dx).arg(kCubeOffsetY + dy).arg(m_ctx->HasDetected()).arg(m_ctx->HasDetected() && m_ctx->DetectedInteractive() == m_cube).arg(cubeModes.Size()).arg(m_cube->HasSelection(0)));
  m_view->Redraw();
  m_view->RedrawImmediate();
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
  m_viewer->SetDefaultLights();
  m_viewer->SetLightOn();
  m_ctx = new AIS_InteractiveContext(m_viewer);
  m_ctx->SetPixelTolerance(4);
  m_ctx->SetAutoActivateSelection(Standard_False);  // displayBody activates the current filter itself
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetDisplayMode(AIS_Shaded);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetDisplayMode(AIS_Shaded);

  m_view = m_viewer->CreateView();
#if defined(_WIN32)
  Handle(WNT_Window) win = new WNT_Window(reinterpret_cast<Aspect_Handle>(winId()));
#elif defined(__APPLE__)
  Handle(Aspect_Window) win = opad_make_cocoa_window(reinterpret_cast<void*>(winId()));
#else
  Handle(Xw_Window) win = new Xw_Window(disp, static_cast<Window>(winId()));
#endif
  m_view->SetWindow(win);
  if (!win->IsMapped()) win->Map();
  m_view->ChangeRenderingParams().NbMsaaSamples = 4;
  m_view->ChangeRenderingParams().RenderResolutionScale = 1.0f;
  m_view->SetImmediateUpdate(Standard_False);
  m_view->Camera()->SetProjectionType(Graphic3d_Camera::Projection_Orthographic);
  m_view->SetProj(V3d_XposYnegZpos);

  m_cube = new NavCube();  // a plain cube whose edges and corners are still hover/click targets
  m_cube->SetSize(58);
  m_cube->SetFontHeight(11);
  m_cube->SetAxesLabels("X", "Y", "Z");
  m_cube->SetBoxSideLabel(V3d_Zpos, "TOP");
  m_cube->SetBoxSideLabel(V3d_Zneg, "BOTTOM");
  m_cube->SetBoxSideLabel(V3d_Yneg, "FRONT");
  m_cube->SetBoxSideLabel(V3d_Ypos, "BACK");
  m_cube->SetBoxSideLabel(V3d_Xpos, "RIGHT");
  m_cube->SetBoxSideLabel(V3d_Xneg, "LEFT");
  m_cube->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER, Graphic3d_Vec2i(kCubeOffsetX, kCubeOffsetY)));
  m_cube->SetViewAnimation(myViewAnimation);
  m_cube->SetFixedAnimationLoop(Standard_False);
  m_cube->SetAutoStartAnimation(Standard_True);
  m_ctx->Display(m_cube, Standard_False);
  m_ctx->Load(m_cube, -1);  // register with the selection manager: Display() with auto-activation off does not
  m_ctx->Activate(m_cube, 0);

  SetRotationMode(AIS_RotationMode_BndBoxActive);
  SetLockOrbitZUp(Standard_True);
  SetAllowRotation(Standard_True);
  SetAllowPanning(Standard_True);
  SetAllowZooming(Standard_True);
  SetAllowZFocus(Standard_False);
  SetAllowDragging(Standard_False);
  setNavPreset(m_preset);
  // OCCT counts a press/release pair as a click only within 3 device pixels; a click that drifts more becomes
  // a rubber band that selects nothing. Allow a little hand jitter, scaled for high-DPI screens.
  myMouseClickThreshold = 5.0 * devicePixelRatioF();
  m_initialised = true;
  applyTokens();
  sync();
}

// ---------------------------------------------------------------- tokens
void Viewport::setTokens(const Tokens& t) {
  m_tokens = t;
  if (m_initialised) applyTokens();
}

void Viewport::applyTokens() {
  const Tokens& t = m_tokens;
  m_view->SetBackgroundColor(occ(t.vp));
  m_view->SetBgGradientStyle(Aspect_GradientFillMethod_None);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetColor(occ(t.hov));
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic)->SetColor(occ(t.hov));
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Dynamic)->SetTransparency(0.35f);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalDynamic)->SetTransparency(0.35f);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetColor(occ(t.sel));
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalSelected)->SetColor(occ(t.sel));
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_Selected)->SetTransparency(0.5f);
  m_ctx->HighlightStyle(Prs3d_TypeOfHighlight_LocalSelected)->SetTransparency(0.35f);
  // View cube per the design: flat three-tone box with dark labels, thin X/Y/Z axes in red/green/blue along
  // the lower edges, and the hovered face/edge/corner filled with the hover accent to show where a click goes.
  m_cube->SetBoxColor(occ(t.mtop));
  m_cube->BoxSideStyle()->SetColor(occ(t.mtop));
  m_cube->BoxEdgeStyle()->SetColor(occ(t.mtop));    // edge and corner bands are drawn on the faces: same colour = invisible
  m_cube->BoxCornerStyle()->SetColor(occ(t.mtop));
  m_cube->SetTextColor(occ(t.medge));
  m_cube->SetInnerColor(occ(t.mleft));
  m_cube->SetBoxTransparency(0.0);
  m_cube->SetSize(64);
  m_cube->SetRoundRadius(0.0);  // a plain cube: no bevelled edges or corners
  m_cube->SetBoxFacetExtension(0.0);
  m_cube->SetBoxEdgeGap(0.0);
  m_cube->SetBoxEdgeMinSize(0.0);
  m_cube->SetBoxCornerMinSize(0.0);
  m_cube->Attributes()->SetFaceBoundaryDraw(Standard_True);  // crisp edges between the faces, as in the design
  m_cube->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(t.medge), Aspect_TOL_SOLID, 1.0));
  m_cube->SetDrawAxes(Standard_True);
  m_cube->SetAxesPadding(6);
  m_cube->SetAxesRadius(0.6);
  m_cube->SetAxesConeRadius(1.2);
  m_cube->SetAxesSphereRadius(1.0);
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
  m_cube->DynamicHilightAttributes()->ShadingAspect()->SetColor(occ(t.hov));
  m_cube->DynamicHilightAttributes()->ShadingAspect()->SetTransparency(0.3f);
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
  m_preset = p;
  AIS_MouseGestureMap& map = ChangeMouseGestureMap();
  map.Clear();
  const unsigned L = Aspect_VKeyMouse_LeftButton, M = Aspect_VKeyMouse_MiddleButton, R = Aspect_VKeyMouse_RightButton;
  const unsigned SHIFT = Aspect_VKeyFlags_SHIFT, CTRL = Aspect_VKeyFlags_CTRL;
  map.Bind(L, AIS_MouseGesture_SelectRectangle);
  map.Bind(L | CTRL, AIS_MouseGesture_SelectRectangle);
  map.Bind(L | SHIFT, AIS_MouseGesture_SelectRectangle);
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
  }
}

// ---------------------------------------------------------------- display styles (F19)
void Viewport::applyStyle(const Handle(AIS_Shape)& ais) {
  Handle(Prs3d_Drawer) d = ais->Attributes();
  d->SetFaceBoundaryDraw(m_style == Style::ShadedEdges);
  d->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.medge), Aspect_TOL_SOLID, 1.0));
  m_ctx->SetDisplayMode(ais, m_style == Style::Wireframe ? AIS_WireFrame : AIS_Shaded, Standard_False);
}

void Viewport::setStyle(Style s) {
  m_style = s;
  if (!m_initialised) return;
  for (auto& [id, it] : m_items) {
    applyStyle(it.ais);
    m_ctx->Redisplay(it.ais, Standard_False);
  }
  redrawScene();
}

void Viewport::setGrid(bool on) {
  m_grid = on;
  if (!m_initialised) return;
  if (on) m_viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
  else m_viewer->DeactivateGrid();
  redrawScene();
}

void Viewport::setShadows(bool on) {
  if (!m_initialised) return;
  for (V3d_ListOfLightIterator it = m_viewer->ActiveLightIterator(); it.More(); it.Next())
    if (it.Value()->Type() == Graphic3d_TypeOfLightSource_Directional) it.Value()->SetCastShadows(on);
  m_view->ChangeRenderingParams().ShadowMapResolution = on ? 2048 : 1024;
  redrawScene();
}

void Viewport::setOrthographic(bool ortho) {
  if (!m_initialised) return;
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
  TopAbs_ShapeEnum t = TopAbs_SHAPE;
  switch (m_filter) {
    case SelFilter::Body: t = TopAbs_SHAPE; break;
    case SelFilter::Face: t = TopAbs_FACE; break;
    case SelFilter::Edge: t = TopAbs_EDGE; break;
    case SelFilter::Vertex: t = TopAbs_VERTEX; break;
  }
  m_ctx->Activate(ais, AIS_Shape::SelectionMode(t));
}

void Viewport::setSelectionFilter(SelFilter f) {
  m_filter = f;
  if (!m_initialised) return;
  clearSelection();
  // Activating a selection mode builds that mode's sensitive entities per object (faces/edges), which is
  // slow across a big assembly, so it runs as a sliced job.
  if (m_filterJob) m_filterJob->cancel();
  auto items = std::make_shared<std::vector<Handle(AIS_Shape)>>();
  for (const auto& [id, it] : m_items) items->push_back(it.ais);
  auto i = std::make_shared<size_t>(0);
  m_filterJob = m_jobs->sliced(tr("Switching selection mode"), [this, items, i](Job&) {
    if (*i >= items->size()) return false;
    activateSelection((*items)[(*i)++]);
    return *i < items->size();
  }, [this](bool) {
    m_filterJob = nullptr;
    redrawScene();
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
    auto it = m_nodeOf.find(obj.get());
    if (it == m_nodeOf.end()) continue;
    opad::Ref r;
    r.body = it->second;
    Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->SelectedOwner());
    if (!owner.IsNull() && owner->HasShape() && m_filter != SelFilter::Body) {
      const TopoDS_Shape& sub = owner->Shape();
      const Item& item = m_items.at(it->second);
      switch (sub.ShapeType()) {
        case TopAbs_FACE: r.kind = opad::Ref::Kind::Face; break;
        case TopAbs_EDGE: r.kind = opad::Ref::Kind::Edge; break;
        case TopAbs_VERTEX: r.kind = opad::Ref::Kind::Vertex; break;
        default: break;
      }
      if (r.kind != opad::Ref::Kind::Body) r.index = opad::subshape_index(item.located, sub);
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
    s->SetColor(occ(m_tokens.sel));
    s->SetTransparency(0.7f);
    s->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
    s->Attributes()->SetFaceBoundaryDraw(Standard_True);
    s->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(occ(m_tokens.sel), Aspect_TOL_SOLID, 1.5));
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
  m_notifyWhenApplied = true;  // selectionChanged fires once the (possibly sliced) un-highlight has settled
  selectNodes({});
}

// A click in the 3D view: OCCT has already changed the context selection; drop any stand-ins and in-flight job.
void Viewport::OnSelectionChanged(const Handle(AIS_InteractiveContext)&, const Handle(V3d_View)&) {
  if (m_selJob) m_selJob->cancel();
  clearShade();
  m_needFit = false;
  redrawScene();
  if (trace::enabled()) trace::log(QStringLiteral("3D click: %1 selected in context").arg(m_ctx->NbSelected()));
  emit selectionChanged();
}

void Viewport::isolate(const std::vector<std::string>& ids) {
  m_isolated.clear();
  for (const auto& id : ids)
    for (const auto& b : m_doc->scene.bodies_under(id)) m_isolated.insert(b);
  m_needFit = !m_isolated.empty();
  sync();
  if (!m_isolated.empty()) fitAll();
  emit isolationChanged();
}

// ---------------------------------------------------------------- camera (F17/F18)
void Viewport::fitAll() {
  if (!m_initialised) return;
  m_view->FitAll(0.02, Standard_False);
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::fitWhenReady() {
  m_needFit = true;
  if (!m_items.empty()) fitAll();
}

void Viewport::cancelMeshing() {
  *m_meshCancel = true;
  m_meshCancel = std::make_shared<std::atomic<bool>>(false);
}

void Viewport::resetMeshing() {
  std::lock_guard<std::mutex> lock(m_meshMu);
  m_meshSkipped.clear();
}

void Viewport::fitNodes(const std::vector<std::string>& ids) {
  if (!m_initialised) return;
  m_needFit = false;
  Bnd_Box box;
  for (const auto& id : ids)
    for (const auto& b : m_doc->scene.bodies_under(id))
      if (m_items.count(b)) box.Add(opad::node_world_bbox(m_doc->doc, m_doc->scene, b));
  if (trace::enabled()) { double a, b, c, d, e, f; if (!box.IsVoid()) box.Get(a, b, c, d, e, f); trace::log(QStringLiteral("fitNodes: box void=%1 [%2 %3 %4]-[%5 %6 %7]").arg(box.IsVoid()).arg(a).arg(b).arg(c).arg(d).arg(e).arg(f)); }
  if (box.IsVoid()) return fitAll();
  m_view->FitAll(box, 0.02, Standard_False);
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::fitSelection() {
  if (!m_initialised) return;
  m_needFit = false;
  Bnd_Box box;
  for (const auto& b : m_shadeBodies) box.Add(opad::node_world_bbox(m_doc->doc, m_doc->scene, b));
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) {
    Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->SelectedOwner());
    auto it = m_nodeOf.find(m_ctx->SelectedInteractive().get());
    if (!owner.IsNull() && owner->HasShape() && m_filter != SelFilter::Body) {
      TopoDS_Shape sub = owner->Shape();
      Handle(AIS_InteractiveObject) obj = m_ctx->SelectedInteractive();
      if (!obj.IsNull() && obj->HasTransformation()) sub = sub.Moved(TopLoc_Location(obj->LocalTransformation()));
      BRepBndLib::Add(sub, box, Standard_True);
    }
    else if (it != m_nodeOf.end()) box.Add(opad::node_world_bbox(m_doc->doc, m_doc->scene, it->second));
  }
  if (trace::enabled()) { double a = 0, b = 0, c = 0, d = 0, e = 0, f = 0; if (!box.IsVoid()) box.Get(a, b, c, d, e, f); trace::log(QStringLiteral("fitSelection: box void=%1 [%2 %3 %4]-[%5 %6 %7]").arg(box.IsVoid()).arg(a).arg(b).arg(c).arg(d).arg(e).arg(f)); }
  if (box.IsVoid()) return fitAll();
  m_view->FitAll(box, 0.02, Standard_False);
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::standardView(const QString& name) {
  if (!m_initialised) return;
  m_needFit = false;
  V3d_TypeOfOrientation o = V3d_XposYnegZpos;
  if (name == "top") o = V3d_Zpos;
  else if (name == "bottom") o = V3d_Zneg;
  else if (name == "front") o = V3d_Yneg;
  else if (name == "back") o = V3d_Ypos;
  else if (name == "right") o = V3d_Xpos;
  else if (name == "left") o = V3d_Xneg;
  else if (name == "iso-back") o = V3d_XnegYposZpos;
  m_view->SetProj(o);
  fitAll();
}

void Viewport::home() { standardView("iso"); }

void Viewport::rollView(double degrees) {
  if (!m_initialised) return;
  m_needFit = false;
  Handle(Graphic3d_Camera) cam = m_view->Camera();
  Handle(Graphic3d_Camera) start = new Graphic3d_Camera(*cam), end = new Graphic3d_Camera(*cam);
  gp_Dir up = cam->Up();
  up.Rotate(gp_Ax1(gp::Origin(), cam->Direction()), degrees * M_PI / 180.0);  // about the axis into the screen
  end->SetUp(up);
  myViewAnimation->SetView(m_view);
  myViewAnimation->SetCameraStart(start);
  myViewAnimation->SetCameraEnd(end);
  myViewAnimation->SetOwnDuration(0.25);
  myViewAnimation->StartTimer(0.0, 1.0, Standard_True);  // the 16 ms timer redraws while it runs
  requestRedraw();
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
  m_needFit = false;
  opad::Camera cam = opad::Camera::from_json(j);
  Handle(Graphic3d_Camera) c = m_view->Camera();
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

// ---------------------------------------------------------------- section (F20)
void Viewport::setSection(bool enabled, const opad::Vec3& origin, const opad::Vec3& normal, bool caps) {
  m_sectionEnabled = enabled;
  m_sectionOrigin = origin;
  m_sectionNormal = normal;
  m_sectionCaps = caps;
  updateClipPlanes();
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
void Viewport::clearDimension() {
  if (!m_initialised) return;
  for (const auto& o : m_dimension) m_ctx->Remove(o, Standard_False);
  m_dimension.clear();
  redrawScene();
}

void Viewport::showDimension(const opad::Vec3& a, const opad::Vec3& b, const QString& label) {
  if (!m_initialised) return;
  clearDimension();
  gp_Pnt pa(a[0], a[1], a[2]), pb(b[0], b[1], b[2]);
  if (pa.Distance(pb) > 1e-9) {
    Handle(AIS_Shape) line = new AIS_Shape(BRepBuilderAPI_MakeEdge(pa, pb).Edge());
    line->SetColor(occ(m_tokens.sel));
    line->SetWidth(1.5);
    line->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(line, Standard_False);
    m_ctx->Deactivate(line);
    m_dimension.push_back(line);
  }
  for (const gp_Pnt& p : {pa, pb}) {
    Handle(AIS_Shape) v = new AIS_Shape(BRepBuilderAPI_MakeVertex(p).Vertex());
    v->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_O, occ(m_tokens.sel), 3.0));
    v->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(v, Standard_False);
    m_ctx->Deactivate(v);
    m_dimension.push_back(v);
  }
  Handle(AIS_TextLabel) text = new AIS_TextLabel();
  text->SetText(TCollection_ExtendedString(label.toStdString().c_str(), Standard_True));
  text->SetPosition(gp_Pnt((pa.X() + pb.X()) / 2, (pa.Y() + pb.Y()) / 2, (pa.Z() + pb.Z()) / 2));
  text->SetHeight(12);
  text->SetColor(occ(m_tokens.sel));
  text->SetDisplayType(Aspect_TODT_SUBTITLE);
  text->SetColorSubTitle(occ(m_tokens.bg2));
  text->SetZLayer(Graphic3d_ZLayerId_Topmost);
  m_ctx->Display(text, Standard_False);
  m_ctx->Deactivate(text);
  m_dimension.push_back(text);
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
}  // namespace

double Viewport::deflectionFor(const std::string& key) { return deflectionForBox(opad::body_bbox(m_doc->doc, key)); }

// Tessellation runs off the UI thread (F21); bodies appear once their mesh is ready.
void Viewport::startMeshing(std::vector<std::string> keys) {
  struct MeshJob { TopoDS_Shape shape; std::string key; };
  std::vector<MeshJob> jobs;
  {
    std::lock_guard<std::mutex> lock(m_meshMu);
    for (const auto& k : keys) {
      if (m_meshed.count(k) || m_meshing.count(k) || m_meshSkipped.count(k)) continue;
      m_meshing.insert(k);
      jobs.push_back({opad::body_shape(m_doc->doc, k), k});
    }
  }
  if (jobs.empty()) return;
  emit meshingProgress(static_cast<int>(m_meshing.size()));
  auto alive = m_alive;
  auto cancel = m_meshCancel;
  auto cache = m_doc->doc.shape_cache;  // the worker fills the bbox cache too, so later UI queries are O(1)
  std::thread([this, alive, cancel, cache, jobs = std::move(jobs)]() {
    for (size_t i = 0; i < jobs.size(); ++i) {
      const auto& j = jobs[i];
      if (*cancel) {
        std::lock_guard<std::mutex> lock(m_meshMu);
        for (size_t k = i; k < jobs.size(); ++k) {
          m_meshing.erase(jobs[k].key);
          if (m_activeCache == cache.get()) m_meshSkipped.insert(jobs[k].key);
        }
        QMetaObject::invokeMethod(this, "requestSync", Qt::QueuedConnection);
        return;
      }
      std::shared_ptr<BodyPrs> prs;
      try {
        const Bnd_Box box = opad::body_bbox(*cache, j.key, j.shape);
        BRepMesh_IncrementalMesh(j.shape, deflectionForBox(box), Standard_False, 20.0 * M_PI / 180.0, Standard_True);
        prs = BodyPrs::build(j.shape, box);  // the shaded presentation, so Display() on the UI thread is cheap
      } catch (...) {
      }
      if (!*alive) return;
      {
        std::lock_guard<std::mutex> lock(m_meshMu);
        m_meshing.erase(j.key);
        if (m_activeCache == cache.get()) {  // a newer document owns the bookkeeping otherwise
          m_meshed.insert(j.key);
          if (prs) m_prs[j.key] = std::move(prs);
        }
      }
      QMetaObject::invokeMethod(this, "requestSync", Qt::QueuedConnection);
    }
  }).detach();
}

void Viewport::benchPick() {
  if (!m_initialised) return;
  m_view->Redraw();  // a frame first: the picker clips to the camera z range, which only Redraw (AutoZFit) updates
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  m_ctx->MoveTo(w / 2, h / 2, m_view, Standard_False);
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
  trace::log(QStringLiteral("bench: pick at view centre detected %1; click selected %2").arg(hit).arg(m_ctx->NbSelected()));
}

// Creates the OpenGL viewer ahead of the first document (about 0.7 s) so that opening a file does not pay
// for it. The native child window exists while hidden, which is all OCCT needs.
void Viewport::warmUp() {
  if (m_initialised) return;
  try {
    initViewer();
    m_view->Redraw();  // first frame compiles the shaders (~0.3 s); better here than when the document appears
  } catch (const Standard_Failure&) {  // no context yet: paintEvent will try again once visible
  }
}

void Viewport::requestSync() {
  if (!m_syncTimer.isActive()) m_syncTimer.start();
}

// Reconciles the context with the scene. Removals and attribute changes are applied at once (cheap);
// bodies to display are added by a sliced job because computing a body's presentation and selection
// entities is the expensive part and must not block the UI (see Jobs.hpp).
void Viewport::sync() {
  if (!m_initialised || m_doc->loading) return;
  trace::Scope scope("Viewport::sync");
  const opad::Scene& scene = m_doc->scene;
  {
    // Mesh bookkeeping is per shape cache: a new document means new TopoDS_Shapes without triangulation.
    std::lock_guard<std::mutex> lock(m_meshMu);
    const void* cache = m_doc->doc.shape_cache.get();
    if (cache != m_activeCache) {
      m_activeCache = cache;
      m_meshed.clear();
      m_meshSkipped.clear();
      m_prs.clear();
    }
  }
  if (!m_isolated.empty()) {  // the mode ends by itself once every isolated object is gone (deleted)
    bool any = false;
    for (const auto& id : m_isolated) {
      const opad::Node* n = scene.node(id);
      if (n && !n->body_missing) { any = true; break; }
    }
    if (!any) {
      m_isolated.clear();
      emit isolationChanged();
    }
  }
  std::set<std::string> keep, replace;
  std::vector<std::string> pending, toAdd;
  for (const auto& id : scene.all_bodies()) {
    const opad::Node* n = scene.node(id);
    if (!n || n->body_missing) continue;
    // Isolate mode shows exactly the isolated set and ignores visibility flags; otherwise the flags rule.
    if (!m_isolated.empty() ? !m_isolated.count(id) : !scene.effectively_visible(id)) continue;
    auto it = m_items.find(id);
    if (it != m_items.end() && it->second.key == n->body_key && it->second.world.m == scene.world(id).m) {
      keep.insert(id);
      Item& item = it->second;
      if (item.color != n->color || item.opacity != n->opacity) {
        item.color = n->color;
        item.opacity = n->opacity;
        item.ais->SetColor(qcolor(n->color));
        item.ais->SetTransparency(1.0 - n->opacity);
        m_ctx->Redisplay(item.ais, Standard_False);
      }
      continue;
    }
    bool meshed;
    {
      std::lock_guard<std::mutex> lock(m_meshMu);
      meshed = m_meshed.count(n->body_key) > 0;
    }
    if (!meshed) {
      pending.push_back(n->body_key);
      continue;
    }
    keep.insert(id);
    if (it != m_items.end()) replace.insert(id);
    toAdd.push_back(id);
  }
  for (auto it = m_items.begin(); it != m_items.end();) {
    if (keep.count(it->first) && !replace.count(it->first)) { ++it; continue; }
    m_ctx->Remove(it->second.ais, Standard_False);
    m_nodeOf.erase(it->second.ais.get());
    it = m_items.erase(it);
  }
  if (!pending.empty()) startMeshing(pending);
  updateAnnotations();
  updateClipPlanes();
  if (m_displayJob) m_displayJob->cancel();
  const int pendingCount = static_cast<int>(pending.size());
  if (toAdd.empty()) {
    finishSync(pendingCount, false);
    return;
  }
  emit meshingProgress(pendingCount + static_cast<int>(toAdd.size()));
  auto ids = std::make_shared<std::vector<std::string>>(std::move(toAdd));
  auto i = std::make_shared<size_t>(0);
  m_displayJob = m_jobs->sliced(tr("Displaying %1 bodies").arg(ids->size()), [this, ids, i, pendingCount](Job& j) {
    if (*i >= ids->size() || m_doc->loading) return false;
    displayBody((*ids)[*i]);
    if ((++*i & 15) == 0) {
      j.setPhase(tr("Displaying %1 bodies").arg(ids->size()), static_cast<int>(*i * 100 / ids->size()));
      emit meshingProgress(pendingCount + static_cast<int>(ids->size() - *i));
      m_view->Invalidate();
      requestRedraw();  // bodies appear as they are added
    }
    return *i < ids->size();
  }, [this, pendingCount](bool completed) {
    m_displayJob = nullptr;
    if (completed) finishSync(pendingCount, true);
  });
}

// Adds one body to the context: presentation + selection entities are computed here.
void Viewport::displayBody(const std::string& id) {
  const opad::Scene& scene = m_doc->scene;
  const opad::Node* n = scene.node(id);
  if (!n || n->body_missing || m_items.count(id)) return;  // the scene moved on since this was queued
  QElapsedTimer t;
  t.start();
  opad::Mat4 world = scene.world(id);
  TopoDS_Shape proto = opad::body_shape(m_doc->doc, n->body_key);
  std::shared_ptr<BodyPrs> prs;
  {
    std::lock_guard<std::mutex> lock(m_meshMu);
    auto p = m_prs.find(n->body_key);
    if (p != m_prs.end()) prs = p->second;
  }
  // Rigid placements go on the object as a local transformation, so the prototype's precomputed arrays
  // (and sub-shape ordinals) are shared by every instance; anything else gets a transformed copy.
  TopoDS_Shape located = proto;
  const bool rigid = world.is_identity() || opad::mat_is_rigid(world);
  if (!rigid) {
    located = opad::node_world_shape(m_doc->doc, scene, id);
    prs.reset();
  }
  Handle(AIS_Shape) ais = new BodyShape(located, prs);
  if (rigid && !world.is_identity()) ais->SetLocalTransformation(opad::trsf_from_mat(world));
  ais->Attributes()->SetTypeOfDeflection(Aspect_TOD_ABSOLUTE);
  ais->Attributes()->SetMaximalChordialDeviation(deflectionFor(n->body_key));
  ais->Attributes()->SetDeviationAngle(20.0 * M_PI / 180.0);
  // Never let OCCT (re)mesh on the UI thread: with auto-triangulation on, building the selection entities
  // re-runs BRepMesh for any face whose mesh is missing or coarser than asked, which froze the app for
  // minutes on big bodies. Meshing happens once, on the worker; unmeshed faces get box sensitives.
  ais->Attributes()->SetAutoTriangulation(Standard_False);
  ais->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
  ais->SetColor(qcolor(n->color));
  if (n->opacity < 1.0) ais->SetTransparency(1.0 - n->opacity);
  applyStyle(ais);
  m_ctx->Display(ais, m_style == Style::Wireframe ? AIS_WireFrame : AIS_Shaded, -1, Standard_False);  // selection activated below, once
  const qint64 displayMs = t.elapsed();
  activateSelection(ais);
  if (trace::enabled() && t.elapsed() > 50) trace::log(QStringLiteral("displayBody %1: display %2 ms, selection %3 ms").arg(QString::fromStdString(n->name)).arg(displayMs).arg(t.elapsed() - displayMs));
  m_items[id] = Item{ais, n->body_key, world, n->color, n->opacity, located};
  m_nodeOf[ais.get()] = id;
}

void Viewport::finishSync(int pendingCount, bool added) {
  emit meshingProgress(pendingCount);
  // Keep fitting while a load is still streaming bodies in, but only until the user moves the camera:
  // every fit, orbit or zoom of theirs clears m_needFit so a later batch never snaps the view back.
  if (added && (m_needFit || m_items.size() <= 1)) m_view->FitAll(0.02, Standard_False);
  if (pendingCount == 0) m_needFit = false;
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::updateAnnotations() {
  if (!m_initialised) return;
  for (const auto& l : m_labels) m_ctx->Remove(l, Standard_False);
  m_labels.clear();
  for (const auto& a : m_doc->scene.annotations) {
    gp_Pnt at(a.anchor.point[0], a.anchor.point[1], a.anchor.point[2]);
    if (a.unresolved) continue;
    if (a.anchor.kind != opad::Ref::Kind::Point) {
      try {
        opad::json info = opad::inspect_ref(m_doc->doc, m_doc->scene, a.anchor);
        opad::json c = info.contains("center") ? info["center"] : info.contains("point") ? info["point"] : info.contains("start") ? info["start"] : info["bbox"]["center"];
        at = gp_Pnt(c[0].get<double>(), c[1].get<double>(), c[2].get<double>());
      } catch (const std::exception&) {
        continue;
      }
    }
    // Amber anchor dot with a bg2 ring, then the note text (author in parentheses).
    Handle(AIS_Shape) dot = new AIS_Shape(BRepBuilderAPI_MakeVertex(at).Vertex());
    dot->Attributes()->SetPointAspect(new Prs3d_PointAspect(Aspect_TOM_BALL, occ(m_tokens.amber), 5.0));
    dot->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(dot, Standard_False);
    m_ctx->Deactivate(dot);
    m_labels.push_back(dot);
    Handle(AIS_TextLabel) label = new AIS_TextLabel();
    std::string text = a.text.size() > 48 ? a.text.substr(0, 45) + "..." : a.text;
    label->SetText(TCollection_ExtendedString(("   " + text + "  (" + a.by + ")").c_str(), Standard_True));
    label->SetPosition(at);
    label->SetHeight(13);
    label->SetColor(occ(m_tokens.amber));
    label->SetDisplayType(Aspect_TODT_SUBTITLE);
    label->SetColorSubTitle(occ(m_tokens.bg2));
    label->SetZLayer(Graphic3d_ZLayerId_Topmost);
    m_ctx->Display(label, Standard_False);
    m_ctx->Deactivate(label);
    m_labels.push_back(label);
  }
}

// ---------------------------------------------------------------- Qt events
Graphic3d_Vec2i Viewport::devicePos(const QPointF& p) const {
  const double s = devicePixelRatioF();
  return Graphic3d_Vec2i(static_cast<int>(p.x() * s), static_cast<int>(p.y() * s));
}

void Viewport::showEvent(QShowEvent* e) {
  QWidget::showEvent(e);
  if (!m_initialised) {
    m_needFit = true;
    initViewer();
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
  // Compare against what OCCT was last told, not the HWND (Qt keeps that in sync, so it always
  // matched and MustBeResized never fired -> the view kept its stale startup size in a corner).
  if (wantW == m_lastSyncedSize.first && wantH == m_lastSyncedSize.second) return;
  m_lastSyncedSize = {wantW, wantH};
  if (QWindow* native = windowHandle()) native->resize(size());
  m_view->MustBeResized();
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::paintEvent(QPaintEvent*) {
  if (!m_initialised) initViewer();
  syncWindowSize();
  QElapsedTimer frame;
  frame.start();
  FlushViewEvents(m_ctx, m_view, Standard_True);
  if (trace::enabled() && frame.elapsed() > 100) trace::log(QStringLiteral("slow frame: %1 ms (%2 objects)").arg(frame.elapsed()).arg(m_items.size()));
  QString hover;
  if (m_ctx->HasDetected()) {
    Handle(AIS_InteractiveObject) obj = m_ctx->DetectedInteractive();
    auto it = m_nodeOf.find(obj.get());
    if (it != m_nodeOf.end()) {
      hover = m_doc->nodeName(it->second);
      Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->DetectedOwner());
      if (!owner.IsNull() && owner->HasShape() && m_filter != SelFilter::Body) {
        const TopoDS_Shape& sub = owner->Shape();
        const char* kind = sub.ShapeType() == TopAbs_FACE ? "face" : sub.ShapeType() == TopAbs_EDGE ? "edge" : "vertex";
        hover += QString::fromUtf8(" › %1 %2").arg(kind).arg(opad::subshape_index(m_items.at(it->second).located, sub));
      }
    }
  }
  if (hover != m_hover) {
    m_hover = hover;
    emit hoverChanged(hover);
  }
}

void Viewport::resizeEvent(QResizeEvent*) {
  if (!m_initialised) return;
  m_view->MustBeResized();
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::mousePressEvent(QMouseEvent* e) {
  setFocus();
  m_pressPos = e->pos();
  m_rightPress = e->button() == Qt::RightButton;
  // A left press on the view cube: dragging orbits the view (the cube turns with it); a click without movement
  // still goes through the controller's click path and snaps to the picked side.
  if (m_initialised && e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier && m_ctx->HasDetected() && m_ctx->DetectedInteractive() == m_cube) {
    ChangeMouseGestureMap().Bind(Aspect_VKeyMouse_LeftButton, AIS_MouseGesture_RotateOrbit);
    m_cubeGesture = true;
  }
  if (m_initialised && UpdateMouseButtons(devicePos(e->position()), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
  if (m_initialised && UpdateMouseButtons(devicePos(e->position()), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
  if (m_cubeGesture && e->button() == Qt::LeftButton) {
    m_cubeGesture = false;
    ChangeMouseGestureMap().Bind(Aspect_VKeyMouse_LeftButton, AIS_MouseGesture_SelectRectangle);
  }
  if (m_rightPress && e->button() == Qt::RightButton && (e->pos() - m_pressPos).manhattanLength() < 4) {
    m_rightPress = false;
    emit contextMenuRequested(e->globalPosition().toPoint());
  }
}

void Viewport::mouseMoveEvent(QMouseEvent* e) {
  if (e->buttons() != Qt::NoButton) m_needFit = false;  // a drag: the user owns the camera now
  if (m_initialised && UpdateMousePosition(devicePos(e->position()), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
}

void Viewport::wheelEvent(QWheelEvent* e) {
  if (!m_initialised) return;
  m_needFit = false;
  const double delta = e->angleDelta().y() / 8.0;
  if (UpdateZoom(Aspect_ScrollDelta(devicePos(e->position()), delta))) requestRedraw();
}
