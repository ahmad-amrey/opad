#include "SketchEditor.hpp"
#include "DimensionHandle.hpp"
#include "ShapeInput.hpp"
#include "SketchGeometryCache.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include "Jobs.hpp"
#include <QCoreApplication>
#include "opad/design/sketch_modify.hpp"
#include "opad/design/sketch_pattern.hpp"
#include "opad/design/sketch_edit.hpp"
#include "I18n.hpp"
#include "Units.hpp"
#include <cmath>
#include <set>

using namespace opad::design;
void SketchEditor::benchHandles() {
  setTool("rect");placePrecise("0","0",0);placePrecise("40","30",0);
  auto phase=std::make_shared<int>(0),ticks=std::make_shared<int>(0),count=std::make_shared<int>(0);auto* timer=new QTimer(this);timer->setInterval(150);
  connect(timer,&QTimer::timeout,this,[this,phase,ticks,count,timer]{try {
    auto require=[](bool ok,const char* text){if(!ok)throw opad::Error(text);};
    if(++*ticks>120)throw opad::Error("handle preview timed out");
    if(m_editJob || m_geometryJob || m_toolPreviewTimer.isActive())return;
    switch((*phase)++) {
      case 0:setTool("offset");sketchPress(20,0,Qt::NoModifier);require(m_sel.size()==4,"one click selects connected rectangle");require(m_dimensionHandle->isVisible(),"offset handle missing");break;
      case 1:{
        require(m_toolPreview && m_sk.entities.size()==4 && m_toolPreview->entities.size()>4,"automatic non-destructive offset preview");
        const auto before=option("distance","5 mm");const QPointF local(20,20),start=m_dimensionHandle->mapToGlobal(local.toPoint());
        QMouseEvent press(QEvent::MouseButtonPress,local,start,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(m_dimensionHandle,&press);
        QMouseEvent move(QEvent::MouseMove,local+QPointF(35,25),start+QPointF(35,25),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(m_dimensionHandle,&move);
        QMouseEvent release(QEvent::MouseButtonRelease,local+QPointF(35,25),start+QPointF(35,25),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(m_dimensionHandle,&release);
        require(option("distance")!=before,"drag did not update distance");break;
      }
      case 2:require(bool(m_toolPreview),"drag preview missing");m_viewport->grabImage().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_HANDLES")+".png");m_dimensionHandle->grab().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_HANDLES")+".field.png");applyTool(true);require(m_sk.entities.size()>4 && m_tool=="offset","offset commit, the tool kept by Shift");sketchPress(40,15,Qt::NoModifier);require(!m_sel.empty(),"select another edge after offset");break;
      case 3:{require(bool(m_toolPreview),"second edge preview missing");
        for(int i=0;i<12;++i){sketchPress(20,0,Qt::NoModifier);sketchPress(40,15,Qt::NoModifier);previewTool();}
        undo();require(m_tool=="select" && m_sk.entities.size()==4,"active offset undo closes tool and restores original");
        setTool("offset");sketchPress(-15,-15,Qt::NoModifier);sketchMove(55,45,Qt::NoModifier,true);sketchRelease(55,45,Qt::NoModifier);require(m_sel.size()==4,"offset window selection");break;
      }
      case 4:{require(bool(m_toolPreview),"box selection preview");
        QKeyEvent override(QEvent::ShortcutOverride,Qt::Key_7,Qt::NoModifier,"7");QApplication::sendEvent(m_viewport,&override);require(override.isAccepted(),"floating input shortcut override");
        QKeyEvent key(QEvent::KeyPress,Qt::Key_7,Qt::NoModifier,"7");QApplication::sendEvent(m_viewport,&key);require(option("distance")=="7","floating number capture");break;
      }
      case 5:{require(bool(m_toolPreview),"numeric preview");applyTool();setTool("offset");sketchPress(20,0,Qt::NoModifier);
        auto* field=m_dimensionHandle->findChild<QLineEdit*>();field->setFocus();QKeyEvent undoKey(QEvent::KeyPress,Qt::Key_Z,Qt::ControlModifier);QApplication::sendEvent(field,&undoKey);break;}
      case 6:require(m_tool=="select" && m_sk.entities.size()==4,"Ctrl+Z in numeric field must undo geometry and close tool");require(!m_dimensionHandle->isVisible(),"stale handle after undo");
        // The offset relationship (the tool's default): 3 mm, sharp corners, then its label double-clicked and 1 typed.
        setTool("offset");m_options["distance"]="3 mm";m_options["corners"]="sharp";sketchPress(20,0,Qt::NoModifier);break;
      case 7:require(bool(m_toolPreview),"offset relationship preview");applyTool();require(m_tool=="select" && !m_dimensionHandle->isVisible(),"the offset tool closes after Apply");break;
      case 8:{
        const SkConstraint* rel=nullptr;for(const auto& c:m_sk.constraints)if(c.type==SkConstraint::Type::Offset)rel=&c;
        require(rel && rel->sources==4 && rel->refs.size()==8 && std::fabs(std::fabs(rel->value)-3)<1e-9,"the offset is held by an offset relationship of 3 mm");
        const int id=rel->id;const std::vector<int> made(rel->refs.begin()+4,rel->refs.end());
        auto width=[&](bool offset){double lo=1e300,hi=-1e300;const SkConstraint* c=m_sk.constraint(id);
          for(size_t i=offset?4:0;i<(offset?c->refs.size():4);++i)for(int p:m_sk.entity(c->refs[i])->p){lo=std::min(lo,m_sk.point(p)->x);hi=std::max(hi,m_sk.point(p)->x);}return hi-lo;};
        require(std::fabs(std::fabs(width(true)-width(false))/2-3)<1e-6,"its curves lie 3 mm off the rectangle");
        double lu=0,lv=0;labelPosition(*rel,lu,lv);
        require(hitTest(lu,lv).kind==Hit::Dimension && hitTest(lu,lv).id==id,"its distance label is hit where it is drawn");
        sketchDoubleClick(lu,lv);
        require(m_dimEditing==id && m_dimEdit && m_dimEdit->isVisible() && m_dimEdit->text()==units::editable(units::Kind::Length,3),"a double-click on the label opens its distance (3 mm) for typing");
        m_dimEdit->setText("1");m_options["expression"]="1";commitDimensionEdit();
        const SkConstraint* now=m_sk.constraint(id);
        require(now && std::fabs(std::fabs(now->value)-1)<1e-9 && std::vector<int>(now->refs.begin()+4,now->refs.end())==made,"typing 1 keeps the relationship and its curves");
        require(std::fabs(std::fabs(width(true)-width(false))/2-1)<1e-6,"the offset is made again 1 mm off the rectangle");
        require(dimensionText(*now)==units::compact(units::Kind::Length,1),"its label reads 1 mm");
        trace::log("bench: sketch offset relationship: made by the tool, double-click edit 3 -> 1 mm PASS");
        // A branching set (a T of three lines, picked as one by the chain click) at -2 mm, as in the report: each line is
        // offset on its own; "the path must be one connected chain" left the tool without a preview.
        begin_change();const int o=m_sk.add_point(100,0);
        for(auto [x,y]:std::vector<std::pair<double,double>>{{80,0},{120,0},{100,20}})m_sk.add_line(o,m_sk.add_point(x,y));
        require(end_change("bench T"),"the T drawn");fitSketch();(*count)=int(m_sk.entities.size());
        setTool("offset");m_options["distance"]="-2 mm";m_options["corners"]="round";sketchPress(110,0,Qt::NoModifier);
        require(m_sel.size()==3 && chainOnClick(),"the chain click takes the T's three lines");
        break;
      }
      case 9:{
        require(m_toolPreview && int(m_toolPreview->entities.size())==(*count)+3,"a preview of three offsets, one per line of the T");
        trace::log("bench: sketch offset of a branching chain: preview shows one offset per chain PASS");
        // Without typing first: the pointer goes to the arrow from a way off, a step at a time as a mouse moves, and the
        // arrow stays put under it (it used to slide along the line and flip over as the pointer crossed it). Zoomed in as
        // in the report: 2 mm far wider than the arrow, which then lies wholly on the far side of the line.
        {
          const auto target=m_frame.to_world(110,0),normal=m_frame.normal();auto eye=target;for(int i=0;i<3;++i)eye[i]+=normal[i]*100;
          m_viewport->setCameraJson({{"eye",eye},{"target",target},{"up",m_frame.y},{"scale",std::max(1,m_viewport->height())/50.0},{"projection","orthographic"},{"absolute",true}});
          sketchMove(110,-6,Qt::NoModifier,false);
          const QPointF foot=m_viewport->widgetPoint(m_frame.to_world(110,0)),two=m_viewport->widgetPoint(m_frame.to_world(110,2));
          require(QLineF(foot,two).length()>DimensionHandle::kApproach+46,"zoomed in: 2 mm wider than the arrow and its reach");
        }
        const QLineF arrow=m_dimensionHandle->arrowLine();
        QPointF at=arrow.center()+QPointF(110,250);
        auto moveTo=[&](QPointF p){double u,v;require(m_viewport->planePoint(p,m_frame,u,v),"the pointer off the sketch plane");sketchMove(u,v,Qt::NoModifier,false);};
        moveTo(at);
        bool reached=false;QLineF held;
        for(int step=0;step<60 && !reached;++step) {
          const QLineF now=m_dimensionHandle->arrowLine();
          if(m_dimensionHandle->approached(at)) {
            if(held.isNull())held=now;
            require(QLineF(now.p1(),held.p1()).length()<0.5 && QLineF(now.p2(),held.p2()).length()<0.5,"the arrow moved away from the approaching pointer");
          }
          if(m_dimensionHandle->grips(at) && QLineF(at,now.center()).length()<3){reached=true;break;}
          const QPointF to=now.center()-at;const double length=std::hypot(to.x(),to.y());
          at+=length<=8?to:to/length*8;moveTo(at);
        }
        require(reached,"the pointer never got onto the offset arrow: it ran away");
        // Pulled from there (the press through the view, as a real one), the distance follows and the preview with it.
        const auto before=option("distance");
        auto send=[&](QEvent::Type type,QPointF p,Qt::MouseButton button,Qt::MouseButtons buttons){
          QMouseEvent e(type,p,m_viewport->mapToGlobal(p),button,buttons,Qt::NoModifier);QApplication::sendEvent(m_viewport,&e);};
        send(QEvent::MouseButtonPress,at,Qt::LeftButton,Qt::LeftButton);
        require(m_dimensionHandle->dragging(),"a press on the arrow pulls it");
        const QPointF out=arrow.p2()-arrow.p1();const QPointF pull=out/std::max(1.0,std::hypot(out.x(),out.y()))*30;
        send(QEvent::MouseMove,at+pull,Qt::NoButton,Qt::LeftButton);
        send(QEvent::MouseButtonRelease,at+pull,Qt::LeftButton,Qt::NoButton);
        require(option("distance")!=before,"pulling the arrow did not change the distance");
        break;
      }
      case 10:{
        require(m_toolPreview && int(m_toolPreview->entities.size())==(*count)+3,"the preview after the pull");
        applyTool();
        int offsets=0;for(const auto& c:m_sk.constraints)if(c.type==SkConstraint::Type::Offset && c.sources==1)++offsets;
        require(offsets==3,"three offset relationships, one per line of the T");
        trace::log("bench: sketch offset arrow: approached and pulled without typing first, three chains applied PASS");
        break;
      }
      default:timer->stop();trace::log("bench: chain selection, automatic offset preview, drag handle, apply and undo PASS");QCoreApplication::exit(0);
    }
  }catch(const std::exception& e){timer->stop();trace::log(QString("bench: handles FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});timer->start();
}
namespace {
// The distance along the corner's second line (chamfer_corner's order) of a cut `first` along its first line at `angle` to it.
double chamferAt(const Sketch& sk,int point,double first,double angle) {
  std::vector<double> directions;const SkPoint* corner=sk.point(point);
  for(const int id:corner?corner_lines(sk,point):std::vector<int>{}) {  // chamfer_corner's two lines, in its order
    const SkEntity& e=*sk.entity(id);const SkPoint* end=sk.point(e.p[e.p[0]==point?1:0]);directions.push_back(std::atan2(end->y-corner->y,end->x-corner->x));
  }
  if(directions.size()!=2)throw opad::Error("pick a corner joining exactly two lines");
  const double second=shapeinput::chamferSecond(first,angle,shapeinput::between(directions[1]-directions[0]));
  if(second<=0)throw opad::Error("at that angle the chamfer never meets the other line");
  return second;
}
const QStringList tools={"move","rotate","align","scale","copy","mirror","rect_pattern","polar_pattern","split","extend","break","chamfer","union","subtract","intersect","heal","explode"};
}
// On by default where the whole shape is what is usually meant: an offset, an align, a rigid group.
bool SketchEditor::chainOnClick() const { return option("chain:"+m_tool,m_tool=="offset"||m_tool=="align"||m_tool=="c:rigid"?"1":"0")=="1"; }
// Align (Modify): a click on a line takes it (and its chain, the option) into the curves that move and makes it the line to
// align, turning about its end nearer the click; a line of those curves picked again becomes the line to align; with Parallel
// or Collinear, a line that stays put is then the line to align with. The preview follows each pick.
// The curve nearest the pointer where it was pressed (m_pressU/V), within the pick distance; lines before other curves
// with linesFirst. The snapped click lands on a line's end point near its end (where one clicks for Align's pivot), and a
// hit test there takes the point, not the line.
int SketchEditor::curveAtPress(bool linesFirst) const {
  const double u=m_pressU,v=m_pressV,t=tol();
  const SkEntity* e=nullptr;
  double best=t;
  std::vector<const SkEntity*> nearby;  // what the curves' index has about the pointer (a converted drawing has 100k curves)
  if(m_geometry && !m_geometryJob) {
    for(size_t i:m_geometry->query(u-t,v-t,u+t,v+t).entities)if(i<m_sk.entities.size())nearby.push_back(&m_sk.entities[i]);
  } else for(const auto& c:m_sk.entities)nearby.push_back(&c);
  for(const int pass:{0,1}) {
    for(const SkEntity* c:nearby) {
      if(c->type==SkEntity::Type::Point)continue;
      if(linesFirst && (c->type==SkEntity::Type::Line)!=(pass==0))continue;
      if(const double d=distanceTo(*c,u,v);d<=best){best=d;e=c;}
    }
    if(e || !linesFirst)break;
  }
  return e?e->id:0;
}
bool SketchEditor::alignClick(double,double) {
  const SkEntity* e=m_sk.entity(curveAtPress(true));
  const double u=m_pressU,v=m_pressV;
  if(!e){emit status(tr("Pick a line of the curves to move."));return true;}
  const QString to=option("alignTo","vertical");
  const bool target=to=="parallel"||to=="collinear";
  const std::vector<int> moving=rigid_closure(m_sk,m_sel);
  const bool moves=std::find(moving.begin(),moving.end(),e->id)!=moving.end();
  int ea=0,eb=0;
  const bool ends=align_ends(m_sk,e->id,ea,eb);  // a line, or an arc's or open spline's chord (a DXF's straight-looking arc)
  QString refused;  // said after the prompt, which would overwrite it
  auto reference=[&]{
    if(!ends){refused=tr("Pick a line, an arc or an open spline to align: a circle has no direction.");return;}
    const SkPoint *a=m_sk.point(ea),*b=m_sk.point(eb);
    const int pivot=std::hypot(a->x-u,a->y-v)<=std::hypot(b->x-u,b->y-v)?a->id:b->id;
    m_picked={e->id};m_options["alignPivot"]=QString::number(pivot);
  };
  if(target && !m_picked.empty() && !moves) {
    if(!ends)refused=tr("Pick a line to align with.");
    else{m_picked.resize(1);m_picked.push_back(e->id);}
  } else if(moves) reference();
  else{pickCurve(e->id);reference();}
  invalidatePreview();rebuild();toolPrompt();emit changed();scheduleToolPreview();
  if(!refused.isEmpty())emit status(refused);
  return true;
}
void SketchEditor::pickCurve(int id) {
  const bool picked=std::find(m_sel.begin(),m_sel.end(),id)!=m_sel.end();
  std::vector<int> ids{id};
  if(chainOnClick())for(int e:connected_entities(m_sk,{id}))if(e!=id && m_sk.entity(e))ids.push_back(e);
  if(picked) {  // a picked curve again drops it (its chain with it: re-adding the chain kept it selected for good)
    const std::set<int> drop(ids.begin(),ids.end());
    m_sel.erase(std::remove_if(m_sel.begin(),m_sel.end(),[&](int s){return drop.count(s)>0;}),m_sel.end());
  } else for(int e:ids)if(std::find(m_sel.begin(),m_sel.end(),e)==m_sel.end())m_sel.push_back(e);
}
bool SketchEditor::modifyClick(double u,double v) {
  if(m_tool=="break_link") {  // a click picks a linked curve, a window several (TODO 11 wave 3, P4); Enter or Apply unlinks them
    const auto hit=hitTest(u,v);
    const auto* e=hit.kind==Hit::Entity?m_sk.entity(hit.id):nullptr;
    if(!e || e->source.is_null())emit status(tr("Pick a linked curve: they are drawn amber."));
    else pickCurve(e->id);
    rebuild();toolPrompt();emit changed();return true;
  }
  if(!tools.contains(m_tool))return false;
  if(m_tool=="align")return alignClick(u,v);
  const auto hit=hitTest(u,v);
  if(m_tool=="mirror" && option("mirrorAxis","picked")=="picked" && option("mirrorStage","seed")=="axis") {
    // The line, then its mirror image as a preview (TODO 11 wave 3, P4): Enter or Apply keeps it.
    if(const auto* e=m_sk.entity(hit.id);e && e->type==SkEntity::Type::Line)m_picked={e->id};
    else emit status(tr("Pick mirror line"));
    rebuild();toolPrompt();emit changed();scheduleToolPreview();return true;
  }
  if(m_tool=="split") {
    if(hit.kind!=Hit::Entity){emit status(tr("Pick inside a curve to split it."));return true;}
    const int id=hit.id;runSketchEdit(tr("Split curve"),[id,u,v](Sketch& sk){split_entity(sk,id,u,v);});return true;
  }
  if(m_tool=="extend") {  // one click (UI-28): the end nearer it runs on to the nearest curve it meets
    if(hit.kind!=Hit::Entity){emit status(tr("Pick a line or an arc near the end to extend."));return true;}
    const int id=hit.id;runSketchEdit(tr("Extend curve"),[id,u,v](Sketch& sk){extend_entity(sk,id,u,v);});
  } else if(m_tool=="union" || m_tool=="subtract" || m_tool=="intersect") {
    m_clicks.push_back({u,v});if(m_clicks.size()>2)m_clicks.erase(m_clicks.begin());
  } else if(hit.kind!=Hit::None) {
    if(m_tool=="chamfer" && hit.kind==Hit::Point)m_sel={hit.id};
    else if(hit.kind==Hit::Entity)pickCurve(hit.id);
  }
  rebuild();toolPrompt();emit changed();scheduleToolPreview();return true;
}

bool SketchEditor::applyModify() {
  if(!tools.contains(m_tool))return false;
  try {
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
    const ParamTable params(defs,m_doc->scene.units);const auto table=sketch_parameters(m_sk,params);
    auto length=[&](const char* key,const char* fallback){return table.length(option(key,fallback).toStdString());};
    std::vector<int> ids;for(int id:m_sel)if(m_sk.entity(id))ids.push_back(id);
    if(m_tool=="mirror") {
      if(ids.empty()){if(m_previewRequested)return true;throw opad::Error("select curves to mirror first");}
      if(option("mirrorAxis","picked")!="picked")mirrorSelection(0);
      else if(!m_picked.empty())mirrorSelection(m_picked.front());
      // Apply with the curves chosen and no line yet: now the line. Never from the preview, which runs 120 ms after each
      // curve picked: that moved the tool on to the line after the first curve, and the next click became the line.
      else if(!m_previewRequested){m_options["mirrorStage"]="axis";toolPrompt();}
    } else if(m_tool=="align") {
      const QString to=option("alignTo","vertical");
      SketchAlign align;
      align.to=to=="horizontal"?AlignTo::Horizontal:to=="parallel"?AlignTo::Parallel:to=="collinear"?AlignTo::Collinear:AlignTo::Vertical;
      if(m_picked.empty()){if(m_previewRequested)return true;throw opad::Error("pick the line to align");}
      if((align.to==AlignTo::Parallel||align.to==AlignTo::Collinear) && m_picked.size()<2){if(m_previewRequested)return true;throw opad::Error("pick the line to align with");}
      align.reference=m_picked[0];align.pivot=option("alignPivot","0").toInt();align.target=m_picked.size()>1?m_picked[1]:0;
      align.hold=option("alignHold","0")=="1";
      runSketchEdit(tr("Align"),[ids,align](Sketch& sk){align_entities(sk,ids,align);});
    } else if(m_tool=="heal") {
      const double tolerance=length("healTolerance","0.05 mm");
      runSketchEdit(tr("Heal endpoints"),[tolerance](Sketch& sk){heal_endpoints(sk,tolerance);heal_to_curves(sk,tolerance);});
    } else if(m_tool=="break") {
      if(ids.size()<2)throw opad::Error("select at least two curves to split at their intersections");
      runSketchEdit(tr("Break at intersections"),[ids](Sketch& sk){break_intersections(sk,ids);});
    } else if(m_tool=="rect_pattern" || m_tool=="polar_pattern") {
      opad::json inputs={{"polar",m_tool=="polar_pattern"}};
      for(const auto& [key,fallback]:std::vector<std::pair<const char*,const char*>>{{"count","3"},{"rows","1"},{"dx","10 mm"},{"dy","10 mm"},{"angle","360 deg"},{"cx","0 mm"},{"cy","0 mm"}}){const auto expr=option(key,fallback).toStdString();inputs[key]=(std::string(key)=="dx"||std::string(key)=="dy"||std::string(key)=="cx"||std::string(key)=="cy")?table.explicit_length(expr):expr;}
      const int existing=m_sel.empty()?0:pattern_of(m_sk,m_sel.front(),true);
      runSketchEdit(tr("Pattern"),[ids,inputs,params,existing](Sketch& sk){if(existing)edit_pattern(sk,existing,inputs,params);else create_pattern(sk,ids,inputs,params);});
    } else if(m_tool=="explode") {
      std::set<int> patterns;for(int id:m_sel)if(int pattern=pattern_of(m_sk,id,true))patterns.insert(pattern);
      if(patterns.empty())throw opad::Error("select a pattern instance; polylines are already editable individual curves");
      runSketchEdit(tr("Explode pattern"),[patterns](Sketch& sk){for(int id:patterns)remove_pattern(sk,id,true);});
    } else if(m_tool=="chamfer") {
      if(m_sel.size()!=1 || !m_sk.point(m_sel.front()))throw opad::Error("pick a corner point");
      // By two distances, or by the first and its angle to the first line (the second distance where the cut meets the other).
      const int id=m_sel.front();const double a=length("first","2 mm");const bool angled=option("chamferMode","distance")=="angle";
      const double b=angled?table.angle(option("chamferAngle","45 deg").toStdString()):length("second","2 mm");
      runSketchEdit(tr("Chamfer"),[id,a,b,angled](Sketch& sk){chamfer_corner(sk,id,a,angled?chamferAt(sk,id,a,b):b);});
    } else if(m_tool=="union" || m_tool=="subtract" || m_tool=="intersect") {
      if(m_clicks.size()!=2)throw opad::Error("pick inside two closed loops first");
      const auto a=m_clicks[0],b=m_clicks[1];const auto operation=m_tool.toStdString();
      runSketchEdit(tr("Combine regions"),[a,b,operation](Sketch& sk){boolean_regions(sk,a.u,a.v,b.u,b.v,operation);});
    } else if(m_tool!="split" && m_tool!="extend") {
      SketchTransform transform;const bool copy=m_tool=="copy";
      if((m_tool=="move"||copy) && option("moveMode","xy")=="polar") {
        const double d=length("moveDistance","10 mm"),a=table.angle(option("moveAngle","0 deg").toStdString());transform.x=d*std::cos(a);transform.y=d*std::sin(a);
      } else if(m_tool=="move"||copy){transform.x=length("dx","10 mm");transform.y=length("dy","0 mm");}
      if(m_tool=="rotate"||m_tool=="scale"){transform.cx=length("cx","0 mm");transform.cy=length("cy","0 mm");}
      if(m_tool=="rotate")transform.angle=table.angle(option("angle","45 deg").toStdString());
      if(m_tool=="scale")transform.scale=table.number(option("scale","2").toStdString());
      const int copies=copy?table.count(option("copies","1").toStdString()):1;  // a copy's count: one after the other
      if(copies<1 || copies>1000)throw opad::Error("the count of copies must be 1 to 1000");
      runSketchEdit(tr("Transform geometry"),[ids,transform,copy,copies](Sketch& sk){for(int i=1;i<=copies;++i){auto step=transform;step.x*=i;step.y*=i;transform_entities(sk,ids,step,copy);}});
    }
  }catch(const std::exception& e){emit status(i18n::t(QString::fromUtf8(e.what())));}
  return true;
}

void SketchEditor::deleteNode() {
  if(m_sel.size()!=1 || !m_sk.point(m_sel.front())){emit status(tr("Select one curve node first."));return;}
  const int id=m_sel.front();runSketchEdit(tr("Delete curve node"),[id](Sketch& sk){delete_curve_node(sk,id);});
}

void SketchEditor::benchModify() {
  const int source=m_sk.add_circle(m_sk.add_point(20,10),3);m_solved=solve(m_sk);rebuild();
  auto phase=std::make_shared<int>(0),ticks=std::make_shared<int>(0),copyId=std::make_shared<int>(0);
  auto* timer=new QTimer(this);timer->setInterval(50);
  connect(timer,&QTimer::timeout,this,[this,timer,phase,ticks,copyId,source]{
    try {
      if(++*ticks>600)throw opad::Error("modify workflow timed out");
      if(m_editJob)return;
      auto require=[](bool ok,const char* why){if(!ok)throw opad::Error(why);};
      switch((*phase)++) {
        case 0:m_sel={source};setTool("copy");m_options["dx"]="10 mm";m_options["dy"]="0 mm";applyTool();break;
        case 1:require(m_sk.entities.size()==2,"copy tool");m_sel={source};setTool("rect_pattern");m_options["count"]="3";m_options["rows"]="1";m_options["dx"]="15 mm";applyTool();break;
        case 2:require(m_sk.entities.size()==4 && m_sk.patterns.size()==1,"pattern creation");*copyId=m_sk.entities.back().id;m_sel={*copyId};setTool("rect_pattern");m_options["count"]="5";applyTool();break;
        case 3:require(m_sk.entities.size()==6 && m_sk.entity(*copyId),"pattern edit retained instance IDs");m_sel={*copyId};setTool("explode");applyTool();break;
        case 4:require(m_sk.patterns.empty() && !m_sk.entity(*copyId)->fixed,"pattern explosion");m_sel={source};setTool("offset");m_options["distance"]="2 mm";applyTool();break;
        case 5:require(m_sk.entities.size()==7,"asynchronous circle offset");m_sel={source};setTool("mirror");m_options["mirrorAxis"]="y";applyTool();break;
        default:require(m_sk.entities.size()==9,"mirror curve and construction axis");require(m_solved.converged,"constrained mirrored geometry");timer->stop();trace::log("bench: sketch transform, pattern, offset and mirror workflow PASS");QCoreApplication::exit(0);break;
      }
    }catch(const std::exception& e){timer->stop();trace::log(QString("bench: sketch modify workflow FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}
  });timer->start();
}
