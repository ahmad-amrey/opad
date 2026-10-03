#include "SketchEditor.hpp"
#include "Jobs.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QSettings>
#include <QToolButton>
#include <cmath>
#include <functional>
#include <set>

using namespace opad::design;
using CT = SkConstraint::Type;

// OPAD_BENCH_SKETCH_SHAPES=<prefix> (TODO 11 UI-17): shapes drawn by the keyboard alone (key events where the keyboard
// is, the pointer never over the view), checked exactly, the typed sizes kept as driving dimensions. L, 0 Tab 0 Enter
// starts on the origin; 50 Tab 30 Enter is a line 50 long at 30 degrees (held against an X axis line), its boxes sitting
// on the rubber band where they measure, never on each other or on the line; 40 Tab 120 is perpendicular to it; 20 Tab 45
// from the last line (the angle box's switch) is held at 45 degrees to it; @-10,5 by its signed ΔX and ΔY. R, 10,10
// Enter, 40 Tab 25 Enter: a 40 x 25 rectangle, its sides dimensioned; a negative width goes left; a zero size is refused.
// U, the slot: two centres 30 apart, then its width 8. C: diameter 20, then (the box's switch) a radius 5. N, a pentagon
// by its diameter, angle and sides, then circumscribed by the box's switch; a three-point arc by its chord and radius; a
// centre arc that sweeps 270 degrees, held as its length so an edited radius keeps the sweep; an arc slot; with the
// panel's switch off nothing typed becomes a dimension; tangent arcs by radius and sweep, one past half a turn; the
// fillet's radius typed before its corner is picked, its arc shown on the hovered corner; a chamfer by 4<30; three copies
// by Shift+C 15<90 Tab 3; a text typed and placed by keys; an image's calibration distance.
void SketchEditor::benchShapes() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_SHAPES");
  QWidget* window = m_viewport->window();
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch shapes: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  auto keyboard = [this]() -> QWidget* { QWidget* w = QApplication::focusWidget(); return w ? w : m_viewport; };
  auto send = [keyboard](int key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(keyboard(), &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QApplication::sendEvent(keyboard(), &release);
  };
  auto type = [send](const QString& chars) {
    for (const QChar c : chars) {
      const int key = c.isDigit() ? Qt::Key_0 + c.digitValue() : c == '.' ? Qt::Key_Period : c == ',' ? Qt::Key_Comma : c == '-' ? Qt::Key_Minus : Qt::Key_unknown;
      send(key, Qt::NoModifier, QString(c));
    }
  };
  auto enter = [send] { send(Qt::Key_Return); };
  auto tab = [send] { send(Qt::Key_Tab); };
  // A tool by its key (the window's shortcut); the ones without a default key from their command.
  auto tool = [this, window, send, check](int key, const QString& text, const QString& name) {
    if (key) send(key, Qt::NoModifier, text);
    else if (auto* action = window->findChild<QAction*>("sketch." + name)) action->trigger();
    check(m_tool == name, QString("%1 starts the %2 tool").arg(key ? text.toUpper() : QStringLiteral("its command"), name));
  };
  auto chip = [this]() -> QToolButton* {
    for (auto* b : m_input->findChildren<QToolButton*>("dynamicInputChip"))
      if (!b->isHidden()) return b;
    return nullptr;
  };
  auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
  auto at = [this, same](int id, double u, double v) { const SkPoint* p = m_sk.point(id); return p && same(p->x, u) && same(p->y, v); };
  auto pointAtXY = [this, same](double u, double v) {
    for (const auto& p : m_sk.points)
      if (same(p.x, u) && same(p.y, v)) return p.id;
    return 0;
  };
  auto lineBetween = [this, at](double u0, double v0, double u1, double v1) {
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Line && e.p.size() == 2 && ((at(e.p[0], u0, v0) && at(e.p[1], u1, v1)) || (at(e.p[0], u1, v1) && at(e.p[1], u0, v0)))) return e.id;
    return 0;
  };
  auto has = [this, same](CT type, std::vector<int> refs, double value = -1) {
    std::sort(refs.begin(), refs.end());
    for (const auto& c : m_sk.constraints) {
      auto r = c.refs;
      std::sort(r.begin(), r.end());
      if (c.type == type && r == refs && (value < 0 || same(c.value, value)) && c.expr.empty() && !c.reference) return true;
    }
    return false;
  };
  auto held = [this, same](double u, double v) { return m_cursor.kind == Snap::Kind::Typed && same(m_cursor.u, u) && same(m_cursor.v, v); };
  auto where = [this] { return QString("(%1, %2)").arg(m_cursor.u, 0, 'g', 12).arg(m_cursor.v, 0, 'g', 12); };
  // Read out on the rubber band: drawn there, or shown by the box that sits there.
  auto reads = [this](const QString& text) {
    const auto marks = readouts();
    return transientTexts().contains(text) || std::any_of(marks.begin(), marks.end(), [&](const Readout& r) { return r.text == text && boxed(r.key); });
  };
  // Box `index` sits off what it measures: its nearest edge within `reach` pixels of (u, v) in the sketch.
  auto sits = [this](int index, double u, double v, int reach) {
    QLineEdit* edit = m_input->box(index);
    if (!edit || !m_input->isVisible()) return false;
    const QRect r(edit->parentWidget()->geometry());
    const QPoint p = m_viewport->widgetPoint(m_frame.to_world(u, v));
    const int dx = std::max({r.left() - p.x(), 0, p.x() - r.right()}), dy = std::max({r.top() - p.y(), 0, p.y() - r.bottom()});
    return std::hypot(dx, dy) <= reach;
  };
  // Box `index` covers no part of the segment (u0, v0) - (u1, v1).
  auto clear = [this](int index, double u0, double v0, double u1, double v1) {
    QLineEdit* edit = m_input->box(index);
    if (!edit) return false;
    const QRect r(edit->parentWidget()->geometry());
    for (int i = 0; i <= 64; ++i)
      if (r.contains(m_viewport->widgetPoint(m_frame.to_world(u0 + (u1 - u0) * i / 64, v0 + (v1 - v0) * i / 64)))) return false;
    return true;
  };
  // The view with the boxes over it (the view's own grab leaves the native boxes out).
  auto shot = [this, check](const QString& path) {
    bool apart = true;
    for (int i = 0; i < m_input->count(); ++i)
      for (int j = i + 1; j < m_input->count(); ++j) apart = apart && !m_input->box(i)->parentWidget()->geometry().intersects(m_input->box(j)->parentWidget()->geometry());
    check(apart, "no box covers another: " + QFileInfo(path).fileName());
    QImage image = m_viewport->grabImage();
    const double scale = double(image.width()) / std::max(1, m_viewport->width());
    if (m_input->isVisible()) {
      QPainter painter(&image);
      const QRect r = m_input->boxesRect();
      painter.drawPixmap(QRectF(r.x() * scale, r.y() * scale, r.width() * scale, r.height() * scale), m_input->shot(), QRectF());
    }
    image.save(path);
  };
  const double degree = M_PI / 180;
  // One part per turn of the event loop (the view repaints between them), each checking as it goes.
  auto steps = std::make_shared<std::vector<std::function<void()>>>();
  auto step = [steps](std::function<void()> part) { steps->push_back(std::move(part)); };

  QApplication::setActiveWindow(window);
  m_viewport->setFocus();
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {50, 30, 100}}, {"target", {50, 30, 0}}, {"up", {0, 1, 0}}, {"scale", 190}, {"projection", "orthographic"}, {"absolute", true}});
  auto* keep = window->findChild<QCheckBox*>("input-addDimensions");
  check(keep && keep->isChecked() && QSettings().value("sketch/input/addDimensions", true).toBool(), "typed values become dimensions by default (the sketch panel's switch is on)");
  check(!m_haveCursor, "the pointer has never been over the view");

  // A line from the keyboard: L, 0 Tab 0 Enter, 50 Tab 30 Enter.
  step([=] {
    tool(Qt::Key_L, "l", "line");
    type("0");
    tab();
    type("0");
    enter();
    const int origin = pointAtXY(0, 0);
    check(m_chain.size() == 1 && m_chain[0] == origin && m_sk.point(origin)->fixed, "0 Tab 0 Enter starts the line on the origin point itself");
    check(m_input->count() == 2 && m_input->key(0) == "length" && m_input->key(1) == "angle", "then the boxes are the length and the angle");
    type("50");
    check(held(50, 0) && reads("50 mm"), "50 typed holds the rubber band 50 long (along X: no pointer) and reads it out " + where() + " " + transientTexts().join(" | "));
    tab();
    type("30");
    const double x1 = 50 * std::cos(30 * degree), y1 = 50 * std::sin(30 * degree);
    check(held(x1, y1) && reads("50 mm") && reads(QString::fromUtf8("30°")), "Tab 30 turns it to 30 degrees, read out by its length and angle " + where());
    const double arc = 40 * m_viewport->pixelSize();
    check(sits(0, x1 / 2, y1 / 2, 30) && !sits(0, 0, 0, 30) && sits(1, arc * std::cos(-M_PI / 7), arc * std::sin(-M_PI / 7), 30) && clear(0, 0, 0, x1, y1) && clear(1, 0, 0, x1, y1) &&
              !transientTexts().contains("50 mm"),
          "the length box sits by the line's middle, the angle box by its arc's start, outside the narrow angle, neither over the line (their values not drawn twice)");
    shot(prefix + ".line.png");
    m_input->shot().save(prefix + ".line-input.png");
    enter();
    const int first = lineBetween(0, 0, x1, y1);
    check(first && m_chain.size() == 2, "Line 50 Tab 30 Enter: a line from the origin to exactly 50 at 30 degrees");
    check(has(CT::Distance, {first}, 50), "its typed length is its driving dimension, 50");
    int axis = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Line && e.fixed && e.construction && at(e.p[0], 0, 0) && at(e.p[1], 1, 0)) axis = e.id;
    check(axis && has(CT::Angle, {axis, first}, 30 * degree), "its typed 30 degrees is held against a fixed line along the X axis");
    type("40");
    tab();
    type("120");
    enter();
    const double x2 = x1 + 40 * std::cos(120 * degree), y2 = y1 + 40 * std::sin(120 * degree);
    const int second = lineBetween(x1, y1, x2, y2);
    check(second && has(CT::Distance, {second}, 40) && has(CT::Perpendicular, {first, second}), "40 Tab 120: 40 long and held perpendicular to the line before");
    QToolButton* angleChip = chip();
    check(angleChip && angleChip->text() == QString::fromUtf8("∠ X axis"), "the angle box measures from the X axis");
    if (angleChip) angleChip->click();
    check(m_angleRelative, "its switch measures from the last line");
    type("20");
    tab();
    type("45");
    const double x3 = x2 + 20 * std::cos(165 * degree), y3 = y2 + 20 * std::sin(165 * degree);
    check(held(x3, y3) && reads(QString::fromUtf8("45°")), "20 Tab 45 from the last line (at 120) goes off at 165 " + where());
    shot(prefix + ".relative.png");
    enter();
    const int third = lineBetween(x2, y2, x3, y3);
    check(third && has(CT::Distance, {third}, 20) && has(CT::Angle, {second, third}, 45 * degree), "and is held at 45 degrees to it");
    // @-10,5: ΔX and ΔY from the last point, kept as its signed horizontal and vertical distances.
    send(Qt::Key_At, Qt::ShiftModifier, "@");
    type("-10,5");
    enter();
    const int fourth = lineBetween(x3, y3, x3 - 10, y3 + 5), from = pointAtXY(x3, y3), to = pointAtXY(x3 - 10, y3 + 5);
    bool across = false, up = false;
    for (const auto& c : m_sk.constraints) {
      across = across || (c.type == CT::HDistance && c.is_signed && c.refs == std::vector<int>{from, to} && same(c.value, -10));
      up = up || (c.type == CT::VDistance && c.is_signed && c.refs == std::vector<int>{from, to} && same(c.value, 5));
    }
    check(fourth && across && up && m_solved.converged, "@-10,5 Enter: 10 left and 5 up, held by its signed horizontal and vertical distances");
    if (QToolButton* c = chip()) c->click();
    check(!m_angleRelative, "the switch back: from the X axis");
    send(Qt::Key_Escape);
    check(m_chain.empty() && m_tool == "line", "Esc ends the chain");
    check(third && std::abs(m_sk.point(m_sk.entity(third)->p[1])->x - x3) < 1e-9 && m_solved.converged, "the solver left the typed geometry where it was");
  });

  // A rectangle: R, 10,10 Enter, 40 Tab 25 Enter.
  step([=] {
    tool(Qt::Key_R, "r", "rect");
    type("10,10");
    enter();
    check(m_clicks.size() == 1 && m_input->count() == 2 && m_input->key(0) == "width" && m_input->key(1) == "height", "10,10 Enter puts the first corner; the boxes are now width and height");
    type("40");
    tab();
    type("25");
    check(held(50, 35) && reads("40 mm") && reads("25 mm"), "40 Tab 25 holds the opposite corner at (50, 35), read out under and beside it " + where());
    check(sits(0, 30, 10, 30) && sits(1, 50, 22.5, 30), "the width box sits under the rectangle's bottom, the height box beside its right side");
    shot(prefix + ".rect.png");
    m_input->shot().save(prefix + ".rect-input.png");
    enter();
    const int bottom = lineBetween(10, 10, 50, 10), right = lineBetween(50, 10, 50, 35);
    check(bottom && right && lineBetween(50, 35, 10, 35) && lineBetween(10, 35, 10, 10) && m_clicks.empty(), "a 40 x 25 rectangle from (10, 10), exactly");
    check(has(CT::Distance, {bottom}, 40) && has(CT::Distance, {right}, 25), "its width and height are its dimensions");
    type("100,0");
    enter();
    type("-20,15");
    enter();
    check(lineBetween(80, 0, 100, 0) && lineBetween(100, 0, 100, 15) && has(CT::Distance, {lineBetween(80, 0, 100, 0)}, 20), "a negative width goes left: (80, 0) to (100, 15), its width 20");
    type("200,0");
    enter();
    type("0");
    check(!m_input->problem("width").isEmpty() && m_input->box(0)->property("invalid").toBool(), "a zero width is refused: " + m_input->problem("width"));
    enter();
    check(m_clicks.size() == 1, "Enter with it leaves the corner waiting");
    send(Qt::Key_Escape);
    send(Qt::Key_Escape);
    check(m_clicks.empty() && m_tool == "rect", "Esc drops the value, then the corner");
  });

  // A slot (U): its centres, then its width.
  step([=] {
    tool(Qt::Key_U, "u", "slot");
    type("0,-40");
    enter();
    type("30");
    tab();
    type("0");
    check(held(30, -40), "the second centre 30 along X " + where());
    enter();
    check(m_clicks.size() == 2 && m_input->count() == 1 && m_input->key(0) == "width", "then the slot's box is its width");
    type("8");
    check(held(30, -36) && reads("8 mm"), "8 holds it 4 off the centre line (on its left: no pointer) " + where());
    shot(prefix + ".slot.png");
    enter();
    const int top = lineBetween(0, -36, 30, -36), under = lineBetween(30, -44, 0, -44), centres = lineBetween(0, -40, 30, -40);
    bool caps = true;
    int arcs = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Arc && (at(e.p[0], 0, -40) || at(e.p[0], 30, -40))) {
        ++arcs;
        caps = caps && std::abs(std::hypot(m_sk.point(e.p[1])->x - m_sk.point(e.p[0])->x, m_sk.point(e.p[1])->y - m_sk.point(e.p[0])->y) - 4) < 1e-9;
      }
    check(top && under && centres && arcs == 2 && caps, "a slot 30 between centres and 8 wide, exactly");
    check(has(CT::Distance, {centres}, 30) && has(CT::Distance, {top, under}, 8) && has(CT::Horizontal, {centres}), "its length, its width and its 0 degrees hold it");
  });

  // A circle by its diameter, then (the box's switch) by its radius.
  step([=] {
    tool(Qt::Key_C, "c", "circle");
    type("0,60");
    enter();
    check(m_input->count() == 1 && m_input->key(0) == "diameter", "the circle's box is its diameter");
    type("20");
    check(held(10, 60) && reads(QString::fromUtf8("Ø 20 mm")), "20 holds the rim 10 from the centre " + where());
    enter();
    int circle = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Circle && at(e.p[0], 0, 60) && same(e.r, 10)) circle = e.id;
    check(circle && has(CT::Diameter, {circle}, 20), "a circle 20 across, its diameter dimensioned");
    type("80,60");
    enter();
    if (QToolButton* c = chip()) c->click();
    check(m_circleRadius && m_input->key(0) == "radius", "the box's switch: its radius");
    type("5");
    enter();
    circle = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Circle && at(e.p[0], 80, 60) && same(e.r, 5)) circle = e.id;
    check(circle && has(CT::Radius, {circle}, 5), "a radius of 5, dimensioned as a radius");
    type("0,0");
    enter();
    if (QToolButton* c = chip()) c->click();
    check(!m_circleRadius && m_input->key(0) == "diameter", "and the switch back to the diameter");
    send(Qt::Key_Escape);
    send(Qt::Key_Escape);
  });

  // A pentagon by its diameter, its angle and its sides.
  step([=] {
    tool(Qt::Key_N, "n", "polygon");
    type("150,60");
    enter();
    check(m_input->count() == 3 && m_input->key(0) == "diameter" && m_input->key(2) == "sides", "the polygon's boxes: diameter, angle, sides");
    send(Qt::Key_Backtab, Qt::ShiftModifier);
    type("7");
    enter();
    check(option("sides") == "7" && m_clicks.size() == 1 && m_input->box(2)->placeholderText() == "7", "Shift+Tab 7 Enter sets the sides only: the corner still waits");
    type("30");
    tab();
    type("90");
    tab();
    type("5");
    check(held(150, 75) && option("sides") == "5", "30 Tab 90 Tab 5: the first corner 15 above the centre, five sides " + where());
    enter();
    int sides = 0, guide = 0;
    for (const auto& e : m_sk.entities) {
      if (e.type == SkEntity::Type::Circle && e.construction && at(e.p[0], 150, 60)) guide = e.id;
      if (e.type == SkEntity::Type::Line && (at(e.p[0], 150, 75) || at(e.p[1], 150, 75))) ++sides;
    }
    check(guide && sides == 2 && same(m_sk.entity(guide)->r, 15) && has(CT::Diameter, {guide}, 30) && has(CT::Vertical, {pointAtXY(150, 60), pointAtXY(150, 75)}),
          "a pentagon 30 across its corners, the first straight up, held so");
    // The diameter box's switch: the same tool draws it circumscribed (sides touching the circle), centre kept.
    type("150,0");
    enter();
    QToolButton* kind = chip();
    check(kind && kind->text() == "Inscribed", "the diameter box's switch says inscribed");
    if (kind) kind->click();
    check(m_tool == "polygon_outer" && m_clicks.size() == 1 && chip() && chip()->text() == "Circumscribed" && m_input->key(0) == "diameter",
          "its click makes it circumscribed, the centre still placed");
    type("20");
    tab();
    type("0");
    enter();
    const double half = 10 * std::tan(36 * degree);
    bool across = false;
    for (const auto& c : m_sk.constraints)
      across = across || (c.type == CT::Distance && same(c.value, 10) && std::find(c.refs.begin(), c.refs.end(), pointAtXY(150, 0)) != c.refs.end());
    check(lineBetween(160, -half, 160, half) && across, "20 Tab 0: a pentagon 20 across its flats, one side upright at x 160, the size held");
    if (QToolButton* c = chip()) c->click();
    send(Qt::Key_Escape);
  });

  // A three-point arc by its chord and its radius; a centre arc that sweeps past half a turn.
  step([=] {
    tool(Qt::Key_A, "a", "arc3");
    type("0,100");
    enter();
    type("40");
    tab();
    type("0");
    enter();
    check(m_input->count() == 1 && m_input->key(0) == "radius", "after its ends, the arc's box is its radius");
    type("19");
    check(!m_input->problem("radius").isEmpty(), "a radius under half the chord is refused: " + m_input->problem("radius"));
    send(Qt::Key_Backspace);
    send(Qt::Key_Backspace);
    type("25");
    check(held(20, 110) && reads("R 25 mm"), "25: the shorter arc's middle 10 above the chord " + where());
    enter();
    int arc = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Arc && at(e.p[0], 20, 85)) arc = e.id;
    check(arc && has(CT::Radius, {arc}, 25) && has(CT::Distance, {pointAtXY(0, 100), pointAtXY(40, 100)}, 40), "an arc of radius 25 over a chord of 40, both dimensioned");
    send(Qt::Key_Escape);
    tool(0, {}, "arcc");
    type("150,120");
    enter();
    type("10");
    tab();
    type("0");
    enter();
    check(m_input->key(0) == "sweep", "after its start, the centre arc's box is its sweep");
    type("270");
    check(held(150, 110), "270 holds its end straight below the centre " + where());
    enter();
    arc = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Arc && at(e.p[0], 150, 120) && at(e.p[1], 160, 120) && at(e.p[2], 150, 110)) arc = e.id;
    check(arc && has(CT::Radius, {arc}, 10), "a counter-clockwise arc of 270 degrees from (160, 120) to (150, 110), its radius dimensioned");
    const SkConstraint* sweep = nullptr;
    int radius = 0;
    for (const auto& c : m_sk.constraints) {
      if (c.type == CT::ArcLength && c.refs == std::vector<int>{arc}) sweep = &c;
      if (c.type == CT::Radius && c.refs == std::vector<int>{arc}) radius = c.id;
    }
    check(sweep && sweep->expr == QString("d%1 * 270 deg").arg(radius).toStdString() && same(sweep->value, 15 * M_PI) && !has(CT::Vertical, {pointAtXY(150, 120), pointAtXY(150, 110)}),
          "its sweep is held as its length, the radius times 270 degrees: " + QString::fromStdString(sweep ? sweep->expr : std::string()));
    begin_change();  // the radius edited: the sweep stays
    m_sk.constraint(radius)->value = 20;
    end_change("radius");
    const SkEntity* e = m_sk.entity(arc);
    const SkPoint *o = e ? m_sk.point(e->p[0]) : nullptr, *s = e ? m_sk.point(e->p[1]) : nullptr, *t = e ? m_sk.point(e->p[2]) : nullptr;
    const double turned = o ? std::fmod(std::atan2(t->y - o->y, t->x - o->x) - std::atan2(s->y - o->y, s->x - o->x) + 4 * M_PI, 2 * M_PI) : 0;
    check(o && std::abs(std::hypot(s->x - o->x, s->y - o->y) - 20) < 1e-6 && std::abs(turned - 1.5 * M_PI) < 1e-6, QString("its radius made 20, it still sweeps 270 degrees (%1)").arg(turned * 180 / M_PI));
    send(Qt::Key_Escape);
  });

  // An arc slot by its radius and start angle, then its sweep and (an option of the tool) its width: held by them.
  step([=] {
    tool(0, {}, "arcslot");
    type("100,-120");
    enter();
    type("20");
    tab();
    type("0");
    enter();
    check(m_input->count() == 2 && m_input->key(0) == "sweep" && m_input->key(1) == "width", "the arc slot's boxes after its start: sweep, and its width");
    type("90");
    tab();
    type("4");
    enter();
    const int centre = pointAtXY(100, -120), start = pointAtXY(120, -120), end = pointAtXY(100, -100);
    int outer = 0, cap = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Arc && e.p[0] == centre && at(e.p[1], 122, -120) && at(e.p[2], 100, -98)) outer = e.id;
      else if (e.type == SkEntity::Type::Arc && e.p[0] == start) cap = e.id;
    check(centre && start && end && outer && cap, "a quarter arc slot of radius 20 and width 4 from (120, -120) round to (100, -100), exactly");
    check(has(CT::Distance, {centre, start}, 20) && has(CT::Horizontal, {centre, start}) && has(CT::Vertical, {centre, end}) && has(CT::Diameter, {cap}, 4),
          "held by its radius, its ends along the axes and its width (the start cap's diameter)");
    send(Qt::Key_Escape);
  });

  // The panel's switch off: what is typed places the shape only. Then a tangent arc off that line's end.
  step([=] {
    if (keep) keep->setChecked(false);
    check(!QSettings().value("sketch/input/addDimensions", true).toBool(), "the panel's switch turns it off (saved)");
    tool(Qt::Key_L, "l", "line");
    type("0,-80");
    enter();
    type("10");
    tab();
    type("0");
    enter();
    const int plain = lineBetween(0, -80, 10, -80);
    bool any = false;
    for (const auto& c : m_sk.constraints) any = any || std::find(c.refs.begin(), c.refs.end(), plain) != c.refs.end();
    check(plain && !any, "then a typed line is placed exactly and nothing holds it");
    send(Qt::Key_Escape);
    if (keep) keep->setChecked(true);
    tool(0, {}, "tangent_arc");
    type("10,-80");
    enter();
    check(m_picked.size() == 1 && m_picked[0] == plain && m_input->count() == 2 && m_input->key(0) == "radius" && m_input->key(1) == "sweep",
          "10,-80 Enter picks the line's end; the boxes are the arc's radius and sweep");
    type("5");
    tab();
    type("90");
    check(held(15, -75) && reads("R 5 mm") && reads(QString::fromUtf8("90°")) && primitivePreview().entities.size() == 1,
          "5 Tab 90: a quarter turn of radius 5 off the line's end, previewed " + where());
    enter();
    int arc = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Arc && at(e.p[0], 10, -75) && at(e.p[1], 10, -80) && at(e.p[2], 15, -75)) arc = e.id;
    check(arc && has(CT::Radius, {arc}, 5) && has(CT::Tangent, {plain, arc}), "a tangent arc of radius 5 from (10, -80) to (15, -75), its radius dimensioned");
    // Past half a turn, off the line's other end: on the way the line goes (-X), round its left to three quarters.
    type("0,-80");
    enter();
    type("4");
    tab();
    type("270");
    check(m_input->problem("sweep").isEmpty() && held(4, -84) && reads(QString::fromUtf8("270°")) && primitivePreview().entities.size() == 1,
          "0,-80 Enter 4 Tab 270: three quarters of a turn of radius 4, previewed " + where());
    enter();
    arc = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Arc && at(e.p[0], 0, -84) && at(e.p[1], 0, -80) && at(e.p[2], 4, -84)) arc = e.id;
    check(arc && has(CT::Tangent, {plain, arc}) && m_solved.converged, "a counter-clockwise arc from (0, -80) round to (4, -84), smooth with the line");
    send(Qt::Key_Escape);
  });

  // The sketch fillet's radius from the start: typed before a corner is picked, its arc shown on the hovered corner.
  step([=] {
    tool(0, {}, "fillet");
    check(m_input->count() == 1 && m_input->key(0) == "radius", "the fillet has its radius box before anything is picked");
    type("3");
    check(m_input->isVisible(), "a digit shows it (the pointer not over the view: in its middle)");
    enter();
    check(option("radius") == "3", "3 Enter sets its radius");
    const auto preview = filletPreview(pointAtXY(50, 10));
    check(preview.size() > 2 && std::abs(std::hypot(preview.front().first - preview.back().first, preview.front().second - preview.back().second) - 3 * std::sqrt(2.0)) < 1e-9,
          "the rectangle's corner hovered shows a quarter arc of radius 3");
    sketchMove(50, 10, Qt::NoModifier, false);
    m_viewport->grabImage().save(prefix + ".fillet.png");
    sketchPress(50, 10, Qt::NoModifier);
    sketchRelease(50, 10, Qt::NoModifier);
    int rounded = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Arc && at(e.p[0], 47, 13)) rounded = e.id;
    check(rounded && has(CT::Radius, {rounded}, 3), "a click rounds it by 3");
  });

  // A chamfer by a distance and an angle from the keyboard: the rectangle's top left corner picked, 4<30 Enter.
  step([=] {
    tool(0, {}, "chamfer");
    sketchPress(10, 35, Qt::NoModifier);
    sketchRelease(10, 35, Qt::NoModifier);
    check(m_sel.size() == 1 && m_sel[0] == pointAtXY(10, 35), "a click picks the rectangle's top left corner");
    check(m_input->key(1) == "second" && chip() && chip()->text() == "or angle", "the chamfer's boxes: two distances, the second's switch offers an angle");
    type("4");
    send(Qt::Key_Less, Qt::ShiftModifier, "<");
    type("30");
    check(option("chamferMode") == "angle" && m_input->key(1) == "chamferAngle" && option("first") == "4" && option("chamferAngle") == "30",
          "4<30: the distance 4, then ('<') the angle 30 to the first line");
    m_input->shot().save(prefix + ".chamfer-input.png");
    enter();
  });
  step([=] {
    const double b = 4 * std::sin(30 * degree) / std::sin(120 * degree);
    check(lineBetween(14, 35, 10, 35 - b) && !pointAtXY(10, 35), QString("Enter cuts it 4 along the top and at 30 degrees to it, %1 down the side").arg(b));
    const auto modes = window->findChildren<QComboBox*>("sketchOption-chamferMode");  // the newest: the panel's fields now
    QComboBox* mode = modes.isEmpty() ? nullptr : modes.last();
    check(mode && mode->currentData().toString() == "angle", "the sketch panel shows it chamfers by a distance and an angle");
    if (QToolButton* c = chip()) c->click();
    check(option("chamferMode") == "distance" && m_input->key(1) == "second", "the switch back: two distances");
    send(Qt::Key_Escape);
  });

  // Copies from the keyboard: Shift+C, the Ø20 circle picked, 15<90 Tab 3 Enter: three copies 15 apart straight up.
  step([=] {
    send(Qt::Key_C, Qt::ShiftModifier, "C");
    check(m_tool == "copy" && m_sel.empty(), "Shift+C starts the copy tool");
    sketchPress(10, 60, Qt::NoModifier);
    sketchRelease(10, 60, Qt::NoModifier);
    check(m_sel.size() == 1 && m_sk.entity(m_sel[0]) && m_sk.entity(m_sel[0])->type == SkEntity::Type::Circle, "a click picks the circle");
    check(m_input->count() == 3 && m_input->key(0) == "dx" && m_input->key(1) == "dy" && m_input->key(2) == "copies", "the copy's boxes: ΔX, ΔY and how many");
    type("15");
    send(Qt::Key_Less, Qt::ShiftModifier, "<");
    type("90");
    tab();
    type("3");
    check(option("moveMode") == "polar" && m_input->key(0) == "moveDistance" && option("moveDistance") == "15" && option("moveAngle") == "90" && option("copies") == "3",
          "15<90 Tab 3: a distance of 15 at 90 degrees, three copies");
    m_input->shot().save(prefix + ".copy-input.png");
    enter();
  });
  step([=] {
    int copies = 0;
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Circle && same(e.r, 10) && (at(e.p[0], 0, 75) || at(e.p[0], 0, 90) || at(e.p[0], 0, 105))) ++copies;
    check(copies == 3, "Enter makes three copies, 15, 30 and 45 above it");
    if (QToolButton* c = chip()) c->click();
    check(option("moveMode") == "xy" && m_input->key(0) == "dx" && std::abs(option("dx").remove(" mm").toDouble()) < 1e-9 && option("dy") == "15 mm",
          "the switch back to ΔX and ΔY keeps the offset: " + option("dx") + ", " + option("dy"));
    send(Qt::Key_Escape);
    send(Qt::Key_Escape);
  });

  // Text from the keyboard alone: every printable key goes into its words (L, the line's key, a comma and '@' too), Tab
  // goes on to its height, X and Y, Enter places it there.
  auto before = std::make_shared<std::set<int>>();
  step([=] {
    for (const auto& p : m_sk.points) before->insert(p.id);
    tool(0, {}, "text");
    check(m_input->count() == 4 && m_input->key(0) == "text" && m_input->key(1) == "height" && m_input->key(2) == "x" && m_input->key(3) == "y",
          "the text tool's boxes: its words, its height, X and Y");
    for (const QChar c : QString("Hi, L@1")) {
      const ushort u = c.toUpper().unicode();
      const int key = c.isLetter() ? Qt::Key_A + (u - 'A') : c == ' ' ? Qt::Key_Space : c == ',' ? Qt::Key_Comma : c == '@' ? Qt::Key_At : Qt::Key_0 + c.digitValue();
      send(key, c.isUpper() || c == '@' ? Qt::ShiftModifier : Qt::NoModifier, QString(c));
    }
    check(m_tool == "text" && option("text") == "Hi, L@1", "Hi, L@1 typed goes into the text box as it is, the tool stays: " + option("text"));
    tab();
    type("12");
    tab();
    type("200");
    tab();
    type("-80");
    check(option("height") == "12" && held(200, -80), "Tab 12 Tab 200 Tab -80: 12 high, held at (200, -80) " + where());
    m_input->shot().save(prefix + ".text-input.png");
    enter();
  });
  step([=] {
    double left = 1e9, top = -1e9;
    int added = 0;
    for (const auto& p : m_sk.points)
      if (!before->count(p.id)) ++added, left = std::min(left, p.x), top = std::max(top, p.y);
    check(added > 20 && left > 199.5 && left < 202 && top > -68.5 && top < -66, QString("Enter writes it there: %1 points from x %2 up to y %3").arg(added).arg(left).arg(top));
    // An image's calibration distance is typed like the others.
    setTool("image_calibrate");
    type("25");
    check(m_input->key(0) == "knownDistance" && option("knownDistance") == "25", "image calibration takes its known distance");
    send(Qt::Key_Escape);
    setTool("select");
    m_viewport->grabImage().save(prefix + ".png");
  });

  auto next = std::make_shared<size_t>(0);
  auto* timer = new QTimer(this);
  timer->setInterval(0);
  auto waited = std::make_shared<QElapsedTimer>();
  connect(timer, &QTimer::timeout, this, [this, steps, next, timer, ok, waited, check] {
    if (m_editJob) {  // a change made on a worker (the text's outlines): the next part sees it done
      if (!waited->isValid()) waited->start();
      if (waited->elapsed() < 30000) return;
      check(false, "an edit job finished");
    }
    waited->invalidate();
    if (*next < steps->size()) return (*steps)[(*next)++]();
    timer->stop();
    QCoreApplication::exit(*ok ? 0 : 2);
  });
  timer->start();
}
