// Image canvases (UI-70, opad/canvas.hpp): a picture imported on a plane at a width, its place read back; moving, sizing,
// calibrating and aligning it append a transform op and never the picture; its flags are an edit of the import; Replace keeps
// the node and its place; a sketch's backdrop becomes a canvas where it lay; the canvas command does all of it.
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

#include <filesystem>
#include <fstream>

#include "check.hpp"
#include "opad/canvas.hpp"
#include "opad/commands.hpp"
#include "opad/design/sketch.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/render.hpp"

using namespace opad;
namespace fs = std::filesystem;

namespace {
struct Files {
  fs::path dir = fs::temp_directory_path() / ("opad-canvas-" + new_uuid());
  Files() { fs::create_directories(dir); }
  ~Files() {
    std::error_code e;
    fs::remove_all(dir, e);
  }
};

std::string be32(uint32_t v) { return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
std::string chunk(const std::string& type, const std::string& data) { return be32(uint32_t(data.size())) + type + data + be32(0); }
// A PNG as far as its headers go (w x h pixels at 100 dpi; `tag` stands for the picture data).
std::string png(uint32_t w, uint32_t h, const std::string& tag) {
  return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", be32(w) + be32(h) + std::string("\x08\x06\0\0\0", 5)) +
         chunk("pHYs", be32(3937) + be32(3937) + std::string(1, '\x01')) + chunk("IDAT", tag) + chunk("IEND", "");
}
fs::path write(const fs::path& p, const std::string& bytes) {
  std::ofstream(p, std::ios::binary) << bytes;
  return p;
}
bool about(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool about(const json& a, double b, double tol) { return about(a.get<double>(), b, tol); }
bool near(const Vec3& a, const Vec3& b, double tol = 1e-6) { return std::fabs(a[0] - b[0]) < tol && std::fabs(a[1] - b[1]) < tol && std::fabs(a[2] - b[2]) < tol; }
double dist(const Vec3& a, const Vec3& b) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

std::string only_canvas(const Scene& s) {
  for (const auto& id : s.all_bodies())
    if (is_canvas(*s.node(id))) return id;
  throw check::Failure("no canvas");
}
json run(Document& d, const std::string& command, json args) { return commands::run(command, args, &d); }

// The XZ plane as the sketch base planes have it: x along X, y along Z, its normal -Y.
Frame xz() { return Frame{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}}; }
Mat4 placed_on(const Frame& f, double u, double v) {
  const Vec3 n = f.normal(), o = f.to_world(u, v);
  Mat4 m;
  for (int r = 0; r < 3; ++r) m.at(r, 0) = f.x[r], m.at(r, 1) = f.y[r], m.at(r, 2) = n[r], m.at(r, 3) = o[r];
  return m;
}
}  // namespace

TEST(canvas_inserted_on_a_plane_at_a_width) {
  Files f;
  const std::string bytes = png(400, 200, "front");  // 101.6 x 50.8 mm at 100 dpi
  const fs::path pic = write(f.dir / "front.png", bytes);
  Document d = Document::create();
  ImportOptions o;
  o.placement = placed_on(xz(), 30, 20);
  o.canvas = {{"plane", xz().to_json()}, {"width", 200.0}, {"center", true}};
  import_file(d, pic, o);
  const Scene s = resolve(d);
  const std::string id = only_canvas(s);
  const Node& n = *s.node(id);
  // The bytes as the file has them; the body is the picture's own rectangle, the width a uniform scale.
  CHECK(n.raster["href"].get<std::string>().find("base64,") != std::string::npos);
  CHECK(n.canvas["selectable"] == true && n.canvas["display_through"] == false && n.canvas["plane"]["y"][2] == 1.0);
  const CanvasPlace p = canvas_place(s, id);
  CHECK(p.on_plane && about(p.body_w, 101.6, 1e-3) && about(p.body_h, 50.8, 1e-3));
  CHECK(about(p.width, 200, 1e-9) && about(p.height, 100, 1e-9) && about(p.x, 30, 1e-9) && about(p.y, 20, 1e-9) && about(p.angle, 0, 1e-12));
  CHECK(mat_is_rigid(s.world(id)));  // a uniform scale: shown through a local transformation
  const auto corners = canvas_points(s.world(id), p.body_w, p.body_h);
  CHECK(near(corners[0], {-70, 0, -30}) && near(corners[2], {130, 0, 70}) && near(corners[4], {30, 0, 20}));
  // The place is given back as it was set, and an older reader just sees a body with a transform.
  CHECK(near(canvas_world(p).apply({0, 0, 0}), s.world(id).apply({0, 0, 0})));
  const json op = d.ops.back().data;
  CHECK(op.contains("canvas") && op["nodes"][0].contains("transform") && op["nodes"][0]["raster"]["corners"][1][0].get<double>() > 101);
  // The same insert planned without the document (the app reads the picture on a worker, a big model is never copied):
  // the same body, the same place, one op.
  Document other = Document::create();
  design::Plan plan = plan_canvas_import(pic, o);
  CHECK(plan.ops.size() == 1u && plan.bodies.size() == 1u && plan.report["canvas"].is_string());
  design::commit(other, std::move(plan));
  const Scene t = resolve(other);
  const std::string planned = only_canvas(t);
  CHECK(t.node(planned)->body_key == n.body_key && t.node(planned)->raster["href"] == n.raster["href"] && t.world(planned).m == s.world(id).m);
  CHECK(other.ops.back().data["canvas"] == op["canvas"]);
}

TEST(canvas_moved_sized_turned_calibrated_and_aligned) {
  Files f;
  const fs::path pic = write(f.dir / "plan.png", png(400, 200, std::string(20000, 'x')));
  Document d = Document::create();
  import_file(d, pic);  // on XY, at the file's size, its lower left corner at the origin
  std::string id = only_canvas(resolve(d));
  // Place: its centre, width and turn; each a transform op of well under a kilobyte (the picture is never stored again).
  size_t before = d.serialize().size();
  json r = run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"x", 10.0}, {"y", -5.0}, {"width", 50.0}, {"angle", 90.0}}}});
  CHECK(d.serialize().size() - before < 1024 && d.ops.back().type == "transform");
  CHECK(about(r["x"], 10, 1e-9) && about(r["y"], -5, 1e-9) && about(r["width"], 50, 1e-9) && about(r["height"], 25, 1e-9) && about(r["angle"], 90, 1e-9));
  Scene s = resolve(d);
  CanvasPlace p = canvas_place(s, id);
  auto pts = canvas_points(s.world(id), p.body_w, p.body_h);
  CHECK(near(pts[4], {10, -5, 0}) && near(pts[0], {22.5, -30, 0}));  // turned a quarter: its bottom edge runs up +Y
  // Height instead of width: the aspect stays.
  r = run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"height", 40.0}}}});
  CHECK(about(r["width"], 80, 1e-9));
  // Calibrate: two of its points 30 mm apart as it stands become 120 mm apart; the first stays put.
  s = resolve(d);
  const double along = 30 / (80 / canvas_place(s, id).body_w);  // 30 mm as shown, in the body's own millimetres
  const Vec3 a = s.world(id).apply({10, 10, 0}), b = s.world(id).apply({10 + along, 10, 0});
  CHECK(about(dist(a, b), 30, 1e-9));
  run(d, "canvas", {{"action", "calibrate"}, {"target", id}, {"points", {a, b}}, {"distance", 120.0}});
  s = resolve(d);
  CHECK(near(s.world(id).apply({10, 10, 0}), a) && about(dist(s.world(id).apply({10, 10, 0}), s.world(id).apply({10 + along, 10, 0})), 120, 1e-6));
  CHECK(about(canvas_place(s, id).width, 320, 1e-6));
  // Align: two canvas points onto two model points off the plane (a residual), turned, scaled and moved in the plane.
  const Vec3 c = s.world(id).apply({0, 0, 0}), e = s.world(id).apply({50, 0, 0});
  r = run(d, "canvas", {{"action", "align"}, {"target", id}, {"points", {c, Vec3{100, 100, 3}, e, Vec3{100, 200, 3}}}});
  CHECK(about(r["residual"], 3, 1e-9));
  s = resolve(d);
  CHECK(near(s.world(id).apply({0, 0, 0}), {100, 100, 0}) && near(s.world(id).apply({50, 0, 0}), {100, 200, 0}));
  CHECK(about(canvas_place(s, id).angle, M_PI / 2, 1e-9) && near(s.world(id).apply_dir({0, 0, 1}), {0, 0, 2}, 1e-9));
  // Under a moved component: the place is still world, the op its local matrix.
  const std::string component = run(d, "component", {{"name", "Board"}})["id"];
  run(d, "transform", {{"target", component}, {"matrix", Mat4::translation(0, 0, 50).to_json()}});
  run(d, "reparent", {{"target", id}, {"parent", component}});
  s = resolve(d);
  p = canvas_place(s, id);
  CHECK(!p.on_plane);  // lifted 50 mm off XY with its component: the canvas's own frame from then on
  run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"x", 5.0}, {"angle", 0.0}}}});
  s = resolve(d);
  CHECK(near(canvas_points(s.world(id), p.body_w, p.body_h)[4], p.plane.to_world(5, 0)) && about(p.plane.origin[2], 50, 1e-9));
  // Locked: placing refuses, the flags still change.
  run(d, "appearance", {{"target", id}, {"locked", true}});
  CHECK_THROWS(run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"x", 0.0}}}}));
  CHECK_THROWS(run(d, "canvas", {{"action", "place"}, {"target", component}, {"set", {{"x", 0.0}}}}));  // not a canvas
}

