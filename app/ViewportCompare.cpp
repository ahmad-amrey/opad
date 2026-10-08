// Viewport, Compare (UI-58): another version's bodies and the moves between the two, drawn with the model and never
// picked. The parts are shaded from arrays built off the UI thread (BodyShape), so showing them walks no triangulation
// here; a new style changes their aspects in place (SynchronizeAspects), never Redisplay.
// Side by side is a second V3d_View of the same viewer and context in a native window left of this one: the same
// structures, no second upload, each object shown in one view or both by its view affinity. Its camera is this view's,
// copied after every frame; mouse navigation over it is handed to this view.
#include "Viewport.hpp"

#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <Graphic3d_CView.hxx>
#include <Graphic3d_Group.hxx>
#include <Image_PixMap.hxx>
#include <Prs3d_Arrow.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_Presentation.hxx>
#include <TopLoc_Location.hxx>
#include <V3d_ImageDumpOptions.hxx>
#if defined(_WIN32)
#include <WNT_Window.hxx>
#elif defined(__APPLE__)
Handle(Aspect_Window) opad_make_cocoa_window(void* nsview);
#else
#include <OpenGl_GraphicDriver.hxx>
#include <Xw_Window.hxx>
#endif

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>

#include "Jobs.hpp"
#include "opad/geometry.hpp"

// A's view: draws through Viewport::drawSide, hands navigation to the viewport (the same local position: the two views
// are the same size), keeps the caption in its top-left corner and the split in step with the parent's size.
class Viewport::SideView : public QWidget {
 public:
  SideView(Viewport* viewport, QWidget* host) : QWidget(host), m_vp(viewport) {
    setObjectName("compareSide");
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::NoFocus);
    m_captionHost = new QWidget(this);  // native and opaque, as the chips row: drawn over the GL surface
    m_captionHost->setAttribute(Qt::WA_NativeWindow);
    m_captionHost->setAutoFillBackground(true);
    auto* row = new QHBoxLayout(m_captionHost);
    row->setContentsMargins(8, 8, 8, 8);
    m_caption = new QLabel(m_captionHost);
    m_caption->setObjectName("chipSel");
    row->addWidget(m_caption);
    paintCaption();
    host->installEventFilter(this);
  }
  QPaintEngine* paintEngine() const override { return nullptr; }
  void setCaption(const QString& text) {
    m_caption->setText(text);
    m_captionHost->adjustSize();
    m_captionHost->move(0, 0);
  }
  QString caption() const { return m_caption->text(); }
  void paintCaption() {
    QPalette pal = m_captionHost->palette();
    pal.setColor(QPalette::Window, m_vp->m_tokens.vp);
    m_captionHost->setPalette(pal);
  }
  bool forwarding() const { return m_forward; }

 protected:
  void paintEvent(QPaintEvent*) override { m_vp->drawSide(true); }
  void resizeEvent(QResizeEvent*) override {
    if (QWindow* native = windowHandle()) native->resize(size());
    if (!m_vp->m_sideView.IsNull()) m_vp->m_sideView->MustBeResized();
    m_vp->drawSide(true);
  }
  // Wheel, middle and right buttons navigate; the left one only on the cube (A's view selects nothing).
  void mousePressEvent(QMouseEvent* e) override {
    if (!m_forward && e->button() == Qt::LeftButton && !m_vp->cubeAt(e->position())) return;
    m_forward = true;
    m_vp->mousePressEvent(e);
    m_vp->m_navSide = true;  // its orbit pivot is found among what A's view draws
  }
  void mouseDoubleClickEvent(QMouseEvent* e) override { mousePressEvent(e); }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (m_forward) m_vp->mouseMoveEvent(e);
  }
  void mouseReleaseEvent(QMouseEvent* e) override {
    if (!m_forward) return;
    m_vp->mouseReleaseEvent(e);
    if (e->buttons() == Qt::NoButton) m_forward = false;
  }
  void wheelEvent(QWheelEvent* e) override {
    m_vp->wheelEvent(e);
    m_vp->m_navSide = true;  // it zooms towards what A's view draws under the pointer
  }
  bool eventFilter(QObject* object, QEvent* e) override {
    if (object == parentWidget() && e->type() == QEvent::Resize) m_vp->layoutSide();
    return QWidget::eventFilter(object, e);
  }

 private:
  Viewport* m_vp;
  QWidget* m_captionHost;
  QLabel* m_caption;
  bool m_forward = false;  // a press was handed over: its moves and release follow it
};

