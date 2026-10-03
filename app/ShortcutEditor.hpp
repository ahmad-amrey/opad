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
void setScope(const QString& id,Scope scope);  // a command that says its own scope (CommandInfo::scope)
bool overlaps(const QString& first,const QString& second);
bool conflicts(const QKeySequence& first,const QKeySequence& second);
void migrate(QSettings&);
void initialize(QAction*,const QKeySequence&,QSettings&);
void updateTooltip(QAction*);
// A key that types a value into a running sketch tool (UI-16): a digit, the decimal point, a comma or a sign, unmodified
// (the keypad's too). A sketch command cannot have one.
bool typesValue(const QKeySequence&);
// While a sketch is open the commands scoped outside it let their keys go (the display styles stay in the menus, their
// 5, 6 and 7 used to switch the style while a value was typed) and take them back after. binding() is the key a command
// has whether or not it is let go, bind() sets it.
void suspendOutsideSketch(const QList<QAction*>&,bool sketching);
QKeySequence binding(const QAction*);
void bind(QAction*,const QKeySequence&);
// Second keys a command answers to while it keeps its default (UI-111): Redo on Ctrl+Shift+Z beside Ctrl+Y. The editor
// shows and changes the first key; a command given another key by the user has that key alone.
QList<QKeySequence> alternates(const QString& id);
// Drops every alternate that another command's key overlaps (Qt fires neither of two equal shortcuts): after all
// commands are made and after the editor applies.
void settleAlternates(const QList<QAction*>& actions);
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
  void describe();  // the selected command's details, for the key in the binding box
  void refresh();
  bool assign(int index,const QKeySequence& key);
  QVector<int> collisions(int index,const QKeySequence& key) const;
  QString name(int index) const;
};
