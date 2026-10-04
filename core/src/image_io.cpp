// Pictures (.png, .jpg, .bmp, .gif, .webp) as a canvas: one flat rectangle as large as the picture at its resolution (96 dpi
// unless the file says), the picture on it as the SVG reader's rasters are: `raster` holds the file's own bytes (never
// decoded or encoded again) and the picture's corners. Nothing is decoded here: size and resolution come from the headers.
#include <BRepBuilderAPI_MakeFace.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "import_common.hpp"
#include "opad/canvas.hpp"
#include "opad/geometry.hpp"

namespace opad::detail {
namespace {

struct Picture {
  std::string mime;
  long w = 0, h = 0;
  double dpi = 0;  // 0: the file does not say
};

uint32_t be(const std::string& b, size_t at, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; ++i) v = (v << 8) | static_cast<unsigned char>(b.at(at + size_t(i)));
  return v;
}
uint32_t le(const std::string& b, size_t at, int n) {
  uint32_t v = 0;
  for (int i = n - 1; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b.at(at + size_t(i)));
  return v;
}

Picture png(const std::string& b) {
  Picture p{"image/png", long(be(b, 16, 4)), long(be(b, 20, 4))};
  for (size_t at = 8; at + 8 <= b.size();) {  // pHYs (pixels per metre) comes before the image data
    const uint32_t length = be(b, at, 4);
    const std::string type = b.substr(at + 4, 4);
    if (type == "pHYs" && length >= 9 && b.at(at + 16) == 1) p.dpi = be(b, at + 8, 4) * 0.0254;
    if (type == "IDAT" || type == "IEND") break;
    at += 12 + size_t(length);
  }
  return p;
}

Picture jpeg(const std::string& b) {
  Picture p{"image/jpeg"};
  for (size_t at = 2; at + 4 <= b.size();) {
    if (static_cast<unsigned char>(b[at]) != 0xFF) break;
    const unsigned marker = static_cast<unsigned char>(b[at + 1]);
    if (marker == 0xFF) {  // fill
      ++at;
      continue;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {  // no length
      at += 2;
      continue;
    }
    if (marker == 0xD9 || marker == 0xDA) break;
    if (marker == 0xE0 && b.compare(at + 4, 5, std::string("JFIF\0", 5)) == 0) {  // density: 1 per inch, 2 per cm
      const unsigned units = static_cast<unsigned char>(b.at(at + 11));
      const double x = be(b, at + 12, 2);
      p.dpi = units == 1 ? x : units == 2 ? x * 2.54 : 0;
    }
    if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {  // a frame header
      p.h = long(be(b, at + 5, 2));
      p.w = long(be(b, at + 7, 2));
      break;
    }
    at += 2 + be(b, at + 2, 2);
  }
  return p;
}

Picture webp(const std::string& b) {
  Picture p{"image/webp"};
  const std::string chunk = b.substr(12, 4);
  if (chunk == "VP8 ") p.w = long(le(b, 26, 2) & 0x3FFF), p.h = long(le(b, 28, 2) & 0x3FFF);
  else if (chunk == "VP8L") p.w = long((le(b, 21, 4) & 0x3FFF) + 1), p.h = long(((le(b, 21, 4) >> 14) & 0x3FFF) + 1);
  else if (chunk == "VP8X") p.w = long(le(b, 24, 3) + 1), p.h = long(le(b, 27, 3) + 1);
  return p;
}

Picture picture(const std::string& b, const std::string& name) {
  Picture p;
  try {
    if (b.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) p = png(b);
    else if (b.size() > 3 && static_cast<unsigned char>(b[0]) == 0xFF && static_cast<unsigned char>(b[1]) == 0xD8) p = jpeg(b);
    else if (b.compare(0, 2, "BM") == 0) {
      p = {"image/bmp", long(int32_t(le(b, 18, 4))), std::labs(long(int32_t(le(b, 22, 4))))};  // a negative height: rows top-down
      p.dpi = le(b, 38, 4) * 0.0254;
    } else if (b.compare(0, 4, "GIF8") == 0) p = {"image/gif", long(le(b, 6, 2)), long(le(b, 8, 2))};
    else if (b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0) p = webp(b);
  } catch (const std::out_of_range&) {
    p = {};
  }
  if (p.mime.empty() || p.w <= 0 || p.h <= 0 || p.w > 1000000 || p.h > 1000000) throw Error("not a picture OPAD reads, or a damaged one: " + name);
  return p;
}

std::string base64(const std::string& bytes) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    unsigned bits = unsigned(static_cast<unsigned char>(bytes[i])) << 16;
    if (i + 1 < bytes.size()) bits |= unsigned(static_cast<unsigned char>(bytes[i + 1])) << 8;
    if (i + 2 < bytes.size()) bits |= unsigned(static_cast<unsigned char>(bytes[i + 2]));
    out += alphabet[(bits >> 18) & 63];
    out += alphabet[(bits >> 12) & 63];
    out += i + 1 < bytes.size() ? alphabet[(bits >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? alphabet[bits & 63] : '=';
  }
  return out;
}

// The node showing a picture on a w x h mm rectangle (its key comes with the body). Corners: the picture's top left, top
// right and bottom left (as SVG images are kept).
json picture_node(const std::string& encoded, const Picture& p, const std::string& name, double w, double h) {
  json node = {{"type", "body"}, {"id", new_uuid()}, {"name", name}, {"representation", "image"}};
  node["raster"] = {{"href", "data:" + p.mime + ";base64," + encoded}, {"corners", {{0, h, 0}, {w, h, 0}, {0, 0, 0}}}, {"px", {p.w, p.h}}};
  if (p.dpi >= 1) node["raster"]["dpi"] = p.dpi;
  return node;
}

TopoDS_Face rectangle(double w, double h) { return BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()), 0, w, 0, h).Face(); }

