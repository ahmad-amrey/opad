// Navigation staples as commands (UI-47): zoom window (Z), previous and next view (F5, Shift+F5), the document's Home set
// to the current view and back, the view twist, the CAD 2D mouse preset, a middle double click fitting everything, the view cube's menu.
// The view side is Viewport (ViewportNavigation.cpp); this area places the commands in the View menus and ribbon tabs.
#include <QActionGroup>
#include <QInputDialog>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Ribbon.hpp"
#include "Units.hpp"
#include "Viewport.hpp"

OPAD_ICON_TABLE(viewnav,
                {"zoomWindow", R"(<rect x="3" y="3" width="11" height="8"/><circle cx="15.5" cy="15.5" r="3.5"/><path d="M18 18l3 3"/>)"},
                {"viewBack", R"(<path d="M9 6l-6 6 6 6"/><path d="M3 12h11a6 6 0 0 1 6 6"/>)"},
                {"viewForward", R"(<path d="M15 6l6 6-6 6"/><path d="M21 12H10a6 6 0 0 0-6 6"/>)"},
                {"twist", R"(<path d="M12 8l4 4-4 4-4-4z"/><path d="M4.5 12a7.5 7.5 0 0 1 13.3-4.7"/><path d="M19 3v5h-5"/>)"},
                {"hiddenLine", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M4 8l8 4 8-4M12 12v8"/>)"},
                {"hiddenEdges", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M4 8l8 4 8-4M12 12v8"/><path d="M4 16l8-4 8 4M12 4v8" stroke-dasharray="2 2"/>)"});

namespace {
// The presets by their setting value (ui/nav): the named ones MainWindow makes, and the generic CAD 2D one.
Viewport::NavPreset presetOf(const QString& name) {
  if (name == "SolidWorks") return Viewport::NavPreset::SolidWorks;
  if (name == "Onshape") return Viewport::NavPreset::Onshape;
  if (name == "Blender") return Viewport::NavPreset::Blender;
  if (name == "CAD2D") return Viewport::NavPreset::Cad2D;
  return Viewport::NavPreset::Fusion;
}
}  // namespace

