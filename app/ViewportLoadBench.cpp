// OPAD_BENCH_LOADING=<prefix> (UI-40): a load keeps the window usable. Cases in tools/bench_cases/viewer.py: 1,000 boxes
// (CI) and the Engine beside the repository. The file is opened again from the bench and watched while it loads: the
// strip's job is the load from first to last (never a sub-job), its overall progress only goes up and, once the document
// is built, the workspace is unlocked (no shade, the view takes the mouse, panels enabled, a view command runs) while the
// bodies still stream in under the same job; the display pump is the load's child; an edit asked for meanwhile waits
// with a toast and runs when every body is shown; the load costs a few full syncs, not one per batch of meshes, and makes
// no empty selection-layer jobs. Then once more, cancelled from the strip as soon as the bodies stream in: the load job
// and its pump stop at once and the bodies not shown stay out. A big file (the Engine) also reports the reading and
// parsing of the .opad in per cent. <prefix>.streaming.png: the status bar while the bodies stream in.
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <functional>

#include "BenchRegistry.hpp"
#include "BrowserPanel.hpp"
#include "MainWindow.hpp"
#include "Toast.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  return done();
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_LOADING, loading) {
  static bool running = false;  // the loads below end in runBench again: this bench is already on the stack
  if (std::exchange(running, true)) return true;
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: loading: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  const auto finish = [&all] {
    QCoreApplication::exit(all ? 0 : 2);
    return true;
  };
  AppDocument* doc = w.m_doc;
  Viewport* v = w.m_viewport;
  QString path = doc->path();
  if (!require(!path.isEmpty() && QFileInfo(path).suffix() == "opad", "an .opad file to open again: " + path)) return finish();
  // The Engine .opad keeps its root hidden, so nothing would stream in: a copy beside the bench's output with an op that
  // shows it before the body store (as the evaluation did), without displaying it here first.
  QString copy;
  if (std::none_of(doc->scene.roots.begin(), doc->scene.roots.end(), [doc](const auto& id) { return doc->scene.node(id)->visible; })) {
    QFile in(path);
    QByteArray text = in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    const qsizetype at = text.indexOf("\n#bodies\n");
    QByteArray ops;
    for (const auto& root : doc->scene.roots)
      ops += QByteArray::fromStdString(opad::json{{"op", "appearance"}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()}, {"by", "bench"},
                                                  {"target", root}, {"visible", true}}.dump()) + "\n";
    copy = value + ".opad";
    QFile out(copy);
    if (!require(at > 0 && out.open(QIODevice::WriteOnly) && out.write(text.insert(at + 1, ops)) > 0, "a copy with the root shown")) return finish();
    path = copy;
  }
  const auto cleanup = qScopeGuard([&copy] { if (!copy.isEmpty()) QFile::remove(copy); });
  const bool big = QFileInfo(path).size() > 50 * 1024 * 1024;
  // Watches one load from openPath to the end of its job; `streaming` runs once, the first time the document is built and
  // bodies are still to come.
  struct Seen {
    bool foreignStrip = false, lockedWhileBuilding = true, unlocked = false, streamed = false, childPump = true, backwards = false;
    int lastOverall = -1, openingSteps = 0, finishedAt = -1;
    QStringList phases;
  };
  auto watch = [&](const std::function<void(Seen&)>& streaming) {
    Seen seen;
    QObject context;
    QObject::connect(doc, &AppDocument::loadProgress, &context, [&seen](const QString& phase, int pct, int) {
      if (phase.startsWith(QStringLiteral("Opening")) && pct >= 0) ++seen.openingSteps;
    });
    doc->newDocument();  // the same file again would keep what is on screen (same bodies, same places): nothing would stream
    waitUntil([&] { return v->displayedCount() == 0 && !w.m_jobs->busy(); }, 30000);  // nothing older than the load runs
    const int syncs = v->syncCount(), begun = w.m_jobs->begun();
    w.openPath(path);
    Job* load = w.m_loadJob;
    QObject::connect(load, &Job::overallChanged, &context, [&seen](int overall) {
      if (overall < seen.lastOverall) seen.backwards = true;
      seen.lastOverall = std::max(seen.lastOverall, overall);
    });
    bool called = false;
    const bool ended = waitUntil([&] {
      if (!w.m_loadJob) return true;
      if (Job* shown = w.m_jobs->current(); shown != load) seen.foreignStrip = true;
      if (doc->loading && (!v->blocked() || w.m_browser->isEnabled())) seen.lockedWhileBuilding = false;
      if (const Job* pump = v->pumpJob(); pump && (pump->kind() != JobKind::Child || pump->parentJob() != load)) seen.childPump = false;
      if (!doc->loading && w.m_loadDocDone && v->remainingBodies() > 0) {
        seen.streamed = true;
        seen.unlocked = !v->blocked() && w.m_browser->isEnabled() && w.m_timeline->isEnabled();
        if (!called) {
          called = true;
          streaming(seen);
        }
      }
      return false;
    }, 300000);
    seen.finishedAt = ended ? v->syncCount() - syncs : -1;
    trace::log(QString("bench: loading: %1 full syncs, %2 jobs begun, overall progress up to %3%, %4 opening steps")
                   .arg(v->syncCount() - syncs).arg(w.m_jobs->begun() - begun).arg(seen.lastOverall).arg(seen.openingSteps));
    return seen;
  };

  // 1. A load watched to the end, with a view command and an edit while the bodies stream in.
  QAction* edit = w.action("edit.restore");  // an edit, refused afterwards with no timeline marker chosen: harmless
  bool viewRan = false, deferred = false;
  int pins = 0;
  const auto counted = QObject::connect(edit, &QAction::triggered, &w, [&pins] { ++pins; });
  const Seen first = watch([&](Seen&) {
    const auto before = v->cameraJson();
    w.action("view.top")->trigger();
    viewRan = v->cameraJson() != before;
    edit->trigger();
    const auto toasts = w.m_toasts->toasts();
    deferred = w.m_afterStream == edit && std::any_of(toasts.begin(), toasts.end(), [](Toast* t) { return t->text().contains(QStringLiteral("Still loading")); });
    QTimer::singleShot(700, &w, [&w, shot = value + ".streaming.png"] { w.statusBar()->grab().save(shot); });  // the strip as the bodies stream in
  });
  waitUntil([&] { return !w.m_jobs->busy(); }, 60000);
  QCoreApplication::processEvents();
  QObject::disconnect(counted);
  require(first.finishedAt >= 0 && v->displayedCount() > 0, QString("loaded again, %1 bodies shown").arg(v->displayedCount()));
  require(!first.foreignStrip, "the strip shows the load job from start to end, never one of its parts");
  require(!first.backwards && first.lastOverall >= 90, QString("the overall progress only goes up (to %1%)").arg(first.lastOverall));
  require(first.streamed && first.unlocked, "the workspace is unlocked while the bodies stream in (view takes the mouse, browser and timeline enabled)");
  require(first.lockedWhileBuilding, "locked while the file is read and built");
  require(viewRan, "a view command (Top) runs while the bodies stream in");
  require(deferred && pins >= 2 && !w.m_afterStream, QString("an edit asked for meanwhile waits with a toast and runs after (%1 runs)").arg(pins));
  require(first.childPump, "the display pump runs as a child of the load");
  require(first.finishedAt >= 0 && first.finishedAt <= 2, QString("%1 full syncs for the load (one per document change, none per batch of meshes)").arg(first.finishedAt));
  if (big) require(first.openingSteps >= 3, QString("the .opad read and parse report per cent (%1 steps)").arg(first.openingSteps));

  // 2. Cancelled from the strip as soon as bodies stream in.
  int shownAtCancel = -1;
  bool cancelledPump = true;
  const Seen second = watch([&](Seen&) {
    Job* load = w.m_loadJob;
    emit w.m_progress->cancelRequested();  // the strip's Cancel
    shownAtCancel = v->displayedCount();
    cancelledPump = !v->pumpJob() && !w.m_loadJob && load;
  });
  waitUntil([&] { return !w.m_jobs->busy(); }, 60000);
  const int total = static_cast<int>(doc->scene.all_bodies().size());
  require(second.streamed && cancelledPump, "Cancel on the strip ends the load job and its display pump at once");
  require(shownAtCancel >= 0 && v->displayedCount() < total, QString("bodies not shown when it was cancelled stay out (%1 shown then, %2 now, of %3)")
                                                                  .arg(shownAtCancel).arg(v->displayedCount()).arg(total));
  return finish();
}
