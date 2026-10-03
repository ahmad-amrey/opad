// Smart selection: the chip, Ctrl+Up / Ctrl+Down, Shift+Space, double-clicks and deleting a feature from its faces
// (SmartSelect.hpp, TODO 11 UI-95).
#include "SmartSelect.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GeomAPI_ProjectPointOnSurf.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <QAction>
#include <QActionGroup>
#include <QBitmap>
#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

#include "AppDocument.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "ShortcutEditor.hpp"
#include "Theme.hpp"
#include "TimelineWidget.hpp"
#include "ToolPanel.hpp"
#include "Units.hpp"
#include "Viewport.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/provenance.hpp"
#include "opad/geometry.hpp"
#include "opad/recognize.hpp"

OPAD_ICON_TABLE(smartselect,
  {"smartLoop", R"(<rect x="4" y="6" width="16" height="12" rx="3"/><circle cx="4" cy="12" r="1.8" fill="currentColor"/>)"},
  {"smartChain", R"(<path d="M3 18c4.5 0 4.5-12 9-12s4.5 12 9 12"/><circle cx="12" cy="6" r="1.8" fill="currentColor"/>)"},
  {"smartUsers", R"(<circle cx="6" cy="5" r="2.2" fill="currentColor"/><circle cx="18" cy="12" r="2.2"/><circle cx="18" cy="19" r="2.2"/><path d="M6 7.2V16a3 3 0 0 0 3 3h6.8M6 12h9.8"/>)"});

namespace {
constexpr size_t kMaxPicks = 2000;  // a rubber band over more is no question of what they belong to
// Bodies with more faces skip bosses, pockets and walls: finding those walks the whole body (2.8 s on the Engine's
// 4140-face casting, for a region that was most of it), where holes, fillets, chamfers, chains and loops stay local.
constexpr int kRegionFaces = 1500;
// What the chip and the menu ask the related command for: history and rules, never "similar" (asked for explicitly).
opad::json kinds(bool regions) {
  opad::json k = {"feature", "import", "body", "hole", "fillet", "chamfer", "tangent", "loop"};
  if (regions)
    for (const char* r : {"boss", "pocket", "wall"}) k.push_back(r);
  return k;
}
bool has(const std::vector<opad::Ref>& refs, const opad::Ref& r) {
  return std::any_of(refs.begin(), refs.end(), [&](const opad::Ref& x) { return x.body == r.body && x.kind == r.kind && x.index == r.index; });
}
using Projector = std::function<QPointF(const opad::Vec3&)>;
// How well a face of an edge answers a double-click on the edge near `middle`: a face seen from behind never does; with the
// pointer off the edge on screen, the face whose inside lies towards it (2 + the cosine), else the one turned most to the
// viewer (seen edge-on, the pointer right on the edge, no pointer).
double pointerSide(const TopoDS_Face& face, const gp_Pnt& middle, const gp_Vec& along, const gp_Vec& towards, const Projector& project, const QPointF& pointer) {
  GeomAPI_ProjectPointOnSurf onFace(middle, BRep_Tool::Surface(face));
  if (onFace.NbPoints() == 0) return -4;
  double u, v;
  onFace.LowerDistanceParameters(u, v);
  gp_Pnt at;
  gp_Vec normal;
  BRepGProp_Face(face).Normal(u, v, at, normal);  // facing out of the material
  if (normal.Magnitude() < 1e-12) return -4;
  normal.Normalize();
  const double facing = normal.Dot(towards);
  if (facing < -1e-3) return facing - 2;
  gp_Vec across = normal.Crossed(along);
  if (!project || across.Magnitude() < 1e-12) return facing;
  across.Normalize();
  // Into the face: the side of the edge where a point just off it lies on the face.
  Bnd_Box box;
  BRepBndLib::Add(face, box);
  const double tol = BRep_Tool::Tolerance(face);
  double step = box.IsVoid() ? 1e-3 : 0.02 * std::sqrt(box.SquareExtent()), inside = 0;
  for (int i = 0; i < 10 && inside == 0; ++i, step /= 2)
    for (double s : {step, -step})
      if (inside == 0 && BRepClass_FaceClassifier(face, middle.Translated(across * s), tol).State() == TopAbs_IN) inside = s;
  if (inside == 0) return facing;
  const QPointF from = project({middle.X(), middle.Y(), middle.Z()});
  const gp_Pnt in = middle.Translated(across * inside);
  const QPointF into = project({in.X(), in.Y(), in.Z()}) - from, off = pointer - from;
  const double li = std::hypot(into.x(), into.y()), lo = std::hypot(off.x(), off.y());
  if (li < 1e-6 || lo < 1) return facing;
  return 2 + QPointF::dotProduct(into, off) / (li * lo);
}
}  // namespace

// ---------------------------------------------------------------- the chip
SmartChip::SmartChip(QWidget* viewport) : QFrame(viewport) {
  setObjectName("smartChip");
  setAttribute(Qt::WA_NativeWindow);  // over the OCCT surface, as the toasts and chips are
  setFocusPolicy(Qt::NoFocus);
  setCursor(Qt::PointingHandCursor);
  setFixedHeight(28);
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(8, 0, 3, 0);
  row->setSpacing(4);
  m_icon = new QLabel(this);
  m_text = new QLabel(this);
  m_text->setObjectName("smartChipText");
  m_hint = new QLabel(this);
  m_hint->setObjectName("smartChipHint");
  m_more = new QToolButton(this);
  m_more->setObjectName("smartChipMore");
  m_more->setAutoRaise(true);
  m_more->setFocusPolicy(Qt::NoFocus);
  m_more->setToolTip(tr("Related selections and what to do with them (Shift+Space)"));
  connect(m_more, &QToolButton::clicked, this, &SmartChip::menuRequested);
  row->addWidget(m_icon);
  row->addWidget(m_text);
  row->addWidget(m_hint);
  row->addWidget(m_more);
  connect(theme::notifier(), &theme::Notifier::changed, this, &SmartChip::restyle);
  restyle();
  hide();
}

void SmartChip::setContent(const QString& icon, const QString& text, const QString& hint, const QList<QAction*>& actions) {
  m_iconName = icon;
  m_text->setText(text);
  m_hint->setText(hint);
  m_hint->setVisible(!hint.isEmpty());
  // The last ones go later, with their actions: this can run inside a button's own click (Isolate moves the selection).
  auto* row = static_cast<QHBoxLayout*>(layout());
  for (QToolButton* b : std::exchange(m_buttons, {})) {
    row->removeWidget(b);
    b->hide();
    if (QAction* a = b->defaultAction(); a && a->parent() == this) a->deleteLater();
    b->deleteLater();
  }
  for (QAction* a : actions) {
    auto* b = new QToolButton(this);
    b->setObjectName("smartChipAction");
    b->setDefaultAction(a);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    b->setIconSize(QSize(16, 16));
    row->insertWidget(row->indexOf(m_more), b);
    b->show();  // made while the chip may be showing: shown by hand, or the layout leaves it out
    m_buttons << b;
  }
  restyle();
  adjustSize();
}

QString SmartChip::text() const { return m_text->text(); }
QString SmartChip::hint() const { return m_hint->isVisibleTo(this) ? m_hint->text() : QString(); }

