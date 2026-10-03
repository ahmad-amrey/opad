#include "opad/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <cctype>

#include "opad/design/sketch_text.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

namespace opad {

// ---------------------------------------------------------------- camera
json Camera::to_json() const {
  json j;
  j["eye"] = {eye[0], eye[1], eye[2]};
  j["target"] = {target[0], target[1], target[2]};
  j["up"] = {up[0], up[1], up[2]};
  j["absolute"] = absolute;
  j["projection"] = perspective ? "perspective" : "orthographic";
  j["scale"] = scale;
  j["fov_deg"] = fov_deg;
  return j;
}

static Vec3 vec3_of(const json& j, Vec3 def) {
  if (!j.is_array() || j.size() != 3) return def;
  return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

Camera Camera::from_json(const json& j) {
  Camera c;
  if (j.contains("view") && j["view"].is_string()) c = preset(j["view"].get<std::string>());
  c.eye = vec3_of(j.value("eye", json()), c.eye);
  c.target = vec3_of(j.value("target", json()), c.target);
  c.up = vec3_of(j.value("up", json()), c.up);
  c.absolute = j.value("absolute", c.absolute);
  std::string proj = j.value("projection", c.perspective ? "perspective" : "orthographic");
  c.perspective = proj == "perspective" || proj == "persp";
  c.scale = j.value("scale", c.scale);
  c.fov_deg = j.value("fov_deg", c.fov_deg);
  return c;
}

Camera Camera::preset(const std::string& name) {
  Camera c;
  if (name == "iso" || name == "home") { c.eye = {1, -1, 1}; c.up = {0, 0, 1}; }
  else if (name == "iso-back") { c.eye = {-1, 1, 1}; c.up = {0, 0, 1}; }
  else if (name == "top") { c.eye = {0, 0, 1}; c.up = {0, 1, 0}; }
  else if (name == "bottom") { c.eye = {0, 0, -1}; c.up = {0, 1, 0}; }
  else if (name == "front") { c.eye = {0, -1, 0}; c.up = {0, 0, 1}; }
  else if (name == "back") { c.eye = {0, 1, 0}; c.up = {0, 0, 1}; }
  else if (name == "right") { c.eye = {1, 0, 0}; c.up = {0, 0, 1}; }
  else if (name == "left") { c.eye = {-1, 0, 0}; c.up = {0, 0, 1}; }
  else throw Error("unknown view '" + name + "' (iso, iso-back, top, bottom, front, back, left, right)");
  return c;
}

// ---------------------------------------------------------------- rasteriser
namespace {

struct V3 {
  double x, y, z;
};
inline V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 mul(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 norm(V3 a) {
  double l = std::sqrt(dot(a, a));
  return l > 0 ? mul(a, 1.0 / l) : V3{0, 0, 1};
}

struct Basis {
  V3 right, up, forward, eye;
  bool perspective;
  double half_w, half_h, focal;  // ortho half extents in mm, or perspective focal length in px
};

}  // namespace

Image render_items(const std::vector<RenderItem>& items, const RenderOptions& opt, json* receipt) {
  if(receipt)(*receipt)["camera"]=nullptr;
  const int ss = std::max(1, std::min(opt.supersample, 4));
  const int W = opt.width * ss, H = opt.height * ss;
  Image out;
  out.width = opt.width;
  out.height = opt.height;

  // World bounding box and sphere.
  double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
  bool any = false;
  for (const auto& it : items) {
    if (!it.mesh) continue;
    const auto& p = it.mesh->positions;
    for (size_t i = 0; i + 2 < p.size(); i += 3) {
      Vec3 w = it.world.apply({p[i], p[i + 1], p[i + 2]});
      for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], w[k]); hi[k] = std::max(hi[k], w[k]); }
      any = true;
    }
    // Lines count too: a drawing made only of lines has no triangles (a solid's edges lie on its surface anyway).
    for (const auto& line : it.lines)
      for (const auto& q : line) {
        Vec3 w = it.world.apply({q[0], q[1], q[2]});
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], w[k]); hi[k] = std::max(hi[k], w[k]); }
        any = true;
      }
  }
  std::vector<float> fb(static_cast<size_t>(W) * H * 3);
  for (size_t i = 0; i < fb.size(); i += 3) { fb[i] = opt.background[0]; fb[i + 1] = opt.background[1]; fb[i + 2] = opt.background[2]; }
  std::vector<float> zb(static_cast<size_t>(W) * H, std::numeric_limits<float>::infinity());
  std::vector<int> ib(static_cast<size_t>(W) * H, -1);

  if (any) {
    V3 center{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    double radius = 0.5 * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    if (radius <= 0) radius = 1;
    const Camera& cam = opt.camera;
    V3 target = opt.fit || !cam.absolute ? center : V3{cam.target[0], cam.target[1], cam.target[2]};
    if (!opt.fit && cam.absolute) target = V3{cam.target[0], cam.target[1], cam.target[2]};
    V3 dir_to_eye = cam.absolute ? norm(sub(V3{cam.eye[0], cam.eye[1], cam.eye[2]}, target)) : norm(V3{cam.eye[0], cam.eye[1], cam.eye[2]});
    V3 forward = mul(dir_to_eye, -1);
    V3 up_hint{cam.up[0], cam.up[1], cam.up[2]};
    if (std::fabs(dot(norm(up_hint), forward)) > 0.999) up_hint = std::fabs(forward.z) > 0.9 ? V3{0, 1, 0} : V3{0, 0, 1};
    V3 right = norm(cross(forward, up_hint));
    V3 up = cross(right, forward);

    Basis b;
    b.right = right; b.up = up; b.forward = forward; b.perspective = cam.perspective;
    // Extents of the bbox corners in camera x/y for fitting.
    double ex = 0, ey = 0;
    for (int c = 0; c < 8; ++c) {
      V3 p{c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2]};
      V3 d = sub(p, target);
      ex = std::max(ex, std::fabs(dot(d, right)));
      ey = std::max(ey, std::fabs(dot(d, up)));
    }
    const double aspect = static_cast<double>(W) / H;
    if (cam.perspective) {
      double fov = cam.fov_deg * M_PI / 180.0;
      double dist = cam.absolute && !opt.fit ? std::sqrt(dot(sub(V3{cam.eye[0], cam.eye[1], cam.eye[2]}, target), sub(V3{cam.eye[0], cam.eye[1], cam.eye[2]}, target)))
                                              : (radius * 1.15) / std::sin(fov / 2);
      b.eye = sub(target, mul(forward, dist));
      b.focal = (H / 2.0) / std::tan(fov / 2);
      b.half_w = b.half_h = 0;
    } else {
      double half_h = cam.scale > 0 && !opt.fit ? cam.scale / 2 : std::max(ey, ex / aspect) * 1.08;
      b.half_h = half_h;
      b.half_w = half_h * aspect;
      b.eye = sub(target, mul(forward, radius * 4));
      b.focal = 0;
    }
    if(receipt){
      Camera actual=cam;actual.absolute=true;actual.eye={b.eye.x,b.eye.y,b.eye.z};
      actual.target={target.x,target.y,target.z};actual.up={up.x,up.y,up.z};
      actual.scale=cam.perspective?0:b.half_h*2;(*receipt)["camera"]=actual.to_json();
    }
    V3 light = norm(add(add(mul(forward, -1), mul(right, 0.35)), mul(up, 0.6)));

    auto project = [&](const Vec3& w, double& sx, double& sy, double& depth) -> bool {
      V3 d = sub(V3{w[0], w[1], w[2]}, b.eye);
      double x = dot(d, b.right), y = dot(d, b.up), z = dot(d, b.forward);
      depth = z;
      if (b.perspective) {
        if (z < 1e-6) return false;
        sx = W / 2.0 + x * b.focal / z;
        sy = H / 2.0 - y * b.focal / z;
      } else {
        sx = (x / b.half_w + 1) * 0.5 * W;
        sy = (1 - y / b.half_h) * 0.5 * H;
      }
      return true;
    };

    struct Tri { double x[3], y[3], z[3]; float shade; int id; float r, g, bl, a; bool smooth = false; float s3[3] = {0, 0, 0}; };
    // Perspective divides by depth, so depth is not linear in screen space but its reciprocal is: interpolating depth
    // itself put large triangles millimetres off and let surfaces behind them show through (B16). Orthographic depth is
    // linear and keeps its exact arithmetic, so orthographic images stay byte-identical.
    auto raster = [&](const Tri& t) {
      int minx = std::max(0, static_cast<int>(std::floor(std::min({t.x[0], t.x[1], t.x[2]}))));
      int maxx = std::min(W - 1, static_cast<int>(std::ceil(std::max({t.x[0], t.x[1], t.x[2]}))));
      int miny = std::max(0, static_cast<int>(std::floor(std::min({t.y[0], t.y[1], t.y[2]}))));
      int maxy = std::min(H - 1, static_cast<int>(std::ceil(std::max({t.y[0], t.y[1], t.y[2]}))));
      if (minx > maxx || miny > maxy) return;
      double area = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.x[2] - t.x[0]) * (t.y[1] - t.y[0]);
      if (std::fabs(area) < 1e-12) return;
      double inv = 1.0 / area;
      const double iz[3] = {1.0 / t.z[0], 1.0 / t.z[1], 1.0 / t.z[2]};
      for (int y = miny; y <= maxy; ++y) {
        double py = y + 0.5;
        for (int x = minx; x <= maxx; ++x) {
          double px = x + 0.5;
          double w0 = ((t.x[1] - px) * (t.y[2] - py) - (t.x[2] - px) * (t.y[1] - py)) * inv;
          double w1 = ((t.x[2] - px) * (t.y[0] - py) - (t.x[0] - px) * (t.y[2] - py)) * inv;
          double w2 = 1 - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          double z = b.perspective ? 1.0 / (w0 * iz[0] + w1 * iz[1] + w2 * iz[2]) : w0 * t.z[0] + w1 * t.z[1] + w2 * t.z[2];
          size_t idx = static_cast<size_t>(y) * W + x;
          if (z >= zb[idx]) continue;
          float* px3 = &fb[idx * 3];
          const float shade = t.smooth ? static_cast<float>(w0 * t.s3[0] + w1 * t.s3[1] + w2 * t.s3[2]) : t.shade;
          if (t.a >= 1.0f) {
            zb[idx] = static_cast<float>(z);
            ib[idx] = t.id;
            px3[0] = t.r * shade; px3[1] = t.g * shade; px3[2] = t.bl * shade;
          } else {
            px3[0] = px3[0] * (1 - t.a) + t.r * shade * t.a;
            px3[1] = px3[1] * (1 - t.a) + t.g * shade * t.a;
            px3[2] = px3[2] * (1 - t.a) + t.bl * shade * t.a;
          }
        }
      }
    };

    for (int pass = 0; pass < 2; ++pass) {  // opaque first, then transparent
      for (const auto& it : items) {
        if (!it.mesh) continue;
        bool transparent = it.opacity < 1.0f;
        if ((pass == 0) == transparent) continue;
        const auto& P = it.mesh->positions;
        const auto& I = it.mesh->indices;
        const auto& N = it.mesh->normals;
        // Index ranges of the highlighted faces (B9).
        std::vector<std::pair<uint32_t, uint32_t>> tinted;
        for (const auto& f : it.mesh->faces)
          if (std::find(it.highlight_faces.begin(), it.highlight_faces.end(), f.face) != it.highlight_faces.end()) tinted.push_back({f.first, f.first + f.count});
        const bool smooth = opt.smooth && N.size() == P.size();
        const Mat4& m = it.world;
        for (size_t k = 0; k + 2 < I.size(); k += 3) {
          Tri t;
          Vec3 w[3];
          bool ok = true;
          for (int c = 0; c < 3 && ok; ++c) {
            size_t vi = static_cast<size_t>(I[k + c]) * 3;
            w[c] = it.world.apply({P[vi], P[vi + 1], P[vi + 2]});
            ok = project(w[c], t.x[c], t.y[c], t.z[c]);
          }
          if (!ok) continue;
          V3 n = norm(cross(sub(V3{w[1][0], w[1][1], w[1][2]}, V3{w[0][0], w[0][1], w[0][2]}),
                            sub(V3{w[2][0], w[2][1], w[2][2]}, V3{w[0][0], w[0][1], w[0][2]})));
          double lam = std::fabs(dot(n, light));
          t.shade = static_cast<float>(0.32 + 0.68 * lam);
          if (smooth) {
            t.smooth = true;
            for (int c = 0; c < 3; ++c) {
              const size_t vi = static_cast<size_t>(I[k + c]) * 3;
              const V3 vn = norm(V3{m.m[0] * N[vi] + m.m[1] * N[vi + 1] + m.m[2] * N[vi + 2], m.m[4] * N[vi] + m.m[5] * N[vi + 1] + m.m[6] * N[vi + 2],
                                    m.m[8] * N[vi] + m.m[9] * N[vi + 1] + m.m[10] * N[vi + 2]});
              t.s3[c] = static_cast<float>(0.32 + 0.68 * std::fabs(dot(vn, light)));
            }
          }
          t.id = it.id;
          t.r = it.color[0]; t.g = it.color[1]; t.bl = it.color[2]; t.a = it.opacity;
          for (const auto& [a, e] : tinted)
            if (k >= a && k < e) {
              t.r = it.color[0] * 0.35f + 0.65f * 1.0f; t.g = it.color[1] * 0.35f + 0.65f * 0.55f; t.bl = it.color[2] * 0.35f + 0.65f * 0.1f;
            }
          raster(t);
        }
      }
    }

    if (opt.edges) {
      // Outline where the item id changes or the depth jumps (silhouettes and creases between bodies).
      double zmin = 1e300, zmax = -1e300;
      for (float z : zb) if (std::isfinite(z)) { zmin = std::min<double>(zmin, z); zmax = std::max<double>(zmax, z); }
      double zthresh = std::max(1e-6, (zmax - zmin) * 0.02);
      std::vector<uint8_t> edge(static_cast<size_t>(W) * H, 0);
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          size_t i = static_cast<size_t>(y) * W + x;
          bool e = false;
          if (x + 1 < W) {
            size_t r = i + 1;
            if (ib[i] != ib[r]) e = true;
            else if (ib[i] >= 0 && std::fabs(zb[i] - zb[r]) > zthresh) e = true;
          }
          if (!e && y + 1 < H) {
            size_t d = i + W;
            if (ib[i] != ib[d]) e = true;
            else if (ib[i] >= 0 && std::fabs(zb[i] - zb[d]) > zthresh) e = true;
          }
          if (e) edge[i] = 1;
        }
      for (size_t i = 0; i < edge.size(); ++i)
        if (edge[i]) { fb[i * 3] *= 0.25f; fb[i * 3 + 1] *= 0.25f; fb[i * 3 + 2] *= 0.25f; }
    }

    // Lines over the shading (B9): the model's edges, then highlighted edges. A line pixel shows unless a surface is in
    // front of it by more than the tessellation can put between an edge and its own faces.
    const double bias = 1.5 * opt.tolerance + 1e-6 * radius;
    auto line = [&](const Vec3& a, const Vec3& e, const float colour[3], int thick) {
      double ax, ay, az, ex2, ey2, ez;
      if (!project(a, ax, ay, az) || !project(e, ex2, ey2, ez)) return;
      const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(ex2 - ax), std::fabs(ey2 - ay)))));
      for (int s = 0; s <= steps; ++s) {
        const double f = static_cast<double>(s) / steps;
        const double x = ax + (ex2 - ax) * f, y = ay + (ey2 - ay) * f;
        const double z = b.perspective ? 1.0 / ((1 - f) / az + f / ez) : az + (ez - az) * f;
        for (int dy = 0; dy < thick; ++dy)
          for (int dx = 0; dx < thick; ++dx) {
            const int px = static_cast<int>(std::floor(x)) + dx - thick / 2, py = static_cast<int>(std::floor(y)) + dy - thick / 2;
            if (px < 0 || py < 0 || px >= W || py >= H) continue;
            const size_t idx = static_cast<size_t>(py) * W + px;
            if (z > zb[idx] + bias) continue;
            fb[idx * 3] = colour[0]; fb[idx * 3 + 1] = colour[1]; fb[idx * 3 + 2] = colour[2];
          }
      }
    };
    const float dark[3] = {0.12f, 0.12f, 0.14f}, orange[3] = {1.0f, 0.5f, 0.05f};
    for (int pass = 0; pass < 2; ++pass)
      for (const auto& it : items)
        for (const auto& poly : pass == 0 ? it.lines : it.highlight_lines)
          for (size_t k = 1; k < poly.size(); ++k)
            line(it.world.apply({poly[k - 1][0], poly[k - 1][1], poly[k - 1][2]}), it.world.apply({poly[k][0], poly[k][1], poly[k][2]}),
                 pass == 0 ? dark : orange, pass == 0 ? ss : 2 * ss);
  }

  // Downsample.
  out.rgb.resize(static_cast<size_t>(opt.width) * opt.height * 3);
  const float inv = 1.0f / (ss * ss);
  for (int y = 0; y < opt.height; ++y)
    for (int x = 0; x < opt.width; ++x) {
      float acc[3] = {0, 0, 0};
      for (int dy = 0; dy < ss; ++dy)
        for (int dx = 0; dx < ss; ++dx) {
          size_t i = (static_cast<size_t>(y * ss + dy) * W + (x * ss + dx)) * 3;
          acc[0] += fb[i]; acc[1] += fb[i + 1]; acc[2] += fb[i + 2];
        }
      uint8_t* p = out.px(x, y);
      for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(std::clamp(acc[c] * inv * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  return out;
}

namespace {

// The body's edges as polylines in its own frame, by edge ordinal (degenerate edges stay empty).
std::vector<std::vector<std::array<float, 3>>> edge_polylines(const TopoDS_Shape& shape, double tolerance) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  std::vector<std::vector<std::array<float, 3>>> out(static_cast<size_t>(edges.Extent()));
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge& e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e)) continue;
    BRepAdaptor_Curve c(e);
    GCPnts_TangentialDeflection sample(c, 0.2, std::max(tolerance, 1e-3));
    for (int k = 1; k <= sample.NbPoints(); ++k) {
      const gp_Pnt p = sample.Value(k);
      out[static_cast<size_t>(i - 1)].push_back({static_cast<float>(p.X()), static_cast<float>(p.Y()), static_cast<float>(p.Z())});
    }
  }
  return out;
}

