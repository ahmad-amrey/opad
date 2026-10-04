// Navigation staples as commands (UI-47): zoom window (Z), previous and next view (F5, Shift+F5), Home set to the current
// view and back, the view twist, the CAD 2D mouse preset, a middle double click fitting everything, the view cube's menu.
// The view side is Viewport (ViewportNavigation.cpp); this area places the commands in the View menus and ribbon tabs.
#include <QActionGroup>
#include <QInputDialog>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>

#include "AreaController.hpp"
#include "Commands.hpp"
#include "Icons.hpp"
#include "Ribbon.hpp"
#include "Units.hpp"
#include "Viewport.hpp"

OPAD_ICON_TABLE(viewnav,
                {"zoomWindow", R"(<rect x="3" y="3" width="11" height="8"/><circle cx="15.5" cy="15.5" r="3.5"/><path d="M18 18l3 3"/>)"},
                {"viewBack", R"(<path d="M9 6l-6 6 6 6"/><path d="M3 12h11a6 6 0 0 1 6 6"/>)"},
                {"viewForward", R"(<path d="M15 6l6 6-6 6"/><path d="M21 12H10a6 6 0 0 0-6 6"/>)"},
                {"twist", R"(<path d="M12 8l4 4-4 4-4-4z"/><path d="M4.5 12a7.5 7.5 0 0 1 13.3-4.7"/><path d="M19 3v5h-5"/>)"});

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
    auto add = [this](const QString& id, const QString& label, const QString& icon, const QKeySequence& key, std::function<void()> fn, bool checkable = false) {
      CommandInfo info{id, label, icon, key};
      info.checkable = checkable;
      return services().addCommand(info, std::move(fn));
    };
    m_zoom = add("view.zoomWindow", tr("Zoom window"), "zoomWindow", QKeySequence("Z"), [this] {
      Viewport* v = services().viewport();
      if (m_zoom->isChecked()) v->startZoomWindow();
      else v->cancelZoomWindow();
    }, true);
    add("view.previous", tr("Previous view"), "viewBack", QKeySequence("F5"), [this] { services().viewport()->previousView(); });
    add("view.next", tr("Next view"), "viewForward", QKeySequence("Shift+F5"), [this] { services().viewport()->nextView(); });
    add("view.setHome", tr("Set current view as Home"), "home", QKeySequence(), [this] { services().viewport()->setHomeView(); });
    add("view.resetHome", tr("Reset Home to the iso view"), "home", QKeySequence(), [this] { services().viewport()->resetHomeView(); });
    add("view.twist", tr("Twist view…"), "twist", QKeySequence(), [this] { twist(); });
    add("view.untwist", tr("Untwist view"), "twist", QKeySequence(), [this] { services().viewport()->twistView(0); });
    m_cad2d = add("nav.cad2d", tr("Navigation: CAD 2D"), "", QKeySequence(), [this] { choosePreset("CAD2D"); }, true);
    m_cad2d->setChecked(QSettings().value("ui/nav").toString() == "CAD2D");
    m_animate = add("view.animate", tr("Animate view changes"), "", QKeySequence(), [this] { services().viewport()->setAnimateViews(m_animate->isChecked()); }, true);
    m_animate->setChecked(QSettings().value("view/animate", true).toBool());
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
      nav->addSeparator();
      nav->addAction(m_animate);
    }
  }

  // Under Fit's arrow in both View tabs: the tabs keep their width (every group of Review and Design large at 1600 px).
  void ribbon(RibbonLayout& layout) override {
    for (const QString id : {"review.view.navigate", "design.view.navigate"})
      if (RibbonLayout::Group* group = layout.group(id))
        for (RibbonLayout::Item& item : group->items)
          if (item.action && item.action->objectName() == "view.fit")
            for (const QString variant : {"view.fitall", "view.zoomWindow", "view.previous", "view.next"}) item.variants << services().action(variant);
  }

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

  void choosePreset(const QString& name) {
    services().viewport()->setNavPreset(presetOf(name));
    QSettings().setValue("ui/nav", name);
    for (const QString id : {"nav.fusion", "nav.solidworks", "nav.onshape", "nav.blender"})
      if (QAction* a = services().action(id)) a->setChecked(false);
    m_cad2d->setChecked(name == "CAD2D");
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
    put({"view.home", "view.setHome", "view.resetHome", "-", "view.fitall", "view.zoomWindow", "view.previous", "view.next", "-", "view.top", "view.front",
         "view.right", "view.iso", "view.bottom", "view.back", "view.left", "-", "view.ortho", "view.rollleft", "view.rollright"});
    if (QAction* reset = services().action("view.resetHome")) reset->setEnabled(services().viewport()->customHome());
    connect(menu, &QMenu::aboutToHide, this, [this] {
      if (QAction* reset = services().action("view.resetHome")) reset->setEnabled(true);
    });
    menu->popup(at);
  }
};
OPAD_AREA(ViewNavigation)
