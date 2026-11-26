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
  // `extra`: a pane C or D (setSideExtras), else A's; A's follows the parent's size for all of them.
  SideView(Viewport* viewport, QWidget* host, bool extra = false) : QWidget(host), m_vp(viewport), m_extra(extra) {
    setObjectName(extra ? "compareExtra" : "compareSide");
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
    if (!extra) host->installEventFilter(this);
  }
  void setView(const Handle(V3d_View)& view) { m_v3d = view; }
  const Handle(V3d_View)& view() const { return m_v3d; }
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
  void paintEvent(QPaintEvent*) override { m_vp->drawPane(this, m_v3d, true); }
  void resizeEvent(QResizeEvent*) override {
    if (QWindow* native = windowHandle()) native->resize(size());
    if (!m_v3d.IsNull()) m_v3d->MustBeResized();
    m_vp->drawPane(this, m_v3d, true);
  }
  void handOver() {  // its orbit pivot and zoom point are found among what this pane draws
    if (m_extra) m_vp->m_navExtra = m_v3d;
    else m_vp->m_navSide = true;
  }
  // Wheel, middle and right buttons navigate; the left one only on the cube (A's view selects nothing).
  void mousePressEvent(QMouseEvent* e) override {
    if (!m_forward && e->button() == Qt::LeftButton && !m_vp->cubeAt(e->position())) return;
    m_forward = true;
    m_vp->mousePressEvent(e);
    handOver();
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
    handOver();
  }
  bool eventFilter(QObject* object, QEvent* e) override {
    if (object == parentWidget() && e->type() == QEvent::Resize) m_vp->layoutSide();
    return QWidget::eventFilter(object, e);
  }

 private:
  Viewport* m_vp;
  bool m_extra;
  Handle(V3d_View) m_v3d;
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
  m_refineTimer.start();  // seen close up already: the parts refined as the document's bodies are
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
  dropRefined(refineComparePrefix());  // their zoom refinements went with them
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

void Viewport::setFaceTints(std::map<std::string, std::shared_ptr<const opad::FaceColors>> tints) { applyTints(std::move(tints), {}, false); }

void Viewport::setVertexPaints(std::map<std::string, std::shared_ptr<const VertexPaint>> paints) { applyTints({}, std::move(paints), true); }

