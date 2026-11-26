// Deviation (opad/deviation.hpp) on generated data with known answers: a box against the box grown by 0.5 mm, a noisy
// sampled sphere (a mesh, like a scan) against the exact sphere, a part with a missing chunk and an added bump (the sign),
// the best fit of a moved copy (two revisions), options (max_distance, region, sample) and an open reference.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRep_Builder.hxx>
#include <Poly_Triangulation.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <random>
#include <string>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/deviation.hpp"
#include "opad/geometry.hpp"

using namespace opad;

namespace {
std::string add_body(Document& d, Scene& scene, const std::string& key, const TopoDS_Shape& shape, const Mat4& local = Mat4()) {
  cache_shape(d, key, shape);
  Node n;
  n.id = new_uuid();
  n.kind = Node::Kind::Body;
  n.name = key;
  n.body_key = key;
  n.local = local;
  scene.nodes[n.id] = n;
  scene.roots.push_back(n.id);
  return n.id;
}

// A triangulation-only face (what an STL import makes) from welded triangles.
TopoDS_Shape mesh_of(const deviation::Triangles& t) {
  Handle(Poly_Triangulation) mesh = new Poly_Triangulation(int(t.vertex_count()), int(t.triangle_count()), false);
  for (size_t v = 0; v < t.vertex_count(); ++v) mesh->SetNode(int(v + 1), gp_Pnt(t.xyz[3 * v], t.xyz[3 * v + 1], t.xyz[3 * v + 2]));
  for (size_t i = 0; i < t.triangle_count(); ++i) mesh->SetTriangle(int(i + 1), Poly_Triangle(int(t.tri[3 * i] + 1), int(t.tri[3 * i + 1] + 1), int(t.tri[3 * i + 2] + 1)));
  TopoDS_Face face;
  BRep_Builder().MakeFace(face, mesh);
  return face;
}

bool about(double a, double b, double tol) { return std::abs(a - b) <= tol; }
double stat(const json& r, const char* key) { return r.at("stats").at(key).get<double>(); }
}  // namespace

// Every point of a 100 x 60 x 40 box lies 0.5 mm inside the box grown by 0.5 mm on every side: -0.5 everywhere (missing
// material). Measured back, the grown box lies +0.5 outside, more near its edges and corners (up to 0.5 * sqrt 3).
TEST(box_inside_offset_box) {
  Document d = Document::create();
  Scene scene;
  const std::string a = add_body(d, scene, "a", BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(100, 60, 40)).Shape());
  const std::string b = add_body(d, scene, "b", BRepPrimAPI_MakeBox(gp_Pnt(-0.5, -0.5, -0.5), gp_Pnt(100.5, 60.5, 40.5)).Shape());
  const json r = deviation::command(d, scene, {{"from", a}, {"to", b}, {"tolerance", 0.1}, {"sample", 20000}, {"both_ways", true}});
  CHECK(r["from"]["kind"] == "solid" && r["to"]["closed"] == true && r["sign_defined"] == true);
  CHECK(r["sampling"]["points"] == 20000 && r["sampling"]["source"] == "surface samples");
  CHECK(about(stat(r, "mean"), -0.5, 1e-3) && about(stat(r, "median"), -0.5, 1e-3) && about(stat(r, "max_abs"), 0.5, 1e-3));
  CHECK(about(stat(r, "rms"), 0.5, 1e-3) && stat(r, "inside_pct") == 100 && stat(r, "within_tolerance_pct") == 0);
  // Histogram: every point in the bin holding -0.5.
  const json& h = r["histogram"];
  size_t total = 0, most = 0;
  for (const auto& c : h["counts"]) total += c.get<size_t>(), most = std::max(most, c.get<size_t>());
  CHECK(total + h["below"].get<size_t>() + h["above"].get<size_t>() == 20000 && most == total && total == 20000);
  // Worst spots are spread over the box, each at -0.5.
  CHECK(r["worst"].size() >= 4);
  for (const auto& s : r["worst"]) CHECK(about(s["value"].get<double>(), -0.5, 1e-3));
  const json& back = r["reverse"];
  CHECK(about(back["stats"]["min"].get<double>(), 0.5, 1e-3) && back["stats"]["max"].get<double>() <= 0.5 * std::sqrt(3.0) + 1e-3);
  CHECK(back["stats"]["max"].get<double>() > 0.6 && back["stats"]["inside_pct"] == 0);
  // A small set is measured on the exact faces.
  const json exact = deviation::command(d, scene, {{"from", a}, {"to", b}, {"sample", 500}});
  CHECK(exact["sampling"]["exact"] == true && about(stat(exact, "mean"), -0.5, 1e-6) && about(stat(exact, "max_abs"), 0.5, 1e-6));
}

