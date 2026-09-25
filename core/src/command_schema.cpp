#include "opad/agent.hpp"
#include "opad/design/feature.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace opad::agent {
namespace {
json type(const char* name){return {{"type",name}};}
json array(json items,int minimum=0,int maximum=0){json out={{"type","array"},{"items",std::move(items)}};if(minimum)out["minItems"]=minimum;if(maximum)out["maxItems"]=maximum;return out;}
json object(json properties,json required=json::array(),bool extra=false){
  if(required.is_null())required=json::array();
  if(properties.is_null())properties=json::object();
  return {{"type","object"},{"properties",std::move(properties)},{"required",std::move(required)},{"additionalProperties",extra}};
}
json choice(json choices){return {{"type","string"},{"enum",std::move(choices)}};}
json vector(int n){return array(type("number"),n,n);}
json expression(const std::string& units={}) {
  json out={{"type",{"number","string"}},{"description","A numeric value or named-parameter expression. Lengths use mm internally, angles use degrees in expressions."}};
  if(!units.empty())out["x-units"]=units;return out;
}
json ref() {
  auto value=object({{"body",type("string")},{"kind",choice({"body","face","edge","vertex","center","point"})},{"index",{{"type","integer"},{"minimum",0}}},{"point",vector(3)},{"hint",type("object")}}, {},false);
  value["anyOf"]={{{"required",{"body"}}},{{"required",{"point"}}}};
  return {{"anyOf",{type("string"),value}},{"description","Body UUID or body/face/N, body/edge/N, body/vertex/N (zero-based); structured {body,kind,index,hint}. Use references from the current document revision."}};
}
json plane() {
  auto frame=object({{"origin",vector(3)},{"x",vector(3)},{"y",vector(3)}},{"origin","x","y"});
  auto p=object({{"base",choice({"xy","xz","yz"})},{"face",ref()},{"feature",type("string")},{"frame",frame}},{},false);
  p["anyOf"]={{{"required",{"base"}}},{{"required",{"face"}}},{{"required",{"feature"}}},{{"required",{"frame"}}}};return p;
}
json sketch_geometry() {
  auto id=json{{"type","integer"},{"minimum",1}};
  auto point=object({{"id",id},{"x",type("number")},{"y",type("number")},{"fixed",type("boolean")}},{"id","x","y"});
  auto entity=object({{"id",id},{"type",choice({"point","line","circle","arc","ellipse","spline"})},{"p",array(id,1)},{"r",{{"type","number"},{"exclusiveMinimum",0}}},
    {"construction",type("boolean")},{"fixed",type("boolean")},{"degree",type("integer")},{"knots",array(type("number"))},{"multiplicities",array(type("integer"))},
    {"weights",array(type("number"))},{"periodic",type("boolean")},{"source",type("object")}},{"id","type","p"});
  entity["description"]="p contains stable point IDs: line [start,end], circle [center] plus r, arc [center,start,end] counterclockwise, spline control points. Coordinates are local to the sketch frame, in mm.";
  auto constraint=object({{"id",id},{"type",choice({"coincident","horizontal","vertical","parallel","perpendicular","collinear","tangent","equal","concentric","midpoint","symmetric","fix","smooth","curvature","distance","hdistance","vdistance","radius","diameter","angle","arc_length"})},
    {"refs",array(id,1)},{"anchors",array(id)},{"value",type("number")},{"expr",type("string")},{"reference",type("boolean")},{"pos",vector(2)}},{"id","type","refs"});
  constraint["description"]="refs are point/entity IDs in this sketch. Dimensions require value (numeric initial value) and optionally expr (e.g. width or thickness/2).";
  return object({{"points",array(point)},{"entities",array(entity)},{"constraints",array(constraint)},{"images",array(type("object"))},{"patterns",array(type("object"))},{"id_watermark",type("integer")}});
}
json input_schema(const design::InputSpec& in) {
  json out;
  if(in.type=="length" || in.type=="angle" || in.type=="number" || in.type=="count")out=expression(in.type=="length"?"mm":in.type=="angle"?"deg":"");
  else if(in.type=="bool")out=type("boolean");
  else if(in.type=="choice")out=choice(in.choices);
  else if(in.type=="text")out=type("string");
  else if(in.type=="plane")out=plane();
  else if(in.type=="axis")out=object({{"base",choice({"x","y","z"})},{"edge",ref()},{"sketch",type("string")},{"entity",type("integer")},{"feature",type("string")}},{},false);
  else if(in.type=="path")out={{"anyOf",{object({{"sketch",type("string")},{"entities",array(type("integer"))}},{"sketch"}),object({{"edges",array(ref(),1)}},{"edges"}),array(ref(),1)}}};
  else if(in.type=="profiles")out=array({{"anyOf",{ref(),object({{"sketch",type("string")},{"at",vector(2)},{"boundary",array(type("integer"))},{"all",type("boolean")}},{"sketch"})}}},in.min_count,in.max_count);
  else if(in.type=="points")out=array({{"anyOf",{ref(),object({{"sketch",type("string")},{"point",type("integer")}},{"sketch","point"})}}},in.min_count,in.max_count);
  else out=array(ref(),in.min_count,in.max_count);
  out["title"]=in.label;
  if(!in.def.is_null())out["default"]=in.def;
  if(!in.show_if.empty())out["x-active-when"]=in.show_if;
  return out;
}
json inferred(const std::string& description) {
  const auto prefix=description.substr(0,description.find(" - "));
  json out;
  if(prefix=="bool")out=type("boolean");
  else if(prefix=="int")out=type("integer");
  else if(prefix.starts_with("number"))out=type("number");
  else if(prefix=="string|array" || prefix=="array|csv")out={{"anyOf",{type("string"),array(type("string"))}}};
  else if(prefix=="uuid|null")out={{"type",{"string","null"}}};
  else if(prefix.starts_with("[16]"))out=vector(16);
  else if(prefix.starts_with("["))out=vector(3);
  else if(prefix.starts_with("array"))out=array(type("object"));
  else if(prefix.starts_with("object"))out=type("object");
  else if(prefix.find('|')!=std::string::npos) {
    json values=json::array();size_t start=0;
    do{auto end=prefix.find('|',start);values.push_back(prefix.substr(start,end==std::string::npos?end:end-start));if(end==std::string::npos)break;start=end+1;}while(true);
    out=choice(values);
  } else out=type("string");
  out["description"]=description;return out;
}
}

