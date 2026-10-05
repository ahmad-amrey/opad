#include "Toast.hpp"

#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "Icons.hpp"
#include "Theme.hpp"

// ---------------------------------------------------------------- Toast
Toast::Toast(const QString& text, const QString& actionText, std::function<void()> callback, int ms, QWidget* host)
    : Toast(text, QString(), actionText.isEmpty() ? QList<Action>() : QList<Action>{{actionText, std::move(callback)}}, ms, host) {}

Toast::Toast(const QString& text, const QString& detail, const QList<Action>& actions, int ms, QWidget* host)
    : QFrame(host), m_ms(ms), m_stacked(!detail.isEmpty() || actions.size() > 1) {
  setObjectName("toast");
  setAttribute(Qt::WA_NativeWindow);  // over the native 3D window
  setAttribute(Qt::WA_StyledBackground);
  setFocusPolicy(Qt::NoFocus);
  m_label = new QLabel(text, this);
  m_label->setObjectName("toastText");
  m_label->setTextFormat(Qt::PlainText);
  if (!detail.isEmpty()) {
    m_detail = new QLabel(detail, this);
    m_detail->setObjectName("toastDetail");
    m_detail->setTextFormat(Qt::PlainText);
  }
  for (const Action& a : actions) {
    auto* button = new QToolButton(this);
    button->setObjectName("toastAction");
    button->setText(a.text);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QToolButton::clicked, this, [this, callback = a.callback] {
      if (property("dismissed").toBool()) return;  // a second click before it went
      if (callback) callback();
      dismiss();
    });
    m_actions << button;
  }
  m_close = new QToolButton(this);
  m_close->setObjectName("toastClose");
  m_close->setToolTip(tr("Dismiss"));
  m_close->setFixedSize(20, 20);
  m_close->setIconSize(QSize(12, 12));
  m_close->setFocusPolicy(Qt::NoFocus);
  connect(m_close, &QToolButton::clicked, this, &Toast::dismiss);
  if (!m_stacked) {  // one row: text, the action, ×
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(12, 6, 6, 6);
    row->setSpacing(8);
    row->addWidget(m_label, 1);
    for (QToolButton* b : m_actions) row->addWidget(b);
    row->addWidget(m_close);
  } else {  // the text and ×, the detail, then the answers at the end of their row (mirrored right to left)
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(12, 8, 6, 6);
    column->setSpacing(4);
    auto* top = new QHBoxLayout;
    top->setSpacing(8);
    top->addWidget(m_label, 1);
    top->addWidget(m_close, 0, Qt::AlignTop);
    column->addLayout(top);
    if (m_detail) {
      auto* line = new QHBoxLayout;
      line->setContentsMargins(0, 0, 26, 0);  // under the text, not the ×
      line->addWidget(m_detail, 1);
      column->addLayout(line);
    }
    auto* answers = new QHBoxLayout;
    answers->setSpacing(4);
    answers->addStretch(1);
    for (QToolButton* b : m_actions) answers->addWidget(b);
    column->addSpacing(2);
    column->addLayout(answers);
  }
  m_timer.setSingleShot(true);
  connect(&m_timer, &QTimer::timeout, this, [this] {
    if (rect().contains(mapFromGlobal(QCursor::pos()))) return m_timer.start(1500);  // held while the pointer is on it
    dismiss();
  });
  if (m_ms > 0) m_timer.start(m_ms);
  refreshIcons();
  connect(theme::notifier(), &theme::Notifier::changed, this, &Toast::refreshIcons);
}

QString Toast::text() const { return m_label->text(); }
QString Toast::detail() const { return m_detail ? m_detail->text() : QString(); }

