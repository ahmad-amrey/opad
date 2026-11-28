#pragma once
// High-level shapes in sketch geometry (TODO 10 B4). A sketch's geometry may carry "shapes": [{kind, picks: [[u, v],
// ...], options, first_id}], which are expanded into ordinary points, entities and constraints before the op is
// stored, so the op format, replay and merges never see them.
#include "../json.hpp"

namespace opad::design {
class ParamTable;

// The kinds `shapes` accepts: every create_primitive kind plus "offset" (options: shape, the index of an earlier shape,
// or entity / entities; distance, + outward and - inward; round).
const std::vector<std::string>& sketch_shape_kinds();

// Expands geometry["shapes"] in order after the listed points, entities and constraints; any of those without an
// "id" gets the next free one. A shape's items take the next ids, or consecutive ones from its "first_id". Returns
// the geometry without "shapes"; `id_map` gets one {shape, kind, points, entities, constraints} per shape.
// A pick coordinate or a length option (radius, diameter, width, height, distance, fillet, fillets) may be an expression
// of `params` ("width / 2", "10 mm"), evaluated once here: the stored curves hold the values (a dimension with expr
// keeps following a parameter). Without params an expression is refused.
json expand_sketch_shapes(const json& geometry, json* id_map = nullptr, const ParamTable* params = nullptr);

}  // namespace opad::design
