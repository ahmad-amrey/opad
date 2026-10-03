// OPAD_BENCH_TOASTPERF=<n>, meant for the Engine: once its bodies are on screen (unhidden in memory, never saved), n toasts
// shown through the stack and dismissed one at a time, each step and the events after it timed on their own ("bench: toast
// perf: ..."); FAIL when one takes a stall's worth (250 ms). Engine, 1295 bodies on screen: 3-95 ms a toast (machine loaded).
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <algorithm>
#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Toast.hpp"
#include "Viewport.hpp"

OPAD_BENCH(OPAD_BENCH_TOASTPERF, toast_perf) {
  struct State {
    int phase = 0, ticks = 0, remaining = -1, settled = 0, count = 3;
    qint64 worst = 0;
  };
  auto st = std::make_shared<State>();
  st->count = std::max(1, value.toInt());
  QObject::connect(w.m_viewport, &Viewport::meshingProgress, &w, [st](int remaining) {
    st->remaining = remaining;
    st->settled = 0;
  });
  auto* timer = new QTimer(&w);
  timer->setInterval(300);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer] {
    if (++st->ticks > 2000) {
      trace::log("bench: toast perf FAIL: the bodies never settled on screen");
      return QCoreApplication::exit(2);
    }
    AppDocument* doc = w.m_doc;
    if (st->phase == 0) {  // the load job ends before its bodies are meshed and displayed
      const auto bodies = doc->scene.all_bodies();
      if (std::none_of(bodies.begin(), bodies.end(), [doc](const std::string& id) { return doc->scene.effectively_visible(id); })) {
        trace::log("bench: toast perf: nothing visible, unhiding (in memory, never saved)");
        std::vector<std::string> hidden;  // collected first: every run() rebuilds the scene being iterated
        for (const auto& [id, node] : doc->scene.nodes)
          if (!node.visible) hidden.push_back(id);
        for (const auto& id : hidden) doc->run("appearance", {{"target", id}, {"visible", true}});
        st->remaining = -1;
        return;
      }
      if (st->remaining > 0 || ++st->settled < 6) return;
      trace::log(QStringLiteral("bench: toast perf: %1 bodies on screen").arg(w.m_viewport->displayedCount()));
      st->phase = 1;
      return;
    }
    if (st->phase > st->count) {
      timer->stop();
      const bool ok = st->worst < 250;
      trace::log(QStringLiteral("bench: toast perf: the slowest step %1 ms %2").arg(st->worst).arg(ok ? "PASS" : "FAIL"));
      return QCoreApplication::exit(ok ? 0 : 2);
    }
    QElapsedTimer t;
    t.start();
    Toast* toast = w.m_toasts->toast(QStringLiteral("Toast %1").arg(st->phase), QStringLiteral("Undo"), [] {}, 0);
    const qint64 shown = t.restart();
    QCoreApplication::processEvents();
    const qint64 after = t.restart();
    toast->dismiss();
    const qint64 dismissed = t.restart();
    QCoreApplication::processEvents();
    const qint64 gone = t.restart();
    st->worst = std::max({st->worst, shown, after, dismissed, gone});
    trace::log(QStringLiteral("bench: toast perf: %1: shown %2 ms, events %3 ms; dismissed %4 ms, events %5 ms").arg(st->phase).arg(shown).arg(after).arg(dismissed).arg(gone));
    ++st->phase;
  });
  timer->start();
  return true;
}
