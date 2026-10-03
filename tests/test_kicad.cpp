// KiCad boards (.kicad_pcb) from synthetic files written here (never the user's boards: TC2030 is GPL-3): the outline
// from shuffled and reversed pieces in every form (lines, arcs old and new, curves, circles, rectangles, polygons), the
// drills (round, oval, offset, duplicated, across the edge), the thickness and mask colour, and the footprints' models
// placed as KiCad places them (top and bottom, turned, offset, rotated, scaled, legacy inches), found through the
// environment, ${KIPRJMOD}, a VRML name's STEP sibling and the user's folders, shared between footprints, with boxes
// for the ones that are missing.
#include <BRepBndLib.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

#include "check.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/kicad_pcb.hpp"

using namespace opad;

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Files {
  std::filesystem::path dir = std::filesystem::temp_directory_path() / ("opad-kicad-" + new_uuid());
  Files() { std::filesystem::create_directories(dir); }
  ~Files() { std::error_code e; std::filesystem::remove_all(dir, e); }
};

void write(const std::filesystem::path& p, const std::string& text) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

void step_box(const std::filesystem::path& p, double x0, double y0, double z0, double dx, double dy, double dz) {
  std::filesystem::create_directories(p.parent_path());
  STEPControl_Writer w;
  w.Transfer(BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), dx, dy, dz).Shape(), STEPControl_AsIs);
  const auto u8 = p.u8string();
  CHECK(w.Write(std::string(u8.begin(), u8.end()).c_str()) == IFSelect_RetDone);
}

void set_env(const char* name, const std::filesystem::path& value) {
#ifdef _WIN32
  const std::string n(name);
  _wputenv_s(std::wstring(n.begin(), n.end()).c_str(), value.wstring().c_str());
#else
  setenv(name, value.string().c_str(), 1);
#endif
}

bool about(double a, double b, double tol = 1e-3) { return std::abs(a - b) <= tol; }

// The board: a 50 x 30 mm rounded rectangle (corner radius 3) on the page from (100,100), its drill/place origin at
// (100,100), so a page point (x, y) is (x - 100, 100 - y) here.
std::string footprint(const std::string& head, const std::string& ref, const std::string& at, const std::string& body) {
  return "  (footprint \"" + head + "\" (layer \"" + (at.find("B.Cu") != std::string::npos ? "B.Cu" : "F.Cu") + "\") (uuid \"" + new_uuid() +
         "\")\n    (at " + at.substr(0, at.find('|')) + ")\n    (property \"Reference\" \"" + ref + "\" (at 0 0) (layer \"F.SilkS\"))\n" + body + "  )\n";
}
std::string model(const std::string& path, const std::string& extra = "(offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))") {
  return "    (model \"" + path + "\" " + extra + ")\n";
}

