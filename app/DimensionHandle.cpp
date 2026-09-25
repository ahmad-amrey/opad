#include "DimensionHandle.hpp"
#include "Jobs.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QHideEvent>
#include <QShowEvent>
#include <Prs3d_Arrow.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <gp_Ax1.hxx>
#include <cmath>
#include <algorithm>

namespace {
class ScalarArrow : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(ScalarArrow,AIS_InteractiveObject)
 public:
  gp_Pnt origin;gp_Dir axis;double pixel=1;
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&,const Handle(Prs3d_Presentation)& prs,Standard_Integer) override {
    auto group=prs->NewGroup();Handle(Graphic3d_AspectFillArea3d) style=new Graphic3d_AspectFillArea3d;
    style->SetInteriorStyle(Aspect_IS_SOLID);style->SetInteriorColor(Quantity_Color(.25,.72,.43,Quantity_TOC_sRGB));
    style->SetShadingModel(Graphic3d_TypeOfShadingModel_Phong);style->SetSuppressBackFaces(false);
    group->SetGroupPrimitivesAspect(style);
    group->AddPrimitiveArray(Prs3d_Arrow::DrawShaded(gp_Ax1(origin,axis),2.4*pixel,48*pixel,7*pixel,15*pixel,24));
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&,Standard_Integer) override {}
};
double nearest(const QPointF& p,const QPointF& a,const QPointF& b,double& t) {
  const auto ab=b-a;const double size=QPointF::dotProduct(ab,ab);
  t=size>1e-12?std::clamp(QPointF::dotProduct(p-a,ab)/size,0.,1.):0.;
  const auto d=p-a-ab*t;return QPointF::dotProduct(d,d);
}
}
DimensionHandle::DimensionHandle(Viewport* view,JobRunner* jobs):QWidget(view),m_jobs(jobs),m_view(view) {
  setObjectName("dimensionHandle");setAttribute(Qt::WA_NativeWindow);setFixedSize(124,32);setMouseTracking(true);
  // Native child windows above OCCT must paint every pixel; transparent holes become black on Windows.
  setAutoFillBackground(true);auto palette=this->palette();palette.setColor(QPalette::Window,QColor(231,240,234));setPalette(palette);
  m_edit=new QLineEdit(this);m_edit->setGeometry(2,2,120,28);m_edit->setAlignment(Qt::AlignCenter);
  m_edit->setStyleSheet("QLineEdit { background: #e7f0ea; color: #17202a; border: 1px solid #8a9990; border-radius: 3px; } QLineEdit:hover, QLineEdit:focus { background: white; }");
  connect(m_edit,&QLineEdit::textEdited,this,&DimensionHandle::valueChanged);
  connect(view,&Viewport::notesMoved,this,[this]{reposition();indexAnchors();});qApp->installEventFilter(this);hide();
  m_arrow=new ScalarArrow;m_arrow->SetInfiniteState(true);
}
DimensionHandle::~DimensionHandle(){if(m_indexJob)m_indexJob->cancel();}
void DimensionHandle::setAnchorSegments(std::vector<Segment> segments) {
  if(m_indexJob)m_indexJob->cancel();m_indexJob=nullptr;m_indexReady=false;m_segments=std::move(segments);
}
void DimensionHandle::indexAnchors() {
  if(m_indexJob)m_indexJob->cancel();m_indexJob=nullptr;m_indexReady=false;
  if(!isVisible() || m_segments.empty())return;
  m_cells.clear();m_screenSegments.resize(m_segments.size());auto index=std::make_shared<size_t>(0);
  const auto axis=m_axis;const double value=m_value;
  m_indexJob=m_jobs->sliced(tr("Preparing drag handle"),[this,index,axis,value](Job&){
    const size_t i=(*index)++;auto a=m_segments[i][0],b=m_segments[i][1];for(int n=0;n<3;++n){a[n]+=axis[n]*value;b[n]+=axis[n]*value;}
    auto& screen=m_screenSegments[i];screen={m_view->widgetPoint(a),m_view->widgetPoint(b)};
    const int x0=std::max(0,int(std::floor((std::min(screen[0].x(),screen[1].x())-12)/64))),x1=std::min(m_view->width()/64,int(std::floor((std::max(screen[0].x(),screen[1].x())+12)/64)));
    const int y0=std::max(0,int(std::floor((std::min(screen[0].y(),screen[1].y())-12)/64))),y1=std::min(m_view->height()/64,int(std::floor((std::max(screen[0].y(),screen[1].y())+12)/64)));
    for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y)m_cells[(quint64(quint32(x))<<32)|quint32(y)].push_back(i);
    return *index<m_segments.size();
  },[this](bool ok){m_indexJob=nullptr;m_indexReady=ok;});
}
void DimensionHandle::configure(const opad::Vec3& origin,const opad::Vec3& axis,double value,const QString& expression) {
  if(!m_dragging){
    if(m_segments.empty())m_origin=origin;
    else if(!m_drawn || m_axis!=axis)for(int i=0;i<3;++i)m_origin[i]=(m_segments.front()[0][i]+m_segments.front()[1][i])*.5;
    m_axis=axis;m_value=value;
  }
  if(!m_edit->hasFocus()){QSignalBlocker block(m_edit);m_edit->setText(expression);}
  show();raise();reposition();indexAnchors();
}
void DimensionHandle::showEvent(QShowEvent*) {reposition();}
void DimensionHandle::hideEvent(QHideEvent*) {if(m_indexJob)m_indexJob->cancel();m_indexJob=nullptr;m_indexReady=false;m_dragging=false;m_drawn=false;m_edit->clearFocus();m_view->removeOverlay(m_arrow);}
void DimensionHandle::reposition() {
  if(!isVisible() || m_arrow.IsNull())return;
  opad::Vec3 tip=m_origin,next=m_origin;
  const double pixel=std::max(.000001,m_view->pixelSize()),step=pixel*50;
  for(int i=0;i<3;++i){tip[i]+=m_axis[i]*m_value;next[i]+=m_axis[i]*step;}
  m_screenAxis=(QPointF(m_view->widgetPoint(next))-QPointF(m_view->widgetPoint(m_origin)))/step;
  if(QPointF::dotProduct(m_screenAxis,m_screenAxis)<1e-6)m_screenAxis={0,-1/pixel};
  auto* arrow=static_cast<ScalarArrow*>(m_arrow.get());
  const bool changed=!m_drawn || arrow->origin.Distance(gp_Pnt(tip[0],tip[1],tip[2]))>1e-10 || std::abs(arrow->pixel-pixel)>1e-10 || !arrow->axis.IsEqual(gp_Dir(m_axis[0],m_axis[1],m_axis[2]),1e-10);
  arrow->origin=gp_Pnt(tip[0],tip[1],tip[2]);
  arrow->axis=gp_Dir(m_axis[0],m_axis[1],m_axis[2]);arrow->pixel=pixel;
  const auto end=arrow->origin.Translated(gp_Vec(arrow->axis)*48*pixel);
  m_arrowStart=m_view->widgetPoint(tip);m_arrowEnd=m_view->widgetPoint({end.X(),end.Y(),end.Z()});
  if(changed){arrow->SetToUpdate();if(!m_drawn)m_view->showOverlay(m_arrow);else m_view->updateOverlay(m_arrow);m_drawn=true;}
  const auto at=m_arrowEnd.toPoint()+QPoint(14,-height()/2);
  move(std::clamp(at.x(),0,std::max(0,m_view->width()-width())),std::clamp(at.y(),0,std::max(0,m_view->height()-height())));
}
void DimensionHandle::mousePressEvent(QMouseEvent* e) {
  if(e->button()!=Qt::LeftButton)return;
  m_dragging=true;m_start=e->globalPosition();m_startValue=m_value;m_edit->clearFocus();e->accept();
}
void DimensionHandle::mouseMoveEvent(QMouseEvent* e) {
  if(!m_dragging)return;
  const auto delta=e->globalPosition()-m_start;
  m_value=m_startValue+QPointF::dotProduct(delta,m_screenAxis)/QPointF::dotProduct(m_screenAxis,m_screenAxis);
  const auto expression=QString::number(m_value,'g',9)+" mm";m_edit->setText(expression);reposition();emit valueChanged(expression);e->accept();
}
void DimensionHandle::mouseReleaseEvent(QMouseEvent* e) {if(e->button()==Qt::LeftButton){m_dragging=false;reposition();e->accept();}}
bool DimensionHandle::eventFilter(QObject* target,QEvent* event) {
  if(!isVisible())return false;
  if(target==m_view && (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseMove || event->type()==QEvent::MouseButtonRelease)) {
    auto* mouse=static_cast<QMouseEvent*>(event);double t;
    if(event->type()==QEvent::MouseButtonPress && mouse->button()==Qt::LeftButton && nearest(mouse->position(),m_arrowStart,m_arrowEnd,t)<100){mousePressEvent(mouse);return true;}
    if(m_dragging){if(event->type()==QEvent::MouseMove)mouseMoveEvent(mouse);else if(event->type()==QEvent::MouseButtonRelease)mouseReleaseEvent(mouse);return true;}
    if(event->type()==QEvent::MouseMove && mouse->buttons()==Qt::NoButton && !m_edit->hasFocus() && m_indexReady) {
      double best=144;opad::Vec3 closest;bool found=false;
      const auto key=(quint64(quint32(int(mouse->position().x())/64))<<32)|quint32(int(mouse->position().y())/64);
      for(size_t index:m_cells.value(key)){const auto& segment=m_segments[index];const auto& screen=m_screenSegments[index];
        double part;const double distance=nearest(mouse->position(),screen[0],screen[1],part);
        if(distance<best){best=distance;found=true;for(int i=0;i<3;++i)closest[i]=segment[0][i]+part*(segment[1][i]-segment[0][i]);}
      }
      if(found){m_origin=closest;reposition();}
    }
  }
  if(event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress) {
    auto* widget=qobject_cast<QWidget*>(target);if(!widget || (widget!=m_view && !m_view->window()->isAncestorOf(widget)))return false;
    if(qobject_cast<QLineEdit*>(widget))return false;
    auto* key=static_cast<QKeyEvent*>(event);
    if(key->modifiers()==Qt::NoModifier && ((!key->text().isEmpty() && key->text().front().isDigit()) || key->key()==Qt::Key_Minus || key->key()==Qt::Key_Period)) {
      key->accept();if(event->type()==QEvent::KeyPress){m_edit->setFocus();m_edit->setText(key->text());emit valueChanged(m_edit->text());}return true;
    }
  }
  return QWidget::eventFilter(target,event);
}
