#pragma once
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>
#include <functional>
#include "document.hpp"
#include "scene.hpp"

namespace opad {

json document_info(const Document& doc, const Scene& scene);
// geometry=false skips the measurements that walk the geometry (bbox, counts, volume, area) so the call
// is O(1); the app fills those in separately off the click path.
json node_properties(const Document& doc, const Scene& scene, const std::string& node_id, bool geometry = true,
                     const std::function<bool()>& cancelled = {});
json inspect_ref(const Document& doc, const Scene& scene, const Ref& ref);
// cancelled: polled while the distance is computed (a body-to-body distance can take a while); a true
// answer ends it with Error("cancelled").
json measure_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b, const std::function<bool()>& cancelled = {});
// Edge-to-edge distance with selectable points of interest. The picked positions are projected onto their
// edges, then snap to nearby endpoints, midpoints or curve extrema within snap_tolerance. The returned
// `anchors` array also contains the closest, farthest and endpoint-based alternatives.
json measure_edge_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b,
                           const Vec3& picked_a, const Vec3& picked_b, double snap_tolerance,
                           const std::function<bool()>& cancelled = {});
json measure_angle(const Document& doc, const Scene& scene, const Ref& a, const Ref& b);
json measure_radius(const Document& doc, const Scene& scene, const Ref& a);
json measure_bbox(const Document& doc, const Scene& scene, const std::vector<Ref>& refs);
// {min, max, size, center, diagonal} of a box ({} for an empty one), as every result reports boxes.
json bbox_to_json(const Bnd_Box& box);
// What the entity filters look at for one face, edge or vertex (world coordinates): type, curve or surface, radius,
// direction (straight edges), normal (planar faces), axis, bbox.
json describe_entity(const TopoDS_Shape& sub);
// Whether a description passes the filters of query_entities and rule selectors (TODO 10 B7): curve, surface,
// radius_min/max, parallel_to ("x"|"y"|"z" or [x,y,z]), normal ("+x".. or [x,y,z]), at_plane {axis, value},
// bounds {min, max}. Throws for a filter it does not know.
bool entity_matches(const json& detail, const json& filters, double tolerance = 1e-5);
// Axis-aligned world bounding box of body nodes (empty list = all visible bodies). False when empty.
bool scene_bbox(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, Vec3& lo, Vec3& hi);
// The same as sizes are reported (TODO 10 B10): the union of the bodies' tight boxes (node_tight_bbox), not the
// padded view boxes scene_bbox adds up. Walks the geometry the first time per body: workers only.
bool scene_tight_bbox(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, Vec3& lo, Vec3& hi,
                      const std::function<bool()>& cancelled = {});

}  // namespace opad
