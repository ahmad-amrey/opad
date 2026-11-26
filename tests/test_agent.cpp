#include <cstdio>
#include <set>
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
  {  // two chains apart (an L and a line): one offset relationship each, as if each were offset alone
    design::Sketch two;const auto p=two.add_point(0,30),q=two.add_point(0,40),r=two.add_point(10,40),s=two.add_point(30,30),t=two.add_point(30,40);
    const auto l1=two.add_line(p,q),l2=two.add_line(q,r),l3=two.add_line(s,t);
    commands::run("sketch",{{"geometry",two.to_json()}},&doc);scene=resolve(doc);const auto id=scene.sketches.back().id;
    commands::run("sketch_tool",{{"target",id},{"tool","offset"},{"entities",{l1,l2,l3}},{"inputs",{{"distance","2 mm"}}}},&doc);
    const auto made=design::Sketch::from_json(resolve(doc).sketch(id)->geometry);
    std::vector<int> sources;for(const auto& c:made.constraints)if(c.type==design::SkConstraint::Type::Offset)sources.push_back(c.sources);
    CHECK((sources==std::vector<int>{2,1}));CHECK_EQ(made.entities.size(),6u);
  }
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
  // TODO 11: the tracks raised these from 105000 / 50000 each on its own. Live: +2.5 KB explode (UI-35) and +0.5 KB component
  // on feature, sketch and batches (UI-33) on t2b, +2 KB related (UI-94: its refs carry the reference schema) on t6, +0.1 KB
  // KiCad (UI-134) on t4. Headless: +14.95 KB the drawing commands on t5a (project UI-77, sheets UI-76, materials UI-140, bom
  // UI-83, export views UI-87, templates and document properties UI-78; 8.4 KB at the wave-3 merge, +6.55 KB at its final merge:
  // annotations UI-79..81, section, detail and broken views UI-82, parts lists, balloons and issued revisions UI-84), +2 KB explode and component on t2b, +2.7 KB the KiCad
  // commands, linked assets, the image canvas (UI-70) and sketch_tool's project on t4 (1.2 KB at the wave-3 merge, +1.5 KB at its
  // final merge), +1.5 KB related on t6. Merged, what the tracks added within their own budgets adds up
  // as well: measured at the wave-3 merge, live 112671 bytes (2.6 KB past the raises) and headless 66678 (3.6 KB past); after
  // t4's and t5a's final merges, live 113122 and headless 74716; with t2a's final merge (measure's mode and length kind UI-144,
  // view's home UI-47) live 113583. A merge that grows a list past the limits raises them by what it measured and says so in
  // the commit.
  // The git tools (git_status ... git_tag, opad::agent::git_tools: 15 tools with their argument and refusal descriptions and
  // the brief outputSchema each) raised it by 19.1 KB to 132668 bytes: precise enough for an agent to act on refusals.
  // mcp-eval 2026-10-06: the sketch's one id space said in its description, the ids' schema and the brief geometry of
  // sketch_edit and batch steps (+0.94 KB, measured 133607).
  // The clearance tool (read-only, both servers: a and b bodies or components, min distance, intersecting/touching pairs
  // with overlap volumes, paged): +1.5 KB, measured 135241.
  // An agent's evaluation (smartknob): git tools' refusals and previews said more (+0.66 KB, to 133329), tree paging and
  // viewport_image's opt-in visible_ids (+0.28 KB): replies they shrink by tens of KB; open_document and new_document
  // (+1.6 KB with their output schemas), so an agent starts without a person or the CLI.
  // Motion and simulation (+15.0 KB live, measured 152886; +6.9 KB headless, measured 85949): joint (joints, relations and
  // snapping, its frames as references or axes), joint_set, mechanism (with the engines), study (motion, dynamic, static,
  // modal), load, the gear feature kind, and joint steps in model_batch; joint_set and load stay out of batches and a load's
  // faces take a light schema (on) to keep it there.
  // Thermal studies (+1.0 KB live, measured 153916; +1.2 KB headless, measured 87126): the thermal load kinds and their
  // fields (h, ambient, velocity, emissivity, fan, count) and the thermal study kind.
  // Mesh to solid (+2.06 KB live, measured 154946 before thermal; +1.24 KB headless, measured 87191 before thermal): the
  // mesh_to_solid command and the mesh_solid feature kind. Both merged: 155874 live, 88205 headless.
  // Align (+2.2 KB live, measured 158088; +0.73 KB headless, measured 88934): the align feature kind (from/to point, line or
  // plane, motion, flip, offset, angle) and the align_sketch command, also a batch step.
  // Offset relationship (measured 158654 live, 89500 headless): sketch_tool's offset holds its copy by an editable offset
  // dimension (inputs.relation).
  // git_init's own_folder (measured 158967 live): a shared folder refused unless it says move or stay.
  // 2026-11-24, the agent guide by section (live-MCP stress test: include_guide returned ~54 KB): live_diagnostics'
  // guide_section input and its description (measured 159115 live).
  // set_phase (+0.8 KB live; live only): the assistant's few words for what it is working on, shown in the AI chip and
  // as a heading in the activity panel (both merged: measured 160021).
  // Rules, mirror names and ellipse dimensions (measured 159596 live, 90277 headless): query_entities' length/area/loop/bounds_mode filters and
  // expressions, the mirror's name_map, the major_radius/minor_radius ellipse dimensions.
  // Joint re-anchoring (joint's id says at/part/base re-anchor it): measured 160816 live, 90304 headless, within the limits.
  // Second MCP stress test (#17, #25, #26; measured 161670 live, 90918 headless): features and ops take offset, limit and
  // detail (compact pages by default live: replies of 52 KB and 67 KB before), live_state's change_history, joint_set's
  // preview said to have no id. With joint re-anchoring merged: measured 161693 live, 90946 headless.
  // Point clouds (+4.4 KB live, measured 166148; +2.8 KB headless, measured 93856): the cloud read (info, points, region,
  // slice), cloud_sketch, import's scale and up, and the five cloud feature kinds with the region input.
  // Fitted references (measured 165625 live, 93556 headless): the fit_reference and scan_axes tools, {fit, result} in every
  // plane and axis input, Align's frame input, best-fit points in the construction plane and axis.
  CHECK(live<165800);  // 161800 before fitted references; 160900 before paging; all three merged: measured 160762; 160200 before rules, mirror names and ellipse radii; 159300 before set_phase; 159100 before the guide by section; 158600 before the offset relationship; 156400 before Align; 154400 before mesh to solid; 153400 before thermal; 138400 before motion and simulation; both agent-evaluation fix sets merged (measured 137886); 135400 for each alone, 133400 before them; 114300 before the git tools; the drawing commands are file-level for live agents (core/src/live.cpp); 113400 until t2a's final merge
  CHECK(headless<93700);  // 91050 before fitted references; 90450 before paging; 89950 before rules, mirror names and ellipse radii; 89450 before the offset relationship; 88700 before Align; 87600 before mesh to solid; 86500 before thermal; 79100 before motion and simulation; 71050 until t4's final merge (the canvas command, sketch_tool's project: +1.5 KB), 72550 until t5a's (+6.55 KB)
  // Trimmed for the list, still checked in full: sketch_edit's geometry.
  CHECK(agent::live_schema("sketch_edit")["properties"]["geometry"]==agent::live_schema("sketch")["properties"]["geometry"]);
  agent::validate_input(agent::live_output_schema("feature"),{{"result",{{"feature_id","history"},{"body_ids",{"body"}}}}});
  CHECK_THROWS(agent::validate_input(agent::live_output_schema("feature"),{{"result",{{"ids",{"history"}}}}}));
  agent::validate_input(agent::live_output_schema("sketch"),{{"result",{{"sketch_id","sketch"}}}});
  CHECK_THROWS(agent::validate_input(agent::live_output_schema("sketch"),{{"result",{{"sketch_id",12}}}}));
  CHECK_EQ(agent::transaction_policy()["scope"],"connection");
  CHECK(agent::live_guide().at("example").size()>4);
  // The guide by section (live-MCP stress test 2026-11-24: include_guide returned ~54 KB). The index is small, every section
  // reads back as itself, together they are the whole guide, and the resource takes the section after '#'.
  {
    const auto index=agent::guide_index();
    CHECK(index.dump().size()<6000);CHECK(index["sections"].size()>=10);CHECK_EQ(index["total_bytes"],agent::guide().size());
    std::string whole=agent::guide_section("overview")["text"];std::set<std::string> ids;
    for(const auto& s:index["sections"]){
      const auto id=s["id"].get<std::string>();CHECK(ids.insert(id).second);CHECK(id.find(' ')==std::string::npos);
      const auto section=agent::guide_section(id);CHECK_EQ(section["text"].get<std::string>().size(),s["bytes"].get<size_t>());
      CHECK(section["text"].get<std::string>().rfind("## "+section["title"].get<std::string>(),0)==0);
      whole+=section["text"].get<std::string>();
      CHECK_EQ(agent::read_resource("opad://guide/agent#"+id)["contents"][0]["text"],section["text"]);
    }
    CHECK(whole==agent::guide());CHECK(ids.count("units-and-expressions"));
    CHECK_EQ(agent::guide_section("all")["text"],agent::guide());
    CHECK_EQ(agent::read_resource("opad://guide/agent")["contents"][0]["text"],agent::guide());
    CHECK_THROWS(agent::guide_section("nothing"));CHECK_THROWS(agent::read_resource("opad://guide/agent#nothing"));
    CHECK_EQ(agent::resources()["resources"].size(),index["sections"].size()+2);  // the guide, its overview and sections
    json out=json::object();agent::add_guide(out,{{"include_guide",true},{"guide_section","nothing"}});
    CHECK(out["agent_guide"].contains("sections") && out["agent_guide_section"].contains("error") && !out["agent_guide_section"]["sections"].empty());
    out=json::object();agent::add_guide(out,{{"guide_section","references"}});CHECK_EQ(out["agent_guide_section"]["title"],"References");
    agent::validate_input(agent::live_schema("live_diagnostics"),{{"include_guide",true},{"guide_section","references"}});
  }
  CHECK_THROWS(agent::validate_input(agent::live_schema("request_status"),{{"request_id",""}}));
  CHECK(agent::live_mutation("save"));
  // Gap log #4: measure is a read unless pinned; then it needs the revision and a request id like any write.
  CHECK(!agent::live_mutation("measure",{{"kind","bbox"}}) && agent::live_mutation("measure",{{"pin",true}}));
  // An exploded view is a read unless it is saved (TODO 11 UI-35).
  CHECK(!agent::live_mutation("explode",{{"levels",2}}) && agent::live_mutation("explode",{{"name","Exploded"}}) && agent::live_mutation("explode",{{"update",true}}));
  agent::validate_input(agent::live_schema("explode"),{{"levels",2},{"groups",json::array({json::array({"a","b"})})},{"offsets",{{"a",{0,0,5}}}}});
  CHECK_THROWS(agent::validate_input(agent::live_schema("explode"),{{"offsets",{{"a",{0,5}}}}}));
  CHECK_THROWS(agent::validate_input(agent::live_schema("explode"),{{"t",2}}));
  agent::validate_input(agent::live_schema("measure"),{{"kind","bbox"},{"refs",{"a"}}});
  agent::validate_input(agent::live_schema("measure"),{{"queries",{{{"kind","distance"},{"refs",{"a","b"}}}}}});
  agent::validate_input(agent::live_schema("measure"),{{"kind","area"},{"refs",{"a"}},{"at",{1.0,2.0,0.0}}});  // UI-90: the Area tool's measure, where the object was clicked
  agent::validate_input(agent::live_schema("save"),{{"expected_revision",3},{"request_id","save-1"}});
  agent::validate_input(agent::live_schema("save"),{{"path","C:/output/part.opad"},{"overwrite",true},{"expected_revision",3},{"request_id","save-2"}});
  CHECK_THROWS(agent::validate_input(agent::live_schema("save"),{{"path","C:/output/part.opad"}}));
  CHECK_THROWS(agent::validate_input(agent::live_schema("save"),{{"expected_revision",3},{"request_id","save-1"},{"transaction","pending"}}));
  agent::validate_input(agent::live_output_schema("save"),{{"result",{{"path","C:/output/part.opad"},{"saved_revision",3},{"dirty",false}}}});
  Document typed=Document::create();const auto component=commands::run("component",{{"name","Typed component"}},&typed);
  CHECK_EQ(component["component_id"],component["id"]);CHECK_EQ(component["operation_ids"].size(),1u);
  agent::validate_input(agent::live_output_schema("component"),{{"result",component}});
  // Tree paging: an agent's reply on a big assembly stays small (pages of the roots, or of one component's children).
  Document many=Document::create();std::string holder;
  for(int i=0;i<5;++i){const auto c=commands::run("component",{{"name","C"+std::to_string(i)}},&many);if(!i)holder=c["component_id"];}
  for(int i=0;i<3;++i)commands::run("feature",{{"kind","box"},{"parent",holder},{"inputs",{{"length",1},{"width",1},{"height",1}}}},&many);
  const auto page=commands::run("tree",{{"limit",2}},&many);CHECK_EQ(page["roots"].size(),2u);CHECK_EQ(page["total"],5);CHECK_EQ(page["next_offset"],2);
  const auto last=commands::run("tree",{{"offset",4},{"limit",2}},&many);CHECK_EQ(last["roots"].size(),1u);CHECK(last["next_offset"].is_null());
  const auto inside=commands::run("tree",{{"node",holder},{"limit",2},{"depth",1}},&many);
  CHECK_EQ(inside["total"],3);CHECK_EQ(inside["roots"].size(),2u);CHECK_EQ(inside["roots"][0]["type"],"body");CHECK_EQ(inside["next_offset"],2);
  CHECK_EQ(commands::run("tree",json::object(),&many)["roots"].size(),5u);CHECK(!commands::run("tree",json::object(),&many).contains("total"));  // unpaged as before
  CHECK_THROWS(commands::run("tree",{{"node",inside["roots"][0]["id"]}},&many));  // a body has no children
  agent::validate_input(agent::live_schema("tree"),{{"node",holder},{"offset",2},{"limit",10}});
  CHECK(agent::live_mutation("model_batch"));
  // An agent opens or makes a document itself (a window's start page included): writes with a request_id, no revision.
  CHECK(agent::live_mutation("open_document") && agent::live_mutation("new_document"));
  agent::validate_input(agent::live_schema("new_document"),{{"path","C:/work/new.opad"},{"request_id","new-1"}});
  CHECK_THROWS(agent::validate_input(agent::live_schema("open_document"),{{"path","C:/work/a.opad"}}));
  CHECK(!agent::live_schema("open_document")["properties"].contains("expected_revision"));
  const json batchStep={{"id","box"},{"command","feature"},{"arguments",{{"kind","box"}}}};
  agent::validate_input(agent::live_schema("model_batch"),{{"steps",json::array({batchStep})},{"expected_revision",0},{"request_id","batch"}});
  CHECK_THROWS(agent::validate_input(agent::live_schema("model_batch"),{{"steps",json::array()},{"expected_revision",0},{"request_id","batch"}}));
  CHECK_THROWS(agent::validate_input(agent::live_schema("model_batch"),{{"steps",json::array({{{"id","unsafe"},{"command","export"},{"arguments",json::object()}}})},{"expected_revision",0},{"request_id","batch"}}));
  {  // Second MCP stress test #25: features (52 KB) and ops (67 KB) were one reply; pages of compact items now
    Document d=Document::create();
    for(int i=0;i<55;++i)commands::run("feature",{{"kind","box"},{"inputs",{{"length","10 mm"},{"width","10 mm"},{"height","10 mm"}}}},&d);
    const json all=commands::run("features",json::object(),&d);  // the CLI's and scripts' shape stays
    CHECK(all.is_array());CHECK_EQ(all.size(),55u);CHECK(all[0].contains("inputs"));
    const json page=commands::run("features",{{"limit",2},{"offset",1},{"detail","compact"}},&d);
    CHECK_EQ(page["items"].size(),2u);CHECK_EQ(page["total"],55);CHECK_EQ(page["offset"],1);CHECK_EQ(page["next_offset"],3);
    CHECK_EQ(page["items"][0]["id"],all[1]["id"]);CHECK(!page["items"][0].contains("inputs"));
    CHECK_EQ(page["items"][0]["state"],"ok");CHECK_EQ(page["items"][0]["kind"],"box");CHECK_EQ(page["items"][0]["name"],all[1]["name"]);
    const json last=commands::run("features",{{"offset",54},{"limit",5}},&d);
    CHECK_EQ(last["items"].size(),1u);CHECK(last["next_offset"].is_null());CHECK(last["items"][0].contains("inputs"));
    // The live defaults: a page of live_page_limit compact items, small enough for an agent's context.
    const json args=agent::live_read_defaults("features",json::object());
    CHECK_EQ(args["limit"],agent::live_page_limit);CHECK_EQ(args["detail"],"compact");
    const json first=commands::run("features",args,&d);
    CHECK_EQ(first["items"].size(),size_t(agent::live_page_limit));CHECK_EQ(first["next_offset"],agent::live_page_limit);
    std::printf("features: %zu bytes in full, %zu bytes for a live page\n",all.dump().size(),first.dump().size());
    CHECK(first.dump().size()<8000);
    CHECK_EQ(agent::live_read_defaults("features",{{"limit",3},{"detail","full"}})["detail"],"full");
    CHECK(agent::live_read_defaults("tree",json::object()).empty());
    const json ops=commands::run("ops",agent::live_read_defaults("ops",{{"since",d.ops[2].id}}),&d);
    CHECK_EQ(ops["total"],d.ops.size()-3);CHECK_EQ(ops["items"][0]["id"],d.ops[3].id);CHECK_EQ(ops["items"][0]["state"],"live");
    CHECK_EQ(ops["items"][0]["op"],d.ops[3].type);CHECK(!ops["items"][0].contains("result"));CHECK(ops.dump().size()<8000);
    CHECK(commands::run("ops",json::object(),&d).is_array());
    const json fs=agent::live_schema("features");
    CHECK_EQ(fs["properties"]["limit"]["default"],agent::live_page_limit);CHECK_EQ(fs["properties"]["detail"]["default"],"compact");
    agent::validate_input(fs,{{"offset",50},{"limit",50},{"detail","full"}});
    CHECK_THROWS(agent::validate_input(fs,{{"limit",501}}));
    CHECK_THROWS(agent::validate_input(agent::live_schema("ops"),{{"detail","short"}}));
    // #17: live_state's change history only when asked for.
    agent::validate_input(agent::live_schema("live_state"),{{"change_history",true}});
  }
  {  // #26: joint_set with preview writes nothing, so it answers without an id
    Document d=Document::create();
    const std::string part=commands::run("feature",{{"kind","box"},{"inputs",{{"length","20 mm"},{"width","10 mm"},{"height","5 mm"}}}},&d)["body_ids"][0];
    const std::string joint=commands::run("joint",{{"kind","revolute"},{"part",part},{"at",{{"origin",{0,0,0}},{"z",{0,0,1}}}}},&d)["id"];
    const size_t n=d.ops.size();
    const json tried=commands::run("joint_set",{{"values",{{joint,30}}},{"preview",true}},&d);
    CHECK(tried.contains("values") && !tried.contains("id"));CHECK_EQ(d.ops.size(),n);
    CHECK(agent::live_mutation("joint_set",{{"preview",true}})==false);
    agent::validate_input(agent::live_schema("joint_set"),{{"values",{{joint,30}}},{"preview",true}});  // a read: no revision or request id
    const json set=commands::run("joint_set",{{"values",{{joint,30}}}},&d);
    CHECK(set.contains("id"));CHECK_EQ(d.ops.size(),n+1);
  }
  std::cout<<"agent schemas, bounded context, exact validation and stale references: PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
