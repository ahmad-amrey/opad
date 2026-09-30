#include "SketchEditor.hpp"
#include "DimensionHandle.hpp"
#include "SketchPanel.hpp"
#include <QPointer>
#include <QKeyEvent>
#include <QSettings>
#include <QCoreApplication>
#include <QImage>
#include <QFileInfo>
#include "Jobs.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include "I18n.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch_modify.hpp"
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <TopoDS_Compound.hxx>
#include "opad/geometry.hpp"

using namespace opad::design;

SolveOptions SketchEditor::solveOptions() const {
  SolveOptions out;
  out.tolerance=QSettings().value("sketch/tolerance",1e-8).toDouble();
  out.max_iterations=QSettings().value("sketch/iterations",100).toInt();
  return out;
}

QList<ToolStep> SketchEditor::toolSteps() const {
  QStringList labels;
  const bool transform=QStringList{"move","rotate","scale","copy","rect_pattern","polar_pattern","break","explode"}.contains(m_tool);
  if(transform)labels={tr("Select seed curves"),tr("Set parameters and apply")};
  else if(m_tool=="heal")labels={tr("Set gap tolerance"),tr("Apply to merge nearby endpoints")};
  else if(m_tool=="chamfer")labels={tr("Pick a corner"),tr("Set distances and apply")};
  else if(m_tool=="split")labels={tr("Pick inside the curve to split")};
  else if(m_tool=="extend")labels={tr("Pick the curve near its end"),tr("Pick the boundary curve")};
  else if(m_tool=="union"||m_tool=="subtract"||m_tool=="intersect")labels={tr("Pick inside the first loop"),tr("Pick inside the second loop"),tr("Apply to combine the loops")};
  else if(m_tool=="tangent_circle")labels={tr("Pick first line"),tr("Pick second line"),tr("Choose circle side")};
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
  else if(m_tool=="project"||m_tool=="intersect_body"||m_tool=="silhouette"||m_tool=="include3d")labels={tr("Pick source geometry"),tr("Choose link behavior and apply")};
  else if(m_tool=="image_insert")labels={tr("Choose an image file"),tr("Pick insertion point"),tr("Set image size and apply")};
  else if(m_tool=="image_calibrate")labels={tr("Pick first calibration point"),tr("Pick second calibration point"),tr("Enter known distance and apply")};
  else if(m_tool=="image_trace")labels={tr("Choose the backdrop image"),tr("Adjust tracing parameters"),tr("Apply to create editable curves")};
  else if(m_tool.startsWith("image_")||m_tool=="vector_import"||m_tool=="vector_export"||m_tool=="simplify")labels={tr("Choose source and parameters"),tr("Apply")};
  else if(m_tool=="line" || m_tool=="spline")labels={tr("Pick start point"),tr("Add points"),tr("Apply to finish the chain")};
  else if(m_tool=="point")labels={tr("Place point")};
  else if(m_tool=="circle3" || m_tool=="arc3" || m_tool=="arcc" || m_tool=="ellipse" || m_tool=="slot")labels={tr("Pick first point"),tr("Pick second point"),tr("Pick third point")};
  else labels={tr("Pick first point"),tr("Pick second point")};
  int count=int(m_clicks.size());
  if(transform || m_tool=="chamfer")count=m_sel.empty()?0:1;
  if(m_tool=="extend")count=int(m_picked.size());
  if(m_tool=="tangent_circle")count=int(m_picked.size());
  if(m_tool=="text")count=1;
  if(m_tool=="project"||m_tool=="intersect_body"||m_tool=="silhouette"||m_tool=="include3d")count=option("projectionSource").isEmpty()?0:1;
  if(m_tool=="control_spline")count=m_clicks.size()>=2?1:0;
  if(m_tool=="line" || m_tool=="spline") count=m_chain.empty()?0:m_chain.size()==1?1:2;
  if(m_tool.startsWith("c:"))count=int(m_picked.size());
  if(m_tool=="dimension")count=m_dimEditing?2:m_placingDim?1:0;
  if(m_tool=="select" || m_tool=="offset" || m_tool=="node")count=m_sel.empty()?0:1;
  if(m_tool=="mirror")count=option("mirrorAxis","picked")!="picked"?(m_sel.empty()?0:2):option("mirrorStage","seed")!="axis"?0:m_picked.empty()?1:2;
  if(m_tool=="fillet")count=1;
  QList<ToolStep> result;
  for(int i=0;i<labels.size();++i)result.push_back({labels[i],i<count?tr("Ready"):QString()});
  return result;
}

