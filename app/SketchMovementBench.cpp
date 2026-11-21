// OPAD_BENCH_SKETCH_MOVEMENT=<prefix> (tools/bench_cases/sketch.py, case sketch-movement): a constraint added through the
// tool, or a dimension changed in its value box, moves what it names and what must follow, never the rest of the sketch
// (SolveOptions::moves; the plain solve spread the step over every free point).
//  - The profile of the report: a rectangle held square, a circle on its top edge, a construction line across it (nothing
//    holds it level), an arc on that line, a short upright line 4 mm long inside. Coincident: a click on the short line's
//    bottom end, then on the bottom edge: the edge comes up to the point, nothing else moves (to 1e-9), the construction
//    line keeps its angle.
//  - Undo; the bottom edge's length dimension typed 100 -> 120 in its box: the sides and what lies on them follow, the
//    circle, the arc and the short line stay.
#include <QCoreApplication>
#include <QLineEdit>
#include <QTimer>
#include <cmath>
#include <map>
#include <memory>
#include <set>

#include "Jobs.hpp"
#include "SketchEditor.hpp"

using namespace opad::design;

void SketchEditor::benchMovement() {
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch movement: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  using CT = SkConstraint::Type;
  begin_change();
  const int a = m_sk.add_point(0, 0), b = m_sk.add_point(100, 0), c = m_sk.add_point(100, 60), d = m_sk.add_point(0, 60);
  const int bottom = m_sk.add_line(a, b), right = m_sk.add_line(b, c), top = m_sk.add_line(c, d), left = m_sk.add_line(d, a);
  m_sk.add_constraint(CT::Horizontal, {bottom});
  m_sk.add_constraint(CT::Horizontal, {top});
  m_sk.add_constraint(CT::Vertical, {left});
  m_sk.add_constraint(CT::Vertical, {right});
  const int centre = m_sk.add_point(50, 60);
  m_sk.add_circle(centre, 6);
  m_sk.add_constraint(CT::Coincident, {centre, top});
  const int gl = m_sk.add_point(0, 40), gr = m_sk.add_point(100, 40);
  const int guide = m_sk.add_line(gl, gr, true);
  m_sk.add_constraint(CT::Coincident, {gl, left});
  m_sk.add_constraint(CT::Coincident, {gr, right});
  const int ac = m_sk.add_point(30, 40);
  m_sk.add_arc(ac, m_sk.add_point(42, 40), m_sk.add_point(30, 52));
  m_sk.add_constraint(CT::Coincident, {ac, guide});
  const int s0 = m_sk.add_point(80, 20), s1 = m_sk.add_point(80, 24);
  const int stub = m_sk.add_line(s0, s1);
  m_sk.add_constraint(CT::Vertical, {stub});
  m_sk.add_constraint(CT::Distance, {stub}, 4);
  const int width = m_sk.add_constraint(CT::Distance, {bottom}, 100);
  if (SkConstraint* k = m_sk.constraint(width)) k->pos[0] = 50, k->pos[1] = -12;
  check(end_change(QStringLiteral("Movement bench")), "the profile is made");
  fitSketch();
  auto places = [this] {
    std::map<int, std::pair<double, double>> out;
    for (const auto& q : m_sk.points) out[q.id] = {q.x, q.y};
    return out;
  };
  auto still = [this](const std::map<int, std::pair<double, double>>& before, const std::set<int>& moved) {
    QStringList wrong;
    for (const auto& q : m_sk.points)
      if (!moved.count(q.id) && (std::fabs(q.x - before.at(q.id).first) > 1e-9 || std::fabs(q.y - before.at(q.id).second) > 1e-9))
        wrong << QString("point %1 (%2, %3)").arg(q.id).arg(q.x).arg(q.y);
    return wrong;
  };
  auto slope = [this](int line) {
    const SkEntity* e = m_sk.entity(line);
    const SkPoint *p = m_sk.point(e->p[0]), *q = m_sk.point(e->p[1]);
    return std::atan2(q->y - p->y, q->x - p->x);
  };
  QTimer::singleShot(400, this, [=, this] {  // the geometry index ready for picking
    const auto before = places();
    const double guideSlope = slope(guide);
    setTool("c:coincident");
    for (const auto& [u, v] : {std::pair{80.0, 20.0}, std::pair{40.0, 0.0}}) {
      sketchMove(u, v, Qt::NoModifier, false);
      sketchPress(u, v, Qt::NoModifier);
      sketchRelease(u, v, Qt::NoModifier);
    }
    bool added = false;
    for (const auto& k : m_sk.constraints) added = added || (k.type == CT::Coincident && k.refs.size() == 2 && k.refs[0] == s0 && k.refs[1] == bottom);
    check(added, "Coincident: the short line's end, then the bottom edge");
    check(std::fabs(m_sk.point(a)->y - 20) < 1e-7 && std::fabs(m_sk.point(b)->y - 20) < 1e-7, "the bottom edge came up to the point");
    const QStringList moved = still(before, {a, b});
    check(moved.isEmpty(), "nothing else moved: " + (moved.isEmpty() ? QString("none") : moved.join(", ")));
    check(std::fabs(slope(guide) - guideSlope) < 1e-12, "the construction line kept its angle");

    undo();
    setTool("select");
    const auto again = places();
    editDimension(width, false);
    check(m_dimEdit && m_dimEdit->isVisible(), "the width's value box opens");
    if (m_dimEdit) {
      m_dimEdit->setText("120");
      commitDimensionEdit();
    }
    check(std::fabs(m_sk.point(b)->x - m_sk.point(a)->x - 120) < 1e-7, "the bottom edge is 120 mm");
    const QStringList followed = still(again, {a, b, c, d, gl, gr});
    check(followed.isEmpty(), "the sides and what lies on them followed, nothing else: " + (followed.isEmpty() ? QString("none") : followed.join(", ")));
    trace::log(QString("bench: sketch movement: least movement %1").arg(*ok ? "PASS" : "FAIL"));
    QCoreApplication::exit(*ok ? 0 : 2);
  });
}
