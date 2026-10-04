// Accessibility basics (UI-124). F6 / Shift+F6 take the keyboard through the window's regions, ribbon → browser → view (or
// the start page) → timeline → each open panel, and ring the region reached; a control that takes the focus from the
// keyboard (Tab, Shift+Tab, F6) gets the stylesheet's focus ring through the keyFocus property, never after a click; a
// button showing only an icon, with no name of its own, is named by its tooltip; the view says what it is and where its
// cube is. Reduced motion (Motion.hpp) is in Preferences › Display.
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QAction>
#include <QApplication>
#include <QHash>
#include <QVector3D>
#include <QWindow>

#include <array>
#include <cmath>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QStyle>
#include <QTabBar>
#include <QTextDocumentFragment>
#include <QToolButton>

#include "AreaController.hpp"
#include "BrowserPanel.hpp"
#include "CommandHelp.hpp"
#include "Commands.hpp"
#include "KeyText.hpp"
#include "EmptyState.hpp"
#include "Ribbon.hpp"
#include "RichTip.hpp"
#include "Theme.hpp"
#include "TimelineWidget.hpp"
#include "ToolPanel.hpp"
#include "Viewport.hpp"

namespace {
// Four 2 px strips in the selection colour along a region's edges, children of it (native over the OpenGL view).
class RegionRing : public QObject {
 public:
  using QObject::QObject;
  void show(QWidget* around) {
    clear();
    m_around = around;
    const bool native = around->testAttribute(Qt::WA_NativeWindow);
    for (int i = 0; i < 4; ++i) {
      auto* strip = new QWidget(around);
      strip->setObjectName("regionRing");
      strip->setAttribute(Qt::WA_TransparentForMouseEvents);
      if (native) strip->setAttribute(Qt::WA_NativeWindow);
      strip->setAutoFillBackground(true);
      QPalette p = strip->palette();
      p.setColor(QPalette::Window, theme::current().sel);
      strip->setPalette(p);
      m_strips << strip;
    }
    place();
    for (QWidget* s : m_strips) {
      s->show();
      s->raise();
    }
    around->installEventFilter(this);
  }
  void clear() {
    if (m_around) m_around->removeEventFilter(this);
    for (QWidget* s : std::exchange(m_strips, {})) s->deleteLater();
    m_around = nullptr;
  }
  QWidget* around() const { return m_around; }
  bool shown() const { return m_around && !m_strips.isEmpty(); }

 protected:
  bool eventFilter(QObject* o, QEvent* e) override {
    if (o == m_around && e->type() == QEvent::Resize) place();
    return false;
  }

 private:
  void place() {
    if (!m_around || m_strips.size() != 4) return;
    const int w = m_around->width(), h = m_around->height(), t = 2;
    m_strips[0]->setGeometry(0, 0, w, t);
    m_strips[1]->setGeometry(0, h - t, w, t);
    m_strips[2]->setGeometry(0, 0, t, h);
    m_strips[3]->setGeometry(w - t, 0, t, h);
  }
  QPointer<QWidget> m_around;
  QList<QWidget*> m_strips;
};

bool fromKeyboard(Qt::FocusReason reason) { return reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason; }

// The controls the stylesheet rings (Theme.cpp, keyFocus).
bool ringed(const QWidget* w) {
  return qobject_cast<const QAbstractButton*>(w) || qobject_cast<const QAbstractItemView*>(w) || qobject_cast<const QTabBar*>(w) || qobject_cast<const QAbstractSlider*>(w);
}

void setKeyFocus(QWidget* w, bool on) {
  if (w->property("keyFocus").toBool() == on) return;
  w->setProperty("keyFocus", on);
  w->style()->unpolish(w);
  w->style()->polish(w);
  w->update();
}

// "Close  (Esc)" or rich text: the words before the key.
QString nameFromTip(const QString& tip) {
  QString text = Qt::mightBeRichText(tip) ? QTextDocumentFragment::fromHtml(tip).toPlainText() : tip;
  text = text.section('\n', 0, 0);
  static const QRegularExpression key(R"(\s*\([^()]*\)\s*$)");
  return text.remove(key).trimmed();
}

// ---- items a screen reader sees inside painted widgets: the timeline's markers and the view cube's faces
// An item of a widget that paints it: no object of its own, its place and words asked of the widget each time.
class PaintedItem : public QAccessibleInterface, public QAccessibleActionInterface {
 public:
  explicit PaintedItem(QWidget* owner) : m_owner(owner) {}
  QObject* object() const override { return nullptr; }
  QWindow* window() const override { return m_owner ? m_owner->window()->windowHandle() : nullptr; }
  QAccessibleInterface* parent() const override { return QAccessible::queryAccessibleInterface(m_owner.data()); }
  QAccessibleInterface* child(int) const override { return nullptr; }
  QAccessibleInterface* childAt(int, int) const override { return nullptr; }
  int childCount() const override { return 0; }
  int indexOfChild(const QAccessibleInterface*) const override { return -1; }
  void setText(QAccessible::Text, const QString&) override {}
  void* interface_cast(QAccessible::InterfaceType type) override { return type == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface*>(this) : nullptr; }
  QStringList keyBindingsForAction(const QString&) const override { return {}; }

