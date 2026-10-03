#include "SheetCanvas.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QKeyEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyleOptionGraphicsItem>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "AppDocument.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "opad/drawing/paint.hpp"

using opad::drawing::Display;
using opad::drawing::Vec2;

namespace {

// A caption in device pixels at a point of the item (the painter is in paper mm).
void caption(QPainter* p, const QPointF& at, const QString& text, const QColor& fg, const QColor& bg) {
  const QPointF device = p->transform().map(at);
  p->save();
  p->resetTransform();
  QFont font = theme::ui(11);
  p->setFont(font);
  const QRectF box = QFontMetricsF(font).boundingRect(text).adjusted(-5, -2, 5, 2);
  const QRectF r(device + QPointF(4, 4), box.size());
  p->setPen(Qt::NoPen);
  p->setBrush(bg);
  p->drawRoundedRect(r, 3, 3);
  p->setPen(fg);
  p->drawText(r, Qt::AlignCenter, text);
  p->restore();
}
}  // namespace

// A part of the sheet drawn from its display list: the paper's own drawing or one view.
class SheetPartItem : public QGraphicsItem {
 public:
  std::shared_ptr<const Display> display;
  double paperW = 0, paperH = 0;  // the paper the display was drawn on (paper mm, scene y = paperH - y)
  QRectF displayRect;             // local: its primitives' bounds
  // Drawn from a picture rendered on a worker at the zoom in use (painting thousands of curves and the title block's text
  // takes far longer than a frame); until a new display's picture comes, the last one stays where it was seen.
  QImage picture;
  QRectF pictureRect;
  double pictureScale = 0;
  const Display* pictureOf = nullptr;  // the display the picture shows
  const Display* askedOf = nullptr;    // a picture on its way: of what, at what scale, of which part of it
  double askedScale = 0;
  QRectF askedRect;
  void setPicture(QImage image, const QRectF& rect, double scale, const Display* of) {
    prepareGeometryChange();
    picture = std::move(image), pictureRect = rect, pictureScale = scale, pictureOf = of;
    update();
  }
  bool current() const { return display && !picture.isNull() && pictureOf == display.get(); }
  // b: the display's bounds, measured on the worker (arcs and text sampled: too slow here for a big view).
  void setDisplay(std::shared_ptr<const Display> d, double w, double h, const std::array<double, 4>& b = {0, 0, 0, 0}) {
    prepareGeometryChange();
    display = std::move(d);
    paperW = w, paperH = h;
    if (!display) picture = QImage(), pictureOf = nullptr;
    displayRect = display && b[2] > b[0] ? QRectF(QPointF(b[0], h - b[3]), QPointF(b[2], h - b[1])) : QRectF();
    update();
  }
  void paintDisplay(QPainter* p) {
    if (picture.isNull()) return;
    p->save();
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    p->drawImage(pictureRect, picture);
    p->restore();
  }
};

class SheetPaperItem : public SheetPartItem {
 public:
  double w = 420, h = 297;
  void resize(double width, double height) {
    prepareGeometryChange();
    w = width, h = height;
  }
  QRectF boundingRect() const override { return QRectF(-1, -1, w + 4, h + 4) | displayRect; }
  void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(0, 0, 0, theme::current().dark ? 110 : 45));
    p->drawRect(QRectF(1.5, 1.5, w, h));
    p->setBrush(Qt::white);
    p->drawRect(QRectF(0, 0, w, h));
    paintDisplay(p);
  }
};

class SheetViewItem : public SheetPartItem {
 public:
  std::string id, parent, kind, side;
  double gap = 20;
  bool aligned = true, draft = false, final = false, hovered = false;
  QElapsedTimer stale;  // since its linework may be out of date: marked after a moment (a cached view comes back at once)
  void markStale() {
    if (!final) return;
    final = false;
    stale.start();
  }
  QString error;
  QRectF drawn;  // local (the scene when it was drawn): the frame the display was drawn in
  SheetViewItem() {
    setFlag(ItemIsSelectable);
    setZValue(1);
  }
  QRectF frame() const { return drawn.translated(pos()); }  // scene
  void setFrame(const QRectF& box) {
    if (!display) {
      prepareGeometryChange();
      drawn = box;
      setPos(0, 0);
    } else {
      setPos(box.center() - drawn.center());
    }
    update();
  }
  void show(std::shared_ptr<const Display> d, const std::array<double, 4>& bounds, const QRectF& box, bool isDraft, double w, double h) {
    prepareGeometryChange();
    pictureRect.translate(pos());  // the last picture stays where it was seen until the new one comes
    drawn = box;
    setPos(0, 0);
    draft = isDraft;
    final = !isDraft;
    setDisplay(std::move(d), w, h, bounds);
  }
  QRectF boundingRect() const override { return (drawn | displayRect | pictureRect).adjusted(-3, -3, 3, 3); }
  QPainterPath shape() const override {
    QPainterPath path;
    path.addRect(drawn.adjusted(-1.5, -1.5, 1.5, 1.5));
    return path;
  }
  void paint(QPainter* p, const QStyleOptionGraphicsItem* option, QWidget*) override {
    paintDisplay(p);
    const Tokens& t = theme::current();
    const bool selected = option->state & QStyle::State_Selected;
    if (selected || hovered || !display) {
      QPen pen(selected ? t.sel : !display ? t.fg3 : t.fg2, selected ? 1.5 : 1, Qt::DashLine);
      pen.setCosmetic(true);
      p->setPen(pen);
      p->setBrush(selected ? QColor(t.sel.red(), t.sel.green(), t.sel.blue(), 18) : Qt::NoBrush);
      p->drawRect(drawn.adjusted(-1.5, -1.5, 1.5, 1.5));
    }
    if (!error.isEmpty()) caption(p, drawn.topLeft(), error, Qt::white, t.error);
    else if (!final && (!stale.isValid() || stale.elapsed() > 250)) caption(p, drawn.topLeft() + QPointF(-1.5, -1.5), QObject::tr("Updating…"), t.fg, QColor(t.bg2.red(), t.bg2.green(), t.bg2.blue(), 230));
  }
};

