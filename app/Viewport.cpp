#include "Viewport.hpp"

#include <functional>

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
#include <V3d_Trihedron.hxx>
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
  m_timer.setInterval(16);
  connect(&m_timer, &QTimer::timeout, this, [this] {
    if (!m_initialised) return;
    if (!myViewAnimation.IsNull() && !myViewAnimation->IsStopped()) requestRedraw();
    else if (toAskNextFrame()) requestRedraw();
  });
  m_timer.start();
}

Viewport::~Viewport() { *m_alive = false; }

void Viewport::initViewer() {
  if (m_initialised) return;
  Handle(Aspect_DisplayConnection) disp = new Aspect_DisplayConnection();
  Handle(OpenGl_GraphicDriver) driver = new OpenGl_GraphicDriver(disp, Standard_False);
  driver->ChangeOptions().buffersNoSwap = Standard_False;
  driver->ChangeOptions().ffpEnable = Standard_False;
  m_viewer = new V3d_Viewer(driver);
  m_viewer->SetDefaultLights();
  m_viewer->SetLightOn();
  m_ctx = new AIS_InteractiveContext(m_viewer);
  m_ctx->SetPixelTolerance(4);
  m_ctx->SetAutoActivateSelection(Standard_True);
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

  m_cube = new AIS_ViewCube();
  m_cube->SetSize(58);
  m_cube->SetFontHeight(11);
  m_cube->SetAxesLabels("X", "Y", "Z");
  m_cube->SetBoxSideLabel(V3d_Zpos, "TOP");
  m_cube->SetBoxSideLabel(V3d_Zneg, "BOTTOM");
  m_cube->SetBoxSideLabel(V3d_Yneg, "FRONT");
  m_cube->SetBoxSideLabel(V3d_Ypos, "BACK");
  m_cube->SetBoxSideLabel(V3d_Xpos, "RIGHT");
  m_cube->SetBoxSideLabel(V3d_Xneg, "LEFT");
  m_cube->SetTransformPersistence(new Graphic3d_TransformPers(Graphic3d_TMF_TriedronPers, Aspect_TOTP_RIGHT_UPPER, Graphic3d_Vec2i(80, 76)));
  m_cube->SetViewAnimation(myViewAnimation);
  m_cube->SetFixedAnimationLoop(Standard_False);
  m_cube->SetAutoStartAnimation(Standard_True);
  m_ctx->Display(m_cube, Standard_False);

  SetRotationMode(AIS_RotationMode_BndBoxActive);
  SetLockOrbitZUp(Standard_True);
  SetAllowRotation(Standard_True);
  SetAllowPanning(Standard_True);
  SetAllowZooming(Standard_True);
  SetAllowZFocus(Standard_False);
  SetAllowDragging(Standard_False);
  setNavPreset(m_preset);
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
  // View cube: flat three-tone box (m-top sides, m-left edges, m-right corners), fg labels, no axes.
  m_cube->SetBoxColor(occ(t.mtop));
  m_cube->BoxSideStyle()->SetColor(occ(t.mtop));
  m_cube->BoxEdgeStyle()->SetColor(occ(t.mleft));
  m_cube->BoxCornerStyle()->SetColor(occ(t.mright));
  m_cube->SetTextColor(occ(t.dark ? t.medge : t.medge));
  m_cube->SetInnerColor(occ(t.mleft));
  m_cube->SetBoxTransparency(0.0);
  m_cube->SetDrawAxes(Standard_False);
  m_cube->SetSize(36);
  m_ctx->Redisplay(m_cube, Standard_False);
  // Axis triad bottom-left: fg3 arms, fg2 labels.
  m_view->TriedronDisplay(Aspect_TOTP_LEFT_LOWER, occ(t.fg3), 0.06, V3d_ZBUFFER);
  Handle(V3d_Trihedron) tri = m_view->Trihedron();
  if (!tri.IsNull()) {
    tri->SetArrowsColor(occ(t.fg3), occ(t.fg3), occ(t.fg3));
    tri->SetLabelsColor(occ(t.fg2));
  }
  setStyle(m_style);
  updateAnnotations();
  updateClipPlanes();
  requestRedraw();
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
  requestRedraw();
}

