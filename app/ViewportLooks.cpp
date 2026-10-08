// Per-body looks (UI-121): the layers of LookDeltas that features set, composed per body (BodyLook.hpp) and applied by a
// sliced job. Colour and opacity change the object's aspects in place (SynchronizeAspects): no Redisplay, which rebuilds
// the selection owners and drops a selected body from the selection, and no recompute, which walks the triangulation
// again for a body drawn through the stock AIS_Shape path. Offsets go through SetLocation, so picking follows.
#include "Viewport.hpp"

#include <AIS_TexturedShape.hxx>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_PointAspect.hxx>
#include <SelectMgr_ViewerSelector.hxx>
#include <TColStd_ListOfInteger.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "Drawing2D.hpp"
#include "Jobs.hpp"
#include "opad/canvas.hpp"
#include "opad/geometry.hpp"

namespace {
Quantity_Color rgb(const std::array<double, 3>& c) {
  return Quantity_Color(std::clamp(c[0], 0.0, 1.0), std::clamp(c[1], 0.0, 1.0), std::clamp(c[2], 0.0, 1.0), Quantity_TOC_sRGB);
}
// A linetype as the view draws it, worked out once per linetype, pattern and scale (the UI thread's).
drawing2d::LinePattern linePatternOf(const drawing2d::LineStyle& style, double pixelsPerMm) {
  static std::map<std::string, drawing2d::LinePattern> known;
  std::string key = style.linetype + "|" + std::to_string(pixelsPerMm);
  for (double d : style.pattern) key += "|" + std::to_string(d);
  if (const auto it = known.find(key); it != known.end()) return it->second;
  return known[key] = drawing2d::linePattern(drawing2d::dashes(style.linetype, style.pattern), pixelsPerMm);
}
looks::GhostStyle ghostOf(const Tokens& t) { return {{t.ghost.redF(), t.ghost.greenF(), t.ghost.blueF()}, t.ghost.alphaF()}; }  // the theme's role
}  // namespace

bool Viewport::layered() const {
  return std::any_of(m_lookLayers.begin(), m_lookLayers.end(), [](const auto& layer) { return !layer.empty(); });
}

BodyLook Viewport::composeLook(const opad::Node& body) const {
  BodyLook base;
  base.color = body.color;
  base.opacity = body.opacity;
  if (body.representation == "drawing2d" && body.raster.is_null()) {  // a drawing's lines (UI-10), in its layer's weight and type (UI-89)
    if (!body.has_color) base.color = drawingInk();  // DXF colour 7 and no colour: light on dark, dark on light
    const drawing2d::LineStyle style = drawing2d::lineStyle(m_doc->scene, body);  // or its own (UI-92)
    base.lineWidth = lineWidth(drawing2d::linePoints(style.lineweight));  // hairlines at least one screen pixel wide on any display and render scale
    const drawing2d::LinePattern dashes = linePatternOf(style, drawing2d::kPatternPixelsPerMm * style.scale * displayScale() * renderScale());
    base.linePattern = dashes.bits;
    base.lineFactor = dashes.factor;
  }
  if (!body.canvas.is_null()) {  // a canvas's flags (opad/canvas.hpp): picked in the view or only from the browser, drawn through the model
    const opad::CanvasFlags flags = opad::CanvasFlags::of(body.canvas);
    base.pickable = flags.selectable;
    if (flags.through) base.layer = m_throughLayer;
  }
  std::array<const LookDelta*, kLookSources> found{};
  if (layered()) {
    const opad::Scene& scene = m_doc->scene;
    for (size_t s = 0; s < kLookSources; ++s) {
      const auto& layer = m_lookLayers[s];
      if (layer.empty()) continue;
      for (const opad::Node* at = &body; at; at = at->parent.empty() ? nullptr : scene.node(at->parent))  // the nearest entry
        if (const auto it = layer.find(at->id); it != layer.end()) {
          found[s] = &it->second;
          break;
        }
    }
  }
  return looks::compose(base, found, ghostsPickable(), ghostOf(m_tokens));
}

BodyLook Viewport::bodyLook(const std::string& body) const {
  const opad::Node* n = m_doc->scene.node(body);
  return n ? composeLook(*n) : m_doc->scene.sketch(body) ? sketchLook(body) : BodyLook();
}

