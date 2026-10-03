// Materials and mass (TODO 11 UI-140): the library, the names it maps, which node's material decides, masses through
// placements and overrides, and the properties command. Bills of materials (UI-83): top, parts and indented modes,
// quantities from instances and from copies of one solid (never its mirror image), part numbers, exclusions, purchased
// assemblies, masses, CSV and the bom command.
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <filesystem>
#include <set>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/drawing/bom.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/materials.hpp"
#include "opad/step_io.hpp"

using namespace opad;

namespace {

json run(Document& doc, const std::string& command, const json& args) { return commands::run(command, args, &doc); }

std::string add_shape(Document& doc, const TopoDS_Shape& s, const std::string& name, const std::string& material = {}) {
  json meta = {{"name", name}, {"units", "mm"}, {"source", "parts.step"}};
  if (!material.empty()) meta["material"] = material;
  return doc.add_body(brep_from_shape(s), meta);
}

json body(const std::string& name, const std::string& key, const json& transform = nullptr) {
  json n = {{"type", "body"}, {"id", new_uuid()}, {"name", name}, {"key", key}};
  if (!transform.is_null()) n["transform"] = transform;
  return n;
}

json component(const std::string& name, const json& children) { return {{"type", "component"}, {"id", new_uuid()}, {"name", name}, {"children", children}}; }

std::string id_of(const Scene& s, const std::string& name) {
  for (const auto& [id, n] : s.nodes)
    if (n.name == name) return id;
  throw Error("no node " + name);
}

void set(Document& doc, const std::string& name, const json& props) { run(doc, "part_properties", {{"target", id_of(resolve(doc), name)}, {"set", props}}); }

// A 10 x 20 x 30 block (6000 mm3) under a component, beside a sheet face and blocks that name their material in the file.
struct Parts {
  Document doc = Document::create();
  Parts() {
    const std::string block = add_shape(doc, BRepPrimAPI_MakeBox(10, 20, 30).Shape(), "Block");
    const std::string abs = add_shape(doc, BRepPrimAPI_MakeBox(10, 20, 30.5).Shape(), "Cover", "Plastic - ABS (black)");
    const std::string inox = add_shape(doc, BRepPrimAPI_MakeBox(10, 20, 31).Shape(), "Shaft", "Stainless Steel 316L");
    const std::string sheet = add_shape(doc, BRepBuilderAPI_MakeFace(gp_Pln(), 0, 10, 0, 10).Face(), "Sheet");
    const json twice = Mat4{{2, 0, 0, 5, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1}}.to_json();
    doc.append({{"op", "import"},
                {"source", "parts.step"},
                {"nodes", {component("Frame", {body("Plain", block), body("Brass", block), body("Dense", block), body("Given", block),
                                               body("Inherits ABS", abs)}),
                           body("Loose", block), body("Big", block, twice), body("Cover", abs), body("Shaft", inox), body("Sheet", sheet)}}});
    set(doc, "Frame", {{"material", "aluminium-6061"}});
    set(doc, "Brass", {{"material", "Brass CuZn39Pb3"}});
    set(doc, "Dense", {{"density", 2}});
    set(doc, "Given", {{"mass", 5}});
    set(doc, "Big", {{"material", "steel"}});
    set(doc, "Sheet", {{"material", "steel"}});
  }
};

}  // namespace

TEST(material_library) {
  std::set<std::string> ids;
  for (const auto& m : materials()) {
    CHECK(m.density > 0.5 && m.density < 10);
    CHECK(ids.insert(m.id).second);
    CHECK(material(m.id) == &m);
    CHECK(find_material(m.id) == &m);
    CHECK(find_material(m.name) == &m);
  }
  for (const char* id : {"steel", "stainless", "aluminium-6061", "brass", "abs", "pla", "petg", "nylon", "fr4", "glass"}) CHECK(ids.count(id));
  CHECK_EQ(material("aluminium-6061")->density, 2.70);
  CHECK(material("unobtainium") == nullptr);
}

