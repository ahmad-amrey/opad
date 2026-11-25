// Flow studies (sim/flow.cpp, study kind "flow"): the settings read and refused, the fluid found between a part's openings,
// a round fitting meshed as a wedge (its profile from rays) and anything else as the full part, the OpenFOAM case written.
// None of that needs OpenFOAM. The solves (a generated 8" to 6" fitting at 350 CFM, both ways, against the handbook) run as
// ctest flow-solve (label slow, OPAD_TEST_FLOW set) and are skipped when OpenFOAM is missing.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "opad/core.hpp"
#include "opad/geometry.hpp"
#include "opad/sim/cfd.hpp"
#include "opad/sim/fea.hpp"
#include "opad/sim/flow.hpp"
#include "opad/sim/joints.hpp"

using namespace opad;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kIn = 25.4;

std::string add_shape(Document& doc, const TopoDS_Shape& shape, const std::string& name) {
  std::ostringstream s;
  BRepTools::Write(shape, s);
  import_brep(doc, s.str(), name);
  const Scene sc = resolve(doc);
  for (const auto& id : sc.all_bodies())
    if (sc.node(id)->name == name) return id;
  throw Error("no body " + name);
}

// A profile (r, z) revolved about Z: a closed polygon in the XZ plane.
TopoDS_Shape revolved(const std::vector<std::array<double, 2>>& rz) {
  BRepBuilderAPI_MakePolygon poly;
  for (const auto& p : rz) poly.Add(gp_Pnt(p[0], 0, p[1]));
  poly.Close();
  const TopoDS_Face face = BRepBuilderAPI_MakeFace(poly.Wire(), true).Face();
  return BRepPrimAPI_MakeRevol(face, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 2 * kPi).Shape();
}

// The inside of a fitting between an 8" and a 6" duct (inner diameters): a cylinder, a cone of `half_angle` degrees, a
// cylinder, `sleeve` mm each; as the profile of its bore (r of z), blended with arcs of radius `blend` where they meet.
std::vector<std::array<double, 2>> bore(double half_angle, double sleeve, double blend = 0) {
  const double r1 = 4 * kIn, r2 = 3 * kIn, t = std::tan(half_angle * kPi / 180);
  const double cone = (r1 - r2) / t;
  std::vector<std::array<double, 2>> out{{r1, 0}};  // (r, z)
  auto corner = [&](double z, double r, bool first) {
    // The arc tangent to both lines at a corner (z, r): it turns by the cone's angle.
    if (blend <= 0) {
      out.push_back({r, z});
      return;
    }
    const double a = half_angle * kPi / 180, d = blend * std::tan(a / 2);
    const int n = 12;
    // From the corner back along the line before by d, to forward along the line after by d.
    for (int k = 0; k <= n; ++k) {
      const double f = double(k) / n;
      // Points of the arc as the quadratic Bezier through the corner (close to the circle for small angles).
      const double z0 = first ? z - d : z - d * std::cos(a), r0 = first ? r : r + d * std::sin(a);
      const double z2 = first ? z + d * std::cos(a) : z + d, r2b = first ? r - d * std::sin(a) : r;
      const double zz = (1 - f) * (1 - f) * z0 + 2 * f * (1 - f) * z + f * f * z2, rr = (1 - f) * (1 - f) * r0 + 2 * f * (1 - f) * r + f * f * r2b;
      out.push_back({rr, zz});
    }
  };
  corner(sleeve, r1, true);
  corner(sleeve + cone, r2, false);
  out.push_back({r2, 2 * sleeve + cone});
  return out;
}

// The fitting as a wall 2.4 mm thick (its ends: rims with the opening as a hole), or as the fluid itself (its ends: discs).
TopoDS_Shape fitting_wall(const std::vector<std::array<double, 2>>& b) {
  std::vector<std::array<double, 2>> rz = b;
  for (auto it = b.rbegin(); it != b.rend(); ++it) rz.push_back({(*it)[0] + 2.4, (*it)[1]});
  return revolved(rz);
}
TopoDS_Shape fitting_volume(const std::vector<std::array<double, 2>>& b) {
  std::vector<std::array<double, 2>> rz{{0, b.front()[1]}};
  for (const auto& p : b) rz.push_back(p);
  rz.push_back({0, b.back()[1]});
  return revolved(rz);
}

// The flat face whose middle is at height z (a fitting's end).
std::string end_face(const Document& doc, const std::string& body, double z) {
  const Scene sc = resolve(doc);
  const TopoDS_Shape s = node_world_shape(doc, sc, body);
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(s, TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    const TopoDS_Face f = TopoDS::Face(faces(i));
    if (BRepAdaptor_Surface(f).GetType() != GeomAbs_Plane) continue;
    GProp_GProps g;
    BRepGProp::SurfaceProperties(f, g);
    if (std::fabs(g.CentreOfMass().Z() - z) < 1e-6) return body + "/face/" + std::to_string(i - 1);
  }
  throw Error("no flat face at z " + std::to_string(z));
}

