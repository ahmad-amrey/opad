// The shared UI contracts of UI-120 in the running app: the command registry, the ribbon's titled groups and adaptive
// collapse, the panel footer and toasts. Cases in tools/bench_cases/core.py; the offscreen side is tests/test_ui_contracts.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QTimer>

#include <memory>

#include "BenchRegistry.hpp"
#include "DesignPanels.hpp"
#include "PanelFooter.hpp"
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

// Polls `done` every 50 ms until it holds or `ms` have passed, then calls `then` with the outcome.
void waitFor(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(50);
}

// The footer's buttons in reading order (leading side first) and whole: as wide as their labels and keys ask.
bool inReadingOrder(PanelFooter* footer, const QList<QPushButton*>& buttons) {
  const bool rtl = footer->layoutDirection() == Qt::RightToLeft;
  for (int i = 0; i < buttons.size(); ++i) {
    if (buttons[i]->width() < buttons[i]->sizeHint().width()) return false;
    if (i > 0 && (rtl ? buttons[i]->x() >= buttons[i - 1]->x() : buttons[i]->x() <= buttons[i - 1]->x())) return false;
  }
  return true;
}
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

// OPAD_BENCH_FOOTER=1 [OPAD_LANG=ar] on an editable document: the panel footer in the feature panel (Cancel Esc and OK
// Enter at the trailing end: Cancel leaves the feature, OK commits it and closes the panel) and in the guided tools' panel
// (Copy leading, Clear Esc and Pin to document P, which keep the keys with the window: Clear measures again, Pin pins and
// the tool stays).
OPAD_BENCH(OPAD_BENCH_FOOTER, footer) {
  auto require = std::make_shared<Checks>(Checks{"footer"});
  auto finish = [require] { QCoreApplication::exit(require->all ? 0 : 2); };
  const std::string body = w.m_doc->scene.all_bodies().empty() ? std::string() : w.m_doc->scene.all_bodies().front();
  (*require)(!w.m_doc->browse && !body.empty(), "an editable document with a body");
  w.setWorkspace("design");
  w.m_design->startFeature("box");
  FeaturePanel* form = w.m_design->featurePanel();
  PanelFooter* f = form->footer();
  (*require)(w.m_design->featureActive() && w.m_featurePanel->isVisible() && f->isVisibleTo(form) && f->cancelText() == QObject::tr("Cancel") &&
                 PanelFooter::key(f->cancel()) == "Esc" && f->primaryText() == QObject::tr("OK") && PanelFooter::key(f->primary()) == "Enter" &&
                 f->primary()->objectName() == "primary",
             "feature panel: Cancel (Esc) and the primary OK (Enter)");
  (*require)(inReadingOrder(f, {f->cancel(), f->primary()}) && f->geometry().bottom() == form->height() - 1 && f->width() == form->width(),
             QString("feature panel: the footer spans the panel's bottom, buttons whole and in reading order (%1)").arg(f->layoutDirection() == Qt::RightToLeft ? "rtl" : "ltr"));
  const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT");  // <shot>.feature.png, <shot>.tool.png
  if (!shot.isEmpty()) w.m_featurePanel->grab().save(shot + ".feature.png");
  const size_t ops = w.m_doc->doc.ops.size();
  f->cancel()->click();
  (*require)(!w.m_design->featureActive() && !w.m_featurePanel->isVisible() && w.m_doc->doc.ops.size() == ops, "Cancel leaves the feature, nothing is committed");
  w.m_design->startFeature("box");
  f->primary()->click();
  waitFor(&w, [&w, ops] { return !w.m_design->featureActive() && w.m_doc->doc.ops.size() > ops && !w.m_doc->designBusy; }, 20000, [&w, require, finish, ops, body](bool ok) {
    (*require)(ok && !w.m_featurePanel->isVisible(), QString("OK commits the feature and closes the panel (%1 ops more)").arg(w.m_doc->doc.ops.size() - ops));
    w.setWorkspace("review");
    w.startTool("bbox");
    w.m_browser->selectIds({body});
    PanelFooter* t = w.m_toolSteps->footer();
    waitFor(&w, [&w, t] { return t->isVisibleTo(w.m_toolSteps) && t->primary()->isEnabled(); }, 20000, [&w, require, finish, t, body](bool shown) {
      QPushButton* copy = nullptr;
      for (QPushButton* b : t->findChildren<QPushButton*>())
        if (PanelFooter::text(b) == QObject::tr("Copy")) copy = b;
      (*require)(shown && copy && t->cancelText() == QObject::tr("Clear") && PanelFooter::key(t->cancel()) == "Esc" && t->primaryText() == QObject::tr("Pin to document") &&
                     PanelFooter::key(t->primary()) == "P" && t->primary()->focusPolicy() == Qt::NoFocus && t->cancel()->focusPolicy() == Qt::NoFocus,
                 "tool panel: Copy, Clear (Esc) and Pin to document (P) once there is a result; Esc, P and Enter stay with the window");
      (*require)(copy && inReadingOrder(t, {copy, t->cancel(), t->primary()}), "tool panel: Copy on the leading side, Pin at the trailing end");
      if (const QString shot = qEnvironmentVariable("OPAD_BENCH_UISHOT"); !shot.isEmpty()) w.m_toolPanel->grab().save(shot + ".tool.png");
      t->cancel()->click();  // Esc: the result goes, the tool measures again
      waitFor(&w, [&w, t] { return !t->isVisibleTo(w.m_toolSteps) && w.m_lastMeasure.is_null(); }, 10000, [&w, require, finish, t, body](bool cleared) {
        (*require)(cleared && w.m_tool.id == "bbox", "Clear drops the result and the tool waits for a pick");
        w.m_browser->selectIds({body});
        waitFor(&w, [&w, t] { return t->isVisibleTo(w.m_toolSteps) && t->primary()->isEnabled(); }, 20000, [&w, require, finish, t](bool again) {
          const size_t before = w.m_doc->doc.ops.size();
          t->primary()->click();
          (*require)(again && w.m_doc->doc.ops.size() == before + 1 && w.m_doc->doc.ops.back().type == "measurement" && w.m_tool.id == "bbox",
                     "Pin to document pins the measurement and the tool stays");
          w.cancelTool();
          finish();
        });
      });
    });
  });
  return true;
}
