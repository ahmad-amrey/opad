#include "opad/snap2d.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>

#include <algorithm>
#include <cmath>

namespace opad::snap2d {
const char* kind_name(Kind kind) {
  switch (kind) {
    case Kind::Endpoint: return "endpoint";
    case Kind::Midpoint: return "midpoint";
    case Kind::Center: return "center";
    case Kind::Quadrant: return "quadrant";
    case Kind::Intersection: return "intersection";
    case Kind::Nearest: return "nearest";
    default: return "";
  }
}

bool Kinds::on(Kind kind) const {
  switch (kind) {
    case Kind::Endpoint: return endpoint;
    case Kind::Midpoint: return midpoint;
    case Kind::Center: return center;
    case Kind::Quadrant: return quadrant;
    case Kind::Intersection: return intersection;
    case Kind::Nearest: return nearest;
    default: return false;
  }
}

namespace {
double dist(P a, P b) { return std::hypot(a.x - b.x, a.y - b.y); }
P along(P a, P b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
P footOn(P a, P b, P q) {
  const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
  return along(a, b, len2 < 1e-30 ? 0 : std::clamp(((q.x - a.x) * dx + (q.y - a.y) * dy) / len2, 0.0, 1.0));
}
// Within the arc from `from` sweeping `sweep` (signed), a whole circle when |sweep| >= 2 pi.
bool onArc(double angle, double from, double sweep) {
  if (std::abs(sweep) >= 2 * M_PI - 1e-12) return true;
  double t = std::fmod((sweep >= 0 ? angle - from : from - angle) + 8 * M_PI, 2 * M_PI);
  return t <= std::abs(sweep) + 1e-12;
}
}  // namespace

// ---------------------------------------------------------------- building
int Index::add_line(P a, P b) {
  const int id = int(m_round.size());
  m_round.push_back({});
  m_points.push_back({a, Kind::Endpoint, id});
  m_points.push_back({b, Kind::Endpoint, id});
  m_points.push_back({along(a, b, 0.5), Kind::Midpoint, id});
  m_pieces.push_back({a, b, id});
  m_round.back().line = true;
  return id;
}

int Index::add_arc(P center, double radius, double from, double sweep, double tolerance) {
  const int id = int(m_round.size());
  const bool whole = std::abs(sweep) >= 2 * M_PI - 1e-9;
  if (whole) sweep = 2 * M_PI;
  m_round.push_back({true, false, center, radius, from, sweep});
  auto at = [&](double angle) { return P{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)}; };
  m_points.push_back({center, Kind::Center, id});
  if (!whole) {
    m_points.push_back({at(from), Kind::Endpoint, id});
    m_points.push_back({at(from + sweep), Kind::Endpoint, id});
    m_points.push_back({at(from + sweep / 2), Kind::Midpoint, id});
  }
  for (int q = 0; q < 4; ++q)
    if (onArc(q * M_PI / 2, from, sweep)) m_points.push_back({at(q * M_PI / 2), Kind::Quadrant, id});
  const double step = radius > tolerance ? 2 * std::acos(std::max(-1.0, 1 - tolerance / radius)) : M_PI / 4;
  const int n = std::clamp(int(std::ceil(std::abs(sweep) / std::max(step, 1e-6))), 8, 720);
  for (int i = 0; i < n; ++i) m_pieces.push_back({at(from + sweep * i / n), at(from + sweep * (i + 1) / n), id});
  return id;
}

int Index::add_polyline(const std::vector<P>& points, bool closed) {
  const int id = int(m_round.size());
  m_round.push_back({});
  if (points.size() < 2) return id;
  if (!closed) {
    m_points.push_back({points.front(), Kind::Endpoint, id});
    m_points.push_back({points.back(), Kind::Endpoint, id});
  }
  for (size_t i = 0; i + 1 < points.size(); ++i) m_pieces.push_back({points[i], points[i + 1], id});
  if (closed && dist(points.front(), points.back()) > 0) m_pieces.push_back({points.back(), points.front(), id});
  return id;
}

void Index::add_point(P at) { m_points.push_back({at, Kind::Endpoint, -1}); }
void Index::add_center(int curve, P at) { m_points.push_back({at, Kind::Center, curve}); }

void Index::finish() {
  if (m_finished) return;
  m_finished = true;
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  auto grow = [&](P p) { x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y); };
  for (const auto& p : m_points) grow(p.at);
  for (const auto& s : m_pieces) grow(s.a), grow(s.b);
  if (x0 > x1) return;
  const double extent = std::max({x1 - x0, y1 - y0, 1e-9});
  const int cells = std::clamp(int(std::sqrt((m_points.size() + m_pieces.size()) / 2.0)), 1, 512);
  m_cell = extent / cells;
  m_x0 = x0, m_y0 = y0;
  m_nx = int((x1 - x0) / m_cell) + 1, m_ny = int((y1 - y0) / m_cell) + 1;
  m_cellPoints.assign(size_t(m_nx) * m_ny, {});
  m_cellPieces.assign(size_t(m_nx) * m_ny, {});
  auto cellX = [&](double x) { return std::clamp(int((x - m_x0) / m_cell), 0, m_nx - 1); };
  auto cellY = [&](double y) { return std::clamp(int((y - m_y0) / m_cell), 0, m_ny - 1); };
  for (size_t i = 0; i < m_points.size(); ++i) m_cellPoints[size_t(cellY(m_points[i].at.y)) * m_nx + cellX(m_points[i].at.x)].push_back(int(i));
  // Long curves are cut into pieces no longer than a cell, each kept in the (at most four) cells its box touches.
  std::vector<Piece> raw;
  raw.swap(m_pieces);
  for (const auto& s : raw) {
    const int k = std::max(1, int(std::ceil(dist(s.a, s.b) / m_cell)));
    for (int i = 0; i < k; ++i) {
      const Piece piece{along(s.a, s.b, double(i) / k), along(s.a, s.b, double(i + 1) / k), s.curve};
      const int index = int(m_pieces.size());
      m_pieces.push_back(piece);
      for (int cy = cellY(std::min(piece.a.y, piece.b.y)); cy <= cellY(std::max(piece.a.y, piece.b.y)); ++cy)
        for (int cx = cellX(std::min(piece.a.x, piece.b.x)); cx <= cellX(std::max(piece.a.x, piece.b.x)); ++cx) m_cellPieces[size_t(cy) * m_nx + cx].push_back(index);
    }
  }
}