namespace {
Quantity_Color rgb(const std::array<double, 3>& c) {
  return Quantity_Color(std::clamp(c[0], 0.0, 1.0), std::clamp(c[1], 0.0, 1.0), std::clamp(c[2], 0.0, 1.0), Quantity_TOC_sRGB);
}

// Dashed segments from each old centre to the new one, with a solid head at the new end. Sized in the world: the head is
// a fifth of the arrow, so a short move keeps a head that fits it.
class CompareArrows : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(CompareArrows, AIS_InteractiveObject)
 public:
  CompareArrows(const std::vector<Viewport::CompareArrow>& arrows, const Quantity_Color& color) : m_arrows(arrows), m_color(color) {}
  void setArrows(const std::vector<Viewport::CompareArrow>& arrows) { m_arrows = arrows; }
  size_t shown() const { return size_t(std::count_if(m_arrows.begin(), m_arrows.end(), [](const auto& a) { return a.visible; })); }

 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&, const Handle(Prs3d_Presentation)& prs, const Standard_Integer) override {
    std::vector<std::pair<gp_Pnt, gp_Pnt>> lines;
    for (const auto& a : m_arrows) {
      const gp_Pnt from(a.from[0], a.from[1], a.from[2]), to(a.to[0], a.to[1], a.to[2]);
      if (a.visible && from.Distance(to) > 1e-9) lines.emplace_back(from, to);
    }
    if (lines.empty()) return;
    Handle(Graphic3d_ArrayOfSegments) dashes = new Graphic3d_ArrayOfSegments(int(lines.size()) * 2);
    for (const auto& [from, to] : lines) {
      dashes->AddVertex(from);
      dashes->AddVertex(to);
    }
    Handle(Graphic3d_Group) group = prs->NewGroup();
    group->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(m_color, Aspect_TOL_DASH, 2.0));
    group->AddPrimitiveArray(dashes);
    Handle(Graphic3d_Group) heads = prs->NewGroup();
    heads->SetGroupPrimitivesAspect(new Graphic3d_AspectLine3d(m_color, Aspect_TOL_SOLID, 2.0));
    for (const auto& [from, to] : lines) {
      const double length = from.Distance(to);
      heads->AddPrimitiveArray(Prs3d_Arrow::DrawSegments(to, gp_Dir(gp_Vec(from, to)), 0.35, length * 0.2, 12));
    }
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&, const Standard_Integer) override {}

 private:
  std::vector<Viewport::CompareArrow> m_arrows;
  Quantity_Color m_color;
};
}  // namespace

void Viewport::styleComparePart(const Handle(AIS_Shape)& ais, const ComparePart& part) {
  ais->SetColor(rgb(part.color));
  ais->SetTransparency(1.0 - std::clamp(part.opacity, 0.0, 1.0));
  // Line aspects ignore alpha: a ghost's edges are blended towards the background instead, so they fade with it.
  const auto edge = looks::mix(looks::mix(part.color, {0, 0, 0}, 0.35), {m_tokens.vp.redF(), m_tokens.vp.greenF(), m_tokens.vp.blueF()},
                               1 - std::clamp(part.opacity, 0.0, 1.0));
  ais->Attributes()->SetFaceBoundaryDraw(Standard_True);
  ais->Attributes()->SetFaceBoundaryAspect(new Prs3d_LineAspect(rgb(edge), Aspect_TOL_SOLID, 1.0));
  if (const Handle(BodyShape) body = Handle(BodyShape)::DownCast(ais)) body->syncPainted(false);  // changed faces fade with it
}

