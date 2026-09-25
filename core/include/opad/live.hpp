#pragma once
#include "agent.hpp"
namespace opad::agent {
// Shared by the desktop bridge and the stdio proxy. Headless tools stay separate.
const json& live_tools();
bool live_mutation(const std::string& name);
json live_schema(const std::string& name);
json live_result(const json& output, bool error = false);
json live_error(const std::string& code,const std::string& message);
}
