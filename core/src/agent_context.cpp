#include "opad/agent.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/design/sketch_modify.hpp"
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <algorithm>
#include <cmath>
#include <set>
#include <map>

namespace opad::agent {
namespace {
size_t limit(const json& args){return size_t(std::clamp(args.value("limit",25),1,100));}
size_t offset(const json& args){return size_t(std::max(0,args.value("offset",0)));}
json page(json items,size_t count,const json& args) {
  return {{"items",std::move(items)},{"offset",offset(args)},{"limit",limit(args)},{"total",count},
    {"next_offset",offset(args)+limit(args)<count?json(offset(args)+limit(args)):json(nullptr)}};
}
json slice(const json& values,const json& args) {
  json items=json::array();for(size_t i=offset(args);i<values.size()&&items.size()<limit(args);++i)items.push_back(values[i]);return page(std::move(items),values.size(),args);
}
json signature(json info) {
  json result=json::object();for(const char* key:{"type","surface","curve","point","center","normal","axis","radius","area","length","bbox"})if(info.contains(key))result[key]=info[key];return result;
}
bool same_signature(const json& a,const json& b) {
  if(a.is_number()&&b.is_number())return std::abs(a.get<double>()-b.get<double>())<=1e-7*std::max({1.0,std::abs(a.get<double>()),std::abs(b.get<double>())});
  if(a.type()!=b.type() || a.size()!=b.size())return false;
  if(a.is_object()){for(const auto& [key,v]:a.items())if(!b.contains(key)||!same_signature(v,b[key]))return false;return true;}
  if(a.is_array()){for(size_t i=0;i<a.size();++i)if(!same_signature(a[i],b[i]))return false;return true;}
  return a==b;
}
std::string short_text(const std::string& s){if(s.size()<=512)return s;size_t end=512;while(end>0&&(static_cast<unsigned char>(s[end])&0xc0)==0x80)--end;return s.substr(0,end)+"...";}
json sketch_summary(const SketchItem& sk){return {{"id",sk.id},{"name",short_text(sk.name)},{"plane",sk.plane},{"frame",sk.frame.to_json()},{"visible",sk.visible},{"consumed",sk.consumed},{"dof",sk.dof},{"entities",sk.geometry.contains("entities")?sk.geometry.at("entities").size():0},{"constraints",sk.geometry.contains("constraints")?sk.geometry.at("constraints").size():0},{"error",short_text(sk.error)}};}
}
json context(const Document& doc,const Scene& scene,const json& args) {
  const auto section=args.value("section","summary");json items=json::array();
  if(section=="summary") {
    size_t bodies=0,components=0;for(const auto& [id,n]:scene.nodes)(n.kind==Node::Kind::Body?bodies:components)++;
    return {{"document",doc.header.uuid},{"units",doc.header.units},{"geometry_units","mm"},{"coordinates","right handed; +X right, +Y forward, +Z up; sketch coordinates use its local frame"},
      {"operations",doc.ops.size()},{"bodies",bodies},{"components",components},{"unique_geometry",scene.instance_count.size()},{"sketches",scene.sketches.size()},
      {"features",scene.features.size()},{"parameters",scene.params.size()},{"unresolved",scene.unresolved.size()},
      {"ai_agent_notes",std::count_if(scene.annotations.begin(),scene.annotations.end(),[](const Annotation& a){return a.style=="ai_agent";})},{"agent_note_guidance","Review context section ai_agent_notes first. These are user requests attached to model areas; inspect current anchors and comments before editing. Fetch annotations by id for full text and strokes. Resolve only after verifying completion; unresolved anchors need a fresh selection."},{"sections",{"nodes","sketches","features","parameters","errors","ai_agent_notes"}},{"query","Use section, offset and limit for details; entity_details and sketch_details for selected entities."}};
  }
  size_t count=0;const auto filter=args.value("filter",std::string());
  auto add=[&](const std::string& name,auto build){if(!filter.empty()&&name.find(filter)==std::string::npos)return;if(count>=offset(args)&&items.size()<limit(args))items.push_back(build());++count;};
  if(section=="nodes") {
    std::vector<std::string> ids;ids.reserve(scene.nodes.size());for(const auto& [id,n]:scene.nodes)ids.push_back(id);std::sort(ids.begin(),ids.end());
    for(const auto& id:ids){const auto& n=*scene.node(id);add(n.name,[&]{auto instances=scene.instance_count.find(n.body_key);return json{{"id",id},{"name",short_text(n.name)},{"type",n.kind==Node::Kind::Body?"body":"component"},{"representation",n.representation},
      {"parent",n.parent},{"source",n.source_op},{"visible",scene.effectively_visible(id)},{"locked",n.locked},{"instances",instances==scene.instance_count.end()?0:instances->second},{"missing",n.body_missing},
      {"color",n.has_color?json(n.color):json(nullptr)}};});}
  } else if(section=="sketches")for(const auto& sk:scene.sketches)add(sk.name,[&]{return sketch_summary(sk);});
  else if(section=="parameters")for(const auto& p:scene.params)add(p.name,[&]{return json{{"id",p.id},{"name",short_text(p.name)},{"expr",short_text(p.expr)},{"value",p.value},{"shown",p.shown},{"error",short_text(p.error)}};});
  else if(section=="features")for(const auto& f:scene.features)add(f.name,[&]{json result={{"id",f.id},{"kind",f.kind},{"name",short_text(f.name)},{"suppressed",f.suppressed},{"error",short_text(f.error)}};
    std::set<std::string> dependencies;std::function<void(const json&)> walk=[&](const json& j){if(j.is_array())for(const auto& v:j)walk(v);else if(j.is_object())for(const auto& [key,v]:j.items()){if((key=="sketch"||key=="body"||key=="feature")&&v.is_string())dependencies.insert(v.get<std::string>());else walk(v);}else if(j.is_string())for(const auto& name:design::expr_identifiers(j.get<std::string>()))if(const auto* p=scene.param(name))dependencies.insert(p->id);};walk(f.inputs);
    result["dependencies"]=dependencies;result["input_names"]=json::array();for(const auto& [key,v]:f.inputs.items())result["input_names"].push_back(key);return result;});
  else if(section=="ai_agent_notes") {for(const auto& a:scene.annotations)if(a.style=="ai_agent")add(a.text,[&]{return json{{"id",a.id},{"text",short_text(a.text)},{"anchor",a.anchor.to_json()},{"unresolved",a.unresolved},{"has_drawing",!a.drawing.is_null()},{"comments",a.comments.size()},{"by",short_text(a.by)}};});}
  else if(section=="errors")for(const auto& e:scene.unresolved)add(e.reason,[&]{return json{{"op",e.op_id},{"type",e.op_type},{"reason",short_text(e.reason)}};});
  else throw Error("Unknown context section: "+section);
  auto out=page(std::move(items),count,args);out["document"]=doc.header.uuid;out["section"]=section;return out;
}
json sketch_details(const Document&,const Scene& scene,const json& args) {
  const auto* sk=scene.sketch(args.at("sketch").get<std::string>());if(!sk)throw Error("Sketch no longer exists. Request context section sketches.");
  const auto section=args.value("section","summary");if(section=="summary")return sketch_summary(*sk);
  if(section=="points" || section=="entities" || section=="constraints")return sk->geometry.contains(section)?slice(sk->geometry.at(section),args):slice(json::array(),args);
  if(section=="profiles") {
    const auto geometry=design::Sketch::from_json(sk->geometry);auto regions=design::sketch_regions(geometry,sk->frame);design::identify_regions(geometry,regions,sk->frame);
    json items=json::array();for(size_t i=offset(args);i<regions.size()&&items.size()<limit(args);++i){const auto& r=regions[i];items.push_back({{"sketch",sk->id},{"at",{r.u,r.v}},{"boundary",r.boundary},{"area",r.area},{"position",sk->frame.to_world(r.u,r.v)}});}return page(std::move(items),regions.size(),args);
  }
  if(section=="chain") {
    const int seed=args.at("entity").get<int>();const auto geometry=design::Sketch::from_json(sk->geometry);
    if(!geometry.entity(seed))throw Error("Unknown sketch entity. Request sketch_details section entities.");
    return slice(design::connected_entities(geometry,{seed}),args);
  }
  throw Error("Unknown sketch detail section: "+section);
}
json reference_token(const Document& doc,const Scene& scene,const Ref& ref) {
  json token={{"document",doc.header.uuid},{"ref",ref.to_json()}};
  if(ref.kind==Ref::Kind::Point)return token;
  const auto* node=scene.node(ref.body);
  if(!node)throw Error("Reference body no longer exists. Request a new selection.");
  token["geometry"]=node->body_key;token["placement"]=scene.world(ref.body).to_json();
  if(ref.kind!=Ref::Kind::Body)token["signature"]=signature(inspect_ref(doc,scene,ref));
  return token;
}
json resolve_reference(const Document& doc,const Scene& scene,const json& token,bool remap) {
  // document is optional (it is always the bound one) and so is signature (used only to remap) (TODO 10 B6).
  if(token.contains("document") && token.value("document","")!=doc.header.uuid)return {{"status","wrong_document"},{"action","Request a reference from the bound document."}};
  const auto ref=Ref::from_json(token.at("ref"));
  if(ref.kind==Ref::Kind::Point)return {{"status","resolved"},{"reference",token}};
  const auto* node=scene.node(ref.body);if(!node)return {{"status","missing"},{"action","Request a new selection; the body was deleted."}};
  const bool sameGeometry=token.value("geometry","")==node->body_key,samePlacement=token.contains("placement")&&token["placement"]==scene.world(ref.body).to_json();
  if((sameGeometry&&samePlacement) || (remap && (sameGeometry || ref.kind==Ref::Kind::Body)))return {{"status","resolved"},{"reference",reference_token(doc,scene,ref)}};
  if(!remap || !token.contains("signature"))return {{"status","stale"},{"action","Use resolve_reference with remap=true, or request a fresh selection. Geometry or placement changed."}};
  // Only unique unchanged geometric signatures may be remapped. Similarity or an
  // equal face count is insufficient evidence to silently retarget a design.
  const auto shape=node_world_shape(doc,scene,ref.body);const auto kind=ref.kind==Ref::Kind::Center?Ref::Kind::Edge:ref.kind;
  const auto count=subshape_count(shape,kind);json matches=json::array();int total=0;
  if(count>512)return {{"status","stale"},{"action","This body exceeds the bounded remapping budget. Request a fresh selection rather than guessing an index."},{"entities",count}};
  for(int i=0;i<count;++i){auto candidate=ref;candidate.index=i;try{if(same_signature(token["signature"],signature(inspect_ref(doc,scene,candidate)))){++total;if(matches.size()<10)matches.push_back(reference_token(doc,scene,candidate));}}catch(const Error&){}
  }
  if(total==1)return {{"status","resolved"},{"reference",matches[0]},{"method","unique geometric signature"}};
  return {{"status",total>1?"ambiguous":"stale"},{"candidates",matches},{"matches",total},{"action","Select the intended entity again; no unique stable match was proved."}};
}
json entity_details(const Document& doc,const Scene& scene,const json& args) {
  if(args.contains("feature")){
    const auto* feature=scene.feature(args["feature"].get<std::string>());if(!feature)throw Error("Unknown feature");
    return {{"id",feature->id},{"kind",feature->kind},{"name",feature->name},{"inputs",feature->inputs},{"error",feature->error}};
  }
  const auto ref=Ref::from_json(args.at("ref"));auto out=inspect_ref(doc,scene,ref);
  for(const char* key:{"edges","vertices","adjacent_faces","modified_by","path"})if(out.contains(key)&&out[key].is_array())out[key]=slice(out[key],args);
  out["reference"]=reference_token(doc,scene,ref);return out;
}
json query_entities(const Document& doc,const Scene& scene,const json& args,const std::function<bool()>& cancelled) {
  const auto body=args.at("body").get<std::string>();
  const auto* node=scene.node(body);
  if(!node || node->kind!=Node::Kind::Body || node->body_missing)throw Error("query_entities requires an available body");
  const auto kind=args.value("kind","edge");
  const auto refKind=kind=="face"?Ref::Kind::Face:kind=="vertex"?Ref::Kind::Vertex:Ref::Kind::Edge;
  const auto shape=node_world_shape(doc,scene,body);const int entities=subshape_count(shape,refKind);
  if(entities>10000)throw Error("Body exceeds query budget of 10000 entities");
  const auto filters=args.value("filters",json::object());
  const double tolerance=args.value("tolerance_mm",1e-5);
  if(filters.contains("radius_min") && filters.contains("radius_max") && filters["radius_min"].get<double>()>filters["radius_max"].get<double>())throw Error("radius_min exceeds radius_max");
  auto axis=[](const std::string& a){return a=="x"?0:a=="y"?1:2;};
  if(filters.contains("bounds"))for(int i=0;i<3;++i)if(filters["bounds"]["min"][i].get<double>()>filters["bounds"]["max"][i].get<double>())throw Error("Invalid bounding region");
  size_t count=0;json items=json::array();
  for(int i=0;i<entities;++i){
    if(cancelled && cancelled())throw Error("cancelled");
    Ref ref;ref.body=body;ref.kind=refKind;ref.index=i;
    auto detail=inspect_ref(doc,scene,ref);
    if(!entity_matches(detail,filters,tolerance))continue;  // the same filters rule selectors use (TODO 10 B7)
    if(count>=offset(args) && items.size()<limit(args)){
      detail["reference"]=reference_token(doc,scene,ref);items.push_back(std::move(detail));
    }
    ++count;
  }
  auto out=page(std::move(items),count,args);out["body"]=body;out["coordinates"]="world mm";out["scanned"]=entities;
  out["status"]=count==0?"no_match":args.value("ambiguity","all")=="unique" && count!=1?"ambiguous":"matched";
  out["selection_allowed"]=count>0 && (args.value("ambiguity","all")=="all" || count==1);
  return out;
}
json validate_design(const Document& doc,const Scene& scene,const json& args,const std::function<bool()>& cancelled) {
  auto bodies=args.contains("select")?args["select"].get<std::vector<std::string>>():scene.all_bodies();
  json items=json::array();bool valid=scene.unresolved.empty();
  for(size_t i=offset(args);i<bodies.size()&&items.size()<limit(args);++i){
    if(cancelled && cancelled())throw Error("cancelled");
    const auto* node=scene.node(bodies[i]);if(!node)throw Error("Unknown validation body: "+bodies[i]);
    auto shape=node_world_shape(doc,scene,bodies[i]);const bool ok=!shape.IsNull()&&BRepCheck_Analyzer(shape).IsValid();valid=valid&&ok;
    int solids=0;for(TopExp_Explorer ex(shape,TopAbs_SOLID);ex.More();ex.Next())++solids;
    GProp_GProps volume,area;BRepGProp::VolumeProperties(shape,volume);BRepGProp::SurfaceProperties(shape,area);
    items.push_back({{"id",bodies[i]},{"valid",ok},{"solids",solids},{"volume_mm3",volume.Mass()},{"area_mm2",area.Mass()},{"representation",node->representation}});
  }
  auto out=page(std::move(items),bodies.size(),args);out["valid_page"]=valid;out["unresolved"]=scene.unresolved.size();out["scope"]="Geometry validity and exact measurements; not a manufacturing assessment.";return out;
}

json resources() {
  return {{"resources", json::array({{{"uri", "opad://guide/agent"}, {"name", "agent-guide"}, {"title", "OPAD agent guide"},
    {"description", "Units and expressions, identifiers and body_ids per feature kind, base-plane frames, sketch geometry, profiles, references, feature conventions, new body names and batch references."},
    {"mimeType", "text/markdown"}, {"size", guide().size()}}})}};
}

json read_resource(const std::string& uri) {
  if (uri != "opad://guide/agent") throw Error("Unknown resource " + uri + "; resources/list names opad://guide/agent.");
  return {{"contents", json::array({{{"uri", uri}, {"mimeType", "text/markdown"}, {"text", guide()}}})}};
}
}