void Viewport::setCompare(const std::vector<ComparePart>& parts, const std::vector<CompareArrow>& arrows, const QColor& arrowColor) {
  if (!m_initialised) return;
  clearCompare();
  for (const auto& part : parts) m_compareViews.push_back(part.view);
  for (const auto& part : parts) {  // one entry per part, null when it cannot be drawn, so restyleCompare keeps the order
    Handle(AIS_Shape) ais;
    if (!part.shape.IsNull()) try {
        gp_Trsf placed;
        if (!part.world.is_identity()) placed = opad::trsf_from_mat(part.world);  // throws when not rigid: such a part comes baked
        ais = part.prs ? Handle(AIS_Shape)(new BodyShape(part.shape, part.prs)) : new AIS_Shape(part.shape);
        ais->Attributes()->SetAutoTriangulation(Standard_False);  // meshed already
        ais->SetMaterial(Graphic3d_NameOfMaterial_Plastified);
        if (placed.Form() != gp_Identity) ais->SetLocalTransformation(placed);
        styleComparePart(ais, part);
        if (part.visible) m_ctx->Display(ais, AIS_Shaded, -1, Standard_False);  // mode -1: never picked
        // Never selected, but navigated about: the orbit pivot and the zoom point land on it while it is drawn.
        if (part.prs && !part.prs->navigation.IsNull()) {
          Handle(NavigationShape) nav = new NavigationShape(part.prs->navigation);
          if (placed.Form() != gp_Identity) nav->SetLocalTransformation(placed);
          m_navSelection->Load(nav, -1);
          m_navSelection->Activate(nav, 0);
          m_navExtras[nav.get()] = {ais, part.prs->box};
          m_compareNav.push_back(nav);
        }
      } catch (const std::exception&) {
        ais.Nullify();
      }
    m_compareParts.emplace_back(part.id, ais);
  }
  if (!arrows.empty()) {
    m_compareArrows = new CompareArrows(arrows, Quantity_Color(arrowColor.redF(), arrowColor.greenF(), arrowColor.blueF(), Quantity_TOC_sRGB));
    m_ctx->Display(m_compareArrows, 0, -1, Standard_False);
    m_ctx->SetZLayer(m_compareArrows, Graphic3d_ZLayerId_Topmost);  // over the parts they join
  }
  applySideMasks();
  redrawScene();
}

void Viewport::restyleCompare(const std::vector<ComparePart>& parts, const std::vector<CompareArrow>& arrows) {
  if (!m_initialised) return;
  bool shown = false;
  for (size_t k = 0; k < parts.size() && k < m_compareParts.size(); ++k) {
    const ComparePart& part = parts[k];
    const Handle(AIS_Shape)& ais = m_compareParts[k].second;
    if (ais.IsNull()) continue;
    if (!part.visible) {
      if (m_ctx->IsDisplayed(ais)) m_ctx->Erase(ais, Standard_False);
      continue;
    }
    styleComparePart(ais, part);
    ais->SynchronizeAspects();
    if (!m_ctx->IsDisplayed(ais)) {
      m_ctx->Display(ais, AIS_Shaded, -1, Standard_False);
      shown = true;
    }
  }
  if (const auto drawn = Handle(CompareArrows)::DownCast(m_compareArrows); !drawn.IsNull()) {
    drawn->setArrows(arrows);
    m_ctx->Redisplay(drawn, Standard_False);  // a few segments, nothing to pick
  }
  if (shown) applySideMasks();  // a part displayed for the first time starts in every view
  redrawScene();
}