// Snap guides, and the frame of a view being placed.
class SheetGuides : public QGraphicsItem {
 public:
  QRectF area;
  std::vector<QLineF> lines;
  QRectF ghost;
  QString label;
  void setArea(const QRectF& r) {
    prepareGeometryChange();
    area = r;
  }
  QRectF boundingRect() const override { return area.adjusted(-area.width(), -area.height(), area.width(), area.height()); }
  void set(std::vector<QLineF> l, QRectF g = {}, QString text = {}) {
    lines = std::move(l), ghost = g, label = std::move(text);
    update();
  }
  void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override {
    const Tokens& t = theme::current();
    QPen pen(t.sel, 1, Qt::DashLine);
    pen.setCosmetic(true);
    p->setPen(pen);
    for (const auto& l : lines) p->drawLine(l);
    if (ghost.isValid()) {
      QPen frame(t.sel, 1.5, Qt::DashLine);
      frame.setCosmetic(true);
      p->setPen(frame);
      p->setBrush(QColor(t.sel.red(), t.sel.green(), t.sel.blue(), 30));
      p->drawRect(ghost);
      if (!label.isEmpty()) caption(p, ghost.topLeft(), label, t.onsel, t.sel);
    }
  }
};

SheetCanvas::SheetCanvas(AppDocument* doc, JobRunner* jobs, QWidget* parent) : QGraphicsView(parent), m_doc(doc), m_jobs(jobs) {
  setObjectName("sheetCanvas");
  setLayoutDirection(Qt::LeftToRight);  // paper is never mirrored
  m_scene = new QGraphicsScene(this);
  m_scene->setItemIndexMethod(QGraphicsScene::NoIndex);
  setScene(m_scene);
  m_paper = new SheetPaperItem;
  m_scene->addItem(m_paper);
  m_guides = new SheetGuides;
  m_guides->setZValue(10);
  m_scene->addItem(m_guides);
  setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
  setResizeAnchor(QGraphicsView::AnchorViewCenter);
  setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
  setFrameShape(QFrame::NoFrame);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  setPaperSize(420, 297);
  m_poll.setInterval(30);
  connect(&m_poll, &QTimer::timeout, this, &SheetCanvas::drain);
  m_render.setSingleShot(true);
  m_render.setInterval(350);
  connect(&m_render, &QTimer::timeout, this, &SheetCanvas::renderPictures);
  connect(m_scene, &QGraphicsScene::selectionChanged, this, [this] {
    if (!m_quietSelection) emitSelection();
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { viewport()->update(); });
}

SheetCanvas::~SheetCanvas() {
  disconnect(m_scene, nullptr, this, nullptr);  // the scene goes after the members: its items' selection is no news
  if (m_job) m_job->cancel();
  if (m_place.job) m_place.job->cancel();
}

// ---------------------------------------------------------------- sheet and parts
void SheetCanvas::setPaperSize(double w, double h) {
  if (w == m_paperW && h == m_paperH && m_scene->sceneRect().width() > 0) return;
  m_paperW = w, m_paperH = h;
  m_paper->resize(w, h);
  m_guides->setArea(QRectF(0, 0, w, h));
  m_scene->setSceneRect(-1.5 * w, -1.5 * h, 4 * w, 4 * h);  // room to pan the paper off centre
}

QRectF SheetCanvas::sceneBox(const std::array<double, 4>& b) const { return QRectF(QPointF(b[0], m_paperH - b[3]), QPointF(b[2], m_paperH - b[1])); }

SheetViewItem* SheetCanvas::viewItem(const std::string& id) const {
  const auto it = m_views.find(id);
  return it == m_views.end() ? nullptr : it->second;
}

void SheetCanvas::setSheet(const std::string& id) {
  if (id == m_sheet) return refresh();
  cancelPlacement();
  m_sheet = id;
  m_fitted = false;
  m_draftsShown = 0;
  dropViews({});
  m_paper->setDisplay(nullptr, 0, 0);
  m_outbox.reset();
  refresh();
  if (isVisible()) fitSheet();
}

void SheetCanvas::refresh() {
  if (!isVisible() || !m_doc->hasDocument) {
    m_dirty = true;
    return;
  }
  m_dirty = false;
  const opad::Sheet* s = m_sheet.empty() ? nullptr : m_doc->scene.sheet(m_sheet);
  if (!s) {
    dropViews({});
    m_paper->setDisplay(nullptr, 0, 0);
    m_outbox.reset();
    return;
  }
  const bool resized = s->width != m_paperW || s->height != m_paperH;
  setPaperSize(s->width, s->height);
  if (resized && m_fitted) fitSheet();  // another paper (a template file's, Sheet properties): all of it in view again
  syncFromScene();
  for (auto& [id, item] : m_views) item->markStale();
  QTimer::singleShot(300, viewport(), [vp = viewport()] { vp->update(); });  // the marks of those still waiting
  if (m_job || m_again) {  // after the running one: its worker stops at its next step
    m_again = true;
    if (m_job) m_job->cancel();
    return;
  }
  start();
}

void SheetCanvas::pause() {
  ++m_paused;
  if (m_job) {
    m_again = true;
    m_job->cancel();
  }
}

void SheetCanvas::resume() {
  if (m_paused > 0 && --m_paused == 0 && (m_again || m_dirty) && !m_job) {
    m_again = false;
    refresh();
  }
}

void SheetCanvas::syncFromScene() {
  const opad::Sheet* s = m_doc->scene.sheet(m_sheet);
  std::set<std::string> keep;
  if (s)
    for (const auto& id : s->views) {
      const opad::SheetView* v = m_doc->scene.sheet_view(id);
      if (!v) continue;
      keep.insert(id);
      SheetViewItem*& item = m_views[id];
      if (!item) {
        item = new SheetViewItem;
        item->id = id;
        item->setAcceptHoverEvents(false);
        m_scene->addItem(item);
      }
      item->kind = v->kind;
      item->parent = v->parent;
      item->side = v->def.value("side", "");
      item->gap = v->def.value("gap", 20.0);
      item->aligned = v->def.value("align", true);
    }
  dropViews(keep);
}

void SheetCanvas::dropViews(const std::set<std::string>& keep) {
  bool selected = false;
  m_quietSelection = true;  // the scene tells of each selected item it loses: once, after
  for (auto it = m_views.begin(); it != m_views.end();) {
    if (keep.count(it->first)) {
      ++it;
      continue;
    }
    selected = selected || it->second->isSelected();
    delete it->second;
    it = m_views.erase(it);
  }
  m_quietSelection = false;
  if (selected) emitSelection();
}

void SheetCanvas::start() {
  if (m_paused) {  // a command waits for the document: after it
    m_again = true;
    return;
  }
  const std::string id = m_sheet;
  auto box = std::make_shared<Outbox>();
  m_outbox = box;
  const QString phase = tr("Projecting the views");
  QPointer<SheetCanvas> self(this);
  m_job = m_doc->readAsync(
      m_jobs, tr("Drawing the sheet"),
      [box, id, phase](const opad::Document& doc, const opad::Scene& scene, Progress p) {
        using namespace opad::drawing;
        const opad::Sheet* sheet = scene.sheet(id);
        if (!sheet) return;
        const auto send = [&](Part part) {
          std::lock_guard<std::mutex> lock(box->mu);
          box->parts.push_back(std::move(part));
        };
        p.setPhase(phase, -1);
        const auto frames = opad::drawing::layout(doc, scene, *sheet);
        Part f;
        f.frames = frames;
        send(std::move(f));
        opad::json skipped = opad::json::array();
        auto paper = std::make_shared<Display>();
        draw_paper(*paper, doc, scene, *sheet);
        draw_items(*paper, doc, scene, *sheet, frames, "", skipped);
        Part pp;
        pp.kind = Part::Paper;
        pp.display = paper;
        pp.bounds = paper->bounds();
        send(std::move(pp));
        for (size_t i = 0; i < frames.size(); ++i) {
          if (p.cancelled()) return;
          const ViewFrame& fr = frames[i];
          const opad::SheetView* v = scene.sheet_view(fr.id);
          if (!v || !fr.error.empty()) continue;
          const ViewSpec spec = view_spec(scene, *v);
          const auto progress = [&](double t, const std::string&) {
            p.setPhase(phase, t < 0 ? -1 : static_cast<int>(100 * (static_cast<double>(i) + t) / static_cast<double>(frames.size())));
            return !p.cancelled();
          };
          const auto part = [&](const ViewGeometry& g, bool draft) {
            auto out = std::make_shared<Display>();
            draw_view(*out, fr, *v, g);
            if (!draft) draw_items(*out, doc, scene, *sheet, frames, fr.id, skipped);
            Part vp;
            vp.kind = Part::View;
            vp.id = fr.id;
            vp.display = out;
            vp.box = fr.box;
            vp.bounds = out->bounds();
            vp.draft = draft;
            send(std::move(vp));
          };
          auto g = cached_projection(doc, scene, spec);
          if (!g) {
            // Never projected as it is now: a quick draft first where the final linework comes from the exact tier (slow from
            // a few hundred faces on); a big model's hybrid tier is quicker than meshing it for a draft.
            if (spec.quality != Quality::Draft && choose_tier(doc, scene, spec) == Quality::Exact) {
              ViewSpec draft = spec;
              draft.quality = Quality::Draft;
              part(*project(doc, scene, draft, progress), true);
            }
            g = project(doc, scene, spec, progress);
          }
          part(*g, false);
        }
      },
      [self, box](bool ok, const QString& error) {
        if (!self) return;
        if (box == self->m_outbox) self->drain();
        self->m_job = nullptr;
        self->m_poll.stop();
        if (!ok && error != "cancelled") trace::log("sheet: " + error);
        if ((self->m_again || self->m_dirty) && !self->m_paused) {
          self->m_again = false;
          self->refresh();
        }
      });
  if (!m_job) {  // the document is being read or recomputed: again in a moment
    m_again = false;
    QTimer::singleShot(150, this, [this] {
      if (!m_job) refresh();
    });
    return;
  }
  m_poll.start();
}

void SheetCanvas::drain() {
  if (!m_outbox) return;
  std::vector<Part> parts;
  {
    std::lock_guard<std::mutex> lock(m_outbox->mu);
    parts.swap(m_outbox->parts);
  }
  if (parts.empty()) return;
  for (auto& p : parts) apply(p);
  emit partsArrived();
}

void SheetCanvas::apply(Part& part) {
  if (part.kind == Part::Frames) {  // a new pass: every view is drawn again (at once when its projection is cached)
    for (const auto& f : part.frames) {
      SheetViewItem* item = viewItem(f.id);
      if (!item) continue;
      item->markStale();
      item->error = QString::fromStdString(f.error);
      const bool none = f.box[2] <= f.box[0] && f.box[3] <= f.box[1];
      item->setFrame(none ? QRectF(toScene(f.at) - QPointF(20, 10), QSizeF(40, 20)) : sceneBox(f.box));
    }
    if (m_drag.active)  // the user is dragging: keep what they see
      for (const auto& [it, o] : m_drag.origins) it->setPos(o + m_drag.delta);
  } else if (part.kind == Part::Paper) {
    m_paper->setDisplay(part.display, m_paperW, m_paperH, part.bounds);
    renderPictures();
  } else if (SheetViewItem* item = viewItem(part.id)) {
    if (!part.draft || !item->final) {
      item->show(part.display, part.bounds, sceneBox(part.box), part.draft, m_paperW, m_paperH);
      m_draftsShown += part.draft;
      partLog.push_back({part.id, part.draft});
      renderPictures();
    }
  }
}

// ---------------------------------------------------------------- pictures
void SheetCanvas::renderLater() { m_render.start(); }

void SheetCanvas::renderPictures() {
  const double k = pixelsPerMm() * devicePixelRatioF();
  const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
  const QRectF around = visible.adjusted(-visible.width() / 4, -visible.height() / 4, visible.width() / 4, visible.height() / 4);
  std::vector<SheetPartItem*> parts{m_paper};
  for (auto& [id, item] : m_views) parts.push_back(item);
  for (SheetPartItem* item : parts) {
    if (!item->display || item->displayRect.isEmpty()) continue;
    // The whole part while it fits a picture at this zoom, else what is seen of it and a margin around.
    const QRectF whole = item->displayRect;
    const QRectF want = std::max(whole.width(), whole.height()) * k <= 4096 ? whole : whole.intersected(around.translated(-item->pos()));
    if (want.isEmpty()) continue;
    double scale = k;
    const double longest = std::max(want.width(), want.height()) * scale;
    if (longest > 4096) scale *= 4096 / longest;
    const auto close = [&](double s) { return s > 0 && std::fabs(s - scale) < 0.2 * scale; };
    if (item->current() && close(item->pictureScale) && item->pictureRect.contains(want)) continue;
    if (item->askedOf == item->display.get() && close(item->askedScale) && item->askedRect.contains(want)) continue;  // on its way
    item->askedOf = item->display.get(), item->askedScale = scale, item->askedRect = want;
    auto display = item->display;
    const double ph = item->paperH;
    auto out = std::make_shared<QImage>();
    QPointer<SheetCanvas> self(this);
    const std::string id = item == m_paper ? std::string() : static_cast<SheetViewItem*>(item)->id;
    ++m_rendering;
    m_jobs->async(
        tr("Drawing the sheet"),
        [display, want, scale, ph, out](Progress) {
          QImage img(std::max(1, static_cast<int>(std::ceil(want.width() * scale))), std::max(1, static_cast<int>(std::ceil(want.height() * scale))),
                     QImage::Format_ARGB32_Premultiplied);
          img.fill(Qt::transparent);
          QPainter p(&img);
          p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
          opad::drawing::paint(p, *display, {want.left(), ph - want.bottom(), want.right(), ph - want.top()}, QRectF(0, 0, img.width(), img.height()), 1.0);
          p.end();
          *out = std::move(img);
        },
        [self, id, display, out, want, scale](bool ok, const QString&) {
          if (!self) return;
          --self->m_rendering;
          SheetPartItem* item = id.empty() ? static_cast<SheetPartItem*>(self->m_paper) : self->viewItem(id);
          if (!ok || !item || item->display != display) return;
          item->setPicture(std::move(*out), want, scale, display.get());
          emit self->partsArrived();
        });
  }
}

// ---------------------------------------------------------------- view
void SheetCanvas::fitSheet() {
  const double m = 0.03 * std::max(m_paperW, m_paperH);
  fitInView(QRectF(-m, -m, m_paperW + 2 * m, m_paperH + 2 * m), Qt::KeepAspectRatio);
  m_fitted = true;
  renderLater();
}

void SheetCanvas::zoomBy(double factor, const QPointF&) {
  const double now = pixelsPerMm(), next = std::clamp(now * factor, 0.2, 400.0);
  if (std::fabs(next - now) < 1e-12) return;
  scale(next / now, next / now);
  renderLater();
}

void SheetCanvas::showEvent(QShowEvent* e) {
  QGraphicsView::showEvent(e);
  QTimer::singleShot(0, this, [this] {
    if (!isVisible()) return;
    if (m_dirty) refresh();
    if (!m_fitted) fitSheet();
  });
}

void SheetCanvas::resizeEvent(QResizeEvent* e) {
  QGraphicsView::resizeEvent(e);
  renderLater();
}

void SheetCanvas::scrollContentsBy(int dx, int dy) {
  QGraphicsView::scrollContentsBy(dx, dy);
  renderLater();
}

void SheetCanvas::drawBackground(QPainter* p, const QRectF& rect) { p->fillRect(rect, theme::current().vp); }

// ---------------------------------------------------------------- selection
std::vector<std::string> SheetCanvas::selectedViews() const {
  std::vector<std::string> out;
  const opad::Sheet* s = m_doc->scene.sheet(m_sheet);
  if (!s) return out;
  for (const auto& id : s->views)  // in the sheet's order
    if (SheetViewItem* item = viewItem(id); item && item->isSelected()) out.push_back(id);
  return out;
}

void SheetCanvas::selectViews(const std::vector<std::string>& ids) {
  m_quietSelection = true;
  for (auto& [id, item] : m_views) item->setSelected(std::find(ids.begin(), ids.end(), id) != ids.end());
  m_quietSelection = false;
}

void SheetCanvas::emitSelection() { emit selectionChanged(selectedViews()); }

std::vector<SheetViewItem*> SheetCanvas::family(SheetViewItem* item) const {
  std::vector<SheetViewItem*> out{item};
  for (size_t i = 0; i < out.size(); ++i)
    for (const auto& [id, v] : m_views)
      if (v->parent == out[i]->id && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
  return out;
}

// ---------------------------------------------------------------- mouse
namespace {
SheetViewItem* viewAt(const std::map<std::string, SheetViewItem*>& views, const QPointF& at) {
  SheetViewItem* best = nullptr;
  double area = 1e300;
  for (const auto& [id, item] : views) {
    const QRectF f = item->frame().adjusted(-1.5, -1.5, 1.5, 1.5);
    if (f.contains(at) && f.width() * f.height() < area) best = item, area = f.width() * f.height();
  }
  return best;
}
}  // namespace

QPointF SheetCanvas::snapCentre(const QPointF& centre, const std::vector<SheetViewItem*>& moving, bool grid) {
  const double tol = 6 / std::max(pixelsPerMm(), 1e-6);
  double bx = tol, by = tol;
  QPointF out = centre;
  bool sx = false, sy = false;
  for (const auto& [id, item] : m_views) {
    if (std::find(moving.begin(), moving.end(), item) != moving.end()) continue;
    const QPointF c = item->frame().center();
    if (std::fabs(c.x() - centre.x()) < bx) bx = std::fabs(c.x() - centre.x()), out.setX(c.x()), sx = true;
    if (std::fabs(c.y() - centre.y()) < by) by = std::fabs(c.y() - centre.y()), out.setY(c.y()), sy = true;
  }
  if (grid) {  // whole millimetres on paper
    if (!sx) out.setX(std::round(out.x()));
    if (!sy) out.setY(m_paperH - std::round(m_paperH - out.y()));
  }
  std::vector<QLineF> lines;
  if (sx) lines.push_back(QLineF(out.x(), -0.05 * m_paperH, out.x(), 1.05 * m_paperH));
  if (sy) lines.push_back(QLineF(-0.05 * m_paperW, out.y(), 1.05 * m_paperW, out.y()));
  m_guides->set(lines, m_place.active ? m_guides->ghost : QRectF(), m_guides->label);
  return out;
}

void SheetCanvas::mousePressEvent(QMouseEvent* e) {
  setFocus();
  const QPointF at = mapToScene(e->pos());
  if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && m_space)) {
    m_panning = true;
    m_panLast = e->pos();
    viewport()->setCursor(Qt::ClosedHandCursor);
    return;
  }
  if (m_place.active) {
    if (e->button() == Qt::LeftButton) placeAt(toPaper(at));
    else if (e->button() == Qt::RightButton) cancelPlacement();
    return;
  }
  if (e->button() != Qt::LeftButton) return QGraphicsView::mousePressEvent(e);
  if (SheetViewItem* item = viewAt(m_views, at)) {
    if (e->modifiers() & Qt::ControlModifier) {
      item->setSelected(!item->isSelected());
      return;
    }
    if (!item->isSelected()) {
      m_scene->clearSelection();
      item->setSelected(true);
    }
    m_drag = Drag();
    m_drag.active = true;
    m_drag.id = item->id;
    m_drag.start = at;
    for (SheetViewItem* v : family(item)) m_drag.origins[v] = v->pos();
    return;
  }
  if (!(e->modifiers() & Qt::ControlModifier)) m_scene->clearSelection();
  setDragMode(QGraphicsView::RubberBandDrag);
  QGraphicsView::mousePressEvent(e);
}

