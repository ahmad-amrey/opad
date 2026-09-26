#include "MainWindow.hpp"
#include "DesignController.hpp"
#include "opad/step_io.hpp"
#include "opad/geometry.hpp"
#include "opad/design/sketch_geom.hpp"
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <cmath>

bool MainWindow::benchTodo5() {
  const auto prefix=qEnvironmentVariable("OPAD_BENCH_PLANES");if(prefix.isEmpty())return false;
  struct State {int phase=0,ticks=0;bool originSet=false;opad::json before,plane;};auto state=std::make_shared<State>();auto* timer=new QTimer(this);timer->setInterval(100);
  connect(timer,&QTimer::timeout,this,[this,state,timer,prefix]{try {
    auto require=[](bool ok,const char* text){if(!ok)throw opad::Error(text);};
    if(++state->ticks>400)throw opad::Error("plane workflow timed out");
    auto* picker=m_design->planePicker();auto* sketch=m_design->sketch();
    switch(state->phase++) {
      case 0:{m_doc->newDocument();const auto c=m_viewport->cameraJson();require(c["target"]==opad::json::array({0.,0.,0.}),"new document Home");opad::import_brep(m_doc->doc,opad::brep_from_shape(BRepPrimAPI_MakeBox(gp_Pnt(-10,-10,-10),20,20,20).Shape()),"Origin blocker");m_doc->refresh();m_viewport->fitAll();break;}
      case 1:QMetaObject::invokeMethod(m_annotations,"addRequested");require(m_annotationEditor && !m_annotationEditor->drawingMode() && !m_annotationEditor->anchored(),"Add note without selection starts guided picking");m_annotationEditor->cancel();m_design->startSketch();break;
      case 2:{auto* tiles=m_viewport->findChild<QWidget*>("planeTiles");require(tiles&&tiles->isVisible(),"visible corner plane widget");require(m_viewport->selectedCandidates().empty(),"no selected origin overlays");tiles->grab().save(prefix+".tiles.png");m_design->planePanel()->grab().save(prefix+".plane-panel.png");QMouseEvent click(QEvent::MouseButtonPress,QPointF(39,37),QPointF(tiles->mapToGlobal(QPoint(39,37))),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(tiles,&click);break;}
      case 3:if(!picker->positioning()){--state->phase;break;}if(!state->originSet){picker->setOrigin(12,13);state->originSet=true;--state->phase;break;}m_design->planePanel()->grab().save(prefix+".origin-panel.png");picker->apply();break;
      case 4:require(sketch->active(),"origin Apply enters sketch");require(std::abs(sketch->plane()["frame"]["origin"][0].get<double>()-12)<1e-8,"numeric origin");sketch->setTool("rect");sketch->placePrecise("0","0",0);sketch->placePrecise("20","10",0);state->before=sketch->geometry();state->plane=sketch->plane();m_design->redefineSketchPlane();picker->choose({{"base","xy"}});break;
      case 5:if(!picker->positioning()){--state->phase;break;}picker->setOrigin(22,33);picker->apply();break;
      case 6:{const auto old=opad::design::Sketch::from_json(state->before),now=opad::design::Sketch::from_json(sketch->geometry());for(const auto& p:old.points){require(now.point(p.id),"stable point ID");require(std::abs(now.point(p.id)->x+10-p.x)<1e-7&&std::abs(now.point(p.id)->y+20-p.y)<1e-7,"origin-only change preserves world geometry");}sketch->undo();auto undone=sketch->geometry(),original=state->before;undone.erase("id_watermark");original.erase("id_watermark");require(undone==original&&sketch->plane()==state->plane,"single replane undo");sketch->redo();m_design->redefineSketchPlane();picker->cancel();require(!m_design->pickingPlane()&&sketch->active(),"cancel restores active sketch");m_design->finishSketch();break;}
      case 7:if(sketch->active()||m_doc->designBusy){--state->phase;break;}m_design->pickSketchPlane([this](opad::json,opad::Frame f){m_viewport->lookAt(f,false,false);});picker->choose({{"base","xz"}});break;
      case 8:if(m_design->pickingPlane()){--state->phase;break;}m_viewport->setTwoDimensional(true);m_design->startSketch();break;
      case 9:if(!picker->positioning()){--state->phase;break;}require(!m_viewport->findChild<QWidget*>("planeTiles")->isVisible(),"2D mode chooses camera plane automatically");picker->apply();break;
      case 10:require(sketch->active(),"automatic 2D sketch");m_design->finishSketch();m_viewport->setTwoDimensional(false);m_design->startFeature("mirror");m_design->featurePanel()->activate("plane");break;
      case 11:require(m_design->featureActive()&&m_design->pickingPlane(),"feature plane picker stays active");picker->choose({{"base","yz"}});break;
      case 12:if(m_design->pickingPlane()){--state->phase;break;}require(m_design->featureActive()&&m_design->featurePanel()->picks("plane").value("base","")=="yz","feature plane accepted");m_design->escape();break;
      case 13:{
        m_design->startSketch();
        const auto body=m_doc->scene.all_bodies().front();
        m_viewport->selectRefs({opad::Ref::from_json({{"body",body},{"kind","face"},{"index",1}})},{});
        picker->selectionChanged();break;
      }
      case 14:{
        if(!picker->positioning()){picker->selectionChanged();--state->phase;break;}
        bool found=false;
        for(double x:{-10.,10.})for(double y:{-10.,10.})for(double z:{-10.,10.}) {
          const auto point=m_viewport->widgetPoint({x,y,z});opad::Ref ref;
          if(found||!m_viewport->referenceAt(point,ref)||ref.kind!=opad::Ref::Kind::Vertex)continue;
          QMouseEvent press(QEvent::MouseButtonPress,point,m_viewport->mapToGlobal(point),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
          QMouseEvent release(QEvent::MouseButtonRelease,point,m_viewport->mapToGlobal(point),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
          QApplication::sendEvent(m_viewport,&press);QApplication::sendEvent(m_viewport,&release);found=true;
        }
        require(found,"vertex snapping uses model picking with no origin plane obstruction");break;
      }
      case 15:
        if(!sketch->active()){picker->apply();--state->phase;break;}
        require(sketch->plane()["support"].contains("face")&&sketch->plane()["origin"].contains("ref"),"face support and snapped origin retain references");
        m_design->finishSketch();break;
      case 16:
        if(sketch->active()||m_doc->designBusy){--state->phase;break;}
        m_design->applyOps({opad::design::make_feature_op("plane","Raised plane",{{"mode","offset"},{"plane",{{"base","xy"}}},{"distance","30 mm"}})},"bench construction plane");break;
      case 17:
        if(m_doc->designBusy){--state->phase;break;}
        require(!m_doc->scene.features.empty(),"construction plane created");m_design->startSketch();m_design->planePanel()->findChild<QCheckBox*>("constructionPlanes")->setChecked(true);break;
      case 18:{
        const auto id=m_doc->scene.features.back().id;
        const auto key=opad::json{{"feature",id}}.dump();m_viewport->selectRefs({}, {key});
        if(m_viewport->selectedCandidates().empty()){--state->phase;break;}
        picker->selectionChanged();break;
      }
      case 19:
        if(!picker->positioning()){--state->phase;break;}
        picker->apply();break;
      case 20:
        require(sketch->active()&&std::abs(sketch->frame().origin[2]-30)<1e-8,"construction plane picked with projected default origin");
        m_design->redefineSketchPlane();picker->cancel();require(sketch->active(),"construction replane cancellation");m_design->finishSketch();break;
      case 21:
        m_doc->newDocument();opad::import_brep(m_doc->doc,opad::brep_from_shape(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(25,15,0),gp_Dir(0,0,1)),8,10).Shape()),"Circle reference");m_doc->refresh();m_viewport->fitAll();break;
      case 22:
        m_viewport->lookAt(opad::design::base_frame("yz"),false,false);m_design->startSketch();
        m_viewport->findChild<QWidget*>("planeTiles")->grab().save(prefix+".edge-on-tiles.png");picker->choose({{"base","xy"}});break;
      case 23:
        if(!picker->positioning()){--state->phase;break;}
        require(!m_design->planePanel()->findChild<QComboBox*>("originPicking"),"origin snapping needs no mode selector");break;
      case 24:{
        bool found=false;
        for(int i=0;i<24&&!found;++i) {
          const double angle=i*2*3.141592653589793/24;
          const auto point=m_viewport->widgetPoint({25+8*std::cos(angle),15+8*std::sin(angle),10});opad::Ref ref;
          if(!m_viewport->originReferenceAt(point,ref))continue;
          QMouseEvent press(QEvent::MouseButtonPress,point,m_viewport->mapToGlobal(point),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
          QMouseEvent release(QEvent::MouseButtonRelease,point,m_viewport->mapToGlobal(point),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
          QApplication::sendEvent(m_viewport,&press);QApplication::sendEvent(m_viewport,&release);found=true;
        }
        if(!found){m_viewport->grabImage().save(prefix+".circle-pick.png");m_design->planePanel()->grab().save(prefix+".circle-panel.png");}
        require(found,"circle edge is pickable in center mode");break;
      }
      case 25:
        if(!sketch->active()){picker->apply();--state->phase;break;}
        require(sketch->plane()["origin"].contains("ref")&&std::abs(sketch->frame().origin[0]-25)<1e-7&&std::abs(sketch->frame().origin[1]-15)<1e-7&&std::abs(sketch->frame().origin[2])<1e-7,"circle center snapped and projected onto plane");
        m_design->redefineSketchPlane();picker->choose({{"base","xy"}});break;
      case 26:{
        if(!picker->positioning()){--state->phase;break;}
        m_viewport->configureGrid(10,100);m_viewport->setGridSnap(true);
        const auto start=m_viewport->widgetPoint({0,0,0}),end=m_viewport->widgetPoint({42,32,0});
        QMouseEvent press(QEvent::MouseButtonPress,start,m_viewport->mapToGlobal(start),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent move(QEvent::MouseMove,end,m_viewport->mapToGlobal(end),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease,end,m_viewport->mapToGlobal(end),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(m_viewport,&press);QApplication::sendEvent(m_viewport,&move);QApplication::sendEvent(m_viewport,&release);picker->apply();break;
      }
      case 27:
        require(sketch->active()&&std::abs(sketch->frame().origin[0]-40)<1e-7&&std::abs(sketch->frame().origin[1]-30)<1e-7,"origin dragging snaps to grid");state->before=m_viewport->cameraJson();m_design->finishSketch();break;
      case 28:
        if(sketch->active()||m_doc->designBusy){--state->phase;break;}
        require(state->before==m_viewport->cameraJson(),"finishing sketch preserves camera");break;
      case 29:{
        m_doc->newDocument();opad::design::Sketch geometry;const auto a=geometry.add_point(0,0),b=geometry.add_point(20,0);geometry.add_line(a,b);
        auto frame=opad::design::base_frame("xz");frame.origin={0,0,30};
        auto plan=opad::design::plan_ops(m_doc->doc,{opad::design::make_sketch_op("Plane source",{{"frame",frame.to_json()}},geometry.to_json())});m_doc->commitPlan(std::move(plan),tr("sketch"));
        state->plane={{"sketch",m_doc->scene.sketches.front().id}};m_viewport->lookAt(frame,true,false);m_viewport->setSelectionFilter(Viewport::SelFilter::Edge);break;
      }
      case 30:m_viewport->isolate({m_doc->scene.sketches.front().id});break;
      case 31:{
        m_viewport->grabImage().save(prefix+".isolated-sketch.png");
        opad::Ref ref;require(m_viewport->referenceAt(m_viewport->widgetPoint({10,0,30}),ref) && ref.body==m_doc->scene.sketches.front().id,"isolated sketch is displayed and pickable");
        m_viewport->isolate({});m_design->pickSketchPlane([state](opad::json plane,opad::Frame frame){state->before=plane;state->plane["resolved"]=frame.to_json();},false);break;
      }
      case 32:{
        const auto id=m_doc->scene.sketches.front().id;const auto point=m_viewport->widgetPoint({10,0,30});
        QMouseEvent hover(QEvent::MouseMove,point,m_viewport->mapToGlobal(point),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(m_viewport,&hover);
        m_viewport->grabImage().save(prefix+".sketch-plane-hover.png");
        m_viewport->selectRefs({}, {opad::json{{"sketch",id}}.dump()});require(!m_viewport->selectedCandidates().empty(),"existing sketch has a plane candidate");picker->selectionChanged();break;
      }
      case 33:{
        if(m_design->pickingPlane()){--state->phase;break;}
        require(state->before.contains("sketch") && std::abs(state->plane.at("resolved").at("origin")[2].get<double>()-30)<1e-8,"sketch plane pick preserves origin");
        m_design->editOp(m_doc->scene.sketches.front().id);m_viewport->standardView("top");m_viewport->setTwoDimensional(true);
        const auto camera=m_viewport->cameraJson();const auto eye=camera.at("eye").get<opad::Vec3>(),target=camera.at("target").get<opad::Vec3>();
        require(std::abs(eye[0]-target[0])<1e-6 && std::abs(eye[2]-target[2])<1e-6 && std::abs(eye[1]-target[1])>1,"2D aligns to the edited sketch plane");
        m_viewport->setTwoDimensional(false);m_design->finishSketch();break;
      }
      default:timer->stop();trace::log("bench: plane workflows, origins, undo/redo, sketch isolation/picking and sketch-aligned 2D PASS");QCoreApplication::exit(0);break;
    }
  }catch(const std::exception& e){timer->stop();trace::log(QString("bench: TODO 5 FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});timer->start();return true;
}