void Viewport::setGrid(bool on) {
  m_grid = on;
  if (!m_initialised) return;
  if (on) m_viewer->ActivateGrid(Aspect_GT_Rectangular, Aspect_GDM_Lines);
  else m_viewer->DeactivateGrid();
  requestRedraw();
}

void Viewport::setShadows(bool on) {
  if (!m_initialised) return;
  for (V3d_ListOfLightIterator it = m_viewer->ActiveLightIterator(); it.More(); it.Next())
    if (it.Value()->Type() == Graphic3d_TypeOfLightSource_Directional) it.Value()->SetCastShadows(on);
  m_view->ChangeRenderingParams().ShadowMapResolution = on ? 2048 : 1024;
  requestRedraw();
}

void Viewport::setOrthographic(bool ortho) {
  if (!m_initialised) return;
  m_view->Camera()->SetProjectionType(ortho ? Graphic3d_Camera::Projection_Orthographic : Graphic3d_Camera::Projection_Perspective);
  m_view->Invalidate();
  requestRedraw();
}

bool Viewport::isOrthographic() const {
  return !m_initialised || m_view->Camera()->ProjectionType() == Graphic3d_Camera::Projection_Orthographic;
}

// ---------------------------------------------------------------- selection (F22)
void Viewport::activateSelection(const Handle(AIS_Shape)& ais) {
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
  m_ctx->ClearSelected(Standard_False);
  for (auto& [id, it] : m_items) activateSelection(it.ais);
  requestRedraw();
  emit selectionChanged();
}

