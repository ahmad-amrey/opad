// A stand-in kicad-cli for the tests and benches (OPAD_KICAD_CLI). `version` prints OPAD_FAKE_KICAD_VERSION (else 9.0.1).
// `pcb export step ... -o <out> <board>` writes the board as OPAD's own reader builds it at --user-origin, laid out and named
// the way KiCad's exporter does it (read in KiCad's sources, 7.0 to 10.0): one assembly with a part per footprint named by its
// reference (OPAD_FAKE_KICAD_UNNAMED=1: by its model instead), the board "<stem> PCB" (7) or "<stem>_PCB" (8 and later), no
// 2D layers and nothing for a missing model. --include-tracks: KiCad 8 adds each track and pad on its own ("<stem>_track_1",
// "<stem>_pad_3", a pad at every footprint), 9 one "<stem>_copper"; --include-pads (9) one "<stem>_pad" of several solids;
// --include-silkscreen (9) "<stem>_silkscreen". --no-dnp and --no-components are honoured. Every call is a line in
// OPAD_FAKE_KICAD_LOG; OPAD_FAKE_KICAD_FAIL=1 makes it say why and fail.
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

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
  const char* version = std::getenv("OPAD_FAKE_KICAD_VERSION");
  if (args.size() == 1 && args[0] == "version") {
    std::printf("%s\n", version && *version ? version : "9.0.1");
    return 0;
  }
  const int major = version && *version ? std::atoi(version) : 9;
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
  bool tracks = false, pads = false, silkscreen = false;
  for (size_t i = 3; i + 1 < args.size(); ++i) {
    if (args[i] == "-o" || args[i] == "--output") out = path_from_utf8(args[++i]);
    else if (args[i] == "--user-origin") {
      double x = 0, y = 0;
      if (std::sscanf(args[++i].c_str(), "%lfx%lfmm", &x, &y) == 2) o.kicad.origin_at = {x, y};
    } else if (args[i] == "--no-dnp") o.kicad.dnp = false;
    else if (args[i] == "--no-components" || args[i] == "--board-only") o.kicad.components = false;
    else if (args[i] == "--include-tracks") tracks = true;
    else if (args[i] == "--include-pads") pads = true;
    else if (args[i] == "--include-silkscreen") silkscreen = true;
  }
  try {
    const std::filesystem::path board = path_from_utf8(args.back());
    const auto u8 = board.stem().u8string();
    const std::string stem(u8.begin(), u8.end());
    Document doc = Document::create();
    import_kicad_pcb(doc, board, o);
    json& root = doc.ops.back().data["nodes"][0];
    json kept = json::array();
    for (auto n : root["children"]) {
      if (n.value("name", "") == "Layers") continue;
      if (n.value("name", "") == "Board") n["name"] = stem + (major < 8 ? " PCB" : "_PCB");
      if (n.contains("kicad")) {
        json children = json::array();
        for (const auto& c : n["children"])
          if (!c.value("placeholder", false)) children.push_back(c);
        if (children.empty()) continue;  // KiCad leaves a footprint whose model is missing out
        const char* unnamed = std::getenv("OPAD_FAKE_KICAD_UNNAMED");
        const json models = n["kicad"].value("models", json::array());
        const auto model = path_from_utf8(models.empty() ? std::string() : models[0].get<std::string>()).stem().u8string();
        n["name"] = unnamed && *unnamed && !model.empty() ? std::string(model.begin(), model.end()) : n["kicad"].value("ref", "");
        n.erase("kicad");
        n["children"] = children;
      }
      kept.push_back(n);
    }
    auto add = [&](const std::string& name, const TopoDS_Shape& shape) {
      const std::string key = doc.add_body(brep_from_shape(shape), {{"name", name}});
      kept.push_back({{"type", "body"}, {"id", new_uuid()}, {"name", name}, {"key", key}, {"color", {0.8, 0.5, 0.2}}});
    };
    // A pad under every footprint with a model, on its side (also where the model is missing: pads are the board's).
    KicadOptions every;
    every.origin_at = o.kicad.origin_at;
    std::vector<TopoDS_Shape> under;
    const json placed = kicad_board(board, every);
    for (const auto& c : placed["components"]) {
      const bool bottom = c.value("side", "") == "bottom";
      under.push_back(BRepPrimAPI_MakeBox(gp_Pnt(c["at"][0].get<double>() - 0.5, c["at"][1].get<double>() - 0.3, bottom ? -0.035 : 1.6), 1, 0.6, 0.035).Shape());
    }
    if (tracks && major == 8) {
      add(stem + "_track_1", BRepPrimAPI_MakeBox(gp_Pnt(-5, -5, 1.6), 10, 0.2, 0.035).Shape());
      add(stem + "_track_2", BRepPrimAPI_MakeBox(gp_Pnt(-5, 5, 1.6), 10, 0.2, 0.035).Shape());
      for (size_t i = 0; i < under.size(); ++i) add(stem + "_pad_" + std::to_string(i + 1), under[i]);
    } else if (tracks) {
      add(stem + "_copper", BRepPrimAPI_MakeBox(gp_Pnt(-5, -5, 1.6), 10, 10, 0.035).Shape());
    }
    if (pads && major >= 9 && !under.empty()) {
      TopoDS_Compound all;
      BRep_Builder b;
      b.MakeCompound(all);
      for (const auto& s : under) b.Add(all, s);
      add(stem + "_pad", all);
    }
    if (silkscreen && major >= 9) add(stem + "_silkscreen", BRepPrimAPI_MakeBox(gp_Pnt(-2, -2, 1.64), 4, 1, 0.01).Shape());
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
