#include "MainWindow.hpp"
#include "opad/agent.hpp"
#include "opad/geometry.hpp"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include "AnnotationEditor.hpp"
#include "GuidedTool.hpp"

// OPAD_BENCH_NOTES=<prefix>: the note and hand drawing editors driven through their input paths (clicks, strokes,
// keys, pickers), then cards, filters, Delete, Undo/Redo, Cancel and save/reopen. Screenshots at <prefix>.*.png.
bool MainWindow::benchTodo9() {
  const auto prefix=qEnvironmentVariable("OPAD_BENCH_NOTES");if(prefix.isEmpty())return false;
  struct State {int phase=0,ticks=0,wait=0;size_t ops=0;std::string id;QPoint position;opad::json firstPlane,camera;};auto state=std::make_shared<State>();
  auto* timer=new QTimer(this);timer->setInterval(400);
  connect(timer,&QTimer::timeout,this,[this,state,timer,prefix] {try {
    if(++state->ticks>200)throw opad::Error("annotation workflow timeout");
    if(m_doc->designBusy || m_doc->snapshotBusy())return;
    auto require=[](bool ok,const char* why){if(!ok)throw opad::Error(why);};
    auto mouse=[](QWidget* widget,QEvent::Type type,QPointF at,Qt::MouseButton button,Qt::MouseButtons buttons){QMouseEvent e(type,at,widget->mapToGlobal(at.toPoint()),button,buttons,Qt::NoModifier);QApplication::sendEvent(widget,&e);};
    auto click=[&](QPoint at){mouse(m_viewport,QEvent::MouseButtonPress,at,Qt::LeftButton,Qt::LeftButton);mouse(m_viewport,QEvent::MouseButtonRelease,at,Qt::LeftButton,Qt::NoButton);};
    auto stroke=[&](QPoint at,QEvent::Type press=QEvent::MouseButtonPress){mouse(m_viewport,press,at,Qt::LeftButton,Qt::LeftButton);for(int i=1;i<=12;++i)mouse(m_viewport,QEvent::MouseMove,at+QPoint(i*4,i*2),Qt::NoButton,Qt::LeftButton);mouse(m_viewport,QEvent::MouseButtonRelease,at+QPoint(50,25),Qt::LeftButton,Qt::NoButton);};
    auto key=[&](QWidget* widget,int k,Qt::KeyboardModifiers modifiers=Qt::NoModifier){QKeyEvent over(QEvent::ShortcutOverride,k,modifiers);QApplication::sendEvent(widget,&over);QKeyEvent press(QEvent::KeyPress,k,modifiers);QApplication::sendEvent(widget,&press);};
    // The view with its native overlays (prompt, badge) and the floating panel where they are on screen.
    auto capture=[&](const QString& suffix){
      auto image=m_viewport->grabImage();image.setDevicePixelRatio(m_viewport->devicePixelRatioF());QPainter painter(&image);
      for(auto* widget:m_viewport->findChildren<QWidget*>(QString(),Qt::FindDirectChildrenOnly))if(widget->isVisible()&&widget->objectName().startsWith("annotation"))painter.drawPixmap(widget->pos(),widget->grab());
      if(m_annotationPanel->isVisible())painter.drawPixmap(m_viewport->mapFromGlobal(m_annotationPanel->pos()),m_annotationPanel->grab());
      painter.end();image.save(prefix+suffix);};
    auto camera=[&](const opad::json& eye,const opad::json& up){m_viewport->setCameraJson({{"eye",eye},{"target",{0,0,0}},{"up",up},{"scale",100},{"projection","orthographic"},{"absolute",true}});};
    auto panelChild=[&](const char* name){auto* w=m_annotationPanel->findChild<QWidget*>(name);require(w,name);return w;};
    auto choose=[&](const QString& name){auto* button=m_annotationPanel->findChild<QToolButton*>(name);require(button && button->isVisible(),"picker exists");button->click();};
    auto strokes=[&]{return static_cast<QLabel*>(panelChild("annotationStrokeCount"))->text();};
    switch(state->phase++) {
      case 0:
        m_doc->newDocument();m_doc->run("feature",{{"kind","box"},{"inputs",{{"length",60},{"width",40},{"height",10}}}});
        camera({0,0,100},{0,1,0});action("select.faces")->trigger();break;
      case 1:
        require(action("annotate.draw")->shortcut()==QKeySequence("Shift+N") && action("annotate.show")->shortcut().isEmpty(),"Shift+N assigned to drawing only");
        action("annotate.draw")->trigger();require(m_annotationEditor && m_doc->annotationEditing && m_annotationEditor->drawingMode(),"drawing editor starts");
        require(m_annotationPanel->isVisible() && m_annotationPanel->isWindow() && action("annotate.draw")->isChecked(),"floating tool panel and checked command");
        require(m_viewport->findChild<PromptBar*>("annotationPrompt")->isVisible(),"interactive guide visible");break;
      case 2:{
        require(m_annotationPanel->findChildren<QComboBox*>().empty(),"visual pickers replace dropdowns");
        auto* save=static_cast<QPushButton*>(panelChild("annotationSave"));require(!save->isEnabled(),"anchor required");
        choose("annotationType_ai_agent");
        static_cast<QPlainTextEdit*>(panelChild("annotationText"))->setPlainText("Round the marked corner to radius 2 mm");
        click(QPoint(12,m_viewport->height()-12));require(!m_annotationEditor->anchored() && !save->isEnabled(),"empty space does not anchor");
        const QPoint at=m_viewport->widgetPoint({0,0,10});
        click(at);require(m_annotationEditor->anchored() && !save->isEnabled() && strokes()=="STROKES · 0","target pick creates no stroke");
        require(m_annotationEditor->target().kind==opad::Ref::Kind::Face && std::abs(m_annotationEditor->target().point[2]-10)<1e-6,"face picked where clicked");
        require(!m_viewport->annotationTargetRect().isNull() && m_viewport->selection().empty(),"target highlighted, not selected");
        auto* badge=m_viewport->findChild<QFrame*>("annotationAnchorBadge");require(badge && badge->isVisible(),"badge visible");
        require(badge->findChildren<QLabel*>().back()->text().startsWith("Plane · face"),"plane badge names the face");
        state->firstPlane=m_viewport->annotationCameraPlane({0,0,10}).to_json();stroke(at+QPoint(-85,-35));
        require(strokes()=="STROKES · 1" && save->isEnabled(),"first stroke enables Save");
        camera({70,-80,100},{0,0,1});
        const QPoint after=m_viewport->widgetPoint({0,0,10});
        choose("annotationColor_green");choose("annotationWidth_8");stroke(after+QPoint(-85,0));
        key(m_viewport,Qt::Key_3);key(m_viewport,Qt::Key_BracketLeft);stroke(after+QPoint(-85,35));  // blue, 8 -> 4 px
        choose("annotationColor_white");choose("annotationWidth_1");stroke(after+QPoint(-85,70));
        require(strokes()=="STROKES · 4","four strokes");
        stroke(after+QPoint(20,70),QEvent::MouseButtonDblClick);require(strokes()=="STROKES · 5","a quick second stroke (double click) still draws");
        m_annotationPanel->findChildren<QToolButton*>("annotationRemoveStroke").back()->click();require(strokes()=="STROKES · 4","individual stroke removal");
        static_cast<QToolButton*>(panelChild("annotationUndo"))->click();require(strokes()=="STROKES · 5","stroke undo");
        static_cast<QToolButton*>(panelChild("annotationRedo"))->click();require(strokes()=="STROKES · 4","stroke redo");
        key(m_viewport,Qt::Key_E);require(m_annotationPanel->findChild<QToolButton*>("annotationEraser")->isChecked(),"E picks the eraser");
        click(after+QPoint(-65,80));require(strokes()=="STROKES · 3","eraser hit removes stroke");
        click(after+QPoint(200,-150));require(strokes()=="STROKES · 3","eraser miss removes nothing");
        key(m_viewport,Qt::Key_Z,Qt::ControlModifier);require(strokes()=="STROKES · 4","Ctrl+Z restores the erased stroke");
        static_cast<QToolButton*>(panelChild("annotationClear"))->click();require(strokes()=="STROKES · 0" && !save->isEnabled(),"clear disables Save");
        m_annotationEditor->undo();require(strokes()=="STROKES · 4","clear undo");
        key(m_viewport,Qt::Key_B);require(m_annotationPanel->findChild<QToolButton*>("annotationPen")->isChecked(),"B picks the pen");
        break;
      }
      case 3:{  // the cube is picked where the last frame drew it; a hidden bench window has none of its own
        m_viewport->grabImage();
        int first=0,last=-1;  // where the cube is drawn at this scale: scan down its axis
        for(int dy=-100;dy<=100;dy+=4)if(m_viewport->cubeAt(QPointF(m_viewport->width()-100,104+dy))){if(last<first)first=dy;last=dy;}
        const QPoint cube(m_viewport->width()-100,104+(first+last)/2);
        require(last>=first && m_viewport->cubeAt(cube),"view cube found");
        state->camera=m_viewport->cameraJson();click(cube);require(strokes()=="STROKES · 4","a click on the view cube draws nothing");
        break;
      }
      case 4:
        if(m_viewport->cameraJson()==state->camera && ++state->wait<5){--state->phase;break;}
        trace::log(QString("bench: TODO9 view cube click %1 the camera").arg(m_viewport->cameraJson()==state->camera?"did not change":"changed"));
        camera({70,-80,100},{0,0,1});
        m_annotationPanel->grab().save(prefix+".drawing-panel.png");capture(".drawing-editor.png");
        static_cast<QPushButton*>(panelChild("annotationSave"))->click();break;
      case 5:{
        require(!m_doc->annotationEditing && m_doc->scene.annotations.size()==1 && !m_annotationPanel->isVisible(),"one saved drawing, editor closed");
        require(!action("annotate.draw")->isChecked(),"command unchecked after Save");
        const auto& a=m_doc->scene.annotations.front();state->id=a.id;
        require(a.anchor.kind==opad::Ref::Kind::Face,"first click retains face anchor");
        require(a.style=="ai_agent" && a.text=="Round the marked corner to radius 2 mm","type and request saved");
        const auto& s=a.drawing["strokes"];
        require(s.size()==4 && s[0]["color"]=="red" && s[1]["color"]=="green" && s[2]["color"]=="blue" && s[3]["color"]=="white","four colors retained");
        require(s[0]["plane"]["x"]==state->firstPlane["x"] && s[0]["plane"]["y"]==state->firstPlane["y"],"earlier stroke retains original world plane");
        require(s[1]["plane"]!=state->firstPlane,"orbit changes next stroke plane");
        const auto expected=m_viewport->annotationCameraPlane({0,0,10}).to_json();
        require(s[1]["plane"]["x"]==expected["x"] && s[1]["plane"]["y"]==expected["y"],"new stroke perpendicular to current camera");
        require(s[1]["width"]==8 && s[2]["width"]==4 && s[3]["width"]==1,"new stroke widths retained");
        require(std::abs(a.drawing["plane"]["origin"][2].get<double>()-10)<1e-5,"plane passes through clicked face");
        auto* card=m_viewport->findChild<NoteCard*>();require(card,"note card exists");
        state->ops=m_doc->doc.ops.size();const auto before=m_doc->doc.serialize();
        auto* handle=card->findChild<QLabel*>("noteDragHandle");require(handle,"drag handle exists");
        const QPoint old=card->pos();
        mouse(handle,QEvent::MouseButtonPress,QPoint(20,8),Qt::LeftButton,Qt::LeftButton);
        mouse(handle,QEvent::MouseMove,QPoint(90,68),Qt::NoButton,Qt::LeftButton);
        mouse(handle,QEvent::MouseButtonRelease,QPoint(20,8),Qt::LeftButton,Qt::NoButton);
        require(card->pos()!=old,"card moved by mouse dragging");state->position=card->pos();
        require(m_doc->doc.serialize()==before && m_doc->doc.ops.size()==state->ops,"drag does not change document");
        m_doc->refresh();break;
      }
      case 6:{
        auto* card=m_viewport->findChild<NoteCard*>();require(card && card->pos()==state->position,"position survives scene refresh");
        auto* filter=m_annotations->findChild<QComboBox*>("annotationTypeFilter");filter->setCurrentIndex(filter->findData("issue"));break;
      }
      case 7:{
        require(m_viewport->findChildren<NoteCard*>().empty(),"type filter hides viewport cards");
        auto* filter=m_annotations->findChild<QComboBox*>("annotationTypeFilter");filter->setCurrentIndex(filter->findData("ai_agent"));break;
      }
      case 8:{
        require(m_viewport->findChildren<NoteCard*>().size()==1,"AI filter shows drawing");
        m_doc->saveAs(prefix+".opad");m_viewport->grabImage().save(prefix+".png");grab().save(prefix+".ui.png");
        break;
      }
      case 9: {auto* card=m_viewport->findChild<NoteCard*>();card->findChild<QPushButton*>("deleteNote")->click();break;}
      case 10:
        require(m_doc->scene.annotations.empty(),"Delete removes drawing");m_doc->undo();break;
      case 11:
        require(m_doc->scene.annotations.size()==1,"Undo restores drawing");m_doc->redo();break;
      case 12:
        require(m_doc->scene.annotations.empty(),"Redo deletes drawing");m_doc->undo();
        action("annotate.draw")->trigger();require(m_annotationEditor,"second drawing starts");state->ops=m_doc->doc.ops.size();
        click(m_viewport->widgetPoint({0,0,10}));stroke(m_viewport->widgetPoint({0,0,10}));
        key(m_viewport,Qt::Key_Escape);require(!m_annotationEditor || !m_annotationPanel->isVisible(),"Esc cancels");break;
      case 13:
        require(!m_doc->annotationEditing && m_doc->doc.ops.size()==state->ops && !m_annotationEditor,"Cancel creates no operation");
        m_doc->open(prefix+".opad");require(m_doc->scene.annotations.size()==1,"save/reopen retains drawing");
        require(opad::agent::context(m_doc->doc,m_doc->scene,opad::json::object())["ai_agent_notes"]==1,"agent sees queued note");
        camera({0,0,100},{0,1,0});
        action("annotate.add")->trigger();require(m_annotationEditor && !m_annotationEditor->drawingMode() && action("annotate.add")->isChecked(),"text note uses guided editor");break;
      case 14:{
        require(m_annotationPanel->findChild<QWidget*>("annotationDrawingEditor")==nullptr || !m_annotationPanel->findChild<QWidget*>("annotationDrawingEditor")->isVisible(),"one editor in the panel");
        auto* save=static_cast<QPushButton*>(panelChild("annotationSave"));require(!save->isEnabled(),"empty note cannot save");
        m_annotationPanel->grab().save(prefix+".note-empty-panel.png");
        auto* text=static_cast<QPlainTextEdit*>(panelChild("annotationText"));
        text->setPlainText("Inspect this face");require(!save->isEnabled(),"typed note needs anchor");
        click(m_viewport->widgetPoint({0,0,10}));require(save->isEnabled(),"anchor and text enables note Save");
        require(m_viewport->findChild<QFrame*>("annotationAnchorBadge")->findChildren<QLabel*>().back()->text()=="Note · face 5","note badge");
        m_annotationPanel->grab().save(prefix+".note-panel.png");capture(".note-editor.png");
        key(text,Qt::Key_Return,Qt::ControlModifier);break;
      }
      case 15:
        require(m_doc->scene.annotations.size()==2 && !m_doc->annotationEditing,"Ctrl+Enter saves text note");
        require(m_doc->scene.annotations.back().drawing.is_null() && m_doc->scene.annotations.back().text=="Inspect this face","text note has no drawing");
        // Selected first, command second: the one selected face is the target straight away.
        {opad::Ref face;face.body=m_doc->scene.annotations.back().anchor.body;face.kind=opad::Ref::Kind::Face;face.index=0;m_viewport->selectRefs({face});}
        action("annotate.add")->trigger();
        require(m_annotationEditor && m_annotationEditor->anchored() && m_annotationEditor->target().kind==opad::Ref::Kind::Face && m_annotationEditor->target().index==0,"a selected face is adopted");
        break;
      case 16:  // the old selection is cleared in a sliced job: a frame later
        require(m_annotationEditor && m_annotationEditor->anchored() && m_viewport->selection().empty(),"adopted target is highlighted, not selected");
        m_annotationEditor->cancel();action("select.bodies")->trigger();break;
      case 17:case 21:case 24:
        action("annotate.add")->trigger();break;
      case 18:case 22:case 25:{
        const int phase=state->phase-1;
        const auto point=phase==18?opad::Vec3{0,0,10}:phase==22?opad::Vec3{0,-20,10}:opad::Vec3{-30,-20,10};
        if(phase==18){click(m_viewport->widgetPoint({25,15,10}));require(m_annotationEditor->anchored(),"first pick");}  // then picked again: the note moves
        click(m_viewport->widgetPoint(point));
        static_cast<QPlainTextEdit*>(panelChild("annotationText"))->setPlainText("Object anchor test");
        require(!m_viewport->annotationTargetRect().isNull(),"object outline visible");
        auto* save=static_cast<QPushButton*>(panelChild("annotationSave"));require(save->isEnabled(),"body/edge/vertex anchor allows Save");
        if(phase==18)break;  // a body's tint may come from a worker: shown a tick later
        capture(phase==22?".edge-target.png":".vertex-target.png");
        save->click();break;
      }
      case 19:
        capture(".body-target.png");static_cast<QPushButton*>(panelChild("annotationSave"))->click();break;
      case 20:require(m_doc->scene.annotations.back().anchor.kind==opad::Ref::Kind::Body,"body anchor retained");action("select.edges")->trigger();break;
      case 23:
        require(m_doc->scene.annotations.back().anchor.kind==opad::Ref::Kind::Edge,"edge anchor retained");action("select.vertices")->trigger();break;
      case 26:
        require(m_doc->scene.annotations.back().anchor.kind==opad::Ref::Kind::Vertex,"vertex anchor retained");
        trace::log("bench: TODO9 multi-plane drawing, pickers, keys, history, guides, four anchor kinds, text notes and reopen PASS");timer->stop();QCoreApplication::exit(0);break;
    }
  } catch(const std::exception& e){trace::log(QString("bench: TODO9 FAIL: %1 (phase %2)").arg(e.what()).arg(state->phase-1));timer->stop();QCoreApplication::exit(2);}});
  timer->start();return true;
}

