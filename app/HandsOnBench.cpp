// OPAD_BENCH_HANDSON=<prefix> (the hands-on test of the final build, 2026-10-05) on a saved box: what the desktop showed, through
// the events the window gets.
//  - Minimised and restored: the 3D view draws its whole frame again (it stayed black until the pointer moved over it).
//  - The browser stays asked for while a page hides the view and shows it again (it was gone after an .opad opened from the
//    Drafting viewer, View > Browser still ticked: the first click did nothing).
//  - A press on a chip over the view (the isolation's ×, the smart selection's chip) is the chip's: no rubber band follows the
//    pointer afterwards; a press the view had whose release went elsewhere (a menu, a dialog) is dropped at the next move,
//    nothing selected; a button held since a press the view never had (a file dialog closed by a double click) starts none.
//  - A value box by an arrow (a sketch's Offset): typed 10, a click elsewhere (the panel's Preview), then 45 is 45, not 1045.
//  - The trace's watchdog: 300 ms of frames drawn back to back (the cube's turn, Fit's glide) is no stall; 300 ms without a
//    frame is one.
//  - Shift+Alt+R (Open file location) reaches its command: enabled, the only binding of that key, no menu mnemonic or widget
//    taking it at the override stage over the view.
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenuBar>
#include <QMouseEvent>
#include <QShortcut>
#include <QThread>
#include <QWindowStateChangeEvent>

#include <memory>
#include <string>

#include "BenchRegistry.hpp"
#include "BrowserOverlay.hpp"
#include "DimensionHandle.hpp"
#include "Drawing2DBench.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "SmartSelect.hpp"
#include "Viewport.hpp"
#include "ViewportChips.hpp"

