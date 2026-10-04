// The workspaces' promises (UI-104): a Design command started from Review switches to Design; a sketch puts its Sketch
// tab first in Design with Finish sketch as the primary button, keeps Design while it is open and goes back to where it
// was started from; Interference is one command whose Keep as check stores the Interference check feature in Design; a
// drawing file viewed comes into Drafting and the next document goes back. Case in tools/bench_cases/core.py.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

#include <functional>
#include <memory>
#include <vector>

#include "BenchRegistry.hpp"
#include "CheckPanel.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"

namespace {
struct Step {
  std::function<bool()> ready;
  std::function<void(bool)> act;
  int ms = 20000;
};

void runSteps(QObject* context, std::shared_ptr<std::vector<Step>> steps, size_t i, std::function<void()> finish) {
  if (i >= steps->size()) return finish();
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [=] {
    const Step& step = (*steps)[i];
    const bool ok = !step.ready || step.ready();
    if (!ok && clock->elapsed() < step.ms) return;
    timer->stop();
    timer->deleteLater();
    step.act(ok);
    runSteps(context, steps, i + 1, finish);
  });
  timer->start(50);
}
}  // namespace

// OPAD_BENCH_WORKSPACES=<drawing file> on an editable document with a body.
OPAD_BENCH(OPAD_BENCH_WORKSPACES, workspaces) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: workspaces: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  const QString box = w.m_doc->path(), drawing = value;
  DesignController* design = w.m_design;
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](std::function<bool()> ready, std::function<void(bool)> act) { steps->push_back({std::move(ready), std::move(act)}); };
  auto kind = [design] { return design->featureActive() && design->featurePanel()->spec() ? QString::fromStdString(design->featurePanel()->spec()->kind) : QString(); };
  auto pickXY = [design] {
    design->planePicker()->choose({{"base", "xy"}});
    QTimer::singleShot(300, design, [design] { if (design->planePicker()->positioning()) design->planePicker()->apply(); });
  };
  add({}, [=, &w](bool) {
    w.setWorkspace("review");
    require(w.workspaceId() == "review" && !w.m_doc->browse && !w.m_doc->scene.all_bodies().empty(), "an editable document with a body, in Review");
    w.action("design.extrude")->trigger();  // E
  });
  add([=] { return design->featureActive(); }, [=, &w](bool open) {
    require(open && kind() == "extrude" && w.workspaceId() == "design", "Extrude (E) started from Review switches to Design: " + w.workspaceId());
    design->escape();
    w.setWorkspace("review");
    w.action("view.fit")->trigger();
    require(w.workspaceId() == "review", "Fit stays in Review");
    w.action("design.sketch")->trigger();
  });
  add([=] { return design->pickingPlane(); }, [=, &w](bool picking) {
    require(picking && w.workspaceId() == "design", "New sketch from Review: Design, a plane to pick");
    pickXY();
  });
  add([=] { return design->sketchActive(); }, [=, &w](bool sketching) {
    QToolButton* finish = nullptr;
    if (RibbonPage* page = w.m_ribbon->page("design.sketch"))
      for (QToolButton* b : page->findChildren<QToolButton*>())
        if (b->defaultAction() == w.action("sketch.finish")) finish = b;
    const QStringList tabs = w.m_ribbon->tabIds();
    require(sketching && tabs.value(0) == "design.sketch" && tabs.mid(1) == QStringList({"design.solid", "design.assemble", "design.construct", "design.inspect", "design.insert", "design.view"}) &&
                w.m_ribbon->currentPage() == w.m_ribbon->page("design.sketch"),
            "sketching: the Sketch tab first and current, the Design tabs after it: " + tabs.join(' '));
    RibbonPage* page = w.m_ribbon->page("design.sketch");
    const bool last = finish && page && page->groups().last()->buttons().contains(finish);
    require(finish && finish->property("ribbonPrimary").toBool() && finish->toolButtonStyle() == Qt::ToolButtonTextUnderIcon && last && finish->isEnabled(),
            "Finish sketch: the primary button, large, at the end of the tab");
    require(w.m_sketchMenu && w.m_sketchMenu->menuAction()->isVisible(), "the menu bar's Sketch menu is shown");
    w.setWorkspace("review");
    require(w.workspaceId() == "design" && w.m_ribbon->tabIds().value(0) == "design.sketch", "while sketching another workspace is refused");
    w.action("sketch.cancel")->trigger();
  });
  add([=] { return !design->sketchActive(); }, [=, &w](bool left) {
    require(left && !w.m_ribbon->contextualTabShown("design.sketch") && w.workspaceId() == "design" && !w.m_sketchMenu->menuAction()->isVisible(),
            "cancelled: the Sketch tab and menu go, Design stays (the command switched to it)");
    w.setWorkspace("review");
    design->startSketch();  // not a command: as an area or a lesson starts one
  });
  add([=] { return design->pickingPlane(); }, [=](bool) { pickXY(); });
  add([=] { return design->sketchActive(); }, [=, &w](bool sketching) {
    require(sketching && w.workspaceId() == "design" && w.m_ribbon->tabIds().value(0) == "design.sketch", "a sketch started in Review shows in Design");
    w.action("sketch.cancel")->trigger();
  });
  add([=] { return !design->sketchActive(); }, [=, &w](bool left) {
    require(left && w.workspaceId() == "review", "left again: back in Review, where it was started: " + w.workspaceId());
    w.startCheck(false);  // Inspect > Interference
  });
  add([=, &w] { return w.m_checks->keepButton()->isVisible(); }, [=, &w](bool shown) {
    require(shown && w.workspaceId() == "review", "Interference checked in Review: Keep as check offered");
    w.m_checks->keepButton()->click();
  });
  add([=] { return design->featureActive(); }, [=, &w](bool open) {
    require(open && kind() == "interference" && w.workspaceId() == "design", "Keep as check: the Interference check feature in Design: " + kind());
    const CommandInfo* stored = w.m_commands.find("design.interference");
    require(stored && stored->workspaces.isEmpty() && stored->menuPath.isEmpty() && w.m_commands.find("inspect.interference")->workspaces.contains("design"),
            "one Interference command on the ribbon and in the menus");
    design->escape();
    w.setWorkspace("review");
    w.openPath(drawing);
  });
  add([=, &w] { return w.m_doc->browse && !w.m_doc->loading && w.viewingDrawing(); }, [=, &w](bool viewing) {
    require(viewing && w.workspaceId() == "drafting" && w.m_ribbon->tabIds() == QStringList({"drafting.home", "drafting.annotate", "drafting.view", "drafting.output"}),
            "a drawing file viewed comes into Drafting: " + w.workspaceId() + " " + w.m_ribbon->tabIds().join(' '));
    w.openPath(box);
  });
  add([=, &w] { return !w.m_doc->browse && !w.m_doc->loading && w.m_doc->path() == box; }, [=, &w](bool back) {
    require(back && w.workspaceId() == "review", "the next document goes back to Review: " + w.workspaceId());
  });
  runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
