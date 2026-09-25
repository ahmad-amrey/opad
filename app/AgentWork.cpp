#include "AgentBridge.hpp"
#include "DesignController.hpp"
#include "Viewport.hpp"
#include "opad/live.hpp"
#include "opad/geometry.hpp"
#include "opad/render.hpp"
#include "opad/inspect.hpp"
#include <QLocalSocket>
#include <QUuid>
#include <QThread>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>
using opad::json;
using namespace opad::agent;
namespace {
std::string newId(){return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();}
void release(std::shared_ptr<void> value){auto* thread=QThread::create([value=std::move(value)]{});QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();}
void checkReferences(const opad::Document& doc,const opad::Scene& scene,const json& args){
  std::set<std::string> checked;
  for(const auto& token:args.value("references",json::array())){
    if(resolve_reference(doc,scene,token).value("status","")!="resolved")throw opad::Error("stale_reference: request a fresh selection or resolve_reference before editing");
    checked.insert(opad::Ref::from_json(token.at("ref")).to_json().dump());
  }
  std::function<void(const json&)> walk=[&](const json& value){
    bool isRef=false;
    if(value.is_string()){const auto& text=value.get_ref<const std::string&>();isRef=text.find("/face/")!=text.npos || text.find("/edge/")!=text.npos || text.find("/vertex/")!=text.npos || text.find("/center/")!=text.npos;}
    if(value.is_object() && value.contains("body")){const auto kind=value.value("kind","body");isRef=kind!="body" && kind!="point";}
    if(isRef){auto ref=opad::Ref::from_json(value);if(!checked.count(ref.to_json().dump()))throw opad::Error("unchecked_reference: supply entity_details.reference in references for every subshape input");return;}
    if(value.is_object()){for(const auto& [key,child]:value.items())if(key!="references")walk(child);}
    else if(value.is_array())for(const auto& child:value)walk(child);
  };walk(args);
}
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
}
void AgentBridge::snapshot(std::function<void(std::shared_ptr<Snapshot>,QString)> done){
  if(m_cache && m_cache->revision==m_doc->revision){done(m_cache,{});return;}
  const auto revision=m_doc->revision,generation=m_doc->generation;QPointer<AgentBridge> self=this;
  if(!m_doc->captureSnapshot(m_jobs,[self,revision,generation,done](std::shared_ptr<opad::Document> copy,const QString& error){
    if(!self)return;if(!copy){done({},error);return;}
    auto value=std::make_shared<Snapshot>();value->doc=std::move(copy);value->revision=revision;
    self->m_jobs->async(tr("Preparing agent context"),[value](Progress p){value->scene=opad::resolve(*value->doc);if(p.cancelled())throw opad::Error("cancelled");},
      [self,value,done,generation](bool ok,const QString& error){
        if(!self)return;if(!ok){done({},error);return;}
        if(generation!=self->m_doc->generation || value->revision!=self->m_doc->revision){done({},tr("Document changed while preparing context. Retry."));return;}
        self->m_cache=value;done(value,{});
      });
  }))done({},tr("Document is busy. Retry after the current operation."));
}
void AgentBridge::save(const std::shared_ptr<Session>& session,const json& args,const std::string& receipt){
  if(m_prepared){fail(session,"prepared_active",tr("Commit or cancel the current transaction first."),receipt);return;}
  const auto revision=m_doc->revision,epoch=++m_epoch;
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
        activity(tr("Saved %1").arg(m_doc->path()));emit statusChanged();
      },m_benchDelay);
  }catch(const std::exception& e){m_busy=false;m_owner.clear();fail(session,"save_failed",QString::fromUtf8(e.what()),receipt);emit statusChanged();}
}
void AgentBridge::execute(const std::shared_ptr<Session>& session,std::string name,json args,const std::string& receipt){
  if(m_busy){fail(session,"busy",tr("An agent operation is still running. Wait or use Stop."),receipt);return;}
  const bool write=live_mutation(name),preview=args.value("preview",false);const auto transaction=args.value("transaction","");
  const auto previewId=args.value("preview_id","");
  if(!previewId.empty() && (!m_prepared || m_prepared->id!=previewId || m_prepared->owner!=session->socket)){
    fail(session,"unknown_preview",tr("No matching prepared operation belongs to this connection."));return;
  }
  if(!transaction.empty() && (!m_prepared || !m_prepared->transaction || m_prepared->id!=transaction || m_prepared->owner!=session->socket)){
    fail(session,"unknown_transaction",tr("No matching transaction belongs to this connection. Disconnect discards staged work. Read current context, begin a new transaction and replan with new request IDs."),receipt);return;
  }
  if(write && transaction.empty() && m_prepared && m_prepared->transaction){fail(session,"transaction_active",tr("Commit or cancel the current transaction first."),receipt);return;}
  if(name=="export" && (preview || !transaction.empty())){fail(session,"invalid_export",tr("Commit the design before exporting it."),receipt);return;}
  const auto state=liveState();
  m_busy=true;m_owner=session->socket;const auto epoch=++m_epoch,revision=m_doc->revision;
  const auto started=std::make_shared<QElapsedTimer>();started->start();activity(tr("Agent: %1").arg(QString::fromStdString(name)));
  auto ready=[this,session,name,args=std::move(args),receipt,write,preview,transaction,epoch,revision,state,started](std::shared_ptr<Snapshot> source,const QString& error){
    if(epoch!=m_epoch || !session->socket || !session->bound){fail(session,"cancelled",tr("Agent operation cancelled."),receipt);return;}
    if(!source){m_busy=false;fail(session,"snapshot_failed",error,receipt);return;}
    if(name=="transaction_begin"){
      m_prepared=std::make_shared<Prepared>();m_prepared->snapshot=source;m_prepared->id=newId();m_prepared->label=QString::fromStdString(args.at("label").get<std::string>());m_prepared->owner=session->socket;m_prepared->transaction=true;
      m_prepared->receipts.push_back(receipt);
      m_busy=false;m_receipts[receipt].state="staged";reply(session,live_result({{"state","staged"},{"transaction",m_prepared->id},{"base_revision",revision},{"revision",revision},{"lifetime",transaction_policy()}}),receipt);return;
    }
    struct Result {std::shared_ptr<Snapshot> snapshot;json output,delta;TopoDS_Shape preview;std::shared_ptr<const BodyPrs> prs;std::vector<std::string> hidden;};auto result=std::make_shared<Result>();
    const auto baseline=!transaction.empty() && m_cache?m_cache:source;
    auto job=m_jobs->async(tr("Agent: %1").arg(QString::fromStdString(name)),[source,baseline,result,args,name,write,preview,transaction,state,delay=m_benchDelay](Progress p)mutable{
      p.setPhase(tr("Inspecting inputs"));
      if(write)checkReferences(*source->doc,source->scene,args);
      for(const char* key:{"expected_revision","request_id","transaction","preview","references"})args.erase(key);
      if(write){result->snapshot=std::make_shared<Snapshot>();result->snapshot->doc=std::make_shared<opad::Document>(*source->doc);result->snapshot->revision=source->revision;args["by"]="Agent";}
      auto working=write?result->snapshot:source;
      if(p.cancelled())throw opad::Error("cancelled");
      p.setPhase(tr("Computing geometry and context"));
      if(name=="live_state"){
        result->output=state;auto& refs=result->output["selection"];for(auto& r:refs){
          auto ref=opad::Ref::from_json(r);if(source->scene.node(ref.body))r=entity_details(*source->doc,source->scene,{{"ref",r},{"limit",10}});
        }
      }else if(name=="context")result->output=context(*source->doc,source->scene,args);
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
        options.fit=args.value("fit",false)||args.contains("view");options.camera=options.fit?opad::Camera::preset(args.value("view","iso")):opad::Camera::from_json(state.at("camera"));
        const auto png=opad::encode_png(opad::render_scene(*source->doc,source->scene,options));
        result->output={{"image",QByteArray::fromStdString(png).toBase64().toStdString()},{"camera",options.camera.to_json()},{"selection",state["selection"]},{"preview_id",args.value("preview_id","")},{"rendering","software geometry view; UI overlays are not included"}};
      }else result->output=opad::commands::run(name,args,working->doc.get());
      if(p.cancelled())throw opad::Error("cancelled");
      if(write && name!="export"){
        p.setPhase(tr("Validating design"));working->scene=opad::resolve(*working->doc);
        if(result->output.is_object()) {
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
        if(working->scene.unresolved.size()>source->scene.unresolved.size())throw opad::Error("The edit introduced unresolved operations; document unchanged.");
        for(const auto& feature:working->scene.features)if(!feature.error.empty()){
          auto* before=source->scene.feature(feature.id);if(!before || before->error!=feature.error)throw opad::Error(feature.error);
        }
        result->delta=changes(baseline->scene,working->scene);
        result->delta["scope"]=transaction.empty()?"command":"transaction";
        TopoDS_Compound compound;BRep_Builder builder;builder.MakeCompound(compound);bool any=false;int checkedSolids=0;
        result->delta["references"]=json::array();
        for(const auto& [id,node]:working->scene.nodes){
          if(p.cancelled())throw opad::Error("cancelled");if(node.kind!=opad::Node::Kind::Body)continue;
          const auto* prior=baseline->scene.node(id);if(prior && prior->body_key==node.body_key && baseline->scene.world(id).to_json()==working->scene.world(id).to_json())continue;
          auto shape=opad::node_world_shape(*working->doc,working->scene,id);
          if(node.representation=="solid"){
            if(!BRepCheck_Analyzer(shape).IsValid())throw opad::Error("Generated body failed solid validation; document unchanged.");++checkedSolids;
          }
          if(result->delta["references"].size()<100)result->delta["references"].push_back(reference_token(*working->doc,working->scene,opad::Ref::from_json(json{{"body",id},{"kind","body"}})));
          if(preview || !transaction.empty()){
            shape=BRepBuilderAPI_Copy(shape,true,false).Shape();BRepMesh_IncrementalMesh(shape,0.1,false,0.4,false);builder.Add(compound,shape);any=true;
            if(prior)result->hidden.push_back(id);
          }
        }
        if(any){Bnd_Box box;BRepBndLib::Add(compound,box);result->preview=compound;result->prs=BodyPrs::build(compound,box);}
        result->delta["validation"]={{"changed_solid_bodies_checked",checkedSolids},{"valid",true},{"unresolved",working->scene.unresolved.size()}};
      }
      // The isolated acceptance harness can emulate an uninterruptible kernel tail.
      // Its result must never commit after Stop, disconnect, access revocation or a manual edit.
      if(write && delay>0)QThread::msleep(static_cast<unsigned long>(delay));
    },[this,session,result,name,receipt,write,preview,transaction,epoch,revision,started](bool ok,const QString& error){
      if(epoch!=m_epoch || !session->socket || !session->bound){fail(session,"cancelled",tr("Agent operation cancelled."),receipt);release(result);return;}
      m_busy=false;m_job.clear();
      if(!ok){fail(session,error=="cancelled"?"cancelled":"invalid_operation",error,receipt);release(result);return;}
      if(session->target!=target() || m_doc->revision!=revision || (write && (!m_edit || editorBusy()))){fail(session,"stale_revision",tr("The document or access changed. The computed edit was discarded."),receipt);release(result);return;}
      if(write && name!="export"){
        auto prepared=std::make_shared<Prepared>();prepared->snapshot=result->snapshot;prepared->id=newId();prepared->label=QString::fromStdString(name);prepared->owner=session->socket;prepared->transaction=!transaction.empty();prepared->result=std::move(result->output);prepared->changes=std::move(result->delta);
        if(prepared->transaction){prepared->id=m_prepared->id;prepared->label=m_prepared->label;prepared->receipts=m_prepared->receipts;}
        prepared->receipts.push_back(receipt);
        clearPrepared(!prepared->transaction);m_prepared=prepared;
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
            connect(m_viewport,&Viewport::filterApplied,this,[this,refs,revision,epoch]{if(m_doc->revision==revision && epoch==m_epoch)m_viewport->selectRefs(refs);},Qt::SingleShotConnection);
            m_viewport->setSelectionFilter(kind==opad::Ref::Kind::Face?Viewport::SelFilter::Face:kind==opad::Ref::Kind::Edge?Viewport::SelFilter::Edge:Viewport::SelFilter::Vertex);
          }
        }
        if(write)m_receipts[receipt].state="committed";
        json out={{"state",write?"committed":"read"},{"revision",revision},{"result",std::move(result->output)},{"elapsed_ms",started->elapsed()}};
        auto response=live_result(out);
        if(name=="viewport_image"){
          // Keep binary data out of the text context; MCP image content is inspectable by the client.
          auto image=out["result"]["image"].get<std::string>();out["result"].erase("image");response=live_result(out);
          response["content"].push_back({{"type","image"},{"mimeType","image/png"},{"data",std::move(image)}});
        }
        reply(session,std::move(response),receipt);activity(tr("Completed: %1 (%2 ms)").arg(QString::fromStdString(name)).arg(started->elapsed()));
      }
      trace::log(QString("agent: %1 %2 ms revision %3").arg(QString::fromStdString(name)).arg(started->elapsed()).arg(m_doc->revision));release(result);emit statusChanged();
    });
    m_job=job;connect(job,&Job::phaseChanged,this,[this](const QString& phase,int percent){if(m_panel)m_panel->setContext(percent<0?phase:phase+QString(" (%1%)").arg(percent));});
  };
  if(!transaction.empty() || !previewId.empty())ready(m_prepared->snapshot,{});else snapshot(std::move(ready));
}
void AgentBridge::commit(const std::shared_ptr<Session>& session,const std::string& id,const std::string& receipt,unsigned long long revision){
  if(!m_prepared || m_prepared->id!=id || m_prepared->owner!=session->socket){fail(session,"unknown_prepared",tr("No matching prepared operation belongs to this connection."),receipt);return;}
  if(m_prepared->snapshot->revision!=revision || m_doc->revision!=revision || editorBusy() || !m_edit || !m_enabled){clearPrepared();fail(session,"stale_revision",tr("The document changed. Prepare the operation again."),receipt);return;}
  auto prepared=std::move(m_prepared);m_viewport->clearPreviewBodies();
  try{
    if(!m_follow)m_viewport->setCameraJson(m_viewport->cameraJson()); // cancel an earlier pending load-fit
    m_committing=true;m_doc->commitSnapshot(*prepared->snapshot->doc,prepared->snapshot->scene,revision,tr("Agent: %1").arg(prepared->label));m_committing=false;
    m_receipts[receipt].state="committed";
    m_receipts[receipt].revision=m_doc->revision;
    for(const auto& key:prepared->receipts){m_receipts[key].state="committed";m_receipts[key].revision=m_doc->revision;}
    auto change=prepared->changes;change["revision"]=m_doc->revision;change["agent"]=session->agent.toStdString();m_changes.push_back(change);if(m_changes.size()>20)m_changes.erase(m_changes.begin());
    reply(session,live_result({{"state","committed"},{"revision",m_doc->revision},{"result",prepared->result},{"changes",change}}),receipt);
    if(m_follow && prepared->changes.contains("geometry") && !prepared->changes["geometry"].empty())m_viewport->fitNodesWhenReady(prepared->changes["geometry"].get<std::vector<std::string>>());
    activity(tr("Applied: %1 — Undo is available").arg(prepared->label));
  }catch(const std::exception& e){m_committing=false;fail(session,"commit_failed",QString::fromUtf8(e.what()),receipt);}
  release(prepared);emit statusChanged();
}