void Viewport::clearCompare() {
  if (!m_initialised || (m_compareParts.empty() && m_compareArrows.IsNull())) return;
  for (const auto& nav : m_compareNav) {
    m_navExtras.erase(nav.get());
    m_navSelection->Remove(nav);
  }
  m_compareNav.clear();
  for (const auto& [id, ais] : m_compareParts)
    if (!ais.IsNull()) m_ctx->Remove(ais, Standard_False);  // Remove resets the view affinity
  m_compareParts.clear();
  m_compareViews.clear();
  if (!m_compareArrows.IsNull()) m_ctx->Remove(m_compareArrows, Standard_False);
  m_compareArrows.Nullify();
  redrawScene();
}

namespace {
bool sameColors(const opad::FaceColors& x, const opad::FaceColors& y) { return x.colors == y.colors && x.face == y.face; }

// The file's face colours with `tint` over them: a tinted face takes its tint, the others keep theirs.
std::shared_ptr<const opad::FaceColors> paintedOver(const std::shared_ptr<const opad::FaceColors>& file, const opad::FaceColors& tint) {
  if (!file || file->empty()) return std::make_shared<const opad::FaceColors>(tint);
  auto out = std::make_shared<opad::FaceColors>(*file);
  const int shift = int(out->colors.size());
  out->colors.insert(out->colors.end(), tint.colors.begin(), tint.colors.end());
  if (out->face.size() < tint.face.size()) out->face.resize(tint.face.size(), -1);
  for (size_t i = 0; i < tint.face.size(); ++i)
    if (tint.face[i] >= 0) out->face[i] = tint.face[i] + shift;
  return out;
}
}  // namespace

void Viewport::setFaceTints(std::map<std::string, std::shared_ptr<const opad::FaceColors>> tints) {
  if (!m_initialised) return;
  bool redraw = false, glowsStale = false;
  // An object's drawn arrays changed: its glow was made from the old ones.
  auto recompute = [&](const Handle(BodyShape)& body) {
    m_ctx->RecomputePrsOnly(body, Standard_False);
    redraw = true;
    if (const auto glow = m_bodyGlows.find(body.get()); glow != m_bodyGlows.end()) {
      m_ctx->Remove(glow->second, Standard_False);
      m_bodyGlows.erase(glow);
      glowsStale = true;
    }
  };
  // Tints no longer wanted (or wanted otherwise, or on a body drawn again since): the body's own arrays again.
  for (auto it = m_faceTints.begin(); it != m_faceTints.end();) {
    const auto want = tints.find(it->first);
    const auto item = m_items.find(it->first);
    const bool drawnSince = item == m_items.end() || item->second.key != it->second.key || (it->second.ais && item->second.ais.get() != it->second.ais);
    if (want != tints.end() && want->second && !drawnSince && sameColors(*want->second, *it->second.colors)) {
      ++it;
      continue;
    }
    if (item != m_items.end() && it->second.ais && item->second.ais.get() == it->second.ais)
      if (const auto body = Handle(BodyShape)::DownCast(item->second.ais); !body.IsNull()) {
        const auto refined = m_refined.find(item->second.key);
        if (body->setDisplayPrs(refined != m_refined.end() ? refined->second.prs : nullptr)) recompute(body);
      }
    it = m_faceTints.erase(it);
  }
  // New ones: the body's base mesh coloured on a worker (bodies drawn rigidly from worker arrays only).
  struct Build {
    std::string id, key;
    TopoDS_Shape shape;
    Bnd_Box box;
    double deflection = 0;
    std::shared_ptr<const opad::FaceColors> colors;
    std::shared_ptr<BodyPrs> out;
  };
  auto builds = std::make_shared<std::vector<Build>>();
  for (const auto& [id, colors] : tints) {
    if (!colors || colors->empty() || m_faceTints.count(id)) continue;
    const auto item = m_items.find(id);
    if (item == m_items.end() || !item->second.rigid || item->second.stretch != 1) continue;
    const auto body = Handle(BodyShape)::DownCast(item->second.ais);
    if (body.IsNull() || body->curveOnly() || !body->prs() || body->prs()->triangles.IsNull()) continue;
    m_faceTints[id] = {colors, nullptr, item->second.key};
  }
  for (const auto& [id, tint] : m_faceTints) {
    if (tint.ais) continue;  // drawn already
    const Item& item = m_items.at(id);
    const auto& base = Handle(BodyShape)::DownCast(item.ais)->prs();
    try {
      builds->push_back({id, item.key, opad::body_shape(m_doc->doc, item.key), base->box, base->deflection, paintedOver(base->faceColors, *tint.colors), nullptr});
    } catch (const std::exception&) {
    }
  }
  if (glowsStale) applySelectionLayers();
  if (redraw) {
    if (m_style == Style::HiddenEdges) scheduleEdgeOverlay();
    redrawScene();
  }
  if (builds->empty() || !m_jobs) return;
  const unsigned serial = ++m_faceTintSerial;  // the bodies still waiting are in this job too
  m_jobs->async(tr("Colouring the changed faces"), [builds](Progress p) {
    for (auto& b : *builds) {
      if (p.cancelled()) return;
      b.out = BodyPrs::build(b.shape, b.box, true, b.colors);  // the base mesh's own triangles: drawn as the body is picked
      b.out->deflection = b.deflection;
    }
  }, [this, builds, serial](bool ok, const QString&) {
    if (!ok || serial != m_faceTintSerial) return;
    bool redraw = false, glowsStale = false;
    for (const auto& b : *builds) {
      const auto tint = m_faceTints.find(b.id);
      const auto item = m_items.find(b.id);
      if (!b.out || tint == m_faceTints.end() || tint->second.ais || item == m_items.end() || item->second.key != b.key) continue;
      const auto body = Handle(BodyShape)::DownCast(item->second.ais);
      if (body.IsNull()) continue;
      tint->second.ais = body.get();
      if (!body->setDisplayPrs(b.out)) continue;
      m_ctx->RecomputePrsOnly(body, Standard_False);
      redraw = true;
      if (const auto glow = m_bodyGlows.find(body.get()); glow != m_bodyGlows.end()) {
        m_ctx->Remove(glow->second, Standard_False);
        m_bodyGlows.erase(glow);
        glowsStale = true;
      }
    }
    if (glowsStale) applySelectionLayers();
    if (!redraw) return;
    if (m_style == Style::HiddenEdges) scheduleEdgeOverlay();
    redrawScene();
  }, JobKind::Background);
}