TEST(material_names_from_files_and_tools) {
  const std::vector<std::pair<const char*, const char*>> cases = {
      {"Steel", "steel"},
      {"STEEL, MILD", "steel"},
      {"S235JR", "steel"},
      {"C45", "steel"},
      {"AISI 1045", "steel"},
      {"Stainless Steel", "stainless"},
      {"Stainless Steel 316L", "stainless"},
      {"SS304", "stainless"},
      {"SUS304", "stainless"},
      {"1.4301", "stainless"},
      {"AISI 316L", "stainless"},
      {"A2-70", "stainless"},
      {"Aluminum 6061-T6", "aluminium-6061"},
      {"Aluminium", "aluminium-6061"},
      {"AL6061T6", "aluminium-6061"},
      {"EN AW-6082", "aluminium-6061"},
      {"AlMg3", "aluminium-6061"},
      {"Brass", "brass"},
      {"CuZn39Pb3", "brass"},
      {"Copper", "copper"},
      {"Cu-ETP", "copper"},
      {"Ti-6Al-4V", "titanium"},
      {"ABS", "abs"},
      {"Plastic - ABS (black)", "abs"},
      {"PLA+", "pla"},
      {"PETG", "petg"},
      {"PET-G", "petg"},
      {"Nylon", "nylon"},
      {"PA6", "nylon"},
      {"PA66 GF30", "nylon"},
      {"Glass-filled nylon", "nylon"},
      {"PA12", "nylon-12"},
      {"PA 12", "nylon-12"},
      {"Polycarbonate", "polycarbonate"},
      {"POM-C", "pom"},
      {"Acetal", "pom"},
      {"FR4", "fr4"},
      {"FR-4 TG150", "fr4"},
      {"Glass", "glass"},
      {"Borosilicate glass", "glass"},
  };
  for (const auto& [text, id] : cases) {
    const Material* m = find_material(text);
    if (!m || m->id != id) throw check::Failure(std::string("'") + text + "' -> " + (m ? m->id : "nothing") + ", not " + id);
  }
  for (const char* text : {"", "Default", "Generic", "Paint", "Unobtainium", "Plastic", "13045", "ssd"}) {
    if (const Material* m = find_material(text)) throw check::Failure(std::string("'") + text + "' -> " + m->id);
  }
}

TEST(material_of_the_nearest_node) {
  Parts p;
  const Scene s = resolve(p.doc);
  const auto of = [&](const char* name) { return material_of(p.doc, s, id_of(s, name)); };
  CHECK_EQ(of("Plain").id, "aluminium-6061");
  CHECK_EQ(of("Plain").from, id_of(s, "Frame"));
  CHECK_EQ(of("Plain").shown(), "Aluminium 6061");
  CHECK_EQ(of("Brass").id, "brass");
  CHECK_EQ(of("Brass").shown(), "Brass CuZn39Pb3");  // as written
  CHECK_EQ(of("Brass").density, 8.5);
  CHECK_EQ(of("Dense").text, "");  // a density alone decides too
  CHECK_EQ(of("Dense").density, 2);
  CHECK_EQ(of("Inherits ABS").id, "aluminium-6061");  // the component's property beats the file
  CHECK_EQ(of("Cover").id, "abs");                    // the file's name when nobody set one
  CHECK_EQ(of("Cover").from, "");
  CHECK_EQ(of("Shaft").id, "stainless");
  CHECK_EQ(of("Loose").text, "");
  CHECK_EQ(of("Loose").density, 0);
  CHECK_EQ(of("Frame").id, "aluminium-6061");
}

TEST(body_masses) {
  Parts p;
  const Scene s = resolve(p.doc);
  const auto mass = [&](const char* name) { return body_mass(p.doc, s, id_of(s, name)); };
  CHECK_NEAR(*mass("Plain"), 6000 * 2.70 / 1000, 1e-9);
  CHECK_NEAR(*mass("Brass"), 6000 * 8.50 / 1000, 1e-9);
  CHECK_NEAR(*mass("Dense"), 12, 1e-9);
  CHECK_NEAR(*mass("Given"), 5, 0);
  CHECK_NEAR(*mass("Big"), 8 * 6000 * 7.85 / 1000, 1e-9);  // placed at twice its size
  CHECK_NEAR(*mass("Cover"), 6100 * 1.04 / 1000, 1e-9);
  CHECK_NEAR(*mass("Shaft"), 6200 * 8.00 / 1000, 1e-9);
  std::string why;
  CHECK(!body_mass(p.doc, s, id_of(s, "Loose"), &why));
  CHECK_EQ(why, "no material");
  CHECK(!body_mass(p.doc, s, id_of(s, "Sheet"), &why));
  CHECK_EQ(why, "not a solid");
  set(p.doc, "Loose", {{"material", "Unobtainium"}});
  const Scene after = resolve(p.doc);
  CHECK(!body_mass(p.doc, after, id_of(after, "Loose"), &why));
  CHECK_EQ(why, "no density for the material 'Unobtainium'");
  CHECK_EQ(mass_unit("kg"), 1000);
  CHECK_THROWS(mass_unit("oz"));
}

