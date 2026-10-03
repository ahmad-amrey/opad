// Exploded views (TODO 11 UI-35): units by level, keep/split/groups, small parts riding on what they touch, the three
// modes, staging, manual offsets, the scene for measuring, and the optional `explode` object on view ops.
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/explode.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

using namespace opad;

namespace {

struct Device {
  Document doc = Document::create();
  std::string device = new_uuid(), shell = new_uuid(), lid = new_uuid(), led = new_uuid(), pcb = new_uuid(), board = new_uuid(),
              chip = new_uuid(), cap1 = new_uuid(), cap2 = new_uuid(), solder = new_uuid(), screws = new_uuid();
  std::vector<std::string> screw;
};

std::string box(Document& doc, double x0, double y0, double z0, double x1, double y1, double z1) {
  return doc.add_body(brep_from_shape(BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), gp_Pnt(x1, y1, z1)).Shape()), json::object());
}

json body(const std::string& id, const std::string& name, const std::string& key, const Mat4& m = Mat4::identity()) {
  json n = {{"type", "body"}, {"id", id}, {"name", name}, {"key", key}};
  if (!m.is_identity()) n["transform"] = m.to_json();
  return n;
}

json component(const std::string& id, const std::string& name, json children, const Mat4& m = Mat4::identity()) {
  json n = {{"type", "component"}, {"id", id}, {"name", name}, {"children", std::move(children)}};
  if (!m.is_identity()) n["transform"] = m.to_json();
  return n;
}

// A 100 x 60 enclosure: bottom shell, lid with a tiny light pipe on top, a PCB subassembly (board, chip, two
// capacitors, a solder joint) and four screws driven up through the floor from a component placed 2 mm down.
Device device() {
  Device d;
  const std::string screwKey = d.doc.add_body(brep_from_shape(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5, 12).Shape()), json::object());
  json screws = json::array();
  const double corners[4][2] = {{4, 4}, {96, 4}, {4, 56}, {96, 56}};
  for (int i = 0; i < 4; ++i) {
    d.screw.push_back(new_uuid());
    screws.push_back(body(d.screw.back(), "Screw " + std::to_string(i + 1), screwKey, Mat4::translation(corners[i][0], corners[i][1], 0)));
  }
  json pcb = json::array({body(d.board, "Board", box(d.doc, 5, 5, 5, 95, 55, 6.6)), body(d.chip, "Chip", box(d.doc, 40, 25, 6.6, 50, 35, 8.6)),
                          body(d.cap1, "C1", box(d.doc, 20, 20, 6.6, 22, 21.2, 7.6)), body(d.cap2, "C2", box(d.doc, 70, 40, 6.6, 72, 41.2, 7.6)),
                          body(d.solder, "Joint", box(d.doc, 21.8, 20.4, 6.4, 22.3, 20.8, 6.9))});
  json top = json::array({body(d.shell, "Shell bottom", box(d.doc, 0, 0, 0, 100, 60, 20)), body(d.lid, "Lid", box(d.doc, 0, 0, 20, 100, 60, 25)),
                          body(d.led, "Light pipe", box(d.doc, 48, 28, 25, 50, 30, 27)), component(d.pcb, "PCB", pcb),
                          component(d.screws, "Screws", screws, Mat4::translation(0, 0, -2))});
  d.doc.append({{"op", "import"}, {"source", "test"}, {"units", "mm"}, {"nodes", json::array({component(d.device, "Device", top)})}});
  return d;
}

const ExplodeUnit& unit_of(const std::vector<ExplodeUnit>& units, const std::string& id) {
  for (const auto& u : units)
    if (u.id == id) return u;
  throw check::Failure("no unit " + id);
}

struct Box3 {
  Vec3 lo, hi;
};

bool holds(const ExplodeUnit& u, const std::string& body) { return std::find(u.bodies.begin(), u.bodies.end(), body) != u.bodies.end(); }

int count_level(const std::vector<ExplodeUnit>& units, int level) {
  return static_cast<int>(std::count_if(units.begin(), units.end(), [&](const ExplodeUnit& u) { return u.level == level; }));
}

ExplodeSpec spec_of(const json& j) { return ExplodeSpec::from_json(j); }

json dump(const std::vector<ExplodeUnit>& units) {
  json out = json::array();
  for (const auto& u : units) out.push_back({u.id, u.bodies, u.parent, u.level, u.dir, u.distance, u.t0, u.t1});
  return out;
}

}  // namespace