 protected:
  QRect global(const QRect& r) const { return m_owner && r.isValid() ? QRect(m_owner->mapToGlobal(r.topLeft()), r.size()) : QRect(); }
  QPointer<QWidget> m_owner;
};

// A marker: its op as the tooltip names it, its state (tombstoned, suppressed, ...), selected while current; Press selects
// what it touches, Show menu opens its menu.
class MarkerItem : public PaintedItem {
 public:
  MarkerItem(TimelineWidget* timeline, int index) : PaintedItem(timeline), m_index(index) {}
  bool isValid() const override { return m_owner && m_index < timeline()->markerCount(); }
  QString text(QAccessible::Text t) const override {
    const opad::Op* op = isValid() ? timeline()->markerOp(m_index) : nullptr;
    if (!op) return {};
    if (t == QAccessible::Name) return timeline()->describe(*op);
    if (t == QAccessible::Description) return timeline()->markerState(m_index);
    return {};
  }
  QRect rect() const override { return isValid() ? global(timeline()->markerGeometry(m_index)) : QRect(); }
  QAccessible::Role role() const override { return QAccessible::ListItem; }
  QAccessible::State state() const override {
    QAccessible::State s;
    s.focusable = s.selectable = true;
    if (!isValid()) {
      s.invisible = true;
      return s;
    }
    const bool current = timeline()->currentMarker() == m_index;
    s.selected = current;
    s.focused = current && timeline()->hasFocus();
    s.invisible = s.offscreen = !timeline()->isVisible() || !timeline()->markerArea().intersects(timeline()->markerGeometry(m_index));
    return s;
  }
  QStringList actionNames() const override { return {pressAction(), showMenuAction()}; }
  void doAction(const QString& name) override {
    const opad::Op* op = isValid() ? timeline()->markerOp(m_index) : nullptr;
    if (!op) return;
    const std::string id = op->id;
    timeline()->setCurrentOp(id);
    if (name == pressAction()) emit timeline()->opClicked(id);
    else if (name == showMenuAction()) timeline()->openMenu();
  }

 private:
  TimelineWidget* timeline() const { return static_cast<TimelineWidget*>(m_owner.data()); }
  int m_index;
};

// The timeline: a list of its markers, then its own child widgets (the scroll bar).
class TimelineItems : public QAccessibleWidget {
 public:
  explicit TimelineItems(TimelineWidget* w) : QAccessibleWidget(w, QAccessible::List) {}
  ~TimelineItems() override {
    for (QAccessible::Id id : std::as_const(m_markers)) QAccessible::deleteAccessibleInterface(id);
  }
  int childCount() const override { return timeline()->markerCount() + QAccessibleWidget::childCount(); }
  QAccessibleInterface* child(int i) const override {
    const int n = timeline()->markerCount();
    if (i < 0) return nullptr;
    if (i >= n) return QAccessibleWidget::child(i - n);
    if (!m_markers.contains(i)) m_markers.insert(i, QAccessible::registerAccessibleInterface(new MarkerItem(timeline(), i)));
    return QAccessible::accessibleInterface(m_markers.value(i));
  }
  int indexOfChild(const QAccessibleInterface* c) const override {
    for (auto it = m_markers.cbegin(); it != m_markers.cend(); ++it)
      if (QAccessible::accessibleInterface(it.value()) == c) return it.key() < timeline()->markerCount() ? it.key() : -1;
    const int own = QAccessibleWidget::indexOfChild(c);
    return own < 0 ? -1 : own + timeline()->markerCount();
  }

