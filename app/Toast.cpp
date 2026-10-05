#include "Toast.hpp"

#include <QCursor>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPainterPath>
#include <QToolButton>

#include <algorithm>

#include "Icons.hpp"
#include "Theme.hpp"

// ---------------------------------------------------------------- Toast
Toast::Toast(const QString& text, const QString& actionText, std::function<void()> callback, int ms, QWidget* host)
    : QFrame(host), m_ms(ms), m_callback(std::move(callback)) {
  setObjectName("toast");
  setAttribute(Qt::WA_NativeWindow);  // over the native 3D window
  setAttribute(Qt::WA_StyledBackground);
  setFocusPolicy(Qt::NoFocus);
  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(12, 6, 6, 6);
  row->setSpacing(8);
  m_label = new QLabel(text, this);
  m_label->setObjectName("toastText");
  m_label->setTextFormat(Qt::PlainText);
  row->addWidget(m_label, 1);
  if (!actionText.isEmpty()) {
    m_action = new QToolButton(this);
    m_action->setObjectName("toastAction");
    m_action->setText(actionText);
    m_action->setFocusPolicy(Qt::NoFocus);
    m_action->setCursor(Qt::PointingHandCursor);
    row->addWidget(m_action);
    connect(m_action, &QToolButton::clicked, this, [this] {
      if (m_callback) m_callback();
      dismiss();
    });
  }
  m_close = new QToolButton(this);
  m_close->setObjectName("toastClose");
  m_close->setToolTip(tr("Dismiss"));
  m_close->setFixedSize(20, 20);
  m_close->setIconSize(QSize(12, 12));
  m_close->setFocusPolicy(Qt::NoFocus);
  row->addWidget(m_close);
  connect(m_close, &QToolButton::clicked, this, &Toast::dismiss);
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

int Toast::naturalWidth() {
  ensurePolished();
  m_label->ensurePolished();
  const QMargins m = layout()->contentsMargins();
  const int spacing = layout()->spacing();
  return m.left() + m.right() + m_label->fontMetrics().horizontalAdvance(m_label->text()) + 2 + spacing + (m_action ? m_action->sizeHint().width() + spacing : 0) +
         m_close->width();
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
  auto* t = new Toast(text, actionText, std::move(callback), ms, m_host);
  // Its width: the text on one line if it fits, else wrapped at the widest a toast gets; then as tall as that asks.
  const int widest = std::min(kMaxWidth, std::max(160, m_host->width() - 2 * kMargin));
  const bool wrap = t->naturalWidth() > widest;
  t->findChild<QLabel*>("toastText")->setWordWrap(wrap);
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
