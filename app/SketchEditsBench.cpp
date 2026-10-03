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
// "Merge") and is merged into it on the drop, one undo step; dropped near a line it is put on it. Trim on a spline and an
// ellipse: the hovered piece between two uprights in red, a click takes it (the pieces' ends on the uprights); a spline cuts
// a line too; a fence takes an ellipse's side; extend runs a line on to an ellipse. <prefix>.fillet.png, <prefix>.fence.png, <prefix>.merge.png, <prefix>.spline.png.
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
  auto extended = std::make_shared<std::pair<double, double>>();  // the line extended to the ellipse, and where it meets it
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
        break;
      }
      case 2: {
        // Trim on a spline and an ellipse (the kernel's cuts): a spline through four points crossed by two uprights, an
        // ellipse crossed by one, a post across the spline's left part.
        setTool("select");
        begin_change();
        SkEntity fit;
        fit.type = SkEntity::Type::Spline;
        for (const auto& [x, y] : std::vector<std::pair<double, double>>{{90, 40}, {100, 46}, {120, 46}, {130, 40}}) fit.p.push_back(m_sk.add_point(x, y));
        fit.id = m_sk.next_id();
        m_sk.entities.push_back(fit);
        const int left = m_sk.add_line(m_sk.add_point(105, 35), m_sk.add_point(105, 52)), right = m_sk.add_line(m_sk.add_point(115, 35), m_sk.add_point(115, 52));
        const int post = m_sk.add_line(m_sk.add_point(95, 35), m_sk.add_point(95, 55));
        SkEntity oval;
        oval.type = SkEntity::Type::Ellipse;
        oval.p = {m_sk.add_point(150, 45), m_sk.add_point(160, 45)};
        oval.r = 5;
        oval.id = m_sk.next_id();
        m_sk.entities.push_back(oval);
        const int cut = m_sk.add_line(m_sk.add_point(155, 35), m_sk.add_point(155, 55));
        end_change(QStringLiteral("Bench"));
        rebuild();
        setTool("trim");
        double hu = 0, hv = 0;  // on the spline midway between the uprights, away from its points (the pick reach is a few mm)
        for (const auto& [x, y] : sampled(*m_sk.entity(fit.id)))
          if (std::abs(x - 110) < std::abs(hu - 110)) hu = x, hv = y;
        sketchMove(hu, hv, Qt::NoModifier, false);
        const auto piece = trimPreview(fit.id, hu, hv);
        const bool between = !piece.empty() && std::all_of(piece.begin(), piece.end(), [](const auto& p) { return p.first > 104.99 && p.first < 115.01; });
        check(m_hover.kind == Hit::Entity && m_hover.id == fit.id && piece.size() > 2 && between && transientSolid(t.red) >= piece.size() - 1,
              "hovering a spline between two uprights shows the piece between them in red");
        m_viewport->grabImage().save(prefix + ".spline.png");
        size_t before = m_undo.size();
        sketchPress(hu, hv, Qt::NoModifier);
        sketchRelease(hu, hv, Qt::NoModifier);
        auto onLine = [this](int point, int line) {
          return std::any_of(m_sk.constraints.begin(), m_sk.constraints.end(), [&](const SkConstraint& c) { return c.type == SkConstraint::Type::Coincident && c.refs == std::vector<int>{point, line}; });
        };
        const SkEntity* first = m_sk.entity(fit.id);
        const SkEntity* second = m_sk.entities.empty() ? nullptr : &m_sk.entities.back();
        check(first && second && first != second && first->type == SkEntity::Type::Spline && second->type == SkEntity::Type::Spline && first->p.front() == fit.p.front() &&
                  second->p.back() == fit.p.back() && equal(m_sk.point(first->p.back())->x, 105) && equal(m_sk.point(second->p.front())->x, 115) &&
                  onLine(first->p.back(), left) && onLine(second->p.front(), right) && m_undo.size() == before + 1 && m_solved.converged,
              "a click takes it: two pieces left, their ends on the uprights, one undo step");
        // A spline cuts a line: the post loses its part above the spline's left piece.
        sketchMove(95, 52, Qt::NoModifier, false);
        sketchPress(95, 52, Qt::NoModifier);
        sketchRelease(95, 52, Qt::NoModifier);
        const SkEntity* postNow = m_sk.entity(post);
        check(postNow && endY(post) > 40 && endY(post) < 47, QString("a spline cuts a line too: the post now ends on it (%1)").arg(endY(post)));
        // A fence across the ellipse's side past the upright takes that side.
        before = m_undo.size();
        sketchPress(157, 47, Qt::NoModifier);
        sketchMove(160, 47, Qt::NoModifier, true);
        sketchMove(163, 47, Qt::NoModifier, true);
        const size_t red = transientSolid(t.red);
        sketchRelease(163, 47, Qt::NoModifier);
        const SkEntity* rest = m_sk.entity(oval.id);
        check(red >= 3 && rest && rest->type == SkEntity::Type::Spline && equal(m_sk.point(rest->p.front())->x, 155) && equal(m_sk.point(rest->p.back())->x, 155) &&
                  onLine(rest->p.front(), cut) && onLine(rest->p.back(), cut) && m_undo.size() == before + 1,
              "a fence across an ellipse past an upright shows that side in red and takes it: one piece round the other side, its ends on the upright");
        undo();
        check(m_sk.entity(oval.id) && m_sk.entity(oval.id)->type == SkEntity::Type::Ellipse, "Undo brings the ellipse back");
        // Extend runs a line on to an ellipse: shown on its samples, made by the kernel.
        begin_change();
        const int reach = m_sk.add_line(m_sk.add_point(125, 48.5), m_sk.add_point(136, 48.5));
        end_change(QStringLiteral("Bench"));
        setTool("extend");
        sketchMove(131, 48.52, Qt::NoModifier, false);
        const double meets = 150 - 10 * std::sqrt(1 - 0.49);
        const auto shown = extendPreview(reach, 131, 48.52);
        check(m_hover.kind == Hit::Entity && m_hover.id == reach && shown.size() == 2 && std::abs(shown.back().first - meets) < 0.05, QString("hovering a line's end before an ellipse shows the run to it (to %1)").arg(shown.empty() ? 0.0 : shown.back().first));
        sketchPress(131, 48.52, Qt::NoModifier);
        sketchRelease(131, 48.52, Qt::NoModifier);
        *extended = {reach, meets};
        break;
      }
      case 3: {  // the extend ran on a worker
        const SkEntity* e = m_sk.entity(int(extended->first));
        check(e && std::abs(m_sk.point(e->p[1])->x - extended->second) < 1e-6, "a click runs it on to the ellipse");
        timer->stop();
        QCoreApplication::exit(*ok ? 0 : 2);
        return;
      }
    }
  });
  timer->start();
}