// A label in the built-in stroke font, 1 px lines, cap height `h` px, top-left at (x, y).
void draw_label(Image& img, const std::string& text, int x, int y, int h) {
  std::vector<std::vector<std::pair<double, double>>> lines;
  try {
    lines = design::stroke_text(text);
  } catch (const std::exception&) {
    return;
  }
  auto dot = [&](int px, int py) {
    if (px < 0 || py < 0 || px >= img.width || py >= img.height) return;
    uint8_t* p = img.px(px, py);
    p[0] = p[1] = p[2] = 40;
  };
  for (const auto& l : lines)
    for (size_t k = 1; k < l.size(); ++k) {
      const double ax = x + l[k - 1].first * h, ay = y + (1 - l[k - 1].second) * h, ex = x + l[k].first * h, ey = y + (1 - l[k].second) * h;
      const int steps = std::max(1, static_cast<int>(std::ceil(std::max(std::fabs(ex - ax), std::fabs(ey - ay)))));
      for (int s = 0; s <= steps; ++s) dot(static_cast<int>(std::lround(ax + (ex - ax) * s / steps)), static_cast<int>(std::lround(ay + (ey - ay) * s / steps)));
    }
}

// Several fitted preset views in one image, each labelled with its name (B9).
Image render_grid(const Document& doc, const Scene& scene, const RenderOptions& opt, json* receipt) {
  const int n = static_cast<int>(opt.views.size());
  const int cols = n <= 1 ? 1 : n <= 4 ? 2 : 3, rows = (n + cols - 1) / cols;
  const int cw = opt.width / cols, ch = opt.height / rows;
  if (cw < 16 || ch < 16) throw Error("render: the image is too small for that many views");
  Image out;
  out.width = opt.width;
  out.height = opt.height;
  out.rgb.assign(static_cast<size_t>(opt.width) * opt.height * 3, 0);
  for (size_t i = 0; i < out.rgb.size(); i += 3)
    for (int c = 0; c < 3; ++c) out.rgb[i + c] = static_cast<uint8_t>(std::clamp(opt.background[c] * 255.0f + 0.5f, 0.0f, 255.0f));
  json views = json::array();
  for (int v = 0; v < n; ++v) {
    RenderOptions o = opt;
    o.views.clear();
    o.camera = Camera::preset(opt.views[static_cast<size_t>(v)]);
    o.fit = true;
    o.width = cw;
    o.height = ch;
    json r;
    const Image cell = render_scene(doc, scene, o, &r);
    const int x0 = (v % cols) * cw, y0 = (v / cols) * ch;
    for (int y = 0; y < ch; ++y) std::copy_n(cell.px(0, y), static_cast<size_t>(cw) * 3, out.px(x0, y0 + y));
    for (int y = y0; y < y0 + ch; ++y) if (x0 > 0) { uint8_t* p = out.px(x0, y); p[0] = p[1] = p[2] = 190; }
    for (int x = x0; x < x0 + cw; ++x) if (y0 > 0) { uint8_t* p = out.px(x, y0); p[0] = p[1] = p[2] = 190; }
    std::string label = opt.views[static_cast<size_t>(v)];
    std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    // A plate behind the label so the model cannot hide it.
    const int plate_w = 16 + static_cast<int>(std::ceil(11 * (0.9 * static_cast<double>(label.size()) - 0.25)));
    for (int y = y0 + 3; y < std::min(y0 + 26, y0 + ch); ++y)
      for (int x = x0 + 3; x < std::min(x0 + plate_w, x0 + cw); ++x) {
        uint8_t* p = out.px(x, y);
        for (int c = 0; c < 3; ++c) p[c] = static_cast<uint8_t>(std::clamp(opt.background[c] * 255.0f + 0.5f, 0.0f, 255.0f));
      }
    draw_label(out, label, x0 + 8, y0 + 8, 11);
    views.push_back({{"view", opt.views[static_cast<size_t>(v)]}, {"camera", r.value("camera", json())}, {"cell", {x0, y0, cw, ch}}});
    if (receipt && v == 0) (*receipt)["visible_ids"] = r.value("visible_ids", json::array());
  }
  if (receipt) {
    (*receipt)["views"] = views;
    (*receipt)["camera"] = views[0]["camera"];
  }
  return out;
}

}  // namespace