// A scan-like mesh: a sphere of radius 20 tessellated, each vertex moved along its radius by uniform noise in +-0.05 mm.
// Against the exact sphere every vertex reads its own noise: |d| <= 0.05, mean ~ 0, rms ~ 0.05 / sqrt 3.
TEST(noisy_sphere_mesh_against_exact_sphere) {
  Document d = Document::create();
  Scene scene;
  const TopoDS_Shape sphere = BRepPrimAPI_MakeSphere(gp_Pnt(0, 0, 0), 20).Shape();
  deviation::Triangles t = deviation::triangles_of(sphere, 0.004);
  std::mt19937 random(7);
  std::uniform_real_distribution<double> noise(-0.05, 0.05);
  std::vector<double> expected;
  for (size_t v = 0; v < t.vertex_count(); ++v) {
    gp_Vec p(t.xyz[3 * v], t.xyz[3 * v + 1], t.xyz[3 * v + 2]);
    const double e = noise(random);
    p.Normalize();
    p *= 20 + e;
    expected.push_back(e);
    t.xyz[3 * v] = float(p.X()), t.xyz[3 * v + 1] = float(p.Y()), t.xyz[3 * v + 2] = float(p.Z());
  }
  CHECK(t.vertex_count() > 3000);
  const std::string scan = add_body(d, scene, "scan", mesh_of(t));
  const std::string cad = add_body(d, scene, "cad", sphere);
  double mean = 0, sq = 0;
  for (double e : expected) mean += e, sq += e * e;
  mean /= double(expected.size());
  const double rms = std::sqrt(sq / double(expected.size()));
  const json r = deviation::command(d, scene, {{"from", scan}, {"to", cad}, {"tolerance", 0.03}, {"exact", true}});
  CHECK(r["from"]["kind"] == "mesh" && r["sampling"]["source"] == "mesh vertices" && r["sampling"]["subsampled"] == false);
  CHECK(r["sampling"]["points"].get<size_t>() == t.vertex_count() && r["sampling"]["exact"] == true);
  CHECK(about(stat(r, "mean"), mean, 2e-4) && about(stat(r, "rms"), rms, 2e-4) && stat(r, "max_abs") <= 0.0501);
  CHECK(about(stat(r, "within_tolerance_pct"), 60, 3) && about(stat(r, "outside_pct"), 20, 3) && about(stat(r, "inside_pct"), 20, 3));
  // On B's mesh alone (no exact faces) the chords of a finely meshed sphere add little.
  const json meshed = deviation::command(d, scene, {{"from", scan}, {"to", cad}, {"tolerance", 0.03}, {"exact", false}});
  CHECK(meshed["sampling"]["exact"] == false && about(stat(meshed, "rms"), rms, 3e-3));
  // Subsampled: an even share of the vertices, said so.
  const json some = deviation::command(d, scene, {{"from", scan}, {"to", cad}, {"tolerance", 0.03}, {"sample", 1000}});
  CHECK(some["sampling"]["points"] == 1000 && some["sampling"]["subsampled"] == true && some["sampling"]["vertices"] == t.vertex_count());
  // Both ways: the sphere sampled against the scan mesh reads the noise as well.
  const json both = deviation::command(d, scene, {{"from", scan}, {"to", cad}, {"tolerance", 0.03}, {"both_ways", true}, {"sample", 20000}});
  CHECK(both["reverse"]["stats"]["max_abs"].get<double>() < 0.06 && std::abs(both["reverse"]["stats"]["mean"].get<double>()) < 0.01);
}

