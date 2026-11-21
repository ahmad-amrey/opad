// OPAD_BENCH_SKETCH_MEASURE=<prefix> (tools/bench_cases/sketch.py, case sketch-measure): measuring in a sketch is not
// dimensioning, and a constraint under the pointer says what it is.
//  - D starts Measure in the sketch (Distance's key outside it), Shift+D Dimension, D Measure again; keys go to the view as
//    typed ones do (Qt offers them to the shortcuts first).
//  - Measure through the view's own press and release: a line reads its length, then a point its distance square to that
//    line (dashed between them, the values written beside it, in the status line and in the steps); a third pick, a circle,
//    starts again with its diameter, then the point gives the nearest distance and the one to the centre; Backspace takes
//    the point back, Esc clears. The sketch is the same after all of it: no constraint, no point moved, no undo step.
//  - Hovering a dimension's value names it in the status bar at once ("Distance dimension: 32 mm") and in a tooltip after a
//    rest, with what it holds and what a double-click does; off it both go. A badge's text names its constraint.
#include <QApplication>
#include <QKeyEvent>
#include <QTimer>
#include <QToolTip>
#include <cmath>
#include <memory>

#include "Jobs.hpp"
#include "SketchEditor.hpp"

using namespace opad::design;

void SketchEditor::benchMeasure() {
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch measure: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  auto send = [this](int key, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QApplication::setActiveWindow(m_viewport->window());
    m_viewport->setFocus();
    QKeyEvent press(QEvent::KeyPress, key, mods);  // not spontaneous: the shortcut override first, then the window's shortcuts
    QApplication::sendEvent(m_viewport, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods);
    QApplication::sendEvent(m_viewport, &release);
  };
  auto clickAt = [this](double u, double v) {
    sketchMove(u, v, Qt::NoModifier, false);
    sketchPress(u, v, Qt::NoModifier);
    sketchRelease(u, v, Qt::NoModifier);
  };
  auto has = [](const QStringList& list, const QString& text) {
    for (const QString& s : list)
      if (s.contains(text)) return true;
    return false;
  };

  // The sketch: a horizontal line 32 mm long with its length dimension, a free point 12 mm above it, a circle of 8 mm.
  begin_change();
  const int a = m_sk.add_point(-20, -6), b = m_sk.add_point(12, -6);
  const int line = m_sk.add_line(a, b);
  const int point = m_sk.add_point(0, 6);
  const int centre = m_sk.add_point(-10, 12);
  const int circle = m_sk.add_circle(centre, 4);
  const int horizontal = m_sk.add_constraint(SkConstraint::Type::Horizontal, {line});
  const int length = m_sk.add_constraint(SkConstraint::Type::Distance, {line}, 32);
  if (SkConstraint* c = m_sk.constraint(length)) c->pos[0] = -4, c->pos[1] = -12;
  check(end_change(QStringLiteral("Measure bench")), "the sketch is made");
  fitSketch();
  const Sketch before = m_sk;
  const size_t steps = m_undo.size();

  // Keys.
  send(Qt::Key_D);
  check(m_tool == "measure", "D starts Measure in a sketch (" + m_tool + ")");
  send(Qt::Key_D, Qt::ShiftModifier);
  check(m_tool == "dimension", "Shift+D starts Dimension (" + m_tool + ")");
  send(Qt::Key_D);
  check(m_tool == "measure", "D goes back to Measure (" + m_tool + ")");

  QTimer::singleShot(400, this, [=, this] {  // the geometry index ready for picking
    clickAt(-6, -6);
    QStringList m = measured();
    check(m.size() == 3 && m.value(0).startsWith(tr("Length %1").arg(QString())) && m.value(0).contains("32"), "a line: its length, " + m.join(" | "));
    check(has(transientTexts(), "32") && toolSteps().value(0).picked.contains(entityName(line)), "... written in the view and in the first step");
    clickAt(0, 6);
    m = measured();
    check(m.value(0).contains("12") && m.value(0).startsWith(tr("Distance %1 (square to the line)").section(' ', 0, 0)) && m_rubberKinds.count("measure"),
          "a point after it: the distance square to the line, dashed between them, " + m.join(" | "));
    clickAt(-14, 12);
    m = measured();
    check(m_picked.size() == 1 && m.value(0).contains("8"), "a third pick starts again: the circle's diameter, " + m.join(" | "));
    clickAt(0, 6);
    m = measured();
    check(m.size() == 2 && m.value(0).contains("7.") && m.value(1).contains("11."),  // 7.66 and 11.66 mm
          "a circle and a point: the nearest distance and the one to the centre, " + m.join(" | "));
    send(Qt::Key_Backspace);
    check(m_picked.size() == 1 && m_tool == "measure", "Backspace takes the last pick back");
    send(Qt::Key_Escape);
    check(m_picked.empty() && measured().isEmpty() && !has(transientTexts(), "8") && m_tool == "measure", "Esc clears the measure, the tool stays");
    bool same = m_undo.size() == steps && m_sk.constraints.size() == before.constraints.size() && m_sk.points.size() == before.points.size();
    for (size_t i = 0; same && i < m_sk.points.size(); ++i) same = m_sk.points[i].x == before.points[i].x && m_sk.points[i].y == before.points[i].y;
    check(same, "nothing was added to the sketch or moved, no undo step");

    // Hovering.
    const QString hTip = constraintTip(horizontal), dTip = constraintTip(length);
    check(hTip.startsWith(tr("Horizontal constraint")) && hTip.contains(entityName(line)), "a badge names its constraint: " + hTip.section('\n', 0, 0));
    check(dTip.startsWith(tr("Distance dimension")) && dTip.contains("32") && dTip.contains(tr("Holds the line's length")), "a dimension names itself, its value and what it holds");
    auto hover = std::make_shared<QString>();
    auto connection = std::make_shared<QMetaObject::Connection>(connect(m_viewport, &Viewport::hoverChanged, this, [hover](const QString& text) { *hover = text; }));
    setTool("select");
    double lu = 0, lv = 0;
    if (const SkConstraint* c = m_sk.constraint(length)) labelPosition(*c, lu, lv);
    m_inView = true;
    sketchMove(lu, lv, Qt::NoModifier, false);
    check(m_hover.kind == Hit::Dimension && m_hover.id == length && hover->startsWith(tr("Distance dimension")), "hovering its value names it in the status bar at once: " + *hover);
    QTimer::singleShot(900, this, [=, this] {
      check(QToolTip::isVisible() && QToolTip::text().contains(tr("Distance dimension")) && QToolTip::text().contains("Double"),
            "... and in a tooltip after a rest: " + QToolTip::text().replace('\n', " / "));
      sketchMove(lu + 40, lv + 40, Qt::NoModifier, false);
      check(hover->isEmpty() && m_tipShown == 0, "off it, the hover text goes");
      disconnect(*connection);
      trace::log(QString("bench: sketch measure: measuring and naming constraints %1").arg(*ok ? "PASS" : "FAIL"));
      QCoreApplication::exit(*ok ? 0 : 2);
    });
  });
}