TEST(keep_the_pcb_whole_split_the_screws) {
  Device d = device();
  const Scene s = resolve(d.doc);
  const ExplodeSpec spec = spec_of({{"levels", 1}, {"keep", {d.pcb}}, {"split", {d.screws}}});
  CHECK_EQ(explode_root(s, spec), d.device);  // the lone root component is skipped
  CHECK_EQ(explode_depth(s, spec), 2);
  const auto units = explode_units(d.doc, s, spec);
  CHECK_EQ(count_level(units, 1), 4);  // shell, lid (with the light pipe), PCB, screws
  CHECK_EQ(count_level(units, 2), 4);  // each screw
  const ExplodeUnit& pcb = unit_of(units, d.pcb);
  CHECK_EQ(pcb.bodies.size(), size_t(5));
  for (const auto& b : {d.board, d.chip, d.cap1, d.cap2, d.solder}) CHECK(holds(pcb, b));
  CHECK(holds(unit_of(units, d.lid), d.led));  // small, sits on the lid
  for (const auto& u : units) CHECK(u.id != d.led);
  const ExplodeUnit& screws = unit_of(units, d.screws);
  CHECK(screws.bodies.empty());
  for (const auto& id : d.screw) {
    const ExplodeUnit& u = unit_of(units, id);
    CHECK_EQ(u.level, 2);
    CHECK_EQ(units[static_cast<size_t>(u.parent)].id, d.screws);
    CHECK(u.distance > 0);
  }
  // Radial: the bottom shell, nearly the whole model, stays; the lid lifts off it. The board and the screws lie in the
  // shell's box: they leave it towards the rest of the model (up), only as far as they must, so the board ends between
  // the shells. Each screw spreads out from the screws' centre, level.
  CHECK_NEAR(unit_of(units, d.shell).distance, 0, 1e-12);
  CHECK_NEAR(unit_of(units, d.lid).dir[2], 1, 1e-9);
  CHECK_NEAR(pcb.dir[2], 1, 1e-9);
  CHECK_NEAR(screws.dir[2], 1, 1e-9);
  const auto at1 = explode_unit_offsets(units, spec, 1);
  auto index = [&](const std::string& id) { return static_cast<size_t>(&unit_of(units, id) - units.data()); };
  CHECK(pcb.lo[2] + at1[index(d.pcb)][2] > unit_of(units, d.shell).hi[2] + at1[index(d.shell)][2]);
  CHECK(pcb.hi[2] + at1[index(d.pcb)][2] < unit_of(units, d.lid).lo[2] + at1[index(d.lid)][2]);
  CHECK(pcb.lo[2] + at1[index(d.pcb)][2] > screws.hi[2] + at1[index(d.screws)][2]);  // and the screws'
  const ExplodeUnit& s1 = unit_of(units, d.screw[0]);
  CHECK(s1.dir[0] < -0.8 && s1.dir[1] < -0.4 && std::abs(s1.dir[2]) < 1e-9);
  CHECK_NEAR(s1.centre[2], 4, 1e-6);  // through the component's 2 mm drop
  // Parents come first; the level control and the distance are separate inputs: levels 1 never moves the board alone.
  for (size_t i = 0; i < units.size(); ++i) CHECK(units[i].parent < static_cast<int>(i));
  // Nothing stays buried: every pair of siblings ends apart.
  for (size_t i = 0; i < units.size(); ++i)
    for (size_t j = i + 1; j < units.size(); ++j) {
      if (units[i].parent != units[j].parent) continue;
      bool overlap = true;
      for (size_t k = 0; k < 3; ++k)
        overlap = overlap && std::min(units[i].hi[k] + at1[i][k], units[j].hi[k] + at1[j][k]) - std::max(units[i].lo[k] + at1[i][k], units[j].lo[k] + at1[j][k]) > 0.05;
      CHECK(!overlap);
    }
}

