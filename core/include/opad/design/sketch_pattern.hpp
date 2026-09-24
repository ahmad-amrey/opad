#pragma once
#include "sketch.hpp"
namespace opad::design {
int create_pattern(Sketch& sk,const std::vector<int>& seeds,const json& inputs,const ParamTable& parameters={});
void edit_pattern(Sketch& sk,int id,const json& inputs,const ParamTable& parameters={});
void evaluate_patterns(Sketch& sk,const ParamTable& parameters={});
void refresh_patterns(Sketch& sk);
void remove_pattern(Sketch& sk,int id,bool explode=false);
int pattern_of(const Sketch& sk,int entity,bool include_seed=false);
}