void SheetCanvas::mouseMoveEvent(QMouseEvent* e) {
  const QPointF at = mapToScene(e->pos());
  const Vec2 paper = toPaper(at);
  emit cursorMoved(paper[0], paper[1], paper[0] >= 0 && paper[1] >= 0 && paper[0] <= m_paperW && paper[1] <= m_paperH);
  if (m_panning) {
    const QPoint d = e->pos() - m_panLast;
    m_panLast = e->pos();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
    verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
    return;
  }
  if (m_place.active) return updatePlacement(at);
  if (m_drag.active) {
    SheetViewItem* item = viewItem(m_drag.id);
    if (!item) return;
    QPointF d = at - m_drag.start;
    if (!m_drag.moved && QLineF(QPointF(), d).length() * pixelsPerMm() < QApplication::startDragDistance()) return;
    m_drag.moved = true;
    int sx = 0, sy = 0;
    if (item->kind == "projected") {
      if (item->side == "left" || item->side == "right") sx = 1;
      if (item->side == "top" || item->side == "bottom") sy = 1;
    }
    std::vector<SheetViewItem*> moving;
    for (const auto& [v, o] : m_drag.origins) moving.push_back(v);
    if (sx) d.setY(0);
    else if (sy) d.setX(0);
    else {  // free: the centre snaps to the other views' centres and to whole millimetres
      const QPointF c = item->frame().center() - (item->pos() - m_drag.origins[item]);
      d = snapCentre(c + d, moving, !(e->modifiers() & Qt::ShiftModifier)) - c;
    }
    m_drag.delta = d;
    for (const auto& [v, o] : m_drag.origins) v->setPos(o + d);
    return;
  }
  SheetViewItem* hover = dragMode() == QGraphicsView::RubberBandDrag ? nullptr : viewAt(m_views, at);
  for (auto& [id, item] : m_views)
    if (item->hovered != (item == hover)) {
      item->hovered = item == hover;
      item->update();
    }
  viewport()->setCursor(hover ? Qt::SizeAllCursor : Qt::ArrowCursor);
  QGraphicsView::mouseMoveEvent(e);
}

