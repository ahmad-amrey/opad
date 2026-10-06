// OPAD_BENCH_START_COVER=1 on a box (Windows; skipped elsewhere): the start page must never be what shows where the 3D view
// is once a document is open. The 3D view is a native OpenGL window: until it draws a frame, its area of the window keeps
// what was painted there before, the start page. The window is drawn for this bench (off the screen, never activated, no
// taskbar entry; a hidden window never paints), the start page shown (the document closed), then the document opened
// again from it: the view must be exposed and draw frames once it takes the start page's place, and the start page must
// stay hidden. Nothing here reads the window's pixels (a capture exposes windows itself and would hide the fault).
#ifdef _WIN32
#include <windows.h>
#endif
#include <QCoreApplication>
#include <QTimer>
#include <QWindow>
#include <memory>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "EmptyState.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
struct PaintCount : QObject {
  int paints = 0;
  bool eventFilter(QObject*, QEvent* e) override {
    if (e->type() == QEvent::Paint) ++paints;
    return false;
  }
};
}  // namespace

OPAD_BENCH(OPAD_BENCH_START_COVER, startCover) {
  static int round = 0;
  static PaintCount* count = nullptr;
  MainWindow* win = &w;
#ifndef _WIN32
  trace::log("bench: start-cover: Windows only, skipped PASS");
  QCoreApplication::exit(0);
  return true;
#else
  if (round++ == 0) {
    // Drawn, out of the user's way: off the screen, never activated, no taskbar entry; shown by Qt itself (the process
    // started hidden, which took the first show).
    HWND h = reinterpret_cast<HWND>(w.winId());
    SetWindowLongPtrW(h, GWL_EXSTYLE, GetWindowLongPtrW(h, GWL_EXSTYLE) | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    w.setAttribute(Qt::WA_ShowWithoutActivating);
    w.hide();
    w.setGeometry(-6000, 100, 1600, 1000);
    w.show();
    const QString file = w.m_doc->path();
    QTimer::singleShot(800, &w, [win, file] {
      win->m_doc->closeDocument();  // the start page
      QTimer::singleShot(1500, win, [win, file] {
        if (win->m_stack->currentWidget() != win->m_empty || !win->m_empty->isVisible())
          return trace::log("bench: start-cover: FAIL the start page after closing"), QCoreApplication::exit(2);
        count = new PaintCount;
        count->setParent(win);
        win->m_viewport->installEventFilter(count);
        win->openPath(file);  // opened again from the start page
      });
    });
    return true;
  }
  QTimer::singleShot(1500, &w, [win] {  // loaded, its bodies displayed
    const bool exposed = win->m_viewport->windowHandle() && win->m_viewport->windowHandle()->isExposed();
    const bool startHidden = win->m_stack->currentWidget() == win->m_viewport && !win->m_empty->isVisible();
    trace::log(QStringLiteral("bench: start-cover: opened from the start page: view exposed %1, %2 frames painted, start page hidden %3")
                   .arg(exposed).arg(count ? count->paints : -1).arg(startHidden));
    if (!startHidden) return trace::log("bench: start-cover: FAIL the start page is still the central page"), QCoreApplication::exit(2);
    if (!exposed || !count || count->paints == 0)
      return trace::log("bench: start-cover: FAIL the view took the start page's place but never drew: the window keeps the start page there"), QCoreApplication::exit(2);
    trace::log("bench: start-cover: opened from the start page, the view is exposed and draws where the start page was PASS");
    QCoreApplication::exit(0);
  });
  return true;
#endif
}