std::shared_ptr<const BodyPrs> Viewport::displayArrays(const std::string& key) const {
  std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_meshMu));
  const auto it = m_prs.find(key);
  return it == m_prs.end() ? nullptr : it->second;
}

void Viewport::fitBox(const Bnd_Box& box) {
  if (!m_initialised) return;
  if (box.IsVoid()) return fitAll();
  m_needFit = false;
  m_view->FitAll(box, 0.02, Standard_False);
  m_view->Invalidate();
  requestRedraw();
}

opad::json Viewport::benchCompareState() const {
  opad::json parts = opad::json::array();
  for (const auto& [id, ais] : m_compareParts) {
    if (ais.IsNull()) {
      parts.push_back({{"id", id}, {"displayed", false}});
      continue;
    }
    Quantity_Color c;
    ais->Color(c);
    double r = 0, g = 0, b = 0;
    c.Values(r, g, b, Quantity_TOC_sRGB);
    const auto body = Handle(BodyShape)::DownCast(ais);
    const size_t painted = !body.IsNull() && body->prs() ? body->prs()->painted.size() : 0;  // colours of its own changed faces
    parts.push_back({{"id", id}, {"displayed", m_initialised && m_ctx->IsDisplayed(ais)}, {"color", {r, g, b}}, {"transparency", ais->Transparency()}, {"painted", painted}});
  }
  const auto arrows = Handle(CompareArrows)::DownCast(m_compareArrows);
  return {{"parts", parts}, {"arrows", arrows.IsNull() ? 0 : arrows->shown()}};
}