Vec3 unit(const Vec3& v) {
  const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return l > 1e-15 ? Vec3{v[0] / l, v[1] / l, v[2] / l} : v;
}

// The import op of a picture's canvas node (w x h mm): placed as the options say (a width scales it uniformly, the body
// stays the picture's own rectangle; centred on request), with its flags and the plane its place is given in (the
// placement's, unless the caller chose the plane).
json canvas_import(json node, const std::string& name, double w, double h, const ImportOptions& opt) {
  const json given = opt.canvas.is_object() ? opt.canvas : json::object();
  CanvasFlags flags = CanvasFlags::of(given);
  if (!flags.plane.is_object()) flags.plane = Frame{opt.placement.apply({0, 0, 0}), unit(opt.placement.apply_dir({1, 0, 0})), unit(opt.placement.apply_dir({0, 1, 0}))}.to_json();
  Mat4 placement = opt.placement;
  if (const double width = given.value("width", 0.0); width > 0) {
    Mat4 scale;
    scale.at(0, 0) = scale.at(1, 1) = scale.at(2, 2) = width / w;
    placement = placement * scale;
  }
  if (opt.center_drawing || given.value("center", false)) placement = placement * Mat4::translation(-w / 2, -h / 2, 0);
  if (!placement.is_identity()) node["transform"] = placement.to_json();
  json op = {{"op", "import"}, {"source", name}, {"units", "mm"}, {"nodes", json::array({node})}, {"canvas", flags.to_json()}};
  if (!opt.parent.empty()) op["parent"] = opt.parent;
  return op;
}

}  // namespace

bool picture_size(const std::string& bytes, long& w, long& h, double& dpi, std::string& mime) {
  try {
    const Picture p = picture(bytes, "");
    w = p.w, h = p.h, dpi = p.dpi, mime = p.mime;
    return true;
  } catch (const Error&) {
    return false;
  }
}

CanvasBody canvas_body(const std::string& bytes, const std::string& name, double w, double h, const std::string& encoded) {
  const Picture p = picture(bytes, name);
  if (!(w > 0) || !(h > 0)) throw Error("a canvas needs a positive size: " + name);
  CanvasBody out;
  out.px_w = p.w, out.px_h = p.h, out.dpi = p.dpi;
  out.node = picture_node(encoded.empty() ? base64(bytes) : encoded, p, name, w, h);
  const TopoDS_Face face = rectangle(w, h);
  out.body.meta = {{"name", name}, {"units", "mm"}, {"source", name}, {"representation", "image"}};
  out.body.key = body_key_for(face, &out.body.brep);
  out.body.shape = std::make_shared<TopoDS_Shape>(face);
  out.node["key"] = out.body.key;
  return out;
}

ImportResult import_image(Document& doc, const std::filesystem::path& file, const ImportOptions& opt) {
  if (opt.progress && !opt.progress(-1, "reading")) throw Error("import cancelled");
  const std::string bytes = read_text_file(file);
  const auto u8 = file.filename().u8string(), stem = file.stem().u8string();
  const std::string name(u8.begin(), u8.end());
  const Picture p = picture(bytes, name);
  const double dpi = p.dpi >= 1 ? p.dpi : 96.0, w = double(p.w) * 25.4 / dpi, h = double(p.h) * 25.4 / dpi;
  ImportResult res;
  Document staged = doc;
  json body = picture_node(base64(bytes), p, std::string(stem.begin(), stem.end()), w, h);
  body["key"] = store_body(staged, rectangle(w, h), {{"name", body["name"]}, {"units", "mm"}, {"source", name}, {"representation", "image"}}, opt, false, &res);
  ++res.bodies;
  res.op_id = staged.append(canvas_import(body, name, w, h, opt), opt.author).id;
  doc = std::move(staged);
  res.info = {{"px", {p.w, p.h}}, {"dpi", dpi}, {"size_mm", {w, h}}, {"canvas", body["id"]}};
  return res;
}

}  // namespace opad::detail

namespace opad {
design::Plan plan_canvas_import(const std::filesystem::path& file, const ImportOptions& opt) {
  const std::string bytes = read_text_file(file);
  const auto u8 = file.filename().u8string(), stem = file.stem().u8string();
  const std::string name(u8.begin(), u8.end());
  long pw = 0, ph = 0;
  double dpi = 0;
  std::string mime;
  if (!detail::picture_size(bytes, pw, ph, dpi, mime)) throw Error("not a picture OPAD reads, or a damaged one: " + name);
  if (dpi < 1) dpi = 96;
  const double w = double(pw) * 25.4 / dpi, h = double(ph) * 25.4 / dpi;
  detail::CanvasBody made = detail::canvas_body(bytes, std::string(stem.begin(), stem.end()), w, h);
  made.body.meta["source"] = name;
  json op = detail::canvas_import(made.node, name, w, h, opt);
  op["id"] = new_uuid();
  design::Plan plan;
  plan.report = {{"op", op["id"]}, {"canvas", made.node["id"]}, {"px", {pw, ph}}, {"size_mm", {w, h}}};
  plan.ops.push_back(std::move(op));
  plan.bodies.push_back(std::move(made.body));
  return plan;
}
}  // namespace opad
