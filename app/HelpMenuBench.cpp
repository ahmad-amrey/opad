// Benches of the Help menu's windows (UI-108) and the coach card; the help area is HelpArea.cpp.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "CommandHelp.hpp"
#include "HelpClip.hpp"
#include "HelpReference.hpp"
#include "HelpWindows.hpp"
#include "I18n.hpp"
#include "opad/util.hpp"
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>

// OPAD_BENCH_HELPMENU=<prefix> (a document with a box, saved as box.opad): the Help menu (UI-108). Its own entries come
// first: Help for this tool (F1), Tool guide, Shortcuts cheat sheet (Ctrl+/), Getting started, then Report a problem.
// The cheat sheet lists the keys by group with the preset's mouse and the keys of every tool, finds a key, follows the
// navigation preset and closes on Esc; Getting started plays each lesson's clip, its Try it runs the command it teaches
// (Measure starts Distance, Sketch switches to Design and asks for a plane) and says what an unavailable one needs;
// Report a problem lists the versions and the document's kind and size, never its name. The report is not copied: the
// clipboard is the user's. Saved as <prefix>.sheet/.start/.report.png.
OPAD_BENCH(OPAD_BENCH_HELPMENU, helpmenu) {
  const QString prefix = value;
  QSettings().setValue("ui/tipAnimate", true);
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: help menu: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };
  auto escape = [](QWidget* window) {
    QKeyEvent e(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(window, &e);
  };
  add(800, [=, &w] {
    QMenu* menu = nullptr;
    for (QAction* top : w.menuBar()->actions())
      if (top->menu() && top->menu()->actions().contains(w.action("help.current"))) menu = top->menu();
    QStringList ids;
    for (QAction* a : menu ? menu->actions() : QList<QAction*>()) ids << (a->isSeparator() ? QString("-") : a->objectName());
    check(ids.mid(0, 7) == QStringList({"help.current", "help.reference", "help.shortcuts", "help.start", "-", "help.report", "-"}) && ids.contains("help.about"),
          "the Help menu: its own entries first, the window's after (" + ids.join(' ') + ")");
    check(w.action("help.current")->shortcut() == QKeySequence("F1") && w.action("help.shortcuts")->shortcut() == QKeySequence("Ctrl+/"), "F1 and Ctrl+/");
    // The cheat sheet.
    w.action("help.shortcuts")->trigger();
    auto* sheet = w.findChild<ShortcutSheet*>();
    const QStringList titles = sheet ? sheet->titles() : QStringList();
    const QString mouse = QCoreApplication::translate("help", "Mouse"), every = QCoreApplication::translate("help", "In every tool");
    check(sheet && sheet->isVisible() && titles.contains(opGroup(w.action("view.fit"))) && titles.contains(opGroup(w.action("design.extrude"))) && titles.contains(mouse) &&
              titles.endsWith(every),
          "the cheat sheet groups the keys, then the mouse and every tool's keys (" + titles.join(", ") + ")");
    const QStringList rows = sheet ? sheet->shown() : QStringList();
    const QString orbit = QCoreApplication::translate("help", "Orbit") + " Shift+" + QCoreApplication::translate("help", "Middle drag");
    check(rows.contains(help::find("view.fit")->title + " F") && rows.contains(orbit), "with Fit on F and the preset's orbit (" + orbit + ")");
    if (!sheet) return;
    sheet->grab().save(prefix + ".sheet.png");
    sheet->setFilter("Ctrl+/");
    check(sheet->shown() == QStringList({help::find("help.shortcuts")->title + " Ctrl+/"}) && sheet->titles().size() == 1, "searching a key finds its command (" + sheet->shown().join(" | ") + ")");
    sheet->setFilter(QString());
    escape(sheet);
    check(!sheet->isVisible(), "Esc closes it");
    w.action("nav.solidworks")->trigger();
    w.action("help.shortcuts")->trigger();
    check(sheet->isVisible() && sheet->shown().contains(QCoreApplication::translate("help", "Orbit") + " " + QCoreApplication::translate("help", "Middle drag")), "it follows the navigation preset");
    sheet->close();
    w.action("nav.fusion")->trigger();
    // Getting started.
    w.action("help.start")->trigger();
    auto* start = w.findChild<GettingStarted*>();
    check(start && start->isVisible() && start->current() == 0 && GettingStarted::lessons("fusion").size() == 6 && start->clip()->isVisible() &&
              start->clip()->clip() == "nav.fusion" && start->clip()->playing(),
          "Getting started opens at moving around, with the preset's clip");
  });
  add(500, [=, &w] {
    auto* start = w.findChild<GettingStarted*>();
    if (!start) return check(false, "Getting started");
    start->grab().save(prefix + ".start.png");
    start->open(2);
    check(start->clip()->clip() == "inspect.distance" && start->tryButton()->isVisibleTo(start) && start->tryButton()->isEnabled() && start->needs()->isHidden(), "Measure: its clip and Try it");
    w.action("inspect.distance")->setEnabled(false);
    check(!start->tryButton()->isEnabled() && !start->needs()->isHidden(), "a command not available now: Try it waits and says what it needs (" + start->needs()->text() + ")");
    w.action("inspect.distance")->setEnabled(true);
    start->tryButton()->click();
    check(w.m_tool.id == "distance", "Try it starts Distance");
    w.cancelTool();
    w.setWorkspace("review");
    start->open(3);
    start->tryButton()->click();
    check(w.workspaceId() == "design" && w.m_design->pickingPlane(), "Sketch's Try it switches to Design and asks for a plane (" + w.workspaceId() + ")");
    w.m_design->escape();
    escape(start);
    check(!start->isVisible(), "Esc closes Getting started");
    // Report a problem.
    w.action("help.report")->trigger();
    auto* report = w.findChild<ProblemReport*>();
    const QString facts = report ? report->findChild<QPlainTextEdit*>("problemFacts")->toPlainText() : QString();
    check(report && report->isVisible() && facts.contains("OPAD " + QString::fromStdString(opad::version_string())) && facts.contains("Qt ") && facts.contains("Open CASCADE") &&
              facts.contains("Document: OPAD") && facts.contains("1 bodies"),
          "Report a problem lists the versions and the document's kind and size");
    const QString name = QFileInfo(w.m_doc->path()).fileName();
    check(!name.isEmpty() && !facts.contains(name) && !facts.contains(QFileInfo(w.m_doc->path()).absolutePath()), "never the document's name or folder (" + name + ")");
    if (!report) return;
    report->description()->setPlainText("Fit went empty");
    check(report->report().startsWith("What happened:\nFit went empty\n") && report->report().contains("- Document: OPAD"), "the report: what happened, then the facts");
    report->grab().save(prefix + ".report.png");
    if (i18n::current() != "en")
      check(start->windowTitle() != "Getting started" && report->windowTitle() != "Report a problem" && start->layoutDirection() == Qt::RightToLeft, "the windows in the UI language and direction");
    report->close();
    w.action("help.current")->trigger();  // no tool, no card: the guide where it was
    auto* reference = w.findChild<CommandReference*>();
    check(reference && reference->isVisible(), "F1 with no tool opens the tool guide");
    if (reference) reference->close();
  });
  add(0, [=] {
    trace::log(QString("bench: help menu: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [&w, steps, next, check](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, &w, [steps, next, check, i] {
      try { (*steps)[i].fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1);
    });
  };
  (*next)(0);
  return true;
}

// OPAD_BENCH_COACH=<prefix> (an empty document beside coach-box.opad): the coach card of an empty document (UI-108). It shows
// at the bottom centre of the view in Review and Design with its clip playing; New sketch switches to Design and asks for
// a plane, Box opens the box's panel, and the card gives way to either and comes back when they are left; its × hides it
// for this document only; a document with a body has none; Don't show again is for good. Saved as <prefix>.card.png.
OPAD_BENCH(OPAD_BENCH_COACH, coach) {
  static bool started = false;  // opening coach-box.opad below finishes a load, which asks the benches again
  if (std::exchange(started, true)) return true;
  const QString prefix = value;
  QSettings().setValue("ui/tipAnimate", true);
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: coach: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  struct Step { int delay; std::function<void()> fn; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](int delay, std::function<void()> fn) { steps->push_back({delay, std::move(fn)}); };
  auto card = [&w] { return w.m_viewport->findChild<CoachCard*>(); };
  const QString box = QFileInfo(w.m_doc->path()).dir().filePath("coach-box.opad");
  add(800, [=, &w] {
    CoachCard* c = card();
    check(c && c->isVisible() && w.workspaceId() == "review", "an empty document shows the coach card (in " + w.workspaceId() + ")");
    if (!c) return;
    const QRect view = w.m_viewport->rect(), at = c->geometry();
    check(view.contains(at) && std::abs(at.center().x() - view.center().x()) <= 2 && at.bottom() > view.height() * 2 / 3, "at the bottom centre of the view");
    check(c->clip()->clip() == "design.extrude" && c->clip()->playing(), "its clip plays");
    if (i18n::current() != "en") check(c->layoutDirection() == Qt::RightToLeft && c->button("design.sketch")->text() != "New sketch", "in the UI language and direction");
    w.setWorkspace("design");
    check(c->isVisible(), "in Design too");
  });
  add(300, [=, &w] {
    CoachCard* c = card();
    c->grab().save(prefix + ".card.png");
    w.setWorkspace("review");
    c->button("design.sketch")->click();
    check(w.workspaceId() == "design" && w.m_design->pickingPlane() && !c->isVisible(), "New sketch: Design, a plane to pick, the card out of the way");
    w.m_design->escape();
  });
  add(700, [=, &w] {
    CoachCard* c = card();
    check(c->isVisible(), "back once the plane pick is left");
    c->button("design.box")->click();
    check(w.m_design->featureActive() && !c->isVisible(), "Box: its panel, the card out of the way");
    w.m_design->escape();
  });
  add(700, [=, &w] {
    CoachCard* c = card();
    check(c->isVisible(), "back once the box's panel is left");
    c->closeButton()->click();
  });
  add(700, [=, &w] {
    check(!card()->isVisible(), "the x hides it for this document");
    w.action("file.new")->trigger();
  });
  add(400, [=, &w] {
    check(card()->isVisible(), "a new document has it again");
    w.openPath(box);
  });
  add(2500, [=, &w] {
    check(!w.m_doc->scene.all_bodies().empty() && !card()->isVisible(), "a document with a body has none");
    w.action("file.new")->trigger();
  });
  add(400, [=, &w] {
    check(card()->isVisible(), "a new one again");
    card()->neverButton()->click();
    w.action("file.new")->trigger();
  });
  add(700, [=, &w] {
    check(!card()->isVisible() && !QSettings().value("help/coach", true).toBool(), "Don't show again: none in a new document either");
    trace::log(QString("bench: coach: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [&w, steps, next, check](size_t i) {
    if (i >= steps->size()) return;
    QTimer::singleShot((*steps)[i].delay, &w, [steps, next, check, i] {
      try { (*steps)[i].fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      (*next)(i + 1);
    });
  };
  (*next)(0);
  return true;
}