// Free aspect: width and height set together stretch it (x and y scaled apart, the body still the picture's rectangle);
// alone, either keeps the proportions it has; calibrating scales both; the picture's proportions back make it a similarity.
TEST(canvas_stretched_out_of_its_proportions) {
  Files f;
  Document d = Document::create();
  import_file(d, write(f.dir / "wide.png", png(400, 200, "wide")));  // 101.6 x 50.8 mm
  const std::string id = only_canvas(resolve(d));
  const size_t before = d.serialize().size();
  json r = run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"x", 0.0}, {"y", 0.0}, {"width", 100.0}, {"height", 100.0}, {"angle", 30.0}}}});
  CHECK(d.serialize().size() - before < 1024 && d.ops.back().type == "transform" && r["stretched"] == true);
  Scene s = resolve(d);
  CanvasPlace p = canvas_place(s, id);
  CHECK(p.stretched() && about(p.width, 100, 1e-9) && about(p.height, 100, 1e-9) && about(p.angle, M_PI / 6, 1e-9) && p.on_plane);
  CHECK(!mat_is_rigid(s.world(id)));
  auto pts = canvas_points(s.world(id), p.body_w, p.body_h);
  CHECK(about(dist(pts[0], pts[1]), 100, 1e-9) && about(dist(pts[1], pts[2]), 100, 1e-9) && near(pts[4], {0, 0, 0}));
  Bnd_Box box;
  BRepBndLib::AddOptimal(node_world_shape(d, s, id), box, false, false);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  CHECK(about(x1 - x0, 100 * (std::cos(M_PI / 6) + std::sin(M_PI / 6)), 1e-6) && about(y1 - y0, x1 - x0, 1e-6));  // a square, turned
  // Width alone: the proportions it has (square) stay.
  r = run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"width", 50.0}}}});
  CHECK(about(r["width"], 50, 1e-9) && about(r["height"], 50, 1e-9));
  r = run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"height", 80.0}}}});
  CHECK(about(r["width"], 80, 1e-9) && about(r["height"], 80, 1e-9));
  // Calibrated: both ways alike, still square.
  s = resolve(d);
  const Vec3 a = s.world(id).apply({0, 0, 0}), b = s.world(id).apply({p.body_w, 0, 0});
  r = run(d, "canvas", {{"action", "calibrate"}, {"target", id}, {"points", {a, b}}, {"distance", 40.0}});
  CHECK(about(r["width"], 40, 1e-9) && about(r["height"], 40, 1e-9));
  // The picture's proportions again: a similarity, as before it was stretched.
  r = run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"width", 40.0}, {"height", 40.0 * p.body_h / p.body_w}}}});
  s = resolve(d);
  CHECK(!r.contains("stretched") && !canvas_place(s, id).stretched() && mat_is_rigid(s.world(id)));
}

