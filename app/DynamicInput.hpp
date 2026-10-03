#pragma once
// The value boxes of the tool that runs (TODO 11 UI-16): one small box per value its current step takes, beside the
// pointer, the pointer's (or the option's) value shown grey until something is typed. The tool hands it the keys typed
// over the view or a tool panel (a digit starts the first box); the boxes then have the keyboard: Tab / Shift+Tab (or a
// comma) go round them, Enter is the tool's (it uses the typed values), Esc undoes the edit, then drops the typed values,
// then is the tool's; Backspace in an empty box takes the tool's last point back; Up/Down or the wheel step a value.
// Rules without widgets: InputKeys.hpp. A native child that covers the view (it sits over OpenGL), masked to its boxes:
// each box is a rounded box of its own, in a row beside the pointer or, anchored, on what it measures (a length beside the
// rubber band's middle, an angle past its arc; UI-17), never over another; numbers stay left to right. A typed value the
// box is not being typed into is locked (accent border, padlock); one that does not evaluate is red, its tooltip says why. Embedded in another widget (the DimensionHandle by an arrow), one box that holds
// its value (`valued`) gets the same keys: the first key typed replaces the value, Esc undoes, then gives the keyboard back.
#include <QList>
#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWidget>
#include <functional>

class QAction;
class QLabel;
class QLineEdit;
class QToolButton;

class DynamicInput : public QWidget {
  Q_OBJECT
 public:
  struct Field {
    QString key, label;
    QString live;         // shown grey while nothing is typed: where the pointer is, or the option's value
    bool option = false;  // the tool's option `key`: typing sets it at once (it waits for what it applies to), Esc puts it back
    QString chip;         // a switch after the box (what an angle is measured from): a click emits chipClicked
    QString tip;          // more for the tooltip (the keys that switch a point's boxes)
    bool valued = false;  // the box holds the value itself (nothing is "typed" over it); Up/Down and the wheel emit stepped
    bool text = false;    // words, not a number: every printable key typed goes into it as it is (a comma, '@'), no stepping
  };
  // Beside the pointer over `view`; with a `host`, a plain part of that widget (no frame of its own), `view` taking the
  // keyboard back.
  explicit DynamicInput(QWidget* view, QWidget* host = nullptr);
  void setFields(const QList<Field>& fields);  // the step's boxes; while the keys stay the same the typed values stay
  int count() const { return int(m_boxes.size()); }
  QString key(int index) const { return index >= 0 && index < count() ? m_boxes[index].field.key : QString(); }
  QLineEdit* box(int index) const { return index >= 0 && index < count() ? m_boxes[index].edit : nullptr; }
  int current() const { return m_current; }  // the box being typed into, -1 none
  bool typed() const;                        // something typed and not used yet
  QString text(const QString& key) const;    // what was typed into that box (empty: the pointer gives it)
  bool editing() const;                      // a box has the keyboard
  void type(const QString& text);            // a value key typed elsewhere: into the box being typed (the first when none); a key
                                             // that is no part of a number into the text box
  void cycle(bool back);                     // Tab, Shift+Tab
  bool backspace();                          // the last character of the box being typed; false when it is empty
  void dropTyped();                          // Esc: the typed values go, option boxes put their old values back
  void used();                               // the typed values were used: forgotten, options keep them
  void setText(int index, const QString& text);  // typed into that box by the tool (a value carried over from another box)
  void select(int index);                        // that box is typed into next
  void setProblem(const QString& key, const QString& problem);  // why its text does not evaluate (red), empty: it does
  QString problem(const QString& key) const;
  // A key about to be typed into box `index`: true when the tool took it (a prefix that switches its boxes).
  void setKeyHook(std::function<bool(int index, QChar c)> hook) { m_keyHook = std::move(hook); }
  // Where a box sits: off `at` (view coordinates) the way `out` points, its label left out (the place says what it is).
  struct Anchor {
    bool on = false;
    QPointF at, out;
  };
  // The boxes anchored where they measure (by index), the others in a row `gap` px beside the pointer (view coordinates),
  // never under it. A cursor drawn on a grid node is up to half a step off the hidden pointer: the gap grows by as much.
  void placeNear(const QPoint& cursor, const QList<Anchor>& anchors = {}, int gap = 20);
  QRect boxesRect() const;  // where the boxes are, in the view
  QPixmap shot();           // the boxes as shown (benches)
  // Where a typed key may be the tool's: the view's window or one of its tool panels, and not a text field there.
  static bool takesKeysFrom(QWidget* view, QObject* target);
 signals:
  void optionEdited(const QString& key, const QString& value);  // typed into an option box, or its old value put back
  void typedChanged();
  void valueTyped(const QString& key);  // the text of that box changed (typed, undone, dropped)
  void dropped();                       // Esc dropped the typed values
  void chipClicked(const QString& key);
  void stepped(int index, double steps, Qt::KeyboardModifiers modifiers);  // Up/Down or the wheel in a valued box
  void committed();  // Enter in a box
  void escaped();    // Esc in a box with nothing left to undo there
  void undoPoint();  // Backspace in an empty box
 protected:
  bool eventFilter(QObject* target, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
 private:
  struct Box {
    Field field;
    QWidget* pill = nullptr;  // the box's own rounded frame (not embedded)
    QLabel* label = nullptr;
    QLineEdit* edit = nullptr;
    QAction* lock = nullptr;
    QToolButton* chip = nullptr;
    QString problem;
    QString before;        // the text when the box became the one typed into: Esc puts it back
    QString optionBefore;  // an option box: the option's value before anything was typed
    bool typed = false;
  };
  void edited(int index);
  void nudge(int index, double steps, Qt::KeyboardModifiers modifiers);
  void makeCurrent(int index, bool selectAll);
  void focusBox(int index);
  void giveBack();  // a box had the keyboard: back to the view
  void restyle();
  void fit();
  void arrange();  // the boxes placed, the mask on them
  bool anchored(int index) const { return index < m_anchors.size() && m_anchors[index].on; }
  QList<Box> m_boxes;
  QList<Anchor> m_anchors;
  int m_current = -1;
  bool m_wasTyped = false;
  int m_look = -1;
  QPoint m_cursor;
  int m_gap = 20;
  std::function<bool(int, QChar)> m_keyHook;
  QWidget* m_view;
  bool m_embedded;
};
