#pragma once
// Keeps the open document and its file in step (UI-56). The file and its folder are watched (a save by tmp + rename
// drops the file's own watch) and stat'ed again on activation and every few seconds (network drives may not notify).
// When the file is not what this session last read or wrote, it is read on a worker and compared with that:
//   the same ops              -> nothing (touched, or written back alike)
//   new ops, nothing unsaved  -> they come in, one undo step
//   new ops, unsaved changes  -> banner: Merge (the file's, then yours) / Reload / Save as / Overwrite
//   another history          -> banner (a reset, rebase or other branch; another document): Reload / Save as / Overwrite
//   unreadable, deleted       -> banner
// Save refuses while the file differs (AppDocument::saveBlocked); Overwrite, and Reload over unsaved changes, ask first.
#include <QFileSystemWatcher>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

#include "AppDocument.hpp"

class Banner;
class Job;
class JobRunner;

class DiskSync : public QObject {
  Q_OBJECT
 public:
  DiskSync(AppDocument* doc, JobRunner* jobs, QWidget* viewport, QWidget* window);
  void check();  // one stat; reads the file on a worker when it is not what this session last read or wrote
  // A command of the window changed the file (a branch switch, a merge, a pull: UI-62): what it is now comes in without a
  // question while nothing is unsaved here, new ops merged and another history reloaded. Asked again when unsaved.
  void adopt();
  // Whether git holds the document in a stopped merge (UI-63): its conflict markers are then resolved (vcs.resolve), not
  // overwritten. gitChanged: git's view moved on (the banner follows).
  void setConflicted(std::function<bool()> conflicted) { m_conflicted = std::move(conflicted); }
  void gitChanged();
  bool bench();  // OPAD_BENCH_EXTERNAL_CHANGE=<prefix>
 private:
  void watch();
  void schedule(int ms = 300);
  void read(bool everyBody = false);
  void decide();  // what the last read means for the session
  void showMerge(const opad::MergePlan& plan);
  void showReplaced(const QString& text);
  void showUnreadable();
  void showDeleted();
  void confirm(const QString& title, const QString& text, const QString& action, std::function<void()> fn);
  void merge();
  void reload(bool asked = false);
  void overwrite();
  void trigger(const char* action);  // a window action (Save, Save as, Regenerate)
  void status(const QString& text);
  bool idle() const;  // nothing holds the document: the file's changes may come in now
  QString name() const;
  AppDocument* m_doc;
  JobRunner* m_jobs;
  QWidget* m_window;
  Banner* m_banner;
  QFileSystemWatcher m_watcher;
  QTimer m_debounce, m_poll;
  QPointer<Job> m_job;
  std::shared_ptr<AppDocument::DiskRead> m_read;  // the file as last read, until acted on
  bool m_decided = false;    // the banner (or nothing) for m_read is up
  bool m_saveAfter = false;  // the window's Save was refused: it runs once the file turns out unchanged (until the path changes)
  bool m_reloadAfter = false;
  bool m_adopt = false;      // the next change is the window's own (adopt)
  std::function<bool()> m_conflicted;
  std::optional<AppDocument::DiskStat> m_dismissed;  // the banner was closed for this state of the file
  int m_reads = 0;
};
