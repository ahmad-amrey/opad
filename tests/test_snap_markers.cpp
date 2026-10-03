// Snap markers and constraint pictograms (app/SnapMarkers.hpp, TODO 11 UI-23, UI-21): every one fits its size, none is
// empty or degenerate, and every marker has a shape of its own.
#include "check.hpp"
#include "SnapMarkers.hpp"
#include <algorithm>
#include <set>
using namespace snapmarkers;

namespace {
bool fits(const std::vector<Seg>& segs, double size) {
  for (const auto& s : segs)
    for (const double c : {s.x0, s.y0, s.x1, s.y1})
      if (std::abs(c) > size / 2 + 1e-9) return false;
  return true;
}
bool solid(const std::vector<Seg>& segs) {
  return !segs.empty() && std::all_of(segs.begin(), segs.end(), [](const Seg& s) { return std::hypot(s.x1 - s.x0, s.y1 - s.y0) > 1e-9; });
}
// A shape as text, rounded: equal for the same segments in the same order.
std::string shape(const std::vector<Seg>& segs) {
  std::string out;
  for (const auto& s : segs)
    for (const double c : {s.x0, s.y0, s.x1, s.y1}) out += std::to_string(std::lround(c * 100)) + ",";
  return out;
}
// Endpoints reached once (ends of an open path) count; a closed path has none.
int ends(const std::vector<Seg>& segs) {
  std::vector<std::pair<long, long>> pts;
  for (const auto& s : segs) {
    pts.push_back({std::lround(s.x0 * 1000), std::lround(s.y0 * 1000)});
    pts.push_back({std::lround(s.x1 * 1000), std::lround(s.y1 * 1000)});
  }
  int odd = 0;
  for (const auto& p : pts) odd += std::count(pts.begin(), pts.end(), p) % 2;
  return odd;
}
const Marker kMarkers[] = {Marker::Endpoint, Marker::Midpoint, Marker::Centre, Marker::Quadrant, Marker::Intersection, Marker::Apparent, Marker::Nearest,
                           Marker::Perpendicular, Marker::Tangent, Marker::Extension, Marker::Tracking, Marker::Locked, Marker::Grid};
const Glyph kGlyphs[] = {Glyph::Coincident, Glyph::OnCurve, Glyph::Midpoint, Glyph::Horizontal, Glyph::Vertical, Glyph::Perpendicular, Glyph::Tangent,
                         Glyph::Parallel, Glyph::Equal, Glyph::Concentric, Glyph::Collinear, Glyph::Symmetric, Glyph::Fix};
}  // namespace

TEST(every_marker_fits_its_size_and_has_a_shape_of_its_own) {
  std::set<std::string> shapes;
  for (const Marker m : kMarkers) {
    for (const double size : {10.0, 7.0, 24.0}) {
      const auto segs = marker(m, size);
      CHECK(solid(segs));
      CHECK(fits(segs, size));
    }
    shapes.insert(shape(marker(m)));
  }
  CHECK(shapes.size() == std::size(kMarkers));
}

TEST(the_point_markers_are_their_shapes) {
  // Endpoint: a closed square; midpoint: a closed triangle; quadrant: a closed diamond; centre: a closed ring.
  CHECK(marker(Marker::Endpoint).size() == 4 && ends(marker(Marker::Endpoint)) == 0);
  CHECK(marker(Marker::Midpoint).size() == 3 && ends(marker(Marker::Midpoint)) == 0);
  CHECK(marker(Marker::Quadrant).size() == 4 && ends(marker(Marker::Quadrant)) == 0);
  CHECK(marker(Marker::Centre).size() >= 12 && ends(marker(Marker::Centre)) == 0);
  // Intersection: two crossing strokes through the point; apparent: the same in a square.
  const auto x = marker(Marker::Intersection);
  CHECK(x.size() == 2 && std::abs(x[0].x0 + x[0].x1) < 1e-12 && std::abs(x[0].y0 + x[0].y1) < 1e-12);
  CHECK(marker(Marker::Apparent).size() == 6);
  // The square spans the size: 10 px across.
  double low = 0, high = 0;
  for (const auto& s : marker(Marker::Endpoint, 10)) low = std::min({low, s.x0, s.x1}), high = std::max({high, s.x0, s.x1});
  CHECK_NEAR(high - low, 10, 1e-12);
}

TEST(every_pictogram_fits_and_differs) {
  std::set<std::string> shapes;
  for (const Glyph g : kGlyphs) {
    const auto segs = glyph(g, 12);
    CHECK(solid(segs));
    CHECK(fits(segs, 12));
    shapes.insert(shape(segs));
  }
  CHECK(shapes.size() == std::size(kGlyphs));
  // Horizontal and vertical are each other turned a quarter.
  const auto h = glyph(Glyph::Horizontal), v = glyph(Glyph::Vertical);
  CHECK(h.size() == v.size());
  for (size_t i = 0; i < h.size(); ++i) CHECK(std::abs(h[i].x0 - v[i].y0) < 1e-12 && std::abs(h[i].y0 - v[i].x0) < 1e-12);
}

TEST(a_badge_is_a_closed_frame_of_its_size) {
  const auto b = badge(14, 14, 3);
  CHECK(b.size() == 8 && ends(b) == 0 && fits(b, 14));
}

CHECK_MAIN()
