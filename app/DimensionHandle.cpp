#include "DimensionHandle.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "Units.hpp"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainterPath>
#include <QRegion>
#include <QSignalBlocker>
#include <QHideEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWheelEvent>
#include <Graphic3d_ArrayOfSegments.hxx>
#include <Graphic3d_ArrayOfTriangles.hxx>
#include <Graphic3d_AspectFillArea3d.hxx>
#include <Graphic3d_AspectLine3d.hxx>
#include <cmath>
#include <algorithm>

namespace {
Quantity_Color occ(const QColor& c) { return Quantity_Color(c.redF(), c.greenF(), c.blueF(), Quantity_TOC_sRGB); }

// Flat and facing the camera: drawn in the screen plane along the axis's projection, so it reads the same from any
// angle (the shaded 3D arrow it replaces shrank to a disc when the axis pointed at the viewer). Looking along the
// axis, a ring with a dot (towards the viewer) or with a cross (away) stands in for it. Selection colour, outlined.
class ScalarArrow : public AIS_InteractiveObject {
  DEFINE_STANDARD_RTTI_INLINE(ScalarArrow,AIS_InteractiveObject)
 public:
  gp_Pnt origin;gp_Dir axis;gp_Dir view{0,0,-1};double pixel=1;QColor fill,rim;
  static constexpr double kLength=46,kHead=17,kShaft=3,kWing=9;  // widget pixels
 protected:
  void Compute(const Handle(PrsMgr_PresentationManager)&,const Handle(Prs3d_Presentation)& prs,Standard_Integer) override {
    const gp_Vec a(axis),d(view);
    gp_Vec along=a-d*a.Dot(d);const double visible=along.Magnitude();
    Handle(Graphic3d_AspectFillArea3d) style=new Graphic3d_AspectFillArea3d;
    style->SetInteriorStyle(Aspect_IS_SOLID);style->SetInteriorColor(occ(fill));
    style->SetShadingModel(Graphic3d_TypeOfShadingModel_Unlit);style->SetSuppressBackFaces(false);
    Handle(Graphic3d_AspectLine3d) outline=new Graphic3d_AspectLine3d(occ(rim),Aspect_TOL_SOLID,1.5);
    auto fillGroup=prs->NewGroup();fillGroup->SetGroupPrimitivesAspect(style);
    auto lineGroup=prs->NewGroup();lineGroup->SetGroupPrimitivesAspect(outline);
    const double p=pixel;
    if(visible>0.25) {
      along/=visible;const gp_Vec side=d.Crossed(along).Normalized();
      auto at=[&](double u,double v){return origin.Translated(along*(u*p)+side*(v*p));};
      const gp_Pnt s0=at(0,-kShaft),s1=at(kLength-kHead,-kShaft),s2=at(kLength-kHead,kShaft),s3=at(0,kShaft);
      const gp_Pnt h0=at(kLength-kHead,-kWing),h1=at(kLength,0),h2=at(kLength-kHead,kWing);
      Handle(Graphic3d_ArrayOfTriangles) body=new Graphic3d_ArrayOfTriangles(9);
      for(const auto& q:{s0,s1,s2,s0,s2,s3,h0,h1,h2})body->AddVertex(q);
      fillGroup->AddPrimitiveArray(body);
      const std::vector<gp_Pnt> rimPoints={s0,s1,h0,h1,h2,s2,s3,s0};
      Handle(Graphic3d_ArrayOfSegments) lines=new Graphic3d_ArrayOfSegments(int(2*(rimPoints.size()-1)));
      for(size_t i=1;i<rimPoints.size();++i){lines->AddVertex(rimPoints[i-1]);lines->AddVertex(rimPoints[i]);}
      lineGroup->AddPrimitiveArray(lines);
      return;
    }
    // Along the view: a ring in the screen plane, a dot for "towards you", a cross for "away".
    const gp_Vec u=gp_Vec(d).Crossed(std::abs(d.Z())<0.9?gp_Vec(0,0,1):gp_Vec(1,0,0)).Normalized(),v=gp_Vec(d).Crossed(u);
    const int segments=32;const double r=kWing*p;
    Handle(Graphic3d_ArrayOfTriangles) disc=new Graphic3d_ArrayOfTriangles(segments*3);
    Handle(Graphic3d_ArrayOfSegments) ring=new Graphic3d_ArrayOfSegments(segments*2+4);
    for(int i=0;i<segments;++i){
      const double t0=2*M_PI*i/segments,t1=2*M_PI*(i+1)/segments;
      const gp_Pnt a0=origin.Translated(u*(r*std::cos(t0))+v*(r*std::sin(t0))),a1=origin.Translated(u*(r*std::cos(t1))+v*(r*std::sin(t1)));
      disc->AddVertex(origin);disc->AddVertex(a0);disc->AddVertex(a1);ring->AddVertex(a0);ring->AddVertex(a1);
    }
    const double k=r*0.45;
    if(a.Dot(d)>0){ring->AddVertex(origin.Translated((u+v)*k*0.7));ring->AddVertex(origin.Translated(-(u+v)*k*0.7));
      ring->AddVertex(origin.Translated((u-v)*k*0.7));ring->AddVertex(origin.Translated(-(u-v)*k*0.7));}
    else{ring->AddVertex(origin.Translated(u*k*0.25));ring->AddVertex(origin.Translated(-u*k*0.25));
      ring->AddVertex(origin.Translated(v*k*0.25));ring->AddVertex(origin.Translated(-v*k*0.25));}
    fillGroup->AddPrimitiveArray(disc);lineGroup->AddPrimitiveArray(ring);
  }
  void ComputeSelection(const Handle(SelectMgr_Selection)&,Standard_Integer) override {}
};
double nearest(const QPointF& p,const QPointF& a,const QPointF& b,double& t) {
  const auto ab=b-a;const double size=QPointF::dotProduct(ab,ab);
  t=size>1e-12?std::clamp(QPointF::dotProduct(p-a,ab)/size,0.,1.):0.;
  const auto d=p-a-ab*t;return QPointF::dotProduct(d,d);
}
// A round step for dragging at this zoom: about two pixels, 1, 2 or 5 times a power of ten.
double dragStep(double pixel) {
  const double raw=std::max(1e-6,pixel*2),power=std::pow(10.0,std::floor(std::log10(raw)));
  for(double m:{1.0,2.0,5.0,10.0})if(power*m>=raw)return power*m;
  return power*10;
}
// The same, round in the shown unit (UI-123): 0.1 in rather than 2.54 mm.
double shownStep(double pixel) {return units::fromDisplay(units::Kind::Length,dragStep(units::toDisplay(units::Kind::Length,pixel)));}
// The value in the shown unit to the step's decimals, as the expression the feature stores: "12.5 mm", "0.49 in".
QString lengthText(double value,double step) {
  return QString::number(units::toDisplay(units::Kind::Length,value),'f',std::min(6,units::decimalsFor(step)))+' '+QString::fromStdString(units::current().length);
}
bool plainNumber(const QString& text) {
  QString t=text.trimmed();const QString unit=QString::fromStdString(units::current().length);
  if(t.endsWith(unit))t.chop(unit.size());else if(t.endsWith("mm"))t.chop(2);
  bool ok=false;t.trimmed().toDouble(&ok);return ok;
}
}