// The sign: a "print" of a 100 x 60 x 40 block missing a 20 x 20 x 10 corner and carrying a 10 x 10 x 2 bump, measured
// against the design. The missing corner reads negative (down to -10, its floor's middle), the bump positive (+2), the
// rest zero; the worst spots find both places apart.
TEST(missing_chunk_and_bump_signs) {
  Document d = Document::create();
  Scene scene;
  const TopoDS_Shape block = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(100, 60, 40)).Shape();
  TopoDS_Shape print = BRepAlgoAPI_Cut(block, BRepPrimAPI_MakeBox(gp_Pnt(-1, -1, 30), gp_Pnt(20, 20, 41)).Shape()).Shape();
  print = BRepAlgoAPI_Fuse(print, BRepPrimAPI_MakeBox(gp_Pnt(60, 25, 40), gp_Pnt(70, 35, 42)).Shape()).Shape();
  const std::string printed = add_body(d, scene, "print", mesh_of(deviation::triangles_of(print, 0.01)));
  const std::string design = add_body(d, scene, "design", block);
  const json r = deviation::command(d, scene, {{"from", printed}, {"to", design}, {"tolerance", 0.2}, {"sample", 50000}});
  CHECK(r["sign_defined"] == true);
  CHECK(about(stat(r, "min"), -10, 0.05) && about(stat(r, "max"), 2, 1e-3) && stat(r, "inside_pct") > 0 && stat(r, "outside_pct") > 0);
  CHECK(about(stat(r, "median"), 0, 1e-4));
  bool missing = false, bump = false;
  for (const auto& s : r["worst"]) {
    const double v = s["value"].get<double>();
    const gp_Pnt at(s["position"][0].get<double>(), s["position"][1].get<double>(), s["position"][2].get<double>());
    if (v < -9 && at.X() < 21 && at.Y() < 21) missing = true;
    if (v > 1.9 && at.X() > 59 && at.X() < 71) bump = true;
  }
  CHECK(missing && bump);
  // As a solid from (sampled by area), the same signs.
  const std::string solid = add_body(d, scene, "print-solid", print);
  const json s = deviation::command(d, scene, {{"from", solid}, {"to", design}, {"tolerance", 0.2}, {"sample", 50000}});
  CHECK(s["from"]["kind"] == "solid" && about(stat(s, "min"), -10, 0.3) && about(stat(s, "max"), 2, 1e-3));
  // max_distance leaves the far points out, region keeps only a box around the bump.
  const json close = deviation::command(d, scene, {{"from", printed}, {"to", design}, {"tolerance", 0.2}, {"max_distance", 3}});
  CHECK(close["ignored"].get<size_t>() > 0 && stat(close, "min") >= -3);
  const json box = deviation::command(d, scene, {{"from", printed}, {"to", design}, {"region", {{"min", {55, 20, 39}}, {"max", {75, 40, 43}}}}});
  CHECK(box["sampling"]["in_region"].get<size_t>() < box["sampling"]["vertices"].get<size_t>() && stat(box, "min") >= -1e-6);
  // A face of the solid: its top (the bump's top lies outside, so the region of the block's top face has no positive).
  const json face = deviation::command(d, scene, {{"from", solid}, {"to", design}, {"region", {solid + "/face/0"}}, {"sample", 2000}});
  CHECK(face["stats"]["count"].get<size_t>() > 0);
}

// Two revisions: the design moved by (0.3, -0.2, 0.1) and turned 0.5 degrees. The best fit brings it back (rms ~ 0) and
// says what it did; the document is not changed.
TEST(best_fit_brings_a_moved_copy_back) {
  Document d = Document::create();
  Scene scene;
  TopoDS_Shape part = BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), gp_Pnt(80, 50, 30)).Shape(), BRepPrimAPI_MakeBox(gp_Pnt(10, 10, 20), gp_Pnt(30, 40, 31)).Shape()).Shape();
  gp_Trsf turn, shift;
  turn.SetRotation(gp_Ax1(gp_Pnt(40, 25, 15), gp_Dir(0.3, 0.2, 1)), 0.5 * M_PI / 180);
  shift.SetTranslation(gp_Vec(0.3, -0.2, 0.1));
  const std::string moved = add_body(d, scene, "moved", part, mat_from_trsf(shift * turn));
  const std::string design = add_body(d, scene, "design", part);
  const json plain = deviation::command(d, scene, {{"from", moved}, {"to", design}, {"tolerance", 0.01}, {"sample", 20000}});
  CHECK(stat(plain, "max_abs") > 0.2);
  const json r = deviation::command(d, scene, {{"from", moved}, {"to", design}, {"tolerance", 0.01}, {"sample", 20000}, {"align", true}});
  const json& fit = r["alignment"];
  CHECK(fit["rms_before"].get<double>() > 0.1 && fit["rms_after"].get<double>() < 1e-3);
  CHECK(about(fit["turned_deg"].get<double>(), 0.5, 0.01) && stat(r, "max_abs") < 0.01 && stat(r, "within_tolerance_pct") > 99);
}

// An open reference (a square sheet): the sign is undefined beyond its border, and the report says so.
TEST(open_reference_says_sign_undefined) {
  Document d = Document::create();
  Scene scene;
  const std::string sheet = add_body(d, scene, "sheet", BRepBuilderAPI_MakeFace(gp_Pln(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 0, 50, 0, 50).Face());
  const std::string block = add_body(d, scene, "block", BRepPrimAPI_MakeBox(gp_Pnt(40, 40, 1), gp_Pnt(70, 70, 5)).Shape());
  const json r = deviation::command(d, scene, {{"from", block}, {"to", sheet}, {"sample", 5000}});
  CHECK(r["to"]["closed"] == false && r["sign_defined"] == false && r.contains("sign_note") && r["sign_undefined"].get<size_t>() > 0);
  // Above the sheet (inside its border) the sign holds: positive.
  const json above = deviation::command(d, scene, {{"from", block}, {"to", sheet}, {"region", {{"min", {40, 40, 0}}, {"max", {49, 49, 6}}}}});
  CHECK(above["sign_undefined"] == 0 && stat(above, "min") > 0.9);
  // Bad input says what is wrong.
  bool refused = false;
  try {
    deviation::command(d, scene, {{"from", block}, {"to", block}});
  } catch (const Error&) {
    refused = true;
  }
  CHECK(refused);
}

int main(int argc, char** argv) { return check::run_all(argc, argv); }