BodyLook Viewport::shownLook(const std::string& body) const {
  if (const auto it = m_items.find(body); it != m_items.end()) return it->second.look;
  const auto sketch = m_sketchWires.find(body);
  return sketch == m_sketchWires.end() ? BodyLook() : sketch->second.look;
}

BodyLook Viewport::sketchLook(const std::string& id) const {
  BodyLook base;
  base.color = {m_tokens.sel.redF(), m_tokens.sel.greenF(), m_tokens.sel.blueF()};
  std::array<const LookDelta*, kLookSources> found{};
  for (size_t s = 0; s < kLookSources; ++s)
    if (const auto it = m_lookLayers[s].find(id); it != m_lookLayers[s].end()) found[s] = &it->second;
  return looks::compose(base, found, ghostsPickable(), ghostOf(m_tokens));
}

QString Viewport::hoverName(const std::string& node) const {
  const auto it = m_items.find(node);
  const bool ghost = it != m_items.end() && it->second.look.ghost;
  return m_doc->nodeName(node) + (ghost ? tr(" (inactive)") : m_doc->scene.effectively_locked(node) ? tr(" (locked)") : QString());
}

std::string Viewport::drawnAt(const QPointF& point) {
  if (!m_initialised || m_navSelector.IsNull()) return {};
  const Graphic3d_Vec2i at = devicePos(point);
  m_navSelector->Pick(at.x(), at.y(), m_view);
  for (int i = 1; i <= m_navSelector->NbPicked(); ++i) {  // nearest first
    const auto node = m_navNodes.find(m_navSelector->Picked(i)->Selectable().get());
    const auto item = node == m_navNodes.end() ? m_items.end() : m_items.find(node->second);
    if (item != m_items.end() && m_ctx->IsDisplayed(item->second.ais)) return node->second;
  }
  return {};
}

std::string Viewport::ghostAt(const QPointF& point) {
  const std::string id = drawnAt(point);
  const auto item = m_items.find(id);
  return item != m_items.end() && item->second.look.ghost ? id : std::string();
}

gp_Vec Viewport::lookOffset(const std::string& node) const {
  std::array<double, 3> o{0, 0, 0};
  if (const auto it = m_items.find(node); it != m_items.end()) o = it->second.look.offset;  // as drawn
  else if (const opad::Node* n = layered() ? m_doc->scene.node(node) : nullptr) o = composeLook(*n).offset;  // a component
  return gp_Vec(o[0], o[1], o[2]);
}

std::unordered_map<std::string, opad::Vec3> Viewport::shownOffsets() const {
  std::unordered_map<std::string, opad::Vec3> out;
  for (const auto& [id, item] : m_items)
    if (item.look.offset != std::array<double, 3>{0, 0, 0}) out[id] = {item.look.offset[0], item.look.offset[1], item.look.offset[2]};
  return out;
}

opad::Vec3 Viewport::shownOffset(const std::string& node) const {
  const gp_Vec o = lookOffset(node);
  return {o.X(), o.Y(), o.Z()};
}

void Viewport::setLookLayer(LookSource source, std::map<std::string, LookDelta> deltas) {
  auto& layer = m_lookLayers[static_cast<size_t>(source)];
  std::unordered_map<std::string, LookDelta> next(std::make_move_iterator(deltas.begin()), std::make_move_iterator(deltas.end()));
  if (next == layer) return;
  layer = std::move(next);
  scheduleLooks();
}

void Viewport::setGhostsPickable(bool on) {
  const bool was = ghostsPickable();
  m_ghostsPickable = on;
  referencesChanged(was);
}

void Viewport::referencesChanged(bool wasPickable) {
  if (ghostsPickable() != wasPickable && layered()) scheduleLooks();  // nothing to do without a layer that could ghost
}