std::string board_text() {
  std::string s = R"((kicad_pcb (version 20241229) (generator "pcbnew") (generator_version "9.0")
  (general (thickness 1.6) (legacy_teardrops no))
  (paper "A4")
  (layers (0 "F.Cu" signal) (2 "B.Cu" signal) (25 "Edge.Cuts" user))
  (setup
    (stackup
      (layer "F.SilkS" (type "Top Silk Screen"))
      (layer "F.Mask" (type "Top Solder Mask") (color "Blue") (thickness 0.01))
      (layer "F.Cu" (type "copper") (thickness 0.035))
      (layer "dielectric 1" (type "core") (thickness 1.51) (material "FR4"))
      (layer "B.Cu" (type "copper") (thickness 0.035))
      (layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.01)))
    (pad_to_mask_clearance 0)
    (aux_axis_origin 100 100))
  (net 0 "")
  (gr_text "skipped (with a quoted \" and parens ((" (at 120 90) (layer "F.SilkS"))
  (gr_line (start 120 100) (end 103 100) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts"))
  (gr_line (start 150 127) (end 150 103) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts"))
  (gr_arc (start 147 130) (mid 149.12132 129.12132) (end 150 127) (stroke (width 0.1) (type solid)) (layer "Edge.Cuts"))
  (gr_line (start 147 130) (end 103 130) (layer "Edge.Cuts"))
  (gr_line (start 100 127) (end 100 115) (layer "Edge.Cuts"))
  (gr_arc (start 103 127) (end 103 130) (angle 90) (layer "Edge.Cuts"))
  (gr_line (start 100 103) (end 100 115) (layer "Edge.Cuts"))
  (gr_curve (pts (xy 147 100) (xy 135 100) (xy 125 100) (xy 120 100)) (layer "Edge.Cuts"))
  (gr_arc (start 103 103) (end 103 100) (angle -90) (layer "Edge.Cuts"))
  (gr_arc (start 147 100) (mid 149.12132 100.87868) (end 150 103) (layer "Edge.Cuts"))
  (gr_circle (center 110 110) (end 112 110) (layer "Edge.Cuts"))
  (gr_poly (pts (xy 125 108) (xy 129 108) (xy 129 112) (xy 125 112)) (layer "Edge.Cuts"))
  (gr_rect (start 135 120) (end 140 125) (layer "Edge.Cuts"))
  (gr_line (start 0 0) (end 10 0) (layer "F.SilkS"))
  (segment (start 1 1) (end 2 2) (width 0.2) (layer "F.Cu") (net 0))
))";
  const std::string box = "${KICAD9_3DMODEL_DIR}/Test.3dshapes/box.step";
  const std::string yard = "    (fp_rect (start -1.5 -1) (end 1.5 1) (layer \"F.CrtYd\"))\n    (pad \"1\" smd rect (at -0.8 0) (size 0.8 0.9) (layers \"F.Cu\"))\n";
  std::string fps;
  fps += footprint("MountingHole:MountingHole_3.2mm", "H1", "145 105", "    (pad \"\" np_thru_hole circle (at 0 0) (size 3.2 3.2) (drill 3.2) (layers \"*.Cu\"))\n");
  fps += footprint("Test:Slot", "J3", "115 125 90", "    (pad \"1\" thru_hole oval (at 0 0 90) (size 2 3) (drill oval 1 2) (layers \"*.Cu\"))\n");
  fps += footprint("Test:Castellated", "TP2", "150 115", "    (pad \"1\" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 1) (layers \"*.Cu\"))\n");
  fps += footprint("Test:Offset", "TP1", "120 105",
                   "    (pad \"1\" thru_hole circle (at 1 0) (size 2 2) (drill 0.8 (offset 0.5 0)) (layers \"*.Cu\"))\n"
                   "    (pad \"2\" thru_hole circle (at 1 0) (size 2 2) (drill 0.8 (offset 0.5 0)) (layers \"*.Cu\"))\n");
  fps += footprint("Test:R", "R1", "110 120", yard + model(box));
  fps += footprint("Test:R", "R2", "130 120 90", yard + model(box));
  std::string yard_b = yard;
  yard_b.replace(yard_b.find("F.CrtYd"), 7, "B.CrtYd");
  fps += footprint("Test:R", "U1", "140 112 180|B.Cu", yard_b + model(box, "(offset (xyz 1 0 0.5)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 90))"));
  fps += footprint("Test:Conn", "J1", "105 106", yard + model("${KIPRJMOD}/models/conn.wrl"));
  fps += footprint("Test:Missing", "J2", "135 105",
                   "    (property \"Height\" \"3 mm\" (at 0 0) (layer \"F.Fab\") (hide yes))\n    (fp_rect (start -2 -1.5) (end 2 1.5) (layer \"F.CrtYd\"))\n" +
                       model("${KICAD9_3DMODEL_DIR}/Missing.3dshapes/none.step"));
  fps += footprint("Test:R", "S1", "105 125", yard + model(box, "(offset (xyz 0 0 0)) (scale (xyz 2 2 2)) (rotate (xyz 0 0 0))"));
  fps += footprint("Test:R", "D1", "125 128", "    (attr smd dnp)\n" + yard + model(box));
  fps += footprint("Test:Other", "O1", "142 125", model("${OPAD_TEST_NOT_SET}/Other.3dshapes/other.step"));
  fps += footprint("Test:Hidden", "N1", "112 104", model(box, "(hide yes) (offset (xyz 0 0 0))"));
  fps += footprint("Test:NoModel", "N2", "113 104", yard);
  fps += "  (module Test:Legacy (layer F.Cu) (tedit 5E1) (tstamp 5E2) (at 120 115)\n    (fp_text reference X1 (at 0 0) (layer F.SilkS))\n"
         "    (model ${KICAD9_3DMODEL_DIR}/Test.3dshapes/box.step (at (xyz 0.1 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))))\n";
  s.insert(s.rfind(')'), fps);
  return s;
}

