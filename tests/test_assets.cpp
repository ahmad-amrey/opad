// Linked assets (assets.hpp) from files written here: a STEP of two boxes and a small KiCad board. Linking registers the
// bodies without storing them; a reopened document reads them from the file (also as an older build sees it: listed,
// missing); a missing or untrusted file leaves only its bodies missing; a changed file syncs as one edit of the import with
// node ids and unchanged keys kept, and what depends on it regenerates; embed and pack; paths follow Save As.
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Writer.hxx>
#include <TopoDS_Compound.hxx>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "check.hpp"
#include "opad/assets.hpp"
#include "opad/commands.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/kicad_pcb.hpp"
#include "../core/src/import_common.hpp"  // the content cache, as a slow read fills it

using namespace opad;
namespace fs = std::filesystem;

namespace {

// Removes a folder entry by entry, retrying a file another process holds for a moment, and gives up after that: libstdc++'s
// remove_all (GCC 15, Windows) spun for ever inside embed_and_pack about once in twenty runs.
void remove_tree(const fs::path& dir) {
  std::error_code e;
  std::vector<fs::path> files, dirs;
  for (auto it = fs::recursive_directory_iterator(dir, e); !e && it != fs::recursive_directory_iterator(); it.increment(e))
    (it->is_directory(e) ? dirs : files).push_back(it->path());
  std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) { return a.native().size() > b.native().size(); });
  dirs.push_back(dir);
  for (const auto* list : {&files, &dirs})
    for (const auto& p : *list)
      for (int attempt = 0; attempt < 50 && !fs::remove(p, e) && fs::exists(p, e); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

struct Files {
  fs::path dir = fs::temp_directory_path() / ("opad-assets-" + new_uuid());
  Files() { fs::create_directories(dir); }
  ~Files() { remove_tree(dir); }
};

// Two boxes in one STEP: A (10 mm, its corner at (x, 5, z)) and B (5 mm, far off), two roots named by the writer's counter
// (another name in every file written: the nodes are known by their place).
void two_boxes(const fs::path& p, double x, double z = 30, bool swapped = false) {
  fs::create_directories(p.parent_path());
  BRep_Builder b;
  TopoDS_Compound c;
  b.MakeCompound(c);
  const TopoDS_Shape a = BRepPrimAPI_MakeBox(gp_Pnt(x, 5, z), 10, 10, 10).Shape(), small = BRepPrimAPI_MakeBox(gp_Pnt(100, 100, 0), 5, 5, 5).Shape();
  b.Add(c, swapped ? small : a);
  b.Add(c, swapped ? a : small);
  STEPControl_Writer w;
  w.Transfer(c, STEPControl_AsIs);
  const auto u8 = p.u8string();
  CHECK(w.Write(std::string(u8.begin(), u8.end()).c_str()) == IFSelect_RetDone);
}

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

const Op& last_import(const Document& d) {
  for (auto it = d.ops.rbegin(); it != d.ops.rend(); ++it)
    if (it->type == "import") return *it;
  throw check::Failure("no import");
}

double volume(const Document& d, const Scene& s, const std::string& id) {
  GProp_GProps g;
  BRepGProp::VolumeProperties(node_world_shape(d, s, id), g);
  return g.Mass();
}

// The linked bodies in the order of the file: A, B.
std::string linked(const Scene& s, size_t i) {
  std::vector<std::string> out;
  for (const auto& id : s.all_bodies())
    if (s.node(id)->linked) out.push_back(id);
  if (i >= out.size()) throw check::Failure("no linked body " + std::to_string(i));
  return out[i];
}

std::string body_named(const Scene& s, const std::string& suffix) {
  for (const auto& id : s.all_bodies())
    if (s.node(id)->name.size() >= suffix.size() && s.node(id)->name.compare(s.node(id)->name.size() - suffix.size(), suffix.size(), suffix) == 0) return id;
  std::string names;
  for (const auto& id : s.all_bodies()) names += " '" + s.node(id)->name + "'";
  throw check::Failure("no body named *" + suffix + ":" + names);
}

bool about(double a, double b, double tol = 1e-3) { return std::abs(a - b) <= tol; }

}  // namespace

TEST(link_keeps_bodies_out_of_the_store) {
  Files f;
  const fs::path step = f.dir / "parts" / "model.step";
  two_boxes(step, 5);
  Document d = Document::create();
  d.path = f.dir / "design.opad";
  ImportOptions o;
  const ImportResult r = link_file(d, step, o);
  CHECK_EQ(r.bodies, 2);
  CHECK(r.info["linked"].get<bool>());
  const json& a = last_import(d).data["asset"];
  CHECK_EQ(a["kind"], "step");
  CHECK_EQ(a["path"], "parts/model.step");
  CHECK_EQ(a["storage"], "linked");
  CHECK_EQ(a["sha256"], sha256_file(step));
  CHECK_EQ(a["sha256"], file_sha256(step));  // remembered by path, size and time: the same answer
  const Scene s = resolve(d);
  CHECK_EQ(s.all_bodies().size(), 2u);
  for (const auto& id : s.all_bodies()) {
    CHECK(!s.node(id)->body_missing && s.node(id)->linked);
    CHECK(d.body(s.node(id)->body_key)->external);
  }
  CHECK(about(volume(d, s, linked(s, 0)), 1000));
  CHECK(!d.has_live_bodies());
  const std::string text = d.serialize();
  CHECK(text.find("#body ") == std::string::npos);  // nothing of the asset in the store
  // The same file linked again: the same keys (derived from the content), other node ids (another import).
  Document again = Document::create();
  link_file(again, step);
  const Scene s2 = resolve(again);
  CHECK_EQ(s2.node(linked(s2, 0))->body_key, s.node(linked(s, 0))->body_key);
  CHECK(linked(s2, 0) != linked(s, 0));
}

