// OPAD_BENCH_PATHS=<prefix> (UI-07) on tools/bench_cases/vcs.py's paths/doc/model.opad: a document in a git work tree
// with an STL imported from paths/parts. Open file location and Copy path from File (their records and keys) and on the
// ribbon (the File group of Review's and Design's Export tabs), the status
// path's menu (Copy relative path), the browser's document row (a real right click's menu), the import's timeline marker
// (its tooltip and menu, and again once the source file is gone: the nearest folder), the start page's and File > Recent's
// menus (a right click on a Recent entry never opens it; Remove from list). The file manager is never started: a
// launcher records what would run, and the clipboard is never taken: what would be copied is recorded. Pictures at
// <prefix>.<step>.png.
#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QMouseEvent>
#include <QToolButton>
#include <QTimer>
#include <QTreeWidget>
#include <memory>

#include "AppDocument.hpp"
#include "BenchMenus.hpp"
#include "BenchRegistry.hpp"
#include "BrowserPanel.hpp"
#include "FileLocation.hpp"
#include "MainWindow.hpp"
#include "Ribbon.hpp"
#include "StatusRow.hpp"
#include "TimelineWidget.hpp"
#include "Toast.hpp"

using namespace benchmenus;

OPAD_BENCH(OPAD_BENCH_PATHS, paths) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  const QString prefix = value;
  auto launched = std::make_shared<std::vector<location::Command>>();
  location::setLauncher([launched](const location::Command& c) {
    trace::log(QStringLiteral("bench: paths: would run %1 %2 (folder %3)").arg(c.program, c.arguments.join(' '), c.folder));
    launched->push_back(c);
    return true;
  });
  auto require = [](bool ok, const QString& why) {
    if (!ok) throw std::runtime_error(why.toStdString());
  };
  auto pass = [](const QString& what) { trace::log("bench: paths: " + what + " PASS"); };
  auto copied = std::make_shared<QString>();
  location::setCopier([copied](const QString& text) { *copied = text; });
  auto clip = [copied] { return *copied; };
  QTimer::singleShot(300, &w, [=, &w] {
    int code = 0;
    try {
      const QString doc = w.m_doc->path(), native = QDir::toNativeSeparators(doc);
      const QString top = QFileInfo(doc).absolutePath() + "/..";
      const QString stl = QDir::cleanPath(top + "/parts/bracket.stl");
      require(QFileInfo::exists(stl), "the case's STL is missing: " + stl);
      // File: the commands, their records and keys, after Save screenshot…
      w.updateCommands();
      QAction* reveal = w.action("file.reveal");
      QAction* copy = w.action("file.copyPath");
      require(reveal && copy && reveal->isEnabled() && copy->isEnabled(), "file.reveal / file.copyPath missing or disabled");
      const CommandInfo* info = w.m_commands.find("file.reveal");
      require(info && info->menuPath == "file" && info->key == QKeySequence("Shift+Alt+R") && reveal->shortcut() == QKeySequence("Shift+Alt+R") &&
                  w.m_commands.find("file.copyPath")->key == QKeySequence("Shift+Alt+C"),
              "the records (menu path, keys)");
      QMenu* file = nullptr;
      for (QAction* a : w.menuBar()->actions())
        if (a->menu() && a->menu()->actions().contains(w.action("file.open"))) file = a->menu();
      const QList<QAction*> items = file ? file->actions() : QList<QAction*>();
      require(items.indexOf(reveal) == items.indexOf(w.action("file.screenshot")) + 1 && items.indexOf(copy) == items.indexOf(reveal) + 1,
              "File: Open file location and Copy path after Save screenshot…");
      // The ribbon: Review > Share and Drafting > Output, their File group, and so both workspaces in the commands' records.
      for (const QString& tab : {QStringLiteral("review.share"), QStringLiteral("drafting.output")}) {
        RibbonPage* page = w.m_ribbon->page(tab);
        QList<QAction*> tools;
        for (RibbonGroup* g : page ? page->groups() : QList<RibbonGroup*>())
          if (g->title() == QCoreApplication::translate("MainWindow", "File"))
            for (QToolButton* b : g->buttons()) tools << b->defaultAction();
        require(tools.contains(reveal) && tools.contains(copy) && tools.indexOf(copy) == tools.indexOf(reveal) + 1, tab + ": no Open file location / Copy path in its File group");
      }
      require(info->workspaces.contains("review") && info->workspaces.contains("drafting") && w.m_commands.find("file.copyPath")->workspaces.contains("drafting"),
              "the records' workspaces: " + info->workspaces.join(' '));
      if (const int at = w.m_ribbon->tabIds().indexOf("review.share"); at >= 0 && !prefix.isEmpty()) {
        const int was = w.m_ribbon->currentTab();
        w.m_ribbon->setCurrentTab(at);
        QCoreApplication::processEvents();
        w.m_ribbon->grab().save(prefix + ".ribbon.png");
        w.m_ribbon->setCurrentTab(was);
      }
      pass("the ribbon: Open file location and Copy path in the File group of Review > Share and Drafting > Output");
      reveal->trigger();
      require(!launched->empty() && launched->back().selects && launched->back().arguments.contains(native) &&
                  QFileInfo(launched->back().program).fileName().compare("explorer.exe", Qt::CaseInsensitive) == 0,
              "File > Open file location runs Explorer with the document selected");
      copy->trigger();
      require(clip() == native, "Copy path: clipboard " + clip());
      require(!w.m_toasts->toasts().isEmpty() && w.m_toasts->toasts().last()->text().contains(native), "Copy path says so in a toast");
      pass("File > Open file location selects the document in Explorer, Copy path copies " + native + " and says so");
      // The status path's menu: Copy relative path inside the work tree.
      {
        std::unique_ptr<QMenu> m(w.m_statusPath->menu(&w));
        require(names(m.get()) == QStringList({"location.reveal", "location.copy", "location.copyRelative"}), "path menu: " + names(m.get()).join(' '));
        named(m.get(), "location.copyRelative")->trigger();
        require(clip() == "doc/model.opad", "Copy relative path: " + clip());
        if (!prefix.isEmpty()) w.statusBar()->grab().save(prefix + ".status.png");
      }
      pass("the status path's menu: Open file location, Copy path, Copy relative path (doc/model.opad)");
      // The browser's document row, as a right click there opens it.
      QTreeWidget* tree = w.m_browser->tree();
      QTreeWidgetItem* row = tree->topLevelItem(0);
      require(row && row->data(0, Qt::UserRole).toString() == "document", "no document row");
      QStringList seen;
      withPopup([&] { emit tree->customContextMenuRequested(tree->visualItemRect(row).center()); }, [&](QMenu* m) {
        seen = names(m);
        if (!prefix.isEmpty()) m->grab().save(prefix + ".document-menu.png");
        if (QAction* a = named(m, "location.copy")) a->trigger();
      });
      require(seen.contains("location.reveal") && seen.contains("location.copy") && clip() == native, "document row menu: " + seen.join(' '));
      pass("the browser's document row menu: Open file location, Copy path");
      // The import's marker: its source in the tooltip and the menu.
      std::string import;
      for (const auto& op : w.m_doc->doc.ops)
        if (op.type == "import") import = op.id;
      require(!import.empty(), "no import op");
      const QString nativeStl = QDir::toNativeSeparators(stl);
      require(w.m_timeline->tooltip(import).contains(nativeStl.toHtmlEscaped()), "import tooltip: " + w.m_timeline->tooltip(import));
      withPopup([&] { w.timelineMenu(import, w.m_timeline->mapToGlobal(QPoint(140, 20))); }, [&](QMenu* m) {
        seen = names(m);
        if (!prefix.isEmpty()) m->grab().save(prefix + ".timeline-menu.png");
        if (QAction* a = named(m, "location.reveal")) a->trigger();
      });
      require(seen.contains("location.copy") && launched->back().selects && launched->back().arguments.contains(nativeStl), "import marker menu: " + seen.join(' '));
      pass("the import marker: from " + nativeStl + " in its tooltip, its menu selects it in Explorer");
      // The source gone: said in the tooltip (looked up again after 2 s), the nearest folder shown.
      require(QFile::rename(stl, stl + ".moved"), "could not move the STL away");
      QTimer::singleShot(2100, &w, [=, &w] {
        int code = 0;
        try {
          const QString tip = w.m_timeline->tooltip(import);
          const QString nativeStl = QDir::toNativeSeparators(stl);
          require(tip.contains(QCoreApplication::translate("Locations", "from %1 (not found there now)").arg(nativeStl).toHtmlEscaped()), "tooltip of a gone source: " + tip);
          QStringList seen;
          withPopup([&] { w.timelineMenu(import, w.m_timeline->mapToGlobal(QPoint(140, 20))); }, [&](QMenu* m) {
            seen = names(m);
            if (!prefix.isEmpty()) m->grab().save(prefix + ".timeline-gone-menu.png");
            if (QAction* a = named(m, "location.reveal")) a->trigger();
          });
          require(!launched->back().selects && QFileInfo(launched->back().folder) == QFileInfo(QFileInfo(stl).absolutePath()) &&
                      w.m_toasts->toasts().last()->text() == QCoreApplication::translate("location", "%1 is no longer there; its nearest folder is shown").arg(nativeStl),
                  "a gone source: " + launched->back().folder + " / " + w.m_toasts->toasts().last()->text());
          pass("a source that is gone: said in the tooltip, its folder shown");
          // Recent files: the context menu on the start page's list and on File > Recent (a right click never opens).
          w.addRecent(stl + ".moved");
          w.addRecent(doc);
          const QList<RecentCard*> cards = w.m_empty->cards();
          require(cards.size() >= 2 && cards.front()->path() == doc, "the start page lists no recent files");
          {
            QMenu* m = w.m_empty->cardMenu(cards.front());  // what a right click on the card shows
            seen = names(m);
            if (!prefix.isEmpty()) m->grab().save(prefix + ".start-menu.png");
            delete m;
          }
          require(seen == QStringList({"recent.open", "location.reveal", "location.copy", "location.copyRelative", "recent.remove"}), "start page menu: " + seen.join(' '));
          QMenu* recent = w.m_recentMenu;
          QAction* entry = recent->actions().value(1);
          require(entry && entry->data().toString() == stl + ".moved", "File > Recent entries carry their paths");
          bool opened = false;
          QObject::connect(entry, &QAction::triggered, &w, [&opened] { opened = true; });
          recent->popup(QPoint(0, 0));
          QCoreApplication::processEvents();
          const QPoint at = recent->actionGeometry(entry).center();
          withPopup([&] {
                      for (QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
                        QMouseEvent e(type, at, recent->mapToGlobal(at), Qt::RightButton, type == QEvent::MouseButtonPress ? Qt::RightButton : Qt::NoButton, Qt::NoModifier);
                        QApplication::sendEvent(recent, &e);
                      }
                    },
                    [&](QMenu* m) {
                      seen = names(m);
                      if (!prefix.isEmpty()) m->grab().save(prefix + ".recent-menu.png");
                      if (QAction* a = named(m, "recent.remove")) a->trigger();
                    });
          QCoreApplication::processEvents();
          require(!opened && seen.contains("location.copy") && !w.recent().contains(stl + ".moved") && !recent->isVisible(),
                  QStringLiteral("File > Recent right click: opened %1, menu %2, still listed %3").arg(opened).arg(seen.join(' ')).arg(w.recent().contains(stl + ".moved")));
          require(w.m_empty->cards().size() == w.recent().size(), "the start page follows Remove from list");
          pass("recent files: the start page's and File > Recent's menus (a right click never opens the entry), Remove from list");
          QFile::rename(stl + ".moved", stl);
          trace::log("bench: paths: Open file location and Copy path everywhere PASS");
        } catch (const std::exception& e) {
          trace::log(QStringLiteral("bench: paths: %1 FAIL").arg(QString::fromUtf8(e.what())));
          code = 2;
        }
        location::setLauncher({});
        location::setCopier({});
        QCoreApplication::exit(code);
      });
      return;
    } catch (const std::exception& e) {
      trace::log(QStringLiteral("bench: paths: %1 FAIL").arg(QString::fromUtf8(e.what())));
      code = 2;
    }
    location::setLauncher({});
    location::setCopier({});
    QCoreApplication::exit(code);
  });
  return true;
}