struct Fixture {
  Files files;
  bool quiet = (configure_kernel_logging(false), true);
  std::filesystem::path board = files.dir / "proj" / "board.kicad_pcb";
  std::filesystem::path mine = files.dir / "mine";
  Fixture() {
    write(board, board_text());
    step_box(files.dir / "lib3d" / "Test.3dshapes" / "box.step", -1, -0.5, 0, 2, 1, 0.5);
    step_box(files.dir / "proj" / "models" / "conn.step", 0, 0, 0, 1, 1, 1);
    step_box(mine / "other.step", 0, 0, 0, 1, 2, 3);
    set_env("KICAD9_3DMODEL_DIR", files.dir / "lib3d");
  }
  Document read(bool viewer = false, bool dnp = true, ImportResult* result = nullptr) const {
    Document d = Document::create();
    ImportOptions o;
    o.viewer = viewer;
    o.kicad.dnp = dnp;
    o.kicad.model_dirs = {mine};
    const ImportResult r = import_file(d, board, o);
    if (result) *result = r;
    return d;
  }
};

const Node* named(const Scene& s, const std::string& name) {
  for (const auto& [id, n] : s.nodes)
    if (n.name == name) return &n;
  return nullptr;
}
Bnd_Box world_box(const Document& d, const Scene& s, const Node* component) {
  Bnd_Box box;
  for (const auto& id : component->children) BRepBndLib::Add(node_world_shape(d, s, id), box);
  return box;
}
bool box_is(const Bnd_Box& b, double x0, double y0, double z0, double x1, double y1, double z1) {
  double a, c, e, f, g, h;
  b.Get(a, c, e, f, g, h);
  const double gap = b.GetGap();
  return about(a + gap, x0, 2e-3) && about(c + gap, y0, 2e-3) && about(e + gap, z0, 2e-3) && about(f - gap, x1, 2e-3) && about(g - gap, y1, 2e-3) && about(h - gap, z1, 2e-3);
}
}  // namespace

TEST(board_outline_holes_and_colour) {
  Fixture f;
  ImportResult r;
  const Document d = f.read(false, true, &r);
  const Scene s = resolve(d);
  CHECK(s.unresolved.empty());
  const Node* board = named(s, "Board");
  CHECK(board && board->has_color && about(board->color[2], 0.52) && about(board->color[0], 0.06));
  const TopoDS_Shape shape = body_shape(d, board->body_key);
  CHECK(BRepCheck_Analyzer(shape).IsValid());
  Bnd_Box box;
  BRepBndLib::AddOptimal(shape, box, false, false);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  CHECK(about(x0, 0) && about(x1, 50) && about(y0, -30) && about(y1, 0) && about(z0, 0) && about(z1, 1.6));
  // The rounded rectangle less the circle, square and rectangle cutouts and the drills: the mounting hole, the slot, the
  // offset drill (its duplicate drilled once) and half of the drill across the right edge.
  const double outline = 1500 - (36 - 9 * kPi) - 4 * kPi - 16 - 25;
  const double drills = kPi * 1.6 * 1.6 + (kPi * 0.25 + 1) + kPi * 0.16 + kPi * 0.25 / 2;
  GProp_GProps props;
  BRepGProp::VolumeProperties(shape, props);
  CHECK(about(props.Mass(), (outline - drills) * 1.6, 0.02));
  // The slot lies along X (its long side turned by the footprint's 90 degrees); the offset drill sits 1.5 mm right of the
  // footprint.
  auto solid = [&](double x, double y) {
    BRepClass3d_SolidClassifier c(shape, gp_Pnt(x, y, 0.8), 1e-7);
    return c.State() == TopAbs_IN;
  };
  CHECK(!solid(15.8, -25) && solid(15, -24.2) && !solid(15, -25));
  CHECK(!solid(21.5, -5) && solid(20.9, -5) && solid(22.1, -5));
  CHECK(!solid(49.8, -15) && solid(49.8, -16));
  CHECK(r.info["holes"] == 5 && r.info["thickness"] == 1.6 && r.info["outlines"] == 4);
}

