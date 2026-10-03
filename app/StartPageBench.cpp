// Bench of the start page (UI-113); the page is EmptyState.cpp.
#include "MainWindow.hpp"
#include "BenchRegistry.hpp"
#include "HelpReference.hpp"
#include "I18n.hpp"
#include <QApplication>
#include <QDragEnterEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QUrl>

#include "opad/cache.hpp"

// OPAD_BENCH_STARTPAGE=<prefix> (a document with a box): the start page with four recent files, the box document, a copy
// of it, an SVG drawing and one that is gone. Each card gets its picture from opad-cli on a worker (the drawing too) and
// the gone one is marked Missing, with Remove missing files offered; the pictures come from the cache the second time,
// without the CLI. A card's menu opens the file, its folder (Open file location) or copies the path, a gone one offers
// Locate… instead of Open; Remove from list drops a card and the File menu's entry. Cards are named for screen readers,
// Tab reaches them and the Menu key opens their menu. The templates: the built-in three, then Save as template puts a
// copy of the document in the templates folder (a new identity) and the page lists it; New from it opens an untitled copy
// with its own identity; Design in inches makes a document in inches, Sketch on a plane one in Design asking for the
// sketch's plane. Learn opens the tool guide; Clone shows only with
// its command. A file dropped on the page opens. Saved as <prefix>.page.png and <prefix>.empty.png (no recent files).
OPAD_BENCH(OPAD_BENCH_STARTPAGE, startpage) {
  static bool started = false;  // opening a template's copy and the dropped file finish loads, which ask the benches again
  if (std::exchange(started, true)) return true;
  const QString prefix = value;
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: start page: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  // Steps run in turn; a step returning false is asked again every 100 ms until it is done or its time is up.
  struct Step { int timeout; std::function<bool()> fn; QString what; };
  auto steps = std::make_shared<std::vector<Step>>();
  auto add = [steps](std::function<void()> fn) { steps->push_back({0, [fn] { fn(); return true; }, {}}); };
  auto until = [steps](int timeout, const QString& what, std::function<bool()> fn) { steps->push_back({timeout, std::move(fn), what}); };

  const QString document = w.m_doc->path();
  const QString dir = QFileInfo(prefix).absolutePath() + "/startpage-files";
  QDir(dir).removeRecursively();
  QDir().mkpath(dir);
  const QString copy = dir + "/Bracket copy.opad", drawing = dir + "/Plate.svg", gone = dir + "/Gone.opad";
  QFile::copy(document, copy);
  QFile svg(drawing);
  if (svg.open(QIODevice::WriteOnly))
    svg.write("<svg width=\"40mm\" viewBox=\"0 0 40 40\"><rect width=\"30\" height=\"16\"/><circle cx=\"8\" cy=\"8\" r=\"4\"/></svg>");
  svg.close();
  const QString pictures = QString::fromStdU16String((opad::cache_dir() / "start-page").u16string());
  QDir(pictures).removeRecursively();
  auto revealed = std::make_shared<QStringList>(), copied = std::make_shared<QStringList>();
  EmptyState::setFileActions([revealed](const QString& p) { *revealed << p; }, [copied](const QString& p) { *copied << p; });
  const QString templateFile = templates::folder() + "/Bench part.opad";
  QFile::remove(templateFile);
  auto clock = std::make_shared<QElapsedTimer>();
  auto card = [&w](const QString& path) -> RecentCard* {
    for (RecentCard* c : w.m_empty->cards())
      if (c->path() == path) return c;
    return nullptr;
  };

  add([=, &w] {
    w.saveTemplate(templateFile);  // a template of this document
  });
  until(10000, "the template", [=, &w] { return QFileInfo::exists(templateFile) && !w.m_doc->snapshotBusy() && !w.m_doc->designBusy; });
  add([=, &w] {
    const QStringList recent{document, copy, drawing, gone};
    w.m_settings.setValue("ui/recent", recent);
    w.m_empty->setRecent(recent);
    w.rebuildRecentMenu();
    clock->start();
    w.action("file.close")->trigger();
  });
  until(90000, "pictures", [=, &w] {
    return w.m_stack->currentWidget() == w.m_empty && w.m_empty->isVisible() && !w.m_empty->reading() && QFileInfo::exists(templateFile) &&
           card(document) && card(document)->state() != RecentCard::State::Unknown;
  });
  add([=, &w] {
    const qint64 first = clock->elapsed();
    QStringList states;
    for (RecentCard* c : w.m_empty->cards()) states << QString("%1 %2%3").arg(QFileInfo(c->path()).fileName()).arg(int(c->state())).arg(c->hasPicture() ? " picture" : "");
    check(w.m_empty->cards().size() == 4, "four recent files as cards (" + states.join(", ") + ")");
    check(card(document)->hasPicture() && card(copy)->hasPicture() && card(drawing)->hasPicture() && card(document)->state() == RecentCard::State::Present,
          QString("each file there has its picture, read on a worker in %1 ms").arg(first));
    check(card(gone)->state() == RecentCard::State::Missing && !card(gone)->hasPicture(), "the gone file is marked Missing");
    bool offered = false;
    for (QPushButton* b : w.m_empty->findChildren<QPushButton*>("startLinkSmall"))
      offered = offered || (b->text() == QCoreApplication::translate("EmptyState", "Remove missing files") && b->isVisibleTo(w.m_empty));
    check(offered, "Remove missing files is offered");
    check(card(document)->accessibleName() == QFileInfo(document).fileName() && card(document)->accessibleDescription().contains(QDir::toNativeSeparators(QFileInfo(document).absolutePath())) &&
              card(gone)->accessibleDescription().contains(QCoreApplication::translate("RecentCard", "Missing: the file is not there any more")),
          "cards are named for screen readers, the gone one says so (" + card(gone)->accessibleDescription() + ")");
    w.m_empty->grab().save(prefix + ".page.png");
    clock->start();
    w.m_empty->refresh();
  });
  until(5000, "cached pictures", [=, &w] { return !w.m_empty->reading(); });
  add([=, &w] {
    check(clock->elapsed() < 3000 && card(document)->hasPicture() && QDir(pictures).entryList({"*.png"}).size() >= 3,
          QString("the second time the pictures come from the cache (%1 ms)").arg(clock->elapsed()));
    // Menus: a file there, a gone one.
    QMenu* menu = w.m_empty->cardMenu(card(document));
    QStringList ids;
    for (QAction* a : menu->actions())
      if (!a->isSeparator()) ids << a->objectName();
    check(ids == QStringList({"recent.open", "recent.reveal", "recent.copy", "recent.remove"}), "a card's menu: Open, Open file location, Copy path, Remove from list (" + ids.join(' ') + ")");
    for (QAction* a : menu->actions())
      if (a->objectName() == "recent.reveal" || a->objectName() == "recent.copy") a->trigger();
    check(revealed->value(0) == document && copied->value(0) == document, "Open file location and Copy path get the file");
    delete menu;
    menu = w.m_empty->cardMenu(card(gone));
    ids.clear();
    for (QAction* a : menu->actions())
      if (!a->isSeparator()) ids << a->objectName();
    check(ids == QStringList({"recent.locate", "recent.copy", "recent.remove"}), "a gone file's menu: Locate…, Copy path, Remove from list (" + ids.join(' ') + ")");
    delete menu;
    // Remove from list.
    menu = w.m_empty->cardMenu(card(copy));
    for (QAction* a : menu->actions())
      if (a->objectName() == "recent.remove") a->trigger();
    delete menu;
    QStringList fileMenu;
    for (QAction* a : w.m_recentMenu->actions()) fileMenu << a->text();
    check(!card(copy) && w.m_empty->cards().size() == 3 && !w.recent().contains(copy) && !fileMenu.contains(copy), "Remove from list drops the card and the File menu's entry");
    for (QPushButton* b : w.m_empty->findChildren<QPushButton*>("startLinkSmall"))
      if (b->text() == QCoreApplication::translate("EmptyState", "Remove missing files")) b->click();
    check(!card(gone) && !w.recent().contains(gone) && w.recent().contains(document), "Remove missing files drops the gone one only");
    // The keyboard: Tab reaches a card, the Menu key opens its menu.
    w.activateWindow();
    card(document)->setFocus(Qt::TabFocusReason);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Menu, Qt::NoModifier);
    QApplication::sendEvent(card(document), &key);
  });
  until(2000, "the card's menu", [=, &w] {
    for (QMenu* m : w.m_empty->findChildren<QMenu*>("recentMenu"))
      if (m->isVisible()) return true;
    return false;
  });
  // The page hidden (a document opened, the window closed) while it renders: the reading and its thumbnailer stop.
  add([=, &w] {
    for (QMenu* m : w.m_empty->findChildren<QMenu*>("recentMenu")) m->close();
    QDir(pictures).removeRecursively();
    w.m_empty->refresh();
    check(w.m_empty->reading(), "without the cache the page renders its pictures again");
    w.m_stack->setCurrentIndex(1);
  });
  until(500, "the reading to stop", [=, &w] { return !w.m_jobs->titles().contains(QCoreApplication::translate("EmptyState", "Reading recent files")); });
  add([=, &w] {
    check(!w.m_empty->isVisible() && !w.m_empty->reading(), "hiding the page stops its reading and the thumbnailer");
    w.m_stack->setCurrentIndex(0);
    check(w.m_empty->reading(), "shown again, it reads again");
  });
  until(90000, "the pictures again", [=, &w] { return !w.m_empty->reading() && QDir(pictures).entryList({"*.png"}).size() >= 2; });
  add([=, &w] {
    for (QMenu* m : w.m_empty->findChildren<QMenu*>("recentMenu")) m->close();
    check(true, "the Menu key opens a card's menu");
    // Templates and Learn.
    QStringList titles;
    for (QPushButton* b : w.m_empty->templateButtons()) titles << b->text();
    check(titles.size() >= 4 && titles[0] == QCoreApplication::translate("EmptyState", "Design in millimetres") && titles[1] == QCoreApplication::translate("EmptyState", "Design in inches") &&
              titles[2] == QCoreApplication::translate("EmptyState", "Sketch on a plane") && titles.contains("Bench part"),
          "templates: the built-in three, then the saved one (" + titles.join(", ") + ")");
    QStringList learn;
    for (QPushButton* b : w.m_empty->learnButtons()) learn << b->text();
    check(learn.size() == 3 && learn[0] == w.action("help.start")->text().remove('&') && learn[1] == w.action("help.reference")->text().remove('&'), "Learn: " + learn.join(", "));
    check(w.m_empty->cloneButton()->isHidden() == !w.action("file.clone"), "Clone repository only with its command");
    w.m_empty->learnButtons().value(1)->click();
    auto* reference = w.findChild<CommandReference*>();
    check(reference && reference->isVisible(), "Tool guide opens from Learn");
    if (reference) reference->close();
    if (i18n::current() != "en")
      check(w.m_empty->layoutDirection() == Qt::RightToLeft && titles[0] != "Design in millimetres", "the page in the UI's language and direction");
    for (QPushButton* b : w.m_empty->templateButtons())
      if (b->text() == "Bench part") b->click();
  });
  until(30000, "the copy of the template", [=, &w] { return w.m_doc->hasDocument && !w.m_doc->loading && !w.m_loadJob; });
  add([=, &w] {
    std::string uuid;
    try { uuid = opad::Document::load(std::filesystem::path(templateFile.toStdU16String())).header.uuid; } catch (...) {}
    const std::string source = opad::Document::load(std::filesystem::path(document.toStdU16String())).header.uuid;
    check(!uuid.empty() && uuid != source, "Save as template wrote a copy with an identity of its own");
    check(w.m_doc->path().isEmpty() && w.m_doc->doc.header.uuid != uuid && w.m_doc->scene.all_bodies().size() == 1 && !w.recent().contains(templateFile) &&
              w.windowTitle().startsWith(QCoreApplication::translate("AppDocument", "Untitled")),
          "New from it: an untitled copy with its own identity, not a recent file (" + w.windowTitle() + ")");
    w.action("file.close")->trigger();
  });
  until(3000, "the start page again", [=, &w] { return w.m_stack->currentWidget() == w.m_empty; });
  add([=, &w] {
    for (QPushButton* b : w.m_empty->templateButtons())
      if (b->text() == QCoreApplication::translate("EmptyState", "Design in inches")) b->click();
  });
  until(10000, "the document in inches", [=, &w] { return w.m_doc->hasDocument && w.m_doc->scene.units == "in" && !w.m_doc->designBusy; });
  add([=, &w] {
    check(w.m_doc->scene.units == "in" && w.m_doc->path().isEmpty(), "Design in inches: a new document in inches");
    w.action("file.close")->trigger();
    for (QPushButton* b : w.m_empty->templateButtons())
      if (b->text() == QCoreApplication::translate("EmptyState", "Sketch on a plane")) b->click();
  });
  until(10000, "the sketch's plane pick", [=, &w] { return w.m_doc->hasDocument && w.m_design->pickingPlane(); });
  add([=, &w] {
    check(w.workspaceId() == "design" && w.m_design->pickingPlane() && w.m_doc->path().isEmpty(), "Sketch on a plane: a new document in Design, asking for the plane");
    w.m_design->escape();
    w.action("file.close")->trigger();
    w.m_empty->setRecent({});
    w.m_empty->grab().save(prefix + ".empty.png");
    check(w.m_empty->dropZone()->isVisibleTo(w.m_empty) && w.m_empty->cards().isEmpty(), "no recent files: the drop zone and the View / Edit cards");
    // A file dropped on the page opens.
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(document)});
    QDragEnterEvent enter(QPoint(40, 40), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w.m_empty, &enter);  // a drop goes where the drag entered
    check(enter.isAccepted(), "the page takes a dragged file");
    QDropEvent drop(QPointF(40, 40), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w.m_empty, &drop);
  });
  until(30000, "the dropped file", [=, &w] { return w.m_doc->hasDocument && !w.m_doc->loading && !w.m_loadJob; });
  add([=, &w] {
    check(QFileInfo(w.m_doc->path()) == QFileInfo(document), "a file dropped on the page opens");
    EmptyState::setFileActions({}, {});
    QFile::remove(templateFile);
    trace::log(QString("bench: start page: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  auto next = std::make_shared<std::function<void(size_t, int)>>();
  *next = [&w, steps, next, check, failed](size_t i, int waited) {
    if (i >= steps->size()) return;
    QTimer::singleShot(waited == 0 ? 300 : 100, &w, [&w, steps, next, check, failed, i, waited] {
      bool done = true;
      try { done = (*steps)[i].fn(); } catch (const std::exception& e) { check(false, QString::fromUtf8(e.what())); }
      if (!done && waited < (*steps)[i].timeout) return (*next)(i, waited + 100);
      if (!done) {
        check(false, QString("waited too long for %1 (document %2, loading %3, design busy %4, load job %5, start page %6)").arg((*steps)[i].what).arg(w.m_doc->hasDocument)
                         .arg(w.m_doc->loading).arg(w.m_doc->designBusy).arg(bool(w.m_loadJob)).arg(w.m_stack->currentWidget() == w.m_empty));
        trace::log(QString("bench: start page: FAIL: %1").arg(failed->join("; ")));
        return QCoreApplication::exit(2);
      }
      (*next)(i + 1, 0);
    });
  };
  (*next)(0, 0);
  return true;
}
