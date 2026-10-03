#include "SketchEditor.hpp"
#include "SketchSnap.hpp"
#include "Jobs.hpp"
#include <QAction>
#include <QCheckBox>
#include <QCoreApplication>
#include <QSettings>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_GRID=<prefix> (TODO 11 UI-18): grid snapping through the tool code, as the mouse drives it. One
// switch (F9's action, the panel's checkbox and the viewport agree), the sketch shows its own grid (G hides it there
// only, apart from snapping), the step follows the zoom, a pointer near a node lands on it although the angle or a
// guide would take it, a guide away from the nodes is quantised along itself, a dragged point lands on a node (Alt
// drags freely), and off is off.
void SketchEditor::benchGrid() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_GRID");
  bool ok = true;
  auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sketch grid: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  auto same = [](double a, double b) { return std::abs(a - b) < 1e-9; };
  auto at = [&](int id, double u, double v) { const SkPoint* p = m_sk.point(id); return p && std::abs(p->x - u) < 1e-6 && std::abs(p->y - v) < 1e-6; };  // after a solve
  QWidget* window = m_viewport->window();
  auto* f9 = window->findChild<QAction*>("view.gridSnap");
  auto* box = window->findChild<QCheckBox*>("snap-grid");
  auto* g = window->findChild<QAction*>("view.grid");
  if (!f9 || !box || !g) {
    check(false, "the F9 and G actions and the panel's checkbox exist");
    return QCoreApplication::exit(2);
  }
  QSettings().setValue("sketch/snap/angle", true);
  QSettings().setValue("sketch/angleStep", 15);
  QSettings().setValue("view/tracking", true);

  f9->setChecked(false);
  check(!m_viewport->gridSnap() && !box->isChecked(), "off: the switch and the panel agree");
  check(m_viewport->gridShown() && g->isChecked() && !QSettings().value("view/grid", false).toBool(), "the sketch shows its grid although the 3D grid is off, and G says so");
  g->trigger();
  check(!m_viewport->gridShown() && !g->isChecked() && !QSettings().value("sketch/grid", true).toBool() && !QSettings().value("view/grid", false).toBool(),
        "G hides the sketch's grid and keeps that for sketches only");
  f9->trigger();
  check(m_viewport->gridSnap() && box->isChecked() && !m_viewport->gridShown() && QSettings().value("view/gridSnap").toBool(),
        "F9 turns snapping on, the panel shows it, the setting is saved, a hidden grid stays hidden");
  m_viewport->endSketchInput();  // as replacing the sketch's plane does: out and in again
  check(!m_viewport->gridShown() && !g->isChecked(), "out of the sketch, G shows the 3D grid (off)");
  m_viewport->beginSketchInput(this, m_frame, m_id);
  check(!m_viewport->gridShown() && !g->isChecked(), "back in, the sketch's grid is still hidden");
  g->trigger();
  check(m_viewport->gridShown() && g->isChecked() && QSettings().value("sketch/grid", false).toBool() && !QSettings().value("view/grid", false).toBool(), "G shows it again");
  box->click();
  check(!m_viewport->gridSnap() && !f9->isChecked() && !QSettings().value("view/gridSnap").toBool(), "the panel's checkbox is the same switch (off)");
  box->click();
  check(m_viewport->gridSnap() && f9->isChecked(), "the panel's checkbox is the same switch (on)");

  // A view 105 mm across: a 10 mm grid. Ten times closer: 1 mm.
  auto camera = [&](double scale) {
    m_viewport->setCameraJson({{"eye", {0, 0, 100}}, {"target", {0, 0, 0}}, {"up", {0, 1, 0}}, {"scale", scale}, {"projection", "orthographic"}, {"absolute", true}});
  };
  camera(100);
  const double scale = 100 * 105 / (std::max(m_viewport->width(), m_viewport->height()) * m_viewport->pixelSize());
  camera(scale / 10);
  check(same(m_viewport->gridStep(), 1), QString("zoomed in, the step is 1 mm (%1)").arg(m_viewport->gridStep()));
  camera(scale);
  const double s = m_viewport->gridStep(), px = m_viewport->pixelSize();
  m_viewport->grabImage();
  check(same(s, 10) && same(m_viewport->gridShownStep(), s), QString("the step follows the zoom: 10 mm, as drawn (%1, drawn %2)").arg(s).arg(m_viewport->gridShownStep()));
  if (s / px < 4 * tol() / px) {
    check(false, QString("grid cells of %1 px are too small for this bench").arg(s / px));
    return QCoreApplication::exit(2);
  }

  auto place = [&](double u, double v, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    sketchMove(u, v, mods, false);
    sketchPress(u, v, mods);
    sketchRelease(u, v, mods);
  };
  auto constrained = [&](SkConstraint::Type type) {
    const SkEntity* line = m_sk.entities.empty() ? nullptr : &m_sk.entities.back();
    for (const auto& c : m_sk.constraints)
      if (line && c.type == type && c.refs == std::vector<int>{line->id}) return true;
    return false;
  };
  setTool("line");
  place(s + 3 * px, s - 2 * px);
  check(!m_chain.empty() && at(m_chain.back(), s, s), "a click 3 px off a node lands on it");
  // Node (5s, 2s) is 14 degrees from (s, s), beside the 15 degree ray: the pointer 5 px off it towards the ray, which
  // holds that pointer too (and used to take the point off the grid).
  const double rx = std::cos(M_PI / 12), ry = std::sin(M_PI / 12), k = 4 * s * rx + s * ry, fu = s + k * rx - 5 * s, fv = s + k * ry - 2 * s;
  const double gap = std::hypot(fu, fv), step = std::min(5 * px, gap), cu = 5 * s + fu * step / gap, cv = 2 * s + fv * step / gap;
  sketchsnap::Guide ray;
  check(sketchsnap::angleRay(s, s, cu, cv, 15 * M_PI / 180, tol(), ray), "the angle inference holds the next pointer");
  sketchMove(cu, cv, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Grid && m_cursor.grid && same(m_cursor.u, 5 * s) && same(m_cursor.v, 2 * s), "the grid node wins over the angle, with the grid marker");
  m_viewport->grabImage().save(prefix + ".node.png");
  place(cu, cv);
  check(at(m_chain.back(), 5 * s, 2 * s), "the click lands exactly on the node");
  place(5 * s + 3 * px, 4 * s - 2 * px);  // straight above, a little to the side (it gave a slanted line)
  check(at(m_chain.back(), 5 * s, 4 * s) && constrained(SkConstraint::Type::Vertical), "straight above: on the node, vertical and constrained so");
  place(7.4 * s, 4 * s + 3 * px);  // horizontal, away from any node: whole steps along the guide
  check(at(m_chain.back(), 7 * s, 4 * s) && constrained(SkConstraint::Type::Horizontal), "a horizontal guide away from the nodes snaps along itself");
  // 1.4 steps along the 30 degree ray, 2 px beside it and 0.37 steps from the nearest node: one whole step along the ray.
  const double c30 = std::cos(M_PI / 6), s30 = std::sin(M_PI / 6), ru = 7 * s + 1.4 * s * c30 - 2 * px * s30, rv = 4 * s + 1.4 * s * s30 + 2 * px * c30;
  sketchMove(ru, rv, Qt::NoModifier, false);
  check(m_cursor.kind == Snap::Kind::Angle && m_cursor.grid && same(m_cursor.u, 7 * s + s * c30) && same(m_cursor.v, 4 * s + s * s30), "the angle ray takes whole grid steps (polar snap)");
  place(ru, rv);
  check(at(m_chain.back(), 7 * s + s * c30, 4 * s + s * s30), "the click lands one step along the ray");
  finishChain();

  setTool("point");
  place(-2 * s + 2 * px, -s - 3 * px);
  const int free = m_sk.points.back().id;
  check(at(free, -2 * s, -s), "a point lands on the node");
  setTool("select");
  sketchMove(-2 * s, -s, Qt::NoModifier, false);
  sketchPress(-2 * s, -s, Qt::NoModifier);
  const size_t undo = m_undo.size();
  sketchMove(-2 * s + 0.2 * s, -s + 0.3 * s, Qt::NoModifier, true);
  sketchMove(-2 * s + 0.37 * s, -s + 0.61 * s, Qt::NoModifier, true);
  check(at(free, -2 * s, 0) && m_dragGrid && same(m_dragGridU, -2 * s) && same(m_dragGridV, 0), "a dragged point snaps to the node nearest where it is dragged");
  m_viewport->grabImage().save(prefix + ".drag.png");
  sketchMove(-2 * s + 0.37 * s, -s + 0.61 * s, Qt::AltModifier, true);
  check(at(free, -1.63 * s, -0.39 * s), "with Alt it drags freely");
  sketchMove(-2 * s + 1.38 * s, -s + 0.61 * s, Qt::NoModifier, true);
  sketchRelease(-2 * s + 1.38 * s, -s + 0.61 * s, Qt::NoModifier);
  check(at(free, -s, 0) && m_undo.size() == undo + 1, "dropped on a node, one undo step");

  f9->trigger();
  check(!m_viewport->gridSnap() && !box->isChecked() && m_viewport->gridShown(), "F9 turns snapping off again, the grid stays");
  setTool("line");
  place(-5 * s + 3 * px, 3 * s + 2 * px);
  check(!m_chain.empty() && at(m_chain.back(), -5 * s + 3 * px, 3 * s + 2 * px), "off, a click stays where it is");
  finishChain();
  setTool("select");
  QCoreApplication::exit(ok ? 0 : 2);
}