TEST(levels_and_small_parts) {
  Device d = device();
  const Scene s = resolve(d.doc);
  const auto two = explode_units(d.doc, s, spec_of({{"levels", 2}}));
  // The PCB opens: the capacitors and the joint ride on the board (the joint via C1), the chip is big enough to fly.
  const ExplodeUnit& board = unit_of(two, d.board);
  CHECK_EQ(board.level, 2);
  CHECK_EQ(two[static_cast<size_t>(board.parent)].id, d.pcb);
  for (const auto& b : {d.cap1, d.cap2, d.solder}) CHECK(holds(board, b));
  CHECK_EQ(unit_of(two, d.chip).level, 2);
  CHECK_NEAR(board.distance, 0, 1e-12);  // the board is nearly the whole PCB: it stays, the chip lifts off it
  CHECK_NEAR(unit_of(two, d.chip).dir[2], 1, 1e-12);
  CHECK_EQ(count_level(two, 2), 2 + 4);
  CHECK_EQ(dump(explode_units(d.doc, s, spec_of({{"levels", 0}}))), dump(two));  // depth 2: every level is two
  // Without attach_small every part is its own unit; with levels 1 the PCB moves whole anyway.
  const auto loose = explode_units(d.doc, s, spec_of({{"levels", 2}, {"attach_small", false}}));
  CHECK_EQ(unit_of(loose, d.cap1).level, 2);
  CHECK_EQ(unit_of(loose, d.led).level, 1);
  const auto one = explode_units(d.doc, s, spec_of({{"levels", 1}}));
  CHECK_EQ(count_level(one, 2), 0);
  CHECK_EQ(unit_of(one, d.pcb).bodies.size(), size_t(5));
  // A size threshold in mm instead of the share of the parent.
  const auto big = explode_units(d.doc, s, spec_of({{"levels", 2}, {"small_size", 20}}));
  CHECK(holds(unit_of(big, d.board), d.chip));
  // keep wins over the level.
  const auto kept = explode_units(d.doc, s, spec_of({{"levels", 0}, {"keep", {d.pcb}}}));
  CHECK_EQ(unit_of(kept, d.pcb).bodies.size(), size_t(5));
  // Deterministic.
  CHECK_EQ(dump(explode_units(d.doc, s, spec_of({{"levels", 2}}))), dump(two));
}

TEST(groups) {
  Device d = device();
  const Scene s = resolve(d.doc);
  const auto units = explode_units(d.doc, s, spec_of({{"levels", 2}, {"groups", json::array({json::array({d.cap1, d.cap2}), json::array({d.lid, d.shell})})}}));
  const ExplodeUnit& caps = unit_of(units, d.cap1);
  CHECK(holds(caps, d.cap2) && holds(caps, d.solder));  // the joint sits on the group's box, the group stays itself
  CHECK_EQ(caps.name, "C1 +1");
  CHECK_EQ(units[static_cast<size_t>(caps.parent)].id, d.pcb);
  const ExplodeUnit& housing = unit_of(units, d.lid);
  CHECK(holds(housing, d.shell) && holds(housing, d.led));
  for (const auto& u : units) CHECK(u.id != d.shell && u.id != d.cap2);
}

TEST(staging_and_manual_offsets) {
  Device d = device();
  const Scene s = resolve(d.doc);
  ExplodeSpec spec = spec_of({{"levels", 1}, {"keep", {d.pcb}}, {"split", {d.screws}}, {"offsets", {{d.lid, {0, 0, 30}}}}});
  const auto units = explode_units(d.doc, s, spec);
  const ExplodeUnit& lid = unit_of(units, d.lid);
  CHECK_NEAR(lid.t0, 0, 1e-12);
  CHECK_NEAR(lid.t1, 0.5, 1e-12);
  CHECK_NEAR(unit_of(units, d.screw[0]).t0, 0.5, 1e-12);
  auto at = [&](double t) { return explode_offsets(units, spec, t); };
  for (const auto& [id, v] : at(0)) CHECK(v == (Vec3{0, 0, 0}));
  CHECK_NEAR(at(1)[d.lid][2], lid.distance + 30, 1e-9);
  CHECK(at(1)[d.led] == at(1)[d.lid]);
  CHECK_NEAR(at(0.25)[d.lid][2], 0.5 * (lid.distance + 30), 1e-9);  // half way through its stage, eased
  // At 0.5 the screws have moved with their folder only; by 1 they have spread from it too.
  const ExplodeUnit& folder = unit_of(units, d.screws);
  const ExplodeUnit& s1 = unit_of(units, d.screw[0]);
  for (int i = 0; i < 3; ++i) {
    const double folderMove = folder.dir[static_cast<size_t>(i)] * folder.distance;
    CHECK_NEAR(at(0.5)[d.screw[0]][static_cast<size_t>(i)], folderMove, 1e-9);
    CHECK_NEAR(at(1)[d.screw[0]][static_cast<size_t>(i)], folderMove + s1.dir[static_cast<size_t>(i)] * s1.distance, 1e-9);
  }
  // together: everything at once; units: one after another, never overlapping.
  spec.stages = "together";
  const auto together = explode_units(d.doc, s, spec);
  for (const auto& u : together) CHECK(u.t0 == 0 && u.t1 == 1);
  spec.stages = "units";
  auto seq = explode_units(d.doc, s, spec);
  std::erase_if(seq, [](const ExplodeUnit& u) { return u.distance == 0; });
  std::sort(seq.begin(), seq.end(), [](const ExplodeUnit& a, const ExplodeUnit& b) { return a.t0 < b.t0; });
  CHECK_EQ(seq.size(), size_t(7));  // the bottom shell stays
  for (size_t i = 1; i < seq.size(); ++i) CHECK(seq[i].t0 >= seq[i - 1].t1 - 1e-12);
  CHECK_EQ(seq.front().level, 1);
  CHECK_EQ(seq.back().level, 2);
}

