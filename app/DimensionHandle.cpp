#include "DimensionHandle.hpp"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <cmath>
#include <algorithm>

DimensionHandle::DimensionHandle(Viewport* view):QWidget(view),m_view(view) {
  setObjectName("dimensionHandle");setAttribute(Qt::WA_NativeWindow);setFixedSize(160,40);setMouseTracking(true);
  m_edit=new QLineEdit(this);m_edit->setGeometry(40,6,116,28);m_edit->setAlignment(Qt::AlignCenter);
  m_edit->setStyleSheet("QLineEdit { background: rgba(245,248,250,165); color: #17202a; border: 1px solid #8a9990; border-radius: 4px; } QLineEdit:hover, QLineEdit:focus { background: rgba(255,255,255,245); }");
  connect(m_edit,&QLineEdit::textEdited,this,&DimensionHandle::valueChanged);
  connect(view,&Viewport::notesMoved,this,&DimensionHandle::reposition);view->installEventFilter(this);hide();
}
void DimensionHandle::configure(const opad::Vec3& origin,const opad::Vec3& axis,double value,const QString& expression) {
  m_origin=origin;m_axis=axis;m_value=value;
  if(!m_edit->hasFocus()){QSignalBlocker block(m_edit);m_edit->setText(expression);}
  reposition();show();raise();
}
void DimensionHandle::reposition() {
  opad::Vec3 tip=m_origin,next=m_origin;
  const double step=std::max(1.0,m_view->pixelSize()*50);
  for(int i=0;i<3;++i){tip[i]+=m_axis[i]*m_value;next[i]+=m_axis[i]*step;}
  m_screenAxis=(QPointF(m_view->widgetPoint(next))-QPointF(m_view->widgetPoint(m_origin)))/step;
  if(QPointF::dotProduct(m_screenAxis,m_screenAxis)<1e-6)m_screenAxis={0,-1/std::max(.001,m_view->pixelSize())};
  const auto at=m_view->widgetPoint(tip)-QPoint(20,20);
  move(std::clamp(at.x(),0,std::max(0,m_view->width()-width())),std::clamp(at.y(),0,std::max(0,m_view->height()-height())));update();
}
void DimensionHandle::paintEvent(QPaintEvent*) {
  QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.setBrush(QColor(245,250,247,205));p.setPen(QPen(QColor(55,125,80),2));p.drawEllipse(QPointF(20,20),17,17);
  const double length=std::hypot(m_screenAxis.x(),m_screenAxis.y());const QPointF d=m_screenAxis*(11/length),n(-d.y()*.35,d.x()*.35),c(20,20);
  p.drawLine(c-d,c+d);p.drawLine(c+d,c+d*.45+n);p.drawLine(c+d,c+d*.45-n);p.drawLine(c-d,c-d*.45+n);p.drawLine(c-d,c-d*.45-n);
}
void DimensionHandle::mousePressEvent(QMouseEvent* e) {
  if(e->button()!=Qt::LeftButton)return;
  m_dragging=true;m_start=e->globalPosition();m_startValue=m_value;e->accept();
}
void DimensionHandle::mouseMoveEvent(QMouseEvent* e) {
  if(!m_dragging)return;
  const auto delta=e->globalPosition()-m_start;
  m_value=m_startValue+QPointF::dotProduct(delta,m_screenAxis)/QPointF::dotProduct(m_screenAxis,m_screenAxis);
  const auto expression=QString::number(m_value,'g',9)+" mm";m_edit->setText(expression);emit valueChanged(expression);e->accept();
}
void DimensionHandle::mouseReleaseEvent(QMouseEvent* e) {if(e->button()==Qt::LeftButton){m_dragging=false;reposition();e->accept();}}
bool DimensionHandle::eventFilter(QObject* target,QEvent* event) {
  if(target==m_view && isVisible() && event->type()==QEvent::KeyPress) {
    auto* key=static_cast<QKeyEvent*>(event);
    if(key->modifiers()==Qt::NoModifier && ((!key->text().isEmpty() && key->text().front().isDigit()) || key->key()==Qt::Key_Minus || key->key()==Qt::Key_Period)) {
      m_edit->setFocus();m_edit->setText(key->text());emit valueChanged(m_edit->text());return true;
    }
  }
  return QWidget::eventFilter(target,event);
}
