#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include <QPointer>
#include <QKeyEvent>
#include <QSettings>
#include <QCoreApplication>
#include "Jobs.hpp"
#include <cmath>
#include "I18n.hpp"

using namespace opad::design;

SolveOptions SketchEditor::solveOptions() const {
  SolveOptions out;
  out.tolerance=QSettings().value("sketch/tolerance",1e-8).toDouble();
  out.max_iterations=QSettings().value("sketch/iterations",100).toInt();
  return out;
}

QList<ToolStep> SketchEditor::toolSteps() const {
  QStringList labels;
  if(m_tool=="tangent_circle")labels={tr("Pick first line"),tr("Pick second line"),tr("Choose circle side")};
  else if(m_tool=="tangent_arc")labels={tr("Pick line endpoint"),tr("Pick arc endpoint")};
  else if(m_tool=="text")labels={tr("Set text, font and height"),tr("Pick insertion point")};
  else if(m_tool=="conic")labels={tr("Pick start point"),tr("Pick tangent intersection"),tr("Pick end point")};
  else if(m_tool=="rect3")labels={tr("Pick first corner"),tr("Pick base direction"),tr("Set rectangle height")};
  else if(m_tool=="arcslot")labels={tr("Pick arc centre"),tr("Pick start point"),tr("Pick end point")};
  else if(m_tool=="cslot")labels={tr("Pick slot centre"),tr("Pick cap centre"),tr("Set slot width")};
  else if(m_tool=="control_spline")labels={tr("Pick control points"),tr("Apply to finish the chain")};
  else if(m_tool=="select")labels={tr("Select geometry"),tr("Drag, constrain or modify")};
  else if(m_tool=="dimension")labels={tr("Pick geometry to measure"),tr("Place the label"),tr("Set expression and apply")};
  else if(m_tool.startsWith("c:"))labels={tr("Pick first geometry"),tr("Pick related geometry")};
  else if(m_tool=="offset")labels={tr("Select a connected chain"),tr("Set distance and apply")};
  else if(m_tool=="mirror")labels={tr("Select curves"),tr("Pick mirror line")};
  else if(m_tool=="fillet")labels={tr("Set radius"),tr("Pick a corner")};
  else if(m_tool=="node")labels={tr("Select a spline node"),tr("Set weights and apply")};
  else if(m_tool=="trim")labels={tr("Pick the segment to remove")};
  else if(m_tool=="project")labels={tr("Pick source geometry")};
  else if(m_tool=="line" || m_tool=="spline")labels={tr("Pick start point"),tr("Add points"),tr("Apply to finish the chain")};
  else if(m_tool=="point")labels={tr("Place point")};
  else if(m_tool=="circle3" || m_tool=="arc3" || m_tool=="arcc" || m_tool=="ellipse" || m_tool=="slot")labels={tr("Pick first point"),tr("Pick second point"),tr("Pick third point")};
  else labels={tr("Pick first point"),tr("Pick second point")};
  int count=int(m_clicks.size());
  if(m_tool=="tangent_circle")count=int(m_picked.size());
  if(m_tool=="text")count=1;
  if(m_tool=="control_spline")count=m_clicks.size()>=2?1:0;
  if(m_tool=="line" || m_tool=="spline") count=m_chain.empty()?0:m_chain.size()==1?1:2;
  if(m_tool.startsWith("c:"))count=int(m_picked.size());
  if(m_tool=="dimension")count=m_dimEditing?2:m_placingDim?1:0;
  if(m_tool=="select" || m_tool=="offset" || m_tool=="mirror" || m_tool=="node")count=m_sel.empty()?0:1;
  if(m_tool=="fillet")count=1;
  QList<ToolStep> result;
  for(int i=0;i<labels.size();++i)result.push_back({labels[i],i<count?tr("Ready"):QString()});
  return result;
}

