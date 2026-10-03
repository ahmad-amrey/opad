#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "Jobs.hpp"
#include "TimelineWidget.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QTimer>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_CLIPBOARD=<prefix> (TODO 11 UI-129): Copy, Cut and Paste (the Edit commands Ctrl+C, Ctrl+X, Ctrl+V) on a
// 40 x 25 rectangle held by its typed sizes and a circle of diameter 10. Copy puts application/x-opad+json on the system
// clipboard (the curves, their points, the constraints among them, the base point at the lower left); Paste reads it on a
// worker, then the copy follows the pointer (its outline drawn by its base point, X and Y boxes) and a click places it with
// new ids and the constraints kept, selected, in one undo step (undo, redo); a second paste typed @0,-40 lands 40 below
// where it was copied; Cut takes the selection to the clipboard in one undo step; Copy with base point takes the clicked
// corner as the base; after Finish sketch, Ctrl+C on the timeline copies the marker's op id, Ctrl+V outside a sketch
// changes nothing. <prefix>.png: the paste following the pointer.
void SketchEditor::benchClipboard() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_CLIPBOARD");
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: clipboard: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  QWidget* window = m_viewport->window();
  auto action = [window](const char* id) { return window->findChild<QAction*>(id); };
  if (!action("edit.copy") || !action("edit.cut") || !action("edit.paste") || !action("sketch.copybase")) {
    check(false, "the Copy, Cut, Paste and Copy with base point commands exist");
    return QCoreApplication::exit(2);
  }
  check(action("edit.copy")->shortcut() == QKeySequence::Copy && action("edit.cut")->shortcut() == QKeySequence::Cut && action("edit.paste")->shortcut() == QKeySequence::Paste,
        "Ctrl+C, Ctrl+X and Ctrl+V are theirs");
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {60, 10, 100}}, {"target", {60, 10, 0}}, {"up", {0, 1, 0}}, {"scale", 140}, {"projection", "orthographic"}, {"absolute", true}});
  setTool("rect");
  placePrecise("0", "0", 0);
  enter("@40,25");
  setTool("circle");
  enter("60,12");
  enter("10");
  setTool("select");
  auto equal = [](double a, double b) { return std::abs(a - b) < 1e-6; };
  auto curves = [this] {
    std::vector<int> out;
    for (const auto& e : m_sk.entities) out.push_back(e.id);
    return out;
  };
  auto lowest = [this](const std::vector<int>& ids, double& u, double& v) {
    u = v = 1e300;
    for (int id : ids)
      if (const SkEntity* e = m_sk.entity(id))
        for (const auto& [x, y] : sampled(*e)) u = std::min(u, x), v = std::min(v, y);
  };
  auto dimensions = [this] { return std::count_if(m_sk.constraints.begin(), m_sk.constraints.end(), [](const SkConstraint& c) { return c.is_dimension(); }); };
  auto clip = [] {
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    return mime && mime->hasFormat(kClipMime) ? opad::json::parse(mime->data(kClipMime).toStdString()) : opad::json();
  };
  const auto original = curves();
  check(original.size() == 5 && dimensions() == 3, "a rectangle with its width and height and a circle with its diameter");
  m_sel = original;
  action("edit.copy")->trigger();
  const opad::json copied = clip();
  check(copied.is_object() && copied.value("format", "") == "opad.sketch.clipboard" && copied.at("sketch").at("entities").size() == 5 &&
            equal(copied.at("base")[0].get<double>(), 0) && equal(copied.at("base")[1].get<double>(), 0),
        "Copy puts the five curves on the system clipboard as application/x-opad+json, based at their lower left");
  check(copied.is_object() && copied.at("sketch").at("constraints").size() >= 7 && m_sk.entities.size() == 5, "with the constraints among them; the sketch stays as it was");
  const size_t steps = m_undo.size();
  action("edit.paste")->trigger();

  auto phase = std::make_shared<int>(0), ticks = std::make_shared<int>(0);
  auto made = std::make_shared<std::vector<int>>();
  auto* timer = new QTimer(this);
  timer->setInterval(50);
  connect(timer, &QTimer::timeout, this, [=] {
    if (++*ticks > 600) {
      check(false, QString("phase %1 in time").arg(*phase));
      timer->stop();
      return QCoreApplication::exit(2);
    }
    if (m_editJob || m_doc->designBusy) return;
    switch (*phase) {
      case 0: {  // the paste waits for the clipboard's worker
        if (!pasting()) return;
        sketchMove(100, 50, Qt::AltModifier, false);
        check(m_tool == "paste" && m_clip->curves == 5 && transientSolid(m_viewport->tokens().hov) >= 12, "Paste carries the copy's outline at the pointer");
        check(m_input->count() == 2 && m_input->key(0) == "x" && m_input->key(1) == "y" && m_sk.entities.size() == 5, "with X and Y boxes; nothing is added before the click");
        m_viewport->grabImage().save(prefix + ".png");
        sketchPress(100, 50, Qt::AltModifier);
        sketchRelease(100, 50, Qt::AltModifier);
        *made = m_sel;
        double u = 0, v = 0;
        lowest(*made, u, v);
        check(m_sk.entities.size() == 10 && made->size() == 5 && m_tool == "select" && equal(u, 100) && equal(v, 50), "a click places it there, selected");
        check(dimensions() == 6 && m_solved.converged && std::none_of(made->begin(), made->end(), [&](int id) { return std::count(original.begin(), original.end(), id); }),
              "with new ids and its dimensions");
        check(m_undo.size() == steps + 1, "in one undo step");
        undo();
        check(m_sk.entities.size() == 5, "Undo takes it back");
        redo();
        check(m_sk.entities.size() == 10, "Redo puts it back");
        action("edit.paste")->trigger();
        ++*phase;
        break;
      }
      case 1: {
        if (!pasting()) return;
        check(enter("@0,-40").isEmpty(), "typed @0,-40 (the command line's entry, through the boxes)");
        double u = 0, v = 0;
        lowest(m_sel, u, v);
        check(m_sk.entities.size() == 15 && equal(u, 0) && equal(v, -40), "lands the copy 40 below where it was copied");
        const size_t before = m_undo.size();
        action("edit.cut")->trigger();
        check(m_sk.entities.size() == 10 && m_undo.size() == before + 1 && clip().at("sketch").at("entities").size() == 5, "Cut takes the selection to the clipboard in one undo step");
        m_sel = {original[0], original[1], original[2], original[3]};
        action("sketch.copybase")->trigger();
        check(m_tool == "copybase", "Copy with base point asks for the base point");
        sketchMove(40, 25, Qt::NoModifier, false);
        sketchPress(40, 25, Qt::NoModifier);
        sketchRelease(40, 25, Qt::NoModifier);
        const auto based = clip();
        check(m_tool == "select" && m_sel.size() == 4 && based.at("sketch").at("entities").size() == 4 && equal(based.at("base")[0].get<double>(), 40) &&
                  equal(based.at("base")[1].get<double>(), 25),
              "a click on the corner copies the rectangle about it, the selection kept");
        for (auto* panel : window->findChildren<SketchPanel*>()) QMetaObject::invokeMethod(panel, "finishRequested");
        ++*phase;
        break;
      }
      case 2: {
        if (m_active) return;
        auto* timeline = window->findChild<TimelineWidget*>();
        const std::string op = m_doc->scene.sketches.empty() ? std::string() : m_doc->scene.sketches.back().id;
        check(timeline && !op.empty(), "the sketch is finished");
        if (!timeline || op.empty()) break;
        timeline->setCurrentOp(op);
        timeline->setFocus();
        action("edit.copy")->trigger();
        check(QApplication::clipboard()->text() == QString::fromStdString(op), "Ctrl+C on the timeline copies the marker's op id");
        const size_t ops = m_doc->doc.ops.size();
        action("edit.paste")->trigger();
        check(m_doc->doc.ops.size() == ops && !m_active, "Ctrl+V outside a sketch changes nothing");
        timer->stop();
        QCoreApplication::exit(*ok ? 0 : 2);
        return;
      }
    }
  });
  timer->start();
}
