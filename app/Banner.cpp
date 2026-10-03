#include "Banner.hpp"

#include <QEvent>
#include <QHBoxLayout>
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
  m_close->setFocusPolicy(Qt::NoFocus);
  m_close->setToolTip(tr("Later"));
  connect(m_close, &QToolButton::clicked, this, [this] { dismiss(); emit closed(); });
  row->addWidget(m_close, 0, Qt::AlignTop);
  connect(theme::notifier(), &theme::Notifier::changed, this, &Banner::restyle);
  viewport->installEventFilter(this);
  restyle();
  hide();
}

void Banner::present(const QString& state, Tone tone, const QString& title, const QString& text, const QString& details) {
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
  b->setFocusPolicy(Qt::NoFocus);
  b->setCursor(Qt::PointingHandCursor);
  connect(b, &QPushButton::clicked, this, [fn = std::move(fn)] { fn(); });
  m_buttons->addWidget(b);
  b->show();  // at once (a layout shows a new child on the next event-loop turn)
  place();
  return b;
}

QPushButton* Banner::button(const QString& action) const {
  for (auto* b : findChildren<QPushButton*>())
    if (b->property("action").toString() == action && b->isVisibleTo(this)) return b;
  return nullptr;
}

void Banner::dismiss() {
  m_state.clear();
  setProperty("state", QString());
  hide();
}

void Banner::flash() {
  m_flash = true;
  restyle();
  QTimer::singleShot(700, this, [this] { m_flash = false; restyle(); });
}

bool Banner::eventFilter(QObject* watched, QEvent* event) {
  if (watched == m_viewport && event->type() == QEvent::Resize) place();
  return QFrame::eventFilter(watched, event);
}

void Banner::place() {
  if (isHidden()) return;
  int top = 48;  // below the chips row
  for (auto* prompt : m_viewport->findChildren<PromptBar*>(QString(), Qt::FindDirectChildrenOnly))
    if (prompt->isVisible()) top = std::max(top, prompt->geometry().bottom() + 8);
  const int w = std::max(240, std::min(760, m_viewport->width() - 24));
  setFixedWidth(w);
  setFixedHeight(std::max(layout()->hasHeightForWidth() ? layout()->heightForWidth(w) : 0, layout()->minimumSize().height()));
  move((m_viewport->width() - w) / 2, top);
  raise();
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
