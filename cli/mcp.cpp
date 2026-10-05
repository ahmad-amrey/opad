// MCP 2025-11-25, newline-delimited JSON-RPC over stdio. No shell or Python runtime required.
#include "opad/core.hpp"
#include "opad/agent.hpp"
#include "opad/live.hpp"
#include <Standard_Failure.hxx>
#include <iostream>
#include <string>
#ifdef OPAD_GIT_TOOLS
#include <QFileInfo>

#include "Git.hpp"
#include "GitAgent.hpp"
#endif

namespace {
// Git for agents (app/GitAgent.cpp): the same tools as the live server's, on the repository `repo` names, under the
// user's branch protection as OPAD's Preferences keep it (the same QSettings: GitAgent::useAppSettings).
const char* const kGitInstructions =
    " Git: git_status, git_log, git_branches, git_diff (OPAD's semantic diff for .opad), git_init, git_fetch, git_branch_create, git_switch, git_commit, "
    "git_merge (OPAD's merge driver for .opad), git_merge_abort, git_resolve, git_pull, git_push and git_tag work on the repository repo names. The user's protected branches (OPAD Preferences > Version control) refuse agents' commits and "
    "merges into them, and when set, pushes: work on a branch (git_branch_create) instead and let the user merge it. Force push, reset, rebase, branch "
    "deletion, clean, stash drop and discarding changes are not offered: if one is truly needed, it can be done with the git CLI, but only after asking the "
    "user and getting explicit confirmation, and only when needed.";
}

