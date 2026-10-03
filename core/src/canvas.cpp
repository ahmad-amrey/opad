// Image canvases (UI-70, opad/canvas.hpp): where a canvas is, the transforms its tools make, and the plans that replace its
// picture or turn a sketch's backdrop into one.
#include "opad/canvas.hpp"

#include <algorithm>
#include <cmath>

#include "opad/design/sketch.hpp"
#include "opad/geometry.hpp"

namespace opad {
namespace {
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul(const Vec3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
Vec3 unit(const Vec3& a) {
  const double l = length(a);
  return l > 1e-15 ? mul(a, 1 / l) : a;
}

// A matrix with these columns (the fourth the translation).
Mat4 columns(const Vec3& x, const Vec3& y, const Vec3& z, const Vec3& t) {
  Mat4 m;
  for (int r = 0; r < 3; ++r) m.at(r, 0) = x[r], m.at(r, 1) = y[r], m.at(r, 2) = z[r], m.at(r, 3) = t[r];
  return m;
}

std::string base64_decode(const std::string& text) {
  std::string out;
  out.reserve(text.size() * 3 / 4);
  unsigned bits = 0;
  int count = 0;
  for (const unsigned char c : text) {
    const int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (v < 0) continue;
    bits = (bits << 6) | static_cast<unsigned>(v);
    if ((count += 6) >= 8) out += static_cast<char>((bits >> (count -= 8)) & 0xFF);
  }
  return out;
}

std::string utf8(const std::u8string& s) { return std::string(s.begin(), s.end()); }

// The import op's data as replay sees it (edits merged).
json import_data(const Document& doc, const std::string& import_id) {
  for (const auto& e : effective_ops(doc))
    if (e.op->id == import_id) return e.data();
  throw Error("the canvas's import is gone");
}
}  // namespace

CanvasFlags CanvasFlags::of(const json& c) {
  CanvasFlags f;
  if (!c.is_object()) return f;
  if (const auto it = c.find("selectable"); it != c.end() && it->is_boolean()) f.selectable = it->get<bool>();
  if (const auto it = c.find("display_through"); it != c.end() && it->is_boolean()) f.through = it->get<bool>();
  if (const auto it = c.find("flip"); it != c.end() && it->is_array() && it->size() == 2 && (*it)[0].is_boolean() && (*it)[1].is_boolean())
    f.flip = {(*it)[0].get<bool>(), (*it)[1].get<bool>()};
  if (const auto it = c.find("plane"); it != c.end() && it->is_object()) f.plane = *it;
  return f;
}

json CanvasFlags::to_json() const {
  json j = {{"selectable", selectable}, {"display_through", through}, {"flip", {flip[0], flip[1]}}};
  if (plane.is_object()) j["plane"] = plane;
  return j;
}

bool is_canvas(const Node& n) { return n.kind == Node::Kind::Body && n.representation == "image" && n.raster.is_object() && n.raster.contains("corners"); }

// The body's rectangle from the raster's corners (top left, top right, bottom left).
void canvas_body_size(const Node& n, double& w, double& h) {
  const json& c = n.raster.at("corners");
  const Vec3 tl = c.at(0).get<Vec3>(), tr = c.at(1).get<Vec3>(), bl = c.at(2).get<Vec3>();
  w = length(sub(tr, tl));
  h = length(sub(tl, bl));
  if (!(w > 0) || !(h > 0)) throw Error("the canvas " + n.name + " has no size");
}

bool CanvasPlace::stretched() const { return width > 0 && body_w > 0 && body_h > 0 && std::fabs(height * body_w / (width * body_h) - 1) > 1e-9; }

const Node& canvas_node(const Scene& scene, const std::string& id) {
  const Node* n = scene.node(id);
  if (!n || !is_canvas(*n)) throw Error("not an image canvas: " + id);
  return *n;
}

CanvasPlace canvas_place(const Scene& scene, const std::string& id) {
  const Node& n = canvas_node(scene, id);
  CanvasPlace p;
  canvas_body_size(n, p.body_w, p.body_h);
  const Mat4 w = scene.world(id);
  const Vec3 xs = w.apply_dir({1, 0, 0}), ys = w.apply_dir({0, 1, 0}), x = unit(xs), y = unit(ys), normal = cross(x, y);
  const Vec3 centre = w.apply({p.body_w / 2, p.body_h / 2, 0});
  p.width = length(xs) * p.body_w;
  p.height = length(ys) * p.body_h;
  const CanvasFlags flags = CanvasFlags::of(n.canvas);
  p.plane = flags.plane.is_object() ? Frame::from_json(flags.plane) : Frame{w.apply({0, 0, 0}), x, y};
  p.on_plane = dot(normal, p.plane.normal()) > 1 - 1e-9 && std::fabs(dot(sub(centre, p.plane.origin), p.plane.normal())) <= 1e-6 * std::max(1.0, p.width);
  if (!p.on_plane) p.plane = Frame{centre, x, y};
  p.plane.to_local(centre, p.x, p.y);
  p.angle = std::atan2(dot(x, p.plane.y), dot(x, p.plane.x));
  return p;
}

Mat4 canvas_world(const CanvasPlace& p) {
  if (!(p.width > 0) || !(p.height > 0) || !(p.body_w > 0) || !(p.body_h > 0) || !std::isfinite(p.width) || !std::isfinite(p.height) || !std::isfinite(p.x) ||
      !std::isfinite(p.y) || !std::isfinite(p.angle))
    throw Error("a canvas needs a positive size and a place");
  const double s = p.width / p.body_w, t = p.height / p.body_h, sy = std::fabs(t - s) <= 1e-12 * s ? s : t, c = std::cos(p.angle), sn = std::sin(p.angle);
  const Vec3 x = add(mul(p.plane.x, c), mul(p.plane.y, sn)), y = add(mul(p.plane.x, -sn), mul(p.plane.y, c)), n = p.plane.normal();
  const Vec3 origin = sub(p.plane.to_world(p.x, p.y), add(mul(x, p.body_w / 2 * s), mul(y, p.body_h / 2 * sy)));
  return columns(mul(x, s), mul(y, sy), mul(n, s), origin);
}

std::array<Vec3, 5> canvas_points(const Mat4& w, double bw, double bh) {
  return {w.apply({0, 0, 0}), w.apply({bw, 0, 0}), w.apply({bw, bh, 0}), w.apply({0, bh, 0}), w.apply({bw / 2, bh / 2, 0})};
}

Mat4 affine_inverse(const Mat4& m) {
  const double a = m.at(0, 0), b = m.at(0, 1), c = m.at(0, 2), d = m.at(1, 0), e = m.at(1, 1), f = m.at(1, 2), g = m.at(2, 0), h = m.at(2, 1), i = m.at(2, 2);
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-300) throw Error("the placement cannot be inverted");
  Mat4 r;
  r.at(0, 0) = (e * i - f * h) / det, r.at(0, 1) = (c * h - b * i) / det, r.at(0, 2) = (b * f - c * e) / det;
  r.at(1, 0) = (f * g - d * i) / det, r.at(1, 1) = (a * i - c * g) / det, r.at(1, 2) = (c * d - a * f) / det;
  r.at(2, 0) = (d * h - e * g) / det, r.at(2, 1) = (b * g - a * h) / det, r.at(2, 2) = (a * e - b * d) / det;
  for (int row = 0; row < 3; ++row) r.at(row, 3) = -(r.at(row, 0) * m.at(0, 3) + r.at(row, 1) * m.at(1, 3) + r.at(row, 2) * m.at(2, 3));
  return r;
}

json canvas_transform_op(const Scene& scene, const std::string& id, const Mat4& world) {
  const Node& n = canvas_node(scene, id);
  const Mat4 local = n.parent.empty() ? world : affine_inverse(scene.world(n.parent)) * world;
  return {{"op", "transform"}, {"target", id}, {"matrix", local.to_json()}};
}

Mat4 canvas_calibrate(const Mat4& world, const Vec3& a, const Vec3& b, double distance) {
  const double ab = length(sub(b, a));
  if (!(ab > 1e-9) || !(distance > 0) || !std::isfinite(distance)) throw Error("calibrating needs two different points and a positive distance");
  const double k = distance / ab;
  return columns({k, 0, 0}, {0, k, 0}, {0, 0, k}, sub(a, mul(a, k))) * world;
}

Mat4 canvas_align(const Mat4& world, const Vec3& a, const Vec3& a_to, const Vec3& b, const Vec3& b_to, double* residual) {
  const Vec3 o = world.apply({0, 0, 0}), n = unit(cross(world.apply_dir({1, 0, 0}), world.apply_dir({0, 1, 0})));
  auto onto = [&](const Vec3& p) { return sub(p, mul(n, dot(sub(p, o), n))); };
  const Vec3 from = onto(a), to = onto(a_to), u = sub(onto(b), from), v = sub(onto(b_to), to);
  if (residual) *residual = std::max(std::fabs(dot(sub(a_to, o), n)), std::fabs(dot(sub(b_to, o), n)));
  if (length(u) < 1e-9 || length(v) < 1e-9) throw Error("aligning needs two different points on the canvas and two different points of the model");
  const double k = length(v) / length(u), t = std::atan2(dot(cross(u, v), n), dot(u, v)), c = std::cos(t), s = std::sin(t);
  // k R about the normal (Rodrigues), taking `from` to `to`.
  Mat4 m;
  const double skew[3][3] = {{0, -n[2], n[1]}, {n[2], 0, -n[0]}, {-n[1], n[0], 0}};
  for (int r = 0; r < 3; ++r)
    for (int col = 0; col < 3; ++col) m.at(r, col) = k * ((r == col ? c : 0) + (1 - c) * n[r] * n[col] + s * skew[r][col]);
  const Vec3 moved = m.apply_dir(from);
  for (int r = 0; r < 3; ++r) m.at(r, 3) = to[r] - moved[r];
  return m * world;
}

design::Plan plan_canvas_replace(Document& doc, const std::string& id, const std::filesystem::path& file) {
  const Scene scene = resolve(doc);
  const Node& n = canvas_node(scene, id);
  if (n.linked) throw Error("a linked picture is replaced through its link (Replace linked file)");
  const CanvasPlace was = canvas_place(scene, id);
  const std::string bytes = read_text_file(file), name = utf8(file.filename().u8string());
  long pw = 0, ph = 0;
  double dpi = 0;
  std::string mime;
  if (!detail::picture_size(bytes, pw, ph, dpi, mime)) throw Error("not a picture OPAD reads, or a damaged one: " + name);
  if (dpi < 1) dpi = 96;
  detail::CanvasBody made = detail::canvas_body(bytes, utf8(file.stem().u8string()), double(pw) * 25.4 / dpi, double(ph) * 25.4 / dpi);
  // The node keeps its id (every op on it still applies), its import's other fields and its place: the width stays, the
  // height follows the new picture.
  const json old = import_data(doc, n.source_op);
  json node = made.node;
  if (const json& nodes = old.value("nodes", json::array()); !nodes.empty())
    for (const auto& [k, v] : nodes[0].items())
      if (!node.contains(k) && k != "transform") node[k] = v;
  node["id"] = id;
  CanvasPlace next = was;
  next.body_w = made.node["raster"]["corners"][1][0].get<double>();
  next.body_h = made.node["raster"]["corners"][0][1].get<double>();
  next.height = was.width * next.body_h / next.body_w;
  const Mat4 world = canvas_world(next);
  node["transform"] = (n.parent.empty() ? world : affine_inverse(scene.world(n.parent)) * world).to_json();
  doc.add_body(made.body.key, std::string(made.body.brep), made.body.meta);
  cache_shape(doc, made.body.key, *made.body.shape);
  design::Plan plan = design::plan_ops(doc, {design::make_edit_op(n.source_op, {{"nodes", json::array({node})}, {"source", name}}), canvas_transform_op(scene, id, world)}, false);
  plan.bodies.insert(plan.bodies.begin(), std::move(made.body));
  plan.report["canvas"] = id;
  plan.report["import"] = n.source_op;
  plan.report["size_mm"] = {next.width, next.height};
  return plan;
}

design::Plan plan_canvas_from_backdrop(Document& doc, const std::string& sketch, const std::vector<int>& images) {
  const Scene scene = resolve(doc);
  const SketchItem* item = scene.sketch(sketch);
  if (!item) throw Error("no such sketch: " + sketch);
  const design::Sketch before = design::Sketch::from_json(item->geometry);
  design::Sketch after = before;
  after.id_watermark = after.next_id() - 1;  // the images' ids are never given again
  after.images = json::array();
  std::vector<json> ops;
  std::vector<design::NewBody> bodies;
  json canvases = json::array();
  const Frame& f = item->frame;
  for (const auto& image : before.images) {
    const int image_id = image.at("id").get<int>();
    if (!images.empty() && std::find(images.begin(), images.end(), image_id) == images.end()) {
      after.images.push_back(image);
      continue;
    }
    const std::string encoded = image.at("data").get<std::string>();
    const std::filesystem::path source = path_from_utf8(image.value("name", std::string("backdrop")));
    detail::CanvasBody made = detail::canvas_body(base64_decode(encoded), utf8(source.stem().u8string()), image.at("width").get<double>(),
                                                  image.at("height").get<double>(), encoded);
    // Where the editor draws it: its lower left corner at `position`, turned by `angle` in the sketch's plane.
    const double a = image.value("angle", 0.0), u = image.at("position")[0].get<double>(), v = image.at("position")[1].get<double>();
    const Vec3 x = add(mul(f.x, std::cos(a)), mul(f.y, std::sin(a))), y = add(mul(f.x, -std::sin(a)), mul(f.y, std::cos(a)));
    made.node["transform"] = columns(x, y, f.normal(), f.to_world(u, v)).to_json();
    if (const double opacity = image.value("opacity", 0.5); opacity < 1) made.node["opacity"] = opacity;
    CanvasFlags flags;
    flags.plane = f.to_json();
    canvases.push_back(made.node["id"]);
    ops.push_back({{"op", "import"}, {"id", new_uuid()}, {"source", utf8(source.filename().u8string())}, {"units", "mm"}, {"nodes", json::array({made.node})}, {"canvas", flags.to_json()}});
    doc.add_body(made.body.key, std::string(made.body.brep), made.body.meta);
    cache_shape(doc, made.body.key, *made.body.shape);
    bodies.push_back(std::move(made.body));
  }
  if (ops.empty()) throw Error("the sketch has no backdrop image to turn into a canvas");
  design::Plan plan = design::plan_ops(doc, {design::make_edit_op(sketch, {{"geometry_delta", design::sketch_delta(before.to_json(), after.to_json())}})}, false);
  plan.ops.insert(plan.ops.begin(), ops.begin(), ops.end());
  plan.bodies.insert(plan.bodies.begin(), bodies.begin(), bodies.end());
  plan.report["canvases"] = canvases;
  return plan;
}

}  // namespace opad