TEST(reopen_reads_the_file_and_old_builds_list_it) {
  Files f;
  const fs::path step = f.dir / "model.step";
  two_boxes(step, 5);
  Document d = Document::create();
  import_brep(d, brep_from_shape(BRepPrimAPI_MakeBox(2, 2, 2).Shape()), "Own");  // an ordinary, stored body beside it
  d.save_as(f.dir / "design.opad");
  link_file(d, step);
  const std::string rename = resolve(d).roots.back();
  d.append({{"op", "rename"}, {"target", rename}, {"name", "Bought part"}});
  d.save();
  // As a build without assets reads it: the components are there, the asset's bodies missing, the rest whole.
  Document old = Document::parse(read_text_file(f.dir / "design.opad"));
  const Scene before = resolve(old);
  int missing = 0;
  for (const auto& id : before.all_bodies()) missing += before.node(id)->body_missing;
  CHECK_EQ(missing, 2);
  CHECK_EQ(before.node(rename)->name, "Bought part");
  CHECK(before.unresolved.size() == 2 && before.unresolved[0].reason.find("model.step") != std::string::npos);
  CHECK_EQ(old.serialize(), read_text_file(f.dir / "design.opad"));  // and writes it back as it was
  // This build reads the file.
  Document reopened = Document::load(f.dir / "design.opad");
  const auto states = load_assets(reopened);
  CHECK_EQ(states.size(), 1u);
  CHECK_EQ(states[0].state, "ok");
  CHECK_EQ(states[0].unbound, 0);
  const Scene s = resolve(reopened);
  for (const auto& id : s.all_bodies()) CHECK(!s.node(id)->body_missing);
  CHECK(s.unresolved.empty());
  CHECK(about(volume(reopened, s, linked(s, 1)), 125));
  CHECK_EQ(reopened.serialize(), read_text_file(f.dir / "design.opad"));
  CHECK(load_assets(reopened)[0].state == "ok");  // loaded already: nothing read again
  // node_properties says where it comes from
  const json props = node_properties(reopened, s, linked(s, 0), false);
  CHECK(props["linked"].get<bool>() && props["linked_file"] == "model.step");
}

TEST(missing_untrusted_and_network_files) {
  Files f;
  const fs::path elsewhere = f.dir / "elsewhere" / "model.step";
  two_boxes(elsewhere, 5);
  fs::create_directories(f.dir / "project");
  Document d = Document::create();
  import_brep(d, brep_from_shape(BRepPrimAPI_MakeBox(2, 2, 2).Shape()), "Own");
  d.save_as(f.dir / "project" / "design.opad");
  link_file(d, elsewhere);  // the user picked it: read now
  CHECK_EQ(last_import(d).data["asset"]["path"], "../elsewhere/model.step");
  d.save();
  // Reopened: outside the document's folder (no git work tree around it) -> not read without trust.
  Document reopened = Document::load(f.dir / "project" / "design.opad");
  auto states = load_assets(reopened);
  CHECK_EQ(states[0].state, "untrusted");
  CHECK_EQ(states[0].unbound, 2);
  Scene s = resolve(reopened);
  int missing = 0;
  for (const auto& id : s.all_bodies()) missing += s.node(id)->body_missing;
  CHECK_EQ(missing, 2);  // the stored body is fine
  AssetOptions trust;
  trust.trusted = {f.dir / "elsewhere"};
  CHECK_EQ(load_assets(reopened, trust)[0].state, "ok");
  s = resolve(reopened);
  for (const auto& id : s.all_bodies()) CHECK(!s.node(id)->body_missing);
  // A git work tree around both makes the file part of the project.
  fs::create_directories(f.dir / ".git");
  Document in_repo = Document::load(f.dir / "project" / "design.opad");
  CHECK_EQ(load_assets(in_repo)[0].state, "ok");
  remove_tree(f.dir / ".git");
  // Gone: missing, the document still opens.
  fs::remove(elsewhere);
  Document gone = Document::load(f.dir / "project" / "design.opad");
  states = load_assets(gone, trust);
  CHECK_EQ(states[0].state, "missing");
  CHECK(resolve(gone).unresolved.size() == 2);
  // A network path is never looked at unless trusted by name.
  const json remote = {{"abs", "//server/share/model.step"}, {"kind", "step"}};
  CHECK(locate_asset(gone, remote).empty());
  CHECK(!asset_trusted(gone, path_from_utf8("//server/share/model.step")));
#ifdef _WIN32
  CHECK(!asset_trusted(Document::create(), path_from_utf8("//server/share/model.step")));
  Document shared = Document::create();
  shared.append({{"op", "import"}, {"source", "model.step"}, {"nodes", json::array()}, {"asset", remote}});
  CHECK_EQ(asset_status(shared)[0].state, "untrusted");  // reported, not looked at
  CHECK(asset_trusted(shared, path_from_utf8("//server/share/model.step"), AssetOptions{{path_from_utf8("//server/share")}}));
#endif
}

