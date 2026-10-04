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
// The clicks as picks; an arc slot runs counter-clockwise from its second pick to its third, so a sweep typed negative
// (clockwise) swaps them.
std::vector<std::pair<double,double>> picksOf(const QString& kind,const std::vector<std::pair<double,double>>& clicks,double sweep) {
  auto picks=clicks;
  if(kind=="arcslot" && picks.size()==3 && sweep<0)std::swap(picks[1],picks[2]);
  return picks;
}
}

bool SketchEditor::primitiveClick(const Snap& s) {
  const double u=s.u,v=s.v;
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
  m_clicks.push_back(s);  // with its typed values
  if(m_tool!="control_spline" && int(m_clicks.size())>=(m_tool=="tangent_circle"?1:picksNeeded(m_tool)))finishPrimitive();
  toolPrompt();rebuild();return true;
}

void SketchEditor::finishPrimitive() {
  if(m_clicks.empty())return;
  try {
    auto options=primitiveOptions();
    std::vector<std::pair<double,double>> picks;for(const auto& p:m_clicks)picks.push_back({p.u,p.v});
    // What each click snapped to stays (UI-21): the point it landed on is the primitive's point there, the curve it lies on
    // and what else held it (a midpoint, a tracked point's level) are kept as constraints.
    opad::json snaps=opad::json::array();bool snapped=false;
    for(const auto& c:m_clicks) {
      opad::json snap,holds=opad::json::array();
      if(c.entity)holds.push_back(opad::json::array({"coincident",c.entity}));
      for(const auto& h:c.holds)holds.push_back(opad::json::array({SkConstraint::type_name(h.type),h.ref}));
      if(c.point)snap={{"point",c.point}};else if(!holds.empty())snap={{"holds",holds}};
      snapped|=!snap.is_null();snaps.push_back(snap);
    }
    if(snapped)options["snaps"]=snaps;
    const auto sweep=m_clicks.back().typed.find("sweep");
    begin_change();const auto made=create_primitive(m_sk,m_tool.toStdString(),picksOf(m_tool,picks,sweep==m_clicks.back().typed.end()?0:sweep->second.first),options);
    // The typed sizes kept as dimensions (UI-17), on what create_primitive made, in its order.
    const Snap& second=m_clicks[std::min<size_t>(1,m_clicks.size()-1)];const Snap& last=m_clicks.back();
    auto direction=[&](int a,int b){const auto *p=m_sk.point(a),*q=m_sk.point(b);return std::atan2(q->y-p->y,q->x-p->x);};
    const double px=m_viewport->pixelSize();
    if(m_tool=="rect3" && made.size()>=4) {
      const auto* base=m_sk.entity(made[0]);const auto *a=m_sk.point(base->p[0]),*c=m_sk.point(m_sk.entity(made[1])->p[1]);
      const double mu=(a->x+c->x)/2,mv=(a->y+c->y)/2;
      labelOff(keepTyped(second,"length",SkConstraint::Type::Distance,{made[0]}),made[0],mu,mv,24*px);keepDirection(second,"angle",{made[0]},direction(base->p[0],base->p[1]));
      labelOff(keepTyped(last,"height",SkConstraint::Type::Distance,{made[1]}),made[1],mu,mv,24*px);
      keepAligned(second,{made[0]});  // its base along an axis (UI-23); the third click is a height, square to it
    } else if(m_tool=="circle2" && !made.empty())keepTyped(second,"diameter",SkConstraint::Type::Diameter,{made[0]});
    else if(m_tool=="tangent_arc" && !made.empty())keepTyped(last,"radius",SkConstraint::Type::Radius,{made[0]});
    else if(m_tool=="polygon_outer" && made.size()>=4) {  // across flats: two opposite sides, or (an odd count) the centre to a side
      const size_t sides=made.size()-1;const auto* guide=m_sk.entity(made.back());
      if(sides%2==0)keepTyped(second,"diameter",SkConstraint::Type::Distance,{made[0],made[sides/2]});
      else keepTyped(second,"diameter",SkConstraint::Type::Distance,{guide->p[0],made[0]},0.5);
      Snap across=second;std::swap(across.horizontal,across.vertical);keepAligned(across,{made[sides-1]});  // the side facing the click, square to the way to its middle
    } else if(m_tool=="arcslot" && made.size()>=4) {  // its centre line's radius, its start (and end) along an axis
      const bool swapped=sweep!=m_clicks.back().typed.end() && sweep->second.first<0;
      const int centre=m_sk.entity(made[0])->p[0],start=m_sk.entity(made[swapped?2:3])->p[0],end=m_sk.entity(made[swapped?3:2])->p[0];
      keepTyped(second,"radius",SkConstraint::Type::Distance,{centre,start});
      keepDirection(second,"angle",{centre,start},direction(centre,start));
      keepDirection(last,"sweep",{centre,end},direction(centre,end));
      keepAligned(second,{centre,start});keepAligned(last,{centre,end});
      keepTyped(last,"width",SkConstraint::Type::Diameter,{made[swapped?2:3]});  // its width typed: the start cap's diameter
    } else if(m_tool=="cslot" && made.size()>=4) {
      const int c1=m_sk.entity(made[2])->p[0],c2=m_sk.entity(made[3])->p[0];const auto *p=m_sk.point(c1),*q=m_sk.point(c2);
      const double r=std::hypot(m_sk.point(m_sk.entity(made[2])->p[1])->x-p->x,m_sk.point(m_sk.entity(made[2])->p[1])->y-p->y),l=std::max(1e-12,std::hypot(q->x-p->x,q->y-p->y));
      const double ux=(q->x-p->x)/l,uy=(q->y-p->y)/l;
      labelAt(keepTyped(second,"length",SkConstraint::Type::Distance,{c1,c2}),(p->x+q->x)/2-uy*(r+20*px),(p->y+q->y)/2+ux*(r+20*px));  // above it
      keepDirection(second,"angle",{c1,c2},direction(c1,c2));
      keepAligned(second,{c1,c2});
      labelAt(keepTyped(last,"width",SkConstraint::Type::Distance,{made[0],made[1]}),p->x-ux*(r+30*px),p->y-uy*(r+30*px));  // past the first cap
    }
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
  if(m_tool=="tangent_arc"){options["line"]=m_picked.at(0);options["smooth"]=true;}  // on from the line, past half a turn too
  return options;
}

Sketch SketchEditor::primitivePreview() const {
  Sketch preview;
  if(!variants.contains(m_tool) || m_tool=="text" || m_tool=="tangent_circle" || (m_tool=="tangent_arc" && m_picked.empty()) || m_clicks.empty())return preview;
  std::vector<std::pair<double,double>> picks;for(const auto& p:m_clicks)picks.push_back({p.u,p.v});picks.push_back({m_cursor.u,m_cursor.v});
  if(m_tool!="control_spline" && int(picks.size())<picksNeeded(m_tool))return preview;
  const auto sweep=m_typedValues.find("sweep");
  const SkEntity* line=m_tool=="tangent_arc"?m_sk.entity(m_picked.front()):nullptr;
  if(line){for(int id:line->p)if(const auto* p=m_sk.point(id))preview.points.push_back(*p);preview.entities.push_back(*line);}  // what the arc leaves
  try {create_primitive(preview,m_tool.toStdString(),picksOf(m_tool,picks,sweep==m_typedValues.end()?0:sweep->second),primitiveOptions());}catch(...){return Sketch{};}
  if(line)preview.entities.erase(preview.entities.begin());  // drawn already
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
