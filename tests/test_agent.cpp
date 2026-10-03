#include <cstdio>
#include "opad/agent.hpp"
#include "opad/live.hpp"
#include "opad/core.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch.hpp"
#include "check.hpp"
#include <iostream>

using namespace opad;
void check_schema(const json& schema){
  if(!schema.is_object())return;
  if(schema.contains("required"))CHECK(schema["required"].is_array());
  if(schema.contains("properties")){
    CHECK(schema["properties"].is_object());for(const auto& property:schema["properties"].items())check_schema(property.value());
  }
  for(const char* key:{"anyOf","allOf","oneOf"})if(schema.contains(key)){CHECK(schema[key].is_array());for(const auto& child:schema[key])check_schema(child);}
  for(const char* key:{"items","if","then","else"})if(schema.contains(key))check_schema(schema[key]);
}
int main(){try {
  Document doc=Document::create();
  commands::run("param",{{"name","height"},{"expr","10 mm"}},&doc);
  commands::run("feature",{{"kind","box"},{"inputs",{{"length","60 mm"},{"width","40 mm"},{"height","height"}}}},&doc);
  auto scene=resolve(doc);const auto body=scene.all_bodies().front();
  auto summary=agent::context(doc,scene,json::object());CHECK_EQ(summary["bodies"],1);CHECK_EQ(summary["parameters"],1);CHECK(summary.dump().size()<2000);
  auto nodes=agent::context(doc,scene,{{"section","nodes"},{"limit",1}});CHECK_EQ(nodes["items"].size(),1u);CHECK(nodes["next_offset"].is_null());
  const auto count=doc.ops.size();
  CHECK_THROWS(commands::run("feature",{{"kind","box"},{"inputs",{{"height",true}}}},&doc));
  CHECK_THROWS(commands::run("feature",{{"kind","box"},{"inputs",{{"heigth","5 mm"}}}},&doc));
  CHECK_EQ(doc.ops.size(),count);
  CHECK_THROWS(agent::validate_input(agent::feature_schema("extrude"),json::object()));
  agent::validate_input(agent::feature_schema("box"),{{"height","height * 2"}});
  Ref bottom,top;bool haveBottom=false,haveTop=false;
  for(int i=0;i<6;++i){Ref ref;ref.body=body;ref.kind=Ref::Kind::Face;ref.index=i;const auto detail=agent::entity_details(doc,scene,{{"ref",ref.to_json()}});
    if(detail.contains("normal") && detail["normal"][2].get<double>()<-.9){bottom=ref;haveBottom=true;}
    if(detail.contains("normal") && detail["normal"][2].get<double>()>.9){top=ref;haveTop=true;}
  }
  CHECK(haveBottom&&haveTop);const auto bottomToken=agent::reference_token(doc,scene,bottom),topToken=agent::reference_token(doc,scene,top);
  CHECK_EQ(agent::resolve_reference(doc,scene,bottomToken)["status"],"resolved");
  commands::run("param",{{"name","height"},{"expr","15 mm"}},&doc);scene=resolve(doc);
  CHECK_EQ(agent::resolve_reference(doc,scene,bottomToken)["status"],"stale");
  CHECK_EQ(agent::resolve_reference(doc,scene,bottomToken,true)["status"],"resolved");
  CHECK_EQ(agent::resolve_reference(doc,scene,topToken,true)["status"],"stale");
  auto wrong=bottomToken;wrong["document"]="another-document";CHECK_EQ(agent::resolve_reference(doc,scene,wrong)["status"],"wrong_document");
  auto query=agent::query_entities(doc,scene,{{"body",body},{"filters",{{"at_plane",{{"axis","z"},{"value",15}}}}}});
  CHECK_EQ(query["total"],4);CHECK_EQ(query["status"],"matched");
  for(const auto& item:query["items"])CHECK_EQ(agent::resolve_reference(doc,scene,item["reference"])["status"],"resolved");
  auto firstPage=agent::query_entities(doc,scene,{{"body",body},{"limit",2},{"ambiguity","unique"},{"filters",{{"parallel_to","x"}}}});
  CHECK_EQ(firstPage["total"],4);CHECK_EQ(firstPage["items"].size(),2u);CHECK_EQ(firstPage["next_offset"],2);
  CHECK_EQ(firstPage["status"],"ambiguous");CHECK(!firstPage["selection_allowed"].get<bool>());
  auto topFace=agent::query_entities(doc,scene,{{"body",body},{"kind","face"},{"ambiguity","unique"},{"filters",{{"normal","+z"}}}});
  CHECK_EQ(topFace["total"],1);CHECK(topFace["selection_allowed"].get<bool>());
  CHECK_EQ(agent::query_entities(doc,scene,{{"body",body},{"filters",{{"at_plane",{{"axis","z"},{"value",10}}}}}})["total"],0);
  CHECK_THROWS(agent::query_entities(doc,scene,{{"body",body}},[]{return true;}));
  auto validation=agent::validate_design(doc,scene,json::object());CHECK(validation["valid_page"].get<bool>());CHECK_EQ(validation["items"][0]["solids"],1);
  CHECK(std::abs(validation["items"][0]["volume_mm3"].get<double>()-36000)<1e-5);
  design::Sketch sketch;const auto a=sketch.add_point(0,0),b=sketch.add_point(20,0),c=sketch.add_point(20,20),d=sketch.add_point(0,20);
  const auto first=sketch.add_line(a,b);sketch.add_line(b,c);sketch.add_line(c,d);sketch.add_line(d,a);
  commands::run("sketch",{{"geometry",sketch.to_json()}},&doc);scene=resolve(doc);const auto sk=scene.sketches.back().id;
  const auto edges=agent::sketch_details(doc,scene,{{"sketch",sk},{"section","entities"},{"limit",2}});CHECK_EQ(edges["items"].size(),2u);CHECK_EQ(edges["next_offset"],2);
  CHECK_EQ(agent::sketch_details(doc,scene,{{"sketch",sk},{"section","chain"},{"entity",first}})["total"],4);
  const auto profiles=agent::sketch_details(doc,scene,{{"sketch",sk},{"section","profiles"}});CHECK_EQ(profiles["total"],1);CHECK(!profiles["items"][0]["boundary"].empty());
  commands::run("sketch_tool",{{"target",sk},{"tool","offset"},{"entities",{first}},{"chain",true},{"inputs",{{"distance","2 mm"}}}},&doc);
  scene=resolve(doc);CHECK(scene.sketch(sk)->geometry.at("entities").size()>4);
  commands::run("sketch_edit",{{"target",sk},{"plane",{{"base","yz"}}}},&doc);scene=resolve(doc);CHECK(std::abs(scene.sketch(sk)->frame.normal()[0])>.99);
  for(const auto& command:commands::list()) {
    const auto schema=agent::command_schema(command);CHECK_EQ(schema["type"],"object");
    check_schema(schema);
    if(command.name=="feature"){CHECK(schema["properties"]["kind"].contains("enum"));CHECK_THROWS(agent::validate_input(schema,{{"doc","x"},{"kind","box"},{"inputz",json::object()}}));}
    if(command.name=="context"){CHECK_THROWS(agent::validate_input(schema,{{"doc","x"},{"limit",1000}}));CHECK(!agent::command_schema(command,true)["properties"].contains("doc"));}
  }
  for(const auto& spec:design::feature_specs()){
    const auto discovery=commands::run("feature_schema",{{"kind",spec.kind}});
    check_schema(discovery.at("inputSchema"));
    agent::validate_input(discovery.at("inputSchema"),discovery.at("example").at("arguments").at("inputs"),spec.kind);
  }
  for(const auto& tool:agent::live_tools()){check_schema(tool["inputSchema"]);check_schema(tool.at("outputSchema"));}
  // TODO 10 B12: agents load tools/list on every discovery; it must not creep back up (162 KB before, 98 KB after).
  // Raise a budget only for tools worth their size, and say so in the commit.
  const size_t live=agent::live_tools().dump().size();
  size_t headless=0;for(const auto& c:commands::list())headless+=agent::command_schema(c).dump().size()+c.description.size();
  std::printf("tools/list: live %zu bytes, headless schemas %zu bytes\n",live,headless);
  CHECK(live<105000);
  CHECK(headless<51200);  // 50000 until the KiCad commands (kicad_models, kicad_sync_preview), 50500 until linked assets (the asset command, kept out of the live list too, and import's link)
  // Trimmed for the list, still checked in full: sketch_edit's geometry.
  CHECK(agent::live_schema("sketch_edit")["properties"]["geometry"]==agent::live_schema("sketch")["properties"]["geometry"]);
  agent::validate_input(agent::live_output_schema("feature"),{{"result",{{"feature_id","history"},{"body_ids",{"body"}}}}});
  CHECK_THROWS(agent::validate_input(agent::live_output_schema("feature"),{{"result",{{"ids",{"history"}}}}}));
  agent::validate_input(agent::live_output_schema("sketch"),{{"result",{{"sketch_id","sketch"}}}});
  CHECK_THROWS(agent::validate_input(agent::live_output_schema("sketch"),{{"result",{{"sketch_id",12}}}}));
  CHECK_EQ(agent::transaction_policy()["scope"],"connection");
  CHECK(agent::live_guide().at("example").size()>4);
  CHECK_THROWS(agent::validate_input(agent::live_schema("request_status"),{{"request_id",""}}));
  CHECK(agent::live_mutation("save"));
  // Gap log #4: measure is a read unless pinned; then it needs the revision and a request id like any write.
  CHECK(!agent::live_mutation("measure",{{"kind","bbox"}}) && agent::live_mutation("measure",{{"pin",true}}));
  agent::validate_input(agent::live_schema("measure"),{{"kind","bbox"},{"refs",{"a"}}});
  agent::validate_input(agent::live_schema("measure"),{{"queries",{{{"kind","distance"},{"refs",{"a","b"}}}}}});
  agent::validate_input(agent::live_schema("save"),{{"expected_revision",3},{"request_id","save-1"}});
  agent::validate_input(agent::live_schema("save"),{{"path","C:/output/part.opad"},{"overwrite",true},{"expected_revision",3},{"request_id","save-2"}});
  CHECK_THROWS(agent::validate_input(agent::live_schema("save"),{{"path","C:/output/part.opad"}}));
  CHECK_THROWS(agent::validate_input(agent::live_schema("save"),{{"expected_revision",3},{"request_id","save-1"},{"transaction","pending"}}));
  agent::validate_input(agent::live_output_schema("save"),{{"result",{{"path","C:/output/part.opad"},{"saved_revision",3},{"dirty",false}}}});
  Document typed=Document::create();const auto component=commands::run("component",{{"name","Typed component"}},&typed);
  CHECK_EQ(component["component_id"],component["id"]);CHECK_EQ(component["operation_ids"].size(),1u);
  agent::validate_input(agent::live_output_schema("component"),{{"result",component}});
  CHECK(agent::live_mutation("model_batch"));
  const json batchStep={{"id","box"},{"command","feature"},{"arguments",{{"kind","box"}}}};
  agent::validate_input(agent::live_schema("model_batch"),{{"steps",json::array({batchStep})},{"expected_revision",0},{"request_id","batch"}});
  CHECK_THROWS(agent::validate_input(agent::live_schema("model_batch"),{{"steps",json::array()},{"expected_revision",0},{"request_id","batch"}}));
  CHECK_THROWS(agent::validate_input(agent::live_schema("model_batch"),{{"steps",json::array({{{"id","unsafe"},{"command","export"},{"arguments",json::object()}}})},{"expected_revision",0},{"request_id","batch"}}));
  std::cout<<"agent schemas, bounded context, exact validation and stale references: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
