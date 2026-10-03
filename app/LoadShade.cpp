#include "LoadShade.hpp"

#include <QPainter>

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

void LoadShade::showEvent(QShowEvent*) { m_timer.start(); }
void LoadShade::hideEvent(QHideEvent*) { m_timer.stop(); }

void LoadShade::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), QColor(0, 0, 0, t.dark ? 120 : 80));  // a real darkening, in both themes
  const QRect card(m_spinner.x() - 36, m_spinner.y() - 36, 72, 72);
  p.setPen(QPen(t.line, 1));
  p.setBrush(t.bg2);
  p.drawRoundedRect(card, 6, 6);
  const QRect ring = card.adjusted(20, 20, -20, -20);
  p.setPen(QPen(t.bg4, 3));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(ring);
  p.setPen(QPen(t.sel, 3, Qt::SolidLine, Qt::RoundCap));
  p.drawArc(ring, (90 - m_angle) * 16, -100 * 16);
}