TEST(axis_and_stack) {
  Device d = device();
  const Scene s = resolve(d.doc);
  const json base = {{"levels", 1}, {"keep", {d.pcb}}, {"split", {d.screws}}};
  json axis = base;
  axis["mode"] = "axis";
  axis["axis"] = {0, 0, 2};
  for (const auto& u : explode_units(d.doc, s, spec_of(axis))) {
    if (u.level != 1) continue;
    CHECK(std::abs(u.dir[0]) < 1e-12 && std::abs(u.dir[1]) < 1e-12 && std::abs(std::abs(u.dir[2]) - 1) < 1e-12);
  }
  CHECK_NEAR(unit_of(explode_units(d.doc, s, spec_of(axis)), d.lid).dir[2], 1, 1e-12);
  // Stack along +Z: screws (lowest) stay, then the shell, the board and the lid each start a gap above what is under them.
  json stack = base;
  stack["mode"] = "stack";
  const ExplodeSpec spec = spec_of(stack);
  const auto units = explode_units(d.doc, s, spec);
  const auto moves = explode_unit_offsets(units, spec, 1);
  auto lowest = [&](const std::string& id) { const size_t i = static_cast<size_t>(&unit_of(units, id) - units.data()); return units[i].lo[2] + moves[i][2]; };
  auto highest = [&](const std::string& id) { const size_t i = static_cast<size_t>(&unit_of(units, id) - units.data()); return units[i].hi[2] + moves[i][2]; };
  CHECK_NEAR(unit_of(units, d.screws).distance, 0, 1e-12);
  CHECK(lowest(d.shell) > highest(d.screws));
  CHECK(lowest(d.pcb) > highest(d.shell));
  CHECK(lowest(d.lid) > highest(d.pcb));
  const double gap = lowest(d.pcb) - highest(d.shell);
  CHECK_NEAR(lowest(d.lid) - highest(d.pcb), gap, 1e-9);
  for (const auto& id : d.screw) CHECK_NEAR(unit_of(units, id).distance, 0, 1e-12);  // side by side: no pile
  json wide = stack;
  wide["spacing"] = 2;
  const auto wider = explode_units(d.doc, s, spec_of(wide));
  CHECK(unit_of(wider, d.lid).distance > unit_of(units, d.lid).distance);
}

TEST(exploded_scene_for_measuring) {
  Device d = device();
  const Scene s = resolve(d.doc);
  const Ref lid{d.lid}, shell{d.shell};
  CHECK_NEAR(measure_distance(d.doc, s, lid, shell).value("value", -1.0), 0, 1e-6);
  const ExplodeSpec spec = spec_of({{"levels", 1}, {"keep", {d.pcb}}, {"split", {d.screws}}});
  const auto units = explode_units(d.doc, s, spec);
  const auto offsets = explode_offsets(units, spec, 1);
  const Scene x = exploded_scene(s, offsets);
  const double gap = unit_of(units, d.lid).distance + unit_of(units, d.shell).distance;
  CHECK_NEAR(measure_distance(d.doc, x, lid, shell).value("value", -1.0), gap, 1e-6);
  // A body under a moved component (the screws sit 2 mm down) moves by exactly its world offset.
  const Vec3 before = s.world(d.screw[0]).apply({0, 0, 0}), after = x.world(d.screw[0]).apply({0, 0, 0});
  for (size_t i = 0; i < 3; ++i) CHECK_NEAR(after[i] - before[i], offsets.at(d.screw[0])[i], 1e-9);
  CHECK(s.node(d.screw[0])->local.at(2, 3) == 0);  // the resolved scene is untouched
  // ... and under a turned, translated parent the move is still in world space.
  Document doc = Document::create();
  const std::string part = new_uuid(), turned = new_uuid();
  Mat4 m = Mat4::translation(10, 0, 0);
  m.at(0, 0) = 0; m.at(0, 1) = -1; m.at(1, 0) = 1; m.at(1, 1) = 0;
  doc.append({{"op", "import"}, {"source", "test"}, {"units", "mm"},
              {"nodes", json::array({component(turned, "Turned", json::array({body(part, "Part", box(doc, 0, 0, 0, 5, 5, 5), Mat4::translation(1, 2, 3))}), m)})}});
  const Scene t = resolve(doc);
  const Scene tx = exploded_scene(t, {{part, Vec3{1, 2, 3}}});
  const Vec3 p0 = t.world(part).apply({1, 1, 1}), p1 = tx.world(part).apply({1, 1, 1});
  CHECK_NEAR(p1[0] - p0[0], 1, 1e-12);
  CHECK_NEAR(p1[1] - p0[1], 2, 1e-12);
  CHECK_NEAR(p1[2] - p0[2], 3, 1e-12);
}