// ---------------------------------------------------------------- side by side
void Viewport::setSideBySide(bool on, const QString& caption) {
  if (on == (m_side != nullptr)) {
    if (on) setSideCaption(caption);
    return;
  }
  QWidget* host = parentWidget();
  if (!m_initialised || !host) return;
  trace::Scope scope(on ? "Viewport: side by side" : "Viewport: side by side off");
  if (on) {
    m_hostMargins = host->contentsMargins();
    m_side = new SideView(this, host);
    m_side->setCaption(caption);
    m_sideView = m_viewer->CreateView();
#if defined(_WIN32)
    Handle(Aspect_Window) win = new WNT_Window(reinterpret_cast<Aspect_Handle>(m_side->winId()));
#elif defined(__APPLE__)
    Handle(Aspect_Window) win = opad_make_cocoa_window(reinterpret_cast<void*>(m_side->winId()));
#else
    Handle(Aspect_Window) win = new Xw_Window(Handle(OpenGl_GraphicDriver)::DownCast(m_viewer->Driver())->GetDisplayConnection(),
                                              static_cast<Aspect_Drawable>(m_side->winId()));
#endif
    m_sideView->SetImmediateUpdate(Standard_False);
    m_sideView->SetWindow(win);
    if (!win->IsMapped()) win->Map();
    layoutSide();
    m_side->show();
    applySideMasks();
    drawSide(true);
  } else {
    const std::set<std::string> hidden = std::exchange(m_sideHidden, {});
    for (const auto& id : hidden) maskSide(id, nullptr);  // every view shows everything again: no mask outlives the view
    m_sideHidden = hidden;
    for (const auto& [id, ais] : m_compareParts)
      if (!ais.IsNull()) ais->ViewAffinity()->SetVisible(true);
    m_sideView->Remove();
    m_sideView.Nullify();
    delete m_side;
    m_side = nullptr;
    host->setContentsMargins(m_hostMargins);
    for (const Graphic3d_ZLayerId layer : {Graphic3d_ZLayerId_Default, Graphic3d_ZLayerId_Topmost}) m_view->View()->InvalidateZLayerBoundingBox(layer);
  }
  redrawScene();
}

void Viewport::setSideCaption(const QString& caption) {
  if (m_side) m_side->setCaption(caption);
}

QWidget* Viewport::sideWidget() const { return m_side; }

Handle(V3d_View) Viewport::navView() const { return m_side && m_navSide && !m_sideView.IsNull() ? m_sideView : m_view; }

// The parent's left half is A's, the right one this view's (the parent's layout keeps us in its contents rectangle), with
// a two-pixel gap between them.
void Viewport::layoutSide() {
  QWidget* host = parentWidget();
  if (!m_side || !host) return;
  const QRect area = host->rect().marginsRemoved(m_hostMargins);
  constexpr int gap = 2;
  const int half = std::max(1, (area.width() - gap) / 2);
  m_side->setGeometry(area.left(), area.top(), half, area.height());
  host->setContentsMargins(m_hostMargins + QMargins(half + gap, 0, 0, 0));
}

void Viewport::setSideHidden(const std::vector<std::string>& ids) {
  std::set<std::string> hidden(ids.begin(), ids.end());
  if (hidden == m_sideHidden) return;
  std::swap(m_sideHidden, hidden);
  if (!m_side) return;
  for (const auto& id : hidden)
    if (!m_sideHidden.count(id)) maskSide(id, nullptr);  // no longer one of B's changes: in A's view again
  applySideMasks();
}