Image render_scene(const Document& doc, const Scene& scene, const RenderOptions& opt, json* receipt) {
  if (!opt.views.empty()) return render_grid(doc, scene, opt, receipt);
  if(receipt)(*receipt)["visible_ids"]=json::array();
  std::vector<std::string> bodies = opt.select.empty() ? scene.all_bodies() : std::vector<std::string>{};
  if (!opt.select.empty())
    for (const auto& id : opt.select) {
      if (!scene.node(id)) throw Error("render: unknown node " + id);
      for (const auto& b : scene.bodies_under(id)) bodies.push_back(b);
    }
  std::sort(bodies.begin(),bodies.end());bodies.erase(std::unique(bodies.begin(),bodies.end()),bodies.end());
  std::vector<Mesh> meshes;
  meshes.reserve(bodies.size());
  std::vector<RenderItem> items;
  int id = 1;
  for (const auto& bid : bodies) {
    const Node* n = scene.node(bid);
    if (!n || n->body_missing) continue;
    if (!opt.ignore_visibility && !scene.effectively_visible(bid)) continue;
    if(n->opacity<=0)continue;
    if(receipt)(*receipt)["visible_ids"].push_back(bid);
    meshes.push_back(tessellate_body(doc, n->body_key, opt.tolerance));
    RenderItem it;
    it.mesh = &meshes.back();
    it.world = scene.world(bid);
    it.color = {static_cast<float>(n->color[0]), static_cast<float>(n->color[1]), static_cast<float>(n->color[2])};
    it.opacity = static_cast<float>(n->opacity);
    it.id = id++;
    // B9: edges and highlights.
    bool mine = false;
    for (const auto& h : opt.highlight) mine |= h.body == bid;
    if (opt.edge_lines || mine) {
      const auto polylines = edge_polylines(body_shape(doc, n->body_key), opt.tolerance);
      if (opt.edge_lines) it.lines = polylines;
      for (const auto& h : opt.highlight) {
        if (h.body != bid) continue;
        if (h.kind == Ref::Kind::Face) it.highlight_faces.push_back(h.index);
        else if ((h.kind == Ref::Kind::Edge || h.kind == Ref::Kind::Center) && h.index >= 0 && static_cast<size_t>(h.index) < polylines.size())
          it.highlight_lines.push_back(polylines[static_cast<size_t>(h.index)]);
      }
    }
    items.push_back(it);
  }
  return render_items(items, opt, receipt);
}

