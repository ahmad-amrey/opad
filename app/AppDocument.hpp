#pragma once
// The live document inside the app: a core Document plus its resolved Scene. All edits go through
// the shared command layer (F36) so the UI never has private shortcuts to core state.
#include <QObject>
#include <QString>

#include "opad/core.hpp"

class AppDocument : public QObject {
  Q_OBJECT
 public:
  explicit AppDocument(QObject* parent = nullptr);

  opad::Document doc;
  opad::Scene scene;
  bool browse = false;       // F1: transient view of a STEP file, nothing is persisted
  bool hasDocument = false;

  void newDocument();
  void open(const QString& path);  // .opad -> load; .step/.stp -> browse mode
  void importStep(const QString& path, const QString& parent = {});
  void save();
  void saveAs(const QString& path);
  opad::json run(const std::string& command, opad::json args);
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
};
