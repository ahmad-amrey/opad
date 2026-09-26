#include "opad/design/sketch_pattern.hpp"
#include "opad/design/sketch_modify.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace opad::design {
namespace {
void erase_ids(Sketch& sk,const std::set<int>& ids) {
  sk.id_watermark=sk.next_id()-1;
  std::erase_if(sk.constraints,[&](const auto& c){return std::any_of(c.refs.begin(),c.refs.end(),[&](int id){return ids.count(id);});});
  std::erase_if(sk.entities,[&](const auto& e){return ids.count(e.id);});
  std::erase_if(sk.points,[&](const auto& p){return ids.count(p.id);});
}
json values(const json& input,const ParamTable& table) {
  auto number=[&](const char* key,double fallback,Dim dim){return eval_input(table,input.value(key,json(fallback)),dim);};
  const double nc=number("count",3,Dim::None),nr=number("rows",1,Dim::None);
  if(nc<1||nr<1||nc!=std::floor(nc)||nr!=std::floor(nr)||nc*nr>1000)throw Error("pattern counts must be whole numbers, with at most 1000 instances");
  return {{"polar",input.value("polar",false)},{"count",int(nc)},{"rows",int(nr)},{"dx",number("dx",10,Dim::Length)},{"dy",number("dy",10,Dim::Length)},
    {"angle",number("angle",360,Dim::Angle)},{"cx",number("cx",0,Dim::Length)},{"cy",number("cy",0,Dim::Length)}};
}
SketchTransform transform(const json& v,int index) {
  SketchTransform t;const int count=v.at("count").get<int>();
  if(v.at("polar").get<bool>()) {
    const double angle=v.at("angle").get<double>();
    const bool full=std::fabs(std::fabs(angle)-2*M_PI)<1e-9;
    t.angle=angle*index/std::max(1,full?count:count-1);t.cx=v.at("cx").get<double>();t.cy=v.at("cy").get<double>();
  } else {t.x=v.at("dx").get<double>()*(index%count);t.y=v.at("dy").get<double>()*(index/count);}
  return t;
}
void update(Sketch& sk,json& pattern) {
  const auto seeds=pattern.at("seeds").get<std::vector<int>>();const auto& v=pattern.at("values");
  const int copies=v.at("count").get<int>()*(v.at("polar").get<bool>()?1:v.at("rows").get<int>())-1;
  auto& instances=pattern["instances"];
  while(int(instances.size())>copies){std::set<int> ids;for(const auto& pair:instances.back().at("map"))ids.insert(pair[1].get<int>());erase_ids(sk,ids);instances.erase(instances.size()-1);}
  for(int i=0;i<copies;++i) {
    const auto t=transform(v,i+1);std::map<int,int> map;
    if(i>=int(instances.size())) {
      const size_t constraints=sk.constraints.size();const auto made=transform_entities(sk,seeds,t,true);sk.constraints.resize(constraints);
      for(size_t k=0;k<seeds.size();++k){const auto source=*sk.entity(seeds[k]),copy=*sk.entity(made[k]);map[source.id]=copy.id;for(size_t p=0;p<source.p.size();++p)map[source.p[p]]=copy.p[p];}
      json pairs=json::array();for(auto [a,b]:map)pairs.push_back({a,b});instances.push_back({{"map",pairs}});
    } else for(const auto& pair:instances[i].at("map"))map[pair[0].get<int>()]=pair[1].get<int>();
    for(const auto& [source,target]:map)if(const auto* p=sk.point(source)) {
      const double x=p->x-t.cx,y=p->y-t.cy;auto* q=sk.point(target);
      q->x=t.cx+t.x+x*std::cos(t.angle)-y*std::sin(t.angle);q->y=t.cy+t.y+x*std::sin(t.angle)+y*std::cos(t.angle);q->fixed=true;
    }
    for(int seed:seeds) {auto e=*sk.entity(seed);e.id=map.at(seed);for(int& p:e.p)p=map.at(p);e.fixed=true;e.source=nullptr;*sk.entity(e.id)=e;}
  }
}
}

// A pattern given only its inputs gets its values (and copies) when the sketch is computed (evaluate_patterns).
void refresh_patterns(Sketch& sk){for(auto& p:sk.patterns)if(p.contains("values") && p["values"].is_object())update(sk,p);}
void evaluate_patterns(Sketch& sk,const ParamTable& parameters){for(auto& p:sk.patterns)p["values"]=values(p.at("inputs"),parameters);refresh_patterns(sk);}
int create_pattern(Sketch& sk,const std::vector<int>& seeds,const json& inputs,const ParamTable& parameters) {
  if(seeds.empty())throw Error("select seed curves for the pattern");
  for(int id:seeds)if(!sk.entity(id)||pattern_of(sk,id))throw Error("pattern seeds must be editable curves");
  const int id=sk.next_id();sk.patterns.push_back({{"id",id},{"seeds",seeds},{"inputs",inputs},{"values",values(inputs,parameters)},{"instances",json::array()}});
  update(sk,sk.patterns.back());sk.validate();return id;
}
void edit_pattern(Sketch& sk,int id,const json& inputs,const ParamTable& parameters) {
  for(auto& p:sk.patterns)if(p.at("id").get<int>()==id){p["values"]=values(inputs,parameters);p["inputs"]=inputs;update(sk,p);sk.validate();return;}
  throw Error("pattern no longer exists");
}
int pattern_of(const Sketch& sk,int entity,bool include_seed) {
  for(const auto& p:sk.patterns) {
    if(p.at("id").get<int>()==entity)return entity;
    if(include_seed)for(int id:p.at("seeds").get<std::vector<int>>())if(id==entity)return p.at("id").get<int>();
    for(const auto& instance:p.value("instances",json::array()))for(const auto& pair:instance.at("map"))if(pair[1].get<int>()==entity)return p.at("id").get<int>();
  }
  return 0;
}
void remove_pattern(Sketch& sk,int id,bool explode) {
  std::set<int> ids;
  for(const auto& p:sk.patterns)if(p.at("id").get<int>()==id)for(const auto& instance:p.value("instances",json::array()))for(const auto& pair:instance.at("map"))ids.insert(pair[1].get<int>());
  sk.id_watermark=sk.next_id()-1;
  for(auto it=sk.patterns.begin();it!=sk.patterns.end();) {if(it->at("id").get<int>()==id)it=sk.patterns.erase(it);else ++it;}
  if(explode){for(int id:ids){if(auto* p=sk.point(id))p->fixed=false;if(auto* e=sk.entity(id)){e->fixed=false;e->source=nullptr;}}}
  else erase_ids(sk,ids);
}
}