TEST(view_op_and_commands) {
  Device d = device();
  const json made = commands::run("explode", {{"levels", 1}, {"keep", d.pcb}, {"split", json::array({d.screws})}, {"name", "Exploded"}}, &d.doc);
  CHECK_EQ(made["root"], d.device);
  CHECK_EQ(made["depth"], 2);
  CHECK_EQ(made["stages"], 2);
  CHECK_EQ(made["units"].size(), size_t(8));
  CHECK(!made.contains("warnings"));
  CHECK(made["offsets"].contains(d.cap1) && made["offsets"].contains(d.led));
  const std::string view = made["id"];
  const Op* op = d.doc.find_op(view);
  CHECK(op && op->type == "view" && op->data["explode"]["keep"] == json::array({d.pcb}));
  CHECK(op->data.contains("camera"));
  // Listed with the views; an update is one edit op; the CLI reads the view back.
  CHECK_EQ(commands::run("annotations", json::object(), &d.doc)["views"][0]["explode"]["split"], json::array({d.screws}));
  const size_t ops = d.doc.ops.size();
  const json updated = commands::run("explode", {{"view", view}, {"levels", 2}, {"offsets", {{d.lid, {0, 0, 10}}}}, {"update", true}}, &d.doc);
  CHECK_EQ(d.doc.ops.size(), ops + 1);
  CHECK_EQ(d.doc.ops.back().type, "edit");
  CHECK_EQ(d.doc.ops.back().data["target"], view);
  const ExplodeSpec saved = view_explode(resolve(d.doc), view);
  CHECK_EQ(saved.levels, 2);
  CHECK(saved.keep.count(d.pcb));  // the rest of the view's explode is kept
  CHECK(saved.offsets.at(d.lid) == (Vec3{0, 0, 10}));
  CHECK_EQ(updated["explode"]["levels"], 2);
  const json read = commands::run("explode", {{"view", view}, {"t", 0}}, &d.doc);
  CHECK_EQ(d.doc.ops.size(), ops + 1);  // a read appends nothing
  CHECK(read["offsets"].empty());
  // Measuring in the view; pinning stays on the assembled model.
  const json refs = json::array({d.lid, d.shell});
  CHECK_NEAR(commands::run("measure", {{"kind", "distance"}, {"refs", refs}}, &d.doc)["value"].get<double>(), 0, 1e-6);
  CHECK(commands::run("measure", {{"kind", "distance"}, {"refs", refs}, {"explode", view}}, &d.doc)["value"].get<double>() > 50);
  CHECK(commands::run("measure", {{"kind", "distance"}, {"refs", refs}, {"explode", {{"levels", 1}, {"mode", "stack"}}}}, &d.doc)["value"].get<double>() > 10);
  CHECK_THROWS(commands::run("measure", {{"kind", "distance"}, {"refs", refs}, {"explode", view}, {"pin", true}}, &d.doc));
  // A plain camera bookmark is not an exploded view; a view command may carry one.
  const std::string plain = commands::run("view", {{"name", "Front"}}, &d.doc)["id"];
  CHECK_THROWS(commands::run("measure", {{"kind", "distance"}, {"refs", refs}, {"explode", plain}}, &d.doc));
  const std::string given = commands::run("view", {{"name", "Stack"}, {"explode", {{"mode", "stack"}, {"t", 0.5}}}}, &d.doc)["id"];
  CHECK_EQ(view_explode(resolve(d.doc), given).t, 0.5);
  // A picture of it (headless render) differs from the assembled one.
  const auto dir = std::filesystem::temp_directory_path();
  const std::string a = (dir / "opad-explode-assembled.png").string(), b = (dir / "opad-explode-view.png").string();
  commands::run("render", {{"out", a}, {"width", 320}, {"height", 240}}, &d.doc);
  commands::run("render", {{"out", b}, {"width", 320}, {"height", 240}, {"explode", view}}, &d.doc);
  CHECK(read_text_file(a) != read_text_file(b));
  std::printf("exploded render: %s\n", b.c_str());
  d.doc.save_as(dir / "opad-explode.opad");  // for trying the CLI and older builds on it
  // Bad input says what is wrong; unknown ids are warnings, not errors.
  CHECK_THROWS(commands::run("explode", {{"mode", "sideways"}}, &d.doc));
  CHECK_THROWS(commands::run("explode", {{"levels", -1}}, &d.doc));
  CHECK_THROWS(commands::run("explode", {{"t", 2}}, &d.doc));
  CHECK_THROWS(commands::run("explode", {{"root", d.lid}}, &d.doc));
  CHECK_THROWS(commands::run("explode", {{"update", true}}, &d.doc));
  CHECK_THROWS(commands::run("explode", {{"view", new_uuid()}}, &d.doc));
  CHECK_EQ(commands::run("explode", {{"keep", new_uuid()}}, &d.doc)["warnings"].size(), size_t(1));
  // root: explode only the PCB.
  const json board = commands::run("explode", {{"root", d.pcb}, {"attach_small", false}}, &d.doc);
  CHECK_EQ(board["root"], d.pcb);
  CHECK_EQ(board["units"].size(), size_t(5));
}

