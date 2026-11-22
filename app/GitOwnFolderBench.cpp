// OPAD_BENCH_OWN_FOLDER=<prefix> (tools/bench_cases/vcs.py, cases own-folder and own-folder-ar) on a saved document that
// shares its folder with other files, beside a folder of its own name (the move must take the next free name): Set up
// repository first asks "Create the repository in its own folder?" (GitWatch::askOwnFolder) with Move (the default), Use
// this folder and Cancel. Cancel changes nothing; Use this folder opens the Set up dialog for the folder as it is; with an
// unsaved change the question says it goes along, and Move saves the document into "<name> 2" next to it: the old file
// gone, every other file where it was, the change in the new file, the recent files entry and the Set up dialog on the new
// folder. Set up there makes the repository in the new folder only, and Set up repository asks no more. <prefix>.question.png
// and <prefix>.setup.png show the two dialogs.
#include <QAbstractButton>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <algorithm>
#include <memory>
#include <utility>

#include "AppDocument.hpp"
#include "AssetMonitor.hpp"
#include "AssetsArea.hpp"
#include "BenchRegistry.hpp"
#include "GitWatch.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Toast.hpp"
#include "opad/assets.hpp"

namespace {
const char* const kGitWatch = "GitWatch";  // the texts' context (tr in GitWatch)
QString gitText(const char* source) { return QCoreApplication::translate(kGitWatch, source); }
}  // namespace

