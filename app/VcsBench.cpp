// Version control benches (VcsArea.cpp): the file changed on disk while open, and the git chip with its dialogs.
#include <QCoreApplication>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <utility>

#include "BenchRegistry.hpp"
#include "DiskSync.hpp"
#include "GitWatch.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"

// OPAD_BENCH_EXTERNAL_CHANGE=<prefix> on a saved document with bodies (DiskSync::bench).
OPAD_BENCH(OPAD_BENCH_EXTERNAL_CHANGE, external_change) {
  auto* disk = w.findChild<DiskSync*>();
  return disk && disk->bench();
}

// OPAD_BENCH_GIT=<prefix> on a saved document outside any repository (GitWatch::bench); the clone's document it opens
// asks again and is told the bench runs. First the area's place in the window: Clone repository… in File after Open…,
// in the palette's File group and found by "git", the chip beside the document's path.
OPAD_BENCH(OPAD_BENCH_GIT, git) {
  auto* git = w.findChild<GitWatch*>();
  static bool placed = false;
  if (git && !std::exchange(placed, true)) {
    QStringList wrong;
    const CommandInfo* clone = w.m_commands.find("file.clone");
    if (!clone || clone->menuPath != "file" || clone->group != commands::defaultGroup("file.open") || !clone->keywords.contains("git") ||
        clone->editsDocument)
      wrong << "the file.clone record";
    QMenu* file = nullptr;
    for (QAction* a : w.menuBar()->actions())
      if (a->menu() && a->menu()->actions().contains(w.action("file.open"))) file = a->menu();
    const QList<QAction*> items = file ? file->actions() : QList<QAction*>();
    if (items.indexOf(w.action("file.clone")) != items.indexOf(w.action("file.open")) + 1) wrong << "Clone repository… right after Open…";
    QWidget* chip = git->chip();
    if (QLayout* layout = w.statusBar()->layout()) layout->activate();  // the chip was shown with the document just now
    // Both are the status bar's normal widgets: a message hides them together.
    if (chip->parentWidget() != w.statusBar() || chip->isHidden() != w.m_statusPath->isHidden() || (chip->x() > w.m_statusPath->x()) == w.isRightToLeft())
      wrong << QStringLiteral("the chip beside the path: %1, %2, x %3 after %4").arg(chip->parentWidget() == w.statusBar() ? "in the status bar" : "elsewhere",
                                                                                     chip->isHidden() ? "hidden" : "shown").arg(chip->x()).arg(w.m_statusPath->x());
    trace::log(QStringLiteral("bench: git: area: Clone repository… in File after Open…, the chip beside the path %1")
                   .arg(wrong.isEmpty() ? "PASS" : "FAIL (" + wrong.join(", ") + ")"));
    if (!wrong.isEmpty()) {
      QCoreApplication::exit(2);
      return true;
    }
  }
  return git && git->bench();
}