TEST(format_round_trip_and_older_readers) {
  Device d = device();
  const std::string view = commands::run("explode", {{"levels", 2}, {"groups", json::array({json::array({d.cap1, d.cap2})})}, {"name", "Exploded"}}, &d.doc)["id"];
  commands::run("explode", {{"view", view}, {"mode", "axis"}, {"update", true}}, &d.doc);
  const std::string text = d.doc.serialize();
  const Document back = Document::parse(text);
  CHECK_EQ(back.serialize(), text);
  const ExplodeSpec spec = view_explode(resolve(back), view);
  CHECK_EQ(spec.mode, "axis");
  CHECK_EQ(spec.groups.size(), size_t(1));
  // The explode object is one line (no sketch-style records) and nothing but an optional field of view and edit ops,
  // which every reader validates by name and camera only: an older build opens the file as a camera bookmark.
  for (const auto& op : back.ops)
    if (op.type == "view" || op.type == "edit") CHECK(op.raw.find('\n') == std::string::npos);
  json old = back.find_op(view)->data;
  Document::validate_op(old);
  old.erase("explode");
  Document::validate_op(old);
  // Readers ignore keys they do not know, in the op and in the explode object (a later build's additions).
  json later = back.find_op(view)->data;
  later["explode"]["future"] = {{"anything", 1}};
  later["display"] = {{"style", "shaded"}};
  later["id"] = new_uuid();
  Document copy = Document::parse(text);
  copy.append(later);
  const Scene s = resolve(copy);
  CHECK(s.unresolved.empty());
  CHECK_EQ(view_explode(s, later["id"]).groups.size(), size_t(1));  // the op as first written, before the edit
}