TEST(properties_show_material_and_mass) {
  Parts p;
  const Scene s = resolve(p.doc);
  const json plain = node_properties(p.doc, s, id_of(s, "Plain"));
  CHECK_EQ(plain["material"], "Aluminium 6061");
  CHECK_EQ(plain["density"], 2.70);
  CHECK_NEAR(plain["mass"].get<double>(), 16.2, 1e-9);
  CHECK(!node_properties(p.doc, s, id_of(s, "Plain"), false).contains("mass"));  // geometry only on request
  CHECK(!node_properties(p.doc, s, id_of(s, "Loose")).contains("mass"));
  CHECK_NEAR(node_properties(p.doc, s, id_of(s, "Given"))["mass"].get<double>(), 5, 0);
  CHECK_EQ(node_properties(p.doc, s, id_of(s, "Cover"))["material"], "Plastic - ABS (black)");
  // A component: the sum of its bodies, all of which have a mass.
  const json frame = node_properties(p.doc, s, id_of(s, "Frame"));
  CHECK_NEAR(frame["mass"].get<double>(), (6000 * 2.70 + 6000 * 8.50 + 6000 * 2 + 6100 * 2.70) / 1000 + 5, 1e-9);
  set(p.doc, "Frame", {{"material", nullptr}});
  const Scene after = resolve(p.doc);
  CHECK(!node_properties(p.doc, after, id_of(after, "Frame")).contains("mass"));  // Plain has no material now
  set(p.doc, "Frame", {{"mass", 1500}});
  CHECK_NEAR(node_properties(p.doc, resolve(p.doc), id_of(after, "Frame"))["mass"].get<double>(), 1500, 0);
}

TEST(part_properties_command) {
  Parts p;
  const Scene s = resolve(p.doc);
  const std::string loose = id_of(s, "Loose"), cover = id_of(s, "Cover");
  CHECK_THROWS(run(p.doc, "part_properties", {{"target", loose}, {"set", {{"density", -1}}}}));
  CHECK_THROWS(run(p.doc, "part_properties", {{"target", loose}, {"set", {{"density", "heavy"}}}}));
  CHECK_THROWS(run(p.doc, "part_properties", {{"target", loose}, {"set", {{"mass", 0}}}}));
  CHECK_THROWS(run(p.doc, "part_properties", {{"target", loose}, {"set", {{"material", 7}}}}));
  CHECK_THROWS(run(p.doc, "part_properties", {{"target", loose}, {"set", {{"material", "Unobtainium"}}}, {"appearance", true}}));
  const size_t before = p.doc.ops.size();
  const json out = run(p.doc, "part_properties", {{"targets", {loose, cover}}, {"set", {{"material", "Brass"}, {"vendor", "Acme"}}}, {"appearance", true}});
  CHECK_EQ(out["material"]["id"], "brass");
  CHECK_EQ(p.doc.ops.size(), before + 4);  // properties and appearance for each
  const Scene after = resolve(p.doc);
  CHECK(after.node(loose)->has_color);
  CHECK_NEAR(after.node(loose)->color[0], material("brass")->color[0], 1e-12);
  CHECK_EQ(after.node(cover)->properties["vendor"], "Acme");
  run(p.doc, "part_properties", {{"target", loose}, {"set", {{"density", nullptr}, {"vendor", nullptr}}}});
  CHECK(!resolve(p.doc).node(loose)->properties.contains("vendor"));
  const json list = commands::run("materials", json::object());
  CHECK_EQ(list["materials"].size(), materials().size());
  CHECK_EQ(commands::run("materials", {{"match", "Aluminum 6061-T6"}})["match"]["id"], "aluminium-6061");
  CHECK(commands::run("materials", {{"match", "Default"}})["match"].is_null());
}

