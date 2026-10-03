// KiCad boards (.kicad_pcb) from synthetic files written here (never the user's boards: TC2030 is GPL-3): the outline
// from shuffled and reversed pieces in every form (lines, arcs old and new, curves, circles, rectangles, polygons), the
// drills (round, oval, offset, duplicated, across the edge), the thickness and mask colour, and the footprints' models
// placed as KiCad places them (top and bottom, turned, offset, rotated, scaled, legacy inches), found through the
// environment, ${KIPRJMOD}, a VRML name's STEP sibling and the user's folders, shared between footprints, with boxes
// for the ones that are missing.
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Builder.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Cylinder.hxx>
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
#include "opad/mesh.hpp"
#ifdef OPAD_HAVE_ZSTD
#include <zstd.h>
#endif

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
  const auto& b = f.board;
  CHECK(kicad_model_file("${KIPRJMOD}/models/conn.wrl", b) == (proj / "models" / "conn.step").lexically_normal());
  CHECK(kicad_model_file("models/conn.step", b) == (proj / "models" / "conn.step").lexically_normal());
  CHECK(kicad_model_file("$(KICAD9_3DMODEL_DIR)\\Test.3dshapes\\box.step", b) == (f.files.dir / "lib3d" / "Test.3dshapes" / "box.step").lexically_normal());
  CHECK(kicad_model_file("${OPAD_TEST_NOT_SET}/Other.3dshapes/other.step", b).empty());
  CHECK(kicad_model_file("${OPAD_TEST_NOT_SET}/Other.3dshapes/other.step", b, {f.mine}) == (f.mine / "other.step").lexically_normal());
  CHECK(kicad_model_file((f.mine / "other.step").string(), b) == (f.mine / "other.step").lexically_normal());
  CHECK(kicad_model_file("kicad-embed://other.step", b, {f.mine}).empty());
  CHECK(kicad_model_file("${KIPRJMOD}/models/none.wrl", b).empty());
  // A VRML file alone is taken, but a STEP anywhere comes first; the download folder is searched last for library models.
  write(f.mine / "alone.wrl", "#VRML V2.0 utf8\n");
  write(f.mine / "both.wrl", "#VRML V2.0 utf8\n");
  step_box(kicad_download_dir() / "Test.3dshapes" / "both.step", 0, 0, 0, 1, 1, 1);
  CHECK(kicad_model_file("${KICAD9_3DMODEL_DIR}/Test.3dshapes/alone.wrl", b, {f.mine}) == (f.mine / "alone.wrl").lexically_normal());
  CHECK(kicad_model_file("${KICAD9_3DMODEL_DIR}/Test.3dshapes/both.wrl", b, {f.mine}) == (kicad_download_dir() / "Test.3dshapes" / "both.step").lexically_normal());
  CHECK(kicad_model_file("${OPAD_TEST_NOT_SET}/Test.3dshapes/both.step", b).empty());  // not a library variable
}

