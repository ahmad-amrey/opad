// opad-cli: the headless command-line surface over the OPAD command layer.
// Every command prints JSON on stdout; errors go to stderr as {"error": "..."} with exit code 1.
#ifdef _WIN32  // first: OCCT's headers leave out parts of it (the code-page API) when they include it themselves
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <Bnd_Box.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <set>
#include <fstream>
#include <thread>

#include "opad/core.hpp"
#include "opad/diff.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/render.hpp"


using opad::json;
int opad_mcp();
int opad_live_mcp(int argc,char** argv);

namespace {

void print_usage() {
  std::printf("opad-cli %s - git-native STEP viewer, headless interface\n\n", opad::version_string().c_str());
  std::printf("usage: opad-cli [--plugin <lib>]... [--compact] <command> [<doc>] [args...]\n\n");
  std::printf("  <doc> is a .opad document, or any file OPAD reads (STEP, IGES, STL, 3MF, OBJ, DXF, SVG, ...) opened read-only.\n");
  std::printf("  Arguments are --key value pairs (JSON values are parsed: numbers, true/false, [..], {..}).\n\n");
  std::printf("commands:\n");
  for (const auto& c : opad::commands::list()) {
    std::printf("  %-12s %s\n", c.name.c_str(), c.description.c_str());
    for (auto it = c.args.begin(); it != c.args.end(); ++it)
      std::printf("      --%-10s %s\n", it.key().c_str(), (it.value().is_string()?it.value().get<std::string>():it.value().dump()).c_str());
  }
  std::printf("\nshorthands:\n");
  std::printf("  new <doc>                     import <doc> <file.step>        append <doc> <op.json|->\n");
  std::printf("  inspect <doc> <ref>...        export <doc> --format stl --out f.stl\n");
  std::printf("  diff <a> <b> [--text] [--metrics]   what changed; a side is a file or git:REV[:path], one file alone = since git:HEAD\n");
  std::printf("  textconv <doc.opad>           the document as readable lines, for git: diff.opad.textconv \"opad-cli textconv\"\n");
  std::printf("  merge-driver %%O %%A %%B [%%P]    git's merge driver: base, ours and theirs merged into ours (exit 0), or ours left\n");
  std::printf("                                as it was (exit 1); merge.opad.driver \"opad-cli merge-driver %%O %%A %%B %%P\"\n");
  std::printf("  render <doc> --out shot.png --view iso --size 1280x720\n");
  std::printf("  probe <file> [--viewer] [--mesh] [--cache]   reads any supported file as OPAD opens it; reports contents and timings\n");
  std::printf("  thumbnail <file> --out <png|bgra> [--size 256]   a picture of the file (Explorer thumbnails)\n");
  std::printf("\nreferences: <uuid> | <uuid>/face/N | <uuid>/edge/N | <uuid>/vertex/N | point/x,y,z\n");
  std::printf("environment: OPAD_AUTHOR (default author), OPAD_CACHE_DIR, OPAD_PLUGINS (path list)\n");
}

// probe: what opening a file costs, phase by phase, without a window (viewer: the desktop's read-only fast path).
json probe(const std::string& file, bool viewer, bool mesh, bool cache) {
  using clock = std::chrono::steady_clock;
  const auto ms = [](clock::time_point a, clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  const auto t0 = clock::now();
  opad::Document doc = opad::Document::create();
  opad::ImportOptions o;
  o.viewer = viewer;
  const bool cached = viewer && cache && opad::viewer_cache_load(doc, opad::path_from_utf8(file), o);
  const opad::ImportResult r = cached ? opad::ImportResult{} : opad::import_file(doc, opad::path_from_utf8(file), o);
  const auto t1 = clock::now();
  opad::warm_shape_cache(doc);
  const auto t2 = clock::now();
  const opad::Scene scene = opad::resolve(doc);
  const auto t3 = clock::now();
  json out = r.to_json();
  out["file"] = file;
  out["viewer"] = viewer;
  out["read_ms"] = ms(t0, t1);
  out["prepare_ms"] = ms(t1, t2);
  out["resolve_ms"] = ms(t2, t3);
  out["body_entries"] = doc.body_count();
  std::set<std::string> representations;
  for (const auto& id : scene.all_bodies()) representations.insert(scene.node(id)->representation);
  out["representations"] = representations;
  if (mesh) {  // the display's tessellation, on every core as the desktop does it
    const auto keys = doc.body_keys();
    std::atomic<size_t> next{0}, triangles{0};
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < std::max(1u, std::thread::hardware_concurrency()); ++i)
      pool.emplace_back([&] {
        for (size_t k; (k = next++) < keys.size();) {
          const double tol = [&] {
            const Bnd_Box box = opad::body_bbox(doc, keys[k]);
            return std::clamp((box.IsVoid() ? 1.0 : std::sqrt(box.SquareExtent())) * 0.001, 0.001, 5.0);
          }();
          triangles += opad::tessellate_body(doc, keys[k], tol).triangle_count();
        }
      });
    for (auto& t : pool) t.join();
    out["mesh_ms"] = ms(t3, clock::now());
    out["triangles"] = triangles.load();
  }
  out["cache"] = cached ? "hit" : "miss";
  if (viewer && cache && !cached) {
    const auto t4 = clock::now();
    opad::viewer_cache_store(doc, opad::path_from_utf8(file), o);
    out["cache_store_ms"] = ms(t4, clock::now());
  }
  return out;
}

// thumbnail: a small picture of a file for Explorer and the Open dialog (shell/thumbnails runs this). Read as the
// viewer reads it, so the viewer cache serves a big STEP opened before; models from the iso corner, drawings from the
// top. `.bgra` output: "OPADTHMB", width and height (uint32), then premultiplied BGRA rows top-down with a transparent
// background, recovered from one render on white and one on black. Any other output: a PNG on white.
json thumbnail(const std::string& file, const std::string& out, int size) {
  const auto path = opad::path_from_utf8(file);
  std::string ext = path.extension().string();
  for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  opad::Document doc = opad::Document::create();
  if (ext == ".opad") {
    doc = opad::Document::load(path);
  } else {
    opad::ImportOptions o;
    o.viewer = true;
    o.center_drawing = ext == ".dxf" || ext == ".dwg" || ext == ".svg";
    if (!opad::viewer_cache_load(doc, path, o)) opad::import_file(doc, path, o);
  }
  const opad::Scene scene = opad::resolve(doc);
  bool drawing = true, any = false;
  Bnd_Box box;
  for (const auto& id : scene.all_bodies()) {
    if (!scene.effectively_visible(id) || scene.node(id)->body_missing) continue;
    any = true;
    drawing = drawing && scene.node(id)->representation == "drawing2d";
    box.Add(opad::node_world_bbox(doc, scene, id));
  }
  if (!any || box.IsVoid()) throw opad::Error("nothing to show");
  opad::RenderOptions opt;
  opt.width = opt.height = std::clamp(size, 16, 1024);
  opt.camera = opad::Camera::preset(drawing ? "top" : "iso");
  opt.edges = !drawing;
  opt.edge_lines = drawing;  // a drawing is its lines
  opt.smooth = true;
  opt.tolerance = std::clamp(std::sqrt(box.SquareExtent()) * 0.002, 0.005, 50.0);  // a few pixels' worth at this size
  const bool raw = out.size() > 5 && out.compare(out.size() - 5, 5, ".bgra") == 0;
  opt.background = {1, 1, 1};
  const opad::Image white = opad::render_scene(doc, scene, opt);
  if (!raw) {
    opad::write_png(opad::path_from_utf8(out), white);
    return {{"out", out}, {"width", white.width}, {"height", white.height}};
  }
  // A drawing stays on its white sheet: dark lines on a transparent background vanish in a dark Explorer.
  opt.background = {0, 0, 0};
  const opad::Image black = drawing ? white : opad::render_scene(doc, scene, opt);
  std::string bytes = "OPADTHMB";
  auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) bytes += char((v >> (8 * k)) & 0xFF); };
  u32(uint32_t(white.width));
  u32(uint32_t(white.height));
  bytes.reserve(bytes.size() + size_t(white.width) * size_t(white.height) * 4);
  for (int y = 0; y < white.height; ++y)
    for (int x = 0; x < white.width; ++x) {
      const uint8_t* w = white.px(x, y);
      const uint8_t* b = black.px(x, y);
      // On white a pixel is c + (1 - a), on black c (premultiplied): a = 1 - (white - black).
      int spread = 0;
      for (int k = 0; k < 3; ++k) spread = std::max(spread, int(w[k]) - int(b[k]));
      const int alpha = std::clamp(255 - spread, 0, 255);
      for (int k = 2; k >= 0; --k) bytes += char(std::min<int>(b[k], alpha));  // BGR
      bytes += char(alpha);
    }
  std::ofstream f(opad::path_from_utf8(out), std::ios::binary);
  f.write(bytes.data(), std::streamsize(bytes.size()));
  if (!f) throw opad::Error("cannot write the thumbnail");
  return {{"out", out}, {"width", white.width}, {"height", white.height}, {"transparent", true}};
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
#ifdef _WIN32
  // Arguments as UTF-8, as the command layer reads them: the narrow argv is in the ANSI code page, and a file named in
  // Arabic or Chinese arrived as question marks.
  std::vector<std::string> utf8;
  std::vector<char*> utf8_argv;
  if (int n = 0; LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n)) {
    for (int i = 0; i < n; ++i) {
      const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
      std::string arg(size > 0 ? static_cast<size_t>(size - 1) : 0, '\0');
      if (size > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, arg.data(), size, nullptr, nullptr);
      utf8.push_back(std::move(arg));
    }
    LocalFree(wide);
    for (auto& arg : utf8) utf8_argv.push_back(arg.data());
    utf8_argv.push_back(nullptr);
    argc = n;
    argv = utf8_argv.data();
  }
