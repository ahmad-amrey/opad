#pragma once
// Design checks beyond "is each solid valid" (TODO 10 B13, B17). Worker-only: they walk the geometry, and take a
// `cancelled` callback so a job can stop them.
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>

#include <functional>
#include <string>
#include <vector>

#include "document.hpp"
#include "scene.hpp"

namespace opad {

// Pairs of solid bodies that overlap (the overlap's volume and box) and, with clearance_mm, pairs closer than that.
// args: select (bodies/components; default every visible solid), clearance_mm, ignore ([[a, b], ...] pairs meant
// to overlap; components stand for their bodies), max_pairs (default 20000), offset/limit over the findings.
// Candidate pairs come from bounding boxes; only those get the exact common / distance. Touching is not overlap.
json check_interference(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled = {});
// The same over given bodies, their shapes and boxes from the caller: a design plan has shapes the document has not
// stored yet (the interference feature, gap log #10).
json interference_of(const Scene& scene, const std::vector<std::string>& bodies, const json& args, const std::function<TopoDS_Shape(const std::string&)>& shape_of,
                     const std::function<Bnd_Box(const std::string&)>& box_of, const std::function<bool()>& cancelled = {});

// 3D-print checks per solid body (TODO 10 B13): faces overhanging more than overhang_deg from vertical (default 45)
// against build_direction ("+z" default, or [x, y, z]), the contact area on the build plate, walls thinner than
// min_wall_mm (default 0.8) where sampled rays cross the body, and thin features (faces narrower than min_wall_mm).
// args: select, offset/limit over the bodies. Bounded: at most 36 samples per face and 4000 per body.
json check_print(const Document& doc, const Scene& scene, const json& args, const std::function<bool()>& cancelled = {});

}  // namespace opad
