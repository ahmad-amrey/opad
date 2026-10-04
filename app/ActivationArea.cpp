// Activate component (UI-33): one component is active, the document root by default (AppDocument::activeComponent,
// session state). The view ghosts everything outside it (the Activation layer of looks), pickable only as references
// (Viewport::ghostsPickable: guided tools, feature inputs, sketch Project, a sketch plane being chosen); the browser has
// a radio on the document and component rows (Alt+click on the row does the same), an 'active' pill on the active one,
// dims what is outside, and its breadcrumb leads to it; the chips row names it with the way back to the root; the
// timeline dims the ops that do not touch it. A ghost under the resting mouse is named in the status bar with its
// component, a double click on it activates that component (as SketchUp opens a group), and so does the right-click
// menu there (Viewport::ghostAt: the navigation selector, which keeps ghosts). New sketches, features, bodies, imports
// and components go into it (DesignController, AppDocument::startImport, design.newcomponent) and F frames it
// (view.fit). The one activated last in a document is remembered (setting view/active/<uuid>) and active again when the
// document is opened again. Active
// component visibility (setting view/activeVisibility) off draws and picks the rest as it is; Inactive opacity (setting
// view/inactiveOpacity, absent: the theme's ghost alpha) is the ghosts' opacity; Only the active component's history
// (setting view/activeHistoryOnly) leaves the other ops off the timeline; all in the Design menu and the chip's right-click.
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "AreaController.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Jobs.hpp"
#include "Ribbon.hpp"
#include "TimelineWidget.hpp"
#include "Viewport.hpp"
#include "ViewportChips.hpp"

OPAD_ICON_TABLE(activation,
                {"radioOn", R"(<circle cx="12" cy="12" r="7"/><circle cx="12" cy="12" r="3.5" fill="currentColor"/>)"},
                {"radioOff", R"(<circle cx="12" cy="12" r="7"/>)"},
                {"activate", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M4 8l8 4 8-4M12 12v8"/><circle cx="12" cy="8" r="1.8" fill="currentColor"/>)"},
                {"activateRoot", R"(<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/><circle cx="12" cy="14" r="2.5" fill="currentColor"/>)"},
                {"activeVisibility", R"(<path d="M10 5l6-3 6 3v8l-6 3" stroke-dasharray="2 2"/><path d="M2 10l6-3 6 3v8l-6 3-6-3z"/><path d="M2 10l6 3 6-3M8 13v8"/>)"},
                {"activeHistory", R"(<path d="M2 12h3M9 12h6M19 12h3"/><circle cx="7" cy="12" r="2" fill="currentColor"/><rect x="15" y="8" width="4" height="8" rx="1" stroke-dasharray="1.5 1.5"/>)"});

namespace {
// `id` is `component` or under it (a node); the root ("") holds everything.
bool under(const opad::Scene& scene, const std::string& id, const std::string& component) {
  if (component.empty()) return true;
  for (const opad::Node* n = scene.node(id); n; n = n->parent.empty() ? nullptr : scene.node(n->parent))
    if (n->id == component) return true;
  return false;
}
}  // namespace

