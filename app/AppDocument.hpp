#pragma once
// The live document inside the app: a core Document plus its resolved Scene. All edits go through
// the shared command layer (F36) so the UI never has private shortcuts to core state.
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <map>
#include <memory>
#include <functional>

#include "opad/core.hpp"
#include "opad/design/feature.hpp"
#include "opad/merge.hpp"
#include "opad/assets.hpp"
#include "opad/step_io.hpp"

class JobRunner;
class Job;
class Progress;
class AppDocument : public QObject {
  Q_OBJECT
  // Snapshots retain the source even if the window closes while its worker is copying.
  std::shared_ptr<opad::Document> m_storage;
 public:
  explicit AppDocument(QObject* parent = nullptr);
  ~AppDocument() override;

  opad::Document& doc;
  opad::Scene scene;
  // Viewer mode: a file other than .opad (STEP, STL, DXF, ...) shown read-only. It was read with ImportOptions::viewer,
  // so nothing is prepared for saving; the view can still change (hide, colour), edits need startEditable() first.
  bool browse = false;
  QString viewing;           // viewer mode: the file shown
  bool viewerOpens = true;   // setting files/viewerMode: false opens other formats as editable, unsaved documents
  // A .opad shown read-only (opad --read-only, a write-protected file, a version from the history): measured, sectioned
  // and changed in looks (hide, colour; never unsaved) as in viewer mode. Edits and Save need a copy: saveAs, detach().
  bool readOnly = false;
  bool viewOnly() const { return browse || readOnly; }
  bool hasDocument = false;
  unsigned long long generation = 0;
  unsigned long long revision = 0;
  bool loading = false;      // a worker thread owns the document content until loadFinished
  opad::json lastLoad;       // what the reader of the last finished open or import said (ImportResult::to_json + "file")
  // Linked files (opad/assets.hpp) of the open document as the last load found them: AssetState::to_json per asset.
  opad::json assetStates = opad::json::array();

  void newDocument();
  // The document becomes an untitled copy with an identity of its own (New from template): Save asks where to put it.
  void detachCopy();
  void closeDocument();  // back to the start screen; nothing is saved here (ask first)
  void open(const QString& path);  // .opad -> load; .step/.stp -> import into a new document
  void importStep(const QString& path, const QString& parent = {});
  // Save refuses (false, saveBlocked) while the file differs from what this session last read or wrote, unless
  // `overwriteDisk` (asked first): changes made outside are merged or reported, never overwritten silently (UI-56).
  bool save(bool overwriteDisk = false);
  bool saveAs(const QString& path);
  // Atomic background save; holds the document write guard until the worker really exits. `overwriteDisk`: past the save
  // guard (DiskSync's Overwrite, asked first).
  Job* saveAsync(JobRunner*, const QString& path, bool overwrite,
                 std::function<void(bool,const QString&)> done, int testDelayMs=0, bool overwriteDisk=false);
  opad::json run(const std::string& command, opad::json args, const QString& label = {});  // label: the undo step's, else by command; a lock refusal comes back as lockedMessage
  // A change refused by a lock (UI-37) in the shown language: the node, what holds its lock and the change refused.
  static QString lockedMessage(const opad::LockedError& e);
  // Several commands as one step to undo, under one label and one refresh (a drawing's layer state restored); all or none.
  opad::json runAll(const std::vector<std::pair<std::string, opad::json>>& commands, const QString& label);
  // Several commands as one step (one undo, one refresh): `commands` calls run() as often as it needs, the scene is not
  // resolved in between (read what you need first). One that throws takes back what the others appended, and rethrows.
  void batch(const QString& label, const std::function<void()>& commands);