TEST(footprints_placed_like_kicad) {
  Fixture f;
  ImportResult r;
  const Document d = f.read(false, true, &r);
  const Scene s = resolve(d);
  // R1 on top as the model is; R2 turned 90 degrees; U1 on the bottom, turned 180, offset (1, 0, 0.5) and the model turned
  // 90 about Z (KiCad negates model rotations); X1 from a KiCad 5 file (offsets in inches); S1 scaled twice.
  CHECK(box_is(world_box(d, s, named(s, "R1 R")), 9, -20.5, 1.6, 11, -19.5, 2.1));
  CHECK(box_is(world_box(d, s, named(s, "R2 R")), 29.5, -21, 1.6, 30.5, -19, 2.1));
  CHECK(box_is(world_box(d, s, named(s, "U1 R")), 38.5, -13, -1.0, 39.5, -11, -0.5));
  CHECK(box_is(world_box(d, s, named(s, "X1 Legacy")), 21.54, -15.5, 1.6, 23.54, -14.5, 2.1));
  CHECK(box_is(world_box(d, s, named(s, "S1 R")), 3, -26, 1.6, 7, -24, 2.6));
  // Found through ${KIPRJMOD} (the STEP beside the VRML name) and through the user's folder by its file name.
  CHECK(box_is(world_box(d, s, named(s, "J1 Conn")), 5, -6, 1.6, 6, -5, 2.6));
  CHECK(box_is(world_box(d, s, named(s, "O1 Other")), 42, -25, 1.6, 43, -23, 4.6));
  // One model, one body entry: R1, R2, U1, D1 and X1 share it; the scaled copy is another.
  const std::string key = s.node(named(s, "R1 R")->children[0])->body_key;
  CHECK(s.instance_count.at(key) == 5 && s.node(named(s, "S1 R")->children[0])->body_key != key);
  // The missing model: a translucent box over the courtyard, as tall as the Height property, just above the board.
  const Node* j2 = named(s, "J2 Missing");
  CHECK(j2 && j2->children.size() == 1);
  const Node* box = s.node(j2->children[0]);
  CHECK(box->name == "none" && box->opacity < 1 && box->has_color);
  CHECK(box_is(world_box(d, s, j2), 33, -6.5, 1.61, 37, -3.5, 4.61));
  // Hidden models and footprints without one are left out; natural order by reference.
  CHECK(!named(s, "N1 Hidden") && !named(s, "N2 NoModel"));
  std::vector<std::string> order;
  for (const auto& id : s.node(s.roots[0])->children) order.push_back(s.node(id)->name.substr(0, s.node(id)->name.find(' ')));
  const std::vector<std::string> expected = {"Board", "D1", "J1", "J2", "O1", "R1", "R2", "S1", "U1", "X1", "Layers"};
  CHECK(order == expected);
  CHECK(r.info["components"] == 9 && r.info["placeholders"] == 1 && r.info["models"] == 4 && r.info["footprints"] == 15);
  CHECK(r.warnings.size() == 1 && r.warnings[0].find("J2 (none.step)") != std::string::npos);
  // The hidden 2D layers for sketches: the outline with the drills, the courtyards on top of the board and under it.
  const Node* layers = named(s, "Layers");
  CHECK(layers && !layers->visible && layers->children.size() == 3);
  std::vector<std::string> names;
  for (const auto& id : layers->children) {
    names.push_back(s.node(id)->name);
    CHECK(s.node(id)->representation == "drawing2d");
  }
  CHECK(names == std::vector<std::string>({"Edge.Cuts", "F.Courtyard", "B.Courtyard"}));
  CHECK(box_is(world_box(d, s, named(s, "U1 R")), 38.5, -13, -1.0, 39.5, -11, -0.5));
  Bnd_Box top;
  BRepBndLib::Add(node_world_shape(d, s, layers->children[1]), top);
  double x0, y0, z0, x1, y1, z1;
  top.Get(x0, y0, z0, x1, y1, z1);
  CHECK(about(z0 + top.GetGap(), 1.6) && about(z1 - top.GetGap(), 1.6));
  // Without do-not-populate parts.
  const Scene without = resolve(f.read(false, false));
  CHECK(!named(without, "D1 R") && named(without, "R1 R"));
}

