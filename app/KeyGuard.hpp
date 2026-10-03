#pragma once
// One-key shortcuts (V hides, N starts a note, 5 shades, Del tombstones) are held back while a name is typed (UI-09):
//  - while an inline editor is open (editor() gives it, null when none) but has not got the keyboard, the typed keys go
//    to it instead of the window's commands, and it takes the keyboard;
//  - for kQuietMs after a modal window closed (a name typed right after "Edit unsaved copy"), a one-key shortcut of the
//    window is not taken: the key goes to the widget with the focus (which may still use it, as a sketch does).
// One key: a printing character (Shift allowed), Delete or Backspace, without Ctrl, Alt or Meta. Installed on the
// application (qApp->installEventFilter); everything else passes untouched.
#include <QElapsedTimer>
#include <QObject>
#include <functional>

class QKeyEvent;
class QWidget;

class KeyGuard : public QObject {
 public:
  static constexpr int kQuietMs = 300;
  KeyGuard(std::function<QWidget*()> editor, QObject* parent = nullptr);
  static bool oneKey(const QKeyEvent* e);
  bool quiet() const { return m_closed.isValid() && m_closed.elapsed() < kQuietMs; }  // a modal window closed just now

 protected:
  bool eventFilter(QObject* object, QEvent* event) override;

 private:
  std::function<QWidget*()> m_editor;
  QElapsedTimer m_closed;
};