void Index::around(P at, double r, std::vector<Point>& points, std::vector<Piece>& pieces) const {
  if (!m_nx || at.x + r < m_x0 || at.y + r < m_y0 || at.x - r > m_x0 + m_nx * m_cell || at.y - r > m_y0 + m_ny * m_cell) return;
  const int cx0 = std::clamp(int((at.x - r - m_x0) / m_cell), 0, m_nx - 1), cx1 = std::clamp(int((at.x + r - m_x0) / m_cell), 0, m_nx - 1);
  const int cy0 = std::clamp(int((at.y - r - m_y0) / m_cell), 0, m_ny - 1), cy1 = std::clamp(int((at.y + r - m_y0) / m_cell), 0, m_ny - 1);
  std::vector<int> found;
  for (int cy = cy0; cy <= cy1; ++cy)
    for (int cx = cx0; cx <= cx1; ++cx)
      for (int i : m_cellPoints[size_t(cy) * m_nx + cx])
        if (dist(m_points[i].at, at) <= r) points.push_back(m_points[i]);
  for (int cy = cy0; cy <= cy1; ++cy)
    for (int cx = cx0; cx <= cx1; ++cx) found.insert(found.end(), m_cellPieces[size_t(cy) * m_nx + cx].begin(), m_cellPieces[size_t(cy) * m_nx + cx].end());
  std::sort(found.begin(), found.end());
  found.erase(std::unique(found.begin(), found.end()), found.end());
  for (int i : found)
    if (dist(footOn(m_pieces[i].a, m_pieces[i].b, at), at) <= r) pieces.push_back(m_pieces[i]);
}

bool Index::onRound(int curve, P q, P& out) const {
  if (curve < 0 || curve >= int(m_round.size()) || !m_round[curve].round) return false;
  const Round& c = m_round[curve];
  const double d = dist(q, c.center);
  if (d < 1e-12 || !onArc(std::atan2(q.y - c.center.y, q.x - c.center.x), c.from, c.sweep)) return false;
  out = {c.center.x + (q.x - c.center.x) * c.radius / d, c.center.y + (q.y - c.center.y) * c.radius / d};
  return true;
}

const Index::Round* Index::round(int curve) const { return curve >= 0 && curve < int(m_round.size()) ? &m_round[curve] : nullptr; }

