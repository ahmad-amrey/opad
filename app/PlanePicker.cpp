#include "PlanePicker.hpp"
#include "GuidedTool.hpp"
#include "ToolValues.hpp"
#include "Units.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch_geom.hpp"
#include "I18n.hpp"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Compound.hxx>
#include <Prs3d_PointAspect.hxx>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QSignalBlocker>
#include <algorithm>
#include <cmath>

using namespace opad::design;

class PlaneTiles : public QWidget {
 public:
  PlaneTiles(Viewport* view):QWidget(view),m_view(view) {
    setObjectName("planeTiles");setAttribute(Qt::WA_NativeWindow);setMouseTracking(true);setFixedSize(234,92);
    auto* timer=new QTimer(this);timer->setInterval(33);
    connect(timer,&QTimer::timeout,this,[this]{if(isVisible()){move(std::max(8,m_view->width()-450),42);raise();update();}});timer->start();hide();
  }
  std::function<void(int)> hovered,chosen;
  int selected=-1;
 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);p.setRenderHint(QPainter::Antialiasing);
    const auto camera=m_view->cameraPlane();
    const gp_Vec right(camera.x[0],camera.x[1],camera.x[2]),vertical(camera.y[0],camera.y[1],camera.y[2]);
    const QColor colors[]={QColor(75,150,235),QColor(80,190,130),QColor(235,170,70)};
    for(int i=0;i<3;++i) {
      const QRectF tile(i*78+2,2,74,88);const auto& t=theme::current();
      p.setBrush(t.bg2);p.setPen(QPen(i==m_hover||i==selected?colors[i]:t.line,i==m_hover||i==selected?2:1));p.drawRoundedRect(tile,5,5);
      const auto frame=base_frame(i==0?"xy":i==1?"xz":"yz");const QPointF center(i*78+39,37);
      auto project=[&](double u,double v){auto w=frame.to_world(u,v);gp_Vec q(w[0],w[1],w[2]);return center+QPointF(q.Dot(right)*23,-q.Dot(vertical)*23);};
      QPolygonF shape;for(auto [u,v]:std::vector<std::pair<double,double>>{{-1,-1},{1,-1},{1,1},{-1,1}})shape<<project(u,v);
      auto fill=colors[i];fill.setAlpha(65);p.setBrush(fill);p.setPen(QPen(colors[i],2));p.drawPolygon(shape);
      p.setPen(t.fg);p.drawText(QRectF(i*78,66,78,20),Qt::AlignCenter,i==0?"XY":i==1?"XZ":"YZ");
    }
  }
  void mouseMoveEvent(QMouseEvent* e) override {const int i=std::clamp(int(e->position().x())/78,0,2);if(i!=m_hover){m_hover=i;if(hovered)hovered(i);update();}}
  void leaveEvent(QEvent*) override {m_hover=-1;if(hovered)hovered(-1);update();}
  void mousePressEvent(QMouseEvent* e) override {if(e->button()==Qt::LeftButton){selected=std::clamp(int(e->position().x())/78,0,2);if(chosen)chosen(selected);}}
 private:Viewport* m_view;int m_hover=-1;
};