std::vector<opad::Ref> Viewport::selection() const {
  std::vector<opad::Ref> out;
  if (!m_initialised) return out;
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

void Viewport::selectNodes(const std::vector<std::string>& ids, const std::function<bool(size_t, size_t)>& progress, const std::function<void()>& done) {
  if (!m_initialised) {
    if (done) done();
    return;
  }
  cancelSelect();  // drop any previous in-flight job
  m_ctx->ClearSelected(Standard_False);
  m_selTargets.clear();
  for (const auto& id : ids)
    for (const auto& body : m_doc->scene.bodies_under(id)) {
      auto it = m_items.find(body);
      if (it != m_items.end()) m_selTargets.push_back(it->second.ais);
    }
  m_selIndex = 0;
  m_selProgress = progress;
  m_selDone = done;
  const size_t total = m_selTargets.size();
  if (total <= 800) {  // small: apply immediately, no async overhead or latency
    for (const auto& t : m_selTargets) m_ctx->AddOrRemoveSelected(t, Standard_False);
    m_selTargets.clear();
    requestRedraw();
    if (progress) progress(total, total);
    auto d = m_selDone;
    m_selProgress = {};
    m_selDone = {};
    if (d) d();
    return;
  }
  stepSelect();  // large: chunk across the event loop so the UI stays responsive and cancellable
}

// One batch of a chunked selection, then yields to the event loop and reschedules itself.
void Viewport::stepSelect() {
  if (m_selTargets.empty()) return;
  const size_t total = m_selTargets.size();
  const size_t end = std::min(m_selIndex + 400, total);
  for (; m_selIndex < end; ++m_selIndex) m_ctx->AddOrRemoveSelected(m_selTargets[m_selIndex], Standard_False);
  const bool cancelled = m_selProgress && !m_selProgress(m_selIndex, total);
  if (m_selIndex >= total || cancelled) {
    requestRedraw();
    auto d = m_selDone;
    m_selTargets.clear();
    m_selIndex = 0;
    m_selProgress = {};
    m_selDone = {};
    if (d) d();
    return;
  }
  QTimer::singleShot(0, this, [this] { stepSelect(); });
}

void Viewport::cancelSelect() {
  if (m_selTargets.empty() && !m_selDone) return;
  auto d = m_selDone;
  m_selTargets.clear();
  m_selIndex = 0;
  m_selProgress = {};
  m_selDone = {};
  if (d) d();  // let the owner tear down its progress UI
}

void Viewport::clearSelection() {
  if (!m_initialised) return;
  m_ctx->ClearSelected(Standard_False);
  requestRedraw();
  emit selectionChanged();
}

void Viewport::OnSelectionChanged(const Handle(AIS_InteractiveContext)&, const Handle(V3d_View)&) { emit selectionChanged(); }

void Viewport::isolate(const std::vector<std::string>& ids) {
  m_isolated.clear();
  for (const auto& id : ids)
    for (const auto& b : m_doc->scene.bodies_under(id)) m_isolated.insert(b);
  sync();
  if (!ids.empty()) fitAll();
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
  Bnd_Box box;
  for (const auto& id : ids)
    for (const auto& b : m_doc->scene.bodies_under(id)) {
      auto it = m_items.find(b);
      if (it != m_items.end()) BRepBndLib::Add(it->second.located, box, Standard_False);
    }
  if (box.IsVoid()) return fitAll();
  m_view->FitAll(box, 0.02, Standard_False);
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::fitSelection() {
  if (!m_initialised) return;
  Bnd_Box box;
  for (m_ctx->InitSelected(); m_ctx->MoreSelected(); m_ctx->NextSelected()) {
    Handle(StdSelect_BRepOwner) owner = Handle(StdSelect_BRepOwner)::DownCast(m_ctx->SelectedOwner());
    if (!owner.IsNull() && owner->HasShape()) BRepBndLib::Add(owner->Shape(), box, Standard_False);
  }
  if (box.IsVoid()) return fitAll();
  m_view->FitAll(box, 0.02, Standard_False);
  m_view->Invalidate();
  requestRedraw();
}

void Viewport::standardView(const QString& name) {
  if (!m_initialised) return;
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
  requestRedraw();
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
  requestRedraw();
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
  requestRedraw();
}

// ---------------------------------------------------------------- scene sync
double Viewport::deflectionFor(const std::string& key) {
  auto it = m_deflection.find(key);
  if (it != m_deflection.end()) return it->second;
  Bnd_Box box;
  BRepBndLib::Add(opad::body_shape(m_doc->doc, key), box, Standard_False);
  double d = 0.1;
  if (!box.IsVoid()) {
    double x0, y0, z0, x1, y1, z1;
    box.Get(x0, y0, z0, x1, y1, z1);
    double diag = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
    d = std::clamp(diag * 0.0008, 0.005, 1.0);
  }
  m_deflection[key] = d;
  return d;
}

// Tessellation runs off the UI thread (F21); bodies appear once their mesh is ready.
void Viewport::startMeshing(std::vector<std::string> keys) {
  struct Job { TopoDS_Shape shape; std::string key; double tol; };
  std::vector<Job> jobs;
  {
    std::lock_guard<std::mutex> lock(m_meshMu);
    for (const auto& k : keys) {
      if (m_meshed.count(k) || m_meshing.count(k) || m_meshSkipped.count(k)) continue;
      m_meshing.insert(k);
      jobs.push_back({opad::body_shape(m_doc->doc, k), k, deflectionFor(k)});
    }
  }
  if (jobs.empty()) return;
  emit meshingProgress(static_cast<int>(m_meshing.size()));
  auto alive = m_alive;
  auto cancel = m_meshCancel;
  std::thread([this, alive, cancel, jobs = std::move(jobs)]() {
    for (size_t i = 0; i < jobs.size(); ++i) {
      const auto& j = jobs[i];
      if (*cancel) {
        std::lock_guard<std::mutex> lock(m_meshMu);
        for (size_t k = i; k < jobs.size(); ++k) {
          m_meshing.erase(jobs[k].key);
          m_meshSkipped.insert(jobs[k].key);
        }
        QMetaObject::invokeMethod(this, "sync", Qt::QueuedConnection);
        return;
      }
      try {
        BRepMesh_IncrementalMesh(j.shape, j.tol, Standard_False, 20.0 * M_PI / 180.0, Standard_True);
      } catch (...) {
      }
      if (!*alive) return;
      {
        std::lock_guard<std::mutex> lock(m_meshMu);
        m_meshing.erase(j.key);
        m_meshed.insert(j.key);
      }
      QMetaObject::invokeMethod(this, "sync", Qt::QueuedConnection);
    }
  }).detach();
}

void Viewport::sync() {
  if (!m_initialised || m_doc->loading) return;
  const opad::Scene& scene = m_doc->scene;
  std::set<std::string> keep;
  std::vector<std::string> pending;
  bool added = false;
  for (const auto& id : scene.all_bodies()) {
    const opad::Node* n = scene.node(id);
    if (!n || n->body_missing || !scene.effectively_visible(id)) continue;
    if (!m_isolated.empty() && !m_isolated.count(id)) continue;
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
    opad::Mat4 world = scene.world(id);
    auto it = m_items.find(id);
    if (it != m_items.end() && it->second.key == n->body_key && it->second.world.to_json() == world.to_json()) {
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
    if (it != m_items.end()) {
      m_ctx->Remove(it->second.ais, Standard_False);
      m_nodeOf.erase(it->second.ais.get());
      m_items.erase(it);
    }
    TopoDS_Shape proto = opad::body_shape(m_doc->doc, n->body_key);
    TopoDS_Shape located = proto;
    if (!world.is_identity()) {
      if (opad::mat_is_rigid(world)) located = proto.Moved(TopLoc_Location(opad::trsf_from_mat(world)));
      else located = opad::node_world_shape(m_doc->doc, scene, id);
    }
    Handle(AIS_Shape) ais = new AIS_Shape(located);
    ais->Attributes()->SetTypeOfDeflection(Aspect_TOD_ABSOLUTE);
    ais->Attributes()->SetMaximalChordialDeviation(deflectionFor(n->body_key));
    ais->Attributes()->SetDeviationAngle(20.0 * M_PI / 180.0);
    ais->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
    ais->SetColor(qcolor(n->color));
    if (n->opacity < 1.0) ais->SetTransparency(1.0 - n->opacity);
    applyStyle(ais);
    m_ctx->Display(ais, m_style == Style::Wireframe ? AIS_WireFrame : AIS_Shaded, 0, Standard_False);
    activateSelection(ais);
    m_items[id] = Item{ais, n->body_key, world, n->color, n->opacity, located};
    m_nodeOf[ais.get()] = id;
    added = true;
  }
  for (auto it = m_items.begin(); it != m_items.end();) {
    if (keep.count(it->first)) { ++it; continue; }
    m_ctx->Remove(it->second.ais, Standard_False);
    m_nodeOf.erase(it->second.ais.get());
    it = m_items.erase(it);
  }
  if (!pending.empty()) startMeshing(pending);
  else emit meshingProgress(0);
  updateAnnotations();
  updateClipPlanes();
  if (added && (m_needFit || m_items.size() <= 1)) {
    if (pending.empty()) m_needFit = false;  // keep re-fitting while meshes are still arriving
    m_view->FitAll(0.02, Standard_False);
  }
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
  FlushViewEvents(m_ctx, m_view, Standard_True);
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
  if (m_initialised && UpdateMouseButtons(devicePos(e->position()), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
}

void Viewport::mouseReleaseEvent(QMouseEvent* e) {
  if (m_initialised && UpdateMouseButtons(devicePos(e->position()), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
  if (m_rightPress && e->button() == Qt::RightButton && (e->pos() - m_pressPos).manhattanLength() < 4) {
    m_rightPress = false;
    emit contextMenuRequested(e->globalPosition().toPoint());
  }
}

void Viewport::mouseMoveEvent(QMouseEvent* e) {
  if (m_initialised && UpdateMousePosition(devicePos(e->position()), qt_buttons(e->buttons()), qt_flags(e->modifiers()), false)) requestRedraw();
}

void Viewport::wheelEvent(QWheelEvent* e) {
  if (!m_initialised) return;
  const double delta = e->angleDelta().y() / 8.0;
  if (UpdateZoom(Aspect_ScrollDelta(devicePos(e->position()), delta))) requestRedraw();
}
