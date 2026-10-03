#pragma once
// Bill of materials (TODO 11 UI-83): the parts and assemblies of a document, or of one component, with quantities, part
// properties (properties ops: part_number, description, material, vendor, notes, any other field) and masses
// (materials.hpp).
// A part is a body, or a component marked bom: purchased (bought as one: what is in it is not listed). Its identity is
// its part_number when it has one, else its shape with its material: the body key, and with match_shapes also the keys
// of the same solid moved and turned (design patterns and copies store each copy's geometry), never its mirror image. An
// assembly's identity is its part_number, else its items with their placements in it. Equal identities are one row
// with a quantity; a row's name is its nodes' name without instance numbers ("Bolt 1", "Bolt 2" -> "Bolt").
// Left out: bom: exclude (and everything under it), mesh and drawing bodies (reference objects; references keeps them)
// and components with nothing left in them. A document with one root component is that assembly.
// Modes: top (the assembly's own items), parts (every part once, quantities summed over the whole product), indented
// (assemblies with their items below them: qty per assembly, total_qty in the product; items numbered 1, 1.2, 1.2.1).
#include <functional>
#include <string>

#include "opad/document.hpp"
#include "opad/scene.hpp"

namespace opad::drawing {

struct BomOptions {
  std::string mode = "parts";   // top | parts | indented
  std::string root;             // a component or body (default: the document)
  bool mass = true;             // walks the geometry the first time: workers only
  bool match_shapes = true;
  bool references = false;
  std::string mass_unit = "g";  // g | kg | lb
  std::function<bool()> cancelled;
};

// {"mode", "assembly": {id, name} | null, "mass_unit", "columns": [custom property names], "rows": [{"item", "qty",
//  ("level", "total_qty" indented), "kind": part|assembly, "name", "part_number", "description", "material",
//  "material_id", "density", "mass", "total_mass" | "mass_error", "vendor", "notes", "purchased", "source",
//  "properties": {custom}, "nodes": [ids], "identity"}], "totals": {"rows", "parts", "mass", "mass_complete"}}.
// Text fields are left out when empty. Throws Error for a wrong mode, unit or root, and "cancelled".
json bom(const Document& doc, const Scene& scene, const BomOptions& options = {});
// RFC 4180 text: UTF-8 with a byte order mark, CRLF, fields quoted when they need it, text that a spreadsheet would
// take for a formula (=, +, -, @) prefixed with '. labels: header text by column (item, level, qty, total_qty,
// part_number, name, description, material, mass, total_mass, vendor, purchased, source, notes).
std::string bom_csv(const json& bom, char separator = ',', const json& labels = json::object());

}  // namespace opad::drawing
