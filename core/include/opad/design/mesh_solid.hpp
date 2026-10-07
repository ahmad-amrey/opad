#pragma once
// Mesh to solid as design ops (design.mesh_solid): a mesh body rebuilt as a sketch and an extrusion or a revolution when it
// is one (the profile's segments fitted with lines, arcs, circles and splines: a faceted cylinder becomes one circle
// extruded), else as a "mesh_solid" feature computing the general B-rep (opad/mesh_solid.hpp). Every result is measured
// against the mesh (MeshDeviation): an extrusion or revolution is only offered when it follows the mesh within the
// tolerance. Planning walks geometry: workers only.
#include <optional>
#include <string>
#include <vector>

#include "../mesh_solid.hpp"
#include "feature.hpp"
#include "sketch.hpp"

namespace opad::design {

// A profile found in a mesh: the sketch (in `frame`) and what the feature does with it.
struct MeshProfile {
  std::string kind;            // extrude | revolve
  Frame frame;
  Sketch sketch;
  std::vector<std::array<double, 2>> regions;  // a point inside each profile region the feature takes (sketch coordinates)
  double distance = 0;         // extrude: along the frame's normal
  int axis = 0;                // revolve: the construction line on the axis (an entity of sketch)
  int curves() const;          // the profile's entities that are not construction
};
// The extrusions and revolutions the mesh is, simplest first (fewest curves); empty when it is neither.
std::vector<MeshProfile> mesh_profiles(const TriMesh& mesh, double tolerance, double angle_deg, const Cancel& cancel = {});

// A 2D polyline of mesh vertices (closed: the last point joins the first) as sketch curves: lines where the points are
// straight, arcs and circles where a circle holds them within the tolerance, splines for smooth runs that neither fits.
// Corners are where the polyline turns by more than `max_turn` (radians); a segment is a chord of an arc only when it cuts
// inside it by less than `chord` (the mesh's own chord error; at least the tolerance). Appends to sketch; returns the ids.
std::vector<int> fit_profile(Sketch& sketch, const std::vector<std::array<double, 2>>& points, bool closed, double tolerance, double max_turn,
                             double chord = 0);

struct MeshConversion {
  std::string method;  // extrude | revolve | solid
  Plan plan;           // ready to commit (sketch and feature ops, the new body's name, the source's removal)
  TopoDS_Shape shape;  // the new body, world coordinates
  MeshDeviation deviation;
  json report;         // method, faces by surface kind, curves, deviation, tolerance ...
};
struct MeshConversionOptions {
  MeshSolidOptions solid;
  std::string mode = "auto";  // auto: a sketch and a feature when the mesh is one; solid: the general B-rep only
  std::string name;           // the new body's name (empty: the mesh's name)
  bool remove_source = false; // a Remove feature takes the mesh out from here on (history kept)
  std::string component;      // the ops are made in this component
};
// `node`: a mesh body of the scene.
MeshConversion convert_mesh(const Document& doc, const Scene& scene, const std::string& node, const MeshConversionOptions& options,
                            const Cancel& cancel = {});

}  // namespace opad::design
