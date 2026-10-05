// OPAD_BENCH_OVERLAY_TRAIL=1 on an empty document: a native overlay over the OCCT view leaves no trail (ViewOverlay.hpp).
// The value boxes beside the pointer are a native child window of the view, masked to the boxes; where they were is the
// view's own window again, whose pixels only OCCT draws, and OCCT draws only on a frame of its own: a move that changed
// nothing in the scene drew none, and the boxes left copies of themselves along the pointer's way. A hidden bench window
// never shows its native view on screen, so the trail itself cannot be seen here: the bench checks what removes it, that
// every paint after an overlay left part of the view shows that part again (a frame of its own, or the last frame shown
// again without drawing the scene, where whole frames are slow), and that a paint with nothing left shows nothing again.
// In 3D (Review, as a feature panel's, the section's or the drawing placer's boxes): the boxes moved five times beside the
// pointer, then hidden; any native overlay (a toast, a chip, a badge, the sketch's value box) moved, resized and hidden; an
// expose of the view's window. In a sketch (Sketch1 on XY): the line tool's X and Y boxes followed the pointer through six
// moves of a few pixels before its first point, then the tool closed.
// Case in tools/bench_cases/sketch.py.
#include <QAction>
#include <QApplication>
#include <QExposeEvent>
#include <QFrame>
#include <QTimer>
#include <QWindow>

#include <functional>

#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "DynamicInput.hpp"
#include "MainWindow.hpp"
#include "SketchEditor.hpp"
#include "Viewport.hpp"

namespace {
bool g_overlayTrail3d = true;  // the 3D part's outcome, for the sketch part's exit code
}