void SheetCanvas::mouseReleaseEvent(QMouseEvent* e) {
  if (m_panning) {
    m_panning = false;
    viewport()->setCursor(m_space ? Qt::OpenHandCursor : Qt::ArrowCursor);
    return;
  }
  if (m_drag.active) {
    if (m_drag.moved) commitDrag();
    m_drag = Drag();
    m_guides->set({});
    return;
  }
  QGraphicsView::mouseReleaseEvent(e);
  if (dragMode() == QGraphicsView::RubberBandDrag) setDragMode(QGraphicsView::NoDrag);
}

void SheetCanvas::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() == Qt::MiddleButton) return fitSheet();
  mousePressEvent(e);
}

void SheetCanvas::wheelEvent(QWheelEvent* e) {
  const double steps = e->angleDelta().y() / 120.0;
  if (steps != 0) zoomBy(std::pow(1.2, steps), e->position());
}

void SheetCanvas::commitDrag() {
  SheetViewItem* item = viewItem(m_drag.id);
  if (!item || !m_runner) return;
  const QPointF d = m_drag.delta;
  const Vec2 pd{d.x(), -d.y()};  // paper: y up
  const auto r2 = [](double v) { return std::round(v * 100) / 100; };
  opad::json set;
  if (item->kind == "projected" && (item->side == "left" || item->side == "right" || item->side == "top" || item->side == "bottom")) {
    const double along = item->side == "right" ? pd[0] : item->side == "left" ? -pd[0] : item->side == "top" ? pd[1] : -pd[1];
    set = {{"gap", r2(std::max(0.0, item->gap + along))}};
  } else {
    const Vec2 at = toPaper(item->frame().center());
    set = {{"at", {r2(at[0]), r2(at[1])}}};
    if (item->kind == "projected") set["align"] = false;  // a corner view leaves its place beside the parent
  }
  auto origins = m_drag.origins;
  QPointer<SheetCanvas> self(this);
  m_runner("sheet_edit", {{"target", item->id}, {"set", set}}, [self, origins](const opad::json& out) {
    if (!self || !out.is_null()) return;
    for (const auto& [v, o] : origins)  // refused: back where they were
      if (std::any_of(self->m_views.begin(), self->m_views.end(), [v](const auto& kv) { return kv.second == v; })) v->setPos(o);
  });
}