TEST(flat_import_parts_ride_on_the_board) {
  // A flat KiCad-style import: board, 300 passives on it, a connector and a standoff under it, all siblings. Boxes from
  // the caller (no geometry walked), so this also shows the cost of a large flat level.
  Document doc = Document::create();
  const std::string key = doc.add_body("DBRep_DrawableShape\n\nCASCADE Topology V1, (c) Matra-Datavision\nLocations 0\n", json::object());
  json nodes = json::array();
  std::unordered_map<std::string, Box3> boxes;
  auto add = [&](const std::string& name, Vec3 lo, Vec3 hi) {
    const std::string id = new_uuid();
    nodes.push_back(body(id, name, key));
    boxes[id] = {lo, hi};
    return id;
  };
  const std::string board = add("Board", {0, 0, 0}, {100, 80, 1.6});
  for (int i = 0; i < 300; ++i) add("R" + std::to_string(i), {2.0 + (i % 30) * 3, 2.0 + (i / 30) * 7, 1.6}, {3.6 + (i % 30) * 3, 2.8 + (i / 30) * 7, 2.1});
  const std::string usb = add("USB", {95, 30, 1.6}, {107, 40, 6});
  const std::string standoff = add("Standoff", {4, 4, -6}, {7, 7, 0});
  doc.append({{"op", "import"}, {"source", "kicad"}, {"units", "mm"}, {"nodes", json::array({component(new_uuid(), "Board assembly", nodes)})}});
  const Scene s = resolve(doc);
  const ExplodeBoxFn box_of = [&](const std::string& id) {
    Bnd_Box b;
    const auto& x = boxes.at(id);
    b.Update(x.lo[0], x.lo[1], x.lo[2], x.hi[0], x.hi[1], x.hi[2]);
    return b;
  };
  const auto t0 = std::chrono::steady_clock::now();
  const auto units = explode_units(doc, s, spec_of({{"levels", 1}}), box_of);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("flat level of 303 parts: %.1f ms\n", ms);
  CHECK_EQ(units.size(), size_t(3));  // the board with its passives, the connector, the standoff
  CHECK_EQ(unit_of(units, board).bodies.size(), size_t(301));
  CHECK_EQ(unit_of(units, usb).bodies.size(), size_t(1));
  CHECK_EQ(unit_of(units, standoff).bodies.size(), size_t(1));  // 7.3 mm across: not small next to a 128 mm board
  // Every frame of a play: thousands of units cost microseconds.
  std::vector<ExplodeUnit> many(5000);
  for (size_t i = 0; i < many.size(); ++i) {
    many[i].id = "u" + std::to_string(i);
    many[i].bodies = {"b" + std::to_string(i)};
    many[i].parent = i < 50 ? -1 : static_cast<int>(i % 50);
    many[i].distance = 10;
    many[i].t1 = 1;
  }
  const ExplodeSpec spec;
  const auto p0 = std::chrono::steady_clock::now();
  for (int k = 0; k < 20; ++k) explode_offsets(many, spec, k / 19.0);
  const double per = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count() / 20;
  std::printf("explode_offsets, 5000 units: %.3f ms per frame\n", per);
  CHECK(per < 8);
}

TEST(spec_json) {
  const std::string pcb = new_uuid();
  ExplodeSpec s = ExplodeSpec::from_json({{"levels", 0}, {"mode", "stack"}, {"axis", {1, 0, 0}}, {"keep", {pcb}}, {"offsets", {{pcb, {1, 2, 3}}}}, {"stages", "units"}, {"t", 0.25}});
  CHECK_EQ(ExplodeSpec::from_json(s.to_json()).to_json(), s.to_json());
  CHECK_EQ(s.to_json()["t"], 0.25);
  const json plain = ExplodeSpec{}.to_json();
  CHECK(!plain.contains("axis") && !plain.contains("keep") && !plain.contains("stages") && !plain.contains("duration"));
  CHECK_THROWS(ExplodeSpec::from_json(json::array()));
  CHECK_THROWS(ExplodeSpec::from_json({{"axis", {0, 0, 0}}}));
  CHECK_THROWS(ExplodeSpec::from_json({{"keep", "abc"}}));
  CHECK_THROWS(ExplodeSpec::from_json({{"groups", json::array({json::array()})}}));
  CHECK_THROWS(ExplodeSpec::from_json({{"offsets", {{"a", {1, 2}}}}}));
  CHECK_THROWS(ExplodeSpec::from_json({{"stages", "random"}}));
  CHECK_THROWS(ExplodeSpec::from_json({{"spacing", -1}}));
  CHECK_EQ(ExplodeSpec::from_json({{"unknown", true}}).levels, 1);
}

