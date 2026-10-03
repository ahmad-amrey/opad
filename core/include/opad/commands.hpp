#pragma once
// The command layer (F36): everything the CLI, Python, plugins and the UI do is a named command
// with JSON in and JSON out. Commands taking a `doc` path load it; the GUI passes its live Document instead.
#include <functional>

#include "render.hpp"
#include <string>
#include <vector>

#include "document.hpp"

namespace opad::commands {

struct CommandInfo {
  std::string name;
  std::string description;
  json args;  // {"arg": "type - description" | JSON Schema property, ...}
  bool mutates = false;
};

using Handler = std::function<json(Document* live, const json& args)>;

void register_command(const CommandInfo& info, Handler h);
std::vector<CommandInfo> list();
bool exists(const std::string& name);

// Runs a command. When `live` is null, args["doc"] names the document: an .opad file is loaded (and saved
// afterwards for mutating commands unless args["save"] == false); a .step file is opened in browse mode.
json run(const std::string& name, const json& args, Document* live = nullptr);

// The screenshot options render and the live viewport_image share (TODO 10 B9): views, edge_lines, highlight, shading.
void apply_picture_options(RenderOptions& options, const json& args);

// Exporter registry (built-in formats plus plugins).
using ExportFn = std::function<json(const Document&, const json& args)>;
void register_exporter(const std::string& format, ExportFn fn);
bool has_exporter(const std::string& format);
json run_exporter(const std::string& format, const Document& doc, const json& args);
std::vector<std::string> exporter_formats();
// The export command's work on a document already resolved (the app runs it on a worker with its own scene); `progress`
// (2D views) returns false to cancel.
json export_document(const Document& doc, const Scene& scene, const json& args, const std::function<bool(double, const std::string&)>& progress = {});

}  // namespace opad::commands