void SmartChip::restyle() {
  const Tokens& t = theme::current();
  setStyleSheet(QString("QFrame#smartChip { background: %1; border: 1px solid %2; border-radius: 4px; }"
                        "QLabel#smartChipText { background: transparent; color: %3; font-size: 12px; }"
                        "QLabel#smartChipHint { background: transparent; color: %4; font-size: 11px; padding: 0 2px; }"
                        "QToolButton { background: transparent; border: none; border-radius: 3px; padding: 2px; }"
                        "QToolButton:hover { background: %5; }")
                    .arg(theme::css(t.bg3), theme::css(t.candidate), theme::css(t.fg), theme::css(t.fg3), theme::css(t.bg4)));
  m_icon->setPixmap(icons::pixmap(m_iconName.isEmpty() ? QString("dot") : m_iconName, t.candidate, 16, devicePixelRatioF()));
  m_more->setIcon(icons::themed("chevronDown", 16));
}

void SmartChip::enterEvent(QEnterEvent* e) {
  QFrame::enterEvent(e);
  emit hovered(true);
}

void SmartChip::leaveEvent(QEvent* e) {
  QFrame::leaveEvent(e);
  emit hovered(false);
}

void SmartChip::mouseReleaseEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint())) emit clicked();
  e->accept();
}

void SmartChip::resizeEvent(QResizeEvent* e) {
  QFrame::resizeEvent(e);
  QBitmap mask(size());  // a native child cannot be translucent: rounded by a mask
  mask.fill(Qt::color0);
  QPainter p(&mask);
  p.setPen(Qt::NoPen);
  p.setBrush(Qt::color1);
  p.drawRoundedRect(QRectF(rect()), 4, 4);
  p.end();
  setMask(mask);
}

// ---------------------------------------------------------------- the area
void SmartSelect::buildActions() {
  CommandInfo shrink;
  shrink.id = "select.shrink";
  shrink.label = tr("Select less");
  shrink.icon = "chevronDown";
  shrink.key = QKeySequence("Ctrl+Down");
  shrink.keywords = {tr("smart selection"), tr("shrink the selection"), tr("back")};
  m_shrink = services().addCommand(shrink, [this] { this->shrink(); });
  m_shrink->setProperty("shortcutHint", tr("Back down the steps Ctrl+Up climbed: from a feature's faces to the faces picked first, from a component to its body."));
  CommandInfo related;
  related.id = "select.related";
  related.label = tr("Related selections…");
  related.icon = "list";
  related.key = QKeySequence("Shift+Space");
  related.keywords = {tr("smart selection"), tr("feature of a face"), tr("find in timeline")};
  m_related = services().addCommand(related, [this] { showMenu(); });
  m_related->setProperty("shortcutHint", tr("What the picked faces or edges belong to (the feature that made them, a hole, a fillet chain, a loop) and what to do with it: delete, edit, suppress, find it in the timeline."));
  CommandInfo suggest;
  suggest.id = "select.suggest";
  suggest.label = tr("Suggest related selections");
  suggest.keywords = {tr("smart selection"), tr("chip")};
  suggest.checkable = true;
  m_suggest = services().addCommand(suggest, [this] {
    QSettings().setValue("selection/suggest", m_suggest->isChecked());
    if (!m_suggest->isChecked()) return hideChip();
    if (idle() && subPicks(m_current)) request(true);
  });
  m_suggest->setChecked(QSettings().value("selection/suggest", true).toBool());
  m_suggest->setProperty("shortcutHint", tr("After faces or edges are picked, a chip beside them names what they belong to. Off: it comes on Ctrl+Up, Shift+Space or Del only."));
  for (QAction* a : {m_shrink, m_related, m_suggest}) shortcuts::updateTooltip(a);
  if (QAction* parent = services().action("edit.selectparent")) {  // Ctrl+Up: this area grows picked faces (command)
    parent->setProperty("shortcutHint", tr("Grow the selection: picked faces or edges to the feature or detail they belong to, then the body, its component, the parent. Ctrl+Down goes back."));
    shortcuts::updateTooltip(parent);
  }
}

void SmartSelect::menus(QMenuBar*, const QMap<QString, QMenu*>& menus) {
  QMenu* edit = menus.value("edit");
  QAction* parent = services().action("edit.selectparent");
  if (!edit || !parent) return;
  const QList<QAction*> all = edit->actions();
  const int at = int(all.indexOf(parent));
  QAction* before = at >= 0 && at + 1 < all.size() ? all[at + 1] : nullptr;
  edit->insertActions(before, {m_shrink, m_related, m_suggest});
  // How long the picks stand before the chip asks (setting selection/suggestDelay, ms).
  auto* delay = new QMenu(tr("Suggestion delay"), edit);
  delay->setObjectName("smartSuggestDelay");
  auto* group = new QActionGroup(delay);
  const int current = QSettings().value("selection/suggestDelay", 250).toInt();
  for (int ms : {0, 250, 500, 1000}) {
    QAction* a = delay->addAction(ms == 0 ? tr("At once") : tr("After %1 s").arg(QLocale().toString(ms / 1000.0)));
    a->setCheckable(true);
    a->setChecked(ms == current);
    a->setData(ms);
    group->addAction(a);
    connect(a, &QAction::triggered, this, [this, ms] {
      QSettings().setValue("selection/suggestDelay", ms);
      m_wait.setInterval(ms);
    });
  }
  edit->insertMenu(before, delay);
  delay->menuAction()->setEnabled(m_suggest->isChecked());
  connect(m_suggest, &QAction::toggled, delay->menuAction(), &QAction::setEnabled);
}

void SmartSelect::ready() {
  Viewport* v = services().viewport();
  m_chip = new SmartChip(v);
  connect(m_chip, &SmartChip::hovered, this, [this](bool on) { hover(on ? (m_found.active >= 0 ? m_found.active : m_found.best) : -1); });
  connect(m_chip, &SmartChip::clicked, this, [this] {
    if (m_found.active < 0 && m_found.best >= 0) return choose(m_found.best);
    showMenu();
  });
  connect(m_chip, &SmartChip::menuRequested, this, [this] { showMenu(); });
  m_wait.setSingleShot(true);
  m_wait.setInterval(std::clamp(QSettings().value("selection/suggestDelay", 250).toInt(), 0, 5000));
  connect(&m_wait, &QTimer::timeout, this, &SmartSelect::run);
  m_settle.setSingleShot(true);
  m_settle.setInterval(200);
  connect(&m_settle, &QTimer::timeout, this, &SmartSelect::refreshChip);
  m_dropSnapshot.setSingleShot(true);
  m_dropSnapshot.setInterval(30000);  // a copy of a big document is not kept for nothing
  connect(&m_dropSnapshot, &QTimer::timeout, this, [this] { m_snap = {}; });
  m_doubleTimer.setSingleShot(true);
  m_doubleTimer.setInterval(300);  // the second click changed nothing the view reports: act anyway
  connect(&m_doubleTimer, &QTimer::timeout, this, [this] {
    if (std::exchange(m_double, false)) doubleClicked(m_doubleAlt, m_doubleAt);
  });
  // The camera moves: the chip goes, and comes back beside the picks once the view is still.
  connect(v, &Viewport::notesMoved, this, [this] {
    if (!m_chip->isVisible() && !m_settle.isActive()) return;
    m_chip->hide();
    m_settle.start();
  });
  connect(services().design(), &DesignController::stateChanged, this, [this] {
    if (idle()) return;
    m_pending = Pending::None;
    hideChip();
  });
  v->installEventFilter(this);
}

bool SmartSelect::idle() const {
  const AppDocument* doc = services().document();
  const DesignController* design = services().design();
  const Viewport* v = services().viewport();
  return doc && design && v && doc->hasDocument && !doc->loading && !doc->annotationEditing && !design->sketchActive() && !design->ownsSelection() &&
         !v->pickAccumulate();
}