json feature_schema(const std::string& kind) {
  const auto* spec=design::feature_spec(kind);if(!spec)throw Error("Unsupported feature kind: "+kind+". Request feature_kinds.");
  auto out=object(json::object());
  for(const auto& input:spec->inputs){out["properties"][input.name]=input_schema(input);
    if(!input.optional && input.def.is_null() && input.show_if.empty())out["required"].push_back(input.name);
    if(!input.optional && input.def.is_null() && !input.show_if.empty()){
      const auto equal=input.show_if.find('=');const auto key=input.show_if.substr(0,equal),choices=input.show_if.substr(equal+1);json values=json::array();size_t start=0;
      do{const auto end=choices.find('|',start);const auto value=choices.substr(start,end==std::string::npos?end:end-start);values.push_back(value=="true"?json(true):value=="false"?json(false):json(value));if(end==std::string::npos)break;start=end+1;}while(true);
      if(!out.contains("allOf"))out["allOf"]=json::array();
      out["allOf"].push_back({{"if",{{"properties",{{key,{{"enum",values}}}}},{"required",{key}}}},{"then",{{"required",{input.name}}}}});
    }
  }
  out["description"]=spec->hint;return out;
}
json command_schema(const commands::CommandInfo& command,bool live) {
  json properties=json::object(),required=json::array();
  for(const auto& [key,value]:command.args.items())properties[key]=value.is_object()?value:inferred(value.get<std::string>());
  const auto& name=command.name;
  if(name=="drawing_to_sketch")properties["doc"]=type("string");
  if(properties.contains("doc")){if(live)properties.erase("doc");else required.push_back("doc");}
  if(command.mutates)properties["save"]={{"type","boolean"},{"default",true}};
  const std::map<std::string,std::vector<std::string>> needed={
    {"properties",{"node"}},{"measure",{"kind","refs"}},{"import",{"file"}},{"export",{"format","out"}},{"render",{"out"}},{"diff",{"a","b"}},
    {"annotate",{"anchor","text"}},{"delete",{"target"}},{"rename",{"target","name"}},{"appearance",{"target"}},{"transform",{"target","matrix"}},
    {"reparent",{"target"}},{"section",{"origin","normal"}},{"view",{"camera"}},{"param",{"name"}},{"param_delete",{"name"}},
    {"sketch_edit",{"target"}},{"feature",{"kind"}},{"feature_edit",{"target"}},{"drawing_to_sketch",{"layers"}},
    {"query_entities",{"body"}},{"feature_schema",{"kind"}},{"sketch_details",{"sketch"}},{"resolve_reference",{"reference"}},{"sketch_tool",{"target","tool"}}
  };
  if(auto it=needed.find(name);it!=needed.end())for(const auto& key:it->second)required.push_back(key);
  if(properties.contains("ref"))properties["ref"]=ref();
  if(properties.contains("refs"))properties["refs"]=array(ref(),1,100);
  if(properties.contains("anchor"))properties["anchor"]=ref();
  if(properties.contains("select"))properties["select"]={{"anyOf",{type("string"),array(type("string"))}}};
  if(name=="sketch" || name=="sketch_edit"){
    properties["geometry"]=sketch_geometry();properties["plane"]=plane();
  }
  if(name=="feature") {
    json kinds=json::array();for(const auto& spec:design::feature_specs())kinds.push_back(spec.kind);
    properties["kind"]=choice(kinds);properties["inputs"]={{"type","object"},{"description","Request feature_schema for this kind before supplying inputs. Unknown input names are rejected."}};
  }
  if(name=="export")properties["format"]=choice(commands::exporter_formats());
  if(name=="import_brep")properties["brep"]=type("string");
  if(name=="drawing_to_sketch")properties["layers"]=array(object({{"id",type("string")},{"construction",{{"type","boolean"},{"default",false}}}},{"id"}),1);
  if(name=="param")properties["expr"]={{"type","string"},{"description","A dimension expression such as 20 mm or width/2. Explicit units are recommended."}};
  if(name=="render" || name=="view")properties["camera"]=object({{"eye",vector(3)},{"target",vector(3)},{"up",vector(3)},
    {"projection",choice({"orthographic","perspective"})},{"scale",type("number")},{"fov_deg",type("number")},{"view",type("string")},{"absolute",type("boolean")}},{});
  if(name=="render"){
    properties["fit"]=type("boolean");properties["ignore_visibility"]=type("boolean");properties["supersample"]={{"type","integer"},{"minimum",1},{"maximum",4}};
    for(const char* key:{"width","height"})properties[key]={{"type","integer"},{"minimum",16},{"maximum",4096}};
  }
  if(live)properties.erase("save");
  auto out=object(properties,required);
  if(name=="entity_details")out["anyOf"]={{{"required",{"ref"}}},{{"required",{"feature"}}}};
  if(name=="inspect" || name=="append" || name=="import_brep"){
    const auto keys=name=="inspect"?std::vector<std::string>{"ref","refs"}:name=="append"?std::vector<std::string>{"op","ops"}:std::vector<std::string>{"brep","file"};
    out["anyOf"]=json::array();for(const auto& key:keys)out["anyOf"].push_back({{"required",{key}}});
  }
  return out;
}

