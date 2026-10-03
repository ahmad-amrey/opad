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
// diamond quadrant, cross intersection, hourglass on a curve, # grid, plus at a tracked point).
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

  // L: (-30, -20) -> (10, -20); N: (0, -30) -> (0, -5) crossing it at (0, -20); a circle C about (25, 15), radius 10.
  setTool("line");
  place(-30, -20, Qt::AltModifier);
  place(10, -20, Qt::AltModifier);
  finishChain();
  const int l = m_sk.entities.back().id, l0 = m_sk.entity(l)->p[0];
  place(0, -30, Qt::AltModifier);
  place(0, -5, Qt::AltModifier);
  finishChain();
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
  setTool("select");
  shot(".png");
  QCoreApplication::exit(ok ? 0 : 2);
}
