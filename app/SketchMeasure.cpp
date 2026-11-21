// The sketch's Measure tool (D in a sketch: Distance's key outside it; Dimension is Shift+D) and the tooltip that names a
// constraint's badge or a dimension's value under the pointer.
//
// Measure takes its picks as Dimension does (m_picked: a point entity stands for its point), so Esc and Backspace step back
// the same way, and adds nothing to the sketch. One point: where it is; one curve: its sizes (a line's length, run and angle,
// a circle's diameter, an arc's radius, length and sweep, any other curve's length); two picks: the distance between them
// (two points with their run along X and Y, a point square to a line, parallel lines across, else the nearest points of the
// two, with the distance between centres where a circle or an arc is picked) and the angle of two lines that are not parallel.
// A third pick starts again from it. The values are written beside a dashed line in the view, in the status line and in the
// panel's steps.
#include <QCursor>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <limits>

#include "CommandHelp.hpp"
#include "I18n.hpp"
#include "SketchEditor.hpp"
#include "Units.hpp"

using namespace opad::design;
using ET = SkEntity::Type;
using CT = SkConstraint::Type;

namespace {
using Poly = std::vector<std::pair<double, double>>;
QString len(double mm) { return units::format(units::Kind::Length, mm); }
QString ang(double rad) { return units::format(units::Kind::Angle, rad * 180 / M_PI); }

// The point of segment a-b nearest (x, y).
std::pair<double, double> onSegment(double x, double y, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
  const double t = l2 > 0 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / l2, 0.0, 1.0) : 0.0;
  return {ax + t * dx, ay + t * dy};
}

// The nearest points of two polylines (one point: a polyline of one): every vertex against the other's segments, and where
// segments cross (distance zero there).
struct Near {
  double d = std::numeric_limits<double>::infinity();
  double ax = 0, ay = 0, bx = 0, by = 0;
};
Near nearest(const Poly& a, const Poly& b) {
  Near n;
  auto consider = [&n](double ax, double ay, double bx, double by) {
    const double d = std::hypot(bx - ax, by - ay);
    if (d < n.d) n = {d, ax, ay, bx, by};
  };
  auto vertices = [&](const Poly& p, const Poly& q, bool swapped) {
    for (const auto& [x, y] : p) {
      if (q.size() == 1) {
        swapped ? consider(q[0].first, q[0].second, x, y) : consider(x, y, q[0].first, q[0].second);
        continue;
      }
      for (size_t i = 1; i < q.size(); ++i) {
        const auto [qx, qy] = onSegment(x, y, q[i - 1].first, q[i - 1].second, q[i].first, q[i].second);
        swapped ? consider(qx, qy, x, y) : consider(x, y, qx, qy);
      }
    }
  };
  vertices(a, b, false);
  vertices(b, a, true);
  for (size_t i = 1; i < a.size(); ++i)
    for (size_t j = 1; j < b.size(); ++j) {
      const double px = a[i - 1].first, py = a[i - 1].second, rx = a[i].first - px, ry = a[i].second - py;
      const double qx = b[j - 1].first, qy = b[j - 1].second, sx = b[j].first - qx, sy = b[j].second - qy;
      const double cross = rx * sy - ry * sx;
      if (std::fabs(cross) < 1e-15) continue;
      const double t = ((qx - px) * sy - (qy - py) * sx) / cross, u = ((qx - px) * ry - (qy - py) * rx) / cross;
      if (t >= 0 && t <= 1 && u >= 0 && u <= 1) consider(px + t * rx, py + t * ry, px + t * rx, py + t * ry);
    }
  return n;
}
}  // namespace

QString SketchEditor::entityName(int id) const {
  if (m_sk.point(id)) return tr("Point %1").arg(id);
  const SkEntity* e = m_sk.entity(id);
  if (!e) return QString::number(id);
  switch (e->type) {
    case ET::Line: return tr("Line %1").arg(id);
    case ET::Circle: return tr("Circle %1").arg(id);
    case ET::Arc: return tr("Arc %1").arg(id);
    case ET::Ellipse: return tr("Ellipse %1").arg(id);
    case ET::Spline: return tr("Spline %1").arg(id);
    case ET::Point: break;
  }
  return tr("Point %1").arg(id);
}