void SketchEditor::applyTool() {
  if(!m_active)return;
  if(m_editJob)return;
  if(!m_previewRequested)m_toolPreviewTimer.stop();
  if(!m_previewRequested && m_toolPreview) {
    ++m_modelRevision;
    m_viewport->removeOverlay(m_toolPreviewOverlay);m_toolPreviewOverlay.Nullify();
    m_undo.push_back({m_sk,m_plane,m_frame});m_redo.clear();m_sk=*m_toolPreview;m_solved=m_previewSolved;m_toolPreview.reset();m_modified=true;m_panelFieldsDirty=true;
    m_clicks.clear();m_picked.clear();m_sel.clear();updateDimensionHandle();rebuild();scheduleFill();toolPrompt();emit changed();return;
  }
  if(applyReference()||applyImageTool())return;
  if(applyModify())return;
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
  const bool preview=m_previewRequested;
  m_previewComputing=preview;
  const int previewRevision=m_previewRevision;
  const int modelRevision=m_modelRevision;
  const auto solved=std::make_shared<SolveResult>();const auto options=solveOptions();const int session=m_session;
  auto shape=std::make_shared<TopoDS_Compound>();auto prs=std::make_shared<std::shared_ptr<BodyPrs>>();const auto frame=m_frame;
  QPointer<SketchEditor> guard(this);
  m_editJob=m_jobs->async(label,[after,before,solved,options,work,preview,shape,prs,frame](Progress progress){
    if(progress.cancelled())return;work(*after);if(progress.cancelled())return;
    after->validate();*solved=solve(*after,options);
    if(!solved->converged)throw opad::Error("the operation conflicts with existing constraints");
    if(preview) {
      BRep_Builder builder;builder.MakeCompound(*shape);
      std::map<int,const SkEntity*> oldEntities;for(const auto& e:before->entities)oldEntities[e.id]=&e;
      std::map<int,const SkPoint*> oldPoints,newPoints;for(const auto& p:before->points)oldPoints[p.id]=&p;for(const auto& p:after->points)newPoints[p.id]=&p;
      for(const auto& e:after->entities) {
        if(progress.cancelled())return;
        const auto* old=oldEntities.count(e.id)?oldEntities[e.id]:nullptr;
        bool same=old && old->type==e.type && old->p==e.p && old->r==e.r && old->weights==e.weights;
        if(same)for(int id:e.p){const auto* p=oldPoints.count(id)?oldPoints[id]:nullptr;const auto* q=newPoints[id];if(!p||!q||p->x!=q->x||p->y!=q->y){same=false;break;}}
        if(same)continue;const auto edge=entity_edge(*after,e,frame);if(!edge.IsNull())builder.Add(*shape,edge);
      }
      Bnd_Box box;BRepBndLib::Add(*shape,box);*prs=BodyPrs::build(*shape,box);
    }
  },[this,guard,before,after,solved,session,preview,previewRevision,modelRevision,shape,prs](bool ok,const QString& error){
    if(!guard || !m_active || m_session!=session)return;
    m_editJob=nullptr;m_previewComputing=false;
    // A preview for an older value: shown while the handle is still dragged (the newest one is computed next), but
    // never kept for Apply. Otherwise the newer run replaces it.
    const bool stale=preview && previewRevision!=m_previewRevision;
    if(stale && !m_dimensionHandle->dragging())return;
    if(!ok){if(!stale){if(preview){m_viewport->removeOverlay(m_toolPreviewOverlay);m_toolPreviewOverlay.Nullify();}emit status(error);}return;}
    if(m_modelRevision!=modelRevision)return;
    if(preview){
      if(!stale){m_toolPreview=after;m_previewSolved=*solved;}
      m_viewport->removeOverlay(m_toolPreviewOverlay);
      auto overlay=new BodyShape(*shape,*prs);overlay->SetColor(Quantity_Color(.95,.65,.2,Quantity_TOC_sRGB));overlay->SetWidth(2*m_viewport->displayScale());m_toolPreviewOverlay=overlay;if(m_visible)m_viewport->showOverlay(m_toolPreviewOverlay);
      if(!stale)emit status(tr("Preview ready. Apply to keep it, or change parameters and preview again."));return;}
    ++m_modelRevision;m_undo.push_back({*before,m_plane,m_frame});m_redo.clear();m_sk=*after;m_solved=*solved;m_modified=true;m_panelFieldsDirty=true;
    m_clicks.clear();m_picked.clear();m_sel.clear();rebuild();scheduleFill();toolPrompt();emit changed();
    if(m_tool=="mirror")m_options["mirrorStage"]="seed";
  });
}