PlanePicker::PlanePicker(AppDocument* doc,Viewport* view,JobRunner* jobs,QWidget* window):QObject(window),m_doc(doc),m_view(view),m_jobs(jobs),m_snap(view) {
  auto* body=new QWidget;auto* layout=new QVBoxLayout(body);
  m_steps=new ToolStepsPanel(body);m_steps->setSummary({},{},{});m_steps->setFixedHeight(125);layout->addWidget(m_steps);
  auto* hint=new QLabel(tr("Click an origin plane (in the view or the corner tiles), a planar face or a sketch."),body);hint->setObjectName("planeHint");hint->setWordWrap(true);layout->addWidget(hint);
  m_construction=new QCheckBox(tr("Show construction planes"),body);m_construction->setObjectName("constructionPlanes");layout->addWidget(m_construction);
  connect(m_construction,&QCheckBox::toggled,this,[this]{constructionPlanes();});
  m_originControls=new QWidget(body);auto* form=new QFormLayout(m_originControls);form->setContentsMargins(0,0,0,0);
  m_u=new QLineEdit(body);m_v=new QLineEdit(body);form->addRow(tr("Plane X"),m_u);form->addRow(tr("Plane Y"),m_v);  // in the shown unit
  auto* explanation=new QLabel(tr("Coordinates use the selected plane's original axes and origin. Off-plane references are projected onto it."),body);explanation->setWordWrap(true);form->addRow(explanation);
  auto* reset=new QPushButton(tr("Reset origin"),body);form->addRow(reset);layout->addWidget(m_originControls);
  connect(reset,&QPushButton::clicked,this,[this]{++m_serial;if(m_job)m_job->cancel();m_job=nullptr;m_origin={{"world",{0,0,0}}};m_frame=m_supportFrame;double u,v;m_supportFrame.to_local({0,0,0},u,v);m_frame.origin=m_supportFrame.to_world(u,v);refresh();});
  // A box left as shown keeps its value: its text is rounded to the shown unit.
  auto numeric=[this]{if(m_refreshing||(!m_u->isModified()&&!m_v->isModified()))return;try{std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});ParamTable params(defs,units::current().length);double u,v;m_supportFrame.to_local(m_frame.origin,u,v);if(m_u->isModified())u=params.length(m_u->text().toStdString());if(m_v->isModified())v=params.length(m_v->text().toStdString());m_u->setModified(false);m_v->setModified(false);setOrigin(u,v);}catch(const std::exception& e){m_status->setText(i18n::t(QString::fromUtf8(e.what())));m_apply->setEnabled(false);}};
  connect(m_u,&QLineEdit::editingFinished,this,numeric);connect(m_v,&QLineEdit::editingFinished,this,numeric);
  // Enter in Plane X or Plane Y is OK too, with what was typed (as a feature panel's boxes): the value first, then the plane.
  for(auto* box:{m_u,m_v})connect(box,&QLineEdit::returnPressed,this,[this,numeric]{numeric();if(m_apply->isEnabled())apply();});
  m_status=new QLabel(body);m_status->setWordWrap(true);layout->addWidget(m_status);layout->addStretch();auto* footer=new QHBoxLayout;layout->addLayout(footer);
  m_back=new QPushButton(tr("Back"),body);m_apply=new QPushButton(tr("OK"),body);m_apply->setObjectName("primary");auto* cancelButton=new QPushButton(tr("Cancel"),body);m_back->setToolTip(tr("Return to plane selection"));m_apply->setToolTip(tr("Use this plane and close"));cancelButton->setToolTip(tr("Cancel without applying changes"));footer->addWidget(m_back);footer->addWidget(m_apply);footer->addWidget(cancelButton);
  connect(m_back,&QPushButton::clicked,this,&PlanePicker::back);connect(m_apply,&QPushButton::clicked,this,&PlanePicker::apply);connect(cancelButton,&QPushButton::clicked,this,&PlanePicker::cancel);
  m_panel=new ToolPanel("sketch-plane","plane",&Tokens::sel,tr("Choose sketch plane"),body,470,window);
  m_panel->setEscapeHandler([this]{cancel();});connect(m_panel,&ToolPanel::visibilityChanged,this,[this](bool on){if(!on&&m_active)cancel();});
  m_tiles=new PlaneTiles(view);m_tiles->chosen=[this](int i){choose({{"base",i==0?"xy":i==1?"xz":"yz"}});};
  m_tiles->hovered=[this](int i){if(!m_active||m_originStage)return;if(i<0)preview(nullptr);else {const auto f=base_frame(i==0?"xy":i==1?"xz":"yz");preview(&f);}};
  // UI-122: the origin's X and Y typed over the view or the panel (never the filters' digits), the marker following; Enter
  // uses the plane, Esc puts the origin back, then cancels.
  m_values=new ToolValues(view,this);
  m_values->fields=[this]{
    if(!m_active||!m_originStage)return QList<DynamicInput::Field>{};
    double u,v;m_supportFrame.to_local(m_frame.origin,u,v);
    return QList<DynamicInput::Field>{ToolValues::box("x",tr("Plane X"),units::editable(units::Kind::Length,u)),ToolValues::box("y",tr("Plane Y"),units::editable(units::Kind::Length,v))};
  };
  m_values->edited=[this](const QString& key,const QString& value){  // or the origin before, put back by Esc
    const auto mm=units::parse(units::Kind::Length,value);
    m_values->input()->setProblem(key,mm?QString():tr("Not a length"));
    if(!mm)return;
    double u,v;m_supportFrame.to_local(m_frame.origin,u,v);setOrigin(key=="x"?*mm:u,key=="y"?*mm:v);
  };
  m_values->commit=[this]{apply();};
  m_values->escape=[this]{cancel();};
  view->installEventFilter(this);
}