 private:
  TimelineWidget* timeline() const { return static_cast<TimelineWidget*>(widget()); }
  mutable QHash<int, QAccessible::Id> m_markers;
};

// A face of the view cube: a button named as the view it turns to ("Top view"), shown while it faces the camera; Press
// runs the window's view command (view.top ...).
struct CubeSide {
  const char* view;
  QVector3D normal;
};
constexpr int kCubeSides = 6;
const CubeSide kSides[kCubeSides] = {{"top", {0, 0, 1}}, {"front", {0, -1, 0}}, {"right", {1, 0, 0}}, {"bottom", {0, 0, -1}}, {"back", {0, 1, 0}}, {"left", {-1, 0, 0}}};

class CubeFace : public PaintedItem {
 public:
  CubeFace(Viewport* view, int side) : PaintedItem(view), m_side(side) {}
  bool isValid() const override { return !m_owner.isNull(); }
  QAction* command() const { return m_owner ? m_owner->window()->findChild<QAction*>(QString("view.") + kSides[m_side].view) : nullptr; }
  QString text(QAccessible::Text t) const override {
    QAction* a = command();
    if (t == QAccessible::Name) return a ? QString(a->text()).remove('&') : QString();
    if (t == QAccessible::Accelerator && a) return a->shortcut().toString(QKeySequence::NativeText);
    return {};
  }
  // How much the face looks at the camera (1: straight on, 0 or less: edge on or away) and where its centre is drawn.
  double facing(QPointF* centre = nullptr) const {
    auto* view = static_cast<Viewport*>(m_owner.data());
    const QRect cube = view ? view->cubeRect() : QRect();
    const opad::json camera = cube.isEmpty() ? opad::json() : view->cameraJson();
    if (!camera.contains("eye")) return -1;
    auto vec = [&](const char* key) { return QVector3D(camera[key][0].get<float>(), camera[key][1].get<float>(), camera[key][2].get<float>()); };
    const QVector3D dir = (vec("target") - vec("eye")).normalized(), right = QVector3D::crossProduct(dir, vec("up")).normalized(),
                    up = QVector3D::crossProduct(right, dir);
    const QVector3D n = kSides[m_side].normal;
    if (centre) *centre = QPointF(cube.center()) + QPointF(QVector3D::dotProduct(n, right), -QVector3D::dotProduct(n, up)) * 32;
    return -QVector3D::dotProduct(n, dir);
  }
  QRect rect() const override {
    QPointF centre;
    const double f = facing(&centre);
    if (f <= 0) return {};
    const int size = std::max(8, int(std::lround(56 * f)));
    return global(QRect(centre.toPoint() - QPoint(size / 2, size / 2), QSize(size, size)));
  }
  QAccessible::Role role() const override { return QAccessible::PushButton; }
  QAccessible::State state() const override {
    QAccessible::State s;
    s.invisible = s.offscreen = !m_owner || !m_owner->isVisible() || facing() < 0.15;
    s.disabled = !command() || !command()->isEnabled();
    return s;
  }
  QStringList actionNames() const override { return {pressAction()}; }
  void doAction(const QString& name) override {
    if (QAction* a = command(); a && a->isEnabled() && name == pressAction()) a->trigger();
  }

