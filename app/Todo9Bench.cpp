#include "MainWindow.hpp"
#include "opad/agent.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>

bool MainWindow::benchTodo9() {
  const auto prefix=qEnvironmentVariable("OPAD_BENCH_NOTES");if(prefix.isEmpty())return false;
  struct State {int phase=0,ticks=0;size_t ops=0;std::string id;QPoint position;};auto state=std::make_shared<State>();
  auto* timer=new QTimer(this);timer->setInterval(400);
  connect(timer,&QTimer::timeout,this,[this,state,timer,prefix] {try {
    if(++state->ticks>150)throw opad::Error("annotation workflow timeout");
    if(m_doc->designBusy || m_doc->snapshotBusy())return;
    auto require=[](bool ok,const char* why){if(!ok)throw opad::Error(why);};
    auto mouse=[](QWidget* widget,QEvent::Type type,QPointF at,Qt::MouseButton button,Qt::MouseButtons buttons){QMouseEvent e(type,at,widget->mapToGlobal(at.toPoint()),button,buttons,Qt::NoModifier);QApplication::sendEvent(widget,&e);};
    switch(state->phase++) {
      case 0:
        m_doc->newDocument();m_doc->run("feature",{{"kind","box"},{"inputs",{{"length",60},{"width",40},{"height",10}}}});
        m_viewport->setCameraJson({{"eye",{0,0,100}},{"target",{0,0,0}},{"up",{0,1,0}},{"scale",100},{"projection","orthographic"},{"absolute",true}});break;
      case 1:
        action("annotate.draw")->trigger();require(m_handDrawing && m_doc->annotationEditing,"drawing editor starts");break;
      case 2:{
        auto* panel=m_viewport->findChild<QFrame*>("handDrawingPanel");require(panel,"drawing panel visible");
        const auto combos=panel->findChildren<QComboBox*>();combos[0]->setCurrentIndex(combos[0]->findData("ai_agent"));
        panel->findChild<QPlainTextEdit*>()->setPlainText("Round the marked corner to radius 2 mm");
        const QPoint at=m_viewport->widgetPoint({0,0,10});
        mouse(m_viewport,QEvent::MouseButtonPress,at,Qt::LeftButton,Qt::LeftButton);
        for(int i=1;i<=12;++i)mouse(m_viewport,QEvent::MouseMove,at+QPoint(i*4,i*2),Qt::NoButton,Qt::LeftButton);
        mouse(m_viewport,QEvent::MouseButtonRelease,at+QPoint(50,25),Qt::LeftButton,Qt::NoButton);
        combos[1]->setCurrentIndex(1);combos[2]->setCurrentIndex(2);
        mouse(m_viewport,QEvent::MouseButtonPress,at+QPoint(-35,30),Qt::LeftButton,Qt::LeftButton);
        mouse(m_viewport,QEvent::MouseMove,at+QPoint(0,55),Qt::NoButton,Qt::LeftButton);
        mouse(m_viewport,QEvent::MouseButtonRelease,at+QPoint(35,30),Qt::LeftButton,Qt::NoButton);
        panel->grab().save(prefix+".drawing-panel.png");
        panel->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();break;
      }
      case 3:{
        require(!m_doc->annotationEditing && m_doc->scene.annotations.size()==1,"one saved drawing");
        const auto& a=m_doc->scene.annotations.front();state->id=a.id;
        require(a.anchor.kind==opad::Ref::Kind::Face,"first click retains face anchor");
        require(a.drawing["strokes"].size()==2 && a.drawing["strokes"][1]["color"]=="blue","two colors retained");
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
      case 4:{
        auto* card=m_viewport->findChild<NoteCard*>();require(card && card->pos()==state->position,"position survives scene refresh");
        auto* filter=m_annotations->findChild<QComboBox*>("annotationTypeFilter");filter->setCurrentIndex(filter->findData("issue"));break;
      }
      case 5:{
        require(m_viewport->findChildren<NoteCard*>().empty(),"type filter hides viewport cards");
        auto* filter=m_annotations->findChild<QComboBox*>("annotationTypeFilter");filter->setCurrentIndex(filter->findData("ai_agent"));break;
      }
      case 6:{
        require(m_viewport->findChildren<NoteCard*>().size()==1,"AI filter shows drawing");
        m_doc->saveAs(prefix+".opad");m_viewport->grabImage().save(prefix+".png");grab().save(prefix+".ui.png");
        break;
      }
      case 7: {auto* card=m_viewport->findChild<NoteCard*>();card->findChild<QPushButton*>("deleteNote")->click();break;}
      case 8:
        require(m_doc->scene.annotations.empty(),"Delete removes drawing");m_doc->undo();break;
      case 9:
        require(m_doc->scene.annotations.size()==1,"Undo restores drawing");m_doc->redo();break;
      case 10:
        require(m_doc->scene.annotations.empty(),"Redo deletes drawing");m_doc->undo();
        action("annotate.draw")->trigger();require(m_handDrawing,"second drawing starts");state->ops=m_doc->doc.ops.size();m_handDrawing->cancel();break;
      case 11:
        require(!m_doc->annotationEditing && m_doc->doc.ops.size()==state->ops,"Cancel creates no operation");
        m_doc->open(prefix+".opad");require(m_doc->scene.annotations.size()==1,"save/reopen retains drawing");
        require(opad::agent::context(m_doc->doc,m_doc->scene,opad::json::object())["ai_agent_notes"]==1,"agent sees queued note");
        trace::log("bench: TODO9 drawing, face anchor, drag, filters, Delete, Undo/Redo, Cancel and reopen PASS");timer->stop();QCoreApplication::exit(0);break;
    }
  } catch(const std::exception& e){trace::log(QString("bench: TODO9 FAIL: %1").arg(e.what()));timer->stop();QCoreApplication::exit(2);}});
  timer->start();return true;
}