void SketchEditor::placePrecise(const QString& u,const QString& v,int mode) {
  try {
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
    const auto table=sketch_parameters(m_sk,ParamTable(defs,m_doc->scene.units));
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
  invalidatePreview();
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
  invalidatePreview();
  m_sel=connected_entities(m_sk,m_sel);rebuild();emit this->changed();
}
void SketchEditor::selectType() {
  invalidatePreview();
  const auto* seed=m_sel.empty()?nullptr:m_sk.entity(m_sel.front());
  const auto type=seed?seed->type:SkEntity::Type::Point;
  m_sel.clear();for(const auto& e:m_sk.entities)if(seed?e.type==type:selectable(e.id))m_sel.push_back(e.id);
  rebuild();emit changed();
}

void SketchEditor::redefinePlane(const opad::json& plane,const opad::Frame& frame) {
  begin_change();
  const auto normal=m_frame.normal();double separation=0,orientation=0;
  for(int i=0;i<3;++i){separation+=(frame.origin[i]-m_frame.origin[i])*normal[i];orientation+=std::abs(frame.x[i]-m_frame.x[i])+std::abs(frame.y[i]-m_frame.y[i]);}
  if(std::abs(separation)<1e-7 && orientation<1e-7){double u,v;m_frame.to_local(frame.origin,u,v);shift_sketch_origin(m_sk,u,v);}
  m_plane=plane;m_frame=frame;
  if(!end_change(tr("Redefine sketch plane")))return;
  m_viewport->endSketchInput();m_viewport->beginSketchInput(this,frame,m_id);fitSketch();
  m_fill.clear();rebuild();scheduleFill();emit changed();
}

void SketchEditor::benchWorkflow() {
  if(qEnvironmentVariableIsSet("OPAD_BENCH_SKETCH_REFERENCE")) {
    const QString prefix=qEnvironmentVariable("OPAD_BENCH_SKETCH_REFERENCE");QImage image(32,32,QImage::Format_RGB32);image.fill(Qt::white);
    for(int y=4;y<28;++y)for(int x=4;x<28;++x)if(x<10||x>=22||y<10||y>=22)image.setPixelColor(x,y,Qt::black);image.save(prefix+".source.png");
    auto phase=std::make_shared<int>(0),ticks=std::make_shared<int>(0);auto* timer=new QTimer(this);timer->setInterval(50);
    connect(timer,&QTimer::timeout,this,[this,prefix,timer,phase,ticks]{try {
      if(++*ticks>1200)throw opad::Error("reference workflow timed out");if(m_editJob||m_imageJob)return;
      auto require=[](bool ok,const char* why){if(!ok)throw opad::Error(why);};
      switch((*phase)++) {
        case 0:setTool("image_insert");m_options["imageFile"]=prefix+".source.png";m_options["imageWidth"]="32 mm";placePrecise("0","0",0);applyTool();break;
        case 1:require(m_sk.images.size()==1 && m_imagePrs.size()==1,"embedded backdrop");setTool("image_calibrate");m_options["knownDistance"]="16 mm";placePrecise("0","0",0);placePrecise("32","0",0);applyTool();break;
        case 2:require(std::fabs(m_sk.images[0]["width"].get<double>()-16)<1e-8,"image calibration");setTool("image_trace");m_options["smoothing"]="0";m_options["noise"]="2";m_options["cornerAngle"]="180";previewTool();break;
        case 3:if(m_toolPreview){require(m_sk.entities.empty() && m_undo.size()==2,"preview does not modify the sketch");fitSketch();m_viewport->grabImage().save(prefix+".preview.png");applyTool();--*phase;break;}require(m_sk.entities.size()==8,"editable trace with hole");setTool("project");m_options["projectionSource"]="{\"base\":\"x\"}";applyTool();break;
        case 4:require(m_sk.entities.size()==9 && !m_sk.entities.back().source.is_null(),"linked work geometry");m_sel={m_sk.entities.back().id};setTool("break_link");applyTool();break;
        case 5:require(m_sk.entities.back().source.is_null()&&!m_sk.entities.back().fixed,"break reference link");setTool("vector_export");m_options["vectorFile"]=prefix+".svg";applyTool();break;
        case 6:require(QFileInfo(prefix+".svg").size()>0,"SVG export");setTool("vector_import");applyTool();break;
        case 7:require(m_sk.entities.size()==18,"SVG import as editable curves");setTool("image_edit");m_options["imageAngle"]="30 deg";m_options["imageOpacity"]="0.7";applyTool();break;
        case 8:{require(std::fabs(m_sk.images[0]["angle"].get<double>()-M_PI/6)<1e-8,"image rotation");auto camera=m_viewport->cameraJson();camera["scale"]=48;camera["target"]={8,8,0};camera["eye"]={8,8,100};m_viewport->setCameraJson(camera);m_viewport->grabImage().save(prefix+".viewport.png");for(auto* panel:m_viewport->window()->findChildren<SketchPanel*>())panel->grab().save(prefix+".panel.png");setTool("image_remove");applyTool();break;}
        case 9:require(m_sk.images.empty()&&m_imagePrs.empty(),"remove backdrop");undo();require(m_sk.images.size()==1,"image undo");break;
        case 10:for(auto* panel:m_viewport->window()->findChildren<SketchPanel*>())QMetaObject::invokeMethod(panel,"finishRequested");break;
        case 11:if(m_active||m_doc->designBusy){--*phase;break;}require(m_imagePrs.empty()&&!m_imageJob,"image cleanup on sketch exit");require(!m_doc->scene.sketches.empty()&&!m_doc->scene.sketches.back().geometry.at("images").empty(),"backdrop committed to document");break;
        case 12:case 13:case 14:case 15:case 16:case 17:case 18:case 19:break;
        default:{auto camera=m_viewport->cameraJson();camera["scale"]=48;camera["target"]={8,8,0};camera["eye"]={8,8,100};m_viewport->setCameraJson(camera);m_viewport->grabImage().save(prefix+".saved.png");timer->stop();trace::log("bench: sketch projection, image and vector workflow PASS");QCoreApplication::exit(0);break;}
      }
    }catch(const std::exception& e){timer->stop();trace::log(QString("bench: sketch reference workflow FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});timer->start();return;
  }

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
    undo();require(std::fabs(m_frame.origin[2])<1e-9,"replane undo");redo();require(std::fabs(m_frame.origin[2]-12)<1e-9,"replane redo");
    setTool("arcc");placePrecise("100","0",0);placePrecise("110","0",0);placePrecise("100","10",0);
    setTool("dimension");m_options["dimensionType"]="arc_length";m_options["reference"]="1";
    const int arc=m_sk.entities.back().id;dimensionClick({Hit::Entity,arc},107,7);placeDimension(116,16);
    require(m_sk.constraints.back().type==SkConstraint::Type::ArcLength && m_sk.constraints.back().reference,"arc length reference from side panel");
    const auto camera=m_viewport->cameraJson();
    QCoreApplication::processEvents();
    m_viewport->grabImage().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_WORKFLOW")+".viewport.png");
    m_viewport->window()->grab().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_WORKFLOW")+".window.png");
    for(auto* panel:m_viewport->window()->findChildren<SketchPanel*>())panel->grab().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_WORKFLOW")+".panel.png");
    end();const auto restored=m_viewport->cameraJson();
    require(restored.at("eye")==camera.at("eye") && restored.at("target")==camera.at("target"),"camera preserved on sketch exit");
    trace::log("bench: sketch guided workflow PASS");QCoreApplication::exit(0);
  }catch(const std::exception& e){trace::log(QString("bench: sketch guided workflow FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}
}

void SketchEditor::benchDrag() {
  for(int i=0;i<200;++i){int a=m_sk.add_point(i*30,0),b=m_sk.add_point(i*30+10,0),line=m_sk.add_line(a,b);m_sk.add_constraint(SkConstraint::Type::Distance,{line},10);}
  m_tool="select";m_dragging=true;m_dragMoved=false;m_dragHit={Hit::Point,3};m_dragU=10;m_dragV=0;m_dragStart={{3,{10,0}}};
  sketchMove(12,2,Qt::NoModifier,true);sketchMove(15,10,Qt::NoModifier,true);sketchMove(20,15,Qt::NoModifier,true);sketchRelease(20,15,Qt::NoModifier);
  auto ticks=std::make_shared<int>(0);auto* timer=new QTimer(this);timer->setInterval(20);
  connect(timer,&QTimer::timeout,this,[this,timer,ticks]{
    if(m_editJob && ++*ticks<1000)return;
    timer->stop();trace::log(QString("bench: drag final x=%1 y=%2 undo=%3").arg(m_sk.point(3)->x,0,'g',12).arg(m_sk.point(3)->y,0,'g',12).arg(m_undo.size()));bool ok=!m_editJob && m_undo.size()==1 && std::hypot(m_sk.point(3)->x-20,m_sk.point(3)->y-15)<1e-3;
    undo();ok &= std::hypot(m_sk.point(3)->x-10,m_sk.point(3)->y)<1e-9;
    trace::log(QString("bench: asynchronous sketch drag and single undo %1").arg(ok?"PASS":"FAIL"));QCoreApplication::exit(ok?0:2);
  });timer->start();
}

void SketchEditor::fitSketch() {
  if(!m_active)return;m_viewport->lookAt(m_frame,false,false);double x0,y0,x1,y1;
  bool any=sketch_bounds(m_sk,x0,y0,x1,y1);
  for(const auto& image:m_sk.images) {
    const double a=image.value("angle",0.0),x=image.at("position")[0].get<double>(),y=image.at("position")[1].get<double>(),w=image.at("width").get<double>(),h=image.at("height").get<double>();
    for(auto [u,v]:std::vector<std::pair<double,double>>{{0,0},{w,0},{w,h},{0,h}}){double px=x+u*std::cos(a)-v*std::sin(a),py=y+u*std::sin(a)+v*std::cos(a);if(!any){x0=x1=px;y0=y1=py;any=true;}else{x0=std::min(x0,px);x1=std::max(x1,px);y0=std::min(y0,py);y1=std::max(y1,py);}}
  }
  if(!any || x1-x0+y1-y0<1e-8) {
    // An empty sketch frames the visible model as seen on its plane (a fixed 120 mm window shrank a 20 mm face the
    // plane pick had just filled the view with). The view boxes are cached per body: no geometry walked here.
    any=false;
    for(const auto& id:m_doc->scene.all_bodies()) {
      if(!m_doc->scene.effectively_visible(id))continue;
      Bnd_Box box;try{box=opad::node_world_bbox(m_doc->doc,m_doc->scene,id);}catch(const std::exception&){continue;}
      if(box.IsVoid())continue;double bx0,by0,bz0,bx1,by1,bz1;box.Get(bx0,by0,bz0,bx1,by1,bz1);
      for(int c=0;c<8;++c){double u,v;m_frame.to_local({c&1?bx1:bx0,c&2?by1:by0,c&4?bz1:bz0},u,v);if(!any){x0=x1=u;y0=y1=v;any=true;}else{x0=std::min(x0,u);x1=std::max(x1,u);y0=std::min(y0,v);y1=std::max(y1,v);}}
    }
    if(!any || x1-x0+y1-y0<1e-8){x0=y0=-60;x1=y1=60;}
  }
  const double aspect=double(std::max(1,m_viewport->width()))/std::max(1,m_viewport->height());const double scale=std::max({20.0,(y1-y0)*1.3,(x1-x0)*1.3/aspect});
  auto camera=m_viewport->cameraJson();auto center=m_frame.to_world((x0+x1)/2,(y0+y1)/2),eye=center;const auto normal=m_frame.normal();for(int i=0;i<3;++i)eye[i]+=normal[i]*std::max(100.0,scale*2);
  camera["eye"]=eye;camera["target"]=center;camera["scale"]=scale;camera["projection"]="orthographic";m_viewport->setCameraJson(camera);
}
void SketchEditor::analyseSketch() {
  if(m_sk.points.size()<=300){Sketch copy=m_sk;m_solved=solve(copy,solveOptions());return;}
  const auto copy=std::make_shared<Sketch>(m_sk);const auto result=std::make_shared<SolveResult>();const auto options=solveOptions();const int session=m_session;
  QPointer<SketchEditor> guard(this);m_editJob=m_jobs->async(tr("Analysing sketch constraints"),[copy,result,options](Progress p){if(!p.cancelled())*result=solve(*copy,options);},[this,guard,result,session](bool ok,const QString& error){if(!guard||!m_active||session!=m_session)return;m_editJob=nullptr;if(ok){m_solved=*result;rebuild();emit changed();}else emit status(error);});
}

void SketchEditor::previewTool() {
  // The shown preview stays until this one replaces it (or fails): no blank frame between two previews.
  if(!m_active||m_editJob)return;invalidatePreview(true);m_previewRequested=true;applyTool();m_previewRequested=false;
}
void SketchEditor::invalidatePreview(bool keepOverlay) {
  ++m_previewRevision;
  if(!keepOverlay){m_viewport->removeOverlay(m_toolPreviewOverlay);m_toolPreviewOverlay.Nullify();}
  if(!m_toolPreview)return;m_toolPreview.reset();rebuild();
}
void SketchEditor::scheduleToolPreview() {
  // While the offset arrow is dragged the preview follows as fast as it is computed (throttled, the shown one stays
  // until the next replaces it); otherwise it waits for the value to rest. Restarting the timer on every move meant
  // no preview until the button was let go.
  const bool live=m_dimensionHandle->dragging();
  invalidatePreview(live);updateDimensionHandle();
  const QStringList tools={"offset","move","rotate","scale","copy","mirror","rect_pattern","polar_pattern","chamfer","union","subtract","intersect"};
  if(m_active && tools.contains(m_tool) && (!m_sel.empty() || m_clicks.size()==2)) {
    if(!live)m_toolPreviewTimer.start(120);
    else if(!m_toolPreviewTimer.isActive())m_toolPreviewTimer.start(16);
  }
  else m_toolPreviewTimer.stop();
}
void SketchEditor::updateDimensionHandle() {
  if(!m_active || !m_visible || m_tool!="offset" || m_sel.empty()){m_dimensionHandle->hide();return;}
  if(m_dimensionHandle->interacting())return;
  // The arrow goes to the selected curve under the pointer and stays on it after the pointer leaves it (on its way to
  // the arrow): falling back to the first selected curve made the arrow jump back there, so only that edge could be
  // pulled.
  auto selected=[&](int id){return std::find(m_sel.begin(),m_sel.end(),id)!=m_sel.end() && m_sk.entity(id);};
  if(m_hover.kind==Hit::Entity && selected(m_hover.id))m_offsetAnchor=m_hover.id;
  if(!selected(m_offsetAnchor)){m_offsetAnchor=0;for(int id:m_sel)if(m_sk.entity(id)){m_offsetAnchor=id;break;}}
  const SkEntity* entity=m_offsetAnchor?m_sk.entity(m_offsetAnchor):nullptr;
  if(!entity || entity->p.empty())return;
  const auto* a=m_sk.point(entity->p.front());if(!a)return;
  double x=a->x,y=a->y,dx=1,dy=0;
  if(entity->type==SkEntity::Type::Circle || entity->type==SkEntity::Type::Arc){
    const double angle=m_haveCursor?std::atan2(m_cursor.v-y,m_cursor.u-x):0.;dx=std::cos(angle);dy=std::sin(angle);
    const auto* start=entity->p.size()>1?m_sk.point(entity->p[1]):nullptr;const double radius=start?std::hypot(start->x-x,start->y-y):entity->r;x+=radius*dx;y+=radius*dy;
  }
  else if(entity->p.size()>1) {
    const auto* b=m_sk.point(entity->p.back());if(!b)return;
    x=(a->x+b->x)/2;y=(a->y+b->y)/2;dx=b->y-a->y;dy=a->x-b->x;
    const double length=std::hypot(dx,dy);if(length<1e-9)return;dx/=length;dy/=length;
    if(m_haveCursor){const auto points=sampled(*entity);double best=1e300;
      for(size_t i=1;i<points.size();++i){const auto [ax,ay]=points[i-1];const auto [bx,by]=points[i];const double ex=bx-ax,ey=by-ay,l2=ex*ex+ey*ey;if(l2<1e-20)continue;
        const double t=std::clamp(((m_cursor.u-ax)*ex+(m_cursor.v-ay)*ey)/l2,0.,1.),px=ax+t*ex,py=ay+t*ey,d=std::hypot(px-m_cursor.u,py-m_cursor.v);
        if(d<best){best=d;x=px;y=py;dx=ey/std::sqrt(l2);dy=-ex/std::sqrt(l2);}
      }
    }
    // A closed chain grows outward for a positive distance whichever way its curves run: point the arrow outward.
    std::vector<std::pair<std::pair<double,double>,std::pair<double,double>>> loop;std::map<int,int> ends;
    for(int id:m_sel)if(const auto* e=m_sk.entity(id)) {
      if(e->type==SkEntity::Type::Line || e->type==SkEntity::Type::Arc || e->type==SkEntity::Type::Spline){
        const int first=e->type==SkEntity::Type::Arc?e->p[1]:e->p.front(),last=e->type==SkEntity::Type::Arc?e->p[2]:e->p.back();++ends[first];++ends[last];}
      const auto pts=sampled(*e);for(size_t i=1;i<pts.size();++i)loop.push_back({pts[i-1],pts[i]});
    }
    const bool closed=!ends.empty() && std::all_of(ends.begin(),ends.end(),[](const auto& end){return end.second%2==0;});
    if(closed) {
      const double probe=std::max(1e-9,0.25*tol());const double qx=x+dx*probe,qy=y+dy*probe;bool inside=false;
      for(const auto& [a,b]:loop)if((a.second>qy)!=(b.second>qy) && qx<a.first+(qy-a.second)*(b.first-a.first)/(b.second-a.second))inside=!inside;
      if(inside){dx=-dx;dy=-dy;}
    }
  }
  try {
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
    const auto text=option("distance","5 mm");const double value=sketch_parameters(m_sk,ParamTable(defs,m_doc->scene.units)).length(text.toStdString());
    opad::Vec3 direction;for(int i=0;i<3;++i)direction[i]=m_frame.x[i]*dx+m_frame.y[i]*dy;
    m_dimensionHandle->configure(m_frame.to_world(x,y),direction,value,text);
  }catch(const std::exception&){}
}
