#include "SketchEditor.hpp"
#include "Jobs.hpp"
#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QSettings>
#include <QTimer>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_CROSSLOCK=<prefix> (TODO 11 UI-19): cross-locking through the tool code as the mouse and the keyboard
// drive it, with the dwell timer running. Passing over a point does not acquire it, resting on it does; two tracked
// points offer their crossing (A.x, B.y); the user's case: acquire A, follow A's vertical, hold Shift (the lock), rest
// on B while locked (acquired), go back level with B far off the line, click: exactly (A.x, B.y); a Shift tap leaves the
// lock on until Esc (the polyline goes on) or the click; the locked line stops on a circle (and the point lands on it)
// and on grid lines; at most six tracked points, the oldest out; resting again lets one go; a new tool starts afresh.
void SketchEditor::benchCrossLock() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_CROSSLOCK");
  bool ok = true;
  auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sketch crosslock: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  auto exact = [](double a, double b) { return std::abs(a - b) < 1e-9; };
  auto at = [&](int id, double u, double v) { const SkPoint* p = m_sk.point(id); return p && exact(p->x, u) && exact(p->y, v); };
  auto* f9 = m_viewport->window()->findChild<QAction*>("view.gridSnap");
  if (!f9) {
    check(false, "the F9 action exists");
    return QCoreApplication::exit(2);
  }
  QSettings().setValue("view/tracking", true);
  QSettings().setValue("view/extensions", true);
  QSettings().setValue("sketch/snap/angle", true);
  QSettings().setValue("sketch/angleStep", 15);
  f9->setChecked(false);
  // A view about 105 mm across (as the grid bench): a 10 mm grid.
  auto camera = [&](double scale) {
    m_viewport->setCameraJson({{"eye", {0, 0, 100}}, {"target", {0, 0, 0}}, {"up", {0, 1, 0}}, {"scale", scale}, {"projection", "orthographic"}, {"absolute", true}});
  };
  camera(100);
  camera(100 * 105 / (std::max(m_viewport->width(), m_viewport->height()) * m_viewport->pixelSize()));
  const double px = m_viewport->pixelSize();
  trace::log(QString("bench: sketch crosslock: pixel %1 mm, capture %2 mm, grid %3 mm").arg(px).arg(tol()).arg(m_viewport->gridStep()));

  auto rest = [](int ms) {  // the event loop runs meanwhile: the dwell timer fires
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
  };
  auto shot = [&](const char* name) {  // and a turn of the event loop: a grab takes a while
    m_viewport->grabImage().save(prefix + name);
    rest(20);
  };
  auto key = [&](QEvent::Type type, int code, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent e(type, code, mods);
    QApplication::sendEvent(m_viewport, &e);
  };
  auto shift = [&](bool down) { key(down ? QEvent::KeyPress : QEvent::KeyRelease, Qt::Key_Shift, down ? Qt::ShiftModifier : Qt::NoModifier); };
  auto place = [&](double u, double v, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    sketchMove(u, v, mods, false);
    sketchPress(u, v, mods);
    sketchRelease(u, v, mods);
  };
  auto dwell = [&](int id, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    const SkPoint* p = m_sk.point(id);
    sketchMove(p->x + px, p->y, mods, false);
    rest(450);
  };
  using Tracked = std::vector<int>;

  // A and B off the grid, a fourth point D and a circle about C, exactly where they are put (Alt).
  const double ax = -31.7, ay = -12.3, bx = 18.6, by = 23.9;
  setTool("point");
  place(ax, ay, Qt::AltModifier);
  const int a = m_sk.points.back().id;
  place(bx, by, Qt::AltModifier);
  const int b = m_sk.points.back().id;
  place(-15.5, -40.2, Qt::AltModifier);
  const int d = m_sk.points.back().id;
  setTool("circle");
  place(5.3, -20.1, Qt::AltModifier);
  place(5.3 + 9.7, -20.1, Qt::AltModifier);
  const int circle = m_sk.entities.back().id, c = m_sk.entities.back().p[0];
  check(at(a, ax, ay) && at(b, bx, by) && m_sk.entities.back().type == SkEntity::Type::Circle, "A, B and a circle are drawn");

  setTool("line");
  sketchMove(ax + px, ay, Qt::NoModifier, false);  // over A and on at once
  sketchMove(ax + 20, ay + 20, Qt::NoModifier, false);
  rest(450);
  check(m_tracked.empty(), "passing over a point does not acquire it");
  dwell(a);
  check(m_tracked == Tracked{a}, "resting on A acquires it");
  dwell(b);
  check((m_tracked == Tracked{a, b}), "resting on B acquires it too");
  sketchMove(ax + 3 * px, by - 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Cross && exact(m_cursor.u, ax) && exact(m_cursor.v, by) && ((m_cursor.target == a && m_cursor.other == b) || (m_cursor.target == b && m_cursor.other == a)),
        QString("near (A.x, B.y) the pointer lands exactly on the crossing of A's vertical and B's horizontal (%1, %2)").arg(m_cursor.u).arg(m_cursor.v));
  shot(".cross.png");

  // The user's case: a polyline from P0, acquire A, follow its vertical, hold Shift, acquire B while locked, go back level
  // with B far off the line, click.
  setTool("line");
  check(m_tracked.empty(), "a tool started again starts its tracking afresh");
  place(35.2, -38.4, Qt::AltModifier);
  const int p0 = m_chain.back();
  dwell(a);
  check(m_tracked == Tracked{a}, "A acquired");
  sketchMove(ax + 2 * px, ay + 15, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Aligned && m_cursor.target == a && exact(m_cursor.u, ax) && m_cursor.onLine, "the pointer follows A's vertical");
  shift(true);
  check(m_lock && !m_lock->sticky && m_lock->line.anchor == a && m_lock->line.dx == 0, "holding Shift locks onto A's vertical");
  sketchMove(bx + px, by, Qt::ShiftModifier, false);
  check(m_cursor.kind == Snap::Kind::Locked && exact(m_cursor.u, ax) && m_hover.kind == Hit::Point && m_hover.id == b, "on B the pointer stays on the locked line, B under it");
  rest(450);
  check((m_tracked == Tracked{a, b}) && m_lock && m_lock->line.anchor == a, "resting on B acquires it while the lock holds");
  sketchMove(ax + 25, by - 2 * px, Qt::ShiftModifier, false);
  check(m_cursor.kind == Snap::Kind::Locked && m_cursor.other == b && exact(m_cursor.u, ax) && exact(m_cursor.v, by),
        QString("level with B, 25 mm off the line: exactly (A.x, B.y), where B's horizontal crosses it (%1, %2)").arg(m_cursor.u).arg(m_cursor.v));
  check(transientTexts().contains("Locked ∩ tracking"), "named \"Locked ∩ tracking\"");
  shot(".locked.png");
  sketchPress(ax + 25, by - 2 * px, Qt::ShiftModifier);
  sketchRelease(ax + 25, by - 2 * px, Qt::ShiftModifier);
  const int x = m_chain.back();
  check(m_chain.size() == 2 && m_chain.front() == p0 && at(x, ax, by), "the click lands exactly on (A.x, B.y)");
  check(!m_lock, "the click lets go of the lock");
  sketchMove(ax + 25, by + 15, Qt::ShiftModifier, false);
  check(!m_lock, "Shift still down after the click: no new lock until it is pressed again");
  shift(false);

  // A Shift tap on A's horizontal: the lock stays without Shift, until Esc (the polyline goes on).
  sketchMove(ax + 27, ay + 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Aligned && m_cursor.target == a && exact(m_cursor.v, ay), "the pointer follows A's horizontal");
  shift(true);
  shift(false);
  check(m_lock && m_lock->sticky && m_lock->line.anchor == a && m_lock->line.dy == 0 && keyHints().contains("Esc release lock"), "a Shift tap leaves the lock on, the prompt says Esc lets go");
  sketchMove(ax + 26, ay + 10, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Locked && exact(m_cursor.v, ay) && exact(m_cursor.u, ax + 26), "without Shift the pointer stays on A's horizontal");
  key(QEvent::KeyPress, Qt::Key_Escape);
  check(!m_lock && m_tool == "line" && m_chain.size() == 2, "Esc lets go of the lock, the polyline goes on");
  sketchMove(ax + 26, ay + 10, Qt::NoModifier, false);
  check(m_cursor.kind != Snap::Kind::Locked && !exact(m_cursor.v, ay), "and the pointer is free");

  sketchMove(ax + 27, ay + 2 * px, Qt::NoModifier, false);
  shift(true);
  key(QEvent::KeyPress, Qt::Key_F24, Qt::ShiftModifier);  // as Shift with a key that types ('@', a capital)
  shift(false);
  check(!m_lock, "Shift pressed with another key is no tap: no lock stays");

  // Locked again (a tap), the line stops where it crosses the circle: the click lands there, on the circle.
  sketchMove(ax + 27, ay + 2 * px, Qt::NoModifier, false);
  shift(true);
  shift(false);
  const double cx = 5.3 - std::sqrt(9.7 * 9.7 - 7.8 * 7.8);
  sketchMove(cx + 2 * px, ay + 15, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Locked && m_cursor.curve == circle && m_cursor.entity == circle && exact(m_cursor.u, cx) && exact(m_cursor.v, ay),
        QString("the locked line stops where it crosses the circle (%1, %2)").arg(m_cursor.u).arg(m_cursor.v));
  shot(".curve.png");
  place(cx + 2 * px, ay + 15);
  const int y = m_chain.back();
  bool onCircle = false;
  for (const auto& k : m_sk.constraints) onCircle |= k.type == SkConstraint::Type::Coincident && k.refs == std::vector<int>{y, circle};
  check(m_chain.size() == 3 && std::abs(m_sk.point(y)->x - cx) < 1e-6 && std::abs(m_sk.point(y)->y - ay) < 1e-6 && onCircle && !m_lock, "the click lands there, on the circle, and lets go of the lock");

  // With grid snapping the locked line stops on the grid lines it crosses; a tap lets go of a lock that stays.
  f9->trigger();
  sketchMove(ax + 27, ay + 2 * px, Qt::NoModifier, false);
  shift(true);
  shift(false);
  sketchMove(22.4, ay + 9, Qt::NoModifier, false);
  const double step = m_viewport->gridStep();
  check(m_lock && m_cursor.kind == Snap::Kind::Locked && m_cursor.grid && exact(m_cursor.u, sketchsnap::onGrid(22.4, step)) && exact(m_cursor.v, ay),
        QString("on the grid line it crosses (%1, %2; step %3)").arg(m_cursor.u).arg(m_cursor.v).arg(step));
  shift(true);
  shift(false);
  check(!m_lock, "a Shift tap lets go of a lock that stays");
  f9->trigger();

  // At most six tracked points, the oldest out; resting on one again lets it go; a new tool starts afresh.
  setTool("line");
  const int origin = m_sk.points.front().id;
  for (int id : {a, b, c, d, p0, origin}) dwell(id);
  check((m_tracked == Tracked{a, b, c, d, p0, origin}), QString("six points tracked (%1)").arg(m_tracked.size()));
  dwell(x);
  check((m_tracked == Tracked{b, c, d, p0, origin, x}), "a seventh: the oldest (A) goes");
  sketchMove(0, 45, Qt::NoModifier, false);
  shot(".six.png");
  dwell(c);
  check((m_tracked == Tracked{b, d, p0, origin, x}), "resting on a tracked point again lets it go");
  setTool("rect");
  check(m_tracked.empty() && !m_lock, "another tool starts without them");

  // Off any guide, holding Shift locks the way from the shape's last click to the pointer; letting go after a while frees it.
  place(-20, -20, Qt::AltModifier);
  sketchMove(-10, -15, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::None, "the rectangle's corner follows the pointer freely");
  shift(true);
  sketchMove(0, 10, Qt::ShiftModifier, false);
  const double along = (20 * 2 + 30 * 1) / std::sqrt(5.0);
  check(m_lock && m_cursor.kind == Snap::Kind::Locked && exact(m_cursor.u, -20 + along * 2 / std::sqrt(5.0)) && exact(m_cursor.v, -20 + along / std::sqrt(5.0)),
        QString("holding Shift keeps it on the way from the first corner (%1, %2)").arg(m_cursor.u).arg(m_cursor.v));
  rest(350);
  shift(false);
  sketchMove(0, 10, Qt::NoModifier, false);
  check(!m_lock && exact(m_cursor.u, 0) && exact(m_cursor.v, 10), "Shift let go after a hold: free again");
  setTool("select");
  shot(".png");
  QCoreApplication::exit(ok ? 0 : 2);
}