TEST(canvas_flags_are_an_edit_of_its_import) {
  Files f;
  Document d = Document::create();
  import_file(d, write(f.dir / "a.png", png(100, 100, "a")));
  const std::string id = only_canvas(resolve(d)), import_id = d.ops.back().id;
  const size_t before = d.serialize().size();
  json r = run(d, "canvas", {{"action", "flags"}, {"target", id}, {"set", {{"flip", {true, false}}, {"display_through", true}, {"selectable", false}}}});
  CHECK(d.serialize().size() - before < 1024);
  const Op& edit = d.ops.back();
  CHECK(edit.type == "edit" && edit.data["target"] == import_id && edit.data["set"].size() == 1u);
  CHECK(edit.data["set"]["canvas"]["flip"][0] == true && edit.data["set"]["canvas"].contains("plane"));  // always the whole object
  const CanvasFlags flags = CanvasFlags::of(resolve(d).node(id)->canvas);
  CHECK(flags.flip[0] && !flags.flip[1] && flags.through && !flags.selectable);
  CHECK(r["display_through"] == true && r["selectable"] == false);
  // A body that is no picture is no canvas; an SVG raster neither has the canvas object.
  CHECK(CanvasFlags::of(json()).selectable && !CanvasFlags::of(json()).through);
}