void SketchEditor::applyTool() {
  if(!m_active)return;
  if(m_tool=="offset")return offsetSelection();
  if(m_tool=="control_spline")return finishPrimitive();
  if(m_tool=="line" || m_tool=="spline")return finishChain();
  if(m_tool=="dimension" && m_dimEditing && m_dimEdit) {
    m_dimEdit->setText(option("expression",m_dimEdit->text()));
    commitDimensionEdit();return;
  }
  if(m_tool=="node" && m_sel.size()==1) {
    int entity=0,index=0;
    for(const auto& e:m_sk.entities)if(e.degree && !e.fixed) {
      auto it=std::find(e.p.begin(),e.p.end(),m_sel.front());if(it!=e.p.end()){entity=e.id;index=int(it-e.p.begin());break;}
    }
    if(!entity)return;
    begin_change();
    try {
      auto* e=m_sk.entity(entity);
      auto weight=[&](int at,const QString& key){bool ok=false;const double v=option(key,"1").toDouble(&ok);if(!ok || !std::isfinite(v) || v<=0)throw opad::Error("spline weights must be positive");e->weights[size_t(at)]=v;};
      weight(index,"weight");
      if(index%3==0 && e->degree==3){if(index>0)weight(index-1,"incoming");if(index+1<int(e->p.size()))weight(index+1,"outgoing");}
      end_change(tr("Spline node weights"));
    } catch(const std::exception& e){cancel_change();emit status(QString::fromUtf8(e.what()));}
    return;
  }
  toolPrompt();
}

void SketchEditor::runSketchEdit(const QString& label,std::function<void(Sketch&)> work) {
  if(!m_active || m_editJob)return;
  const auto before=std::make_shared<Sketch>(m_sk),after=std::make_shared<Sketch>(m_sk);
  const auto solved=std::make_shared<SolveResult>();const auto options=solveOptions();const int session=m_session;
  QPointer<SketchEditor> guard(this);
  m_editJob=m_jobs->async(label,[after,solved,options,work](Progress progress){
    if(progress.cancelled())return;work(*after);if(progress.cancelled())return;
    after->validate();*solved=solve(*after,options);
    if(!solved->converged)throw opad::Error("the operation conflicts with existing constraints");
  },[this,guard,before,after,solved,session](bool ok,const QString& error){
    if(!guard || !m_active || m_session!=session)return;
    m_editJob=nullptr;
    if(!ok){emit status(error);return;}
    if(m_sk.to_json()!=before->to_json())return;
    m_undo.push_back(*before);m_redo.clear();m_sk=*after;m_solved=*solved;m_modified=true;
    m_clicks.clear();m_picked.clear();m_sel.clear();rebuild();scheduleFill();toolPrompt();emit changed();
  });
}

void SketchEditor::placePrecise(const QString& u,const QString& v,int mode) {
  try {
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
    const auto table=sketch_parameters(m_sk,ParamTable(defs));
    double x=table.length(u.toStdString()),y=mode==2?table.angle(v.toStdString()):table.length(v.toStdString());
    if(mode==2){const double r=x;x=r*std::cos(y);y=r*std::sin(y);}
    if(mode) {
      if(!m_chain.empty()){const auto* p=m_sk.point(m_chain.back());x+=p->x;y+=p->y;}
      else if(!m_clicks.empty()){x+=m_clicks.back().u;y+=m_clicks.back().v;}
    }
    if(m_tool=="select")return emit status(tr("Choose a drawing tool first."));
    click(Snap{x,y},Qt::AltModifier); // exact coordinates: no inferred constraints or grid snapping
  }catch(const std::exception& e){emit status(i18n::t(QString::fromUtf8(e.what())));}
}

void SketchEditor::stepBack() {
  if(!m_clicks.empty())m_clicks.pop_back();
  else if(!m_picked.empty()){m_picked.pop_back();m_placingDim=false;}
  else if(!m_chain.empty()){undo();}
  else setTool("select");
  toolPrompt();rebuild();emit changed();
}

void SketchEditor::toggleReference() {
  if(!m_active)return;
  begin_change();bool any=false;
  for(int id:m_sel)if(auto* c=m_sk.constraint(id);c && c->is_dimension()) {
    c->reference=!c->reference;c->expr.clear();c->value=dimension_value(m_sk,*c);any=true;
  }
  if(any)end_change(tr("Driving / reference"));else cancel_change();
}

bool SketchEditor::selectable(int id) const {
  if(m_tool!="select" || m_selectionFilter=="all")return true;
  if(m_sk.point(id))return m_selectionFilter=="point";
  if(const auto* e=m_sk.entity(id)) {
    if(m_selectionFilter=="construction")return e->construction;
    if(m_selectionFilter=="arc")return e->type==SkEntity::Type::Circle || e->type==SkEntity::Type::Arc;
    return m_selectionFilter==SkEntity::type_name(e->type);
  }
  for(const auto& c:m_sk.constraints)if(c.id==id)return m_selectionFilter==(c.is_dimension()?"dimension":"constraint");
  return false;
}