void SheetCanvas::benchDrag(const std::string& id, Vec2 delta) {
  SheetViewItem* item = viewItem(id);
  if (!item) return;
  m_drag = Drag();
  m_drag.active = m_drag.moved = true;
  m_drag.id = id;
  for (SheetViewItem* v : family(item)) m_drag.origins[v] = v->pos();
  m_drag.delta = QPointF(delta[0], -delta[1]);
  for (const auto& [v, o] : m_drag.origins) v->setPos(o + m_drag.delta);
  commitDrag();
  m_drag = Drag();
}

void SheetCanvas::contextMenuEvent(QContextMenuEvent* e) {
  if (m_place.active) return;
  SheetViewItem* item = viewAt(m_views, mapToScene(e->pos()));
  if (item && !item->isSelected()) {
    m_scene->clearSelection();
    item->setSelected(true);
  }
  emit contextMenuRequested(selectedViews(), e->globalPos());
}

// ---------------------------------------------------------------- keys
bool SheetCanvas::event(QEvent* e) {
  if (e->type() == QEvent::ShortcutOverride) {  // these keys are the sheet's while it has the focus
    auto* k = static_cast<QKeyEvent*>(e);
    const bool plain = !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
    if (plain && (k->key() == Qt::Key_F || k->key() == Qt::Key_Home || k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace ||
                  k->key() == Qt::Key_Escape || k->key() == Qt::Key_Space)) {
      e->accept();
      return true;
    }
  }
  return QGraphicsView::event(e);
}

