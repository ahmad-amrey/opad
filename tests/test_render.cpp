// Headless software renderer (core/src/render.cpp): depth correctness and pinned output, on hand-made meshes so the
// result does not depend on OCCT's tessellation.
#include <cmath>
#include <cstdint>
#include <array>
#include <cstdio>
#include <string>

#include "check.hpp"
#include "opad/render.hpp"

using namespace opad;

namespace {

// Axis-aligned box as 12 triangles; `flip` swaps the diagonal of every quad (different triangulations of one shape).
Mesh box(double x0, double y0, double z0, double x1, double y1, double z1, bool flip = false) {
  Mesh m;
  const double c[8][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
                          {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
  for (const auto& p : c)
    for (double v : p) m.positions.push_back(static_cast<float>(v));
  const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
  for (const auto& q : quads) {
    const int a = q[flip ? 1 : 0], b = q[flip ? 2 : 1], d = q[flip ? 0 : 3], e = q[flip ? 3 : 2];
    for (int i : {a, b, e, a, e, d}) m.indices.push_back(static_cast<uint32_t>(i));
  }
  return m;
}

// FNV-1a over the pixels: a pinned image that must only change on purpose.
uint64_t digest(const Image& img) {
  uint64_t h = 1469598103934665603ull;
  for (uint8_t v : img.rgb) h = (h ^ v) * 1099511628211ull;
  return h;
}

RenderItem item(const Mesh& m, float r, float g, float b, int id, float opacity = 1.0f) {
  RenderItem it;
  it.mesh = &m;
  it.color = {r, g, b};
  it.id = id;
  it.opacity = opacity;
  return it;
}

// A fixed little scene: overlapping boxes, one translucent, drawn with edges.
std::vector<Mesh> baseline_meshes() {
  return {box(-30, -20, 0, 30, 20, 8), box(-10, -10, 8, 10, 10, 30), box(15, -25, -5, 40, 5, 12, true)};
}

}  // namespace

// B16: two large plates 0.03 mm apart (the phone's label under its flex) seen in perspective from near and far. Depth
// interpolated linearly in screen space instead of as 1/w let the lower plate show through along triangle diagonals.
TEST(render_perspective_depth_is_exact) {
  const Mesh lower = box(-45, -45, -0.2, 45, 45, 0);         // red, inset so the upper plate hides all of it
  const Mesh upper = box(-50, -50, 0.03, 50, 50, 1.03, true);  // green, the other diagonal
  const std::vector<RenderItem> items = {item(lower, 1, 0, 0, 1), item(upper, 0, 1, 0, 2)};
  for (double distance : {120.0, 250.0, 600.0, 2000.0}) {
    for (const auto& dir : {Vec3{-140, -60, 190}, Vec3{0.3, -0.2, 1}, Vec3{-1, 0.1, 0.4}}) {
      RenderOptions o;
      o.width = 320;
      o.height = 200;
      o.supersample = 2;
      o.edges = false;
      o.fit = false;
      o.background = {0, 0, 0};
      o.camera.perspective = true;
      o.camera.absolute = true;
      o.camera.fov_deg = 38;
      o.camera.up = {1, 0, 0};
      const double n = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
      o.camera.target = {0, 5, 3};
      o.camera.eye = {dir[0] / n * distance, 5 + dir[1] / n * distance, 3 + dir[2] / n * distance};
      const Image img = render_items(items, o);
      int red = 0, green = 0;
      for (size_t i = 0; i < img.rgb.size(); i += 3) {
        red += img.rgb[i] > 0;
        green += img.rgb[i + 1] > 0;
      }
      if (red || green <= 20)
        std::printf("distance %g, direction (%g,%g,%g): %d red, %d green pixels\n", distance, dir[0], dir[1], dir[2], red, green);
      CHECK(green > 20);
      CHECK_EQ(red, 0);
    }
  }
}

// Orthographic output is pinned: the B16 fix changed perspective only. Update a value only for an intended change.
TEST(render_orthographic_is_pinned) {
  const auto meshes = baseline_meshes();
  const std::vector<RenderItem> items = {item(meshes[0], 0.8f, 0.5f, 0.2f, 1), item(meshes[1], 0.2f, 0.5f, 0.8f, 2),
                                         item(meshes[2], 0.3f, 0.8f, 0.3f, 3, 0.5f)};
  RenderOptions o;
  o.width = 160;
  o.height = 120;
  const uint64_t iso = digest(render_items(items, o));
  o.camera = Camera::preset("top");
  const uint64_t top = digest(render_items(items, o));
  std::printf("orthographic digests: iso %016llx top %016llx\n", static_cast<unsigned long long>(iso), static_cast<unsigned long long>(top));
  CHECK_EQ(iso, 0x7ad84adc89e0a70dull);
  CHECK_EQ(top, 0x3f016cacf9e85187ull);
}

TEST(render_perspective_is_pinned) {
  const auto meshes = baseline_meshes();
  const std::vector<RenderItem> items = {item(meshes[0], 0.8f, 0.5f, 0.2f, 1), item(meshes[1], 0.2f, 0.5f, 0.8f, 2),
                                         item(meshes[2], 0.3f, 0.8f, 0.3f, 3, 0.5f)};
  RenderOptions o;
  o.width = 160;
  o.height = 120;
  o.camera.perspective = true;
  const uint64_t fitted = digest(render_items(items, o));
  o.fit = false;
  o.camera.absolute = true;
  o.camera.eye = {-90, -70, 80};
  o.camera.target = {0, 0, 5};
  const uint64_t close = digest(render_items(items, o));
  std::printf("perspective digests: fitted %016llx close %016llx\n", static_cast<unsigned long long>(fitted), static_cast<unsigned long long>(close));
  // Changed deliberately by the B16 fix (before: 7e61e9e9c6a56e9b, e0f44b633fa6362a).
  CHECK_EQ(fitted, 0xba799c08991e46d3ull);
  CHECK_EQ(close, 0xa313d6b74d723c26ull);
}

// TODO 10 B9: the model's edges as lines, highlighted faces and edges, smooth shading. Off by default (the pinned
// images above do not change); on, each is deterministic and pinned here.
TEST(render_lines_highlights_and_smooth_shading) {
  auto meshes = baseline_meshes();
  for (auto& m : meshes) {
    // Face ranges (6 indices per quad) and a normal per corner pointing away from the box centre.
    for (int f = 0; f < 6; ++f) m.faces.push_back({f, static_cast<uint32_t>(f * 6), 6});
    float c[3] = {0, 0, 0};
    for (size_t i = 0; i < m.positions.size(); ++i) c[i % 3] += m.positions[i] / 8;
    for (size_t i = 0; i < m.positions.size(); i += 3) {
      const float d[3] = {m.positions[i] - c[0], m.positions[i + 1] - c[1], m.positions[i + 2] - c[2]};
      const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      for (int k = 0; k < 3; ++k) m.normals.push_back(d[k] / n);
    }
  }
  std::vector<RenderItem> items = {item(meshes[0], 0.8f, 0.5f, 0.2f, 1), item(meshes[1], 0.2f, 0.5f, 0.8f, 2), item(meshes[2], 0.3f, 0.8f, 0.3f, 3, 0.5f)};
  RenderOptions o;
  o.width = 160;
  o.height = 120;
  const uint64_t plain = digest(render_items(items, o));
  CHECK_EQ(plain, 0x7ad84adc89e0a70dull);  // face ranges and normals alone change nothing
  // The tall box's twelve edges as lines: the ones behind the wide box stay hidden.
  const auto& p = meshes[1].positions;
  auto corner = [&](int i) { return std::array<float, 3>{p[i * 3], p[i * 3 + 1], p[i * 3 + 2]}; };
  for (auto [a, b] : std::vector<std::pair<int, int>>{{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}})
    items[1].lines.push_back({corner(a), corner(b)});
  o.edge_lines = true;
  const Image lined = render_items(items, o);
  CHECK(digest(lined) != plain);
  CHECK(digest(render_items(items, o)) == digest(lined));
  // The wide box's top face (face 1) tinted; the tall box's first edge in the highlight colour.
  items[0].highlight_faces = {1};
  items[1].highlight_lines = {items[1].lines[4]};
  o.camera = Camera::preset("top");
  const Image lit = render_items(items, o);
  int orange = 0;
  for (size_t i = 0; i < lit.rgb.size(); i += 3) orange += lit.rgb[i] > 200 && lit.rgb[i + 1] > 90 && lit.rgb[i + 1] < 190 && lit.rgb[i + 2] < 90;
  CHECK(orange > 200);
  o.smooth = true;
  o.camera = Camera::preset("iso");
  const Image smooth = render_items(items, o);
  const uint64_t d_lined = digest(lined), d_lit = digest(lit), d_smooth = digest(smooth);
  std::printf("B9 digests: lines %016llx highlight %016llx smooth %016llx\n", static_cast<unsigned long long>(d_lined),
              static_cast<unsigned long long>(d_lit), static_cast<unsigned long long>(d_smooth));
  CHECK_EQ(d_lined, 0xf3149a9b115237bdull);
  CHECK_EQ(d_lit, 0xd26923a8f18e117bull);
  CHECK_EQ(d_smooth, 0x0810569e26811e97ull);
}

CHECK_MAIN()
