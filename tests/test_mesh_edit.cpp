// Mesh editing for print prep (opad/mesh_edit.hpp, design/mesh_features.cpp) on generated STLs with known answers: scale
// per axis and to a size, Booleans between meshes and with a solid, cuts by a plane with caps (and an open mesh without),
// shells split into parts, mirrored copies, repair of a broken mesh, simplify, drop to bed, and the results replayed the
// same after saving (no new body entries on a forced regeneration).
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/design/feature.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/mesh_edit.hpp"
#include "opad/step_io.hpp"

using namespace opad;
namespace mo = opad::meshops;

namespace {

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-meshedit-" + new_uuid());
  Files() { std::filesystem::create_directories(dir); }
  ~Files() {
    std::error_code e;
    std::filesystem::remove_all(dir, e);
  }
};

void write_stl(const std::filesystem::path& p, const mo::Mesh& m) {
  std::string s(80, '\0');
  auto put = [&](auto v) {
    char b[sizeof(v)];
    std::memcpy(b, &v, sizeof(v));
    s.append(b, sizeof(v));
  };
  put(uint32_t(m.triangles()));
  for (size_t t = 0; t < m.triangles(); ++t) {
    for (int k = 0; k < 3; ++k) put(0.0f);
    for (int c = 0; c < 3; ++c)
      for (int k = 0; k < 3; ++k) put(float(m.xyz[size_t(m.tri[t * 3 + c]) * 3 + k]));
    put(uint16_t(0));
  }
  std::ofstream f(p, std::ios::binary);
  f.write(s.data(), std::streamsize(s.size()));
}

mo::Mesh box_mesh(double x0, double y0, double z0, double x1, double y1, double z1) {
  return mo::mesh_solid_copy(BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), gp_Pnt(x1, y1, z1)).Shape(), 0.01);
}
mo::Mesh sphere_mesh(double r, double deflection) { return mo::mesh_solid_copy(BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), r).Shape(), deflection); }

mo::Mesh joined(mo::Mesh a, const mo::Mesh& b) {
  const uint32_t base = uint32_t(a.vertices());
  a.xyz.insert(a.xyz.end(), b.xyz.begin(), b.xyz.end());
  for (uint32_t v : b.tri) a.tri.push_back(base + v);
  return a;
}

Document import_stl(const Files& f, const std::string& name, const mo::Mesh& m) {
  write_stl(f.dir / name, m);
  Document d = Document::create();
  ImportOptions o;
  o.viewer = false;
  import_file(d, f.dir / name, o);
  return d;
}

void import_into(Document& d, const Files& f, const std::string& name, const mo::Mesh& m) {
  write_stl(f.dir / name, m);
  ImportOptions o;
  o.viewer = false;
  import_file(d, f.dir / name, o);
}

json refs(const std::vector<std::string>& ids) {
  json a = json::array();
  for (const auto& id : ids) a.push_back({{"body", id}});
  return a;
}

