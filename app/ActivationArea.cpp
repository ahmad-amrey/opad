// Activate component (UI-33): one component is active, the document root by default (AppDocument::activeComponent,
// session state). The view ghosts everything outside it (the Activation layer of looks), pickable only as references
// (Viewport::ghostsPickable: guided tools, feature inputs, sketch Project, a sketch plane being chosen); the browser has
// a radio on the document and component rows and dims what is outside; the chips row names it with the way back to the
// root; the timeline dims the ops that do not touch it. New sketches, features, bodies, imports and components go into
// it (DesignController, AppDocument::startImport, design.newcomponent) and F frames it (view.fit).
#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>

#include <algorithm>
#include <map>
#include <set>

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
                {"activateRoot", R"(<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/><circle cx="12" cy="14" r="2.5" fill="currentColor"/>)"});

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
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* design = menus.value("design");
    if (!design) return;
    const QList<QAction*> items = design->actions();
    const qsizetype at = items.indexOf(services().action("design.newcomponent"));
    design->insertActions(at >= 0 && at + 1 < items.size() ? items[at + 1] : nullptr, {m_activate, m_root});  // after New component
  }

  void ribbon(RibbonLayout& layout) override {
    layout.addAction("design.assemble.components", m_activate);
    layout.addAction("design.assemble.components", m_root, RibbonLayout::Size::Small);
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
    connect(doc, &AppDocument::activeComponentChanged, this, &Activation::refresh);
    // Choosing a sketch plane takes a face of another component too (a reference, as a guided tool's picks are).
    DesignController* design = services().design();
    connect(design, &DesignController::stateChanged, this, [this, design] { services().viewport()->setGhostsPickable(design->pickingPlane()); });
  }

  void contextMenu(const SelectionContext& selection, QMenu& menu) override {
    if (selection.sketching || !services().document()->hasDocument) return;
    const std::string& active = services().document()->activeComponent();
    const std::string id = target(selection);
    if (id.empty() && active.empty()) return;
    menu.addSeparator();
    if (!id.empty() && id != active) menu.addAction(m_activate);
    if (!active.empty()) menu.addAction(m_root);
  }

  void documentChanged(bool) override { refresh(); }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (object == m_chip && event->type() == QEvent::MouseButtonRelease) {  // the chip's way back to the root
      setActive({});
      return true;
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

  void setActive(const std::string& id) const {
    services().guarded([&] { services().document()->setActiveComponent(id); });
  }

  void decorate(const browser::Row& row, browser::Decoration& d) const {
    const AppDocument* doc = services().document();
    const std::string& active = doc->activeComponent();
    if (row.kind == "document" || (row.kind == "component" && row.node)) {
      const std::string id = row.kind == "document" ? std::string() : row.id;
      const bool on = id == active;
      d.lead.icon = on ? "radioOn" : "radioOff";
      d.lead.color = on ? &Tokens::sel : &Tokens::fg3;
      d.lead.tooltip = on ? tr("Active: new sketches, features, bodies and imports go here") : id.empty() ? tr("Activate the root (the whole model)") : tr("Activate this component");
      d.lead.clicked = [this, id, on] { if (!on) setActive(id); };  // the active one's click is no colour pick either
      d.bold = d.bold || (on && !id.empty());
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
    std::map<std::string, LookDelta> layer;
    if (shown) {
      LookDelta ghost;
      ghost.ghost = true;
      for (const auto& root : scene.roots) layer[root] = ghost;
      layer[active] = LookDelta{};  // the nearest entry wins: what is under it is drawn as it is
      for (const auto& sketch : scene.sketches)
        if (sketch.component.empty() || !under(scene, sketch.component, active)) layer[sketch.id] = ghost;
    }
    Viewport* view = services().viewport();
    if (shown || m_ghosting) view->setLookLayer(LookSource::Activation, std::move(layer));  // the root active: the layer left alone
    m_ghosting = shown;
    if (shown && !view->ghostsPickable()) {  // what was selected outside it is a ghost now: not selected any more
      const auto picked = view->selection();
      if (std::any_of(picked.begin(), picked.end(), [&](const opad::Ref& r) { return r.kind != opad::Ref::Kind::Point && !under(scene, r.body, active); })) view->clearSelection();
    }
    m_chip->setVisible(shown);
    if (shown) {
      m_chip->setText(tr("Active: %1  ×").arg(doc->nodeName(active)));
      m_chip->setToolTip(tr("%1 is the active component: new sketches, features, bodies and imports go into it, the rest of the model is ghosted. Click to activate the root.").arg(doc->nodeName(active)));
    }
    if (!shown) services().timeline()->setDimmedOps({});
    else if (doc->rollback().empty()) {  // an edit rolled back to an earlier op: the markers keep what they said
      QElapsedTimer clock;
      clock.start();
      const std::set<std::string> in = opad::ops_in_component(doc->doc, scene, active);
      std::set<std::string> dimmed;
      for (const auto& op : doc->doc.ops)
        if (!in.count(op.id) && !(op.type == "delete" && in.count(op.data.value("target", ""))))  // nor the tombstone of one that does
          dimmed.insert(op.id);
      services().timeline()->setDimmedOps(std::move(dimmed));
      if (trace::enabled()) trace::log(QStringLiteral("activation: timeline scope %1 ms").arg(clock.elapsed()));
    }
    services().browser()->refreshDecorations();
    services().updateCommands();
  }

  QAction* m_activate = nullptr;
  QAction* m_root = nullptr;
  QLabel* m_chip = nullptr;
  bool m_ghosting = false;  // the Activation layer is ours and set (a component was active)
};

OPAD_AREA(Activation)