void SheetCanvas::keyPressEvent(QKeyEvent* e) {
  switch (e->key()) {
    case Qt::Key_F:
    case Qt::Key_Home: return fitSheet();
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
      if (!selectedViews().empty()) emit deleteRequested(selectedViews());
      return;
    case Qt::Key_Escape:
      if (m_place.active) return cancelPlacement();
      return m_scene->clearSelection();
    case Qt::Key_Space:
      if (!e->isAutoRepeat()) {
        m_space = true;
        viewport()->setCursor(Qt::OpenHandCursor);
      }
      return;
    default: QGraphicsView::keyPressEvent(e);
  }
}

void SheetCanvas::keyReleaseEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) {
    m_space = false;
    viewport()->setCursor(Qt::ArrowCursor);
    return;
  }
  QGraphicsView::keyReleaseEvent(e);
}

// ---------------------------------------------------------------- placing a new view
void SheetCanvas::placeBase(const std::string& orient, std::function<void(bool)> done) {
  cancelPlacement();
  const opad::Sheet* s = m_doc->scene.sheet(m_sheet);
  if (!s) {
    if (done) done(false);
    return;
  }
  m_place.active = true;
  m_place.orient = orient;
  m_place.done = std::move(done);
  m_place.sizes[""] = {40, 30};
  // Drawn from what the sheet's first base view draws, at the sheet's scale.
  opad::json source;
  for (const auto& id : s->views)
    if (const opad::SheetView* v = m_doc->scene.sheet_view(id); v && v->kind == "base" && v->def.contains("source")) {
      source = v->def["source"];
      break;
    }
  const double scale = s->scale;
  auto size = std::make_shared<std::array<double, 2>>(std::array<double, 2>{40, 30});
  QPointer<SheetCanvas> self(this);
  m_place.job = m_doc->readAsync(
      m_jobs, tr("Measuring the view"),
      [orient, source, scale, size](const opad::Document& doc, const opad::Scene& scene, Progress) {
        opad::SheetView probe;
        probe.kind = "base";
        probe.def = {{"kind", "base"}, {"orient", {{"preset", orient}}}};
        if (!source.is_null()) probe.def["source"] = source;
        const auto e = opad::drawing::view_extent(doc, scene, opad::drawing::view_spec(scene, probe));
        *size = {std::max(5.0, (e[2] - e[0]) * scale), std::max(5.0, (e[3] - e[1]) * scale)};
      },
      [self, size](bool ok, const QString&) {
        if (!self || !self->m_place.active) return;
        if (ok) self->m_place.sizes[""] = *size;
        self->m_place.sized = true;
        self->updatePlacement(self->mapToScene(self->mapFromGlobal(QCursor::pos())));
      });
  m_place.source = source;
  emit promptChanged(tr("Click on the sheet to place the view · Esc cancels"));
  updatePlacement(mapToScene(mapFromGlobal(QCursor::pos())));
}

