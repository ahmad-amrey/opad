#pragma once
// The live document inside the app: a core Document plus its resolved Scene. All edits go through
// the shared command layer (F36) so the UI never has private shortcuts to core state.
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>

#include "opad/core.hpp"
#include "opad/step_io.hpp"

class AppDocument : public QObject {
  Q_OBJECT
 public:
  explicit AppDocument(QObject* parent = nullptr);
  ~AppDocument() override;

  opad::Document doc;
  opad::Scene scene;
  bool browse = false;       // F1: transient view of a STEP file, nothing is persisted
  bool hasDocument = false;
  bool loading = false;      // a worker thread owns the document content until loadFinished

  void newDocument();
  void closeDocument();  // back to the start screen; nothing is saved here (ask first)
  void open(const QString& path);  // .opad -> load; .step/.stp -> browse mode
  void importStep(const QString& path, const QString& parent = {});
  void save();
  void saveAs(const QString& path);
  opad::json run(const std::string& command, opad::json args);

  // Long loads run off the UI thread; progress and the result come back through the signals below.
  void startOpen(const QString& path);
  void startImport(const QString& path, const QString& parent = {});
  void cancelLoad();
  void refresh();

  QString title() const;
  QString path() const;
  bool isDirty() const { return hasDocument && !browse && doc.dirty; }

  // Undo/redo over the op log. Each command's appended ops form one step; undo pops them off the log (their
  // persisted text is kept, so redo then save writes them back byte-identically) and redo pushes them back.
  // The document counts as clean whenever the log and body store match the snapshot taken at load/save,
  // so undoing back to the saved state clears the asterisk. Depth is a setting (edit/undoDepth).
  bool canUndo() const { return !m_undo.empty() && !loading; }
  bool canRedo() const { return !m_redo.empty() && !loading; }
  QString undoLabel() const { return m_undo.empty() ? QString() : m_undo.back().label; }
  QString redoLabel() const { return m_redo.empty() ? QString() : m_redo.back().label; }
  void undo();
  void redo();
  void setUndoLimit(int steps);
  int undoLimit() const { return m_undoLimit; }
  const opad::Node* node(const std::string& id) const { return scene.node(id); }
  QString nodeName(const std::string& id) const;

 signals:
  void changed();
  void pathChanged();
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
  std::vector<Step> m_undo, m_redo;
  int m_undoLimit = 50;
  std::vector<std::string> m_savedIds;
  size_t m_savedBodies = 0;
  std::shared_ptr<std::atomic<bool>> m_cancel;
  std::shared_ptr<std::atomic<bool>> m_alive;
};
