// Geometry tests: STEP import/export, inspection, measuring, rendering, diff. argv[1] = fixtures directory.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/geometry.hpp"

using namespace opad;
namespace fs = std::filesystem;

static fs::path g_fixtures;
static fs::path g_tmp;

static fs::path fixture(const char* name) { return g_fixtures / name; }
static fs::path tmp(const char* name) { return g_tmp / name; }

static std::string find_node(const Scene& s, const std::string& name) {
  for (const auto& [id, n] : s.nodes)
    if (n.name == name) return id;
  throw check::Failure("node not found: " + name);
}

static std::string read_file(const fs::path& p) { return read_text_file(p); }

TEST(import_single_body_box) {
  Document d = Document::create();
  ImportResult r = import_step(d, fixture("box.step"));
  CHECK_EQ(r.bodies, 1);
  CHECK_EQ(r.new_entries, 1);
  CHECK(r.warnings.empty());
  Scene s = resolve(d);
  CHECK_EQ(s.roots.size(), 1u);
  const Node* n = s.node(s.roots[0]);
  CHECK(n->kind == Node::Kind::Body);
  CHECK_EQ(n->name, "Block");
  CHECK(n->has_color);
  CHECK_NEAR(n->color[2], 0.8, 0.01);
  CHECK_EQ(n->body_key.size(), 64u);
  CHECK(d.body(n->body_key) != nullptr);
  CHECK_EQ(d.body(n->body_key)->meta["source"], "box.step");
  json props = node_properties(d, s, n->id);
  CHECK_NEAR(props["volume"].get<double>(), 40.0 * 30 * 20 - M_PI * 16 * 20, 1.0);
  CHECK_EQ(props["faces"].get<int>(), 7);
  CHECK(props["solid"].get<bool>());
}

TEST(import_assembly_with_instances) {
  Document d = Document::create();
  ImportResult r = import_step(d, fixture("assembly.step"));
  Scene s = resolve(d);
  // Fixture > Plate, Lid, Fasteners > 4 x (Bolt > head + shank)
  CHECK_EQ(r.bodies, 10);
  CHECK_EQ(d.body_count(), 4u);  // plate, lid, head, shank: instances share entries (F4)
  CHECK_EQ(r.new_entries, 4);
  std::string fix = find_node(s, "Fixture");
  CHECK_EQ(s.node(fix)->children.size(), 3u);
  std::string fast = find_node(s, "Fasteners");
  CHECK_EQ(s.node(fast)->children.size(), 4u);
  CHECK_EQ(s.bodies_under(fast).size(), 8u);
  int max_instances = 0;
  for (const auto& [k, c] : s.instance_count) max_instances = std::max(max_instances, c);
  CHECK_EQ(max_instances, 4);
  // Placement: the lid sits 5 mm above the plate.
  std::string lid = find_node(s, "Lid");
  json lp = node_properties(d, s, lid);
  CHECK_NEAR(lp["bbox"]["min"][2].get<double>(), 5.0, 1e-6);
  CHECK_NEAR(lp["volume"].get<double>(), 100.0 * 60 * 3, 1e-3);
  json info = document_info(d, s);
  CHECK_EQ(info["components"].get<int>(), 6);
  CHECK_NEAR(info["bbox"]["size"][0].get<double>(), 100.0, 1e-6);
  d.save_as(tmp("assembly.opad"));
  CHECK(fs::exists(tmp("assembly.opad")));
}

TEST(determinism_same_keys_and_stable_text) {
  Document a = Document::create(), b = Document::create();
  import_step(a, fixture("assembly.step"));
  import_step(b, fixture("assembly.step"));
  const std::vector<std::string> akeys = a.body_keys(), bkeys = b.body_keys();
  std::set<std::string> ka(akeys.begin(), akeys.end()), kb(bkeys.begin(), bkeys.end());
  CHECK(ka == kb);
  a.save_as(tmp("det.opad"));
  std::string t1 = read_file(tmp("det.opad"));
  Document c = Document::load(tmp("det.opad"));
  c.save_as(tmp("det2.opad"));
  CHECK_EQ(read_file(tmp("det2.opad")), t1);
  CHECK(t1.find('\r') == std::string::npos);
}

