// OPAD_BENCH_PERF=<prefix> (UI-11): the evaluation's latency budgets on a large document. Cases in
// tools/bench_cases/viewer.py: 1,000 boxes made for the run (CI) and the Engine beside the repository (its hidden root
// shown in memory). Each step (a hide, its undo and redo, Hide others and its undo, select all, a filter switch there and
// back, a new document) is timed from the command until everything it started has settled, through the watchdog
// (OPAD_TRACE_STALL_MS, default 50): neither the command itself nor any stall after it may take longer than
// OPAD_BENCH_PERF_BUDGET ms (default 150) of UI-thread CPU time, nor 4x that in wall time: other builds and benches share
// the machine, and a step waiting for a core is not the step's work, while a wait of 600 ms still fails. A round with a
// step over budget runs once more on the file reopened, and that round counts. The trace ends with the stall histogram.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>

#include <algorithm>
#include <functional>

#include "BenchRegistry.hpp"
#include "BrowserPanel.hpp"
#include "MainWindow.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return done();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_PERF, perf) {
  // A round with a step over budget is run once more on the file reopened (a second invocation, once the load is done),
  // and only that round counts. A real regression fails both.
  static int round = 0;
  ++round;
  bool all = true;
  QStringList over;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: perf: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const auto finish = [&all] {
    trace::log("bench: perf: " + trace::stallHistogram());
    QCoreApplication::exit(all ? 0 : 2);
    return true;
  };
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  const QString path = doc->path();
  const int budget = qEnvironmentVariableIntValue("OPAD_BENCH_PERF_BUDGET") > 0 ? qEnvironmentVariableIntValue("OPAD_BENCH_PERF_BUDGET") : 150;
  for (const auto& root : doc->scene.roots)  // the Engine .opad keeps its root hidden: shown here, in memory
    if (const auto* n = doc->scene.node(root); n && !n->visible) doc->run("appearance", {{"target", root}, {"visible", true}});
  auto expected = [doc] {
    int n = 0;
    for (const auto& id : doc->scene.all_bodies()) n += doc->scene.effectively_visible(id) && !doc->scene.node(id)->body_missing;
    return n;
  };
  auto idle = [&w, doc, v, &expected] {
    return !w.m_jobs->busy() && !w.m_selFileTimer.isActive() && !w.m_displayJob && w.m_meshRemaining == 0 && !doc->designBusy && v->displayedCount() + v->skippedCount() >= expected();
  };
  const bool displayed = waitUntil([&] { return idle() && v->displayedCount() > 0; }, 240000);
  if (!require(displayed, QString("%1 bodies displayed").arg(v->displayedCount()))) return finish();
  const auto bodies = doc->scene.all_bodies();
  const int shown = v->displayedCount();
  if (!require(bodies.size() >= 100 && !doc->browse, QString("%1 bodies, editable (100 wanted)").arg(bodies.size()))) return finish();
  // One step: `fn`, then everything it started until `done` holds and no job runs. The command and the longest stall after
  // it are both within the budget.
  QStringList results;
  auto step = [&](const QString& what, const std::function<void()>& fn, const std::function<bool()>& done) {
    waitUntil(idle, 60000);
    trace::resetStalls();
    QElapsedTimer clock;
    clock.start();
    const qint64 cpu0 = trace::threadCpuMs();
    fn();
    const qint64 call = clock.elapsed(), callCpu = trace::threadCpuMs() - cpu0;
    const bool settled = waitUntil([&] { return done() && !w.m_jobs->busy() && !w.m_selFileTimer.isActive(); }, 60000);
    const qint64 total = clock.elapsed();
    const trace::Stalls s = trace::stalls();
    const QString line = QString("%1: %2 ms in the command (%3 ms CPU), %4 stalls, longest %5 ms (%6 ms CPU), settled after %7 ms")
                             .arg(what).arg(call).arg(callCpu).arg(s.count).arg(s.longest).arg(s.longestCpu).arg(total);
    if (!settled || std::max(callCpu, s.longestCpu) >= budget || std::max(call, s.longest) >= 4 * budget) over << line;
    results << line;
  };
  const std::string one = bodies[bodies.size() / 2];
  step("hide a body", [&] { doc->run("appearance", {{"target", one}, {"visible", false}}); }, [&] { return v->displayedCount() == shown - 1; });
  step("undo the hide", [&] { doc->undo(); }, [&] { return v->displayedCount() == shown; });
  step("redo the hide", [&] { doc->redo(); }, [&] { return v->displayedCount() == shown - 1; });
  step("undo it again", [&] { doc->undo(); }, [&] { return v->displayedCount() == shown; });
  w.m_browser->setSelectedIds({one});
  w.onBrowserSelection({one});
  waitUntil(idle, 30000);
  step("Hide others", [&] { w.action("view.hideothers")->trigger(); }, [&] { return v->displayedCount() == 1; });
  step("undo Hide others", [&] { doc->undo(); }, [&] { return v->displayedCount() == shown; });
  bool applied = false;
  const auto watch = QObject::connect(v, &Viewport::selectionApplied, &w, [&applied] { applied = true; });
  const auto roots = doc->scene.roots;
  step("select all", [&] { w.m_browser->setSelectedIds(roots); w.onBrowserSelection(roots); }, [&] { return applied && !v->selection().empty(); });
  QObject::disconnect(watch);
  bool filtered = false;
  const auto filter = QObject::connect(v, &Viewport::filterApplied, &w, [&filtered] { filtered = true; });
  step("Face filter", [&] { w.action("select.faces")->trigger(); }, [&] { return filtered; });
  filtered = false;
  step("Body filter", [&] { w.action("select.bodies")->trigger(); }, [&] { return filtered; });
  QObject::disconnect(filter);
  step("new document", [&] { doc->newDocument(); }, [&] { return v->displayedCount() == 0; });
  if (!over.isEmpty() && round == 1 && !path.isEmpty()) {
    for (const auto& line : over) trace::log("bench: perf: round 1 over budget: " + line);
    trace::log("bench: perf: once more on the file reopened");
    w.openPath(path);
    return true;
  }
  for (const auto& line : results) require(!over.contains(line), line);
  return finish();
}
