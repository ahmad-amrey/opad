#include "SketchEditor.hpp"
#include "SketchGeometryCache.hpp"
#include "opad/canvas.hpp"
#include "DimensionHandle.hpp"
#include "SketchPanel.hpp"
#include "SketchSteps.hpp"
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
  out.tolerance=m_settings.tolerance;
  out.max_iterations=m_settings.iterations;
  return out;
}

// The tool's steps (SketchSteps.hpp), and what each done one took, in place of "Ready" (UI-25): where a click went (in the
// document's unit), the point or curve picked, how many curves are selected, the value set.
QList<ToolStep> SketchEditor::toolSteps() const {
  QList<ToolStep> out;
  const auto* entry = sketchsteps::find(m_tool.toStdString());
  if (!entry) return {{tr("Choose a tool"), {}}};  // never: every tool has its steps (sketch-steps test, sketch-steps bench)
  for (const char* step : entry->steps) out.push_back({i18n::t(step), {}});
  const QString& t = m_tool;
  const double unit = unitLength();
  auto number = [&](double mm) {
    QString s = QString::number(mm / unit, 'f', 3);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.'))) s.chop(1);
    return s == "-0" ? QStringLiteral("0") : s;
  };
  auto at = [&](double u, double v) { return number(u) + ", " + number(v); };
  auto name = [&](int id) {
    if (m_sk.point(id)) return tr("Point %1").arg(id);
    const SkEntity* e = m_sk.entity(id);
    if (!e) return QString::number(id);
    switch (e->type) {
      case SkEntity::Type::Line: return tr("Line %1").arg(id);
      case SkEntity::Type::Circle: return tr("Circle %1").arg(id);
      case SkEntity::Type::Arc: return tr("Arc %1").arg(id);
      case SkEntity::Type::Ellipse: return tr("Ellipse %1").arg(id);
      case SkEntity::Type::Spline: return tr("Spline %1").arg(id);
      case SkEntity::Type::Point: break;
    }
    return tr("Point %1").arg(id);
  };
  auto names = [&](const std::vector<int>& ids) { QStringList n; for (int id : ids) n << name(id); return n.join(", "); };
  auto selection = [&] {
    int curves = 0;
    for (int id : m_sel) curves += (m_geometry ? m_geometry->entity(m_sk, id) : m_sk.entity(id)) != nullptr;  // indexed: a box may select 30,000
    if (curves != int(m_sel.size())) return tr("%1 selected").arg(m_sel.size());
    return curves == 1 ? tr("1 curve") : tr("%1 curves").arg(curves);
  };
  auto done = [&](int i, const QString& value) { if (i >= 0 && i < out.size() && !value.isEmpty()) out[i].picked = value; };
  auto file = [&](const char* key) { return QFileInfo(option(key)).fileName(); };
  static const QStringList selecting = {"select", "move", "rotate", "scale", "copy", "rect_pattern", "polar_pattern", "break", "explode", "break_link", "offset", "copybase"};
  if (selecting.contains(t)) {
    if (!m_sel.empty()) done(0, selection());
  } else if (t == "chamfer" || t == "node") {
    if (!m_sel.empty()) done(0, name(m_sel.front()));
  } else if (t == "fillet") done(0, option("radius", "2 mm"));
  else if (t == "heal") done(0, option("healTolerance", "0.05 mm"));
  else if (t == "simplify") done(0, option("curveTolerance"));
  else if (t == "text") done(0, option("text", "OPAD"));
  else if (t == "extend" || t == "tangent_circle" || t.startsWith("c:")) {
    for (size_t i = 0; i < m_picked.size(); ++i) done(int(i), name(m_picked[i]));
  } else if (t == "tangent_arc") {
    if (!m_picked.empty() && !m_clicks.empty()) done(0, name(m_picked.front()));
  } else if (t == "mirror") {
    const bool picked = option("mirrorAxis", "picked") == "picked";
    if (!m_sel.empty() && (!picked || option("mirrorStage", "seed") == "axis")) done(0, selection());
    if (!picked) done(1, option("mirrorAxis") == "x" ? tr("X axis") : tr("Y axis"));
    else if (!m_picked.empty()) done(1, name(m_picked.front()));
  } else if (t == "dimension") {
    const auto edited = std::find_if(m_sk.constraints.begin(), m_sk.constraints.end(), [&](const SkConstraint& c) { return m_dimEditing && c.id == m_dimEditing; });
    if (const SkConstraint* c = edited == m_sk.constraints.end() ? nullptr : &*edited) {
      done(0, names(c->refs));
      done(1, dimensionText(*c));
    } else if (m_placingDim) done(0, names(m_pendingDim.refs));
  } else if (t == "line" || t == "spline") {
    if (const SkPoint* p = m_chain.empty() ? nullptr : pointOf(m_chain.front())) done(0, at(p->x, p->y));
    if (m_chain.size() >= 2) done(1, m_chain.size() == 2 ? tr("1 more point") : tr("%1 more points").arg(m_chain.size() - 1));
  } else if (t == "control_spline") {
    if (m_clicks.size() >= 2) done(0, tr("%1 points").arg(m_clicks.size()));
  } else if (sketchkeys::referenceTool(t.toStdString())) {
    if (!m_sources.isEmpty()) done(0, m_sources.size() == 1 ? tr("1 source") : tr("%1 sources").arg(m_sources.size()));
  } else if (t == "image_edit" || t == "image_trace" || t == "image_remove") {
    if (!m_sk.images.empty()) done(0, tr("Image %1").arg(option("imageId", QString::number(m_sk.images.back().at("id").get<int>()))));
  } else if (t == "vector_import" || t == "vector_export") done(0, file("vectorFile"));
  else {  // shapes, points of an image, loops: where each click went (an image's place after its file)
    const int from = t == "image_insert" ? 1 : 0;
    if (from) done(0, file("imageFile"));
    for (size_t i = 0; i < m_clicks.size(); ++i) done(from + int(i), at(m_clicks[i].u, m_clicks[i].v));
  }
  return out;
}