DimensionHandle::DimensionHandle(Viewport* view,JobRunner* jobs):QWidget(view),m_jobs(jobs),m_view(view) {
  setObjectName("dimensionHandle");setAttribute(Qt::WA_NativeWindow);setAttribute(Qt::WA_StyledBackground);setMouseTracking(true);
  // Native child windows above OCCT must paint every pixel; transparent holes become black on Windows (the rounded
  // corners are cut off by a mask instead, see resizeEvent).
  setAutoFillBackground(true);setFixedHeight(30);
  auto* row=new QHBoxLayout(this);row->setContentsMargins(8,2,6,2);row->setSpacing(6);
  m_label=new QLabel(tr("Distance"),this);m_label->setObjectName("dimensionLabel");
  m_edit=new QLineEdit(this);m_edit->setObjectName("dimensionValue");m_edit->setFrame(false);m_edit->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);
  m_edit->setToolTip(tr("Type a value or an expression · Enter applies · Esc restores · ↑/↓ or the wheel step it (Shift ×10, Ctrl ×0.1)"));
  m_result=new QLabel(this);m_result->setObjectName("dimensionResult");m_result->hide();
  row->addWidget(m_label);row->addWidget(m_edit,1);row->addWidget(m_result);
  m_edit->installEventFilter(this);
  connect(m_edit,&QLineEdit::textEdited,this,[this](const QString& text){fit();emit valueChanged(text);});
  connect(m_edit,&QLineEdit::returnPressed,this,[this]{emit valueChanged(m_edit->text());emit accepted();});
  connect(view,&Viewport::notesMoved,this,[this]{reposition();indexAnchors();});qApp->installEventFilter(this);hide();
  connect(theme::notifier(),&theme::Notifier::changed,this,[this]{restyle();if(m_drawn){m_arrow->SetToUpdate();m_view->updateOverlay(m_arrow);}});
  m_arrow=new ScalarArrow;m_arrow->SetInfiniteState(true);
  restyle();
}
DimensionHandle::~DimensionHandle(){if(m_indexJob)m_indexJob->cancel();}
void DimensionHandle::restyle() {
  const Tokens& t=theme::current();const bool active=m_dragging||m_edit->hasFocus();
  auto palette=this->palette();palette.setColor(QPalette::Window,t.bg2);setPalette(palette);
  setStyleSheet(QString("#dimensionHandle { background: %1; border: 1px solid %2; border-radius: 5px; }"
                        "#dimensionLabel { color: %3; font-size: 11px; }"
                        "#dimensionResult { color: %4; font-size: 11px; }"
                        "#dimensionValue { background: transparent; color: %5; border: none; font-family: '%6'; font-size: 12px; selection-background-color: %7; }")
                    .arg(theme::css(t.bg2),theme::css(active?t.sel:t.line),theme::css(t.fg2),theme::css(t.fg3),theme::css(t.fg),theme::mono().family(),theme::css(t.selbg)));
  auto* arrow=static_cast<ScalarArrow*>(m_arrow.get());arrow->fill=t.sel;arrow->rim=t.dark?QColor("#0b0d10"):QColor("#ffffff");
}
void DimensionHandle::setLabel(const QString& label){m_label->setText(label);fit();}
void DimensionHandle::fit() {
  // Wide enough for what is typed; the evaluated value shows next to an expression.
  const bool expression=!plainNumber(m_edit->text()) && !m_edit->text().trimmed().isEmpty();
  if(expression)m_result->setText(QString("= %1").arg(lengthText(m_value,shownStep(std::max(1e-6,m_view->pixelSize())))));
  m_result->setVisible(expression);
  m_edit->setFixedWidth(std::clamp(m_edit->fontMetrics().horizontalAdvance(m_edit->text()+"  ")+6,56,220));
  adjustSize();
}
void DimensionHandle::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  QPainterPath path;path.addRoundedRect(QRectF(rect()),5,5);setMask(QRegion(path.toFillPolygon().toPolygon()));
}
void DimensionHandle::setAnchorSegments(std::vector<Segment> segments) {
  if(m_indexJob)m_indexJob->cancel();m_indexJob=nullptr;m_indexReady=false;m_segments=std::move(segments);
}
void DimensionHandle::indexAnchors() {
  if(m_indexJob)m_indexJob->cancel();m_indexJob=nullptr;m_indexReady=false;
  if(!isVisible() || m_segments.empty())return;
  m_cells.clear();m_screenSegments.resize(m_segments.size());auto index=std::make_shared<size_t>(0);
  const auto axis=m_axis;const double value=m_value*m_scale;
  m_indexJob=m_jobs->sliced(tr("Preparing drag handle"),[this,index,axis,value](Job&){
    const size_t i=(*index)++;auto a=m_segments[i][0],b=m_segments[i][1];for(int n=0;n<3;++n){a[n]+=axis[n]*value;b[n]+=axis[n]*value;}
    auto& screen=m_screenSegments[i];screen={m_view->widgetPoint(a),m_view->widgetPoint(b)};
    const int x0=std::max(0,int(std::floor((std::min(screen[0].x(),screen[1].x())-12)/64))),x1=std::min(m_view->width()/64,int(std::floor((std::max(screen[0].x(),screen[1].x())+12)/64)));
    const int y0=std::max(0,int(std::floor((std::min(screen[0].y(),screen[1].y())-12)/64))),y1=std::min(m_view->height()/64,int(std::floor((std::max(screen[0].y(),screen[1].y())+12)/64)));
    for(int x=x0;x<=x1;++x)for(int y=y0;y<=y1;++y)m_cells[(quint64(quint32(x))<<32)|quint32(y)].push_back(i);
    return *index<m_segments.size();
  },[this](bool ok){m_indexJob=nullptr;m_indexReady=ok;});
}
void DimensionHandle::setText(const QString& text,bool notify) {
  {QSignalBlocker block(m_edit);m_edit->setText(text);}
  fit();if(notify)emit valueChanged(text);
}
void DimensionHandle::configure(const opad::Vec3& origin,const opad::Vec3& axis,double value,const QString& expression) {
  if(!m_dragging){
    if(m_segments.empty())m_origin=origin;
    else if(!m_drawn || m_axis!=axis)for(int i=0;i<3;++i)m_origin[i]=(m_segments.front()[0][i]+m_segments.front()[1][i])*.5;
    m_axis=axis;m_value=value;
  }
  if(!m_edit->hasFocus())setText(expression,false);else fit();
  show();raise();reposition();indexAnchors();
}
void DimensionHandle::showEvent(QShowEvent*) {restyle();reposition();}
void DimensionHandle::hideEvent(QHideEvent*) {if(m_indexJob)m_indexJob->cancel();m_indexJob=nullptr;m_indexReady=false;m_dragging=false;m_drawn=false;m_edit->clearFocus();m_view->removeOverlay(m_arrow);}
void DimensionHandle::reposition() {
  if(!isVisible() || m_arrow.IsNull())return;
  opad::Vec3 tip=m_origin,next=m_origin;
  const double pixel=std::max(.000001,m_view->pixelSize()),step=pixel*50;
  for(int i=0;i<3;++i){tip[i]+=m_axis[i]*m_value*m_scale;next[i]+=m_axis[i]*step;}
  m_screenAxis=(QPointF(m_view->widgetPoint(next))-QPointF(m_view->widgetPoint(m_origin)))/step;
  if(QPointF::dotProduct(m_screenAxis,m_screenAxis)*pixel*pixel<0.0625)m_screenAxis={0,-1/pixel};  // along the view: pull up
  auto* arrow=static_cast<ScalarArrow*>(m_arrow.get());
  const auto view=m_view->viewDirection();const gp_Dir viewDir(view[0],view[1],view[2]);
  const bool changed=!m_drawn || arrow->origin.Distance(gp_Pnt(tip[0],tip[1],tip[2]))>1e-10 || std::abs(arrow->pixel-pixel)>1e-10
    || !arrow->axis.IsEqual(gp_Dir(m_axis[0],m_axis[1],m_axis[2]),1e-10) || !arrow->view.IsEqual(viewDir,1e-9);
  arrow->origin=gp_Pnt(tip[0],tip[1],tip[2]);arrow->view=viewDir;
  arrow->axis=gp_Dir(m_axis[0],m_axis[1],m_axis[2]);arrow->pixel=pixel;
  // Where it is on screen, for the press test and to place the box: tip to head (the tip alone when seen end-on).
  const gp_Vec along=gp_Vec(arrow->axis)-gp_Vec(viewDir)*gp_Vec(arrow->axis).Dot(gp_Vec(viewDir));
  const auto end=along.Magnitude()>0.25?arrow->origin.Translated(along.Normalized()*ScalarArrow::kLength*pixel):arrow->origin;
  m_arrowStart=m_view->widgetPoint(tip);m_arrowEnd=m_view->widgetPoint({end.X(),end.Y(),end.Z()});
  if(changed){arrow->SetToUpdate();if(!m_drawn)m_view->showOverlay(m_arrow);else m_view->updateOverlay(m_arrow);m_drawn=true;}
  const QPointF out=m_arrowEnd-m_arrowStart;const double len=std::hypot(out.x(),out.y());
  const QPointF gap=len>1?out/len*16:QPointF(16,0);
  QPoint at=(m_arrowEnd+gap).toPoint();if(gap.x()<0)at.rx()-=width();at.ry()-=height()/2;
  move(std::clamp(at.x(),0,std::max(0,m_view->width()-width())),std::clamp(at.y(),0,std::max(0,m_view->height()-height())));
}
bool DimensionHandle::overArrow(const QPointF& p) const {
  double t;
  return isVisible() && nearest(p,m_arrowStart,m_arrowEnd,t)<kHitRadius*kHitRadius;
}
void DimensionHandle::mousePressEvent(QMouseEvent* e) {
  if(e->button()!=Qt::LeftButton)return;
  m_dragging=true;m_start=e->globalPosition();m_startValue=m_value;m_edit->clearFocus();restyle();e->accept();
}
void DimensionHandle::mouseMoveEvent(QMouseEvent* e) {
  if(!m_dragging)return;
  const auto delta=e->globalPosition()-m_start;
  const double raw=m_startValue+QPointF::dotProduct(delta,m_screenAxis)/QPointF::dotProduct(m_screenAxis,m_screenAxis)/std::max(1e-9,m_scale);
  const double step=shownStep(std::max(1e-6,m_view->pixelSize()));
  m_value=std::round(raw/step)*step;  // round values at this zoom, not 12.3456789 mm
  setText(lengthText(m_value,step),false);reposition();emit valueChanged(m_edit->text());e->accept();
}
void DimensionHandle::mouseReleaseEvent(QMouseEvent* e) {if(e->button()==Qt::LeftButton){m_dragging=false;restyle();reposition();e->accept();}}
void DimensionHandle::wheelEvent(QWheelEvent* e) {nudge(e->angleDelta().y()>0?1:-1,e->modifiers());e->accept();}
void DimensionHandle::nudge(double steps,Qt::KeyboardModifiers modifiers) {
  const double step=units::fromDisplay(units::Kind::Length,modifiers.testFlag(Qt::ShiftModifier)?10:modifiers.testFlag(Qt::ControlModifier)?0.1:1);  // in the shown unit
  m_value=std::round((m_value+steps*step)/step)*step;
  setText(lengthText(m_value,std::min(step,units::fromDisplay(units::Kind::Length,1))),true);reposition();
}
bool DimensionHandle::eventFilter(QObject* target,QEvent* event) {
  if(!isVisible())return false;
  if(target==m_edit) {
    if(event->type()==QEvent::FocusIn){
      m_before=m_edit->text();restyle();
      // Clicked or tabbed into: select all, so typing replaces the value. (A digit typed over the view focuses the box
      // with that digit already in it; selecting it would lose it to the next key.)
      const auto reason=static_cast<QFocusEvent*>(event)->reason();
      if(reason==Qt::MouseFocusReason || reason==Qt::TabFocusReason || reason==Qt::BacktabFocusReason)QTimer::singleShot(0,m_edit,&QLineEdit::selectAll);
    }
    else if(event->type()==QEvent::FocusOut)restyle();
    else if(event->type()==QEvent::KeyPress || event->type()==QEvent::ShortcutOverride) {
      auto* key=static_cast<QKeyEvent*>(event);
      if(key->key()==Qt::Key_Escape){key->accept();if(event->type()==QEvent::KeyPress){if(m_edit->text()!=m_before)setText(m_before,true);m_edit->clearFocus();}return true;}
      if(key->key()==Qt::Key_Up || key->key()==Qt::Key_Down){key->accept();if(event->type()==QEvent::KeyPress)nudge(key->key()==Qt::Key_Up?1:-1,key->modifiers());return true;}
    }
    return false;
  }
  if(target==m_view && (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseMove || event->type()==QEvent::MouseButtonRelease)) {
    auto* mouse=static_cast<QMouseEvent*>(event);
    if(event->type()==QEvent::MouseButtonPress && mouse->button()==Qt::LeftButton && overArrow(mouse->position())){mousePressEvent(mouse);return true;}
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
      key->accept();if(event->type()==QEvent::KeyPress){m_edit->setFocus();setText(key->text(),true);m_edit->deselect();m_edit->setCursorPosition(m_edit->text().size());}return true;
    }
  }
  return QWidget::eventFilter(target,event);
}