void SheetCanvas::placeProjected(const std::string& parent, std::function<void(bool)> done) {
  cancelPlacement();
  SheetViewItem* p = viewItem(parent);
  const opad::Sheet* s = m_doc->scene.sheet(m_sheet);
  if (!s || !p || !p->error.isEmpty()) {
    if (done) done(false);
    return;
  }
  m_place.active = m_place.projected = true;
  m_place.parent = parent;
  m_place.done = std::move(done);
  for (const char* side : {"right", "left", "top", "bottom", "top-right", "top-left", "bottom-right", "bottom-left"}) m_place.sizes[side] = {40, 30};
  // The parent's scale: its frame against its extent (sheet scale unless it has its own).
  double scale = s->scale;
  if (const opad::SheetView* v = m_doc->scene.sheet_view(parent); v && v->kind == "base" && v->def.value("scale", "sheet") != "sheet") {
    try {
      scale = opad::drawing::parse_scale(v->def["scale"].get<std::string>());
    } catch (const std::exception&) {
    }
  }
  const std::string sheet = m_sheet;
  auto sizes = std::make_shared<std::map<std::string, std::array<double, 2>>>(m_place.sizes);
  QPointer<SheetCanvas> self(this);
  m_place.job = m_doc->readAsync(
      m_jobs, tr("Measuring the view"),
      [sheet, parent, scale, sizes](const opad::Document& doc, const opad::Scene& scene, Progress) {
        for (auto& [side, size] : *sizes) {
          opad::SheetView probe;
          probe.sheet = sheet;
          probe.parent = parent;
          probe.kind = "projected";
          probe.def = {{"sheet", sheet}, {"kind", "projected"}, {"parent", parent}, {"side", side}};
          const auto e = opad::drawing::view_extent(doc, scene, opad::drawing::view_spec(scene, probe));
          size = {std::max(5.0, (e[2] - e[0]) * scale), std::max(5.0, (e[3] - e[1]) * scale)};
        }
      },
      [self, sizes](bool ok, const QString&) {
        if (!self || !self->m_place.active) return;
        if (ok) self->m_place.sizes = *sizes;
        self->m_place.sized = true;
        self->updatePlacement(self->mapToScene(self->mapFromGlobal(QCursor::pos())));
      });
  emit promptChanged(tr("Move to the side of the view where the new one goes, then click · Esc cancels"));
  updatePlacement(mapToScene(mapFromGlobal(QCursor::pos())));
}