// ---------------------------------------------------------------- from a shape
std::shared_ptr<Index> index_shape(const TopoDS_Shape& shape) {
  auto index = std::make_shared<Index>();
  if (shape.IsNull()) return index;
  Bnd_Box box;
  BRepBndLib::Add(shape, box);
  const double size = box.IsVoid() ? 1 : std::sqrt(box.SquareExtent());
  double zSum = 0;
  int zCount = 0;
  auto flat = [&](const gp_Pnt& p) {
    zSum += p.Z(), ++zCount;
    return P{p.X(), p.Y()};
  };
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge& e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e)) continue;
    try {
      const BRepAdaptor_Curve c(e);
      const double t0 = c.FirstParameter(), t1 = c.LastParameter();
      if (c.GetType() == GeomAbs_Line) {
        index->add_line(flat(c.Value(t0)), flat(c.Value(t1)));
        continue;
      }
      if (c.GetType() == GeomAbs_Circle && std::abs(c.Circle().Axis().Direction().Z()) > 1 - 1e-9) {
        const gp_Circ circle = c.Circle();
        const gp_Dir x = circle.XAxis().Direction();
        const double sign = circle.Axis().Direction().Z() > 0 ? 1 : -1, base = std::atan2(x.Y(), x.X());
        flat(c.Value(t0));
        index->add_arc(flat(circle.Location()), circle.Radius(), base + sign * t0, sign * (t1 - t0), std::max(circle.Radius() * 1e-3, size * 1e-7));
        continue;
      }
      GCPnts_TangentialDeflection sampler(c, t0, t1, 0.1, std::max(size * 1e-4, 1e-7), 2);
      std::vector<P> pts;
      for (int k = 1; k <= sampler.NbPoints(); ++k) pts.push_back(flat(sampler.Value(k)));
      const bool closed = c.Value(t0).Distance(c.Value(t1)) <= std::max(size * 1e-9, 1e-9);
      if (closed && pts.size() > 2) pts.pop_back();
      const int id = index->add_polyline(pts, closed);
      if (c.GetType() == GeomAbs_Ellipse) index->add_center(id, flat(c.Ellipse().Location()));
    } catch (const Standard_Failure&) {
      continue;  // a curve that cannot be evaluated cannot be snapped to
    }
  }
  for (TopExp_Explorer v(shape, TopAbs_VERTEX, TopAbs_EDGE); v.More(); v.Next()) index->add_point(flat(BRep_Tool::Pnt(TopoDS::Vertex(v.Current()))));
  index->z = zCount ? zSum / zCount : 0;
  index->finish();
  return index;
}

// ---------------------------------------------------------------- asking
namespace {
struct Found {
  P a, b;  // common coordinates
  int source, curve;
};
// Exact where both curves say what they are (lines and circles), else the pieces' crossing as it was found.
P refine(const std::vector<Placed>& sources, const Found& u, const Found& v, P approx) {
  auto shape = [&](const Found& f, bool& line, bool& circle, P& p0, P& p1, P& centre, double& r) {
    const Index::Round* round = sources[f.source].index->round(f.curve);
    const Placed& s = sources[f.source];
    line = round && round->line;
    circle = round && round->round;
    p0 = f.a, p1 = f.b;
    if (circle) {
      centre = {s.a * round->center.x + s.b * round->center.y + s.c, s.d * round->center.x + s.e * round->center.y + s.f};
      r = round->radius * std::sqrt(std::abs(s.a * s.e - s.b * s.d));
    }
  };
  bool la, ca, lb, cb;
  P a0, a1, ac, b0, b1, bc;
  double ar = 0, br = 0;
  shape(u, la, ca, a0, a1, ac, ar);
  shape(v, lb, cb, b0, b1, bc, br);
  auto lineCircle = [&](P p0, P p1, P c, double r, P& out) {
    const double dx = p1.x - p0.x, dy = p1.y - p0.y, fx = p0.x - c.x, fy = p0.y - c.y;
    const double A = dx * dx + dy * dy, B = 2 * (fx * dx + fy * dy), C = fx * fx + fy * fy - r * r, disc = B * B - 4 * A * C;
    if (A < 1e-30 || disc < 0) return false;
    double best = 1e300;
    for (const double sign : {-1.0, 1.0}) {
      const P q = along(p0, p1, (-B + sign * std::sqrt(disc)) / (2 * A));
      if (dist(q, approx) < best) best = dist(q, approx), out = q;
    }
    return true;
  };
  P out = approx;
  if (la && cb) lineCircle(a0, a1, bc, br, out);
  else if (ca && lb) lineCircle(b0, b1, ac, ar, out);
  else if (ca && cb) {
    const double d = dist(ac, bc);
    if (d > 1e-12 && d <= ar + br && d >= std::abs(ar - br)) {
      const double along0 = (ar * ar - br * br + d * d) / (2 * d), h = std::sqrt(std::max(0.0, ar * ar - along0 * along0));
      const P m = along(ac, bc, along0 / d);
      const double ux = (bc.x - ac.x) / d, uy = (bc.y - ac.y) / d;
      const P p{m.x - uy * h, m.y + ux * h}, q{m.x + uy * h, m.y - ux * h};
      out = dist(p, approx) < dist(q, approx) ? p : q;
    }
  }
  return out;
}
}  // namespace