// A session object's place in A's view: left out when it is one of B's changes. ais null: the body or sketch of this id
// (with its glow and images).
void Viewport::maskSide(const std::string& id, const Handle(AIS_InteractiveObject)& ais) {
  if (!m_side || m_sideView.IsNull()) return;
  const bool shown = !m_sideHidden.count(id);
  auto mask = [&](const Handle(AIS_InteractiveObject)& object) {
    if (!object.IsNull()) m_ctx->SetViewAffinity(object, m_sideView, shown);
  };
  if (!ais.IsNull()) return mask(ais);
  if (const auto item = m_items.find(id); item != m_items.end()) {
    mask(item->second.ais);
    if (const auto glow = m_bodyGlows.find(item->second.ais.get()); glow != m_bodyGlows.end()) mask(glow->second);
  }
  if (const auto wire = m_sketchWires.find(id); wire != m_sketchWires.end()) {
    mask(wire->second.ais);
    for (const auto& image : wire->second.backdrops) mask(image);
  }
}

void Viewport::applySideMasks() {
  if (!m_initialised) return;
  const bool side = m_side && !m_sideView.IsNull();
  for (size_t k = 0; k < m_compareParts.size(); ++k) {
    const Handle(AIS_Shape)& ais = m_compareParts[k].second;
    if (ais.IsNull()) continue;
    const char view = k < m_compareViews.size() ? m_compareViews[k] : 0;
    m_ctx->SetViewAffinity(ais, m_view, !side || view != 'A');
    if (side) m_ctx->SetViewAffinity(ais, m_sideView, view != 'B');
  }
  if (side)
    for (const auto& id : m_sideHidden) maskSide(id, nullptr);
  // The layers' boxes are cached per view and the z range comes from them: what a view shows now must not be clipped.
  for (const Handle(V3d_View)& view : {m_view, m_sideView})
    if (!view.IsNull())
      for (const Graphic3d_ZLayerId layer : {Graphic3d_ZLayerId_Default, Graphic3d_ZLayerId_Topmost}) view->View()->InvalidateZLayerBoundingBox(layer);
  if (side) m_sideView->Invalidate();
  redrawScene();
}

// After a frame of this view (handleViewRedraw) and on the side's own paint: its camera becomes this view's (with its own
// aspect), its look this view's (rendering, background, the section plane); then all of it is drawn again, or only its
// immediate layer when nothing moved.
void Viewport::drawSide(bool full) {
  if (!m_side || m_sideView.IsNull() || !m_initialised) return;
  const Handle(Graphic3d_Camera)& from = m_view->Camera();
  const Handle(Graphic3d_Camera)& to = m_sideView->Camera();
  const bool moved = from->ProjectionType() != to->ProjectionType() || from->Scale() != to->Scale() || from->FOVy() != to->FOVy() ||
                     !from->Eye().IsEqual(to->Eye(), 0) || !from->Center().IsEqual(to->Center(), 0) || !from->Up().IsEqual(to->Up(), 0);
  if (moved || full || m_sideView->IsInvalidated()) {
    Standard_Integer w = 0, h = 0;
    m_sideView->Window()->Size(w, h);
    to->CopyMappingData(from);
    to->CopyOrientationData(from);
    if (w > 0 && h > 0) to->SetAspect(double(w) / h);
    m_sideView->ChangeRenderingParams() = m_view->RenderingParams();
    if (m_sideView->BackgroundColor() != m_view->BackgroundColor()) m_sideView->SetBackgroundColor(m_view->BackgroundColor());
    Quantity_Color top, bottom, hadTop, hadBottom;
    const Aspect_GradientBackground gradient = m_view->GradientBackground(), had = m_sideView->GradientBackground();
    gradient.Colors(top, bottom);
    had.Colors(hadTop, hadBottom);
    if (had.BgGradientFillMethod() != gradient.BgGradientFillMethod() || hadTop != top || hadBottom != bottom)
      m_sideView->SetBgGradientColors(top, bottom, gradient.BgGradientFillMethod(), Standard_False);
    if (m_sideView->ClipPlanes() != m_view->ClipPlanes()) m_sideView->SetClipPlanes(m_view->ClipPlanes());
    m_side->paintCaption();
    m_sideView->Invalidate();
    m_sideView->Redraw();
  } else {
    m_sideView->RedrawImmediate();  // a hover: the immediate layer is shown in every view
  }
}