// The app's Part properties dialog edits several nodes at once: shared values shown, mixed ones left alone unless typed.
TEST(part_properties_editor) {
  Parts p;
  const std::string loose = id_of(resolve(p.doc), "Loose"), cover = id_of(resolve(p.doc), "Cover");
  run(p.doc, "part_properties", {{"targets", {loose, cover}}, {"set", {{"vendor", "Acme"}, {"density", 1.1}}}});
  run(p.doc, "part_properties", {{"target", loose}, {"set", {{"part_number", "P-1"}, {"material", "brass"}}}});
  run(p.doc, "part_properties", {{"target", cover}, {"set", {{"part_number", "P-2"}}}});
  const Scene s = resolve(p.doc);
  const json shared = drawing::shared_part_properties(s, {loose, cover});
  CHECK_EQ(shared["values"], json({{"density", 1.1}, {"vendor", "Acme"}}));  // ordered by key
  CHECK_EQ(shared["mixed"], json({"material", "part_number"}));  // one sets it, the other does not; or both, differently
  CHECK_EQ(drawing::shared_part_properties(s, {loose})["values"]["material"], "brass");
  CHECK(drawing::shared_part_properties(s, {"nobody"})["values"].empty());
  // Unchanged fields, a mixed field left empty and whitespace are no change; cleared shared fields are removed.
  const json none = drawing::part_properties_change(shared, {{"vendor", " Acme "}, {"density", 1.1}, {"part_number", ""}, {"material", nullptr}, {"notes", "  "}});
  CHECK(none.empty());
  const json set = drawing::part_properties_change(shared, {{"vendor", ""}, {"density", 2.5}, {"part_number", "P-9"}, {"description", " Lid "}});
  CHECK_EQ(set, json({{"vendor", nullptr}, {"density", 2.5}, {"part_number", "P-9"}, {"description", "Lid"}}));
  run(p.doc, "part_properties", {{"targets", {loose, cover}}, {"set", set}});
  const Scene after = resolve(p.doc);
  CHECK_EQ(after.node(cover)->properties, json({{"density", 2.5}, {"part_number", "P-9"}, {"description", "Lid"}}));
  CHECK_EQ(after.node(loose)->properties["material"], "brass");  // mixed and left alone
}

namespace {

// A robot as a STEP file would bring it (one root component): a steel base, two identical wheel units (a wheel and an
// axle each), four screws (instances) in a component of A2 stainless, a bracket with a corner notch (a chiral solid), a
// copy of it turned and moved (its own key, as design copies are stored) and its mirror image, two spacers that share a
// part number, a mesh guard, an excluded template, a purchased motor with a given mass and a component left empty.
struct Robot {
  Document doc = Document::create();
  std::string root;
  Robot() {
    const TopoDS_Shape bracket = BRepAlgoAPI_Cut(BRepPrimAPI_MakeBox(30, 20, 10).Shape(), BRepPrimAPI_MakeBox(5, 5, 5).Shape()).Shape();
    gp_Trsf turn, mirror;
    turn.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), M_PI / 2);
    turn.SetTranslationPart(gp_Vec(100, 40, 0));
    mirror.SetMirror(gp_Ax2(gp_Pnt(15, 0, 0), gp_Dir(1, 0, 0)));
    const std::string base = add_shape(doc, BRepPrimAPI_MakeBox(100, 80, 10).Shape(), "Base");
    const std::string wheel = add_shape(doc, BRepPrimAPI_MakeCylinder(20, 10).Shape(), "Wheel");
    const std::string axle = add_shape(doc, BRepPrimAPI_MakeCylinder(3, 30).Shape(), "Axle");
    const std::string screw = add_shape(doc, BRepPrimAPI_MakeCylinder(2, 12).Shape(), "Screw");
    const std::string b1 = add_shape(doc, bracket, "Bracket");
    const std::string b2 = add_shape(doc, BRepBuilderAPI_Transform(bracket, turn, true).Shape(), "Bracket copy");
    const std::string b3 = add_shape(doc, BRepBuilderAPI_Transform(bracket, mirror, true).Shape(), "Bracket mirror");
    const std::string spacer = add_shape(doc, BRepPrimAPI_MakeBox(10, 10, 5).Shape(), "Spacer");
    const std::string spacer6 = add_shape(doc, BRepPrimAPI_MakeBox(10, 10, 6).Shape(), "Spacer");
    const auto at = [](double x, double y, double z) { return Mat4::translation(x, y, z).to_json(); };
    json guard = body("Guard", base);
    guard["representation"] = "mesh";
    json fasteners = json::array();
    for (int i = 0; i < 4; ++i) fasteners.push_back(body("Screw " + std::to_string(i + 1), screw, at(10 + 80 * (i % 2), 10 + 60 * (i / 2), 10)));
    const json unit = json::array({body("Wheel", wheel), body("Axle", axle, at(0, 0, -10))});
    json robot = component("Robot", {body("Base", base), component("Wheel unit:1", unit), component("Wheel unit:2", unit), component("Fasteners", fasteners),
                                     body("Bracket", b1), body("Bracket copy", b2), body("Bracket mirror", b3), body("Spacer", spacer), body("Spacer alt", spacer6),
                                     guard, body("Template", base), component("Motor", {body("Motor can", wheel), body("Motor shaft", axle)}),
                                     component("Empty", json::array({body("Excluded too", base)}))});
    // The two wheel units' children must keep their own ids.
    for (auto& c : robot["children"])
      if (c["name"].get<std::string>().rfind("Wheel unit", 0) == 0)
        for (auto& k : c["children"]) k["id"] = new_uuid();
    root = robot["id"];
    doc.append({{"op", "import"}, {"source", "robot.step"}, {"nodes", json::array({robot})}});
    set(doc, "Base", {{"material", "Steel"}, {"description", "Plate, \"flat\""}, {"notes", "=1+2"}, {"finish", "painted"}});
    set(doc, "Fasteners", {{"material", "A2-70"}});
    set(doc, "Spacer", {{"part_number", "SP-10"}, {"vendor", "Acme"}});
    set(doc, "Spacer alt", {{"part_number", "SP-10"}});
    set(doc, "Template", {{"bom", "exclude"}});
    set(doc, "Excluded too", {{"bom", "exclude"}});
    set(doc, "Motor", {{"bom", "purchased"}, {"mass", 350}, {"part_number", "M-42"}});
    const Scene s = resolve(doc);
    for (const auto& id : s.node(root)->children)
      if (s.node(id)->name.rfind("Wheel unit", 0) == 0)
        for (const auto& k : s.node(id)->children)
          if (s.node(k)->name == "Wheel") run(doc, "part_properties", {{"target", k}, {"set", {{"material", "PA12"}}}});
  }
  json bom(const std::string& mode, drawing::BomOptions o = {}) const {
    o.mode = mode;
    return drawing::bom(doc, resolve(doc), o);
  }
};