// The project's text variables (<board>.kicad_pro) name model folders, ${KIPRJMOD} inside them too; a footprint whose only
// model is KiCad's own VRML is read in KiCad's units (2.54 mm, Z up) instead of a box.
TEST(project_variables_and_vrml) {
  Files files;
  const auto board = files.dir / "proj" / "vars.kicad_pcb";
  write(files.dir / "proj" / "vars.kicad_pro", R"({"meta": {"version": 3}, "text_variables": {"VENDOR": "${KIPRJMOD}/vendor", "EMPTY": ""}})");
  step_box(files.dir / "proj" / "vendor" / "part.step", -1, -1, 0, 2, 2, 1);
  write(files.dir / "proj" / "models" / "only.wrl",
        "#VRML V2.0 utf8\nShape {\n appearance Appearance { material Material { diffuseColor 0.8 0.7 0.5 } }\n geometry IndexedFaceSet {\n"
        "  coord Coordinate { point [0 0 0, 1 0 0, 1 1 0, 0 1 0, 0 0 1, 1 0 1, 1 1 1, 0 1 1] }\n"
        "  coordIndex [0,2,1,-1, 0,3,2,-1, 4,5,6,-1, 4,6,7,-1, 0,1,5,-1, 0,5,4,-1, 1,2,6,-1, 1,6,5,-1, 2,3,7,-1, 2,7,6,-1, 3,0,4,-1, 3,4,7,-1]\n }\n}\n");
  write(board, "(kicad_pcb (version 20241229) (general (thickness 1.6))\n  (gr_rect (start 100 100) (end 140 120) (layer \"Edge.Cuts\"))\n" +
                   footprint("Test:Part", "P1", "110 110", model("${VENDOR}/part.step")) + footprint("Test:Vrml", "V1", "120 112", model("${KIPRJMOD}/models/only.wrl")) +
                   footprint("Test:Empty", "E1", "130 110", model("${EMPTY}/part.step")) + ")\n");
  Document d = Document::create();
  ImportOptions o;
  o.kicad.origin = "page";
  const ImportResult r = import_file(d, board, o);
  const Scene s = resolve(d);
  CHECK(r.info["placeholders"] == 1 && r.info["models"] == 2);
  CHECK(box_is(world_box(d, s, named(s, "P1 Part")), 109, -111, 1.6, 111, -109, 2.6));
  const Node* v1 = named(s, "V1 Vrml");
  CHECK(v1 && s.node(v1->children[0])->representation == "mesh" && s.node(v1->children[0])->opacity >= 1);
  CHECK(box_is(world_box(d, s, v1), 120, -112, 1.6, 122.54, -109.46, 4.14));
  CHECK(named(s, "E1 Empty") && s.node(named(s, "E1 Empty")->children[0])->name == "part");
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

// The models a board names, and KiCad's library models it misses fetched from the library (a local copy reached through
// file:// here, laid out like the repository: <release>/<library>.3dshapes/<file>): the board's release first, then
// master; a missing file or one that is not a STEP fails alone; the next read finds the downloads.
TEST(library_models_download) {
  Files files;
  const auto repo = files.dir / "repo";
  step_box(repo / "8.0.0" / "Fake.3dshapes" / "part.step", -1, -1, 0, 2, 2, 1);
  step_box(repo / "master" / "Fake.3dshapes" / "newer.step", -1, -1, 0, 2, 2, 2);
  write(repo / "8.0.0" / "Fake.3dshapes" / "page.step", "<html>not found</html>");
  const std::string root = repo.generic_string();
  set_env("OPAD_KICAD_MODELS_URL", path_from_utf8("file://" + std::string(root[0] == '/' ? "" : "/") + root + "/"));
  const auto board = files.dir / "lib.kicad_pcb";
  write(board, "(kicad_pcb (version 20241229) (general (thickness 1.6))\n  (gr_rect (start 0 0) (end 40 20) (layer \"Edge.Cuts\"))\n" +
                   footprint("Fake:A", "U1", "5 5", model("${KICAD8_3DMODEL_DIR}/Fake.3dshapes/part.wrl")) +
                   footprint("Fake:A", "U2", "10 5", model("${KICAD8_3DMODEL_DIR}/Fake.3dshapes/part.wrl")) +
                   footprint("Fake:B", "U3", "15 5", model("${KICAD8_3DMODEL_DIR}/Fake.3dshapes/newer.step")) +
                   footprint("Fake:C", "U4", "20 5", model("${KICAD8_3DMODEL_DIR}/Fake.3dshapes/nowhere.step")) +
                   footprint("Fake:D", "U5", "25 5", model("${KICAD8_3DMODEL_DIR}/Fake.3dshapes/page.step")) +
                   footprint("Fake:E", "U6", "30 5", model("${OPAD_TEST_NOT_SET}/Mine.3dshapes/mine.step")) +
                   footprint("Fake:F", "U7", "35 5", model("${KICAD8_3DMODEL_DIR}/../escape.step")) + ")\n");
  const json before = kicad_models(board);
  CHECK(before["models"].size() == 6 && before["missing"] == 6 && before["downloadable"] == 4);
  CHECK(before["models"][0]["refs"] == json::array({"U1", "U2"}) && before["models"][0]["library"] == "Fake.3dshapes/part.step" && before["models"][0]["tag"] == "8.0.0");
  Document d0 = Document::create();
  CHECK(import_file(d0, board, {}).info["downloadable"] == 4);
  std::vector<std::string> phases;
  const json got = kicad_download_models(board, {}, [&](double, const std::string& what) { return phases.push_back(what), true; });
  CHECK(got["downloaded"] == json::array({"Fake.3dshapes/part.step", "Fake.3dshapes/newer.step"}) && got["failed"].size() == 2);
  CHECK(phases.size() == 4 && phases[3] == "downloading 3D models 4/4");
  CHECK(std::filesystem::exists(kicad_download_dir() / "README.txt") && !std::filesystem::exists(kicad_download_dir() / "Fake.3dshapes" / "page.step"));
  const json after = kicad_models(board);
  CHECK(after["found"] == 2 && after["downloadable"] == 2);
  Document d = Document::create();
  const ImportResult r = import_file(d, board, {});
  CHECK(r.info["models"] == 2 && r.info["placeholders"] == 4 && r.info["downloadable"] == 2);
  CHECK_THROWS(kicad_download_models(board, {}, [](double, const std::string&) { return false; }));  // cancelled
  set_env("OPAD_KICAD_MODELS_URL", "");
}

#ifdef OPAD_HAVE_ZSTD
// KiCad 9 embeds models in the board or in a footprint (kicad-embed://name): zstd-compressed, base64 between bars, in
// lines. The footprint's own file comes before the board's of the same name; one named nowhere is a box and a warning.
std::string embedded_record(const std::string& name, const std::string& content) {
  std::string packed(ZSTD_compressBound(content.size()), '\0');
  packed.resize(ZSTD_compress(packed.data(), packed.size(), content.data(), content.size(), 9));
  static const char* digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string text;
  for (size_t i = 0; i < packed.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(packed[i]) << 16) | (i + 1 < packed.size() ? static_cast<unsigned char>(packed[i + 1]) << 8 : 0) |
                       (i + 2 < packed.size() ? static_cast<unsigned char>(packed[i + 2]) : 0);
    text += digits[(v >> 18) & 63];
    text += digits[(v >> 12) & 63];
    text += i + 1 < packed.size() ? digits[(v >> 6) & 63] : '=';
    text += i + 2 < packed.size() ? digits[v & 63] : '=';
  }
  std::string lines;
  for (size_t i = 0; i < text.size(); i += 76) lines += "\n        " + std::string(i ? "" : "|") + text.substr(i, 76);
  return "(embedded_files (file (name \"" + name + "\") (type model) (data" + lines + "|) (checksum \"0\")))\n";
}