void SketchEditor::applyTool() {
  if(!m_active)return;
  if(!m_previewRequested)dropPreviewJob();  // Apply (or Enter) right after a value changed: not lost to the preview being computed
  if(m_editJob)return;
  if(!m_previewRequested)m_toolPreviewTimer.stop();
  if(!m_previewRequested && m_toolPreview) {
    ++m_modelRevision;
    m_viewport->removeOverlay(m_toolPreviewOverlay);m_toolPreviewOverlay.Nullify();
    m_undo.push_back({m_sk,m_plane,m_frame});m_redo.clear();m_sk=*m_toolPreview;m_solved=m_previewSolved;m_toolPreview.reset();m_modified=true;m_panelFieldsDirty=true;
    m_clicks.clear();m_picked.clear();m_sel.clear();applied();updateDimensionHandle();rebuild();scheduleFill();toolPrompt();
    emit changed();return;
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
    if(!entity){if(!m_previewRequested)emit status(tr("Choose a control node on an editable spline."));return;}
    try {
      const auto* e=m_sk.entity(entity);
      auto weight=[&](const QString& key){bool ok=false;const double v=option(key,"1").toDouble(&ok);if(!ok || !std::isfinite(v) || v<=0)throw opad::Error("spline weights must be positive");return v;};
      std::vector<std::pair<size_t,double>> weights{{size_t(index),weight("weight")}};
      if(index%3==0 && e->degree==3){if(index>0)weights.push_back({size_t(index-1),weight("incoming")});if(index+1<int(e->p.size()))weights.push_back({size_t(index+1),weight("outgoing")});}
      // As the other Apply tools (TODO 11 wave 3, P4): the curve previews its new shape while the weights change, Enter or
      // Apply keeps it.
      runSketchEdit(tr("Spline node weights"),[entity,weights](Sketch& sk){
        auto* spline=sk.entity(entity);if(!spline)throw opad::Error("the spline no longer exists");
        if(spline->weights.size()<spline->p.size())spline->weights.resize(spline->p.size(),1.0);
        for(const auto& [at,w]:weights)if(at<spline->weights.size())spline->weights[at]=w;
      });
    } catch(const std::exception& e){if(!m_previewRequested)emit status(i18n::t(QString::fromUtf8(e.what())));}
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
    if(!ok){
      if(stale)return;
      if(preview){m_viewport->removeOverlay(m_toolPreviewOverlay);m_toolPreviewOverlay.Nullify();}
      // A source that gives nothing here (an edge square to the plane, a body the plane misses): that pick is not kept, the
      // others stay and preview again. The worker named the ones that failed (applyReference); a failure of them all
      // together (the solver's) drops the one the last pick added, else only says why.
      if(preview && sketchkeys::referenceTool(m_tool.toStdString()) && !m_sources.isEmpty()) {
        QStringList drop;
        if(m_sourcesFailed)for(const auto& source:*m_sourcesFailed)if(m_sources.contains(source))drop<<source;
        if(drop.isEmpty() && !m_sourceAdded.isEmpty() && m_sources.contains(m_sourceAdded))drop<<m_sourceAdded;
        m_sourcesFailed.reset();
        if(drop.isEmpty()){emit status(i18n::t(error));return;}
        for(const auto& source:drop)m_sources.removeOne(source);
        if(drop.contains(m_sourceAdded))m_sourceAdded.clear();
        emit status(tr("Not added: %1").arg(i18n::t(error)));rebuild();emit changed();emit workflowChanged();scheduleToolPreview();return;
      }
      emit status(i18n::t(error));return;
    }
    if(m_modelRevision!=modelRevision)return;
    if(preview){
      if(!stale){m_toolPreview=after;m_previewSolved=*solved;}
      m_viewport->removeOverlay(m_toolPreviewOverlay);
      auto overlay=new BodyShape(*shape,*prs);overlay->SetColor(Quantity_Color(.95,.65,.2,Quantity_TOC_sRGB));overlay->SetWidth(2*m_viewport->displayScale());overlay->Attributes()->WireAspect()->SetTypeOfLine(Aspect_TOL_DASH);  // dashed: what Enter would add, apart from the amber references (as the guides draw it)
      m_toolPreviewOverlay=overlay;if(m_visible)showToolPreview();
      if(!stale)emit status(tr("Preview ready. Press Enter or Apply to keep it, or change the values."));return;}
    ++m_modelRevision;m_undo.push_back({*before,m_plane,m_frame});m_redo.clear();m_sk=*after;m_solved=*solved;m_modified=true;m_panelFieldsDirty=true;
    m_clicks.clear();m_picked.clear();m_sel.clear();applied();rebuild();scheduleFill();toolPrompt();emit changed();
  });
}