TEST(sync_keeps_ids_and_regenerates_what_depends) {
  Files f;
  const fs::path step = f.dir / "model.step";
  two_boxes(step, 5);
  Document d = Document::create();
  d.save_as(f.dir / "design.opad");
  link_file(d, step);
  const std::string import_id = last_import(d).id;
  Scene s = resolve(d);
  const std::string a = linked(s, 0), b = linked(s, 1);
  const std::string key_a = s.node(a)->body_key, key_b = s.node(b)->body_key;
  // A design around it: a 40 mm block with box A cut out of it (A stays).
  design::apply_ops(d, {design::make_feature_op("box", "Block", {{"length", 40}, {"width", 40}, {"height", 40}})});
  s = resolve(d);
  const std::string block = s.features.back().result["bodies"][0]["id"];
  design::apply_ops(d, {design::make_feature_op("combine", "Pocket", {{"target", {block}}, {"tools", {a}}, {"operation", "cut"}, {"keep_tools", true}})});
  d.append({{"op", "appearance"}, {"target", a}, {"color", {1, 0, 0}}});
  s = resolve(d);
  const double carved = volume(d, s, block);
  CHECK(about(carved, 64000 - 1000));
  d.save();
  // Nothing changed: up to date, no ops.
  design::Plan none = plan_asset_sync(d, import_id);
  CHECK(none.ops.empty() && none.report["up_to_date"].get<bool>());
  // Reopened with another file of the same shapes: "changed", yet every part is as synced (its geometry gives its key).
  AssetOptions uncached;
  uncached.cache = false;  // the version synced is not remembered
  std::ofstream(step, std::ios::app) << "\n";
  Document reopened = Document::load(f.dir / "design.opad");
  AssetState st = load_assets(reopened, uncached)[0];
  CHECK_EQ(st.state, "changed");
  CHECK(!reopened.body(key_a)->meta.value("stale", false) && !reopened.body(key_b)->meta.value("stale", false));
  CHECK(st.reason.find("as synced") != std::string::npos);
  two_boxes(step, 5, 38);  // A up 8 mm: 2 mm of it left in the block; B as it was
  // Without the version synced at hand B still keeps its key; A, read as it is now, is marked stale.
  CHECK_EQ(plan_asset_sync(reopened, import_id, uncached).report["kept"], 1);
  Document later = Document::load(f.dir / "design.opad");
  st = load_assets(later, uncached)[0];
  CHECK(later.body(key_a)->meta.value("stale", false) && !later.body(key_b)->meta.value("stale", false));
  CHECK(st.reason.find("1 parts differ") != std::string::npos);
  design::Plan from_later = plan_asset_sync(later, import_id, uncached);
  CHECK_EQ(from_later.report["kept"], 1);
  CHECK_EQ(from_later.report["regenerated"].size(), 1u);
  // The session that read the version synced keeps B's key.
  const size_t ops = d.ops.size();
  design::Plan plan = plan_asset_sync(d, import_id);
  CHECK(!plan.report["up_to_date"].get<bool>());
  CHECK_EQ(plan.report["kept"], 1);
  CHECK_EQ(plan.report["changed"].size(), 1u);
  CHECK(plan.report["added"].empty() && plan.report["removed"].empty());
  CHECK_EQ(plan.report["regenerated"].size(), 1u);  // the pocket
  // What a sync preview says of it (UI-134): A changed by node, B kept; the pocket recomputed with another block.
  const json parts = asset_sync_parts(d, import_id, plan), affects = asset_sync_affects(d, import_id, plan);
  CHECK(parts["changed"].size() == 1 && parts["changed"][0]["node"] == a && parts["added"].empty() && parts["removed"].empty() && parts["kept"] == 1);
  CHECK(affects["sketches"].empty() && affects["features"].size() == 1 && affects["features"][0]["name"] == "Pocket" && affects["features"][0]["bodies_changed"] == 1);
  design::commit(d, std::move(plan));
  CHECK_EQ(d.ops.size(), ops + 2);  // one edit of the import, one regen
  CHECK_EQ(d.ops[ops].type, "edit");
  CHECK_EQ(d.ops[ops].data["target"], import_id);
  s = resolve(d);
  CHECK(s.node(a) && s.node(b));  // the same nodes
  CHECK(s.node(a)->body_key != key_a && s.node(b)->body_key == key_b);
  CHECK(s.node(a)->has_color && s.node(a)->color[0] == 1);  // the colour given before still applies
  CHECK(about(volume(d, s, block), 64000 - 200));
  CHECK(s.unresolved.empty());
  CHECK_EQ(asset_of(d, import_id)["sha256"], sha256_file(step));
  // Saved and opened again: the file is the one synced.
  d.save();
  Document third = Document::load(f.dir / "design.opad");
  CHECK_EQ(load_assets(third)[0].state, "ok");
  s = resolve(third);
  CHECK(about(volume(third, s, a), 1000) && s.unresolved.empty());
  CHECK(third.body(s.node(a)->body_key)->external);
  // Moved elsewhere: sync from the new place only updates where it is.
  fs::create_directories(f.dir / "moved");
  fs::rename(step, f.dir / "moved" / "model.step");
  design::Plan relocate = plan_asset_sync(third, import_id, {}, f.dir / "moved" / "model.step");
  CHECK(relocate.report["up_to_date"].get<bool>() && relocate.ops.size() == 1);
  design::commit(third, std::move(relocate));
  CHECK_EQ(asset_of(third, import_id)["path"], "moved/model.step");
}

// The same parts written the other way round (roots are known by position): each is found again by its geometry, so ids,
// keys and what was made from them stay as they were.
TEST(reordered_parts_found_by_geometry) {
  Files f;
  const fs::path step = f.dir / "model.step";
  two_boxes(step, 5);
  Document d = Document::create();
  d.save_as(f.dir / "design.opad");
  link_file(d, step);
  const std::string import_id = last_import(d).id;
  Scene s = resolve(d);
  const std::string a = linked(s, 0), b = linked(s, 1);
  const std::string key_a = s.node(a)->body_key, key_b = s.node(b)->body_key;
  d.append({{"op", "rename"}, {"target", a}, {"name", "Big box"}});
  d.save();
  two_boxes(step, 5, 30, true);
  AssetOptions uncached;
  uncached.cache = false;
  Document reopened = Document::load(f.dir / "design.opad");
  CHECK_EQ(load_assets(reopened, uncached)[0].state, "changed");
  s = resolve(reopened);
  CHECK(about(volume(reopened, s, a), 1000) && about(volume(reopened, s, b), 125));
  for (const auto& entry : reopened.bodies()) CHECK(!entry.meta.value("stale", false));
  design::Plan plan = plan_asset_sync(reopened, import_id, uncached);
  CHECK_EQ(plan.report["kept"], 2);
  CHECK(plan.report["changed"].empty() && plan.report["added"].empty() && plan.report["removed"].empty());
  design::commit(reopened, std::move(plan));
  s = resolve(reopened);
  CHECK(s.node(a)->body_key == key_a && s.node(b)->body_key == key_b && s.node(a)->name == "Big box");
}

