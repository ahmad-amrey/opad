#pragma once
// The Help menu's own windows (UI-108): the shortcuts cheat sheet, Getting started and Report a problem. The help area
// (HelpArea.cpp) opens them; what they show comes from the commands, their help records and the clips. Every key they
// name is the user's key now (keys::, TODO 11 wave 3), and they follow a change while open.
#include <QDialog>
#include <QFrame>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

class ClipView;
class QAction;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace help {
struct KeyRow {
  QString label;
  QList<QStringList> keys;  // each alternate's caps, left to right: Redo {Ctrl, Y}, {Ctrl, Shift, Z}
  QString search;           // what a search also finds besides the label and text(): Qt's names ("ctrl+/")
  QString text() const;     // as read: "Ctrl+Y / Ctrl+Shift+Z" (keys::joined, " / " between alternates)
};
struct KeyGroup {
  QString title;
  QList<KeyRow> rows;
};
// Every command with a key that works now, by its group (the registry's, as the palette shows it) in command order, the
// sketch's first while sketching; then the mouse of the navigation preset ("fusion", "solidworks", "onshape",
// "blender") and the keys every tool knows (Help for this tool's own row only while it has a key).
QList<KeyGroup> keyGroups(const QList<QAction*>& actions, bool sketching, const QString& preset);
QList<KeyRow> mouseRows(const QString& preset);  // orbit, pan, zoom, select, select in a window
KeyRow keyRow(const QString& label, const QList<QKeySequence>& keys);  // a row of keys::caps, alternates in order
// Report a problem: what the user wrote and the facts about the program and the computer, as one text.
QString problemReport(const QString& description, const QStringList& facts);
}  // namespace help

// Help > Shortcuts cheat sheet: every key at a glance in columns, searched by name or key; Esc or the cheat sheet's own
// key (help.shortcuts, whatever the user bound) closes it.
class ShortcutSheet : public QWidget {
  Q_OBJECT
 public:
  explicit ShortcutSheet(QWidget* parent = nullptr);
  void setGroups(const QList<help::KeyGroup>& groups);
  void setFilter(const QString& text);
  QStringList titles() const;  // the groups shown now, in order
  QStringList shown() const;   // the rows shown now: "label keys"
  QKeySequence closeKey() const;  // besides Esc: the key that opened it
 signals:
  void editRequested();  // Change shortcuts…

 protected:
  void keyPressEvent(QKeyEvent* e) override;
  bool eventFilter(QObject* watched, QEvent* event) override;  // the closing key in the empty search field

 private:
  struct Row {
    QWidget* widget;
    QString text, search;
    int group;
  };
  void rebind();  // the closing key follows help.shortcuts'
  QLineEdit* m_search;
  class QShortcut* m_again;
  QWidget* m_page;
  QList<QWidget*> m_groups;
  QStringList m_titles;
  QList<Row> m_rows;
};

// Help > Getting started: six short lessons (move around, open, measure, sketch, make it solid, find any command), each
// with its clip and a button that runs the command it teaches.
class GettingStarted : public QWidget {
  Q_OBJECT
 public:
  struct Lesson {
    QString title, text, clip, command;
  };
  // `lookup`: a command's QAction (null: none); `run` runs one (the help area switches the workspace first when needed).
  GettingStarted(std::function<QAction*(const QString&)> lookup, std::function<void(const QString&)> run, QWidget* parent = nullptr);
  static QList<Lesson> lessons(const QString& preset);  // with the user's keys now (help::expand)
  void setPreset(const QString& preset);  // the navigation lesson follows the mouse preset
  void open(int lesson);  // shown and raised at that lesson
  int current() const;
  ClipView* clip() const { return m_clip; }
  QPushButton* tryButton() const { return m_try; }
  QLabel* needs() const { return m_needs; }  // what the lesson's command needs when it is not available now

 protected:
  void keyPressEvent(QKeyEvent* e) override;

 private:
  void refresh();
  std::function<QAction*(const QString&)> m_lookup;
  std::function<void(const QString&)> m_run;
  QString m_preset = "fusion";
  QList<Lesson> m_lessons;
  QListWidget* m_list;
  QLabel *m_title, *m_text, *m_needs;
  ClipView* m_clip;
  QPushButton *m_try, *m_next;
  QPointer<QAction> m_action;
  QMetaObject::Connection m_changed;
};

// The coach card of an empty document (UI-108): how a design starts (sketch on a plane, a closed profile, extrude) with
// the extrude clip and buttons for the first step, a native child over the bottom of the viewport. Its × hides it for
// this document, Don't show again for good (the help area keeps setting help/coach).
class CoachCard : public QFrame {
  Q_OBJECT
 public:
  explicit CoachCard(QWidget* viewport);
  ClipView* clip() const { return m_clip; }
  QPushButton* button(const QString& command) const;  // the button that runs a command: design.sketch, design.box, file.import, help.start
  QToolButton* closeButton() const { return m_close; }
  QPushButton* neverButton() const { return m_never; }
  QString steps() const;  // the line of first steps, with the user's keys
 signals:
  void run(const QString& command);
  void dismissed();   // ×: not for this document
  void neverAgain();  // Don't show again

 private:
  void restyle();
  void rekey();  // the first steps' keys
  ClipView* m_clip;
  QLabel* m_steps;
  QToolButton* m_close;
  QPushButton* m_never;
};

// Help > Report a problem: what happened, in the user's words, and the facts OPAD knows (version, system, the document's
// kind and size, never its path); Copy, Save, or open the issue page with the report on the clipboard. Nothing is sent
// by itself.
class ProblemReport : public QDialog {
  Q_OBJECT
 public:
  ProblemReport(const QStringList& facts, const QString& issueUrl, QWidget* parent = nullptr);
  QString report() const;
  QPlainTextEdit* description() const { return m_description; }
  void copy();  // the report to the clipboard

 private:
  QStringList m_facts;
  QString m_url;
  QPlainTextEdit* m_description;
  QLabel* m_status;
};