TEST(embedded_models) {
  Files files;
  auto step_text = [&](const char* leaf, double dz) {
    step_box(files.dir / leaf, -1, -1, 0, 2, 2, dz);
    return read_text_file(files.dir / leaf);
  };
  const std::string board_model = step_text("a.step", 1), own_model = step_text("b.step", 3);
  const auto board = files.dir / "embed.kicad_pcb";
  write(board, "(kicad_pcb (version 20241229) (general (thickness 1.6))\n  (gr_rect (start 0 0) (end 40 20) (layer \"Edge.Cuts\"))\n" +
                   embedded_record("part.step", board_model) +
                   footprint("Emb:Board", "U1", "10 10", model("kicad-embed://part.step")) +
                   footprint("Emb:Board", "U2", "20 10", model("kicad-embed://part.step")) +
                   footprint("Emb:Own", "U3", "30 10", model("kicad-embed://part.step") + "    " + embedded_record("part.step", own_model)) +
                   footprint("Emb:None", "U4", "35 10", model("kicad-embed://none.step")) + ")\n");
  Document d = Document::create();
  ImportOptions o;
  o.kicad.origin = "page";
  const ImportResult r = import_file(d, board, o);
  const Scene s = resolve(d);
  CHECK(r.info["models"] == 2 && r.info["placeholders"] == 1 && r.info["downloadable"] == 0);
  CHECK(r.warnings.size() == 2 && r.warnings[0].find("none.step") != std::string::npos);
  CHECK(box_is(world_box(d, s, named(s, "U1 Board")), 9, -11, 1.6, 11, -9, 2.6));
  CHECK(box_is(world_box(d, s, named(s, "U3 Own")), 29, -11, 1.6, 31, -9, 4.6));
  CHECK(s.node(named(s, "U1 Board")->children[0])->body_key == s.node(named(s, "U2 Board")->children[0])->body_key);
  const json list = kicad_models(board);
  CHECK(list["models"].size() == 2 && list["found"] == 1 && list["missing"] == 1);  // by name
}
#endif