std::string table(const json& b) {
  std::string out;
  for (const auto& r : b["rows"]) {
    out += r["item"].get<std::string>() + " " + r["name"].get<std::string>() + " x" + std::to_string(r["qty"].get<long long>());
    if (r.contains("total_qty")) out += "/" + std::to_string(r["total_qty"].get<long long>());
    out += "; ";
  }
  return out;
}

const json& row(const json& b, const std::string& name) {
  for (const auto& r : b["rows"])
    if (r["name"] == name) return r;
  throw check::Failure("no row " + name + " in " + table(b));
}

void same(const std::string& got, const std::string& want) {
  if (got != want) throw check::Failure("\n got: " + got + "\nwant: " + want);
}

}  // namespace

TEST(bom_modes) {
  Robot r;
  const json top = r.bom("top");
  CHECK_EQ(top["assembly"]["name"], "Robot");
  same(table(top), "1 Base x1; 2 Wheel unit x2; 3 Fasteners x1; 4 Bracket x2; 5 Bracket mirror x1; 6 Spacer x2; 7 Motor x1; ");
  CHECK_EQ(row(top, "Wheel unit")["kind"], "assembly");
  CHECK_EQ(row(top, "Wheel unit")["nodes"].size(), 2u);
  CHECK_EQ(row(top, "Motor")["kind"], "part");
  CHECK_EQ(row(top, "Motor")["purchased"], true);
  CHECK_EQ(row(top, "Spacer")["part_number"], "SP-10");
  CHECK_EQ(row(top, "Spacer")["identity"], "pn:SP-10");
  CHECK_EQ(row(top, "Spacer")["vendor"], "Acme");
  CHECK_EQ(row(top, "Base")["source"], "robot.step");
  const json parts = r.bom("parts");
  same(table(parts), "1 Base x1; 2 Wheel x2; 3 Axle x2; 4 Screw x4; 5 Bracket x2; 6 Bracket mirror x1; 7 Spacer x2; 8 Motor x1; ");
  CHECK_EQ(parts["totals"]["parts"], 15);
  CHECK_EQ(parts["totals"]["rows"], 8);
  const json indented = r.bom("indented");
  same(table(indented),
       "1 Base x1/1; 2 Wheel unit x2/2; 2.1 Wheel x1/2; 2.2 Axle x1/2; 3 Fasteners x1/1; 3.1 Screw x4/4; 4 Bracket x2/2; 5 Bracket mirror x1/1; "
       "6 Spacer x2/2; 7 Motor x1/1; ");
  CHECK_EQ(row(indented, "Wheel")["level"], 2);
  CHECK_EQ(row(indented, "Wheel")["nodes"].size(), 2u);  // both units' wheels: balloons find their row by node
  CHECK_THROWS(r.bom("flat"));
}

