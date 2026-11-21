// OPAD_BENCH_SKETCH_CHAINDIM=<prefix> (tools/bench_cases/sketch.py, case sketch-chaindim), through the view's own mouse and
// key events:
//  - A double-click on a curve selects the whole chain connected to it through shared ends (an open L of three lines),
//    not the separate line beside it; Shift+double-click on that one adds it; a double-click on a dimension's value still
//    opens its box.
//  - The Dimension tool (Shift+D): picking a line, then moving to place the value draws the whole dimension at the pointer
//    (two extension lines, the dimension line and four arrow strokes) as it moves; after the click that places it the value
//    box opens with the whole dimension still drawn while a value is typed; Enter sets it.
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTimer>
#include <cmath>
#include <memory>
#include <set>

#include "Jobs.hpp"
#include "SketchEditor.hpp"
#include "Theme.hpp"

using namespace opad::design;

void SketchEditor::benchChainDim() {
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch chain dim: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  begin_change();
  const int a = m_sk.add_point(0, 0), b = m_sk.add_point(30, 0), c = m_sk.add_point(30, 20), d = m_sk.add_point(0, 20);
  const int l1 = m_sk.add_line(a, b), l2 = m_sk.add_line(b, c), l3 = m_sk.add_line(c, d);  // an open chain of three
  const int lone = m_sk.add_line(m_sk.add_point(0, -15), m_sk.add_point(30, -15));        // apart from it
  const int width = m_sk.add_constraint(SkConstraint::Type::Distance, {l1}, 30);
  if (SkConstraint* k = m_sk.constraint(width)) k->pos[0] = 15, k->pos[1] = 8;
  check(end_change(QStringLiteral("Chain bench")), "the sketch is made");
  fitSketch();
  auto at = [this](double u, double v) { return m_viewport->widgetPoint(m_frame.to_world(u, v)); };
  auto mouse = [this](QEvent::Type type, const QPoint& p, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {}) {
    QMouseEvent e(type, QPointF(p), QPointF(m_viewport->mapToGlobal(p)), button, buttons, mods);
    QApplication::sendEvent(m_viewport, &e);
  };
  auto click = [mouse](const QPoint& p, Qt::KeyboardModifiers mods = {}) {
    mouse(QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton, mods);
    mouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
    mouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
  };
  auto doubleClick = [mouse, click](const QPoint& p, Qt::KeyboardModifiers mods = {}) {
    click(p, mods);
    mouse(QEvent::MouseButtonDblClick, p, Qt::LeftButton, Qt::LeftButton, mods);
    mouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
  };
  auto key = [this](int k, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QWidget* to = QApplication::focusWidget() ? QApplication::focusWidget() : m_viewport;
    QKeyEvent press(QEvent::KeyPress, k, mods, text);
    QApplication::sendEvent(to, &press);
    QKeyEvent release(QEvent::KeyRelease, k, mods, text);
    QApplication::sendEvent(to, &release);
  };
  auto has = [this](int id) { return std::find(m_sel.begin(), m_sel.end(), id) != m_sel.end(); };
  QTimer::singleShot(400, this, [=, this] {  // the geometry index ready for picking
    setTool("select");
    doubleClick(at(30, 10));  // the L's upright side
    check(m_sel.size() == 3 && has(l1) && has(l2) && has(l3) && !has(lone), QString("a double-click selects the connected chain (%1 selected)").arg(m_sel.size()));
    doubleClick(at(15, -15), Qt::ShiftModifier);
    check(m_sel.size() == 4 && has(lone), "Shift+double-click adds the other chain");
    click(at(40, 40));  // nothing there: the selection goes
    double lu = 0, lv = 0;
    if (const SkConstraint* k = m_sk.constraint(width)) labelPosition(*k, lu, lv);
    doubleClick(at(lu, lv));
    check(m_dimEdit && m_dimEdit->isVisible() && m_dimEditing == width, "a double-click on a dimension's value still opens its box");
    key(Qt::Key_Escape);
    key(Qt::Key_Escape);

    // The dimension tool: the whole dimension drawn while placed and typed.
    QApplication::setActiveWindow(m_viewport->window());
    m_viewport->setFocus();
    {  // to the view, as typed there (the value box just closed had the keyboard)
      QKeyEvent press(QEvent::KeyPress, Qt::Key_D, Qt::ShiftModifier, "D");
      QApplication::sendEvent(m_viewport, &press);
    }
    check(m_tool == "dimension", "Shift+D starts Dimension (" + m_tool + ")");
    const QColor pending = m_viewport->tokens().hov;
    click(at(15, -15));  // the lone line
    mouse(QEvent::MouseMove, at(15, -22), Qt::NoButton, Qt::NoButton);
    mouse(QEvent::MouseMove, at(15, -25), Qt::NoButton, Qt::NoButton);
    const size_t placing = sketchThin(pending);
    check(m_placingDim && placing >= 7, QString("placing it draws the whole dimension at the pointer: %1 lines and arrow strokes").arg(placing));
    click(at(15, -25));
    const bool typing = m_dimEdit && m_dimEdit->isVisible();
    const int made = m_dimEditing;
    auto drawnWhole = [this, made]() -> size_t {  // the new dimension's strokes in whatever colour it is drawn now
      size_t all = 0;
      std::set<QRgb> seen;
      for (const QColor& c : {m_viewport->tokens().hov, m_viewport->tokens().fg2, m_viewport->tokens().fg, m_viewport->tokens().selected3d})
        if (seen.insert(c.rgba()).second) all += sketchThin(c);
      return all;
    };
    check(typing && made && drawnWhole() >= 14, QString("placed: the value box opens and the dimension stays drawn whole (%1 strokes for two dimensions)").arg(drawnWhole()));
    if (typing) {
      m_dimEdit->setFocus();
      m_dimEdit->selectAll();
      for (const QChar ch : QString("40")) key(Qt::Key_0 + ch.digitValue(), {}, QString(ch));
      check(drawnWhole() >= 14 && m_dimEdit->text() == "40", "typing a value keeps the dimension drawn: " + m_dimEdit->text());
      key(Qt::Key_Return);
    }
    const SkConstraint* set = m_sk.constraint(made);
    check(set && std::fabs(set->value - 40) < 1e-6 && drawnWhole() >= 14, "Enter sets it to 40 mm, drawn as before");
    trace::log(QString("bench: sketch chain dim: chains and the live dimension %1").arg(*ok ? "PASS" : "FAIL"));
    QCoreApplication::exit(*ok ? 0 : 2);
  });
}