struct Fitting {
  Document doc = Document::create();
  std::string body, big, small;  // the 8" end's face, the 6" end's
  double length = 0;
};
Fitting fitting(double half_angle, bool wall = true, double blend = 0, double sleeve = 60) {
  Fitting f;
  const auto b = bore(half_angle, sleeve, blend);
  f.length = b.back()[1];
  f.body = add_shape(f.doc, wall ? fitting_wall(b) : fitting_volume(b), "Fitting");
  f.big = end_face(f.doc, f.body, 0);
  f.small = end_face(f.doc, f.body, f.length);
  return f;
}

// References to faces of a body that need not exist (the settings alone are read).
const std::string kA0 = "0f0e0d0c-0b0a-4908-8706-050403020100/face/0", kA1 = "0f0e0d0c-0b0a-4908-8706-050403020100/face/1",
                  kE0 = "0f0e0d0c-0b0a-4908-8706-050403020100/edge/0";

json cfm(double v) { return {{"value", v}, {"unit", "cfm"}}; }

double g_seconds = 0;  // the last flow()'s wall time, printed by show()
sim::StudyRun flow(Fitting& f, json settings) {
  const auto t0 = std::chrono::steady_clock::now();
  sim::StudyRun r = sim::run_study(f.doc, resolve(f.doc), {{"kind", "flow"}, {"settings", settings}});
  g_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return r;
}

std::string text(const std::filesystem::path& p) { return read_text_file(p); }

// The fvSchemes a fitting's case is written with.
std::string text_of_case(Fitting& f, const sim::FlowSettings& s) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-test-flow-" + new_uuid().substr(0, 8));
  const Scene sc = resolve(f.doc);
  sim::write_flow_case(f.doc, sc, s, sim::plan_flow(f.doc, sc, s), dir);
  const std::string out = text(dir / "system" / "fvSchemes");
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return out;
}

}  // namespace

TEST(flow_settings_read) {
  const auto s = sim::flow_settings({{"inlet", kA0}, {"outlet", json::array({kA1})}, {"flow", cfm(350)}});
  CHECK_NEAR(s.flow_m3s, 350 * 4.719474e-4, 1e-6);
  CHECK(s.drive == sim::FlowSettings::Drive::Flow);
  CHECK_EQ(s.fluid, std::string("air"));
  CHECK_NEAR(s.ambient, 20, 1e-12);
  CHECK_NEAR(s.rho, 1.204, 0.01);
  CHECK_NEAR(s.nu, 1.51e-5, 0.03e-5);
  CHECK_EQ(s.inlet.size(), size_t(1));
  CHECK_EQ(s.inlet[0].index, 0);
  CHECK_NEAR(sim::flow_settings({{"inlet", kA0}, {"outlet", kA1}, {"flow", {{"value", 360}, {"unit", "m3/h"}}}}).flow_m3s, 0.1, 1e-12);
  CHECK_NEAR(sim::flow_settings({{"inlet", kA0}, {"outlet", kA1}, {"flow", {{"value", 12}, {"unit", "l/s"}}}}).flow_m3s, 0.012, 1e-12);
  // Water at 20 degC: 998 kg/m3, 1.0e-6 m2/s.
  const auto w = sim::flow_settings({{"inlet", kA0}, {"outlet", kA1}, {"flow", cfm(1)}, {"fluid", "water"}});
  CHECK_NEAR(w.rho, 998.2, 0.5);
  CHECK_NEAR(w.nu, 1.0e-6, 0.02e-6);
  // A fan: its curve in its units, or its two ends.
  const auto c = sim::flow_settings({{"inlet", kA0}, {"outlet", kA1},
                                     {"fan", {{"curve", {{0, 250}, {200, 150}, {400, 0}}}, {"unit", "cfm"}, {"pressure_unit", "Pa"}}}});
  CHECK(c.drive == sim::FlowSettings::Drive::Fan);
  CHECK_NEAR(c.fan.Qmax, 400 * 4.719474e-4, 1e-6);
  CHECK_NEAR(c.fan.pressure(0), 250, 1e-9);
  CHECK_NEAR(c.fan.pressure(200 * 4.719474e-4), 150, 1e-6);
  const auto e = sim::flow_settings({{"inlet", kA0}, {"outlet", kA1}, {"fan", {{"free_flow", 720}, {"shutoff_pressure", 1}, {"pressure_unit", "inH2O"}}}});
  CHECK_NEAR(e.fan.Qmax, 0.2, 1e-9);
  CHECK_NEAR(e.fan.pressure(0), 249.0889, 0.01);
  const auto a = sim::flow_settings({{"domain", "around"}, {"direction", {0, 1, 0}}, {"speed", {{"value", 36}, {"unit", "km/h"}}}});
  CHECK_NEAR(a.speed_ms, 10, 1e-9);
  CHECK(a.drive == sim::FlowSettings::Drive::Speed);
}

