#pragma once
#include "commands.hpp"
#include "scene.hpp"

namespace opad::agent {
// Shared discovery/validation for headless MCP and the live bridge. Geometry and
// context functions are worker-only; callers provide the cached resolved scene.
json command_schema(const commands::CommandInfo& command, bool live = false);
json feature_schema(const std::string& kind);
void validate_input(const json& schema, const json& value, const std::string& path = "arguments");
json context(const Document&, const Scene&, const json& args);
json sketch_details(const Document&, const Scene&, const json& args);
json entity_details(const Document&, const Scene&, const json& args);
json reference_token(const Document&, const Scene&, const Ref&);
json resolve_reference(const Document&, const Scene&, const json& token, bool remap = false);
json query_entities(const Document&, const Scene&, const json& args, const std::function<bool()>& cancelled = {});
json validate_design(const Document&, const Scene&, const json& args, const std::function<bool()>& cancelled = {});
// The agent guide (core/res/agent_guide.md, compiled in, TODO 10 B2): the conventions schemas cannot carry. Both MCP
// servers list it as a resource; live_diagnostics returns it with include_guide.
const std::string& guide();
json resources();                                // resources/list result
json read_resource(const std::string& uri);      // resources/read result; throws for an unknown uri
}