OPAD_BENCH(OPAD_BENCH_HANDSON, handson) {
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: hands-on: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
  };
  Viewport* v = w.m_viewport;
  auto script = std::make_shared<bench2d::Script>();
  auto body = std::make_shared<std::string>();
  auto mouse = [](QWidget* to, QEvent::Type type, const QPointF& at, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QMouseEvent e(type, at, to->mapToGlobal(at), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
    QApplication::sendEvent(to, &e);
  };
  script->add("the box is displayed", [] {}, [v, &w, body] {
    if (v->pumpJob() || v->remainingBodies() > 0 || v->displayedCount() < 1 || w.m_jobs->busy()) return false;
    const auto bodies = w.m_doc->scene.all_bodies();
    if (bodies.empty()) return false;
    *body = bodies.front();
    return true;
  });
  script->add("restored", [=, &w] {
    v->standardView("iso");
    v->fitAll();
    v->grabImage();  // a frame: nothing is left to draw
    const int before = v->exposeRedraws();
    QWindowStateChangeEvent restored(Qt::WindowMinimized);  // what the window gets as it comes back from the taskbar
    QApplication::sendEvent(&w, &restored);
    require(v->exposeRedraws() == before + 1 && v->frameInvalidated(), "restored from the taskbar: the view draws its whole frame again (it stayed black)");
    // A page shown in the view's place and the view back (Drafting, then an .opad): the browser is still asked for.
    BrowserOverlay* overlay = w.m_browserOverlay;
    const bool asked = overlay->requested() && w.action("panel.browser")->isChecked();
    QHideEvent hide;
    QApplication::sendEvent(v, &hide);
    QShowEvent show;
    QApplication::sendEvent(v, &show);
    require(asked && overlay->requested() && w.action("panel.browser")->isChecked(), "the view hidden by a page and shown again: the browser is still asked for, View > Browser ticked");
  });
  script->add("a press on the isolation chip", [=] {
    v->isolate({*body});
  }, [v] { return v->isIsolated(); });
  script->add("its release", [=, &w] {
    w.updateChips();
    QLabel* chip = w.m_chips->isolationChip();
    const QPointF at(chip->width() / 2.0, chip->height() / 2.0);
    mouse(chip, QEvent::MouseButtonPress, at, Qt::LeftButton);
    require(!v->gestureHeld(), "a press on the isolation chip (a label) is the chips row's, not the view's");
    mouse(chip, QEvent::MouseButtonRelease, at, Qt::NoButton);
  }, [v] { return !v->isIsolated(); });
  script->add("the smart selection's chip", [=] {
    require(!v->gestureHeld(), "its release ended the isolation; no rubber band follows the pointer");
    if (auto* smart = v->findChild<SmartChip*>()) {
      mouse(smart, QEvent::MouseButtonPress, QPointF(smart->width() / 2.0, smart->height() / 2.0), Qt::LeftButton);
      require(!v->gestureHeld(), "a press on the smart selection's chip is the chip's, not the view's");
      mouse(smart, QEvent::MouseButtonRelease, QPointF(-20, -20), Qt::NoButton);  // let go off it: no click
    } else {
      require(false, "no smart selection chip");
    }
    // A press the view had, released elsewhere (a menu item, a dialog): its band goes at the next move, nothing selected.
    const auto selected = v->selection().size();
    const QPointF from(12, v->height() - 12), to(v->width() - 12, 40);
    const int dropped = v->droppedGestures();
    mouse(v, QEvent::MouseButtonPress, from, Qt::LeftButton);
    mouse(v, QEvent::MouseMove, (from + to) / 2, Qt::LeftButton);
    const bool band = v->gestureHeld();
    mouse(v, QEvent::MouseMove, to, Qt::NoButton);
    require(band && !v->gestureHeld() && v->droppedGestures() == dropped + 1 && v->selection().size() == selected,
            "a press on the view released elsewhere: dropped at the next move, nothing selected");
    mouse(v, QEvent::MouseButtonPress, to, Qt::LeftButton);  // the next click is a click again
    mouse(v, QEvent::MouseButtonRelease, to, Qt::NoButton);
    require(!v->gestureHeld(), "the next click is a click (no band from where the stray press was)");
    // The button held since a press the view never had (a file dialog double-clicked shut), Ctrl coming up: no band starts.
    mouse(v, QEvent::MouseMove, from, Qt::LeftButton, Qt::ControlModifier);
    mouse(v, QEvent::MouseMove, to, Qt::LeftButton);
    require(!v->gestureHeld(), "a button held since a press another window had starts no band");
    mouse(v, QEvent::MouseButtonRelease, to, Qt::NoButton);
  });
  script->add("a value box by an arrow", [=, &w] {
    auto* handle = new DimensionHandle(v, w.m_jobs);
    handle->setCapturesKeys(false);  // as the sketch has it: its keys come through type()
    handle->setLabel("Offset");
    handle->configure({0, 0, 0}, {1, 0, 0}, 2, "2 mm");
    handle->show();
    auto text = [handle] { return handle->input()->box(0)->text(); };
    handle->type("1");
    handle->type("0");
    const QString typed = text();
    QWidget* elsewhere = w.menuBar();  // a click on another widget (a tool panel's Preview button takes no keyboard)
    mouse(elsewhere, QEvent::MouseButtonPress, QPointF(2, 2), Qt::LeftButton);
    mouse(elsewhere, QEvent::MouseButtonRelease, QPointF(2, 2), Qt::NoButton);
    handle->type("4");
    handle->type("5");
    require(typed == "10" && text() == "45", "typed 10, a click elsewhere, then 45: the box reads " + text() + " (10 first: " + typed + ")");
    handle->hide();
    handle->deleteLater();
  });
  // The watchdog: frames drawn back to back for 300 ms (the cube's turn, Fit's glide) starve its timer but are no stall; 300 ms
  // without a frame is one.
  auto since = std::make_shared<QElapsedTimer>();
  auto busy = [](bool drawing) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 300) {
      QThread::msleep(15);
      if (drawing) trace::frameDrawn();
    }
  };
  script->add("an animation", [=] {
    trace::resetStalls();
    busy(true);
    since->start();
  }, [since] { return since->elapsed() > 200; });
  script->add("a stall", [=] {
    require(trace::stalls().count == 0, QString("300 ms of frames back to back: no stall logged (%1)").arg(trace::stalls().count));
    busy(false);
    since->start();
  }, [since] { return since->elapsed() > 200; });
  script->add("Shift+Alt+R", [=, &w] {
    require(trace::stalls().count == 1 && trace::stalls().longest >= 280, QString("300 ms without a frame: one stall (%1, %2 ms)").arg(trace::stalls().count).arg(trace::stalls().longest));
    const QKeySequence key("Shift+Alt+R");
    w.updateCommands();
    QAction* reveal = w.action("file.reveal");
    QStringList others;
    for (QAction* a : w.findChildren<QAction*>())
      if (a != reveal && a->shortcuts().contains(key)) others << a->objectName();
    for (QShortcut* s : w.findChildren<QShortcut*>())
      if (s->key() == key) others << "shortcut " + s->objectName();
    for (QAction* a : w.menuBar()->actions())  // a mnemonic is Alt+letter: Shift+Alt+R never matches it, but say so
      if (QKeySequence::mnemonic(a->text()) == key) others << "menu " + a->text();
    QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_R, Qt::ShiftModifier | Qt::AltModifier, "R");
    over.setAccepted(false);  // as Qt asks
    QApplication::sendEvent(v, &over);
    require(reveal && reveal->isEnabled() && reveal->shortcuts().contains(key) && others.isEmpty() && !over.isAccepted(),
            "Shift+Alt+R reaches Open file location: enabled, its key alone (" + others.join(", ") + "), not taken over the view");
  });
  bench2d::Script::run(&w, script, 0, require, [all] {
    trace::log(QString("bench: hands-on fixes %1").arg(*all ? "PASS" : "FAIL"));
    QCoreApplication::exit(*all ? 0 : 2);
  });
  return true;
}
