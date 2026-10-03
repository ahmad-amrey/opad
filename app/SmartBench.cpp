#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "SmartArea.hpp"
#include "opad/geometry.hpp"
#include "opad/recognize.hpp"
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <QApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolButton>

// OPAD_BENCH_SMART=<prefix> (TODO 11 UI-97, SmartArea; cases in tools/bench_cases/smart.py), on an imported plate with
// four through holes Ø6 and a blind Ø6: Select similar from one hole wall selects the four walls, again the next rule
// (every inside R3 face, the blind one too); the area's places (Remove faces under Offset face's arrow, Select similar in
// Inspect > Results, the Edit menu and the context menu of the picks); Remove faces started on the four shows its preview
// and commits through the panel path (Enter in the view) as one feature op that takes the holes away; Undo brings them
// back. Then the body picked whole: Select similar selects its top perimeter (the view switches to edges), again the
// bottom one, on to the upward faces (back to faces) and all holes: the modal Select by geometry's rules without the
// dialog. Shots at <prefix>.similar.png / .preview.png / .panel.png / .body.png. On the STEP opened in viewer mode Remove
// faces is left out (editing asks to save first).
OPAD_BENCH(OPAD_BENCH_SMART, smart) {
  struct State {int phase=0,ticks=0,wait=0;std::string body;opad::Ref wall;std::vector<opad::Ref> four;int faces=0;size_t ops=0;double volume=0;std::vector<opad::Recognized> rules;};
  auto state=std::make_shared<State>();
  SmartArea* area=nullptr;
  for(AreaController* a:w.m_areas)if(auto* smart=dynamic_cast<SmartArea*>(a))area=smart;
  if(!area){trace::log("bench: smart FAIL: the smart area is off");QCoreApplication::exit(2);return true;}
  auto* timer=new QTimer(&w);timer->setInterval(200);
  QObject::connect(timer,&QTimer::timeout,&w,[&w,area,state,timer,prefix=value] {try {
    if(++state->ticks>600)throw opad::Error("timed out in phase "+std::to_string(state->phase));
    if(w.m_doc->loading || w.m_doc->designBusy || w.m_doc->snapshotBusy() || w.m_jobs->busy())return;
    auto require=[](bool ok,const std::string& why){if(!ok)throw opad::Error(why);};
    auto body=[&]{return opad::node_world_shape(w.m_doc->doc,w.m_doc->scene,state->body);};
    auto volume=[](const TopoDS_Shape& s){GProp_GProps g;BRepGProp::VolumeProperties(s,g);return g.Mass();};
    // The area's context-menu entries for what is picked now: after Properties, before what follows it.
    auto contextEntries=[&](const std::vector<opad::Ref>& picks){
      QMenu menu;QAction* after=menu.addSeparator();menu.insertAction(after,w.action("inspect.properties"));
      SelectionContext c;c.ids={state->body};c.refs=picks;area->contextMenu(c,menu);
      const auto all=menu.actions();return all.back()==after && all.front()==w.action("inspect.properties")?all.mid(1,all.size()-2):QList<QAction*>{nullptr};};
    auto same=[](std::vector<opad::Ref> a,std::vector<opad::Ref> b){
      auto key=[](const opad::Ref& r){return r.body+"/"+std::to_string(int(r.kind))+"/"+std::to_string(r.index);};
      std::vector<std::string> x,y;for(const auto& r:a)x.push_back(key(r));for(const auto& r:b)y.push_back(key(r));
      std::sort(x.begin(),x.end());std::sort(y.begin(),y.end());return x==y;};
    switch(state->phase) {
      case 0: {
        require(w.m_doc->scene.all_bodies().size()==1,"one imported body");
        state->body=w.m_doc->scene.all_bodies().front();
        // What the bench expects, read the same way the app does (a small plate: fine on the UI thread here).
        opad::Recognizer recognizer(body());
        for(const auto& h:recognizer.all("hole"))
          if(h.params["through"]==true && std::abs(h.params["diameter"].get<double>()-6)<1e-6)
            for(int f:h.faces){opad::Ref r;r.body=state->body;r.kind=opad::Ref::Kind::Face;r.index=f;state->four.push_back(r);}
        require(state->four.size()==4,"four through hole walls recognised");
        state->wall=state->four.front();
        state->faces=recognizer.face_count();state->volume=volume(body());state->ops=w.m_doc->doc.ops.size();
        w.setWorkspace("design");w.action("select.faces")->trigger();
        break;
      }
      case 1:
        if(w.m_viewport->selectionFilter()!=Viewport::SelFilter::Face)return;
        w.m_viewport->selectRefs({state->wall});
        break;
      case 2:
        require(w.m_viewport->selection().size()==1,"one hole wall picked");
        w.action("select.similar")->trigger();
        break;
      case 3:
        require(area->similar().rules.size()==2,"two rules for a through hole wall: its holes, then every inside R3 face");
        require(same(w.m_viewport->selection(),state->four),"Select similar picked the four through holes");
        require(w.statusBar()->currentMessage().contains("4 selected"),"the status bar says what was selected");
        trace::log("bench: smart: Select similar from one wall selected the four through holes, status \""+w.statusBar()->currentMessage()+"\" PASS");
        w.m_ribbon->setCurrentTab(1);w.grab().save(prefix+".similar.png");  // Design > Modify, with Remove faces
        {
          QAction* similar=w.action("select.similar");QToolButton* offset=nullptr;bool results=false,edit=false;
          if(RibbonPage* page=w.m_ribbon->page("design.modify"))
            for(RibbonGroup* g:page->groups())for(QToolButton* b:g->buttons())if(b->defaultAction()==w.action("design.offset_face"))offset=b;
          require(offset && offset->popupMode()==QToolButton::MenuButtonPopup && offset->menu() && offset->menu()->actions()==QList<QAction*>{w.action("design.remove_faces")},
                  "Remove faces is under Offset face's arrow");
          if(RibbonPage* page=w.m_ribbon->page("review.inspect"))
            for(RibbonGroup* g:page->groups())results=results || (g->menu()->actions().contains(similar) && g->menu()->actions().indexOf(similar)==g->menu()->actions().indexOf(w.action("inspect.properties"))+1);
          require(results,"Select similar follows Properties in Inspect > Results");
          for(QMenu* m:w.menuBar()->findChildren<QMenu*>()){const auto a=m->actions();const int at=a.indexOf(w.action("edit.selecttouched"));edit=edit || (at>=0 && a.value(at+1)==similar);}
          require(edit,"Select similar follows Select touched in the Edit menu");
          require(contextEntries(w.m_viewport->selection())==QList<QAction*>({similar,w.action("design.remove_faces")}),"the context menu of picked faces offers Select similar and Remove faces after Properties");
          trace::log("bench: smart: Remove faces under Offset face's arrow, Select similar in Inspect > Results, the Edit menu and the context menu PASS");
        }
        w.action("select.similar")->trigger();  // again: the next rule
        break;
      case 4:
        require(w.m_viewport->selection().size()==5 && area->similar().current==1,"again: every inside R3 face, the blind hole too");
        trace::log("bench: smart: Select similar again selected the next rule (5 faces) PASS");
        if(w.m_doc->browse){trace::log("bench: smart: Select similar works on a file opened in viewer mode PASS");w.action("select.bodies")->trigger();state->phase=9;return;}
        w.m_viewport->selectRefs(state->four);
        break;
      case 5:
        require(same(w.m_viewport->selection(),state->four),"the four walls picked again");
        w.action("design.remove_faces")->trigger();
        require(w.m_design->featureActive() && w.m_featurePanel->isVisible(),"Remove faces opens its panel");
        break;
      case 6:
        if(w.m_viewport->previewBodyCount()==0){require(++state->wait<50,"Remove faces shows a preview");return;}
        trace::log("bench: smart: Remove faces adopted the four picks and shows its preview PASS");
        w.m_viewport->grabImage().save(prefix+".preview.png");w.m_featurePanel->grab().save(prefix+".panel.png");
        {QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(w.m_viewport,&enter);}  // OK, as a user in the view
        break;
      case 7: {
        if(w.m_design->featureActive())return;
        require(w.m_doc->doc.ops.size()>state->ops,"the feature was appended");
        const opad::Feature* f=w.m_doc->scene.features.empty()?nullptr:&w.m_doc->scene.features.back();
        require(f && f->kind=="remove_faces","the last feature is Remove faces");
        const TopoDS_Shape now=body();const int faces=opad::subshape_count(now,opad::Ref::Kind::Face);
        require(faces==state->faces-4,"the four walls are gone ("+std::to_string(faces)+" faces)");
        require(std::abs(volume(now)-state->volume-4*M_PI*9*10)<1e-3,"the holes are filled");
        trace::log(QString("bench: smart: Remove faces committed one op, %1 -> %2 faces, holes filled PASS").arg(state->faces).arg(faces));
        w.m_doc->undo();
        break;
      }
      case 8: {
        require(opad::subshape_count(body(),opad::Ref::Kind::Face)==state->faces && w.m_doc->doc.ops.size()==state->ops,"Undo brings the holes back");
        trace::log("bench: smart: Undo restored the holes PASS");
        w.action("select.bodies")->trigger();
        break;
      }
      // The body picked whole: its edges and faces by rule, cycled as the face's rules are.
      case 9:
        if(w.m_viewport->selectionFilter()!=Viewport::SelFilter::Body)return;
        w.m_viewport->selectNodes({state->body});
        break;
      case 10: {
        const auto picks=w.m_viewport->selection();
        require(picks.size()==1 && picks.front().kind==opad::Ref::Kind::Body,"the plate picked whole");
        state->rules=opad::Recognizer(body()).body_rules();
        std::string names;for(const auto& r:state->rules)names+=" "+r.rule;
        require(names==" top bottom x y z circle up holes","the plate's rules:"+names);
        require(state->rules[0].edges.size()==9 && state->rules[1].edges.size()==8 && state->rules[6].faces.size()==2,"top 9 edges (outline, five rims), bottom 8, two upward faces");
        require(contextEntries(picks)==QList<QAction*>{w.action("select.similar")},"the context menu of a body offers Select similar alone");
        w.action("select.similar")->trigger();
        break;
      }
      case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: {
        // Every press: the next rule, in the filter its members need (the first switches the view from bodies to edges).
        const size_t rule=size_t(state->phase-11);const opad::Recognized& r=state->rules[rule];
        auto refs=[&]{std::vector<opad::Ref> v;for(int i:r.faces.empty()?r.edges:r.faces){opad::Ref x;x.body=state->body;x.kind=r.faces.empty()?opad::Ref::Kind::Edge:opad::Ref::Kind::Face;x.index=i;v.push_back(x);}return v;}();
        const auto filter=r.faces.empty()?Viewport::SelFilter::Edge:Viewport::SelFilter::Face;
        if(w.m_viewport->selectionFilter()!=filter || !same(w.m_viewport->selection(),refs)){require(++state->wait<50,"Select similar on the body: rule "+r.rule+" selected in its filter");return;}
        state->wait=0;
        require(area->similar().current==rule && area->similar().rules.size()==state->rules.size(),"the app found the same rules");
        require(w.action(filter==Viewport::SelFilter::Edge?"select.edges":"select.faces")->isChecked(),"the filter chips follow");
        trace::log(QString("bench: smart: body rule %1: %2 %3, status \"%4\" PASS").arg(QString::fromStdString(r.rule)).arg(refs.size()).arg(r.faces.empty()?"edges":"faces").arg(w.statusBar()->currentMessage()));
        if(rule==0){
          require(w.statusBar()->currentMessage().startsWith("Top perimeter · 9 selected · again: Bottom perimeter"),"the status bar names the rule and the next");
          w.m_viewport->grabImage().save(prefix+".body.png");
        }
        if(rule+1==state->rules.size()){timer->stop();QCoreApplication::exit(0);return;}
        w.action("select.similar")->trigger();
        break;
      }
    }
    ++state->phase;
  }catch(const std::exception& e){timer->stop();trace::log(QString("bench: smart FAIL: %1").arg(e.what()));QCoreApplication::exit(2);}});
  timer->start();
  return true;
}
