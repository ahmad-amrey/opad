#include "opad/live.hpp"
#include <set>
namespace opad::agent {
namespace {
const std::set<std::string> excluded={"new","append","diff","cache","gc","mesh","render"};
json object(json properties={},json required=json::array()) {
  if(properties.is_null())properties=json::object();
  return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
}
json str(){return {{"type","string"},{"minLength",1},{"maxLength",200}};}
json revision(){return {{"type","integer"},{"minimum",0}};}
}
bool live_mutation(const std::string& name) {
  if(name=="transaction_begin" || name=="transaction_commit" || name=="preview_commit")return true;
  for(const auto& c:commands::list())if(c.name==name)return c.mutates || name=="export";
  return false;
}
const json& live_tools() {
  static const json tools=[] {
  json out=json::array();
  auto add=[&](const std::string& name,const std::string& description,json schema){out.push_back({{"name",name},{"description",description},{"inputSchema",schema},{"annotations",{{"readOnlyHint",!live_mutation(name)},{"openWorldHint",false}}}});};
  add("live_instances","List running OPAD windows. Choose the intended instance and document explicitly.",object());
  add("live_bind","Bind this connection to exactly one instance and document target; never follows a newly opened document.",object({{"instance",str()},{"target",str()}},{"instance","target"}));
  add("live_state","Current revision, camera, selection, edit session and bounded recent changes.",object());
  add("live_select","Select up to 25 bodies or subshapes of the same kind in the viewport. Selection is applied asynchronously; inspect live_state afterwards. Does not edit geometry.",object({{"refs",{{"type","array"},{"items",{{"type",{"string","object"}}}},{"minItems",1},{"maxItems",25}}},{"expected_revision",revision()}},{"refs","expected_revision"}));
  add("request_status","Find an earlier request's committed, failed, cancelled or pending receipt after reconnecting.",object({{"request_id",str()}},{"request_id"}));
  add("stop","Cancel unfinished agent work and remove temporary previews. Completed edits remain undoable.",object());
  add("transaction_begin","Start an isolated group of dependent edits, committed as one Undo step. Pass its ID to subsequent commands.",object({{"label",str()},{"expected_revision",revision()},{"request_id",str()}},{"label","expected_revision","request_id"}));
  for(const auto name:{"transaction_commit","transaction_cancel","preview_commit","preview_cancel"}) {
    auto schema=object({{"id",str()}},{"id"});
    if(live_mutation(name)){schema["properties"]["expected_revision"]=revision();schema["properties"]["request_id"]=str();schema["required"].push_back("expected_revision");schema["required"].push_back("request_id");}
    add(name,std::string(name).find("commit")!=std::string::npos?"Commit the prepared result if the live revision still matches.":"Discard the prepared result without changing the document.",schema);
  }
  add("viewport_image","Render the live or prepared geometry on a worker; returns PNG plus revision. Optional fit uses an isometric fitted camera without moving the user's view.",object({{"width",{{"type","integer"},{"minimum",64},{"maximum",2048},{"default",960}}},{"height",{{"type","integer"},{"minimum",64},{"maximum",2048},{"default",640}}},{"fit",{{"type","boolean"},{"default",false}}},{"transaction",str()},{"preview_id",str()}}));
  out.back()["inputSchema"]["properties"]["view"]={{"type","string"},{"enum",{"iso","top","bottom","front","back","left","right"}},{"description","Optional fitted rendering direction; leaves the user's camera unchanged."}};
  for(const auto& c:commands::list())if(!excluded.count(c.name)) {
    auto schema=command_schema(c,true);
    schema["properties"]["transaction"]=str();
    if(live_mutation(c.name)) {
      schema["properties"]["expected_revision"]=revision();schema["properties"]["request_id"]=str();
      schema["properties"]["preview"]={{"type","boolean"},{"default",false}};
      schema["properties"]["references"]={{"type","array"},{"items",{{"type","object"}}},{"maxItems",100}};
      schema["required"].push_back("expected_revision");schema["required"].push_back("request_id");
    }
    add(c.name,c.description+" Live mode: no file reload/save. For subshape inputs, supply current entity_details reference tokens in references; stale indices are refused.",schema);
  }
  return out;
  }();return tools;
}
json live_schema(const std::string& name){for(const auto& t:live_tools())if(t["name"]==name)return t["inputSchema"];throw Error("Unsupported live tool: "+name);}
json live_result(const json& output,bool error) {return {{"content",json::array()},{"structuredContent",output.is_object()?output:json{{"result",output}}},{"isError",error}};}
json live_error(const std::string& code,const std::string& message){return live_result({{"error",{{"code",code},{"message",message}}}},true);}
}
