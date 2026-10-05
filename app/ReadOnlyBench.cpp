// OPAD_BENCH_READONLY=<prefix> on tools/bench_cases/vcs.py's read-only/model.opad, write-protected (UI-62: a version
// opened read-only from the history is such a copy, in another window started with --read-only). It shows itself read-only
// (title, chip and its Save a copy card, status path, browser without renames), takes view changes without becoming
// unsaved, asks for a copy before any edit (a command, a design tool, the timeline) and refuses edits and saving over the
// file underneath, shows lengths in another unit without changing the file. Save a copy writes on a worker and the copy is
// edited from then on, the file opened unchanged; a copy that cannot be written leaves the document as its file is (the step
// that moved the linked part's path for it taken back, not unsaved). Another OPAD started as the history starts it (VersionControl::
// readOnlyArguments, hidden, OPAD_BENCH_READONLY=child:<prefix>) opens the writable read-only/writable.opad read-only, and
// Edit unsaved copy makes it an unsaved document that can be edited. <prefix>.chips.png, .opened.png (read-only), .window.png.
#include <QAbstractButton>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QTimer>
#include <QToolButton>
#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "BrowserPanel.hpp"
#include "DesignController.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "StatusRow.hpp"
#include "TimelineWidget.hpp"
#include "Units.hpp"
#include "VersionControl.hpp"
#include "ViewportChips.hpp"

namespace {
// The message boxes that show (their text), pressing the button named in `press` on the next one.
class Answers : public QObject {
 public:
  using QObject::QObject;
  QStringList seen;
  QString press;
  bool eventFilter(QObject* o, QEvent* e) override {
    if (e->type() == QEvent::Show)
      if (auto* box = qobject_cast<QMessageBox*>(o)) {
        seen << box->text();
        if (!press.isEmpty())  // before BenchQuiet's Cancel (installed earlier, so called later)
          QTimer::singleShot(0, box, [box, text = std::exchange(press, QString())] {
            for (QAbstractButton* b : box->buttons())
              if (b->text() == text) return b->click();
          });
      }
    return false;
  }
};

void pollUntil(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(50);
}
}  // namespace

