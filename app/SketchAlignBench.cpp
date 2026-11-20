// Bench: sketch Align driven through the editor's own press/release on a document's sketch (OPAD_BENCH_SKETCH_ALIGN).
//
// The first sketch of the document opens; Align (Vertical) is chosen and its first curve with two ends (a line, an arc,
// an open spline) is pressed a tenth of the way along from its first end, a few pixels off the curve: the snap there lands
// on the end point, which the pick once took for that point and ignored the click. The preview must come, Enter must
// keep it, the curve's ends must stand one above the other and every point of the curves that moved keep its distances
// to the others (a rigid group turns as one). The sketch is left unsaved. Case: tools/bench_cases/sketch_align.py.
#include <QCoreApplication>
#include <QTimer>

#include <cmath>
#include <memory>
#include <set>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"
#include "SketchEditor.hpp"
#include "Jobs.hpp"
#include "AppDocument.hpp"
#include "opad/design/sketch.hpp"
#include "opad/design/sketch_modify.hpp"

using namespace opad::design;

OPAD_BENCH(OPAD_BENCH_SKETCH_ALIGN, sketch_align) {
  if (w.m_doc->scene.sketches.empty()) {
    trace::log("bench: sketch align: the document has no sketch FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  struct State {
    int phase = 0, ticks = 0, curve = 0, a = 0, b = 0;
    Sketch before;
    QString lastStatus;
  };
  auto st = std::make_shared<State>();
  DesignController* design = w.m_design;
  QObject::connect(design->sketch(), &SketchEditor::status, design, [st](const QString& s) { st->lastStatus = s; });
  design->editOp(w.m_doc->scene.sketches.front().id);
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [st, design, timer] {
    auto finish = [timer](bool ok, const QString& why) {
      timer->stop();
      trace::log(QString("bench: sketch align: %1 %2").arg(why, ok ? "PASS" : "FAIL"));
      QCoreApplication::exit(ok ? 0 : 2);
    };
    SketchEditor* sk = design->sketch();
    if (++st->ticks > 200) return finish(false, QString("timed out in phase %1 (status: %2)").arg(st->phase).arg(st->lastStatus));
    if (!sk->active() || sk->busy()) return;
    switch (st->phase) {
      case 0: {
        st->before = Sketch::from_json(sk->geometry());
        for (const auto& e : st->before.entities)
          if (align_ends(st->before, e.id, st->a, st->b)) { st->curve = e.id; break; }
        if (!st->curve) return finish(false, "the sketch has no line, arc or open spline");
        const SkPoint *p = st->before.point(st->a), *q = st->before.point(st->b);
        // A tenth along the chord, nudged across it by a little (a press beside the curve, inside the pick distance).
        const double dx = q->x - p->x, dy = q->y - p->y, len = std::hypot(dx, dy);
        const double u = p->x + 0.1 * dx - dy / len * 0.01 * len, v = p->y + 0.1 * dy + dx / len * 0.01 * len;
        sk->setTool("align");
        sk->sketchPress(u, v, Qt::NoModifier);
        sk->sketchRelease(u, v, Qt::NoModifier);
        trace::log(QString("bench: sketch align: pressed %1, %2 by curve %3").arg(u).arg(v).arg(st->curve));
        st->phase = 1;
        return;
      }
      case 1:
        if (!st->lastStatus.contains("Preview ready")) {
          if (st->ticks > 60) return finish(false, "no preview after the click (status: " + st->lastStatus + ")");
          return;
        }
        trace::log("bench: sketch align: the click previews PASS");
        sk->done();  // Enter
        st->phase = 2;
        return;
      case 2: {
        const Sketch after = Sketch::from_json(sk->geometry());
        const SkPoint *p = after.point(st->a), *q = after.point(st->b);
        if (!p || !q) return finish(false, "the curve's ends are gone");
        if (std::fabs(p->x - q->x) > 1e-6) return finish(false, QString("the ends are %1 mm apart across").arg(std::fabs(p->x - q->x)));
        // Points that moved keep their distances among themselves.
        std::vector<int> moved;
        for (const auto& pt : after.points)
          if (const SkPoint* o = st->before.point(pt.id); o && std::hypot(o->x - pt.x, o->y - pt.y) > 1e-9) moved.push_back(pt.id);
        double worst = 0;
        for (size_t i = 0; i < moved.size(); ++i)
          for (size_t j = i + 1; j < moved.size(); ++j) {
            const SkPoint *a0 = st->before.point(moved[i]), *b0 = st->before.point(moved[j]), *a1 = after.point(moved[i]), *b1 = after.point(moved[j]);
            worst = std::max(worst, std::fabs(std::hypot(a0->x - b0->x, a0->y - b0->y) - std::hypot(a1->x - b1->x, a1->y - b1->y)));
          }
        if (moved.empty()) return finish(false, "nothing moved");
        if (worst > 1e-6) return finish(false, QString("the shape changed by %1 mm").arg(worst));
        return finish(true, QString("curve %1 stands upright; %2 points turned as one").arg(st->curve).arg(moved.size()));
      }
    }
  });
  timer->start();
  return true;
}
