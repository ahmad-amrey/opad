#include "SketchEditor.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch_text.hpp"
#include "Jobs.hpp"
#include <QCoreApplication>
#include "SketchPanel.hpp"
#include "opad/design/sketch_create.hpp"
#include <QFont>
#include <QFontMetricsF>
#include <QPainterPath>
#include <cmath>

using namespace opad::design;
namespace {
const QStringList variants={"rect3","circle2","tangent_circle","tangent_arc","cslot","arcslot","polygon_outer","control_spline","conic","text"};
int picksNeeded(const QString& kind){return kind=="circle2"||kind=="polygon_outer"||kind=="tangent_arc"?2:kind=="text"?1:3;}
}

bool SketchEditor::primitiveClick(double u,double v) {
  if(!variants.contains(m_tool))return false;
  if(m_tool=="text"){createText(u,v);return true;}
  if(m_tool=="tangent_circle" && m_picked.size()<2) {
    const Hit h=hitTest(u,v);const auto* e=m_sk.entity(h.id);
    if(!e || e->type!=SkEntity::Type::Line){emit status(tr("Pick two straight lines, then choose the circle side."));return true;}
    if(std::find(m_picked.begin(),m_picked.end(),e->id)==m_picked.end())m_picked.push_back(e->id);
    toolPrompt();rebuild();return true;
  }
  if(m_tool=="tangent_arc" && m_clicks.empty()) {
    const Hit h=hitTest(u,v);
    for(const auto& e:m_sk.entities)if(e.type==SkEntity::Type::Line && (h.id==e.id || std::find(e.p.begin(),e.p.end(),h.id)!=e.p.end())){m_picked={e.id};break;}
    if(m_picked.empty()){emit status(tr("Pick the end of a straight line first."));return true;}
  }
  m_clicks.push_back({u,v});
  if(m_tool!="control_spline" && int(m_clicks.size())>=(m_tool=="tangent_circle"?1:picksNeeded(m_tool)))finishPrimitive();
  toolPrompt();rebuild();return true;
}

void SketchEditor::finishPrimitive() {
  if(m_clicks.empty())return;
  try {
    const auto options=primitiveOptions();
    std::vector<std::pair<double,double>> picks;for(const auto& p:m_clicks)picks.push_back({p.u,p.v});
    begin_change();create_primitive(m_sk,m_tool.toStdString(),picks,options);
    if(end_change(tr("Create geometry"))){m_clicks.clear();m_picked.clear();}else if(!m_clicks.empty())m_clicks.pop_back();
  }catch(const std::exception& e){cancel_change();emit status(QString::fromUtf8(e.what()));if(!m_clicks.empty())m_clicks.pop_back();}
  toolPrompt();rebuild();
}

opad::json SketchEditor::primitiveOptions() const {
  std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
  const auto table=sketch_parameters(m_sk,ParamTable(defs,m_doc->scene.units));
  opad::json options=opad::json::object();
  if(m_tool=="polygon_outer")options["sides"]=table.count(option("sides","6").toStdString());
  if(m_tool=="arcslot")options["width"]=table.length(option("width","2 mm").toStdString());
  if(m_tool=="tangent_circle") {options["radius"]=table.length(option("radius","2 mm").toStdString());options["lines"]=m_picked;}
  if(m_tool=="conic")options["rho"]=table.number(option("rho","0.5").toStdString());
  if(m_tool=="control_spline")options["degree"]=table.count(option("degree","3").toStdString());
  if(m_tool=="tangent_arc")options["line"]=m_picked.at(0);
  return options;
}

Sketch SketchEditor::primitivePreview() const {
  Sketch preview;
  if(!variants.contains(m_tool) || m_tool=="text" || m_tool.startsWith("tangent") || m_clicks.empty())return preview;
  std::vector<std::pair<double,double>> picks;for(const auto& p:m_clicks)picks.push_back({p.u,p.v});picks.push_back({m_cursor.u,m_cursor.v});
  if(m_tool!="control_spline" && int(picks.size())<picksNeeded(m_tool))return preview;
  try {create_primitive(preview,m_tool.toStdString(),picks,primitiveOptions());}catch(...){return Sketch{};}
  return preview;
}