// What reading a changed board again would do to its import, per reference designator, matched by footprint uuid: moved,
// turned, flipped, model changed, added, removed, unchanged; the board's thickness, drills and outline.
TEST(sync_preview) {
  Files files;
  auto fp = [](const std::string& id, const std::string& ref, const std::string& at, const std::string& layer, const std::string& model_name) {
    return "  (footprint \"Sync:" + ref.substr(0, 1) + "\" (layer \"" + layer + "\") (uuid \"" + id + "\") (at " + at + ")\n    (property \"Reference\" \"" + ref +
           "\")\n    (model \"${KIPRJMOD}/" + model_name + "\"))\n";
  };
  const std::string hole = "  (footprint \"MountingHole\" (layer \"F.Cu\") (at 45 25) (pad \"\" np_thru_hole circle (at 0 0) (size 3 3) (drill 3)))\n";
  const auto board = files.dir / "sync.kicad_pcb";
  write(board, "(kicad_pcb (version 20241229) (general (thickness 1.6))\n  (gr_rect (start 0 0) (end 50 30) (layer \"Edge.Cuts\"))\n" +
                   fp("aaaaaaaa-0000-0000-0000-000000000001", "R1", "10 10", "F.Cu", "m1.step") + fp("aaaaaaaa-0000-0000-0000-000000000002", "R2", "20 10 90", "F.Cu", "m1.step") +
                   fp("aaaaaaaa-0000-0000-0000-000000000003", "U1", "30 10", "F.Cu", "m2.step") + fp("aaaaaaaa-0000-0000-0000-000000000004", "D1", "40 10", "F.Cu", "m1.step") +
                   fp("aaaaaaaa-0000-0000-0000-000000000005", "C1", "10 20", "F.Cu", "m1.step") + hole + ")\n");
  Document d = Document::create();
  import_file(d, board, {});
  const json same = kicad_sync_preview(d, {}, board);
  CHECK(!same["changed"].get<bool>() && same["unchanged"] == 5 && same["added"].empty() && same["removed"].empty());
  d.save_as(files.dir / "sync.opad");
  const Document saved = Document::load(files.dir / "sync.opad");
  write(board, "(kicad_pcb (version 20241229) (general (thickness 1.2))\n  (gr_rect (start 0 0) (end 50 32) (layer \"Edge.Cuts\"))\n" +
                   fp("aaaaaaaa-0000-0000-0000-000000000001", "R1", "11 8", "F.Cu", "m1.step") + fp("aaaaaaaa-0000-0000-0000-000000000002", "R2", "20 10 180", "F.Cu", "m2.step") +
                   fp("aaaaaaaa-0000-0000-0000-000000000004", "D1", "40 10", "B.Cu", "m1.step") + fp("aaaaaaaa-0000-0000-0000-000000000005", "C1", "10 20", "F.Cu", "m1.step") +
                   fp("aaaaaaaa-0000-0000-0000-000000000006", "N1", "25 25", "F.Cu", "m1.step") + ")\n");
  const json p = kicad_sync_preview(saved);  // the board beside the document, the only KiCad import
  CHECK(p["changed"].get<bool>() && p["unchanged"] == 1);
  CHECK(p["moved"].size() == 2 && p["moved"][0]["ref"] == "R1" && about(p["moved"][0]["dx"], 1) && about(p["moved"][0]["dy"], 2) && about(p["moved"][0]["drot"], 0));
  CHECK(p["moved"][1]["ref"] == "R2" && about(p["moved"][1]["drot"], 90) && about(p["moved"][1]["dx"], 0));
  CHECK(p["models_changed"].size() == 1 && p["models_changed"][0]["ref"] == "R2" && p["models_changed"][0]["after"][0] == "${KIPRJMOD}/m2.step");
  CHECK(p["flipped"].size() == 1 && p["flipped"][0]["ref"] == "D1" && p["flipped"][0]["side"] == "bottom");
  CHECK(p["added"].size() == 1 && p["added"][0]["ref"] == "N1" && p["removed"].size() == 1 && p["removed"][0]["ref"] == "U1");
  CHECK(p["thickness"]["before"] == 1.6 && p["thickness"]["after"] == 1.2 && p["holes"]["before"] == 1 && p["holes"]["after"] == 0);
  CHECK(about(p["outline"]["before"]["area"], 1500) && about(p["outline"]["after"]["area"], 1600));
  CHECK_THROWS(kicad_sync_preview(Document::create()));
}

