// OPAD_BENCH_VERSION=<prefix> (VersionControl::bench): the Version control panel and its commands on a document in a
// repository of its own with a bare remote (tools/bench_cases/vcs.py "version"), git run from outside only as another
// clone would (someone else's push). Pictures at <prefix>.<step>.png.
#include <QAbstractButton>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <memory>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "BenchRegistry.hpp"
#include "CompareMode.hpp"
#include "GitWatch.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "PanelFooter.hpp"
#include "Toast.hpp"
#include "ToolPanel.hpp"
#include "VersionControl.hpp"
#include "VersionPanel.hpp"
#include "Viewport.hpp"

OPAD_BENCH(OPAD_BENCH_VERSION, version) {
  auto* vc = w.findChild<VersionControl*>();
  return vc && vc->bench(value);
}

bool VersionControl::bench(const QString& prefix) {
  static bool started = false;
  if (std::exchange(started, true)) return true;
  m_benching = true;
  QString cli = qEnvironmentVariable("OPAD_BENCH_CLI");
  if (cli.isEmpty()) cli = git::Install::here().cli;
  struct State {
    size_t step = 0;
    int wait = 0, exit = 0, ops = 0;
    bool running = false;
    QString file, dir, error, current, other, first, boxBody, merged;
    std::vector<QStringList> pending;
    std::function<void()> next;
    QElapsedTimer since;
  };
  auto st = std::make_shared<State>();
  AppDocument* doc = m_services.document();
  QWidget* window = m_services.window();
  st->file = doc->path();
  st->dir = QFileInfo(st->file).absolutePath();
  st->other = st->dir + "-other";
  using D = git::Repo::Doc;
  using Y = git::Repo::Sync;
  auto require = [](bool ok, const QString& why) {
    if (!ok) throw std::runtime_error(why.toStdString());
  };
  auto pass = [](const QString& what) { trace::log("bench: version: " + what + " PASS"); };
  auto shot = [prefix](QWidget* widget, const QString& suffix) {
    if (widget) widget->grab().save(prefix + suffix);
  };
  auto idle = [this, doc] { return settled() && m_git->idle() && !doc->loading && !doc->designBusy && !doc->snapshotBusy(); };
  auto finished = [this, st](const QString& what) {  // the last command of ours ended this way
    if (m_lastDone == what + " failed") throw std::runtime_error((what + " failed: " + m_lastFailure).toStdString());
    return m_lastDone == what + " ok";
  };
  auto dialog = [window](const char* name) -> QDialog* {
    for (QDialog* d : window->findChildren<QDialog*>(QString::fromLatin1(name)))
      if (d->isVisible()) return d;
    return nullptr;
  };
  auto press = [](QWidget* parent, const char* action) {
    for (QPushButton* b : parent->findChildren<QPushButton*>())
      if (b->property("action") == action && b->isEnabled()) {
        b->click();
        return true;
      }
    return false;
  };
  // A question of ours, answered at once: a bench closes every message box as soon as it shows (MainWindowBench.cpp).
  auto answer = [window](const char* box, const char* id) {
    for (QMessageBox* m : window->findChildren<QMessageBox*>(QString::fromLatin1(box)))
      if (m->isVisible())
        for (QAbstractButton* b : m->buttons())
          if (b->property("answer") == id) {
            b->click();
            return true;
          }
    return false;
  };
  auto toast = [this](const QString& text) {
    for (const QString& said : m_said)
      if (said.contains(text)) return true;
    return false;
  };
  auto withAction = [this](const QString& action) {
    for (Toast* t : m_services.viewport()->findChildren<Toast*>())
      if (t->isVisible() && t->actionButton() && t->actionButton()->text() == action) return t;
    return static_cast<Toast*>(nullptr);
  };
  auto run = [this](const QStringList& args) { return git::run(m_git->context(), args); };  // a quick local read
  auto fileOps = [st] { return int(opad::Document::load(std::filesystem::path(st->file.toStdU16String()), [](const std::string&) { return true; }).ops.size()); };
  auto box = [doc](int x) {
    doc->run("feature", {{"kind", "box"}, {"inputs", {{"x", std::to_string(x) + " mm"}, {"length", "5 mm"}, {"width", "5 mm"}, {"height", "5 mm"}}}});
  };
  auto item = [](QTreeWidget* tree, const QString& text, int column) -> QTreeWidgetItem* {
    for (int i = 0; i < tree->topLevelItemCount(); ++i)
      if (tree->topLevelItem(i)->text(column).contains(text)) return tree->topLevelItem(i);
    return nullptr;
  };
  auto commitDialog = [this, dialog, press](const QString& text) {  // Commit… with this message, pressed
    m_services.action("vcs.commit")->trigger();
    QDialog* d = dialog("vcsCommit");
    if (!d) throw std::runtime_error("the commit dialog");
    d->findChild<QPlainTextEdit*>("message")->setPlainText(text);
    if (!press(d, "vcsCommitRun")) throw std::runtime_error("Commit enabled");
  };
  auto gitArgs = [this](const QStringList& args) {
    return QStringList{m_git->repo().program, "-c", "user.name=Someone Else", "-c", "user.email=else@example.com"} + args;
  };
  // Commands one after another as typed in a terminal (another clone of the remote); the first failure stops the rest.
  auto external = [this, st](std::vector<QStringList> commands) {
    st->pending = std::move(commands);
    st->running = true;
    st->exit = 0;
    st->next = [this, st] {
      if (st->pending.empty() || st->exit != 0) {
        st->running = false;
        return;
      }
      const QStringList c = st->pending.front();
      st->pending.erase(st->pending.begin());
      st->current = QFileInfo(c.front()).completeBaseName() + ' ' + c.mid(1).join(' ');
      st->since.start();
      auto* p = new QProcess(this);
      p->setWorkingDirectory(QFileInfo(st->other).absolutePath());
      connect(p, &QProcess::finished, this, [st, p](int code) {
        if (trace::enabled()) trace::log(QStringLiteral("version: outside, %1: %2 in %3 ms").arg(st->current).arg(code).arg(st->since.elapsed()));
        st->exit = code;
        if (code) st->error = st->current + ": " + QString::fromUtf8(p->readAllStandardError());
        p->deleteLater();
        st->next();
      });
      connect(p, &QProcess::errorOccurred, this, [st, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        st->exit = -1;
        st->error = st->current + ": " + p->errorString();
        p->deleteLater();
        st->next();
      });
      p->start(c.front(), c.mid(1));
    };
    st->next();
  };
  auto outside = [st] {
    if (st->running) return false;
    if (st->exit != 0) throw std::runtime_error(st->error.toStdString());
    return true;
  };
  std::vector<std::function<bool()>> steps;
  // OPAD_BENCH_VERSION=perf:<prefix> on a big committed document (the Engine): how long each command takes, with nothing
  // on the UI thread meanwhile (the trace's stall lines). git's author comes from GIT_CONFIG_GLOBAL.
  auto clock = std::make_shared<QElapsedTimer>();
  auto timed = [clock](const QString& what) { trace::log(QStringLiteral("bench: version: perf: %1 in %2 ms PASS").arg(what).arg(clock->elapsed())); };
  if (prefix.startsWith("perf:")) steps = {
      [=, this] {
        if (!idle() || m_git->repo().state != git::Repo::State::Ready || doc->loading) return false;
        if (m_git->repo().needsDriver()) m_git->setUpDriver();
        clock->start();
        m_services.action("vcs.panel")->trigger();
        return true;
      },
      [=, this] {
        if (!idle() || m_history.empty()) return false;
        timed(QStringLiteral("the panel's history and branches (%1 commits)").arg(m_history.size()));
        const auto bodies = doc->scene.all_bodies();
        doc->run("transform", {{"target", bodies.front()}, {"matrix", {1, 0, 0, 10, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}});
        clock->start();
        m_services.action("vcs.commit")->trigger();
        return true;
      },
      [=, this] {
        QDialog* d = dialog("vcsCommit");
        require(d, "the commit dialog");
        if (d->findChild<QLabel*>("suggestion")->property("state") == "reading") return false;
        timed("the commit message suggested: \"" + d->findChild<QPlainTextEdit*>("message")->toPlainText() + "\"");
        clock->start();
        require(press(d, "vcsCommitRun"), "Commit");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("commit")) return false;
        timed("saved and committed");
        std::unique_ptr<QMenu> menu(m_panel->commitMenu(m_history.back()));
        clock->start();
        menu->findChild<QAction*>("vcs.restore")->trigger();
        require(answer("vcsRestore", "restore"), "Restore");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("restore")) return false;
        timed(QStringLiteral("the first commit restored as new changes (%1 ops)").arg(doc->doc.ops.size()));
        doc->undo();
        clock->start();
        m_services.action("vcs.pack")->trigger();
        return true;
      },
      [=, this] {
        if (!idle() || !finished("pack")) return false;
        timed("packed");
        return true;
      },
  };
  else steps = {
      [=, this] {  // the clone's merge driver (the case committed .gitattributes asking for it)
        if (!idle() || m_git->repo().state != git::Repo::State::Ready) return false;
        require(m_git->repo().needsDriver(), "the driver not set up yet");
        m_git->setUpDriver();
        return true;
      },
      [=, this] {
        if (!idle() || m_git->repo().needsDriver()) return false;
        // The area's commands: in File > Version control, the palette's group, the chip's menu.
        require(m_services.action("vcs.panel") && m_services.action("vcs.commit") && m_services.action("vcs.pull") && m_services.action("vcs.push"), "the commands");
        require(m_services.commands().find("vcs.commit")->group == tr("Version control"), "the Version control group");
        std::unique_ptr<QMenu> chip(m_git->menu(window));
        QStringList names;
        for (QAction* a : chip->actions()) names << a->objectName();
        require(names.mid(0, 4) == QStringList({"vcs.panel", "vcs.commit", "vcs.pull", "vcs.push"}), "the chip's menu starts with them: " + names.join(", "));
        pass("commands in File, the palette and the chip's menu");
        m_services.action("vcs.panel")->trigger();
        return true;
      },
      [=, this] {
        if (!idle() || !m_tool || !m_tool->isVisible() || m_history.empty()) return false;
        require(m_panel->showsRepository() && m_panel->branchLabel()->text() == "main", "the panel: on main");
        require(m_panel->documentLabel()->text().contains(tr("committed")), "the document committed");
        require(m_history.size() == 1 && m_history[0].subject == "first" && m_panel->history()->topLevelItemCount() == 1, "the history: one commit");
        require(m_panel->footer()->primaryText() == tr("Commit…"), "the footer's Commit…");
        require(m_branches.size() == 1 && m_remotes == QStringList{"origin"}, "one branch, the remote origin");
        st->first = m_history[0].hash;
        shot(m_tool, ".panel.png");
        pass("the panel: branch, document, history");
        box(40);
        require(doc->isDirty() && m_panel->documentLabel()->property("state") == "unsaved", "unsaved changes shown");
        m_services.action("vcs.commit")->trigger();
        return true;
      },
      [=, this] {  // Commit…: the message from the semantic diff, saved first, the author asked
        QDialog* d = dialog("vcsCommit");
        require(d, "the commit dialog");
        auto* suggestion = d->findChild<QLabel*>("suggestion");
        if (suggestion->property("state") == "reading") return false;
        auto* files = d->findChild<QListWidget*>("files");
        const QString message = d->findChild<QPlainTextEdit*>("message")->toPlainText();
        require(suggestion->property("state") == "suggested", "a suggestion: " + suggestion->text());
        require(files->count() >= 1 && files->item(0)->data(Qt::UserRole) == "model.opad" && files->item(0)->checkState() == Qt::Checked &&
                    files->item(0)->text().contains(tr("(unsaved changes: saved first)")),
                "the document ticked, saved first");
        require(message.contains("Box") && message != tr("Update %1").arg("model.opad"), "the message names the change: " + message);
        require(d->findChild<QCheckBox*>("amend")->isEnabled() && d->findChild<QCheckBox*>("push")->isEnabled(), "amend (not pushed yet) and push offered");
        st->merged = message;
        shot(d, ".commit.png");
        pass("commit dialog: \"" + message + "\" suggested");
        require(press(d, "vcsCommitRun"), "Commit");
        return true;
      },
      [=, this] {  // UI-136: no author yet: asked first
        QDialog* d = dialog("gitIdentity");
        if (!d) return false;
        d->findChild<QLineEdit*>("name")->setText("OPAD Bench");
        d->findChild<QLineEdit*>("email")->setText("bench@example.com");
        require(press(d, "gitIdentitySave"), "Save the author");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("commit") || m_history.size() != 2) return false;
        require(!doc->isDirty() && m_git->repo().doc() == D::Clean, "saved and committed");
        const git::Result log = run({"log", "-2", "--format=%an|%s"});
        require(log.ok() && QString::fromUtf8(log.out).startsWith("OPAD Bench|" + st->merged + "\nbench|first"), "git log: " + QString::fromUtf8(log.out));
        require(m_history.size() == 2 && m_panel->history()->topLevelItemCount() == 2, "the history read again");
        require(toast(tr("Committed %1: %2").arg(m_history[0].shortHash, st->merged)), "the Committed toast");
        pass("committed, saved first, the author asked");
        m_services.action("vcs.push")->trigger();
        return true;
      },
      [=, this] {  // Push: -u origin main the first time
        if (!idle() || !finished("push") || m_git->repo().status.upstream.isEmpty()) return false;
        const git::Status& s = m_git->repo().status;
        require(s.upstream == "origin/main" && m_git->repo().sync() == Y::Synced, "following origin/main, in step: " + s.upstream);
        require(m_panel->syncLabel()->text().contains(tr("follows %1").arg("origin/main")), "the panel says so");
        require(toast(tr("Pushed %1 to %2: it follows %2/%1 from now on").arg("main", "origin")), "the Pushed toast");
        pass("pushed with -u origin main");
        require(press(m_panel, "vcsNewBranch"), "New branch…");
        QDialog* d = dialog("vcsNewBranch");
        require(d, "the new branch dialog");
        auto* name = d->findChild<QLineEdit*>("name");
        name->setText("bad name");
        require(!d->findChild<QLabel*>("problem")->text().isEmpty() && !press(d, "vcsNewBranchRun"), "a bad name refused");
        name->setText("feature/arm");
        shot(d, ".branch.png");
        require(press(d, "vcsNewBranchRun"), "Create");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("switch") || m_git->repo().status.branch != "feature/arm") return false;
        pass("a new branch, switched to");
        box(80);
        commitDialog("Longer arm");
        return true;
      },
      [=, this] {  // back to main: the document as main has it comes in by itself
        if (!idle() || !finished("commit") || m_branches.size() < 3) return false;
        st->ops = int(doc->doc.ops.size());
        m_panel->setPage(Branches);
        QTreeWidgetItem* main = item(m_panel->branches(), "main", 0);
        require(main && m_panel->branchOf(main) && !m_panel->branchOf(main)->remote, "main in the list");
        m_panel->branches()->setCurrentItem(main);
        shot(m_tool, ".branches.png");
        require(press(m_panel, "vcsSwitch"), "Switch");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("switch") || m_git->repo().status.branch != "main" || int(doc->doc.ops.size()) == st->ops) return false;
        require(!doc->isDirty() && int(doc->doc.ops.size()) == fileOps() && int(doc->doc.ops.size()) < st->ops, "main's document reloaded");
        require(toast(tr("Switched to %1").arg("main")), "the Switched toast");
        pass("switched back to main: its document came in");
        QTreeWidgetItem* arm = item(m_panel->branches(), "feature/arm", 0);
        require(arm, "feature/arm in the list");
        m_panel->branches()->setCurrentItem(arm);
        require(press(m_panel, "vcsMerge"), "Merge into current…");
        return true;
      },
      [=, this] {  // the incoming preview, then Compare on it
        if (!idle()) return false;
        QDialog* d = dialog("vcsIncoming");
        require(d, "the incoming dialog");
        require(d->findChild<QLabel*>("incomingHead")->text().contains(tr("%1 has nothing they lack: it moves up to them (fast-forward).").arg("main")), "fast-forward said");
        require(d->findChild<QTreeWidget*>("commits")->topLevelItemCount() == 1, "one commit comes in");
        require(d->findChild<QTreeWidget*>("incomingChanges") && d->findChild<QTreeWidget*>("incomingChanges")->topLevelItemCount() > 0, "the changes listed");
        shot(d, ".incoming.png");
        pass("merge preview: " + d->findChild<QLabel*>("incomingDocument")->text());
        require(press(d, "vcsIncomingCompare"), "Preview in Compare");
        return true;
      },
      [=, this] {
        if (!m_compare->active() || !m_compare->settled()) return false;
        require(m_compare->versions().size() >= 2, "versions");
        bool after = false;
        for (const auto& v : m_compare->versions()) after = after || v.label == tr("After merging %1").arg("feature/arm");
        require(after, "Compare shows the document after the merge");
        shot(m_compare->toolPanel(), ".incoming-compare.png");
        pass("the merge previewed in Compare");
        require(press(dialog("vcsIncoming"), "vcsIncomingMerge"), "Merge");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("merge") || int(doc->doc.ops.size()) != fileOps()) return false;
        require(!m_compare->active() && !dialog("vcsIncoming"), "the preview closed");
        require(int(doc->doc.ops.size()) == st->ops && !doc->isDirty() && doc->scene.all_bodies().size() == 3, "feature/arm's box came in");
        require(m_git->repo().sync() == Y::Ahead, "main ahead of origin/main");
        pass("merged feature/arm (fast-forward): the document took it in");
        QTreeWidgetItem* arm = item(m_panel->branches(), "feature/arm", 0);
        require(arm, "feature/arm in the list");
        m_panel->branches()->setCurrentItem(arm);
        require(press(m_panel, "vcsDelete"), "Delete…");
        require(answer("vcsDeleteBranch", "delete"), "Delete asked first");
        return true;
      },
      [=, this] {  // someone else pushes a box of their own
        if (!idle() || !finished("delete") || item(m_panel->branches(), "feature/arm", 0)) return false;
        pass("a merged branch deleted");
        QStringList feature{cli, "feature", st->other + "/model.opad", "--kind", "box", "--inputs", R"({"x":"120 mm","length":"5 mm","width":"5 mm","height":"5 mm"})"};
        external({gitArgs({"clone", "-q", st->dir + "-remote.git", st->other}), feature,
                  gitArgs({"-C", st->other, "commit", "-q", "-am", "Box from elsewhere"}), gitArgs({"-C", st->other, "push", "-q"})});
        return true;
      },
      [=, this] {
        if (!outside() || !idle()) return false;
        backgroundFetch();
        return true;
      },
      [=, this] {  // the background fetch: the chip's count, said once with Pull
        if (!idle() || m_lastDone.isEmpty()) return false;
        require(finished("background fetch") && m_git->repo().status.behind == 1, "fetched in the background: one behind");
        require(toast(tr("%1 has new commits: %n to pull.", nullptr, 1).arg("origin/main")), "new commits said");
        require(m_git->chip()->property("text").toString().contains(tr("↓%1").arg(1)), "the chip's ↓1");
        pass("fetched in the background: the chip shows ↓1, Pull offered");
        m_services.action("vcs.pull")->trigger();
        return true;
      },
      [=, this] {  // Pull: fetched, both sides changed the design, the driver merges
        if (!idle()) return false;
        QDialog* d = dialog("vcsIncoming");
        if (!d) {
          if (m_lastDone == "pull failed") throw std::runtime_error(("pull: " + m_lastFailure).toStdString());
          return false;
        }
        require(d->findChild<QTreeWidget*>("commits")->topLevelItemCount() == 1 && d->findChild<QLabel*>("incomingHead")->text().contains(tr("Both sides have commits of their own: git makes a merge commit.")),
                "one commit, a merge commit to come");
        require(d->findChild<QLabel*>("incomingDesign") && !d->findChild<QLabel*>("incomingConflicts"), "regenerate said, no conflicts");
        shot(d, ".pull.png");
        pass("pull preview: " + d->findChild<QLabel*>("incomingDocument")->text());
        require(press(d, "vcsIncomingMerge"), "Pull");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("merge") || int(doc->doc.ops.size()) != fileOps() || doc->scene.all_bodies().size() != 4) return false;
        require(!doc->isDirty() && m_git->repo().status.ahead == 2 && m_git->repo().status.behind == 0, "the merge commit, ahead by 2");
        require(withAction(tr("Regenerate")), "Regenerate offered");
        pass("pulled: the driver merged both boxes, Regenerate offered");
        m_services.action("vcs.push")->trigger();
        return true;
      },
      [=, this] {
        if (!idle() || !finished("push") || m_git->repo().sync() != Y::Synced) return false;
        pass("pushed the merge");
        m_panel->setPage(History);
        require(m_history.size() >= 4 && m_history.back().hash == st->first, "the history: " + QString::number(m_history.size()));
        QTreeWidgetItem* first = item(m_panel->history(), st->first.left(7), 0);
        require(first, "the first commit listed");
        m_panel->history()->setCurrentItem(first);
        shot(m_tool, ".history.png");
        require(press(m_panel, "vcsCompare"), "Compare");
        return true;
      },
      [=, this] {
        if (!m_compare->active() || !m_compare->settled()) return false;
        require(m_compare->panel()->picker(0)->currentText().startsWith(st->first.left(7)), "A: the first commit");
        pass("compared the first commit with this session");
        m_compare->close();
        std::unique_ptr<QMenu> menu(m_panel->commitMenu(*m_panel->commitOf(m_panel->history()->currentItem())));
        QAction* restore = menu->findChild<QAction*>("vcs.restore");
        require(restore && restore->isEnabled(), "Restore as new changes…");
        restore->trigger();
        st->ops = int(doc->doc.ops.size());
        require(answer("vcsRestore", "restore"), "Restore asked first");  // before the bench's quiet mode closes it
        return true;
      },
      [=, this] {
        if (!idle() || !finished("restore")) return false;
        require(doc->isDirty() && int(doc->doc.ops.size()) > st->ops && doc->scene.all_bodies().size() == 1, "one box, as in the first commit");
        Toast* t = withAction(tr("Undo"));
        require(t, "the Restored toast with Undo");
        t->actionButton()->click();
        require(int(doc->doc.ops.size()) == st->ops && !doc->isDirty() && doc->scene.all_bodies().size() == 4, "Undo takes it back");
        pass("restored the first commit as new changes, one undo step");
        std::unique_ptr<QMenu> menu(m_panel->commitMenu(*m_panel->commitOf(m_panel->history()->currentItem())));
        menu->findChild<QAction*>("vcs.openReadOnly")->trigger();
        return true;
      },
      [=, this] {
        if (!idle() || !finished("open")) return false;
        const QFileInfo copy(m_lastOpened);
        require(copy.exists() && !copy.isWritable() && copy.fileName().contains(st->first.left(7)), "a read-only copy: " + m_lastOpened);
        require(opad::Document::load(std::filesystem::path(m_lastOpened.toStdU16String())).ops.size() ==
                    opad::Document::parse(run({"cat-file", "blob", st->first + ":model.opad"}).out.toStdString()).ops.size(),
                "the first commit's document");
        pass("opened the first commit read-only (a copy for another window)");
        std::unique_ptr<QMenu> menu(m_panel->commitMenu(*m_panel->commitOf(m_panel->history()->currentItem())));
        menu->findChild<QAction*>("vcs.branchFrom")->trigger();
        QDialog* d = dialog("vcsNewBranch");
        require(d && d->findChild<QLabel*>("from")->text().startsWith(st->first.left(7)), "starts at the first commit");
        d->findChild<QLineEdit*>("name")->setText("from-first");
        d->findChild<QCheckBox*>("switch")->setChecked(false);
        require(press(d, "vcsNewBranchRun"), "Create");
        return true;
      },
      [=, this] {  // a conflict: both sides rename the same box
        if (!idle() || !finished("branch") || !item(m_panel->branches(), "from-first", 0)) return false;
        require(m_git->repo().status.branch == "main" && run({"rev-parse", "from-first"}).out.trimmed() == st->first.toLatin1(), "from-first at the first commit");
        pass("a branch from a commit of the history");
        st->boxBody = QString::fromStdString(doc->scene.all_bodies().front());
        require(press(m_panel, "vcsNewBranch"), "New branch…");
        QDialog* d = dialog("vcsNewBranch");
        d->findChild<QLineEdit*>("name")->setText("rename");
        require(press(d, "vcsNewBranchRun"), "Create");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("switch")) return false;
        doc->run("rename", {{"target", st->boxBody.toStdString()}, {"name", "Theirs"}});
        commitDialog("Rename to Theirs");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("commit")) return false;
        git::Branch main;
        for (const auto& b : m_branches)
          if (b.name == "main") main = b;
        switchTo(main);
        return true;
      },
      [=, this] {
        if (!idle() || !finished("switch") || m_git->repo().status.branch != "main") return false;
        doc->run("rename", {{"target", st->boxBody.toStdString()}, {"name", "Ours"}});
        commitDialog("Rename to Ours");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("commit")) return false;
        mergeBranch("rename");
        return true;
      },
      [=, this] {
        if (!idle()) return false;
        QDialog* d = dialog("vcsIncoming");
        require(d, "the incoming dialog");
        auto* stop = d->findChild<QLabel*>("incomingConflicts");
        require(stop && stop->text().contains("Ours: name"), "the conflict named: " + (stop ? stop->text() : QString()));
        shot(d, ".conflict.png");
        pass("merge preview: the conflict said before merging");
        require(press(d, "vcsIncomingMerge"), "Merge anyway");
        return true;
      },
      [=, this] {
        if (!idle() || m_lastDone != "merge failed" || !m_git->repo().merging) return false;
        for (QMessageBox* m : window->findChildren<QMessageBox*>("vcsFailed")) m->close();
        require(m_lastFailure.contains(git::explain("Automatic merge failed")) || m_lastFailure.contains(git::explain("CONFLICT (")), "the stop explained: " + m_lastFailure);
        require(!doc->isDirty() && doc->nodeName(st->boxBody.toStdString()) == "Ours", "the document keeps ours");
        m_services.action("vcs.commit")->trigger();
        require(!dialog("vcsCommit") && m_lastFailure.contains(tr("Files are still in conflict: resolve them first, or abort the merge.")),
                "Commit refused while files are in conflict");
        require(press(m_panel, "vcsAbortMerge") && !m_panel->button("vcsCommitMerge")->isEnabled(), "Abort merge offered, Commit the merge not");
        shot(m_tool, ".merging.png");
        require(answer("vcsAbortMerge", "abort"), "Abort asked first");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("abort") || m_git->repo().merging) return false;
        require(m_git->repo().doc() == D::Clean && !doc->isDirty(), "back as before the merge");
        pass("the merge stopped on the conflict and was aborted");
        m_services.action("vcs.pack")->trigger();
        return true;
      },
      [=, this] {
        if (!idle() || !finished("pack")) return false;
        require(toast(tr("Packed the repository: %1 MB of loose objects and packs now take %2 MB.").section(':', 0, 0)), "the Packed toast");
        pass("packed the repository");
        m_services.action("vcs.fetch")->trigger();
        return true;
      },
      [=, this] {
        if (!idle() || !finished("fetch")) return false;
        require(toast(tr("Fetched: nothing new for %1").arg("main")), "fetched, nothing new");
        pass("fetched");
        std::unique_ptr<QMenu> menu(m_panel->commitMenu(m_history.front()));
        QAction* previous = menu->findChild<QAction*>("vcs.comparePrevious");
        require(previous && previous->isEnabled(), "Compare with the commit before");
        previous->trigger();
        return true;
      },
      [=, this] {
        if (!m_compare->active() || !m_compare->settled()) return false;
        require(m_compare->panel()->picker(0)->currentText() == tr("Before %1").arg(m_history.front().shortHash) &&
                    m_compare->panel()->picker(1)->currentText().startsWith(m_history.front().shortHash),
                "A: the commit before, B: the commit");
        pass("compared a commit with the one before");
        m_compare->close();
        // An unmerged branch: deleting it asks twice.
        QTreeWidgetItem* rename = item(m_panel->branches(), "rename", 0);
        require(rename, "the rename branch listed");
        m_panel->setPage(Branches);
        m_panel->branches()->setCurrentItem(rename);
        m_answers["vcsDeleteUnmerged"] = "force";
        require(press(m_panel, "vcsDelete") && answer("vcsDeleteBranch", "delete"), "Delete…");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("delete") || item(m_panel->branches(), "rename", 0)) return false;
        pass("an unmerged branch deleted after the second question");
        // Unsaved and uncommitted changes before a switch: saved first, then git refuses to overwrite them, in words.
        box(160);
        m_answers["vcsUnsaved"] = "save";
        m_answers["vcsUncommitted"] = "anyway";
        git::Branch target;
        for (const auto& b : m_branches)
          if (b.name == "from-first") target = b;
        require(!target.name.isEmpty(), "from-first listed");
        switchTo(target);
        return true;
      },
      [=, this] {
        if (!idle() || m_lastDone != "switch failed") return false;
        require(!doc->isDirty() && m_git->repo().doc() == D::Modified && m_git->repo().status.branch == "main", "saved first, still on main");
        require(m_lastFailure.contains(git::explain("would be overwritten by checkout")), "the refusal in words: " + m_lastFailure);
        for (QMessageBox* m : window->findChildren<QMessageBox*>("vcsFailed")) m->close();
        pass("a switch over uncommitted changes: saved first, refused in words");
        commitDialog("Box at 160");
        return true;
      },
      [=, this] {
        if (!idle() || !finished("commit") || m_git->repo().doc() != D::Clean) return false;
        pass("committed what the switch refused to overwrite");
        return true;
      },
  };
  auto* timer = new QTimer(this);
  timer->setInterval(150);
  connect(timer, &QTimer::timeout, this, [this, st, steps, timer] {
    try {
      if (st->step >= steps.size()) {
        timer->stop();
        QCoreApplication::exit(0);
        return;
      }
      if (steps[st->step]()) {
        ++st->step;
        st->wait = 0;
        m_lastDone.clear();  // what the step started ends later
      } else if (++st->wait > 1200) {  // 3 min: git is slow on a busy machine
        QString why = QStringLiteral("timed out in step %1 (last: %2, %3; running %4)").arg(st->step).arg(m_lastDone, m_lastFailure).arg(m_running);
        if (st->running) why += QStringLiteral("; outside: %1 for %2 ms").arg(st->current).arg(st->since.elapsed());
        throw std::runtime_error(why.toStdString());
      }
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QStringLiteral("bench: version: FAIL %1").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
