#pragma once
#include <QApplication>
#include <QFrame>
#include <QPainter>
#include <QSettings>
#include <QTimer>
#include <QVariantAnimation>
#include <QCursor>
#include "Motion.hpp"
#include "Theme.hpp"

// Attached to the scene but composited by the OS, just like the measurement
// overlay: native OpenGL child windows cannot alpha-blend QWidget children.
// Collapsing physically shrinks the input window to the search/tree preview.
class BrowserOverlay : public QFrame {
 public:
  BrowserOverlay(QWidget* browser, QWidget* scene)
      : QFrame(scene->window(), Qt::Tool | Qt::FramelessWindowHint), m_browser(browser), m_scene(scene) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setObjectName("browserOverlay");
    browser->setParent(this);
    browser->setAutoFillBackground(false);
    browser->setStyleSheet("QWidget { background: transparent; } QTreeWidget { border: none; } QLineEdit { background: rgba(40,45,52,35); }");
    m_auto=QSettings().value("ui/browserAutoHide",true).toBool();
    m_expanded=!m_auto; m_progress=m_expanded?1.0:0.0;
    m_scene->installEventFilter(this);
    m_animation.setDuration(180); m_animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_animation,&QVariantAnimation::valueChanged,this,[this](const QVariant& value) {
      m_progress=value.toReal(); resize(width(),currentHeight()); update();
    });
    connect(&m_animation,&QVariantAnimation::finished,this,[this] { m_browser->setVisible(m_expanded); });
    m_poll.setInterval(100);
    connect(&m_poll,&QTimer::timeout,this,[this] {
      if (!isVisible()) return;
      place();
      if (QApplication::mouseButtons()!=Qt::NoButton) return;
      QWidget* focus=QApplication::focusWidget();
      const bool active=rect().contains(mapFromGlobal(QCursor::pos())) || (isActiveWindow() && focus && isAncestorOf(focus));
      expand(!m_auto || active || (isActiveWindow() && QApplication::activePopupWidget()));
    });
    m_poll.start(); place(); snapshot(); m_browser->setVisible(m_expanded);
  }
  void setVisible(bool on) override { m_requested=on; QFrame::setVisible(on && m_scene->isVisible()); }
  void setAutoHide(bool on) { m_auto=on; QSettings().setValue("ui/browserAutoHide",on); expand(!on); }
  void place() {
    const int h=std::max(60,std::min(520,m_scene->height()-64));
    const int w=std::max(1,std::min(352,m_scene->width()-16));
    const bool changed=h!=m_expandedHeight || w!=width();
    m_expandedHeight=h;
    resize(w,currentHeight()); move(m_scene->mapToGlobal(QPoint(8,44)));
    m_browser->setGeometry(0,0,w,h);
    if(changed && !m_expanded) snapshot();
  }
  void reveal() { setVisible(true); expand(true); raise(); }
  void refresh() { snapshot(); update(); }
  bool expanded() const { return m_expanded; }
 protected:
  bool eventFilter(QObject* object,QEvent* event) override {
    if(object==m_scene) {
      if(event->type()==QEvent::Hide) QFrame::hide();
      else if(event->type()==QEvent::Show && m_requested) { place(); QFrame::show(); }
      else if(event->type()==QEvent::Resize || event->type()==QEvent::Move) place();
    }
    return QFrame::eventFilter(object,event);
  }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    if(m_progress>0) {
      QColor bg=theme::current().bg2; bg.setAlphaF(0.94*m_progress);
      p.setPen(Qt::NoPen); p.setBrush(bg); p.drawRoundedRect(rect(),5,5);
    }
    if(m_browser->isVisible()) return;
    if(m_snapshot.isNull()) snapshot();
    QImage faded=m_snapshot.toImage();
    QPainter mask(&faded); mask.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    QLinearGradient gradient(0,0,0,std::min(160,m_expandedHeight));
    gradient.setColorAt(0,QColor::fromRgbF(1,1,1,0.65+0.35*m_progress));
    gradient.setColorAt(0.3,QColor::fromRgbF(1,1,1,0.55+0.45*m_progress));
    gradient.setColorAt(1,QColor::fromRgbF(1,1,1,m_progress));
    mask.fillRect(faded.rect(),gradient); mask.end(); p.drawImage(QPoint(0,0),faded);
  }
 private:
  int currentHeight() const { const int collapsed=std::min(160,m_expandedHeight); return qRound(collapsed+(m_expandedHeight-collapsed)*m_progress); }
  void snapshot() { if(m_browser->width()>0) m_snapshot=m_browser->grab(); }
  void expand(bool on) {
    if(on==m_expanded) return;
    snapshot(); m_browser->hide(); m_expanded=on;
    m_animation.stop(); m_animation.setDuration(motion::milliseconds(180)); m_animation.setStartValue(m_progress); m_animation.setEndValue(on?1.0:0.0); m_animation.start();
  }
  QWidget *m_browser, *m_scene;
  QPixmap m_snapshot;
  QVariantAnimation m_animation;
  QTimer m_poll;
  bool m_auto=true, m_expanded=false, m_requested=true;
  int m_expandedHeight=520;
  qreal m_progress=0;
};