QImage Viewport::grabSide() {
  if (!m_side || m_sideView.IsNull()) return QImage();
  drawSide(true);
  Image_PixMap pix;
  V3d_ImageDumpOptions o;
  o.Width = static_cast<int>(m_side->width() * m_side->devicePixelRatioF());
  o.Height = static_cast<int>(m_side->height() * m_side->devicePixelRatioF());
  o.BufferType = Graphic3d_BT_RGB;
  o.ToAdjustAspect = Standard_True;
  if (!m_sideView->ToPixMap(pix, o)) return QImage();
  QImage img(static_cast<int>(pix.Width()), static_cast<int>(pix.Height()), QImage::Format_RGB888);
  for (int y = 0; y < img.height(); ++y) {
    uchar* row = img.scanLine(y);
    for (int x = 0; x < img.width(); ++x) {
      const Quantity_ColorRGBA c = pix.PixelColor(x, y);
      row[x * 3] = static_cast<uchar>(c.GetRGB().Red() * 255);
      row[x * 3 + 1] = static_cast<uchar>(c.GetRGB().Green() * 255);
      row[x * 3 + 2] = static_cast<uchar>(c.GetRGB().Blue() * 255);
    }
  }
  return img;
}

opad::Vec3 Viewport::benchOrbitPivot(const opad::Vec3& at, bool side) {
  if (!m_initialised) return at;
  FlushViewEvents(m_ctx, m_view, Standard_True);  // the z range the picker clips to (a hidden window paints no frame)
  if (side) drawSide(true);
  Standard_Integer x = 0, y = 0;
  m_view->Convert(at[0], at[1], at[2], x, y);
  const bool was = std::exchange(m_navSide, side);
  const gp_Pnt p = orbitPoint(Graphic3d_Vec2i(x, y));
  m_navSide = was;
  return {p.X(), p.Y(), p.Z()};
}

opad::json Viewport::benchSideState() {
  if (!m_initialised) return opad::json::object();
  FlushViewEvents(m_ctx, m_view, Standard_True);  // what the next frame does (a hidden window paints none)
  auto camera = [](const Handle(Graphic3d_Camera)& c) {
    return opad::json{{"eye", {c->Eye().X(), c->Eye().Y(), c->Eye().Z()}}, {"center", {c->Center().X(), c->Center().Y(), c->Center().Z()}},
                      {"up", {c->Up().X(), c->Up().Y(), c->Up().Z()}}, {"scale", c->Scale()}, {"aspect", c->Aspect()}};
  };
  QWidget* host = parentWidget();
  opad::json out = {{"side", m_side != nullptr}, {"main", camera(m_view->Camera())}, {"rect", {x(), y(), width(), height()}},
                    {"host", {host ? host->width() : 0, host ? host->height() : 0}}};
  const int mainId = m_view->View()->Identification(), sideId = m_side ? m_sideView->View()->Identification() : -1;
  auto shown = [&](const Handle(AIS_InteractiveObject)& ais) {
    return opad::json{{"main", ais->ViewAffinity()->IsVisible(mainId)}, {"side", sideId >= 0 && ais->ViewAffinity()->IsVisible(sideId)}};
  };
  opad::json parts = opad::json::array(), bodies = opad::json::object();
  for (const auto& [id, ais] : m_compareParts)
    if (!ais.IsNull()) {
      opad::json part = shown(ais);
      part["id"] = id;
      parts.push_back(part);
    }
  for (const auto& [id, item] : m_items) bodies[id] = shown(item.ais);
  out["parts"] = parts;
  out["bodies"] = bodies;
  if (!m_side) return out;
  out["sideCamera"] = camera(m_sideView->Camera());
  out["sideRect"] = {m_side->x(), m_side->y(), m_side->width(), m_side->height()};
  out["caption"] = m_side->caption().toStdString();
  return out;
}