// A part come in front of one found again by its geometry takes no id another part holds (roots are placed by position:
// the new part's place was the old one's; it gave two nodes one id, and one part vanished): two bodies, two ids, the old kept.
TEST(new_part_in_front_takes_a_free_id) {
  Files f;
  const fs::path obj = f.dir / "parts.obj";
  auto tetra = [](double x, double size, int first) {  // one object named Part
    std::string t = "o Part\n";
    for (const auto& [dx, dy, dz] : std::vector<std::array<double, 3>>{{0, 0, 0}, {size, 0, 0}, {0, size, 0}, {0, 0, size}})
      t += "v " + std::to_string(x + dx) + " " + std::to_string(dy) + " " + std::to_string(dz) + "\n";
    for (const auto& [i, j, k] : std::vector<std::array<int, 3>>{{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}})
      t += "f " + std::to_string(first + i) + " " + std::to_string(first + j) + " " + std::to_string(first + k) + "\n";
    return t;
  };
  write(obj, tetra(0, 10, 1));
  Document d = Document::create();
  d.save_as(f.dir / "design.opad");
  link_file(d, obj);
  const std::string import_id = last_import(d).id;
  Scene s = resolve(d);
  const std::string a = linked(s, 0), key = s.node(a)->body_key;
  write(obj, tetra(50, 7, 1) + tetra(0, 10, 5));
  design::Plan plan = plan_asset_sync(d, import_id);
  CHECK(plan.report["added"].size() == 1 && plan.report["changed"].empty() && plan.report["removed"].empty() && plan.report["kept"] == 1);
  design::commit(d, std::move(plan));
  s = resolve(d);
  std::set<std::string> ids;
  for (const auto& id : s.all_bodies())
    if (s.node(id)->linked) ids.insert(id);
  CHECK_EQ(ids.size(), 2u);
  CHECK(ids.count(a) && s.node(a)->body_key == key);
  CHECK(s.unresolved.empty());
}

TEST(linked_parts_are_read_only) {
  Files f;
  const fs::path step = f.dir / "model.step";
  two_boxes(step, 5);
  Document d = Document::create();
  d.save_as(f.dir / "design.opad");
  link_file(d, step);
  Scene s = resolve(d);
  const std::string a = linked(s, 0), b = linked(s, 1);
  const std::string key_a = s.node(a)->body_key;
  design::apply_ops(d, {design::make_feature_op("box", "Block", {{"length", 40}, {"width", 40}, {"height", 40}})});
  const std::string block = resolve(d).features.back().result["bodies"][0]["id"];
  const json a_ref = json::array({{{"body", a}, {"kind", "body"}}});
  auto refused = [&](const std::string& kind, const json& inputs, const std::string& why) {
    const size_t ops = d.ops.size();
    try {
      commands::run("feature", {{"kind", kind}, {"inputs", inputs}}, &d);
    } catch (const Error& e) {
      if (std::string(e.what()).find(why) == std::string::npos) throw check::Failure(kind + ": " + e.what());
      CHECK_EQ(d.ops.size(), ops);
      return;
    }
    throw check::Failure(kind + " was allowed on a linked part");
  };
  refused("move", {{"bodies", a_ref}, {"dz", "5 mm"}}, "cannot be changed");
  refused("move", {{"bodies", a_ref}, {"dz", "5 mm"}, {"copy", true}}, "cannot be copied");
  refused("pattern_circ", {{"bodies", a_ref}, {"count", "3"}}, "cannot be copied");
  commands::run("feature", {{"kind", "box"}, {"inputs", {{"x", "10 mm"}, {"y", "10 mm"}, {"length", "4 mm"}, {"width", "4 mm"}, {"height", "100 mm"}}}}, &d);
  const std::string pin = resolve(d).features.back().result["bodies"][0]["id"];
  refused("combine", {{"target", {a}}, {"tools", {pin}}, {"operation", "cut"}, {"keep_tools", true}}, "cannot be changed");
  refused("combine", {{"target", {a}}, {"tools", {block}}, {"operation", "cut"}, {"keep_tools", true}}, "cannot be consumed");  // all of A
  refused("combine", {{"target", {block}}, {"tools", {a}}, {"operation", "cut"}}, "cannot be consumed");
  // A tool it is; an automatic cut passes it by (a bore through the block and A cuts the block only); Remove takes it out.
  commands::run("feature", {{"kind", "combine"}, {"inputs", {{"target", {block}}, {"tools", {a}}, {"operation", "cut"}, {"keep_tools", true}}}}, &d);
  commands::run("feature", {{"kind", "cylinder"}, {"inputs", {{"x", "10 mm"}, {"y", "10 mm"}, {"diameter", "4 mm"}, {"height", "60 mm"}, {"operation", "cut"}}}}, &d);
  s = resolve(d);
  CHECK_EQ(s.node(a)->body_key, key_a);
  CHECK(about(volume(d, s, a), 1000));
  CHECK(about(volume(d, s, block), 64000 - 1000 - M_PI * 4 * 30, 0.01));
  CHECK(about(volume(d, s, pin), 16 * 100 - M_PI * 4 * 60, 0.01));  // the bore went through the pin too
  commands::run("feature", {{"kind", "remove"}, {"inputs", {{"bodies", json::array({{{"body", b}, {"kind", "body"}}})}}}}, &d);
  CHECK(!resolve(d).node(b));
  CHECK(d.serialize().find("#body ") != std::string::npos);  // the block's, never A's or B's
  for (const auto& entry : d.bodies()) CHECK(entry.key != key_a || entry.external);
}

TEST(changed_file_shows_the_version_synced_when_remembered) {
  Files f;
  const fs::path step = f.dir / "model.step";
  two_boxes(step, 5);
  Document d = Document::create();
  d.save_as(f.dir / "design.opad");
  link_file(d, step);  // remembered by the file's content and how it is read
  d.save();
  two_boxes(step, 5, 38);
  Document reopened = Document::load(f.dir / "design.opad");
  const auto states = load_assets(reopened);
  CHECK_EQ(states[0].state, "changed");
  CHECK(states[0].reason.find("remembered") != std::string::npos);
  Scene s = resolve(reopened);
  Bnd_Box box = node_world_bbox(reopened, s, linked(s, 0));
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  CHECK(about(z0, 30, 0.1));  // the model as its features were computed, not the file as it is now
  CHECK(!reopened.body(s.node(linked(s, 0))->body_key)->meta.value("stale", false));
  design::Plan plan = plan_asset_sync(reopened, last_import(reopened).id);
  CHECK_EQ(plan.report["kept"], 1);
}