bool SmartSelect::suggesting() const { return !m_suggest || m_suggest->isChecked(); }

bool SmartSelect::subPicks(const std::vector<opad::Ref>& refs) {
  return !refs.empty() && refs.size() <= kMaxPicks &&
         std::all_of(refs.begin(), refs.end(), [](const opad::Ref& r) { return r.kind == opad::Ref::Kind::Face || r.kind == opad::Ref::Kind::Edge; });
}

void SmartSelect::selectionChanged(const SelectionContext& selection) {
  if (m_switching && selection.refs.empty()) return;  // the view's filter changes for a selection of ours: no pick
  m_previous = std::move(m_current);
  m_current = selection.refs;
  const bool ours = m_handedUp || (!m_expect.empty() && smart::sameRefs(m_expect, m_current));
  m_handedUp = false;
  m_expect.clear();
  if (!ours) m_stack.clear();
  hover(-1);
  if (std::exchange(m_double, false)) {  // the second click of a double-click has reached the selection
    m_doubleTimer.stop();
    QTimer::singleShot(0, this, [this, alt = m_doubleAlt, at = m_doubleAt] { doubleClicked(alt, at); });
  }
  if (!idle() || !subPicks(m_current)) {
    m_pending = Pending::None;
    m_wait.stop();
    ++m_token;
    m_found = {};
    return hideChip();
  }
  const AppDocument* doc = services().document();
  if (m_found.ready && smart::sameRefs(m_found.picks, m_current) && m_found.revision == doc->revision && m_found.generation == doc->generation) return refreshChip();
  Found next;
  next.picks = m_current;
  if (ours && !m_chosen.refs.empty() && smart::sameRefs(m_chosen.refs, m_current)) {  // chosen here: its actions at once
    next.candidates = {m_chosen};
    next.active = 0;
    next.corners = m_found.corners;
  }
  m_found = std::move(next);
  refreshChip();
  if (suggesting() || ours) request(ours);  // not suggesting: asked for when a key wants it
}

void SmartSelect::request(bool now) {
  if (!now) return m_wait.start();
  m_wait.stop();
  run();
}

void SmartSelect::run() {
  if (!idle() || !subPicks(m_current)) return;
  const auto picks = m_current;
  const unsigned token = ++m_token;
  const AppDocument* doc = services().document();
  const auto revision = doc->revision, generation = doc->generation;
  withSnapshot([this, token, picks, revision, generation](std::shared_ptr<const opad::Document> document) {
    if (token != m_token) return;
    auto out = std::make_shared<Found>();
    out->picks = picks;
    out->revision = revision;
    out->generation = generation;
    if (m_job) m_job->cancel();
    m_job = services().jobs()->async(tr("Finding related geometry"), [document, out](Progress progress) {
      const opad::design::Cancel cancel = [progress] { return progress.cancelled(); };
      // The picked bodies' faces and edges: how big they are and the picks' box in the world (where the chip goes).
      const opad::Scene scene = opad::resolve(*document);
      std::map<std::string, std::pair<TopTools_IndexedMapOfShape, TopTools_IndexedMapOfShape>> maps;
      Bnd_Box box;
      int largest = 0;
      for (const auto& r : out->picks) {
        if (cancel()) return;
        auto [it, added] = maps.try_emplace(r.body);
        if (added) {
          const TopoDS_Shape shape = opad::node_world_shape(*document, scene, r.body);
          TopExp::MapShapes(shape, TopAbs_FACE, it->second.first);
          TopExp::MapShapes(shape, TopAbs_EDGE, it->second.second);
          out->faces += size_t(it->second.first.Extent());
          largest = std::max(largest, it->second.first.Extent());
        }
        const TopTools_IndexedMapOfShape& map = r.kind == opad::Ref::Kind::Face ? it->second.first : it->second.second;
        if (r.index >= 0 && r.index < map.Extent()) BRepBndLib::Add(map(r.index + 1), box, Standard_True);
      }
      opad::json refs = opad::json::array();
      for (const auto& r : out->picks) refs.push_back(r.str());
      out->candidates = smart::candidates(opad::design::related(*document, {{"refs", refs}, {"kinds", kinds(largest <= kRegionFaces)}, {"limit", 5000}}, cancel));
      if (box.IsVoid()) return;
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      for (int i = 0; i < 8; ++i) out->corners.push_back({i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0});
    }, [this, token, out](bool ok, const QString& error) {
      if (token != m_token) return;  // a newer selection
      m_job = nullptr;
      if (!ok) {
        if (error != "cancelled") trace::log("smart select: " + error);
        m_pending = Pending::None;
        return hideChip();
      }
      const AppDocument* doc = services().document();
      if (doc->revision != out->revision || doc->generation != out->generation) return request(true);
      if (!smart::sameRefs(out->picks, m_current)) return;
      out->best = smart::headline(out->candidates, out->picks, out->faces);
      out->active = smart::matching(out->candidates, out->picks);
      out->ready = true;
      m_found = std::move(*out);
      finished();
    });
  });
}

void SmartSelect::withSnapshot(std::function<void(std::shared_ptr<const opad::Document>)> fn) {
  AppDocument* doc = services().document();
  if (m_snap.doc && m_snap.revision == doc->revision && m_snap.generation == doc->generation) {
    m_dropSnapshot.start();
    return fn(m_snap.doc);
  }
  // Everything that asks while the copy is made gets it (each drops a stale answer itself); a handful at most.
  if (m_afterCapture.size() >= 8) m_afterCapture.erase(m_afterCapture.begin());
  m_afterCapture.push_back(std::move(fn));
  capture();
}

void SmartSelect::capture() {
  if (m_capturing || m_retrying || m_afterCapture.empty()) return;
  AppDocument* doc = services().document();
  const auto revision = doc->revision, generation = doc->generation;
  m_capturing = doc->captureSnapshot(services().jobs(), [this, revision, generation](std::shared_ptr<opad::Document> copy, const QString&) {
    m_capturing = false;
    if (!copy) return m_afterCapture.clear();
    const AppDocument* d = services().document();
    if (d->revision != revision || d->generation != generation) return capture();  // changed meanwhile: what it is now
    m_snap = {std::move(copy), revision, generation};
    m_dropSnapshot.start();
    for (auto& fn : std::exchange(m_afterCapture, {})) fn(m_snap.doc);
  });
  if (!m_capturing) {  // busy (a design change, a save, another copy): once more shortly
    m_retrying = true;
    QTimer::singleShot(250, this, [this] {
      m_retrying = false;
      capture();
    });
  }
}

void SmartSelect::finished() {
  if (trace::enabled()) {
    QString list;
    for (const auto& c : m_found.candidates) list += QString(" %1(%2)").arg(QString::fromStdString(c.kind)).arg(c.count);
    trace::log(QString("smart select: %1 picks, offered %2, active %3, candidates%4").arg(m_found.picks.size()).arg(m_found.best).arg(m_found.active).arg(list));
  }
  refreshChip();
  switch (std::exchange(m_pending, Pending::None)) {
    case Pending::Grow: return grow();
    case Pending::Menu: return showMenu();
    case Pending::Delete: return deletePicks();
    case Pending::Tangent: return tangentFaces();
    case Pending::None: return;
  }
}