  // Design changes are planned on a worker (design::plan_ops reads the document, see DesignController) and
  // committed here. While a plan is being computed the document must not change under it: designBusy makes
  // run() and commitPlan() refuse.
  bool designBusy = false;
  bool annotationEditing = false;  // temporary annotation editor; agent writes wait for Save/Cancel
  opad::json commitPlan(opad::design::Plan&& plan, const QString& label);
  // Roll-back: the scene is replayed up to (not including) this op. Editing a feature or a sketch shows the
  // model as it was when that op was computed, which is what its references mean. Empty = the whole log.
  void setRollback(const std::string& opId);
  const std::string& rollback() const { return m_rollback; }
  // The same, asked for by the user (the timeline's playhead, Roll back to here, UI-99): the model as it was before that
  // op, until it is rolled forward (empty). Nothing is inserted at the marker (UI-130): a step appended meanwhile rolls
  // forward first, so it shows, unless it only edits or deletes earlier steps. An editor's setRollback takes over from it
  // and gives it back as the editor ends (setRollback({})), when what it committed was such an edit or nothing.
  void rollBackTo(const std::string& opId);
  bool rolledBack() const { return m_userRollback && !m_rollback.empty(); }

  // Long loads run off the UI thread; progress and the result come back through the signals below. A file other than
  // .opad opens in viewer mode. Opening while a load runs drops that load (it finishes in the background, unseen).
  void startOpen(const QString& path, bool readOnly = false);  // readOnly: a writable .opad opens read-only too
  void detach();  // read-only -> an unsaved copy of the same content (no file behind it), which can be edited
  // Viewer mode -> an editable, unsaved document with the same content and view changes, prepared on a worker
  // (opad::make_editable). `done(ok, error)` runs on the UI thread.
  void startEditable(JobRunner* jobs, std::function<void(bool, const QString&)> done);
  // After a slow viewer read is on screen (meshed): keeps its shapes and meshes for the next open (opad::viewer_cache_store).
  void storeViewerCache(JobRunner* jobs);
  bool converting() const { return m_converting; }
  // A drawing goes where `placement` puts its XY plane and origin, after `plane` (resolved on the worker) if given.
  // `link`: a linked asset (opad::link_file) rather than a copy.
  void startImport(const QString& path, const QString& parent = {}, const opad::Mat4& placement = {}, const opad::json& plane = {}, bool link = false);
  // Reads the linked files whose bodies are not loaded (missing then, or not trusted) on a worker; `trustAll` for files the
  // user has just agreed to. The bodies join the document on the UI thread; assetStates is updated.
  void loadAssets(JobRunner* jobs, bool trustAll, std::function<void(bool, const QString&)> done = {});
  // Linked files: the folders trusted in the settings (assets/trusted) and this machine's KiCad options.
  static opad::AssetOptions assetOptions();
  static QString assetSummary(const opad::json& states);  // "Linked files: 1 changed since the last sync, ..." or empty
  void cancelLoad();
  void refresh();
  // KiCad boards: the reader's options from the settings (kicad/*).
  static opad::KicadOptions kicadOptions();
  using SnapshotCallback = std::function<void(std::shared_ptr<opad::Document>, const QString&)>;
  // `background`: nobody waits for it (recovery, the agent bridge): no busy cursor, no completion toast.
  bool captureSnapshot(JobRunner* jobs, SnapshotCallback done, bool background = false);
  bool snapshotBusy() const { return m_capturing; }
  // A worker that only reads the document (a bill of materials) without copying it: nothing changes the document
  // meanwhile (designBusy), until the worker has really stopped, also after a cancel; then `done(ok, error)` runs on the
  // UI thread. `work` gets the document and a copy of the scene. Null when the document is busy or there is none.
  Job* readAsync(JobRunner* jobs, const QString& title, std::function<void(const opad::Document&, const opad::Scene&, Progress)> work,
                 std::function<void(bool ok, const QString& error)> done);
  // A writer waits for the copy or the save that reads the document now (a few hundred ms on a big one) instead of
  // failing: `fn` runs once it is done (at once when none runs), dropped when another document comes.
  void afterCapture(std::function<void()> fn);
  void recover(opad::Document&& document, opad::Scene&& resolved);  // an unsaved copy, no path (see Recovered)
  // Prepared on a worker. Swaps the old values back into the caller for worker disposal.
  void commitSnapshot(opad::Document& document, opad::Scene& resolved,
                      unsigned long long expectedRevision, const QString& label);

