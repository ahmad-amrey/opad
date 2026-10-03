#pragma once
#include <QLabel>
#include <QList>
#include <QPair>
#include <QTreeWidget>
#include <QWidget>
#include <functional>
#include <string>
#include <vector>

#include "opad/json.hpp"
#include "opad/util.hpp"

// What the panel shows: the selection's refs (one body ref for a body or component), or an op (a timeline marker).
struct PropertySubject {
  std::vector<opad::Ref> refs;
  std::string op;
};

// A section a feature area adds under the built-in rows (an asset's source and sync state, a part's number): a header,
// label/value rows, and links that run an action when clicked.
struct PropertySection {
  QString title;                                         // shown in capitals, like ADJACENT FACES
  QList<QPair<QString, QString>> rows;                   // label, value: translated and formatted by the area
  QList<QPair<QString, std::function<void()>>> actions;  // a link each ("Locate…"); the panel may be refilled by then
};

// Asked each time the panel fills (a new subject, its geometry measured, a width change, refresh()): O(1) from what the
// area knows, never geometry (measure on a worker, then refresh()). `props` are the rows shown; adds to `out`.
using PropertySectionProvider = std::function<void(const PropertySubject& subject, const opad::json& props, QList<PropertySection>& out)>;

// ---------------------------------------------------------------- properties
class PropertiesPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PropertiesPanel(QWidget* parent = nullptr);
  void showEntity(const QString& title, const QString& subtitle, const QString& id, const opad::json& props);
  void clear();
  void setSubject(PropertySubject subject);  // what showEntity's rows are about (MainWindow sets it first); clear() drops it
  const PropertySubject& subject() const { return m_subject; }
  void addSectionProvider(PropertySectionProvider provider);  // feature areas; sections in the order providers were added
  void refresh();  // fills again: a provider's sections changed
  QTreeWidget* table() const { return m_table; }  // benches
 signals:
  void faceChosen(int index);
 protected:
  bool eventFilter(QObject* o, QEvent* e) override;
 private:
  void addRow(const QString& key, const opad::json& v);
  void fill();
  opad::json m_props;      // what is shown, kept to lay the rows out again when the width changes
  int m_filledWidth = -1;  // value column width the rows were laid out for
  bool m_splitVectors = false;  // some vector did not fit on one line at that width
  PropertySubject m_subject;
  std::vector<PropertySectionProvider> m_providers;
  std::vector<std::function<void()>> m_actions;  // the providers' links as filled (kActionRole indexes it)
  QLabel* m_title;
  QLabel* m_id;
  QLabel* m_subtitle;
  QTreeWidget* m_table;
};
