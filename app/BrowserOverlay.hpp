#pragma once
#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QPropertyAnimation>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QCursor>

// A native child of the scene: collapsed until hovered, never steals an active drag.
class BrowserOverlay : public QFrame {
 public:
  BrowserOverlay(QWidget* browser, QWidget* parent) : QFrame(parent), m_browser(browser), m_animation(this, "maximumHeight") {
    setAttribute(Qt::WA_NativeWindow);
    setObjectName("browserOverlay");
    setStyleSheet("QFrame#browserOverlay { background: palette(base); border: 1px solid palette(mid); border-radius: 6px; }");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 0, 6, 6); layout->setSpacing(0);
    m_header = new QLabel(tr("Browser"), this); m_header->setFixedHeight(30);
    layout->addWidget(m_header); layout->addWidget(browser);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    m_animation.setDuration(180); m_animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_animation, &QPropertyAnimation::valueChanged, this, [this] { resize(width(), maximumHeight()); });
    m_auto = QSettings().value("ui/browserAutoHide", true).toBool();
    setMaximumHeight(m_auto ? 30 : m_expandedHeight); resize(320, maximumHeight());
    m_poll.setInterval(100);
    connect(&m_poll, &QTimer::timeout, this, [this] {
      if (!isVisible() || QApplication::mouseButtons() != Qt::NoButton) return;
      QWidget* focus = QApplication::focusWidget();
      const bool active = rect().contains(mapFromGlobal(QCursor::pos())) || (focus && isAncestorOf(focus));
      expand(!m_auto || active || QApplication::activePopupWidget());
    });
    m_poll.start();
  }
  void setAutoHide(bool on) { m_auto = on; QSettings().setValue("ui/browserAutoHide", on); expand(!on); }
  void place() {
    m_expandedHeight = std::max(30, std::min(520, parentWidget()->height() - 64));
    setFixedWidth(std::max(1, std::min(352, parentWidget()->width() - 16)));
    move(8, 44);
    if (!m_animation.state()) { setMaximumHeight(m_expanded ? m_expandedHeight : 30); resize(width(), maximumHeight()); }
  }
  void reveal() { expand(true); show(); raise(); }
 private:
  void expand(bool on) {
    if (on == m_expanded) return;
    m_expanded = on;
    m_header->setStyleSheet(on ? "" : "color: palette(mid); background: rgba(100,110,120,35);");
    m_animation.stop(); m_animation.setStartValue(height()); m_animation.setEndValue(on ? m_expandedHeight : 30); m_animation.start();
  }
  QWidget* m_browser;
  QLabel* m_header;
  QPropertyAnimation m_animation;
  QTimer m_poll;
  bool m_auto = true, m_expanded = false;
  int m_expandedHeight = 520;
};