#endif
  if (argc >= 2 && std::string(argv[1]) == "merge-driver") {  // git's: merge.opad.driver "opad-cli merge-driver %O %A %B %P"
    std::vector<std::filesystem::path> files;
    for (int i = 2; i < argc; ++i) files.push_back(opad::path_from_utf8(argv[i]));
    return opad::merge_driver(files);
  }
  opad::configure_kernel_logging();
  if (argc >= 2 && std::string(argv[1]) == "mcp") {
    if(argc==2 || (argc==3 && std::string(argv[2])=="--headless"))return opad_mcp();
    if(std::string(argv[2])=="--live")return opad_live_mcp(argc,argv);
    std::cerr<<"usage: opad-cli mcp [--headless | --live --discovery <directory>]\n";return 1;
  }
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

    if (command == "thumbnail") {
      if (positional.empty() || !args.contains("out")) throw opad::Error("usage: opad-cli thumbnail <file> --out <png|bgra> [--size 256]");
      const json out = thumbnail(positional[0], args["out"].get<std::string>(), args.value("size", 256));
      const std::string text = out.dump();
      std::fwrite(text.data(), 1, text.size(), stdout);
      std::fputc('\n', stdout);
      return 0;
    }
    if (command == "textconv") {  // git's diff driver: whatever the file holds, print something readable and succeed
      if (positional.empty()) throw opad::Error("usage: opad-cli textconv <doc.opad>");
      const auto path = opad::path_from_utf8(positional[0]);
      std::string out;
      try {
        out = opad::document_outline(opad::Document::parse_index(opad::read_text_file(path), path));
      } catch (const std::exception& e) {
        out = opad::text_outline(opad::read_text_file(path), e.what());
      }
#ifdef _WIN32
      _setmode(_fileno(stdout), _O_BINARY);  // LF, as git compares it
#endif
      std::fwrite(out.data(), 1, out.size(), stdout);
      return 0;
    }
    if (command == "probe") {
      if (positional.empty()) throw opad::Error("usage: opad-cli probe <file> [--viewer] [--mesh]");
      const json out = probe(positional[0], args.value("viewer", false), args.value("mesh", false), args.value("cache", false));
      const std::string text = compact ? out.dump() : out.dump(2);
      std::fwrite(text.data(), 1, text.size(), stdout);
      std::fputc('\n', stdout);
      return 0;
    }
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
    if (command == "diff" && args.value("text", false)) {
#ifdef _WIN32
      _setmode(_fileno(stdout), _O_BINARY);
#endif
      const std::string text = out.value("text", "");
      std::fwrite(text.data(), 1, text.size(), stdout);
      return 0;
    }
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
