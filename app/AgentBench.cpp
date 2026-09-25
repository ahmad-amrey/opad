#include "AgentBridge.hpp"
#include "DesignController.hpp"
#include "Viewport.hpp"
#include <QFile>
#include <QSaveFile>
#include <QDir>
#include <QApplication>
#include <QDialog>
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
      if(action=="state")result=liveState();
      else if(action=="undo")m_doc->undo();
      else if(action=="redo")m_doc->redo();
      else if(action=="access")setAccess(data.value("enabled",true),data.value("edit",true));
      else if(action=="stop")stop();
      else if(action=="delay")m_benchDelay=std::clamp(data.value("ms",0),0,5000);
      else if(action=="settings"){
        settings();QTimer::singleShot(300,this,[this,directory]{for(auto* dialog:m_window->findChildren<QDialog*>())if(dialog->windowTitle()==tr("AI integration")){dialog->grab().save(directory+"/settings.png");dialog->close();}});
      }
      else if(action=="new")m_doc->newDocument();
      else if(action=="save")m_doc->saveAs(QString::fromStdString(data.at("path").get<std::string>()));
      else if(action=="manual")result=m_doc->run(data.at("command").get<std::string>(),data.at("arguments"));
      else if(action=="select"){
        const auto ref=opad::Ref::from_json(data.at("ref"));
        m_viewport->setSelectionFilter(ref.kind==opad::Ref::Kind::Face?Viewport::SelFilter::Face:ref.kind==opad::Ref::Kind::Edge?Viewport::SelFilter::Edge:Viewport::SelFilter::Body);
        QTimer::singleShot(200,this,[this,ref]{m_viewport->selectRefs({ref});});
      }
      else if(action=="follow"){m_follow=data.at("enabled").get<bool>();}
      else if(action=="edit_sketch")m_design->editOp(data.at("id").get<std::string>());
      else if(action=="cancel_edit"){if(m_design->sketchActive())m_design->cancelSketch();else m_design->escape();}
      else if(action=="quit"){stop();QTimer::singleShot(0,this,[]{QCoreApplication::exit(0);});}
      else throw opad::Error("Unknown bench action");
      if(result.is_null())result={{"ok",true},{"revision",m_doc->revision}};
    }catch(const std::exception& e){result={{"error",e.what()}};}
    QSaveFile output(directory+"/result.json");if(output.open(QIODevice::WriteOnly)){output.write(QByteArray::fromStdString(result.dump()));output.commit();}
    publish();
  });timer->start();
}
