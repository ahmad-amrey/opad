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
  if(name=="save" || name=="transaction_begin" || name=="transaction_commit" || name=="preview_commit")return true;
  for(const auto& c:commands::list())if(c.name==name)return c.mutates || name=="export";
  return false;
}
json transaction_policy() {
  return {{"scope","connection"},{"disconnect","Uncommitted transactions and previews are discarded when this MCP connection closes."},
    {"expected_revision","Use base_revision for every staged command and commit; staging does not advance the live revision."},
    {"recovery","After reconnect, bind the same target and query request_status for any uncertain commit. Discarded transactions cannot resume: inspect current context, begin a new transaction and replan with new request IDs."}};
}
json live_guide() {
  return {{"transactions",transaction_policy()},{"identifiers","result.feature_id identifies a history operation; result.body_ids identifies its output bodies; result.sketch_id identifies a sketch. Legacy result.ids contains operation IDs, not body IDs."},
    {"changes","changes.scope=transaction means cumulative changes since transaction_begin; command means this command only. result identifies the last modeling command, including at transaction_commit."},
    {"example",json::array({
      {{"tool","live_instances"},{"arguments",json::object()},{"use","Explicitly choose instance and target from instances."}},
      {{"tool","live_bind"},{"arguments",{{"instance","$chosen.instance"},{"target","$chosen.target"}}}},
      {{"tool","transaction_begin"},{"arguments",{{"label","Cube"},{"expected_revision","$bind.revision"},{"request_id","unique-begin"}}},{"save","tx=transaction; base=base_revision"}},
      {{"tool","feature"},{"arguments",{{"kind","box"},{"inputs",{{"length",40},{"width",40},{"height",40}}},{"transaction","$tx"},{"expected_revision","$base"},{"request_id","unique-box"}}},{"save","feature=result.feature_id; body=result.body_ids[0]"}},
      {{"tool","validate"},{"arguments",{{"transaction","$tx"}}},{"use","Check validity and volume before commit."}},
      {{"tool","transaction_commit"},{"arguments",{{"id","$tx"},{"expected_revision","$base"},{"request_id","unique-commit"}}}},
      {{"tool","entity_details"},{"arguments",{{"ref","$body"}}},{"use","Use checked reference tokens for subsequent face/edge operations."}},
      {{"tool","save"},{"arguments",{{"path","$absolute_output.opad"},{"expected_revision","$commit.revision"},{"request_id","unique-save"}}},{"use","Persist committed work. Omit path on subsequent saves; an existing different destination requires overwrite=true."}}
    })},{"example_notation","$ variables refer to structuredContent fields from earlier calls, not literal argument values. Keep one persistent MCP connection for the entire transaction."}};
}
json live_output_schema(const std::string& name) {
  const json ids={{"type","array"},{"items",{{"type","string"}}}};
  json props={{"state",{{"type","string"}}},{"revision",revision()},{"base_revision",revision()},
    {"transaction",str()},{"preview_id",str()},{"lifetime",{{"type","object"}}},
    {"error",{{"type","object"},{"properties",{{"code",str()},{"message",{{"type","string"}}},{"next",{{"type","string"}}},{"transport",{{"type","object"}}}}},{"required",{"code","message"}}}},
    {"changes",{{"type","object"},{"properties",{{"scope",{{"enum",{"command","transaction"}}}},{"created",ids},{"modified",ids},{"deleted",ids},{"geometry",ids},{"total",revision()},{"validation",{{"type","object"}}}}}}},
    {"elapsed_ms",{{"type","integer"}}}};
  if(name=="feature" || name=="sketch") {
    json result={{"type","object"},{"properties",{{"ids",ids},{"feature_id",str()},{"body_ids",ids},{"sketch_id",str()}}}};
    result["required"]=name=="feature"?json{"feature_id","body_ids"}:json{"sketch_id"};props["result"]=result;
  } else if(name=="save")props["result"]={{"type","object"},{"properties",{{"path",{{"type","string"},{"minLength",1}}},{"saved_revision",revision()},{"dirty",{{"type","boolean"}}}}},{"required",{"path","saved_revision","dirty"}}};
  else props["result"]={{"description","Command-specific payload; transaction_commit returns the last staged command's result. See context or feature_schema for modeling details."}};
  if(name=="live_instances")props["instances"]={{"type","array"},{"items",{{"type","object"}}}};
  if(name=="live_diagnostics"){
    props["connection"]={{"type","string"}};props["target"]={{"type",{"string","null"}}};props["permissions"]={{"type","object"}};
    props["units"]={{"type",{"string","null"}}};props["transaction_state"]={{"type","object"}};
    props["next_calls"]={{"type","array"},{"items",{{"type","string"}}}};props["guide"]={{"type","object"}};
  }
  // Optional envelope fields accommodate errors and request-status receipts. Payload IDs
  // are required whenever a successful feature/sketch result is present.
  return {{"type","object"},{"properties",props},{"additionalProperties",true}};
}
const json& live_tools() {
  static const json tools=[] {
  json out=json::array();
  auto add=[&](const std::string& name,const std::string& description,json schema){out.push_back({{"name",name},{"description",description},{"inputSchema",schema},{"outputSchema",live_output_schema(name)},{"annotations",{{"readOnlyHint",!live_mutation(name)},{"openWorldHint",false}}}});};
  add("live_diagnostics","Start here: compact read-only connection, target, permissions, units, revision, transaction state and next calls. Set include_example for a typed-ID workflow.",object({{"include_example",{{"type","boolean"},{"default",false}}}}));
  add("live_instances","List running OPAD windows. Choose the intended instance and document explicitly.",object());
  add("live_bind","Bind this connection to exactly one instance and document target; never follows a newly opened document.",object({{"instance",str()},{"target",str()}},{"instance","target"}));
  add("live_state","Current revision, camera, selection, edit session and bounded recent changes.",object());
  add("save","Save the committed live document to disk without a dialog. Omit path to save to its current file; an unsaved document needs an absolute .opad path. Saving to another existing file requires overwrite=true. Finish active editors and commit/cancel previews or transactions first. Requires edit access. Does not add an Undo step or change revision. Reuse request_id only to retrieve the same save receipt.",object({{"path",{{"type","string"},{"minLength",1},{"maxLength",32767}}},{"overwrite",{{"type","boolean"},{"default",false}}},{"expected_revision",revision()},{"request_id",str()}},{"expected_revision","request_id"}));
  add("live_select","Select up to 25 bodies or subshapes of the same kind in the viewport. Selection is applied asynchronously; inspect live_state afterwards. Does not edit geometry.",object({{"refs",{{"type","array"},{"items",{{"type",{"string","object"}}}},{"minItems",1},{"maxItems",25}}},{"expected_revision",revision()}},{"refs","expected_revision"}));
  add("request_status","Find an earlier request's committed, failed, cancelled or pending receipt after reconnecting.",object({{"request_id",str()}},{"request_id"}));
  add("stop","Cancel unfinished agent work and remove temporary previews. Completed edits remain undoable.",object());
  add("transaction_begin","Start a connection-scoped group for one Undo step. Disconnect discards it. Keep expected_revision=base_revision while staging and committing; pass transaction to dependent commands.",object({{"label",str()},{"expected_revision",revision()},{"request_id",str()}},{"label","expected_revision","request_id"}));
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
    add(c.name,c.description,schema);
  }
  return out;
  }();return tools;
}
json live_schema(const std::string& name){for(const auto& t:live_tools())if(t["name"]==name)return t["inputSchema"];throw Error("Unsupported live tool: "+name);}
json live_result(const json& output,bool error) {return {{"content",json::array()},{"structuredContent",output.is_object()?output:json{{"result",output}}},{"isError",error}};}
json live_error(const std::string& code,const std::string& message){return live_result({{"error",{{"code",code},{"message",message}}}},true);}
}