void SmartSelect::refreshChip() {
  if (!m_chip) return;
  if (!idle() || !subPicks(m_current) || !smart::sameRefs(m_found.picks, m_current)) return hideChip();
  const int shown = m_found.active >= 0 ? m_found.active : m_found.best;
  if (shown < 0 || shown >= int(m_found.candidates.size()) || (!suggesting() && m_found.active < 0)) return hideChip();
  if (m_settle.isActive()) return;  // the camera moves: back once it is still
  const smart::Candidate& c = m_found.candidates[size_t(shown)];
  QString hint;
  if (m_found.active < 0)
    if (const QAction* grow = services().action("edit.selectparent")) hint = grow->shortcut().toString(QKeySequence::NativeText);
  m_chip->setContent(iconOf(c), label(c), hint, m_found.active >= 0 ? actionsFor(m_found.active, m_chip) : QList<QAction*>{});
  QStringList tip = {label(c)};
  tip << measures(c);
  if (!hint.isEmpty()) tip << tr("%1 selects it").arg(hint);
  m_chip->setToolTip(tip.join("\n"));
  m_chip->show();
  place();
}

void SmartSelect::hideChip() {
  if (m_chip) m_chip->hide();
  hover(-1);
}

// Beside the picks' box on screen, on the side away from the pointer; inside the view, clear of the view cube and of
// the floating panels.
void SmartSelect::place() {
  if (!m_chip || !m_chip->isVisible()) return;
  const Viewport* v = services().viewport();
  m_chip->adjustSize();
  const QSize size(m_chip->sizeHint().width(), m_chip->height());
  QRect box;
  for (const auto& c : m_found.corners) {
    const QRect p(v->widgetPoint(c), QSize(1, 1));
    box = box.isNull() ? p : box.united(p);
  }
  const QPoint cursor = v->mapFromGlobal(QCursor::pos());
  if (box.isNull()) box = QRect(cursor, QSize(1, 1));
  const QRect area = v->rect().adjusted(8, 8, -8, -8);
  QList<QRect> avoid = {QRect(v->width() - 214, 0, 214, 196)};  // the view cube and its buttons
  for (const ToolPanel* p : services().window()->findChildren<ToolPanel*>())
    if (p->isVisible()) avoid << QRect(v->mapFromGlobal(p->frameGeometry().topLeft()), p->frameGeometry().size()).adjusted(-6, -6, 6, 6);
  std::vector<QPoint> spots = {{box.right() + 12, box.top() - size.height() - 6}, {box.left() - size.width() - 12, box.top() - size.height() - 6},
                               {box.right() + 12, box.bottom() + 6}, {box.left() - size.width() - 12, box.bottom() + 6}};
  if (cursor.x() > box.center().x()) {  // the pointer is on the right: the left side first
    std::swap(spots[0], spots[1]);
    std::swap(spots[2], spots[3]);
  }
  QRect chosen;
  for (const QPoint& p : spots) {
    const QRect r(p, size);
    if (area.contains(r) && std::none_of(avoid.begin(), avoid.end(), [&](const QRect& a) { return a.intersects(r); })) {
      chosen = r;
      break;
    }
  }
  if (chosen.isNull()) {  // nowhere clear: the first spot, inside the view
    chosen = QRect(spots[0], size);
    chosen.moveLeft(std::max(area.left(), std::min(chosen.left(), area.right() - size.width())));
    chosen.moveTop(std::max(area.top(), std::min(chosen.top(), area.bottom() - size.height())));
  }
  m_chip->setGeometry(chosen);
  m_chip->raise();
}

void SmartSelect::positionOverlays(const QRect&) { place(); }

void SmartSelect::hover(int index) {
  Viewport* v = services().viewport();
  if (!v) return;
  if (index < 0 || index >= int(m_found.candidates.size())) return v->showCandidateRefs({});
  const smart::Candidate& c = m_found.candidates[size_t(index)];
  v->showCandidateRefs(smart::sameRefs(c.refs, m_current) ? std::vector<opad::Ref>{} : c.refs);  // the selection is shown already
  if (!c.op.empty())
    if (TimelineWidget* t = services().timeline()) t->pulse(c.op);
}

void SmartSelect::choose(int index) {
  if (index < 0 || index >= int(m_found.candidates.size())) return;
  const smart::Candidate c = m_found.candidates[size_t(index)];
  m_stack.push_back(m_current);
  if (c.kind != "body") m_chosen = c;
  hover(-1);
  trace::log(QString("smart select: chose %1 (%2 refs)").arg(QString::fromStdString(c.kind + " " + c.name)).arg(c.refs.size()));
  select(c.refs);
}

// Selects as a pick would, switching the view to faces or edges first when they are not what it picks now.
void SmartSelect::select(std::vector<opad::Ref> refs, std::function<void()> then) {
  if (refs.empty()) return;
  Viewport* v = services().viewport();
  const opad::Ref::Kind kind = refs.front().kind;
  const auto filter = kind == opad::Ref::Kind::Face ? Viewport::SelFilter::Face : kind == opad::Ref::Kind::Edge ? Viewport::SelFilter::Edge
                      : kind == opad::Ref::Kind::Vertex ? Viewport::SelFilter::Vertex : v->selectionFilter();
  m_expect = refs;
  if (kind == opad::Ref::Kind::Body || v->selectionFilter() == filter) {
    services().select(refs);
    if (then) then();
    return;
  }
  const unsigned token = ++m_switchToken;
  m_switching = true;
  connect(v, &Viewport::filterApplied, this, [this, refs, then, token] {
    if (token != m_switchToken) return;
    m_switching = false;
    m_expect = refs;
    services().select(refs);
    if (then) then();
  }, Qt::SingleShotConnection);
  v->setSelectionFilter(filter);
}

void SmartSelect::grow() {
  if (!idle() || !subPicks(m_current)) return;
  if (!m_found.ready || !smart::sameRefs(m_found.picks, m_current)) {
    m_pending = Pending::Grow;
    if (m_wait.isActive() || (!m_job && !m_capturing)) request(true);
    return;
  }
  if (m_found.best >= 0) return choose(m_found.best);
  // Nothing between the picks and their bodies: the bodies.
  std::vector<opad::Ref> bodies;
  for (const auto& r : m_current)
    if (std::none_of(bodies.begin(), bodies.end(), [&](const opad::Ref& b) { return b.body == r.body; })) {
      opad::Ref b;
      b.body = r.body;
      bodies.push_back(b);
    }
  m_stack.push_back(m_current);
  select(bodies);
}

void SmartSelect::shrink() {
  if (!idle()) return;
  if (m_stack.empty()) return services().showMessage(tr("Nothing to go back to: Ctrl+Up grows the selection first."), 4000);
  auto refs = std::move(m_stack.back());
  m_stack.pop_back();
  hover(-1);
  select(std::move(refs));
}

bool SmartSelect::command(const QString& id, const SelectionContext& selection) {
  if (!idle()) return false;
  if (id == "edit.selectparent") {
    if (subPicks(selection.refs)) {
      grow();
      return true;
    }
    // A body or a component: the window climbs to the parent; Ctrl+Down brings this back.
    const opad::Scene& scene = services().document()->scene;
    const bool climbs = !selection.refs.empty() && std::all_of(selection.refs.begin(), selection.refs.end(), [&](const opad::Ref& r) {
      const opad::Node* n = scene.node(r.body);
      return r.kind == opad::Ref::Kind::Body && n && !n->parent.empty();
    });
    if (climbs) {
      m_stack.push_back(m_current);
      m_handedUp = true;
    }
    return false;
  }
  if (id == "edit.delete" && subPicks(selection.refs)) {
    deletePicks();
    return true;
  }
  return false;
}

