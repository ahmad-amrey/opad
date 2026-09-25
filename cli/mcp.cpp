// MCP 2025-11-25, newline-delimited JSON-RPC over stdio. No shell or Python runtime required.
#include "opad/core.hpp"
#include "opad/agent.hpp"
#include <Standard_Failure.hxx>
#include <iostream>
#include <string>

int opad_mcp() {
  using opad::json;
  bool initialized = false;
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
        result = {{"protocolVersion", "2025-11-25"}, {"capabilities", {{"tools", json::object()}}},
                  {"serverInfo", {{"name", "opad"}, {"version", opad::version_string()}}},
                  {"instructions", "Headless file mode: each successful mutation is saved. Paths are local to this server. Use context for a compact summary, paged entity_details/sketch_details for geometry, and feature_schema for a chosen kind before creating a feature. Use named parameters and expressions, validate exact geometry, and export/render to check the result. Reference tokens detect stale geometry; never reuse an old face index without checking it."}};
      } else if (method == "ping") result = json::object();
      else if (!initialized) throw opad::Error("initialize first");
      else if (method == "tools/list") {
        json list = json::array();
        for (const auto& c : opad::commands::list()) {
          list.push_back({{"name", c.name}, {"description", c.description},
                          {"inputSchema",opad::agent::command_schema(c)},
                          {"annotations",{{"readOnlyHint",!c.mutates && c.name!="export" && c.name!="render" && c.name!="cache"},{"destructiveHint",c.mutates},{"openWorldHint",false}}}});
        }
        result = {{"tools", list}};
      } else if (method == "tools/call") {
        try {
          const json args = params.value("arguments", json::object());
          if (!args.is_object()) throw opad::Error("tool arguments must be an object");
          const auto name=params.at("name").get<std::string>();bool found=false;
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
