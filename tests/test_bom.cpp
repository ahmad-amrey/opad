// Materials and mass (TODO 11 UI-140): the library, the names it maps, which node's material decides, masses through
// placements and overrides, and the properties command.
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <gp_Pln.hxx>

#include <cmath>
#include <set>

#include "check.hpp"
#include "opad/commands.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include "opad/materials.hpp"

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

CHECK_MAIN()