 private:
  int m_side;
};

// The view: its own child widgets (chips, buttons, toasts), then the cube's six faces.
class ViewItems : public QAccessibleWidget {
 public:
  explicit ViewItems(Viewport* w) : QAccessibleWidget(w, QAccessible::Client) {}
  ~ViewItems() override {
    for (QAccessible::Id id : m_faces)
      if (id) QAccessible::deleteAccessibleInterface(id);
  }
  int childCount() const override { return QAccessibleWidget::childCount() + kCubeSides; }
  QAccessibleInterface* child(int i) const override {
    const int own = QAccessibleWidget::childCount();
    if (i < own) return QAccessibleWidget::child(i);
    if (i >= own + kCubeSides) return nullptr;
    QAccessible::Id& id = m_faces[size_t(i - own)];
    if (!id) id = QAccessible::registerAccessibleInterface(new CubeFace(static_cast<Viewport*>(widget()), i - own));
    return QAccessible::accessibleInterface(id);
  }
  int indexOfChild(const QAccessibleInterface* c) const override {
    for (int k = 0; k < kCubeSides; ++k)
      if (m_faces[size_t(k)] && QAccessible::accessibleInterface(m_faces[size_t(k)]) == c) return QAccessibleWidget::childCount() + k;
    return QAccessibleWidget::indexOfChild(c);
  }

 private:
  mutable std::array<QAccessible::Id, kCubeSides> m_faces{};
};

QAccessibleInterface* paintedItems(const QString& className, QObject* object) {
  if (className == QLatin1String("TimelineWidget")) return new TimelineItems(static_cast<TimelineWidget*>(object));
  if (className == QLatin1String("Viewport")) return new ViewItems(static_cast<Viewport*>(object));
  return nullptr;
}
}  // namespace

class AccessibilityArea : public AreaController {
 public:
  explicit AccessibilityArea(AreaServices& services) : AreaController(services) {}

  void buildActions() override {
    static const bool installed = (QAccessible::installFactory(paintedItems), true);
    Q_UNUSED(installed);
    CommandInfo next;
    next.id = "view.nextRegion";
    next.label = tr("Next region");
    next.key = QKeySequence("F6");
    next.keywords = {"keyboard", "focus", "accessibility", "panes"};  // its key is found as the key it has now
    services().addCommand(next, [this] { cycle(1); });
    CommandInfo previous;
    previous.id = "view.previousRegion";
    previous.label = tr("Previous region");
    previous.key = QKeySequence("Shift+F6");
    previous.keywords = {"keyboard", "focus", "accessibility", "panes"};
    services().addCommand(previous, [this] { cycle(-1); });
  }

