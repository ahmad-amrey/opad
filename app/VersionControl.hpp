#pragma once
// Version control of the open document (UI-62), on top of the repository GitWatch follows (UI-61): the Version control
// panel (VersionPanel, in the "version" ToolPanel: the branch and its remote, the document's state, the history of the
// document and the branches) and the commands the area registers, the git chip's menu and the panel run:
//   Commit…   saves first, then git add + commit of the files ticked (the document, OPAD's attribute files), the message
//             suggested from the semantic diff of the last commit and this session, the author checked first, amend while
//             not pushed, push after; then a look at the loose objects (Pack offered).
//   Push      -u <remote> <branch> the first time (a remote added first when there is none), warnings about big files.
//   Fetch     also in the background every 10 minutes (setting git/fetchMinutes), quietly, never asking for a sign-in.
//   Pull      fetch, then the incoming commits and what they change in the document (the driver's merge done on a worker:
//             conflicts, design changed on both sides) with Preview in Compare and Merge.
//   Branches  switch (unsaved and uncommitted changes asked about first), create (here or from a commit), delete, merge
//             into the current one with the same incoming preview; Abort merge while one stopped on conflicts.
//   Resolve   a merge that stopped on the document (UI-63, VersionResolve.cpp): what both sides changed, decided one by
//             one (mine or theirs) or a whole side kept; the file written and added, Regenerate offered, then Commit.
//   History   the commits of the document: compare with this session or with the commit before, open read-only (a
//             write-protected copy in another window, opened with --read-only: AppDocument::readOnly), restore as new
//             changes (append-only tombstones, one undo step), branch from here; narrowed to the commits that touched an op or
//             a node (UI-64: showHistoryOf, from OpProvenance's index, which also gives the timeline its "Added by" lines).
// The file on disk changes under the session (switch, merge, pull): DiskSync::adopt takes it in. git runs on workers
// (GitWatch::job / command), versions are read, merged and diffed on workers; dialogs are window-modal and never block.
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <vector>

#include "Git.hpp"
#include "opad/json.hpp"

class AreaServices;
class CompareMode;
class DiskSync;
class GitWatch;
namespace opad {
struct MergeConflict;
struct Scene;
}
class OpProvenance;
class QDialog;
class QMenu;
class ToolPanel;
class VersionPanel;