class ViewNavigation : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    auto add = [this](const QString& id, const QString& label, const QString& icon, const QKeySequence& key, std::function<void()> fn, bool checkable = false,
                      bool edits = false) {
      CommandInfo info{id, label, icon, key};
      info.checkable = checkable;
      info.editsDocument = edits;  // Home is the document's (a view op): in viewer mode it asks to save as OPAD first
      return services().addCommand(info, std::move(fn));
    };
    m_zoom = add("view.zoomWindow", tr("Zoom window"), "zoomWindow", QKeySequence("Z"), [this] {
      Viewport* v = services().viewport();
      if (m_zoom->isChecked()) v->startZoomWindow();
      else v->cancelZoomWindow();
    }, true);
    add("view.previous", tr("Previous view"), "viewBack", QKeySequence("F5"), [this] { services().viewport()->previousView(); });
    add("view.next", tr("Next view"), "viewForward", QKeySequence("Shift+F5"), [this] { services().viewport()->nextView(); });
    add("view.setHome", tr("Set current view as Home"), "home", QKeySequence(), [this] { setHome(); }, false, true);
    CommandInfo reset{"view.resetHome", tr("Reset Home to the iso view"), "home", QKeySequence()};
    reset.editsDocument = true;
    reset.enabledWhen = [this](const CommandContext& c) { return c.document && !c.viewer && services().viewport()->customHome(); };
    services().addCommand(reset, [this] { resetHome(); });
    add("view.twist", tr("Twist view…"), "twist", QKeySequence(), [this] { twist(); });
    add("view.untwist", tr("Untwist view"), "twist", QKeySequence(), [this] { services().viewport()->twistView(0); });
    m_cad2d = add("nav.cad2d", tr("Navigation: CAD 2D"), "", QKeySequence(), [this] { choosePreset("CAD2D"); }, true);
    m_cad2d->setChecked(QSettings().value("ui/nav").toString() == "CAD2D");
    m_animate = add("view.animate", tr("Animate view changes"), "", QKeySequence(), [this] { services().viewport()->setAnimateViews(m_animate->isChecked()); }, true);
    m_animate->setChecked(QSettings().value("view/animate", true).toBool());
    // UI-45: a heavy model is drawn at a lower resolution, without shadows, while the view moves.
    m_adaptive = add("view.adaptive", tr("Lower quality while navigating"), "", QKeySequence(), [this] { services().viewport()->setAdaptiveQuality(m_adaptive->isChecked()); }, true);
    m_adaptive->setChecked(QSettings().value("view/adaptive", true).toBool());
  }

  void menus(QMenuBar*, const QMap<QString, QMenu*>& menus) override {
    QMenu* view = menus.value("view");
    if (!view) return;
    auto after = [this, view](const QString& id, const QStringList& ids) {  // right after that command, in this order
      QList<QAction*> actions = view->actions();
      const auto at = std::find_if(actions.begin(), actions.end(), [&id](QAction* a) { return a->objectName() == id; });
      QAction* before = at == actions.end() || at + 1 == actions.end() ? nullptr : *(at + 1);
      for (const QString& add : ids) view->insertAction(before, services().action(add));
    };
    after("view.home", {"view.zoomWindow", "view.previous", "view.next", "view.setHome", "view.resetHome"});
    after("view.rollright", {"view.twist", "view.untwist"});
    if (auto* nav = view->findChild<QMenu*>("navigation")) {
      nav->addAction(m_cad2d);
      // Not presets: the two switches follow the presets' submenu in the View menu itself.
      const QList<QAction*> actions = view->actions();
      const qsizetype at = actions.indexOf(nav->menuAction());
      QAction* before = at >= 0 && at + 1 < actions.size() ? actions[at + 1] : nullptr;
      view->insertAction(before, m_animate);
      view->insertAction(before, m_adaptive);
    }
  }

  // Zoom window, Previous and Next view are under Fit's arrow, Set current view as Home and Reset Home under Home's, Hidden
  // edges visible under Hidden line's (UI-48), in the View tabs (MainWindowRibbonTable.cpp): the tabs keep their width.

  void ready() override {
    Viewport* v = services().viewport();
    v->setNavPreset(presetOf(QSettings().value("ui/nav", "Fusion").toString()));  // the saved preset (it was only ticked)
    connect(v, &Viewport::zoomWindowChanged, this, [this](bool on) {
      QSignalBlocker block(m_zoom);
      m_zoom->setChecked(on);
    });
    connect(v, &Viewport::fitRequested, this, [this] { services().action("view.fitall")->trigger(); });
    connect(v, &Viewport::cubeMenuRequested, this, [this](const QPoint& at) { cubeMenu(at); });
    // Another preset chosen: CAD 2D is no longer ticked (MainWindow's own loop ticks only the one chosen).
    for (const QString id : {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"})
      if (QAction* a = services().action(id)) connect(a, &QAction::triggered, this, [this] { m_cad2d->setChecked(false); });
  }

 private:
  QAction* m_zoom = nullptr;
  QAction* m_cad2d = nullptr;
  QAction* m_animate = nullptr;
  QAction* m_adaptive = nullptr;

  void choosePreset(const QString& name) {
    services().viewport()->setNavPreset(presetOf(name));
    QSettings().setValue("ui/nav", name);
    for (const QString id : {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"})
      if (QAction* a = services().action(id)) a->setChecked(false);
    m_cad2d->setChecked(name == "CAD2D");
  }

  // Home is the document's: a view op marked home (an older build lists it as a view named Home), undone like any edit.
  void setHome() {
    services().guarded([this] {
      services().document()->run("view", opad::json{{"home", true}, {"camera", services().viewport()->cameraJson()}});
      services().showMessage(tr("Home (H) is this view now, saved with the document"));
    });
  }

  // Every Home the document has set goes (tombstones, one step); Home is the iso view again. With a design history a
  // tombstone is planned on a worker, as the timeline's Delete is.
  void resetHome() {
    AppDocument* doc = services().document();
    std::vector<opad::json> ops;
    for (const auto& v : doc->scene.views)
      if (v.home) ops.push_back(opad::json{{"op", "delete"}, {"target", v.id}});
    if (ops.empty()) return;
    if (!doc->scene.features.empty() || !doc->scene.sketches.empty()) return services().design()->applyOps(std::move(ops), tr("reset Home"));
    services().guarded([&] { doc->batch(tr("reset Home"), [&] { for (const auto& op : ops) doc->run("delete", opad::json{{"target", op["target"]}}); }); });
  }

  // The twist typed as an angle in the shown unit, counter-clockwise; 0 untwists.
  void twist() {
    Viewport* v = services().viewport();
    bool ok = false;
    const QString text = QInputDialog::getText(services().window(), tr("Twist view"), tr("Turn the view about its axis (counter-clockwise; 0 untwists):"),
                                               QLineEdit::Normal, units::editable(units::Kind::Angle, v->twistAngle()), &ok);
    if (!ok) return;
    const auto degrees = units::parse(units::Kind::Angle, text);
    if (!degrees) return services().showMessage(tr("Not an angle: %1").arg(text));
    v->twistView(*degrees);
  }

  // A right click on the view cube: Home and where it points, framing, history, the standard views, projection, turns.
  void cubeMenu(const QPoint& at) {
    auto* menu = new QMenu(services().window());
    menu->setObjectName("cubeMenu");
    menu->setAttribute(Qt::WA_DeleteOnClose);
    auto put = [this, menu](const QStringList& ids) {
      for (const QString& id : ids)
        if (id == "-") menu->addSeparator();
        else if (QAction* a = services().action(id)) menu->addAction(a);
    };
    services().updateCommands();  // Reset Home: off while Home is the default
    put({"view.home", "view.setHome", "view.resetHome", "-", "view.fitall", "view.zoomWindow", "view.previous", "view.next", "-", "view.top", "view.front",
         "view.right", "view.iso", "view.bottom", "view.back", "view.left", "-", "view.ortho", "view.rollleft", "view.rollright"});
    menu->popup(at);
  }
};
OPAD_AREA(ViewNavigation)
