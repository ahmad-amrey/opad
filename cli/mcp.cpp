// MCP 2025-11-25, newline-delimited JSON-RPC over stdio. No shell or Python runtime required.
#include "opad/core.hpp"
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
                  {"instructions", "Use feature_kinds to discover CAD inputs. Create a document with new, then sketch/feature/param, inspect and export. Each successful mutation is saved. Paths are local to this server."}};
      } else if (method == "ping") result = json::object();
      else if (!initialized) throw opad::Error("initialize first");
      else if (method == "tools/list") {
        json list = json::array();
        for (const auto& c : opad::commands::list()) {
          json properties = json::object();
          for (const auto& [key, description] : c.args.items()) properties[key] = {{"description", description}};
          list.push_back({{"name", c.name}, {"description", c.description},
                          {"inputSchema", {{"type", "object"}, {"properties", properties}, {"additionalProperties", true}}}});
        }
        result = {{"tools", list}};
      } else if (method == "tools/call") {
        try {
          const json args = params.value("arguments", json::object());
          if (!args.is_object()) throw opad::Error("tool arguments must be an object");
          const json output = opad::commands::run(params.at("name").get<std::string>(), args);
          result = {{"content", json::array({{{"type", "text"}, {"text", output.dump()}}})}, {"isError", false}};
        } catch (const Standard_Failure& e) {
          result = {{"content", json::array({{{"type", "text"}, {"text", e.GetMessageString()}}})}, {"isError", true}};
        } catch (const std::exception& e) {
          result = {{"content", json::array({{{"type", "text"}, {"text", e.what()}}})}, {"isError", true}};
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