class VersionControl : public QObject {
  Q_OBJECT
 public:
  VersionControl(AreaServices& services, GitWatch* git, CompareMode* compare, DiskSync* disk);
  GitWatch* git() const { return m_git; }
  AreaServices& services() const { return m_services; }
  enum Page { History, Branches };
  void openPanel(int page = -1);  // -1: the page it showed last
  void commit();
  void push();
  void pull();
  void fetch();
  // Every git/fetchMinutes (10; 0: never) while the branch follows a remote: a quiet fetch that never asks anyone to sign
  // in, so the chip's ↓ count is current; new commits there are said once, with Pull.
  void backgroundFetch();
  void newBranch(const QString& start = {}, const QString& startLabel = {});
  void switchTo(const git::Branch& branch);
  void mergeBranch(const QString& name);
  void deleteBranch(const QString& name, bool force = false);
  void abortMerge();
  void resolveConflicts();  // the document's index stages read and merged on a worker, then the Resolve conflicts dialog
  static QString conflictText(const opad::MergeConflict& c, const opad::Scene& s);  // "Bracket: name"
  void pack();
  void compareWith(const git::Commit& commit);      // the commit (A) with this session (B)
  void comparePrevious(const git::Commit& commit);  // the commit before (A) with it (B)
  void openReadOnly(const git::Commit& commit);     // a read-only copy in another OPAD window
  static QStringList readOnlyArguments(const QString& file) { return {QStringLiteral("--read-only"), file}; }  // that window's
  void restore(const git::Commit& commit);          // the document as it was there, as new changes
  void moreHistory();
  // The History page showing only the commits that touched these ops or nodes (their ops, edits, deletes, renames, ...),
  // newest first, under "Only the commits that touched <label>"; no ids: every commit again.
  void showHistoryOf(const QString& label, const std::vector<std::string>& ids);
  const std::vector<git::Commit>& shownHistory() const { return m_filterLabel.isEmpty() ? m_history : m_filtered; }
  QString historyFilter() const { return m_filterLabel; }
  bool historyFilterRead() const { return m_filterRead; }  // the narrowed list is there (the index was read)
  OpProvenance* provenance() const { return m_provenance; }
  void extendMenu(QMenu* menu);  // the git chip's menu: the panel, Commit…, Pull, Push
  const std::vector<git::Commit>& history() const { return m_history; }
  const std::vector<git::Branch>& branches() const { return m_branches; }
  const QStringList& remotes() const { return m_remotes; }
  bool hasMoreHistory() const { return m_moreHistory && m_filterLabel.isEmpty(); }  // a page was full: older commits may follow
  QString lastOpened() const { return m_lastOpened; }    // the read-only copy written last (benches)
  QString documentPath() const;  // the document relative to the repository's top; empty: not in one
  bool ready() const;            // the document is in a repository git can read
  bool settled() const;          // nothing of ours running, the lists read (benches)
  ToolPanel* toolPanel() const { return m_tool; }
  VersionPanel* panel() const { return m_panel; }
  bool bench(const QString& prefix);  // OPAD_BENCH_VERSION (VersionBench.cpp)
  // What merging a branch or the upstream brings (read on a worker by incoming()).
  struct Incoming {
    QString target, label;           // what is merged ("@{u}", a branch) and how it is named
    QString head, theirs, base;      // commits
    std::vector<git::Commit> commits;  // theirs that HEAD lacks, newest first
    bool fastForward = false, documentChanged = false, design = false;
    QString merged;                  // the document as the merge would leave it (a temporary file), for Compare
    QString summary, error;          // what changes in the document; why the driver would stop
    QStringList conflicts;           // what both sides changed, as words
    opad::json changes = opad::json::array();  // for the list, as Compare lists them
  };
 signals:
  void listsChanged();  // history or branches read again
  void finished(const QString& what, bool ok);  // a command of ours ended (benches)
 private:
  void makePanel();
  void reload();  // history and branches (and remotes) on a worker
  void scheduleReload();
  void failed(const QString& title, const QString& text);
  void done(const QString& what, bool ok, const QString& text = {});
  void say(const QString& text, const QString& action = {}, std::function<void()> fn = {}, int ms = 4000);  // a toast
  // A window-modal question; answers are (id, text, run) and the box gets Cancel too. Benches press them by "answer".
  struct Answer {
    QString id, text;
    std::function<void()> run;
  };
  void ask(const QString& name, const QString& title, const QString& text, const std::vector<Answer>& answers);
  // Unsaved changes saved, uncommitted ones of the document committed (or let through when `anyway`), then `then`.
  void whenClean(const QString& what, std::function<void()> then, bool commitAllowed = true);
  void runCommit(const QStringList& files, const QString& message, bool amend, bool pushAfter);
  void runPush(const QString& remote, bool warned);
  void addRemote(std::function<void(const QString&)> then);
  void incoming(const QString& target, const QString& label, bool fetchFirst);
  void showIncoming(std::shared_ptr<Incoming> in, bool pull);
  void runMerge(std::shared_ptr<Incoming> in);
  void runSwitch(const QStringList& args, const QString& name);
  QString versionsFolder() const;
  AreaServices& m_services;
  GitWatch* m_git;
  CompareMode* m_compare;
  DiskSync* m_disk;
  VersionPanel* m_panel = nullptr;
  ToolPanel* m_tool = nullptr;
  struct Conflicts;  // a merge stopped on the document, as read (VersionResolve.cpp)
  void showConflicts(std::shared_ptr<Conflicts> c);
  void runResolve(std::shared_ptr<Conflicts> c, std::vector<bool> mine, int whole);  // whole: 2 ours, 3 theirs; 0 decided
  void applyFilter();  // the narrowed list again, from the index of HEAD (read first when it moved)
  QPointer<QDialog> m_incoming;
  OpProvenance* m_provenance;
  QString m_filterLabel;
  std::vector<std::string> m_filterIds;
  std::vector<git::Commit> m_filtered;
  bool m_filterRead = false;
  QString m_filterHead;  // the commit it was narrowed at
  std::vector<git::Commit> m_history;
  std::vector<git::Branch> m_branches;
  QStringList m_remotes;
  bool m_moreHistory = false;  // a page of 200 was full: there may be more
  bool m_reading = false, m_again = false;
  int m_running = 0;  // commands of ours
  unsigned m_generation = 0;
  QString m_listed;  // the repository and document the lists are of
  QTimer m_reload, m_fetch;
  bool m_fetching = false;
  QString m_lastFailure, m_lastDone, m_lastOpened;  // benches
  int m_page = History;
  bool m_reopen = false;  // Compare took the panel's place: back when it ends
  bool m_benching = false;
  QStringList m_said;  // the toasts shown (benches)
  QHash<QString, QString> m_answers;  // question -> the answer a bench gives when it comes
};
