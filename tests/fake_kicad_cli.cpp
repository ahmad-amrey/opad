// A stand-in kicad-cli for the tests and benches (OPAD_KICAD_CLI). `version` prints OPAD_FAKE_KICAD_VERSION (else 9.0.1).
// `pcb export step ... -o <out> <board>` writes the board as OPAD's own reader builds it at --user-origin, the way KiCad's
// export is laid out: one assembly with the board and a part per footprint named by its reference (OPAD_FAKE_KICAD_UNNAMED=1:
// by its model instead), no 2D layers and nothing for a missing model; --include-tracks adds a "tracks" plate on the board,
// --no-dnp and --no-components are honoured. Every call is a line in OPAD_FAKE_KICAD_LOG; OPAD_FAKE_KICAD_FAIL=1 makes it say
// why and fail.
#include <BRepPrimAPI_MakeBox.hxx>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "opad/geometry.hpp"
#include "opad/kicad_pcb.hpp"
#include "opad/scene.hpp"

using namespace opad;

int main(int argc, char** argv) {
  configure_kernel_logging(false);
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (const char* log = std::getenv("OPAD_FAKE_KICAD_LOG")) {
    std::ofstream out(log, std::ios::app);
    for (const auto& a : args) out << a << ' ';
    out << '\n';
  }
  if (args.size() == 1 && args[0] == "version") {
    const char* v = std::getenv("OPAD_FAKE_KICAD_VERSION");
    std::printf("%s\n", v && *v ? v : "9.0.1");
    return 0;
  }
  if (args.size() < 4 || args[0] != "pcb" || args[1] != "export" || args[2] != "step") {
    std::fprintf(stderr, "Unknown command\n");
    return 1;
  }
  if (const char* fail = std::getenv("OPAD_FAKE_KICAD_FAIL"); fail && *fail) {
    std::printf("Loading board\nFailed to load board: the file is damaged\n");
    return 2;
  }
  ImportOptions o;
  std::filesystem::path out;
  bool tracks = false;
  for (size_t i = 3; i + 1 < args.size(); ++i) {
    if (args[i] == "-o" || args[i] == "--output") out = path_from_utf8(args[++i]);
    else if (args[i] == "--user-origin") {
      double x = 0, y = 0;
      if (std::sscanf(args[++i].c_str(), "%lfx%lfmm", &x, &y) == 2) o.kicad.origin_at = {x, y};
    } else if (args[i] == "--no-dnp") o.kicad.dnp = false;
    else if (args[i] == "--no-components" || args[i] == "--board-only") o.kicad.components = false;
    else if (args[i] == "--include-tracks") tracks = true;
  }
  try {
    const std::filesystem::path board = path_from_utf8(args.back());
    Document doc = Document::create();
    import_kicad_pcb(doc, board, o);
    json& root = doc.ops.back().data["nodes"][0];
    json kept = json::array();
    for (auto n : root["children"]) {
      if (n.value("name", "") == "Layers") continue;
      if (n.contains("kicad")) {
        json children = json::array();
        for (const auto& c : n["children"])
          if (!c.value("placeholder", false)) children.push_back(c);
        if (children.empty()) continue;  // KiCad leaves a footprint whose model is missing out
        const char* unnamed = std::getenv("OPAD_FAKE_KICAD_UNNAMED");
        const json models = n["kicad"].value("models", json::array());
        const auto stem = path_from_utf8(models.empty() ? std::string() : models[0].get<std::string>()).stem().u8string();
        n["name"] = unnamed && *unnamed && !stem.empty() ? std::string(stem.begin(), stem.end()) : n["kicad"].value("ref", "");
        n.erase("kicad");
        n["children"] = children;
      }
      kept.push_back(n);
    }
    if (tracks) {
      const std::string key = doc.add_body(brep_from_shape(BRepPrimAPI_MakeBox(gp_Pnt(-5, -5, 1.6), 10, 10, 0.035).Shape()), {{"name", "tracks"}});
      kept.push_back({{"type", "body"}, {"id", new_uuid()}, {"name", "tracks"}, {"key", key}, {"color", {0.8, 0.5, 0.2}}});
    }
    root["children"] = kept;
    ExportOptions e;
    export_selection(doc, resolve(doc), out, e);
  } catch (const std::exception& e) {
    std::printf("Error: %s\n", e.what());
    return 3;
  }
  std::printf("Done\n");
  return 0;
}
