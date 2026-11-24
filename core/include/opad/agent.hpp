#pragma once
#include "commands.hpp"
#include "scene.hpp"

namespace opad::design {
class Provenance;
}

namespace opad::agent {
// Shared discovery/validation for headless MCP and the live bridge. Geometry and
// context functions are worker-only; callers provide the cached resolved scene.
json command_schema(const commands::CommandInfo& command, bool live = false);
json feature_schema(const std::string& kind);
void validate_input(const json& schema, const json& value, const std::string& path = "arguments");
json context(const Document&, const Scene&, const json& args);
json sketch_details(const Document&, const Scene&, const json& args);
// `provenance` (optional): one for the document, kept across the calls of a whole selection (created_by found once per body).
json entity_details(const Document&, const Scene&, const json& args, design::Provenance* provenance = nullptr);
json reference_token(const Document&, const Scene&, const Ref&);
json resolve_reference(const Document&, const Scene&, const json& token, bool remap = false);
json query_entities(const Document&, const Scene&, const json& args, const std::function<bool()>& cancelled = {});
json validate_design(const Document&, const Scene&, const json& args, const std::function<bool()>& cancelled = {});
// The agent guide (core/res/agent_guide.md, compiled in, TODO 10 B2): the conventions schemas cannot carry. Both MCP
// servers list it as a resource (opad://guide/agent, and each section as opad://guide/agent#<id>); live_diagnostics returns
// its index with include_guide and one section with guide_section. The whole guide (~50 KB) is too long for many agents'
// tool results (live-MCP stress test, 2026-11-24), so it is offered by `##` section.
const std::string& guide();
json guide_index();  // {"sections": [{"id", "title", "bytes"}], "overview": the text before the first section, "read": how}
// {"id", "title", "text"} of one section ("all": the whole guide); an unknown id throws, naming the ids.
json guide_section(const std::string& id);
json resources();                                // resources/list result
json read_resource(const std::string& uri);      // resources/read result; throws for an unknown uri
}