// Del on picked faces or edges (UI-04): never the body's source. A feature's whole face set deletes the feature (its
// dependents asked for first), a recognised detail's faces start Remove faces; anything else opens the menu of what the
// picks belong to, where deleting that is one entry.
void SmartSelect::deletePicks() {
  if (!m_found.ready || !smart::sameRefs(m_found.picks, m_current)) {
    m_pending = Pending::Delete;
    if (m_wait.isActive() || (!m_job && !m_capturing)) request(true);
    return;
  }
  const int active = m_found.active;
  const smart::Candidate* c = active >= 0 ? &m_found.candidates[size_t(active)] : nullptr;
  if (c && c->feature()) return deleteFeature(*c);
  if (c && c->group() && !c->refs.empty() && c->refs.front().kind == opad::Ref::Kind::Face)
    if (QAction* remove = services().action("design.remove_faces")) return remove->trigger();
  showMenu();
  services().showMessage(tr("Faces and edges go with what made them: delete it from this menu, or remove the faces."), 8000);
}

void SmartSelect::showMenu(const QPoint& global) {
  if (!idle()) return;
  if (!subPicks(m_current)) return services().showMessage(tr("Pick faces or edges first: the menu lists what they belong to."), 5000);
  if (!m_found.ready || !smart::sameRefs(m_found.picks, m_current)) {
    m_pending = Pending::Menu;
    if (m_wait.isActive() || (!m_job && !m_capturing)) request(true);
    return;
  }
  if (m_menu) m_menu->close();
  auto* menu = new QMenu(services().window());
  menu->setObjectName("smartMenu");
  menu->setAttribute(Qt::WA_DeleteOnClose);
  m_menu = menu;
  int n = 0;
  for (size_t i = 0; i < m_found.candidates.size(); ++i) {
    const smart::Candidate& c = m_found.candidates[i];
    if (c.kind == "similar") continue;
    QString text = label(c);
    if (n < 9) text = QString("&%1  %2").arg(n + 1).arg(text);  // 1-9 pick while the menu is open
    ++n;
    QAction* a = menu->addAction(icons::themed(iconOf(c), 16), text);
    a->setProperty("smartCandidate", int(i));
    a->setCheckable(true);
    a->setChecked(int(i) == m_found.active);  // what is selected now
    connect(a, &QAction::triggered, this, [this, i] { choose(int(i)); });
  }
  connect(menu, &QMenu::hovered, this, [this](QAction* a) { hover(a->property("smartCandidate").isValid() ? a->property("smartCandidate").toInt() : -1); });
  connect(menu, &QMenu::aboutToHide, this, [this] { hover(-1); });
  if (const int f = focus(); f >= 0) {
    menu->addSeparator();
    menu->addActions(actionsFor(f, menu));
    if (const smart::Candidate c = m_found.candidates[size_t(f)]; c.feature()) {
      QAction* users = menu->addAction(icons::themed("smartUsers", 16), tr("Select what depends on %1").arg(QString::fromStdString(c.name)));
      users->setObjectName("smartDependents");
      users->setToolTip(tr("The faces of the later features that use it and would fail without it"));
      connect(users, &QAction::triggered, this, [this, c] { services().guarded([&] { selectUsers(c); }); });
    }
  }
  menu->addSeparator();
  menu->addAction(services().action("select.similar"));
  const bool faces = std::all_of(m_current.begin(), m_current.end(), [](const opad::Ref& r) { return r.kind == opad::Ref::Kind::Face; });
  if (QAction* remove = services().action("design.remove_faces"); faces && remove) {
    QAction* a = menu->addAction(icons::themed("removeFaces", 16), tr("Remove the picked faces"));
    connect(a, &QAction::triggered, remove, &QAction::trigger);
  }
  const QPoint at = !global.isNull() ? global : m_chip->isVisible() ? m_chip->mapToGlobal(QPoint(0, m_chip->height() + 2)) : QCursor::pos();
  menu->popup(at);
}

int SmartSelect::focus() const { return m_found.active >= 0 ? m_found.active : m_found.best; }

// What can be done with a candidate: on a feature its history (delete, edit, suppress, find), on a detail without one
// taking its faces away; isolating its bodies on any.
QList<QAction*> SmartSelect::actionsFor(int index, QObject* parent) {
  QList<QAction*> out;
  if (index < 0 || index >= int(m_found.candidates.size())) return out;
  const smart::Candidate c = m_found.candidates[size_t(index)];
  const QString name = c.feature() || c.kind == "import" ? QString::fromStdString(c.name) : label(c);
  auto add = [&](const QString& icon, const QString& text, const char* object, std::function<void()> fn, const QString& key = QString()) {
    auto* a = new QAction(icons::themed(icon, 16), key.isEmpty() ? text : text + "\t" + key, parent);
    a->setObjectName(object);
    a->setToolTip(key.isEmpty() ? text : QString("%1 (%2)").arg(text, key));
    connect(a, &QAction::triggered, this, [this, fn] { services().guarded(fn); });
    out << a;
  };
  if (c.feature()) {
    // Del deletes it when it is what is selected (UI-04); on other picks Del opens the menu.
    const QAction* del = services().action("edit.delete");
    add("delete", tr("Delete %1").arg(name), "smartDelete", [this, c] { deleteFeature(c); },
        index == m_found.active && del ? del->shortcut().toString(QKeySequence::NativeText) : QString());
    add("rename", tr("Edit %1").arg(name), "smartEdit", [this, c] {
      if (services().requireEditable()) services().design()->editOp(c.op);
    }, index == m_found.active ? QStringLiteral("Enter") : QString());
    const opad::Feature* f = services().document()->scene.feature(c.op);
    if (f && !f->suppressed) add("hide", tr("Suppress %1").arg(name), "smartSuppress", [this, c, name] {
      if (!services().requireEditable()) return;
      services().design()->applyOps({opad::design::make_edit_op(c.op, {{"suppressed", true}})}, tr("suppress %1").arg(name), [this, name](bool ok, const QString& error) {
        if (!ok) return emit services().design()->failed(error);
        services().undoToast(tr("Suppressed %1").arg(name));
      });
    });
  }
  if (!c.op.empty()) add("locate", tr("Find %1 in the timeline").arg(name), "smartFind", [this, c] {
    if (TimelineWidget* t = services().timeline()) {
      t->setCurrentOp(c.op);
      t->pulse(c.op);
    }
  });
  const bool faces = !c.refs.empty() && c.refs.front().kind == opad::Ref::Kind::Face;
  if (c.group() && faces) {
    add("removeFaces", tr("Remove %1: the faces around it close the gap").arg(name), "smartRemove", [this, c] {
      select(c.refs, [this] {
        if (QAction* remove = services().action("design.remove_faces")) remove->trigger();
      });
    });
    add("similar", tr("Select similar"), "smartSimilar", [this, c] {
      select(c.refs, [this] {
        if (QAction* similar = services().action("select.similar")) similar->trigger();
      });
    });
  }
  if (c.group() && !measures(c).isEmpty()) add("distance", tr("Measure %1").arg(name), "smartMeasure", [this, c] { measure(c); });
  const auto bodies = c.bodies();
  add("isolate", bodies.size() == 1 ? tr("Isolate %1").arg(services().document()->nodeName(bodies.front())) : tr("Isolate %1 bodies").arg(bodies.size()),
      "smartIsolate", [this, bodies] { services().viewport()->isolate(bodies); });
  return out;
}

