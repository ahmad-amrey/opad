#include "ToolValues.hpp"

#include <QApplication>
#include <QCursor>
#include <QKeyEvent>

#include "DimensionHandle.hpp"
#include "InputKeys.hpp"

ToolValues::ToolValues(QWidget* view, QObject* parent) : QObject(parent), m_view(view), m_input(new DynamicInput(view)) {
  qApp->installEventFilter(this);
  connect(m_input, &DynamicInput::optionEdited, this, [this](const QString& key, const QString& value) {
    if (edited) edited(key, value);
  });
  connect(m_input, &DynamicInput::committed, this, [this] {
    m_input->used();  // the tool has the typed values already
    m_input->hide();
    if (commit) commit();
  });
  connect(m_input, &DynamicInput::escaped, this, [this] {
    m_input->hide();
    if (escape) escape();
  });
  connect(m_input, &DynamicInput::dropped, m_input, &QWidget::hide);
}

void ToolValues::setHandle(DimensionHandle* handle) { m_handle = handle; }

DynamicInput::Field ToolValues::box(const QString& key, const QString& label, const QString& value) {
  DynamicInput::Field field;
  field.key = key;
  field.label = label;
  field.live = value;
  field.option = true;
  return field;
}

bool ToolValues::takes(const QKeyEvent* key) const {
  if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;  // the keypad's digits too
  const bool tab = key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab;
  if (!tab && (key->text().size() != 1 || !inputkeys::valueChar(key->text().front().unicode()))) return false;
  return (m_handle && m_handle->isVisible()) || (fields && !fields().isEmpty());
}

// At the shortcut override (so no window shortcut sees the key) and as it is pressed (then it types), from the view or one
// of its tool panels; a key typed into a box (a text field) is the box's own.
bool ToolValues::eventFilter(QObject* target, QEvent* event) {
  if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyPress) return false;
  auto* key = static_cast<QKeyEvent*>(event);
  if (!m_input || !takes(key) || !DynamicInput::takesKeysFrom(m_view, target)) return false;
  key->accept();
  if (event->type() == QEvent::KeyPress) type(key);
  return true;
}

void ToolValues::type(const QKeyEvent* key) {
  const bool tab = key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab;
  const bool back = key->key() == Qt::Key_Backtab || key->modifiers().testFlag(Qt::ShiftModifier);
  if (m_handle && m_handle->isVisible()) {  // the boxes by the arrow
    if (tab) m_handle->focusValue(back);
    else m_handle->type(key->text());
    return;
  }
  refresh();
  if (!m_input || !m_input->count()) return;
  show();
  if (tab) m_input->cycle(back);
  else m_input->type(key->text());
}

// Beside the pointer when it is over the view, else in the middle of it (typed while the pointer is over a panel).
void ToolValues::show() {
  if (!m_input || m_input->isVisible()) return;
  const QPoint pointer = m_view->mapFromGlobal(QCursor::pos());
  m_input->placeNear(m_view->rect().contains(pointer) ? pointer : m_view->rect().center());
  m_input->show();
  m_input->raise();
}

void ToolValues::refresh() {
  if (!m_input) return;
  m_input->setFields(fields ? fields() : QList<DynamicInput::Field>{});
  if (!m_input->count()) m_input->hide();
}

void ToolValues::reset() {
  if (!m_input) return;
  m_input->used();
  m_input->setFields({});
  m_input->hide();
}