  QString title() const;
  QString path() const;
  bool isDirty() const { return hasDocument && !viewOnly() && doc.dirty; }

  // Undo/redo over the op log. Each command's appended ops form one step; undo pops them off the log (their
  // persisted text is kept, so redo then save writes them back byte-identically) and redo pushes them back.
  // The document counts as clean whenever the log and body store match the snapshot taken at load/save,
  // so undoing back to the saved state clears the asterisk. Depth is a setting (edit/undoDepth).
  bool canUndo() const { return !m_undo.empty() && !loading && (!designBusy || m_capturing); }
  bool canRedo() const { return !m_redo.empty() && !loading && (!designBusy || m_capturing); }
  QString undoLabel() const { return m_undo.empty() ? QString() : m_undo.back().label; }
  QString redoLabel() const { return m_redo.empty() ? QString() : m_redo.back().label; }
  QStringList undoLabels() const;  // every step, the next one to undo first (the Undo arrow's list)
  QStringList redoLabels() const;
  void undo(int steps = 1);  // several steps: one refresh
  void redo(int steps = 1);
  void setUndoLimit(int steps);
  int undoLimit() const { return m_undoLimit; }
  const opad::Node* node(const std::string& id) const { return scene.node(id); }
  QString nodeName(const std::string& id) const;

  // The active component (UI-33): session state, never written. New sketches, features, bodies, imports and components
  // go into it, and the view ghosts everything else. Empty = the document root. Back to the root when it goes (deleted,
  // undone; not while the scene is rolled back to an earlier op) or another document comes in. Also in viewer mode.
  const std::string& activeComponent() const { return m_active; }
  // A component of the scene, or empty; throws for anything else. remember: it comes back when this document is opened
  // again (setting view/active/<uuid>; not for a viewed file, read afresh each time).
  void setActiveComponent(const std::string& id, bool remember = false);
  std::string rememberedComponent() const;  // what setActiveComponent(.., true) last left for this document, if still a component
  // ---- The file on disk (UI-56): what this session last read or wrote there, so that a change made outside (a git
  // pull or checkout, another OPAD, opad-cli) is noticed (DiskSync), merged or reported, and never overwritten.
  struct DiskStat {
    bool exists = false;
    qint64 size = -1, mtime = -1;
    bool operator==(const DiskStat& o) const { return exists == o.exists && size == o.size && mtime == o.mtime; }
    bool operator!=(const DiskStat& o) const { return !(*this == o); }
  };
  // The file as read now, on a worker: its ops, and only the bodies the base does not list (the session has those).
  struct DiskRead {
    QString file;
    DiskStat stat;                                   // taken before reading: a later change is never missed
    std::shared_ptr<const opad::Manifest> base;      // what it was compared with
    std::shared_ptr<opad::Document> doc;
    std::vector<std::string> bodies;                 // every body key the file lists, in its order
    std::shared_ptr<const opad::Manifest> manifest;  // the file's
    opad::Relation relation = opad::Relation::same;  // rewritten without a base (nothing to compare with)
    QString error;                                // unreadable (git conflict markers, not an OPAD document)
  };
  static DiskStat statFile(const QString& file);
  // `cache`: the session's shapes, filled with the bodies read (keys are content hashes). `skipKnown` false reads every
  // body (a reload of a session that lost some).
  static DiskRead readDisk(const QString& file, std::shared_ptr<const opad::Manifest> base, std::shared_ptr<opad::ShapeCache> cache, bool skipKnown = true);
  const QString& diskFile() const { return m_diskFile; }  // empty: no file behind the document
  const DiskStat& diskStat() const { return m_diskStat; }
  std::shared_ptr<const opad::Manifest> diskBase() const { return m_diskBase; }
  bool diskChanged() const;  // one stat: the file exists and is not what this session last read or wrote
  opad::MergePlan planDisk(const DiskRead& read) const;
  // The file's new ops come in before this session's unsaved ones (one undo step `label`); the file is then the saved
  // state. Throws when the plan has an error or a body is missing.
  opad::MergePlan mergeDisk(DiskRead&& read, const QString& label);
  // Replaces the document with the file as read (unsaved changes are dropped; ask first). Throws when a body is missing.
  void reloadDisk(DiskRead&& read);
  void acceptDisk(const DiskRead& read);  // the same op log: only the file's stamp moved on (touched, rewritten alike)
  // Recovery into its file (UI-59): the document is that file's again (Save writes there). Its first `saved` ops are what
  // the file holds (`stat` and `base` as last read), the rest unsaved. DiskSync compares the file with that: a stat that
  // differs makes it read the file and merge or report what changed there.
  struct Recovered {
    QString file;
    size_t saved = 0;
    DiskStat stat;
    std::shared_ptr<const opad::Manifest> base;
  };
  void recover(opad::Document&& document, opad::Scene&& resolved, const Recovered& into);

