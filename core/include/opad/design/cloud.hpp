#pragma once
// Point clouds in the design engine (design/cloud_features.cpp): the features are in build_specs (cloud_outliers,
// cloud_downsample, cloud_normals, cloud_crop, cloud_slice); this is what turns a slice into sketch geometry.
#include <string>

#include "../document.hpp"
#include "../scene.hpp"

namespace opad::design {

// A sketch op (no results: give it to plan_ops) whose geometry is the profile of a cloud's slice: its points chained into
// polylines, thinned to `tolerance` mm (0: the points' spacing) and drawn as lines, closed loops closed. The plane: the
// slice's own when `body` was made by Slice points; else `plane` (a plane input: {"base"} | {"face"} | {"feature"}) with
// the points within `band` mm of it. `gap` (0: automatic) is the jump that ends a chain. Throws when no polyline comes out.
// `report` (optional) gets {"polylines", "lines", "points"}.
json slice_sketch_op(const Document& doc, const Scene& scene, const std::string& body, const json& plane, double band, double tolerance,
                     double gap, const std::string& name, json* report = nullptr);

}  // namespace opad::design
