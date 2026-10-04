// Bench of the states that are not told by colour alone (UI-124): the sketch's free points and degrees of freedom, the
// timeline's suppressed, failed and unresolved markers. The cues are SketchEditor's, SketchPanel's and TimelineWidget's.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "SketchEditor.hpp"

#include <QApplication>
#include <QLabel>
#include <QTimer>

// OPAD_BENCH_CUES=<prefix> (a box and the sketch "Plate", a rectangle held level and plumb only): suppressed, the box's
// marker says so to a screen reader and is struck through (<prefix>.suppressed.png); in the sketch every point that can
// still move is a ring without its dot, the panel counts the degrees of freedom; fixed, no ring is left and the panel
// says the sketch is fully defined (<prefix>.sketch.png).
OPAD_BENCH(OPAD_BENCH_CUES, cues) {
  const QString prefix = value;
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: cues: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  struct Step { int delay; std::function<void()> fn; std::function<bool()> until; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn, std::function<bool()> until = {}) { steps->push_back({delay, std::move(fn), std::move(until)}); };
  auto idle = [&w] { return !w.m_jobs->busy() && !w.m_design->busy(); };
  auto opOf = [&w](const char* type) {
    for (const auto& op : w.m_doc->doc.ops)
      if (op.type == type) return op.id;
    return std::string();
  };
  const std::string feature = opOf("feature"), sketch = opOf("sketch");
  auto suppressed = [&w, feature] { const opad::Feature* f = w.m_doc->scene.feature(feature); return f && f->suppressed; };
  auto panelSays = [&w](const QString& words) {
    for (QLabel* l : w.findChildren<QLabel*>())
      if (l->text().contains(words)) return true;
    for (QWidget* top : QApplication::topLevelWidgets())
      for (QLabel* l : top->findChildren<QLabel*>())
        if (l->text().contains(words)) return true;
    return false;
  };
  add(600, [=, &w] { w.m_design->setSuppressed(feature, true); }, idle);
  add(200, [=, &w] {
    int at = -1;
    for (int i = 0; i < w.m_timeline->markerCount(); ++i)
      if (w.m_timeline->markerOp(i)->id == feature) at = i;
    check(at >= 0 && w.m_timeline->markerState(at).contains(QCoreApplication::translate("TimelineWidget", "suppressed")), "a suppressed feature's marker says so (" + (at >= 0 ? w.m_timeline->markerState(at) : QString()) + ")");
    w.m_timeline->grab().save(prefix + ".suppressed.png");
    w.m_design->setSuppressed(feature, false);
  }, [=] { return idle() && suppressed(); });
  add(200, [=, &w] { w.m_design->editOp(sketch); }, [=] { return idle() && !suppressed(); });
  add(300, [=, &w] {
    SketchEditor* editor = w.m_design->sketch();
    const auto drawn = editor->drawn();
    const int dof = editor->dof();
    check(dof > 0 && drawn["rings"].size() == 4 && drawn["points"].isEmpty(), QString("the rectangle's four free points are rings without their dot (%1 rings, %2 dots, %3 degrees of freedom)")
                                                                                  .arg(drawn["rings"].size()).arg(drawn["points"].size()).arg(dof));
    check(panelSays(QCoreApplication::translate("SketchPanel", " · %1 degrees of freedom").arg(dof)), "the sketch panel counts them");
    editor->selectAll();
    editor->setTool("c:fix");
  }, [&w] { return w.m_design->sketchActive() && !w.m_design->sketch()->busy(); });
  add(300, [=, &w] {
    SketchEditor* editor = w.m_design->sketch();
    const auto drawn = editor->drawn();
    check(editor->dof() == 0 && drawn["rings"].isEmpty() && drawn["points"].size() == 4, QString("fixed: no ring is left (%1 rings, %2 dots)").arg(drawn["rings"].size()).arg(drawn["points"].size()));
    check(panelSays(QCoreApplication::translate("SketchPanel", " · fully defined")), "the sketch panel says it is fully defined");
    w.m_viewport->grabImage().save(prefix + ".sketch.png");
    trace::log(QString("bench: cues: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  }, [&w] { return w.m_design->sketchActive() && !w.m_design->sketch()->busy(); });
  auto next = std::make_shared<std::function<void(size_t, int)>>();
  *next = [&w, steps, next, check](size_t i, int waited) {
    if (i >= steps->size()) return;
    QTimer::singleShot(waited ? 50 : (*steps)[i].delay, &w, [steps, next, check, i, waited] {
      const Step& s = (*steps)[i];
      if (s.until && !s.until() && waited < 300) return (*next)(i, waited + 1);
      try { s.fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1, 0);
    });
  };
  (*next)(0, 0);
  return true;
}
