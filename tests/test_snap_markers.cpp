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
                         Glyph::Parallel, Glyph::Equal, Glyph::Concentric, Glyph::Collinear, Glyph::Symmetric, Glyph::Fix, Glyph::Smooth, Glyph::Curvature};
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

// UI-24: a sketch's constraint badges never cover each other: the first sits above right of its place, more on one place
// run on in a row, crowded places push theirs aside; the order of the places decides who gets the first choice.
TEST(constraint_badges_never_overlap) {
  std::vector<Place> anchors(5, Place{100, 100});  // five constraints on one line
  for (int i = 0; i < 40; ++i) anchors.push_back({double(i % 7) * 9, double(i / 7) * 7});  // a crowded corner
  const auto at = layoutBadges(anchors, 16, 2);
  CHECK(at.size() == anchors.size());
  CHECK_NEAR(at[0].x, 116, 1e-12);
  CHECK_NEAR(at[0].y, 114.4, 1e-12);
  for (size_t i = 0; i < at.size(); ++i)
    for (size_t j = i + 1; j < at.size(); ++j) CHECK(std::abs(at[i].x - at[j].x) >= 18 - 1e-9 || std::abs(at[i].y - at[j].y) >= 18 - 1e-9);
  for (size_t i = 0; i < 5; ++i) CHECK(std::hypot(at[i].x - 100, at[i].y - 100) < 120);  // still beside their line
  CHECK(layoutBadges({}).empty());
}

TEST(a_turned_marker_follows_its_line) {
  // The extension's stem points +x; turned a quarter it points up, a half left; a turn keeps the size and the shape.
  const auto e = marker(Marker::Extension, 10);
  auto stem = [](const std::vector<Seg>& segs) { return segs.back(); };  // the stem: from the bar out along the line
  const auto up = turned(e, std::acos(-1.0) / 2), left = turned(e, std::acos(-1.0));
  CHECK(up.size() == e.size() && fits(up, 10 * std::sqrt(2.0)) && ends(up) == ends(e));
  CHECK(stem(up).y1 > stem(up).y0 + 1 && std::abs(stem(up).x1 - stem(up).x0) < 1e-9);
  CHECK(stem(left).x1 < stem(left).x0 - 1 && std::abs(stem(left).y1 - stem(left).y0) < 1e-9);
  for (size_t i = 0; i < e.size(); ++i) {
    CHECK_NEAR(std::hypot(up[i].x1 - up[i].x0, up[i].y1 - up[i].y0), std::hypot(e[i].x1 - e[i].x0, e[i].y1 - e[i].y0), 1e-12);
    CHECK_NEAR(up[i].x0, -e[i].y0, 1e-12);
    CHECK_NEAR(up[i].y0, e[i].x0, 1e-12);
  }
  CHECK(shape(turned(e, 0)) == shape(e) && shape(turned(e, 2 * std::acos(-1.0))) == shape(e));
}

TEST(a_badge_is_a_closed_frame_of_its_size) {
  const auto b = badge(14, 14, 3);
  CHECK(b.size() == 8 && ends(b) == 0 && fits(b, 14));
}

CHECK_MAIN()
