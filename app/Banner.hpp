#pragma once
// A decision the document is waiting on, across the top of the viewport: a title, a line of why, and the choices as
// buttons (the file changed on disk, UI-56; a clone without OPAD's merge driver, UI-61). A native child, so it floats
// over the OpenGL surface; it keeps to the viewport's top centre, below the chips row and a guided tool's prompt bar,
// and several stack in the order they were made. Keyboard: Tab reaches its buttons (a click never takes the focus), Enter
// or Space presses the focused one, Esc presses the escape button (setEscape) or closes it; when it held the focus, a new
// state's primary button takes it (its escape button in a Danger state: Enter never loses something by itself).
#include <QFrame>
#include <functional>

class QHBoxLayout;
class QLabel;
class QPushButton;
class QToolButton;

class Banner : public QFrame {
  Q_OBJECT
 public:
  enum class Tone { Info, Warning, Danger };
  explicit Banner(QWidget* viewport);
  // Replaces what it shows and its buttons; `state` names it (property "state", read by benches).
  void present(const QString& state, Tone tone, const QString& title, const QString& text, const QString& details = {});
  // A button after the text; `action` names it (property "action").
  QPushButton* addButton(const QString& action, const QString& text, std::function<void()> fn, bool primary = false);
  void setEscape(QPushButton* button);  // what Esc presses (Cancel); without one Esc closes the banner
  QPushButton* button(const QString& action) const;
  QString state() const { return m_state; }
  void dismiss();  // hidden, no state
  void flash();    // draws the eye back to it (something it holds up was tried again)
 signals:
  void closed();  // its close button
 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
 private:
  void place();
  void restyle();
  void escape();
  void watchKeys(QWidget* w);
  QWidget* m_viewport;
  QLabel* m_icon;
  QLabel* m_title;
  QLabel* m_text;
  QHBoxLayout* m_buttons;
  QToolButton* m_close;
  QString m_state;
  Tone m_tone = Tone::Info;
  bool m_flash = false;
  bool m_hadFocus = false;  // a button of the state just replaced had the window's focus
};