void Viewport::scheduleLooks() {
  if (!m_initialised || !m_jobs) return;  // displayBody composes the look of every body it adds
  for (const auto& [id, item] : m_items)
    if (m_lookQueued.insert(id).second) m_lookQueue.push_back(id);
  for (const auto& [id, wire] : m_sketchWires)
    if (m_lookQueued.insert(id).second) m_lookQueue.push_back(id);
  if (m_lookJob || m_lookQueue.empty()) return;
  struct Pass {
    bool selected = false, moved = false, shown = false;  // shown: a body shown or hidden
    size_t changed = 0;
  };
  auto pass = std::make_shared<Pass>();
  // One body per step (compose, compare, and only a changed one touches AIS); the runner packs steps into ~10 ms slices.
  m_lookJob = m_jobs->sliced(tr("Updating %1 bodies").arg(m_lookQueue.size()), [this, pass](Job&) {
    if (m_lookQueue.empty()) return false;
    const std::string id = std::move(m_lookQueue.front());
    m_lookQueue.pop_front();
    m_lookQueued.erase(id);
    const opad::Node* n = m_doc->scene.node(id);
    if (const auto it = m_items.find(id); it != m_items.end() && n && !n->body_missing) {
      const BodyLook look = composeLook(*n);
      if (!(look == it->second.look)) {
        pass->selected = pass->selected || m_ctx->IsSelected(it->second.ais);
        pass->shown = pass->shown || look.visible != it->second.look.visible;
        pass->moved = applyLook(id, it->second, look) || pass->moved;
        if ((++pass->changed & 127) == 0) redrawScene();  // the bodies change as the job goes
      }
    } else if (const auto wire = m_sketchWires.find(id); wire != m_sketchWires.end()) {
      const BodyLook look = sketchLook(id);
      if (!(look == wire->second.look)) {
        pass->selected = pass->selected || m_ctx->IsSelected(wire->second.ais);
        applySketchLook(wire->second, look);
        ++pass->changed;
      }
    }
    return !m_lookQueue.empty();
  }, [this, pass](bool completed) {
    m_lookJob = nullptr;
    if (pass->selected) m_ctx->HilightSelected(Standard_False);  // the highlight was made from the old aspects
    if (pass->moved) {
      clearCenters();  // circle centres were found where the bodies were
      applySelectionLayers();  // the glows of selected bodies follow them
      if (!m_subHl.IsNull() || m_subJob) refreshSubHighlight();
      if (!m_notes.empty()) {  // notes and hand drawings follow the parts they are pinned to
        m_noteCamera.Reset();
        QMetaObject::invokeMethod(this, [this] { emit notesMoved(); }, Qt::QueuedConnection);
      }
    }
    if (pass->moved || pass->shown) {  // a check's colours and overlap lie on the bodies as drawn (exploded, hidden by a look)
      if (!m_tintWanted.empty()) buildCheckTints();
      placeOverlap();
    }
    redrawScene();  // ghosts or not, translucency stays order-independent (setRenderQuality, UI-39)
    if (trace::enabled()) trace::log(QStringLiteral("looks: %1 bodies changed%2").arg(pass->changed).arg(completed ? "" : " (stopped)"));
    if (m_style == Style::HiddenEdges && pass->changed) scheduleEdgeOverlay();  // shown, hidden or moved: the edges follow
    if (completed) emit looksApplied();
  }, JobKind::Background);
}

