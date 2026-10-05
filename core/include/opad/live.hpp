#pragma once
#include "agent.hpp"
namespace opad::agent {
// Shared by the desktop bridge and the stdio proxy. Headless tools stay separate.
const json& live_tools();
bool live_mutation(const std::string& name);
// Whether this call writes: measure only when pinned.
bool live_mutation(const std::string& name, const json& args);
json live_schema(const std::string& name);
json live_output_schema(const std::string& name);
json live_guide();
json transaction_policy();
// The git tools (git_status ... git_push) for both servers: live (the bound document's repository, request_id on the
// writes) or headless (repo names it). Their work is app/GitAgent.cpp; live_tools() lists them. No force or destructive
// operation is among them.
const json& git_tools(bool live);
bool git_tool(const std::string& name);
json live_result(const json& output, bool error = false);
json live_error(const std::string& code,const std::string& message);
}