std::optional<SketchEditor::Measure> SketchEditor::measureResult() const {
  if (m_tool != "measure" || m_picked.empty()) return std::nullopt;
  struct Pick {
    const SkPoint* p = nullptr;
    const SkEntity* e = nullptr;
    Poly poly;
  };
  auto P = [this](int id) { return m_sk.point(id); };
  auto pick = [&](int id) {
    Pick k;
    if ((k.p = P(id))) k.poly = {{k.p->x, k.p->y}};
    else if ((k.e = m_sk.entity(id))) k.poly = sampled(*k.e);
    return k;
  };
  auto isLine = [&](const Pick& k) { return k.e && k.e->type == ET::Line && k.e->p.size() == 2 && P(k.e->p[0]) && P(k.e->p[1]); };
  auto centre = [&](const Pick& k) -> const SkPoint* {
    return k.e && (k.e->type == ET::Circle || k.e->type == ET::Arc || k.e->type == ET::Ellipse) && !k.e->p.empty() ? P(k.e->p[0]) : nullptr;
  };
  Measure m;
  const double px = m_viewport->pixelSize(), off = 16 * px;
  // The values beside the middle of a -> b, on its upper side (or past a when it has no length).
  auto beside = [&](double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay, l = std::hypot(dx, dy);
    double nx = 0.7071, ny = 0.7071;
    if (l > 1e-12) {
      nx = -dy / l, ny = dx / l;
      if (ny < 0 || (std::fabs(ny) < 1e-12 && nx < 0)) nx = -nx, ny = -ny;
    }
    m.lu = (ax + bx) / 2 + nx * off;
    m.lv = (ay + by) / 2 + ny * off;
  };
  auto between = [&](double ax, double ay, double bx, double by) {
    m.segment = true;
    m.au = ax, m.av = ay, m.bu = bx, m.bv = by;
    beside(ax, ay, bx, by);
  };
  const Pick a = pick(m_picked[0]);
  if (a.poly.empty()) return std::nullopt;
  if (m_picked.size() == 1) {
    if (a.p) {
      m.lines << tr("Position %1, %2").arg(len(a.p->x), len(a.p->y));
      m.lu = a.p->x + off, m.lv = a.p->y + off;
      return m;
    }
    const SkEntity& e = *a.e;
    if (isLine(a)) {
      const SkPoint *s = P(e.p[0]), *t = P(e.p[1]);
      const double dx = t->x - s->x, dy = t->y - s->y;
      double angle = std::atan2(dy, dx);
      if (angle < 0) angle += M_PI;
      if (angle >= M_PI - 1e-12) angle -= M_PI;
      m.lines << tr("Length %1").arg(len(std::hypot(dx, dy))) << tr("ΔX %1 · ΔY %2").arg(len(std::fabs(dx)), len(std::fabs(dy)))
              << tr("Angle from X %1").arg(ang(angle));
      beside(s->x, s->y, t->x, t->y);
    } else if (e.type == ET::Circle && centre(a)) {
      m.lines << tr("Diameter %1").arg(len(2 * e.r)) << tr("Radius %1").arg(len(e.r)) << tr("Circumference %1").arg(len(2 * M_PI * e.r));
      m.lu = centre(a)->x + (e.r + off) * 0.7071, m.lv = centre(a)->y + (e.r + off) * 0.7071;
    } else if (e.type == ET::Arc && e.p.size() == 3 && centre(a) && P(e.p[1]) && P(e.p[2])) {
      const SkPoint *c = centre(a), *s = P(e.p[1]), *t = P(e.p[2]);
      const double r = std::hypot(s->x - c->x, s->y - c->y);
      double sweep = std::atan2(t->y - c->y, t->x - c->x) - std::atan2(s->y - c->y, s->x - c->x);
      while (sweep <= 0) sweep += 2 * M_PI;
      m.lines << tr("Radius %1").arg(len(r)) << tr("Arc length %1").arg(len(r * sweep)) << tr("Sweep %1").arg(ang(sweep));
      const auto& mid = a.poly[a.poly.size() / 2];
      const double l = std::max(1e-12, std::hypot(mid.first - c->x, mid.second - c->y));
      m.lu = mid.first + (mid.first - c->x) / l * off, m.lv = mid.second + (mid.second - c->y) / l * off;
    } else {  // an ellipse, a spline: its length along the drawn curve
      double total = 0;
      for (size_t i = 1; i < a.poly.size(); ++i) total += std::hypot(a.poly[i].first - a.poly[i - 1].first, a.poly[i].second - a.poly[i - 1].second);
      m.lines << tr("Length %1").arg(len(total));
      const auto& mid = a.poly[a.poly.size() / 2];
      m.lu = mid.first + off, m.lv = mid.second + off;
    }
    return m;
  }
  const Pick b = pick(m_picked[1]);
  if (b.poly.empty()) return std::nullopt;
  const Near n = nearest(a.poly, b.poly);
  if (a.p && b.p) {
    const double dx = b.p->x - a.p->x, dy = b.p->y - a.p->y;
    m.lines << tr("Distance %1").arg(len(std::hypot(dx, dy))) << tr("ΔX %1 · ΔY %2").arg(len(std::fabs(dx)), len(std::fabs(dy)));
    between(a.p->x, a.p->y, b.p->x, b.p->y);
  } else if ((a.p && isLine(b)) || (b.p && isLine(a))) {
    const SkPoint* q = a.p ? a.p : b.p;
    const SkEntity* line = a.p ? b.e : a.e;
    const SkPoint *s = P(line->p[0]), *t = P(line->p[1]);
    const double dx = t->x - s->x, dy = t->y - s->y, l2 = std::max(1e-24, dx * dx + dy * dy);
    const double k = ((q->x - s->x) * dx + (q->y - s->y) * dy) / l2, fx = s->x + k * dx, fy = s->y + k * dy;
    const double square = std::hypot(q->x - fx, q->y - fy);
    m.lines << tr("Distance %1 (square to the line)").arg(len(square));
    if (n.d > square + 1e-9) m.lines << tr("Nearest %1").arg(len(n.d));  // the foot is past the line's end
    between(q->x, q->y, fx, fy);
  } else if (isLine(a) && isLine(b)) {
    const SkPoint *s = P(a.e->p[0]), *t = P(a.e->p[1]), *s2 = P(b.e->p[0]), *t2 = P(b.e->p[1]);
    const double ux = t->x - s->x, uy = t->y - s->y, vx = t2->x - s2->x, vy = t2->y - s2->y;
    const double lu = std::max(1e-12, std::hypot(ux, uy)), lv = std::max(1e-12, std::hypot(vx, vy));
    const double cross = (ux * vy - uy * vx) / (lu * lv), dot = (ux * vx + uy * vy) / (lu * lv);
    if (std::fabs(cross) < 1e-9) {  // parallel: across, square to them
      const double k = ((s2->x - s->x) * ux + (s2->y - s->y) * uy) / (lu * lu), fx = s->x + k * ux, fy = s->y + k * uy;
      m.lines << tr("Distance %1 (the lines are parallel)").arg(len(std::hypot(s2->x - fx, s2->y - fy)));
      between(fx, fy, s2->x, s2->y);
    } else {
      const double acute = std::acos(std::min(1.0, std::fabs(dot)));
      m.lines << tr("Angle %1 (and %2)").arg(ang(acute), ang(M_PI - acute));
      m.lines << (n.d < 1e-9 ? tr("The lines cross") : tr("Nearest %1").arg(len(n.d)));
      if (n.d >= 1e-9) between(n.ax, n.ay, n.bx, n.by);
      else beside(n.ax, n.ay, n.ax, n.ay);
    }
  } else {  // circles, arcs, ellipses, splines, and points or lines with them
    m.lines << (n.d < 1e-9 ? tr("They meet") : tr("Nearest %1").arg(len(n.d)));
    if (n.d >= 1e-9) between(n.ax, n.ay, n.bx, n.by);
    else beside(n.ax, n.ay, n.ax, n.ay);
    const SkPoint *ca = a.p ? a.p : centre(a), *cb = b.p ? b.p : centre(b);
    if (ca && cb && ca != cb) m.lines << (a.p || b.p ? tr("To the centre %1") : tr("Centre to centre %1")).arg(len(std::hypot(cb->x - ca->x, cb->y - ca->y)));
  }
  return m;
}

