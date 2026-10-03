#pragma once
// The value boxes of the tool that runs (TODO 11 UI-16): one small box per value its current step takes, beside the
// pointer, the pointer's (or the option's) value shown grey until something is typed. The tool hands it the keys typed
// over the view or a tool panel (a digit starts the first box); the boxes then have the keyboard: Tab / Shift+Tab (or a
// comma) go round them, Enter is the tool's (it uses the typed values), Esc undoes the edit, then drops the typed values,
// then is the tool's; Backspace in an empty box takes the tool's last point back; Up/Down step a value. Rules without
// widgets: InputKeys.hpp. A native child of the view (it sits over OpenGL), rounded by a mask; numbers stay left to right.
#include <QList>
#include <QPoint>
#include <QString>
#include <QWidget>

class QLabel;
class QLineEdit;

class DynamicInput : public QWidget {
  Q_OBJECT
 public:
  struct Field {
    QString key, label;
    QString live;         // shown grey while nothing is typed: where the pointer is, or the option's value
    bool option = false;  // the tool's option `key`: typing sets it at once (it waits for what it applies to), Esc puts it back
  };
  explicit DynamicInput(QWidget* view);
  void setFields(const QList<Field>& fields);  // the step's boxes; while the keys stay the same the typed values stay
  int count() const { return int(m_boxes.size()); }
  QString key(int index) const { return index >= 0 && index < count() ? m_boxes[index].field.key : QString(); }
  QLineEdit* box(int index) const { return index >= 0 && index < count() ? m_boxes[index].edit : nullptr; }
  int current() const { return m_current; }  // the box being typed into, -1 none
  bool typed() const;                        // something typed and not used yet
  QString text(const QString& key) const;    // what was typed into that box (empty: the pointer gives it)
  bool editing() const;                      // a box has the keyboard
  void type(const QString& text);            // a value key typed elsewhere: into the box being typed (the first when none)
  void cycle(bool back);                     // Tab, Shift+Tab
  bool backspace();                          // the last character of the box being typed; false when it is empty
  void dropTyped();                          // Esc: the typed values go, option boxes put their old values back
  void used();                               // the typed values were used: forgotten, options keep them
  void placeNear(const QPoint& cursor);      // beside the pointer (view coordinates), never under it
  // Where a typed key may be the tool's: the view's window or one of its tool panels, and not a text field there.
  static bool takesKeysFrom(QWidget* view, QObject* target);
 signals:
  void optionEdited(const QString& key, const QString& value);  // typed into an option box, or its old value put back
  void typedChanged();
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
    QLabel* label = nullptr;
    QLineEdit* edit = nullptr;
    QString before;        // the text when the box became the one typed into: Esc puts it back
    QString optionBefore;  // an option box: the option's value before anything was typed
    bool typed = false;
  };
  void edited(int index);
  void makeCurrent(int index, bool selectAll);
  void focusBox(int index);
  void giveBack();  // a box had the keyboard: back to the view
  void restyle();
  void fit();
  QList<Box> m_boxes;
  int m_current = -1;
  bool m_wasTyped = false;
  int m_look = -1;
  QPoint m_cursor;
};