TEST(inspect_faces_edges_and_measure) {
  Document d = Document::create();
  import_step(d, fixture("box.step"));
  Scene s = resolve(d);
  std::string block = s.roots[0];
  TopoDS_Shape proto = body_shape(d, s.node(block)->body_key);
  int nfaces = subshape_count(proto, Ref::Kind::Face);
  int cyl = -1, top = -1;
  for (int i = 0; i < nfaces; ++i) {
    json f = inspect_ref(d, s, Ref::parse(block + "/face/" + std::to_string(i)));
    CHECK_EQ(f["type"], "face");
    if (f["surface"] == "cylinder") cyl = i;
    if (f["surface"] == "plane" && std::fabs(f["normal"][2].get<double>() - 1.0) < 1e-9) top = i;
  }
  CHECK(cyl >= 0);
  CHECK(top >= 0);
  json hole = inspect_ref(d, s, Ref::parse(block + "/face/" + std::to_string(cyl)));
  CHECK_NEAR(hole["radius"].get<double>(), 4.0, 1e-9);
  CHECK(hole["adjacent_faces"].size() == 2);
  json rad = measure_radius(d, s, Ref::parse(block + "/face/" + std::to_string(cyl)));
  CHECK_NEAR(rad["diameter"].get<double>(), 8.0, 1e-9);
  // The displayed radius must start on the hole's axis and end on its surface.
  double radiusSq = 0;
  for (int i = 0; i < 3; ++i) {
    const double delta = rad["point_b"][i].get<double>() - rad["point_a"][i].get<double>();
    radiusSq += delta * delta;
  }
  CHECK_NEAR(std::sqrt(radiusSq), 4.0, 1e-9);
  CHECK_NEAR(rad["point_a"][2].get<double>(), rad["point_b"][2].get<double>(), 1e-9);
  CHECK_NEAR(rad["point_a"][0].get<double>(), hole["axis_origin"][0].get<double>(), 1e-9);
  CHECK_NEAR(rad["point_a"][1].get<double>(), hole["axis_origin"][1].get<double>(), 1e-9);
  int circularEdges = 0;
  std::vector<Ref> circleRefs;
  for (const auto& index : hole["edges"]) {
    const Ref edge = Ref::parse(block + "/edge/" + std::to_string(index.get<int>()));
    const json info = inspect_ref(d, s, edge);
    if (info["curve"] != "circle") continue;
    ++circularEdges;
    circleRefs.push_back(edge);
    const json circle = measure_radius(d, s, edge);
    double lengthSq = 0;
    for (int i = 0; i < 3; ++i) {
      CHECK_NEAR(circle["point_a"][i].get<double>(), info["center"][i].get<double>(), 1e-9);
      const double delta = circle["point_b"][i].get<double>() - circle["point_a"][i].get<double>();
      lengthSq += delta * delta;
    }
    CHECK_NEAR(std::sqrt(lengthSq), circle["value"].get<double>(), 1e-9);
  }
  CHECK(circularEdges > 0);
  CHECK(circleRefs.size() >= 2);
  const json edgeA = inspect_ref(d, s, circleRefs[0]), edgeB = inspect_ref(d, s, circleRefs[1]);
  Vec3 pickedA, pickedB;
  for (int i = 0; i < 3; ++i) {
    pickedA[i] = edgeA["start"][i].get<double>();
    pickedB[i] = edgeB["start"][i].get<double>();
  }
  const json anchored = measure_edge_distance(d, s, circleRefs[0], circleRefs[1], pickedA, pickedB, 0.5);
  CHECK(anchored["anchors"].is_array() && anchored["anchors"].size() >= 3);
  CHECK_EQ(anchored["anchor_index"].get<int>(), 0);
  bool hasClosest = false, hasFarthest = false;
  double closestValue = 0, farthestValue = 0;
  for (const auto& option : anchored["anchors"]) {
    if (option["kind"] == "closest") { hasClosest = true; closestValue = option["value"].get<double>(); }
    if (option["kind"] == "farthest") { hasFarthest = true; farthestValue = option["value"].get<double>(); }
    CHECK(option["point_a"].size() == 3 && option["point_b"].size() == 3);
  }
  CHECK(hasClosest && hasFarthest);
  CHECK(farthestValue + 1e-9 >= closestValue);
  for (int i = 0; i < 3; ++i) CHECK_NEAR(anchored["point_a"][i].get<double>(), pickedA[i], 1e-7);
  json topf = inspect_ref(d, s, Ref::parse(block + "/face/" + std::to_string(top)));
  CHECK_NEAR(topf["area"].get<double>(), 40.0 * 30 - M_PI * 16, 1e-6);
  // Angle between the hole axis and the top face normal is 0.
  json ang = measure_angle(d, s, Ref::parse(block + "/face/" + std::to_string(top)), Ref::parse(block + "/face/" + std::to_string(cyl)));
  CHECK_NEAR(ang["value"].get<double>(), 0.0, 1e-9);
  CHECK(ang["origin"].is_array() && ang["origin"].size() == 3);
  // Distance from a point to the body.
  json dist = measure_distance(d, s, Ref::parse("point/5,5,50"), Ref::parse(block));
  CHECK_NEAR(dist["value"].get<double>(), 30.0, 1e-6);
  json e = inspect_ref(d, s, Ref::parse(block + "/edge/0"));
  CHECK_EQ(e["type"], "edge");
  CHECK(e["length"].get<double>() > 0);
  json bb = measure_bbox(d, s, {Ref::parse(block)});
  CHECK_NEAR(bb["size"][0].get<double>(), 40.0, 1e-6);
  CHECK_THROWS(inspect_ref(d, s, Ref::parse(block + "/face/999")));
}