// ---------------------------------------------------------------- PNG (zlib deflate with fixed Huffman codes)
namespace {

uint32_t crc_table[256];
void init_crc() {
  static bool done = false;
  if (done) return;
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
  done = true;
}
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xffffffffu) {
  for (size_t i = 0; i < n; ++i) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c;
}

struct BitWriter {
  std::string out;
  uint32_t acc = 0;
  int nbits = 0;
  void put(uint32_t v, int n) {  // LSB first
    acc |= v << nbits;
    nbits += n;
    while (nbits >= 8) { out.push_back(static_cast<char>(acc & 0xff)); acc >>= 8; nbits -= 8; }
  }
  void put_rev(uint32_t code, int n) {  // Huffman codes are MSB first
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) r |= ((code >> i) & 1) << (n - 1 - i);
    put(r, n);
  }
  void flush() { if (nbits > 0) { out.push_back(static_cast<char>(acc & 0xff)); acc = 0; nbits = 0; } }
};

const int len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void put_literal(BitWriter& bw, int v) {
  if (v < 144) bw.put_rev(0x30 + v, 8);
  else if (v < 256) bw.put_rev(0x190 + (v - 144), 9);
  else if (v < 280) bw.put_rev(v - 256, 7);
  else bw.put_rev(0xc0 + (v - 280), 8);
}

