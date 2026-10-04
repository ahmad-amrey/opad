#pragma once
#include <Bnd_Box.hxx>
#include <TopoDS_Shape.hxx>
#include <functional>
#include <optional>
#include "document.hpp"
#include "scene.hpp"

namespace opad {

json document_info(const Document& doc, const Scene& scene);
// geometry=false skips the measurements that walk the geometry (bbox, counts, volume, area) so the call
// is O(1); the app fills those in separately off the click path.
json node_properties(const Document& doc, const Scene& scene, const std::string& node_id, bool geometry = true,
                     const std::function<bool()>& cancelled = {});
json inspect_ref(const Document& doc, const Scene& scene, const Ref& ref);
// Where a note pinned to `ref` is drawn (world): a point itself; the centre of a body's tight box, of a component's (its
// bodies') or of a sketch's; a sub-shape's centre as inspect_ref gives it (a face's centroid, a circle's centre, a vertex,
// an edge's start), else its box centre. No mass properties. Throws when the reference does not resolve. Measures the
// geometry the first time per body: workers only, but for a point.
Vec3 annotation_anchor(const Document& doc, const Scene& scene, const Ref& ref);
// cancelled: polled while the distance is computed (a body-to-body distance can take a while); a true
// answer ends it with Error("cancelled").
json measure_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b, const std::function<bool()>& cancelled = {});
// The distance between two shapes, surface to surface: value, point_a, point_b, delta, and approximate with
// tolerance_mm when only the meshes could say (gap log #4). measure_distance adds the refs and warnings.
json shape_distance(const TopoDS_Shape& s1, const TopoDS_Shape& s2, const std::function<bool()>& cancelled = {});
// Edge-to-edge distance with selectable points of interest. The picked positions are projected onto their
// edges, then snap to nearby endpoints, midpoints or curve extrema within snap_tolerance. The returned
// `anchors` array also contains the closest, farthest and endpoint-based alternatives.
json measure_edge_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b,
                           const Vec3& picked_a, const Vec3& picked_b, double snap_tolerance,
                           const std::function<bool()>& cancelled = {});
// Centre to centre (UI-144): between the references' centres, as centre_a / centre_b name them: a circle's, an ellipse's or
// a sphere's centre, a cylinder's or cone's axis where it passes the face's centroid, a face's or curve's centroid, a solid
// body's volume centroid (another body's area centroid), a vertex or point itself. mode "center".
json measure_center_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b);
// The farthest two points of the references (UI-144), mode "max": among their vertices, their edges sampled finely and the
// mesh nodes of their curved faces (a plane's farthest point is on its boundary), then climbed to the farthest pair. Exact
// between straight-edged, flat shapes; else approximate within tolerance_mm. cancelled: as measure_distance.
json measure_max_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b, const std::function<bool()>& cancelled = {});
// Length and area (UI-144). An edge: kind "length", its length along the curve, start, end, its middle as point, and loops:
// each wire of a face around it that holds it, with that face, its perimeter (seams left out), its edge count and whether it
// is the face's outer loop. A face: kind "area", its area, centroid as point, perimeter (every loop), outer_perimeter and
// loops. A body: kind "area", its surface area and, for a solid, its volume.
json measure_length(const Document& doc, const Scene& scene, const Ref& a);
json measure_angle(const Document& doc, const Scene& scene, const Ref& a, const Ref& b);
json measure_radius(const Document& doc, const Scene& scene, const Ref& a);
json measure_bbox(const Document& doc, const Scene& scene, const std::vector<Ref>& refs);
// The area a pick encloses and its perimeter (UI-90, measure_area.cpp). Faces (a drawing's fills, a solid's faces): their
// exact area and every boundary's length. Edges: the closed loops they form (a loop inside another is a hole); one edge
// that does not close alone grows into the smallest loop it lies on among its drawing body's edges (ends meeting within
// tolerance, dangling edges left out; every edge cut where another crosses it or ends on it, the part of the picked edge
// nearest `clicked`, else at its middle; cells narrower than a few tolerances are noise). Picked edges that run past each other are trimmed where they meet
// when that closes them (trimmed). Points (vertices, centres, free points): the polygon through them, closed back to
// the first. {kind "area", value (mm²), perimeter (mm), loops, holes, closed, grown, boundary [[world point..]..] (at most
// ~2000 points), center (inside the area, for its label), normal, refs}; while the picks do not close: closed false,
// value 0, open_ends (and ends, their points). Throws Error for a body or a mix of fills, objects and points.
json measure_area(const Document& doc, const Scene& scene, const std::vector<Ref>& refs, const std::function<bool()>& cancelled = {},
                  const std::optional<Vec3>& clicked = std::nullopt);
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