TEST(measure_between_bodies_in_assembly) {
  Document d = Document::load(tmp("assembly.opad"));
  Scene s = resolve(d);
  std::string plate = find_node(s, "Plate"), lid = find_node(s, "Lid");
  json dist = measure_distance(d, s, Ref::parse(plate), Ref::parse(lid));
  CHECK_NEAR(dist["value"].get<double>(), 0.0, 1e-6);
  std::vector<std::string> bolts;
  for (const auto& [id, n] : s.nodes)
    if (n.name.rfind("Bolt ", 0) == 0 && n.kind == Node::Kind::Component) bolts.push_back(id);
  CHECK_EQ(bolts.size(), 4u);
  json bb = measure_bbox(d, s, {Ref::parse(bolts[0])});
  CHECK(bb["size"][2].get<double>() > 10.0);
  // Measurement pinning through the command layer.
  json res = commands::run("measure", json{{"kind", "distance"}, {"refs", json::array({plate, lid})}, {"pin", true}}, &d);
  CHECK(res.contains("pinned_op"));
  Scene s2 = resolve(d);
  CHECK_EQ(s2.measurements.size(), 1u);
  CHECK(!s2.measurements[0].unresolved);
}

TEST(export_step_roundtrip_preserves_geometry_and_names) {
  Document d = Document::load(tmp("assembly.opad"));
  Scene s = resolve(d);
  std::string fast = find_node(s, "Fasteners");
  ExportOptions o;
  o.format = "step";
  o.select = {fast};
  ExportResult r = export_selection(d, s, tmp("fasteners.step"), o);
  CHECK_EQ(r.bodies, 8);
  CHECK(fs::file_size(tmp("fasteners.step")) > 1000);
  Document back = Document::create();
  ImportResult ir = import_step(back, tmp("fasteners.step"));
  CHECK_EQ(ir.bodies, 8);
  Scene sb = resolve(back);
  Vec3 lo, hi, lo2, hi2;
  CHECK(scene_bbox(d, s, s.bodies_under(fast), lo, hi));
  CHECK(scene_bbox(back, sb, {}, lo2, hi2));
  for (int k = 0; k < 3; ++k) {
    CHECK_NEAR(lo[k], lo2[k], 1e-3);
    CHECK_NEAR(hi[k], hi2[k], 1e-3);
  }
  bool has_bolt_name = false;
  for (const auto& [id, n] : sb.nodes) if (n.name.find("Bolt") != std::string::npos) has_bolt_name = true;
  CHECK(has_bolt_name);
  // Whole document to AP242.
  ExportOptions o2;
  o2.step_schema = "AP242";
  export_selection(d, s, tmp("all.step"), o2);
  std::string head = read_file(tmp("all.step")).substr(0, 600);
  CHECK(head.find("AP242") != std::string::npos);
}