mo::Mesh world(const Document& d, const std::string& node) { return mo::from_shape(node_world_shape(d, resolve(d), node)); }
Bnd_Box box(const Document& d, const std::string& node) { return mo::bounds(world(d, node)); }
std::array<double, 6> extent(const Bnd_Box& b) {
  std::array<double, 6> e;
  b.Get(e[0], e[1], e[2], e[3], e[4], e[5]);
  return e;
}
bool close_to(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

TEST(scale_per_axis_and_to_size) {
  Files f;
  Document d = import_stl(f, "block.stl", box_mesh(0, 0, 0, 20, 10, 5));
  const std::string body = resolve(d).all_bodies().front();
  CHECK_EQ(resolve(d).node(body)->representation, std::string("mesh"));
  // Per axis about the origin.
  design::apply_ops(d, {design::make_feature_op("scale", "S1", {{"bodies", refs({body})}, {"mode", "axes"}, {"factor_x", "2"}, {"factor_y", "1"}, {"factor_z", "0.5"}})});
  auto e = extent(box(d, body));
  CHECK(close_to(e[3], 40, 1e-5) && close_to(e[4], 10, 1e-5) && close_to(e[5], 2.5, 1e-5));
  CHECK_EQ(resolve(d).node(body)->representation, std::string("mesh"));
  CHECK(mo::check(world(d, body)).printable());
  // To a size with locked proportions (X 80 mm: everything doubles), about the bed (bottom middle stays).
  design::apply_ops(d, {design::make_feature_op("scale", "S2", {{"bodies", refs({body})}, {"mode", "size"}, {"size_x", "80 mm"}, {"about", "bed"}})});
  e = extent(box(d, body));
  CHECK(close_to(e[3] - e[0], 80, 1e-5) && close_to(e[4] - e[1], 20, 1e-5) && close_to(e[5] - e[2], 5, 1e-5));
  CHECK(close_to(e[2], 0, 1e-6) && close_to((e[0] + e[3]) / 2, 20, 1e-5));
  // Percent; an older op without a mode is a factor.
  design::apply_ops(d, {design::make_feature_op("scale", "S3", {{"bodies", refs({body})}, {"mode", "percent"}, {"percent", "50"}})});
  e = extent(box(d, body));
  CHECK(close_to(e[3] - e[0], 40, 1e-5));
  design::apply_ops(d, {design::make_feature_op("scale", "S4", {{"bodies", refs({body})}, {"factor", "2"}, {"about", "centre"}})});
  e = extent(box(d, body));
  CHECK(close_to(e[3] - e[0], 80, 1e-5));
  // A solid scaled per axis too.
  Document s = Document::create();
  design::apply_ops(s, {design::make_feature_op("box", "B", {{"length", "10 mm"}, {"width", "10 mm"}, {"height", "10 mm"}})});
  const std::string solid = resolve(s).all_bodies().front();
  design::apply_ops(s, {design::make_feature_op("scale", "S", {{"bodies", refs({solid})}, {"mode", "axes"}, {"factor_x", "3"}, {"factor_y", "1"}, {"factor_z", "1"}})});
  const auto se = extent(mo::tight_box(node_world_shape(s, resolve(s), solid)));
  CHECK(close_to(se[3] - se[0], 30, 1e-4) && close_to(se[4] - se[1], 10, 1e-4));
}

TEST(mesh_booleans_with_meshes_and_solids) {
  Files f;
  Document d = import_stl(f, "a.stl", box_mesh(0, 0, 0, 20, 20, 20));
  import_into(d, f, "b.stl", box_mesh(10, 10, 10, 30, 30, 30));
  auto bodies = resolve(d).all_bodies();
  CHECK_EQ(bodies.size(), size_t(2));
  const std::string a = bodies[0], b = bodies[1];
  // Cut, keeping the tool: 8000 - 1000.
  design::apply_ops(d, {design::make_feature_op("combine", "Cut", {{"target", refs({a})}, {"tools", refs({b})}, {"operation", "cut"}, {"keep_tools", true}})});
  CHECK(close_to(mo::volume(world(d, a)), 7000, 1e-6));
  CHECK(mo::check(world(d, a)).printable());
  CHECK_EQ(resolve(d).all_bodies().size(), size_t(2));
  // Join: one body, 7000 + 8000.
  design::apply_ops(d, {design::make_feature_op("combine", "Join", {{"target", refs({a})}, {"tools", refs({b})}, {"operation", "join"}})});
  CHECK_EQ(resolve(d).all_bodies().size(), size_t(1));
  CHECK(close_to(mo::volume(world(d, a)), 15000, 1e-6));
  // A solid sphere cut out of the mesh: the result is a mesh, close to the exact volume.
  design::apply_ops(d, {design::make_feature_op("sphere", "Ball", {{"diameter", "10 mm"}})});
  const std::string ball = resolve(d).all_bodies().back();
  CHECK(ball != a);
  const double before = mo::volume(world(d, a));
  design::apply_ops(d, {design::make_feature_op("combine", "Hollow", {{"target", refs({a})}, {"tools", refs({ball})}, {"operation", "cut"}, {"tolerance", "0.01 mm"}})});
  const double cut = before - mo::volume(world(d, a));
  const double octant = 4.0 / 3 * M_PI * 125 / 8;  // the ball at the origin, the block's corner there: one eighth inside
  CHECK(close_to(cut, octant, octant * 0.01));
  CHECK(mo::check(world(d, a)).printable());
  // A solid target and a mesh tool: the solid becomes a mesh (its node kept).
  Document m = Document::create();
  design::apply_ops(m, {design::make_feature_op("box", "B", {{"length", "40 mm"}, {"width", "40 mm"}, {"height", "40 mm"}})});
  const std::string solid = resolve(m).all_bodies().front();
  import_into(m, f, "c.stl", box_mesh(-5, -5, -5, 5, 5, 50));
  const std::string pin = resolve(m).all_bodies().back();
  design::apply_ops(m, {design::make_feature_op("combine", "Drill", {{"target", refs({solid})}, {"tools", refs({pin})}, {"operation", "cut"}})});
  CHECK_EQ(resolve(m).node(solid)->representation, std::string("mesh"));
  CHECK(close_to(mo::volume(world(m, solid)), 64000 - 10 * 10 * 40, 1e-6));  // the box is centred in X and Y, 0..40 high
  // An open mesh is refused with what is wrong with it.
  mo::Mesh open = box_mesh(100, 0, 0, 110, 10, 10);
  open.tri.resize(open.tri.size() - 3);
  import_into(m, f, "open.stl", open);
  const std::string broken = resolve(m).all_bodies().back();
  bool said = false;
  try {
    design::apply_ops(m, {design::make_feature_op("combine", "J", {{"target", refs({solid})}, {"tools", refs({broken})}, {"operation", "join"}})});
  } catch (const std::exception& e) {
    said = std::string(e.what()).find("Repair") != std::string::npos;
  }
  CHECK(said);
}

TEST(split_by_plane_caps_and_parts) {
  Files f;
  Document d = import_stl(f, "block.stl", box_mesh(0, 0, 0, 20, 20, 20));
  const std::string body = resolve(d).all_bodies().front();
  design::apply_ops(d, {design::make_feature_op("plane", "Mid", {{"mode", "offset"}, {"plane", {{"base", "xy"}}}, {"distance", "5 mm"}})});
  const std::string plane = resolve(d).features.back().id;
  design::apply_ops(d, {design::make_feature_op("split", "Cut", {{"bodies", refs({body})}, {"plane", {{"feature", plane}}}})});
  auto bodies = resolve(d).all_bodies();
  CHECK_EQ(bodies.size(), size_t(2));
  double total = 0;
  for (const auto& b : bodies) {
    CHECK(mo::check(world(d, b)).printable());  // capped
    total += mo::volume(world(d, b));
  }
  CHECK(close_to(total, 8000, 1e-6));
  CHECK(close_to(mo::volume(world(d, body)), 20 * 20 * 15, 1e-6));  // the side the normal points to stays the body
  // Two shells in one STL become two bodies.
  Document p = import_stl(f, "two.stl", joined(box_mesh(0, 0, 0, 10, 10, 10), box_mesh(20, 0, 0, 25, 5, 5)));
  const std::string two = resolve(p).all_bodies().front();
  design::apply_ops(p, {design::make_feature_op("split_parts", "Parts", {{"bodies", refs({two})}})});
  bodies = resolve(p).all_bodies();
  CHECK_EQ(bodies.size(), size_t(2));
  CHECK(close_to(mo::volume(world(p, bodies[0])), 1000, 1e-6) && close_to(mo::volume(world(p, bodies[1])), 125, 1e-6));
  // An open mesh (a scan) is clipped without caps.
  mo::Mesh sheet = box_mesh(0, 0, 0, 10, 10, 10);
  sheet.tri.resize(sheet.tri.size() - 6);
  const mo::Cut c = mo::split_by_plane(sheet, {0, 0, 4}, {0, 0, 1});
  CHECK(!c.capped && !c.above.empty() && !c.below.empty());
}

TEST(keep_one_side_of_a_cut) {
  Files f;
  Document k = import_stl(f, "block.stl", box_mesh(0, 0, -10, 20, 20, 10));
  const std::string kb = resolve(k).all_bodies().front();
  design::apply_ops(k, {design::make_feature_op("split", "Cut", {{"bodies", refs({kb})}, {"plane", {{"base", "xy"}}}, {"keep", "below"}})});
  CHECK_EQ(resolve(k).all_bodies().size(), size_t(1));
  const auto e = extent(box(k, kb));
  CHECK(close_to(e[5], 0, 1e-6) && close_to(e[2], -10, 1e-6));
  CHECK(close_to(mo::volume(world(k, kb)), 4000, 1e-6));
}

TEST(mirror_and_pattern_copies_of_a_mesh) {
  Files f;
  Document d = import_stl(f, "block.stl", box_mesh(5, 0, 0, 15, 10, 10));
  const std::string body = resolve(d).all_bodies().front();
  design::apply_ops(d, {design::make_feature_op("mirror", "M", {{"bodies", refs({body})}, {"plane", {{"base", "yz"}}}})});
  auto bodies = resolve(d).all_bodies();
  CHECK_EQ(bodies.size(), size_t(2));
  const std::string copy = bodies.back();
  CHECK_EQ(resolve(d).node(copy)->representation, std::string("mesh"));
  CHECK(close_to(mo::volume(world(d, copy)), 1000, 1e-6));  // turned outward again
  CHECK(close_to(extent(box(d, copy))[0], -15, 1e-6));
  design::apply_ops(d, {design::make_feature_op("pattern_rect", "P", {{"bodies", refs({body})}, {"axis", {{"base", "x"}}}, {"count", "3"}, {"spacing", "20 mm"}})});
  CHECK_EQ(resolve(d).all_bodies().size(), size_t(4));
  design::apply_ops(d, {design::make_feature_op("move", "C", {{"bodies", refs({body})}, {"dx", "0 mm"}, {"dy", "0 mm"}, {"dz", "30 mm"}, {"copy", true}})});
  CHECK_EQ(resolve(d).all_bodies().size(), size_t(5));
  CHECK(close_to(extent(box(d, resolve(d).all_bodies().back()))[2], 30, 1e-6));
}

TEST(repair_closes_holes_and_turns_triangles) {
  const mo::Mesh whole = box_mesh(0, 0, 0, 10, 10, 10);
  const size_t n = whole.triangles();
  auto all_at = [&](size_t t, int axis, double v) {
    for (int k = 0; k < 3; ++k)
      if (whole.xyz[size_t(whole.tri[t * 3 + k]) * 3 + size_t(axis)] != v) return false;
    return true;
  };
  mo::Mesh m;
  m.xyz = whole.xyz;
  size_t flipped = 0, doubled = 0;
  for (size_t t = 0; t < n; ++t) {
    if (all_at(t, 2, 10)) continue;  // the top's triangles gone: a square hole
    const uint32_t* v = &whole.tri[t * 3];
    if (all_at(t, 2, 0) && !flipped++) m.tri.insert(m.tri.end(), {v[0], v[2], v[1]});  // a bottom one turned round
    else m.tri.insert(m.tri.end(), {v[0], v[1], v[2]});
    if (all_at(t, 0, 0) && !doubled++) m.tri.insert(m.tri.end(), {v[0], v[1], v[2]});  // a side one twice
  }
  const mo::Report before = mo::check(m);
  std::printf("  broken box: %zu triangles, %zu open, %zu holes, %zu duplicates, %zu flipped, %zu non-manifold\n", m.triangles(), before.open_edges,
              before.holes, before.duplicates, before.flipped_edges, before.non_manifold_edges);
  CHECK(before.open_edges == 4 && before.holes == 1 && before.duplicates == 1 && before.flipped_edges == 2 && before.non_manifold_edges == 3 && !before.printable());
  const mo::RepairResult r = mo::repair(m);
  CHECK(r.after.printable());
  CHECK(r.holes_filled == 1 && r.removed_duplicates == 1 && r.flipped >= 1);
  CHECK_EQ(r.mesh.triangles(), n);
  CHECK(close_to(mo::volume(r.mesh), 1000, 1e-9));
  // Inside out as a whole: turned outward.
  mo::Mesh inv = box_mesh(0, 0, 0, 10, 10, 10);
  for (size_t t = 0; t < inv.triangles(); ++t) std::swap(inv.tri[t * 3 + 1], inv.tri[t * 3 + 2]);
  CHECK(mo::volume(inv) < 0);
  CHECK(close_to(mo::volume(mo::repair(inv).mesh), 1000, 1e-9));
  // Two boxes through each other cross; a box alone does not.
  CHECK(mo::check(joined(box_mesh(0, 0, 0, 10, 10, 10), box_mesh(5, 5, 5, 15, 15, 15))).self_intersections > 0);
  CHECK_EQ(mo::check(box_mesh(0, 0, 0, 10, 10, 10)).self_intersections, size_t(0));
  // The feature, on a document.
  Files f;
  Document d = import_stl(f, "broken.stl", m);
  const std::string body = resolve(d).all_bodies().front();
  const json res = design::apply_ops(d, {design::make_feature_op("mesh_repair", "Fix", {{"bodies", refs({body})}})});
  CHECK(mo::check(world(d, body)).printable());
  const json stored = resolve(d).features.back().result;
  CHECK(stored.contains("repair") && stored["repair"][0]["after"]["printable"] == true);
  (void)res;
}

TEST(simplify_to_a_share_and_a_tolerance) {
  const mo::Mesh ball = sphere_mesh(20, 0.002);
  std::printf("  ball: %zu triangles, %s\n", ball.triangles(), mo::check(ball, {false, 0}).to_json().dump().c_str());
  CHECK(ball.triangles() > 20000);
  mo::SimplifyOptions opt;
  opt.target_triangles = ball.triangles() / 4;
  double err = 0;
  const mo::Mesh quarter = mo::simplify(ball, opt, &err);
  CHECK(quarter.triangles() <= ball.triangles() / 4 && quarter.triangles() > ball.triangles() / 8);
  std::printf("  quarter: %s\n", mo::check(quarter, {false, 0}).to_json().dump().c_str());
  CHECK(mo::check(quarter).closed());
  CHECK(err < 0.2);
  CHECK(close_to(mo::volume(quarter), mo::volume(ball), mo::volume(ball) * 0.01));
  mo::SimplifyOptions tol;
  tol.tolerance = 0.5;
  const mo::Mesh coarse = mo::simplify(ball, tol, &err);
  CHECK(coarse.triangles() < quarter.triangles() && err <= 0.5 + 1e-6);
  Files f;
  Document d = import_stl(f, "ball.stl", ball);
  const std::string body = resolve(d).all_bodies().front();
  design::apply_ops(d, {design::make_feature_op("mesh_simplify", "Light", {{"bodies", refs({body})}, {"mode", "ratio"}, {"ratio", "10"}})});
  CHECK(world(d, body).triangles() <= ball.triangles() / 10 + 1);
}

TEST(drop_to_bed_and_centre) {
  Files f;
  Document d = import_stl(f, "float.stl", box_mesh(30, 40, 12, 40, 60, 22));
  const std::string body = resolve(d).all_bodies().front();
  const std::string key = resolve(d).node(body)->body_key;
  design::apply_ops(d, {design::make_feature_op("drop_to_bed", "Drop", {{"bodies", refs({body})}})});
  auto e = extent(box(d, body));
  CHECK(close_to(e[2], 0, 1e-9) && close_to(e[0], 30, 1e-9));
  CHECK_EQ(resolve(d).node(body)->body_key, key);  // placed, not rebuilt
  design::apply_ops(d, {design::make_feature_op("drop_to_bed", "Centre", {{"bodies", refs({body})}, {"center", true}})});
  e = extent(box(d, body));
  CHECK(close_to(e[0] + e[3], 0, 1e-9) && close_to(e[1] + e[4], 0, 1e-9) && close_to(e[2], 0, 1e-9));
}

TEST(results_replay_the_same_after_saving) {
  Files f;
  Document d = import_stl(f, "a.stl", box_mesh(-10, -10, -10, 10, 10, 10));
  import_into(d, f, "b.stl", sphere_mesh(8, 0.05));
  auto bodies = resolve(d).all_bodies();
  design::apply_ops(d, {design::make_feature_op("combine", "Cut", {{"target", refs({bodies[0]})}, {"tools", refs({bodies[1]})}, {"operation", "cut"}})});
  design::apply_ops(d, {design::make_feature_op("scale", "S", {{"bodies", refs({bodies[0]})}, {"mode", "axes"}, {"factor_x", "1.5"}, {"factor_y", "1"}, {"factor_z", "1"}})});
  design::apply_ops(d, {design::make_feature_op("split", "Half", {{"bodies", refs({bodies[0]})}, {"plane", {{"base", "yz"}}}})});
  const size_t entries = d.body_count();
  const design::Plan again = design::plan_regenerate(d, true);
  CHECK(again.bodies.empty());  // recomputed, every result the same (keys of the same text)
  design::commit(d, design::plan_regenerate(d, true));
  CHECK_EQ(d.body_count(), entries);
  d.save_as(f.dir / "edited.opad");
  const Document back = Document::load(f.dir / "edited.opad");
  const Scene s = resolve(back);
  CHECK_EQ(s.all_bodies().size(), resolve(d).all_bodies().size());
  for (const auto& id : s.all_bodies()) {
    CHECK_EQ(s.node(id)->representation, std::string("mesh"));
    CHECK(mo::from_shape(node_world_shape(back, s, id)).xyz == world(d, id).xyz);
  }
}

// The Booleans on two meshes of over a million triangles each (as heavy-mesh's tube): seconds, not minutes.
TEST(boolean_of_two_million_triangle_meshes) {
  if (const char* e = std::getenv("OPAD_TEST_MESH_HEAVY"); !e || !*e) return;
  const mo::Mesh a = sphere_mesh(50, 0.0004), b = [] {
    mo::Mesh m = sphere_mesh(50, 0.0004);
    for (size_t v = 0; v < m.vertices(); ++v) m.xyz[v * 3] += 40;
    return m;
  }();
  std::printf("  meshes: %zu and %zu triangles\n", a.triangles(), b.triangles());
  CHECK(a.triangles() > 1000000);
  const auto t0 = std::chrono::steady_clock::now();
  const mo::Mesh u = mo::boolean(a, b, mo::Op::Union);
  const auto t1 = std::chrono::steady_clock::now();
  const mo::Cut c = mo::split_by_plane(u, {20, 0, 0}, {1, 0, 0});
  const auto t2 = std::chrono::steady_clock::now();
  const mo::Report r = mo::check(u);
  const auto t3 = std::chrono::steady_clock::now();
  auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
  std::printf("  union %.0f ms (%zu triangles), split %.0f ms, check %.0f ms\n", ms(t0, t1), u.triangles(), ms(t1, t2), ms(t2, t3));
  CHECK(r.printable() && c.capped);
  CHECK(ms(t0, t1) < 30000);
}

CHECK_MAIN()
