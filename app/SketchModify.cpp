#include "SketchEditor.hpp"
#include "DimensionHandle.hpp"
#include "ShapeInput.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include "Jobs.hpp"
#include <QCoreApplication>
#include "opad/design/sketch_modify.hpp"
#include "opad/design/sketch_pattern.hpp"
#include "opad/design/sketch_edit.hpp"
#include <cmath>
#include <set>

using namespace opad::design;
void SketchEditor::benchHandles() {
  setTool("rect");placePrecise("0","0",0);placePrecise("40","30",0);
  auto phase=std::make_shared<int>(0),ticks=std::make_shared<int>(0);auto* timer=new QTimer(this);timer->setInterval(150);
  connect(timer,&QTimer::timeout,this,[this,phase,ticks,timer]{try {
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
      case 2:require(bool(m_toolPreview),"drag preview missing");m_viewport->grabImage().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_HANDLES")+".png");m_dimensionHandle->grab().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_HANDLES")+".field.png");applyTool();require(m_sk.entities.size()>4,"offset commit");sketchPress(40,15,Qt::NoModifier);require(!m_sel.empty(),"select another edge after offset");break;
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
      case 6:require(m_tool=="select" && m_sk.entities.size()==4,"Ctrl+Z in numeric field must undo geometry and close tool");require(!m_dimensionHandle->isVisible(),"stale handle after undo");break;
      default:timer->stop();trace::log("bench: chain selection, automatic offset preview, drag handle, apply and undo PASS");QCoreApplication::exit(0);
    }
  }catch(const std::exception& e){timer->stop();trace::log(QString("bench: handles FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});timer->start();
}
namespace {
// The distance along the corner's second line (chamfer_corner's order) of a cut `first` along its first line at `angle` to it.
double chamferAt(const Sketch& sk,int point,double first,double angle) {
  std::vector<double> directions;const SkPoint* corner=sk.point(point);
  for(const auto& e:sk.entities)if(corner && e.type==SkEntity::Type::Line && std::find(e.p.begin(),e.p.end(),point)!=e.p.end()) {
    const SkPoint* end=sk.point(e.p[e.p[0]==point?1:0]);directions.push_back(std::atan2(end->y-corner->y,end->x-corner->x));
  }
  if(directions.size()!=2)throw opad::Error("pick a corner joining exactly two lines");
  const double second=shapeinput::chamferSecond(first,angle,shapeinput::between(directions[1]-directions[0]));
  if(second<=0)throw opad::Error("at that angle the chamfer never meets the other line");
  return second;
}
const QStringList tools={"move","rotate","scale","copy","mirror","rect_pattern","polar_pattern","split","extend","break","chamfer","union","subtract","intersect","heal","explode"};
}
bool SketchEditor::modifyClick(double u,double v) {
  if(!tools.contains(m_tool))return false;
  const auto hit=hitTest(u,v);
  if(m_tool=="mirror" && option("mirrorAxis","picked")=="picked" && option("mirrorStage","seed")=="axis") {
    if(const auto* e=m_sk.entity(hit.id);e && e->type==SkEntity::Type::Line)m_picked={e->id};
    else emit status(tr("Pick mirror line"));
    rebuild();toolPrompt();emit changed();return true;
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
    else if(hit.kind==Hit::Entity){auto at=std::find(m_sel.begin(),m_sel.end(),hit.id);if(at==m_sel.end())m_sel.push_back(hit.id);else m_sel.erase(at);if(option("chain","0")=="1")selectConnected();}
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
      if(ids.empty())throw opad::Error("select curves to mirror first");
      if(option("mirrorAxis","picked")!="picked")mirrorSelection(0);
      else if(!m_picked.empty())mirrorSelection(m_picked.front());
      else {m_options["mirrorStage"]="axis";toolPrompt();}
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
  }catch(const std::exception& e){emit status(QString::fromUtf8(e.what()));}
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