OPAD_BENCH(OPAD_BENCH_OVERLAY_TRAIL, overlayTrail) {
  auto require = [](bool ok, const QString& what) {
    trace::log(QString("bench: overlay trail: 3D: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    g_overlayTrail3d = g_overlayTrail3d && ok;
    return ok;
  };
  Viewport* v = w.m_viewport;
  QApplication::setActiveWindow(&w);
  v->benchPaint();
  // A paint after `change` showed what it left again (one repair); `what` says what changed.
  auto repaired = [v](const std::function<void()>& change) {
    const int before = v->overlayRepairs();
    change();
    v->benchPaint();
    return v->overlayRepairs() == before + 1;
  };
  {
    DynamicInput boxes(v);  // as ToolValues makes them for a feature panel, the section or the drawing placer
    DynamicInput::Field x, y;
    x.key = "x";
    x.label = "X";
    x.live = "10 mm";
    y.key = "y";
    y.label = "Y";
    y.live = "20 mm";
    boxes.setFields({x, y});
    boxes.placeNear(QPoint(200, 200));
    boxes.show();
    v->benchPaint();
    require(boxes.testAttribute(Qt::WA_NativeWindow) && boxes.parentWidget() == v && !boxes.isWindow() && !boxes.mask().isEmpty(),
            "the value boxes are a native child window of the view, masked to the boxes (what they leave is the view's)");
    int moved = 0, shown = 0;
    const int reshows = v->overlayReshows();
    for (int i = 1; i <= 5; ++i) {
      const QRect before = boxes.boxesRect();
      shown += repaired([&] { boxes.placeNear(QPoint(200 + 7 * i, 200 + 3 * i)); });
      moved += boxes.boxesRect() != before;
    }
    require(moved == 5 && shown == 5, QString("the boxes followed the pointer five times (%1 moved) and each paint after showed the part they left again (%2; %3 "
                                              "by the last frame shown again, no frame of its own)").arg(moved).arg(shown).arg(v->overlayReshows() - reshows));
    // A model whose whole frames are slow (the Engine): the part they left is shown again from the last frame, the scene
    // not drawn again.
    const qint64 frameMs = v->fullFrameMs();
    v->benchFullFrameMs(Viewport::kSmoothFrameMs * 3);
    const int slowReshows = v->overlayReshows();
    const bool slowShown = repaired([&] { boxes.placeNear(QPoint(260, 240)); });
    require(slowShown && v->overlayReshows() == slowReshows + 1 && !v->frameInvalidated(),
            "with slow frames, a move of the boxes shows the last frame again, no scene redrawn");
    v->benchFullFrameMs(frameMs);
    require(repaired([&] { boxes.hide(); }), "the boxes hidden: the paint after showed where they were again");
  }
  {
    QFrame card(v);  // any native overlay over the view: a toast, a chip, a badge, the sketch's value box, a prompt bar
    card.setAttribute(Qt::WA_NativeWindow);
    card.setAutoFillBackground(true);
    card.setGeometry(40, 40, 120, 30);
    card.show();
    v->benchPaint();
    require(repaired([&] { card.move(70, 52); }), "a native overlay moved: the paint after showed the part it left again");
    require(repaired([&] { card.resize(90, 30); }), "resized smaller: the same");
    require(repaired([&] { card.hide(); }), "hidden: the same");
  }
  require(repaired([&] {
            QExposeEvent expose(QRegion(10, 10, 60, 40));  // the system uncovered part of the view's own window
            if (QWindow* window = v->windowHandle()) QCoreApplication::sendEvent(window, &expose);
          }),
          "an expose of the view's window: the paint after showed that part again");
  {
    const int repairs = v->overlayRepairs(), reshows = v->overlayReshows();
    v->benchPaint();
    require(v->overlayRepairs() == repairs && v->overlayReshows() == reshows, "a paint with nothing left by an overlay shows nothing again (no extra frame per move)");
  }
  // The sketch: the line tool's boxes beside the pointer.
  w.setWorkspace("design");
  w.m_design->benchSketch([&w] { w.m_design->sketch()->benchTrail(); });
  QTimer::singleShot(150000, qApp, [] { trace::log("bench: overlay trail: did not end FAIL"); QCoreApplication::exit(2); });
  return true;
}

void SketchEditor::benchTrail() {
  bool ok = g_overlayTrail3d;
  auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: overlay trail: sketch: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  auto* f9 = m_viewport->window()->findChild<QAction*>("view.gridSnap");
  if (!f9) {
    check(false, "the F9 action exists");
    return QCoreApplication::exit(2);
  }
  // 0.11 mm a pixel, grid snapping off: the boxes follow the pointer itself.
  auto camera = [&](double scale) {
    m_viewport->setCameraJson({{"eye", {0, 0, 100}}, {"target", {0, 0, 0}}, {"up", {0, 1, 0}}, {"scale", scale}, {"projection", "orthographic"}, {"absolute", true}});
  };
  camera(100);
  camera(100 * 0.11 / m_viewport->pixelSize());
  m_viewport->grabImage();
  const double s = m_viewport->gridStep(), px = m_viewport->pixelSize();
  f9->setChecked(false);
  setTool("line");
  auto move = [&](double u, double v) { sketchMove(u, v, Qt::NoModifier, false); };
  const double u0 = 9.3 * s, v0 = 6.2 * s;  // clear of the origin and its axes: nothing to snap to
  move(u0, v0);
  m_viewport->benchPaint();
  check(m_input->isVisible() && m_input->count() == 2, QString("the line tool's X and Y boxes beside the pointer, before its first point (%1 mm a pixel)").arg(px));
  check(m_input->testAttribute(Qt::WA_NativeWindow) && m_input->parentWidget() == m_viewport && !m_input->isWindow(),
        "the boxes are a native child window of the view, masked to the boxes");
  const size_t redisplays = m_transientRedisplays;
  const int reshows = m_viewport->overlayReshows();
  int moved = 0, shown = 0;
  for (int i = 1; i <= 6; ++i) {
    const QRect before = m_input->boxesRect();
    const int repairs = m_viewport->overlayRepairs();
    move(u0 + 3 * i * px, v0 - 2 * i * px);
    m_viewport->benchPaint();
    moved += m_input->boxesRect() != before;
    shown += m_viewport->overlayRepairs() == repairs + 1;
  }
  trace::log(QString("bench: overlay trail: sketch: the rubber band's overlay redisplayed %1 times in the six moves").arg(m_transientRedisplays - redisplays));
  check(moved == 6 && shown == 6, QString("the boxes followed the pointer (%1 of 6 moved) and each paint after showed the part they left again (%2 of 6; %3 by the "
                                          "last frame shown again, no frame of its own)").arg(moved).arg(shown).arg(m_viewport->overlayReshows() - reshows));
  const int repairs = m_viewport->overlayRepairs();
  setTool("select");
  m_viewport->benchPaint();
  check(!m_input->isVisible() && m_viewport->overlayRepairs() == repairs + 1, "the tool closed: the boxes went and the paint after showed where they were again");
  QCoreApplication::exit(ok ? 0 : 2);
}
