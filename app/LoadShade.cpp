#include "LoadShade.hpp"

#include <QFontMetrics>
#include <QPainter>

#include <algorithm>

#include "Theme.hpp"

// ---------------------------------------------------------------- LoadShade
LoadShade::LoadShade(QWidget* owner) : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus) {
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setCursor(Qt::BusyCursor);
  m_timer.setInterval(30);
  connect(&m_timer, &QTimer::timeout, this, [this] {
    m_angle = (m_angle + 10) % 360;
    update(QRect(m_spinner - QPoint(40, 40), QSize(80, 80)));
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void LoadShade::place(const QRect& globalArea, const QPoint& spinnerCentreGlobal) {
  setGeometry(globalArea);
  m_spinner = spinnerCentreGlobal - globalArea.topLeft();
  update();
}

void LoadShade::setStatus(const QString& title, const QString& phase, int percent) {
  m_title = title;
  m_phase = percent >= 0 && !phase.isEmpty() ? QString("%1 · %2%").arg(phase).arg(percent) : phase;
  update();
}

QString LoadShade::text() const { return m_phase.isEmpty() ? m_title : m_title + QChar('\n') + m_phase; }

// The spinner's card: 72 px square, or wide enough for the text under the spinner (at most 440 px, elided beyond).
QRect LoadShade::card() const {
  if (m_title.isEmpty() && m_phase.isEmpty()) return QRect(m_spinner.x() - 36, m_spinner.y() - 36, 72, 72);
  QFont bold = font();
  bold.setBold(true);
  const int text = std::max(QFontMetrics(bold).horizontalAdvance(m_title), fontMetrics().horizontalAdvance(m_phase));
  const int width = std::clamp(text + 40, 200, 440);
  return QRect(m_spinner.x() - width / 2, m_spinner.y() - 36, width, 72 + 2 * fontMetrics().height() + 10);
}

void LoadShade::showEvent(QShowEvent*) { m_timer.start(); }
void LoadShade::hideEvent(QHideEvent*) { m_timer.stop(); }

void LoadShade::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), QColor(0, 0, 0, t.dark ? 120 : 80));  // a real darkening, in both themes
  const QRect box = card();
  p.setPen(QPen(t.line, 1));
  p.setBrush(t.bg2);
  p.drawRoundedRect(box, 6, 6);
  const QRect ring(m_spinner.x() - 16, m_spinner.y() - 16, 32, 32);
  if (!m_title.isEmpty() || !m_phase.isEmpty()) {
    const int line = fontMetrics().height(), width = box.width() - 24;
    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.setPen(t.fg);
    p.drawText(QRect(box.x() + 12, box.y() + 72, width, line), Qt::AlignHCenter, QFontMetrics(bold).elidedText(m_title, Qt::ElideMiddle, width));
    p.setFont(font());
    p.setPen(t.fg2);
    p.drawText(QRect(box.x() + 12, box.y() + 72 + line + 2, width, line), Qt::AlignHCenter, fontMetrics().elidedText(m_phase, Qt::ElideRight, width));
  }
  p.setPen(QPen(t.bg4, 3));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(ring);
  p.setPen(QPen(t.sel, 3, Qt::SolidLine, Qt::RoundCap));
  p.drawArc(ring, (90 - m_angle) * 16, -100 * 16);
}