bool Viewport::applyLook(const std::string& id, Item& item, const BodyLook& look) {
  const BodyLook was = item.look;
  item.look = look;
  const Handle(AIS_Shape)& ais = item.ais;
  if (look.color != was.color || look.opacity != was.opacity || look.ghost != was.ghost || look.lineWidth != was.lineWidth || look.linePattern != was.linePattern ||
      look.lineFactor != was.lineFactor) {
    ais->SetColor(rgb(look.color));
    ais->SetTransparency(1.0 - look.opacity);
    applyStyle(ais, &look);  // a ghost's edges fade with it
    if (const Handle(BodyShape) body = Handle(BodyShape)::DownCast(ais)) body->syncPainted(look.ghost);  // face colours fade too
    ais->SynchronizeAspects();
  }
  if (look.visible != was.visible) {
    if (!look.visible) {
      m_ctx->Erase(ais, Standard_False);
      if (const auto glow = m_bodyGlows.find(ais.get()); glow != m_bodyGlows.end()) {
        m_ctx->Remove(glow->second, Standard_False);
        m_bodyGlows.erase(glow);
      }
    } else if (!m_previewHidden.count(id)) {  // a feature preview stands in for it: clearPreviewBodies shows it again
      m_ctx->Display(ais, m_style == Style::Wireframe ? AIS_WireFrame : !Handle(AIS_TexturedShape)::DownCast(ais).IsNull() ? 3 : AIS_Shaded, -1, Standard_False);
    }
    if (!item.navigation.IsNull()) {  // the orbit pivot never lands on what is not drawn
      if (look.visible) m_navSelection->Activate(item.navigation, 0);
      else m_navSelection->Deactivate(item.navigation, 0);
    }
  }
  if (look.shownPickable() != was.shownPickable() || look.visible != was.visible) {
    if (look.shownPickable()) activateSelection(ais);
    else m_ctx->Deactivate(ais);
  }
  if (look.layer != was.layer && !m_ctx->IsSelected(ais)) m_ctx->SetZLayer(ais, look.layer);  // selected: Topmost until deselected
  if (look.offset == was.offset) return false;
  placeItem(id, item);  // offset after the placement
  return true;
}

void Viewport::applySketchLook(SketchWire& wire, const BodyLook& look) {
  const BodyLook was = wire.look;
  wire.look = look;
  const Handle(AIS_Shape)& ais = wire.ais;
  if (look.color != was.color || look.opacity != was.opacity) {
    const Quantity_Color c = rgb(looks::mix(look.color, {m_tokens.vp.redF(), m_tokens.vp.greenF(), m_tokens.vp.blueF()}, 1 - look.opacity));
    const Handle(Prs3d_Drawer)& d = ais->Attributes();
    for (const Handle(Prs3d_LineAspect)& line : {d->WireAspect(), d->LineAspect(), d->FreeBoundaryAspect()})
      if (!line.IsNull()) line->SetColor(c);
    if (!d->PointAspect().IsNull()) d->PointAspect()->SetColor(c);
    ais->SynchronizeAspects();
  }
  if (look.visible != was.visible) {
    if (look.visible) m_ctx->Display(ais, AIS_WireFrame, -1, Standard_False);
    else m_ctx->Erase(ais, Standard_False);
    for (const auto& image : wire.backdrops)
      if (look.visible) m_ctx->Display(image, 3, -1, Standard_False);
      else m_ctx->Erase(image, Standard_False);
  }
  if (look.shownPickable() != was.shownPickable() || look.visible != was.visible) {
    if (look.shownPickable()) activateSelection(ais);
    else m_ctx->Deactivate(ais);
  }
  if (look.layer != was.layer && !m_ctx->IsSelected(ais)) m_ctx->SetZLayer(ais, look.layer);
  if (look.offset != was.offset) {
    gp_Trsf moved;
    moved.SetTranslation(gp_Vec(look.offset[0], look.offset[1], look.offset[2]));
    const TopLoc_Location at = look.offset == std::array<double, 3>{0, 0, 0} ? TopLoc_Location() : TopLoc_Location(moved);
    m_ctx->SetLocation(ais, at);
    for (const auto& image : wire.backdrops) m_ctx->SetLocation(image, at);
  }
}

