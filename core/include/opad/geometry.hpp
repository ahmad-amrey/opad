#pragma once
// OCCT bridge: BREP text <-> TopoDS_Shape, sub-shape ordinals, transforms.
#include <Bnd_Box.hxx>
#include <gp_Circ.hxx>
#include <map>
#include <functional>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>

#include "document.hpp"
#include "scene.hpp"

namespace opad {

std::string brep_from_shape(const TopoDS_Shape& s);
TopoDS_Shape shape_from_brep(const std::string& brep);

// Prototype shape (identity placement) of a body-store entry; cached per document.
TopoDS_Shape body_shape(const Document& doc, const std::string& key);
// Viewer mode: registers a live shape under `key` so body_shape() never has to parse BREP text for it.
void cache_shape(const Document& doc, const std::string& key, const TopoDS_Shape& shape);
// Shape of a body node placed in world coordinates.
TopoDS_Shape node_world_shape(const Document& doc, const Scene& scene, const std::string& node_id);
// Parses every body entry into the cache (and computes its bbox) so later body_shape/body_bbox calls are
// O(1). Meant for a loading thread; progress(i, n) may return false to stop early.
void warm_shape_cache(const Document& doc, const std::function<bool(size_t, size_t)>& progress = {});
// Axis-aligned bounding box of a body-store entry in its own frame, cached per document. The first call
// walks the geometry (or the triangulation when present); the app warms it on the meshing thread.
Bnd_Box body_bbox(const Document& doc, const std::string& key);
Bnd_Box body_bbox(ShapeCache& cache, const std::string& key, const TopoDS_Shape& proto);  // same, for callers that hold the cache
// Replaces the cached box by the one of the body's triangulation. The box of an unmeshed body comes from its
// surfaces' poles and untrimmed extents, which can reach metres beyond a small part (and Fit All with it).
Bnd_Box refine_body_bbox(ShapeCache& cache, const std::string& key, const TopoDS_Shape& meshed);
// World-space box of a body node: the cached prototype box with its corners transformed (O(1) per node).
Bnd_Box node_world_bbox(const Document& doc, const Scene& scene, const std::string& node_id);
// Tight boxes (TODO 10 B3/B10): BRepBndLib::AddOptimal on the exact geometry, without the tolerance and meshing
// padding of the boxes above (those fit views and cull; these are sizes to report). A body's box is cached by its
// key and shifted for a translated node; a turned node is measured in place. Walks the geometry the first time:
// workers only.
Bnd_Box tight_bbox(const TopoDS_Shape& shape);
Bnd_Box node_tight_bbox(const Document& doc, const Scene& scene, const std::string& node_id);
// Triangulation-only bodies use facet, facet-side and mesh-node ordinals.
bool is_mesh_shape(const TopoDS_Shape& shape);
struct MeshCircle { gp_Circ circle; int index=0, segments=0; std::vector<int> edges; std::vector<gp_Pnt> rim; };
std::vector<MeshCircle> mesh_circles(const TopoDS_Shape& shape);
// Sub-shape by ordinal in the prototype (faces/edges/vertices are enumerated in TopExp_Explorer order).
TopoDS_Shape subshape(const TopoDS_Shape& proto, Ref::Kind kind, int index);
int subshape_count(const TopoDS_Shape& proto, Ref::Kind kind);
int subshape_index(const TopoDS_Shape& proto, const TopoDS_Shape& sub);  // -1 when not found

Mat4 mat_from_trsf(const gp_Trsf& t);
gp_Trsf trsf_from_mat(const Mat4& m);  // throws Error for non-rigid matrices
bool mat_is_rigid(const Mat4& m);

// SHA-256 body key for a shape (hashes its canonical BREP text).
std::string body_key_for(const TopoDS_Shape& s, std::string* brep_out = nullptr);

}  // namespace opad
