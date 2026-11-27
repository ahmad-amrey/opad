#include "opad/model_batch.hpp"
#include "opad/geometry.hpp"
#include "opad/design/feature.hpp"
#include <BRepCheck.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_ListOfStatus.hxx>
#include <BRepCheck_Result.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <optional>
#include <sstream>
#include <Standard_Failure.hxx>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>

namespace opad::agent {
// Every face/edge/vertex input needs a current token in `references`, or to be a reference this connection
// was given earlier whose body still has the key and placement it had then.
void check_references(const Document& doc,const Scene& scene,const json& args,const KnownRefs* known){
  std::set<std::string> checked;
  for(const auto& token:args.value("references",json::array())){
    if(resolve_reference(doc,scene,token).value("status","")!="resolved")throw Error("stale_reference: request a fresh selection or resolve_reference before editing");
    checked.insert(Ref::from_json(token.at("ref")).to_json().dump());
  }
  std::function<void(const json&)> walk=[&](const json& value){
    bool isRef=false;
    if(value.is_string()){const auto& text=value.get_ref<const std::string&>();isRef=text.find("/face/")!=text.npos || text.find("/edge/")!=text.npos || text.find("/vertex/")!=text.npos || text.find("/center/")!=text.npos;}
    if(value.is_object() && value.contains("body")){const auto kind=value.value("kind","body");isRef=kind!="body" && kind!="point" && !value.contains("select");}  // a rule names no entity: it is resolved when computed
    if(isRef){
      auto ref=Ref::from_json(value);const auto key=ref.to_json().dump();if(checked.count(key))return;
      if(known)if(auto it=known->find(key);it!=known->end()){
        const auto* node=scene.node(ref.body);
        if(node && node->body_key==it->second.geometry && scene.world(ref.body).to_json().dump()==it->second.placement)return;
        throw Error("stale_reference: "+key+" was given for an earlier state of its body; request it again (entity_details or query_entities)");
      }
      throw Error("unchecked_reference: supply entity_details.reference in references for every subshape input, or use a reference this connection was given by entity_details, query_entities or the selection");
    }
    if(value.is_object()){for(const auto& [key,child]:value.items())if(key!="references")walk(child);}
    else if(value.is_array())for(const auto& child:value)walk(child);
  };walk(args);
}
void model_batch(Document& doc,const json& args,json& output,const BatchOptions& options){
  const KnownRefs* known=options.known;const BatchSteps* before=options.before;
  auto cancelled=[&]{return options.cancelled && options.cancelled();};
  static const std::set<std::string> allowed={"component","param","sketch","sketch_edit","align_sketch","feature","feature_edit","rename","reparent","appearance","transform","joint"};
  std::map<std::string,json> schemas,results;std::set<std::string> earlier,defined;
  for(const auto& step:args.at("steps"))if(step.contains("id") && step["id"].is_string())defined.insert(step["id"].get<std::string>());
  // An id this batch defines is this batch's step (a later one is a forward reference); only others may name a step
  // of an earlier batch on this connection.
  auto older=[&](const std::string& id){return !defined.count(id) && before && before->count(id);};
  for(const auto& command:commands::list())if(allowed.count(command.name))schemas[command.name]=command_schema(command,true);
  static const std::string syntax="Write @{<step id>#/<path in that step's result>}, for example @{cabin#/body_ids/0} (the first body made by the step with id \"cabin\"); @{cabin/body_ids/0} is the same. Paths start at the step's result, without /result/: feature steps have feature_id and body_ids, sketch steps sketch_id, component steps component_id.";
  // Step ids are [A-Za-z0-9_], so the first '#' or '/' ends the id; '#' may be followed by the pointer's '/' or not.
  auto symbol=[](const std::string& text)->std::pair<std::string,std::string>{
    if(!text.starts_with("@{") || !text.ends_with("}"))return {};
    const auto body=text.substr(2,text.size()-3);const auto end=body.find_first_of("#/");
    std::string id=body.substr(0,end),path=end==std::string::npos?std::string():body.substr(end+(body[end]=='#'?1:0));
    if(!path.empty() && path[0]!='/')path="/"+path;
    if(id.empty() || path.size()<2)throw Error("Batch reference "+text+" needs a step id and a path. "+syntax);
    return {id,path};
  };
  auto keysOf=[](const json& value){std::string keys;for(const auto& [key,item]:value.items())keys+=(keys.empty()?"":", ")+key;return keys.empty()?std::string("none"):keys;};
  // Walks the pointer itself so a wrong path names the step and what it does have, not a raw JSON exception.
  auto lookup=[&](const std::string& text,const std::string& id,const std::string& path)->const json&{
    // This batch's steps first, then this connection's earlier batches.
    const json* at=results.count(id)?&results.at(id):&before->at(id);std::string where="step '"+id+"' result";
    for(size_t start=1;start<=path.size();){
      const auto end=std::min(path.find('/',start),path.size());std::string token=path.substr(start,end-start);start=end+1;
      for(size_t i=0;(i=token.find('~',i))!=std::string::npos;++i)token.replace(i,2,token.compare(i,2,"~1")==0?"/":"~");
      if(at->is_object()){
        if(!at->contains(token)){
          std::string example=at->contains("body_ids")?"@{"+id+"#/body_ids/0}":at->contains("sketch_id")?"@{"+id+"#/sketch_id}":at->contains("component_id")?"@{"+id+"#/component_id}":"";
          throw Error("Batch reference "+text+": "+where+" has no '"+token+"'; its keys are "+keysOf(*at)+"."+(example.empty()?"":" For example "+example+".")+(token=="result"?" Paths start at the step's result, without /result/.":""));
        }
        at=&(*at)[token];
      }else if(at->is_array()){
        const bool digits=!token.empty() && token.size()<10 && std::all_of(token.begin(),token.end(),[](unsigned char c){return std::isdigit(c);});
        if(!digits || std::stoull(token)>=at->size())throw Error("Batch reference "+text+": "+where+" is a list of "+std::to_string(at->size())+" item(s); '"+token+"' is not an index from 0 to "+std::to_string(at->size()?at->size()-1:0)+".");
        at=&(*at)[std::stoull(token)];
      }else throw Error("Batch reference "+text+": "+where+" is "+at->dump()+", which has no '"+token+"'.");
      where+="/"+token;
    }
    return *at;
  };
  // @{pat#/body_ids/*} is the whole list: spliced in as an element of an array, or the array itself as a value.
  auto wholeList=[](const json& value){if(!value.is_string())return false;const auto& text=value.get_ref<const std::string&>();return text.starts_with("@{") && text.ends_with("/*}");};
  auto lookupList=[&](const std::string& text)->json{
    const auto one=text.substr(0,text.size()-3)+"}";const auto [id,path]=symbol(one);const json& list=lookup(text,id,path);
    if(!list.is_array())throw Error("Batch reference "+text+": step '"+id+"' "+path.substr(1)+" is "+std::string(list.type_name())+" "+list.dump()+", not a list; /* takes every item of a list such as body_ids.");
    for(const auto& item:list)if(!item.is_string())throw Error("Batch reference "+text+" holds "+item.dump()+", not identifier strings.");
    return list;
  };
  // Before anything runs, a whole-list reference standing for an array is checked as a list of one.
  std::function<void(json&)> listShaped=[&](json& value){
    if(value.is_object()){for(auto& [key,child]:value.items()){
      if(wholeList(child)){
        if(key=="target")throw Error("Batch reference "+child.get<std::string>()+" is a whole list; use targets (an array) instead of target, or pick one item such as /0.");
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
        throw Error("Batch reference "+text+" must name an earlier step of this batch or of an earlier batch on this connection; earlier steps are "+(steps.empty()?std::string("none"):steps)+(older.empty()?std::string():"; earlier batches' are "+older)+". "+syntax);
      }
    }else if(value.is_array() || value.is_object())for(const auto& child:value)preflightRefs(child);
  };
  // validate "final": the steps run with the design's errors recorded, not thrown (a cut moved away and back again), and
  // only the batch's end state is checked; "each" (the default) checks every step as a single command is checked.
  const std::string validate=args.value("validate","each");
  if(validate!="each" && validate!="final")throw Error("validate is \"each\" (every step checked as one command is, the default) or \"final\" (only the batch's end state)");
  const bool finalOnly=validate=="final";
  // A batch-level parent is where every body its feature steps make goes, unless a step names its own.
  const json batchParent=args.contains("parent")?args["parent"]:json(),batchComponent=args.contains("component")?args["component"]:json();
  // Validate every command and dependency before computing any geometry.
  for(const auto& step:args.at("steps")){
    const auto id=step.at("id").get<std::string>(),command=step.at("command").get<std::string>();
    if(id.empty() || id.size()>64 || !std::all_of(id.begin(),id.end(),[](unsigned char c){return std::isalnum(c) || c=='_';}) || earlier.count(id))throw Error("Batch step IDs must be unique letters, digits or underscores");
    if(!allowed.count(command))throw Error("Unsupported batch command: "+command);
    auto input=step.at("arguments");listShaped(input);validate_input(schemas.at(command),input);
    if(command=="feature")validate_input(feature_schema(input.at("kind").get<std::string>()),input.value("inputs",json::object()));
    preflightRefs(input);preflightRefs(step.value("references",json::array()));
    if(command=="feature" && !input.contains("parent") && batchParent.is_string()){
      const auto [ref,path]=symbol(batchParent.get<std::string>());
      if(!ref.empty() && !earlier.count(ref) && !older(ref))throw Error("The batch parent "+batchParent.get<std::string>()+" names step '"+ref+"', which does not come before feature step '"+id+"'; put that component step first.");
    }
    if((command=="feature" || command=="sketch") && !input.contains("component") && batchComponent.is_string()){
      const auto [ref,path]=symbol(batchComponent.get<std::string>());
      if(!ref.empty() && !earlier.count(ref) && !older(ref))throw Error("The batch component "+batchComponent.get<std::string>()+" names step '"+ref+"', which does not come before "+command+" step '"+id+"'; put that component step first.");
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
        if(!value.is_string())throw Error("Batch reference "+text+" is "+std::string(value.type_name())+" "+value.dump()+", not an identifier string."+(value.is_array()?" Add an index, for example "+text.substr(0,text.size()-1)+"/0}, or /* for the whole list where a list is accepted.":""));
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
  // A batch-level parent or component is a default, not the step's own choice: the warning that body_name, color and
  // parent were not used (a fillet, cut or join makes no bodies) is the step's only when the step set one of them.
  static const std::string unusedStyle="body_name, color and parent apply to the bodies a feature makes";
  std::set<std::string> valid;  // body keys found valid by an earlier step
  Scene scene=resolve(doc);
  std::optional<Scene> initial;std::optional<design::DeferErrors> deferred;
  if(finalOnly){initial.emplace(scene);deferred.emplace();}
  for(const auto& step:args.at("steps")){
    const auto id=step.at("id").get<std::string>(),command=step.at("command").get<std::string>();
    output["failed_step"]=id;
    const auto started=std::chrono::steady_clock::now();
    auto elapsed=[&]{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();};
    json invalidIds=json::array();
    try{
      if(cancelled())throw Error("cancelled");
      auto input=step.at("arguments");
      const bool ownStyle=input.contains("body_name") || input.contains("color") || input.contains("parent");
      if(command=="feature" && !input.contains("parent") && !batchParent.is_null())input["parent"]=batchParent;
      if((command=="feature" || command=="sketch") && !input.contains("component") && !batchComponent.is_null())input["component"]=batchComponent;
      expand(input);validate_input(schemas.at(command),input);
      auto checked=input;checked["references"]=step.value("references",json::array());expand(checked["references"]);
      check_references(doc,scene,checked,known);
      input["by"]="Agent";const auto begin=doc.ops.size();
      auto value=commands::run(command,input,&doc);Scene after=resolve(doc);
      if(command=="feature" || command=="sketch")for(size_t i=begin;i<doc.ops.size();++i){
        const auto& op=doc.ops[i];
        if(command=="sketch" && op.type=="sketch")value["sketch_id"]=op.id;
        if(command=="feature" && op.type=="feature"){
          value["feature_id"]=op.id;value["body_ids"]=json::array();
          if(const auto* feature=after.feature(op.id))for(const auto& body:feature->result.value("bodies",json::array()))value["body_ids"].push_back(body.at("id"));
        }
      }
      if(command=="component")value["component_id"]=value.at("id");
      // Each step is checked as a single command is: a later step (a join, an intersect) can swallow an invalid body and
      // leave a valid one, so checking only the batch's end state committed geometry no step alone would have.
      if(cancelled())throw Error("cancelled");
      if(!finalOnly)for(const auto& body:invalid_changed_solids(doc,scene,after,&valid))invalidIds.push_back(body);
      if(!invalidIds.empty()){
        std::string names;
        for(const auto& body:invalidIds)names+=(names.empty()?"":"; ")+describe_invalid_solid(doc,after,body.get<std::string>());
        throw Error("Batch step '"+id+"' ("+command+(command=="feature"?" "+input.value("kind",""):"")+") made a body that failed solid validation: "+names+
                    "; the batch was discarded, document unchanged. Run this step alone with preview=true to see it, then change its inputs (a smaller section, a smoother path).");
      }
      if(command=="feature" && !ownStyle && value.contains("warnings") && value["warnings"].is_array()){
        json kept=json::array();
        for(const auto& w:value["warnings"])if(!(w.is_string() && w.get_ref<const std::string&>().starts_with(unusedStyle)))kept.push_back(w);
        if(kept.empty())value.erase("warnings");else value["warnings"]=std::move(kept);
      }
      if(options.step_done)options.step_done(command,input,value,scene,after);
      scene=std::move(after);
      results[id]=value;output["steps"].push_back({{"id",id},{"command",command},{"state","computed"},{"result",value},{"elapsed_ms",elapsed()}});
    }catch(const Error& e){
      if(std::string_view(e.what())!="cancelled"){
        json failed={{"id",id},{"command",command},{"state","failed"},{"error",e.what()},{"elapsed_ms",elapsed()}};
        if(!invalidIds.empty())failed["invalid_body_ids"]=invalidIds;
        output["steps"].push_back(std::move(failed));
      }
      throw;
    }
  }
  output.erase("failed_step");
  if(finalOnly){
    deferred.reset();
    // The end state as one command's: no design item newly failing, nothing newly unresolved, every changed solid valid.
    json errors=json::array();std::set<std::string> named;  // a failing feature or sketch is unresolved too: said once
    for(const auto& f:scene.features)if(!f.error.empty()){
      const auto* before=initial->feature(f.id);if(!before || before->error!=f.error){errors.push_back(describe_feature_error(f));named.insert(f.id);}
    }
    for(const auto& k:scene.sketches)if(!k.error.empty()){
      const auto* before=initial->sketch(k.id);if(!before || before->error!=k.error){errors.push_back("sketch '"+k.name+"' ("+k.id+") failed: "+k.error);named.insert(k.id);}
    }
    for(const auto& p:scene.params)if(!p.error.empty()){
      const auto* before=initial->param(p.name);if(!before || before->error!=p.error)errors.push_back("parameter '"+p.name+"' failed: "+p.error);
    }
    for(const auto& u:scene.unresolved){
      bool old=false;for(const auto& v:initial->unresolved)if(v.op_id==u.op_id && v.reason==u.reason){old=true;break;}
      if(!old && !named.count(u.op_id))errors.push_back("unresolved "+u.op_type+" "+u.op_id+(u.reason.empty()?"":": "+u.reason));
    }
    json invalid=json::array();
    if(errors.empty())for(const auto& body:invalid_changed_solids(doc,*initial,scene,&valid)){invalid.push_back(body);errors.push_back("a body failed solid validation: "+describe_invalid_solid(doc,scene,body));}
    if(!errors.empty()){
      output["failed_step"]="final";
      output["final_check"]={{"state","failed"},{"errors",errors}};
      if(!invalid.empty())output["final_check"]["invalid_body_ids"]=invalid;
      std::string text;for(size_t i=0;i<errors.size() && i<5;++i)text+=(text.empty()?"":"; ")+errors[i].get<std::string>();
      if(errors.size()>5)text+=" (and "+std::to_string(errors.size()-5)+" more)";
      throw Error("The batch's end state (validate \"final\") is not valid: "+text+"; the batch was discarded, document unchanged.");
    }
    output["final_check"]={{"state","valid"}};
  }
  if(options.valid_keys)options.valid_keys->insert(valid.begin(),valid.end());
}
std::string invalid_solid_reason(const TopoDS_Shape& shape){
  if(shape.IsNull())return "the body has no geometry";
  std::map<std::string,std::map<std::string,int>> found;  // status -> sub-shape kind -> count
  std::vector<std::string> order;
  try{
    BRepCheck_Analyzer analyzer(shape);
    if(analyzer.IsValid())return {};
    static const char* const kinds[]={"compound","compound solid","solid","shell","face","wire","edge","vertex","shape"};
    auto note=[&](BRepCheck_Status status,TopAbs_ShapeEnum on){
      if(status==BRepCheck_NoError)return;
      std::ostringstream text;BRepCheck::Print(status,text);
      std::string name=text.str();
      while(!name.empty() && (name.back()=='\n' || name.back()=='\r' || name.back()==' '))name.pop_back();
      if(name.rfind("BRepCheck_",0)==0)name=name.substr(10);
      std::string words;  // SelfIntersectingWire -> self intersecting wire
      for(char c:name){if(std::isupper(static_cast<unsigned char>(c)) && !words.empty())words+=' ';words+=char(std::tolower(static_cast<unsigned char>(c)));}
      if(!found.count(words))order.push_back(words);
      ++found[words][kinds[std::min<int>(int(on),8)]];
    };
    for(const TopAbs_ShapeEnum type:{TopAbs_VERTEX,TopAbs_EDGE,TopAbs_WIRE,TopAbs_FACE,TopAbs_SHELL,TopAbs_SOLID}){
      TopTools_IndexedMapOfShape subs;TopExp::MapShapes(shape,type,subs);
      for(int i=1;i<=subs.Extent();++i){
        const Handle(BRepCheck_Result)& r=analyzer.Result(subs(i));if(r.IsNull())continue;
        for(const auto status:r->Status())note(status,type);
        for(r->InitContextIterator();r->MoreShapeInContext();r->NextShapeInContext())
          for(const auto status:r->StatusOnShape())note(status,type);
      }
    }
  }catch(const Standard_Failure&){return "the kernel could not check it";}
  if(order.empty())return "the kernel's check fails without naming a sub-shape";
  std::string out;
  for(size_t i=0;i<order.size() && i<4;++i){
    std::string on;
    for(const auto& [kind,count]:found[order[i]])on+=(on.empty()?"":", ")+std::to_string(count)+" "+kind+(count==1?"":"s");
    out+=(out.empty()?"":"; ")+order[i]+" on "+on;
  }
  if(order.size()>4)out+="; and "+std::to_string(order.size()-4)+" more kinds";
  return out;
}
const Feature* feature_that_made(const Scene& scene,const std::string& body){
  const Feature* last=nullptr;
  for(const auto& f:scene.features)
    for(const auto& b:f.result.value("bodies",json::array()))if(b.is_object() && b.value("id","")==body)last=&f;
  return last;
}
std::string describe_feature_error(const Feature& f){
  return f.kind+" '"+f.name+"' ("+f.id+") failed: "+f.error;
}
std::string describe_invalid_solid(const Document& doc,const Scene& scene,const std::string& body){
  const auto* node=scene.node(body);
  std::string out=(node && !node->name.empty()?"'"+node->name+"' ":"")+"("+body+")";
  if(const auto* f=feature_that_made(scene,body))out+=", made by "+f->kind+" '"+f->name+"' ("+f->id+")";
  std::string why;
  try{why=invalid_solid_reason(node_world_shape(doc,scene,body));}catch(const std::exception&){}
  if(!why.empty())out+=": "+why;
  return out;
}
std::vector<std::string> invalid_changed_solids(const Document& doc,const Scene& before,const Scene& after,std::set<std::string>* valid){
  std::vector<std::string> out;
  for(const auto& [id,node]:after.nodes){
    if(node.kind!=Node::Kind::Body || node.representation!="solid" || node.body_missing)continue;
    const auto* prior=before.node(id);
    if(prior && prior->body_key==node.body_key)continue;  // moved or restyled only: a rigid placement keeps validity
    if(valid && !node.body_key.empty() && valid->count(node.body_key))continue;
    bool ok=false;
    try{ok=BRepCheck_Analyzer(node_world_shape(doc,after,id)).IsValid();}catch(const Standard_Failure&){}
    if(!ok)out.push_back(id);
    else if(valid && !node.body_key.empty())valid->insert(node.body_key);
  }
  return out;
}
}  // namespace opad::agent
