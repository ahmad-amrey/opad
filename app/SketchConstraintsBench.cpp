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
// a coincidence is a dot on its point, the explicit one and the corners where two sides end (no constraint there); hovering
// a badge lights up what it holds, hovering a corner the sides meeting there; Show constraints (the panel's check box and
// the command) hides them, leaving those selected.
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
  int corner = 0;
  for (int id : m_joinDots)
    if (const auto* p = m_sk.point(id); p && std::hypot(p->x - 30, p->y - 20) < 1e-9) corner = id;
  check(m_joinDots.size() == 4 && corner && coincidenceDots() == 5, QString("the four corners, where two sides end on one point, are dots too (%1 of %2 dots)").arg(m_joinDots.size()).arg(coincidenceDots()));
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
  sketchMove(30, 20, Qt::NoModifier, false);
  check(m_hover.kind == Hit::Point && m_hover.id == corner && transientSolid(m_viewport->tokens().hov) == 2, QString("hovering a corner's dot lights up the two sides ending there (%1)").arg(transientSolid(m_viewport->tokens().hov)));
  sketchMove(15, 40, Qt::NoModifier, false);

  // Show constraints: off hides them all, but a selected one; the command turns them on again.
  box->click();
  check(!showConstraints() && m_glyphHits.empty() && m_coincidentDots.empty() && m_joinDots.empty() && coincidenceDots() == 0 && badgeTriangles() == 0, "Show constraints off hides the badges and the dots");
  m_sel = {horizontal};
  rebuild();
  check(m_glyphHits.size() == 1 && std::get<0>(m_glyphHits[0]) == horizontal, "a selected constraint still shows");
  m_sel.clear();
  command->trigger();
  QCoreApplication::processEvents();
  check(showConstraints() && box->isChecked() && m_glyphHits.size() == 9 && m_coincidentDots.size() == 1 && m_joinDots.size() == 4, "the command shows them again, the check box follows");

  // Badges face the viewer (the user's "constraint symbols get distorted in 3D"): an upright apart holding only a vertical, its
  // badge laid out face-on, then the view orbited to see the sketch at a slant, with nothing laying the badges out again. On
  // the screen the badge's pictogram (an upright bar) is still as tall as it was, beside its line where the layout put it;
  // lying in the plane it came out half as tall.
  begin_change();
  const int lone = m_sk.add_line(m_sk.add_point(70, -10), m_sk.add_point(70, 10));
  const int flat = m_sk.add_constraint(SkConstraint::Type::Vertical, {lone});
  end_change("bench constraints");
  m_viewport->setCameraJson({{"eye", {70, 0, 100}}, {"target", {70, 0, 0}}, {"up", {0, 1, 0}}, {"scale", 60}, {"projection", "orthographic"}, {"absolute", true}});
  rebuild();
  double fu = 70, fv = 0;
  for (const auto& [id, u, v] : m_glyphHits)
    if (id == flat) fu = u, fv = v;
  const QPoint offset = m_viewport->widgetPoint(m_frame.to_world(fu, fv)) - m_viewport->widgetPoint(m_frame.to_world(70, 0));
  const QColor back = m_viewport->tokens().green;
  // The badge's pictogram on the screen: the box of the pixels of its colour about where it should be, in widget pixels.
  auto badgeBox = [&](const QPoint& centre) {
    const QImage image = m_viewport->grabImage();
    const double k = m_viewport->width() > 0 ? double(image.width()) / m_viewport->width() : 1;
    const int cx = int(centre.x() * k), cy = int(centre.y() * k), reach = int(30 * k);
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    for (int y = std::max(0, cy - reach); y < std::min(image.height(), cy + reach); ++y)
      for (int x = std::max(0, cx - reach); x < std::min(image.width(), cx + reach); ++x) {
        const QColor c = image.pixelColor(x, y);
        if (std::abs(c.red() - back.red()) + std::abs(c.green() - back.green()) + std::abs(c.blue() - back.blue()) > 12) continue;
        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
      }
    return x1 < 0 ? QRectF() : QRectF(x0 / k, y0 / k, (x1 - x0 + 1) / k, (y1 - y0 + 1) / k);
  };
  const QRectF faceOn = badgeBox(m_viewport->widgetPoint(m_frame.to_world(fu, fv)));
  m_viewport->setCameraJson({{"eye", {70, -60, 35}}, {"target", {70, 0, 0}}, {"up", {0, 0, 1}}, {"scale", 60}, {"projection", "orthographic"}, {"absolute", true}});
  const QRectF slanted = badgeBox(m_viewport->widgetPoint(m_frame.to_world(70, 0)) + offset);
  m_viewport->grabImage().save(prefix + ".slanted.png");
  const bool tall = faceOn.height() > 7 && faceOn.height() < 20;
  check(tall && std::abs(slanted.width() - faceOn.width()) < 2 && std::abs(slanted.height() - faceOn.height()) < 2,
        QString("a badge seen at a slant is as it is face-on, beside its line (pictogram %1 x %2 px face-on, %3 x %4 at a slant)")
            .arg(faceOn.width()).arg(faceOn.height()).arg(slanted.width()).arg(slanted.height()));
  double su = 70, sv = 0;
  m_viewport->planePoint(QPointF(m_viewport->widgetPoint(m_frame.to_world(70, 0)) + offset), m_frame, su, sv);
  sketchMove(su, sv, Qt::NoModifier, false);
  check(m_hover.kind == Hit::Dimension && m_hover.id == flat, "at a slant the badge is hovered where it shows");
  QCoreApplication::exit(ok ? 0 : 2);
}