namespace opad::commands {
void register_agent_commands(const std::function<void(const CommandInfo&, Handler)>& add) {
  auto bounded=[](json properties){properties["doc"]={{"type","string"}};properties["offset"]={{"type","integer"},{"minimum",0},{"default",0}};properties["limit"]={{"type","integer"},{"minimum",1},{"maximum",100},{"default",25}};return properties;};
  auto run=[](auto fn){return [fn](Document* doc,const json& args){if(!doc)throw Error("Pass a document or bind a live session");return fn(*doc,resolve(*doc),args);};};
  add({"context","Bounded document summary and pages of nodes, sketches, parameters, history and errors",bounded({{"section",{{"type","string"},{"enum",{"summary","nodes","sketches","features","parameters","errors","ai_agent_notes"}},{"default","summary"}}},{"filter",{{"type","string"}}}}),false},run(agent::context));
  add({"sketch_details","Sketch geometry, constraints, degrees of freedom, profiles and connected chains in bounded pages",bounded({{"sketch",{{"type","string"}}},{"section",{{"type","string"},{"enum",{"summary","points","entities","constraints","profiles","chain"}},{"default","summary"}}},{"entity",{{"type","integer"}}}}),false},run(agent::sketch_details));
  const json number={{"type","number"}},axis={{"type","string"},{"enum",{"x","y","z"}}};
  const json vector={{"type","array"},{"items",number},{"minItems",3},{"maxItems",3}};
  const json filters={{"type","object"},{"additionalProperties",false},{"properties",{
    {"curve",{{"type","string"},{"enum",{"line","circle","ellipse","hyperbola","parabola","bezier","bspline","offset","other"}}}},
    {"surface",{{"type","string"},{"enum",{"plane","cylinder","cone","sphere","torus","bezier","bspline","revolution","extrusion","offset","other"}}}},
    {"parallel_to",axis},{"normal",{{"type","string"},{"enum",{"+x","-x","+y","-y","+z","-z"}}}},
    {"radius_min",{{"type","number"},{"minimum",0}}},{"radius_max",{{"type","number"},{"minimum",0}}},
    {"at_plane",{{"type","object"},{"additionalProperties",false},{"properties",{{"axis",axis},{"value",number}}},{"required",{"axis","value"}}}},
    {"bounds",{{"type","object"},{"additionalProperties",false},{"properties",{{"min",vector},{"max",vector}}},{"required",{"min","max"}}}}
  }}};
  add({"query_entities","Paged geometric selection with evidence and current reference tokens. Filters are ANDed in world mm. at_plane selects entities whose entire bounding box lies on an axis-aligned plane; parallel_to matches straight edges; bounds requires full containment. unique ambiguity mode never silently chooses among matches. Rerun after regeneration.",bounded({
    {"body",{{"type","string"}}},{"kind",{{"type","string"},{"enum",{"edge","face","vertex"}},{"default","edge"}}},{"filters",filters},
    {"ambiguity",{{"type","string"},{"enum",{"all","unique"}},{"default","all"}}},{"tolerance_mm",{{"type","number"},{"minimum",1e-7},{"maximum",1},{"default",1e-5}}}
  }),false},run([](const Document& d,const Scene& s,const json& a){return agent::query_entities(d,s,a);}));
  add({"entity_details","Exact geometry and paged adjacency with a checked reference token",bounded({{"ref",{{"type",{"string","object"}}}},{"feature",{{"type","string"}}}}),false},run(agent::entity_details));
  add({"resolve_reference","Check a reference token, optionally remap only a unique proven geometric match",{{"doc",{{"type","string"}}},{"reference",{{"type","object"},{"required",{"document","ref"}}}},{"remap",{{"type","boolean"},{"default",false}}}},false},run([](const Document& d,const Scene& s,const json& a){return agent::resolve_reference(d,s,a.at("reference"),a.value("remap",false));}));
  add({"validate","Exact solid counts, volumes, areas and kernel validity in bounded pages",bounded({{"select",{{"type","array"},{"items",{{"type","string"}}}}}}),false},run([](const Document& d,const Scene& s,const json& a){return agent::validate_design(d,s,a);}));
  add({"feature_schema","Input schema, defaults and an example for one supported feature kind",{{"kind",{{"type","string"}}}},false},[](Document*,const json& a){
    const auto kind=a.at("kind").get<std::string>();const auto schema=agent::feature_schema(kind);json example=json::object();
    for(const auto& input:design::feature_spec(kind)->inputs){
      if(!input.show_if.empty()){
        const auto equal=input.show_if.find('=');const auto key=input.show_if.substr(0,equal),choices=input.show_if.substr(equal+1);
        const auto value=example.contains(key)?(example[key].is_string()?example[key].get<std::string>():example[key].dump()):std::string();
        if(("|"+choices+"|").find("|"+value+"|")==std::string::npos)continue;
      }
      if(!input.def.is_null()){example[input.name]=input.def;continue;}
      if(input.optional)continue;
      if(input.type=="profiles")example[input.name]=json::array({{{"sketch","<sketch-id from context>"},{"at",{5,5}}}});
      else if(input.type=="axis")example[input.name]={{"base","z"}};
      else if(input.type=="plane")example[input.name]={{"base","xy"}};
      else if(input.type=="path")example[input.name]={{"sketch","<sketch-id from context>"}};
      else if(input.type=="points")example[input.name]=json::array({{{"sketch","<sketch-id from context>"},{"point",1}}});
      else example[input.name]=json::array({{{"body","<body-id from context>"},{"kind",input.type=="faces"?"face":input.type=="edges"?"edge":"body"},{"index",0}}});
      if(example[input.name].is_array())while(example[input.name].size()<size_t(input.min_count))example[input.name].push_back(example[input.name].front());
    }
    return json{{"kind",kind},{"inputSchema",schema},{"example",{{"tool","feature"},{"arguments",{{"kind",kind},{"inputs",example}}}}},{"reference_help","Replace placeholders with current context IDs. Request sketch_details profiles for a valid at/boundary; entity_details provides face/edge tokens. Headless calls also require doc."}};
  });
  json editInputs={{"type","object"},{"additionalProperties",false},{"properties",json::object()}};
  for(const char* key:{"distance","x","y","cx","cy","angle","scale","tolerance","first","second"})editInputs["properties"][key]={{"type",{"number","string"}}};
  editInputs["properties"]["round"]={{"type","boolean"},{"default",true}};
  editInputs["properties"]["boundary"]={{"type","integer"},{"minimum",1}};
  editInputs["properties"]["at"]={{"type","array"},{"items",{{"type","number"}}},{"minItems",2},{"maxItems",2}};
  add({"sketch_tool","Modify sketch geometry using the same offset, transform, repair and chain algorithms as the desktop. Coordinates are local mm; angle inputs accept degree expressions.",
    {{"doc",{{"type","string"}}},{"target",{{"type","string"}}},{"tool",{{"type","string"},{"enum",{"offset","move","copy","rotate","scale","mirror","split","extend","heal","break_intersections","chamfer","delete"}}}},
     {"entities",{{"type","array"},{"items",{{"type","integer"},{"minimum",1}}},{"maxItems",10000}}},{"chain",{{"type","boolean"},{"default",false}}},{"inputs",editInputs},{"by",{{"type","string"}}}},true},
    [](Document* doc,const json& a){
      if(!doc)throw Error("Pass a document or bind a live session");
      const auto scene=resolve(*doc);const auto target=a.at("target").get<std::string>();const auto* item=scene.sketch(target);if(!item)throw Error("Sketch no longer exists. Request context section sketches.");
      auto sk=design::Sketch::from_json(item->geometry);auto ids=a.value("entities",std::vector<int>{});
      for(int id:ids)if(!sk.entity(id)&&!sk.point(id)&&!sk.constraint(id))throw Error("Unknown sketch entity ID: "+std::to_string(id));
      if(a.value("chain",false))ids=design::connected_entities(sk,ids);
      std::vector<design::ParamDef> definitions;for(const auto& p:scene.params)definitions.push_back({p.id,p.name,p.expr,p.comment});const design::ParamTable parameters(definitions,scene.units);
      const auto tool=a.at("tool").get<std::string>();const auto inputs=a.value("inputs",json::object());
      auto length=[&](const char* key,double fallback){return design::eval_input(parameters,inputs.value(key,json(fallback)),design::Dim::Length);};
      auto single=[&]{if(ids.size()!=1)throw Error(tool+": select exactly one entity or point");return ids.front();};
      if(tool=="offset")design::offset_entities(sk,ids,length("distance",5),inputs.value("round",true));
      else if(tool=="move" || tool=="copy" || tool=="rotate" || tool=="scale" || tool=="mirror") {
        design::SketchTransform transform;transform.x=length("x",0);transform.y=length("y",0);transform.cx=length("cx",0);transform.cy=length("cy",0);
        transform.angle=design::eval_input(parameters,inputs.value("angle",json(0)),design::Dim::Angle);transform.scale=design::eval_input(parameters,inputs.value("scale",json(1)),design::Dim::None);transform.mirror=tool=="mirror";
        design::transform_entities(sk,ids,transform,tool=="copy" || tool=="mirror");
      }else if(tool=="heal"){const auto tolerance=length("tolerance",.01);design::heal_endpoints(sk,tolerance);design::heal_to_curves(sk,tolerance);}
      else if(tool=="split" || tool=="extend") {const auto at=inputs.at("at").get<std::array<double,2>>();if(tool=="split")design::split_entity(sk,single(),at[0],at[1]);else design::extend_entity(sk,single(),inputs.at("boundary").get<int>(),at[0],at[1]);}
      else if(tool=="break_intersections")design::break_intersections(sk,ids);
      else if(tool=="chamfer")design::chamfer_corner(sk,single(),length("first",1),length("second",1));
      else if(tool=="delete")for(int id:ids)sk.remove(id);
      else throw Error("Unsupported sketch tool: "+tool);
      sk.validate();return design::apply_ops(*doc,{design::make_edit_op(target,{{"geometry_delta",design::sketch_delta(item->geometry,sk.to_json())}})},a.value("by",""));
    });
}
}