void SketchEditor::selectConnected() {
  std::set<int> ids(m_sel.begin(),m_sel.end()),points;
  for(int id:ids)if(m_sk.point(id))points.insert(id);
  bool changed=true;
  while(changed){changed=false;for(const auto& e:m_sk.entities){bool touch=ids.count(e.id);for(int p:e.p)touch|=points.count(p)>0;if(!touch)continue;if(ids.insert(e.id).second)changed=true;for(int p:e.p)if(points.insert(p).second)changed=true;}}
  m_sel.assign(ids.begin(),ids.end());rebuild();emit this->changed();
}
void SketchEditor::selectType() {
  const auto* seed=m_sel.empty()?nullptr:m_sk.entity(m_sel.front());
  const auto type=seed?seed->type:SkEntity::Type::Point;
  m_sel.clear();for(const auto& e:m_sk.entities)if(seed?e.type==type:selectable(e.id))m_sel.push_back(e.id);
  rebuild();emit changed();
}

void SketchEditor::redefinePlane(const opad::json& plane,const opad::Frame& frame) {
  m_plane=plane;m_frame=frame;m_modified=true;
  m_viewport->endSketchInput();m_viewport->beginSketchInput(this,frame,m_id);m_viewport->lookAt(frame,true,false);
  m_fill.clear();rebuild();scheduleFill();emit changed();
}

void SketchEditor::benchWorkflow() {
  try {
    auto require=[](bool ok,const char* why){if(!ok)throw opad::Error(why);};
    m_viewport->setGridSnap(false);
    setTool("rect");placePrecise("0","0",0);placePrecise("40","25",0);
    require(m_sk.entities.size()==4,"precise rectangle");
    setTool("select");sketchPress(-5,-5,Qt::NoModifier);sketchMove(45,30,Qt::NoModifier,true);sketchRelease(45,30,Qt::NoModifier);
    int curves=0;for(int id:m_sel)if(m_sk.entity(id))++curves;
    require(curves==4,"window selection");
    m_sel={m_sk.entities.front().id};selectConnected();curves=0;for(int id:m_sel)if(m_sk.entity(id))++curves;
    require(curves==4,"connected chain");
    setTool("polygon");m_options["sides"]="7";placePrecise("70","10",0);placePrecise("80","10",0);
    require(m_sk.entities.size()==12,"panel polygon sides"); // seven sides and its construction circle
    const auto old=m_sk.to_json();undo();require(m_sk.entities.size()==4,"single-step polygon undo");redo();require(m_sk.to_json()==old,"polygon redo");
    const int line=m_sk.entities.front().id;
    begin_change();const int dim=m_sk.add_constraint(SkConstraint::Type::Distance,{line},40);require(end_change("dimension"),"dimension creation");
    editDimension(dim,false);m_options["expression"]="50 mm";applyTool();require(std::fabs(dimension_value(m_sk,*m_sk.constraint(dim))-50)<1e-6,"panel dimension edit");
    m_sel={dim};toggleReference();require(m_sk.constraint(dim)->reference,"reference dimension");
    const auto geometry=m_sk.to_json();
    opad::Frame frame;frame.origin={0,0,12};redefinePlane({{"base","xy"},{"frame",frame.to_json()}},frame);
    require(m_sk.to_json()==geometry,"replane preserved constraints and geometry");
    const auto camera=m_cameraBefore;
    QCoreApplication::processEvents();
    m_viewport->grabImage().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_WORKFLOW")+".viewport.png");
    m_viewport->window()->grab().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_WORKFLOW")+".window.png");
    for(auto* panel:m_viewport->window()->findChildren<SketchPanel*>())panel->grab().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_WORKFLOW")+".panel.png");
    end();const auto restored=m_viewport->cameraJson();
    require(restored.at("eye")==camera.at("eye") && restored.at("target")==camera.at("target"),"camera restoration");
    trace::log("bench: sketch guided workflow PASS");QCoreApplication::exit(0);
  }catch(const std::exception& e){trace::log(QString("bench: sketch guided workflow FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}
}