TEST(export_obj_stl_glb) {
  Document d = Document::load(tmp("assembly.opad"));
  Scene s = resolve(d);
  std::string plate = find_node(s, "Plate");
  ExportOptions o;
  o.format = "obj";
  o.select = {plate};
  ExportResult r = export_selection(d, s, tmp("plate.obj"), o);
  CHECK_EQ(r.files.size(), 2u);  // obj + mtl
  std::string obj = read_file(tmp("plate.obj"));
  CHECK(obj.find("o Plate") != std::string::npos);
  CHECK(obj.find("\nf ") != std::string::npos);
  CHECK(read_file(tmp("plate.mtl")).find("Kd") != std::string::npos);

  o.format = "stl";
  o.select = {};
  r = export_selection(d, s, tmp("all.stl"), o);
  std::string stl = read_file(tmp("all.stl"));
  uint32_t ntri = 0;
  std::memcpy(&ntri, stl.data() + 80, 4);
  CHECK_EQ(stl.size(), 84u + ntri * 50u);
  CHECK(ntri >= 12 * 10);

  o.ascii = true;
  o.per_body = true;
  r = export_selection(d, s, tmp("per.stl"), o);
  CHECK_EQ(r.files.size(), 10u);
  CHECK(read_file(r.files[0]).rfind("solid ", 0) == 0);

  o = ExportOptions{};
  o.format = "glb";
  r = export_selection(d, s, tmp("all.glb"), o);
  CHECK_EQ(read_file(tmp("all.glb")).substr(0, 4), "glTF");
}

TEST(render_png_is_deterministic) {
  Document d = Document::load(tmp("assembly.opad"));
  Scene s = resolve(d);
  RenderOptions o;
  o.width = 320;
  o.height = 200;
  Image img = render_scene(d, s, o);
  CHECK_EQ(img.width, 320);
  CHECK_EQ(img.height, 200);
  int non_white = 0;
  for (size_t i = 0; i < img.rgb.size(); i += 3)
    if (img.rgb[i] != 255 || img.rgb[i + 1] != 255 || img.rgb[i + 2] != 255) ++non_white;
  CHECK(non_white > 320 * 200 / 10);
  std::string png1 = encode_png(img);
  CHECK_EQ(png1.substr(1, 3), "PNG");
  Image img2 = render_scene(d, s, o);
  CHECK(encode_png(img2) == png1);
  write_png(tmp("shot.png"), img);
  CHECK(fs::file_size(tmp("shot.png")) == png1.size());
  CHECK(png1.size() < img.rgb.size() / 4);  // compression actually happened
  for (const char* v : {"top", "front", "right", "iso-back"}) {
    o.camera = Camera::preset(v);
    CHECK(!render_scene(d, s, o).rgb.empty());
  }
  o.camera.perspective = true;
  CHECK(!render_scene(d, s, o).rgb.empty());
  // Hidden bodies are not drawn.
  std::string lid = find_node(s, "Lid");
  commands::run("appearance", json{{"target", lid}, {"visible", false}}, &d);
  Scene s2 = resolve(d);
  o.camera = Camera::preset("top");
  Image hidden = render_scene(d, s2, o);
  CHECK(encode_png(hidden) != encode_png(render_scene(d, s, o)));
}