 signals:
  void activeComponentChanged();
  // A viewer document became editable: the same shapes, now under content keys (live key -> content key). Emitted just
  // before the scene changes to them, so a view can keep what it has drawn.
  void bodyKeysRenamed(const std::map<std::string, std::string>& keys);
  void newDocumentCreated();
  void aboutToReplace();  // end transient tools before changing document identity
  void changed();
  void pathChanged();
  void saved();  // successful explicit Save / Save As, not an open or title change
  // Save found the file changed on disk and wrote nothing. `retry`: the window's Save (or Save as), run again once the file
  // turns out unchanged; a save of another caller (saveAsync: an agent, Commit) failed for good.
  void saveBlocked(bool retry);
  void message(const QString& text);
  void loadProgress(const QString& phase, int percent);  // percent < 0: unknown
  void loadFinished(bool ok, const QString& error);
  void undoChanged();  // stacks or labels changed

 private:
  opad::ImportOptions loadOptions(const std::shared_ptr<std::atomic<bool>>& cancel, const QString& file);
  struct Step {
    QString label;
    size_t count = 0;            // ops on the log while the step sits on the undo stack
    std::vector<opad::Op> ops;   // the ops themselves while it sits on the redo stack
  };
  void recordStep(const QString& label, size_t opsBefore);
  void clearHistory();
  void markSaved();      // snapshot the state the file holds (or the empty state of a new document)
  void updateDirty();    // dirty = log or body store differs from the snapshot
  static QString labelFor(const std::string& command, const opad::json& args);
  void checkActive();  // the active component still a component of the (whole) scene, else the root
  std::string m_rollback, m_active;
  bool m_userRollback = false;  // m_rollback is the user's (rollBackTo), not an editor's
  std::string m_resume;         // the user's roll-back an editor took over (setRollback): back to it when the editor ends
  bool changesBefore(const std::string& point, size_t from) const;
  void dropRollback();
  std::vector<Step> m_undo, m_redo;
  int m_undoLimit = 50;
  std::vector<std::string> m_savedIds;
  size_t m_savedBodies = 0;
  void setDisk(const QString& file, const DiskStat& stat, std::shared_ptr<const opad::Manifest> base);
  void wroteDisk();  // after a save on the UI thread: the file holds the document
  QString m_diskFile;
  DiskStat m_diskStat;
  std::shared_ptr<const opad::Manifest> m_diskBase;
  std::shared_ptr<std::atomic<bool>> m_cancel;
  std::shared_ptr<std::atomic<bool>> m_alive;
  std::shared_ptr<std::atomic<unsigned>> m_loadToken = std::make_shared<std::atomic<unsigned>>(0);  // the load whose result counts
  bool m_capturing = false;
  bool m_batching = false;  // inside batch(): run() neither records a step nor refreshes
  bool m_converting = false;
  QString m_cacheSource;  // the viewed file, when its read was slow enough to remember
  bool m_cacheCenter = false;
  double m_cacheReadMs = 0;  // how long that read took: the viewer cache keeps it only when it reads back twice as fast
};