void validate_input(const json& schema,const json& value,const std::string& path) {
  auto fail=[&](const std::string& why){throw Error(path+": "+why);};
  if(schema.contains("allOf"))for(const auto& rule:schema["allOf"])validate_input(rule,value,path);
  if(schema.contains("if")){bool applies=true;try{validate_input(schema["if"],value,path);}catch(const Error&){applies=false;}if(applies&&schema.contains("then"))validate_input(schema["then"],value,path);}
  if(schema.contains("anyOf")) {
    bool valid=false;for(const auto& option:schema["anyOf"])try{validate_input(option,value,path);valid=true;break;}catch(const Error&){}
    if(!valid)fail("does not match a supported input format (see the tool schema)");
  }
  if(schema.contains("type")) {
    const auto types=schema["type"].is_array()?schema["type"]:json::array({schema["type"]});bool match=false;
    for(const auto& t:types){const auto s=t.get<std::string>();match=match || (s=="object"&&value.is_object()) || (s=="array"&&value.is_array()) || (s=="string"&&value.is_string()) || (s=="number"&&value.is_number()) || (s=="integer"&&value.is_number_integer()) || (s=="boolean"&&value.is_boolean()) || (s=="null"&&value.is_null());}
    if(!match)fail("expected "+schema["type"].dump());
  }
  if(schema.contains("enum") && std::find(schema["enum"].begin(),schema["enum"].end(),value)==schema["enum"].end())fail("expected one of "+schema["enum"].dump());
  if(value.is_number()){
    const auto n=value.get<double>();if(!std::isfinite(n))fail("must be finite");
    if(schema.contains("minimum")&&n<schema["minimum"].get<double>())fail("below minimum");
    if(schema.contains("maximum")&&n>schema["maximum"].get<double>())fail("above maximum");
    if(schema.contains("exclusiveMinimum")&&n<=schema["exclusiveMinimum"].get<double>())fail("must be greater than "+schema["exclusiveMinimum"].dump());
  }
  if(value.is_array()){
    if(value.size()<schema.value("minItems",size_t(0)))fail("too few items");
    if(schema.contains("maxItems")&&value.size()>schema["maxItems"].get<size_t>())fail("too many items");
    if(schema.contains("items"))for(size_t i=0;i<value.size();++i)validate_input(schema["items"],value[i],path+"["+std::to_string(i)+"]");
  }
  if(value.is_string()){
    const auto length=value.get_ref<const std::string&>().size();
    if(schema.contains("minLength")&&length<schema["minLength"].get<size_t>())fail("string is too short");
    if(schema.contains("maxLength")&&length>schema["maxLength"].get<size_t>())fail("string is too long");
  }
  if(value.is_object()){
    for(const auto& key:schema.value("required",json::array()))if(!value.contains(key.get<std::string>()))fail("missing required field "+key.get<std::string>());
    const auto properties=schema.value("properties",json::object());
    for(const auto& [key,item]:value.items()){
      if(properties.contains(key))validate_input(properties.at(key),item,path+"."+key);
      else if(schema.contains("additionalProperties")&&schema["additionalProperties"]==false)fail("unknown field "+key);
    }
  }
}
}
