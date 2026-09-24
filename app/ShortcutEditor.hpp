#pragma once
#include <QAction>
#include <QDialog>
#include <QKeySequence>
#include <QSettings>
#include <QVector>
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QKeySequenceEdit;
class QLabel;

namespace shortcuts {
enum Scope { Everywhere, OutsideSketch, SketchOnly };
Scope scope(const QString& id);
bool overlaps(const QString& first,const QString& second);
bool conflicts(const QKeySequence& first,const QKeySequence& second);
void migrate(QSettings&);
void initialize(QAction*,const QKeySequence&,QSettings&);
void updateTooltip(QAction*);
}

class ShortcutEditor : public QDialog {
 public:
  explicit ShortcutEditor(const QList<QAction*>& actions,QWidget* parent=nullptr);
  void accept() override;
 private:
  struct Entry { QAction* action; QTreeWidgetItem* item; QKeySequence key; QKeySequence initial; };
  QVector<Entry> m_entries;
  QTreeWidget* m_tree;
  QLineEdit* m_search;
  QKeySequenceEdit* m_lookup;
  QKeySequenceEdit* m_binding;
  QLabel* m_details;
  int current() const;
  void filter();
  void selectCurrent();
  void refresh();
  bool assign(int index,const QKeySequence& key);
  QVector<int> collisions(int index,const QKeySequence& key) const;
  QString name(int index) const;
};
