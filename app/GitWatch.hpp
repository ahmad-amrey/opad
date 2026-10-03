#pragma once
// The open document's repository on the UI thread (UI-61). A chip in the status bar shows it: the branch, whether the
// document is untracked, uncommitted or in conflict, ahead/behind its upstream, "not in git", "git not found". It
// follows git by events, never by a poll: .git/HEAD, index and config, the folders of the refs the branch follows, the
// document's folder (a save, a `git init`), OPAD coming to the front. git itself runs on workers: JobRunner::quiet for
// the status (one process; a probe of the repository, its config and the tools when that changed), JobRunner::async
// with the strip and Cancel for anything that may take long. The chip's menu sets up a repository (init -b main,
// .gitattributes, .gitignore, Git LFS, the managed merge and diff driver) or only this clone's driver config, and a
// managed driver config whose OPAD moved is repaired by itself; a clone whose attributes want OPAD's driver but whose
// config lacks it gets a banner over the viewport (Set up merging, or Later for this session). Clone repository… copies
// one (progress and Cancel in the strip), sets the clone up the same way and asks the window to open its document.
//
// UI-136: "git not found" offers Locate git… (checked on a worker, kept in the setting git/path); a folder git refuses
// as owned by someone else offers Trust this folder; the author (user.name, user.email) is asked for after a set up
// and by ensureIdentity before anything commits; commands get this opad.exe as GIT_ASKPASS when no credential helper
// is configured (askpassDialog, in its own process) and ssh in BatchMode, and their errors as sentences.
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>

#include "Git.hpp"

class Banner;
class Job;
class JobRunner;
class QLabel;
class QMenu;
class QWidget;

class GitWatch : public QObject {
  Q_OBJECT
 public:
  GitWatch(JobRunner* jobs, QWidget* window, QWidget* viewport = nullptr);  // viewport: where banners go
  QWidget* chip() const { return m_chip; }
  void setFile(const QString& file);  // the open document ("": none); the same file again: its state may have moved
  void refresh(bool probe = false);   // the status now (probe: the repository, its config and the tools again)
  const git::Repo& repo() const { return m_repo; }
  git::Context context() const;
  // A git command as a job (the strip after 0.5 s, Cancel), then `done` on the UI thread and a refresh. The default
  // options stop git after 60 s: network commands pass git::RunOptions::network().
  Job* command(const QString& title, const QStringList& args, std::function<void(const git::Result&)> done = {}, git::RunOptions o = {});
  // The chip's actions by object name: git.compare, git.setup, git.driver, git.identity, git.trust, git.clone, git.locate,
  // git.refresh.
  QMenu* menu(QWidget* parent);
  void setUp();        // the Set up repository dialog
  void setUpDriver();  // this clone's merge and diff driver (and LFS hooks when the attributes use LFS)
  void cloneRepository();  // the Clone repository dialog: an address and a folder, then clone, set up, openRequested
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
  void openRequested(const QString& file);  // a document of a new clone
  void compareRequested();  // the menu's Compare with the last commit… (CompareMode, UI-58)
 private:
  void schedule(bool probe, int ms = 250);
  void watch();
  void render();
  void runSetUp(const QString& folder, const git::SetupOptions& o);
  void runClone(const QString& url, const QString& folder);
  void chooseDocument(const QString& folder, const QStringList& documents);
  void updateBanner();
  void status(const QString& text);
  void failed(const QString& title, const QString& text);
  JobRunner* m_jobs;
  QWidget* m_window;
  QWidget* m_viewport;
  Banner* m_banner = nullptr;  // made when first needed, so the file-on-disk banner (made first) stays above it
  QWidget* m_chip;
  QLabel* m_icon;
  QLabel* m_text;
  QFileSystemWatcher m_watcher;
  QTimer m_debounce;
  QString m_file, m_program;
  git::Repo m_repo;
  bool m_running = false, m_again = false, m_probe = false, m_retried = false;
  bool m_driverJob = false;  // Set up merging runs: the banner waits for its result
  int m_busy = 0;  // jobs of ours changing the repository: reads wait for them
  unsigned m_generation = 0;
  int m_runs = 0;  // reads started: benches check nothing polls
  QDateTime m_configStamp, m_attributesStamp;
  QElapsedTimer m_activated;  // the last probe because OPAD came to the front
  QSet<QString> m_repaired;  // tops whose stale driver config was rewritten this session
  QSet<QString> m_later;     // tops whose Set up merging banner was closed this session
  QString m_lastFailure;     // the last error shown (benches)
};
