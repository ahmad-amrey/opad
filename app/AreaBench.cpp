// The feature-area seams (AreaController.hpp) in the running app: a probe area, made only for OPAD_BENCH_AREAS, records
// every hook; the bench drives the window and checks what reached it. Cases in tools/bench_cases/core.py.
#include <QCoreApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>

#include "AreaController.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"

namespace {
class ProbeArea : public AreaController {
 public:
  using AreaController::AreaController;
  QStringList hooks;  // construction hooks, in call order
  QAction* command = nullptr;
  QLabel* label = nullptr;
  int ran = 0, asked = 0;
  bool veto = false;
  SelectionContext selected, menuSelection;
  QStringList menuEntries;
  std::vector<bool> changes;  // documentChanged(replaced)
  QRect overlays;

  void buildActions() override {
    hooks << "buildActions";
    command = services().addAction("probe.command", "Probe command", "dot", QKeySequence(), [this] { ++ran; });
  }
  void menus(QMenuBar*, const QMap<QString, QMenu*>& byId) override {
    hooks << "menus";
    if (QMenu* tools = byId.value("tools")) tools->addAction(command);
  }
  void ribbon(RibbonLayout& layout) override {
    hooks << "ribbon";
    layout.addGroup("review.view", {command, nullptr});
    layout.addTab("review", "review.probe", "Probe", {{command}, {}});
  }
  void statusWidgets(QStatusBar* bar) override {
    hooks << "statusWidgets";
    label = new QLabel("probe", bar);
    bar->addPermanentWidget(label);
  }
  void ready() override { hooks << "ready"; }
  void contextMenu(const SelectionContext& selection, QMenu& menu) override {
    menuSelection = selection;
    menu.addAction(command);
    menuEntries.clear();
    for (QAction* a : menu.actions()) menuEntries << a->objectName();
    QTimer::singleShot(0, &menu, &QMenu::close);  // the bench's menu shows off screen: close it again
  }
  void selectionChanged(const SelectionContext& selection) override { selected = selection; }
  void positionOverlays(const QRect& viewport) override { overlays = viewport; }
  void documentChanged(bool replaced) override { changes.push_back(replaced); }
  bool maybeClose() override {
    ++asked;
    return !veto;
  }
};

[[maybe_unused]] const bool probeAdded = areas::add("Probe", [](AreaServices& s) -> AreaController* {
  return qEnvironmentVariableIsSet("OPAD_BENCH_AREAS") ? new ProbeArea(s) : nullptr;
});
}  // namespace

// OPAD_BENCH_AREAS=1 on a document with a body: the hooks in order, the command in the Tools menu, the ribbon and the
// status bar, documentChanged on edits and loads, selectionChanged from the browser, the context menu's entry and
// selection, positionOverlays with the viewport, maybeClose keeping the document and then letting it go.
OPAD_BENCH(OPAD_BENCH_AREAS, areas) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: areas: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  ProbeArea* probe = nullptr;
  for (AreaController* area : w.m_areas)
    if (auto* p = dynamic_cast<ProbeArea*>(area)) probe = p;
  require(probe && areas::names().contains("Probe") && areas::clashes().isEmpty() && probe->objectName() == "Probe", "probe area registered and made");
  if (!probe || w.m_doc->scene.all_bodies().empty()) {
    QCoreApplication::exit(2);
    return true;
  }
  require(probe->hooks == QStringList({"buildActions", "menus", "ribbon", "statusWidgets", "ready"}), "construction hooks in order: " + probe->hooks.join(' '));
  bool inTools = false;
  for (QAction* menu : w.menuBar()->actions())
    inTools = inTools || (menu->text() == QObject::tr("&Tools") && menu->menu()->actions().contains(probe->command));
  require(w.action("probe.command") == probe->command && w.m_actions.contains(probe->command) && inTools, "command registered, in the Tools menu");
  int buttons = 0;
  for (QToolButton* b : w.m_ribbon->findChildren<QToolButton*>())
    if (b->defaultAction() == probe->command) ++buttons;
  bool tab = false;
  for (QTabBar* bar : w.m_ribbon->findChildren<QTabBar*>())
    for (int i = 0; i < bar->count(); ++i) tab = tab || bar->tabText(i) == "Probe";
  require(buttons == 2 && tab, QString("ribbon: a group on Review > View and a Probe tab (%1 buttons)").arg(buttons));
  require(probe->label && probe->label->parentWidget() == w.statusBar() && probe->label->isVisibleTo(w.statusBar()), "status widget in the status bar");
  require(!probe->changes.empty() && probe->changes.back(), "documentChanged: the load replaced the document");
  const std::string body = w.m_doc->scene.all_bodies().front();
  w.m_doc->run("appearance", opad::json{{"target", body}, {"visible", false}});
  require(!probe->changes.back(), "documentChanged: an edit is not a replacement");
  w.m_browser->selectIds({body});
  require(probe->selected.ids == std::vector<std::string>{body} && probe->selected.refs.size() == 1 && !probe->selected.sketching, "selectionChanged from the browser");
  w.showContextMenu(w.mapToGlobal(QPoint(200, 200)), {body});
  require(probe->menuSelection.ids == std::vector<std::string>{body} && probe->menuEntries.contains("probe.command") && probe->menuEntries.contains("edit.hide"),
          "context menu: the probe's entry after the built-in ones, for the right object");
  w.positionOverlays();
  require(probe->overlays == QRect(w.m_viewport->mapToGlobal(QPoint(0, 0)), w.m_viewport->size()) && !probe->overlays.isEmpty(), "positionOverlays: the viewport, global");
  probe->command->trigger();
  require(probe->ran == 1, "the command runs");
  const auto generation = w.m_doc->generation;
  const int asked = probe->asked;  // opening the file on the command line asked once already
  probe->veto = true;
  w.action("file.new")->trigger();
  require(probe->asked == asked + 1 && w.m_doc->generation == generation && w.m_doc->scene.all_bodies().size() == 1, "maybeClose false keeps the document");
  probe->veto = false;
  w.action("file.new")->trigger();
  require(probe->asked == asked + 2 && w.m_doc->generation != generation && w.m_doc->scene.all_bodies().empty() && probe->changes.back(), "maybeClose true lets New replace it");
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