Snap snap(const std::vector<Placed>& sources, P at, double aperture, const Kinds& kinds) {
  Snap best;
  int bestClass = 3;
  double bestDistance = 1e300;
  auto consider = [&](int cls, P q, Kind kind, int source, int curve, int otherSource = -1, int otherCurve = -1) {
    const double d = dist(q, at);
    if (d > aperture || cls > bestClass || (cls == bestClass && d >= bestDistance)) return;
    bestClass = cls, bestDistance = d;
    best = {kind, q, source, curve, otherSource, otherCurve};
  };
  std::vector<Found> found;
  for (size_t s = 0; s < sources.size(); ++s) {
    const Placed& p = sources[s];
    const double det = p.a * p.e - p.b * p.d;
    if (!p.index || std::abs(det) < 1e-30) continue;
    const double scale = std::sqrt(std::abs(det));
    const P local{(p.e * (at.x - p.c) - p.b * (at.y - p.f)) / det, (-p.d * (at.x - p.c) + p.a * (at.y - p.f)) / det};
    auto out = [&](P q) { return P{p.a * q.x + p.b * q.y + p.c, p.d * q.x + p.e * q.y + p.f}; };
    std::vector<Index::Point> points;
    std::vector<Index::Piece> pieces;
    p.index->around(local, aperture / scale, points, pieces);
    for (const auto& q : points)
      if (kinds.on(q.kind)) consider(q.kind == Kind::Endpoint || q.kind == Kind::Center ? 0 : 1, out(q.at), q.kind, int(s), q.curve);
    for (const auto& piece : pieces) found.push_back({out(piece.a), out(piece.b), int(s), piece.curve});
  }
  if (kinds.intersection && bestClass > 0) {
    const size_t n = std::min<size_t>(found.size(), 600);
    for (size_t i = 0; i < n; ++i)
      for (size_t j = i + 1; j < n; ++j) {
        const Found &u = found[i], &v = found[j];
        if (u.source == v.source && u.curve == v.curve) continue;
        const double dx = u.b.x - u.a.x, dy = u.b.y - u.a.y, ex = v.b.x - v.a.x, ey = v.b.y - v.a.y, den = dx * ey - dy * ex;
        if (std::abs(den) < 1e-30) continue;
        const double s = ((v.a.x - u.a.x) * ey - (v.a.y - u.a.y) * ex) / den, t = ((v.a.x - u.a.x) * dy - (v.a.y - u.a.y) * dx) / den;
        if (s < -1e-9 || s > 1 + 1e-9 || t < -1e-9 || t > 1 + 1e-9) continue;
        consider(1, refine(sources, u, v, along(u.a, u.b, s)), Kind::Intersection, u.source, u.curve, v.source, v.curve);
      }
  }
  if (kinds.nearest && bestClass > 1)
    for (const auto& f : found) {
      P q = footOn(f.a, f.b, at);
      // A round curve: the exact circle, not its chord.
      const Placed& p = sources[f.source];
      const double det = p.a * p.e - p.b * p.d;
      const P local{(p.e * (at.x - p.c) - p.b * (at.y - p.f)) / det, (-p.d * (at.x - p.c) + p.a * (at.y - p.f)) / det};
      P exact;
      if (p.index->onRound(f.curve, local, exact)) q = {p.a * exact.x + p.b * exact.y + p.c, p.d * exact.x + p.e * exact.y + p.f};
      consider(2, q, Kind::Nearest, f.source, f.curve);
    }
  return best;
}
}  // namespace opad::snap2d
