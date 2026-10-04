// The workspaces' promises (UI-104): a Design command started from Review switches to Design; a sketch puts its Sketch
// tab first in Design with Finish sketch as the primary button, keeps Design while it is open and goes back to where it
// was started from; Interference is one command whose Keep as check stores the Interference check feature in Design; a
// drawing file viewed comes into Drafting, where Draw on drawing (after Edit unsaved copy) opens a sketch on the drawing's
// plane with its Sketch tab first in Drafting and Finish keeps Drafting; the next document goes back. Suppress and Roll
// back to here act on the timeline's marker. Case in tools/bench_cases/core.py.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "BenchRegistry.hpp"
#include "CheckPanel.hpp"
#include "DesignController.hpp"
#include "DesignPanels.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"
#include "SketchEditor.hpp"
#include "TimelineWidget.hpp"
#include "opad/design/drawing_sketch.hpp"

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
  static bool started = false;  // runBench comes back after every load (the drawing, the box again): the steps run once
  if (std::exchange(started, true)) return true;
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
  // Solid > History's Suppress: the marker selected on the timeline (here the box), from Review into Design; again: back.
  auto boxOp = std::make_shared<std::string>();
  for (const auto& op : w.m_doc->doc.ops)
    if (op.type == "feature" && boxOp->empty()) *boxOp = op.id;
  auto suppressed = [&w, boxOp] { const opad::Feature* f = w.m_doc->scene.feature(*boxOp); return f && f->suppressed; };
  add({}, [=, &w](bool) {
    w.setWorkspace("review");
    w.m_timeline->setCurrentOp(*boxOp);
    w.action("design.suppress")->trigger();
  });
  add([=] { return !design->busy() && suppressed(); }, [=, &w](bool done) {
    require(done && w.workspaceId() == "design", "Suppress from Review: the timeline's selected feature suppressed, in Design");
    w.action("design.suppress")->trigger();
  });
  add([=] { return !design->busy() && !suppressed(); }, [=, &w](bool back) {
    require(back, "Suppress again: the feature is back");
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
  const QStringList draftingTabs{"drafting.home", "drafting.annotate", "drafting.view", "drafting.output"};
  add([=, &w] { return w.m_doc->browse && !w.m_doc->loading && w.viewingDrawing(); }, [=, &w](bool viewing) {
    require(viewing && w.workspaceId() == "drafting" && w.m_ribbon->tabIds() == draftingTabs,
            "a drawing file viewed comes into Drafting: " + w.workspaceId() + " " + w.m_ribbon->tabIds().join(' '));
    const CommandInfo* draw = w.m_commands.find("design.drawOnDrawing");
    require(draw && draw->editsDocument && draw->workspaces == QStringList{"drafting"} && w.action("design.drawOnDrawing")->isEnabled(),
            "Draw on drawing in Drafting, offered on the viewed file (it asks for an editable copy first)");
    w.makeEditable({}, [&w] { w.action("design.drawOnDrawing")->trigger(); });  // the question's Edit unsaved copy, then the command again
  });
  auto drawingFrame = [&w] {
    std::vector<opad::design::DrawingLayer> layers;
    for (const auto& id : w.m_doc->scene.all_bodies())
      if (w.m_doc->scene.node(id)->representation == "drawing2d") layers.push_back({id, false});
    return opad::design::drawing_frame(w.m_doc->scene, layers);
  };
  auto same = [](const opad::Frame& a, const opad::Frame& b) {
    auto equal = [](const opad::Vec3& p, const opad::Vec3& q) { return std::abs(p[0] - q[0]) + std::abs(p[1] - q[1]) + std::abs(p[2] - q[2]) < 1e-9; };
    return equal(a.origin, b.origin) && equal(a.x, b.x) && equal(a.y, b.y);
  };
  add([=] { return design->sketchActive(); }, [=, &w](bool sketching) {
    const QStringList tabs = w.m_ribbon->tabIds();
    require(sketching && !w.m_doc->browse && w.workspaceId() == "drafting" && tabs.value(0) == "drafting.sketch" && tabs.mid(1) == draftingTabs &&
                w.m_ribbon->currentPage() == w.m_ribbon->page("drafting.sketch") && !w.m_ribbon->contextualTabShown("design.sketch"),
            "Draw on drawing: an editable copy, a sketch in Drafting with its Sketch tab first and current: " + w.workspaceId() + " " + tabs.join(' '));
    require(sketching && same(design->sketch()->frame(), drawingFrame()), "the sketch lies on the drawing's own plane and origin");
    w.setWorkspace("review");
    require(w.workspaceId() == "drafting" && w.m_ribbon->tabIds().value(0) == "drafting.sketch", "while it is open another workspace is refused");
    SketchEditor* sketch = design->sketch();
    sketch->setTool("line");
    sketch->enter("0,0");
    sketch->enter("@20,0");
    sketch->done();
    require(!sketch->empty(), "a line drawn on it (typed points)");
    w.action("sketch.finish")->trigger();
  });
  add([=, &w] { return !design->sketchActive() && !design->busy() && !w.m_doc->scene.sketches.empty(); }, [=, &w](bool finished) {
    require(finished && w.workspaceId() == "drafting" && w.m_ribbon->tabIds() == draftingTabs && w.m_doc->scene.sketches.size() == 1 &&
                same(w.m_doc->scene.sketches.back().frame, drawingFrame()),
            "Finish: the sketch is in the document on the drawing's plane, Drafting stays with its own tabs: " + w.m_ribbon->tabIds().join(' '));
    // Roll back to here on the drawing's marker: the model as it was before the sketch.
    std::string drawingOp;
    for (const auto& op : w.m_doc->doc.ops)
      if (op.type == "import" && drawingOp.empty()) drawingOp = op.id;
    const std::string sketchOp = w.m_doc->scene.sketches.empty() ? std::string() : w.m_doc->scene.sketches.back().id;
    w.m_timeline->setCurrentOp(drawingOp);
    w.action("timeline.rollBack")->trigger();
    require(!drawingOp.empty() && w.m_doc->rolledBack() && w.m_doc->rollback() == sketchOp && w.m_doc->scene.sketches.empty(),
            "Roll back to here on the drawing's marker: the model stops before the sketch");
    w.action("timeline.rollForward")->trigger();
    require(!w.m_doc->rolledBack() && w.m_doc->scene.sketches.size() == 1, "rolled forward: the sketch is back");
    w.openPath(box);
  });
  add([=, &w] { return !w.m_doc->browse && !w.m_doc->loading && w.m_doc->path() == box; }, [=, &w](bool back) {
    require(back && w.workspaceId() == "review", "the next document goes back to Review: " + w.workspaceId());
  });
  runSteps(&w, steps, 0, [all] { QCoreApplication::exit(*all ? 0 : 2); });
  return true;
}