  void ready() override {
    m_ring = new RegionRing(this);
    qApp->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
      if (m_ring->shown() && !(now && (now == m_ring->around() || m_ring->around()->isAncestorOf(now)))) m_ring->clear();
    });
    Viewport* view = services().viewport();
    view->setAccessibleName(tr("Model view"));
    auto describe = [view] {  // Home's and Fit's keys now (keys::notifier)
      view->setAccessibleDescription(help::expand(tr("The view cube at the top right turns the view: click a face, an edge or a corner. Home ({key:view.home}) goes home, Fit ({key:view.fit}) fits.")));
    };
    describe();
    connect(keys::notifier(), &keys::Notifier::changed, view, describe);
    if (auto* timeline = services().window()->findChild<TimelineWidget*>()) {
      timeline->setAccessibleName(tr("Timeline"));
      timeline->setAccessibleDescription(help::expand(tr("The document's history. Left and Right step through it, Home and End go to its ends, {fixed:enter} or {fixed:f2} edits a feature or a sketch, {fixed:space} suppresses a feature, {fixed:del} tombstones, {fixed:shiftDel} restores, the Menu key opens the marker's menu.")));
    }
    if (services().browser()) {
      services().browser()->tree()->setAccessibleName(tr("Browser"));
      services().browser()->tree()->setAccessibleDescription(help::expand(tr("{fixed:space} shows or hides the selected objects, {fixed:enter} fits the view to one or edits a sketch, {fixed:f2} renames, {fixed:del} deletes, the Menu key opens the menu.")));
    }
    nameButtons(services().window());
  }

  // The regions F6 visits, in order, as the window is now: what takes the focus and what is ringed.
  struct Region {
    QString id;
    QWidget* focus;
    QWidget* area;  // where the region is: the focus is in it when inside this
  };
  QList<Region> regions() const {
    QMainWindow* w = services().window();
    QList<Region> out;
    if (auto* ribbon = w->findChild<RibbonBar*>(); ribbon && ribbon->isVisible()) out.push_back({"ribbon", ribbon->tabBar(), ribbon});
    auto* overlay = w->findChild<QFrame*>("browserOverlay");
    if (BrowserPanel* browser = services().browser(); browser && overlay && overlay->isVisible()) out.push_back({"browser", browser->tree(), overlay});
    if (Viewport* view = services().viewport(); view->isVisible()) out.push_back({"view", view, view});
    else if (auto* start = w->findChild<EmptyState*>(); start && start->isVisible()) out.push_back({"start", startFocus(start), start});
    if (auto* timeline = w->findChild<TimelineWidget*>(); timeline && timeline->isVisible()) out.push_back({"timeline", timeline, timeline});
    for (ToolPanel* panel : w->findChildren<ToolPanel*>())
      if (panel->isVisible()) out.push_back({"panel:" + panel->id(), panelFocus(panel), panel});
    return out;
  }

  // Where the keyboard is: the region holding the focus (or the active tool window); -1 when none does.
  int current(const QList<Region>& list) const {
    QWidget* focus = QApplication::focusWidget();
    if (!focus) focus = m_entered;  // no window is active (a hidden window, the app in the background): where F6 went last
    QWidget* active = QApplication::activeWindow();
    for (int i = 0; i < list.size(); ++i) {
      QWidget* area = list[i].area;
      if (focus && (focus == area || area->isAncestorOf(focus))) return i;
      if (!focus && active && active == area->window() && area->isWindow()) return i;
    }
    return -1;
  }

  void cycle(int step) {
    const QList<Region> list = regions();
    if (list.isEmpty()) return;
    const int at = current(list);
    const int next = at < 0 ? (step > 0 ? 0 : int(list.size()) - 1) : (at + step + int(list.size())) % int(list.size());
    enter(list[next]);
  }

  void enter(const Region& r) {
    QWidget* window = r.area->window();
    window->raise();
    window->activateWindow();
    if (auto* tree = qobject_cast<QAbstractItemView*>(r.focus); tree && !tree->currentIndex().isValid() && tree->model() && tree->model()->rowCount() > 0)
      tree->setCurrentIndex(tree->model()->index(0, 0));
    r.focus->setFocus(Qt::TabFocusReason);  // a keyboard move: the control's ring shows
    setKeyFocus(r.focus, ringed(r.focus));
    // The view, the timeline and the start page draw no ring of their own, nor does the browser in its see-through window.
    if (!ringed(r.focus) || r.id == "browser") m_ring->show(r.area->isWindow() ? r.focus : r.area);
    else m_ring->clear();
    m_region = r.id;
    m_entered = r.focus;
  }
  QString lastRegion() const { return m_region; }
  QWidget* ring() const { return m_ring->shown() ? m_ring->around() : nullptr; }

 protected:
  bool eventFilter(QObject* o, QEvent* e) override {
    switch (e->type()) {
      case QEvent::FocusIn:
      case QEvent::FocusOut:
        if (auto* w = qobject_cast<QWidget*>(o); w && ringed(w)) setKeyFocus(w, e->type() == QEvent::FocusIn && fromKeyboard(static_cast<QFocusEvent*>(e)->reason()));
        break;
      case QEvent::MouseButtonPress:
        if (m_ring->shown()) m_ring->clear();
        break;
      case QEvent::Polish:
      case QEvent::ToolTipChange:
        if (auto* b = qobject_cast<QAbstractButton*>(o)) name(b);
        break;
      case QEvent::KeyPress: {
        // The window's F6 and F1 (Help for this tool) do not reach a floating panel or the browser (other windows): their
        // keys are taken here; F1 over a command's card is the card's (RichTip).
        auto* w = qobject_cast<QWidget*>(o);
        QWidget* top = w ? w->window() : nullptr;
        if (!top || top == services().window() || top->parentWidget() != services().window()) break;
        const QKeySequence pressed(static_cast<QKeyEvent*>(e)->keyCombination());
        for (const char* id : {"view.nextRegion", "view.previousRegion", "help.current"})
          if (QAction* a = services().action(id); a && a->isEnabled() && a->shortcuts().contains(pressed) && (QLatin1String(id) != QLatin1String("help.current") || RichTip::instance()->state() == RichTip::State::Hidden)) {
            a->trigger();
            return true;
          }
        break;
      }
      default:
        break;
    }
    return false;
  }

 private:
  static QWidget* startFocus(EmptyState* start) {
    if (!start->cards().isEmpty()) return start->cards().first();
    for (QPushButton* b : start->findChildren<QPushButton*>("primary")) return b;
    return start;
  }

  static QWidget* panelFocus(ToolPanel* panel) {
    for (QWidget* w = panel->nextInFocusChain(); w && w != panel; w = w->nextInFocusChain())
      if (panel->isAncestorOf(w) && w->isVisibleTo(panel) && w->isEnabled() && (w->focusPolicy() & Qt::TabFocus)) return w;
    return panel;
  }

  // An icon-only button with no name of its own speaks its tooltip.
  static void name(QAbstractButton* b) {
    if (!b->accessibleName().isEmpty() && !b->property("nameFromTip").toBool()) return;
    QString text = b->text();
    if (auto* tool = qobject_cast<QToolButton*>(b); tool && tool->defaultAction()) text = tool->defaultAction()->text();
    if (!text.remove('&').trimmed().isEmpty()) return;
    const QString tip = nameFromTip(b->toolTip());
    if (tip.isEmpty()) return;
    b->setAccessibleName(tip);
    b->setProperty("nameFromTip", true);
  }

  static void nameButtons(QWidget* window) {
    for (QAbstractButton* b : window->findChildren<QAbstractButton*>()) name(b);
  }

  RegionRing* m_ring = nullptr;
  QString m_region;
  QPointer<QWidget> m_entered;
};

