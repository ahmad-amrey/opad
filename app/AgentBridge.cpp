#include "AgentBridge.hpp"
#include "DesignController.hpp"
#include "Viewport.hpp"
#include "Panels.hpp"
#include "opad/live.hpp"
#include <QLocalSocket>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QThread>
#include <QVBoxLayout>
#include <QPushButton>
#include <QCryptographicHash>
#include <mutex>
using opad::json;
using namespace opad::agent;
namespace {
std::string uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();}
void dispose(std::shared_ptr<void> value){auto* thread=QThread::create([value=std::move(value)]{});QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();}
}
AgentBridge::AgentBridge(AppDocument* doc,DesignController* design,Viewport* viewport,JobRunner* jobs,QWidget* window)
 :QObject(window),m_doc(doc),m_design(design),m_viewport(viewport),m_jobs(jobs),m_window(window) {
  m_instance=QString::fromStdString(uuid());m_endpoint="opad-"+m_instance;
  QSettings settings;
  m_directory=(settings.format()==QSettings::IniFormat?QFileInfo(settings.fileName()).absolutePath():QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))+"/agent";
  QDir().mkpath(m_directory);m_file=m_directory+"/"+m_instance+".json";
  m_lock=std::make_unique<QLockFile>(m_file+".lock");m_lock->tryLock(0);
  m_follow=settings.value("agent/follow",false).toBool();
  m_server.setSocketOptions(QLocalServer::UserAccessOption);
  connect(&m_server,&QLocalServer::newConnection,this,&AgentBridge::accept);
  connect(doc,&AppDocument::aboutToReplace,this,[this]{stop();if(m_cache)dispose(std::move(m_cache));m_changes=json::array();for(auto& s:m_sessions)s->bound=false;});
  connect(doc,&AppDocument::changed,this,[this]{if(!m_committing)clearPrepared();if(m_cache)dispose(std::move(m_cache));publish();});
  connect(doc,&AppDocument::pathChanged,this,&AgentBridge::publish);
  connect(design,&DesignController::stateChanged,this,[this]{if(editorBusy() && !m_doc->snapshotBusy())clearPrepared();emit statusChanged();});
  setAccess(settings.value("agent/enabled",false).toBool(),settings.value("agent/edit",false).toBool());
  // Test instances use separate INI settings and an explicit opt-in environment variable.
  if(!qEnvironmentVariable("OPAD_BENCH_SETTINGS").isEmpty() && qEnvironmentVariableIsSet("OPAD_BENCH_AGENT"))setAccess(true,true);
}
AgentBridge::~AgentBridge(){
  stop();
  for(const auto& session:m_sessions)if(session->socket){QObject::disconnect(session->socket,nullptr,this,nullptr);session->socket->abort();}
  m_sessions.clear();m_server.close();m_publication->closed=true;
  auto* thread=QThread::create([writer=m_publication,file=m_file]{std::lock_guard lock(writer->mutex);QFile::remove(file);});
  connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();
  if(m_cache)dispose(std::move(m_cache));
}
QString AgentBridge::target()const{return m_doc->hasDocument?QString::fromStdString(m_doc->doc.header.uuid)+":"+QString::number(m_doc->generation):QString();}
json AgentBridge::descriptor()const{return {{"instance",m_instance.toStdString()},{"endpoint",m_endpoint.toStdString()},{"target",target().toStdString()},{"document",m_doc->hasDocument?m_doc->doc.header.uuid:""},{"title",m_doc->title().toStdString()},{"path",m_doc->path().toStdString()},{"pid",QCoreApplication::applicationPid()},{"enabled",m_enabled},{"edit",m_edit},{"version",opad::version_string()}};}
void AgentBridge::publish(){
  // Fixed-size descriptor; serialize/write on a worker. Serialize publishes under one mutex
  // and discard older generations, including those still pending when the window closes.
  const auto data=descriptor();const auto file=m_file;
  const auto sequence=++m_publication->sequence;
  auto* thread=QThread::create([writer=m_publication,data,file,sequence]{std::lock_guard lock(writer->mutex);if(writer->closed || writer->sequence!=sequence)return;QSaveFile f(file);if(f.open(QIODevice::WriteOnly)){f.write(QByteArray::fromStdString(data.dump()));f.commit();}});
  connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();emit statusChanged();
}
void AgentBridge::setAccess(bool enabled,bool edit){
  if(!enabled || !edit)stop();m_enabled=enabled;m_edit=edit;
  QSettings s;s.setValue("agent/enabled",enabled);s.setValue("agent/edit",edit);
  if(enabled && !m_server.isListening() && !m_server.listen(m_endpoint)){m_enabled=false;activity(tr("Connection failed: %1").arg(m_server.errorString()));}
  if(!m_enabled){m_server.close();const auto sessions=m_sessions;for(auto& session:sessions)if(session->socket)session->socket->disconnectFromServer();}
  publish();
}
bool AgentBridge::editorBusy()const{return m_doc->loading || m_doc->designBusy || m_doc->annotationEditing || m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane();}
json AgentBridge::editingState()const {
  const std::string edit=m_doc->annotationEditing?"annotation":m_design->sketchActive()?"sketch":m_design->featureActive()?"feature":m_design->pickingPlane()?"plane":"none";
  json owner=nullptr;
  const auto socket=m_busy?m_owner:(m_prepared?m_prepared->owner:QPointer<QLocalSocket>());
  for(const auto& s:m_sessions)if(s->socket && s->socket==socket)owner={{"kind","agent"},{"client_id",s->clientId},{"name",s->agent.toStdString()}};
  if(edit!="none")owner={{"kind","human"}};
  return {{"revision",m_doc->revision},{"edit_session",edit},{"human_edit",edit!="none"},
    {"loading",m_doc->loading},{"snapshot_busy",m_doc->snapshotBusy()},{"design_busy",m_doc->designBusy},
    {"agent_busy",m_busy},{"owner",owner},{"active_operation_id",m_busy?json(m_activeOperation):json(nullptr)},
    {"prepared_id",m_prepared?json(m_prepared->id):json(nullptr)}};
}
void AgentBridge::waitForIdle(const std::shared_ptr<Session>& s,int timeout,unsigned long long epoch,std::shared_ptr<QElapsedTimer> timer){
  if(!s->socket || !s->bound)return;
  if(!m_enabled || s->target!=target()){fail(s,"target_changed",tr("The document changed. Use live_instances and explicitly bind again."));return;}
  // Prepared work owned by this connection is available for its next staged command.
  const bool idle=!m_busy && !editorBusy() && (!m_prepared || m_prepared->owner==s->socket);
  const bool human=m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane();
  if(idle || human || timer->elapsed()>=timeout){
    reply(s,live_result({{"state","read"},{"revision",m_doc->revision},{"result",{{"idle",idle},{"timed_out",!idle && !human && timer->elapsed()>=timeout},{"editing",editingState()}}},{"elapsed_ms",timer->elapsed()}}));return;
  }
  QTimer::singleShot(20,this,[this,s,timeout,epoch,timer]{waitForIdle(s,timeout,epoch,timer);});
}
QString AgentBridge::stateText()const {
  if(!m_enabled)return tr("Disabled");if(m_busy)return tr("Busy");
  if(m_doc->annotationEditing || m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane())return tr("Waiting for active editor");
  if(m_doc->snapshotBusy())return tr("Capturing document");
  for(const auto& s:m_sessions)if(s->bound && s->socket && s->socket->state()==QLocalSocket::ConnectedState)return tr("Connected: %1").arg(s->agent);
  return m_seenClient?tr("Disconnected"):tr("Waiting for client");
}
json AgentBridge::liveState()const{
  auto out=descriptor();out["editing"]=editingState();out["revision"]=m_doc->revision;out["dirty"]=m_doc->isDirty();out["busy"]=m_busy;
  out["edit_session"]=out["editing"]["edit_session"];
  out["camera"]=m_viewport->cameraJson();out["selection"]=json::array();
  auto refs=m_viewport->selection();for(size_t i=0;i<std::min(size_t(100),refs.size());++i)out["selection"].push_back(refs[i].to_json());
  out["selection_total"]=refs.size();out["changes"]=m_changes;out["follow_changes"]=m_follow;
  out["profile_selection"]=json::array();const auto profiles=m_viewport->selectedCandidates();
  for(size_t i=0;i<std::min(size_t(100),profiles.size());++i){auto profile=json::parse(profiles[i],nullptr,false);out["profile_selection"].push_back(profile.is_discarded()?json(profiles[i]):profile);}
  if(m_design->sketchActive())out["active_sketch"]=m_design->sketch()->agentContext();
  if(m_prepared)out["prepared"]={{"id",m_prepared->id},{"base_revision",m_prepared->snapshot->revision}};
  return out;
}
void AgentBridge::accept(){while(m_server.hasPendingConnections()){
  auto session=std::make_shared<Session>();session->socket=m_server.nextPendingConnection();session->clientId=uuid();m_sessions.push_back(session);
  connect(session->socket,&QLocalSocket::readyRead,this,[this,session]{read(session);});
  connect(session->socket,&QLocalSocket::disconnected,this,[this,session]{
    session->bound=false;
    if(m_prepared && m_prepared->owner==session->socket)clearPrepared();
    if(m_busy && m_owner==session->socket){++m_epoch;if(m_job)m_job->cancel();m_busy=false;m_owner.clear();}
    if(session->socket)session->socket->deleteLater();std::erase(m_sessions,session);emit statusChanged();
  });
}}
void AgentBridge::read(const std::shared_ptr<Session>& session){
  if(!session->socket)return;session->input+=session->socket->readAll();
  if(session->input.size()>8*1024*1024){session->socket->abort();return;}
  if(session->receiving || !session->input.contains('\n'))return;
  const auto end=session->input.indexOf('\n');auto line=session->input.left(end);session->input.remove(0,end+1);session->receiving=true;session->requestTimer.start();
  struct Parsed{json request;std::string hash;};auto parsed=std::make_shared<Parsed>();
  m_jobs->async(tr("Reading agent request"),[line,parsed](Progress){
    parsed->request=json::parse(line.toStdString());const auto name=parsed->request.at("name").get<std::string>();
    validate_input(live_schema(name),parsed->request.value("arguments",json::object()));
    parsed->hash=QCryptographicHash::hash(QByteArray::fromStdString(parsed->request.dump()),QCryptographicHash::Sha256).toHex().toStdString();
  },[this,session,parsed](bool ok,const QString& error){if(!session->socket)return;if(!ok){fail(session,"invalid_arguments",error);return;}dispatch(session,std::move(parsed->request),std::move(parsed->hash));});
}
void AgentBridge::reply(const std::shared_ptr<Session>& session,json result,const std::string& receipt){
  if(!result["structuredContent"].contains("elapsed_ms") && session->requestTimer.isValid())result["structuredContent"]["elapsed_ms"]=session->requestTimer.elapsed();
  auto bytes=std::make_shared<QByteArray>();
  m_jobs->async(tr("Sending agent result"),[bytes,result=std::move(result)](Progress)mutable{
    result["content"].insert(result["content"].begin(),json{{"type","text"},{"text",result.at("structuredContent").dump()}});
    *bytes=QByteArray::fromStdString(result.dump());
    if(bytes->size()>12*1024*1024)*bytes=QByteArray::fromStdString(live_error("context_too_large","Request a smaller page or inspect a single entity.").dump());
    *bytes+='\n';
  },[this,session,bytes,receipt](bool ok,const QString&){
    const auto output=ok?*bytes:QByteArray::fromStdString(live_result({{"state",receipt.empty()?"response_cancelled":m_receipts[receipt].state},{"message","Response serialization cancelled; query current context."}}).dump())+'\n';
    if(!receipt.empty())m_receipts[receipt].response=output;
    if(session->socket && session->socket->state()==QLocalSocket::ConnectedState)session->socket->write(output);
    session->receiving=false;if(session->socket)read(session);
  });
}
void AgentBridge::fail(const std::shared_ptr<Session>& s,const std::string& code,const QString& message,const std::string& receipt){
  if(!receipt.empty())m_receipts[receipt].state=code=="cancelled"?"cancelled":"failed";
  auto error=live_error(code,message.toStdString());
  error["structuredContent"]["state"]=code=="cancelled"?"cancelled":"failed";
  error["structuredContent"]["error"]["next"]="Read live_state and the relevant context or feature_schema, correct the failed inputs, then use a new request_id. Query request_status before retrying an interrupted request.";
  // Capture this on the UI thread at rejection, before serialization or another event.
  auto& detail=error["structuredContent"]["error"];
  detail["editing"]=editingState();detail["client_id"]=s->clientId;
  const bool transient=(code=="busy" || code=="edit_session_busy") &&
    !m_design->sketchActive() && !m_design->featureActive() && !m_design->pickingPlane();
  detail["retryable"]=transient;detail["retry_after_ms"]=transient?json(50):json(nullptr);
  if(transient)detail["next"]="Call wait_for_idle, check revision and transaction state, then retry with a new request_id.";
  if(code=="not_bound" || code=="target_changed")detail["next"]=json{"live_instances","live_bind"};
  activity(tr("Agent error: %1").arg(message));reply(s,std::move(error),receipt);
}
void AgentBridge::replyReceipt(const std::shared_ptr<Session>& session,const Receipt& receipt){
  if(receipt.state=="cancelled"){
    auto result=live_error("cancelled","This prepared or unfinished operation was cancelled; it did not commit.");result["structuredContent"]["state"]="cancelled";reply(session,std::move(result));return;
  }
  if(receipt.response.isEmpty()){reply(session,live_result({{"state",receipt.state}}));return;}
  auto bytes=std::make_shared<QByteArray>();
  m_jobs->async(tr("Sending agent result"),[bytes,receipt](Progress){
    auto result=json::parse(receipt.response.toStdString());result["structuredContent"]["state"]=receipt.state;
    if(receipt.revision)result["structuredContent"]["revision"]=receipt.revision;
    if(!result["content"].empty() && result["content"][0].value("type","")=="text")result["content"][0]["text"]=result["structuredContent"].dump();
    *bytes=QByteArray::fromStdString(result.dump())+'\n';
  },[this,session,bytes](bool ok,const QString& error){
    if(!ok){fail(session,"response_cancelled",error);return;}
    if(session->socket)session->socket->write(*bytes);session->receiving=false;if(session->socket)read(session);
  });
}
void AgentBridge::dispatch(const std::shared_ptr<Session>& s,json request,std::string hash){
  const auto name=request.at("name").get<std::string>();auto args=request.value("arguments",json::object());
  if(!m_enabled){fail(s,"access_disabled",tr("Agent access is disabled."));return;}
  if(name=="live_bind"){
    if(args.at("instance")!=m_instance.toStdString() || args.at("target")!=target().toStdString() || target().isEmpty()){fail(s,"wrong_target",tr("Choose the current document explicitly using live_instances."));return;}
    if(request.value("version","")!=opad::version_string()){fail(s,"version_mismatch",tr("Use the CLI installed with this OPAD version."));return;}
    s->bound=true;s->target=target();s->agent=QString::fromStdString(request.value("client",json::object()).value("name","MCP client")).left(100);m_seenClient=true;
    activity(tr("Connected: %1").arg(s->agent));auto state=liveState();state["client_id"]=s->clientId;reply(s,live_result(state));return;
  }
  if(name=="live_diagnostics") {
    if(!s->bound || s->target!=target()) {
      json out={{"connection","target_changed"},{"target",s->target.toStdString()},{"permissions",{{"confirmed",false}}},{"units",nullptr},
        {"transaction_state",{{"state","none"},{"scope","connection"}}},{"next_calls",{"live_instances","live_bind"}}};
      if(args.value("include_example",false))out["guide"]=live_guide();if(args.value("include_guide",false))out["agent_guide"]=guide();reply(s,live_result(out));return;
    }
    json transaction={{"state","none"},{"scope","connection"}};
    if(m_prepared) {
      const bool owned=m_prepared->owner==s->socket;
      transaction["state"]=owned?(m_prepared->transaction?"staged":"preview"):"owned_by_another_connection";
      if(owned){transaction["id"]=m_prepared->id;transaction["base_revision"]=m_prepared->snapshot->revision;}
    }
    json next=m_busy?json{"live_diagnostics","stop"}:editorBusy()?json{"live_state"}:
      m_prepared?(m_prepared->owner!=s->socket?json{"live_state"}:m_prepared->transaction?json{"validate","transaction_commit","transaction_cancel"}:json{"viewport_image","preview_commit","preview_cancel"}):
      m_edit?json{"context","feature_schema","transaction_begin"}:json{"context","entity_details","viewport_image"};
    json out={{"connection","bound"},{"instance",m_instance.toStdString()},{"target",target().toStdString()},
      {"permissions",{{"enabled",m_enabled},{"edit",m_edit}}},{"revision",m_doc->revision},{"units",m_doc->scene.units},
      {"geometry_units","mm"},{"busy",m_busy},{"editor_busy",editorBusy()},{"transaction_state",transaction},{"next_calls",next}};
    if(args.value("include_example",false))out["guide"]=live_guide();if(args.value("include_guide",false))out["agent_guide"]=guide();reply(s,live_result(out));return;
  }
  if(!s->bound || s->target!=target()){
    if(s->target.isEmpty())fail(s,"not_bound",tr("This connection is not bound to a document yet. Call live_instances, choose the window and document, then live_bind."));
    else fail(s,"target_changed",tr("The document changed. Use live_instances and explicitly bind again."));
    return;
  }
  if(name=="wait_for_idle"){auto timer=std::make_shared<QElapsedTimer>();timer->start();waitForIdle(s,args.value("timeout_ms",2000),m_epoch,timer);return;}
  if(name=="stop"){stop();reply(s,live_result({{"stopped",true}}));return;}
  if(name=="live_state"){
    if(m_busy)reply(s,live_result({{"state","read"},{"revision",m_doc->revision},{"result",liveState()}}));
    else execute(s,name,args,{});
    return;
  }
  if(name=="live_select" && (editorBusy() || args.at("expected_revision").get<unsigned long long>()!=m_doc->revision)){fail(s,"selection_busy_or_stale",tr("Finish the active sketch or feature operation before agent edits."));return;}
  const auto key=(s->target+"/").toStdString()+args.value("request_id","");
  if(name=="request_status"){
    auto it=m_receipts.find(key);if(it==m_receipts.end())reply(s,live_result({{"state","unknown"},{"request_id",args["request_id"]}}));
    else replyReceipt(s,it->second);return;
  }
  const bool write=live_mutation(name);
  if(write){
    auto found=m_receipts.find(key);if(found!=m_receipts.end()){
      if(found->second.hash!=hash){fail(s,"request_id_reused",tr("This request ID already belongs to different arguments."));return;}
      replyReceipt(s,found->second);return;
    }
    if(!m_edit){fail(s,"read_only",tr("Agent access is view-only. Enable editing in Settings to change or export the document."));return;}
    if(m_doc->snapshotBusy() && !m_busy && !m_design->sketchActive() && !m_design->featureActive() && !m_design->pickingPlane()){
      if(s->requestTimer.elapsed()>=2000){fail(s,"edit_session_busy",tr("Document is busy. Retry after the current operation."));return;}
      // A recovery checkpoint can briefly own the write guard between MCP calls.
      // Wait for its worker instead of making unattended saves race autosave.
      const auto epoch=m_epoch;
      QTimer::singleShot(10,this,[this,s,request=std::move(request),hash=std::move(hash),epoch]()mutable{
        if(!s->socket || !s->bound)return;
        if(epoch!=m_epoch){fail(s,"cancelled",tr("Agent operation cancelled."));return;}
        dispatch(s,std::move(request),std::move(hash));
      });return;
    }
    if(editorBusy()){fail(s,"edit_session_busy",tr("Finish the active sketch or feature operation before agent edits."));return;}
    if(args.at("expected_revision").get<unsigned long long>()!=m_doc->revision){fail(s,"stale_revision",tr("The document changed. Read its current context and replan the edit."));return;}
    if(m_receipts.size()>=10000){fail(s,"session_limit",tr("This application session has reached its request limit. Save and restart OPAD."));return;}
  }
  if(name=="transaction_cancel" || name=="preview_cancel"){
    if(!m_prepared || m_prepared->id!=args.at("id").get<std::string>() || m_prepared->owner!=s->socket){fail(s,"unknown_prepared",tr("No matching prepared operation belongs to this connection."));return;}
    clearPrepared();reply(s,live_result({{"cancelled",true}}));return;
  }
  if(m_busy){fail(s,"busy",tr("An agent operation is still running. Wait or use Stop."));return;}
  if(write)m_receipts.emplace(key,Receipt{hash});
  m_activeOperation=args.value("request_id",name);
  if(name=="save"){save(s,args,key);return;}
  if(name=="transaction_commit" || name=="preview_commit"){commit(s,args.at("id").get<std::string>(),key,args.at("expected_revision").get<unsigned long long>());return;}
  execute(s,name,std::move(args),write?key:std::string());
}
void AgentBridge::clearPrepared(bool cancelReceipts){if(m_prepared){
  if(cancelReceipts)for(const auto& key:m_prepared->receipts)if(m_receipts[key].state=="staged")m_receipts[key].state="cancelled";
  m_viewport->clearPreviewBodies();dispose(std::move(m_prepared));
}}
void AgentBridge::stop(){++m_epoch;clearPrepared();if(m_job)m_job->cancel();m_busy=false;m_owner.clear();emit statusChanged();}
void AgentBridge::disconnectClients(){
  stop();const auto sessions=m_sessions;
  for(const auto& session:sessions){session->bound=false;if(session->socket)session->socket->abort();}
  emit statusChanged();
}
void AgentBridge::activity(const QString& text){
  if(!m_panel)showActivity();m_stateLabel->setText(stateText());m_activity->addItem(text);
  while(m_activity->count()>100)delete m_activity->takeItem(0);m_activity->scrollToBottom();emit statusChanged();
}
void AgentBridge::showActivity(){
  if(!m_panel){auto* content=new QWidget;auto* layout=new QVBoxLayout(content);m_stateLabel=new QLabel;m_stateLabel->setWordWrap(true);layout->addWidget(m_stateLabel);m_activity=new QListWidget;layout->addWidget(m_activity);
    auto* follow=new QCheckBox(tr("Follow changes"));follow->setChecked(m_follow);layout->addWidget(follow);
    connect(follow,&QCheckBox::toggled,this,[this](bool on){m_follow=on;QSettings().setValue("agent/follow",on);});
    auto* cancel=new QPushButton(tr("Stop agent work"));layout->addWidget(cancel);connect(cancel,&QPushButton::clicked,this,&AgentBridge::stop);
    m_panel=new ToolPanel("agent-activity","history",&Tokens::sel,tr("Agent activity"),content,360,m_window);
    connect(this,&AgentBridge::statusChanged,m_stateLabel,[this]{m_stateLabel->setText(stateText()+"\n"+m_doc->title());});
  }
  m_panel->anchorTo(QRect(m_viewport->mapToGlobal(QPoint()),m_viewport->size()));m_panel->show();m_panel->raise();
}
