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
// A command whose key no setting changes (its action's "fixedShortcut", CommandInfo::fixedKey): Esc, which every tool,
// panel and prompt names. The editor lists it with the reserved keys; initialize() ignores a saved one.
bool fixedKey(const QAction*);
void updateTooltip(QAction*);
// A key that types a value into a running sketch tool (UI-16): a digit, the decimal point, a comma or a sign, unmodified
// (the keypad's too). A sketch command cannot have one.
bool typesValue(const QKeySequence&);
// While a sketch is open the commands scoped outside it let their keys go (the display styles stay in the menus, their
// 5, 6 and 7 used to switch the style while a value was typed) and take them back after. binding() is the key a command
// has whether or not it is let go (bindings() with the alternate), bind() sets them.
void suspendOutsideSketch(const QList<QAction*>&,bool sketching);
QKeySequence binding(const QAction*);
QList<QKeySequence> bindings(const QAction*);
void bind(QAction*,const QList<QKeySequence>&);
inline void bind(QAction* a,const QKeySequence& key) {bind(a,QList<QKeySequence>{key});}  // the key alone
// A second key a command answers to (UI-111): Redo on Ctrl+Shift+Z beside Ctrl+Y by default, while it keeps its default
// key. The editor shows and changes one beside the key, saved under shortcutAlternates/<id> (an older build ignores it;
// empty: none); the key and it are the action's shortcuts() in that order.
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
  struct Entry {
    QAction* action; QTreeWidgetItem* item; QKeySequence key; QKeySequence initial; QKeySequence alternate, initialAlternate;
    QKeySequence& slot(int s) { return s ? alternate : key; }
    const QKeySequence& slot(int s) const { return s ? alternate : key; }
  };
  struct Hit { int entry, slot; };  // slot 0: the key, 1: the alternate
  QVector<Entry> m_entries;
  QTreeWidget* m_tree;
  QLineEdit* m_search;
  QKeySequenceEdit* m_lookup;
  QKeySequenceEdit* m_binding;
  QKeySequenceEdit* m_alternate;
  QLabel* m_details;
  int current() const;
  void filter();
  void selectCurrent();
  void describe();  // the selected command's details, for the key in the binding box
  void refresh();
  bool assign(int index,const QKeySequence& key,int slot=0);
  QVector<Hit> collisions(int index,const QKeySequence& key,int slot=0) const;
  QString name(int index) const;
  QString name(const Hit& hit) const;  // "(alternate)" after an alternate's
};
