// OPAD_BENCH_SKETCH_FILLET=<prefix> (tools/bench_cases/sketch.py, case sketch-fillet): Sketch fillet on a profile like a
// drawing turned into a sketch, through the tool's own clicks (hover, press, release at the corner).
//  - The report's corner (an upright line with its length dimension meeting a long arc, a construction line ending there
//    too): the hover previews the arc, a click rounds the corner, the sketch solves, its length dimension kept.
//  - A line and an arc whose ends are two points held by a coincident constraint: the click says why nothing happens, on the
//    status line and in a toast (it did nothing visible before).
#include <QCoreApplication>
#include <QTimer>
#include <cmath>
#include <memory>

#include "Jobs.hpp"
#include "SketchEditor.hpp"
#include "Toast.hpp"
#include "opad/design/sketch_modify.hpp"

using namespace opad::design;

void SketchEditor::benchFillet() {
  auto ok = std::make_shared<bool>(true);
  auto check = [ok](bool pass, const QString& what) {
    trace::log(QString("bench: sketch fillet: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    *ok = *ok && pass;
  };
  using CT = SkConstraint::Type;
  begin_change();
  const int corner = m_sk.add_point(10, 5);
  const int upright = m_sk.add_line(m_sk.add_point(10, 1), corner);
  m_sk.add_constraint(CT::Vertical, {upright});
  const int length = m_sk.add_constraint(CT::Distance, {upright}, 4);
  const double end = -60 * M_PI / 180;
  m_sk.add_arc(m_sk.add_point(14.2, 10.6), corner, m_sk.add_point(14.2 + 7 * std::cos(end), 10.6 + 7 * std::sin(end)));
  const int guide = m_sk.add_line(corner, m_sk.add_point(12, 5), true);
  m_sk.add_constraint(CT::Horizontal, {guide});
  const int a1 = m_sk.add_point(30, 5), b0 = m_sk.add_point(30, 5);
  m_sk.add_line(m_sk.add_point(30, 1), a1);
  m_sk.add_arc(m_sk.add_point(34.2, 10.6), b0, m_sk.add_point(34.2 + 7 * std::cos(end), 10.6 + 7 * std::sin(end)));
  m_sk.add_constraint(CT::Coincident, {a1, b0});
  check(end_change(QStringLiteral("Fillet bench")), "the profile is made");
  fitSketch();
  QTimer::singleShot(400, this, [=, this] {  // the geometry index ready for picking
    setTool("fillet");
    auto said = std::make_shared<QString>();
    auto toldStatus = connect(this, &SketchEditor::status, this, [said](const QString& t) { *said = t; });
    auto arcs = [this] { return std::count_if(m_sk.entities.begin(), m_sk.entities.end(), [](const SkEntity& e) { return e.type == SkEntity::Type::Arc; }); };
    auto clickAt = [this](double u, double v) {
      sketchMove(u, v, Qt::NoModifier, false);
      sketchPress(u, v, Qt::NoModifier);
      sketchRelease(u, v, Qt::NoModifier);
    };
    sketchMove(10, 5, Qt::NoModifier, false);
    check(m_hover.kind == Hit::Point && m_hover.id == corner && m_rubberKinds.count("arc"), "hovering the corner previews the fillet's arc");
    const auto before = arcs();
    clickAt(10, 5);
    check(arcs() == before + 1 && m_sk.constraint(length) && std::fabs(m_sk.constraint(length)->value - 4) < 1e-9,
          "a click rounds the corner, the line's 4 mm dimension kept (" + *said + ")");
    said->clear();
    clickAt(30, 5);
    const bool toast = [this] {
      for (Toast* t : m_viewport->window()->findChildren<Toast*>())
        if (t->text().contains("Sketch fillet")) return true;
      return false;
    }();
    check(arcs() == before + 1 && said->contains("only one line") && toast,
          "two ends held together: nothing made, and the status line and a toast say why: " + *said);
    disconnect(toldStatus);
    trace::log(QString("bench: sketch fillet: corners rounded or refused with a reason %1").arg(*ok ? "PASS" : "FAIL"));
    QCoreApplication::exit(*ok ? 0 : 2);
  });
}
