#pragma once
// Image canvases (UI-70): a picture imported as one flat rectangle (image_io.cpp) whose raster holds the file's own bytes,
// stored once and never decoded or encoded again. Everything after the import is an op on it, never the picture again:
//   placement (move, turn, size)   transform ops; the matrix is the canvas's placement times a scale, uniform unless the
//                                  canvas was stretched out of its picture's proportions (free aspect: x and y apart)
//   opacity, visible, locked       appearance ops
//   flags                          an edit of the import op setting its whole "canvas" object:
//     {"selectable": true, "display_through": false, "flip": [false, false], "plane": {"origin", "x", "y"}}
// "plane" is the plane the canvas was placed on: its centre (x, y) and turn are given in that frame. An older build keeps
// the key and shows the picture where the transform ops put it, unmirrored.
#include <array>
#include <filesystem>
#include <vector>

#include "design/feature.hpp"
#include "scene.hpp"
#include "step_io.hpp"

namespace opad {

struct CanvasFlags {
  bool selectable = true, through = false;
  std::array<bool, 2> flip{false, false};  // mirrored left-right, upside down
  json plane;                              // a Frame; null: none was recorded
  static CanvasFlags of(const json& canvas);
  json to_json() const;  // always complete
};

// Where a canvas is: its centre (x, y) and turn (radians, about the plane's normal) in its plane, its shown size, and the
// picture's own rectangle (the body, mm) that the placement scales (by width / body_w along x, height / body_h along y).
struct CanvasPlace {
  Frame plane;
  double x = 0, y = 0, width = 0, height = 0, angle = 0;
  double body_w = 0, body_h = 0;
  bool on_plane = true;  // false: moved off its plane since (a Move): the plane is then the canvas's own frame
  bool stretched() const;  // shown out of its picture's proportions
};

bool is_canvas(const Node& n);  // a body showing a picture of its own (representation image, a raster)
const Node& canvas_node(const Scene& scene, const std::string& id);  // throws when `id` is not a canvas
void canvas_body_size(const Node& n, double& w, double& h);          // its picture's own rectangle (the body), mm; throws without one
CanvasPlace canvas_place(const Scene& scene, const std::string& id);
// The canvas's world matrix for that place: a similarity while width and height keep the picture's proportions (to 1e-12),
// else x and y scaled apart.
Mat4 canvas_world(const CanvasPlace& place);
// Bottom left, bottom right, top right, top left and the centre, world (handles, snapping).
std::array<Vec3, 5> canvas_points(const Mat4& world, double body_w, double body_h);
// The transform op putting the canvas at `world` (its local matrix under its parent).
json canvas_transform_op(const Scene& scene, const std::string& id, const Mat4& world);
// Calibrate: the canvas scaled about `a` so that its points `a` and `b` (world) lie `distance` apart.
Mat4 canvas_calibrate(const Mat4& world, const Vec3& a, const Vec3& b, double distance);
// Align to model ("scale relative to other objects"): turned, scaled and moved in its own plane so that its points `a` and
// `b` land on `a_to` and `b_to` projected on that plane; `residual`: how far the farther of those lies off it.
Mat4 canvas_align(const Mat4& world, const Vec3& a, const Vec3& a_to, const Vec3& b, const Vec3& b_to, double* residual = nullptr);
Mat4 affine_inverse(const Mat4& m);  // throws for a singular matrix

// A picture imported as a canvas (as import_file does, ImportOptions::canvas) without reading the document: its import op and
// body as a plan to commit, so inserting a canvas into a big model copies nothing (viewer mode and linking go through
// import_file / link_file).
design::Plan plan_canvas_import(const std::filesystem::path& file, const ImportOptions& opt);
// Replace: `file` (a picture) shown by the canvas in its place (its centre, turn and width; the height follows the picture):
// an edit of its import giving the node (same id, so every op on it still applies) the new picture, then a transform op.
// Both plans stage their new bodies in `doc`: a copy the caller owns, or the document the plan is committed to.
design::Plan plan_canvas_replace(Document& doc, const std::string& id, const std::filesystem::path& file);
// A sketch's backdrop images (`images`: their ids; empty: all) as canvases where they lie, with their opacity, and taken out
// of the sketch: one plan. The bytes go over as the sketch has them.
design::Plan plan_canvas_from_backdrop(Document& doc, const std::string& sketch, const std::vector<int>& images = {});
// Backdrop image records (as a sketch keeps them: data, name, position, width, height, angle, opacity) as canvases where they
// lie on `frame`: an import op and a body each (staged in `doc`), the canvases' node ids. A sketch's image tools end here:
// the pictures inserted while it was edited reach the document as canvases on its plane, never as its records.
struct CanvasImports {
  std::vector<json> ops;
  std::vector<design::NewBody> bodies;
  json canvases = json::array();
};
CanvasImports canvas_imports(Document& doc, const json& images, const Frame& frame);

namespace detail {
// A picture's canvas node without a document: a `w` x `h` mm rectangle (its body `key`, BREP text and meta) showing
// `bytes` (base64 in the raster, never re-encoded). Throws for bytes that are no picture OPAD reads.
struct CanvasBody {
  json node;  // type, id, name, key, representation, raster
  design::NewBody body;
  long px_w = 0, px_h = 0;
  double dpi = 0;  // 0: the file does not say
};
CanvasBody canvas_body(const std::string& bytes, const std::string& name, double w, double h, const std::string& base64 = {});
// Size and resolution from a picture's headers: false when it is no picture OPAD reads.
bool picture_size(const std::string& bytes, long& w, long& h, double& dpi, std::string& mime);
}  // namespace detail

}  // namespace opad
