// A small picture of a file, for Explorer's thumbnails (opad-cli thumbnail, shell/thumbnails) and the desktop's start
// page (opad-cli, or the desktop app itself where no opad-cli is beside it: opad --thumbnail).
#include <Bnd_Box.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/render.hpp"
#include "opad/step_io.hpp"
#include "opad/util.hpp"

namespace opad {

// thumbnail: a small picture of a file for Explorer and the Open dialog (shell/thumbnails runs this). Read as the
// viewer reads it, so the viewer cache serves a big STEP opened before; models from the iso corner, drawings from the
// top. `.bgra` output: "OPADTHMB", width and height (uint32), then premultiplied BGRA rows top-down with a transparent
// background, recovered from one render on white and one on black. Any other output: a PNG on white.
json write_thumbnail(const std::string& file, const std::string& out, int size) {
  const auto path = path_from_utf8(file);
  std::string ext = path.extension().string();
  for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  Document doc = Document::create();
  if (ext == ".opad") {
    doc = Document::load(path);
  } else {
    ImportOptions o;
    o.viewer = true;
    o.center_drawing = ext == ".dxf" || ext == ".dwg" || ext == ".svg";
    if (!viewer_cache_load(doc, path, o)) import_file(doc, path, o);
  }
  const Scene scene = resolve(doc);
  bool drawing = true, any = false;
  Bnd_Box box;
  for (const auto& id : scene.all_bodies()) {
    if (!scene.effectively_visible(id) || scene.node(id)->body_missing) continue;
    any = true;
    drawing = drawing && scene.node(id)->representation == "drawing2d";
    box.Add(node_world_bbox(doc, scene, id));
  }
  if (!any || box.IsVoid()) throw Error("nothing to show");
  RenderOptions opt;
  opt.width = opt.height = std::clamp(size, 16, 1024);
  opt.camera = Camera::preset(drawing ? "top" : "iso");
  opt.edges = !drawing;
  opt.edge_lines = drawing;  // a drawing is its lines
  opt.smooth = true;
  opt.tolerance = std::clamp(std::sqrt(box.SquareExtent()) * 0.002, 0.005, 50.0);  // a few pixels' worth at this size
  const bool raw = out.size() > 5 && out.compare(out.size() - 5, 5, ".bgra") == 0;
  opt.background = {1, 1, 1};
  const Image white = render_scene(doc, scene, opt);
  if (!raw) {
    write_png(path_from_utf8(out), white);
    return {{"out", out}, {"width", white.width}, {"height", white.height}};
  }
  // A drawing stays on its white sheet: dark lines on a transparent background vanish in a dark Explorer.
  opt.background = {0, 0, 0};
  const Image black = drawing ? white : render_scene(doc, scene, opt);
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
  std::ofstream f(path_from_utf8(out), std::ios::binary);
  f.write(bytes.data(), std::streamsize(bytes.size()));
  if (!f) throw Error("cannot write the thumbnail");
  return {{"out", out}, {"width", white.width}, {"height", white.height}, {"transparent", true}};
}

}  // namespace opad