int Toast::naturalWidth() {
  ensurePolished();
  m_label->ensurePolished();
  const QMargins m = layout()->contentsMargins();
  const int text = m_label->fontMetrics().horizontalAdvance(m_label->text()) + 2;
  if (!m_stacked) {
    const int spacing = layout()->spacing();
    return m.left() + m.right() + text + spacing + (m_actions.isEmpty() ? 0 : m_actions.first()->sizeHint().width() + spacing) + m_close->width();
  }
  int answers = 0;
  for (QToolButton* b : m_actions) answers += b->sizeHint().width() + 4;
  int detail = 0;
  if (m_detail) {
    m_detail->ensurePolished();
    detail = m_detail->fontMetrics().horizontalAdvance(m_detail->text()) + 2 + 26;
  }
  return m.left() + m.right() + std::max({text + 8 + m_close->width(), detail, answers});
}

void Toast::setWrapped(bool on) {
  m_label->setWordWrap(on);
  if (m_detail) m_detail->setWordWrap(on);
}

void Toast::refreshIcons() { m_close->setIcon(icons::icon("close", theme::current().fg3)); }

void Toast::dismiss() {
  if (property("dismissed").toBool()) return;
  setProperty("dismissed", true);
  m_timer.stop();
  hide();
  emit dismissed(this);
  deleteLater();
}

void Toast::resizeEvent(QResizeEvent* e) {
  QFrame::resizeEvent(e);
  QPainterPath path;
  path.addRoundedRect(QRectF(rect()), 4, 4);
  setMask(QRegion(path.toFillPolygon().toPolygon()));
}

// ---------------------------------------------------------------- ToastStack
ToastStack::ToastStack(QWidget* host) : QObject(host), m_host(host) { host->installEventFilter(this); }

Toast* ToastStack::toast(const QString& text, const QString& actionText, std::function<void()> callback, int ms) {
  return add(new Toast(text, actionText, std::move(callback), ms, m_host));
}

Toast* ToastStack::ask(const QString& text, const QString& detail, const QList<Toast::Action>& answers, int ms) {
  return add(new Toast(text, detail, answers, ms, m_host));
}

Toast* ToastStack::add(Toast* t) {
  // Its width: the text on one line if it fits, else wrapped at the widest a toast gets; then as tall as that asks.
  const int widest = std::min(kMaxWidth, std::max(160, m_host->width() - 2 * kMargin));
  const bool wrap = t->naturalWidth() > widest;
  t->setWrapped(wrap);
  t->layout()->activate();
  const int width = wrap ? widest : t->sizeHint().width();
  t->setFixedSize(width, std::max(32, wrap ? t->layout()->heightForWidth(width) : t->sizeHint().height()));
  connect(t, &Toast::dismissed, this, [this](Toast* gone) {
    m_toasts.removeAll(gone);
    place();
  });
  m_toasts.removeAll(nullptr);
  m_toasts << t;
  while (m_toasts.size() > kMax) m_toasts.first()->dismiss();  // dismissed() takes it off the list
  t->show();
  place();
  return t;
}

QList<Toast*> ToastStack::toasts() const {
  QList<Toast*> out;
  for (const QPointer<Toast>& t : m_toasts)
    if (t && t->isVisible()) out << t;
  return out;
}

void ToastStack::clear() {
  for (Toast* t : toasts()) t->dismiss();
}

void ToastStack::place() {
  int bottom = m_host->height() - kMargin;
  const QList<Toast*> shown = toasts();
  for (auto it = shown.rbegin(); it != shown.rend(); ++it) {  // newest lowest
    Toast* t = *it;
    t->move((m_host->width() - t->width()) / 2, bottom - t->height());
    t->raise();
    bottom -= t->height() + kGap;
  }
}

void ToastStack::setHost(QWidget* host) {
  if (!host || host == m_host) return;
  m_host->removeEventFilter(this);
  m_host = host;
  host->installEventFilter(this);
  for (const QPointer<Toast>& t : m_toasts) {
    if (!t) continue;
    const bool shown = t->isVisible();
    t->setParent(host);  // hides it
    t->setVisible(shown);
  }
  place();
}

bool ToastStack::eventFilter(QObject* o, QEvent* e) {
  if (o == m_host && e->type() == QEvent::Resize) place();
  return QObject::eventFilter(o, e);
}
