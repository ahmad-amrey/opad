// OPAD_BENCH_OWN_FOLDER=<prefix> (tools/bench_cases/vcs.py, cases own-folder and own-folder-ar) on a saved document that
// shares its folder with other files, beside a folder of its own name (the move must take the next free name): Set up
// repository first asks "Create the repository in its own folder?" (GitWatch::askOwnFolder) with Move (the default), Use
// this folder and Cancel. Cancel changes nothing; Use this folder opens the Set up dialog for the folder as it is; with an
// unsaved change the question says it goes along, and Move saves the document into "<name> 2" next to it: the old file
// gone, every other file where it was, the change in the new file, the recent files entry and the Set up dialog on the new
// folder. Set up there makes the repository in the new folder only, and Set up repository asks no more. <prefix>.question.png
// and <prefix>.setup.png show the two dialogs.
#include <QAbstractButton>
#include <algorithm>
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
#include <memory>
#include <utility>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "GitWatch.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"

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