bool SketchEditor::placePrecise(const QString& u,const QString& v,int mode) {
  try {
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
    const auto table=sketch_parameters(m_sk,ParamTable(defs,m_doc->scene.units));
    double x=table.length(u.toStdString()),y=mode==2?table.angle(v.toStdString()):table.length(v.toStdString());
    if(mode==2){const double r=x;x=r*std::cos(y);y=r*std::sin(y);}
    if(mode) {
      if(!m_chain.empty()){const auto* p=m_sk.point(m_chain.back());x+=p->x;y+=p->y;}
      else if(!m_clicks.empty()){x+=m_clicks.back().u;y+=m_clicks.back().v;}
    }
    if(m_tool=="select"){emit status(tr("Choose a drawing tool first."));return false;}
    click(Snap{x,y},Qt::AltModifier); // exact coordinates: no inferred constraints or grid snapping
    return true;
  }catch(const std::exception& e){emit status(i18n::t(QString::fromUtf8(e.what())));return false;}
}

// ---------------------------------------------------------------- Undo point, Done, Esc (UI-20)
// "Back" used to undo a whole step and leave the tool (undo() ends it), "Cancel tool" kept everything and the prompt
// promised that Esc steps back: three meanings. Now one model (SketchKeys.hpp) for the keys, the panel and the prompt.
sketchkeys::State SketchEditor::keyState() const {
  sketchkeys::State s;
  s.tool=m_tool.toStdString();s.chain=m_chain.size();s.clicks=m_clicks.size();s.picks=m_picked.size()+(sketchkeys::referenceTool(s.tool)?size_t(m_sources.size()):0);
  s.boxSelecting=m_boxSelecting || m_fencing;s.selection=!m_sel.empty();
  s.mirrorAxis=m_tool=="mirror" && option("mirrorStage","seed")=="axis";
  s.mirrorSeeds=m_tool=="mirror" && option("mirrorAxis","picked")=="picked" && !s.mirrorAxis;
  s.typed=m_input && m_input->typed();s.applies=appliesOnEnter();s.locked=m_lock && m_lock->sticky;
  if(s.locked && m_pointer.kind==Snap::Kind::Locked)s.stops=size_t(std::max(0,m_pointer.stops-(m_pointer.stop>=0?1:0)));
  s.guide=!m_lock && m_inView && placing() && m_pointer.onLine;
  s.snaps=!m_lock && m_inView && placing() && m_pointer.choices>1?size_t(m_pointer.choices-1):0;
  return s;
}