void SheetCanvas::updatePlacement(const QPointF& at) {
  if (!m_place.active) return;
  if (!m_place.projected) {
    const auto size = m_place.sizes[""];
    const QPointF c = snapCentre(at, {}, true);
    m_guides->set(m_guides->lines, QRectF(c - QPointF(size[0] / 2, size[1] / 2), QSizeF(size[0], size[1])), QString());
    m_place.ghost = m_guides->ghost;
    return;
  }
  SheetViewItem* p = viewItem(m_place.parent);
  if (!p) return cancelPlacement();
  const QRectF f = p->frame();
  const QPointF v = at - f.center();
  // Eight sectors around the parent (paper y up): the side the cursor is on.
  static const char* sides[] = {"right", "top-right", "top", "top-left", "left", "bottom-left", "bottom", "bottom-right"};
  const double angle = std::atan2(-v.y(), v.x());
  const int sector = (static_cast<int>(std::lround(angle / (M_PI / 4))) + 8) % 8;
  m_place.side = sides[sector];
  const auto size = m_place.sizes[m_place.side];
  const double w = size[0], h = size[1], least = 4;
  QPointF c = f.center();
  const bool right = m_place.side.find("right") != std::string::npos, left = m_place.side.find("left") != std::string::npos;
  const bool top = m_place.side.find("top") != std::string::npos, bottom = m_place.side.find("bottom") != std::string::npos;
  if (right) c.setX(std::max(at.x(), f.right() + least + w / 2));
  if (left) c.setX(std::min(at.x(), f.left() - least - w / 2));
  if (top) c.setY(std::min(at.y(), f.top() - least - h / 2));
  if (bottom) c.setY(std::max(at.y(), f.bottom() + least + h / 2));
  m_place.ghost = QRectF(c - QPointF(w / 2, h / 2), QSizeF(w, h));
  m_guides->set({}, m_place.ghost, QString());
}

void SheetCanvas::placeAt(Vec2 paper) {
  if (!m_place.active || !m_runner) return;
  updatePlacement(toScene(paper));
  const auto r2 = [](double v) { return std::round(v * 100) / 100; };
  opad::json args = {{"sheet", m_sheet}};
  if (m_place.projected) {
    SheetViewItem* p = viewItem(m_place.parent);
    if (!p) return cancelPlacement();
    const QRectF f = p->frame(), g = m_place.ghost;
    const double dx = std::max(g.left() - f.right(), f.left() - g.right()), dy = std::max(g.top() - f.bottom(), f.top() - g.bottom());
    const bool corner = m_place.side.find('-') != std::string::npos;
    args["parent"] = m_place.parent;
    args["side"] = m_place.side;
    args["gap"] = r2(std::max(2.0, corner ? std::min(dx, dy) : std::max(dx, dy)));
  } else {
    const Vec2 at = toPaper(m_place.ghost.center());
    args["kind"] = "base";
    args["orient"] = m_place.orient;
    args["at"] = {r2(at[0]), r2(at[1])};
    if (m_place.source.is_object()) {
      if (m_place.source.contains("nodes")) args["select"] = m_place.source["nodes"];
      if (m_place.source.contains("hide")) args["hide"] = m_place.source["hide"];
    }
  }
  auto done = std::move(m_place.done);
  m_place.done = nullptr;
  cancelPlacement();
  m_runner("sheet_view", args, [done](const opad::json& out) {
    if (done) done(!out.is_null());
  });
}

void SheetCanvas::cancelPlacement() {
  if (!m_place.active) return;
  if (m_place.job) m_place.job->cancel();
  auto done = std::move(m_place.done);
  m_place = Placement();
  m_guides->set({});
  emit promptChanged(QString());
  if (done) done(false);
}


// ---------------------------------------------------------------- state
std::vector<SheetCanvas::ViewState> SheetCanvas::viewStates() const {
  std::vector<ViewState> out;
  const opad::Sheet* s = m_doc->scene.sheet(m_sheet);
  if (!s) return out;
  for (const auto& id : s->views)
    if (SheetViewItem* item = viewItem(id))
      out.push_back({id, item->frame(), item->displayRect.translated(item->pos()), item->draft, item->final, item->current(),
                     item->display ? static_cast<int>(item->display->prims.size()) : 0, item->error});
  return out;
}

int SheetCanvas::paperPrims() const { return m_paper->display ? static_cast<int>(m_paper->display->prims.size()) : 0; }
bool SheetCanvas::paperPictured() const { return m_paper->current(); }

void SheetCanvas::leaveEvent(QEvent* e) {
  for (auto& [id, item] : m_views)
    if (std::exchange(item->hovered, false)) item->update();
  emit cursorMoved(0, 0, false);
  QGraphicsView::leaveEvent(e);
}
