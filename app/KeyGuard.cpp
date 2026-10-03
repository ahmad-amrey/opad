#include "KeyGuard.hpp"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QWidget>

KeyGuard::KeyGuard(std::function<QWidget*()> editor, QObject* parent) : QObject(parent), m_editor(std::move(editor)) {}

bool KeyGuard::oneKey(const QKeyEvent* e) {
  if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
  if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) return true;
  return (e->key() >= Qt::Key_Space && e->key() <= Qt::Key_ydiaeresis) || (!e->text().isEmpty() && e->text().at(0).isPrint());
}

bool KeyGuard::eventFilter(QObject* object, QEvent* event) {
  const auto type = event->type();
  if (type == QEvent::Hide) {
    if (auto* w = qobject_cast<QWidget*>(object); w && w->isWindow() && w->isModal()) m_closed.start();
    return false;
  }
  if (type != QEvent::ShortcutOverride && type != QEvent::KeyPress) return false;
  auto* key = static_cast<QKeyEvent*>(event);
  if (!oneKey(key)) return false;
  QWidget* editor = m_editor ? m_editor() : nullptr;
  if (editor && object != editor && !editor->hasFocus()) {
    if (type == QEvent::KeyPress) {  // into the editor, which takes the keyboard from here on
      editor->window()->activateWindow();
      editor->setFocus(Qt::OtherFocusReason);
      QKeyEvent press(QEvent::KeyPress, key->key(), key->modifiers(), key->text(), key->isAutoRepeat(), key->count());
      QCoreApplication::sendEvent(editor, &press);
    }
    event->accept();  // an accepted override: no shortcut, the key comes as a press
    return true;
  }
  if (type == QEvent::ShortcutOverride && quiet()) event->accept();  // no window shortcut; a widget's own keys (a sketch's) still work
  return false;
}