bool SketchEditor::undoPoint() {
  if(!m_active || m_editJob)return false;
  using sketchkeys::Back;
  const Back back=sketchkeys::backspace(keyState());
  if(back==Back::None)return false;
  if(back==Back::Delete){deleteSelection();return true;}
  invalidatePreview();unlock();
  if(!m_chain.empty()) {
    // Chain-local: back to before the last point, drawing goes on from the one before. Each point of a chain is one
    // undo step until the chain ends (finishChain folds them into one), so the newest step holds the sketch to go back
    // to. undo() would also have ended the chain and the tool.
    if(m_undo.size()<=m_chainUndoStart){emit status(tr("That point is older than the undo history."));return false;}
    m_chain.pop_back();
    m_sk=m_undo.back().geometry;m_undo.pop_back();
    m_tracked.erase(std::remove_if(m_tracked.begin(),m_tracked.end(),[this](int id){return !m_sk.point(id);}),m_tracked.end());
    m_conflicts.clear();analyseSketch();scheduleFill();
  } else if(!m_clicks.empty()) {
    m_clicks.pop_back();
    if(m_clicks.empty() && (m_tool=="tangent_arc" || m_tool=="extend"))m_picked.clear();  // picked with that click
  } else if(sketchkeys::referenceTool(m_tool.toStdString()) && !m_sources.isEmpty()) {
    m_sources.removeLast();emit workflowChanged();  // the last source picked
  } else {
    m_picked.pop_back();m_placingDim=false;
    if(m_tool=="dimension" && !m_picked.empty()) {  // what the pick left measures alone (a line) waits to be placed again
      const int keep=m_picked.front();m_picked.clear();dimensionClick({m_sk.point(keep)?Hit::Point:Hit::Entity,keep},0,0);
    }
  }
  toolPrompt();rebuild();emit changed();scheduleToolPreview();
  return true;
}

bool SketchEditor::done() {
  dropPreviewJob();
  if(!m_active || m_editJob)return false;
  switch(sketchkeys::enter(keyState())) {
    case sketchkeys::Enter::None:return false;
    case sketchkeys::Enter::UseTyped:return useTyped();
    case sketchkeys::Enter::EndChain:if(m_tool=="control_spline")finishPrimitive();else finishChain();break;
    case sketchkeys::Enter::PickMirrorLine:m_options["mirrorStage"]="axis";toolPrompt();break;  // the curves are chosen: now the line
    case sketchkeys::Enter::Apply:applyTool();break;
  }
  rebuild();emit changed();return true;
}

bool SketchEditor::escape() {
  if(!m_active || m_editJob)return false;
  using sketchkeys::Esc;
  switch(sketchkeys::escape(keyState())) {
    case Esc::None:return false;
    case Esc::CancelBox:m_boxSelecting=m_fencing=false;break;
    case Esc::DropTyped:m_input->dropTyped();break;  // option values go back to what they were
    case Esc::Unlock:unlock();resnap();break;  // the step goes on
    case Esc::BackToCurves:m_options["mirrorStage"]="seed";toolPrompt();break;
    case Esc::EndChain:  // as Enter, and over for sure (a control-point spline with too few points is dropped); tracking starts afresh
      if(m_tool=="control_spline"){finishPrimitive();m_clicks.clear();toolPrompt();}else finishChain();
      m_tracked.clear();
      break;
    case Esc::CancelStep:cancel_change();m_clicks.clear();m_picked.clear();m_sources.clear();m_placingDim=false;m_tracked.clear();scheduleToolPreview();toolPrompt();break;
    case Esc::CloseTool:setTool("select");break;
    case Esc::ClearSelection:m_sel.clear();break;
  }
  rebuild();emit changed();return true;
}