void put_match(BitWriter& bw, int len, int dist) {
  int li = 28;
  while (li > 0 && len_base[li] > len) --li;
  put_literal(bw, 257 + li);
  if (len_extra[li]) bw.put(static_cast<uint32_t>(len - len_base[li]), len_extra[li]);
  int di = 29;
  while (di > 0 && dist_base[di] > dist) --di;
  bw.put_rev(static_cast<uint32_t>(di), 5);
  if (dist_extra[di]) bw.put(static_cast<uint32_t>(dist - dist_base[di]), dist_extra[di]);
}

std::string zlib_compress(const std::string& in) {
  BitWriter bw;
  bw.out.push_back(static_cast<char>(0x78));
  bw.out.push_back(static_cast<char>(0x01));
  bw.put(1, 1);  // BFINAL
  bw.put(1, 2);  // BTYPE = fixed Huffman
  const size_t n = in.size();
  const uint8_t* d = reinterpret_cast<const uint8_t*>(in.data());
  const size_t HSIZE = 1 << 15;
  std::vector<int> head(HSIZE, -1), prev(n, -1);
  auto hash3 = [&](size_t i) { return ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & (HSIZE - 1); };
  size_t i = 0;
  while (i < n) {
    int best_len = 0, best_dist = 0;
    if (i + 2 < n) {
      int h = hash3(i);
      int cand = head[h];
      int chain = 0;
      while (cand >= 0 && chain++ < 32 && i - static_cast<size_t>(cand) <= 32768) {
        size_t len = 0;
        while (len < 258 && i + len < n && d[cand + len] == d[i + len]) ++len;
        if (static_cast<int>(len) > best_len) { best_len = static_cast<int>(len); best_dist = static_cast<int>(i - cand); if (len == 258) break; }
        cand = prev[cand];
      }
    }
    if (best_len >= 3) {
      put_match(bw, best_len, best_dist);
      for (int k = 0; k < best_len; ++k, ++i)
        if (i + 2 < n) { int h = hash3(i); prev[i] = head[h]; head[h] = static_cast<int>(i); }
    } else {
      put_literal(bw, d[i]);
      if (i + 2 < n) { int h = hash3(i); prev[i] = head[h]; head[h] = static_cast<int>(i); }
      ++i;
    }
  }
  put_literal(bw, 256);
  bw.flush();
  uint32_t a = 1, b = 0;
  for (size_t k = 0; k < n; ++k) { a = (a + d[k]) % 65521; b = (b + a) % 65521; }
  uint32_t adler = (b << 16) | a;
  for (int k = 3; k >= 0; --k) bw.out.push_back(static_cast<char>((adler >> (k * 8)) & 0xff));
  return bw.out;
}

