#include "opad/sim/inertia.hpp"

#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Mat.hxx>

#include <cmath>

#include "opad/geometry.hpp"
#include "opad/materials.hpp"

namespace opad::sim {

PartMass part_mass(const Document& doc, const Scene& scene, const std::string& node, double default_density) {
  PartMass out;
  const Node* n = scene.node(node);
  if (!n) throw Error("no node " + node);
  const std::vector<std::string> bodies = n->kind == Node::Kind::Body ? std::vector<std::string>{node} : scene.bodies_under(node);
  GProp_GProps total;
  bool any = false;
  for (const auto& b : bodies) {
    const Node* bn = scene.node(b);
    if (!bn || bn->body_missing || bn->representation != "solid") continue;
    TopoDS_Compound solids;
    BRep_Builder builder;
    builder.MakeCompound(solids);
    int count = 0;
    for (TopExp_Explorer e(node_world_shape(doc, scene, b), TopAbs_SOLID); e.More(); e.Next(), ++count) builder.Add(solids, e.Current());
    if (!count) {
      out.notes.push_back("\"" + bn->name + "\" has no solid: left out of the mass");
      continue;
    }
    MaterialChoice m = material_of(doc, scene, b);
    double density = m.density;  // g/cm3
    if (density <= 0) {
      density = default_density;
      out.notes.push_back("\"" + bn->name + "\" has no material: taken as steel (" + std::to_string(default_density).substr(0, 4) + " g/cm3)");
    }
    GProp_GProps g;
    BRepGProp::VolumeProperties(solids, g);
    if (g.Mass() <= 0) continue;
    double rho = density * 1e-6;  // kg/mm3
    // A body's own mass property (grams) wins over its volume x density.
    if (const double given = property_number(bn->properties.value("mass", json())); given > 0) rho = given / 1000 / g.Mass();
    total.Add(g, rho);
    ++out.bodies;
    any = true;
  }
  if (!any) throw Error("\"" + n->name + "\" has no solid body to weigh");
  out.mass = total.Mass();
  const gp_Pnt c = total.CentreOfMass();
  out.centre = {c.X(), c.Y(), c.Z()};
  const gp_Mat I = total.MatrixOfInertia();  // about the centre of mass, world axes
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) out.inertia[size_t(r * 3 + k)] = I(r + 1, k + 1);
  return out;
}

}  // namespace opad::sim