OPAD_BENCH(OPAD_BENCH_OWN_FOLDER, own_folder) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  auto* git = w.findChild<GitWatch*>();
  struct State {
    int step = 0;
    bool ok = true;
    QString prefix, file, dir, folder, target;
    QStringList before;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  st->prefix = value;
  st->file = QFileInfo(w.m_doc->path()).absoluteFilePath();
  st->dir = QFileInfo(st->file).absolutePath();
  st->folder = QDir(st->dir).filePath(QFileInfo(st->file).completeBaseName() + " 2");  // "<name>" is taken
  st->target = QDir(st->folder).filePath(QFileInfo(st->file).fileName());
  st->before = QDir(st->dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
  st->before.removeAll(QFileInfo(st->file).fileName());
  st->clock.start();
  auto check = [st](bool pass, const QString& what) {
    trace::log(QStringLiteral("bench: own folder: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    st->ok = st->ok && pass;
  };
  if (!git || st->file.isEmpty()) {
    check(false, "a saved document and the git chip");
    QCoreApplication::exit(2);
    return true;
  }
  auto question = [&w]() -> QMessageBox* {
    for (auto* box : w.findChildren<QMessageBox*>("gitOwnFolder"))
      if (box->isVisible()) return box;
    return nullptr;
  };
  auto setupDialog = [&w]() -> QDialog* {
    for (auto* d : w.findChildren<QDialog*>("gitSetup"))
      if (d->isVisible()) return d;
    return nullptr;
  };
  auto button = [](QWidget* parent, const char* name) { return parent ? parent->findChild<QPushButton*>(QString::fromLatin1(name)) : nullptr; };
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [=, &w] {
    using S = git::Repo::State;
    auto finish = [&](bool pass) {
      timer->stop();
      trace::log(QStringLiteral("bench: own folder: Set up repository on a shared folder %1").arg(pass ? "PASS" : "FAIL"));
      QCoreApplication::exit(pass ? 0 : 2);
    };
    if (st->clock.elapsed() > 90000) {
      check(false, QStringLiteral("step %1 within 90 s").arg(st->step));
      return finish(false);
    }
    switch (st->step) {
      case 0: {  // the chip has read the folder: no repository yet
        if (!git->idle() || git->repo().state != S::NotRepo) return;
        git->setUp();
        st->step = 1;
        return;
      }
      case 1: {
        QMessageBox* box = question();
        if (!box) return;
        const QString info = box->informativeText();
        check(box->text() == gitText("Create the repository in its own folder?") && info.contains(QFileInfo(st->file).fileName()) &&
                  info.contains(QFileInfo(st->folder).fileName()) && !info.contains(gitText("Its unsaved changes are saved with it.")),
              "the question names the document and the new folder beside it, the next free name (" + QFileInfo(st->folder).fileName() + ")");
        check(button(box, "gitOwnFolderMove") && button(box, "gitOwnFolderHere") && button(box, "gitOwnFolderCancel") &&
                  box->defaultButton() == button(box, "gitOwnFolderMove"),
              "Move into its own folder (the default), Use this folder, Cancel");
        if (!st->prefix.isEmpty()) box->grab().save(st->prefix + ".question.png");
        button(box, "gitOwnFolderCancel")->click();
        st->step = 2;
        return;
      }
      case 2: {
        if (question()) return;
        check(!setupDialog() && QFileInfo::exists(st->file) && !QFileInfo::exists(st->folder) && QFileInfo(w.m_doc->path()) == QFileInfo(st->file),
              "Cancel: nothing moved, no Set up dialog");
        git->setUp();
        st->step = 3;
        return;
      }
      case 3: {
        QMessageBox* box = question();
        if (!box) return;
        button(box, "gitOwnFolderHere")->click();
        st->step = 4;
        return;
      }
      case 4: {
        QDialog* d = setupDialog();
        if (!d) return;
        auto* where = d->findChild<QLabel*>("gitSetupWhere");
        check(where && where->text().contains(QDir::toNativeSeparators(st->dir)) && !QFileInfo::exists(st->folder),
              "Use this folder: the Set up dialog for the folder as it is (" + (where ? where->text() : QString()) + ")");
        d->reject();
        st->step = 5;
        return;
      }
      case 5: {
        if (setupDialog()) return;
        const auto bodies = w.m_doc->scene.all_bodies();
        if (!bodies.empty()) w.m_doc->run("rename", opad::json{{"target", bodies.front()}, {"name", "Own folder body"}});
        check(w.m_doc->isDirty(), "an unsaved change");
        git->setUp();
        st->step = 6;
        return;
      }
      case 6: {
        QMessageBox* box = question();
        if (!box) return;
        check(box->informativeText().contains(gitText("Its unsaved changes are saved with it.")), "the question says the unsaved changes go along");
        button(box, "gitOwnFolderMove")->click();
        st->step = 7;
        return;
      }
      case 7: {
        QDialog* d = setupDialog();
        if (!d || !git->idle()) return;
        QStringList after = QDir(st->dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name);
        QStringList expected = st->before;
        expected << QFileInfo(st->folder).fileName();
        expected.sort(Qt::CaseInsensitive);
        after.sort(Qt::CaseInsensitive);
        check(QFileInfo(w.m_doc->path()) == QFileInfo(st->target) && QFileInfo::exists(st->target) && !QFileInfo::exists(st->file) && !w.m_doc->isDirty(),
              "Move: the document saved in " + QDir::toNativeSeparators(st->target) + ", the old file gone, nothing unsaved");
        check(after == expected, "every other file and folder where it was (" + after.join(", ") + ")");
        QFile saved(st->target);
        check(saved.open(QIODevice::ReadOnly) && saved.readAll().contains("Own folder body"), "the unsaved change is in the moved file");
        const QStringList recent = w.recent();
        check(!recent.isEmpty() && QFileInfo(recent.front()) == QFileInfo(st->target) &&
                  std::none_of(recent.begin(), recent.end(), [&](const QString& p) { return QFileInfo(p) == QFileInfo(st->file); }),
              "the recent files entry follows it");
        auto* where = d->findChild<QLabel*>("gitSetupWhere");
        check(where && where->text().contains(QDir::toNativeSeparators(st->folder)), "the Set up dialog is for the new folder");
        if (!st->prefix.isEmpty()) d->grab().save(st->prefix + ".setup.png");
        QPushButton* run = nullptr;
        for (auto* b : d->findChildren<QPushButton*>())
          if (b->property("action").toString() == "gitSetupRun") run = b;
        if (!run) {
          check(false, "the Set up button");
          return finish(false);
        }
        run->click();
        st->step = 8;
        return;
      }
      case 8: {
        if (!git->idle() || git->repo().state != S::Ready) return;
        check(QFileInfo(git->repo().top) == QFileInfo(st->folder) && QFileInfo::exists(st->folder + "/.git") && !QFileInfo::exists(st->dir + "/.git"),
              "the repository is the new folder's alone (" + QDir::toNativeSeparators(git->repo().top) + ")");
        git->setUp();  // a repository now: Set up OPAD in this repository, no question
        st->step = 9;
        return;
      }
      case 9: {
        QDialog* d = setupDialog();
        if (!d) return;
        check(!question(), "Set up repository asks no more in the document's own folder");
        d->reject();
        return finish(st->ok);
      }
    }
  });
  timer->start();
  return true;
}

// OPAD_BENCH_OWN_FOLDER_LINKED=copy|keep (cases own-folder-linked and own-folder-linked-keep) on model.opad linking part.step
// beside it, with notes.txt there too. The own-folder question offers "Also copy its 1 linked file into the new folder",
// checked. copy: Move copies part.step into model/assets/ and relinks it by "assets/part.step" (the original stays where it
// was); set up there, nothing is outside the repository. keep: unchecked, the link becomes "../part.step"; once set up, a
// toast says one linked file is outside the repository, and its Copy into the repository copies and relinks it the same way.
OPAD_BENCH(OPAD_BENCH_OWN_FOLDER_LINKED, own_folder_linked) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  auto* git = w.findChild<GitWatch*>();
  auto* assets = w.findChild<AssetsArea*>();
  struct State {
    int step = 0;
    bool ok = true, copy = true;
    QString dir, folder, part;
    std::string import;
    QElapsedTimer clock;
  };
  auto st = std::make_shared<State>();
  st->copy = value != "keep";
  st->dir = QFileInfo(w.m_doc->path()).absolutePath();
  st->folder = QDir(st->dir).filePath(QFileInfo(w.m_doc->path()).completeBaseName());
  st->part = QDir(st->dir).filePath("part.step");
  st->clock.start();
  auto check = [st](bool pass, const QString& what) {
    trace::log(QStringLiteral("bench: own folder linked: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    st->ok = st->ok && pass;
  };
  if (!git || !assets || !assets->monitor()) {
    check(false, "the git chip and the linked files area");
    QCoreApplication::exit(2);
    return true;
  }
  auto question = [&w]() -> QMessageBox* {
    for (auto* box : w.findChildren<QMessageBox*>("gitOwnFolder"))
      if (box->isVisible()) return box;
    return nullptr;
  };
  auto setupDialog = [&w]() -> QDialog* {
    for (auto* d : w.findChildren<QDialog*>("gitSetup"))
      if (d->isVisible()) return d;
    return nullptr;
  };
  auto recorded = [&w, st]() { return QString::fromStdString(opad::asset_of(w.m_doc->doc, st->import).value("path", std::string())); };
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [=, &w] {
    using S = git::Repo::State;
    auto finish = [&](bool pass) {
      timer->stop();
      trace::log(QStringLiteral("bench: own folder linked: its linked file copied into the repository %1").arg(pass ? "PASS" : "FAIL"));
      QCoreApplication::exit(pass ? 0 : 2);
    };
    if (st->clock.elapsed() > 90000) {
      check(false, QStringLiteral("step %1 within 90 s").arg(st->step));
      return finish(false);
    }
    switch (st->step) {
      case 0: {  // the chip and the linked file's state read
        if (!git->idle() || git->repo().state != S::NotRepo || assets->monitor()->checking() || assets->monitor()->assets().empty()) return;
        st->import = assets->monitor()->assets().begin()->first;
        check(recorded() == "part.step" && !opad::asset_of(w.m_doc->doc, st->import).contains("abs"), "linked beside the document by its relative path alone: " + recorded());
        git->setUp();
        st->step = 1;
        return;
      }
      case 1: {
        QMessageBox* box = question();
        if (!box) return;
        auto* copy = box->findChild<QCheckBox*>("gitOwnFolderCopy");
        check(copy && copy->isChecked() && copy->text().contains("1"), "the question offers to copy its 1 linked file, checked");
        if (copy && !st->copy) copy->setChecked(false);
        if (QPushButton* move = box->findChild<QPushButton*>("gitOwnFolderMove")) move->click();
        st->step = 2;
        return;
      }
      case 2: {
        QDialog* d = setupDialog();
        const QString want = st->copy ? "assets/part.step" : "../part.step";
        if (!d || !git->idle() || recorded() != want) return;
        check(QFileInfo(w.m_doc->path()).absolutePath() == QFileInfo(st->folder).absoluteFilePath(), "moved into " + QDir::toNativeSeparators(st->folder));
        check(QFileInfo::exists(st->part), "the original linked file stays where it was");
        check(QFileInfo::exists(st->folder + "/assets/part.step") == st->copy && !opad::asset_of(w.m_doc->doc, st->import).contains("abs"),
              st->copy ? "copied into the new folder's assets/, relinked by assets/part.step" : "not copied: linked by ../part.step");
        QPushButton* run = nullptr;
        for (auto* b : d->findChildren<QPushButton*>())
          if (b->property("action").toString() == "gitSetupRun") run = b;
        if (!run) return finish(false);
        run->click();
        st->step = 3;
        return;
      }
      case 3: {
        if (!git->idle() || git->repo().state != S::Ready || assets->monitor()->checking()) return;
        const auto out = assets->outside(git->repo().top);
        if (st->copy) {
          check(out.empty(), "set up: nothing is outside the repository");
          for (auto* t : w.findChildren<Toast*>()) check(!t->text().contains("outside the repository"), "no outside notice: " + t->text());
          return finish(st->ok);
        }
        check(out.size() == 1, QStringLiteral("set up: %1 linked file outside the repository").arg(out.size()));
        st->step = 4;
        return;
      }
      case 4: {  // the notice, then its action
        Toast* notice = nullptr;
        const QString action = QCoreApplication::translate(kGitWatch, "Copy into the repository");  // the context is not looked at
        for (auto* t : w.findChildren<Toast*>())
          if (t->isVisible() && t->actionButton() && t->actionButton()->text() == action) notice = t;
        if (!notice) return;
        check(true, "the notice: " + notice->text());
        notice->actionButton()->click();
        st->step = 5;
        return;
      }
      case 5: {
        if (recorded() != "assets/part.step") return;
        check(QFileInfo::exists(st->folder + "/assets/part.step") && QFileInfo::exists(st->part), "Copy into the repository: copied and relinked by assets/part.step");
        check(assets->outside(git->repo().top).empty() || assets->monitor()->checking(), "nothing outside the repository now");
        return finish(st->ok);
      }
    }
  });
  timer->start();
  return true;
}
