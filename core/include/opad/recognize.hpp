#pragma once
// Feature recognition on a solid's faces and edges (TODO 11 UI-97): the groups a selection grows to on bodies without
// history (imported, or dumb) and what Remove faces takes away. Every group follows an explicit rule (how two faces
// meet at an edge, a surface type, an axis, a radius, a thickness): no similarity scores, no nearest-neighbour guesses.
//   edges:  smooth (tangent faces), convex or concave (the material's angle under / over 180 degrees), open.
//   tangent  faces joined by smooth edges; edges continuing each other tangentially
//   loop     the closed chain of a face's boundary holding the picked edges
//   fillet   blends (cylinder, torus, sphere or a surface of one cross radius, two or more smooth edges) of one radius
//   chamfer  narrow planes or cones between two faces they meet at equal angles, chained across their ends
//   hole     a concave cylinder with its coaxial steps, cones and floor: through or blind, counterbore, countersink
//   boss / pocket  the faces an inner loop of a face encloses, or a region closed by concave / convex edges
//   wall     a plane and the parallel face facing the other way thinner than a quarter of it, with its rims
//   similar  the body's entities of the same rule: holes of a diameter, fillets of a radius, faces of a normal ...
// Walks the geometry: workers only.
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <TopoDS_Shape.hxx>

#include "json.hpp"

namespace opad {

struct Recognized {
  std::string kind;               // tangent, loop, fillet, chamfer, hole, boss, pocket, wall, similar
  std::string rule;               // similar: what the members share (hole, fillet, chamfer, wall, radius, normal, area, direction, length, type)
  std::string label;              // English: "Hole Ø6 through", "Fillet chain R3", "Wall 2 mm", "All holes Ø6"
  std::vector<int> faces, edges;  // ordinals from 0 in TopExp::MapShapes order, as references count them
  json params = json::object();   // diameter, depth, through, type, radius, convex, distance, thickness, count, ...
};

class Recognizer {
 public:
  enum class Join { Smooth, Convex, Concave, Open };  // Open: one face (a seam, a sheet's border) or degenerate

  // `body` in any frame (sizes and directions are reported in it): world coordinates for what users see.
  explicit Recognizer(const TopoDS_Shape& body, std::function<bool()> cancelled = {});
  ~Recognizer();
  Recognizer(const Recognizer&) = delete;
  Recognizer& operator=(const Recognizer&) = delete;

  int face_count() const;
  int edge_count() const;
  Join join(int edge);

  Recognized tangent_faces(const std::vector<int>& faces);
  Recognized tangent_edges(const std::vector<int>& edges);
  std::vector<Recognized> loops(const std::vector<int>& edges);  // one per face boundary holding them all
  std::optional<Recognized> fillet(int face);                     // the chain of blends of its radius through it
  std::optional<Recognized> chamfer(int face);
  std::optional<Recognized> hole(int face);
  std::optional<Recognized> wall(int face);
  std::optional<Recognized> boss_or_pocket(const std::vector<int>& faces);  // the smallest holding them all
  // Every group of a kind on the body (hole, fillet, chamfer, wall, boss, pocket), in face order.
  std::vector<Recognized> all(const std::string& kind);
  // Entities following the picked one's rule (faces: radius, normal, area; edges: direction, radius, length).
  std::vector<Recognized> similar_faces(int face);
  std::vector<Recognized> similar_edges(int edge);
  // Every group holding the picks (a picked edge is held when a face of it is), the kinds asked for, groups first
  // (smallest first), then chains and loops, then similar ones. Groups equal to the picks themselves are left out,
  // except for kinds that say what the picks are (a single fillet face).
  std::vector<Recognized> around(const std::vector<int>& faces, const std::vector<int>& edges, const std::set<std::string>& kinds);

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

// The kinds Recognizer::around knows, in the order lists show them.
const std::vector<std::string>& recognizer_kinds();
// Rule selectors {"select":{"recognized":"hole","diameter":6}}: splits a filter object into the recognition part
// (recognized and the group parameters: diameter, radius, distance, thickness, depth, type, through, cb_diameter,
// cs_diameter) and the entity filters left for entity_matches. first is empty without "recognized".
std::pair<json, json> split_recognized(const json& filters);
// The faces (ordinals) of every group of that kind whose parameters match within `tolerance` (mm; booleans and
// strings exactly).
std::vector<int> recognized_faces(const TopoDS_Shape& body, const json& recognition, double tolerance, const std::function<bool()>& cancelled = {});

}  // namespace opad
