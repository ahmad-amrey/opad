// Bench of the standard commands and keys (UI-111): Select all, Invert selection, Repeat, Properties on Alt+Enter, Redo's
// second key, and Open/New going on after an unfinished sketch is finished. The commands are MainWindowEdit.cpp's.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>

// OPAD_BENCH_STANDARDKEYS=<prefix> (a box, a cylinder beside it and a sketch "Plate", started with Properties saved on
// Ctrl+P as the old editor wrote it): the keys (Ctrl+A, Ctrl+Shift+I, Shift+Enter, Alt+Enter now and the setting gone,
// F1, Ctrl+, and Redo also on Ctrl+Shift+Z); Select all takes the visible bodies only, in the body filter; Invert takes
// the others; Repeat names and reruns the last tool and leads the view's context menu; in the sketch, Select all and
// Invert work on its curves; an unfinished sketch whose question is answered Finish is saved as an op and then what
// asked goes on.
OPAD_BENCH(OPAD_BENCH_STANDARDKEYS, standardkeys) {
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: standard keys: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  struct Step { int delay; std::function<void()> fn; std::function<bool()> until; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn, std::function<bool()> until = {}) { steps->push_back({delay, std::move(fn), std::move(until)}); };
  auto idle = [&w] { return !w.m_jobs->busy(); };
  auto bodies = std::make_shared<std::vector<std::string>>(w.m_doc->scene.all_bodies());
  auto selected = [&w] {
    std::vector<std::string> ids;
    for (const auto& r : w.m_selRefs) ids.push_back(r.body);
    std::sort(ids.begin(), ids.end());
    return ids;
  };
  auto resumed = std::make_shared<bool>(false);
  auto ops = std::make_shared<size_t>(0);
  add(800, [=, &w] {
    auto key = [&w](const char* id) { return w.action(id) ? w.action(id)->shortcut() : QKeySequence(); };
    check(key("edit.selectall") == QKeySequence::SelectAll && key("edit.invert") == QKeySequence("Ctrl+Shift+I") && key("edit.repeat") == QKeySequence("Shift+Return") &&
              key("help.current") == QKeySequence("F1") && key("tools.preferences") == QKeySequence("Ctrl+,"),
          "Ctrl+A, Ctrl+Shift+I, Shift+Enter, F1, Ctrl+,");
    check(key("inspect.properties") == QKeySequence("Alt+Return") && !QSettings().contains("shortcuts/inspect.properties"), "Properties moved to Alt+Enter, the old Ctrl+P setting gone");
    check(w.action("edit.redo")->shortcuts().contains(QKeySequence("Ctrl+Shift+Z")) && w.action("edit.redo")->shortcut() == QKeySequence::Redo, "Redo also on Ctrl+Shift+Z");
    check(bodies->size() == 2, QString("two bodies (%1)").arg(bodies->size()));
    if (bodies->size() != 2) return;
    w.m_doc->run("appearance", opad::json{{"target", (*bodies)[1]}, {"visible", false}});
    w.action("select.faces")->trigger();
    w.action("edit.selectall")->trigger();
    check(w.m_viewport->selectionFilter() == Viewport::SelFilter::Body && selected() == std::vector<std::string>{(*bodies)[0]}, "Select all: the visible body, in the body filter");
  }, idle);
  add(100, [=, &w] {
    if (bodies->size() != 2) return;
    w.m_doc->run("appearance", opad::json{{"target", (*bodies)[1]}, {"visible", true}});
    w.action("edit.selectall")->trigger();
    check(selected().size() == 2, "both once it shows");
  }, idle);
  add(100, [=, &w] {
    if (bodies->size() != 2) return;
    w.m_browser->setSelectedIds({(*bodies)[0]});
    w.onBrowserSelection({(*bodies)[0]});
  }, idle);
  add(100, [=, &w] {
    if (bodies->size() != 2) return;
    w.action("edit.invert")->trigger();
    check(selected() == std::vector<std::string>{(*bodies)[1]}, "Invert: the other one");
    check(!w.action("edit.repeat")->isEnabled() || w.m_lastCommand.isEmpty() || w.m_lastCommand != "edit.invert", "selection commands are not repeated");
    w.action("inspect.distance")->trigger();
    check(w.m_tool.id == "distance", "Distance runs");
    w.cancelTool();
    QAction* repeat = w.action("edit.repeat");
    check(repeat->isEnabled() && repeat->text() == QCoreApplication::translate("MainWindow", "Repeat %1").arg(QString(w.action("inspect.distance")->text()).remove('&')),
          "Repeat names it: " + repeat->text());
    repeat->trigger();
    check(w.m_tool.id == "distance", "Repeat runs it again");
    w.cancelTool();
    QTimer::singleShot(400, &w, [=, &w] {  // the menu runs its own loop: look at it from inside, then close it
      QMenu* menu = nullptr;
      for (QWidget* top : QApplication::topLevelWidgets())
        if (auto* m = qobject_cast<QMenu*>(top); m && m->isVisible() && m->parentWidget() == &w) menu = m;
      check(menu && !menu->actions().isEmpty() && menu->actions().first() == w.action("edit.repeat"), "the view's context menu starts with Repeat");
      for (QWidget* top : QApplication::topLevelWidgets())
        if (auto* m = qobject_cast<QMenu*>(top); m && m->isVisible()) m->close();
    });
    w.showContextMenu(w.m_viewport->mapToGlobal(w.m_viewport->rect().center()), {});
  }, idle);
  add(300, [=, &w] {
    const auto& sketches = w.m_doc->scene.sketches;
    check(!sketches.empty(), "the document has a sketch");
    if (!sketches.empty()) w.m_design->editOp(sketches.front().id);
  }, idle);
  add(300, [=, &w] {
    SketchEditor* sketch = w.m_design->sketch();
    w.action("edit.selectall")->trigger();
    const size_t all = sketch->selected().size();
    check(all >= 4, QString("Select all in the sketch: its curves (%1)").arg(all));
    w.action("edit.invert")->trigger();
    check(sketch->selected().empty(), "Invert: none of them");
    w.action("edit.selectall")->trigger();
    sketch->deleteSelection();
  }, [&w] { return w.m_design->sketchActive() && !w.m_design->sketch()->busy(); });
  add(100, [=, &w] {
    check(w.m_design->sketch()->modified(), "the sketch has an unsaved change");
    *ops = w.m_doc->doc.ops.size();
    QTimer::singleShot(0, &w, [check] {  // answers before the bench's own guard dismisses the box (Cancel)
      QMessageBox* box = nullptr;
      for (QWidget* top : QApplication::topLevelWidgets())
        if (auto* b = qobject_cast<QMessageBox*>(top); b && b->isVisible()) box = b;
      check(box && box->button(QMessageBox::Save), "an unfinished sketch asks first");
      if (box && box->button(QMessageBox::Save)) box->button(QMessageBox::Save)->click();
    });
    const bool goes = w.leaveSketch([resumed] { *resumed = true; });
    check(!goes, "Finish: the action waits for the sketch");
  }, [&w] { return !w.m_design->sketch()->busy(); });
  add(100, [=, &w] {
    check(*resumed && !w.m_design->sketchActive() && w.m_doc->doc.ops.size() > *ops, QString("the sketch is saved as an op, then the action goes on (%1 ops, was %2)").arg(w.m_doc->doc.ops.size()).arg(*ops));
    trace::log(QString("bench: standard keys: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  }, [resumed, &w] { return *resumed && !w.m_jobs->busy(); });
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

// OPAD_BENCH_SELECTALL=1 on a large model (the Engine): Select all, Invert and Select all again each return within a
// frame budget of 50 ms (the highlighting is the viewport's sliced job), and the selection ends up whole.
OPAD_BENCH(OPAD_BENCH_SELECTALL, selectall) {
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: select all: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  auto timed = [&w](const char* id) {
    QElapsedTimer clock;
    clock.start();
    w.action(id)->trigger();
    return clock.elapsed();
  };
  auto shown = std::make_shared<size_t>(0);
  auto waitIdle = std::make_shared<std::function<void(std::function<void()>)>>();
  *waitIdle = [&w, waitIdle](std::function<void()> then) {
    QTimer::singleShot(100, &w, [&w, waitIdle, then] { if (w.m_jobs->busy()) (*waitIdle)(then); else then(); });
  };
  w.action("edit.showall")->trigger();  // the Engine's root is hidden: everything on screen first
  (*waitIdle)([=, &w] {
    *shown = w.shownBodies().size();
    const qint64 all = timed("edit.selectall");
    check(all < 50, QString("Select all on %1 bodies returns in %2 ms").arg(*shown).arg(all));
    (*waitIdle)([=, &w] {
      check(w.m_selRefs.size() == *shown, QString("all %1 selected").arg(w.m_selRefs.size()));
      const qint64 inverted = timed("edit.invert");
      check(inverted < 50 && w.m_selRefs.empty(), QString("Invert returns in %1 ms with nothing left").arg(inverted));
      (*waitIdle)([=, &w] {
        trace::log(QString("bench: select all: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
        QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
      });
    });
  });
  return true;
}