void SmartSelect::contextMenu(const SelectionContext& selection, QMenu& menu) {
  if (selection.sketching || !idle() || !subPicks(selection.refs) || !m_found.ready || !smart::sameRefs(m_found.picks, selection.refs)) return;
  menu.addSeparator();
  if (m_found.best >= 0) {
    QString text = tr("Select %1").arg(label(m_found.candidates[size_t(m_found.best)]));
    if (const QAction* grow = services().action("edit.selectparent"); grow && !grow->shortcut().isEmpty()) text += "\t" + grow->shortcut().toString(QKeySequence::NativeText);
    QAction* a = menu.addAction(icons::themed(iconOf(m_found.candidates[size_t(m_found.best)]), 16), text);
    connect(a, &QAction::triggered, this, [this, best = m_found.best] { choose(best); });
  }
  if (const int f = focus(); f >= 0 && m_found.candidates[size_t(f)].feature())
    for (QAction* a : actionsFor(f, &menu))
      if (a->objectName() != "smartIsolate") menu.addAction(a);
  menu.addAction(m_related);
}

void SmartSelect::documentChanged(bool replaced) {
  ++m_token;
  m_pending = Pending::None;
  m_found = {};
  m_stack.clear();
  m_expect.clear();
  m_chosen = {};
  m_switching = false;
  if (replaced) {
    m_snap = {};
    m_afterCapture.clear();
    m_current.clear();
    m_previous.clear();
  }
  hideChip();
  if (idle() && subPicks(m_current)) request(false);  // the picks may still stand
}

void SmartSelect::doubleClicked(bool alt, const QPoint& at) {
  if (!idle()) return;
  const auto picks = services().viewport()->selection();
  if (picks.size() != 1) return;
  const opad::Ref r = picks.front();
  if (r.kind == opad::Ref::Kind::Edge) return chain(r, alt, at);
  if (r.kind != opad::Ref::Kind::Face) return;
  // Again on a face of the feature chosen before: edit that feature. Before the double-click: what was selected before its
  // first click, whether that click had reached the selection when the second came or not.
  const auto before = smart::sameRefs(m_doubleFirst, picks) ? std::exchange(m_doubleBefore, {}) : std::exchange(m_doubleFirst, {});
  m_doubleBefore.clear();
  m_doubleFirst.clear();
  if (m_chosen.feature() && smart::sameRefs(before, m_chosen.refs) && has(m_chosen.refs, r) && !services().document()->browse) {
    trace::log("smart select: double-click edits " + QString::fromStdString(m_chosen.name));
    const std::string op = m_chosen.op;  // a copy: the roll-back that editing starts with clears m_chosen (documentChanged)
    services().design()->editOp(op);
    return;
  }
  if (!smart::sameRefs(m_current, picks)) m_current = picks;
  alt ? tangentFaces() : grow();
}

void SmartSelect::tangentFaces() {
  if (!idle() || !subPicks(m_current)) return;
  if (!m_found.ready || !smart::sameRefs(m_found.picks, m_current)) {
    m_pending = Pending::Tangent;
    if (m_wait.isActive() || (!m_job && !m_capturing)) request(true);
    return;
  }
  for (size_t i = 0; i < m_found.candidates.size(); ++i)
    if (m_found.candidates[i].kind == "tangent") return choose(int(i));
  services().showMessage(tr("No face continues this one tangentially."), 4000);
}

// An edge's loop on the face on the pointer's side of it, or its tangent chain: found on a worker (pointerSide).
void SmartSelect::chain(const opad::Ref& edge, bool tangent, const QPoint& at) {
  const Viewport* v = services().viewport();
  const opad::Vec3 view = v->viewDirection();
  const auto project = at.x() >= 0 ? v->projector() : Projector();
  const QPointF pointer(at);
  const unsigned token = ++m_chainToken;
  withSnapshot([this, edge, tangent, view, project, pointer, token](std::shared_ptr<const opad::Document> document) {
    if (token != m_chainToken) return;
    auto result = std::make_shared<std::vector<int>>();
    if (m_chainJob) m_chainJob->cancel();
    m_chainJob = services().jobs()->async(tangent ? tr("Finding the tangent chain") : tr("Finding the loop"), [document, edge, tangent, view, project, pointer, result](Progress progress) {
      const opad::Scene scene = opad::resolve(*document);
      const TopoDS_Shape shape = opad::node_world_shape(*document, scene, edge.body);
      opad::Recognizer recognizer(shape, [progress] { return progress.cancelled(); });
      if (edge.index < 0 || edge.index >= recognizer.edge_count()) return;
      if (tangent) {
        *result = recognizer.tangent_edges({edge.index}).edges;
        return;
      }
      TopTools_IndexedMapOfShape faces, edges;
      TopExp::MapShapes(shape, TopAbs_FACE, faces);
      TopExp::MapShapes(shape, TopAbs_EDGE, edges);
      BRepAdaptor_Curve curve(TopoDS::Edge(edges(edge.index + 1)));
      const double t0 = curve.FirstParameter(), t1 = curve.LastParameter();
      double t = (t0 + t1) / 2;
      if (project) {  // the edge's point nearest the pointer on screen
        double nearest = 1e300;
        for (int i = 0; i <= 32; ++i) {
          const double s = t0 + (t1 - t0) * i / 32;
          const gp_Pnt p = curve.Value(s);
          const QPointF d = project({p.X(), p.Y(), p.Z()}) - pointer;
          if (const double l = QPointF::dotProduct(d, d); l < nearest) nearest = l, t = s;
        }
      }
      gp_Pnt middle;
      gp_Vec along;
      curve.D1(t, middle, along);
      const gp_Vec towards(-view[0], -view[1], -view[2]);
      double best = -1e300;
      for (const auto& loop : recognizer.loops({edge.index})) {
        const int f = loop.params.value("face", -1);
        if (f < 0 || f >= faces.Extent()) continue;
        if (const double score = pointerSide(TopoDS::Face(faces(f + 1)), middle, along, towards, project, pointer); score > best) {
          best = score;
          *result = loop.edges;
        }
      }
    }, [this, token, edge, tangent, result](bool ok, const QString& error) {
      if (token != m_chainToken) return;
      m_chainJob = nullptr;
      if (!ok) return services().showMessage(i18n::t(error), 6000);
      const auto picks = services().viewport()->selection();
      if (picks.size() != 1 || picks.front().body != edge.body || picks.front().index != edge.index || picks.front().kind != edge.kind) return;  // picked on
      if (result->size() <= 1)
        return services().showMessage(tangent ? tr("No edge continues this one tangentially.") : tr("This edge closes a loop on its own."), 4000);
      std::vector<opad::Ref> refs;
      for (int i : *result) {
        opad::Ref r;
        r.body = edge.body;
        r.kind = opad::Ref::Kind::Edge;
        r.index = i;
        refs.push_back(r);
      }
      trace::log(QString("smart select: double-click selected the %1 (%2 edges)").arg(tangent ? "tangent chain" : "loop").arg(refs.size()));
      m_stack.push_back(m_current);
      select(refs);
    });
  });
}

