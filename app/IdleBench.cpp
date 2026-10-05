// OPAD_BENCH_IDLE=<ms>: the document given on the command line opened, the window then left to itself for that long (what
// it does on its own after an open: linked files checked, KiCad models offered and downloaded, syncs), then closed. The
// trace (OPAD_TRACE) is the record; it logs "bench: idle ... PASS" once the time is up with the window still answering.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QTimer>
#include <QToolButton>

#include <memory>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Toast.hpp"

OPAD_BENCH(OPAD_BENCH_IDLE, idle) {
  const int ms = std::max(1000, value.toInt());
  QElapsedTimer clock;
  clock.start();
  qint64 last = 0, longest = 0;
  while (clock.elapsed() < ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    const qint64 now = clock.elapsed();
    longest = std::max(longest, now - last);
    last = now;
  }
  trace::log(QString("bench: idle: %1 ms after the open, the longest gap between event rounds %2 ms, jobs %3 %4")
                 .arg(ms).arg(longest).arg(w.m_jobs->busy() ? "running" : "done").arg(longest < 5000 ? "PASS" : "FAIL"));
  QCoreApplication::exit(longest < 5000 ? 0 : 2);
  return true;
}

// OPAD_BENCH_TOAST_MODAL=1: a toast's action that asks (a modal box, as KiCad's "Download…" does) while the toast times out
// and newer toasts push it out of the stack: the answer comes back to a live window and the action goes on. The toast used
// to be deleted under its own click handler there, and the answer crashed the app.
OPAD_BENCH(OPAD_BENCH_TOAST_MODAL, toastModal) {
  ToastStack* stack = w.m_toasts;
  auto open = std::make_shared<qint64>(-1);
  auto after = std::make_shared<bool>(false);
  auto gone = std::make_shared<bool>(false);
  QPointer<Toast> toast;
  toast = stack->toast("3 3D models come from KiCad's library", "Download…", [stack, open, after, gone, &toast] {
    QElapsedTimer held;
    held.start();
    // A modal loop as the question box runs one, kept past the toast's own 300 ms while newer toasts push it out.
    QEventLoop loop;
    QTimer::singleShot(100, &loop, [stack] {
      for (int i = 0; i < 6; ++i) stack->toast(QString("Another message %1").arg(i), QString(), {}, 200);
    });
    QTimer::singleShot(1200, &loop, &QEventLoop::quit);
    loop.exec();
    *gone = toast.isNull();
    *open = held.elapsed();
    *after = true;  // the action goes on after the answer
  }, 300);
  QToolButton* button = toast ? toast->findChild<QToolButton*>("toastAction") : nullptr;
  if (button) button->click();
  QElapsedTimer clock;
  clock.start();
  while (clock.elapsed() < 800) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  const bool ok = button && *after && *open >= 1000;
  trace::log(QString("bench: toast modal: an action that runs an event loop for %1 ms (its toast deleted meanwhile: %2, it is deleted after the action) went on to its end %3")
                 .arg(*open).arg(*gone).arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
