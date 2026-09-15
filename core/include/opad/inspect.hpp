#pragma once
#include "document.hpp"
#include "scene.hpp"

namespace opad {

json document_info(const Document& doc, const Scene& scene);
// geometry=false skips the measurements that walk the geometry (bbox, counts, volume, area) so the call
// is O(1); the app fills those in separately off the click path.
json node_properties(const Document& doc, const Scene& scene, const std::string& node_id, bool geometry = true);
json inspect_ref(const Document& doc, const Scene& scene, const Ref& ref);
json measure_distance(const Document& doc, const Scene& scene, const Ref& a, const Ref& b);
json measure_angle(const Document& doc, const Scene& scene, const Ref& a, const Ref& b);
json measure_radius(const Document& doc, const Scene& scene, const Ref& a);
json measure_bbox(const Document& doc, const Scene& scene, const std::vector<Ref>& refs);
// Axis-aligned world bounding box of body nodes (empty list = all visible bodies). False when empty.
bool scene_bbox(const Document& doc, const Scene& scene, const std::vector<std::string>& bodies, Vec3& lo, Vec3& hi);

}  // namespace opad