// The settings refused, saying why: `needle` in the message.
bool says(const json& settings, const std::string& needle) {
  std::string message;
  try {
    sim::flow_settings(settings);
  } catch (const std::exception& e) {
    message = e.what();
  }
  if (message.empty() || message.find(needle) == std::string::npos) {
    std::printf("  refused with \"%s\", wanted \"%s\"\n", message.c_str(), needle.c_str());
    return false;
  }
  return true;
}

TEST(flow_settings_refused) {
  const json ends = {{"inlet", kA0}, {"outlet", kA1}};
  bool ok = true;
  auto with = [&](json add) {
    json j = ends;
    for (const auto& [k, v] : add.items()) j[k] = v;
    return j;
  };
  ok &= says(json::array(), "is an object");
  ok &= says({{"outlet", kA1}, {"flow", cfm(1)}}, "give inlet and outlet");                // no inlet
  ok &= says(ends, "give flow");                                                       // neither flow nor fan
  ok &= says(with({{"flow", cfm(1)}, {"fan", {{"free_flow", 1}, {"shutoff_pressure", 1}}}}), "give flow");  // both
  ok &= says(with({{"flow", {{"value", 1}, {"unit", "gallons"}}}}), "not \"gallons\"");
  ok &= says(with({{"flow", {{"value", -1}, {"unit", "cfm"}}}}), "above 0");
  ok &= says(with({{"flow", 350}}), "flow is {value");                                     // a bare number: which unit?
  ok &= says(with({{"flow", cfm(1)}, {"fluid", "oil"}}), "fluid is air or water");
  ok &= says(with({{"flow", cfm(1)}, {"quality", "best"}}), "quality is quick");
  ok &= says(with({{"flow", cfm(1)}, {"mesh", "2d"}}), "mesh is auto");
  ok &= says(with({{"flow", cfm(1)}, {"fluid", "water"}, {"ambient", 120}}), "water between");
  ok &= says(with({{"flow", cfm(1)}, {"extend", {{"inlet", -1}}}}), "extend is");
  ok &= says(with({{"flow", cfm(1)}, {"iterations", 5}}), "iterations is");
  ok &= says(with({{"flow", cfm(1)}, {"velocity", 3}}), "unknown setting \"velocity\"");                // unknown key
  ok &= says(with({{"flow", cfm(1)}, {"speed", 3}}), "speed is for a part in a stream");                   // around's
  ok &= says({{"inlet", kA0}, {"outlet", kA0}, {"flow", cfm(1)}}, "the same face");  // the same face
  ok &= says({{"inlet", kE0}, {"outlet", kA1}, {"flow", cfm(1)}}, "not an edge");  // not a face
  ok &= says(with({{"fan", {{"curve", {{0, 100}, {0, 50}}}}}}), "larger flow than the one before");       // flow not rising along the curve
  ok &= says(with({{"fan", {{"free_flow", 100}}}}), "fan is {curve");                    // no shut-off pressure
  ok &= says(with({{"fan", {{"free_flow", 100}, {"shutoff_pressure", 50}, {"pressure_unit", "bar"}}}}), "not \"bar\"");
  ok &= says({{"domain", "around"}, {"speed", 3}}, "needs direction");                     // no direction
  ok &= says({{"domain", "around"}, {"direction", {0, 0, 0}}, {"speed", 3}}, "direction has no length");
  ok &= says({{"domain", "around"}, {"direction", {1, 0, 0}}, {"speed", 3}, {"inlet", kA0}}, "inlet is for the inside");
  ok &= says({{"domain", "outside"}}, "domain is inside");
  CHECK(ok);
}