OPAD_AREA(AccessibilityArea)

// ---------------------------------------------------------------- bench
#include <QAccessible>
#include <QSettings>
#include <QTimer>
#include <QTreeWidgetItemIterator>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DimensionHandle.hpp"
#include "HelpClip.hpp"
#include "MainWindow.hpp"
#include "Motion.hpp"
#include "SketchEditor.hpp"

// OPAD_BENCH_A11Y=<prefix> (a box and the sketch "Plate"): with the Section panel open and the Distance tool running, the
// accessibility tree of the window and its panels holds no button without a name, and a browser row says whether it is
// shown. F6 goes ribbon, browser, view, timeline, then each panel, and round again; Shift+F6 goes back; F6 pressed in a
// panel (another window) goes on too; the view and the timeline are ringed. A control focused by Tab gets the focus ring,
// one focused by a click does not. Reduced motion holds the clips and makes the camera jump; without it the camera turns.
// The extrude arrow takes a press 11 px off its shaft, not 14 (a 24 px target), and sketch points take one 12 px away.
// Saved as <prefix>.timeline.png (ringed).
OPAD_BENCH(OPAD_BENCH_A11Y, accessibility) {
  const QString prefix = value;
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: accessibility: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  AccessibilityArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (a->objectName() == "AccessibilityArea") area = static_cast<AccessibilityArea*>(a);
  if (!area) {
    check(false, "the accessibility area");
    QCoreApplication::exit(2);
    return true;
  }
  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };

  add(500, [=, &w] {
    w.action("panel.section")->trigger();
    w.startTool("distance");
  });
  add(500, [=, &w] {
    // Every button in the window and its open panels has a name.
    QStringList unnamed;
    int buttons = 0;
    std::function<void(QAccessibleInterface*, int)> walk = [&](QAccessibleInterface* face, int depth) {
      if (!face || depth > 40) return;
      const QAccessible::Role role = face->role();
      const bool button = role == QAccessible::PushButton || role == QAccessible::CheckBox || role == QAccessible::RadioButton || role == QAccessible::ButtonMenu ||
                          role == QAccessible::ButtonDropDown || role == QAccessible::ButtonDropGrid;
      if (button && !face->state().invisible) {
        ++buttons;
        if (face->text(QAccessible::Name).trimmed().isEmpty()) {
          QObject* o = face->object();
          unnamed << (o ? QString("%1 %2 in %3").arg(o->metaObject()->className(), o->objectName(), o->parent() ? o->parent()->objectName() : QString()) : QString("?"));
        }
      }
      for (int i = 0; i < face->childCount(); ++i) walk(face->child(i), depth + 1);
    };
    QList<QWidget*> windows{&w};
    for (QWidget* top : QApplication::topLevelWidgets())
      if (top != &w && top->isVisible() && top->parentWidget() == &w) windows << top;
    for (QWidget* top : windows) walk(QAccessible::queryAccessibleInterface(top), 0);
    check(buttons > 30 && unnamed.isEmpty(), QString("%1 buttons in %2 windows, none without a name (%3)").arg(buttons).arg(windows.size()).arg(unnamed.join("; ")));
    // A browser row says what its eye and lock show.
    QTreeWidgetItem* row = nullptr;
    for (QTreeWidgetItemIterator it(w.m_browser->tree()); *it && !row; ++it)
      if ((*it)->data(0, Qt::UserRole).toString() == "body") row = *it;
    const QString described = row ? row->data(0, Qt::AccessibleDescriptionRole).toString() : QString();
    check(described.contains(QCoreApplication::translate("BrowserPanel", "shown")), "a browser row says it is shown (" + described + ")");
    // F6 round the regions.
    const auto order = area->regions();
    QStringList ids, visited;
    for (const auto& r : order) ids << r.id;
    for (int i = 0; i <= ids.size(); ++i) {
      w.action("view.nextRegion")->trigger();
      visited << area->lastRegion();
    }
    check(ids.size() >= 5 && ids.mid(0, 4) == QStringList({"ribbon", "browser", "view", "timeline"}) && ids.mid(4).filter("panel:").size() == ids.size() - 4,
          "the regions: " + ids.join(", "));
    check(visited.mid(0, ids.size()) == ids && visited.last() == ids.first(), "F6 visits them in turn and comes round (" + visited.join(" ") + ")");
    w.action("view.previousRegion")->trigger();
    check(area->lastRegion() == ids.last(), "Shift+F6 goes back (" + area->lastRegion() + ")");
    // F6 pressed in a panel: another window, the key reaches the region cycle all the same.
    QWidget* inPanel = order.last().focus;
    QKeyEvent f6(QEvent::KeyPress, Qt::Key_F6, Qt::NoModifier);
    QApplication::sendEvent(inPanel, &f6);
    check(area->lastRegion() == ids.first(), "F6 in a panel goes on to the ribbon (" + area->lastRegion() + ")");
    check(w.m_ribbon->tabBar()->property("keyFocus").toBool() && w.m_ribbon->tabBar()->window()->focusWidget() == w.m_ribbon->tabBar(), "the ribbon's tabs have the focus and its ring");
    w.action("view.nextRegion")->trigger();
    w.action("view.nextRegion")->trigger();
    check(area->lastRegion() == "view" && area->ring() == w.m_viewport, "the view is ringed");
    w.action("view.nextRegion")->trigger();
    check(area->lastRegion() == "timeline" && area->ring() == w.m_timeline, "the timeline is ringed");
    w.m_timeline->grab().save(prefix + ".timeline.png");
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), w.m_timeline->mapToGlobal(QPointF(5, 5)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w.m_timeline, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(5, 5), w.m_timeline->mapToGlobal(QPointF(5, 5)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w.m_timeline, &release);
    check(!area->ring(), "a click takes the ring away");
    // The focus ring: from the keyboard, not from a click.
    QPushButton* button = nullptr;
    for (QPushButton* b : w.m_sectionPanel->findChildren<QPushButton*>())
      if (b->isVisibleTo(w.m_sectionPanel)) { button = b; break; }
    bool tabbed = false, clicked = true;
    if (button) {
      QFocusEvent in(QEvent::FocusIn, Qt::TabFocusReason);
      QApplication::sendEvent(button, &in);
      tabbed = button->property("keyFocus").toBool();
      QFocusEvent out(QEvent::FocusOut, Qt::TabFocusReason);
      QApplication::sendEvent(button, &out);
      QFocusEvent click(QEvent::FocusIn, Qt::MouseFocusReason);
      QApplication::sendEvent(button, &click);
      clicked = button->property("keyFocus").toBool();
    }
    check(button && tabbed && !clicked && qApp->styleSheet().contains("[keyFocus=\"true\"]"), "a button tabbed to is ringed, a clicked one is not");
    w.cancelTool();
    // Reduced motion.
    motion::setReduced(true);
    const opad::json before = w.m_viewport->cameraJson();
    w.m_viewport->rollView(90);
    const bool jumped = w.m_viewport->cameraJson()["up"] != before["up"];
    check(jumped && !clips::animations(), "reduced motion: the camera jumps, the clips hold still");
    motion::setReduced(false);
    const opad::json turned = w.m_viewport->cameraJson();
    w.m_viewport->rollView(-90);
    check(w.m_viewport->cameraJson()["up"] == turned["up"] && clips::animations(), "without it the camera turns over a few frames");
    QSettings().remove("ui/reduceMotion");
    // Hit targets: the extrude arrow.
    const auto& sketches = w.m_doc->scene.sketches;
    w.m_viewport->standardView("iso");  // the arrow seen from the side
    if (!sketches.empty()) {
      w.m_design->startFeature("extrude");
      w.m_design->featurePanel()->setPicks("profiles", opad::json::array({opad::json{{"sketch", sketches.front().id}, {"at", {30.0, 5.0}}}}));
    }
  });
  for (int i = 0; i < 60; ++i)  // the preview, and with it the arrow, comes from a worker: waits up to 15 s
    add(250, [] {});
  add(0, [=, &w] {
    DimensionHandle* handle = nullptr;  // the extrude's (the sketch has one of its own, hidden)
    for (DimensionHandle* h : w.m_viewport->findChildren<DimensionHandle*>())
      if (h->isVisible()) handle = h;
    const QLineF line = handle ? handle->arrowLine() : QLineF();
    bool inside = false, outside = true;
    if (handle && handle->isVisible() && line.length() > 4) {
      const QPointF mid = line.pointAt(0.5), normal = QPointF(-line.dy(), line.dx()) / line.length();
      inside = handle->overArrow(mid + normal * 11);
      outside = handle->overArrow(mid + normal * 14);
    }
    check(handle && handle->isVisible() && inside && !outside && DimensionHandle::kHitRadius >= 12, QString("the extrude arrow takes a press 11 px off its shaft, not 14 (%1 px long)").arg(line.length()));
    check(SketchEditor::kHandlePixels >= 12, "sketch points and glyphs take a click 12 px away");
    w.m_design->escape();
    trace::log(QString("bench: accessibility: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [&w, steps, next, check](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, &w, [&w, steps, next, check, i] {
      try { (*steps)[i].fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      size_t after = i + 1;
      const auto handles = w.m_viewport->findChildren<DimensionHandle*>();
      if (std::any_of(handles.begin(), handles.end(), [](DimensionHandle* h) { return h->isVisible() && h->arrowLine().length() > 4; }))
        while (after + 1 < steps->size() && (*steps)[after].delay == 250) ++after;  // the arrow is there: no more waiting
      (*next)(after);
    });
  };
  (*next)(0);
  return true;
}
