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

void KeyGuard::type(QWidget* editor, const Key& key) {  // into the editor, which takes the keyboard from here on
  if (!editor->hasFocus()) {
    editor->window()->activateWindow();
    editor->setFocus(Qt::OtherFocusReason);
  }
  QKeyEvent press(QEvent::KeyPress, key.key, key.modifiers, key.text);
  QCoreApplication::sendEvent(editor, &press);
}

void KeyGuard::release(const std::function<void()>& then) {
  std::vector<Key> held;
  if (m_holds > 0 && --m_holds == 0) held.swap(m_held);
  if (then) then();  // keys typed meanwhile (into a dialog it opens) are its own
  if (QWidget* editor = held.empty() || !m_editor ? nullptr : m_editor())
    for (const Key& key : held) type(editor, key);
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
  if (!editor && m_holds > 0) {  // kept for the editor the resumed command opens
    if (type == QEvent::KeyPress) m_held.push_back({key->key(), key->modifiers(), key->text()});
    event->accept();
    return true;
  }
  if (editor && object != editor && !editor->hasFocus()) {
    if (type == QEvent::KeyPress) this->type(editor, {key->key(), key->modifiers(), key->text()});
    event->accept();  // an accepted override: no shortcut, the key comes as a press
    return true;
  }
  if (type == QEvent::ShortcutOverride && quiet()) event->accept();  // no window shortcut; a widget's own keys (a sketch's) still work
  return false;
}
