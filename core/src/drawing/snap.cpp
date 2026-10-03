// Snaps on a 2D drawing (TODO 11 UI-78): points of interest and the curves' segments in two uniform grids.
#include "opad/drawing/snap.hpp"

#include <algorithm>
#include <cmath>

namespace opad::drawing {
namespace {

constexpr double kTau = 6.283185307179586;

double dist(Vec2 a, Vec2 b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

Vec2 closest_on(Vec2 p, Vec2 a, Vec2 b) {
  const double dx = b[0] - a[0], dy = b[1] - a[1], len2 = dx * dx + dy * dy;
  const double t = len2 > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / len2, 0.0, 1.0) : 0;
  return {a[0] + t * dx, a[1] + t * dy};
}

bool cross(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Vec2& out) {
  const double rx = b[0] - a[0], ry = b[1] - a[1], sx = d[0] - c[0], sy = d[1] - c[1];
  const double den = rx * sy - ry * sx;
  if (std::fabs(den) < 1e-15 * (std::hypot(rx, ry) * std::hypot(sx, sy) + 1e-300)) return false;  // parallel
  const double t = ((c[0] - a[0]) * sy - (c[1] - a[1]) * sx) / den, u = ((c[0] - a[0]) * ry - (c[1] - a[1]) * rx) / den;
  if (t < -1e-9 || t > 1 + 1e-9 || u < -1e-9 || u > 1 + 1e-9) return false;
  out = {a[0] + t * rx, a[1] + t * ry};
  return true;
}

// The point halfway along a polyline.
Vec2 halfway(const std::vector<Vec2>& pts) {
  double total = 0;
  for (size_t i = 1; i < pts.size(); ++i) total += dist(pts[i - 1], pts[i]);
  double left = total / 2;
  for (size_t i = 1; i < pts.size(); ++i) {
    const double l = dist(pts[i - 1], pts[i]);
    if (l >= left && l > 0) return {pts[i - 1][0] + (pts[i][0] - pts[i - 1][0]) * left / l, pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * left / l};
    left -= l;
  }
  return pts.empty() ? Vec2{0, 0} : pts.back();
}

// The angles q + k * 2 pi (q a multiple of a quarter turn) between a0 and a1.
std::vector<double> quarters(double a0, double a1) {
  std::vector<double> out;
  for (int q = 0; q < 4; ++q) {
    const double base = q * kTau / 4;
    for (double a = base + std::ceil((a0 - base) / kTau - 1e-12) * kTau; a <= a1 + 1e-12; a += kTau) out.push_back(a);
  }
  return out;
}

}  // namespace

const char* snap_kind_name(SnapKind k) {
  switch (k) {
    case SnapKind::End: return "end";
    case SnapKind::Mid: return "mid";
    case SnapKind::Centre: return "centre";
    case SnapKind::Quadrant: return "quadrant";
    case SnapKind::Intersection: return "intersection";
    case SnapKind::Nearest: return "nearest";
  }
  return "";
}

long SnapIndex::cell(double v) const { return static_cast<long>(std::floor(v / m_cell)); }

void SnapIndex::put(std::unordered_map<long long, std::vector<int>>& grid, Vec2 p, int i) {
  auto& list = grid[key(cell(p[0]), cell(p[1]))];
  if (list.empty() || list.back() != i) list.push_back(i);
}

SnapIndex::SnapIndex(const Display& d, double tol) {
  std::unordered_map<std::string, int> sources;
  int curves = 0;
  for (const auto& prim : d.prims) {
    if (prim.kind != Prim::Kind::Curve) continue;
    const Curve& c = prim.curve;
    auto [it, fresh] = sources.emplace(prim.source, static_cast<int>(m_sources.size()));
    if (fresh) m_sources.push_back(prim.source);
    const int src = it->second, id = curves++;
    const auto point = [&](Vec2 p, SnapKind k) { m_points.push_back({p, k, src, id}); };
    std::vector<Vec2> pts;
    try {
      pts = c.sample(tol);
    } catch (const std::exception&) {
      continue;
    }
    for (size_t i = 1; i < pts.size(); ++i)
      if (dist(pts[i - 1], pts[i]) > 0) m_segments.push_back({pts[i - 1], pts[i], id, src});
    if (c.type == Curve::Type::Arc || c.type == Curve::Type::Ellipse) {
      const bool full = c.a1 - c.a0 >= kTau - 1e-9;
      const double cr = std::cos(c.rot), sr = std::sin(c.rot), r2 = c.type == Curve::Type::Arc ? c.r1 : c.r2;
      const auto at = [&](double t) {  // an arc's angle, an ellipse's parameter
        const double x = c.r1 * std::cos(t), y = r2 * std::sin(t);
        return c.type == Curve::Type::Arc ? Vec2{c.c[0] + x, c.c[1] + y} : Vec2{c.c[0] + x * cr - y * sr, c.c[1] + x * sr + y * cr};
      };
      point(c.c, SnapKind::Centre);
      for (double q : quarters(c.a0, full ? c.a0 + kTau - 1e-9 : c.a1)) point(at(q), SnapKind::Quadrant);
      if (!full) {
        point(at(c.a0), SnapKind::End);
        point(at(c.a1), SnapKind::End);
        point(at((c.a0 + c.a1) / 2), SnapKind::Mid);
      }
    } else if (c.type == Curve::Type::Line && c.pts.size() >= 2) {
      point(c.pts[0], SnapKind::End);
      point(c.pts[1], SnapKind::End);
      point({(c.pts[0][0] + c.pts[1][0]) / 2, (c.pts[0][1] + c.pts[1][1]) / 2}, SnapKind::Mid);
    } else if (pts.size() >= 2) {  // polylines and splines
      const bool closed = dist(pts.front(), pts.back()) < 1e-9;
      if (c.type == Curve::Type::Polyline && c.pts.size() <= 8) {  // a few corners (a frame, a block): each one
        for (size_t i = 0; i + (closed ? 1 : 0) < c.pts.size(); ++i) point(c.pts[i], SnapKind::End);
        for (size_t i = 1; i < c.pts.size(); ++i) point({(c.pts[i - 1][0] + c.pts[i][0]) / 2, (c.pts[i - 1][1] + c.pts[i][1]) / 2}, SnapKind::Mid);
      } else if (!closed) {
        point(pts.front(), SnapKind::End);
        point(pts.back(), SnapKind::End);
        point(halfway(pts), SnapKind::Mid);
      }
    }
  }
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  for (const auto& s : m_segments)
    for (const Vec2& p : {s.a, s.b}) x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
  m_cell = x1 > x0 || y1 > y0 ? std::max({(x1 - x0) / 200, (y1 - y0) / 200, 10 * tol, 1e-6}) : 1;
  for (size_t i = 0; i < m_points.size(); ++i) put(m_pointGrid, m_points[i].at, static_cast<int>(i));
  for (size_t i = 0; i < m_segments.size(); ++i) {  // every cell it passes, by steps of half a cell
    const Segment& s = m_segments[i];
    const int steps = static_cast<int>(std::ceil(dist(s.a, s.b) / (m_cell / 2)));
    for (int k = 0; k <= steps; ++k) {
      const double t = steps ? static_cast<double>(k) / steps : 0;
      put(m_segmentGrid, {s.a[0] + t * (s.b[0] - s.a[0]), s.a[1] + t * (s.b[1] - s.a[1])}, static_cast<int>(i));
    }
  }
}

std::optional<Snap> SnapIndex::find(Vec2 p, double radius, unsigned kinds, int curves) const {
  if (!(radius > 0) || empty()) return std::nullopt;
  const long ix0 = cell(p[0] - radius), ix1 = cell(p[0] + radius), iy0 = cell(p[1] - radius), iy1 = cell(p[1] + radius);
  if ((ix1 - ix0 + 1) * (iy1 - iy0 + 1) > 40000) return std::nullopt;  // seen from so far that nothing is to be told apart
  std::optional<Snap> best;
  const auto offer = [&](Vec2 at, SnapKind k, int src, int curve) {
    const double d = dist(at, p);
    if (d <= radius && (!best || d < best->distance - 1e-12)) best = Snap{k, at, d, m_sources[static_cast<size_t>(src)], curve};
  };
  std::vector<int> near;
  for (long ix = ix0; ix <= ix1; ++ix)
    for (long iy = iy0; iy <= iy1; ++iy) {
      if (auto it = m_pointGrid.find(key(ix, iy)); it != m_pointGrid.end())
        for (int i : it->second) {
          const Point& q = m_points[static_cast<size_t>(i)];
          if ((kinds & snap_bit(q.kind)) && (curves <= 0 || q.curve < curves)) offer(q.at, q.kind, q.source, q.curve);
        }
      if (auto it = m_segmentGrid.find(key(ix, iy)); it != m_segmentGrid.end())
        for (int i : it->second)
          if (curves <= 0 || m_segments[static_cast<size_t>(i)].curve < curves) near.push_back(i);
    }
  std::sort(near.begin(), near.end());
  near.erase(std::unique(near.begin(), near.end()), near.end());
  if ((kinds & snap_bit(SnapKind::Intersection)) && near.size() <= 400)  // where two curves cross
    for (size_t i = 0; i < near.size(); ++i)
      for (size_t j = i + 1; j < near.size(); ++j) {
        const Segment &a = m_segments[static_cast<size_t>(near[i])], &b = m_segments[static_cast<size_t>(near[j])];
        Vec2 at;
        if (a.curve != b.curve && cross(a.a, a.b, b.a, b.b, at)) offer(at, SnapKind::Intersection, a.source, a.curve);
      }
  if (best || !(kinds & snap_bit(SnapKind::Nearest))) return best;
  for (int i : near) {
    const Segment& s = m_segments[static_cast<size_t>(i)];
    offer(closest_on(p, s.a, s.b), SnapKind::Nearest, s.source, s.curve);
  }
  return best;
}

}  // namespace opad::drawing