TEST(bom_identity_options) {
  Robot r;
  drawing::BomOptions o;
  o.match_shapes = false;
  same(table(r.bom("parts", o)), "1 Base x1; 2 Wheel x2; 3 Axle x2; 4 Screw x4; 5 Bracket x1; 6 Bracket copy x1; 7 Bracket mirror x1; 8 Spacer x2; 9 Motor x1; ");
  o = {};
  o.references = true;
  same(table(r.bom("parts", o)), "1 Base x1; 2 Wheel x2; 3 Axle x2; 4 Screw x4; 5 Bracket x2; 6 Bracket mirror x1; 7 Spacer x2; 8 Guard x1; 9 Motor x1; ");
  // Another material makes another part; one component's BoM.
  const Scene s = resolve(r.doc);
  set(r.doc, "Bracket copy", {{"material", "PLA"}});
  same(table(r.bom("parts")), "1 Base x1; 2 Wheel x2; 3 Axle x2; 4 Screw x4; 5 Bracket x1; 6 Bracket copy x1; 7 Bracket mirror x1; 8 Spacer x2; 9 Motor x1; ");
  o = {};
  o.root = id_of(s, "Fasteners");
  const json fasteners = r.bom("top", o);
  CHECK_EQ(fasteners["assembly"]["name"], "Fasteners");
  same(table(fasteners), "1 Screw x4; ");
  o.root = id_of(s, "Base");
  same(table(r.bom("top", o)), "1 Base x1; ");
  o.root = new_uuid();
  CHECK_THROWS(r.bom("top", o));
}

// Design copies: a circular pattern stores each copy's geometry under its own key, turned; a mirrored notched block
// is another part.
TEST(bom_design_copies) {
  Document doc = Document::create();
  const std::string block = run(doc, "feature", {{"kind", "box"}, {"inputs", {{"x", "40 mm"}, {"length", "20 mm"}, {"width", "10 mm"}, {"height", "6 mm"}}}})["body_ids"][0];
  run(doc, "feature", {{"kind", "box"},
                       {"inputs", {{"plane", {{"origin", {0, 0, 3}}, {"normal", {0, 0, 1}}}}, {"x", "47 mm"}, {"y", "2 mm"}, {"length", "8 mm"}, {"width", "8 mm"}, {"height", "8 mm"}, {"operation", "cut"},
                                   {"targets", {block}}}}});
  run(doc, "feature", {{"kind", "pattern_circ"}, {"inputs", {{"bodies", json::array({{{"body", block}, {"kind", "body"}}})}, {"count", "4"}}}});
  run(doc, "feature", {{"kind", "mirror"}, {"inputs", {{"bodies", json::array({{{"body", block}, {"kind", "body"}}})}, {"plane", {{"base", "yz"}}}}}});
  const Scene s = resolve(doc);
  std::set<std::string> keys;
  for (const auto& id : s.all_bodies()) keys.insert(s.node(id)->body_key);
  CHECK_EQ(s.all_bodies().size(), 5u);
  CHECK_EQ(keys.size(), 5u);
  same(table(drawing::bom(doc, s)), "1 Box1 x4; 2 Box1 5 x1; ");
  CHECK_EQ(drawing::bom(doc, s)["rows"][0]["source"], "design");
  // The mirror image of a symmetric part is the part turned.
  Document pin = Document::create();
  const std::string body = run(pin, "feature", {{"kind", "cylinder"}, {"inputs", {{"x", "30 mm"}, {"diameter", "6 mm"}, {"height", "20 mm"}}}})["body_ids"][0];
  run(pin, "feature", {{"kind", "mirror"}, {"inputs", {{"bodies", json::array({{{"body", body}, {"kind", "body"}}})}, {"plane", {{"base", "yz"}}}}}});
  same(table(drawing::bom(pin, resolve(pin))), "1 Cylinder1 x2; ");
}

