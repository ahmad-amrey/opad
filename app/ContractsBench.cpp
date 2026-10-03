// The shared UI contracts of UI-120 in the running app: the command registry, the ribbon's titled groups and adaptive
// collapse, the panel footer and toasts. Cases in tools/bench_cases/core.py; the offscreen side is tests/test_ui_contracts.
#include <QCoreApplication>
#include <QMenu>

#include "BenchRegistry.hpp"
#include "CommandPalette.hpp"
#include "MainWindow.hpp"

namespace {
struct Checks {
  QString bench;
  bool all = true;
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: %1: %2 %3").arg(bench, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  }
};
}  // namespace

// OPAD_BENCH_COMMANDS=1 on a STEP file in viewer mode: every command of the window has its record, in command order, with
// group, menu path, workspaces and editsDocument; an area's command registered with its whole record is enabled by its
// enabledWhen as the selection moves, and as an edit it asks to save the viewed file first instead of running.
OPAD_BENCH(OPAD_BENCH_COMMANDS, commands) {
  Checks require{"commands"};
  const CommandRegistry& registry = w.m_commands;
  QStringList names;
  for (QAction* a : w.m_actions) names << a->objectName();
  require(registry.ids() == names && registry.actions() == w.m_actions && registry.clashes().isEmpty(),
          QString("every command has one record, in command order (%1)").arg(names.size()));
  bool built = true;
  for (const QString& id : names) built = built && registry.editsDocument(id) == MainWindow::isEditAction(id) && !registry.find(id)->group.isEmpty();
  require(built, "built-in records: a group each, editsDocument as isEditAction");
  const CommandInfo* extrude = registry.find("design.extrude");
  require(extrude && extrude->group == commands::defaultGroup("design.extrude") && extrude->menuPath == "design/create" &&
              extrude->workspaces == QStringList{"design"} && opGroup(w.action("design.extrude")) == extrude->group,
          "design.extrude: group " + (extrude ? extrude->group + ", menu " + extrude->menuPath + ", workspaces " + extrude->workspaces.join(' ') : QString("none")));
  const CommandInfo* fit = registry.find("view.fit");
  require(fit && fit->workspaces == QStringList({"review", "design"}) && fit->menuPath == "view" && registry.find("nav.fusion")->menuPath == "view/navigation" &&
              registry.find("sketch.line")->workspaces == QStringList{"sketch"} && registry.inWorkspace("review").contains("inspect.distance") &&
              !registry.inWorkspace("review").contains("design.extrude"),
          "workspaces from the ribbon, menu paths from the menu bar");
  // An area's command with its whole record.
  require(w.m_doc->browse && !w.m_doc->scene.all_bodies().empty(), "a STEP file in viewer mode");
  static int edits = 0, looks = 0;
  CommandInfo edit;
  edit.id = "bench.edit";
  edit.label = "Bench edit";
  edit.group = "Bench";
  edit.keywords = {"probe"};
  edit.editsDocument = true;
  edit.enabledWhen = [](const CommandContext& c) { return c.document && !c.selection.empty(); };
  QAction* editAction = w.m_areaServices.addCommand(edit, [] { ++edits; });
  CommandInfo look = edit;
  look.id = "bench.look";
  look.editsDocument = false;
  QAction* lookAction = w.m_areaServices.addCommand(look, [] { ++looks; });
  w.m_areaServices.updateCommands();
  require(w.action("bench.edit") == editAction && registry.find("bench.edit")->group == "Bench" && registry.inGroup("Bench").size() == 2 &&
              w.m_areaServices.commands().editsDocument("bench.edit") && !editAction->isEnabled(),
          "an area's command: registered with its record, disabled with nothing selected");
  const std::string body = w.m_doc->scene.all_bodies().front();
  w.m_browser->selectIds({body});
  require(editAction->isEnabled() && lookAction->isEnabled(), "enabledWhen follows the selection");
  editAction->trigger();  // viewer mode: "Save first to edit" (dismissed by the bench), the command does not run
  lookAction->trigger();
  require(edits == 0 && looks == 1, QString("viewer mode: the edit asked to save first (ran %1), the other ran (%2)").arg(edits).arg(looks));
  w.m_browser->selectIds({});
  require(!editAction->isEnabled(), "a cleared selection disables it again");
  QCoreApplication::exit(require.all ? 0 : 2);
  return true;
}