// A board's look depends on its 3D models: a model that changes, appears or goes missing makes the board "changed" though
// its file is the same; the version synced (board and models) is remembered and shown until the sync takes the new one.
TEST(kicad_board_remembered_with_its_models) {
  Files f;
  const fs::path board = f.dir / "hw" / "board.kicad_pcb", model = f.dir / "hw" / "m1.step";
  two_boxes(model, 0);
  write(board, "(kicad_pcb (version 20241229) (general (thickness 1.6))\n  (gr_rect (start 0 0) (end 50 30) (layer \"Edge.Cuts\"))\n"
               "  (footprint \"Sync:R\" (layer \"F.Cu\") (uuid \"aaaaaaaa-0000-0000-0000-000000000001\") (at 10 10)\n"
               "    (property \"Reference\" \"R1\")\n    (model \"${KIPRJMOD}/m1.step\"))\n)\n");
  Document d = Document::create();
  d.save_as(f.dir / "hw" / "enclosure.opad");
  link_file(d, board);
  const std::string import_id = last_import(d).id;
  CHECK(last_import(d).data["asset"].contains("models_sha256"));
  d.save();
  CHECK_EQ(asset_status(d)[0].state, "ok");
  auto cube = [](const Document& doc, const Scene& s) {  // the model's 10 mm box
    for (const auto& id : s.all_bodies())
      if (about(volume(doc, s, id), 1000)) return id;
    throw check::Failure("no 10 mm box");
  };
  auto bottom = [](const Document& doc, const Scene& s, const std::string& id) {
    double x0, y0, z0, x1, y1, z1;
    node_world_bbox(doc, s, id).Get(x0, y0, z0, x1, y1, z1);
    return z0;
  };
  const double was = bottom(d, resolve(d), cube(d, resolve(d)));
  two_boxes(model, 0, 50);  // the model's box 20 mm higher; the board file as it was
  Document reopened = Document::load(f.dir / "hw" / "enclosure.opad");
  const AssetState st = load_assets(reopened)[0];
  CHECK_EQ(st.state, "changed");
  CHECK(st.reason.find("remembered") != std::string::npos);
  Scene s = resolve(reopened);
  for (const auto& id : s.all_bodies()) CHECK(!s.node(id)->body_missing && !reopened.body(s.node(id)->body_key)->meta.value("stale", false));
  CHECK(about(bottom(reopened, s, cube(reopened, s)), was, 0.01));  // as synced, not as the model is now
  const std::string board_key = s.node(body_named(s, "Board"))->body_key;
  design::Plan plan = plan_asset_sync(reopened, import_id);
  CHECK(!plan.report["up_to_date"].get<bool>());
  CHECK_EQ(plan.report["changed"].size(), 1u);  // the box; the model's other box and the board keep their keys
  design::commit(reopened, std::move(plan));
  s = resolve(reopened);
  CHECK(about(bottom(reopened, s, cube(reopened, s)), was + 20, 0.01));
  CHECK_EQ(s.node(body_named(s, "Board"))->body_key, board_key);
  CHECK_EQ(asset_status(reopened)[0].state, "ok");
  fs::remove(model);  // gone: a placeholder would stand in for it
  CHECK_EQ(asset_status(reopened)[0].state, "changed");
}

// A file read through a converter (as kicad-cli's STEP of a board): the source is watched and synced, the derived file
// read; the converter (AssetOptions::derive) makes it again when it is missing or the source changed.
TEST(derived_file_read_in_the_sources_place) {
  Files f;
  const fs::path source = f.dir / "part.src";
  write(source, "30");
  int runs = 0;
  AssetOptions converter;
  converter.derive = [&](const json& asset, const fs::path& from, const std::function<bool(double, const std::string&)>&) {
    CHECK_EQ(asset["derived"]["builder"]["name"], "test-converter");
    ++runs;
    const fs::path out = f.dir / "made" / "part.step";
    two_boxes(out, 5, std::stod(read_text_file(from)));
    return out;
  };
  Document d = Document::create();
  d.save_as(f.dir / "design.opad");
  link_derived(d, source, converter.derive(json{{"derived", {{"builder", {{"name", "test-converter"}}}}}}, source, {}), {{"name", "test-converter"}, {"version", 1}});
  const std::string import_id = last_import(d).id;
  const json asset = last_import(d).data["asset"];
  CHECK_EQ(asset["path"], "part.src");
  CHECK_EQ(asset["derived"]["path"], "made/part.step");
  CHECK_EQ(asset["derived"]["kind"], "step");
  CHECK_EQ(asset["derived"]["sha256"], sha256_file(f.dir / "made" / "part.step"));
  Scene s = resolve(d);
  CHECK_EQ(s.all_bodies().size(), 2u);
  const std::string key_b = s.node(linked(s, 1))->body_key;
  d.save();
  AssetOptions uncached;
  uncached.cache = false;
  Document reopened = Document::load(f.dir / "design.opad");
  AssetState st = load_assets(reopened, uncached)[0];
  CHECK_EQ(st.state, "ok");
  CHECK(st.derived == fs::absolute(f.dir / "made" / "part.step").lexically_normal());
  CHECK(about(volume(reopened, resolve(reopened), linked(resolve(reopened), 0)), 1000));
  // The source changes: changed; the derived file is still the one synced, so the model is shown as synced.
  write(source, "38");
  reopened = Document::load(f.dir / "design.opad");
  st = load_assets(reopened, uncached)[0];
  CHECK_EQ(st.state, "changed");
  for (const auto& b : reopened.bodies()) CHECK(!b.meta.value("stale", false));
  CHECK_THROWS(plan_asset_sync(reopened, import_id, uncached));  // no converter here
  design::Plan plan = plan_asset_sync(reopened, import_id, converter);
  CHECK_EQ(runs, 2);
  CHECK_EQ(plan.report["kept"], 1);
  CHECK_EQ(plan.report["changed"].size(), 1u);
  design::commit(reopened, std::move(plan));
  CHECK_EQ(asset_of(reopened, import_id)["derived"]["sha256"], sha256_file(f.dir / "made" / "part.step"));
  CHECK_EQ(asset_of(reopened, import_id)["sha256"], sha256_file(source));
  CHECK_EQ(asset_status(reopened)[0].state, "ok");
  CHECK_EQ(resolve(reopened).node(linked(resolve(reopened), 1))->body_key, key_b);
  reopened.save();
  // The derived file gone (a clone): missing without the converter, made again with it.
  remove_tree(f.dir / "made");
  CHECK_EQ(asset_status(reopened, converter)[0].state, "ok");  // its shapes are loaded here: nothing to sync (the app's monitor)
  Document clone = Document::load(f.dir / "design.opad");
  st = load_assets(clone, uncached)[0];
  CHECK_EQ(st.state, "missing");
  CHECK(st.reason.find("test-converter") != std::string::npos);
  st = load_assets(clone, [&] { AssetOptions o = converter; o.cache = false; return o; }())[0];
  CHECK_EQ(runs, 3);
  CHECK(st.unbound == 0 && resolve(clone).unresolved.empty());
  // Pack takes the derived file along.
  const json packed = pack_asset(clone, import_id);
  CHECK_EQ(asset_of(clone, import_id)["derived"]["path"], "assets/.opad/part.step");
  CHECK(fs::exists(f.dir / "assets" / ".opad" / "part.step") && packed["copied"] == 2);
}

