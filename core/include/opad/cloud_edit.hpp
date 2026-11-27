#pragma once
// Editing a point cloud by hand (the Edit point cloud mode, app/CloudEditArea.cpp; the feature kind cloud_edit): a scan used
// as the reference of a reverse-engineered part is cleaned up the way a sculptor works a model, points pushed in or pulled
// out under a brush, smoothed onto their local surface, deleted, and a few added where the scan left a hole.
//
// What a session changed is an Edit against the cloud as it was: the points moved (ordinals and offsets, nothing of the
// points left alone), those deleted, and the new ones (ordinal base + k, with their final places and attributes). That is
// what a cloud_edit feature stores in its inputs (to_json: floats as base64 little-endian, exact), and `apply` makes the
// edited cloud from the base and it, so replaying it gives the same points bit for bit. The point cloud's own format
// (opad/point_cloud.hpp) is untouched: the result is an ordinary cloud.
//
// The Editor is the session's working state over a base cloud (worker side: one caller at a time). Brushes are cylinders
// along the view through the surface point under the pointer: a point is under the brush when its distance from that axis
// is below the radius and it lies within the radius of the surface point along the view, with a smooth falloff
// (1 - t^2)^2 of t = distance / radius.
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "point_cloud.hpp"

namespace opad::cloud {

struct Edit {
  size_t base = 0;              // the points of the cloud it was made on
  std::vector<uint32_t> moved;  // base ordinals, sorted
  std::vector<float> offsets;   // 3 per moved point (mm, the cloud's own frame)
  Region deleted;               // base and added ordinals
  Points added;                 // ordinal base + k; deleted ones stay listed (and in `deleted`) so ordinals keep
  size_t strokes = 0;           // how many changes made it (a report)
  bool empty() const { return moved.empty() && deleted.empty() && added.empty(); }
  json to_json() const;         // {base, moved: [[first, last], ...], offsets: base64, deleted, added: {xyz, rgb?, normals?, scalar?}, strokes}
  static Edit from_json(const json& j);  // throws on anything malformed
  json summary() const;         // {moved, deleted, added, strokes}: counts
};

// The edited cloud: the base with the offsets applied, the deleted points left out and the added ones (not deleted) after
// it, attributes as the base has them. Throws when the base has another number of points than the edit was made on.
Points apply(const Points& base, const Edit& edit);

// A brush in the cloud's own frame.
struct Brush {
  Vec3 centre{0, 0, 0};    // the surface point under the pointer
  Vec3 toward{0, 0, 1};    // unit, from the surface toward the eye (the view's direction reversed)
  double radius = 1;       // mm at the centre's depth
};
double falloff(double t);  // (1 - t^2)^2 for t in [0, 1), else 0

class Editor {
 public:
  // Builds the neighbour grid over the base (a worker's work: about a second for 20 million points).
  explicit Editor(std::shared_ptr<const Points> base, const Cancel& cancel = {});
  ~Editor();
  const Points& base() const { return *m_base; }
  std::shared_ptr<const Points> basePtr() const { return m_base; }
  double spacing() const { return m_spacing; }
  void reset(const Edit& edit = {});  // the working state becomes this edit (made on this base: throws otherwise)
  Edit edit() const;                  // the working state, compact
  size_t size() const { return m_base->size() + m_added.size(); }  // ordinals in use, deleted ones included
  bool deleted(uint32_t i) const { return i < m_deleted.size() && m_deleted[i]; }
  Vec3 at(uint32_t i) const;          // where point i is now
  const Points& added() const { return m_added; }  // ordinal base + k, deleted ones too
  size_t changes() const { return m_strokes; }
  void countStroke() { ++m_strokes; }

  // The points under a brush and their weights; `ids` sorted.
  void under(const Brush& b, std::vector<uint32_t>& ids, std::vector<float>& weights) const;
  // Pull (amount > 0, toward the eye side of the surface) or push (< 0): each point under the brush moves amount x weight
  // along its normal turned to face the eye, or along `toward` when the cloud has no normals. Returns the points moved.
  std::vector<uint32_t> push(const Brush& b, double amount);
  // Each point under the brush moves strength x weight of the way onto the plane through its k nearest neighbours.
  std::vector<uint32_t> smooth(const Brush& b, double strength, int k = 10);
  std::vector<uint32_t> erase(const Brush& b);  // deletes the points under the brush
  size_t erase(const Region& r);                // deletes these (base and added ordinals); returns how many were not yet
  size_t keep(const Region& r);                 // deletes every other point
  // A new point on the local surface at `seed` (a picked point): the plane fitted to its k nearest points, met by the line
  // through `seed` along `toward` (the pointer's ray), with the plane's normal (turned as the neighbours' normals, else to
  // the eye) and the neighbours' colour and scalar weighted by nearness. nullopt: too few points near it.
  std::optional<uint32_t> add(const Vec3& seed, const Vec3& toward, int k = 12);
  // The points inside a screen polygon (view: cloud frame -> NDC, row-major, as in_polygon), where they are now, deleted
  // ones left out.
  Region inPolygon(const std::array<double, 16>& view, const std::vector<std::array<double, 2>>& polygon, const Cancel& cancel = {}) const;
  // The k nearest live points of a place (working positions), nearest first.
  void nearest(const Vec3& q, size_t k, std::vector<uint32_t>& out) const;

 private:
  void candidates(const Vec3& c, double radius, std::vector<uint32_t>& out) const;  // live points within radius (now)
  void offsetBy(uint32_t i, float dx, float dy, float dz);
  std::shared_ptr<const Points> m_base;
  std::unique_ptr<Grid> m_grid;
  double m_spacing = 0;
  std::unordered_map<uint32_t, std::array<float, 3>> m_offset;  // base points moved
  std::vector<uint8_t> m_deleted;                               // base + added
  Points m_added;
  float m_reach = 0;  // the largest offset so far: grid queries (on the old places) reach this much further
  size_t m_strokes = 0;
};

// Floats as base64 (little-endian IEEE), bytes as base64: compact and exact in JSON.
std::string floats_to_base64(const float* v, size_t n);
std::vector<float> floats_from_base64(const std::string& s);
std::string bytes_to_base64(const uint8_t* v, size_t n);
std::vector<uint8_t> bytes_from_base64(const std::string& s);

}  // namespace opad::cloud
