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
// live_diagnostics' guide fields: include_guide -> agent_guide = guide_index(), guide_section -> agent_guide_section (one
// section, "all" the whole guide; an unknown id answers with the ids, it does not fail the call).
void add_guide(json& out, const json& args);
json transaction_policy();
// A live read's defaults that keep the reply small (#25): features and ops are pages of live_page_limit compact items
// unless the call says otherwise (limit up to 500, detail=full). Other tools' arguments come back unchanged.
json live_read_defaults(const std::string& name, json args);
constexpr int live_page_limit = 50;
// The git tools (git_status ... git_push) for both servers: live (the bound document's repository, request_id on the
// writes) or headless (repo names it). Their work is app/GitAgent.cpp; live_tools() lists them. No force or destructive
// operation is among them.
const json& git_tools(bool live);
bool git_tool(const std::string& name);
json live_result(const json& output, bool error = false);
json live_error(const std::string& code,const std::string& message);
}
