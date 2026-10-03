#include "SketchEditor.hpp"
#include "SketchSnap.hpp"
#include "Jobs.hpp"
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QTimer>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_GRIDCURSOR=<prefix>: with grid snapping on, the drawing cursor jumps between the grid nodes (AutoCAD's
// SNAP, LibreCAD, Fusion). The user's GIF: a node was marked (OCCT's grid echo, a grey star on the node nearest the
// pointer) while the line started on a curve the pointer was over and the rubber band followed the 75 degree ray off the
// grid. Here the editor draws the cursor at the snapped point and the system pointer is blank over the view while a tool
// takes points; the rubber band, the value boxes and the click use that point, the first click of a line too: beside an
// existing line (away from its ends), along a 75 degree direction from the last point, at offsets all round nodes. An
// endpoint and a midpoint in reach win (the cursor jumps there with their marker). The pointer comes back after Esc
// closes the tool, with snapping off, with Alt, over the view cube and during a camera gesture; a dragged point's cursor
// is on its node (crisp: on whole pixels), a dragged line's under the hand. Then the review's cases: the tools that pick
// curves (Dimension, Move, a constraint) hit-test where the pointer is, not at a node; Alt pressed or let go without a
// move; a point a hair off a node is reused; a move within one cell redraws nothing; the value boxes stay clear of the
// hidden pointer; an overlay entered or made meanwhile; a gesture's release; a drag off the view; every tenth line on
// multiples of ten steps after a pan; a tilted plane; a set spacing zoomed far in.
// Shots: <prefix>.near-line.png, .angle.png, .offsets.png, .endpoint.png, .drag.png.
void SketchEditor::benchGridCursor() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_GRIDCURSOR");
  bool ok = true;
  auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sketch gridcursor: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  auto* f9 = m_viewport->window()->findChild<QAction*>("view.gridSnap");
  if (!f9) {
    check(false, "the F9 action exists");
    return QCoreApplication::exit(2);
  }
  for (const char* key : {"endpoint", "midpoint", "center", "quadrant", "intersection", "apparent", "perpendicular", "tangent", "nearest", "angle", "inference"})
    QSettings().setValue(QString("sketch/snap/") + key, true);
  QSettings().setValue("sketch/angleStep", 15);
  QSettings().setValue("view/tracking", true);
  QSettings().setValue("view/extensions", true);
  QSettings().setValue("view/orthoSnap", false);
  refreshSnap();
  f9->setChecked(false);
  check(!m_viewport->benchGridEcho(), "OCCT's grid echo is off (its star marked a node the click did not use)");

  // 0.11 mm a pixel: a 5 mm grid, 45 px a step.
  auto camera = [&](double scale) {
    m_viewport->setCameraJson({{"eye", {0, 0, 100}}, {"target", {0, 0, 0}}, {"up", {0, 1, 0}}, {"scale", scale}, {"projection", "orthographic"}, {"absolute", true}});
  };
  camera(100);
  camera(100 * 0.11 / m_viewport->pixelSize());
  auto rest = [](int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
  };
  auto shot = [&](const char* name) {
    m_viewport->grabImage().save(prefix + name);
    rest(20);
  };
  m_viewport->grabImage();  // a frame: the grid laid out for this zoom, the cube pickable
  const double s = m_viewport->gridStep(), px = m_viewport->pixelSize();
  trace::log(QString("bench: sketch gridcursor: step %1 mm = %2 px, capture %3 px").arg(s).arg(s / px).arg(tol() / px));
  if (!(s / px >= 24 && s / px < 60)) {
    check(false, QString("a grid step of %1 px (24 to 60 wanted)").arg(s / px));
    return QCoreApplication::exit(2);
  }
  auto same = [&](double a, double b) { return std::abs(a - b) < 1e-9 * s; };
  auto on = [&](double x) { return std::abs(x / s - std::round(x / s)) < 1e-9; };  // on a grid line
  auto node = [&](double u, double v) { return on(u) && on(v); };
  auto at = [&](int id, double u, double v) { const SkPoint* p = m_sk.point(id); return p && same(p->x, u) && same(p->y, v); };
  // The system pointer is blank over the view (the widget's cursor), and the editor draws its own: 4 strokes, at (u, v).
  auto blank = [&] { return m_viewport->ownCursor() && m_viewport->testAttribute(Qt::WA_SetCursor) && m_viewport->cursor().shape() == Qt::BlankCursor; };
  auto pointer = [&] { return !m_viewport->ownCursor() && !m_viewport->testAttribute(Qt::WA_SetCursor) && !m_drawnCursor && transientCursor() == 0; };
  auto drawn = [&](double u, double v) { return blank() && m_drawnCursor && same(m_drawnCursor->first, u) && same(m_drawnCursor->second, v) && transientCursor() == 4; };
  auto where = [&] {
    return m_drawnCursor ? QString("cursor (%1, %2) steps").arg(m_drawnCursor->first / s, 0, 'g', 12).arg(m_drawnCursor->second / s, 0, 'g', 12) : QString("no cursor drawn");
  };
  auto move = [&](double u, double v, Qt::KeyboardModifiers mods = Qt::NoModifier) { sketchMove(u, v, mods, false); };
  auto click = [&](double u, double v, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    sketchMove(u, v, mods, false);
    sketchPress(u, v, mods);
    sketchRelease(u, v, mods);
  };
  auto key = [&](int k) {
    QKeyEvent press(QEvent::KeyPress, k, Qt::NoModifier);
    QApplication::sendEvent(m_viewport, &press);
  };
  // A line from the origin to (7.3, 2.1) steps, off the grid, as in the GIF; its first click 11 px from the origin lands on
  // the node there, which is the origin point.
  f9->setChecked(true);
  setTool("line");
  move(2.3 * s, 3.4 * s);
  check(drawn(2 * s, 3 * s), "the line tool with grid snapping: the pointer is blank over the view, the cursor drawn on the nearest node (" + where() + ")");
  check(cursorCrisp(), "its arms on whole device pixels");
  click(0.2 * s, -0.15 * s);
  check(m_chain.size() == 1 && m_chain.front() == m_sk.points.front().id, "a click 11 px from the origin starts on it (the node there is the origin point)");
  click(7.3 * s, 2.1 * s, Qt::AltModifier);
  const SkEntity* line = m_sk.entities.empty() ? nullptr : &m_sk.entities.back();
  const int end = line && line->p.size() == 2 ? line->p[1] : 0;
  check(line && line->type == SkEntity::Type::Line && at(end, 7.3 * s, 2.1 * s), "with Alt the line ends off the grid, where the pointer is");
  check(pointer(), "with Alt the pointer is the system's, no cursor drawn");
  const int lineId = line ? line->id : 0;
  done();

  // The GIF: a new line started beside that line, near its middle, 3 px off: the node 16 px away, not the line.
  const double k = 0.46, nu = 7.3 * k * s, nv = 2.1 * k * s, len = std::hypot(7.3, 2.1);
  const double bu = nu - 3 * px * 2.1 / len, bv = nv + 3 * px * 7.3 / len;
  move(bu, bv);
  check(std::hypot(bu - 3 * s, bv - s) > tol() && m_cursor.kind != Snap::Kind::Curve && node(m_cursor.u, m_cursor.v) && same(m_cursor.u, 3 * s) && same(m_cursor.v, s) && drawn(3 * s, s),
        "3 px from the line, away from its ends: the cursor on node (3, 1), not on the line (" + where() + ")");
  check(m_hover.kind == Hit::Entity && transientSolid(m_viewport->tokens().hov.lighter(115)) == 0, "the line under the pointer is not lit up (the click does not use it)");
  shot(".near-line.png");
  click(bu, bv);
  bool onLine = false;
  for (const auto& c : m_sk.constraints) onLine |= c.type == SkConstraint::Type::Coincident && std::find(c.refs.begin(), c.refs.end(), lineId) != c.refs.end();
  check(m_chain.size() == 1 && at(m_chain.back(), 3 * s, s) && !onLine, "the line's first point is that node, not on the line");

  // 2.6 steps from (3, 1) at 75.4 degrees: the 15 degree rays hold that pointer, the cursor is on node (4, 4) all the same.
  const double a = 75.4 * M_PI / 180, ru = 3 * s + 2.6 * s * std::cos(a), rv = s + 2.6 * s * std::sin(a);
  sketchsnap::Guide ray;
  check(sketchsnap::angleRay(3 * s, s, ru, rv, 15 * M_PI / 180, tol(), ray), "the 75 degree ray holds the pointer");
  move(ru, rv);
  bool band = false;  // the rubber band's length readout runs to the cursor
  for (const auto& r : readouts()) band |= r.key == "length" && same(r.tu, 4 * s) && same(r.tv, 4 * s) && same(r.fu, 3 * s) && same(r.fv, s);
  check(m_cursor.kind != Snap::Kind::Angle && same(m_cursor.u, 4 * s) && same(m_cursor.v, 4 * s) && drawn(4 * s, 4 * s) && band,
        "at 75 degrees between nodes: the cursor and the rubber band's end on node (4, 4), the angle only read out (" + where() + ")");
  shot(".angle.png");
  click(ru, rv);
  bool angled = false;
  for (const auto& c : m_sk.constraints) angled |= c.type == SkConstraint::Type::Angle || c.type == SkConstraint::Type::Horizontal || c.type == SkConstraint::Type::Vertical;
  check(m_chain.size() == 2 && at(m_chain.back(), 4 * s, 4 * s) && !angled, "the click lands on that node, unconstrained");

  // Offsets all round nodes on the left (nothing else in reach there): the cursor and every click on the nearest node.
  struct Probe { double nu, nv, du, dv; };
  const Probe probes[] = {{-2, 3, 0.3, 0.2}, {-5, 3, -0.45, 0.1}, {-5, 5, 0.49, -0.49}, {-3, 5, -0.2, -0.4}, {-2, 4, 0.05, 0.45}, {-4, 2, -0.38, -0.33}};
  bool all = true;
  for (const auto& p : probes) {
    const double u = (p.nu + p.du) * s, v = (p.nv + p.dv) * s;
    move(u, v);
    const bool cursor = m_cursor.grid && same(m_cursor.u, p.nu * s) && same(m_cursor.v, p.nv * s) && drawn(p.nu * s, p.nv * s);
    if (p.nu == -3) shot(".offsets.png");
    click(u, v);
    const bool placed = at(m_chain.back(), p.nu * s, p.nv * s);
    if (!cursor || !placed) trace::log(QString("bench: sketch gridcursor: offset (%1, %2) from node (%3, %4): %5, placed %6").arg(p.du).arg(p.dv).arg(p.nu).arg(p.nv).arg(where()).arg(placed));
    all = all && cursor && placed;
  }
  check(all, "offsets up to half a step all round six nodes: the cursor on the node, the click there");

  // An object snap in reach wins over the grid: the cursor jumps to the line's end (off the grid) with the endpoint's
  // marker, and to its middle with the midpoint's.
  move(3.65 * s + 2 * px, 1.05 * s - 3 * px);
  check(m_cursor.kind == Snap::Kind::Midpoint && drawn(3.65 * s, 1.05 * s) && m_marker == snapmarkers::Marker::Midpoint,
        "within reach of the line's middle: the cursor there with the midpoint's marker (" + where() + ")");
  move(7.3 * s + 3 * px, 2.1 * s - 2 * px);
  check(m_cursor.kind == Snap::Kind::Point && m_cursor.point == end && !node(m_cursor.u, m_cursor.v) && drawn(7.3 * s, 2.1 * s) && m_marker == snapmarkers::Marker::Endpoint,
        "within reach of the line's end: the cursor jumps there (off the grid) with the endpoint's marker (" + where() + ")");
  shot(".endpoint.png");
  click(7.3 * s + 3 * px, 2.1 * s - 2 * px);
  check(m_chain.back() == end, "the click ends on that endpoint");

  // The camera's gestures and the view cube take the system pointer back; the next plain move over the sketch hides it.
  auto mouse = [&](QEvent::Type type, const QPointF& at, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent e(type, at, m_viewport->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(m_viewport, &e);
  };
  const QPointF mid(m_viewport->width() * 0.4, m_viewport->height() * 0.55);
  mouse(QEvent::MouseMove, mid, Qt::NoButton, Qt::NoButton);
  check(blank() && m_drawnCursor && node(m_drawnCursor->first, m_drawnCursor->second), "a move over the view: blank, the cursor on a node (" + where() + ")");
  QPointF cube;
  for (int y = 10; y < 260 && cube.isNull(); y += 6)
    for (int x = m_viewport->width() - 10; x > m_viewport->width() - 260 && cube.isNull(); x -= 6)
      if (m_viewport->cubeAt(QPointF(x, y))) cube = QPointF(x, y);
  if (cube.isNull()) trace::log("bench: sketch gridcursor: the view cube was not found (no frame?): its check left out");
  else {
    mouse(QEvent::MouseMove, cube, Qt::NoButton, Qt::NoButton);
    check(pointer(), "over the view cube: the system pointer, no cursor drawn");
    mouse(QEvent::MouseMove, mid, Qt::NoButton, Qt::NoButton);
    check(blank() && m_drawnCursor, "back over the sketch: blank again, the cursor drawn");
  }
  mouse(QEvent::MouseButtonPress, mid, Qt::MiddleButton, Qt::MiddleButton);
  check(pointer(), "a middle press (a camera gesture): the system pointer");
  mouse(QEvent::MouseMove, mid + QPointF(30, 10), Qt::NoButton, Qt::MiddleButton);
  check(pointer(), "panning: still the system pointer");
  mouse(QEvent::MouseButtonRelease, mid + QPointF(30, 10), Qt::MiddleButton, Qt::NoButton);
  mouse(QEvent::MouseMove, mid + QPointF(31, 10), Qt::NoButton, Qt::NoButton);
  check(blank() && m_drawnCursor && node(m_drawnCursor->first, m_drawnCursor->second), "the next plain move: blank, the cursor on a node (" + where() + ")");
  sketchLeave();
  check(!m_drawnCursor && transientCursor() == 0, "off the view: no cursor drawn (the widget's blank cursor is not shown there)");

  // F9 off: the system pointer, no cursor drawn, the point free; on again: the cursor.
  move(-1.3 * s, 2.7 * s);
  f9->setChecked(false);
  check(pointer() && same(m_pointer.u, -1.3 * s) && same(m_pointer.v, 2.7 * s), "F9 off: the system pointer, the point where it is");
  f9->setChecked(true);
  check(drawn(-s, 3 * s), "F9 on again: blank, the cursor on node (-1, 3) at once (" + where() + ")");

  // Esc ends the polyline, then closes the tool: the system pointer again.
  key(Qt::Key_Escape);
  check(m_tool == "line" && m_chain.empty() && blank(), "Esc ends the polyline, the tool stays: still blank");
  key(Qt::Key_Escape);
  check(m_tool == "select" && pointer(), "Esc again closes the tool: the system pointer is back, no cursor drawn");

  // Dragging a point with the select tool: its node is the cursor while it moves, the pointer back on the drop.
  int free = 0;
  for (const auto& p : m_sk.points)
    if (at(p.id, -2 * s, 3 * s)) free = p.id;
  move(-2 * s, 3 * s);
  check(pointer(), "hovering with the select tool: the system pointer");
  sketchPress(-2 * s, 3 * s, Qt::NoModifier);
  sketchMove(-2 * s + 0.6 * s, 3 * s + 0.2 * s, Qt::NoModifier, true);
  sketchMove(-2 * s + 1.37 * s, 3 * s + 0.39 * s, Qt::NoModifier, true);
  check(free && at(free, -s, 3 * s) && drawn(-s, 3 * s), "dragging a point: it and the cursor on node (-1, 3), the pointer blank (" + where() + ")");
  check(cursorCrisp(), "the cursor's arms on whole device pixels (crisp wherever the node falls)");
  shot(".drag.png");
  sketchRelease(-2 * s + 1.37 * s, 3 * s + 0.39 * s, Qt::NoModifier);
  check(pointer() && at(free, -s, 3 * s), "dropped on the node: the system pointer again");

  // A line dragged by its middle: the cursor stays under the hand, moved by whole steps (its start, the origin, is where
  // the grid takes the line from, 160 px away).
  sketchPress(3.65 * s, 1.05 * s, Qt::NoModifier);
  const bool grabbed = m_dragHit.kind == Hit::Entity && m_dragHit.id == lineId;
  sketchMove(3.65 * s + 1.3 * s, 1.05 * s + 0.6 * s, Qt::NoModifier, true);
  sketchMove(3.65 * s + 2.2 * s, 1.05 * s + 1.1 * s, Qt::NoModifier, true);
  check(grabbed && blank() && m_drawnCursor && same(m_drawnCursor->first, 5.65 * s) && same(m_drawnCursor->second, 2.05 * s),
        "dragging a line by its middle: the cursor under the hand by whole steps, not at the line's start (" + where() + ")");
  sketchRelease(3.65 * s + 2.2 * s, 1.05 * s + 1.1 * s, Qt::NoModifier);
  undo();

  // The tools that pick curves hit-test where the pointer is, grid snapping or not: an off-grid line, picked between nodes
  // (node (-5, -5) is 14 px off the pointer and 13 px off the line).
  setTool("line");
  click(-6.3 * s, -5.2 * s, Qt::AltModifier);
  click(-3.6 * s, -4.15 * s, Qt::AltModifier);
  const int slanted = m_sk.entities.empty() ? 0 : m_sk.entities.back().id;
  done();
  const double pu = -5.22 * s, pv = -4.78 * s;
  setTool("dimension");
  move(pu, pv);
  check(pointer() && !node(m_cursor.u, m_cursor.v), "the Dimension tool with grid snapping: the system pointer, no node taken (a pick is where the pointer is)");
  click(pu, pv);
  check(m_picked.size() == 1 && m_picked.front() == slanted && m_placingDim, "its click on a line between nodes picks that line");
  setTool("move");
  m_sel.clear();
  click(pu, pv);
  check(std::find(m_sel.begin(), m_sel.end(), slanted) != m_sel.end(), "the Move tool's click there selects that line");
  m_sel.clear();
  setTool("c:horizontal");
  click(pu, pv);
  bool level = false;
  for (const auto& c : m_sk.constraints) level |= c.type == SkConstraint::Type::Horizontal && c.refs.size() == 1 && c.refs.front() == slanted;
  check(level, "a Horizontal constraint's click there holds that line");

  // Alt pressed or let go with the mouse still: the cursor drawn (or the pointer shown) and the next click agree at once.
  setTool("line");
  move(-3.3 * s, 6.2 * s);
  const bool before = drawn(-3 * s, 6 * s);
  QKeyEvent altDown(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
  QApplication::sendEvent(m_viewport, &altDown);
  check(before && pointer() && same(m_pointer.u, -3.3 * s) && same(m_pointer.v, 6.2 * s), "Alt pressed with the mouse still: the system pointer, the point where it is");
  sketchPress(-3.3 * s, 6.2 * s, Qt::AltModifier);
  sketchRelease(-3.3 * s, 6.2 * s, Qt::AltModifier);
  check(m_chain.size() == 1 && at(m_chain.back(), -3.3 * s, 6.2 * s), "a click then starts where the pointer is");
  QKeyEvent altUp(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
  QApplication::sendEvent(m_viewport, &altUp);
  check(drawn(-3 * s, 6 * s), "Alt let go with the mouse still: blank, the cursor on node (-3, 6) at once (" + where() + ")");
  sketchPress(-3.3 * s, 6.2 * s, Qt::NoModifier);
  sketchRelease(-3.3 * s, 6.2 * s, Qt::NoModifier);
  check(m_chain.size() == 2 && at(m_chain.back(), -3 * s, 6 * s), "a click then lands on that node");
  done();

  // A point 0.7 px off node (-6, 6), the hidden pointer 22 px from it (out of the endpoint snap's reach) but in the node's
  // cell: the cursor on that point, and the click starts there, never on a second point beside it.
  const int hairy = m_sk.add_point(-6 * s + 0.6 * px, 6 * s - 0.4 * px);
  m_solved = solve(m_sk);
  rebuild();
  move(-5.62 * s, 5.7 * s);
  check(m_cursor.point == hairy && m_cursor.kind == Snap::Kind::Point && drawn(-6 * s + 0.6 * px, 6 * s - 0.4 * px),
        "a point a hair off the node: the cursor on it, as its snap (" + where() + ")");
  click(-5.62 * s, 5.7 * s);
  check(m_chain.size() == 1 && m_chain.front() == hairy, "the click starts on that point, no second point beside it");
  done();

  // The same picture is not redisplayed (no frame for a move within one cell); the next cell is.
  move(-4.2 * s, 7.1 * s);
  const size_t shown = m_transientRedisplays;
  move(-4.1 * s, 7.15 * s);
  const bool kept = m_transientRedisplays == shown && drawn(-4 * s, 7 * s);
  move(-4.6 * s, 7.1 * s);
  check(kept && m_transientRedisplays > shown && drawn(-5 * s, 7 * s), "a move within one cell: not redisplayed; into the next: redisplayed, the cursor on node (-5, 7)");

  // The value boxes beside the drawn cursor never come under the hidden pointer (up to half a step off it all round).
  bool clear = m_input->count() > 0;
  for (const auto& [du, dv] : {std::pair{0.45, -0.45}, std::pair{0.45, 0.45}, std::pair{-0.45, -0.45}, std::pair{-0.45, 0.45}}) {
    const double u = (-4 + du) * s, v = (7 + dv) * s;
    move(u, v);
    const QPoint hand = m_viewport->widgetPoint(m_frame.to_world(u, v));
    if (m_input->boxesRect().adjusted(-4, -4, 4, 4).contains(hand)) {
      trace::log(QString("bench: sketch gridcursor: the hidden pointer (%1, %2) px is over the boxes").arg(hand.x()).arg(hand.y()));
      clear = false;
    }
  }
  check(clear, "the value boxes stay clear of the hidden pointer half a step off the cursor, all round");

  // Onto an overlay on the view (Qt sends the view no Leave): the drawn cursor goes; a move over the sketch brings it back.
  move(-4.2 * s, 7.1 * s);
  QEnterEvent enter(QPointF(4, 4), QPointF(4, 4), m_viewport->mapToGlobal(QPointF(4, 4)));
  QApplication::sendEvent(m_input, &enter);
  const bool gone = !m_drawnCursor && transientCursor() == 0;
  move(-4.2 * s, 7.1 * s);
  check(gone && drawn(-4 * s, 7 * s), "onto a value box: the drawn cursor goes; back over the sketch: drawn again");
  // An overlay made while the pointer is blank (a toast) keeps the arrow.
  auto* late = new QWidget(m_viewport);
  late->resize(20, 20);
  late->ensurePolished();
  check(blank() && late->testAttribute(Qt::WA_SetCursor) && late->cursor().shape() == Qt::ArrowCursor, "an overlay made while the pointer is blank keeps the arrow");
  delete late;
  // A camera gesture ended: blank again at once (a click right after it goes where the cursor is drawn).
  mouse(QEvent::MouseButtonPress, mid, Qt::MiddleButton, Qt::MiddleButton);
  const bool aside = pointer();
  mouse(QEvent::MouseButtonRelease, mid, Qt::MiddleButton, Qt::NoButton);
  check(aside && blank(), "a middle press: the system pointer; released: blank again without a move");

  // Dragging a point off the view: the system pointer there (the press holds the mouse, the view's blank cursor would stay
  // over the ribbon and the panels), blank again back over the view.
  setTool("select");
  const QPointF grab = QPointF(m_viewport->widgetPoint(m_frame.to_world(-6 * s, 6 * s)));
  mouse(QEvent::MouseButtonPress, grab, Qt::LeftButton, Qt::LeftButton);
  mouse(QEvent::MouseMove, grab + QPointF(30, 20), Qt::NoButton, Qt::LeftButton);
  const bool inside = m_tool == "select" && m_dragging && blank() && m_drawnCursor;
  mouse(QEvent::MouseMove, QPointF(-25, grab.y()), Qt::NoButton, Qt::LeftButton);
  const bool outside = pointer();
  mouse(QEvent::MouseMove, grab + QPointF(60, 20), Qt::NoButton, Qt::LeftButton);
  const bool back = blank() && m_drawnCursor;
  mouse(QEvent::MouseButtonRelease, grab + QPointF(60, 20), Qt::LeftButton, Qt::NoButton);
  check(inside && outside && back && pointer(), QString("dragging a point off the view and back: blank %1, the system pointer off it %2, blank back %3, the pointer after the drop").arg(int(inside)).arg(int(outside)).arg(int(back)));
  undo();

  // Every tenth line darker on multiples of ten steps, through the sketch's axes, wherever the view is.
  const double sc = 100 * 0.11 / [&] { camera(100); return m_viewport->pixelSize(); }();
  m_viewport->setCameraJson({{"eye", {37.4 * s, 13.6 * s, 100}}, {"target", {37.4 * s, 13.6 * s, 0}}, {"up", {0, 1, 0}}, {"scale", sc}, {"projection", "orthographic"}, {"absolute", true}});
  m_viewport->grabImage();
  {
    const QPointF o = m_viewport->benchGridOrigin();
    const double major = 10 * m_viewport->gridShownStep();
    check(major > 0 && std::abs(o.x() / major - std::round(o.x() / major)) < 1e-9 && std::abs(o.y() / major - std::round(o.y() / major)) < 1e-9,
          QString("panned to (37.4, 13.6) steps: the grid laid out from (%1, %2) mm, on multiples of ten steps (%3 mm)").arg(o.x()).arg(o.y()).arg(major));
  }
  // A plane tilted 60 degrees from the screen: along the tilt a step is still 24 px at least.
  const double c60 = 0.5, s60 = std::sqrt(3.0) / 2;
  m_viewport->setCameraJson({{"eye", {0, -100 * s60, 100 * c60}}, {"target", {0, 0, 0}}, {"up", {0, c60, s60}}, {"scale", sc}, {"projection", "orthographic"}, {"absolute", true}});
  m_viewport->grabImage();
  {
    const double along = m_viewport->gridStep() * c60 / m_viewport->pixelSize();
    check(along >= 24 * (1 - 1e-6) && along < 61, QString("a plane tilted 60 degrees: a step of %1 px along the tilt (24 to 60)").arg(along));
  }
  // A set 10 mm spacing zoomed far in (0.005 mm a pixel): fractions of it 24 to 60 px apart, not 2000 px.
  m_viewport->configureGrid(10, 100);
  camera(100);
  camera(100 * 0.005 / m_viewport->pixelSize());
  m_viewport->grabImage();
  {
    const double step = m_viewport->gridStep(), on = step / m_viewport->pixelSize(), parts = 10 / step;
    check(on >= 24 * (1 - 1e-9) && on <= 60 * (1 + 1e-9) && std::abs(parts - std::round(parts)) < 1e-9 && m_viewport->gridShownStep() == step,
          QString("a set 10 mm spacing zoomed in to 0.005 mm a pixel: a step of %1 mm = %2 px, a whole fraction of it, as drawn").arg(step).arg(on));
  }
  m_viewport->configureGrid(0, 100);
  QCoreApplication::exit(ok ? 0 : 2);
}