int opad_mcp() {
  using opad::json;
  bool initialized = false;
#ifdef OPAD_GIT_TOOLS
#ifdef OPAD_SINGLE_FILE
  gitagent::useAppSettings(QFileInfo(git::selfFile()).absolutePath(), true);
#else
  gitagent::useAppSettings(QFileInfo(git::selfFile()).absolutePath(), false);
#endif
#endif
  std::string line;
  while (std::getline(std::cin, line)) {
    json id = nullptr;
    try {
      if (line.size() > 8 * 1024 * 1024) throw opad::Error("request exceeds 8 MB");
      json request = json::parse(line);
      if (!request.is_object() || request.value("jsonrpc", "") != "2.0" || !request.contains("method") || !request["method"].is_string())
        throw opad::Error("invalid JSON-RPC request");
      if (request.contains("id")) id = request["id"];
      const std::string method = request["method"];
      if (!request.contains("id")) continue;  // notifications never have responses
      json params = request.value("params", json::object()), result;
      if (method == "initialize") {
        initialized = true;
        result = {{"protocolVersion", "2025-11-25"}, {"capabilities", {{"tools", json::object()}, {"resources", json::object()}}},
                  {"serverInfo", {{"name", "opad"}, {"version", opad::version_string()}}},
                  {"instructions", "Headless file mode: each successful mutation is saved. Paths are local to this server. Review context section ai_agent_notes first: AI agent notes are user requests tied to model anchors; fetch annotations by id for full text, comments and drawing strokes, inspect current references, and resolve only after verifying completion. Read the agent guide first (resources/read opad://guide/agent): units, frames, sketch geometry, references and feature conventions. Use context for a compact summary, paged entity_details/sketch_details for geometry, and feature_schema for a chosen kind before creating a feature. Use named parameters and expressions, validate exact geometry, and export/render to check the result. Reference tokens detect stale geometry; never reuse an old face index without checking it."
#ifdef OPAD_GIT_TOOLS
                                        + std::string(kGitInstructions)
#endif
                  }};
      } else if (method == "ping") result = json::object();
      else if (!initialized) throw opad::Error("initialize first");
      else if (method == "resources/list") result = opad::agent::resources();
      else if (method == "resources/templates/list") result = {{"resourceTemplates", json::array()}};
      else if (method == "resources/read") {
        try {
          result = opad::agent::read_resource(params.value("uri", ""));
        } catch (const std::exception& e) {
          std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32002}, {"message", e.what()}}}}.dump() << '\n' << std::flush;
          continue;
        }
      } else if (method == "tools/list") {
        json list = json::array();
        for (const auto& c : opad::commands::list()) {
          json schema = opad::agent::command_schema(c);
          // The full sketch geometry schema is on the sketch tool; sketch_edit names it (the server checks it in full).
          if (c.name == "sketch_edit") schema["properties"]["geometry"] = {{"type", "object"}, {"description", "Sketch geometry as the sketch tool's schema gives it: points, entities, constraints and shapes, all ids in one id space across the sketch; checked in full by the server."}};
          list.push_back({{"name", c.name}, {"description", c.description},
                          {"inputSchema", schema},
                          {"annotations",{{"readOnlyHint",!c.mutates && c.name!="export" && c.name!="render" && c.name!="project" && c.name!="cache"},{"destructiveHint",c.mutates},{"openWorldHint",false}}}});
        }
#ifdef OPAD_GIT_TOOLS
        for (const auto& tool : opad::agent::git_tools(false)) list.push_back(tool);
#endif
        result = {{"tools", list}};
      } else if (method == "tools/call") {
        try {
          const json args = params.value("arguments", json::object());
          if (!args.is_object()) throw opad::Error("tool arguments must be an object");
          const auto name=params.at("name").get<std::string>();bool found=false;
#ifdef OPAD_GIT_TOOLS
          if (opad::agent::git_tool(name)) {
            json schema;
            for (const auto& tool : opad::agent::git_tools(false))
              if (tool["name"] == name) schema = tool["inputSchema"];
            if (schema.is_null()) throw opad::Error("Unsupported tool: " + name + ". Request tools/list.");
            opad::agent::validate_input(schema, args);
            json output;
            try {
              gitagent::Call call;
              call.repo = QString::fromStdString(args.at("repo").get<std::string>());
              call.policy = gitagent::Policy::read();
              json rest = args;
              rest.erase("repo");
              output = gitagent::run(name, rest, call);
            } catch (const std::exception& e) {
              const auto* refused = dynamic_cast<const gitagent::Refused*>(&e);
              json failure = {{"code", refused ? refused->code : std::string("git_failed")}, {"message", e.what()}};
              if (refused && !refused->next.empty()) failure["next"] = refused->next;
              result = {{"content", json::array({{{"type", "text"}, {"text", failure.dump()}}})}, {"isError", true}, {"structuredContent", {{"error", failure}}}};
              std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump() << '\n' << std::flush;
              continue;
            }
            result = {{"content", json::array({{{"type", "text"}, {"text", output.dump()}}})}, {"isError", false}, {"structuredContent", output}};
            std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump() << '\n' << std::flush;
            continue;
          }
#endif
          for(const auto& command:opad::commands::list())if(command.name==name){opad::agent::validate_input(opad::agent::command_schema(command),args);found=true;break;}
          if(!found)throw opad::Error("Unsupported tool: "+name+". Request tools/list.");
          const json output = opad::commands::run(name, args);
          result = {{"content", json::array({{{"type", "text"}, {"text", output.dump()}}})}, {"isError", false},{"structuredContent",output.is_object()?output:json{{"result",output}}}};
        } catch (const Standard_Failure& e) {
          result = {{"content", json::array({{{"type", "text"}, {"text", e.GetMessageString()}}})}, {"isError", true}};
        } catch (const std::exception& e) {
          const json failure={{"code","invalid_operation"},{"message",e.what()},{"next","Inspect the referenced entities and feature_schema, correct the failed inputs, then retry. The connection remains available."}};
          result = {{"content", json::array({{{"type", "text"}, {"text",failure.dump()}}})}, {"isError", true},{"structuredContent",{{"error",failure}}}};
        }
      } else {
        std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32601}, {"message", "method not found"}}}}.dump() << '\n' << std::flush;
        continue;
      }
      std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump() << '\n' << std::flush;
    } catch (const json::parse_error& e) {
      std::cout << json{{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", e.what()}}}}.dump() << '\n' << std::flush;
    } catch (const std::exception& e) {
      std::cout << json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32600}, {"message", e.what()}}}}.dump() << '\n' << std::flush;
    }
  }
  return 0;
}
