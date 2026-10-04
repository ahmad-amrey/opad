// Components without dialogs (UI-34). New component makes "Component N" in the active component (or the one selected),
// activates it (Activate new components, setting assembly/activateNew, on by default) and starts its rename in the
// browser, an Activate box under the name to change that there (ActivateToggle); Component from selection (Ctrl+G) puts
// the selected bodies and components into a new component where they are (their nearest common component) in one step
// and starts its rename; Move to component… is a list over the view that narrows as one types (ComponentPicker), the move
// one step. Both, and the browser's drop, keep what moves where it is (the reparent command's keep_place). Opacity is a
// slider in the right-click menu and in the Opacity command's popup (OpacitySlider), drawn live through the Edit layer of
// looks and written for the bodies under the selection, as one step, when it is let go.
#include <QAction>
#include <QCursor>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTreeWidgetItemIterator>
#include <QWidgetAction>

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "AreaController.hpp"
#include "AssemblyWidgets.hpp"
#include "BodyLook.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "Icons.hpp"
#include "Ribbon.hpp"
#include "Viewport.hpp"

OPAD_ICON_TABLE(components, {"componentFrom", R"(<rect x="3" y="3" width="18" height="18" rx="2" stroke-dasharray="3 2"/><path d="M8 9.5l4-2 4 2v5l-4 2-4-2z"/>)"});