TEST(saved_and_viewed) {
  Fixture f;
  Document d = f.read();
  const auto path = f.files.dir / "board.opad";
  d.save_as(path);
  const Document again = Document::load(path);
  const Scene s = resolve(again), before = resolve(d);
  CHECK(s.all_bodies().size() == before.all_bodies().size() && s.unresolved.empty());
  const Op* op = nullptr;
  for (const auto& o : again.ops)
    if (o.type == "import") op = &o;
  CHECK(op && op->data["kicad"]["origin"][0] == 100.0 && op->data["kicad"]["thickness"] == 1.6);
  // Viewer mode: live shapes, nothing to save.
  ImportResult r;
  const Document view = f.read(true, true, &r);
  CHECK(view.has_live_bodies() && resolve(view).all_bodies().size() == before.all_bodies().size());
  CHECK(r.info["placeholders"] == 1);
}

TEST(model_lookup) {
  Fixture f;
  const auto proj = f.board.parent_path();
  CHECK(kicad_model_file("${KIPRJMOD}/models/conn.wrl", proj) == (proj / "models" / "conn.step").lexically_normal());
  CHECK(kicad_model_file("models/conn.step", proj) == (proj / "models" / "conn.step").lexically_normal());
  CHECK(kicad_model_file("$(KICAD9_3DMODEL_DIR)\\Test.3dshapes\\box.step", proj) == (f.files.dir / "lib3d" / "Test.3dshapes" / "box.step").lexically_normal());
  CHECK(kicad_model_file("${OPAD_TEST_NOT_SET}/Other.3dshapes/other.step", proj).empty());
  CHECK(kicad_model_file("${OPAD_TEST_NOT_SET}/Other.3dshapes/other.step", proj, {f.mine}) == (f.mine / "other.step").lexically_normal());
  CHECK(kicad_model_file((f.mine / "other.step").string(), proj) == (f.mine / "other.step").lexically_normal());
  CHECK(kicad_model_file("kicad-embed://other.step", proj, {f.mine}).empty());
  CHECK(kicad_model_file("${KIPRJMOD}/models/none.wrl", proj).empty());
}

TEST(panels_open_outlines_and_errors) {
  Files files;
  const auto p = files.dir / "panel.kicad_pcb";
  write(p, R"((kicad_pcb (version 20221018) (general (thickness 0.8))
  (gr_rect (start 0 0) (end 10 10) (layer "Edge.Cuts"))
  (gr_rect (start 20 0) (end 30 10) (layer "Edge.Cuts"))
  (gr_line (start 40 0) (end 50 0) (layer "Edge.Cuts"))
  (gr_line (start 50 0) (end 50 10) (layer "Edge.Cuts"))
  (footprint "Test:Far" (layer "F.Cu") (at 80 80) (pad "1" thru_hole circle (at 0 0) (size 2 2) (drill 1)))
  (via (at 5 5) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu"))
  (via blind (at 6 6) (size 0.6) (drill 0.3) (layers "F.Cu" "In1.Cu"))
))");
  Document d = Document::create();
  const ImportResult r = import_file(d, p, {});
  const Scene s = resolve(d);
  CHECK(named(s, "Board 1") && named(s, "Board 2") && !named(s, "Board"));
  int unclosed = 0, outside = 0;
  for (const auto& w : r.warnings) unclosed += w.find("do not close") != std::string::npos, outside += w.find("outside the board") != std::string::npos;
  CHECK(unclosed == 1 && outside == 1 && r.info["holes"] == 1);
  ImportOptions vias;
  vias.kicad.vias = true;
  Document drilled = Document::create();
  CHECK(import_file(drilled, p, vias).info["holes"] == 2);  // the through via, not the blind one
  Bnd_Box box;
  for (const auto& id : s.all_bodies())
    if (s.node(id)->representation != "drawing2d") BRepBndLib::Add(node_world_shape(d, s, id), box);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  CHECK(about(z1 - box.GetGap(), 0.8) && about(x1 - x0 - 2 * box.GetGap(), 30));  // centred on the outlines (no origin set)
  write(p, "(not_a_board (version 1))");
  CHECK_THROWS(import_file(d, p, {}));
  write(p, "(kicad_pcb (version 1) (gr_line (start 0 0)");
  CHECK_THROWS(import_file(d, p, {}));
}

int main(int argc, char** argv) { return check::run_all(argc, argv); }