// Compare's face colours and Deviation's vertex paints share the tinted arrays (m_faceTints); each call manages its own kind
// and leaves the other's entries alone (a body has one of them at a time: the first one asked for).
void Viewport::applyTints(std::map<std::string, std::shared_ptr<const opad::FaceColors>> tints, std::map<std::string, std::shared_ptr<const VertexPaint>> paints,
                          bool painting) {
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
  // Tints no longer wanted (or wanted otherwise, or on a body drawn again since): the body's own arrays again. A paint
  // replaced by another keeps the old colours drawn until the new ones are ready (no flash of the plain body).
  for (auto it = m_faceTints.begin(); it != m_faceTints.end();) {
    if ((it->second.paint != nullptr) != painting) {
      ++it;
      continue;
    }
    const auto item = m_items.find(it->first);
    const bool drawnSince = item == m_items.end() || item->second.key != it->second.key || (it->second.drawnOn && item->second.ais.get() != it->second.drawnOn);
    if (painting) {
      const auto want = paints.find(it->first);
      if (want != paints.end() && want->second && !drawnSince) {
        if (want->second != it->second.paint) {
          it->second.paint = want->second;
          it->second.ais = nullptr;  // made again below; drawnOn keeps the old colours meanwhile
          it->second.arrays.reset();
          m_refined.erase(refineTintKey(it->first));
        }
        ++it;
        continue;
      }
    } else {
      const auto want = tints.find(it->first);
      if (want != tints.end() && want->second && !drawnSince && sameColors(*want->second, *it->second.colors)) {
        ++it;
        continue;
      }
    }
    if (item != m_items.end() && it->second.drawnOn && item->second.ais.get() == it->second.drawnOn)
      if (const auto body = Handle(BodyShape)::DownCast(item->second.ais); !body.IsNull()) {
        const auto refined = m_refined.find(item->second.key);
        if (body->setDisplayPrs(refined != m_refined.end() ? refined->second.prs : nullptr)) recompute(body);
      }
    m_refined.erase(refineTintKey(it->first));  // the coloured arrays' refinement too
    it = m_faceTints.erase(it);
  }
  // New ones: the body's base mesh coloured on a worker (bodies drawn rigidly from worker arrays only).
  struct Build {
    std::string id, key;
    TopoDS_Shape shape;
    Bnd_Box box;
    double deflection = 0;
    std::shared_ptr<const opad::FaceColors> colors;
    std::shared_ptr<const VertexPaint> paint;
    std::shared_ptr<const BodyPrs> base;
    gp_Trsf placed;
    std::shared_ptr<BodyPrs> out;
  };
  auto builds = std::make_shared<std::vector<Build>>();
  auto admit = [&](const std::string& id, std::shared_ptr<const opad::FaceColors> colors, std::shared_ptr<const VertexPaint> paint) {
    if (m_faceTints.count(id)) return;
    const auto item = m_items.find(id);
    if (item == m_items.end() || !item->second.rigid || item->second.stretch != 1) return;
    const auto body = Handle(BodyShape)::DownCast(item->second.ais);
    if (body.IsNull() || body->curveOnly() || !body->prs() || body->prs()->triangles.IsNull()) return;
    FaceTint tint;
    tint.colors = colors ? std::move(colors) : std::make_shared<const opad::FaceColors>();
    tint.paint = std::move(paint);
    tint.key = item->second.key;
    m_faceTints[id] = std::move(tint);
  };
  if (painting) {
    for (const auto& [id, paint] : paints)
      if (paint) admit(id, nullptr, paint);
  } else {
    for (const auto& [id, colors] : tints)
      if (colors && !colors->empty()) admit(id, colors, nullptr);
  }
  for (const auto& [id, tint] : m_faceTints) {
    if (tint.ais || (tint.paint != nullptr) != painting) continue;  // drawn already, or the other kind's
    const Item& item = m_items.at(id);
    const auto& base = Handle(BodyShape)::DownCast(item.ais)->prs();
    try {
      Build b{id, item.key, {}, base->box, base->deflection, nullptr, tint.paint, base, opad::trsf_from_mat(item.world), nullptr};
      if (!tint.paint) {
        b.shape = opad::body_shape(m_doc->doc, item.key);
        b.colors = paintedOver(base->faceColors, *tint.colors);
      }
      builds->push_back(std::move(b));
    } catch (const std::exception&) {
    }
  }
  if (glowsStale) applySelectionLayers();
  if (redraw) {
    if (m_style == Style::HiddenEdges) scheduleEdgeOverlay();
    redrawScene();
  }
  if (builds->empty() || !m_jobs) return;
  unsigned& counter = painting ? m_paintSerial : m_faceTintSerial;
  const unsigned serial = ++counter;  // the bodies still waiting are in this job too
  m_jobs->async(painting ? tr("Colouring the deviation map") : tr("Colouring the changed faces"), [builds](Progress p) {
    for (auto& b : *builds) {
      if (p.cancelled()) return;
      if (b.paint) {
        b.out = b.paint->paint(*b.base, b.placed, [p] { return p.cancelled(); });  // the base mesh's arrays, coloured
        continue;
      }
      b.out = BodyPrs::build(b.shape, b.box, true, b.colors);  // the base mesh's own triangles: drawn as the body is picked
      b.out->deflection = b.deflection;
    }
  }, [this, builds, serial, painting](bool ok, const QString&) {
    if (!ok || serial != (painting ? m_paintSerial : m_faceTintSerial)) return;
    bool redraw = false, glowsStale = false;
    for (const auto& b : *builds) {
      const auto tint = m_faceTints.find(b.id);
      const auto item = m_items.find(b.id);
      if (!b.out || tint == m_faceTints.end() || tint->second.ais || tint->second.paint != b.paint || item == m_items.end() || item->second.key != b.key) continue;
      const auto body = Handle(BodyShape)::DownCast(item->second.ais);
      if (body.IsNull()) continue;
      tint->second.ais = tint->second.drawnOn = body.get();
      tint->second.arrays = b.out;  // refined like the body's own once the camera rests close (refineVisible)
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
    if (painting) emit vertexPaintsDrawn();
    if (!redraw) return;
    if (m_style == Style::HiddenEdges) scheduleEdgeOverlay();
    redrawScene();
    m_refineTimer.start();  // zoomed in already: the coloured faces refined too
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
    m_sideView = paneView(m_side);
    layoutSide();
    m_side->show();
    applySideMasks();
    drawSide(true);
  } else {
    dropExtras();
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

Handle(V3d_View) Viewport::navView() const {
  if (m_side && !m_navExtra.IsNull()) return m_navExtra;
  return m_side && m_navSide && !m_sideView.IsNull() ? m_sideView : m_view;
}

Handle(V3d_View) Viewport::paneView(SideView* pane) {
  Handle(V3d_View) view = m_viewer->CreateView();
#if defined(_WIN32)
  Handle(Aspect_Window) win = new WNT_Window(reinterpret_cast<Aspect_Handle>(pane->winId()));
#elif defined(__APPLE__)
  Handle(Aspect_Window) win = opad_make_cocoa_window(reinterpret_cast<void*>(pane->winId()));
#else
  Handle(Aspect_Window) win = new Xw_Window(Handle(OpenGl_GraphicDriver)::DownCast(m_viewer->Driver())->GetDisplayConnection(),
                                            static_cast<Aspect_Drawable>(pane->winId()));
#endif
  view->SetImmediateUpdate(Standard_False);
  view->SetWindow(win);
  if (!win->IsMapped()) win->Map();
  pane->setView(view);
  return view;
}

void Viewport::setSideExtras(const QStringList& captions) {
  QWidget* host = parentWidget();
  const int want = m_side ? int(std::min<qsizetype>(captions.size(), 2)) : 0;
  if (want == int(m_extraPanes.size())) {  // the same panes: their captions
    for (int k = 0; k < want; ++k) m_extraPanes[size_t(k)].widget->setCaption(captions[k]);
    return;
  }
  dropExtras();
  if (!want || !host || !m_initialised) {
    layoutSide();
    redrawScene();
    return;
  }
  trace::Scope scope("Viewport: more panes side by side");
  for (int k = 0; k < want; ++k) {
    Pane pane;
    pane.letter = char('C' + k);
    pane.widget = new SideView(this, host, true);
    pane.widget->setCaption(captions[k]);
    pane.view = paneView(pane.widget);
    m_extraPanes.push_back(pane);
  }
  layoutSide();
  for (const auto& pane : m_extraPanes) pane.widget->show();
  applySideMasks();
  drawSide(true);
}

void Viewport::dropExtras() {
  if (m_extraPanes.empty()) return;
  m_navExtra.Nullify();
  for (auto& pane : m_extraPanes) {
    pane.view->Remove();  // the view goes before its window
    delete pane.widget;
  }
  m_extraPanes.clear();
}

QImage Viewport::grabExtra(int k) {
  if (k < 0 || size_t(k) >= m_extraPanes.size()) return QImage();
  const Pane& pane = m_extraPanes[size_t(k)];
  drawPane(pane.widget, pane.view, true);
  Image_PixMap pix;
  V3d_ImageDumpOptions o;
  o.Width = static_cast<int>(pane.widget->width() * pane.widget->devicePixelRatioF());
  o.Height = static_cast<int>(pane.widget->height() * pane.widget->devicePixelRatioF());
  o.BufferType = Graphic3d_BT_RGB;
  o.ToAdjustAspect = Standard_True;
  if (!pane.view->ToPixMap(pix, o)) return QImage();
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

// Two panes: the parent's left half is A's, the right one this view's (the parent's layout keeps us in its contents
// rectangle). Three: columns A | C | this view. Four: A and this view above, C and D below. Two pixels between them.
void Viewport::layoutSide() {
  QWidget* host = parentWidget();
  if (!host) return;
  if (!m_side) return host->setContentsMargins(m_hostMargins);
  const QRect area = host->rect().marginsRemoved(m_hostMargins);
  constexpr int gap = 2;
  if (m_extraPanes.empty()) {
    const int half = std::max(1, (area.width() - gap) / 2);
    m_side->setGeometry(area.left(), area.top(), half, area.height());
    host->setContentsMargins(m_hostMargins + QMargins(half + gap, 0, 0, 0));
  } else if (m_extraPanes.size() == 1) {
    const int third = std::max(1, (area.width() - 2 * gap) / 3);
    m_side->setGeometry(area.left(), area.top(), third, area.height());
    m_extraPanes[0].widget->setGeometry(area.left() + third + gap, area.top(), third, area.height());
    host->setContentsMargins(m_hostMargins + QMargins(2 * (third + gap), 0, 0, 0));
  } else {
    const int half = std::max(1, (area.width() - gap) / 2), tall = std::max(1, (area.height() - gap) / 2);
    m_side->setGeometry(area.left(), area.top(), half, tall);
    m_extraPanes[0].widget->setGeometry(area.left(), area.top() + tall + gap, half, area.height() - tall - gap);
    m_extraPanes[1].widget->setGeometry(area.left() + half + gap, area.top() + tall + gap, area.width() - half - gap, area.height() - tall - gap);
    host->setContentsMargins(m_hostMargins + QMargins(half + gap, 0, 0, area.height() - tall));
  }
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
    if (object.IsNull()) return;
    m_ctx->SetViewAffinity(object, m_sideView, shown);
    for (const auto& pane : m_extraPanes) m_ctx->SetViewAffinity(object, pane.view, false);  // C and D: their file alone
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
    const bool extra = view == 'C' || view == 'D';
    m_ctx->SetViewAffinity(ais, m_view, (!side && !extra) || (side && view != 'A' && !extra));
    if (side) m_ctx->SetViewAffinity(ais, m_sideView, view == 0 || view == 'A');
    for (const auto& pane : m_extraPanes) m_ctx->SetViewAffinity(ais, pane.view, view == pane.letter);
  }
  if (!m_compareArrows.IsNull())
    for (const auto& pane : m_extraPanes) m_ctx->SetViewAffinity(m_compareArrows, pane.view, false);  // A to B's moves
  if (side) {
    for (const auto& id : m_sideHidden) maskSide(id, nullptr);
    if (!m_extraPanes.empty()) {  // C and D show their file alone: none of the session's own objects
      for (const auto& [id, item] : m_items) maskSide(id, nullptr);
      for (const auto& [id, wire] : m_sketchWires) maskSide(id, nullptr);
    }
  }
  // The layers' boxes are cached per view and the z range comes from them: what a view shows now must not be clipped.
  std::vector<Handle(V3d_View)> views = {m_view, m_sideView};
  for (const auto& pane : m_extraPanes) views.push_back(pane.view);
  for (const Handle(V3d_View)& view : views)
    if (!view.IsNull())
      for (const Graphic3d_ZLayerId layer : {Graphic3d_ZLayerId_Default, Graphic3d_ZLayerId_Topmost}) view->View()->InvalidateZLayerBoundingBox(layer);
  if (side) m_sideView->Invalidate();
  for (const auto& pane : m_extraPanes) pane.view->Invalidate();
  redrawScene();
}

// After a frame of this view (handleViewRedraw) and on the side's own paint: its camera becomes this view's (with its own
// aspect), its look this view's (rendering, background, the section plane); then all of it is drawn again, or only its
// immediate layer when nothing moved.
void Viewport::drawSide(bool full) {
  if (!m_side || m_sideView.IsNull() || !m_initialised) return;
  drawPane(m_side, m_sideView, full);
  for (const auto& pane : m_extraPanes) drawPane(pane.widget, pane.view, full);
}

void Viewport::drawPane(SideView* pane, const Handle(V3d_View)& paneView, bool full) {
  if (!m_side || !pane || paneView.IsNull() || !m_initialised) return;
  const Handle(Graphic3d_Camera)& from = m_view->Camera();
  const Handle(Graphic3d_Camera)& to = paneView->Camera();
  const bool moved = from->ProjectionType() != to->ProjectionType() || from->Scale() != to->Scale() || from->FOVy() != to->FOVy() ||
                     !from->Eye().IsEqual(to->Eye(), 0) || !from->Center().IsEqual(to->Center(), 0) || !from->Up().IsEqual(to->Up(), 0);
  if (moved || full || paneView->IsInvalidated()) {
    Standard_Integer w = 0, h = 0;
    paneView->Window()->Size(w, h);
    to->CopyMappingData(from);
    to->CopyOrientationData(from);
    if (w > 0 && h > 0) to->SetAspect(double(w) / h);
    paneView->ChangeRenderingParams() = m_view->RenderingParams();
    if (paneView->BackgroundColor() != m_view->BackgroundColor()) paneView->SetBackgroundColor(m_view->BackgroundColor());
    Quantity_Color top, bottom, hadTop, hadBottom;
    const Aspect_GradientBackground gradient = m_view->GradientBackground(), had = paneView->GradientBackground();
    gradient.Colors(top, bottom);
    had.Colors(hadTop, hadBottom);
    if (had.BgGradientFillMethod() != gradient.BgGradientFillMethod() || hadTop != top || hadBottom != bottom)
      paneView->SetBgGradientColors(top, bottom, gradient.BgGradientFillMethod(), Standard_False);
    if (paneView->ClipPlanes() != m_view->ClipPlanes()) paneView->SetClipPlanes(m_view->ClipPlanes());
    pane->paintCaption();
    paneView->Invalidate();
    paneView->Redraw();
  } else {
    paneView->RedrawImmediate();  // a hover: the immediate layer is shown in every view
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
    opad::json out = {{"main", ais->ViewAffinity()->IsVisible(mainId)}, {"side", sideId >= 0 && ais->ViewAffinity()->IsVisible(sideId)}};
    opad::json extra = opad::json::array();
    for (const auto& pane : m_extraPanes) extra.push_back(bool(ais->ViewAffinity()->IsVisible(pane.view->View()->Identification())));
    out["extra"] = extra;
    return out;
  };
  opad::json parts = opad::json::array(), bodies = opad::json::object();
  for (const auto& [id, ais] : m_compareParts)
    if (!ais.IsNull()) {
      opad::json part = shown(ais);
      part["id"] = id;
      part["displayed"] = m_ctx->IsDisplayed(ais);
      parts.push_back(part);
    }
  for (const auto& [id, item] : m_items) bodies[id] = shown(item.ais);
  out["parts"] = parts;
  out["bodies"] = bodies;
  if (!m_side) return out;
  out["sideCamera"] = camera(m_sideView->Camera());
  out["sideRect"] = {m_side->x(), m_side->y(), m_side->width(), m_side->height()};
  out["caption"] = m_side->caption().toStdString();
  opad::json extras = opad::json::array();
  for (const auto& pane : m_extraPanes)
    extras.push_back({{"caption", pane.widget->caption().toStdString()}, {"rect", {pane.widget->x(), pane.widget->y(), pane.widget->width(), pane.widget->height()}},
                      {"camera", camera(pane.view->Camera())}});
  out["extras"] = extras;
  return out;
}