TEST(diff_documents_and_image) {
  Document a = Document::load(tmp("assembly.opad"));
  Document b = Document::load(tmp("assembly.opad"));
  Scene s = resolve(b);
  std::string plate = find_node(s, "Plate");
  commands::run("annotate", json{{"anchor", plate}, {"text", "check thickness"}}, &b);
  commands::run("transform", json{{"target", find_node(s, "Lid")}, {"matrix", Mat4::translation(0, 0, 20).to_json()}}, &b);
  json diff = diff_documents(a, b);
  CHECK_EQ(diff["ops"]["added"].size(), 2u);
  CHECK_EQ(diff["ops"]["removed"].size(), 0u);
  CHECK_EQ(diff["geometry"]["moved"].get<int>(), 1);
  RenderOptions o;
  o.width = 200;
  o.height = 120;
  Image img = render_diff(a, b, o);
  bool has_red = false, has_green = false;
  for (size_t i = 0; i < img.rgb.size(); i += 3) {
    if (img.rgb[i] > 150 && img.rgb[i + 1] < 110 && img.rgb[i + 2] < 110) has_red = true;
    if (img.rgb[i + 1] > 120 && img.rgb[i] < 110 && img.rgb[i + 2] < 120) has_green = true;
  }
  CHECK(has_red);
  CHECK(has_green);
}

TEST(command_layer_browse_mode_and_paths) {
  json info = commands::run("info", json{{"doc", fixture("box.step").string()}});
  CHECK_EQ(info["bodies"].get<int>(), 1);
  json tree = commands::run("tree", json{{"doc", tmp("assembly.opad").string()}, {"depth", 1}});
  CHECK_EQ(tree["roots"].size(), 1u);
  CHECK(tree["roots"][0].contains("children_count"));
  json ops = commands::run("ops", json{{"doc", tmp("assembly.opad").string()}, {"type", "import"}});
  CHECK_EQ(ops.size(), 1u);
  // Mutating a .step in browse mode is transient.
  json ann = commands::run("annotate", json{{"doc", fixture("box.step").string()}, {"anchor", "point/0,0,0"}, {"text", "x"}});
  CHECK(ann.value("transient", false));
  json cmds = commands::run("commands", json::object());
  CHECK(cmds.size() > 15);
  CHECK_THROWS(commands::run("nope", json::object()));
  // Mesh for plugins.
  json mesh = commands::run("mesh", json{{"doc", fixture("box.step").string()}, {"tolerance", 0.5}});
  CHECK_EQ(mesh.size(), 1u);
  CHECK(mesh[0]["indices"].size() >= 36);
}

TEST(import_into_component_and_gc) {
  Document d = Document::load(tmp("assembly.opad"));
  Scene s = resolve(d);
  std::string fast = find_node(s, "Fasteners");
  ImportOptions o;
  o.parent = fast;
  ImportResult r = import_step(d, fixture("box.step"), o);
  Scene s2 = resolve(d);
  CHECK_EQ(s2.node(fast)->children.size(), 5u);
  CHECK_EQ(d.body_count(), 5u);
  commands::run("delete", json{{"target", r.op_id}}, &d);
  CHECK_EQ(resolve(d).node(fast)->children.size(), 4u);
  CHECK_EQ(d.gc().size(), 1u);
  CHECK_EQ(d.body_count(), 4u);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: opad-test-step <fixtures-dir> [filter]\n");
    return 2;
  }
  g_fixtures = argv[1];
  g_tmp = fs::temp_directory_path() / "opad-test-step";
  fs::create_directories(g_tmp);
  return check::run_all(argc - 1, argv + 1);
}
