// OPAD_BENCH_STARTUP=1 (UI-44): the startup's order, read from startup::marks() once the file has loaded: the viewer is
// made after the window was shown and on a later event-loop turn (after its first expose, or kExposeWaitMs for a window
// started hidden, as here), its first frame on a turn after that, the file opened after the first frame; each step's own
// time is logged (the evaluation's launch froze 0.8-1.5 s in one piece before the window was ever painted).
#include <QCoreApplication>

#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "StartUp.hpp"

OPAD_BENCH(OPAD_BENCH_STARTUP, startup) {
  const startup::Marks& m = startup::marks();
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: startup: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  require(m.shown >= 0 && m.viewer >= 0 && m.frame >= 0 && m.opened >= 0, "every step ran");
  // The marks are whole milliseconds and the wait a timer of its own started just after `shown`: 1 ms early is in time.
  require(m.viewer >= m.shown + (m.byExpose ? 0 : startup::kExposeWaitMs - 2) && (!m.byExpose || m.viewer >= m.exposed),
          QString("the viewer is made after the window is shown (%1 ms) %2 (at %3 ms)")
              .arg(m.shown).arg(m.byExpose ? QString("and exposed (%1 ms)").arg(m.exposed) : QString("and the wait for an expose")).arg(m.viewer));
  require(m.frame >= m.viewerDone && m.opened >= m.frameDone,
          QString("its first frame (at %1 ms) after it (done at %2 ms), the file opened (at %3 ms) after the first frame (done at %4 ms)")
              .arg(m.frame).arg(m.viewerDone).arg(m.opened).arg(m.frameDone));
  require(m.viewerDone - m.viewer < 1500 && m.frameDone - m.frame < 1500,
          QString("making the viewer took %1 ms, its first frame %2 ms (each on its own turn)").arg(m.viewerDone - m.viewer).arg(m.frameDone - m.frame));
  require(w.m_viewport->displayedCount() > 0, QString("the file is shown (%1 bodies)").arg(w.m_viewport->displayedCount()));
  QCoreApplication::exit(all ? 0 : 2);
  return true;
}
