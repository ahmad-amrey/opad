#pragma once
#include <QLabel>
#include <QTreeWidget>
#include <QWidget>

#include "opad/json.hpp"

// ---------------------------------------------------------------- properties
class PropertiesPanel : public QWidget {
  Q_OBJECT
 public:
  explicit PropertiesPanel(QWidget* parent = nullptr);
  void showEntity(const QString& title, const QString& subtitle, const QString& id, const opad::json& props);
  void clear();
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
  QLabel* m_title;
  QLabel* m_id;
  QLabel* m_subtitle;
  QTreeWidget* m_table;
};
