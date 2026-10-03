#include "AgentBridge.hpp"
#include "DesignController.hpp"
#include "Viewport.hpp"
#include "opad/live.hpp"
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
#include <cctype>
using opad::json;
using namespace opad::agent;
namespace {
std::string newId(){return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();}
void release(std::shared_ptr<void> value){auto* thread=QThread::create([value=std::move(value)]{});QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();}
// Every face/edge/vertex input needs a current token in `references`, or (TODO 10 B6) to be a reference this connection
// was given earlier whose body still has the key and placement it had then.
void checkReferences(const opad::Document& doc,const opad::Scene& scene,const json& args,const AgentBridge::KnownRefs* known=nullptr){
  std::set<std::string> checked;
  for(const auto& token:args.value("references",json::array())){
    if(resolve_reference(doc,scene,token).value("status","")!="resolved")throw opad::Error("stale_reference: request a fresh selection or resolve_reference before editing");
    checked.insert(opad::Ref::from_json(token.at("ref")).to_json().dump());
  }
  std::function<void(const json&)> walk=[&](const json& value){
    bool isRef=false;
    if(value.is_string()){const auto& text=value.get_ref<const std::string&>();isRef=text.find("/face/")!=text.npos || text.find("/edge/")!=text.npos || text.find("/vertex/")!=text.npos || text.find("/center/")!=text.npos;}
    if(value.is_object() && value.contains("body")){const auto kind=value.value("kind","body");isRef=kind!="body" && kind!="point" && !value.contains("select");}  // a rule (B7) names no entity: it is resolved when computed
    if(isRef){
      auto ref=opad::Ref::from_json(value);const auto key=ref.to_json().dump();if(checked.count(key))return;
      if(known)if(auto it=known->find(key);it!=known->end()){
        const auto* node=scene.node(ref.body);
        if(node && node->body_key==it->second.geometry && scene.world(ref.body).to_json().dump()==it->second.placement)return;
        throw opad::Error("stale_reference: "+key+" was given for an earlier state of its body; request it again (entity_details or query_entities)");
      }
      throw opad::Error("unchecked_reference: supply entity_details.reference in references for every subshape input, or use a reference this connection was given by entity_details, query_entities or the selection");
    }
    if(value.is_object()){for(const auto& [key,child]:value.items())if(key!="references")walk(child);}
    else if(value.is_array())for(const auto& child:value)walk(child);
  };walk(args);
}
// A small declarative language over existing typed commands, never executable code.
void modelBatch(opad::Document& doc,const json& args,Progress progress,json& output,const AgentBridge::KnownRefs* known,const AgentBridge::BatchSteps* before){
  static const std::set<std::string> allowed={"component","param","sketch","sketch_edit","feature","feature_edit","rename","reparent","appearance","transform"};
  std::map<std::string,json> schemas,results;std::set<std::string> earlier,defined;
  for(const auto& step:args.at("steps"))if(step.contains("id") && step["id"].is_string())defined.insert(step["id"].get<std::string>());
  // An id this batch defines is this batch's step (a later one is a forward reference); only others may name a step
  // of an earlier batch on this connection.
  auto older=[&](const std::string& id){return !defined.count(id) && before && before->count(id);};
  for(const auto& command:opad::commands::list())if(allowed.count(command.name))schemas[command.name]=command_schema(command,true);
  static const std::string syntax="Write @{<step id>#/<path in that step's result>}, for example @{cabin#/body_ids/0} (the first body made by the step with id \"cabin\"); @{cabin/body_ids/0} is the same. Paths start at the step's result, without /result/: feature steps have feature_id and body_ids, sketch steps sketch_id, component steps component_id.";
  // Step ids are [A-Za-z0-9_], so the first '#' or '/' ends the id; '#' may be followed by the pointer's '/' or not.
  auto symbol=[](const std::string& text)->std::pair<std::string,std::string>{
    if(!text.starts_with("@{") || !text.ends_with("}"))return {};
    const auto body=text.substr(2,text.size()-3);const auto end=body.find_first_of("#/");
    std::string id=body.substr(0,end),path=end==std::string::npos?std::string():body.substr(end+(body[end]=='#'?1:0));
    if(!path.empty() && path[0]!='/')path="/"+path;
    if(id.empty() || path.size()<2)throw opad::Error("Batch reference "+text+" needs a step id and a path. "+syntax);
    return {id,path};
  };
  auto keysOf=[](const json& value){std::string keys;for(const auto& [key,item]:value.items())keys+=(keys.empty()?"":", ")+key;return keys.empty()?std::string("none"):keys;};
  // Walks the pointer itself so a wrong path names the step and what it does have, not a raw JSON exception.
  auto lookup=[&](const std::string& text,const std::string& id,const std::string& path)->const json&{
    // This batch's steps first, then this connection's earlier batches (gap log #14).
    const json* at=results.count(id)?&results.at(id):&before->at(id);std::string where="step '"+id+"' result";
    for(size_t start=1;start<=path.size();){
      const auto end=std::min(path.find('/',start),path.size());std::string token=path.substr(start,end-start);start=end+1;
      for(size_t i=0;(i=token.find('~',i))!=std::string::npos;++i)token.replace(i,2,token.compare(i,2,"~1")==0?"/":"~");
      if(at->is_object()){
        if(!at->contains(token)){
          std::string example=at->contains("body_ids")?"@{"+id+"#/body_ids/0}":at->contains("sketch_id")?"@{"+id+"#/sketch_id}":at->contains("component_id")?"@{"+id+"#/component_id}":"";
          throw opad::Error("Batch reference "+text+": "+where+" has no '"+token+"'; its keys are "+keysOf(*at)+"."+(example.empty()?"":" For example "+example+".")+(token=="result"?" Paths start at the step's result, without /result/.":""));
        }
        at=&(*at)[token];
      }else if(at->is_array()){
        const bool digits=!token.empty() && token.size()<10 && std::all_of(token.begin(),token.end(),[](unsigned char c){return std::isdigit(c);});
        if(!digits || std::stoull(token)>=at->size())throw opad::Error("Batch reference "+text+": "+where+" is a list of "+std::to_string(at->size())+" item(s); '"+token+"' is not an index from 0 to "+std::to_string(at->size()?at->size()-1:0)+".");
        at=&(*at)[std::stoull(token)];
      }else throw opad::Error("Batch reference "+text+": "+where+" is "+at->dump()+", which has no '"+token+"'.");
      where+="/"+token;
    }
    return *at;
  };
  // @{pat#/body_ids/*} is the whole list: spliced in as an element of an array, or the array itself as a value.
  auto wholeList=[](const json& value){if(!value.is_string())return false;const auto& text=value.get_ref<const std::string&>();return text.starts_with("@{") && text.ends_with("/*}");};
  auto lookupList=[&](const std::string& text)->json{
    const auto one=text.substr(0,text.size()-3)+"}";const auto [id,path]=symbol(one);const json& list=lookup(text,id,path);
    if(!list.is_array())throw opad::Error("Batch reference "+text+": step '"+id+"' "+path.substr(1)+" is "+std::string(list.type_name())+" "+list.dump()+", not a list; /* takes every item of a list such as body_ids.");
    for(const auto& item:list)if(!item.is_string())throw opad::Error("Batch reference "+text+" holds "+item.dump()+", not identifier strings.");
    return list;
  };
  // Before anything runs, a whole-list reference standing for an array is checked as a list of one.
  std::function<void(json&)> listShaped=[&](json& value){
    if(value.is_object()){for(auto& [key,child]:value.items()){
      if(wholeList(child)){
        if(key=="target")throw opad::Error("Batch reference "+child.get<std::string>()+" is a whole list; use targets (an array) instead of target, or pick one item such as /0.");
        child=json::array({child});
      }else listShaped(child);
    }}else if(value.is_array())for(auto& child:value)if(!wholeList(child))listShaped(child);
  };
  std::function<void(const json&)> preflightRefs=[&](const json& value){
    if(value.is_string()){
      const auto text=value.get<std::string>();const auto [id,path]=symbol(text);
      if(!id.empty() && !earlier.count(id) && !older(id)){
        std::string steps;for(const auto& step:earlier)steps+=(steps.empty()?"":", ")+step;
        std::string older;if(before)for(const auto& [step,result]:*before)older+=(older.empty()?"":", ")+step;
        throw opad::Error("Batch reference "+text+" must name an earlier step of this batch or of an earlier batch on this connection; earlier steps are "+(steps.empty()?std::string("none"):steps)+(older.empty()?std::string():"; earlier batches' are "+older)+". "+syntax);
      }
    }else if(value.is_array() || value.is_object())for(const auto& child:value)preflightRefs(child);
  };
  // A batch-level parent is where every body its feature steps make goes, unless a step names its own (TODO 10 B14).
  const json batchParent=args.contains("parent")?args["parent"]:json();
  // Validate every command and dependency before computing any geometry.
  for(const auto& step:args.at("steps")){
    const auto id=step.at("id").get<std::string>(),command=step.at("command").get<std::string>();
    if(id.empty() || id.size()>64 || !std::all_of(id.begin(),id.end(),[](unsigned char c){return std::isalnum(c) || c=='_';}) || earlier.count(id))throw opad::Error("Batch step IDs must be unique letters, digits or underscores");
    if(!allowed.count(command))throw opad::Error("Unsupported batch command: "+command);
    auto input=step.at("arguments");listShaped(input);validate_input(schemas.at(command),input);
    if(command=="feature")validate_input(feature_schema(input.at("kind").get<std::string>()),input.value("inputs",json::object()));
    preflightRefs(input);preflightRefs(step.value("references",json::array()));
    if(command=="feature" && !input.contains("parent") && batchParent.is_string()){
      const auto [ref,path]=symbol(batchParent.get<std::string>());
      if(!ref.empty() && !earlier.count(ref) && !older(ref))throw opad::Error("The batch parent "+batchParent.get<std::string>()+" names step '"+ref+"', which does not come before feature step '"+id+"'; put that component step first.");
    }
    earlier.insert(id);
  }
  output={{"steps",json::array()},{"atomic",true},{"persistence","not_saved"}};
  std::function<void(json&)> expand=[&](json& value){
    if(wholeList(value)){value=lookupList(value.get<std::string>());return;}
    if(value.is_string()){
      const auto text=value.get<std::string>();const auto [id,path]=symbol(text);
      if(!id.empty()){
        value=lookup(text,id,path);
        if(!value.is_string())throw opad::Error("Batch reference "+text+" is "+std::string(value.type_name())+" "+value.dump()+", not an identifier string."+(value.is_array()?" Add an index, for example "+text.substr(0,text.size()-1)+"/0}, or /* for the whole list where a list is accepted.":""));
      }
    }else if(value.is_array()){
      json spliced=json::array();
      for(auto& child:value){
        if(wholeList(child)){for(const auto& item:lookupList(child.get<std::string>()))spliced.push_back(item);continue;}
        expand(child);spliced.push_back(std::move(child));
      }
      value=std::move(spliced);
    }else if(value.is_object())for(auto& child:value)expand(child);
  };
  for(const auto& step:args.at("steps")){
    const auto id=step.at("id").get<std::string>(),command=step.at("command").get<std::string>();
    output["failed_step"]=id;
    if(progress.cancelled())throw opad::Error("cancelled");
    auto input=step.at("arguments");
    if(command=="feature" && !input.contains("parent") && !batchParent.is_null())input["parent"]=batchParent;
    expand(input);validate_input(schemas.at(command),input);
    auto checked=input;checked["references"]=step.value("references",json::array());expand(checked["references"]);
    checkReferences(doc,opad::resolve(doc),checked,known);
    input["by"]="Agent";QElapsedTimer timer;timer.start();const auto begin=doc.ops.size();
    auto value=opad::commands::run(command,input,&doc);const auto scene=opad::resolve(doc);
    if(command=="feature" || command=="sketch")for(size_t i=begin;i<doc.ops.size();++i){
      const auto& op=doc.ops[i];
      if(command=="sketch" && op.type=="sketch")value["sketch_id"]=op.id;
      if(command=="feature" && op.type=="feature"){
        value["feature_id"]=op.id;value["body_ids"]=json::array();
        if(const auto* feature=scene.feature(op.id))for(const auto& body:feature->result.value("bodies",json::array()))value["body_ids"].push_back(body.at("id"));
      }
    }
    if(command=="component")value["component_id"]=value.at("id");
    results[id]=value;output["steps"].push_back({{"id",id},{"command",command},{"state","computed"},{"result",value},{"elapsed_ms",timer.elapsed()}});
  }
  output.erase("failed_step");
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
void AgentBridge::snapshot(std::function<void(std::shared_ptr<Snapshot>,QString)> done,int waited){
  if(m_cache && m_cache->revision==m_doc->revision){done(m_cache,{});return;}
  // Another capture or a save holds the document (a recovery checkpoint right after a commit): wait for it rather
  // than fail the call (gap log #3: "snapshot_failed: Document is busy" after every commit).
  if(m_doc->snapshotBusy() && waited<30000){
    QPointer<AgentBridge> self=this;
    QTimer::singleShot(20,this,[self,done=std::move(done),waited]()mutable{if(self)self->snapshot(std::move(done),waited+20);});
    return;
  }
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
  const auto state=liveState();
  m_busy=true;m_owner=session->socket;const auto epoch=++m_epoch,revision=m_doc->revision;
  const auto started=std::make_shared<QElapsedTimer>();started->start();activity(tr("Agent: %1").arg(QString::fromStdString(name)));
  auto ready=[this,session,name,args=std::move(args),receipt,write,preview,transaction,epoch,revision,state,started,known=session->known,steps=session->steps](std::shared_ptr<Snapshot> source,const QString& error){
    if(epoch!=m_epoch || !session->socket || !session->bound){fail(session,"cancelled",tr("Agent operation cancelled."),receipt);return;}
    if(!source){m_busy=false;fail(session,"snapshot_failed",error,receipt);return;}
    if(name=="transaction_begin"){
      m_prepared=std::make_shared<Prepared>();m_prepared->snapshot=source;m_prepared->id=newId();m_prepared->label=QString::fromStdString(args.at("label").get<std::string>());m_prepared->owner=session->socket;m_prepared->transaction=true;
      m_prepared->receipts.push_back(receipt);
      m_busy=false;m_receipts[receipt].state="staged";reply(session,live_result({{"state","staged"},{"transaction",m_prepared->id},{"base_revision",revision},{"revision",revision},{"lifetime",transaction_policy()}}),receipt);return;
    }
    struct Result {std::shared_ptr<Snapshot> snapshot;json output,delta;TopoDS_Shape preview;std::shared_ptr<const BodyPrs> prs;std::vector<std::string> hidden;};auto result=std::make_shared<Result>();
    const auto baseline=!transaction.empty() && m_cache?m_cache:source;
    auto job=m_jobs->async(tr("Agent: %1").arg(QString::fromStdString(name)),[source,baseline,result,args,name,write,preview,transaction,state,known,steps,delay=m_benchDelay](Progress p)mutable{
      p.setPhase(tr("Inspecting inputs"));
      if(write && name!="model_batch")checkReferences(*source->doc,source->scene,args,known.get());
      const bool compact=args.value("verbosity","full")=="compact";  // TODO 10 B11
      for(const char* key:{"expected_revision","request_id","transaction","preview","references","verbosity"})args.erase(key);
      if(write){result->snapshot=std::make_shared<Snapshot>();result->snapshot->doc=std::make_shared<opad::Document>(*source->doc);result->snapshot->revision=source->revision;args["by"]="Agent";}
      auto working=write?result->snapshot:source;
      if(p.cancelled())throw opad::Error("cancelled");
      p.setPhase(tr("Computing geometry and context"));
      if(name=="live_state"){
        result->output=state;auto& refs=result->output["selection"];
        opad::design::Provenance provenance(*source->doc,[p]{return p.cancelled();});  // created_by: once per body, not per pick
        for(auto& r:refs){
          if(p.cancelled())throw opad::Error("cancelled");
          auto ref=opad::Ref::from_json(r);if(source->scene.node(ref.body))r=entity_details(*source->doc,source->scene,{{"ref",r},{"limit",10}},&provenance);
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
        result->output={{"image",QByteArray::fromStdString(png).toBase64().toStdString()},{"camera",metadata["camera"]},{"visible_ids",metadata["visible_ids"]},
          {"selection",state["selection"]},{"preview_id",args.value("preview_id","")},{"transaction",transaction},{"render_ms",renderTimer.elapsed()},
          {"rendering","software geometry view; visible_ids lists submitted visible bodies (including occluded bodies); UI overlays are not included"}};
        if(metadata.contains("views"))result->output["views"]=metadata["views"];
      }else if(name=="model_batch")modelBatch(*working->doc,args,p,result->output,known.get(),steps.get());
      else result->output=opad::commands::run(name,args,working->doc.get());
      if(p.cancelled())throw opad::Error("cancelled");
      if(write && name!="export"){
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
            if(!BRepCheck_Analyzer(shape).IsValid())throw opad::Error("Generated body failed solid validation; document unchanged.");++checkedSolids;checked.insert(id);
          }
          if(result->delta["references"].size()<100)result->delta["references"].push_back(reference_token(*working->doc,working->scene,opad::Ref::from_json(json{{"body",id},{"kind","body"}})));
          if(preview || !transaction.empty()){
            shape=BRepBuilderAPI_Copy(shape,true,false).Shape();BodyPrs::meshForDisplay(shape,0.1);builder.Add(compound,shape);any=true;
            if(prior)result->hidden.push_back(id);
          }
        }
        if(any){Bnd_Box box;BRepBndLib::Add(compound,box);result->preview=compound;result->prs=BodyPrs::build(compound,box);}
        if(compact) {
          // TODO 10 B11: this command's own ids and counts (not the transaction's cumulative lists), references without
          // signatures, and no batch-wide operation_ids (every step receipt has its own).
          auto own=changes(source->scene,working->scene);
          json slim={{"scope","command"},{"created",own["created"]},{"modified",own["modified"]},{"deleted",own["deleted"]},{"total",own["total"]},
                     {"counts",{{"created",own["created"].size()},{"modified",own["modified"].size()},{"deleted",own["deleted"].size()}}}};
          for(const char* key:{"bodies","bodies_total","validation"})if(result->delta.contains(key))slim[key]=result->delta[key];
          result->delta=std::move(slim);
          if(name=="model_batch" && result->output.is_object())result->output.erase("operation_ids");
          std::function<void(json&)> unsign=[&](json& v){
            if(v.is_object()){if(v.contains("ref") && v.contains("geometry"))v.erase("signature");for(auto& [k,c]:v.items())unsign(c);}
            else if(v.is_array())for(auto& c:v)unsign(c);
          };
          unsign(result->output);
        }
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
      }
      // The isolated acceptance harness can emulate an uninterruptible kernel tail.
      // Its result must never commit after Stop, disconnect, access revocation or a manual edit.
      if(write && delay>0)QThread::msleep(static_cast<unsigned long>(delay));
    },[this,session,result,name,receipt,write,preview,transaction,epoch,revision,started](bool ok,const QString& error){
      if(epoch!=m_epoch || !session->socket || !session->bound){fail(session,"cancelled",tr("Agent operation cancelled."),receipt);release(result);return;}
      m_busy=false;m_job.clear();
      if(!ok){
        if(name=="model_batch" && error!="cancelled" && result->output.is_object()){
          for(auto& step:result->output["steps"])step["state"]="discarded";
          m_receipts[receipt].state=error=="cancelled"?"cancelled":"failed";
          auto failure=live_error("batch_failed",error.toStdString());failure["structuredContent"]["state"]=m_receipts[receipt].state;
          failure["structuredContent"]["revision"]=revision;failure["structuredContent"]["result"]=result->output;
          reply(session,std::move(failure),receipt);
        }else fail(session,error=="cancelled"?"cancelled":"invalid_operation",error,receipt);
        release(result);return;
      }
      if(session->target!=target() || m_doc->revision!=revision || (write && (!m_edit || editorBusy()))){fail(session,"stale_revision",tr("The document or access changed. The computed edit was discarded."),receipt);release(result);return;}
      if(write && name!="export"){
        auto prepared=std::make_shared<Prepared>();prepared->snapshot=result->snapshot;prepared->id=newId();prepared->label=QString::fromStdString(name);prepared->owner=session->socket;prepared->transaction=!transaction.empty();prepared->result=std::move(result->output);prepared->changes=std::move(result->delta);
        if(name=="model_batch"){  // later batches on this connection may refer to these steps
          auto next=std::make_shared<BatchSteps>(session->steps?*session->steps:BatchSteps{});
          for(const auto& step:prepared->result.value("steps",json::array()))(*next)[step.at("id").get<std::string>()]=step.value("result",json::object());
          session->steps=next;
        }
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