// OPAD_BENCH_ANNOTATE=1 with --bench-select, meant for the Engine: the drawing and note editors on the heaviest body
// (or whatever body is under the view), every input handler timed on its own ("bench: annotate: <step> N ms"), so
// the trace's stall lines belong to the editor and not to this driver batching events.
bool MainWindow::benchAnnotateLarge() {
  if(!qEnvironmentVariableIsSet("OPAD_BENCH_ANNOTATE"))return false;
  struct State {int phase=0,ticks=0,remaining=-1,settled=0;std::string heavy;QPoint at;bool filtered=true;};auto state=std::make_shared<State>();
  auto waitFilter=[this,state]{
    state->filtered=false;auto once=std::make_shared<QMetaObject::Connection>();
    *once=connect(m_viewport,&Viewport::filterApplied,this,[state,once]{disconnect(*once);state->filtered=true;});};
  connect(m_viewport,&Viewport::meshingProgress,this,[state](int remaining){state->remaining=remaining;state->settled=0;});
  auto* timer=new QTimer(this);timer->setInterval(300);
  connect(timer,&QTimer::timeout,this,[this,state,timer,waitFilter] {try {
    if(++state->ticks>600)throw opad::Error("annotation bench timeout");
    if(!state->filtered || m_doc->designBusy)return;
    auto require=[](bool ok,const char* why){if(!ok)throw opad::Error(why);};
    auto timed=[](const QString& step,const std::function<void()>& f){QElapsedTimer t;t.start();f();trace::log(QString("bench: annotate: %1 %2 ms").arg(step).arg(t.elapsed()));};
    auto mouse=[this](QEvent::Type type,QPointF at,Qt::MouseButton button,Qt::MouseButtons buttons){QMouseEvent e(type,at,m_viewport->mapToGlobal(at.toPoint()),button,buttons,Qt::NoModifier);QApplication::sendEvent(m_viewport,&e);};
    auto click=[&](QPoint at){mouse(QEvent::MouseButtonPress,at,Qt::LeftButton,Qt::LeftButton);mouse(QEvent::MouseButtonRelease,at,Qt::LeftButton,Qt::NoButton);};
    auto stroke=[&](QPoint at,int n){
      timed(QString("stroke %1 press").arg(n),[&]{mouse(QEvent::MouseButtonPress,at,Qt::LeftButton,Qt::LeftButton);});
      QElapsedTimer t;t.start();qint64 worst=0;
      for(int i=1;i<=40;++i){QElapsedTimer m;m.start();mouse(QEvent::MouseMove,at+QPoint(i*3,int(20*std::sin(i*0.3))),Qt::NoButton,Qt::LeftButton);worst=std::max(worst,m.elapsed());}
      trace::log(QString("bench: annotate: stroke %1 40 moves %2 ms (worst %3 ms)").arg(n).arg(t.elapsed()).arg(worst));
      timed(QString("stroke %1 release").arg(n),[&]{mouse(QEvent::MouseButtonRelease,at+QPoint(120,0),Qt::LeftButton,Qt::NoButton);});};
    auto key=[&](int k,Qt::KeyboardModifiers modifiers=Qt::NoModifier){QKeyEvent over(QEvent::ShortcutOverride,k,modifiers);QApplication::sendEvent(m_viewport,&over);QKeyEvent press(QEvent::KeyPress,k,modifiers);QApplication::sendEvent(m_viewport,&press);};
    switch(state->phase++) {
      case 0:{  // the load job ends before its bodies are meshed and displayed
        const auto bodies=m_doc->scene.all_bodies();
        if(std::none_of(bodies.begin(),bodies.end(),[this](const std::string& id){return m_doc->scene.effectively_visible(id);})) {
          trace::log("bench: annotate: nothing visible, unhiding (in memory, never saved)");
          std::vector<std::string> hidden;  // collected first: every run() rebuilds the scene being iterated
          for(const auto& [id,node]:m_doc->scene.nodes)if(!node.visible)hidden.push_back(id);
          for(const auto& id:hidden)m_doc->run("appearance",{{"target",id},{"visible",true}});
          state->remaining=-1;--state->phase;break;
        }
        if(state->remaining!=0 || ++state->settled<4){--state->phase;break;}
        waitFilter();action("select.faces")->trigger();break;
      }
      case 1:{
        state->heavy=m_viewport->benchHeaviest();require(!state->heavy.empty(),"a body");
        m_viewport->grabImage();  // a frame: the picker clips to the camera's z range
        const auto box=opad::node_world_bbox(m_doc->doc,m_doc->scene,state->heavy);
        const gp_XYZ c=(box.CornerMin().XYZ()+box.CornerMax().XYZ())/2;
        bool found=false;  // the nearest point over the heaviest body, else over any body near the view centre
        for(const QPoint centre:{m_viewport->widgetPoint({c.X(),c.Y(),c.Z()}),m_viewport->rect().center()})
          for(int r=0;r<=300 && !found;r+=6)for(int k=0;k<(r?16:1) && !found;++k) {
            const QPoint p=centre+QPoint(int(r*std::cos(k*M_PI/8)),int(r*std::sin(k*M_PI/8)));
            opad::Ref ref;
            if(m_viewport->rect().adjusted(40,120,-400,-40).contains(p) && m_viewport->referenceAt(p,ref) && (ref.body==state->heavy || centre==m_viewport->rect().center())){state->at=p;state->heavy=ref.body;found=true;}
          }
        require(found,"a body face on screen");
        trace::log(QString("bench: annotate: target body %1 at %2,%3").arg(m_doc->nodeName(state->heavy)).arg(state->at.x()).arg(state->at.y()));break;
      }
      case 2:timed("open drawing editor",[&]{action("annotate.draw")->trigger();});require(m_annotationEditor && m_annotationEditor->drawingMode(),"drawing editor");break;
      case 3:timed("pick the target",[&]{click(state->at);});require(m_annotationEditor->anchored(),"anchored");break;
      case 4:stroke(state->at+QPoint(-60,-40),1);break;
      case 5:stroke(state->at+QPoint(-60,0),2);break;
      case 6:timed("orbit (front view)",[&]{m_viewport->standardView("front");});break;
      case 7:m_viewport->grabImage();stroke(state->at+QPoint(-60,40),3);break;
      case 8:key(Qt::Key_E);timed("erase",[&]{click(state->at+QPoint(-60+30,40+int(20*std::sin(10*0.3))));});key(Qt::Key_B);
        timed("undo",[&]{key(Qt::Key_Z,Qt::ControlModifier);});break;
      case 9:timed("save the drawing",[&]{m_annotationPanel->findChild<QPushButton*>("annotationSave")->click();});require(!m_annotationEditor || !m_annotationPanel->isVisible(),"saved");
        waitFilter();action("select.bodies")->trigger();break;
      case 10:timed("open note editor",[&]{action("annotate.add")->trigger();});break;
      case 11:m_viewport->grabImage();timed("pick a body target",[&]{click(state->at);});require(m_annotationEditor && m_annotationEditor->anchored(),"body anchored");break;
      case 12:break;  // the body's tint may come from a worker
      case 13:timed("cancel the note",[&]{key(Qt::Key_Escape);});break;
      case 14:trace::log("bench: annotate large PASS");timer->stop();QCoreApplication::exit(0);break;
    }
  } catch(const std::exception& e){trace::log(QString("bench: annotate large FAIL: %1 (phase %2)").arg(e.what()).arg(state->phase-1));timer->stop();QCoreApplication::exit(2);}});
  timer->start();return true;
}