QStringList SketchEditor::measured() const {
  const auto m = measureResult();
  return m ? m->lines : QStringList();
}

void SketchEditor::measureClick(const Hit& h) {
  if (h.kind == Hit::Point || h.kind == Hit::Entity) {
    if (m_picked.size() >= 2) m_picked.clear();  // a third pick starts again from it
    int id = h.id;
    if (const SkEntity* e = m_sk.entity(id); e && e->type == ET::Point && !e->p.empty()) id = e->p[0];
    if (std::find(m_picked.begin(), m_picked.end(), id) == m_picked.end()) m_picked.push_back(id);
  } else if (h.kind == Hit::None) m_picked.clear();  // a click on nothing: measure afresh
  toolPrompt();
  if (const auto m = measureResult()) emit status(tr("Measure: %1").arg(m->lines.join(" · ")));
  rebuild();
}

QString SketchEditor::constraintTip(int id) const {
  const auto found = std::find_if(m_sk.constraints.begin(), m_sk.constraints.end(), [id](const SkConstraint& k) { return k.id == id; });
  if (found == m_sk.constraints.end()) return {};
  const SkConstraint* c = &*found;
  QString name, does;
  const bool lineLength = c->type == CT::Distance && c->refs.size() == 1;
  switch (c->type) {
    case CT::Coincident: name = tr("Coincident constraint"), does = tr("Holds a point on another point or on a curve"); break;
    case CT::Horizontal: name = tr("Horizontal constraint"), does = tr("Keeps the line, or the two points, level with the X axis"); break;
    case CT::Vertical: name = tr("Vertical constraint"), does = tr("Keeps the line, or the two points, upright along the Y axis"); break;
    case CT::Parallel: name = tr("Parallel constraint"), does = tr("Keeps the two lines parallel"); break;
    case CT::Perpendicular: name = tr("Perpendicular constraint"), does = tr("Keeps the two lines at right angles"); break;
    case CT::Collinear: name = tr("Collinear constraint"), does = tr("Keeps the two lines on one line"); break;
    case CT::Tangent: name = tr("Tangent constraint"), does = tr("Keeps the curves touching, running the same way where they meet"); break;
    case CT::Equal: name = tr("Equal constraint"), does = tr("Keeps the lengths, or the radii, the same"); break;
    case CT::Concentric: name = tr("Concentric constraint"), does = tr("Keeps the circles and arcs on one centre"); break;
    case CT::Midpoint: name = tr("Midpoint constraint"), does = tr("Holds the point at the middle of the line"); break;
    case CT::Symmetric: name = tr("Symmetric constraint"), does = tr("Keeps the two points mirror images about the line"); break;
    case CT::Fix: name = tr("Fix constraint"), does = tr("Holds it where it is"); break;
    case CT::Smooth: name = tr("Smooth join (G2)"), does = tr("Joins the curves without a kink or a jump in curvature"); break;
    case CT::Curvature: name = tr("Equal curvature constraint"), does = tr("Gives the spline's end the curvature of what it joins"); break;
    case CT::Rigid: name = tr("Rigid constraint"), does = tr("Keeps the curves' shape: they move and turn as one piece"); break;
    case CT::Distance:
      name = tr("Distance dimension"), does = lineLength ? tr("Holds the line's length") : tr("Holds the distance between them");
      break;
    case CT::HDistance: name = tr("Horizontal distance dimension"), does = tr("Holds the distance along X"); break;
    case CT::VDistance: name = tr("Vertical distance dimension"), does = tr("Holds the distance along Y"); break;
    case CT::Radius: name = tr("Radius dimension"), does = tr("Holds the radius"); break;
    case CT::Diameter: name = tr("Diameter dimension"), does = tr("Holds the diameter"); break;
    case CT::Angle: name = tr("Angle dimension"), does = tr("Holds the angle between the two lines"); break;
    case CT::ArcLength: name = tr("Arc length dimension"), does = tr("Holds the arc's length"); break;
    case CT::Offset:
      name = tr("Offset distance"), does = tr("Keeps the offset copy at this distance from its chain, as one rigid piece");
      break;
  }
  if (c->is_dimension()) {
    name = tr("%1: %2").arg(name, dimensionText(*c));
    if (c->reference) does = tr("Measured only (a reference dimension): it never drives the sketch");
  }
  QStringList held;
  for (size_t i = 0; i < c->refs.size() && (c->type != CT::Offset || int(i) < c->sources); ++i) held << entityName(c->refs[i]);
  QStringList lines{name, does};
  if (!held.isEmpty() && held.size() <= 4) lines << tr("On %1").arg(held.join(", "));
  else if (!held.isEmpty()) lines << tr("On %1 items").arg(held.size());
  if (m_conflicts.count(c->id)) lines << tr("In conflict with the others: the last change was refused");
  lines << help::expand(c->is_dimension() ? tr("Double-click to change the value · {fixed:del} deletes it when selected")
                                          : tr("Click to select it · {fixed:del} deletes it when selected"));
  return lines.join('\n');
}

// Trim, Extend and Split hover the curve under a badge (their clicks take the curve, SketchEditor::hitTest), but the badge
// is named all the same: a hover-only test at the pointer.
int SketchEditor::tipTarget() const {
  if (!m_active || !m_inView) return 0;
  if (m_hover.kind == Hit::Dimension) return m_hover.id;
  if (m_tool == "trim" || m_tool == "extend" || m_tool == "split") return constraintAt(m_lastU, m_lastV).id;
  return 0;
}

// The hover moved onto a constraint, or off one: the status bar names it at once, the tooltip follows after a rest.
void SketchEditor::hoverTip() {
  const int id = tipTarget();
  if (id == m_tipShown) return;
  m_tipShown = id;
  m_tipTimer.stop();
  if (!id) {
    QToolTip::hideText();
    emit m_viewport->hoverChanged(QString());
    return;
  }
  const QString tip = constraintTip(id);
  emit m_viewport->hoverChanged(tip.section('\n', 0, 0));
  m_tipTimer.start();
}
