#pragma once
// Deterministic software rasteriser used for headless screenshots (render command, N4). The GUI uses OCCT AIS.
#include <array>
#include <map>
#include <string>
#include <vector>

#include "document.hpp"
#include "mesh.hpp"
#include "scene.hpp"

namespace opad {

struct Image {
  int width = 0, height = 0;
  std::vector<uint8_t> rgb;  // width*height*3
  uint8_t* px(int x, int y) { return &rgb[static_cast<size_t>((y * width + x) * 3)]; }
  const uint8_t* px(int x, int y) const { return &rgb[static_cast<size_t>((y * width + x) * 3)]; }
};

struct RenderItem {
  const Mesh* mesh = nullptr;
  Mat4 world;
  std::array<float, 3> color{0.75f, 0.75f, 0.78f};
  float opacity = 1.0f;
  int id = 0;
  // TODO 10 B9: polylines (local coordinates) drawn over the shading: the model's edges, and highlighted edges in the
  // highlight colour; face ordinals tinted with it.
  std::vector<std::vector<std::array<float, 3>>> lines, highlight_lines;
  std::vector<int> highlight_faces;
};

struct Camera {
  Vec3 eye{1, -1, 1};   // direction from target towards the eye (or an absolute eye when `absolute`)
  Vec3 target{0, 0, 0};
  Vec3 up{0, 0, 1};
  bool absolute = false;
  bool perspective = false;
  double scale = 0;     // ortho: world height (mm) covered by the image; 0 = fit
  double fov_deg = 40;
  json to_json() const;
  static Camera from_json(const json& j);
  static Camera preset(const std::string& name);  // iso, top, bottom, front, back, left, right
};

struct RenderOptions {
  int width = 1280, height = 720;
  Camera camera = Camera::preset("iso");
  std::array<float, 3> background{1.0f, 1.0f, 1.0f};
  bool edges = true;
  bool fit = true;
  double tolerance = 0.2;
  int supersample = 2;
  std::vector<std::string> select;  // node ids to draw; empty = all visible
  bool ignore_visibility = false;
  // TODO 10 B9, all off by default so every image made before stays byte-identical:
  bool edge_lines = false;          // the model's edges as dark lines, hidden where a surface is in front
  bool smooth = false;              // shading from per-vertex normals instead of flat facets
  std::vector<Ref> highlight;       // faces and edges tinted in orange
  std::vector<std::string> views;   // several fitted preset views ("iso", "front", ...) in one labelled grid
};

Image render_items(const std::vector<RenderItem>& items, const RenderOptions& opt, json* receipt = nullptr);
Image render_scene(const Document& doc, const Scene& scene, const RenderOptions& opt, json* receipt = nullptr);
void write_png(const std::filesystem::path& path, const Image& img);
std::string encode_png(const Image& img);

}  // namespace opad