TEST(flow_kind_is_registered) {
  const auto& kinds = sim::study_kinds();
  CHECK(std::find(kinds.begin(), kinds.end(), "flow") != kinds.end());
  const json e = sim::engines();
  CHECK(e.contains("flow"));
  CHECK_EQ(e["flow"].get<bool>(), sim::openfoam().found());
  if (!sim::openfoam().found()) {
    // Said before anything is written, in the words engines() gives.
    CHECK(e["unavailable"]["flow"].get<std::string>().find("OpenFOAM") != std::string::npos);
    Fitting f = fitting(35);
    const size_t ops = f.doc.ops.size();
    CHECK_THROWS(commands::run("study", {{"kind", "flow"}, {"settings", {{"inlet", f.big}, {"outlet", f.small}, {"flow", cfm(350)}}}}, &f.doc));
    CHECK_EQ(f.doc.ops.size(), ops);
    // run: false defines it with the reason as a warning.
    const json r = commands::run("study", {{"kind", "flow"}, {"run", false}, {"settings", {{"inlet", f.big}, {"outlet", f.small}, {"flow", cfm(350)}}}}, &f.doc);
    CHECK(r.value("warning", std::string()).find("OpenFOAM") != std::string::npos);
    CHECK_EQ(r["plan"]["mesh"].get<std::string>(), std::string("axisymmetric"));
    // Settings it cannot run are refused even then, before anything is written.
    const size_t defined = f.doc.ops.size();
    CHECK_THROWS(commands::run("study", {{"kind", "flow"}, {"run", false}, {"settings", {{"inlet", f.big}, {"outlet", f.big}, {"flow", cfm(350)}}}}, &f.doc));
    CHECK_THROWS(commands::run("study", {{"id", r["id"]}, {"run", false}, {"settings", {{"fluid", "oil"}}}}, &f.doc));
    CHECK_EQ(f.doc.ops.size(), defined);
    // A change it can run is kept, and planned again: the other way round.
    const json back = commands::run("study", {{"id", r["id"]}, {"run", false}, {"settings", {{"inlet", f.small}, {"outlet", f.big}}}}, &f.doc);
    CHECK_NEAR(back["plan"]["axis"][2].get<double>(), -1, 1e-9);
    // The study's own refusals come first: a bad setting is named even without OpenFOAM.
    try {
      flow(f, {{"inlet", f.big}, {"outlet", f.small}});
      CHECK(false);
    } catch (const Error& err) {
      CHECK(std::string(err.what()).find("give flow") != std::string::npos);
    }
  }
}

// OPAD_OPENFOAM naming OpenCFD's Windows installer root or the OpenFOAM-v<N> folder in it: the programs are in
// platforms/<build>/bin (bin/ holds shell scripts only), and a Pstream built against MS-MPI without msmpi.dll is a problem.
TEST(flow_openfoam_installer_layout) {
  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / ("opad-test-foam-" + new_uuid().substr(0, 8));
  const fs::path project = root / "msys64" / "home" / "ofuser" / "OpenFOAM" / "OpenFOAM-v2512";
  const fs::path bin = project / "platforms" / "win64MingwDPInt32Opt" / "bin";
  fs::create_directories(bin);
  fs::create_directories(project / "bin");
  fs::create_directories(project / "etc");
  auto touch = [](const fs::path& p, const std::string& text = "x") { std::ofstream(p, std::ios::binary) << text; };
  touch(bin / "simpleFoam.exe");
  touch(project / "bin" / "foamEtcFile");
  touch(project / "etc" / "openfoam");
  touch(project / "etc" / "controlDict");
  const char* old = std::getenv("OPAD_OPENFOAM");
  const std::string saved = old ? old : "";
  auto use = [](const fs::path& p) {
#ifdef _WIN32
    _putenv_s("OPAD_OPENFOAM", path_to_utf8(p).c_str());
#else
    setenv("OPAD_OPENFOAM", path_to_utf8(p).c_str(), 1);
#endif
  };
  for (const fs::path& given : {root, project}) {
    use(given);
    const auto f = sim::openfoam();
    CHECK(f.bin == bin);
    CHECK(f.project == project);
#ifdef _WIN32
    CHECK(f.wrapper.empty());  // a shell script: never run from Windows
    CHECK(f.found());
#endif
  }
#ifdef _WIN32
  touch(bin / "libPstream.dll", std::string("MZ\0\0...MSMPI.DLL\0...", 22));
  fs::create_directories(root / "thirdParty");
  touch(root / "thirdParty" / "msmpisetup.exe");
  use(root);
  const auto f = sim::openfoam();
  // A machine with MS-MPI installed has no problem; one without is told to run the installer's own MS-MPI setup.
  if (!f.problem.empty()) {
    CHECK(!f.found());
    CHECK(f.problem.find("msmpisetup.exe") != std::string::npos);
  }
#endif
  use(saved);
  std::error_code ec;
  fs::remove_all(root, ec);
}

