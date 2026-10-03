#include "SketchEditor.hpp"
#include "SketchPanel.hpp"
#include "Jobs.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <cmath>

using namespace opad::design;

// OPAD_BENCH_SKETCH_CONSTRAINTS=<prefix> (TODO 11 UI-24): constraints show as pictograms in badges, laid out so that none
// covers another (a rectangle's bottom line holding a horizontal, an equal, a perpendicular and a midpoint on one spot);
// an explicit coincidence is a dot on its point; hovering a badge lights up what it holds; Show constraints (the panel's
// check box and the command) hides them, leaving those selected.
void SketchEditor::benchConstraints() {
  const QString prefix = qEnvironmentVariable("OPAD_BENCH_SKETCH_CONSTRAINTS");
  bool ok = true;
  auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: sketch constraints: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  QWidget* window = m_viewport->window();
  auto* panel = window->findChild<SketchPanel*>();
  auto* box = window->findChild<QCheckBox*>("sketch-showConstraints");
  auto* command = window->findChild<QAction*>("sketch.showConstraints");
  if (!panel || !box || !command) {
    check(false, "the panel's Show constraints check box and the command exist");
    return QCoreApplication::exit(2);
  }
  auto place = [&](double u, double v) {
    sketchMove(u, v, Qt::AltModifier, false);
    sketchPress(u, v, Qt::AltModifier);
    sketchRelease(u, v, Qt::AltModifier);
  };
  m_viewport->setGridSnap(false);
  m_viewport->setCameraJson({{"eye", {15, 10, 100}}, {"target", {15, 10, 0}}, {"up", {0, 1, 0}}, {"scale", 60}, {"projection", "orthographic"}, {"absolute", true}});
  setShowConstraints(true);
  setTool("rect");
  place(0, 0);
  place(30, 20);
  setTool("select");
  auto line = [&](double y0, double y1) {
    for (const auto& e : m_sk.entities)
      if (e.type == SkEntity::Type::Line && std::abs(m_sk.point(e.p[0])->y - y0) < 1e-9 && std::abs(m_sk.point(e.p[1])->y - y1) < 1e-9 && std::abs(m_sk.point(e.p[0])->x - m_sk.point(e.p[1])->x) > 1) return e.id;
    return 0;
  };
  const int bottom = line(0, 0), top = line(20, 20);
  int left = 0;
  for (const auto& e : m_sk.entities)
    if (e.type == SkEntity::Type::Line && std::abs(m_sk.point(e.p[0])->x) < 1e-9 && std::abs(m_sk.point(e.p[1])->x) < 1e-9) left = e.id;
  // More on the bottom line: equal to the top, square to the left side, a point held at its middle, a point on the left side.
  begin_change();
  const int equal = m_sk.add_constraint(SkConstraint::Type::Equal, {bottom, top});
  const int square = m_sk.add_constraint(SkConstraint::Type::Perpendicular, {bottom, left});
  const int middle = m_sk.add_point(15, 0);
  m_sk.add_constraint(SkConstraint::Type::Midpoint, {middle, bottom});
  const int onSide = m_sk.add_point(0, 10);
  const int coincident = m_sk.add_constraint(SkConstraint::Type::Coincident, {onSide, left});
  const bool kept = end_change("bench constraints");
  int horizontal = 0;
  for (const auto& c : m_sk.constraints)
    if (c.type == SkConstraint::Type::Horizontal && c.refs == std::vector<int>{bottom}) horizontal = c.id;
  check(kept && bottom && top && left && horizontal && equal && square, "a rectangle with an equal, a perpendicular, a midpoint and a coincidence");
  QCoreApplication::processEvents();

  // Badges: one per constraint and curve, pictograms (no letters), none covering another on the screen.
  double rx = 1, ry = 0, ux = 0, uy = 1;
  pixelAxes(rx, ry, ux, uy);
  const double det = rx * uy - ux * ry;
  std::vector<std::pair<double, double>> screen;
  for (const auto& [id, u, v] : m_glyphHits) screen.push_back({(u * uy - ux * v) / det, (rx * v - u * ry) / det});
  bool apart = true;
  for (size_t i = 0; i < screen.size(); ++i)
    for (size_t j = i + 1; j < screen.size(); ++j)
      apart = apart && (std::abs(screen[i].first - screen[j].first) >= 17.9 || std::abs(screen[i].second - screen[j].second) >= 17.9);
  QStringList letters;
  for (const QString& text : overlayTexts())
    if (QStringList{"H", "V", "=", "//", "_|_", "M", "F", "T"}.contains(text)) letters << text;
  // horizontal x2, vertical x2, equal x2, perpendicular x2, midpoint (on its point): 9 badges
  check(m_glyphHits.size() == 9 && badgeTriangles() == 2 * m_glyphHits.size() && letters.isEmpty(), QString("%1 badges with pictograms, no letters (%2)").arg(m_glyphHits.size()).arg(letters.join(" ")));
  check(apart, "no badge covers another, also four on the bottom line's middle");
  check(m_coincidentDots.size() == 1 && std::get<0>(m_coincidentDots[0]) == coincident && std::abs(std::get<2>(m_coincidentDots[0]) - 10) < 1e-6, "the coincidence is a dot on its point");
  m_viewport->grabImage().save(prefix + ".png");

  // Hovering a badge lights up what it holds.
  double hu = 0, hv = 0;
  for (const auto& [id, u, v] : m_glyphHits)
    if (id == horizontal) hu = u, hv = v;
  sketchMove(hu, hv, Qt::NoModifier, false);
  check(m_hover.kind == Hit::Dimension && m_hover.id == horizontal && transientSolid(m_viewport->tokens().hov) > 0, "hovering the horizontal's badge lights up the bottom line");
  m_viewport->grabImage().save(prefix + ".hover.png");
  sketchMove(15, 40, Qt::NoModifier, false);
  check(m_hover.kind == Hit::None && transientSolid(m_viewport->tokens().hov) == 0, "off the badge nothing is lit");

  // Show constraints: off hides them all, but a selected one; the command turns them on again.
  box->click();
  check(!showConstraints() && m_glyphHits.empty() && m_coincidentDots.empty() && badgeTriangles() == 0, "Show constraints off hides the badges and the dot");
  m_sel = {horizontal};
  rebuild();
  check(m_glyphHits.size() == 1 && std::get<0>(m_glyphHits[0]) == horizontal, "a selected constraint still shows");
  m_sel.clear();
  command->trigger();
  QCoreApplication::processEvents();
  check(showConstraints() && box->isChecked() && m_glyphHits.size() == 9 && m_coincidentDots.size() == 1, "the command shows them again, the check box follows");
  QCoreApplication::exit(ok ? 0 : 2);
}