void SketchEditor::closeTool() {
  for(int rung=0;rung<6 && m_tool!="select" && escape();++rung){}
}

QString SketchEditor::keyHints() const {
  using namespace sketchkeys;
  const State s=keyState();QStringList out;
  const Back back=sketchkeys::backspace(s);
  if(back==Back::UndoPoint)out<<tr("⌫ undo point");
  else if(back==Back::UndoPick)out<<tr("⌫ undo pick");
  else if(back==Back::Delete)out<<tr("Del/⌫ delete");
  const Esc esc=sketchkeys::escape(s);
  if(sketchkeys::enter(s)==Enter::UseTyped)out<<tr("Enter use typed values");
  if(esc==Esc::DropTyped)out<<tr("Esc drop typed values");
  else if(esc==Esc::Unlock)out<<tr("Esc release lock");
  else if(esc==Esc::EndChain)  // Enter does the same; only a second Esc goes on to close the tool
    out<<(m_tool!="line" && std::max(s.chain,s.clicks)>1?tr("Enter/Esc finish spline"):tr("Enter/Esc end chain"));
  else if(sketchkeys::enter(s)==Enter::PickMirrorLine)out<<tr("Enter pick mirror line");
  else if(sketchkeys::enter(s)==Enter::Apply)out<<tr("Enter apply");
  if(esc==Esc::BackToCurves)out<<tr("Esc back to curves");
  else if(esc==Esc::CancelStep)out<<(back==Back::UndoPick?tr("Esc clear picks"):tr("Esc cancel shape"));
  else if(esc==Esc::CloseTool)out<<tr("Esc close tool");
  else if(esc==Esc::ClearSelection)out<<tr("Esc clear selection");
  const Shift shift=sketchkeys::shift(s);  // last: it comes and goes with the guides under the pointer
  if(shift==Shift::Lock)out<<tr("Shift lock");
  else if(shift==Shift::NextStop)out<<tr("Shift next stop");
  else if(shift==Shift::NextSnap)out<<tr("Shift next snap");
  return out.join(QStringLiteral(" · "));
}