void PlanePicker::start(bool positionOrigin,std::function<void(ToolPanel*)> open,bool adoptSelection) {
  // A feature's plane input never takes the selection: that is the feature's own picks (one draft face became its neutral
  // plane, TODO 11 P3).
  const auto selected=adoptSelection?m_view->selection():std::vector<opad::Ref>{};
  stop();m_cameraBefore=m_view->cameraJson();m_active=true;m_positionOrigin=positionOrigin;m_originStage=false;m_oldFilter=m_view->selectionFilter();m_tiles->selected=-1;m_status->clear();
  // Construction planes are offered when there are any: they are made to be sketched on, and hidden behind an unticked
  // box the one just made could not be found.
  {const QSignalBlocker block(m_construction);m_construction->setChecked(std::any_of(m_doc->scene.features.begin(),m_doc->scene.features.end(),[](const opad::Feature& f){return !f.suppressed && f.result.contains("plane");}));}
  m_view->clearSelection();m_view->clearCandidates();m_view->setSelectionFilter(Viewport::SelFilter::Face);m_tiles->move(std::max(8,m_view->width()-450),42);m_tiles->show();constructionPlanes();refresh();open(m_panel);
  if(selected.size()==1 && selected.front().kind==opad::Ref::Kind::Face) {
    m_positionOrigin=false;choose({{"face",selected.front().to_json()}});
  } else if(m_view->twoDimensional()) {
    const auto frame=m_view->cameraPlane();choose({{"frame",frame.to_json()}});
  }
}
void PlanePicker::stop(bool restoreCamera) {
  const bool wasActive=m_active;m_active=false;m_originStage=false;m_drag=false;m_mouseDown=false;++m_serial;++m_candidateSerial;m_values->reset();
  if(m_job)m_job->cancel();m_job=nullptr;preview(nullptr);m_tiles->hide();m_panel->hide();
  if(wasActive){if(restoreCamera)m_view->setCameraJson(m_cameraBefore);m_view->clearCandidates();m_view->clearSelection();m_view->setSelectionFilter(m_oldFilter);}
}
void PlanePicker::cancel(){if(!m_active)return;stop();emit cancelled();}
void PlanePicker::choose(const opad::json& support) {
  if(!m_active)return;const int serial=++m_serial;if(m_job)m_job->cancel();
  auto doc=std::make_shared<opad::Document>(m_doc->doc);auto scene=std::make_shared<opad::Scene>(m_doc->scene);auto plane=std::make_shared<opad::json>(support);auto frame=std::make_shared<opad::Frame>();QPointer<PlanePicker> guard(this);
  m_status->setText(tr("Resolving sketch plane"));m_apply->setEnabled(false);
  m_job=m_jobs->async(tr("Resolving sketch plane"),[doc,scene,plane,frame](Progress p){if(p.cancelled())return;*frame=resolve_plane(*doc,*scene,*plane);if(plane->contains("face"))(*plane)["face"]=make_ref(*doc,*scene,opad::Ref::from_json(plane->at("face")));},[this,guard,serial,plane,frame](bool ok,const QString& error){
    if(!guard||!m_active||serial!=m_serial)return;m_job=nullptr;if(!ok){m_status->setText(i18n::t(error));return;}m_status->clear();m_support=*plane;m_supportFrame=*frame;m_frame=*frame;
    if(!m_positionOrigin){const auto value=*plane;const auto f=*frame;stop(false);emit accepted(value,f);return;}
    m_view->lookAt(*frame,true,false);m_originStage=true;m_origin={{"world",{0,0,0}}};double u,v;frame->to_local({0,0,0},u,v);m_frame.origin=frame->to_world(u,v);
    ++m_candidateSerial;m_view->clearCandidates();m_tiles->hide();m_view->clearSelection();m_view->setSelectionFilter(Viewport::SelFilter::Vertex);refresh();
    if(m_view->objectSnap())m_view->snapIndexesReady();  // the sketches' and drawings' snaps for the origin, indexed on a worker
  });
}
// What a plane candidate is called: the base planes and construction planes by name, a sketch by its own.
QString PlanePicker::candidateName(const opad::json& support) const {
  if(support.contains("base")){const std::string b=support.value("base",std::string());return b=="xz"?tr("XZ plane"):b=="yz"?tr("YZ plane"):tr("XY plane");}
  if(support.contains("sketch"))if(const auto* s=m_doc->scene.sketch(support.value("sketch",std::string())))return QString::fromStdString(s->name);
  if(support.contains("feature"))if(const auto* f=m_doc->scene.feature(support.value("feature",std::string())))return QString::fromStdString(f->name);
  return {};
}
void PlanePicker::selectionChanged() {
  if(!m_active||m_originStage)return;const auto candidates=m_view->selectedCandidates();const auto refs=m_view->selection();
  if(!candidates.empty())choose(opad::json::parse(candidates.back()));else if(!refs.empty()&&refs.back().kind==opad::Ref::Kind::Face)choose({{"face",refs.back().to_json()}});
}
void PlanePicker::back(){if(!m_active)return;++m_serial;if(m_job)m_job->cancel();m_job=nullptr;m_originStage=false;m_view->setCameraJson(m_cameraBefore);m_view->setSelectionFilter(Viewport::SelFilter::Face);preview(nullptr);m_tiles->show();constructionPlanes();refresh();}
void PlanePicker::setOrigin(double u,double v){if(!m_originStage||!std::isfinite(u)||!std::isfinite(v))return;++m_serial;if(m_job)m_job->cancel();m_job=nullptr;m_origin={{"uv",{u,v}}};m_frame=m_supportFrame;m_frame.origin=m_supportFrame.to_world(u,v);m_status->clear();refresh();}
void PlanePicker::apply(){if(!m_active||!m_originStage||m_job)return;const opad::json plane={{"support",m_support},{"origin",m_origin},{"frame",m_frame.to_json()}};const auto frame=m_frame;stop(false);emit accepted(plane,frame);}
// What the plane step took (UI-25: never "Ready"): a base plane, a body's face, a sketch's or a construction plane's name.
QString PlanePicker::supportName() const {
  if(m_support.contains("base"))return candidateName(m_support);
  if(m_support.contains("face")) {
    std::string body;try{body=opad::Ref::from_json(m_support.at("face")).body;}catch(...){}
    const opad::Node* n=m_doc->scene.node(body);return n?tr("Face of %1").arg(QString::fromStdString(n->name)):tr("A face");
  }
  if(m_support.contains("sketch"))if(const auto* s=m_doc->scene.sketch(m_support.value("sketch",std::string())))return QString::fromStdString(s->name);
  if(m_support.contains("feature"))if(const auto* f=m_doc->scene.feature(m_support.value("feature",std::string())))return QString::fromStdString(f->name);
  return tr("The view's plane");
}
QList<ToolStep> PlanePicker::steps() const {
  QList<ToolStep> steps;steps.push_back({tr("Choose plane"),m_originStage?supportName():QString()});
  if(m_positionOrigin)steps.push_back({tr("Position sketch origin"),{}});
  return steps;
}
void PlanePicker::refresh(){
  m_refreshing=true;
  m_steps->setSteps(steps(),{});
  m_panel->findChild<QLabel*>("planeHint")->setVisible(!m_originStage);m_originControls->setVisible(m_originStage);m_construction->setVisible(!m_originStage);m_apply->setVisible(m_originStage);m_apply->setEnabled(!m_job);m_back->setVisible(m_originStage);
  if(m_originStage){double u,v;m_supportFrame.to_local(m_frame.origin,u,v);m_u->setText(units::editable(units::Kind::Length,u));m_v->setText(units::editable(units::Kind::Length,v));preview(&m_frame);}m_refreshing=false;
}
void PlanePicker::constructionPlanes(){
  const int serial=++m_candidateSerial;if(!m_active||m_originStage)return;
  // TODO 11 P3: the origin planes are in the view too, not only in the corner tiles: faint squares about the origin, shown at
  // once (three faces); construction planes (drawn more solid) and sketches follow from a worker.
  const double size=std::max(10.0,m_view->pixelSize()*70);
  auto candidates=std::make_shared<std::vector<Viewport::Candidate>>(originPlanes(size));m_view->showCandidates(*candidates);
  auto frames=std::make_shared<std::vector<std::pair<std::string,opad::Frame>>>();if(m_construction->isChecked())for(const auto& f:m_doc->scene.features)if(f.result.contains("plane"))frames->push_back({f.id,opad::Frame::from_json(f.result.at("plane"))});
  auto sketches=std::make_shared<std::vector<opad::SketchItem>>(m_doc->scene.sketches);
  QPointer<PlanePicker> guard(this);
  m_jobs->async(tr("Preparing construction planes"),[frames,sketches,candidates,size](Progress p){for(const auto& [id,frame]:*frames){if(p.cancelled())return;candidates->push_back({(id.starts_with("sketch:")?opad::json{{"sketch",id.substr(7)}}:opad::json{{"feature",id}}).dump(),BRepBuilderAPI_MakeFace(frame_plane(frame),-size,size,-size,size).Face(),true});}
    for(const auto& sketch:*sketches)if(sketch.visible){
      if(p.cancelled())return;BRep_Builder builder;TopoDS_Compound shape;builder.MakeCompound(shape);
      const auto geometry=Sketch::from_json(sketch.geometry);
      for(const auto& entity:geometry.entities){if(p.cancelled())return;const auto edge=entity_edge(geometry,entity,sketch.frame);if(!edge.IsNull())builder.Add(shape,edge);}
      Bnd_Box box;BRepBndLib::Add(shape,box);candidates->push_back({opad::json{{"sketch",sketch.id}}.dump(),shape,false,BodyPrs::build(shape,box)});
    }
  },[this,guard,candidates,serial](bool ok,const QString&){if(guard&&ok&&m_active&&!m_originStage&&serial==m_candidateSerial)m_view->showCandidates(*candidates);});
}
std::vector<Viewport::Candidate> PlanePicker::originPlanes(double halfSize){
  halfSize=std::max(10.0,halfSize);std::vector<Viewport::Candidate> out;
  for(const char* base:{"xy","xz","yz"})out.push_back({opad::json{{"base",base}}.dump(),BRepBuilderAPI_MakeFace(frame_plane(base_frame(base)),-halfSize,halfSize,-halfSize,halfSize).Face(),false});
  return out;
}
void PlanePicker::preview(const opad::Frame* frame){
  if(!m_preview.IsNull()){m_view->removeOverlay(m_preview);m_preview.Nullify();}for(auto* label:{&m_xLabel,&m_yLabel})if(!label->IsNull()){m_view->removeOverlay(*label);label->Nullify();}if(!frame)return;
  TopoDS_Compound shape;BRep_Builder builder;builder.MakeCompound(shape);const double size=m_view->pixelSize()*(m_originStage?22:65);
  auto point=[&](double u,double v){auto w=frame->to_world(u,v);return gp_Pnt(w[0],w[1],w[2]);};auto edge=[&](double u,double v,double x,double y){builder.Add(shape,BRepBuilderAPI_MakeEdge(point(u,v),point(x,y)).Edge());};
  if(m_originStage){edge(-size*.25,0,size,0);edge(0,-size*.25,0,size);edge(-size*.15,-size*.15,size*.15,size*.15);edge(-size*.15,size*.15,size*.15,-size*.15);}
  else {edge(-size,-size,size,-size);edge(size,-size,size,size);edge(size,size,-size,size);edge(-size,size,-size,-size);}
  m_preview=new AIS_Shape(shape);m_preview->SetInfiniteState(true);m_preview->SetColor(Quantity_Color(.95,.7,.2,Quantity_TOC_sRGB));m_preview->SetWidth(2);m_view->showOverlay(m_preview);
  if(m_originStage){m_xLabel=new AIS_TextLabel();m_yLabel=new AIS_TextLabel();m_xLabel->SetText(TCollection_ExtendedString("X"));m_yLabel->SetText(TCollection_ExtendedString("Y"));m_xLabel->SetPosition(point(size*1.2,0));m_yLabel->SetPosition(point(0,size*1.2));for(auto label:{m_xLabel,m_yLabel}){label->SetInfiniteState(true);label->SetColor(Quantity_Color(1,1,1,Quantity_TOC_sRGB));m_view->showOverlay(label);}}
}
void PlanePicker::pickOrigin(const opad::Ref& ref){
  const int serial=++m_serial;if(m_job)m_job->cancel();auto doc=std::make_shared<opad::Document>(m_doc->doc);auto scene=std::make_shared<opad::Scene>(m_doc->scene);auto origin=std::make_shared<opad::json>();auto frame=std::make_shared<opad::Frame>();const auto support=m_support;QPointer<PlanePicker> guard(this);m_apply->setEnabled(false);
  m_job=m_jobs->async(tr("Resolving sketch origin"),[doc,scene,ref,origin,frame,support](Progress p){if(p.cancelled())return;*origin={{"ref",make_ref(*doc,*scene,ref)}};*frame=resolve_plane(*doc,*scene,{{"support",support},{"origin",*origin}});},[this,guard,serial,origin,frame](bool ok,const QString& error){if(!guard||!m_active||serial!=m_serial)return;m_job=nullptr;if(!ok){m_status->setText(error);m_apply->setEnabled(true);return;}m_origin=*origin;m_frame=*frame;m_status->clear();refresh();});
}
bool PlanePicker::eventFilter(QObject* object,QEvent* event){
  if(object!=m_view||!m_active)return false;
  if(event->type()==QEvent::KeyPress){auto* key=static_cast<QKeyEvent*>(event);if(key->key()==Qt::Key_Escape){cancel();return true;}
    // Enter is OK in the origin stage (the panel's OK, the guide's Enter), as in the other tool panels.
    if(m_originStage && (key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter) && !(key->modifiers()&~Qt::KeypadModifier)){apply();return true;}}
  if(!m_originStage){
    if(event->type()==QEvent::MouseMove)QTimer::singleShot(0,this,[this]{
      if(!m_active || m_originStage)return;const auto id=m_view->hoveredCandidate();const auto previous=property("hoveredSketchPlane").toString();
      if(previous==QString::fromStdString(id))return;setProperty("hoveredSketchPlane",QString::fromStdString(id));
      if(!id.empty()){const auto ref=opad::json::parse(id);if(ref.contains("sketch"))if(const auto* sketch=m_doc->scene.sketch(ref.at("sketch").get<std::string>())){preview(&sketch->frame);m_status->setText(tr("Sketch plane: %1 - click to use").arg(QString::fromStdString(sketch->name)));return;}
        // An origin or a construction plane in the view: outlined and named as the tiles' hover does.
        const auto* feature=ref.contains("feature")?m_doc->scene.feature(ref.value("feature",std::string())):nullptr;
        if(ref.contains("base") || (feature && feature->result.contains("plane"))){const auto frame=feature?opad::Frame::from_json(feature->result.at("plane")):base_frame(ref.value("base",std::string()));preview(&frame);m_status->setText(tr("%1 - click to use").arg(candidateName(ref)));return;}}
      if(!previous.isEmpty()){preview(nullptr);m_status->clear();}
    });return false;
  }
  if(event->type()==QEvent::MouseButtonPress){auto* e=static_cast<QMouseEvent*>(event);if(e->button()!=Qt::LeftButton||QRect(m_view->width()-205,0,205,185).contains(e->position().toPoint()))return false;
    m_mouseDown=true;setProperty("originHoverRef",QString());const auto marker=m_view->widgetPoint(m_frame.origin);m_drag=(marker-e->position().toPoint()).manhattanLength()<18;
    placeOrigin(e->position(),e->modifiers().testFlag(Qt::AltModifier));m_drag=true;return true;
  }
  if(event->type()==QEvent::MouseMove&&m_drag){auto* e=static_cast<QMouseEvent*>(event);
    placeOrigin(e->position(),e->modifiers().testFlag(Qt::AltModifier));return true;}
  if(event->type()==QEvent::MouseButtonRelease&&m_mouseDown){auto* e=static_cast<QMouseEvent*>(event);if(e->button()==Qt::LeftButton){m_mouseDown=false;m_drag=false;placeOrigin(e->position(),e->modifiers().testFlag(Qt::AltModifier));return true;}}
  return false;
}
// A vertex or a round edge's centre under the pointer is the origin as a reference (it follows the model); else the point the
// pointer snaps to on the plane (PlaneSnap: the sketches' and drawings' object snaps, the plane's own origin, a grid node),
// projected onto it. Alt held: where the pointer meets the plane.
void PlanePicker::placeOrigin(const QPointF& point,bool free) {
  opad::Ref ref;
  if(!free && m_view->originReferenceAt(point,ref)) {
    const auto key=QString::fromStdString(ref.str());
    if(property("originHoverRef").toString()!=key){setProperty("originHoverRef",key);pickOrigin(ref);}
    return;
  }
  setProperty("originHoverRef",QString());
  const PlaneSnap::Result snap=m_snap.at(point,m_supportFrame,TopoDS_Face(),{free,true,true});
  if(!snap.ok)return;
  setOrigin(snap.u,snap.v);
  if(snap.exact())m_status->setText(tr("Origin on: %1").arg(Viewport::snapWord(snap.kind)));
}