TEST(canvas_replaced_in_its_place) {
  Files f;
  Document d = Document::create();
  import_file(d, write(f.dir / "old.png", png(400, 200, "old")));
  const std::string id = only_canvas(resolve(d));
  run(d, "canvas", {{"action", "place"}, {"target", id}, {"set", {{"x", 40.0}, {"y", 10.0}, {"width", 120.0}, {"angle", 30.0}}}});
  run(d, "appearance", {{"target", id}, {"opacity", 0.4}});
  run(d, "rename", {{"target", id}, {"name", "Front view"}});
  const Scene was = resolve(d);
  const std::string key = was.node(id)->body_key;
  // A square picture: the width stays, the height follows; the same node, its name and opacity, the new bytes.
  CHECK_THROWS(run(d, "canvas", {{"action", "replace"}, {"target", id}, {"file", (f.dir / "new.png").string()}}));  // not there
  CHECK_THROWS(run(d, "canvas", {{"action", "replace"}, {"target", id}, {"file", write(f.dir / "bad.png", "no picture").string()}}));
  write(f.dir / "new.png", png(300, 300, "new picture"));
  const json r = run(d, "canvas", {{"action", "replace"}, {"target", id}, {"file", (f.dir / "new.png").string()}});
  const Scene s = resolve(d);
  const Node& n = *s.node(id);
  CHECK(n.body_key != key && n.name == "Front view" && about(n.opacity, 0.4, 1e-12) && s.unresolved.empty());
  const CanvasPlace p = canvas_place(s, id);
  CHECK(about(p.width, 120, 1e-9) && about(p.height, 120, 1e-9) && about(p.x, 40, 1e-9) && about(p.y, 10, 1e-9) && about(p.angle, M_PI / 6, 1e-9));
  CHECK(about(r["width"], 120, 1e-9));
  CHECK(n.raster["px"][0] == 300);
  // Both pictures stay in history: the old one is back once the replace's ops are gone (undo).
  d.truncate_ops(d.ops.size() - 2);
  CHECK(resolve(d).node(id)->body_key == key);
}

