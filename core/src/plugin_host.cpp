#include "opad/plugin_host.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "opad/commands.hpp"
#include "opad/plugin.h"
#include "opad/util.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct opad_host {
  int unused = 0;
};

namespace opad {
namespace {

opad_host g_host;
std::vector<LoadedPlugin> g_plugins;
std::mutex g_mu;

char* dup_string(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (p) std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

Document* to_doc(opad_document* d) { return reinterpret_cast<Document*>(d); }
opad_document* from_doc(const Document* d) { return reinterpret_cast<opad_document*>(const_cast<Document*>(d)); }

json call_plugin(int (*fn)(opad_host*, opad_document*, const char*, char**, void*), Document* doc, const json& args, void* user) {
  char* out = nullptr;
  std::string a = args.dump();
  int rc = fn(&g_host, from_doc(doc), a.c_str(), &out, user);
  std::string text = out ? out : "";
  std::free(out);
  json r = text.empty() ? json::object() : json::parse(text, nullptr, false);
  if (r.is_discarded()) r = json{{"raw", text}};
  if (rc != 0) throw Error(r.is_object() && r.contains("error") ? r["error"].get<std::string>() : "plugin call failed (" + std::to_string(rc) + ")");
  return r;
}

extern "C" int host_run_command(opad_host*, opad_document* doc, const char* name, const char* args_json, char** out_json) {
  try {
    json a = (args_json && *args_json) ? json::parse(args_json) : json::object();
    json r = commands::run(name ? name : "", a, to_doc(doc));
    *out_json = dup_string(r.dump());
    return 0;
  } catch (const std::exception& e) {
    *out_json = dup_string(json{{"error", e.what()}}.dump());
    return 1;
  }
}

extern "C" void host_free_string(char* s) { std::free(s); }

extern "C" int host_register_command(opad_host*, const char* name, const char* description, opad_command_fn fn, void* user) {
  if (!name || !fn) return 1;
  commands::CommandInfo info{name, description ? description : "", json::object(), true};
  commands::register_command(info, [fn, user](Document* d, const json& a) { return call_plugin(fn, d, a, user); });
  return 0;
}

extern "C" int host_register_exporter(opad_host*, const char* format, opad_export_fn fn, void* user) {
  if (!format || !fn) return 1;
  commands::register_exporter(format, [fn, user](const Document& d, const json& a) {
    return call_plugin(fn, const_cast<Document*>(&d), a, user);
  });
  return 0;
}

extern "C" void host_log(opad_host*, int level, const char* message) {
  std::fprintf(stderr, "[plugin:%d] %s\n", level, message ? message : "");
}

const opad_host_api g_api = {OPAD_PLUGIN_ABI_VERSION, host_run_command, host_free_string, host_register_command,
                             host_register_exporter, host_log};

}  // namespace

LoadedPlugin load_plugin(const std::filesystem::path& lib) {
  opad_plugin_init_fn init = nullptr;
#if defined(_WIN32)
  HMODULE h = LoadLibraryW(lib.wstring().c_str());
  if (!h) throw Error("cannot load plugin " + lib.string() + " (error " + std::to_string(GetLastError()) + ")");
  init = reinterpret_cast<opad_plugin_init_fn>(reinterpret_cast<void*>(GetProcAddress(h, "opad_plugin_init")));
#else
  void* h = dlopen(lib.string().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) throw Error(std::string("cannot load plugin ") + lib.string() + ": " + (dlerror() ? dlerror() : ""));
  init = reinterpret_cast<opad_plugin_init_fn>(dlsym(h, "opad_plugin_init"));
#endif
  if (!init) throw Error("plugin " + lib.string() + " does not export opad_plugin_init");
  opad_plugin_info info{};
  int rc = init(&g_api, &g_host, &info);
  if (rc != 0) throw Error("plugin " + lib.string() + " failed to initialise (" + std::to_string(rc) + ")");
  if (info.abi_version != OPAD_PLUGIN_ABI_VERSION)
    throw Error("plugin " + lib.string() + " targets ABI " + std::to_string(info.abi_version) + ", host is " +
                std::to_string(OPAD_PLUGIN_ABI_VERSION));
  LoadedPlugin p{info.name ? info.name : lib.stem().string(), info.version ? info.version : "", lib};
  std::lock_guard<std::mutex> lock(g_mu);
  g_plugins.push_back(p);
  return p;
}

std::vector<LoadedPlugin> loaded_plugins() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_plugins;
}

}  // namespace opad
