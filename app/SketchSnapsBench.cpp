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

// OPAD_BENCH_SKETCH_SNAPS=<prefix> (TODO 11 UI-23, UI-21): snapping through the tool code as the mouse drives it. Each
// object snap shows a marker of its own shape where the pointer lands (square endpoint, triangle midpoint, circle centre,
// diamond quadrant, cross intersection, hourglass on a curve, # grid, plus at a tracked point); a click keeps what it
// snapped to as constraints (a midpoint, both curves, a quadrant above its centre, level with a tracked point), shown
// before as pictograms beside the pointer, variant primitives too; perpendicular, tangent and apparent-intersection snaps;
// Shift taps go through the snaps in reach; an extension's marker turned along its line, a lock's line thick dashed; slots,
// polygons and ellipses infer horizontal, vertical and angles; Ortho F8.
void SketchEditor::benchSnaps() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_SNAPS");
  bool ok = true;
  auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sketch snaps: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  auto exact = [](double a, double b) { return std::abs(a - b) < 1e-9; };
  auto* f9 = m_viewport->window()->findChild<QAction*>("view.gridSnap");
  if (!f9) {
    check(false, "the F9 action exists");
    return QCoreApplication::exit(2);
  }
  for (const char* key : {"endpoint", "midpoint", "center", "quadrant", "intersection", "nearest", "angle", "inference"}) QSettings().setValue(QString("sketch/snap/") + key, true);
  QSettings().setValue("view/tracking", true);
  QSettings().setValue("view/extensions", true);
  f9->setChecked(false);
  auto camera = [&](double scale) {
    m_viewport->setCameraJson({{"eye", {0, 0, 100}}, {"target", {0, 0, 0}}, {"up", {0, 1, 0}}, {"scale", scale}, {"projection", "orthographic"}, {"absolute", true}});
  };
  camera(100);
  camera(100 * 105 / (std::max(m_viewport->width(), m_viewport->height()) * m_viewport->pixelSize()));
  const double px = m_viewport->pixelSize();
  trace::log(QString("bench: sketch snaps: pixel %1 mm, capture %2 mm").arg(px).arg(tol()));
  auto rest = [](int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
  };
  auto shot = [&](const char* name) {
    m_viewport->grabImage().save(prefix + name);
    rest(20);
  };
  auto place = [&](double u, double v, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    sketchMove(u, v, mods, false);
    sketchPress(u, v, mods);
    sketchRelease(u, v, mods);
  };
  using M = snapmarkers::Marker;
  auto shows = [&](M marker) { return m_marker && *m_marker == marker; };
  auto where = [&] {  // what the pointer snapped to, for a failure
    return QString(" [%1: kind %2 at (%3, %4), %5 of %6, h %7 v %8]").arg(m_tool).arg(int(m_cursor.kind)).arg(m_cursor.u, 0, 'g', 10).arg(m_cursor.v, 0, 'g', 10).arg(m_cursor.choice).arg(m_cursor.choices).arg(m_cursor.horizontal).arg(m_cursor.vertical);
  };

  // L: (-30, -20) -> (10, -20); N: (0, -30) -> (0, -5) crossing it at (0, -20); a circle C about (25, 15), radius 10.
  setTool("line");
  place(-30, -20, Qt::AltModifier);
  place(10, -20, Qt::AltModifier);
  finishChain();
  const int l = m_sk.entities.back().id, l0 = m_sk.entity(l)->p[0];
  place(0, -30, Qt::AltModifier);
  place(0, -5, Qt::AltModifier);
  finishChain();
  const int nl = m_sk.entities.back().id;
  setTool("circle");
  place(25, 15, Qt::AltModifier);
  place(35, 15, Qt::AltModifier);
  const int c = m_sk.entities.back().id, centre = m_sk.entity(c)->p[0];
  check(m_sk.entities.size() == 3 && m_sk.entity(c)->type == SkEntity::Type::Circle, "two lines and a circle are drawn");

  // Markers (UI-23): each object snap by its shape.
  setTool("point");
  sketchMove(-30 + 2 * px, -20 + px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Point && m_cursor.point == l0 && shows(M::Endpoint), "an endpoint: the square");
  sketchMove(-10 + 2 * px, -20 - px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Midpoint && exact(m_cursor.u, -10) && shows(M::Midpoint), "a midpoint: the triangle");
  shot(".midpoint.png");
  sketchMove(25 + px, 15 + px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Point && m_cursor.point == centre && shows(M::Centre), "a centre: the circle");
  sketchMove(25 + px, 25 - 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Quadrant && exact(m_cursor.u, 25) && exact(m_cursor.v, 25) && shows(M::Quadrant), "a quadrant: the diamond");
  sketchMove(2 * px, -20 + 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Intersection && std::abs(m_cursor.u) < 1e-9 && std::abs(m_cursor.v + 20) < 1e-9 && shows(M::Intersection), "an intersection: the cross");
  shot(".intersection.png");
  sketchMove(-22, -20 + 3 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Curve && exact(m_cursor.v, -20) && shows(M::Nearest), "on a curve: the hourglass");
  sketchMove(-40.2, 30.3, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::None && !m_marker, "nothing near: a dot, no marker");
  f9->setChecked(true);
  const double step = m_viewport->gridStep();
  sketchMove(-40 + 0.2 * step, 30 + 0.2 * step, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Grid && shows(M::Grid), "a grid node: the grid marker");
  f9->setChecked(false);
  // A tracked point (resting on it) draws the plus; following its horizontal is tracking (the plus at the pointer).
  sketchMove(-30 + px, -20, Qt::NoModifier, false);
  rest(450);
  check(m_tracked == std::vector<int>{l0}, "resting on L's start tracks it");
  sketchMove(-45, -20 + 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Aligned && shows(M::Tracking), "on its horizontal: tracking, the plus");
  shot(".tracking.png");

  // Snaps create constraints (UI-21); the pictograms beside the pointer say which before the click.
  using G = snapmarkers::Glyph;
  using CT = SkConstraint::Type;
  auto has = [&](CT type, std::vector<int> refs) {
    return std::any_of(m_sk.constraints.begin(), m_sk.constraints.end(), [&](const SkConstraint& k) { return k.type == type && k.refs == refs; });
  };
  auto solved = [&] { return m_solved.converged && m_undo.size() > 0; };
  auto newPoint = [&] { return m_sk.entities.back().p.front(); };  // the point tool's newest point
  check(m_glyphs == std::vector<G>{G::Horizontal}, "level with the tracked point: the horizontal pictogram");
  place(-45, -20 + 2 * px);
  const int level = newPoint();
  check(has(CT::Horizontal, {level, l0}) && exact(m_sk.point(level)->y, -20), "the click keeps it level with the tracked point (horizontal between the two)");
  sketchMove(25 + px, 15, Qt::NoModifier, false);
  rest(450);
  check((m_tracked == std::vector<int>{l0, centre}), "the circle's centre tracked too");
  sketchMove(25 + 2 * px, -20 + 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Cross && (m_glyphs == std::vector<G>{G::Horizontal, G::Vertical}), "where L.start's horizontal crosses the centre's vertical: both pictograms");
  place(25 + 2 * px, -20 + 2 * px);
  const int cross = newPoint();
  check(has(CT::Horizontal, {cross, l0}) && has(CT::Vertical, {cross, centre}) && solved(), "the click keeps both: level with L.start, above the centre");
  sketchMove(25 + px, 25 - 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Quadrant && (m_glyphs == std::vector<G>{G::OnCurve, G::Vertical}), "a quadrant: on the circle, above its centre");
  shot(".glyphs.png");
  place(25 + px, 25 - 2 * px);
  const int quadrant = newPoint();
  check(has(CT::Coincident, {quadrant, c}) && has(CT::Vertical, {quadrant, centre}), "the click keeps it on the circle and above the centre");
  sketchMove(2 * px, -20 + 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Intersection && (m_glyphs == std::vector<G>{G::OnCurve}), "an intersection: on the curves");
  place(2 * px, -20 + 2 * px);
  const int meet = newPoint();
  check(has(CT::Coincident, {meet, l}) && has(CT::Coincident, {meet, nl}), "the click keeps it on both lines");
  sketchMove(-30 + px, -20 + px, Qt::NoModifier, false);
  check(m_cursor.point == l0 && (m_glyphs == std::vector<G>{G::Coincident}), "onto a point: coincident (the point itself is used)");
  setTool("line");
  sketchMove(-10 + 2 * px, -20 - px, Qt::NoModifier, false);
  check(m_glyphs == std::vector<G>{G::Midpoint}, "a line started at a midpoint: the midpoint pictogram");
  place(-10 + 2 * px, -20 - px);
  const int middle = m_chain.back();
  check(has(CT::Midpoint, {middle, l}) && exact(m_sk.point(middle)->x, -10), "the line's start keeps to L's middle");
  place(-10, -2, Qt::AltModifier);
  finishChain();
  // Variant primitives keep the points their picks snapped to (SketchPrimitives, create_primitive's snaps).
  setTool("rect3");
  sketchMove(-30 + px, -20 + px, Qt::NoModifier, false);
  check(m_cursor.point == l0 && (m_glyphs == std::vector<G>{G::Coincident}), "a 3-point rectangle from L.start: coincident");
  place(-30 + px, -20 + px);
  place(-30, -40, Qt::AltModifier);
  place(-42, -40, Qt::AltModifier);
  const SkEntity* base = m_sk.entity(m_sk.entities[m_sk.entities.size() - 4].id);
  check(base && base->type == SkEntity::Type::Line && base->p.front() == l0, "its first corner is L.start itself, not a copy beside it");
  setTool("circle2");
  sketchMove(-10 + px, -20 + px, Qt::NoModifier, false);
  check(m_cursor.point == middle && (m_glyphs == std::vector<G>{G::Coincident}), "a 2-point circle across the midpoint point: coincident");
  place(-10 + px, -20 + px);
  place(2 * px, -20 + 2 * px);
  const int across = m_sk.entities.back().id;
  check(m_sk.entity(across)->type == SkEntity::Type::Circle && has(CT::Coincident, {middle, across}) && has(CT::Coincident, {meet, across}) && solved(),
        "the circle passes through both points it snapped to");
  setTool("crect");
  place(45, -30, Qt::AltModifier);
  place(25 + px, 15 + px);
  bool corner = false;
  for (size_t i = m_sk.entities.size() - 5; i < m_sk.entities.size(); ++i) corner |= m_sk.entities[i].type == SkEntity::Type::Line && !m_sk.entities[i].construction && std::count(m_sk.entities[i].p.begin(), m_sk.entities[i].p.end(), centre);
  check(corner && solved(), "a centre rectangle's clicked corner is the circle's centre it snapped to");

  // UI-23: perpendicular, tangent and apparent-intersection snaps, Shift taps through the snaps in reach, the horizontal,
  // vertical and angle inference of every tool, Ortho (F8). A fresh part of the sketch, 100 mm up.
  const double y0 = 100;
  const opad::json view = m_viewport->cameraJson();
  m_viewport->setCameraJson({{"eye", {0, y0, 100}}, {"target", {0, y0, 0}}, {"up", {0, 1, 0}}, {"scale", view.value("scale", 100.0)}, {"projection", "orthographic"}, {"absolute", true}});
  check(std::abs(m_viewport->pixelSize() - px) < 1e-6 * px, "the view moved up, the same scale");
  auto key = [&](QEvent::Type type, int code, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent e(type, code, mods);
    QApplication::sendEvent(m_viewport, &e);
  };
  auto shift = [&](bool down) { key(down ? QEvent::KeyPress : QEvent::KeyRelease, Qt::Key_Shift, down ? Qt::ShiftModifier : Qt::NoModifier); };
  auto line = [&](double x0, double y, double x1, double y1) {
    setTool("line");
    place(x0, y, Qt::AltModifier);
    place(x1, y1, Qt::AltModifier);
    finishChain();
    return m_sk.entities.back().id;
  };
  const int pl = line(-40, y0 - 15, -10, y0 - 15), r1 = line(-45, y0 + 20, -35, y0 + 20), r2 = line(-25, y0 + 10, -25, y0 + 15);
  setTool("circle");
  place(20, y0, Qt::AltModifier);
  place(28, y0, Qt::AltModifier);
  const int q = m_sk.entities.back().id;
  // Perpendicular: from (-32, y0 - 5) down onto P.
  setTool("line");
  place(-32, y0 - 5, Qt::AltModifier);
  sketchMove(-32 + 2 * px, y0 - 15 + 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Perpendicular && exact(m_cursor.u, -32) && exact(m_cursor.v, y0 - 15) && shows(M::Perpendicular) && (m_glyphs == std::vector<G>{G::OnCurve, G::Perpendicular}),
        "square onto P from the last point: the perpendicular marker, on P and perpendicular");
  shot(".perpendicular.png");
  place(-32 + 2 * px, y0 - 15 + 2 * px);
  const int foot = m_chain.back(), down = m_sk.entities.back().id;
  check(has(CT::Perpendicular, {down, pl}) && has(CT::Coincident, {foot, pl}) && solved(), "the segment keeps square to P, its end on P");
  // Tangent: from that foot onto the circle Q.
  double tx[2], ty[2];
  const int touching = sketchsnap::tangents(sketchsnap::Curve{20, y0, 0, 0, 8, 0, 2 * M_PI}, -32, y0 - 15, tx, ty);
  sketchMove(tx[0] + px, ty[0] - px, Qt::NoModifier, false);
  check(touching == 2 && m_cursor.kind == Snap::Kind::Tangent && std::abs(m_cursor.u - tx[0]) < 1e-9 && shows(M::Tangent) && (m_glyphs == std::vector<G>{G::OnCurve, G::Tangent}),
        "touching Q from the last point: the tangent marker, on Q and tangent");
  shot(".tangent.png");
  place(tx[0] + px, ty[0] - px);
  check(has(CT::Tangent, {m_sk.entities.back().id, q}) && has(CT::Coincident, {m_chain.back(), q}) && solved(), "the segment keeps tangent to Q, its end on Q");
  finishChain();
  // Apparent intersection: R1 (horizontal) and R2 (vertical) would cross at (-25, y0 + 20).
  setTool("point");
  sketchMove(-25 + 2 * px, y0 + 20 + px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Apparent && exact(m_cursor.u, -25) && exact(m_cursor.v, y0 + 20) && shows(M::Apparent) && (m_glyphs == std::vector<G>{G::OnCurve}),
        QString("where R1 and R2 would cross: the apparent intersection marker, on both") + where());
  shot(".apparent.png");
  place(-25 + 2 * px, y0 + 20 + px);
  check(has(CT::Coincident, {newPoint(), r1}) && has(CT::Coincident, {newPoint(), r2}) && solved(), "the click keeps the point on both lines");
  // An extension's marker follows its line out of the end it goes on from (E, slanted 45 degrees up right, its end tracked);
  // a Shift lock onto it draws the locked line thick dashed.
  const int ext = line(30, y0 - 22, 36, y0 - 16), ee = m_sk.entity(ext)->p[1];
  setTool("point");
  sketchMove(36 + px, y0 - 16, Qt::NoModifier, false);
  rest(450);
  check(m_tracked == std::vector<int>{ee}, "resting on E's end tracks it");
  auto turn = [&](double a) { return std::abs(std::remainder(m_markerTurn - a, 2 * M_PI)) < 0.02; };
  sketchMove(41 + px, y0 - 11 - 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Extension && m_cursor.target == ext && shows(M::Extension) && turn(M_PI / 4) && (m_glyphs == std::vector<G>{G::OnCurve}) && !transientLocked(),
        QString("on E's extension past its end: the extension marker turned along it, out of the end (%1 degrees), on E").arg(m_markerTurn * 180 / M_PI) + where());
  shot(".extension.png");
  sketchMove(28 - px, y0 - 24 + 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Extension && turn(-3 * M_PI / 4), QString("past E's start: turned the other way (%1 degrees)").arg(m_markerTurn * 180 / M_PI) + where());
  sketchMove(41 + px, y0 - 11 - 2 * px, Qt::NoModifier, false);
  shift(true);
  sketchMove(43, y0 - 8, Qt::ShiftModifier, false);
  check(m_lock && m_cursor.kind == Snap::Kind::Locked && std::abs(m_cursor.u - m_cursor.v + y0 - 52) < 1e-9 && transientLocked() == 1 && transientTexts().contains("Locked"),
        QString("Shift held on it: locked, its line drawn thick dashed (%1)").arg(transientLocked()) + where());
  shot(".locked.png");
  rest(350);
  shift(false);
  check(!m_lock && !transientLocked(), "let go: no lock, no thick line");
  // Shift taps go through the snaps in reach: a 0.6 mm line's two ends and its midpoint.
  const int tiny = line(5, y0 - 20, 5.6, y0 - 20), ta = m_sk.entity(tiny)->p[0], tb = m_sk.entity(tiny)->p[1];
  setTool("point");
  sketchMove(5.25, y0 - 20 + px, Qt::NoModifier, false);
  check(m_cursor.choices == 3 && m_cursor.choice == 0 && m_cursor.point == ta && keyHints().contains("Shift next snap") && transientTexts().contains(QString::fromUtf8("Point · 1/3")),
        QString("three snaps in reach: the nearest end shown (1/3), the prompt says Shift goes to the next (%1)").arg(transientTexts().join(" | ")));
  shift(true);
  shift(false);
  check(m_cursor.choice == 1 && m_cursor.point == tb && transientTexts().contains(QString::fromUtf8("Point · 2/3")), "a Shift tap: the other end (2/3)");
  shift(true);
  shift(false);
  check(m_cursor.choice == 2 && m_cursor.kind == Snap::Kind::Midpoint && shows(M::Midpoint) && transientTexts().contains(QString::fromUtf8("Midpoint · 3/3")), "the next: the midpoint (3/3)");
  shot(".cycle.png");
  sketchMove(5.25 + 0.3 * tol(), y0 - 20 + px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Midpoint, "the pointer trembling within the capture keeps it");
  place(5.25 + 0.3 * tol(), y0 - 20 + px);
  check(has(CT::Midpoint, {newPoint(), tiny}) && !m_lock, "the click lands on the snap shown, the midpoint");
  sketchMove(5.25, y0 - 20 + px, Qt::NoModifier, false);
  check(m_cursor.choice == 0, "after the click the nearest is shown first again");
  shift(true);
  rest(350);
  shift(false);
  check(m_cursor.choice == 0 && !m_lock, QString("Shift held, not tapped, does not go on") + where());
  // The horizontal, vertical and angle inference of every tool, kept where the shape has a line or two points for it.
  setTool("slot");
  place(-40, y0 - 22, Qt::AltModifier);
  sketchMove(-28, y0 - 22 + 2 * px, Qt::NoModifier, false);
  check(m_cursor.horizontal && exact(m_cursor.v, y0 - 22) && (m_glyphs == std::vector<G>{G::Horizontal}) && transientTexts().contains("Horizontal"), QString("a slot's second centre level with its first: horizontal") + where());
  place(-28, y0 - 22 + 2 * px);
  place(-30, y0 - 20, Qt::AltModifier);
  int centres = 0;
  for (const auto& e : m_sk.entities)
    if (e.construction && e.type == SkEntity::Type::Line) centres = e.id;
  check(has(CT::Horizontal, {centres}) && solved(), "the slot keeps its centre line horizontal");
  setTool("polygon");
  place(35, y0 + 15, Qt::AltModifier);
  sketchMove(35 + 2 * px, y0 + 22, Qt::NoModifier, false);
  check(m_cursor.vertical && exact(m_cursor.u, 35) && (m_glyphs == std::vector<G>{G::Vertical}), QString("a polygon's corner straight above its centre: vertical") + where());
  place(35 + 2 * px, y0 + 22);
  int hub = 0, top = 0;
  for (const auto& p : m_sk.points) {
    if (exact(p.x, 35) && exact(p.y, y0 + 15)) hub = p.id;
    if (exact(p.x, 35) && exact(p.y, y0 + 22)) top = p.id;
  }
  check(hub && top && has(CT::Vertical, {hub, top}) && solved(), "the polygon keeps that corner above its centre");
  setTool("ellipse");
  place(-5, y0 + 15, Qt::AltModifier);
  sketchMove(-5 + 8 * std::cos(M_PI / 6) - 2 * px, y0 + 15 + 8 * std::sin(M_PI / 6) + 2 * px, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Angle && std::abs(std::atan2(m_cursor.v - y0 - 15, m_cursor.u + 5) - M_PI / 6) < 1e-9 && transientTexts().contains(QString::fromUtf8("30°")),
        QString("an ellipse's axis: the angle ray from its centre, 30 degrees") + where());
  key(QEvent::KeyPress, Qt::Key_Escape);
  // Ortho (F8): the pointer keeps to the horizontal or the vertical from the last point, whichever is nearer.
  auto* f8 = m_viewport->window()->findChild<QAction*>("view.orthoSnap");
  check(f8 && f8->shortcut() == QKeySequence("F8") && !f8->isChecked(), QString("Ortho is F8, off by default (%1, %2)").arg(f8 ? f8->shortcut().toString() : "none").arg(f8 && f8->isChecked()));
  setTool("line");
  place(-45, y0 - 5, Qt::AltModifier);
  sketchMove(-37, y0 - 2, Qt::NoModifier, false);
  check(!exact(m_cursor.v, y0 - 5), "Ortho off: the pointer is free");
  if (f8) f8->setChecked(true);
  check(m_cursor.kind == Snap::Kind::Locked && m_cursor.ortho && exact(m_cursor.v, y0 - 5) && exact(m_cursor.u, -37) && m_cursor.horizontal && transientTexts().contains("Ortho") &&
            (m_glyphs == std::vector<G>{G::Horizontal}) && !transientLocked(),
        QString("F8: at once on the horizontal from the last point, \"Ortho\", horizontal (dashed, not the thick line of a lock)") + where());
  shot(".ortho.png");
  place(-37, y0 - 2);
  check(has(CT::Horizontal, {m_sk.entities.back().id}) && exact(m_sk.point(m_chain.back())->y, y0 - 5), "the segment is kept horizontal");
  sketchMove(-35, y0 + 3, Qt::NoModifier, false);
  check(m_cursor.ortho && m_cursor.vertical && exact(m_cursor.u, -37), QString("further up than across: the vertical") + where());
  if (f8) f8->setChecked(false);
  sketchMove(-35, y0 + 3, Qt::NoModifier, false);
  check(!m_cursor.ortho && m_cursor.kind != Snap::Kind::Locked && !exact(m_cursor.u, -37), QString("F8 again: free of it (the angle ray may hold the pointer)") + where());
  finishChain();
  setTool("select");
  shot(".png");
  QCoreApplication::exit(ok ? 0 : 2);
}
