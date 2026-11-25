#include "AgentBridge.hpp"
#include "DesignController.hpp"
#include "Viewport.hpp"
#include "RecoveryManager.hpp"
#include "AgentClients.hpp"
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QTabWidget>
#include <QScrollBar>
#include <QKeyEvent>
#include <QAction>
#include "Panels.hpp"
// Explicit, isolated acceptance harness. No control files are consumed in ordinary runs.
void AgentBridge::bench(){
  const auto directory=qEnvironmentVariable("OPAD_BENCH_AGENT");
  if(directory.isEmpty() || qEnvironmentVariable("OPAD_BENCH_SETTINGS").isEmpty())return;
  QDir().mkpath(directory);auto* timer=new QTimer(this);timer->setInterval(100);
  connect(timer,&QTimer::timeout,this,[this,directory]{
    if(m_doc->loading)return;
    QFile input(directory+"/action.json");if(!input.exists() || !input.open(QIODevice::ReadOnly))return;
    auto data=opad::json::parse(input.readAll().toStdString(),nullptr,false);input.close();QFile::remove(input.fileName());if(!data.is_object())return;
    opad::json result;
    try{
      const auto action=data.at("action").get<std::string>();
      if(action=="timeline"){
        auto* timeline=m_window->findChild<TimelineWidget*>();
        if(!timeline)throw opad::Error("Timeline unavailable");
        auto* scroll=timeline->findChild<QScrollBar*>();
        if(data.contains("key")){
          const auto key=data["key"].get<std::string>();
          QKeyEvent event(QEvent::KeyPress,key=="home"?Qt::Key_Home:key=="end"?Qt::Key_End:key=="left"?Qt::Key_Left:Qt::Key_Right,Qt::NoModifier);
          QApplication::sendEvent(timeline,&event);
        }
        if(data.contains("scroll"))scroll->setValue(data["scroll"].get<int>());
        if(data.contains("image"))timeline->grab().save(QString::fromStdString(data["image"].get<std::string>()));
        result={{"current",timeline->currentOp()},{"scroll",scroll->value()},{"maximum",scroll->maximum()},{"page",scroll->pageStep()}};
      }
      else if(action=="select_similar"){  // Select similar on the selection (a body: top perimeter first, again: the next rule)
        auto* action=m_window->findChild<QAction*>("select.similar");
        if(!action)throw opad::Error("Select similar action unavailable");
        action->trigger();
      }
      else if(action=="state")result=liveState();
      else if(action=="undo")m_doc->undo();
      else if(action=="redo")m_doc->redo();
      else if(action=="access")setAccess(data.value("enabled",true),data.value("edit",true));
      else if(action=="stop")stop();
      else if(action=="disconnect")disconnectClients();
      else if(action=="registration") {  // a client's row on Connect an AI client: "client" codex (default), claude-code, claude-desktop
        const auto client=data.value("client","codex");
        const QString row=client=="claude-code"?"agentClientClaudeCode":client=="claude-desktop"?"agentClientClaudeDesktop":"agentClientCodex";
        auto* registration=m_window->findChild<AgentClientRow*>(row);
        if(!registration){settings(0);registration=m_window->findChild<AgentClientRow*>(row);}
        if(!registration)throw opad::Error("Registration widget unavailable");
        if(data.contains("executable"))registration->setProgram(QString::fromStdString(data["executable"].get<std::string>()));
        const auto operation=data.value("operation","status");
        if(operation=="connect")registration->connectClient();else if(operation=="remove")registration->removeClient();
        else if(operation=="point")registration->pointHere();else if(operation=="check")registration->refresh();
        result={{"status",registration->report().toStdString()}};
      }
      else if(action=="delay")m_benchDelay=std::clamp(data.value("ms",0),0,5000);
      else if(action=="settings"){
        // tab: 0 Access and activity, 1 Client setup (as before), 2 Connect an AI client.
        const auto tab=data.value("tab",0);settings(tab==1?2:tab==2?0:1);
        QTimer::singleShot(300,this,[this,directory,tab]{for(auto* dialog:m_window->findChildren<QDialog*>("agentSettings")){dialog->grab().save(directory+(tab==1?"/settings-client.png":tab==2?"/settings-connect.png":"/settings.png"));dialog->close();}});
      }
      else if(action=="new")m_doc->newDocument();
      else if(action=="save")m_doc->saveAs(QString::fromStdString(data.at("path").get<std::string>()));
      else if(action=="autosave")m_window->findChild<RecoveryManager*>()->saveNow();
      else if(action=="manual")result=m_doc->run(data.at("command").get<std::string>(),data.at("arguments"));
      else if(action=="select"){
        const auto ref=opad::Ref::from_json(data.at("ref"));
        m_viewport->setSelectionFilter(ref.kind==opad::Ref::Kind::Face?Viewport::SelFilter::Face:ref.kind==opad::Ref::Kind::Edge?Viewport::SelFilter::Edge:Viewport::SelFilter::Body);
        QTimer::singleShot(200,this,[this,ref]{m_viewport->selectRefs({ref});});
      }
      else if(action=="follow"){m_follow=data.at("enabled").get<bool>();}
      else if(action=="edit_sketch")m_design->editOp(data.at("id").get<std::string>());
      else if(action=="annotation_editor") {
        auto* tool=m_window->findChild<QAction*>("annotate.draw");
        if(!tool)throw opad::Error("Hand drawing action unavailable");
        tool->trigger();
      }
      else if(action=="cancel_edit"){if(m_design->sketchActive())m_design->cancelSketch();else m_design->escape();}
      else if(action=="quit"){stop();QTimer::singleShot(0,this,[]{QCoreApplication::exit(0);});}
      else throw opad::Error("Unknown bench action");
      if(result.is_null())result={{"ok",true},{"revision",m_doc->contentRevision}};
    }catch(const std::exception& e){result={{"error",e.what()}};}
    QSaveFile output(directory+"/result.json");if(output.open(QIODevice::WriteOnly)){output.write(QByteArray::fromStdString(result.dump()));output.commit();}
    publish();
  });timer->start();
}