// Pictures as a canvas: a rectangle at the picture's resolution with the file's own bytes on it. Linked, the bytes stay in the
// file (the body entry carries them, never the op); embedded, they come into the document once.
namespace {
std::string be32(uint32_t v) { return {char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
std::string chunk(const std::string& type, const std::string& data) { return be32(uint32_t(data.size())) + type + data + be32(0); }
// A PNG as far as its headers go (w x h pixels, `ppm` pixels per metre; `tag` stands for the picture data).
void png(const fs::path& p, uint32_t w, uint32_t h, uint32_t ppm, const std::string& tag) {
  write(p, std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", be32(w) + be32(h) + std::string("\x08\x06\0\0\0", 5)) +
               chunk("pHYs", be32(ppm) + be32(ppm) + std::string(1, '\x01')) + chunk("IDAT", tag) + chunk("IEND", ""));
}
std::array<double, 2> extent(const Document& d, const Scene& s, const std::string& id) {
  double x0, y0, z0, x1, y1, z1;
  node_world_bbox(d, s, id).Get(x0, y0, z0, x1, y1, z1);
  return {x1 - x0, y1 - y0};
}
}  // namespace

TEST(pictures_import_and_link) {
  Files f;
  const fs::path pic = f.dir / "front.png", photo = f.dir / "photo.jpg";
  png(pic, 400, 200, 3937, "first");  // 100 dpi: 101.6 x 50.8 mm
  // A JPEG's headers: JFIF at 300 dpi, a 640 x 480 frame.
  write(photo, std::string("\xFF\xD8\xFF\xE0\x00\x10JFIF\0\x01\x01\x01\x01\x2C\x01\x2C\0\0", 20) +
                   std::string("\xFF\xC0\x00\x11\x08\x01\xE0\x02\x80\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01\xFF\xD9", 21));
  Document d = Document::create();
  const ImportResult r = import_file(d, photo);
  CHECK_EQ(r.info["px"][0], 640);
  Scene s = resolve(d);
  const Node* n = s.node(s.all_bodies()[0]);
  CHECK_EQ(n->representation, "image");
  CHECK(n->raster["href"].get<std::string>().rfind("data:image/jpeg;base64,", 0) == 0);
  auto size = extent(d, s, n->id);
  CHECK(about(size[0], 640 * 25.4 / 300, 0.01) && about(size[1], 480 * 25.4 / 300, 0.01));
  CHECK_THROWS(import_file(d, [&] { write(f.dir / "fake.png", "not a picture"); return f.dir / "fake.png"; }()));
  // Linked: the op keeps no bytes; the scene shows them, read from the file.
  d.save_as(f.dir / "design.opad");
  link_file(d, pic);
  const std::string import_id = last_import(d).id;
  CHECK_EQ(last_import(d).data["asset"]["kind"], "image");
  CHECK(!last_import(d).data["nodes"][0]["raster"].contains("href"));
  s = resolve(d);
  const std::string canvas = linked(s, 0);
  CHECK(s.node(canvas)->raster["href"].get<std::string>().rfind("data:image/png;base64,", 0) == 0);
  size = extent(d, s, canvas);
  CHECK(about(size[0], 101.6, 0.01) && about(size[1], 50.8, 0.01));
  d.save();
  const std::string text = read_text_file(f.dir / "design.opad");
  CHECK(text.find("data:image/png") == std::string::npos && text.find("data:image/jpeg") != std::string::npos);
  Document reopened = Document::load(f.dir / "design.opad");
  CHECK_EQ(load_assets(reopened)[0].state, "ok");
  CHECK(resolve(reopened).node(canvas)->raster.contains("href"));
  CHECK(!resolve(Document::load(f.dir / "design.opad")).node(canvas)->raster.contains("href"));  // as an older build: a frame
  // Another picture of the same size: the same rectangle, yet another body (its key follows the bytes).
  const std::string was = resolve(reopened).node(canvas)->body_key;
  png(pic, 400, 200, 3937, "second");
  design::Plan plan = plan_asset_sync(reopened, import_id);
  CHECK_EQ(plan.report["changed"].size(), 1u);
  design::commit(reopened, std::move(plan));
  s = resolve(reopened);
  CHECK(s.node(canvas)->body_key != was && s.node(canvas)->raster["href"] != resolve(d).node(canvas)->raster["href"]);
  // Embedded: the bytes come into the document with it.
  design::commit(reopened, plan_asset_embed(reopened, import_id));
  reopened.save();
  fs::remove(pic);
  Document alone = Document::load(f.dir / "design.opad");
  s = resolve(alone);
  CHECK(s.node(canvas)->raster["href"] == resolve(reopened).node(canvas)->raster["href"] && !s.node(canvas)->body_missing);
}

// A picture linked at a width and centred (Insert canvas on a big picture) comes in at that size and place, and every read
// keeps them; a sync keeps the canvas's flags as the user set them (it took the reader's defaults).
TEST(linked_picture_keeps_its_width_and_flags) {
  Files f;
  const fs::path pic = f.dir / "plan.png";
  png(pic, 400, 200, 3937, "first");  // 101.6 x 50.8 mm at its resolution
  Document d = Document::create();
  d.save_as(f.dir / "design.opad");
  ImportOptions o;
  o.canvas = {{"width", 200}, {"center", true}};
  link_file(d, pic, o);
  const std::string import_id = last_import(d).id;
  Scene s = resolve(d);
  const std::string canvas = linked(s, 0);
  auto info = [&](Document& doc) { return commands::run("canvas", {{"action", "info"}, {"target", canvas}}, &doc); };
  json i = info(d);
  CHECK(about(i["width"], 200) && about(i["height"], 100) && about(i["x"], 0) && about(i["y"], 0));
  commands::run("canvas", {{"action", "flags"}, {"target", canvas}, {"set", {{"display_through", true}, {"flip", {true, false}}, {"selectable", false}}}}, &d);
  d.save();
  Document reopened = Document::load(f.dir / "design.opad");
  CHECK_EQ(load_assets(reopened)[0].state, "ok");
  png(pic, 400, 200, 3937, "second");
  design::commit(reopened, plan_asset_sync(reopened, import_id));
  i = info(reopened);
  CHECK(about(i["width"], 200) && about(i["height"], 100) && about(i["x"], 0) && about(i["y"], 0));
  CHECK(i["display_through"] == true && i["flip"] == json({true, false}) && i["selectable"] == false);
}

// A drawing linked where it was placed (UI-68): the placement goes with the asset, so a sync of the changed drawing keeps it,
// node and id; Replace with another kind of file reads it as that kind.
TEST(drawing_linked_with_its_placement) {
  Files f;
  const fs::path plan = f.dir / "plan.dxf";
  auto line = [&](int to) {
    write(plan, "0\nSECTION\n2\nENTITIES\n0\nLINE\n8\nCut\n10\n0\n20\n0\n11\n" + std::to_string(to) + "\n21\n0\n0\nENDSEC\n0\nEOF\n");
  };
  line(40);
  Document d = Document::create();
  d.save_as(f.dir / "design.opad");
  ImportOptions o;
  o.placement = Mat4::translation(10, 20, 5);
  link_file(d, plan, o);
  const Op& op = last_import(d);
  const std::string import_id = op.id;
  CHECK_EQ(op.data["asset"]["kind"], "drawing");
  CHECK(op.data["asset"]["builder"]["options"].contains("placement"));
  Scene s = resolve(d);
  const std::string body = linked(s, 0);
  CHECK_EQ(s.node(body)->representation, "drawing2d");
  auto span = [&](const Scene& sc) {
    double x0, y0, z0, x1, y1, z1;
    node_world_bbox(d, sc, body).Get(x0, y0, z0, x1, y1, z1);
    return std::array<double, 3>{x0, x1, (z0 + z1) / 2};
  };
  auto at = span(s);
  CHECK(about(at[0], 10, 0.01) && about(at[1], 50, 0.01) && about(at[2], 5, 0.01));
  line(60);
  CHECK_EQ(asset_status(d)[0].state, "changed");
  design::Plan synced = plan_asset_sync(d, import_id);
  CHECK_EQ(synced.report["changed"].size(), 1u);
  design::commit(d, std::move(synced));
  s = resolve(d);
  at = span(s);
  CHECK(s.node(body) && about(at[0], 10, 0.01) && about(at[1], 70, 0.01) && about(at[2], 5, 0.01));
  CHECK_EQ(asset_status(d)[0].state, "ok");
  // Replaced by a STEP: read as one.
  two_boxes(f.dir / "model.step", 5);
  design::commit(d, plan_asset_sync(d, import_id, {}, f.dir / "model.step"));
  CHECK_EQ(asset_of(d, import_id)["kind"], "step");
  CHECK_EQ(asset_status(d)[0].state, "ok");
}

TEST(embed_and_pack) {
  Files f;
  const fs::path step = f.dir / "outside" / "model.step";
  two_boxes(step, 5);
  Document d = Document::create();
  d.save_as(f.dir / "project" / "design.opad");
  link_file(d, step);
  const std::string import_id = last_import(d).id;
  // Pack: a copy under assets/, the asset pointing there.
  const json packed = pack_asset(d, import_id);
  CHECK_EQ(packed["path"], "assets/model.step");
  CHECK_EQ(packed["import"], import_id);  // read before the edit was appended (the log may move)
  CHECK_EQ(packed["copied"], 1);
  CHECK(fs::exists(f.dir / "project" / "assets" / "model.step"));
  CHECK_EQ(asset_of(d, import_id)["storage"], "project");
  d.save();
  remove_tree(f.dir / "outside");
  Document reopened = Document::load(f.dir / "project" / "design.opad");
  CHECK_EQ(load_assets(reopened)[0].state, "ok");  // the project's copy, trusted (inside the project)
  // Embed: ordinary bodies, stored, editable; the file is no longer needed.
  design::commit(reopened, plan_asset_embed(reopened, import_id));
  CHECK_EQ(asset_of(reopened, import_id)["storage"], "embedded");
  Scene s = resolve(reopened);
  for (const auto& id : s.all_bodies()) {
    CHECK(!s.node(id)->linked && !s.node(id)->body_missing);
    const BodyEntry* entry = reopened.body(s.node(id)->body_key);
    CHECK(!entry->brep.empty() && sha256_hex(entry->brep) == entry->key);  // committed under the planner's key, not hashed again
  }
  CHECK_THROWS(reopened.add_body("not a hash", std::string("x\n"), json::object()));
  reopened.save();
  remove_tree(f.dir / "project" / "assets");
  Document alone = Document::parse(read_text_file(f.dir / "project" / "design.opad"));
  s = resolve(alone);
  CHECK(s.unresolved.empty());
  CHECK(about(volume(alone, s, s.all_bodies()[0]), 1000));
  CHECK_EQ(load_assets(alone)[0].state, "embedded");
}

TEST(paths_follow_save_as) {
  Files f;
  const fs::path step = f.dir / "lib" / "model.step";
  two_boxes(step, 5);
  Document d = Document::create();
  link_file(d, step);  // no document folder yet: absolute only
  CHECK(!last_import(d).data["asset"].contains("path"));
  d.save_as(f.dir / "work" / "design.opad");
  CHECK_EQ(last_import(d).data["asset"]["path"], "../lib/model.step");
  CHECK(read_text_file(f.dir / "work" / "design.opad").find("\"path\":\"../lib/model.step\"") != std::string::npos);
  // A saved op is never rewritten: Save As elsewhere keeps its line (the absolute path still finds the file).
  d.save_as(f.dir / "other" / "design.opad");
  CHECK_EQ(last_import(d).data["asset"]["path"], "../lib/model.step");
  // An edit op: Document::append checks what the import becomes.
  CHECK_THROWS(d.append({{"op", "edit"}, {"target", last_import(d).id}, {"set", {{"nodes", {{{"type", "body"}, {"id", "x"}}}}}}}));
}

TEST(kicad_board_linked_and_synced) {
  Files f;
  auto fp = [](const std::string& id, const std::string& ref, const std::string& at) {
    return "  (footprint \"Sync:R\" (layer \"F.Cu\") (uuid \"" + id + "\") (at " + at + ")\n    (property \"Reference\" \"" + ref +
           "\")\n    (model \"${KIPRJMOD}/m1.step\"))\n";
  };
  const fs::path board = f.dir / "hw" / "board.kicad_pcb";
  two_boxes(f.dir / "hw" / "m1.step", 0);
  const std::string outline = "(kicad_pcb (version 20241229) (general (thickness 1.6))\n  (gr_rect (start 0 0) (end 50 30) (layer \"Edge.Cuts\"))\n";
  write(board, outline + fp("aaaaaaaa-0000-0000-0000-000000000001", "R1", "10 10") + fp("aaaaaaaa-0000-0000-0000-000000000002", "R2", "20 10 90") + ")\n");
  fs::create_directories(f.dir / ".git");  // one repository: the board in ../hw is part of the project
  Document d = Document::create();
  d.save_as(f.dir / "mech" / "enclosure.opad");
  ImportOptions o;
  link_file(d, board, o);
  const std::string import_id = last_import(d).id;
  const json asset = last_import(d).data["asset"];
  CHECK_EQ(asset["kind"], "kicad_pcb");
  CHECK_EQ(asset["path"], "../hw/board.kicad_pcb");
  CHECK(asset["builder"]["options"]["origin_at"].is_array());  // the frame every sync keeps
  Scene s = resolve(d);
  auto component = [&](const Scene& sc, const std::string& prefix) {
    for (const auto& [id, n] : sc.nodes)
      if (n.kind == Node::Kind::Component && n.name.rfind(prefix, 0) == 0) return id;
    throw check::Failure("no component " + prefix);
  };
  const std::string r1 = component(s, "R1"), r2 = component(s, "R2");
  const std::string board_body = body_named(s, "Board");
  const std::string board_key = s.node(board_body)->body_key;
  d.save();
  // R1 moves, R2 is removed, R3 comes: R1 keeps its node, the board (same outline) its key.
  write(board, outline + fp("aaaaaaaa-0000-0000-0000-000000000001", "R1", "12 10") + fp("aaaaaaaa-0000-0000-0000-000000000003", "R3", "30 20") + ")\n");
  Document reopened = Document::load(f.dir / "mech" / "enclosure.opad");
  CHECK_EQ(load_assets(reopened)[0].state, "changed");
  CHECK(kicad_sync_preview(reopened)["moved"].size() == 1);
  // Synced from the reopened document (the board as it was never read there): the board keeps its key all the same.
  design::Plan again = plan_asset_sync(reopened, import_id);
  design::commit(reopened, std::move(again));
  CHECK_EQ(resolve(reopened).node(board_body)->body_key, board_key);
  // Synced in the session that read the board as it was: the board's body, unchanged, keeps its key.
  const double was = resolve(d).world(r1).at(0, 3);
  design::Plan plan = plan_asset_sync(d, import_id);
  CHECK_EQ(plan.report["removed"].size(), 2u);  // R2's two bodies
  CHECK_EQ(plan.report["added"].size(), 2u);
  design::commit(d, std::move(plan));
  s = resolve(d);
  CHECK(s.node(r1) && !s.node(r2));
  CHECK(about(s.world(r1).at(0, 3) - was, 2));
  CHECK_EQ(s.node(board_body)->body_key, board_key);
  CHECK(!kicad_sync_preview(d)["changed"].get<bool>());  // the preview reads the synced import
  for (const auto& id : s.all_bodies()) CHECK(!s.node(id)->body_missing);
  // Gone from where it was linked, its project copy (laid out as pack does) is read; packing then only points the link at it.
  const fs::path copy = f.dir / "mech" / "assets" / "board";
  fs::create_directories(copy);
  fs::copy_file(board, copy / "board.kicad_pcb");
  fs::copy_file(f.dir / "hw" / "m1.step", copy / "m1.step");
  fs::rename(board, f.dir / "hw" / "board.old");
  const AssetState found = asset_status(d)[0];
  CHECK_EQ(found.state, "ok");
  CHECK(fs::equivalent(found.file, copy / "board.kicad_pcb"));
  const json packed = pack_asset(d, import_id);
  CHECK_EQ(packed["path"], "assets/board/board.kicad_pcb");
  CHECK_EQ(packed["copied"], 0);
  CHECK_EQ(asset_of(d, import_id)["storage"], "project");
}

TEST(command_layer) {
  Files f;
  const fs::path step = f.dir / "model.step";
  two_boxes(step, 5);
  const std::string doc = (f.dir / "design.opad").string();
  commands::run("new", {{"doc", doc}});
  const json r = commands::run("import", {{"doc", doc}, {"file", step.string()}, {"link", true}});
  CHECK(r["info"]["linked"].get<bool>() && r["new_entries"] == 0);
  json st = commands::run("asset", {{"doc", doc}});
  CHECK(st["assets"].size() == 1 && st["assets"][0]["state"] == "ok" && st["assets"][0]["path"] == "model.step");
  const json info = commands::run("info", {{"doc", doc}});
  CHECK(info["bbox"].is_object() && info["unresolved"] == 0);  // read from the file by the command layer
  two_boxes(step, 6);
  CHECK_EQ(commands::run("asset", {{"doc", doc}})["assets"][0]["state"], "changed");
  const json synced = commands::run("asset", {{"doc", doc}, {"action", "sync"}});
  CHECK(!synced["up_to_date"].get<bool>());
  CHECK_EQ(commands::run("asset", {{"doc", doc}})["assets"][0]["state"], "ok");
  CHECK_THROWS(commands::run("asset", {{"doc", doc}, {"action", "explode"}}));
  fs::remove(step);
  CHECK_EQ(commands::run("asset", {{"doc", doc}})["assets"][0]["state"], "missing");
  CHECK_EQ(commands::run("info", {{"doc", doc}})["unresolved"], 2);
}

CHECK_MAIN()