TEST(bom_masses) {
  Robot r;
  const json parts = r.bom("parts");
  CHECK_NEAR(row(parts, "Base")["mass"].get<double>(), 80000 * 7.85 / 1000, 1e-6);
  CHECK_EQ(row(parts, "Base")["material"], "Steel");
  CHECK_EQ(row(parts, "Base")["material_id"], "steel");
  CHECK_NEAR(row(parts, "Wheel")["mass"].get<double>(), M_PI * 400 * 10 * 1.01 / 1000, 1e-6);
  CHECK_NEAR(row(parts, "Wheel")["total_mass"].get<double>(), 2 * M_PI * 400 * 10 * 1.01 / 1000, 1e-6);
  CHECK_EQ(row(parts, "Wheel")["material"], "PA12");
  CHECK_NEAR(row(parts, "Screw")["mass"].get<double>(), M_PI * 4 * 12 * 8.0 / 1000, 1e-6);
  CHECK_EQ(row(parts, "Axle")["mass_error"], "no material");
  CHECK_EQ(row(parts, "Motor")["mass"], 350);
  CHECK_EQ(parts["totals"]["mass_complete"], false);
  CHECK_EQ(row(r.bom("top"), "Wheel unit")["mass_error"], "a part in it has no mass");
  // Everything with a material: the totals add up, in kg.
  for (const char* name : {"Bracket", "Bracket copy", "Bracket mirror", "Spacer", "Spacer alt"}) set(r.doc, name, {{"material", "POM"}});
  const Scene s = resolve(r.doc);
  for (const auto& [id, n] : s.nodes)
    if (n.name == "Axle" && s.node(n.parent)->name != "Motor") run(r.doc, "part_properties", {{"target", id}, {"set", {{"material", "steel"}}}});
  drawing::BomOptions kg;
  kg.mass_unit = "kg";
  const json done = r.bom("top", kg);
  CHECK_EQ(done["totals"]["mass_complete"], true);
  const double bracket = 30 * 20 * 10 - 125, wheel = M_PI * 400 * 10 * 1.01, axle = M_PI * 9 * 30 * 7.85, screw = M_PI * 4 * 12 * 8.0;
  const double total = 80000 * 7.85 + 2 * (wheel + axle) + 4 * screw + 3 * bracket * 1.41 + (500 + 600) * 1.41 + 350 * 1000;
  CHECK_NEAR(done["totals"]["mass"].get<double>(), total / 1e6, 1e-9);
  CHECK_NEAR(row(done, "Wheel unit")["mass"].get<double>(), (wheel + axle) / 1e6, 1e-9);
  CHECK_NEAR(row(done, "Wheel unit")["total_mass"].get<double>(), 2 * (wheel + axle) / 1e6, 1e-9);
  CHECK_EQ(done["mass_unit"], "kg");
  set(r.doc, "Fasteners", {{"mass", 100}});  // a mass property stands for the whole assembly
  const json given = r.bom("top", kg);
  CHECK_NEAR(row(given, "Fasteners")["mass"].get<double>(), 0.1, 1e-12);
  CHECK_NEAR(given["totals"]["mass"].get<double>(), (total - 4 * screw + 100 * 1000) / 1e6, 1e-9);
  drawing::BomOptions none;
  none.mass = false;
  CHECK(!r.bom("parts", none)["totals"].contains("mass"));
  CHECK(!row(r.bom("parts", none), "Base").contains("mass_error"));
}

