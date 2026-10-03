// Bench of Preferences (UI-110); the window is Preferences.cpp, its command and pages PreferencesArea.cpp.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "I18n.hpp"
#include "Preferences.hpp"
#include "Units.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolButton>

// OPAD_BENCH_PREFERENCES=<prefix> (a document with a box): Ctrl+, opens Preferences, not modal, with its pages in order;
// the Edit menu ends in it and the gear menu is short and starts with it. The search keeps the pages with a match and
// marks the matching rows; nothing found says so. Rows change what they stand for at once: Show the grid is the Grid
// command (both ways), a fixed spacing reaches the grid, undo steps the document, decimals the units service, the
// angle step and recovery minutes their settings, viewer mode the document, the navigation radio the preset, and the
// cache size arrives from a worker. The sketch panel's snaps, Polar and its status-bar menu follow the rows and back while
// both are open; a setting written without a word shows once the window is activated. The old commands lead to their
// row (Undo history: General, focused). Saved as
// <prefix>.general.png, <prefix>.search.png, <prefix>.sketch.png.
OPAD_BENCH(OPAD_BENCH_PREFERENCES, preferences) {
  const QString prefix = value;
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: preferences: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  auto finish = [failed] {
    trace::log(QString("bench: preferences: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  };
  QTimer::singleShot(600, &w, [=, &w] {
    try {
      QAction* open = w.action("tools.preferences");
      check(open && open->shortcut() == QKeySequence("Ctrl+,") && open->menuRole() == QAction::PreferencesRole, "Preferences on Ctrl+, (the application menu's on macOS)");
      QMenu* edit = nullptr;
      for (QAction* top : w.menuBar()->actions())
        if (top->menu() && top->menu()->actions().contains(w.action("edit.undo"))) edit = top->menu();
      check(edit && edit->actions().last() == open, "the Edit menu ends in it");
      QToolButton* gear = w.m_ribbon->findChild<QToolButton*>("ribbonSettings");
      QStringList entries;
      for (QAction* a : gear && gear->menu() ? gear->menu()->actions() : QList<QAction*>()) if (!a->isSeparator()) entries << QString(a->text()).remove('&');
      check(gear && gear->menu() && gear->menu()->actions().first() == open && entries.size() <= 8, "the gear menu starts with it and keeps " + QString::number(entries.size()) + " entries: " + entries.join(", "));
      open->trigger();
      auto* dialog = w.findChild<PreferencesDialog*>("preferences");
      check(dialog && dialog->isVisible() && !QApplication::activeModalWidget(), "it opens, not modal");
      if (!dialog) return finish();
      check(dialog->pageIds() == QStringList({"general", "display", "units", "sketch", "grid", "files", "recovery", "vcs", "keyboard", "ai"}), "the pages: " + dialog->pageIds().join(' '));
      if (i18n::current() != "en") check(dialog->layoutDirection() == Qt::RightToLeft && dialog->windowTitle() != "Preferences", "in the UI language and direction");
      dialog->grab().save(prefix + ".general.png");
      // Search.
      const QString snap = i18n::current() == "en" ? "snap" : QCoreApplication::translate("help", "Grid snapping").section(' ', 0, 0);
      dialog->setSearch(snap);
      const QStringList found = dialog->visiblePages();
      check(found.contains("sketch") && found.contains("grid") && !found.contains("ai") && !found.contains("vcs"), "\"" + snap + "\" keeps the snapping pages: " + found.join(' '));
      check(!dialog->marked().isEmpty() && dialog->marked().join('\n').contains(snap, Qt::CaseInsensitive), "and marks the rows: " + dialog->marked().join(" | "));
      dialog->grab().save(prefix + ".search.png");
      dialog->setSearch("zzqx");
      check(dialog->visiblePages().isEmpty() && dialog->findChild<QStackedWidget*>()->isHidden(), "nothing found: no page, a note says so");
      dialog->setSearch(QString());
      check(dialog->visiblePages().size() == dialog->pageIds().size(), "cleared: every page again");
      // Grid.
      dialog->setPage("grid");
      QWidget* grid = dialog->pageWidget("grid");
      auto* show = grid->findChild<QCheckBox*>("view.grid");
      const bool before = w.action("view.grid")->isChecked();
      show->click();
      check(w.action("view.grid")->isChecked() != before, "Show the grid is the Grid command");
      w.action("view.grid")->trigger();
      check(show->isChecked() == before, "and follows it back");
      auto* automatic = grid->findChild<QCheckBox*>("view/gridAutomatic");
      auto* spacing = grid->findChild<QDoubleSpinBox*>("view/gridSpacing");
      if (automatic->isChecked()) automatic->click();
      spacing->setValue(units::toDisplay(units::Kind::Length, 5));
      check(std::abs(QSettings().value("view/gridSpacing").toDouble() - 5) < 1e-9, "a fixed spacing of 5 mm reaches the grid");
      automatic->click();
      check(QSettings().value("view/gridSpacing").toDouble() == 0, "automatic again");
      // General: undo steps.
      dialog->setPage("general");
      auto* undo = dialog->pageWidget("general")->findChild<QSpinBox*>("edit/undoDepth");
      undo->setValue(20);
      check(w.m_doc->undoLimit() == 20, "undo steps: the document keeps 20");
      undo->setValue(50);
      // Typed: nothing applies before Enter (the "2" of 200 would have dropped all but two undo steps for good).
      undo->selectAll();
      for (const QChar c : QString("200")) {
        QKeyEvent digit(QEvent::KeyPress, Qt::Key_0 + c.digitValue(), Qt::NoModifier, QString(c));
        QApplication::sendEvent(undo, &digit);
      }
      check(w.m_doc->undoLimit() == 50 && QSettings().value("edit/undoDepth").toInt() == 50, "typing 200 in Undo steps applies nothing at each keystroke");
      QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(undo, &enter);
      check(w.m_doc->undoLimit() == 200, "and 200 on Enter");
      undo->setValue(50);
      // Units.
      dialog->setPage("units");
      auto* decimals = dialog->pageWidget("units")->findChild<QSpinBox*>("units/decimals");
      const int places = units::current().decimals;
      decimals->setValue(places == 2 ? 4 : 2);
      check(units::current().decimals == (places == 2 ? 4 : 2), "decimal places reach the units service");
      units::setPrecision(places, units::current().radians, units::current().fraction);
      check(decimals->value() == places, "and show what it says");
      // Sketch and snaps.
      dialog->setPage("sketch");
      auto* step = dialog->pageWidget("sketch")->findChild<QDoubleSpinBox*>("sketch/angleStep");
      step->setValue(30);
      check(QSettings().value("sketch/angleStep").toDouble() == 30, "the angle step");
      dialog->grab().save(prefix + ".sketch.png");
      step->setValue(15);
      // The sketch panel's snaps and the status bar are faces of the same settings: each follows the others while open.
      QWidget* snaps = dialog->pageWidget("sketch");
      auto* gridRow = snaps->findChild<QCheckBox*>("view.gridSnap");
      const bool gridSnap = w.action("view.gridSnap")->isChecked();
      w.action("view.gridSnap")->trigger();
      check(gridRow && !snaps->findChild<QCheckBox*>("sketch/snap/grid") && gridRow->isChecked() != gridSnap &&
                dialog->pageWidget("grid")->findChild<QCheckBox*>("view.gridSnap")->isChecked() == gridRow->isChecked(),
            "the sketch page's Grid snapping is F9, as the Grid page's: one switch");
      w.action("view.gridSnap")->trigger();
      auto* endpoint = snaps->findChild<QCheckBox*>("sketch/snap/endpoint");
      auto* panelEndpoint = w.findChild<QCheckBox*>("snap-endpoint");
      auto* panelAngle = w.findChild<QCheckBox*>("snap-angle");
      auto* panelStep = w.findChild<QDoubleSpinBox*>("sketch-angleStep");
      auto* polarRow = snaps->findChild<QCheckBox*>("view.polarSnap");
      check(endpoint && panelEndpoint && panelAngle && panelStep && polarRow, "the sketch panel's snaps and the rows are there");
      if (endpoint && panelEndpoint && panelAngle && panelStep && polarRow) {
        const bool was = endpoint->isChecked();
        panelEndpoint->click();
        check(endpoint->isChecked() != was && QSettings().value("sketch/snap/endpoint").toBool() != was, "the sketch panel's Endpoints: the row follows");
        endpoint->click();
        check(panelEndpoint->isChecked() == was, "the row: the panel's check box follows");
        const bool polar = w.action("view.polarSnap")->isChecked();
        panelAngle->click();
        check(w.action("view.polarSnap")->isChecked() != polar && polarRow->isChecked() != polar, "the panel's Angle increments is Polar: the status bar and its row follow");
        w.action("view.polarSnap")->trigger();
        check(panelAngle->isChecked() == polar && polarRow->isChecked() == polar, "Polar (F10): the panel's check box follows");
        step->setValue(45);
        check(panelStep->value() == 45, "the angle step row: the panel's box follows");
        w.toggleMenu(w.statusBar()->findChild<QToolButton*>("toggle.view.polarSnap"), "view.polarSnap");
        QMenu* menu = nullptr;
        for (QWidget* top : QApplication::topLevelWidgets())
          if (auto* m = qobject_cast<QMenu*>(top); m && m->isVisible() && m->objectName() == "toggleMenu") menu = m;
        const QString half = units::compact(units::Kind::Angle, 22.5);
        for (QAction* a : menu ? menu->actions() : QList<QAction*>())
          if (QString(a->text()).remove('&') == half) a->trigger();
        if (menu) menu->close();
        check(step->value() == 22.5 && panelStep->value() == 22.5, "Polar's menu in the status bar (" + half + "): the row and the panel follow");
        step->setValue(15);
        QSettings().setValue("sketch/snap/midpoint", false);  // written by someone who says nothing
        QEvent activate(QEvent::WindowActivate);
        QCoreApplication::sendEvent(dialog, &activate);
        auto* midpoint = snaps->findChild<QCheckBox*>("sketch/snap/midpoint");
        check(midpoint && !midpoint->isChecked() && !w.findChild<QCheckBox*>("snap-midpoint")->isChecked(), "a setting written without a word shows once the window is activated");
        if (midpoint) midpoint->click();
      }
      // Recovery, files, keyboard.
      dialog->setPage("recovery");
      dialog->pageWidget("recovery")->findChild<QSpinBox*>("recovery/minutes")->setValue(7);
      check(QSettings().value("recovery/minutes").toInt() == 7, "recovery minutes");
      dialog->pageWidget("recovery")->findChild<QSpinBox*>("recovery/minutes")->setValue(2);
      dialog->setPage("files");
      auto* viewer = dialog->pageWidget("files")->findChild<QCheckBox*>("files/viewerMode");
      const bool opens = w.m_doc->viewerOpens;
      viewer->click();
      check(w.m_doc->viewerOpens != opens, "viewer mode reaches the document");
      viewer->click();
      dialog->setPage("keyboard");
      dialog->pageWidget("keyboard")->findChild<QRadioButton*>("nav.solidworks")->click();
      check(w.action("nav.solidworks")->isChecked() && w.m_viewport->navPreset() == Viewport::NavPreset::SolidWorks, "the navigation radio sets the preset");
      dialog->pageWidget("keyboard")->findChild<QRadioButton*>("nav.fusion")->click();
      // The old command leads to its row.
      dialog->setPage("display");
      w.action("tools.undodepth")->trigger();
      QWidget* focused = dialog->pageWidget("general")->findChild<QWidget*>("edit/undoDepth");
      check(dialog->page() == "general" && focused && focused->property("prefMatch").toBool(), "Undo history opens General at its row, marked");
    } catch (const std::exception& e) {
      check(false, QString::fromUtf8(e.what()));
    }
    // The cache size comes from a worker.
    auto* timer = new QTimer(&w);
    auto tries = std::make_shared<int>(0);
    QObject::connect(timer, &QTimer::timeout, &w, [=, &w] {
      auto* dialog = w.findChild<PreferencesDialog*>("preferences");
      auto* info = dialog ? dialog->pageWidget("files")->findChild<QLabel*>("files/cacheInfo") : nullptr;
      const bool measured = info && info->text() != QCoreApplication::translate("help", "Measuring…");
      if (!measured && ++*tries < 100) return;
      timer->stop();
      check(measured, "the cache size from a worker: " + (info ? info->text() : QString()));
      if (dialog) dialog->close();
      check(dialog && !dialog->isVisible(), "Close");
      finish();
    });
    timer->start(100);
  });
  return true;
}