void SketchEditor::createText(double u,double v) {
  try {
    const QString text=option("text","OPAD");if(text.isEmpty() || text.size()>512)throw opad::Error("enter between 1 and 512 text characters");
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});
    const double height=ParamTable(defs,m_doc->scene.units).length(option("height","10 mm").toStdString());if(height<=0)throw opad::Error("text height must be positive");
    const QString style=option("textStyle","outline");
    if(style=="stroke" || style=="block") {
      // The built-in font lives in core (TODO 10 B4): the same letters on every machine and from the CLI and MCP.
      TextOptions t;t.height=height;t.style=style=="stroke"?"stroke":"outline";
      const std::string utf8=text.toStdString();
      runSketchEdit(tr("Creating text outlines"),[utf8,u,v,t](Sketch& sk){add_text(sk,utf8,u,v,t);});
      return;
    }
    QFont font(option("font","Arial"));font.setPixelSize(1000);
    QPainterPath path;path.addText(0,0,font,text);
    const double scale=height/std::max(1.0,QFontMetricsF(font).capHeight());
    runSketchEdit(tr("Creating text outlines"),[path,u,v,scale](Sketch& sk){
      std::map<std::pair<double,double>,int> points;
      auto point=[&](double x,double y){const auto at=std::make_pair(u+x*scale,v-y*scale);auto found=points.find(at);if(found!=points.end())return found->second;return points[at]=sk.add_point(at.first,at.second);};
      int last=0;
      for(int i=0;i<path.elementCount();++i) {
        const auto e=path.elementAt(i);
        if(e.isMoveTo())last=point(e.x,e.y);
        else if(e.isLineTo()){const int next=point(e.x,e.y);if(next!=last)sk.add_line(last,next);last=next;}
        else if(e.type==QPainterPath::CurveToElement && i+2<path.elementCount()) {
          const auto b=path.elementAt(++i),c=path.elementAt(++i);
          SkEntity curve;curve.type=SkEntity::Type::Spline;curve.degree=3;curve.p={last,point(e.x,e.y),point(b.x,b.y),point(c.x,c.y)};
          curve.weights={1,1,1,1};curve.knots={0,1};curve.multiplicities={4,4};curve.id=sk.next_id();last=curve.p.back();sk.entities.push_back(curve);
        }
      }
    });
  }catch(const std::exception& e){emit status(QString::fromUtf8(e.what()));}
}

void SketchEditor::benchPrimitives() {
  try {
    m_viewport->setGridSnap(false);int column=0;
    for(const QString& tool:{"rect3","circle2","polygon_outer","cslot","arcslot","conic","control_spline"}) {
      setTool(tool);const size_t before=m_sk.entities.size();const double x=column++*55;
      placePrecise(QString::number(x),"0",0);placePrecise(QString::number(x+20),"0",0);
      if(tool=="control_spline") {placePrecise(QString::number(x+10),"20",0);placePrecise(QString::number(x+30),"5",0);applyTool();}
      else if(tool=="conic")placePrecise(QString::number(x+15),"20",0);
      else if(tool=="arcslot")placePrecise(QString::number(x),"20",0);
      else if(tool!="circle2" && tool!="polygon_outer")placePrecise(QString::number(x+20),"5",0);
      if(m_sk.entities.size()<=before)throw opad::Error("primitive tool did not create geometry: "+tool.toStdString());
    }
    const size_t count=m_sk.entities.size();
    setTool("text");m_options["text"]="OPAD";m_options["height"]="12 mm";m_panelFieldsDirty=true;emit workflowChanged();
    createText(0,45);
    auto* timer=new QTimer(this);timer->setInterval(50);auto attempts=std::make_shared<int>(0);
    auto block=std::make_shared<size_t>(0);  // entities before the built-in block letters, once they are asked for
    connect(timer,&QTimer::timeout,this,[this,timer,attempts,count,block]{
      if(++*attempts>400){timer->stop();trace::log("bench: primitives text timed out");QCoreApplication::exit(2);return;}
      if(m_editJob)return;
      if(!*block) {
        bool native=false;for(size_t i=count;i<m_sk.entities.size();++i)native|=m_sk.entities[i].degree>0;
        const bool ok=m_sk.entities.size()>count && native;
        for(auto* panel:m_viewport->window()->findChildren<SketchPanel*>())panel->grab().save(qEnvironmentVariable("OPAD_BENCH_SKETCH_PRIMITIVES")+".panel.png");
        trace::log(QString("bench: advanced primitives and native text outlines %1").arg(ok?"PASS":"FAIL"));
        if(!ok){timer->stop();QCoreApplication::exit(2);return;}
        // TODO 10 B4: the built-in font from core, as closed block letters.
        *block=m_sk.entities.size();m_options["textStyle"]="block";createText(0,70);return;
      }
      timer->stop();
      Sketch letters;for(size_t i=*block;i<m_sk.entities.size();++i){const auto& e=m_sk.entities[i];for(int p:e.p)if(!letters.point(p))letters.points.push_back(*m_sk.point(p));letters.entities.push_back(e);}
      const size_t regions=sketch_regions(letters,opad::Frame{}).size();
      const bool ok=m_sk.entities.size()>*block && regions==8;  // O, P, A, D and their four holes
      trace::log(QString("bench: built-in block letters, %1 regions %2").arg(regions).arg(ok?"PASS":"FAIL"));QCoreApplication::exit(ok?0:2);
    });timer->start();
  }catch(const std::exception& e){trace::log(QString("bench: advanced primitives FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}
}
