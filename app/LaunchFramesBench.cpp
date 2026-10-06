// OPAD_BENCH_LAUNCH_FRAMES=1 with OPAD_BENCH_OFFSCREEN (the window drawn off the screen from its first turn, so the startup
// goes on at the first expose, as a launch does) on a box opened from the command line while the start page is the central
// page, under the load shade. Then the file is rewritten as a git switch leaves it (its last step gone) and taken in as the
// window's own change (DiskSync::adopt: a reload), then written back (a merge). After the load and after each reload: the 3D
// view is the central page, the start page hidden, the view's native window shown and exposed, and it painted frames since
// (OPAD_TRACE_FRAMES logs each one with its show, hide, expose and native paint messages). Reads no pixels.
#include <QCoreApplication>
#include <QTimer>
#include <QWindow>
#include <filesystem>
#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "DiskSync.hpp"
#include "EmptyState.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"
#include "opad/util.hpp"

OPAD_BENCH(OPAD_BENCH_LAUNCH_FRAMES, launchFrames) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  MainWindow* win = &w;
  AppDocument* doc = w.m_doc;
  auto* disk = w.findChild<DiskSync*>();
  struct State {
    int step = 0, wait = 0, ticks = 0, frames = 0;
    std::size_t ops = 0;
    std::string kept;
  };
  auto st = std::make_shared<State>();
  // The view drawn where the start page was: its page, shown, exposed, frames since `since`.
  auto drawn = [win](int since, const QString& when) {
    Viewport* v = win->m_viewport;
    const bool page = win->m_stack->currentWidget() == v && !win->m_empty->isVisible();
    const bool exposed = v->isVisible() && v->windowHandle() && v->windowHandle()->isExposed();
    trace::log(QStringLiteral("bench: launch-frames: %1: view page %2, frames %3 since, %4").arg(when).arg(page).arg(v->framesPainted() - since).arg(v->frameState()));
    if (!page) throw std::runtime_error((when + ": the start page is still the central page").toStdString());
    if (!exposed) throw std::runtime_error((when + ": the view is not exposed: it draws no frame and its area keeps what was there (the start page)").toStdString());
    if (v->framesPainted() <= since) throw std::runtime_error((when + ": the view painted no frame").toStdString());
  };
  const std::filesystem::path file(doc->path().toStdU16String());
  std::vector<std::function<bool()>> steps{
      [=] {  // launched with the file: loaded and shown
        if (doc->loading || ++st->wait < 10) return false;
        drawn(0, "launched with the file (opened while the start page was current, under the load shade)");
        trace::log("bench: launch-frames: launched with the file, the view exposed and drawing PASS");
        st->frames = win->m_viewport->framesPainted();
        opad::Document d = opad::Document::load(file);  // as a git switch to a branch one step behind leaves it
        st->ops = d.ops.size();
        const std::string full = opad::read_text_file(file);
        st->kept = full;
        d.truncate_ops(d.ops.size() - 1);
        opad::write_text_file(file, d.serialize());
        disk->adopt();
        return true;
      },
      [=] {
        if (doc->doc.ops.size() != st->ops - 1 || doc->loading || ++st->wait < 10) return false;
        drawn(st->frames, "after a reload from disk (a git switch)");
        trace::log("bench: launch-frames: reloaded from disk, the view exposed and drawing PASS");
        st->frames = win->m_viewport->framesPainted();
        opad::write_text_file(file, st->kept);  // and back: a merge of the file's new step
        disk->adopt();
        return true;
      },
      [=] {
        if (doc->doc.ops.size() != st->ops || doc->loading || ++st->wait < 10) return false;
        drawn(st->frames, "after a merge from disk (a git switch back)");
        trace::log("bench: launch-frames: merged from disk, the view exposed and drawing PASS");
        return true;
      },
  };
  auto* timer = new QTimer(&w);
  timer->setInterval(150);
  QObject::connect(timer, &QTimer::timeout, &w, [st, steps, timer] {
    try {
      if (st->step >= int(steps.size())) {
        timer->stop();
        QCoreApplication::exit(0);
        return;
      }
      if (steps[st->step]()) {
        ++st->step;
        st->wait = st->ticks = 0;
      } else if (++st->ticks > 400) {
        throw std::runtime_error("timed out in step " + std::to_string(st->step));
      }
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QStringLiteral("bench: launch-frames: FAIL %1").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