// What the Explode panel (UI-36) edits: trails, the unit a click lands in, a drag along an axis, keep/split, groups.
TEST(editor_helpers) {
  Device d = device();
  const Scene s = resolve(d.doc);
  ExplodeSpec spec = spec_of({{"levels", 1}, {"keep", {d.pcb}}, {"split", {d.screws}}});
  auto units = explode_units(d.doc, s, spec);
  auto index = [&](const std::string& id) { return explode_unit_of(s, units, id); };
  // A body finds its unit, a kept component is its own, a body riding on another part finds that part's unit.
  CHECK_EQ(units[static_cast<size_t>(index(d.chip))].id, d.pcb);
  CHECK_EQ(units[static_cast<size_t>(index(d.pcb))].id, d.pcb);
  CHECK_EQ(units[static_cast<size_t>(index(d.led))].id, d.lid);
  CHECK_EQ(units[static_cast<size_t>(index(d.screw[1]))].id, d.screw[1]);
  CHECK_EQ(index(d.device), -1);  // the explode root: its bodies are in many units
  CHECK_EQ(index("nothing"), -1);
  // Trails: none assembled; at t = 1 one per moving unit, a screw's from where its folder alone takes it.
  CHECK(explode_trails(units, spec, 0).empty());
  const auto trails = explode_trails(units, spec, 1);
  const auto moves = explode_unit_offsets(units, spec, 1);
  size_t moving = 0;
  for (size_t i = 0; i < units.size(); ++i) {
    const Vec3 base = units[i].parent >= 0 ? moves[static_cast<size_t>(units[i].parent)] : Vec3{0, 0, 0};
    moving += std::hypot(moves[i][0] - base[0], moves[i][1] - base[1], moves[i][2] - base[2]) > 1e-6;
  }
  CHECK_EQ(trails.size(), moving);
  const size_t s1 = static_cast<size_t>(index(d.screw[0])), folder = static_cast<size_t>(index(d.screws));
  for (const auto& tr : trails)
    if (tr.unit == s1)
      for (size_t k = 0; k < 3; ++k) {
        CHECK_NEAR(tr.from[k], units[s1].centre[k] + moves[folder][k], 1e-9);
        CHECK_NEAR(tr.to[k], units[s1].centre[k] + moves[s1][k], 1e-9);
      }
  // A drag along +Z sets the lid's own move there and keeps the rest of it; back to the automatic move drops the offset.
  const ExplodeUnit& lid = units[static_cast<size_t>(index(d.lid))];
  CHECK_NEAR(explode_progress(lid, 0.25), 0.5, 1e-12);  // half way through its stage (level 1 of 2), eased
  CHECK_NEAR(explode_progress(lid, 0.6), 1, 1e-12);
  CHECK_NEAR(explode_progress(units[s1], 0.25), 0, 1e-12);
  const double automatic = explode_travel(lid, spec, {0, 0, 1});
  CHECK_NEAR(automatic, lid.distance * lid.dir[2], 1e-9);
  set_explode_travel(spec, lid, {0, 0, 2}, automatic + 15);
  CHECK_NEAR(explode_travel(lid, spec, {0, 0, 1}), automatic + 15, 1e-9);
  CHECK_NEAR(spec.offsets.at(d.lid)[2], 15, 1e-9);
  set_explode_travel(spec, lid, {1, 0, 0}, 10);  // across: the Z part stays
  CHECK_NEAR(spec.offsets.at(d.lid)[0], 10 - lid.distance * lid.dir[0], 1e-9);
  CHECK_NEAR(spec.offsets.at(d.lid)[2], 15, 1e-9);
  set_explode_travel(spec, lid, {1, 0, 0}, lid.distance * lid.dir[0]);
  set_explode_travel(spec, lid, {0, 0, 1}, automatic);
  CHECK(!spec.offsets.count(d.lid));
  // Keep / split / follow the level.
  CHECK(explode_rule(spec, d.pcb) == ExplodeRule::Keep && explode_rule(spec, d.screws) == ExplodeRule::Split && explode_rule(spec, d.device) == ExplodeRule::Level);
  set_explode_rule(spec, d.pcb, ExplodeRule::Split);
  CHECK(!spec.keep.count(d.pcb) && spec.split.count(d.pcb));
  set_explode_rule(spec, d.pcb, ExplodeRule::Level);
  CHECK(!spec.keep.count(d.pcb) && !spec.split.count(d.pcb));
  // Groups: two screws move as one; grouping one of them again takes it out of the first group, which then goes.
  spec.offsets[d.screw[1]] = {1, 0, 0};
  explode_group(spec, {d.screw[0], d.screw[1], d.screw[0]});
  CHECK_EQ(spec.groups.size(), size_t(1));
  CHECK_EQ(spec.groups[0], (std::vector<std::string>{d.screw[0], d.screw[1]}));
  CHECK(!spec.offsets.count(d.screw[1]));
  units = explode_units(d.doc, s, spec);
  CHECK_EQ(index(d.screw[1]), index(d.screw[0]));
  CHECK_EQ(explode_group_of(spec, d.screw[1]), 0);
  explode_group(spec, {d.screw[1], d.screw[2]});
  CHECK_EQ(spec.groups.size(), size_t(1));
  CHECK_EQ(spec.groups[0], (std::vector<std::string>{d.screw[1], d.screw[2]}));
  explode_group(spec, {d.screw[3]});  // one node is no group
  CHECK_EQ(spec.groups.size(), size_t(1));
  spec.offsets[d.screw[1]] = {0, 0, 5};
  CHECK(explode_ungroup(spec, d.screw[2]) && spec.groups.empty() && !spec.offsets.count(d.screw[1]));
  CHECK(!explode_ungroup(spec, d.screw[2]));
}

CHECK_MAIN()