TEST(flow_round_fitting_is_a_wedge) {
  Fitting f = fitting(35);
  const Scene sc = resolve(f.doc);
  const auto s = sim::flow_settings({{"inlet", f.big}, {"outlet", f.small}, {"flow", cfm(350)}});
  const sim::FlowPlan p = sim::plan_flow(f.doc, sc, s);
  CHECK_EQ(p.mesh, std::string("axisymmetric"));
  CHECK_EQ(p.mode, std::string("wall"));
  CHECK(p.why_3d.empty());
  CHECK_NEAR(p.axis[2], 1, 1e-9);  // from the 8" end into the fitting
  CHECK_NEAR(p.length_mm, f.length, 1e-6);
  CHECK_NEAR(p.inlet.radius_mm, 4 * kIn, 1e-3);
  CHECK_NEAR(p.outlet.radius_mm, 3 * kIn, 1e-3);
  CHECK_NEAR(p.inlet.diameter_mm, 8 * kIn, 0.01);
  CHECK_NEAR(p.inlet.area_mm2, kPi * std::pow(4 * kIn, 2), 1.0);
  // The bore from rays: its corners kept, the straight runs between them dropped.
  CHECK(p.profile.size() >= 4 && p.profile.size() <= 6);
  auto r_at = [&](double z) {
    for (size_t i = 1; i < p.profile.size(); ++i)
      if (z <= p.profile[i][0]) {
        const double f2 = (z - p.profile[i - 1][0]) / (p.profile[i][0] - p.profile[i - 1][0]);
        return p.profile[i - 1][1] + f2 * (p.profile[i][1] - p.profile[i - 1][1]);
      }
    return p.profile.back()[1];
  };
  CHECK_NEAR(r_at(30), 4 * kIn, 0.05);
  CHECK_NEAR(r_at(f.length - 30), 3 * kIn, 0.05);
  const double cone = kIn / std::tan(35 * kPi / 180);
  CHECK_NEAR(r_at(60 + cone / 2), 3.5 * kIn, 0.3);
  // Stubs: 2 diameters before, 6 after.
  CHECK_NEAR(p.steepest_deg, 35, 0.5);
  CHECK(text_of_case(f, s).find("limited corrected 0.33") != std::string::npos);  // skewed past 30 degrees: corrected
  CHECK_NEAR(p.inlet_extension_mm, 2 * 8 * kIn, 0.1);
  CHECK_NEAR(p.outlet_extension_mm, 12 * 6 * kIn, 0.1);
  CHECK(p.cells > 3000 && p.cells < 60000);
  // The other way round: an expansion, along -Z.
  const auto back = sim::plan_flow(f.doc, sc, sim::flow_settings({{"inlet", f.small}, {"outlet", f.big}, {"flow", cfm(350)}}));
  CHECK_EQ(back.mesh, std::string("axisymmetric"));
  CHECK_NEAR(back.axis[2], -1, 1e-9);
  CHECK_NEAR(back.profile.front()[1], 3 * kIn, 1e-3);
  CHECK_NEAR(back.profile.back()[1], 4 * kIn, 1e-3);
  // Asked for the full part, it gets it.
  CHECK_EQ(sim::plan_flow(f.doc, sc, sim::flow_settings({{"inlet", f.big}, {"outlet", f.small}, {"flow", cfm(350)}, {"mesh", "3d"}})).mesh, std::string("3d"));
}

TEST(flow_sudden_step_is_3d) {
  // An 8" bore stepping straight down to 6" (a 90 degree cone): no wedge of single blocks holds it.
  Fitting f = fitting(89.99);
  const auto s = sim::flow_settings({{"inlet", f.big}, {"outlet", f.small}, {"flow", cfm(350)}});
  const auto p = sim::plan_flow(f.doc, resolve(f.doc), s);
  CHECK_EQ(p.mesh, std::string("3d"));
  CHECK(p.why_3d.find("sudden step") != std::string::npos);
  std::printf("  %s\n", p.why_3d.c_str());
  const size_t along = p.why_3d.rfind(") ");
  CHECK(along != std::string::npos);
  CHECK_NEAR(std::atof(p.why_3d.c_str() + along + 2), 60, 0.5);  // where the step is
  // A gentle cone keeps the plain schemes.
  Fitting g = fitting(12);
  const auto q = sim::flow_settings({{"inlet", g.big}, {"outlet", g.small}, {"flow", cfm(350)}});
  CHECK_NEAR(sim::plan_flow(g.doc, resolve(g.doc), q).steepest_deg, 12, 0.3);
  CHECK(text_of_case(g, q).find("limited") == std::string::npos);
}

TEST(flow_fluid_body_is_a_wedge) {
  Fitting f = fitting(12, false, 60);
  const auto p = sim::plan_flow(f.doc, resolve(f.doc), sim::flow_settings({{"inlet", f.big}, {"outlet", f.small}, {"fan", {{"free_flow", 500}, {"shutoff_pressure", 200}, {"unit", "cfm"}}}}));
  CHECK_EQ(p.mesh, std::string("axisymmetric"));
  CHECK_EQ(p.mode, std::string("volume"));
  CHECK_NEAR(p.inlet.radius_mm, 4 * kIn, 1e-3);
  CHECK(p.profile.size() > 6);  // the blends' arcs keep points
}

