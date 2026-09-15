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
  const opad::Node* node(const std::string& id) const { return scene.node(id); }
  QString nodeName(const std::string& id) const;

 signals:
  void changed();
  void pathChanged();
  void message(const QString& text);
  void loadProgress(const QString& phase, int percent);  // percent < 0: unknown
  void loadFinished(bool ok, const QString& error);

 private:
  opad::ImportOptions loadOptions(const std::shared_ptr<std::atomic<bool>>& cancel, const QString& file);
  std::shared_ptr<std::atomic<bool>> m_cancel;
  std::shared_ptr<std::atomic<bool>> m_alive;
};
