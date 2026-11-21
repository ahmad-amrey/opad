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
//  - In Trim (whose clicks take the curve under a badge) the badge is named all the same: through the view's own mouse
//    events the status bar and the tooltip name the Horizontal badge, and a click on that spot still trims the line.
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
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

      // Trim: its clicks take the curve under a badge, yet the badge is named on hover. Through the view's own mouse events.
      setTool("trim");
      QPoint badge;
      for (size_t i = 0; i < m_glyphHits.size() && i < m_glyphAnchors.size(); ++i)
        if (std::get<0>(m_glyphHits[i]) == horizontal) {
          const GlyphAnchor& g = m_glyphAnchors[i];
          badge = m_viewport->widgetPoint(m_frame.to_world(g.u, g.v)) + QPoint(int(std::lround(g.dx)), -int(std::lround(g.dy)));
        }
      auto mouse = [this](QEvent::Type type, const QPoint& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent e(type, QPointF(at), QPointF(m_viewport->mapToGlobal(at)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(m_viewport, &e);
      };
      // Where the badge overlaps its line (a click there is the one the badge used to take): on both, by the view's tests.
      QPoint spot;
      const int reach = int(kHandlePixels) - 1;
      for (int dy = -reach; dy <= reach && spot.isNull(); ++dy)
        for (int dx = -reach; dx <= reach && spot.isNull(); ++dx) {
          double su = 0, sv = 0;
          if (badge.isNull() || !m_viewport->planePoint(QPointF(badge + QPoint(dx, dy)), m_frame, su, sv)) continue;
          const Hit onCurve = hitTest(su, sv);
          if (constraintAt(su, sv).id == horizontal && onCurve.kind == Hit::Entity && onCurve.id == line) spot = badge + QPoint(dx, dy);
        }
      check(!badge.isNull() && !spot.isNull(), "the Horizontal badge is drawn over its line");
      badge = spot;
      mouse(QEvent::MouseMove, badge + QPoint(1, 0), Qt::NoButton, Qt::NoButton);
      mouse(QEvent::MouseMove, badge, Qt::NoButton, Qt::NoButton);
      check(m_tool == "trim" && m_hover.kind != Hit::Dimension && hover->startsWith(tr("Horizontal constraint")),
            "Trim: the pointer on the badge still hovers the curve, and the status bar names the badge: " + *hover);
      QTimer::singleShot(900, this, [=, this] {
        check(QToolTip::isVisible() && QToolTip::text().startsWith(tr("Horizontal constraint")), "... and the tooltip after a rest: " + QToolTip::text().section(QChar('\n'), 0, 0));
        auto said = std::make_shared<QString>();
        connect(this, &SketchEditor::status, this, [said](const QString& text) { *said = text; });
        mouse(QEvent::MouseButtonPress, badge, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, badge, Qt::LeftButton, Qt::NoButton);
        QTimer::singleShot(300, this, [=, this] {  // said: what the status line said after the click
          check(m_sk.entity(line) == nullptr, "a click on the same spot still trims the line (the badge never takes the click); status: " + *said);
          disconnect(*connection);
          trace::log(QString("bench: sketch measure: measuring and naming constraints %1").arg(*ok ? "PASS" : "FAIL"));
          QCoreApplication::exit(*ok ? 0 : 2);
        });
      });
    });
  });
}