// A through-hole part's pins must go down its own drills, whichever side and turn: a 2x3 header model (pins only, pin 1
// at its origin, KiCad's 3D frame: +y is up the page) on footprints turned 0/90/180/270 on top, and the same footprints
// flipped to the bottom as KiCad stores them (pads mirrored in y, orientation negated, B.Cu). Each pin's centre must
// sit on a drill of the board solid, and the pins must cross the board from the part's side.
TEST(pins_go_down_their_drills) {
  Files files;
  BRep_Builder bb;
  TopoDS_Compound pins;
  bb.MakeCompound(pins);
  const double pitch = 2.54;
  for (int c = 0; c < 2; ++c)
    for (int r = 0; r < 3; ++r) bb.Add(pins, BRepPrimAPI_MakeBox(gp_Pnt(c * pitch - 0.25, -r * pitch - 0.25, -3), 0.5, 0.5, 3.5).Shape());
  STEPControl_Writer w;
  w.Transfer(pins, STEPControl_AsIs);
  const auto model_file = files.dir / "header.step";
  CHECK(w.Write(model_file.string().c_str()) == IFSelect_RetDone);
  std::string text = "(kicad_pcb (version 20241229) (general (thickness 1.6))\n  (gr_rect (start 0 0) (end 100 60) (layer \"Edge.Cuts\"))\n";
  int index = 0;
  for (const bool bottom : {false, true})
    for (const int turn : {0, 90, 180, 270}) {
      std::string pads;
      for (int c = 0; c < 2; ++c)
        for (int r = 0; r < 3; ++r)
          pads += "    (pad \"" + std::to_string(c * 3 + r + 1) + "\" thru_hole circle (at " + std::to_string(c * pitch) + " " + std::to_string((bottom ? -1 : 1) * r * pitch) +
                  ") (size 1.7 1.7) (drill 1) (layers \"*.Cu\"))\n";
      const std::string at = std::to_string(15 + 22 * (index % 4)) + " " + std::to_string(bottom ? 40 : 15) + " " + std::to_string(bottom ? -turn : turn);
      text += footprint("Test:Header", "J" + std::to_string(++index), at + (bottom ? "|B.Cu" : ""), pads + model(model_file.generic_string()));
    }
  const auto board = files.dir / "pins.kicad_pcb";
  write(board, text + ")\n");
  Document d = Document::create();
  ImportOptions o;
  o.kicad.origin = "page";
  import_file(d, board, o);
  const Scene s = resolve(d);
  std::vector<gp_Pnt> drills;
  for (TopExp_Explorer e(body_shape(d, named(s, "Board")->body_key), TopAbs_FACE); e.More(); e.Next()) {
    BRepAdaptor_Surface face(TopoDS::Face(e.Current()));
    if (face.GetType() == GeomAbs_Cylinder) drills.push_back(face.Cylinder().Location());
  }
  int pinsChecked = 0;
  for (int i = 1; i <= 8; ++i) {
    const Node* part = named(s, "J" + std::to_string(i) + " Header");
    CHECK(part && !part->children.empty() && !s.node(part->children[0])->body_missing && s.node(part->children[0])->opacity >= 1);
    BRep_Builder parts;
    TopoDS_Compound all;
    parts.MakeCompound(all);
    for (const auto& id : part->children) parts.Add(all, node_world_shape(d, s, id));
    for (TopExp_Explorer e(all, TopAbs_SOLID); e.More(); e.Next()) {
      Bnd_Box box;
      BRepBndLib::Add(e.Current(), box);
      double x0, y0, z0, x1, y1, z1;
      box.Get(x0, y0, z0, x1, y1, z1);
      const double x = (x0 + x1) / 2, y = (y0 + y1) / 2;
      const bool inDrill = std::any_of(drills.begin(), drills.end(), [&](const gp_Pnt& p) { return std::hypot(p.X() - x, p.Y() - y) < 0.01; });
      CHECK(inDrill);
      CHECK(i <= 4 ? (z0 < 0 && about(z1, 2.1, 0.01)) : (z1 > 1.6 && about(z0, -0.5, 0.01)));  // through the board from the part's side
      ++pinsChecked;
    }
  }
  CHECK(pinsChecked == 48);
}

