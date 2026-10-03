#include "SketchEditor.hpp"
#include "Jobs.hpp"
#include <QApplication>
#include <QTimer>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_EDITS=<prefix> (TODO 11 UI-28): the modify tools as the mouse drives them. Fillet: the radius set before
// a corner is picked, the arc shown on a hovered corner where a line meets an arc, a click rounds it (tangent to both, one
// undo step). Trim: a fence dragged across three uprights shows what it takes in red and takes the three pieces in one undo
// step; Esc drops a fence; a plain click still trims one piece. Extend: one click runs a line's end on to the nearest
// upright (the run shown dashed before), a second click to the next. A dragged point snaps to a point in reach (marked
// "Merge") and is merged into it on the drop, one undo step; dropped near a line it is put on it. <prefix>.fillet.png,
// <prefix>.fence.png, <prefix>.merge.png.
void SketchEditor::benchEdits() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_EDITS");
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch edits: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {85, 12, 100}}, {"target", {85, 12, 0}}, {"up", {0, 1, 0}}, {"scale", 220}, {"projection", "orthographic"}, {"absolute", true}});
  const Tokens& t = m_viewport->tokens();
  // The drawing, made at once: a line and an arc meeting at (10, 10); a bar across three uprights; a short line before two
  // walls; two pairs of lines to join by dragging.
  begin_change();
  const int corner = m_sk.add_point(10, 10);
  const int line = m_sk.add_line(corner, m_sk.add_point(40, 10)), arc = m_sk.add_arc(m_sk.add_point(0, 10), corner, m_sk.add_point(0, 20));
  const int bar = m_sk.add_line(m_sk.add_point(45, 15), m_sk.add_point(85, 15));
  std::vector<int> uprights;
  for (double x : {50.0, 60.0, 70.0}) uprights.push_back(m_sk.add_line(m_sk.add_point(x, 0), m_sk.add_point(x, 30)));
  const int stub = m_sk.add_line(m_sk.add_point(90, 0), m_sk.add_point(105, 0));
  for (double x : {115.0, 135.0}) m_sk.add_line(m_sk.add_point(x, -5), m_sk.add_point(x, 5));
  const int m1end = m_sk.add_point(160, 0), m2start = m_sk.add_point(162.5, 1.5);
  m_sk.add_line(m_sk.add_point(150, 0), m1end);
  const int m2 = m_sk.add_line(m2start, m_sk.add_point(170, 10));
  const int rail = m_sk.add_line(m_sk.add_point(145, 20), m_sk.add_point(170, 20)), free = m_sk.add_point(155, 25);
  m_sk.add_line(free, m_sk.add_point(160, 30));
  end_change(QStringLiteral("Bench"));
  rebuild();
  trace::log(QString("bench: sketch edits: pick distance %1 mm").arg(tol()));
  auto endY = [this](int id) { const SkEntity* e = m_sk.entity(id); return e ? std::max(m_sk.point(e->p[0])->y, m_sk.point(e->p[1])->y) : -1.0; };
  auto equal = [](double a, double b) { return std::abs(a - b) < 1e-6; };

  // Fillet where a line meets an arc.
  setTool("fillet");
  m_options["radius"] = "2";
  sketchMove(10.2, 10.1, Qt::NoModifier, false);
  const auto shown = filletPreview(corner);
  check(m_hover.kind == Hit::Point && m_hover.id == corner && shown.size() > 2 && transientSolid(t.hov) >= shown.size() - 1,
        "the radius set first, hovering the corner where a line meets an arc shows the fillet");
  m_viewport->grabImage().save(prefix + ".fillet.png");
  size_t steps = m_undo.size();
  sketchPress(10.2, 10.1, Qt::NoModifier);
  sketchRelease(10.2, 10.1, Qt::NoModifier);
  const SkEntity* round = m_sk.entities.empty() ? nullptr : &m_sk.entities.back();
  const bool tangent = round && round->type == SkEntity::Type::Arc &&
                       std::count_if(m_sk.constraints.begin(), m_sk.constraints.end(), [&](const SkConstraint& c) { return c.type == SkConstraint::Type::Tangent && c.refs.size() == 2 && c.refs[1] == round->id && (c.refs[0] == line || c.refs[0] == arc); }) == 2;
  check(tangent && m_solved.converged && m_undo.size() == steps + 1 && !m_sk.point(corner), "a click rounds the line-arc corner: an arc tangent to both, one undo step");
  check(round && equal(std::hypot(m_sk.point(round->p[1])->x - m_sk.point(round->p[0])->x, m_sk.point(round->p[1])->y - m_sk.point(round->p[0])->y), 2), "of the radius set");

  // Fence trim.
  setTool("trim");
  steps = m_undo.size();
  sketchPress(47, 25, Qt::NoModifier);
  sketchMove(60, 25, Qt::NoModifier, true);
  sketchMove(75, 25, Qt::NoModifier, true);
  check(m_fencing && m_fenceMoved && transientSolid(t.red) >= 3, "a drag with the trim tool is a fence; what it takes shows in red");
  m_viewport->grabImage().save(prefix + ".fence.png");
  sketchRelease(75, 25, Qt::NoModifier);
  check(equal(endY(uprights[0]), 15) && equal(endY(uprights[1]), 15) && equal(endY(uprights[2]), 15) && m_undo.size() == steps + 1,
        "the fence takes the three pieces above the bar, one undo step");
  sketchPress(47, 5, Qt::NoModifier);
  sketchMove(73, 5, Qt::NoModifier, true);
  escape();
  sketchRelease(73, 5, Qt::NoModifier);
  check(!m_fencing && m_undo.size() == steps + 1 && equal(endY(uprights[0]), 15), "Esc drops a fence");
  sketchMove(77.5, 15, Qt::NoModifier, false);
  sketchPress(77.5, 15, Qt::NoModifier);
  sketchRelease(77.5, 15, Qt::NoModifier);
  const SkEntity* barNow = m_sk.entity(bar);
  check(barNow && equal(std::max(m_sk.point(barNow->p[0])->x, m_sk.point(barNow->p[1])->x), 70), "a click still trims one piece (the bar past the last upright)");

  // One-click extend.
  setTool("extend");
  sketchMove(100, 0.1, Qt::NoModifier, false);
  const auto run = extendPreview(stub, 100, 0.1);
  check(m_hover.kind == Hit::Entity && m_hover.id == stub && run.size() == 2 && equal(run.back().first, 115), "hovering a line near its end shows where it runs to");
  sketchPress(100, 0.1, Qt::NoModifier);
  sketchRelease(100, 0.1, Qt::NoModifier);

  auto phase = std::make_shared<int>(0), ticks = std::make_shared<int>(0);
  auto* timer = new QTimer(this);
  timer->setInterval(50);
  connect(timer, &QTimer::timeout, this, [=] {
    if (++*ticks > 400) {
      check(false, QString("phase %1 in time").arg(*phase));
      timer->stop();
      return QCoreApplication::exit(2);
    }
    if (m_editJob) return;
    auto stubEnd = [this, stub] { const SkEntity* e = m_sk.entity(stub); return e ? m_sk.point(e->p[1])->x : -1.0; };
    switch ((*phase)++) {
      case 0:
        check(equal(stubEnd(), 115), "one click runs the end on to the nearest upright");
        sketchMove(108, 0.1, Qt::NoModifier, false);
        sketchPress(108, 0.1, Qt::NoModifier);
        sketchRelease(108, 0.1, Qt::NoModifier);
        break;
      case 1: {
        check(equal(stubEnd(), 135), "a second click on to the next");
        // A dragged point snaps to the point in reach and merges into it on the drop.
        setTool("select");
        const size_t points = m_sk.points.size(), before = m_undo.size();
        sketchMove(162.5, 1.5, Qt::NoModifier, false);
        sketchPress(162.5, 1.5, Qt::NoModifier);
        sketchMove(161, 0.8, Qt::NoModifier, true);
        sketchMove(160.3, 0.2, Qt::NoModifier, true);
        check(m_dropPoint == m1end && transientTexts().contains(tr("Merge")) && equal(m_sk.point(m2start)->x, 160) && equal(m_sk.point(m2start)->y, 0),
              "a dragged point is held to the point in reach, marked Merge");
        m_viewport->grabImage().save(prefix + ".merge.png");
        sketchRelease(160.3, 0.2, Qt::NoModifier);
        check(!m_sk.point(m2start) && m_sk.entity(m2)->p[0] == m1end && m_sk.points.size() == points - 1 && m_undo.size() == before + 1,
              "dropped there it is merged into it: one point, one undo step");
        undo();
        check(m_sk.point(m2start) && m_sk.entity(m2)->p[0] == m2start, "Undo parts them again");
        // Dropped near a line: put on it.
        sketchMove(155, 25, Qt::NoModifier, false);
        sketchPress(155, 25, Qt::NoModifier);
        sketchMove(157, 22, Qt::NoModifier, true);
        sketchMove(157, 20.3, Qt::NoModifier, true);
        const bool held = m_dropCurve == rail && transientTexts().contains(tr("On curve"));
        sketchRelease(157, 20.3, Qt::NoModifier);
        check(held && equal(m_sk.point(free)->y, 20) &&
                  std::any_of(m_sk.constraints.begin(), m_sk.constraints.end(), [&](const SkConstraint& c) { return c.type == SkConstraint::Type::Coincident && c.refs == std::vector<int>{free, rail}; }),
              "dropped near a line it is put on it (a coincidence)");
        // Alt drags freely.
        sketchMove(162.5, 1.5, Qt::NoModifier, false);
        sketchPress(162.5, 1.5, Qt::NoModifier);  // Alt with the press inserts a spline node
        sketchMove(160.3, 0.2, Qt::AltModifier, true);
        sketchRelease(160.3, 0.2, Qt::AltModifier);
        check(m_sk.point(m2start) && equal(m_sk.point(m2start)->x, 160.3), "with Alt it drags freely and stays apart");
        timer->stop();
        QCoreApplication::exit(*ok ? 0 : 2);
        return;
      }
    }
  });
  timer->start();
}