// Planning refused, saying why.
bool plan_says(const Document& doc, const Scene& sc, const json& settings, const std::string& needle) {
  std::string message;
  try {
    sim::plan_flow(doc, sc, sim::flow_settings(settings));
  } catch (const std::exception& e) {
    message = e.what();
  }
  if (message.empty() || message.find(needle) == std::string::npos) {
    std::printf("  refused with \"%s\", wanted \"%s\"\n", message.c_str(), needle.c_str());
    return false;
  }
  return true;
}

TEST(flow_not_round_is_3d) {
  // A rectangular duct: a box with a box cut through it along Z.
  Document doc = Document::create();
  const TopoDS_Shape outer = BRepPrimAPI_MakeBox(gp_Pnt(-30, -20, 0), 60, 40, 100).Shape();
  const TopoDS_Shape hole = BRepPrimAPI_MakeBox(gp_Pnt(-27, -17, -1), 54, 34, 102).Shape();
  const std::string duct = add_shape(doc, BRepAlgoAPI_Cut(outer, hole).Shape(), "Duct");
  const auto s = sim::flow_settings({{"inlet", end_face(doc, duct, 0)}, {"outlet", end_face(doc, duct, 100)}, {"flow", {{"value", 50}, {"unit", "m3/h"}}}});
  const auto p = sim::plan_flow(doc, resolve(doc), s);
  CHECK_EQ(p.mesh, std::string("3d"));
  CHECK(p.why_3d.find("not round") != std::string::npos);
  CHECK_NEAR(p.inlet.area_mm2, 54 * 34, 0.5);
  CHECK_NEAR(p.inlet.diameter_mm, 4.0 * 54 * 34 / (2 * (54 + 34)), 0.01);
  CHECK_NEAR(p.length_mm, 100, 1e-6);
  json asked = {{"inlet", end_face(doc, duct, 0)}, {"outlet", end_face(doc, duct, 100)}, {"flow", {{"value", 50}, {"unit", "m3/h"}}}, {"mesh", "axisymmetric"}};
  CHECK(plan_says(doc, resolve(doc), asked, "mesh axisymmetric: the inlet is not round"));
  // Two round openings off one another's axis: the full part, saying why.
  Document off = Document::create();
  const TopoDS_Shape a = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 20, 50).Shape();
  const TopoDS_Shape b = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(8, 0, 49), gp_Dir(0, 0, 1)), 15, 50).Shape();
  const std::string body = add_shape(off, BRepAlgoAPI_Fuse(a, b).Shape(), "Offset");
  const auto q = sim::plan_flow(off, resolve(off), sim::flow_settings({{"inlet", end_face(off, body, 0)}, {"outlet", end_face(off, body, 99)}, {"flow", cfm(10)}}));
  CHECK_EQ(q.mesh, std::string("3d"));
  CHECK_EQ(q.mode, std::string("volume"));
  CHECK(q.why_3d.find("off the inlet's axis") != std::string::npos);
}

TEST(flow_openings_refused) {
  Fitting f = fitting(35);
  const Scene sc = resolve(f.doc);
  // A face that is not flat: the bore's cylinder.
  std::string curved;
  {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(node_world_shape(f.doc, sc, f.body), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent() && curved.empty(); ++i)
      if (BRepAdaptor_Surface(TopoDS::Face(faces(i))).GetType() != GeomAbs_Plane) curved = f.body + "/face/" + std::to_string(i - 1);
  }
  CHECK(plan_says(f.doc, sc, {{"inlet", curved}, {"outlet", f.small}, {"flow", cfm(1)}}, "must be a flat face"));
  CHECK(plan_says(f.doc, sc, {{"inlet", f.body + "/face/999"}, {"outlet", f.small}, {"flow", cfm(1)}}, "out of range"));
  CHECK(plan_says(f.doc, sc, {{"inlet", "0f0e0d0c-0b0a-4908-8706-0504030201ff/face/0"}, {"outlet", f.small}, {"flow", cfm(1)}}, "names no body"));
  // A rim and a disc: one end of a wall, the other of a fluid body.
  const std::string disc = add_shape(f.doc, fitting_volume(bore(35, 60)), "Fluid");
  const Scene sc2 = resolve(f.doc);
  CHECK(plan_says(f.doc, sc2, {{"inlet", f.big}, {"outlet", end_face(f.doc, disc, f.length)}, {"flow", cfm(1)}}, "pick both ends the same way"));
}

