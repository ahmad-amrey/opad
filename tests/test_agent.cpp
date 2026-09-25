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
  agent::validate_input(agent::live_output_schema("feature"),{{"result",{{"feature_id","history"},{"body_ids",{"body"}}}}});
  CHECK_THROWS(agent::validate_input(agent::live_output_schema("feature"),{{"result",{{"ids",{"history"}}}}}));
  agent::validate_input(agent::live_output_schema("sketch"),{{"result",{{"sketch_id","sketch"}}}});
  CHECK_THROWS(agent::validate_input(agent::live_output_schema("sketch"),{{"result",{{"sketch_id",12}}}}));
  CHECK_EQ(agent::transaction_policy()["scope"],"connection");
  CHECK(agent::live_guide().at("example").size()>4);
  CHECK_THROWS(agent::validate_input(agent::live_schema("request_status"),{{"request_id",""}}));
  CHECK(agent::live_mutation("save"));
  agent::validate_input(agent::live_schema("save"),{{"expected_revision",3},{"request_id","save-1"}});
  agent::validate_input(agent::live_schema("save"),{{"path","C:/output/part.opad"},{"overwrite",true},{"expected_revision",3},{"request_id","save-2"}});
  CHECK_THROWS(agent::validate_input(agent::live_schema("save"),{{"path","C:/output/part.opad"}}));
  CHECK_THROWS(agent::validate_input(agent::live_schema("save"),{{"expected_revision",3},{"request_id","save-1"},{"transaction","pending"}}));
  agent::validate_input(agent::live_output_schema("save"),{{"result",{{"path","C:/output/part.opad"},{"saved_revision",3},{"dirty",false}}}});
  std::cout<<"agent schemas, bounded context, exact validation and stale references: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