void SketchEditor::noteHints() {
  if(const auto shift=sketchkeys::shift(keyState());shift!=m_shiftHint){m_shiftHint=shift;emit hintsChanged();}
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
  if(m_geometry?m_geometry->point(m_sk,id)!=nullptr:m_sk.point(id)!=nullptr)return m_selectionFilter=="point";
  if(const auto* e=m_geometry?m_geometry->entity(m_sk,id):m_sk.entity(id)) {
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

void SketchEditor::selectAll(bool invert) {
  invalidatePreview();
  std::vector<int> picked;
  for(const auto& e:m_sk.entities)if(selectable(e.id) && (!invert || std::find(m_sel.begin(),m_sel.end(),e.id)==m_sel.end()))picked.push_back(e.id);
  m_sel=std::move(picked);rebuild();emit changed();
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
        case 3:if(m_toolPreview){require(m_sk.entities.empty() && m_undo.size()==2,"preview does not modify the sketch");fitSketch();m_viewport->grabImage().save(prefix+".preview.png");applyTool();--*phase;break;}require(m_sk.entities.size()==8,"editable trace with hole");setTool("project");toggleSource("{\"base\":\"x\"}");applyTool();break;
        case 4:require(m_sk.entities.size()==9 && !m_sk.entities.back().source.is_null(),"linked work geometry");m_sel={m_sk.entities.back().id};setTool("break_link");applyTool();break;
        case 5:require(m_sk.entities.back().source.is_null()&&!m_sk.entities.back().fixed,"break reference link");setTool("vector_export");m_options["vectorFile"]=prefix+".svg";applyTool();break;
        case 6:require(QFileInfo(prefix+".svg").size()>0,"SVG export");setTool("vector_import");applyTool();break;
        case 7:require(m_sk.entities.size()==18,"SVG import as editable curves");setTool("image_edit");m_options["imageAngle"]="30 deg";m_options["imageOpacity"]="0.7";applyTool();break;
        case 8:{require(std::fabs(m_sk.images[0]["angle"].get<double>()-M_PI/6)<1e-8,"image rotation");auto camera=m_viewport->cameraJson();camera["scale"]=48;camera["target"]={8,8,0};camera["eye"]={8,8,100};m_viewport->setCameraJson(camera);m_viewport->grabImage().save(prefix+".viewport.png");for(auto* panel:m_viewport->window()->findChildren<SketchPanel*>())panel->grab().save(prefix+".panel.png");setTool("image_remove");applyTool();break;}
        case 9:require(m_sk.images.empty()&&m_imagePrs.empty(),"remove backdrop");undo();require(m_sk.images.size()==1,"image undo");break;
        case 10:for(auto* panel:m_viewport->window()->findChildren<SketchPanel*>())QMetaObject::invokeMethod(panel,"finishRequested");break;
        case 11:if(m_active||m_doc->designBusy){--*phase;break;}require(m_imagePrs.empty()&&!m_imageJob,"image cleanup on sketch exit");require(!m_doc->scene.sketches.empty()&&m_doc->scene.sketches.back().geometry.value("images",opad::json::array()).empty(),"no picture record left in the sketch");{bool canvas=false;for(const auto& id:m_doc->scene.all_bodies())canvas=canvas||opad::is_canvas(*m_doc->scene.node(id));require(canvas,"backdrop committed to document as an image canvas on the sketch's plane");}break;
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
void SketchEditor::dropPreviewJob() {
  if(m_editJob && m_previewComputing){m_editJob->cancel();m_editJob=nullptr;m_previewComputing=false;}
}
void SketchEditor::invalidatePreview(bool keepOverlay) {
  ++m_previewRevision;
  if(!keepOverlay){m_viewport->removeOverlay(m_toolPreviewOverlay);m_toolPreviewOverlay.Nullify();}
  if(!m_toolPreview)return;m_toolPreview.reset();rebuild();
}
void SketchEditor::applied() {
  if(m_tool=="mirror")m_options["mirrorStage"]="seed";  // the next curves to mirror, then their line
  if(m_tool=="image_calibrate"){m_options.remove("knownDistance");m_calibrateShown.clear();}  // the next two clicks show their own distance
  if(sketchkeys::referenceTool(m_tool.toStdString()) && !m_sources.isEmpty()){m_sources.clear();emit workflowChanged();}  // the next sources
}
void SketchEditor::showToolPreview() {
  if(m_toolPreviewOverlay.IsNull())return;
  m_viewport->showOverlay(m_toolPreviewOverlay);
  m_toolPreviewOverlay->SetZLayer(Graphic3d_ZLayerId_TopOSD);  // no depth test there (as the offset's arrow over its body)
}
void SketchEditor::scheduleToolPreview() {
  // While the offset arrow is dragged the preview follows as fast as it is computed (throttled, the shown one stays
  // until the next replaces it); otherwise it waits for the value to rest. Restarting the timer on every move meant
  // no preview until the button was let go. The reference tools preview their sources as they are picked (TODO 11
  // wave 3, P4: the guides show each pick's projection before Enter).
  const bool live=m_dimensionHandle->dragging();
  invalidatePreview(live);updateDimensionHandle();updateInput();
  const QStringList tools={"offset","move","rotate","scale","copy","mirror","rect_pattern","polar_pattern","chamfer","union","subtract","intersect","node"};
  const bool reference=sketchkeys::referenceTool(m_tool.toStdString());
  if(m_active && ((tools.contains(m_tool) && (!m_sel.empty() || m_clicks.size()==2)) || (reference && !m_sources.isEmpty()))) {
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