void chunk(std::string& out, const char* type, const std::string& data) {
  init_crc();
  uint32_t len = static_cast<uint32_t>(data.size());
  for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((len >> (k * 8)) & 0xff));
  std::string body = std::string(type) + data;
  out += body;
  uint32_t c = crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size()) ^ 0xffffffffu;
  for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((c >> (k * 8)) & 0xff));
}

}  // namespace

std::string encode_png(const Image& img) {
  std::string out = "\x89PNG\r\n\x1a\n";
  std::string ihdr;
  auto be32 = [&](std::string& s, uint32_t v) { for (int k = 3; k >= 0; --k) s.push_back(static_cast<char>((v >> (k * 8)) & 0xff)); };
  be32(ihdr, static_cast<uint32_t>(img.width));
  be32(ihdr, static_cast<uint32_t>(img.height));
  ihdr += std::string("\x08\x02\x00\x00\x00", 5);
  chunk(out, "IHDR", ihdr);
  std::string raw;
  raw.reserve(static_cast<size_t>(img.height) * (img.width * 3 + 1));
  for (int y = 0; y < img.height; ++y) {
    raw.push_back('\0');
    raw.append(reinterpret_cast<const char*>(img.px(0, y)), static_cast<size_t>(img.width) * 3);
  }
  chunk(out, "IDAT", zlib_compress(raw));
  chunk(out, "IEND", "");
  return out;
}

void write_png(const std::filesystem::path& path, const Image& img) {
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary);
  if (!f) throw Error("cannot write " + path.string());
  std::string png = encode_png(img);
  f.write(png.data(), static_cast<std::streamsize>(png.size()));
}

}  // namespace opad