TEST(bom_csv) {
  Robot r;
  const json b = r.bom("indented");
  const std::string csv = drawing::bom_csv(b);
  CHECK(csv.rfind("\xEF\xBB\xBF" "Item,Level,Qty,Total qty,Part number,Name,Description,Material,Mass (g),Total mass (g),Vendor,Purchased,Source,Notes,finish\r\n", 0) == 0);
  size_t lines = 0;
  for (size_t at = csv.find("\r\n"); at != std::string::npos; at = csv.find("\r\n", at + 2)) ++lines;
  CHECK_EQ(lines, b["rows"].size() + 1);
  CHECK(csv.find("\r\n1,1,1,1,,Base,\"Plate, \"\"flat\"\"\",Steel,628.00,628.00,,,robot.step,'=1+2,painted\r\n") != std::string::npos);
  CHECK(csv.find("\r\n7,1,1,1,M-42,Motor,,,350.00,350.00,,yes,robot.step,,\r\n") != std::string::npos);
  CHECK(csv.find("\r\n2.2,2,1,2,,Axle,,,,,,,robot.step,,\r\n") != std::string::npos);
  const std::string semicolons = drawing::bom_csv(r.bom("top"), ';', {{"name", "Benennung"}});
  CHECK(semicolons.find("Item;Qty;Part number;Benennung;") != std::string::npos);
  CHECK(semicolons.find(";\"Plate, \"\"flat\"\"\";Steel;628.00;") != std::string::npos);  // quoted for its quotes
  const std::string translated = drawing::bom_csv(b, ',', {{"item", "Pos."}, {"yes", "ja"}});
  CHECK(translated.rfind("\xEF\xBB\xBF" "Pos.,Level,", 0) == 0);
  CHECK(translated.find("\r\n7,1,1,1,M-42,Motor,,,350.00,350.00,,ja,robot.step,,\r\n") != std::string::npos);
}

// File > Export bill of materials works on a STEP file opened in viewer mode (live shapes, no BREP text, random keys): the
// same rows and quantities as the file imported for editing, the masses from the file's own material names.
TEST(bom_viewer_mode) {
  Robot r;
  const auto dir = std::filesystem::temp_directory_path() / ("opad-bom-" + new_uuid());
  std::filesystem::create_directories(dir);
  const auto step = dir / "robot.step";
  json solids = json::array();  // the mesh guard stays out of a STEP file
  const Scene s = resolve(r.doc);
  for (const auto& id : s.node(r.root)->children)
    if (s.node(id)->representation != "mesh") solids.push_back(id);
  run(r.doc, "export", {{"format", "step"}, {"out", step.string()}, {"select", solids}});
  ImportOptions viewer;
  viewer.viewer = true;
  Document viewed = browse_step(step, viewer);
  CHECK(viewed.has_live_bodies());
  Document edited = Document::create();
  import_step(edited, step);
  drawing::BomOptions o;
  o.mass = false;
  for (const char* mode : {"parts", "indented"}) {
    o.mode = mode;
    same(table(drawing::bom(viewed, resolve(viewed), o)), table(drawing::bom(edited, resolve(edited), o)));
  }
  // Masses: a material on the top component reaches every body, live ones too.
  for (const auto& root : resolve(viewed).roots) run(viewed, "part_properties", {{"target", root}, {"set", {{"material", "S235JR"}}}});
  o.mass = true;
  o.mode = "parts";
  const json masses = drawing::bom(viewed, resolve(viewed), o);
  CHECK(masses["totals"]["mass_complete"].get<bool>());
  CHECK_NEAR(row(masses, "Spacer")["mass"].get<double>(), 10 * 10 * 5 * 7.85 / 1000, 1e-6);
  std::filesystem::remove_all(dir);
}

TEST(bom_command) {
  Robot r;
  const auto dir = std::filesystem::temp_directory_path() / ("opad-bom-" + new_uuid());
  const std::string csv = (dir / "robot.csv").string(), js = (dir / "robot.json").string();
  const json wrote = run(r.doc, "bom", {{"format", "csv"}, {"out", csv}, {"mode", "parts"}});
  CHECK_EQ(wrote["rows"], 8);
  const std::string text = read_text_file(csv);
  CHECK(text.rfind("\xEF\xBB\xBF" "Item,Qty,", 0) == 0);
  CHECK_EQ(run(r.doc, "bom", {{"format", "csv"}})["csv"].get<std::string>(), drawing::bom_csv(r.bom("parts")));
  run(r.doc, "bom", {{"mode", "indented"}, {"out", js}, {"mass_unit", "kg"}});
  const json back = json::parse(read_text_file(js));
  CHECK_EQ(back["mode"], "indented");
  CHECK_EQ(back["rows"].size(), 10u);
  CHECK_EQ(run(r.doc, "bom", {{"mode", "top"}})["rows"].size(), 7u);
  CHECK_THROWS(run(r.doc, "bom", {{"format", "xlsx"}}));
  CHECK_THROWS(run(r.doc, "bom", {{"separator", "|"}}));
  CHECK_THROWS(run(r.doc, "bom", {{"mass_unit", "oz"}}));
  std::filesystem::remove_all(dir);
}

CHECK_MAIN()