bool SmartSelect::eventFilter(QObject* watched, QEvent* event) {
  if (watched == services().viewport()) {
    if (event->type() == QEvent::MouseButtonDblClick) {
      const auto* e = static_cast<QMouseEvent*>(event);
      if (e->button() == Qt::LeftButton && idle() && !services().viewport()->sketching()) {
        m_doubleArmed = true;
        m_doubleAlt = e->modifiers() & Qt::AltModifier;
        m_doubleAt = e->position().toPoint();
        // The first click has reached the selection by now (or will with this one): what was selected before it.
        m_doubleBefore = m_previous;
        m_doubleFirst = m_current;
      }
    } else if (event->type() == QEvent::MouseButtonRelease && m_doubleArmed) {
      m_doubleArmed = false;
      m_double = true;  // acted on once this click has reached the selection (selectionChanged)
      m_doubleTimer.start();
    } else if (event->type() == QEvent::KeyPress) {  // Enter on a feature's faces: edit it (the chip's Edit)
      const auto* k = static_cast<QKeyEvent*>(event);
      const int a = m_found.active;
      if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && k->modifiers() == Qt::NoModifier && idle() && m_found.ready && a >= 0 && smart::sameRefs(m_found.picks, m_current) && m_found.candidates[size_t(a)].feature()) {
        const std::string op = m_found.candidates[size_t(a)].op;  // a copy: editing starts with a roll-back (documentChanged)
        if (services().requireEditable()) services().design()->editOp(op);
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------- deleting a feature from its faces
void SmartSelect::deleteCandidate(int index) {
  if (index >= 0 && index < int(m_found.candidates.size())) deleteFeature(m_found.candidates[size_t(index)]);
}

// Planned on a worker first: when later features would fail without it, they are named and the result is previewed
// before anything is committed; the choice is to delete them too or to delete it alone.
void SmartSelect::deleteFeature(const smart::Candidate& c) {
  if (!c.feature() || !services().requireEditable([this, c] { deleteFeature(c); })) return;
  AppDocument* doc = services().document();
  if (doc->designBusy && !m_capturing) return services().showMessage(tr("The design is still being recomputed; try again in a moment."), 5000);
  const auto revision = doc->revision, generation = doc->generation;
  const unsigned token = ++m_deleteToken;
  withSnapshot([this, c, token, revision, generation](std::shared_ptr<const opad::Document> document) {
    if (token != m_deleteToken) return;
    auto plan = std::make_shared<opad::design::Plan>();
    auto parts = std::make_shared<std::vector<Viewport::PreviewPart>>();
    auto hidden = std::make_shared<std::vector<std::string>>();
    services().jobs()->async(tr("Checking what deleting %1 changes").arg(QString::fromStdString(c.name)), [document, op = c.op, plan, parts, hidden](Progress p) {
      const opad::design::Cancel cancel = [p] { return p.cancelled(); };
      *plan = opad::design::plan_ops(*document, {{{"op", "delete"}, {"target", op}}}, false, cancel);
      // The result: the plan committed to a copy (its own shape cache: nothing here touches what the view draws), and
      // every body that changes there meshed for the preview; the ones that go are hidden.
      opad::Document after = *document;
      after.shape_cache = opad::make_shape_cache();
      opad::design::commit(after, opad::design::Plan(*plan));
      const opad::Scene before = opad::resolve(*document), now = opad::resolve(after);
      for (const auto& id : before.all_bodies()) {
        if (cancel()) return;
        const opad::Node* n = now.node(id);
        if (!n || n->body_missing) {
          hidden->push_back(id);
          continue;
        }
        if (n->body_key == before.node(id)->body_key && now.world(id).m == before.world(id).m) continue;
        const TopoDS_Shape shape = BRepBuilderAPI_Copy(opad::node_world_shape(after, now, id)).Shape();
        Bnd_Box box;
        BRepBndLib::Add(shape, box, Standard_False);
        BodyPrs::meshForDisplay(shape, box.IsVoid() ? 0.1 : std::clamp(std::sqrt(box.SquareExtent()) * 0.002, 0.02, 2.0));
        parts->push_back({id, shape, BodyPrs::build(shape, box, true)});
      }
    }, [this, c, token, plan, parts, hidden, revision, generation](bool ok, const QString& error) {
      if (token != m_deleteToken) return;
      if (!ok) {
        if (error != "cancelled") services().showMessage(i18n::t(error), 8000);
        return;
      }
      const AppDocument* d = services().document();
      if (d->revision != revision || d->generation != generation) return services().showMessage(tr("The document changed meanwhile; delete again."), 5000);
      const auto deps = smart::dependents(plan->report, d->scene, {c.op});
      if (trace::enabled()) trace::log(QString("smart select: deleting %1 breaks %2 feature(s)").arg(QString::fromStdString(c.name)).arg(deps.size()));
      if (deps.empty()) return commitDelete(c, {c.op});
      askDependents(c, deps, *parts, *hidden);
    });
  });
}

void SmartSelect::selectUsers(const smart::Candidate& c) {
  if (!c.feature()) return;
  const AppDocument* doc = services().document();
  const auto revision = doc->revision, generation = doc->generation;
  const unsigned token = ++m_usersToken;
  const QString name = QString::fromStdString(c.name);
  withSnapshot([this, c, name, token, revision, generation](std::shared_ptr<const opad::Document> document) {
    if (token != m_usersToken) return;
    auto users = std::make_shared<smart::Users>();
    services().jobs()->async(tr("Finding what depends on %1").arg(name), [document, op = c.op, users](Progress p) {
      *users = smart::usersOf(*document, op, [p] { return p.cancelled(); });
    }, [this, name, token, users, revision, generation](bool ok, const QString& error) {
      if (token != m_usersToken) return;
      if (!ok) {
        if (error != "cancelled") services().showMessage(i18n::t(error), 8000);
        return;
      }
      const AppDocument* d = services().document();
      if (d->revision != revision || d->generation != generation) return services().showMessage(tr("The document changed meanwhile; ask again."), 5000);
      trace::log(QString("smart select: %1 feature(s) depend on %2, %3 face(s)").arg(users->ops.size()).arg(name).arg(users->faces.size()));
      if (users->ops.empty()) return services().showMessage(tr("Nothing depends on %1: no later feature uses it.").arg(name), 6000);
      QStringList names;
      for (const auto& [op, n] : users->ops) names << QString::fromStdString(n);
      if (TimelineWidget* t = services().timeline()) t->pulse(users->ops.front().first);
      const QString said = names.size() == 1 ? tr("%1 depends on %2").arg(names.front(), name) : tr("%1 depend on %2").arg(names.join(", "), name);
      if (users->faces.empty()) return services().showMessage(tr("%1, with no faces of its own to select").arg(said), 8000);
      services().showMessage(said, 8000);
      m_stack.push_back(m_current);
      select(users->faces);
    });
  });
}

void SmartSelect::askDependents(const smart::Candidate& c, const std::vector<std::pair<std::string, std::string>>& deps, const std::vector<Viewport::PreviewPart>& parts,
                                const std::vector<std::string>& hidden) {
  Viewport* v = services().viewport();
  v->setPreviewBodies(parts, hidden);  // the result without it
  const QString name = QString::fromStdString(c.name);
  QStringList names;
  std::vector<std::string> ops = {c.op};
  for (const auto& [op, n] : deps) {
    names << QString::fromStdString(n);
    ops.push_back(op);
  }
  if (m_menu) m_menu->close();
  auto* menu = new QMenu(services().window());
  menu->setObjectName("smartDeleteQuestion");
  menu->setAttribute(Qt::WA_DeleteOnClose);
  m_menu = menu;
  QAction* why = menu->addAction(icons::themed("warning", 16), names.size() == 1 ? tr("%1 uses %2: without it, it fails").arg(names.front(), name)
                                                                                 : tr("%1 use %2: without it, they fail").arg(names.join(", "), name));
  why->setEnabled(false);
  menu->addSeparator();
  auto done = std::make_shared<bool>(false);
  QAction* all = menu->addAction(icons::themed("delete", 16), tr("Delete %1 and %2").arg(name, names.join(", ")));
  all->setObjectName("deleteWithDependents");
  connect(all, &QAction::triggered, this, [this, c, ops, done] {
    *done = true;
    commitDelete(c, ops);
  });
  QAction* only = menu->addAction(tr("Delete %1 only (%2 will fail)").arg(name, names.join(", ")));
  only->setObjectName("deleteOnly");
  connect(only, &QAction::triggered, this, [this, c, done] {
    *done = true;
    commitDelete(c, {c.op});
  });
  QAction* faces = menu->addAction(icons::themed("removeFaces", 16), tr("Remove its faces instead (Remove faces, at the end of the timeline)"));
  faces->setObjectName("deleteFacesInstead");
  connect(faces, &QAction::triggered, this, [this, c, done] {
    *done = true;
    services().viewport()->clearPreviewBodies();
    select(c.refs, [this] {
      if (QAction* remove = services().action("design.remove_faces")) remove->trigger();
    });
  });
  menu->addSeparator();
  menu->addAction(tr("Cancel"))->setObjectName("deleteCancel");
  // Hidden without a choice (Esc, Cancel, a click elsewhere): the preview goes. A choice arrives after the hide.
  connect(menu, &QMenu::aboutToHide, this, [this, done] {
    QTimer::singleShot(0, this, [this, done] {
      if (!*done) services().viewport()->clearPreviewBodies();
    });
  });
  menu->popup(m_chip && m_chip->isVisible() ? m_chip->mapToGlobal(QPoint(0, m_chip->height() + 2)) : QCursor::pos());
}

void SmartSelect::commitDelete(const smart::Candidate& c, std::vector<std::string> ops) {
  services().viewport()->clearPreviewBodies();
  std::vector<opad::json> list;
  for (const auto& op : ops) list.push_back({{"op", "delete"}, {"target", op}});
  const QString name = QString::fromStdString(c.name);
  const size_t n = ops.size();
  services().design()->applyOps(list, tr("delete %1").arg(name), [this, name, n](bool ok, const QString& error) {
    if (!ok) return emit services().design()->failed(error);
    trace::log(QString("smart select: deleted %1 (%2 op(s))").arg(name).arg(n));
    services().undoToast(n == 1 ? tr("Deleted %1").arg(name) : tr("Deleted %1 and %2 feature(s) using it").arg(name).arg(n - 1));
  });
}

// ---------------------------------------------------------------- words and icons
QString SmartSelect::label(const smart::Candidate& c) const {
  const opad::json& p = c.params;
  auto length = [&](const char* key) { return p.contains(key) && p[key].is_number() ? units::compact(units::Kind::Length, p[key].get<double>()) : QString(); };
  QString name;
  if (c.kind == "hole") {
    const std::string type = p.value("type", "simple");
    name = type == "counterbore" ? tr("Counterbored hole Ø%1").arg(length("diameter"))
           : type == "countersink" ? tr("Countersunk hole Ø%1").arg(length("diameter"))
           : p.value("through", false) ? tr("Hole Ø%1 through").arg(length("diameter"))
                                       : tr("Hole Ø%1 × %2").arg(length("diameter"), length("depth"));
  } else if (c.kind == "fillet") {
    name = (c.count == 1 ? tr("Fillet R%1") : tr("Fillet chain R%1")).arg(length("radius"));
  } else if (c.kind == "chamfer") {
    name = (c.count == 1 ? tr("Chamfer %1") : tr("Chamfer chain %1")).arg(length("distance"));
  } else if (c.kind == "boss") {
    name = tr("Boss");
  } else if (c.kind == "pocket") {
    name = tr("Pocket");
  } else if (c.kind == "wall") {
    name = tr("Wall %1").arg(length("thickness"));
  } else if (c.kind == "tangent") {
    name = tr("Tangent chain");
  } else if (c.kind == "loop") {
    name = p.value("outer", true) ? tr("Outer loop") : tr("Inner loop");
  } else if (c.kind == "body") {
    return c.refs.empty() ? QString() : services().document()->nodeName(c.refs.front().body);
  } else {
    name = QString::fromStdString(c.name);  // a feature's or an import's own name
  }
  const bool edges = !c.refs.empty() && c.refs.front().kind == opad::Ref::Kind::Edge;
  if (c.count == 1) return (edges ? tr("%1 · 1 edge") : tr("%1 · 1 face")).arg(name);
  return (edges ? tr("%1 · %2 edges") : tr("%1 · %2 faces")).arg(name).arg(c.count);
}

QStringList SmartSelect::measures(const smart::Candidate& c) const {
  const opad::json& p = c.params;
  auto number = [&](const char* key) { return p.contains(key) && p[key].is_number() ? std::optional<double>(p[key].get<double>()) : std::nullopt; };
  auto length = [](double v) { return units::compact(units::Kind::Length, v); };
  auto angle = [](double v) { return units::compact(units::Kind::Angle, v); };
  QStringList out;
  if (c.kind == "hole") {
    if (const auto d = number("diameter")) out << tr("Ø %1").arg(length(*d));
    if (const auto depth = number("depth")) out << (p.value("through", false) ? tr("Depth %1 (through)") : tr("Depth %1")).arg(length(*depth));
    if (const auto d = number("cb_diameter")) out << tr("Counterbore Ø %1, %2 deep").arg(length(*d), length(number("cb_depth").value_or(0)));
    if (const auto d = number("cs_diameter")) out << tr("Countersink Ø %1, %2").arg(length(*d), angle(number("cs_angle").value_or(0)));
    if (const auto a = number("tip_angle")) out << tr("Drill point %1").arg(angle(*a));
  } else if (c.kind == "fillet") {
    if (const auto r = number("radius")) out << tr("Radius %1").arg(length(*r));
    if (p.contains("convex") && p["convex"].is_boolean()) out << (p["convex"].get<bool>() ? tr("Rounds an outside edge") : tr("Fills an inside corner"));
  } else if (c.kind == "chamfer") {
    if (const auto d = number("distance")) out << tr("Distance %1").arg(length(*d));
    if (const auto w = number("width")) out << tr("Width %1").arg(length(*w));
  } else if (c.kind == "wall") {
    if (const auto t = number("thickness")) out << tr("Thickness %1").arg(length(*t));
  } else if (c.kind == "boss") {
    if (const auto h = number("height")) out << tr("Height %1").arg(length(*h));
  } else if (c.kind == "pocket") {
    if (const auto d = number("depth")) out << tr("Depth %1").arg(length(*d));
  }
  return out;
}

void SmartSelect::measure(const smart::Candidate& c) {
  const QStringList lines = measures(c);
  if (lines.isEmpty()) return;
  const QString text = tr("%1: %2").arg(label(c), lines.join(" · "));
  trace::log("smart select: measured " + text);
  services().toast(text, tr("Copy"), [text] { QGuiApplication::clipboard()->setText(text); }, 10000);
}

QString SmartSelect::iconOf(const smart::Candidate& c) const {
  if (c.kind == "feature") return c.icon.empty() ? QString("box") : QString::fromStdString(c.icon);
  static const std::map<std::string, const char*> byKind = {{"import", "import"}, {"hole", "hole"},     {"fillet", "fillet"},       {"chamfer", "chamfer"},
                                                            {"boss", "extrude"},  {"pocket", "subtract"}, {"wall", "shell"},        {"tangent", "smartChain"},
                                                            {"loop", "smartLoop"}, {"body", "body"}};
  const auto it = byKind.find(c.kind);
  return it == byKind.end() ? QString("dot") : QString(it->second);
}

OPAD_AREA(SmartSelect)
