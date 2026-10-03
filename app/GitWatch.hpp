#pragma once
// The open document's repository on the UI thread (UI-61). A chip in the status bar shows it: the branch, whether the
// document is untracked, uncommitted or in conflict, ahead/behind its upstream, "not in git", "git not found". It
// follows git by events, never by a poll: .git/HEAD, index and config, the folders of the refs the branch follows, the
// document's folder (a save, a `git init`), OPAD coming to the front. git itself runs on workers: JobRunner::quiet for
// the status (one process; a probe of the repository, its config and the tools when that changed), JobRunner::async
// with the strip and Cancel for anything that may take long. The chip's menu sets up a repository (init -b main,
// .gitattributes, .gitignore, Git LFS, the managed merge and diff driver) or only this clone's driver config, and a
// managed driver config whose OPAD moved is repaired by itself.
//
// UI-136: "git not found" offers Locate git… (checked on a worker, kept in the setting git/path); a folder git refuses
// as owned by someone else offers Trust this folder; the author (user.name, user.email) is asked for after a set up
// and by ensureIdentity before anything commits; commands get this opad.exe as GIT_ASKPASS when no credential helper
// is configured (askpassDialog, in its own process) and ssh in BatchMode, and their errors as sentences.
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

#include "Git.hpp"

class Job;
class JobRunner;
class QLabel;
class QMenu;
class QWidget;

class GitWatch : public QObject {
  Q_OBJECT
 public:
  GitWatch(JobRunner* jobs, QWidget* window);
  QWidget* chip() const { return m_chip; }
  void setFile(const QString& file);  // the open document ("": none); the same file again: its state may have moved
  void refresh(bool probe = false);   // the status now (probe: the repository, its config and the tools again)
  const git::Repo& repo() const { return m_repo; }
  git::Context context() const;
  // A git command as a job (the strip after 0.5 s, Cancel), then `done` on the UI thread and a refresh.
  Job* command(const QString& title, const QStringList& args, std::function<void(const git::Result&)> done = {}, git::RunOptions o = {});
  // The chip's actions by object name: git.setup, git.driver, git.identity, git.trust, git.locate, git.refresh.
  QMenu* menu(QWidget* parent);
  void setUp();        // the Set up repository dialog
  void setUpDriver();  // this clone's merge and diff driver (and LFS hooks when the attributes use LFS)
  void locateGit();    // a file dialog, then useProgram
  void useProgram(const QString& path);  // kept as git/path when it runs as git, else said why not
  void trustFolder();  // asks, then safe.directory
  // Runs `then` once git knows the author; asks for a name and an email address first when it does not.
  void ensureIdentity(std::function<void()> then = {});
  void editIdentity(std::function<void()> then = {});
  bool bench();  // OPAD_BENCH_GIT=<prefix>
  // opad.exe as git's GIT_ASKPASS: `opad.exe --askpass <prompt>`, or the one argument git gives when OPAD_ASKPASS is set.
  static bool isAskpass(int argc, char** argv);
  // The prompt in a dialog of its own process (the caller made the QApplication); the answer goes to stdout. 0: answered.
  static int askpassDialog(int argc, char** argv);
 signals:
  void changed();
 private:
  void schedule(bool probe, int ms = 250);
  void watch();
  void render();
  void runSetUp(const QString& folder, const git::SetupOptions& o);
  void status(const QString& text);
  void failed(const QString& title, const QString& text);
  JobRunner* m_jobs;
  QWidget* m_window;
  QWidget* m_chip;
  QLabel* m_icon;
  QLabel* m_text;
  QFileSystemWatcher m_watcher;
  QTimer m_debounce;
  QString m_file, m_program;
  git::Repo m_repo;
  bool m_running = false, m_again = false, m_probe = false, m_retried = false;
  int m_busy = 0;  // jobs of ours changing the repository: reads wait for them
  unsigned m_generation = 0;
  int m_runs = 0;  // reads started: benches check nothing polls
  QDateTime m_configStamp, m_attributesStamp;
  QSet<QString> m_repaired;  // tops whose stale driver config was rewritten this session
  QString m_lastFailure;     // the last error shown (benches)
};
