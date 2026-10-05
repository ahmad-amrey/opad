#include "Banner.hpp"

#include <QAbstractButton>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

#include "GuidedTool.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

Banner::Banner(QWidget* viewport) : QFrame(viewport), m_viewport(viewport) {
  setObjectName("banner");
  setAttribute(Qt::WA_NativeWindow);
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(12, 8, 8, 8);
  row->setSpacing(10);
  m_icon = new QLabel(this);
  m_icon->setFixedSize(18, 18);
  row->addWidget(m_icon, 0, Qt::AlignTop);
  auto* words = new QVBoxLayout;
  words->setSpacing(2);
  m_title = new QLabel(this);
  m_title->setObjectName("bannerTitle");
  m_title->setWordWrap(true);
  m_text = new QLabel(this);
  m_text->setObjectName("secondary");
  m_text->setWordWrap(true);
  words->addWidget(m_title);
  words->addWidget(m_text);
  row->addLayout(words, 1);
  m_buttons = new QHBoxLayout;
  m_buttons->setSpacing(6);
  row->addLayout(m_buttons);
  m_close = new QToolButton(this);
  m_close->setObjectName("bannerClose");
  m_close->setAutoRaise(true);
  m_close->setFixedSize(24, 24);
  m_close->setToolTip(tr("Later"));
  watchKeys(m_close);
  connect(m_close, &QToolButton::clicked, this, [this] { dismiss(); emit closed(); });
  row->addWidget(m_close, 0, Qt::AlignTop);
  connect(theme::notifier(), &theme::Notifier::changed, this, &Banner::restyle);
  viewport->installEventFilter(this);
  restyle();
  hide();
}

void Banner::present(const QString& state, Tone tone, const QString& title, const QString& text, const QString& details) {
  const QWidget* focus = window()->focusWidget();
  m_hadFocus = focus && isAncestorOf(focus) && !isHidden();
  m_state = state;
  m_tone = tone;
  setProperty("state", state);
  m_title->setText(title);
  m_text->setText(text);
  m_text->setVisible(!text.isEmpty());
  setToolTip(details);
  while (QLayoutItem* item = m_buttons->takeAt(0)) {
    if (QWidget* w = item->widget()) {  // the clicked one may still be on the stack: deleted later, gone at once
      w->hide();
      w->setProperty("action", QVariant());
      w->deleteLater();
    }
    delete item;
  }
  restyle();
  show();
  place();
}

QPushButton* Banner::addButton(const QString& action, const QString& text, std::function<void()> fn, bool primary) {
  auto* b = new QPushButton(text, this);
  if (primary) b->setObjectName("primary");
  b->setProperty("action", action);
  b->setCursor(Qt::PointingHandCursor);
  watchKeys(b);
  connect(b, &QPushButton::clicked, this, [fn = std::move(fn)] { fn(); });
  m_buttons->addWidget(b);
  b->show();  // at once (a layout shows a new child on the next event-loop turn)
  place();
  if (primary && m_hadFocus && m_tone != Tone::Danger) b->setFocus(Qt::TabFocusReason);
  return b;
}

void Banner::setEscape(QPushButton* button) {
  button->setProperty("escape", true);
  if (m_hadFocus && m_tone == Tone::Danger) button->setFocus(Qt::TabFocusReason);
}

void Banner::watchKeys(QWidget* w) {
  w->setFocusPolicy(Qt::TabFocus);  // Tab, never a click: the viewport keeps its keys
  w->installEventFilter(this);
}

void Banner::escape() {
  for (auto* b : findChildren<QPushButton*>())
    if (b->property("escape").toBool() && b->isVisibleTo(this)) return b->click();
  m_close->click();
}

QPushButton* Banner::button(const QString& action) const {
  for (auto* b : findChildren<QPushButton*>())
    if (b->property("action").toString() == action && b->isVisibleTo(this)) return b;
  return nullptr;
}

void Banner::dismiss() {
  if (const QWidget* focus = window()->focusWidget(); focus && isAncestorOf(focus)) m_viewport->setFocus(Qt::OtherFocusReason);
  m_state.clear();
  setProperty("state", QString());
  hide();
  place();  // the ones below move up
}

void Banner::flash() {
  m_flash = true;
  restyle();
  QTimer::singleShot(700, this, [this] { m_flash = false; restyle(); });
}

bool Banner::eventFilter(QObject* watched, QEvent* event) {
  if (watched == m_viewport && event->type() == QEvent::Resize) place();
  if (watched != m_viewport && (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
    const int key = static_cast<QKeyEvent*>(event)->key();
    const bool ours = key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space;
    if (event->type() == QEvent::ShortcutOverride && ours) {  // a focused button's, not the window's shortcut (Esc clears a measurement)
      event->accept();
      return true;
    }
    if (event->type() == QEvent::KeyPress && key == Qt::Key_Escape) {
      escape();
      return true;
    }
    if (event->type() == QEvent::KeyPress && (key == Qt::Key_Return || key == Qt::Key_Enter)) {  // Enter presses the focused one
      if (auto* b = qobject_cast<QAbstractButton*>(watched)) b->click();
      return true;
    }
  }
  return QFrame::eventFilter(watched, event);
}

void Banner::place() {
  int top = 48;  // below the chips row
  for (auto* prompt : m_viewport->findChildren<PromptBar*>(QString(), Qt::FindDirectChildrenOnly))
    if (prompt->isVisible()) top = std::max(top, prompt->geometry().bottom() + 8);
  const int w = std::max(240, std::min(760, m_viewport->width() - 24));
  // Every banner of the viewport, stacked in the order they were made (the file on disk above git's).
  for (Banner* b : m_viewport->findChildren<Banner*>(QString(), Qt::FindDirectChildrenOnly)) {
    if (b->isHidden()) continue;
    QLayout* l = b->layout();
    b->setFixedWidth(w);
    b->setFixedHeight(std::max(l->hasHeightForWidth() ? l->heightForWidth(w) : 0, l->minimumSize().height()));
    b->move((m_viewport->width() - w) / 2, top);
    b->raise();
    top += b->height() + 6;
  }
}

void Banner::restyle() {
  const Tokens& t = theme::current();
  const QColor accent = m_tone == Tone::Info ? t.sel : m_tone == Tone::Warning ? t.amber : t.red;
  QString css = QString("QFrame#banner { background: %1; border: 1px solid %2; border-%5: 3px solid %3; }"  // the stripe by the icon
                        "QLabel#bannerTitle { color: %4; font-weight: 600; }")
                    .arg(theme::css(t.bg2), theme::css(m_flash ? accent : t.line), theme::css(accent), theme::css(t.fg), isRightToLeft() ? "right" : "left");
  if (m_tone == Tone::Danger)  // the choice that loses something is red
    css += QString("QPushButton#primary { background: %1; border-color: %1; color: white; } QPushButton#primary:hover { background: %2; }")
               .arg(theme::css(t.red), theme::css(t.red.lighter(115)));
  setStyleSheet(css);
  m_icon->setPixmap(icons::pixmap(m_tone == Tone::Info ? "check" : "warning", accent, 18, devicePixelRatioF()));
  m_close->setIcon(icons::icon("close", t.fg2));
}