// ---------------------------------------------------------------- benches (OPAD_BENCH_LOOKS)
opad::json Viewport::benchLookState(const std::string& body) const {
  const auto it = m_items.find(body);
  const auto wire = m_sketchWires.find(body);
  if (!m_initialised || (it == m_items.end() && wire == m_sketchWires.end())) return {};
  const Handle(AIS_Shape)& ais = it != m_items.end() ? it->second.ais : wire->second.ais;
  TColStd_ListOfInteger modes;
  m_ctx->ActivatedModes(ais, modes);
  Quantity_Color c;
  if (it != m_items.end()) ais->Color(c);
  else c = ais->Attributes()->WireAspect()->Aspect()->Color();  // a sketch: the colour its lines are drawn in
  double r = 0, g = 0, b = 0;
  c.Values(r, g, b, Quantity_TOC_sRGB);
  const gp_XYZ t = ais->LocalTransformation().TranslationPart();
  const Handle(Graphic3d_AspectLine3d)& line = ais->Attributes()->WireAspect()->Aspect();
  const Graphic3d_ZLayerSettings& layer = m_viewer->ZLayerSettings(ais->ZLayer());
  opad::json faces = nullptr;  // Compare's coloured faces: the faces of each colour, whether drawn yet
  if (const auto tint = m_faceTints.find(body); tint != m_faceTints.end()) {
    opad::json counts = opad::json::array();
    for (size_t c = 0; c < tint->second.colors->colors.size(); ++c) counts.push_back(std::count(tint->second.colors->face.begin(), tint->second.colors->face.end(), int(c)));
    const auto shape = Handle(BodyShape)::DownCast(ais);
    faces = {{"faces", counts}, {"drawn", tint->second.ais == ais.get() && !shape.IsNull() && shape->displayPrs() && !shape->displayPrs()->painted.empty()}};
  }
  return {{"faceTint", faces}, {"displayed", m_ctx->IsDisplayed(ais)}, {"activated", modes.Extent()}, {"transparency", ais->Transparency()}, {"color", {r, g, b}},
          {"layer", ais->ZLayer()}, {"depth_test", layer.ToEnableDepthTest()}, {"depth_write", layer.ToEnableDepthWrite()},
          {"translation", {t.X(), t.Y(), t.Z()}}, {"selected", m_ctx->IsSelected(ais)},
          {"lineWidth", line->Width()}, {"lineType", int(line->LineType())}, {"linePattern", line->LinePattern()}, {"lineFactor", line->LineStippleFactor()},
          {"lineColor", {line->Color().Red(), line->Color().Green(), line->Color().Blue()}}};
}

std::string Viewport::benchPickAt(int x, int y, opad::Vec3* at) {
  if (!m_initialised) return {};
  m_ctx->MoveTo(x, y, m_view, Standard_False);
  std::string found;
  if (m_ctx->HasDetected())
    if (const auto node = m_nodeOf.find(m_ctx->DetectedInteractive().get()); node != m_nodeOf.end()) {
      found = node->second;
      if (at && m_ctx->MainSelector()->NbPicked() > 0) {
        const gp_Pnt p = m_ctx->MainSelector()->PickedPoint(1);
        *at = {p.X(), p.Y(), p.Z()};
      }
    }
  m_ctx->ClearDetected(Standard_False);
  return found;
}

void Viewport::benchClickAt(int x, int y) {
  if (!m_initialised) return;
  m_view->Redraw();  // the picker clips to the z range of the last frame
  const QPointF local(x / viewScale().x(), y / viewScale().y());
  for (const QEvent::Type type : {QEvent::MouseMove, QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
    QMouseEvent e(type, local, mapToGlobal(local), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, type == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &e);
  }
  QScopedValueRollback<bool> flushing(m_flushingViewEvents, true);
  FlushViewEvents(m_ctx, m_view, Standard_True);  // what the next frame does (a hidden window has none)
}

bool Viewport::benchBodyPoint(const std::string& body, int& x, int& y) {
  if (!m_initialised) return false;
  m_view->Redraw();  // the picker clips to the z range of the last frame
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  // Inside it, not on a rim the pixel tolerance reaches past: a grid point whose eight neighbours find it too, the one
  // nearest the middle of all that do.
  constexpr int n = 48;
  std::vector<char> hit(n * n, 0);
  double cx = 0, cy = 0;
  int count = 0;
  for (int j = 1; j < n; ++j)
    for (int i = 1; i < n; ++i)
      if (benchPickAt(w * i / n, h * j / n) == body) hit[j * n + i] = 1, cx += i, cy += j, ++count;
  if (!count) return false;
  cx /= count, cy /= count;
  double best = 1e9;
  for (int j = 2; j < n - 1; ++j)
    for (int i = 2; i < n - 1; ++i) {
      bool inside = hit[j * n + i];
      for (int dj = -1; dj <= 1 && inside; ++dj)
        for (int di = -1; di <= 1 && inside; ++di) inside = hit[(j + dj) * n + i + di];
      if (inside && std::hypot(i - cx, j - cy) < best) best = std::hypot(i - cx, j - cy), x = w * i / n, y = h * j / n;
    }
  return best < 1e9;
}