TEST(canvas_from_a_sketch_backdrop) {
  Files f;
  Document d = Document::create();
  const std::string bytes = png(200, 100, "backdrop");
  std::string encoded;
  {
    static const char* abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < bytes.size(); i += 3) {
      unsigned v = unsigned((unsigned char)bytes[i]) << 16 | (i + 1 < bytes.size() ? unsigned((unsigned char)bytes[i + 1]) << 8 : 0) | (i + 2 < bytes.size() ? (unsigned char)bytes[i + 2] : 0);
      encoded += abc[v >> 18 & 63];
      encoded += abc[v >> 12 & 63];
      encoded += i + 1 < bytes.size() ? abc[v >> 6 & 63] : '=';
      encoded += i + 2 < bytes.size() ? abc[v & 63] : '=';
    }
  }
  design::Sketch sk;
  sk.add_line(sk.add_point(0, 0), sk.add_point(10, 0));
  sk.images.push_back({{"id", sk.next_id()}, {"name", (f.dir / "photo.png").string()}, {"data", encoded}, {"position", {5, 7}}, {"width", 40.0}, {"height", 20.0},
                       {"angle", M_PI / 2}, {"opacity", 0.5}});
  const json op = design::make_sketch_op("Sketch", {{"base", "xz"}}, sk.to_json());
  design::apply_ops(d, {op});
  const std::string sketch = resolve(d).sketches.back().id;
  const Frame frame = resolve(d).sketch(sketch)->frame;
  const json r = run(d, "canvas", {{"action", "from_backdrop"}, {"sketch", sketch}});
  const Scene s = resolve(d);
  CHECK(r["canvases"].size() == 1u && s.sketch(sketch)->geometry.value("images", json::array()).empty() && s.unresolved.empty());
  const std::string id = only_canvas(s);
  const Node& n = *s.node(id);
  CHECK(n.raster["href"] == "data:image/png;base64," + encoded && about(n.opacity, 0.5, 1e-12) && n.name == "photo");
  // Where the editor drew it: its lower left corner at (5, 7) of the sketch, turned a quarter, 40 x 20 mm.
  const CanvasPlace p = canvas_place(s, id);
  CHECK(p.on_plane && about(p.width, 40, 1e-9) && about(p.height, 20, 1e-9) && about(p.angle, M_PI / 2, 1e-9));
  CHECK(near(canvas_points(s.world(id), p.body_w, p.body_h)[0], frame.to_world(5, 7)) && near(canvas_points(s.world(id), p.body_w, p.body_h)[1], frame.to_world(5, 47)));
  // The sketch's lines stay; a second conversion has nothing to convert.
  CHECK(s.sketch(sketch)->geometry["entities"].size() == 1u);
  CHECK_THROWS(run(d, "canvas", {{"action", "from_backdrop"}, {"sketch", sketch}}));
}

// Headless pictures (render, thumbnails, agents' images): the canvas shows its picture, upright, mirrored as flipped.
TEST(canvas_rendered_with_its_picture) {
  Files f;
  Image picture;
  picture.width = 8, picture.height = 8;
  picture.rgb.resize(8 * 8 * 3);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 8; ++x) {  // top half red, bottom half blue, the left column black
      uint8_t* p = picture.px(x, y);
      p[0] = x == 0 ? 0 : y < 4 ? 220 : 10, p[1] = x == 0 ? 0 : 20, p[2] = x == 0 ? 0 : y < 4 ? 10 : 230;
    }
  const fs::path pic = write(f.dir / "halves.png", encode_png(picture));
  Document d = Document::create();
  import_file(d, pic);
  RenderOptions o;
  o.width = o.height = 64;
  o.camera = Camera::preset("top");
  o.edges = false;
  o.supersample = 1;
  auto at = [](const Image& img, double fx, double fy) {
    const uint8_t* p = img.px(int(fx * (img.width - 1)), int(fy * (img.height - 1)));
    return std::array<int, 3>{p[0], p[1], p[2]};
  };
  Image shot = render_scene(d, resolve(d), o);
  auto top = at(shot, 0.6, 0.3), bottom = at(shot, 0.6, 0.7), left = at(shot, 0.08, 0.5);
  if (top[0] == top[2]) return;  // this build's OCCT reads no pictures: the canvas keeps its colour (nothing to check)
  CHECK(top[0] > 150 && top[2] < 80 && bottom[2] > 150 && bottom[0] < 80 && left[0] < 60 && left[2] < 60);
  const std::string id = only_canvas(resolve(d));
  run(d, "canvas", {{"action", "flags"}, {"target", id}, {"set", {{"flip", {true, true}}}}});
  shot = render_scene(d, resolve(d), o);
  top = at(shot, 0.6, 0.3), bottom = at(shot, 0.6, 0.7);
  const auto right = at(shot, 0.92, 0.5);
  CHECK(top[2] > 150 && bottom[0] > 150 && right[0] < 60 && right[2] < 60);
}

int main(int argc, char** argv) { return check::run_all(argc, argv); }
