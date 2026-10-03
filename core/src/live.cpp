#include "opad/live.hpp"
#include <set>
namespace opad::agent {
namespace {
// Drawing sheets join the live list with the Drawings workspace (UI-78); until then they are file-level, like project.
const std::set<std::string> excluded={"new","append","diff","cache","gc","mesh","render","project","sheet","sheet_view","sheet_item","sheet_edit","sheet_info","part_properties"};
json object(json properties={},json required=json::array()) {
  if(properties.is_null())properties=json::object();
  return {{"type","object"},{"properties",properties},{"required",required},{"additionalProperties",false}};
}
json str(){return {{"type","string"},{"minLength",1},{"maxLength",200}};}
json revision(){return {{"type","integer"},{"minimum",0}};}
json brief_geometry(){return {{"type","object"},{"description","Sketch geometry as the sketch tool's schema gives it: points, entities, constraints and shapes, all ids in one id space across the sketch; checked in full by the server."}};}
json verbosity(){return {{"type","string"},{"enum",{"full","compact"}},{"default","full"},{"description","compact: this command's own created/modified/deleted ids and counts (not the transaction's cumulative lists), references without signatures, no batch-wide operation_ids."}};}
}
bool live_mutation(const std::string& name) {
  if(name=="model_batch" || name=="save" || name=="transaction_begin" || name=="transaction_commit" || name=="preview_commit" || name=="undo" || name=="redo")return true;
  for(const auto& c:commands::list())if(c.name==name)return c.mutates || name=="export";
  return false;
}
bool live_mutation(const std::string& name,const json& args) {
  // A measurement only writes when pinned; otherwise it is a read and leaves the revision alone (gap log #4).
  if(name=="measure")return args.value("pin",false);
  return live_mutation(name);
}
json transaction_policy() {
  return {{"scope","connection"},{"disconnect","Uncommitted transactions and previews are discarded when this MCP connection closes."},
    {"expected_revision","Use base_revision for every staged command and commit; staging does not advance the live revision."},
    {"recovery","After reconnect, bind the same target and query request_status for any uncertain commit. Discarded transactions cannot resume: inspect current context, begin a new transaction and replan with new request IDs."}};
}
json live_guide() {
  return {{"transactions",transaction_policy()},{"identifiers","result.feature_id identifies a history operation; result.body_ids identifies its output bodies; result.sketch_id identifies a sketch. Legacy result.ids contains operation IDs, not body IDs."},
    {"changes","changes.scope=transaction means cumulative changes since transaction_begin; command means this command only. result identifies the last modeling command, including at transaction_commit."},
    {"batch_references","In model_batch, @{<step id>#/<path>} (or @{<step id>/<path>}) is replaced by a value from that earlier step's result; the path starts at the result itself, without /result/. @{cabin#/body_ids/0} is the first body made by the step with id cabin; @{cabin#/body_ids/*} is all of them, wherever a list is accepted (targets, or an element of one)."},
    {"new_bodies","A feature's new bodies are named after the feature (numbered \"<name> 1\", \"<name> 2\" when it makes several); copies and pieces (pattern, mirror, move with copy, split) are named after their source and go into its component with its colour. body_name, color [r,g,b] and parent on a feature step name, colour and place its new bodies in the same step; model_batch's parent is the default for every feature step. rename, appearance and reparent take targets (a list) instead of target; rename then numbers the names, \"{n}\" marking where."},
    {"body_ids","body_ids lists the bodies a feature made or changed: one per new body (an extrude of several separate profiles makes several), the target body for join/cut/intersect and for modifying features (fillet, shell, ...). Mirror and patterns list only the new copies; all_body_ids adds the picked bodies. Removed bodies are not listed."},
    {"example",json::array({
      {{"tool","live_instances"},{"arguments",json::object()},{"use","Explicitly choose instance and target from instances."}},
      {{"tool","live_bind"},{"arguments",{{"instance","$chosen.instance"},{"target","$chosen.target"}}}},
      {{"tool","transaction_begin"},{"arguments",{{"label","Cube"},{"expected_revision","$bind.revision"},{"request_id","unique-begin"}}},{"save","tx=transaction; base=base_revision"}},
      {{"tool","feature"},{"arguments",{{"kind","box"},{"inputs",{{"length",40},{"width",40},{"height",40}}},{"transaction","$tx"},{"expected_revision","$base"},{"request_id","unique-box"}}},{"save","feature=result.feature_id; body=result.body_ids[0]"}},
      {{"tool","model_batch"},{"arguments",{{"steps",json::array({
          {{"id","group"},{"command","component"},{"arguments",{{"name","Housing"}}}},
          {{"id","cabin"},{"command","feature"},{"arguments",{{"kind","box"},{"name","Cabin"},{"inputs",{{"length",20},{"width",12},{"height",10}}},{"color",{0.2,0.4,0.8}}}}},
          {{"id","row"},{"command","feature"},{"arguments",{{"kind","pattern_rect"},{"inputs",{{"bodies",{"@{cabin#/body_ids/0}"}},{"count",3},{"spacing",25}}}}}},
          {{"id","label"},{"command","rename"},{"arguments",{{"targets",{"@{cabin#/body_ids/0}","@{row#/body_ids/*}"}},{"name","Cabin {n}"}}}}})},
        {"parent","@{group#/component_id}"},{"transaction","$tx"},{"expected_revision","$base"},{"request_id","unique-batch"}}},
        {"use","Several steps in one call. @{cabin#/body_ids/0} is the first body made by the step with id cabin (@{cabin/body_ids/0} is the same); @{row#/body_ids/*} is every body of step row. The batch parent puts the bodies of every feature step into the new component."}},
      {{"tool","validate"},{"arguments",{{"transaction","$tx"}}},{"use","Check validity and volume before commit."}},
      {{"tool","transaction_commit"},{"arguments",{{"id","$tx"},{"expected_revision","$base"},{"request_id","unique-commit"}}}},
      {{"tool","entity_details"},{"arguments",{{"ref","$body"}}},{"use","Use checked reference tokens for subsequent face/edge operations."}},
      {{"tool","save"},{"arguments",{{"path","$absolute_output.opad"},{"expected_revision","$commit.revision"},{"request_id","unique-save"}}},{"use","Persist committed work. Omit path on subsequent saves; an existing different destination requires overwrite=true."}}
    })},{"example_notation","$ variables refer to structuredContent fields from earlier calls, not literal argument values. Keep one persistent MCP connection for the entire transaction."}};
}
json live_output_schema(const std::string& name) {
  const json ids={{"type","array"},{"items",{{"type","string"}}}};
  // The reply envelope, briefly (TODO 10 B12): the full one repeated on every tool was nearly half of tools/list.
  // error: {code, message, next}; changes: {scope, created, modified, deleted, bodies, validation}.
  json props={{"state",{{"type","string"}}},{"revision",{{"type","integer"}}},{"base_revision",{{"type","integer"}}},{"transaction",{{"type","string"}}},
    {"preview_id",{{"type","string"}}},{"error",{{"type","object"},{"required",{"code","message"}}}},{"changes",{{"type","object"}}},{"elapsed_ms",{{"type","integer"}}}};
  if(name=="feature" || name=="sketch") {
    json result={{"type","object"},{"properties",{{"ids",ids},{"operation_ids",ids},{"feature_id",str()},{"body_ids",ids},{"sketch_id",str()}}}};
    result["required"]=name=="feature"?json{"feature_id","body_ids"}:json{"sketch_id"};props["result"]=result;
  } else if(name=="component")props["result"]={{"type","object"},{"properties",{{"component_id",str()},{"operation_ids",ids}}},{"required",{"component_id","operation_ids"}}};
  else if(name=="save")props["result"]={{"type","object"},{"properties",{{"path",{{"type","string"},{"minLength",1}}},{"saved_revision",revision()},{"dirty",{{"type","boolean"}}}}},{"required",{"path","saved_revision","dirty"}}};
  else props["result"]=json::object();
  if(name=="live_instances")props["instances"]={{"type","array"}};
  if(name=="live_diagnostics"){props["connection"]={{"type","string"}};props["next_calls"]={{"type","array"}};}
  // Optional envelope fields accommodate errors and request-status receipts. Payload IDs
  // are required whenever a successful feature/sketch result is present.
  return {{"type","object"},{"properties",props},{"additionalProperties",true}};
}
const json& live_tools() {
  static const json tools=[] {
  json out=json::array();
  auto add=[&](const std::string& name,const std::string& description,json schema){out.push_back({{"name",name},{"description",description},{"inputSchema",schema},{"outputSchema",live_output_schema(name)},{"annotations",{{"readOnlyHint",!live_mutation(name)},{"openWorldHint",false}}}});};
  add("live_diagnostics","Start here: compact read-only connection, target, permissions, units, revision, transaction state and next calls. Set include_example for a typed-ID workflow and include_guide for the agent guide (units, frames, sketch geometry, references, feature conventions; also the MCP resource opad://guide/agent).",object({{"include_example",{{"type","boolean"},{"default",false}}},{"include_guide",{{"type","boolean"},{"default",false}}}}));
  add("live_instances","List running OPAD windows. Choose the intended instance and document explicitly.",object());
  add("live_bind","Bind this connection to exactly one instance and document target; never follows a newly opened document.",object({{"instance",str()},{"target",str()}},{"instance","target"}));
  add("live_state","Current revision, camera, selection, edit session and bounded recent changes.",object());
  add("wait_for_idle","Wait without blocking the UI, up to 10 seconds, for this connection to be ready. Human editors return immediately with idle=false. Does not cancel work or reserve an edit lock; check revision before writing.",object({{"timeout_ms",{{"type","integer"},{"minimum",0},{"maximum",10000},{"default",2000}}}}));
  add("save","Save the committed live document to disk without a dialog. Omit path to save to its current file; an unsaved document needs an absolute .opad path. Saving to another existing file requires overwrite=true. Finish active editors and commit/cancel previews or transactions first. Requires edit access. Does not add an Undo step or change revision. Reuse request_id only to retrieve the same save receipt.",object({{"path",{{"type","string"},{"minLength",1},{"maxLength",32767}}},{"overwrite",{{"type","boolean"},{"default",false}}},{"expected_revision",revision()},{"request_id",str()}},{"expected_revision","request_id"}));
  add("live_select","Select up to 25 bodies or subshapes of the same kind in the viewport. Selection is applied asynchronously; inspect live_state afterwards. Does not edit geometry.",object({{"refs",{{"type","array"},{"items",{{"type",{"string","object"}}}},{"minItems",1},{"maxItems",25}}},{"expected_revision",revision()}},{"refs","expected_revision"}));
  add("request_status","Find an earlier request's committed, failed, cancelled or pending receipt after reconnecting.",object({{"request_id",str()}},{"request_id"}));
  add("stop","Cancel unfinished agent work and remove temporary previews. Completed edits remain undoable.",object());
  // Gap log #14: the document's own undo and redo, one step each (an agent's commit, a batch or a person's edit).
  for(const auto name:{"undo","redo"})
    add(name,std::string(name)=="undo"?"Undo the document's last step (whoever made it), as Edit > Undo does; not during a transaction. The reply names the step.":"Redo the step undo took back, as Edit > Redo does.",
        object({{"expected_revision",revision()},{"request_id",str()}},{"expected_revision","request_id"}));
  add("transaction_begin","Start a connection-scoped group for one Undo step. Disconnect discards it. Keep expected_revision=base_revision while staging and committing; pass transaction to dependent commands.",object({{"label",str()},{"expected_revision",revision()},{"request_id",str()}},{"label","expected_revision","request_id"}));
  for(const auto name:{"transaction_commit","transaction_cancel","preview_commit","preview_cancel"}) {
    auto schema=object({{"id",str()}},{"id"});
    if(live_mutation(name)){schema["properties"]["expected_revision"]=revision();schema["properties"]["request_id"]=str();schema["required"].push_back("expected_revision");schema["required"].push_back("request_id");}
    add(name,std::string(name).find("commit")!=std::string::npos?"Commit the prepared result if the live revision still matches.":"Discard the prepared result without changing the document.",schema);
  }
  add("viewport_image","Render the live or prepared geometry on a worker; returns PNG plus revision. Optional fit uses an isometric fitted camera without moving the user's view.",object({{"width",{{"type","integer"},{"minimum",64},{"maximum",2048},{"default",960}}},{"height",{{"type","integer"},{"minimum",64},{"maximum",2048},{"default",640}}},{"fit",{{"type","boolean"},{"default",false}}},{"transaction",str()},{"preview_id",str()}}));
  out.back()["inputSchema"]["properties"]["view"]={{"type","string"},{"enum",{"iso","top","bottom","front","back","left","right"}},{"description","Optional fitted rendering direction; leaves the user's camera unchanged."}};
  auto& render=out.back()["inputSchema"]["properties"];
  const json vector={{"type","array"},{"items",{{"type","number"}}},{"minItems",3},{"maxItems",3}};
  render["select"]={{"type","array"},{"items",str()},{"minItems",1},{"maxItems",100},{"description","Isolate these body/component IDs. fit=true fits only the rendered selection."}};
  render["ignore_visibility"]={{"type","boolean"},{"default",false},{"description","Include hidden selected bodies without changing document visibility."}};
  render["hide"]={{"type","array"},{"items",str()},{"maxItems",100},{"description","Temporarily exclude these bodies/components, even with ignore_visibility."}};
  render["camera"]=object({{"eye",vector},{"target",vector},{"up",vector},{"absolute",{{"type","boolean"}}},{"projection",{{"type","string"},{"enum",{"orthographic","perspective"}}}},{"scale",{{"type","number"},{"minimum",0}}},{"fov_deg",{{"type","number"},{"exclusiveMinimum",0},{"maximum",179.9}}}},{"eye","target","up"});
  render["camera"]["description"]="Custom camera; absolute defaults true. Cannot combine with view. fit defaults false for a custom camera.";
  // TODO 10 B9: better pictures; every option is off by default, so earlier images are unchanged.
  render["views"]={{"type","array"},{"items",{{"type","string"},{"enum",{"iso","top","bottom","front","back","left","right","iso-back"}}}},{"minItems",1},{"maxItems",9},
    {"description","Several fitted views in one labelled grid image, e.g. [\"iso\",\"front\",\"top\",\"right\"]; result.views gives each view's camera and cell."}};
  render["edges"]={{"type","boolean"},{"default",false},{"description","Draw the model's edges as dark lines (same as edge_lines)."}};
  render["edge_lines"]={{"type","boolean"},{"default",false},{"description","Draw the model's edges as dark lines, hidden behind nearer surfaces."}};
  render["highlight"]={{"type","array"},{"items",{{"type","string"}}},{"maxItems",200},{"description","Face and edge references to tint in orange, e.g. [\"<body>/face/3\", \"<body>/edge/7\"]."}};
  render["shading"]={{"type","string"},{"enum",{"flat","smooth"}},{"default","flat"},{"description","smooth shades with vertex normals, so curved surfaces look curved."}};
  json batchSteps=json::array();
  const std::set<std::string> batchCommands={"component","param","sketch","sketch_edit","feature","feature_edit","rename","reparent","appearance","transform"};
  for(const auto& command:commands::list())if(batchCommands.count(command.name)){
    auto arguments=command_schema(command,true);
    // The sketch geometry schema is on the sketch tool; here it is named, and the batch validates it in full (B12).
    if(arguments["properties"].contains("geometry"))arguments["properties"]["geometry"]=brief_geometry();
    // A list argument may also be one whole-list reference, @{step#/body_ids/*}; the batch checks it once expanded.
    for(auto& [key,property]:arguments["properties"].items())if(property.contains("type") && property["type"]=="array"){
      json list=property;list.erase("description");
      property={{"anyOf",{list,{{"type","string"},{"description","@{<step id>#/<list path>/*}, e.g. @{row#/body_ids/*}"}}}},{"description",property.value("description","")}};
    }
    batchSteps.push_back(object({{"id",str()},{"command",{{"enum",{command.name}}}},{"arguments",arguments},{"references",{{"type","array"},{"items",{{"type","object"}}},{"maxItems",100}}}},{"id","command","arguments"}));
  }
  add("model_batch","Execute 1-100 typed modeling steps atomically with per-step receipts. An identifier string may refer to an earlier step's result as @{<step id>#/<path in that step's result>}: with a step {\"id\":\"cabin\",\"command\":\"feature\",...}, @{cabin#/body_ids/0} is its first body and @{cabin/body_ids/0} is the same. Paths start at the step's result, without /result/: feature steps have feature_id and body_ids (mirror and patterns also all_body_ids), sketch steps sketch_id, component steps component_id. @{pat#/body_ids/*} is the whole list wherever a list is accepted, e.g. targets:[\"@{pat#/body_ids/*}\"]. A reference may also name a step of an earlier batch on the same connection when this batch has no step with that id. Feature steps take body_name, color and parent for the bodies they make; parent here is the default component for all of them. Validates all inputs/dependencies first. Failure discards this whole batch, retaining previous staged work. Uses normal transaction/preview, revision, Stop and Undo semantics. Commit and save remain explicit separate checkpoints; computed receipts do not imply persistence. No file operations or nested batches.",object({
    {"steps",{{"type","array"},{"items",{{"anyOf",batchSteps}}},{"minItems",1},{"maxItems",100}}},
    {"transaction",str()},{"preview",{{"type","boolean"},{"default",false}}},{"expected_revision",revision()},{"request_id",str()},{"verbosity",verbosity()},
    {"parent",{{"type",{"string","null"}},{"description","Default component for every body the feature steps make (a step's own parent wins); a component id or @{step#/component_id} of an earlier component step."}}}
  },{"steps","expected_revision","request_id"}));
  for(const auto& c:commands::list())if(!excluded.count(c.name)) {
    auto schema=command_schema(c,true);
    if(c.name=="sketch_edit")schema["properties"]["geometry"]=brief_geometry();  // the sketch tool has the full schema (B12)
    schema["properties"]["transaction"]=str();
    if(live_mutation(c.name)) {
      schema["properties"]["expected_revision"]=revision();schema["properties"]["request_id"]=str();
      schema["properties"]["preview"]={{"type","boolean"},{"default",false}};
      schema["properties"]["references"]={{"type","array"},{"items",{{"type","object"}}},{"maxItems",100}};
      schema["properties"]["verbosity"]=verbosity();
      // measure writes only with pin: then (and only then) it needs the revision and a request id.
      if(c.name=="measure")schema["properties"]["pin"]={{"type","boolean"},{"default",false},{"description","Append a measurement op; then expected_revision and request_id are required. Without pin, measure is a read."}};
      else{schema["required"].push_back("expected_revision");schema["required"].push_back("request_id");}
    }
    add(c.name,c.description,schema);
  }
  return out;
  }();return tools;
}
json live_schema(const std::string& name){
  for(const auto& t:live_tools())if(t["name"]==name){
    json schema=t["inputSchema"];
    // What tools/list names briefly is still checked in full (B12); batch steps are checked per command by the batch.
    if(name=="sketch_edit")schema["properties"]["geometry"]=live_schema("sketch")["properties"]["geometry"];
    return schema;
  }
  throw Error("Unsupported live tool: "+name);
}
json live_result(const json& output,bool error) {return {{"content",json::array()},{"structuredContent",output.is_object()?output:json{{"result",output}}},{"isError",error}};}
json live_error(const std::string& code,const std::string& message){return live_result({{"error",{{"code",code},{"message",message}}}},true);}
}
