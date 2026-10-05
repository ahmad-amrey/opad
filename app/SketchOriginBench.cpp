#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"
#include "PlanePicker.hpp"
#include "SketchEditor.hpp"
#include "opad/design/feature.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

#include <QCoreApplication>
#include <QTimer>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

#include <cmath>

// OPAD_BENCH_SKETCH_ORIGIN=1 (case sketch-origin in tools/bench_cases/design.py) on the box fixture (30 x 20 x 10, centred on
// the origin): New sketch on the box's top face starts its origin step where the face's frame has it (its lower-left corner,
// as a face picked first and the primitives' grid have it), not under the world origin; the origin then set in the face's
// frame is the sketch's frame in the editor and, once finished, in the document; New sketch on that sketch's plane starts
// at that sketch's own origin.
OPAD_BENCH(OPAD_BENCH_SKETCH_ORIGIN, sketchorigin) {
  struct State {
    int phase = 0, ticks = 0;
    opad::json face;
    opad::Frame faceFrame, previewed;
    std::string sketch;
  };
  auto st = std::make_shared<State>();
  for (const auto& id : w.m_doc->scene.all_bodies()) {
    TopTools_IndexedMapOfShape faces;
    TopExp::MapShapes(opad::node_world_shape(w.m_doc->doc, w.m_doc->scene, id), TopAbs_FACE, faces);
    for (int i = 1; i <= faces.Extent(); ++i) {
      const opad::json d = opad::describe_entity(faces(i));
      if (d.contains("normal") && d["normal"][2].get<double>() > 0.99) st->face = opad::Ref{id, opad::Ref::Kind::Face, i - 1}.to_json();
    }
  }
  if (st->face.is_null()) {
    trace::log("bench: sketch origin FAIL: the document needs a box");
    QCoreApplication::exit(2);
    return true;
  }
  w.setWorkspace("design");
  DesignController* design = w.m_design;
  auto* timer = new QTimer(&w);
  timer->setInterval(100);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, design, st, timer] {
    auto pass = [](const QString& what) { trace::log("bench: sketch origin: " + what + " PASS"); };
    auto same = [](const opad::Vec3& a, const opad::Vec3& b) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]) < 1e-7; };
    auto text = [](const opad::Vec3& p) { return QString("(%1, %2, %3)").arg(p[0]).arg(p[1]).arg(p[2]); };
    try {
      auto require = [](bool ok, const QString& why) {
        if (!ok) throw opad::Error(why.toStdString());
      };
      if (++st->ticks > 300) throw opad::Error("timed out in phase " + std::to_string(st->phase));
      PlanePicker* picker = design->planePicker();
      SketchEditor* sketch = design->sketch();
      if (w.m_doc->designBusy) return;
      switch (st->phase) {
        case 0:
          design->startSketch();
          picker->choose({{"face", st->face}});
          ++st->phase;
          return;
        case 1: {
          if (!picker->positioning()) return;
          st->faceFrame = opad::design::resolve_plane(w.m_doc->doc, w.m_doc->scene, {{"face", st->face}});
          const opad::Vec3 origin = picker->frame().origin;
          require(same(origin, st->faceFrame.origin) && std::abs(origin[0] + 15) < 1e-7 && std::abs(origin[1] + 10) < 1e-7,
                  "the origin step on the top face starts at " + text(origin) + ", not at the face's own origin " + text(st->faceFrame.origin));
          pass("New sketch on a face starts its origin at the face's lower-left corner, as the face's frame has it, not under the world origin");
          picker->setOrigin(5, 4);
          st->previewed = picker->frame();
          require(same(st->previewed.origin, st->faceFrame.to_world(5, 4)), "the origin set in the face's frame is shown there");
          picker->apply();
          ++st->phase;
          return;
        }
        case 2:
          if (!sketch->active()) return;
          require(same(sketch->frame().origin, st->previewed.origin), "the editor's origin " + text(sketch->frame().origin) + " is not the one previewed " + text(st->previewed.origin));
          sketch->setTool("rect");
          sketch->placePrecise("0", "0", 0);
          sketch->placePrecise("6", "3", 0);
          design->finishSketch();
          ++st->phase;
          return;
        case 3: {
          if (sketch->active()) return;
          require(!w.m_doc->scene.sketches.empty(), "the sketch was committed");
          const opad::SketchItem& made = w.m_doc->scene.sketches.back();
          st->sketch = made.id;
          require(same(made.frame.origin, st->previewed.origin) && same(made.frame.x, st->previewed.x) && same(made.frame.y, st->previewed.y),
                  "the committed sketch's origin " + text(made.frame.origin) + " is not the one previewed " + text(st->previewed.origin));
          pass("the sketch is made with the origin previewed, in the editor and in the document");
          design->startSketch();
          picker->choose({{"sketch", st->sketch}});
          ++st->phase;
          return;
        }
        case 4:
          if (!picker->positioning()) return;
          require(same(picker->frame().origin, st->previewed.origin), "a sketch's plane starts its origin at " + text(picker->frame().origin) + ", not at that sketch's origin " + text(st->previewed.origin));
          picker->apply();
          ++st->phase;
          return;
        case 5:
          if (!sketch->active()) return;
          require(same(sketch->frame().origin, st->previewed.origin), "the new sketch on a sketch's plane is not at that sketch's origin");
          pass("New sketch on a sketch's plane starts at that sketch's origin");
          design->cancelSketch();
          timer->stop();
          trace::log("bench: sketch origin PASS");
          QCoreApplication::exit(0);
          return;
      }
    } catch (const std::exception& e) {
      timer->stop();
      trace::log(QString("bench: sketch origin FAIL: %1").arg(QString::fromUtf8(e.what())));
      QCoreApplication::exit(2);
    }
  });
  timer->start();
  return true;
}