// A board with more drills than the default triangulator handles in time (it grew about quadratically with the holes of
// one face): meshed by Delabella, the mesh still closes on the exact volume.
TEST(many_drills_mesh) {
  Files files;
  const auto p = files.dir / "drills.kicad_pcb";
  std::string text = "(kicad_pcb (version 20241229) (general (thickness 1.6))\n(gr_rect (start 0 0) (end 60 40) (layer \"Edge.Cuts\"))\n";
  for (int i = 0; i < 10; ++i)
    for (int j = 0; j < 8; ++j)
      text += "(footprint \"H\" (layer \"F.Cu\") (at " + std::to_string(5 + i * 5.5) + " " + std::to_string(5 + j * 4.3) +
              ") (pad \"\" np_thru_hole circle (at 0 0) (size 1 1) (drill 1)))\n";
  write(p, text + ")\n");
  Document d = Document::create();
  import_file(d, p, {});
  const Scene s = resolve(d);
  const TopoDS_Shape board = body_shape(d, named(s, "Board")->body_key);
  const Mesh m = tessellate(board, 0.01, 10);
  double volume = 0;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const float* a = &m.positions[3 * m.indices[t]];
    const float* b = &m.positions[3 * m.indices[t + 1]];
    const float* c = &m.positions[3 * m.indices[t + 2]];
    volume += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
  }
  const double exact = (60 * 40 - 80 * kPi * 0.25) * 1.6;
  CHECK(std::abs(std::abs(volume) - exact) < exact * 1e-3);
}

int main(int argc, char** argv) {
  const Files cache;  // downloads and the user cache stay in here
  set_env("OPAD_CACHE_DIR", cache.dir);
  return check::run_all(argc, argv);
}