TEST(flow_case_written) {
  Fitting f = fitting(35);
  const Scene sc = resolve(f.doc);
  const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-test-flow-" + new_uuid().substr(0, 8));
  {
    const auto s = sim::flow_settings({{"inlet", f.big}, {"outlet", f.small}, {"flow", cfm(350)}});
    const auto p = sim::plan_flow(f.doc, sc, s);
    sim::write_flow_case(f.doc, sc, s, p, dir / "wedge");
    const std::string bm = text(dir / "wedge" / "system" / "blockMeshDict");
    CHECK(bm.find("type wedge") != std::string::npos);
    CHECK(bm.find("inlet { type patch; faces ((0 0 2 1)); }") != std::string::npos);
    // One block per stretch of the profile and each stub; three vertices per station (the axis, both sides of the wedge).
    const size_t blocks = p.profile.size() + 1;
    size_t hexes = 0;
    for (size_t at = bm.find("hex ("); at != std::string::npos; at = bm.find("hex (", at + 1)) ++hexes;
    CHECK_EQ(hexes, blocks);
    // The wedge carries 5/360 of the flow.
    const std::string U = text(dir / "wedge" / "0" / "U");
    CHECK(U.find("flowRateInletVelocity") != std::string::npos);
    const size_t q = U.find("volumetricFlowRate constant ");
    CHECK(q != std::string::npos);
    CHECK_NEAR(std::atof(U.c_str() + q + 28), 350 * 4.719474e-4 * 5 / 360, 1e-7);
    CHECK(text(dir / "wedge" / "constant" / "momentumTransport").find("kOmegaSST") != std::string::npos);
    CHECK(text(dir / "wedge" / "0" / "nut").find("nutUSpaldingWallFunction") != std::string::npos);
    CHECK(text(dir / "wedge" / "system" / "fvSchemes").find("wallDist") != std::string::npos);
    CHECK(!std::filesystem::exists(dir / "wedge" / "system" / "snappyHexMeshDict"));
  }
  {
    // The full part with a fan: the walls, the stubs' far ends as the openings, the fan's curve at the inlet.
    const auto s = sim::flow_settings({{"inlet", f.big}, {"outlet", f.small}, {"mesh", "3d"}, {"quality", "quick"},
                                       {"fan", {{"curve", {{0, 300}, {300, 200}, {600, 0}}}, {"unit", "cfm"}}}});
    const auto p = sim::plan_flow(f.doc, sc, s);
    sim::write_flow_case(f.doc, sc, s, p, dir / "full");
    CHECK(text(dir / "full" / "system" / "snappyHexMeshDict").find("locationInMesh") != std::string::npos);
    for (const char* stl : {"wall.stl", "inlet.stl", "outlet.stl"}) CHECK(std::filesystem::file_size(dir / "full" / "constant" / "triSurface" / stl) > 200);
    // The curve inline (both OpenFOAMs read a Function1 table), flow in m3/s and kinematic pressure: 300 Pa at no flow.
    const std::string p0 = text(dir / "full" / "0" / "p");
    CHECK(p0.find("fanPressure; fanCurve table ( (0 ") != std::string::npos);
    CHECK(p0.find("file") == std::string::npos);
    CHECK_NEAR(std::atof(p0.c_str() + p0.find("table ( (0 ") + 11), 300 / s.rho, 1e-6);
    // Five writes, the last at the end.
    const std::string control = text(dir / "full" / "system" / "controlDict");
    const int end = std::atoi(control.c_str() + control.find("endTime ") + 8), every = std::atoi(control.c_str() + control.find("writeInterval ") + 14);
    CHECK(every > 0 && end % every == 0 && end >= p.iterations);
    // The inlet's stub reaches 2 diameters out: the inlet's STL lies there (z = -406.4 mm).
    CHECK(text(dir / "full" / "constant" / "triSurface" / "inlet.stl").find("-0.4064") != std::string::npos);
  }
  {
    // Laminar at a trickle (Re below 2000), said by the case.
    const auto s = sim::flow_settings({{"inlet", f.big}, {"outlet", f.small}, {"flow", {{"value", 0.5}, {"unit", "m3/h"}}}});
    sim::write_flow_case(f.doc, sc, s, sim::plan_flow(f.doc, sc, s), dir / "slow");
    CHECK(text(dir / "slow" / "constant" / "momentumTransport").find("laminar") != std::string::npos);
  }
  if (std::getenv("OPAD_KEEP_CFD")) {  // to look at the files
    std::printf("  case files in %s\n", path_to_utf8(dir).c_str());
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------- the solves (ctest flow-solve)
namespace {
bool solves() { return sim::openfoam().found() && std::getenv("OPAD_TEST_FLOW"); }
void show(const char* what, const sim::StudyRun& r) {
  const json& s = r.summary;
  std::printf("%s: dp %.2f Pa (faces %.2f, static %.2f), K %.3f, Q %.4f m3/s, vmax %.2f m/s, %zu recirculation, %s, %d cells, %d iterations%s, %.0f s\n", what,
              s["pressure_drop_pa"].get<double>(), s["pressure_drop_faces_pa"].get<double>(), s["static_drop_pa"].get<double>(), s["loss_coefficient"].get<double>(),
              s["flow_m3s"].get<double>(), s["max_speed_ms"].get<double>(), s["recirculation"].size(), s["mesh"].get<std::string>().c_str(), s["cells"].get<int>(),
              s["iterations"].get<int>(), s["converged"].get<bool>() ? "" : " (not converged)", g_seconds);
  for (const auto& w : s["warnings"]) std::printf("  warning: %s\n", w.get<std::string>().c_str());
}
}  // namespace

// Handbook (ASHRAE / Idelchik) for a 35 degree (half angle) cone between 8" and 6" at 350 CFM: about 5-7 Pa as a contraction,
// about 13 Pa as an expansion. k-omega SST gives less, the same on the fine mesh (OpenFOAM v2512, 12 diameters of outlet
// stub): about 2 Pa and 9.5 Pa (the sudden expansion's Borda-Carnot loss is 9.5 Pa). The bounds are wide: the comparison
// itself is printed. A wedge converges in a few hundred iterations (U's component across it is left out of residualControl).
TEST(flow_solve_cone_35) {
  if (!solves()) return;
  Fitting f = fitting(35);
  const sim::StudyRun in = flow(f, {{"inlet", f.big}, {"outlet", f.small}, {"flow", cfm(350)}});
  show("35 deg contraction", in);
  CHECK_EQ(in.summary["mesh"].get<std::string>(), std::string("axisymmetric"));
  CHECK_NEAR(in.summary["flow_m3s"].get<double>(), 350 * 4.719474e-4, 0.01 * 350 * 4.719474e-4);
  CHECK(in.summary["pressure_drop_pa"].get<double>() > 1 && in.summary["pressure_drop_pa"].get<double>() < 12);
  CHECK(in.summary["converged"].get<bool>());
  CHECK(!in.fea->streamlines.empty());
  CHECK(!in.fea->cut.empty());
  CHECK(!in.series.empty() && !in.t.empty());
  const sim::StudyRun out = flow(f, {{"inlet", f.small}, {"outlet", f.big}, {"flow", cfm(350)}});
  show("35 deg expansion", out);
  CHECK(out.summary["pressure_drop_pa"].get<double>() > 7 && out.summary["pressure_drop_pa"].get<double>() < 22);
  // The jet leaves the wall at the cone: a ring of air turning back along it.
  CHECK(!out.summary["recirculation"].empty());
  CHECK(out.summary["pressure_drop_pa"].get<double>() > in.summary["pressure_drop_pa"].get<double>());
  // A fan whose curve passes 350 CFM at the expansion's pressure drop runs near there.
  const double dp = out.summary["pressure_drop_pa"].get<double>() + 0.5 * 1.204 * std::pow(350 * 4.719474e-4 / (kPi * std::pow(4 * kIn * 1e-3, 2)), 2);
  const sim::StudyRun fan = flow(f, {{"inlet", f.small}, {"outlet", f.big}, {"fan", {{"free_flow", 700}, {"shutoff_pressure", 2 * dp}, {"unit", "cfm"}}}});
  show("35 deg expansion, fan", fan);
  CHECK(fan.summary.contains("fan"));
  CHECK(fan.summary["flow_m3s"].get<double>() > 0.5 * 350 * 4.719474e-4);
}

// 12 degrees with R60 blends: about 3 Pa as a contraction, 5-6 Pa as an expansion (solved: about 1.2 and 5 Pa).
TEST(flow_solve_cone_12_blended) {
  if (!solves()) return;
  Fitting f = fitting(12, true, 60);
  const sim::StudyRun in = flow(f, {{"inlet", f.big}, {"outlet", f.small}, {"flow", cfm(350)}});
  show("12 deg R60 contraction", in);
  const sim::StudyRun out = flow(f, {{"inlet", f.small}, {"outlet", f.big}, {"flow", cfm(350)}});
  show("12 deg R60 expansion", out);
  CHECK(in.summary["pressure_drop_pa"].get<double>() < 8);
  CHECK(out.summary["pressure_drop_pa"].get<double>() < 14);
}

// The same fitting as the full part (snappyHexMesh): within a third of the wedge's answer.
TEST(flow_solve_cone_35_full) {
  if (!solves() || !std::getenv("OPAD_TEST_FLOW_3D")) return;
  Fitting f = fitting(35);
  const sim::StudyRun w = flow(f, {{"inlet", f.small}, {"outlet", f.big}, {"flow", cfm(350)}});
  const sim::StudyRun d = flow(f, {{"inlet", f.small}, {"outlet", f.big}, {"flow", cfm(350)}, {"mesh", "3d"}, {"quality", "quick"}});
  show("35 deg expansion, 3d", d);
  CHECK_NEAR(d.summary["pressure_drop_pa"].get<double>(), w.summary["pressure_drop_pa"].get<double>(), 0.35 * w.summary["pressure_drop_pa"].get<double>());
}

CHECK_MAIN()
