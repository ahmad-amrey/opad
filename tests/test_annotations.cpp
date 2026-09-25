#include "check.hpp"
#include "opad/core.hpp"
#include "opad/agent.hpp"
#include "opad/live.hpp"
#include <limits>

using namespace opad;
static json drawing() {
  return {{"plane",{{"origin",{1.,2.,3.}},{"x",{1.,0.,0.}},{"y",{0.,1.,0.}}}},
    {"strokes",json::array({{{"color","red"},{"width",2},{"points",{{0.,0.},{2.,3.},{5.,4.}}}},
                           {{"color","blue"},{"width",6},{"points",{{1.,0.},{4.,2.}}}}})}};
}
TEST(agent_notes_and_drawing_round_trip) {
  auto doc=Document::create();
  const auto id=commands::run("annotate",{{"anchor","point/1,2,3"},{"text","Round this corner"},{"style","ai_agent"},{"drawing",drawing()}},&doc).at("id").get<std::string>();
  const auto original=doc.serialize();
  CHECK(original.find("\"strokes\": [\n")!=std::string::npos);
  CHECK_EQ(Document::parse(original).serialize(),original);
  commands::run("annotate",{{"anchor","point/1,2,3"},{"reply_to",id},{"text","Use radius 2 mm"}},&doc);
  auto scene=resolve(doc);
  CHECK_EQ(scene.annotations.size(),1u);CHECK_EQ(scene.annotations.front().drawing,drawing());
  CHECK_EQ(scene.annotations.front().comments.size(),1u);
  CHECK_EQ(agent::context(doc,scene,json::object())["ai_agent_notes"],1);
  CHECK_EQ(agent::context(doc,scene,{{"section","ai_agent_notes"}})["items"][0]["id"],json(id));
  const auto details=commands::run("annotations",{{"id",id},{"style","ai_agent"}},&doc);
  CHECK_EQ(details["annotations"][0]["drawing"],drawing());
  CHECK(commands::run("annotations",{{"style","warning"}},&doc)["annotations"].empty());
  doc.append({{"op","edit"},{"target",id},{"set",{{"style","issue"}}}});
  CHECK_EQ(agent::context(doc,resolve(doc),json::object())["ai_agent_notes"],0);
  auto deletion=commands::run("delete_annotation",{{"target",id}},&doc)["id"].get<std::string>();
  CHECK(resolve(doc).annotations.empty());
  doc.append({{"op","delete"},{"target",deletion}});
  CHECK_EQ(resolve(doc).annotations.front().comments.size(),1u);
  CHECK_EQ(resolve(Document::parse(doc.serialize())).annotations.front().drawing,drawing());
  CHECK_THROWS(commands::run("delete_annotation",{{"target",deletion}},&doc));
}
TEST(drawing_validation_and_paging) {
  auto doc=Document::create();
  json op={{"op","annotation"},{"anchor","point/0,0,0"},{"text","Test"},{"drawing",drawing()}};
  auto bad=op;bad["drawing"]["plane"]["y"]={1,0,0};CHECK_THROWS(doc.append(bad));
  bad=op;bad["drawing"]["strokes"][0]["width"]=5;CHECK_THROWS(doc.append(bad));
  bad=op;bad["drawing"]["strokes"][0]["color"]="green";CHECK_THROWS(doc.append(bad));
  bad=op;bad["drawing"]["strokes"][0]["points"][0]={std::numeric_limits<double>::infinity(),0};CHECK_THROWS(doc.append(bad));
  bad=op;bad["drawing"]["strokes"][0]["points"]=json::array();for(int i=0;i<8193;++i)bad["drawing"]["strokes"][0]["points"].push_back({i,0});CHECK_THROWS(doc.append(bad));
  CHECK(doc.ops.empty());
  const auto id=doc.append(op).id;
  CHECK_THROWS(doc.append({{"op","edit"},{"target",id},{"set",{{"drawing",bad["drawing"]}}}}));
  CHECK_EQ(doc.ops.size(),1u);
  for(int i=0;i<3;++i)doc.append(op);
  auto page=commands::run("annotations",{{"offset",1},{"limit",2}},&doc);
  CHECK_EQ(page["annotations"].size(),2u);CHECK_EQ(page["total"],4);CHECK_EQ(page["next_offset"],3);
  const auto schema=agent::live_schema("annotate");
  CHECK(schema["properties"]["drawing"]["properties"].contains("strokes"));
  CHECK(agent::live_mutation("delete_annotation"));
}
TEST(existing_operation_text_is_preserved) {
  auto doc=Document::create();
  const auto id=doc.append({{"op","sketch"},{"name","Old inline sketch"},{"plane",{{"base","xy"}}},{"geometry",{{"points",json::array()},{"entities",json::array()}}}}).id;
  doc.ops.back().raw=doc.ops.back().data.dump(); // Older version stored the sketch on one line.
  const auto original=doc.serialize();auto reopened=Document::parse(original);
  CHECK_EQ(reopened.serialize(),original);
  reopened.append({{"op","annotation"},{"anchor","point/0,0,0"},{"text","New review"}});
  CHECK(reopened.serialize().find(doc.ops.back().raw+"\n")!=std::string::npos);
  CHECK_EQ(reopened.find_op(id)->raw,doc.ops.back().raw);
}
int main(int argc,char** argv){return check::run_all(argc,argv);}
