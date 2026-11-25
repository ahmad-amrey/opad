#include "AgentBridge.hpp"
#include "DiskSync.hpp"
#include <QFileInfo>
#include "DesignController.hpp"
#include "Viewport.hpp"
#include "opad/live.hpp"
#include "opad/model_batch.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh.hpp"
#include "opad/render.hpp"
#include "opad/inspect.hpp"
#include "opad/mass.hpp"
#include "opad/design/provenance.hpp"
#include <QLocalSocket>
#include <QUuid>
#include <QThread>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
#include <QImage>
#include <cctype>
using opad::json;
using namespace opad::agent;
namespace {
std::string newId(){return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();}
void release(std::shared_ptr<void> value){auto* thread=QThread::create([value=std::move(value)]{});QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();}
json changes(const opad::Scene& before,const opad::Scene& after){
  json out={{"created",json::array()},{"modified",json::array()},{"deleted",json::array()},{"geometry",json::array()},{"total",0}};
  auto add=[&](const char* key,const std::string& id){out["total"]=out["total"].get<int>()+1;if(out[key].size()<100)out[key].push_back(id);};
  for(const auto& [id,n]:after.nodes){const auto* previous=before.node(id);if(!previous)add("created",id);else if(n.body_key!=previous->body_key || n.name!=previous->name || n.visible!=previous->visible || n.local.to_json()!=previous->local.to_json() || n.modified_by!=previous->modified_by)add("modified",id);}
  for(const auto& [id,n]:before.nodes)if(!after.node(id))add("deleted",id);
  for(const auto& s:after.sketches){const auto* prior=before.sketch(s.id);if(!prior)add("created",s.id);else if(s.geometry!=prior->geometry || s.frame.to_json()!=prior->frame.to_json())add("modified",s.id);}
  for(const auto& s:before.sketches)if(!after.sketch(s.id))add("deleted",s.id);
  for(const auto& [id,n]:after.nodes)if(n.kind==opad::Node::Kind::Body && out["geometry"].size()<100){const auto* p=before.node(id);if(!p || n.body_key!=p->body_key || before.world(id).to_json()!=after.world(id).to_json())out["geometry"].push_back(id);}
  out["parameters"]=after.params.size();out["features"]=after.features.size();return out;
}
// Compact replies (TODO 10 B11; an agent's evaluation: a Move of two linked boards answered with 84 KB, an import of one
// board listed 217 ids): counts, the roots of what was made and one line per linked file, never a list per node. Lists
// up to kCompactList ids stay as they are, so small edits read as before.
constexpr size_t kCompactList=20,kCompactBodies=10;
// The outermost linked node holding `id` (a linked file's top), or empty when it is not part of one.
std::string linkedTop(const opad::Scene& s,const std::string& id){
  std::string top;
  for(const auto* n=s.node(id);n && n->linked;n=n->parent.empty()?nullptr:s.node(n->parent))top=n->id;
  return top;
}
json compactChanges(const opad::Scene& before,const opad::Scene& after,const json& full){
  std::vector<std::string> created,modified,deleted;
  struct File {size_t nodes=0,bodies=0;std::string change,name;};std::map<std::string,File> files;  // a linked file's top -> what this did to it
  size_t counts[3]={0,0,0};
  auto note=[&](int kind,const opad::Scene& s,const std::string& id){
    ++counts[kind];
    static const char* const words[]={"created","modified","deleted"};
    if(const auto top=linkedTop(s,id);!top.empty()){
      auto& f=files[top];++f.nodes;if(f.change.empty() || id==top)f.change=words[kind];
      if(const auto* n=s.node(top)){f.name=n->name;f.bodies=s.bodies_under(top).size();}
      return;
    }
    (kind==0?created:kind==1?modified:deleted).push_back(id);
  };
  for(const auto& [id,n]:after.nodes){const auto* p=before.node(id);if(!p)note(0,after,id);else if(n.body_key!=p->body_key || n.name!=p->name || n.visible!=p->visible || n.local.to_json()!=p->local.to_json() || n.modified_by!=p->modified_by)note(1,after,id);}
  for(const auto& [id,n]:before.nodes)if(!after.node(id))note(2,before,id);
  for(const auto& s:after.sketches){const auto* prior=before.sketch(s.id);if(!prior)note(0,after,s.id);else if(s.geometry!=prior->geometry || s.frame.to_json()!=prior->frame.to_json())note(1,after,s.id);}
  for(const auto& s:before.sketches)if(!after.sketch(s.id))note(2,before,s.id);
  json out={{"scope","command"},{"total",counts[0]+counts[1]+counts[2]},{"counts",{{"created",counts[0]},{"modified",counts[1]},{"deleted",counts[2]}}}};
  const char* const keys[]={"created","modified","deleted"};const std::vector<std::string>* lists[]={&created,&modified,&deleted};
  for(int k=0;k<3;++k)if(lists[k]->size()<=kCompactList)out[keys[k]]=*lists[k];
  if(created.size()>kCompactList){  // what was made, by its tops: the rest is under them
    const std::set<std::string> made(created.begin(),created.end());json roots=json::array();size_t total=0;
    for(const auto& id:created){const auto* n=after.node(id);if(n && made.count(n->parent))continue;if(++total<=kCompactList)roots.push_back(id);}
    out["created_roots"]=roots;if(total>kCompactList)out["created_roots_total"]=total;
  }
  if(!files.empty()){
    json lines=json::array();
    for(const auto& [top,f]:files)if(lines.size()<kCompactList)lines.push_back({{"id",top},{"name",f.name},{"change",f.change},{"bodies",f.bodies},{"nodes_changed",f.nodes}});
    out["linked_files"]=lines;
  }
  const json bodies=full.value("bodies",json::array());
  if(bodies.size()<=kCompactBodies && !full.contains("bodies_total"))out["bodies"]=bodies;
  else {
    out["bodies_total"]=full.value("bodies_total",bodies.size());json invalid=json::array();
    for(const auto& b:bodies)if(!b.value("valid",true))invalid.push_back(b.at("id"));
    out["invalid_bodies"]=invalid;
  }
  if(full.contains("validation"))out["validation"]=full["validation"];
  return out;
}
// A command's or batch step's own result, compact: lists over kCompactList become <key>_count (a later batch still refers
// to every item: the connection keeps the full results), and a linked file moved as one is one line.
json compactResult(const json& r,const opad::Scene& scene,int depth=0){
  if(!r.is_object())return r;
  json out=json::object();
  for(const auto& [key,value]:r.items()){
    if(key=="placed_ids" && value.is_array()){
      json placed=json::array();
      for(const auto& id:value){const auto* n=id.is_string()?scene.node(id.get<std::string>()):nullptr;placed.push_back({{"id",id},{"name",n?n->name:std::string()},{"bodies",n?scene.bodies_under(n->id).size():0}});}
      out["placed"]=placed;continue;
    }
    if(value.is_array() && value.size()>kCompactList){out[key+"_count"]=value.size();continue;}
    out[key]=value.is_object() && depth<3?compactResult(value,scene,depth+1):value;
  }
  return out;
}
}
void AgentBridge::snapshot(std::function<void(std::shared_ptr<Snapshot>,QString)> done,int waited){
  if(m_cache && m_cache->docRevision==m_doc->docRevision){done(m_cache,{});return;}
  // Another capture or a save holds the document (a recovery checkpoint right after a commit): wait for it rather
  // than fail the call (gap log #3: "snapshot_failed: Document is busy" after every commit).
  if(m_doc->snapshotBusy() && waited<30000){
    QPointer<AgentBridge> self=this;
    QTimer::singleShot(20,this,[self,done=std::move(done),waited]()mutable{if(self)self->snapshot(std::move(done),waited+20);});
    return;
  }
  // The copy is made while the document is guarded (captureSnapshot): it is the log at these revisions.
  const auto revision=m_doc->contentRevision,docRevision=m_doc->docRevision,generation=m_doc->generation;QPointer<AgentBridge> self=this;
  if(!m_doc->captureSnapshot(m_jobs,[self,revision,docRevision,generation,done](std::shared_ptr<opad::Document> copy,const QString& error){
    if(!self)return;if(!copy){done({},error);return;}
    auto value=std::make_shared<Snapshot>();value->doc=std::move(copy);value->revision=revision;value->docRevision=docRevision;
    self->m_jobs->backgroundNext();  // agent traffic: no busy cursor, no completion toast
    self->m_jobs->async(tr("Preparing agent context"),[value](Progress p){
        auto ids=std::make_shared<std::vector<std::string>>();ids->reserve(value->doc->ops.size());
        for(const auto& op:value->doc->ops)ids->push_back(op.id);
        value->baseIds=std::move(ids);
        value->scene=opad::resolve(*value->doc);if(p.cancelled())throw opad::Error("cancelled");},
      [self,value,done,generation](bool ok,const QString& error){
        if(!self)return;if(!ok){done({},error);return;}
        if(generation!=self->m_doc->generation || value->revision!=self->m_doc->contentRevision){done({},tr("Document changed while preparing context. Retry."));return;}
        // A hide or a colour meanwhile: still the content the agent asked about (its commit keeps those), not cached
        if(value->docRevision==self->m_doc->docRevision)self->m_cache=value;
        done(value,{});
      });
  },true))done({},tr("Document is busy. Retry after the current operation."));
}
void AgentBridge::save(const std::shared_ptr<Session>& session,const json& args,const std::string& receipt){
  if(m_prepared){fail(session,"prepared_active",tr("Commit or cancel the current transaction first."),receipt);return;}
  // The window asks the user about the file (it changed on disk: Merge / Reload / Save as / Overwrite): a save now would
  // decide that question behind the user's back, whatever the file's time stamp says.
  if(const auto* disk=m_window->findChild<DiskSync*>();disk && !disk->asking().isEmpty() && args.value("path","").empty()){
    m_receipts[receipt].state="failed";
    auto error=live_error("disk_changed",tr("%1 changed on disk and OPAD is asking the user what to do with it (Reload, Merge, Save as or Overwrite). Nothing was saved: saving now could write over what changed there.").arg(QFileInfo(m_doc->path()).fileName()).toStdString());
    error["structuredContent"]["state"]="failed";
    error["structuredContent"]["error"]["banner"]=disk->asking().toStdString();
    error["structuredContent"]["error"]["next"]="ask_user";
    error["structuredContent"]["error"]["ask_user"]="Ask the user to answer OPAD's card about the file at the top of the 3D view, then read live_state again before saving.";
    activity(tr("Agent save refused: the file changed on disk and the user is being asked"));reply(session,std::move(error),receipt);return;
  }
  const auto revision=m_doc->contentRevision,epoch=++m_epoch;
  m_busy=true;m_owner=session->socket;activity(tr("Agent: %1").arg("save"));
  try {
    m_job=m_doc->saveAsync(m_jobs,QString::fromStdString(args.value("path","")),args.value("overwrite",false),
      [this,session,receipt,revision,epoch](bool written,const QString& error){
        if(epoch==m_epoch){m_busy=false;m_job.clear();m_owner.clear();}
        if(!written){fail(session,error=="cancelled"?"cancelled":"save_failed",error,receipt);emit statusChanged();return;}
        // A save that reached disk stays successful even if the client disconnected
        // immediately afterwards. request_status can retrieve this durable outcome.
        m_receipts[receipt].state="committed";m_receipts[receipt].revision=revision;
        reply(session,live_result({{"state","committed"},{"revision",revision},
          {"result",{{"path",m_doc->path().toStdString()},{"saved_revision",revision},{"dirty",m_doc->isDirty()}}}}),receipt);
        activity(tr("Saved %1").arg(m_doc->path()));
        ActivityEvent e;e.kind=ActivityEvent::Quiet;e.icon=QStringLiteral("save");e.client=who(session).brief;e.canShow=false;
        e.text=tr("Saved %1").arg(QFileInfo(m_doc->path()).fileName());post(e);
        emit statusChanged();
      },m_benchDelay);
  }catch(const std::exception& e){m_busy=false;m_owner.clear();fail(session,"save_failed",QString::fromUtf8(e.what()),receipt);emit statusChanged();}
}
void AgentBridge::execute(const std::shared_ptr<Session>& session,std::string name,json args,const std::string& receipt){
  if(m_busy){fail(session,"busy",tr("An agent operation is still running. Wait or use Stop."),receipt);return;}
  const bool write=live_mutation(name,args),preview=args.value("preview",false);const auto transaction=args.value("transaction","");
  const auto previewId=args.value("preview_id","");
  if(!previewId.empty() && (!m_prepared || m_prepared->id!=previewId || m_prepared->owner!=session->socket)){
    fail(session,"unknown_preview",tr("No matching prepared operation belongs to this connection."));return;
  }
  if(!transaction.empty() && (!m_prepared || !m_prepared->transaction || m_prepared->id!=transaction || m_prepared->owner!=session->socket)){
    fail(session,"unknown_transaction",tr("No matching transaction belongs to this connection. Disconnect discards staged work. Read current context, begin a new transaction and replan with new request IDs."),receipt);return;
  }
  if(write && transaction.empty() && m_prepared && m_prepared->transaction){fail(session,"transaction_active",tr("Commit or cancel the current transaction first."),receipt);return;}
  if(name=="export" && preview){fail(session,"invalid_export",tr("Export a transaction's staged state, or the document; a preview cannot be exported."),receipt);return;}
  const auto state=liveState(name=="live_state" && args.value("change_history",false));const auto linked=name=="context"?linkedFiles():json();  // read here, on the UI thread
  m_busy=true;m_owner=session->socket;const auto epoch=++m_epoch,revision=m_doc->contentRevision;
  const auto started=std::make_shared<QElapsedTimer>();started->start();activity(tr("Agent: %1").arg(QString::fromStdString(name)));
  // The call's row, running until it is done (none for hidden calls and lookups, which fold; a transaction's steps go under
  // its group). The text comes from the arguments; the done form from what changed.
  const auto display=units::current();const QString client=who(session).brief;
  const auto kind=agentsay::kindOf(name,args);
  m_call.running.clear();
  if(kind!=agentsay::Kind::Hidden && kind!=agentsay::Kind::Fold && transaction.empty()){
    ActivityEvent e;e.kind=ActivityEvent::Running;e.icon=QStringLiteral("aiMark");e.client=client;e.canShow=false;
    e.text=agentsay::running(name,args,m_doc->scene,display);m_call.running=e.text;
    dropRetrying();m_running=post(e);m_working.start();
    QTimer::singleShot(420,this,[this]{emit statusChanged();});
  }
  auto ready=[this,session,name,args=std::move(args),receipt,write,preview,transaction,epoch,revision,state,linked,started,known=session->known,steps=session->steps,display,client,kind](std::shared_ptr<Snapshot> source,const QString& error){
    if(epoch!=m_epoch || !session->socket || !session->bound){fail(session,"cancelled",tr("Agent operation cancelled."),receipt);return;}
    if(!source){m_busy=false;fail(session,"snapshot_failed",error,receipt);return;}
    if(name=="transaction_begin"){
      if(m_prepared){m_dropReason=tr("Replaced by a new task. Nothing changed.");clearPrepared(false,true);}  // else its row stayed "preview"
      m_prepared=std::make_shared<Prepared>();m_prepared->snapshot=source;m_prepared->id=newId();m_prepared->label=QString::fromStdString(args.at("label").get<std::string>());m_prepared->owner=session->socket;m_prepared->transaction=true;
      m_prepared->receipts.push_back(receipt);
      ActivityEvent e;e.kind=ActivityEvent::Group;e.state=ActivityEvent::Staged;e.icon=QStringLiteral("commit");e.client=client;
      e.text=tr("%1's task: %2").arg(client,agentsay::quote(m_prepared->label,48));e.second=tr("Preview, not applied yet · %1").arg(agentsay::stepsSoFar(0));
      m_group=m_staged=m_prepared->event=post(e);emit statusChanged();  // the chip and the viewport's preview chip
      m_busy=false;m_receipts[receipt].state="staged";reply(session,live_result({{"state","staged"},{"transaction",m_prepared->id},{"base_revision",revision},{"revision",revision},{"lifetime",transaction_policy()}}),receipt);return;
    }
    struct Result {std::shared_ptr<Snapshot> snapshot;json output,delta,steps;TopoDS_Shape preview;std::shared_ptr<const BodyPrs> prs;std::vector<std::string> hidden;
      agentsay::Sentence said;QStringList stepSentences;QImage thumb,picture;};auto result=std::make_shared<Result>();
    const auto baseline=!transaction.empty() && m_cache?m_cache:source;
    m_jobs->backgroundNext();
    auto job=m_jobs->async(tr("Agent: %1").arg(QString::fromStdString(name)),[source,baseline,result,args,name,write,preview,transaction,state,linked,known,steps,delay=m_benchDelay,display,client,kind](Progress p)mutable{
      p.setPhase(tr("Inspecting inputs"));
      if(write && name!="model_batch")opad::agent::check_references(*source->doc,source->scene,args,known.get());
      const bool compact=args.value("verbosity","full")=="compact";  // TODO 10 B11
      for(const char* key:{"expected_revision","request_id","transaction","references","verbosity"})args.erase(key);
      // A write's preview is staging, the command never sees it; a read keeps its own (joint_set preview=true is a read
      // that only reports: without it the command wrote a pose into the shared copy and answered with its id, #26).
      if(write)args.erase("preview");
      else args=opad::agent::live_read_defaults(name,std::move(args));  // features and ops: a page
      if(write){result->snapshot=std::make_shared<Snapshot>();result->snapshot->doc=std::make_shared<opad::Document>(*source->doc);result->snapshot->revision=source->revision;result->snapshot->docRevision=source->docRevision;result->snapshot->baseIds=source->baseIds;args["by"]="Agent";}
      auto working=write?result->snapshot:source;
      std::set<std::string> batchValid;  // body keys model_batch's steps already found valid
      if(p.cancelled())throw opad::Error("cancelled");
      p.setPhase(tr("Computing geometry and context"));
      if(name=="live_state"){
        result->output=state;auto& refs=result->output["selection"];
        opad::design::Provenance provenance(*source->doc,[p]{return p.cancelled();});  // created_by: once per body, not per pick
        for(auto& r:refs){
          if(p.cancelled())throw opad::Error("cancelled");
          auto ref=opad::Ref::from_json(r);if(source->scene.node(ref.body))r=entity_details(*source->doc,source->scene,{{"ref",r},{"limit",10}},&provenance);
        }
      }else if(name=="context"){
        result->output=context(*source->doc,source->scene,args);
        if(!linked.is_null())result->output["linked_files"]=linked;  // why parts are missing, and that only the user can help
      }
      else if(name=="sketch_details")result->output=sketch_details(*source->doc,source->scene,args);
      else if(name=="query_entities")result->output=query_entities(*source->doc,source->scene,args,[p]{return p.cancelled();});
      else if(name=="entity_details")result->output=entity_details(*source->doc,source->scene,args);
      else if(name=="resolve_reference")result->output=resolve_reference(*source->doc,source->scene,args.at("reference"),args.value("remap",false));
      else if(name=="validate")result->output=validate_design(*source->doc,source->scene,args,[p]{return p.cancelled();});
      else if(name=="live_select"){
        std::string kind;
        for(const auto& value:args.at("refs")){
          const auto ref=opad::Ref::from_json(value);const auto current=ref.to_json().value("kind","body");
          if(ref.kind==opad::Ref::Kind::Point)throw opad::Error("Select a body or a face, edge, vertex or circle-center reference, not a free-space point.");
          if(!kind.empty() && current!=kind)throw opad::Error("Select references of the same kind in one request.");kind=current;
          reference_token(*source->doc,source->scene,ref);
        }
        result->output={{"selection_requested",args.at("refs")}};
      }
      else if(name=="viewport_image"){
        opad::RenderOptions options;options.width=args.value("width",960);options.height=args.value("height",640);options.supersample=1;
        if(args.contains("camera") && args.contains("view"))throw opad::Error("Choose camera or view, not both");
        options.fit=args.value("fit",false)||args.contains("view");
        options.camera=options.fit?opad::Camera::preset(args.value("view","iso")):opad::Camera::from_json(state.at("camera"));
        if(args.contains("camera")){
          auto camera=args["camera"];if(!camera.contains("absolute"))camera["absolute"]=true;
          options.camera=opad::Camera::from_json(camera);
          double direction=0,up=0;for(int i=0;i<3;++i){const auto d=options.camera.eye[i]-(options.camera.absolute?options.camera.target[i]:0);direction+=d*d;up+=options.camera.up[i]*options.camera.up[i];}
          if(direction<1e-18 || up<1e-18)throw opad::Error("Camera direction and up must be nonzero");
          if(options.fit && options.camera.absolute){
            for(int i=0;i<3;++i)options.camera.eye[i]-=options.camera.target[i];
            options.camera.absolute=false;
          }
        }
        options.select=args.value("select",std::vector<std::string>{});options.ignore_visibility=args.value("ignore_visibility",false);
        // TODO 10 B9: views grid, the model's edges (edges or edge_lines), highlights, smooth shading; all off by default.
        auto picture=args;if(args.value("edges",false))picture["edge_lines"]=true;opad::commands::apply_picture_options(options,picture);
        // Use only a temporary scene copy; no camera, visibility or placement edits.
        auto renderingScene=source->scene;
        for(const auto& id:args.value("hide",std::vector<std::string>{})){
          if(!renderingScene.node(id))throw opad::Error("Unknown hidden node: "+id);
          for(const auto& body:renderingScene.bodies_under(id))renderingScene.nodes.at(body).opacity=0;
        }
        json metadata;QElapsedTimer renderTimer;renderTimer.start();
        const auto png=opad::encode_png(opad::render_scene(*source->doc,renderingScene,options,&metadata));
        {  // what the assistant saw, for its row: scaled here, never on the UI thread
          QImage image;
          if(image.loadFromData(reinterpret_cast<const uchar*>(png.data()),int(png.size()),"PNG")){
            result->thumb=image.scaled(96,64,Qt::KeepAspectRatio,Qt::SmoothTransformation);
            result->picture=image.width()>480?image.scaledToWidth(480,Qt::SmoothTransformation):image;
          }
        }
        const json visible=metadata.value("visible_ids",json::array());
        result->output={{"image",QByteArray::fromStdString(png).toBase64().toStdString()},{"camera",metadata["camera"]},{"visible_count",visible.size()},
          {"selection",state["selection"]},{"preview_id",args.value("preview_id","")},{"transaction",transaction},{"render_ms",renderTimer.elapsed()},
          {"rendering","software geometry view; visible_count counts submitted visible bodies (including occluded ones), visible_ids=true lists them; UI overlays are not included"}};
        if(args.value("visible_ids",false))result->output["visible_ids"]=visible;  // opt-in: 140 ids on a board
        if(metadata.contains("views"))result->output["views"]=metadata["views"];
      }else if(name=="model_batch"){
        opad::agent::BatchOptions options{known.get(),steps.get(),[p]{return p.cancelled();},&batchValid};
        options.step_done=[&](const std::string& command,const json& input,const json& value,const opad::Scene& prior,const opad::Scene& scene){
          result->stepSentences.push_back(agentsay::done(command,input,value,prior,scene,display).text);  // the step in the user's words (its group row)
        };
        opad::agent::model_batch(*working->doc,args,result->output,options);
      }
      else result->output=opad::commands::run(name,args,working->doc.get());
      if(p.cancelled())throw opad::Error("cancelled");
      if(write && name!="export"){
        // Who made these steps, so the timeline can say so after the file is reopened ("Claude · 14:02"); data.by stays "Agent".
        if(!client.isEmpty())for(size_t i=source->doc->ops.size();i<working->doc->ops.size();++i)try{
          auto data=working->doc->ops[i].data;data["ai_client"]=client.left(40).toStdString();working->doc->rewrite_op(i,std::move(data));
        }catch(const std::exception&){}
        p.setPhase(tr("Validating design"));working->scene=opad::resolve(*working->doc);
        if(result->output.is_object()) {
          result->output["operation_ids"]=json::array();
          for(size_t i=source->doc->ops.size();i<working->doc->ops.size();++i)result->output["operation_ids"].push_back(working->doc->ops[i].id);
          auto ids=result->output.value("ids",json::array());
          if(args.contains("target"))ids.push_back(args["target"]);
          for(const auto& id:ids)if(id.is_string()) {
            if(const auto* f=working->scene.feature(id.get<std::string>());f && (name=="feature" || name=="feature_edit")) {
              result->output["feature_id"]=f->id;result->output["body_ids"]=json::array();
              for(const auto& body:f->result.value("bodies",json::array()))result->output["body_ids"].push_back(body.at("id"));
            }
            if(const auto* sk=working->scene.sketch(id.get<std::string>());sk)result->output["sketch_id"]=sk->id;
          }
        }
        if(working->scene.unresolved.size()>source->scene.unresolved.size()){  // name them (gap log #12)
          std::string reasons;int fresh=0;
          for(const auto& u:working->scene.unresolved){
            bool before=false;for(const auto& v:source->scene.unresolved)if(v.op_id==u.op_id && v.reason==u.reason){before=true;break;}
            if(before)continue;if(fresh++<5)reasons+=(reasons.empty()?"":"; ")+(u.reason.empty()?u.op_type+" "+u.op_id:u.reason);
          }
          throw opad::Error("The edit introduced unresolved operations; document unchanged: "+reasons+(fresh>5?" (and "+std::to_string(fresh-5)+" more)":""));
        }
        for(const auto& feature:working->scene.features)if(!feature.error.empty()){
          auto* before=source->scene.feature(feature.id);if(!before || before->error!=feature.error)throw opad::Error(feature.error);
        }
        result->delta=changes(baseline->scene,working->scene);
        result->delta["scope"]=transaction.empty()?"command":"transaction";
        TopoDS_Compound compound;BRep_Builder builder;builder.MakeCompound(compound);bool any=false;int checkedSolids=0;std::set<std::string> checked;
        result->delta["references"]=json::array();
        for(const auto& [id,node]:working->scene.nodes){
          if(p.cancelled())throw opad::Error("cancelled");if(node.kind!=opad::Node::Kind::Body)continue;
          const auto* prior=baseline->scene.node(id);if(prior && prior->body_key==node.body_key && baseline->scene.world(id).to_json()==working->scene.world(id).to_json())continue;
          auto shape=opad::node_world_shape(*working->doc,working->scene,id);
          if(node.representation=="solid"){
            if(!batchValid.count(node.body_key) && !BRepCheck_Analyzer(shape).IsValid())throw opad::Error("Generated body failed solid validation; document unchanged.");++checkedSolids;checked.insert(id);
          }
          if(result->delta["references"].size()<100)result->delta["references"].push_back(reference_token(*working->doc,working->scene,opad::Ref::from_json(json{{"body",id},{"kind","body"}})));
          if(preview || !transaction.empty()){
            shape=BRepBuilderAPI_Copy(shape,true,false).Shape();BodyPrs::meshForDisplay(shape,0.1);builder.Add(compound,shape);any=true;
            if(prior)result->hidden.push_back(id);
          }
        }
        if(any){Bnd_Box box;BRepBndLib::Add(compound,box);result->preview=compound;result->prs=BodyPrs::build(compound,box);}
        result->delta["validation"]={{"changed_solid_bodies_checked",checkedSolids},{"valid",true},{"unresolved",working->scene.unresolved.size()}};
        // What this command (not the whole transaction) did to each body it made, changed or moved (TODO 10 B3): a tight
        // box, the volume and validity, so an agent needs no info + validate round trip after every step.
        json bodies=json::array();size_t changedBodies=0;
        for(const auto& [id,node]:working->scene.nodes){
          if(node.kind!=opad::Node::Kind::Body)continue;
          const auto* before=source->scene.node(id);
          if(before && before->body_key==node.body_key && source->scene.world(id).to_json()==working->scene.world(id).to_json())continue;
          if(++changedBodies>100)continue;
          if(p.cancelled())throw opad::Error("cancelled");
          json entry={{"id",id},{"name",node.name}};
          if(node.body_missing){entry["valid"]=false;bodies.push_back(entry);continue;}
          const auto shape=opad::node_world_shape(*working->doc,working->scene,id);
          entry["bbox"]=opad::bbox_to_json(opad::node_tight_bbox(*working->doc,working->scene,id));
          if(node.representation=="solid"){
            entry["volume_mm3"]=opad::volume_properties(shape).mass;
            entry["valid"]=checked.count(id)?true:BRepCheck_Analyzer(shape).IsValid();
          }else entry["representation"]=node.representation;
          bodies.push_back(entry);
        }
        result->delta["bodies"]=bodies;
        if(changedBodies>bodies.size())result->delta["bodies_total"]=changedBodies;
        if(kind!=agentsay::Kind::Hidden)result->said=agentsay::done(name,args,result->output,source->scene,working->scene,display);  // before the compact form
        if(compact) {
          // TODO 10 B11: this command's own changes (not the transaction's cumulative lists) as counts and tops, references
          // without signatures, no batch-wide operation_ids (every step receipt has its own), step results compact.
          result->delta=compactChanges(source->scene,working->scene,result->delta);
          if(result->output.is_object()){
            if(name=="model_batch"){
              result->output.erase("operation_ids");result->steps=result->output.value("steps",json::array());
              for(auto& step:result->output["steps"])if(step.contains("result"))step["result"]=compactResult(step["result"],working->scene);
            }else result->output=compactResult(result->output,working->scene);
          }
          std::function<void(json&)> unsign=[&](json& v){
            if(v.is_object()){if(v.contains("ref") && v.contains("geometry"))v.erase("signature");for(auto& [k,c]:v.items())unsign(c);}
            else if(v.is_array())for(auto& c:v)unsign(c);
          };
          unsign(result->output);
        }
      }
      if(!(write && name!="export") && kind!=agentsay::Kind::Hidden)result->said=agentsay::done(name,args,result->output,source->scene,source->scene,display);
      // The isolated acceptance harness can emulate an uninterruptible kernel tail.
      // Its result must never commit after Stop, disconnect, access revocation or a manual edit.
      if(write && delay>0)QThread::msleep(static_cast<unsigned long>(delay));
    },[this,session,result,name,receipt,write,preview,transaction,epoch,revision,started,client,kind](bool ok,const QString& error){
      if(epoch!=m_epoch || !session->socket || !session->bound){fail(session,"cancelled",tr("Agent operation cancelled."),receipt);release(result);return;}
      m_busy=false;m_job.clear();
      if(!ok){
        if(name=="model_batch" && error!="cancelled" && result->output.is_object()){
          for(auto& step:result->output["steps"])if(step.value("state","")!="failed")step["state"]="discarded";  // the failed step keeps its error
          m_receipts[receipt].state=error=="cancelled"?"cancelled":"failed";
          auto failure=live_error("batch_failed",error.toStdString());failure["structuredContent"]["state"]=m_receipts[receipt].state;
          failure["structuredContent"]["revision"]=revision;failure["structuredContent"]["result"]=result->output;
          reply(session,std::move(failure),receipt);failed("batch_failed",error);
        }else fail(session,error=="cancelled"?"cancelled":"invalid_operation",error,receipt);
        release(result);return;
      }
      if(session->target!=target() || m_doc->contentRevision!=revision || (write && (!canEdit() || editorBusy()))){fail(session,"stale_revision",tr("The document or access changed. The computed edit was discarded."),receipt);release(result);return;}
      if(write && name!="export"){
        auto prepared=std::make_shared<Prepared>();prepared->snapshot=result->snapshot;prepared->id=newId();prepared->label=QString::fromStdString(name);prepared->owner=session->socket;prepared->transaction=!transaction.empty();prepared->result=std::move(result->output);prepared->changes=std::move(result->delta);
        if(name=="model_batch"){  // later batches on this connection may refer to these steps
          auto next=std::make_shared<BatchSteps>(session->steps?*session->steps:BatchSteps{});
          for(const auto& step:result->steps.is_array()?result->steps:prepared->result.value("steps",json::array()))(*next)[step.at("id").get<std::string>()]=step.value("result",json::object());
          session->steps=next;
        }
        prepared->said=result->said;
        if(prepared->transaction){prepared->id=m_prepared->id;prepared->label=m_prepared->label;prepared->receipts=m_prepared->receipts;prepared->steps=m_prepared->steps;prepared->event=m_prepared->event;}
        if(name=="model_batch")prepared->steps<<result->stepSentences;else if(prepared->transaction)prepared->steps<<result->said.text;
        prepared->receipts.push_back(receipt);
        clearPrepared(!prepared->transaction,!prepared->transaction);m_prepared=prepared;
        const auto runningRow=std::exchange(m_running,0);
        if(prepared->transaction){  // the step goes under its task's row
          if(auto* g=row(prepared->event)){
            g->children=prepared->steps;g->second=tr("Preview, not applied yet · %1").arg(agentsay::stepsSoFar(int(prepared->steps.size())));
            for(const auto& id:result->said.subjects)if(std::find(g->subjects.begin(),g->subjects.end(),id)==g->subjects.end())g->subjects.push_back(id);
            post(*g);
          }
          if(runningRow && m_feed)m_feed->remove(runningRow);
        }else if(preview){  // a preview: its row waits, with Show and Discard
          ActivityEvent e;if(auto* r=row(runningRow))e=*r;
          e.kind=ActivityEvent::Group;e.state=ActivityEvent::Staged;e.text=result->said.text;e.icon=result->said.icon;e.client=client;
          e.second=tr("Preview, not applied yet");e.subjects=result->said.subjects;e.children=result->stepSentences;e.canShow=true;
          m_staged=prepared->event=post(e);
        }else prepared->event=runningRow;
        if(preview || prepared->transaction){
          if(!result->preview.IsNull())m_viewport->setPreparedPreview(result->preview,result->prs,result->hidden);
          m_receipts[receipt].state="staged";auto out=live_result({{"state","staged"},{"revision",revision},{prepared->transaction?"transaction":"preview_id",prepared->id},{"result",prepared->result},{"changes",prepared->changes},{"elapsed_ms",started->elapsed()}});
          reply(session,std::move(out),receipt);activity(tr("Preview ready; document unchanged."));
        }else commit(session,prepared->id,receipt,revision);
      }else {
        if(name=="live_select"){
          std::vector<opad::Ref> refs;for(const auto& value:result->output.at("selection_requested"))refs.push_back(opad::Ref::from_json(value));
          const auto kind=refs.front().kind;
          if(kind==opad::Ref::Kind::Body){std::vector<std::string> ids;for(const auto& r:refs)ids.push_back(r.body);m_viewport->selectNodes(ids);}
          else {
            connect(m_viewport,&Viewport::filterApplied,this,[this,refs,revision,epoch]{if(m_doc->contentRevision==revision && epoch==m_epoch)m_viewport->selectRefs(refs);},Qt::SingleShotConnection);
            m_viewport->setSelectionFilter(kind==opad::Ref::Kind::Face?Viewport::SelFilter::Face:kind==opad::Ref::Kind::Edge?Viewport::SelFilter::Edge:Viewport::SelFilter::Vertex);
          }
        }
        if(write)m_receipts[receipt].state="committed";
        // A read's row: folded with the other lookups, or done in place (a look keeps its picture).
        if(kind!=agentsay::Kind::Hidden){
          const auto runningRow=std::exchange(m_running,0);
          const auto& said=result->said;
          if(said.kind==agentsay::Kind::Fold){if(runningRow && m_feed)m_feed->remove(runningRow);addRead(said.text,client);}
          else {
            ActivityEvent e;if(auto* r=row(runningRow))e=*r;
            e.kind=said.kind==agentsay::Kind::Look?ActivityEvent::Look:said.kind==agentsay::Kind::Note?ActivityEvent::Note:said.kind==agentsay::Kind::Quiet?ActivityEvent::Quiet:ActivityEvent::Result;
            e.state=ActivityEvent::Done;e.text=said.text;e.second=said.second;e.icon=said.icon;e.subjects=said.subjects;e.issue=said.issue;e.client=client;
            e.ms=started->elapsed();e.canShow=!said.subjects.empty();
            if(!result->thumb.isNull()){e.thumb=QPixmap::fromImage(result->thumb);e.picture=QPixmap::fromImage(result->picture);}
            post(e);
            // Only the last ten pictures are kept.
            if(m_feed && !result->thumb.isNull()){
              int kept=0;
              for(auto it=m_feed->events().rbegin();it!=m_feed->events().rend();++it)if(!it->thumb.isNull() && ++kept>10){auto old=*it;old.thumb=QPixmap();old.picture=QPixmap();m_feed->post(old);break;}
            }
          }
        }
        json out={{"state",write?"committed":"read"},{"revision",revision},{"result",std::move(result->output)},{"elapsed_ms",started->elapsed()}};
        auto response=live_result(out);
        if(name=="viewport_image"){
          // Keep binary data out of the text context; MCP image content is inspectable by the client.
          auto image=out["result"]["image"].get<std::string>();out["result"].erase("image");response=live_result(out);
          response["content"].push_back({{"type","image"},{"mimeType","image/png"},{"data",std::move(image)}});
        }
        reply(session,std::move(response),receipt);activity(tr("Completed: %1 (%2 ms)").arg(QString::fromStdString(name)).arg(started->elapsed()));
      }
      trace::log(QString("agent: %1 %2 ms revision %3").arg(QString::fromStdString(name)).arg(started->elapsed()).arg(m_doc->contentRevision));release(result);emit statusChanged();
    });
    m_job=job;
  };
  if(!transaction.empty() || !previewId.empty())ready(m_prepared->snapshot,{});else snapshot(std::move(ready));
}
void AgentBridge::commit(const std::shared_ptr<Session>& session,const std::string& id,const std::string& receipt,unsigned long long revision){
  if(!m_prepared || m_prepared->id!=id || m_prepared->owner!=session->socket){fail(session,"unknown_prepared",tr("No matching prepared operation belongs to this connection."),receipt);return;}
  if(m_prepared->snapshot->revision!=revision || m_doc->contentRevision!=revision || editorBusy() || !canEdit() || !m_enabled){m_dropReason=tr("Dropped: the design changed first. Nothing changed.");clearPrepared();fail(session,"stale_revision",tr("The document changed. Prepare the operation again."),receipt);return;}
  auto prepared=std::move(m_prepared);m_viewport->clearPreviewBodies();
  const auto& ids=prepared->snapshot->baseIds;
  AppDocument::SnapshotBase base{revision,prepared->snapshot->docRevision,ids?*ids:std::vector<std::string>{}};
  rebaseCommit(session,std::move(prepared),receipt,std::move(base),0);
}
// The user hid, showed or coloured something since the copy: the agent's document takes those, its ops after them, and
// its scene is resolved again on a worker (it scales with the model: seconds on the Engine). A look made meanwhile
// starts it over; a content change refuses it.
void AgentBridge::rebaseCommit(const std::shared_ptr<Session>& session,std::shared_ptr<Prepared> prepared,const std::string& receipt,
                               AppDocument::SnapshotBase base,int attempt){
  std::optional<AppDocument::LooksSince> looks;
  try{looks=m_doc->looksSince(base);}catch(const std::exception& e){failCommit(session,std::move(prepared),receipt,QString::fromUtf8(e.what()));return;}
  if(!looks){applyCommit(session,std::move(prepared),receipt,base);return;}
  if(attempt>=5){failCommit(session,std::move(prepared),receipt,tr("stale_revision: the view kept changing while the agent's change was applied"));return;}
  m_busy=true;m_owner=session->socket;const auto epoch=++m_epoch;
  auto rebased=std::make_shared<AppDocument::SnapshotBase>(std::move(base));
  auto scene=std::make_shared<opad::Scene>();
  auto moved=std::make_shared<AppDocument::LooksSince>(std::move(*looks));
  m_jobs->backgroundNext();
  m_job=m_jobs->async(tr("Applying the agent's change"),[doc=prepared->snapshot->doc,rebased,scene,moved,hold=m_benchRebaseHold](Progress p){
      AppDocument::rebase(*doc,*rebased,std::move(*moved));
      *scene=opad::resolve(*doc);
      if(hold>0)QThread::msleep(static_cast<unsigned long>(hold));  // the bench's window for a look made meanwhile
      if(p.cancelled())throw opad::Error("cancelled");
    },[this,session,prepared,receipt,rebased,scene,attempt,epoch](bool ok,const QString& error)mutable{
      // The documents and scenes go to a worker to be freed (release), never here.
      if(epoch!=m_epoch || !session->socket || !session->bound){fail(session,"cancelled",tr("Agent operation cancelled."),receipt);release(std::move(prepared));release(std::move(scene));return;}
      m_busy=false;m_job.clear();m_owner.clear();
      if(!ok){release(std::move(scene));failCommit(session,std::move(prepared),receipt,error=="cancelled"?tr("Agent operation cancelled."):error);return;}
      ++m_rebasedCommits;
      std::swap(prepared->snapshot->scene,*scene);release(std::move(scene));  // the scene without the user's looks
      rebaseCommit(session,std::move(prepared),receipt,std::move(*rebased),attempt+1);  // at once when nothing moved meanwhile
    });
  emit statusChanged();
}
void AgentBridge::failCommit(const std::shared_ptr<Session>& session,std::shared_ptr<Prepared> prepared,const std::string& receipt,const QString& error){
  m_committing=false;
  m_running=prepared->event;  // failed() turns the call's row (running, or the staged preview or task) into the failure
  if(prepared->event==m_staged){m_staged=0;m_group=0;}
  fail(session,"commit_failed",error,receipt);
  release(std::move(prepared));emit statusChanged();
}
void AgentBridge::applyCommit(const std::shared_ptr<Session>& session,std::shared_ptr<Prepared> prepared,const std::string& receipt,
                              const AppDocument::SnapshotBase& base){
  if(editorBusy() || !canEdit() || !m_enabled){failCommit(session,std::move(prepared),receipt,tr("stale_revision: the document or access changed while the agent's change was applied"));return;}
  try{
    if(!m_follow)m_viewport->setCameraJson(m_viewport->cameraJson()); // cancel an earlier pending load-fit
    // The undo step reads as what was done: "Claude: Filleted 4 edges of Bracket, 2 mm", a task by its label.
    const QString client=who(session).brief;
    QString done=prepared->transaction?tr("%1 (%2)").arg(agentsay::plain(agentsay::quote(prepared->label,48)),agentsay::changesCount(int(prepared->steps.size()))):agentsay::plain(prepared->said.text);
    if(done.isEmpty())done=prepared->label;
    if(done.size()>60)done=done.left(59)+QChar(0x2026);
    const size_t opsBefore=m_doc->doc.ops.size();  // what follows is what this commit made (its row lists them all)
    m_committing=true;m_doc->commitRebased(*prepared->snapshot->doc,prepared->snapshot->scene,base,tr("%1: %2").arg(client,done),"agent");m_committing=false;
    if(opsBefore<m_doc->doc.ops.size()){  // undo / redo replies keep naming it as before
      m_replyLabels.emplace_back(m_doc->doc.ops[opsBefore].id,tr("Agent: %1").arg(prepared->label));
      if(m_replyLabels.size()>100)m_replyLabels.pop_front();
    }
    m_receipts[receipt].state="committed";
    m_receipts[receipt].revision=m_doc->contentRevision;
    for(const auto& key:prepared->receipts){m_receipts[key].state="committed";m_receipts[key].revision=m_doc->contentRevision;}
    auto change=prepared->changes;change["revision"]=m_doc->contentRevision;change["agent"]=session->agent.toStdString();m_changes.push_back(change);if(m_changes.size()>20)m_changes.erase(m_changes.begin());
    reply(session,live_result({{"state","committed"},{"revision",m_doc->contentRevision},{"result",prepared->result},{"changes",change}}),receipt);
    if(m_follow && prepared->changes.contains("geometry") && !prepared->changes["geometry"].empty())m_viewport->fitNodesWhenReady(prepared->changes["geometry"].get<std::vector<std::string>>());
    activity(tr("Applied: %1 — Undo is available").arg(prepared->label));
    {  // its row, done: what changed, Show, and Undo while it is the next step Undo takes back
      ActivityEvent e;if(auto* r=row(prepared->event))e=*r;
      const auto& said=prepared->said;
      std::vector<std::string> subjects=said.subjects;
      if(subjects.empty() && prepared->changes.contains("geometry"))for(const auto& id:prepared->changes["geometry"])if(id.is_string())subjects.push_back(id.get<std::string>());
      if(prepared->transaction){
        e.kind=ActivityEvent::Group;e.icon=QStringLiteral("commit");e.second=agentsay::changesCount(int(prepared->steps.size()));e.children=prepared->steps;
        for(const auto& id:e.subjects)if(std::find(subjects.begin(),subjects.end(),id)==subjects.end())subjects.push_back(id);
      }else {
        e.kind=said.kind==agentsay::Kind::Note?ActivityEvent::Note:said.kind==agentsay::Kind::Group?ActivityEvent::Group:ActivityEvent::Change;
        e.text=said.text;e.second=said.second;e.icon=said.icon;e.children=prepared->steps;
      }
      e.state=ActivityEvent::Applied;e.subjects=subjects;e.client=client;e.canShow=true;e.issue=false;
      e.op=!said.op.empty() && m_doc->doc.find_op(said.op)?said.op:(m_doc->doc.ops.empty()?std::string():m_doc->doc.ops.back().id);
      e.ops.clear();for(size_t i=std::min(opsBefore,m_doc->doc.ops.size());i<m_doc->doc.ops.size();++i)e.ops.push_back(m_doc->doc.ops[i].id);
      e.canUndo=m_doc->undoTopHas(e.op);e.ms=e.at.msecsTo(QDateTime::currentDateTime());
      post(e);
      if(prepared->event==m_staged){m_staged=0;m_group=0;}
      if(!m_panel || !m_panel->isVisible())++m_unseen;
      refreshRows();
    }
  }catch(const std::exception& e){failCommit(session,std::move(prepared),receipt,QString::fromUtf8(e.what()));return;}
  release(std::move(prepared));emit statusChanged();
}
