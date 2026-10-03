#include "MainWindow.hpp"
#include "opad/geometry.hpp"
#include "opad/recognize.hpp"
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <QApplication>
#include <QKeyEvent>
#include <QStatusBar>

// OPAD_BENCH_SMART=<prefix> (TODO 11 UI-97), on an imported plate with four through holes Ø6 and a blind Ø6: Select
// similar from one hole wall selects the four walls, again the next rule (every inside R3 face, the blind one too);
// Remove faces started on the four shows its preview and commits through the panel path (Enter in the view) as one
// feature op that takes the holes away; Undo brings them back. Shots at <prefix>.similar.png / .preview.png / .panel.png.
bool MainWindow::benchSmart() {
  const auto prefix=qEnvironmentVariable("OPAD_BENCH_SMART");if(prefix.isEmpty())return false;
  struct State {int phase=0,ticks=0,wait=0;std::string body;opad::Ref wall;std::vector<opad::Ref> four;int faces=0;size_t ops=0;double volume=0;};
  auto state=std::make_shared<State>();
  auto* timer=new QTimer(this);timer->setInterval(200);
  connect(timer,&QTimer::timeout,this,[this,state,timer,prefix] {try {
    if(++state->ticks>600)throw opad::Error("timed out in phase "+std::to_string(state->phase));
    if(m_doc->loading || m_doc->designBusy || m_doc->snapshotBusy() || m_jobs->busy())return;
    auto require=[](bool ok,const std::string& why){if(!ok)throw opad::Error(why);};
    auto body=[&]{return opad::node_world_shape(m_doc->doc,m_doc->scene,state->body);};
    auto volume=[](const TopoDS_Shape& s){GProp_GProps g;BRepGProp::VolumeProperties(s,g);return g.Mass();};
    auto same=[](std::vector<opad::Ref> a,std::vector<opad::Ref> b){
      auto key=[](const opad::Ref& r){return r.body+"/"+std::to_string(int(r.kind))+"/"+std::to_string(r.index);};
      std::vector<std::string> x,y;for(const auto& r:a)x.push_back(key(r));for(const auto& r:b)y.push_back(key(r));
      std::sort(x.begin(),x.end());std::sort(y.begin(),y.end());return x==y;};
    switch(state->phase) {
      case 0: {
        require(m_doc->scene.all_bodies().size()==1,"one imported body");
        state->body=m_doc->scene.all_bodies().front();
        // What the bench expects, read the same way the app does (a small plate: fine on the UI thread here).
        opad::Recognizer recognizer(body());
        for(const auto& h:recognizer.all("hole"))
          if(h.params["through"]==true && std::abs(h.params["diameter"].get<double>()-6)<1e-6)
            for(int f:h.faces){opad::Ref r;r.body=state->body;r.kind=opad::Ref::Kind::Face;r.index=f;state->four.push_back(r);}
        require(state->four.size()==4,"four through hole walls recognised");
        state->wall=state->four.front();
        state->faces=recognizer.face_count();state->volume=volume(body());state->ops=m_doc->doc.ops.size();
        setWorkspace(1);action("select.faces")->trigger();
        break;
      }
      case 1:
        if(m_viewport->selectionFilter()!=Viewport::SelFilter::Face)return;
        m_viewport->selectRefs({state->wall});
        break;
      case 2:
        require(m_viewport->selection().size()==1,"one hole wall picked");
        action("select.similar")->trigger();
        break;
      case 3:
        require(m_similar.rules.size()==2,"two rules for a through hole wall: its holes, then every inside R3 face");
        require(same(m_viewport->selection(),state->four),"Select similar picked the four through holes");
        require(statusBar()->currentMessage().contains("4 selected"),"the status bar says what was selected");
        trace::log("bench: smart: Select similar from one wall selected the four through holes, status \""+statusBar()->currentMessage()+"\" PASS");
        m_ribbon->setCurrentTab(1);grab().save(prefix+".similar.png");  // Design > Modify, with Remove faces
        action("select.similar")->trigger();  // again: the next rule
        break;
      case 4:
        require(m_viewport->selection().size()==5 && m_similar.current==1,"again: every inside R3 face, the blind hole too");
        trace::log("bench: smart: Select similar again selected the next rule (5 faces) PASS");
        m_viewport->selectRefs(state->four);
        break;
      case 5:
        require(same(m_viewport->selection(),state->four),"the four walls picked again");
        action("design.remove_faces")->trigger();
        require(m_design->featureActive() && m_featurePanel->isVisible(),"Remove faces opens its panel");
        break;
      case 6:
        if(m_viewport->previewBodyCount()==0){require(++state->wait<50,"Remove faces shows a preview");return;}
        trace::log("bench: smart: Remove faces adopted the four picks and shows its preview PASS");
        m_viewport->grabImage().save(prefix+".preview.png");m_featurePanel->grab().save(prefix+".panel.png");
        {QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(m_viewport,&enter);}  // OK, as a user in the view
        break;
      case 7: {
        if(m_design->featureActive())return;
        require(m_doc->doc.ops.size()>state->ops,"the feature was appended");
        const opad::Feature* f=m_doc->scene.features.empty()?nullptr:&m_doc->scene.features.back();
        require(f && f->kind=="remove_faces","the last feature is Remove faces");
        const TopoDS_Shape now=body();const int faces=opad::subshape_count(now,opad::Ref::Kind::Face);
        require(faces==state->faces-4,"the four walls are gone ("+std::to_string(faces)+" faces)");
        require(std::abs(volume(now)-state->volume-4*M_PI*9*10)<1e-3,"the holes are filled");
        trace::log(QString("bench: smart: Remove faces committed one op, %1 -> %2 faces, holes filled PASS").arg(state->faces).arg(faces));
        m_doc->undo();
        break;
      }
      case 8: {
        require(opad::subshape_count(body(),opad::Ref::Kind::Face)==state->faces && m_doc->doc.ops.size()==state->ops,"Undo brings the holes back");
        trace::log("bench: smart: Undo restored the holes PASS");
        timer->stop();QCoreApplication::exit(0);return;
      }
    }
    ++state->phase;
  }catch(const std::exception& e){timer->stop();trace::log(QString("bench: smart FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});
  timer->start();
  return true;
}
