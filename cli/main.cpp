// opad-cli: the headless command-line surface over the OPAD command layer (PRD section 7.1).
// Every command prints JSON on stdout; errors go to stderr as {"error": "..."} with exit code 1.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "opad/core.hpp"

using opad::json;

namespace {

void print_usage() {
  std::printf("opad-cli %s - git-native STEP viewer, headless interface\n\n", opad::version_string().c_str());
  std::printf("usage: opad-cli [--plugin <lib>]... [--compact] <command> [<doc>] [args...]\n\n");
  std::printf("  <doc> is a .opad document, or a .step file opened in browse mode (read-only, nothing persisted).\n");
  std::printf("  Arguments are --key value pairs (JSON values are parsed: numbers, true/false, [..], {..}).\n\n");
  std::printf("commands:\n");
  for (const auto& c : opad::commands::list()) {
    std::printf("  %-12s %s\n", c.name.c_str(), c.description.c_str());
    for (auto it = c.args.begin(); it != c.args.end(); ++it)
      std::printf("      --%-10s %s\n", it.key().c_str(), it.value().get<std::string>().c_str());
  }
  std::printf("\nshorthands:\n");
  std::printf("  new <doc>                     import <doc> <file.step>        append <doc> <op.json|->\n");
  std::printf("  inspect <doc> <ref>...        diff <a.opad> <b.opad>          export <doc> --format stl --out f.stl\n");
  std::printf("  render <doc> --out shot.png --view iso --size 1280x720\n");
  std::printf("\nreferences: <uuid> | <uuid>/face/N | <uuid>/edge/N | <uuid>/vertex/N | point/x,y,z\n");
  std::printf("environment: OPAD_AUTHOR (default author), OPAD_CACHE_DIR, OPAD_PLUGINS (path list)\n");
}

json parse_value(const std::string& s) {
  if (s == "true") return true;
  if (s == "false") return false;
  if (s == "null") return nullptr;
  if (!s.empty() && (s[0] == '[' || s[0] == '{')) {
    json j = json::parse(s, nullptr, false);
    if (!j.is_discarded()) return j;
  }
  if (!s.empty() && (std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '-' || s[0] == '.')) {
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end && *end == '\0') {
      if (s.find_first_of(".eE") == std::string::npos && v == static_cast<double>(static_cast<long long>(v)))
        return static_cast<long long>(v);
      return v;
    }
  }
  return s;
}

std::string read_all(std::istream& in) {
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  opad::configure_kernel_logging();
  std::vector<std::string> plugins;
  bool compact = false;
  std::string command;
  std::vector<std::string> positional;
  json args = json::object();
  std::vector<std::string> uuids;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h" || a == "help") {
      print_usage();
      return 0;
    }
    if (a == "--compact") { compact = true; continue; }
    if (a == "--plugin" && i + 1 < argc) { plugins.push_back(argv[++i]); continue; }
    if (a.rfind("--", 0) == 0 && a.size() > 2) {
      std::string key = a.substr(2);
      std::string value;
      size_t eq = key.find('=');
      if (eq != std::string::npos) {
        value = key.substr(eq + 1);
        key = key.substr(0, eq);
      } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        value = argv[++i];
      } else {
        args[key] = true;
        continue;
      }
      for (char& c : key) if (c == '-') c = '_';
      if (key == "uuid" || key == "ref") { uuids.push_back(value); continue; }
      args[key] = parse_value(value);
      continue;
    }
    if (command.empty()) command = a;
    else positional.push_back(a);
  }
  if (command.empty()) {
    print_usage();
    return 2;
  }

  try {
    if (const char* env = std::getenv("OPAD_PLUGINS"); env && *env) {
      std::string list = env;
      size_t start = 0;
      while (start < list.size()) {
#ifdef _WIN32
        size_t sep = list.find(';', start);
#else
        size_t sep = list.find(':', start);
#endif
        plugins.push_back(list.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (sep == std::string::npos) break;
        start = sep + 1;
      }
    }
    for (const auto& p : plugins) opad::load_plugin(p);

    // Positional conventions.
    const bool docless = command == "diff" || command == "version" || command == "commands" || command == "cache" ||
                         command == "selection";
    size_t pi = 0;
    if (!docless && pi < positional.size() && !args.contains("doc")) args["doc"] = positional[pi++];
    if (command == "diff") {
      if (pi < positional.size() && !args.contains("a")) args["a"] = positional[pi++];
      if (pi < positional.size() && !args.contains("b")) args["b"] = positional[pi++];
    } else if (command == "import") {
      if (pi < positional.size() && !args.contains("file")) args["file"] = positional[pi++];
    } else if (command == "append") {
      if (pi < positional.size()) {
        std::string src = positional[pi++];
        std::string text = src == "-" ? read_all(std::cin) : opad::read_text_file(src);
        json j = json::parse(text);
        if (j.is_array()) args["ops"] = j;
        else args["op"] = j;
      }
    } else if (command == "inspect") {
      for (; pi < positional.size(); ++pi) uuids.push_back(positional[pi]);
    } else if (command == "measure") {
      for (; pi < positional.size(); ++pi) uuids.push_back(positional[pi]);
      if (!uuids.empty() && !args.contains("refs")) { args["refs"] = uuids; uuids.clear(); }
    } else if (command == "annotate") {
      if (pi < positional.size() && !args.contains("anchor")) args["anchor"] = positional[pi++];
      if (pi < positional.size() && !args.contains("text")) args["text"] = positional[pi++];
    } else if (command == "render") {
      if (pi < positional.size() && !args.contains("out")) args["out"] = positional[pi++];
    }
    if (!uuids.empty()) {
      if (uuids.size() == 1) args["ref"] = uuids[0];
      else args["refs"] = uuids;
    }
    if (args.contains("select") && args["select"].is_string()) {
      // "a,b,c" -> array handled by the command layer; keep as is.
    }

    json out = opad::commands::run(command, args);
    std::string text = compact ? out.dump() : out.dump(2);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
    return 0;
  } catch (const std::exception& e) {
    json err;
    err["error"] = e.what();
    err["command"] = command;
    std::string text = err.dump();
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fputc('\n', stderr);
    return 1;
  }
}