class Activation : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    CommandInfo activate{"assembly.activate", tr("Activate component"), "activate", QKeySequence("Alt+A")};
    activate.group = tr("Assembly");
    activate.keywords = {"active", "edit in place", "context"};
    activate.enabledWhen = [this](const CommandContext& c) {
      const std::string id = c.document ? target(c.selection) : std::string();
      return !id.empty() && id != services().document()->activeComponent();
    };
    m_activate = services().addCommand(activate, [this] {
      const std::string id = target(services().selection());
      if (id.empty()) throw opad::Error("Select a component (or a body in it) to activate.");
      setActive(id);
    });
    m_activate->setProperty("shortcutHint", tr("New sketches, features and bodies go into it; the rest of the model is ghosted."));
    shortcuts::updateTooltip(m_activate);
    CommandInfo root{"assembly.activateRoot", tr("Activate root"), "activateRoot"};
    root.group = activate.group;
    root.keywords = {"active", "deactivate"};
    root.enabledWhen = [this](const CommandContext& c) { return c.document && !services().document()->activeComponent().empty(); };
    m_root = services().addCommand(root, [this] { setActive({}); });
    // How the rest of the model is drawn while a component is active: ghosted (at the theme's opacity or the user's), or
    // as it is (Active component visibility off: nothing ghosted, everything picked as usual).
    QSettings settings;
    CommandInfo visibility = root;
    visibility.id = "assembly.activeVisibility";
    visibility.label = tr("Active component visibility");
    visibility.icon = "activeVisibility";
    visibility.enabledWhen = {};
    visibility.keywords = {"ghost", "fade", "inactive", "transparent"};
    visibility.checkable = true;
    m_visibility = services().addCommand(visibility, [this] {
      QSettings().setValue("view/activeVisibility", m_visibility->isChecked());
      refresh();
    });
    m_visibility->setChecked(settings.value("view/activeVisibility", true).toBool());
    m_visibility->setProperty("shortcutHint", tr("While a component is active the rest of the model is ghosted; off, it is drawn and picked as it is."));
    shortcuts::updateTooltip(m_visibility);
    m_opacity = new QMenu(tr("Inactive opacity"), services().window());
    m_opacity->setObjectName("assembly.inactiveOpacity");
    auto* group = new QActionGroup(m_opacity);
    const double chosen = settings.value("view/inactiveOpacity", 0.0).toDouble();  // 0: the theme's ghost
    for (const double value : {0.0, 0.1, 0.25, 0.5, 0.75}) {
      QAction* a = m_opacity->addAction(value == 0 ? tr("Theme") : tr("%1 %").arg(qRound(value * 100)));
      a->setCheckable(true);
      a->setChecked(std::abs(value - chosen) < 1e-6);
      a->setData(value);
      group->addAction(a);
      connect(a, &QAction::triggered, this, [this, value] {
        if (value == 0) QSettings().remove("view/inactiveOpacity");
        else QSettings().setValue("view/inactiveOpacity", value);
        refresh();
      });
    }
    CommandInfo history = visibility;  // the timeline: only the ops that touch the active component (setting view/activeHistoryOnly)
    history.id = "assembly.activeHistory";
    history.label = tr("Only the active component's history");
    history.icon = "activeHistory";
    history.keywords = {"timeline", "history", "filter"};
    m_history = services().addCommand(history, [this] {
      QSettings().setValue("view/activeHistoryOnly", m_history->isChecked());
      refresh();
    });
    m_history->setChecked(settings.value("view/activeHistoryOnly", false).toBool());
    m_history->setProperty("shortcutHint", tr("While a component is active the timeline shows only the steps that touch it; off, the others are dimmed."));
    shortcuts::updateTooltip(m_history);
    m_chipMenu = new QMenu(services().window());
    m_chipMenu->setObjectName("activationChipMenu");
    m_chipMenu->addActions({m_root, m_visibility, m_opacity->menuAction(), m_history});
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* design = menus.value("design");
    if (!design) return;
    const QList<QAction*> items = design->actions();
    const qsizetype at = items.indexOf(services().action("design.newcomponent"));
    design->insertActions(at >= 0 && at + 1 < items.size() ? items[at + 1] : nullptr, {m_activate, m_root, m_visibility, m_opacity->menuAction(), m_history});  // after New component
  }

  void ribbon(RibbonLayout& layout) override {
    layout.addAction("design.assemble.components", m_activate);
    layout.addAction("design.assemble.components", m_root, RibbonLayout::Size::Small);
    layout.addAction("design.assemble.components", m_visibility, RibbonLayout::Size::Small);
  }

  void ready() override {
    AppDocument* doc = services().document();
    m_chip = new QLabel;
    m_chip->setObjectName("chipSel");
    m_chip->setCursor(Qt::PointingHandCursor);
    m_chip->installEventFilter(this);
    m_chip->hide();
    services().chips()->addChip(m_chip);
    services().browser()->addDecorator([this](const browser::Row& row, browser::Decoration& d) { decorate(row, d); });
    services().browser()->tree()->viewport()->installEventFilter(this);  // Alt+click on a component row activates it
    services().viewport()->installEventFilter(this);  // a ghost's hover hint, right-click and double-click
    m_hoverTimer.setSingleShot(true);
    m_hoverTimer.setInterval(120);  // once the mouse rests: one pick of the navigation selector, not one per move
    connect(&m_hoverTimer, &QTimer::timeout, this, &Activation::hoverHint);
    connect(doc, &AppDocument::activeComponentChanged, this, &Activation::refresh);
    // Choosing a sketch plane takes a face of another component too (a reference, as a guided tool's picks are).
    DesignController* design = services().design();
    connect(design, &DesignController::stateChanged, this, [this, design] { services().viewport()->setGhostsPickable(design->pickingPlane()); });
  }

  void contextMenu(const SelectionContext& selection, QMenu& menu) override {
    const std::optional<QPointF> at = std::exchange(m_menuAt, std::nullopt);  // a right-click in the view, there
    AppDocument* doc = services().document();
    if (selection.sketching || !doc->hasDocument) return;
    const std::string& active = doc->activeComponent();
    const std::string id = target(selection);
    if (id.empty() && active.empty()) return;
    menu.addSeparator();
    if (!id.empty() && id != active) menu.addAction(m_activate);
    // On a ghost: the component it is in (the root's is Activate root, below).
    const opad::Node* ghost = at && m_ghosting ? doc->scene.node(services().viewport()->ghostAt(*at)) : nullptr;
    if (ghost && !ghost->parent.empty() && ghost->parent != id && ghost->parent != active) {
      QAction* a = menu.addAction(icons::themed("activate", 16), tr("Activate %1").arg(doc->nodeName(ghost->parent)));
      connect(a, &QAction::triggered, this, [this, component = ghost->parent] { setActive(component); });
    }
    if (!active.empty()) menu.addAction(m_root);
  }

  void documentChanged(bool replaced) override {
    // A document opened again: the component last activated in it comes back (setting view/active/<uuid>).
    AppDocument* doc = services().document();
    if (replaced && doc->hasDocument && doc->activeComponent().empty())
      if (const std::string id = doc->rememberedComponent(); !id.empty()) return doc->setActiveComponent(id);  // its signal refreshes
    refresh();
  }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (object == m_chip && event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
      setActive({});  // the chip's way back to the root
      return true;
    }
    if (object == m_chip && event->type() == QEvent::ContextMenu) {  // how the rest is drawn
      m_chipMenu->exec(static_cast<QContextMenuEvent*>(event)->globalPos());
      return true;
    }
    if (object == services().viewport()) return viewEvent(event);
    BrowserTree* tree = services().browser()->tree();
    if (object == tree->viewport() && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)) {
      const auto* e = static_cast<QMouseEvent*>(event);
      const QModelIndex index = tree->indexAt(e->position().toPoint());
      const QString kind = index.data(Qt::UserRole).toString();
      if (e->button() == Qt::LeftButton && e->modifiers() == Qt::AltModifier && (kind == "document" || kind == "component")) {
        if (event->type() == QEvent::MouseButtonPress) setActive(kind == "document" ? std::string() : index.data(browser::kIdRole).toString().toStdString());
        return true;  // neither selected nor dragged, the second click of a double click no fit
      }
    }
    return AreaController::eventFilter(object, event);
  }

 private:
  // The component a command or the context menu activates: the one selected, or the one a selected body or sketch is in.
  std::string target(const SelectionContext& selection) const {
    if (selection.ids.size() != 1) return {};
    const opad::Scene& scene = services().document()->scene;
    if (const opad::Node* n = scene.node(selection.ids.front())) return n->kind == opad::Node::Kind::Component ? n->id : n->parent;
    const opad::SketchItem* sketch = scene.sketch(selection.ids.front());
    return sketch ? sketch->component : std::string();
  }

  // The view's events for ghosts (not picked, so the viewport does not see them): resting on one names it with the way to
  // activate its component, a double click activates that, and the right-click menu offers it. True: consumed.
  bool viewEvent(QEvent* event) {
    Viewport* view = services().viewport();
    const auto* e = static_cast<const QMouseEvent*>(event);  // for the mouse events below
    switch (event->type()) {
      case QEvent::MouseMove:
        if (e->buttons() == Qt::NoButton && (ghostsIdle() || !m_hint.isEmpty())) {
          m_hoverAt = e->position();
          m_hoverTimer.start();
        }
        return false;
      case QEvent::Leave:
        m_hoverTimer.stop();
        showHint({});
        return false;
      case QEvent::MouseButtonPress:
        if (e->button() == Qt::RightButton) m_rightAt = e->position();
        return false;
      case QEvent::MouseButtonRelease:
        if (e->button() == Qt::RightButton && m_ghosting && (e->position() - m_rightAt).manhattanLength() < 4) {
          m_menuAt = e->position();  // the menu this release opens asks for it; gone if it opens none
          QTimer::singleShot(0, this, [this] { m_menuAt.reset(); });
        }
        return false;
      case QEvent::MouseButtonDblClick: {
        const opad::Node* ghost = e->button() == Qt::LeftButton && e->modifiers() == Qt::NoModifier && ghostsIdle() ? services().document()->scene.node(view->ghostAt(e->position())) : nullptr;
        if (!ghost) return false;
        setActive(ghost->parent);  // a ghost at the root: the root
        showHint({});
        return true;
      }
      default:
        return false;
    }
  }

  // Ghosts shown and nothing else taking the mouse (references picked, a sketch or feature being edited).
  bool ghostsIdle() const {
    return m_ghosting && !services().viewport()->ghostsPickable() && !services().design()->sketchActive() && !services().design()->featureActive();
  }

  void hoverHint() {
    Viewport* view = services().viewport();
    const opad::Node* ghost = ghostsIdle() && view->hoverText().isEmpty() ? services().document()->scene.node(view->ghostAt(m_hoverAt)) : nullptr;
    const AppDocument* doc = services().document();
    showHint(!ghost ? QString()
             : ghost->parent.empty() ? tr("%1 (inactive) · double-click to activate the root").arg(doc->nodeName(ghost->id))
                                     : tr("%1 (inactive, in %2) · double-click to activate %2").arg(doc->nodeName(ghost->id), doc->nodeName(ghost->parent)));
  }

  // In the status bar's hover text, over the viewport's own (which comes back when the hint goes).
  void showHint(const QString& text) {
    if (text == m_hint) return;
    m_hint = text;
    Viewport* view = services().viewport();
    emit view->hoverChanged(text.isEmpty() ? view->hoverText() : text);
  }

  void setActive(const std::string& id) const {
    services().guarded([&] { services().document()->setActiveComponent(id, true); });
  }

  void decorate(const browser::Row& row, browser::Decoration& d) const {
    const AppDocument* doc = services().document();
    const std::string& active = doc->activeComponent();
    if (row.kind == "document" || (row.kind == "component" && row.node)) {
      const std::string id = row.kind == "document" ? std::string() : row.id;
      const bool on = id == active;
      d.lead.icon = on ? "radioOn" : "radioOff";
      d.lead.color = on ? &Tokens::sel : &Tokens::fg3;
      d.lead.tooltip = on ? tr("Active: new sketches, features, bodies and imports go here") : id.empty() ? tr("Activate the root (the whole model)") : tr("Activate this component (or Alt+click its row)");
      d.lead.clicked = [this, id, on] { if (!on) setActive(id); };  // the active one's click is no colour pick either
      if (on && !id.empty()) {
        d.bold = true;
        browser::Badge pill;
        pill.text = tr("active");
        pill.color = &Tokens::sel;
        pill.tooltip = d.lead.tooltip;
        d.badges.push_back(pill);
      }
    }
    if (active.empty()) return;
    const opad::Scene& scene = doc->scene;
    if (row.node) d.dim = d.dim || (!under(scene, row.id, active) && !under(scene, active, row.id));  // outside it and not above it
    else if (row.kind == "sketch")
      if (const opad::SketchItem* sketch = scene.sketch(row.id)) d.dim = d.dim || sketch->component.empty() || !under(scene, sketch->component, active);
  }

  // Everything that follows the active component: the view's ghosts, the chip, the browser rows, the timeline, commands.
  void refresh() {
    AppDocument* doc = services().document();
    const opad::Scene& scene = doc->scene;
    const std::string& active = doc->activeComponent();
    const bool shown = !active.empty() && scene.node(active);  // rolled back to before it: nothing to set apart
    const bool ghosted = shown && m_visibility->isChecked();
    std::map<std::string, LookDelta> layer;
    if (ghosted) {
      LookDelta ghost;
      ghost.ghost = true;
      if (const double opacity = QSettings().value("view/inactiveOpacity", 0.0).toDouble(); opacity > 0) ghost.ghostOpacity = std::clamp(opacity, 0.02, 1.0);
      for (const auto& root : scene.roots) layer[root] = ghost;
      layer[active] = LookDelta{};  // the nearest entry wins: what is under it is drawn as it is
      for (const auto& sketch : scene.sketches)
        if (sketch.component.empty() || !under(scene, sketch.component, active)) layer[sketch.id] = ghost;
    }
    Viewport* view = services().viewport();
    if (ghosted || m_ghosting) view->setLookLayer(LookSource::Activation, std::move(layer));  // the root active: the layer left alone
    m_ghosting = ghosted;
    if (!ghosted) showHint({});
    if (ghosted && !view->ghostsPickable()) {  // what was selected outside it is a ghost now: not selected any more
      const auto picked = view->selection();
      if (std::any_of(picked.begin(), picked.end(), [&](const opad::Ref& r) { return r.kind != opad::Ref::Kind::Point && !under(scene, r.body, active); })) view->clearSelection();
    }
    m_chip->setVisible(shown);
    if (shown) {
      m_chip->setText(tr("Active: %1  ×").arg(doc->nodeName(active)));
      m_chip->setToolTip(tr("%1 is the active component: new sketches, features, bodies and imports go into it, the rest of the model is ghosted. Click to activate the root, right-click for how the rest is drawn.").arg(doc->nodeName(active)));
    }
    if (!shown) services().timeline()->setDimmedOps({});
    else if (doc->rollback().empty()) {  // an edit rolled back to an earlier op: the markers keep what they said
      QElapsedTimer clock;
      clock.start();
      const std::set<std::string> in = opad::ops_in_component(doc->doc, scene, active);
      std::set<std::string> dimmed;
      for (const auto& op : doc->doc.ops)
        if (!in.count(op.id)) dimmed.insert(op.id);  // tombstoned ones and their tombstones are weighed too
      services().timeline()->setDimmedOps(std::move(dimmed), m_history->isChecked());
      if (trace::enabled()) trace::log(QStringLiteral("activation: timeline scope %1 ms").arg(clock.elapsed()));
    }
    services().browser()->refreshDecorations();
    services().updateCommands();
  }

  QAction* m_activate = nullptr;
  QAction* m_root = nullptr;
  QAction* m_visibility = nullptr;  // Active component visibility (setting view/activeVisibility)
  QMenu* m_opacity = nullptr;       // Inactive opacity (setting view/inactiveOpacity, absent: the theme's)
  QAction* m_history = nullptr;     // Only the active component's history (setting view/activeHistoryOnly)
  QMenu* m_chipMenu = nullptr;      // the chip's right-click
  QLabel* m_chip = nullptr;
  bool m_ghosting = false;  // the Activation layer is ours and set (a component was active)
  QTimer m_hoverTimer;
  QPointF m_hoverAt, m_rightAt;
  QString m_hint;                   // the ghost's hover hint shown
  std::optional<QPointF> m_menuAt;  // where the view was right-clicked, for the menu that click opens
};

OPAD_AREA(Activation)
