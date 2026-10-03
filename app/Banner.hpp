#pragma once
// A decision the document is waiting on, across the top of the viewport: a title, a line of why, and the choices as
// buttons (the file changed on disk, UI-56). A native child, so it floats over the OpenGL surface; it keeps to the
// viewport's top centre, below the chips row and a guided tool's prompt bar.
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
  QWidget* m_viewport;
  QLabel* m_icon;
  QLabel* m_title;
  QLabel* m_text;
  QHBoxLayout* m_buttons;
  QToolButton* m_close;
  QString m_state;
  Tone m_tone = Tone::Info;
  bool m_flash = false;
};