class Components : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    auto selected = [this](const CommandContext& c) { return c.document && !c.sketching && !nodes(c.selection).empty(); };
    auto available = [](const CommandContext& c) { return c.document && !c.sketching; };
    CommandInfo create{"design.newcomponent", tr("New component"), "plus"};
    create.keywords = {"group", "assembly", "part", "subassembly"};
    create.editsDocument = true;
    create.enabledWhen = [](const CommandContext& c) { return c.document && !c.sketching; };
    m_new = services().addCommand(create, [this] { newComponent(); });
    m_new->setProperty("shortcutHint", tr("Made in the active component and activated; type its name in the browser."));
    shortcuts::updateTooltip(m_new);
    CommandInfo group{"design.componentFromSelection", tr("Component from selection"), "componentFrom", QKeySequence("Ctrl+G")};
    group.group = tr("Assembly");
    group.keywords = {"group", "wrap", "make component", "subassembly"};
    group.editsDocument = true;
    group.enabledWhen = selected;
    m_group = services().addCommand(group, [this] { fromSelection(); });
    m_group->setProperty("shortcutHint", tr("The selected bodies and components go into a new component, where they are."));
    shortcuts::updateTooltip(m_group);
    CommandInfo move{"design.reparent", tr("Move to component…"), "reparent"};
    move.keywords = {"reparent", "parent", "move into", "group"};
    move.editsDocument = true;
    move.enabledWhen = available;  // nothing selected: says what to select and waits for it (UI-109)
    m_move = services().addCommand(move, [this] { pickTarget(nodes(services().selection())); });
    m_move->setIconText(tr("Move to"));  // the ribbon's label: the Assemble tab has no room for more
    m_move->setProperty("shortcutHint", tr("Type to find the component; what moves keeps its place."));
    shortcuts::updateTooltip(m_move);
    CommandInfo opacity{"design.opacity", tr("Opacity…"), "wireframe"};
    opacity.keywords = {"transparency", "see-through", "translucent", "alpha"};
    opacity.enabledWhen = available;  // a view setting in viewer mode too; waits for a selection like Move to
    m_opacity = services().addCommand(opacity, [this] { opacityPopup(nodes(services().selection())); });
    m_opacity->setIconText(tr("Opacity"));
    CommandInfo activate{"assembly.activateNew", tr("Activate new components"), "activate"};
    activate.group = group.group;
    activate.keywords = {"active", "new component"};
    activate.checkable = true;
    m_activateNew = services().addCommand(activate, [this] { QSettings().setValue("assembly/activateNew", m_activateNew->isChecked()); });
    m_activateNew->setChecked(QSettings().value("assembly/activateNew", true).toBool());
    m_activateNew->setProperty("shortcutHint", tr("A new component becomes the active one, so what is made next goes into it."));
    shortcuts::updateTooltip(m_activateNew);
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* design = menus.value("design");
    if (!design) return;
    const QList<QAction*> items = design->actions();
    const qsizetype at = items.indexOf(m_new);
    design->insertActions(at >= 0 && at + 1 < items.size() ? items[at + 1] : nullptr, {m_group, m_activateNew});  // after New component
  }

  // Design > Assemble has no room for one more tool at 1600 px: Component from selection drops down from New component.

  // After the window's Lock: the opacity slider, Move to component… and Component from selection.
  void contextMenu(const SelectionContext& selection, QMenu& menu) override {
    const std::vector<std::string> ids = nodes(selection);
    if (selection.sketching || !services().document()->hasDocument || ids.empty()) return;
    auto* slider = new OpacitySlider(opacityOf(ids));
    auto* opacity = new QWidgetAction(&menu);
    opacity->setObjectName("opacitySliderAction");
    opacity->setDefaultWidget(slider);
    wire(slider, ids);
    connect(&menu, &QMenu::aboutToHide, slider, [slider] { slider->finish(true); });
    const QList<QAction*> items = menu.actions();
    const qsizetype at = items.indexOf(services().action("design.lock"));
    menu.insertActions(at >= 0 && at + 1 < items.size() ? items[at + 1] : nullptr, {opacity, m_move, m_group});
  }

 private:
  std::vector<std::string> nodes(const SelectionContext& selection) const {  // the selection's bodies and components
    std::vector<std::string> out;
    for (const auto& id : selection.ids)
      if (services().document()->scene.node(id)) out.push_back(id);
    return out;
  }

  // Without what is under another of them (it moves with that one).
  std::vector<std::string> top(const std::vector<std::string>& ids) const {
    const opad::Scene& scene = services().document()->scene;
    const std::set<std::string> chosen(ids.begin(), ids.end());
    std::vector<std::string> out;
    for (const auto& id : ids) {
      bool inside = false;
      for (const opad::Node* n = scene.node(id); n && !n->parent.empty() && !inside; n = scene.node(n->parent)) inside = chosen.count(n->parent) > 0;
      if (!inside) out.push_back(id);
    }
    return out;
  }

  QString freeName() const {  // "Component N", the first N no node is named
    std::set<QString> taken;
    for (const auto& [id, n] : services().document()->scene.nodes) taken.insert(QString::fromStdString(n.name));
    for (int i = 1;; ++i)
      if (const QString name = tr("Component %1").arg(i); !taken.count(name)) return name;
  }

  // The new component selected (and activated when asked), its name being edited in the browser.
  void settle(const std::string& id, bool activate) {
    if (id.empty()) return;
    BrowserPanel* browser = services().browser();
    browser->selectIds({id});
    if (QAction* a = services().action("assembly.activate"); activate && a && a->isEnabled()) a->trigger();
    services().revealBrowser();
    browser->startRename(id);
  }

  void newComponent() {
    AppDocument* doc = services().document();
    const std::vector<std::string> ids = nodes(services().selection());
    std::string parent = doc->activeComponent();
    if (parent.empty() && ids.size() == 1 && doc->scene.node(ids.front())->kind == opad::Node::Kind::Component) parent = ids.front();
    opad::json args{{"name", freeName().toStdString()}};
    if (!parent.empty()) args["parent"] = parent;
    const std::string before = doc->activeComponent();
    const std::string id = doc->run("component", args).value("id", "");
    settle(id, m_activateNew->isChecked());
    offerActivate(id, before);
  }

  // Activate under the name being typed: ticked or not, it is so at once (back to what was active before), and the next
  // new component starts the same way.
  void offerActivate(const std::string& id, const std::string& before) {
    BrowserTree* tree = services().browser()->tree();
    QWidget* editor = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it && !editor; ++it)
      if ((*it)->data(0, Qt::UserRole).toString() == "component" && (*it)->data(0, browser::kIdRole).toString().toStdString() == id) editor = tree->indexWidget(tree->indexFromItem(*it));
    if (!editor) return;
    if (!m_toggle) {
      m_toggle = new ActivateToggle(tree->viewport());
      connect(m_toggle, &ActivateToggle::toggled, this, [this](bool on) {
        m_activateNew->setChecked(on);
        QSettings().setValue("assembly/activateNew", on);
        services().guarded([&] {
          AppDocument* doc = services().document();
          if (!doc->scene.node(m_made)) return;
          doc->setActiveComponent(on ? m_made : doc->scene.node(m_before) ? m_before : std::string(), true);
        });
      });
    }
    m_made = id;
    m_before = before;
    m_toggle->showFor(editor, m_activateNew->isChecked());
  }

  void fromSelection() {
    AppDocument* doc = services().document();
    const opad::Scene& scene = doc->scene;
    const std::vector<std::string> ids = top(nodes(services().selection()));
    if (ids.empty()) throw opad::Error("Select the bodies or components to put into a new component first.");
    std::vector<std::string> common;  // the components above all of them, outermost first
    for (size_t i = 0; i < ids.size(); ++i) {
      const opad::Node* n = scene.node(ids[i]);
      const std::vector<std::string> above = n->parent.empty() ? std::vector<std::string>() : scene.path_to(n->parent);
      if (i == 0) common = above;
      size_t k = 0;
      while (k < common.size() && k < above.size() && common[k] == above[k]) ++k;
      common.resize(k);
    }
    const std::string parent = common.empty() ? std::string() : common.back();
    std::string id;
    doc->batch(tr("component from selection"), [&] {
      opad::json args{{"name", freeName().toStdString()}};
      if (!parent.empty()) args["parent"] = parent;
      id = doc->run("component", args).value("id", "");
      reparent(ids, id);
    });
    settle(id, false);
  }

  // Under `parent` where they are (keep_place: a transform for what would jump otherwise).
  void reparent(const std::vector<std::string>& ids, const std::string& parent) {
    services().document()->run("reparent", opad::json{{"targets", ids}, {"parent", parent.empty() ? opad::json(nullptr) : opad::json(parent)}, {"keep_place", true}});
  }

  // Where the selection can go: the root and every component that does not move with it, in tree order.
  std::vector<ComponentPicker::Entry> targets(const std::vector<std::string>& ids) const {
    const AppDocument* doc = services().document();
    const opad::Scene& scene = doc->scene;
    const std::set<std::string> moving(ids.begin(), ids.end());
    std::optional<std::string> from;  // their parent, when they share one
    for (const auto& id : ids)
      if (const opad::Node* n = scene.node(id); !from || *from == n->parent) from = n->parent;
      else from = "?";
    std::vector<ComponentPicker::Entry> out;
    out.push_back({"", tr("Document root"), QString(), 0, from && from->empty()});
    std::function<void(const std::string&, int, const QStringList&)> walk = [&](const std::string& id, int depth, const QStringList& path) {
      const opad::Node* n = scene.node(id);
      if (!n || n->kind != opad::Node::Kind::Component || moving.count(id)) return;  // it moves, with what it holds
      const QString name = doc->nodeName(id);
      out.push_back({id, name, path.join(QString::fromUtf8(" › ")), depth, from && *from == id});
      for (const auto& child : n->children) walk(child, depth + 1, path + QStringList{name});
    };
    for (const auto& root : scene.roots) walk(root, 1, {});
    return out;
  }

  void pickTarget(const std::vector<std::string>& ids) {
    if (ids.empty()) throw opad::UserHint("Select the objects to move under another component first.", true);
    if (!m_picker) {
      m_picker = new ComponentPicker(services().window());
      connect(m_picker, &ComponentPicker::chosen, this, [this](const std::string& parent) { services().guarded([&] { moveInto(m_moving, parent); }); });
    }
    m_moving = top(ids);
    m_picker->setEntries(targets(m_moving));
    m_picker->popup(QCursor::pos());
  }

  void moveInto(const std::vector<std::string>& ids, const std::string& parent) {
    AppDocument* doc = services().document();
    if (ids.empty() || (!parent.empty() && !doc->scene.node(parent))) return;  // the document changed under the list
    const QString name = parent.empty() ? tr("the document root") : doc->nodeName(parent);
    reparent(ids, parent);
    services().showMessage(tr("Moved into %1").arg(name));
  }

  // ---- opacity
  double opacityOf(const std::vector<std::string>& ids) const {
    const opad::Scene& scene = services().document()->scene;
    for (const auto& id : ids)
      for (const auto& body : scene.bodies_under(id)) return scene.node(body)->opacity;
    return 1;
  }

  void wire(OpacitySlider* slider, const std::vector<std::string>& ids) {
    connect(slider, &OpacitySlider::previewed, this, [this, ids](double opacity) {
      std::map<std::string, LookDelta> layer;
      for (const auto& id : ids) layer[id].opacity = opacity;  // a component's entry covers what is under it
      services().viewport()->setLookLayer(LookSource::Edit, std::move(layer));
    });
    connect(slider, &OpacitySlider::committed, this, [this, ids](double opacity) { services().guarded([&] { setOpacity(ids, opacity); }); });
    connect(slider, &OpacitySlider::finished, this, [this] { services().viewport()->clearLookLayer(LookSource::Edit); });
  }

  // One step for every body under them (a component's own opacity is not drawn).
  void setOpacity(const std::vector<std::string>& ids, double opacity) {
    AppDocument* doc = services().document();
    std::vector<std::string> bodies;
    std::set<std::string> seen;
    for (const auto& id : ids)
      for (const auto& body : doc->scene.bodies_under(id))
        if (seen.insert(body).second) bodies.push_back(body);
    if (!bodies.empty()) doc->run("appearance", opad::json{{"targets", bodies}, {"opacity", opacity}});
    services().viewport()->clearLookLayer(LookSource::Edit);
  }

  void opacityPopup(const std::vector<std::string>& ids) {
    if (ids.empty()) throw opad::UserHint("Select the objects to make see-through first.", true);
    auto* popup = new OpacityPopup(opacityOf(ids), services().window());
    wire(popup->slider(), ids);
    popup->popup(QCursor::pos());
  }

  QAction *m_new = nullptr, *m_group = nullptr, *m_move = nullptr, *m_opacity = nullptr, *m_activateNew = nullptr;
  ComponentPicker* m_picker = nullptr;
  ActivateToggle* m_toggle = nullptr;
  std::string m_made, m_before;  // the new component the toggle is for, and what was active before it
  std::vector<std::string> m_moving;  // what the picker moves
};

OPAD_AREA(Components)