OPAD_BENCH(OPAD_BENCH_READONLY, read_only) {
  static bool started = false;
  if (std::exchange(started, true)) return true;  // the file it opens loads again and comes back here
  AppDocument* doc = w.m_doc;
  auto all = std::make_shared<bool>(true);
  auto require = [all](bool ok, const QString& what) {
    trace::log(QString("bench: read-only: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    *all = *all && ok;
    return ok;
  };
  auto finish = [all] { QCoreApplication::exit(*all ? 0 : 2); };
  auto chip = [&w] {
    bool label = false, card = false;
    for (QLabel* l : w.m_chips->findChildren<QLabel*>()) label = label || (l->text() == ViewportChips::tr("Read-only") && l->isVisibleTo(w.m_chips));
    for (QToolButton* b : w.m_chips->findChildren<QToolButton*>()) card = card || (b->text() == ViewportChips::tr("Save a copy to edit") && b->isVisibleTo(w.m_chips));
    return label && card;
  };
  auto shownReadOnly = [&w, doc, chip] {
    const QString file = doc->path(), name = QFileInfo(file).fileName();
    return doc->readOnly && !doc->browse && w.windowTitle() == AppDocument::tr("%1 (read-only) - OPAD").arg(name) && chip() &&
           w.m_statusPath->text().startsWith(MainWindow::tr("Read-only: %1").arg(QDir::toNativeSeparators(file))) &&
           w.m_browser->tree()->editTriggers() == QAbstractItemView::NoEditTriggers;
  };
  auto ownBody = [doc] {  // the box: the linked part's bodies take no renames
    for (const auto& id : doc->scene.all_bodies())
      if (!doc->node(id)->linked) return id;
    return std::string();
  };
  auto renames = [doc](const std::string& body) {
    try {
      doc->run("rename", {{"target", body}, {"name", "Renamed"}});
      return true;
    } catch (const std::exception&) {
      return false;
    }
  };
  if (value.startsWith("child:")) {  // started as the history's Open read-only starts another window
    const bool ok = shownReadOnly() && QFileInfo(doc->path()).isWritable() && doc->path().endsWith("writable.opad");
    require(ok, "child: opad --read-only on a writable document: read-only, the title, the chip and its card");
    finish();
    return true;
  }
  auto* answers = new Answers(&w);
  qApp->installEventFilter(answers);

  // ---- a write-protected document
  const QString file = doc->path();
  const QFileInfo opened(file);
  const qint64 size = opened.size();
  const QDateTime mtime = opened.lastModified();
  require(!opened.isWritable() && shownReadOnly(), "a write-protected document opens read-only: title, chip with Save a copy, status path, no renames");
  w.m_chips->grab().save(value + ".chips.png");
  w.grab().save(value + ".opened.png");
  if (ownBody().empty() || doc->scene.features.empty()) {
    require(false, "the case's document has a box feature");
    finish();
    return true;
  }
  const std::string body = ownBody(), feature = doc->scene.features.front().id;
  bool hid = true;
  try {
    doc->run("appearance", {{"target", body}, {"visible", false}});
  } catch (const std::exception&) {
    hid = false;
  }
  require(hid && !doc->node(body)->visible && !doc->isDirty() && !w.windowTitle().contains('*'), "hiding a body is a view change, never unsaved");
  const size_t ops = doc->doc.ops.size();
  QAction* rename = w.action("edit.rename");
  QAction* box = w.action("design.box");
  require(rename && box && rename->isEnabled() && box->isEnabled(), "edit commands stay offered");
  rename->trigger();
  box->trigger();
  emit w.m_timeline->opActivated(feature);
  const QString ask = MainWindow::tr("Save a copy to edit");
  require(answers->seen.count(ask) == 3 && doc->doc.ops.size() == ops && doc->readOnly && !w.m_design->featureActive() && !w.m_design->sketchActive(),
          QString("a command, a design tool and the timeline ask for a copy first (%1 asked), nothing changes").arg(answers->seen.count(ask)));
  int refused = 0;
  auto refuses = [&refused](const std::function<void()>& fn) {
    try {
      fn();
    } catch (const std::exception&) {
      ++refused;
    }
  };
  refuses([&] { doc->run("rename", {{"target", body}, {"name", "Renamed"}}); });
  refuses([&] { doc->save(); });
  refuses([&] { doc->saveAs(file); });
  refuses([&] { doc->saveAsync(w.m_jobs, file, true, [](bool, const QString&) {}); });
  require(refused == 4 && doc->doc.ops.size() == ops, "a rename, Save, Save As over the file and a save on a worker are refused underneath");
  w.setDocumentUnit("in");
  require(units::sessionUnit() == "in" && doc->doc.ops.size() == ops, "lengths in inches for the session, the file not changed");

  // ---- a copy that cannot be written (its folder is missing): the linked part's path step taken for it goes again
  {
    const QStringList steps = doc->undoLabels();
    int written = 0;  // 1 written, 2 not
    QEventLoop wait;
    QTimer::singleShot(30000, &wait, &QEventLoop::quit);
    try {
      doc->saveAsync(w.m_jobs, value + ".missing/copy.opad", false, [&written, &wait](bool ok, const QString&) {
        written = ok ? 1 : 2;
        wait.quit();
      });
      wait.exec();
    } catch (const std::exception&) {}
    require(written == 2 && doc->readOnly && doc->doc.ops.size() == ops && doc->undoLabels() == steps && !doc->isDirty() && !w.windowTitle().contains('*'),
            "a copy that is not written leaves the document as its file: read-only, no linked-paths step, not unsaved");
  }

  // ---- Save a copy: on a worker, then the copy is the document
  const QString copy = value + ".copy.opad";
  QFile::remove(copy);
  w.saveCopy(copy);
  pollUntil(&w, [doc, copy] { return !doc->readOnly && QFileInfo(doc->path()) == QFileInfo(copy) && !doc->snapshotBusy(); }, 30000,
            [=, &w](bool saved) {
    const QFileInfo after(file);
    size_t copied = 0;
    try {
      copied = opad::Document::load(std::filesystem::path(copy.toStdU16String())).ops.size();
    } catch (const std::exception&) {}
    require(saved && !chip() && w.windowTitle() == QFileInfo(copy).fileName() + " - OPAD" && units::sessionUnit().empty() &&
                copied == doc->doc.ops.size() && after.size() == size && after.lastModified() == mtime && !after.isWritable() &&
                doc->undoLabel() == AppDocument::tr("Linked file paths"),
            "Save a copy: written on a worker with the view change and the linked part's path moved (one step), the copy edited from then on "
            "in its own unit, the file opened unchanged");
    require(renames(body) && doc->isDirty() && w.windowTitle().contains('*') && w.m_browser->tree()->editTriggers() != QAbstractItemView::NoEditTriggers,
            "the copy takes edits");
    try {
      doc->save();
    } catch (const std::exception&) {}

    // ---- another OPAD, started as the history starts it, on a writable document
    const QString writable = QFileInfo(file).absolutePath() + "/writable.opad";
    auto* child = new QProcess(&w);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("OPAD_BENCH_READONLY", "child:" + value);
    env.insert("OPAD_TRACE", value + ".child.log");
    const QString settings = qEnvironmentVariable("OPAD_BENCH_SETTINGS");
    env.insert("OPAD_BENCH_SETTINGS", (settings.isEmpty() ? value : settings) + "-child");
    child->setProcessEnvironment(env);
#ifdef Q_OS_WIN
    child->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {  // hidden, as gui_benches starts this one
      a->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
      a->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
    QFile::remove(value + ".child.log");
    auto* limit = new QTimer(child);
    limit->setSingleShot(true);
    QObject::connect(limit, &QTimer::timeout, child, [child] { child->kill(); });  // the one started here, by its handle
    QObject::connect(child, &QProcess::finished, &w, [=, &w](int code, QProcess::ExitStatus status) {
      QFile log(value + ".child.log");
      const QString text = log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString();
      require(status == QProcess::NormalExit && code == 0 && text.contains("bench: read-only: child:") && text.contains("PASS") && !text.contains("FAIL"),
              QString("another OPAD started with --read-only (exit %1)").arg(code));
      child->deleteLater();
      // ---- the same here: openPath(…, true), then Edit unsaved copy
      w.openPath(writable, true);
      pollUntil(&w, [doc, writable] { return !doc->loading && QFileInfo(doc->path()) == QFileInfo(writable); }, 30000, [=, &w](bool loaded) {
        require(loaded && shownReadOnly() && QFileInfo(writable).isWritable(), "a writable document opened read-only here too");
        const std::string other = ownBody();
        answers->press = MainWindow::tr("Edit unsaved copy");
        bool resumed = false;
        const bool editable = w.requireEditable([&resumed] { resumed = true; });
        require(!editable && resumed && !doc->readOnly && doc->path().isEmpty() && !chip() && w.windowTitle().startsWith(AppDocument::tr("Untitled")) &&
                    renames(other) && doc->isDirty(),
                "Edit unsaved copy: an unsaved document of the same content, which takes edits");
        w.grab().save(value + ".window.png");
        finish();
      });
    });
    limit->start(120000);
    child->start(QCoreApplication::applicationFilePath(), VersionControl::readOnlyArguments(writable) << QStringLiteral("--bench-select"));
  });
  return true;
}
