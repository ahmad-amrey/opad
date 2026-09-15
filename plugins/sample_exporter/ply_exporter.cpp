// Sample OPAD plugin: exports the selection as an ASCII PLY mesh with per-vertex colours.
// Only the C ABI in <opad/plugin.h> is used; the geometry is obtained from the host's "mesh" command.
#include <opad/plugin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

const opad_host_api* g_host = nullptr;

char* dup(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

int fail(char** out, const std::string& msg) {
  *out = dup(json{{"error", msg}}.dump());
  return 1;
}

int export_ply(opad_host* host, opad_document* doc, const char* args_json, char** out_json, void*) {
  json args = json::parse(args_json ? args_json : "{}", nullptr, false);
  if (args.is_discarded()) return fail(out_json, "bad args");
  std::string out = args.value("out", "");
  if (out.empty()) return fail(out_json, "ply: \"out\" path required");

  json mesh_args = json::object();
  if (args.contains("select")) mesh_args["select"] = args["select"];
  mesh_args["tolerance"] = args.value("tolerance", 0.1);
  char* mesh_text = nullptr;
  if (g_host->run_command(host, doc, "mesh", mesh_args.dump().c_str(), &mesh_text) != 0) {
    std::string err = mesh_text ? mesh_text : "mesh command failed";
    g_host->free_string(mesh_text);
    return fail(out_json, err);
  }
  json bodies = json::parse(mesh_text);
  g_host->free_string(mesh_text);

  size_t nv = 0, nf = 0;
  for (const auto& b : bodies) {
    nv += b["positions"].size() / 3;
    nf += b["indices"].size() / 3;
  }
  std::ofstream f(out, std::ios::binary);
  if (!f) return fail(out_json, "cannot write " + out);
  f << "ply\nformat ascii 1.0\ncomment OPAD sample plugin export, units mm\n";
  f << "element vertex " << nv << "\nproperty float x\nproperty float y\nproperty float z\n";
  f << "property uchar red\nproperty uchar green\nproperty uchar blue\n";
  f << "element face " << nf << "\nproperty list uchar int vertex_indices\nend_header\n";
  for (const auto& b : bodies) {
    const auto& p = b["positions"];
    int r = static_cast<int>(b["color"][0].get<double>() * 255), g = static_cast<int>(b["color"][1].get<double>() * 255),
        bl = static_cast<int>(b["color"][2].get<double>() * 255);
    for (size_t i = 0; i + 2 < p.size(); i += 3)
      f << p[i].get<double>() << " " << p[i + 1].get<double>() << " " << p[i + 2].get<double>() << " " << r << " " << g << " " << bl << "\n";
  }
  size_t offset = 0;
  for (const auto& b : bodies) {
    const auto& idx = b["indices"];
    for (size_t i = 0; i + 2 < idx.size(); i += 3)
      f << "3 " << offset + idx[i].get<size_t>() << " " << offset + idx[i + 1].get<size_t>() << " " << offset + idx[i + 2].get<size_t>() << "\n";
    offset += b["positions"].size() / 3;
  }
  g_host->log(host, 0, ("wrote " + out).c_str());
  *out_json = dup(json{{"files", {out}}, {"bodies", bodies.size()}, {"vertices", nv}, {"faces", nf}}.dump());
  return 0;
}

}  // namespace

extern "C" OPAD_PLUGIN_EXPORT int opad_plugin_init(const opad_host_api* host, opad_host* h, opad_plugin_info* out) {
  if (!host || host->abi_version != OPAD_PLUGIN_ABI_VERSION) return 1;
  g_host = host;
  out->abi_version = OPAD_PLUGIN_ABI_VERSION;
  out->name = "sample-ply-exporter";
  out->version = "1.0";
  return host->register_exporter(h, "ply", export_ply, nullptr);
}
