#pragma once
// One-key shortcuts (V hides, N starts a note, 5 shades, Del tombstones) are held back while a name is typed (UI-09):
//  - while an inline editor is open (editor() gives it, null when none) but has not got the keyboard, the typed keys go
//    to it instead of the window's commands, and it takes the keyboard;
//  - for kQuietMs after a modal window closed (a name typed right after "Edit unsaved copy"), a one-key shortcut of the
//    window is not taken: the key goes to the widget with the focus (which may still use it, as a sketch does);
//  - between hold() and release() (a command that resumes after a wait: Rename after Edit unsaved copy), one-key presses
//    are kept; release(command) runs the command and gives them to the editor it opened (dropped without one), never to
//    shortcuts.
// One key: a printing character (Shift allowed), Delete or Backspace, without Ctrl, Alt or Meta. Installed on the
// application (qApp->installEventFilter); everything else passes untouched.
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <functional>
#include <vector>

class QKeyEvent;
class QWidget;

class KeyGuard : public QObject {
 public:
  static constexpr int kQuietMs = 300;
  KeyGuard(std::function<QWidget*()> editor, QObject* parent = nullptr);
  static bool oneKey(const QKeyEvent* e);
  bool quiet() const { return m_closed.isValid() && m_closed.elapsed() < kQuietMs; }  // a modal window closed just now
  void hold() { ++m_holds; }  // nests: the last release() delivers
  void release(const std::function<void()>& then = {});  // runs `then` (the resumed command) unheld, then delivers
  bool holding() const { return m_holds > 0; }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override;

 private:
  struct Key {
    int key;
    Qt::KeyboardModifiers modifiers;
    QString text;
  };
  std::function<QWidget*()> m_editor;
  QElapsedTimer m_closed;
  int m_holds = 0;
  std::vector<Key> m_held;
  void type(QWidget* editor, const Key& key);
};
