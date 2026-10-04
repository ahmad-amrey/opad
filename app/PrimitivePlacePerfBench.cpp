#include "BenchRegistry.hpp"
#include "DesignController.hpp"
#include "MainWindow.hpp"
#include "PrimitivePlacer.hpp"

#include <BRepAdaptor_Surface.hxx>

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>

#include <algorithm>

// OPAD_BENCH_PRIMITIVES_PERF=1, meant for the Engine (TODO 11 P1; nothing model-scaled on the UI thread): once its bodies
// are on screen (unhidden in memory, never saved), a cylinder is placed through the view's mouse events and each event's
// handling is timed: the pointer swept over the model while the plane is picked (a pick under it and the hovered face's
// snap points), a click on a planar face of the model (its frame is resolved on a worker), the base sized by ten moves
// (each a preview planned on a worker), the click that fixes it; then Esc three times. PASS when no event took 100 ms, nor
// did the events each sizing move left for the event loop (a panel refit, a preview arriving), starting the cylinder took no
// more than 30 ms beyond starting a pipe (a panel and a guide of its own, nothing placed), and what its start left for the
// event loop (the document and scene copies its face clicks and previews are planned on, made once the panel is up; any
// feature's first preview makes them) stayed under the watchdog's 250 ms stall.
OPAD_BENCH(OPAD_BENCH_PRIMITIVES_PERF, primitives_perf) {
  struct State {
    int phase = 0, ticks = 0, settled = 0, remaining = -1, moves = 0;
    qint64 worst = 0, copies = 0;
    QString worstWhat;
    QPointF face;
    bool found = false;
  };
  auto st = std::make_shared<State>();
  QObject::connect(w.m_viewport, &Viewport::meshingProgress, &w, [st](int remaining) {
    st->remaining = remaining;
    st->settled = 0;
  });
  Viewport* view = w.m_viewport;
  DesignController* design = w.m_design;
  PrimitivePlacer* placer = design->placer();
  // One event through the viewport's filters and handlers, timed (the frame it asks for comes later, as it does for a user).
  auto timed = [st, view](const QString& what, QEvent::Type type, const QPointF& at, Qt::MouseButtons held = Qt::NoButton) {
    const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const Qt::MouseButtons buttons = type == QEvent::MouseMove ? held : type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, at, view->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QElapsedTimer t;
    t.start();
    QApplication::sendEvent(view, &e);
    const qint64 ms = t.elapsed();
    if (ms > st->worst) st->worst = ms, st->worstWhat = what;
    return ms;
  };
  auto* timer = new QTimer(&w);
  timer->setInterval(200);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, st, timer, view, design, placer, timed] {
    auto finish = [timer](bool ok, const QString& line) {
      timer->stop();
      trace::log("bench: primitives perf: " + line + (ok ? " PASS" : " FAIL"));
      QCoreApplication::exit(ok ? 0 : 2);
    };
    if (++st->ticks > 3000) return finish(false, QStringLiteral("timed out in phase %1").arg(st->phase));
    AppDocument* doc = w.m_doc;
    if (doc->loading || doc->designBusy || w.m_jobs->busy() || view->cameraMoving()) return;
    if (st->phase == 0) {  // the bodies on screen
      const auto bodies = doc->scene.all_bodies();
      if (std::none_of(bodies.begin(), bodies.end(), [doc](const std::string& id) { return doc->scene.effectively_visible(id); })) {
        trace::log("bench: primitives perf: nothing visible, unhiding (in memory, never saved)");
        std::vector<std::string> hidden;
        for (const auto& [id, node] : doc->scene.nodes)
          if (!node.visible) hidden.push_back(id);
        for (const auto& id : hidden) doc->run("appearance", {{"target", id}, {"visible", true}});
        st->remaining = -1;
        return;
      }
      if (st->remaining > 0 || ++st->settled < 6) return;
      w.setWorkspace("design");
      view->standardView("iso");
      view->fitAll();
      st->phase = 1;
      return;
    }
    if (st->phase == 1) {
      QElapsedTimer t;
      t.start();
      view->grabImage();  // a frame: the picker's depth range
      const qint64 frame = t.restart();
      design->startFeature("pipe");  // for comparison: a feature that is not placed (its panel, its guide)
      const qint64 pipe = t.restart();
      design->escape();
      t.restart();
      design->startFeature("cylinder");
      const qint64 started = t.restart();
      if (started > pipe + 30) st->worst = std::max(st->worst, started - pipe), st->worstWhat = "starting the cylinder, beyond a pipe's start";
      QCoreApplication::processEvents();  // the plan copies (a timer of 0 ms after the start), on the UI thread
      st->copies = t.restart();
      trace::log(QStringLiteral("bench: primitives perf: a frame of the Engine took %1 ms (the bench's); starting a pipe %2 ms, the cylinder %3 ms, the events after it "
                                "(the plan copies) %4 ms").arg(frame).arg(pipe).arg(started).arg(st->copies));
      st->phase = 2;
      return;
    }
    if (st->phase == 2) {  // the plane being picked: the pointer swept over the view, a planar face of the model found
      if (placer->stage() != PrimitivePlacer::Stage::Place) return;
      qint64 sweep = 0;
      int n = 0;
      for (int j = 1; j < 10; ++j)
        for (int i = 1; i < 14; ++i) {
          const QPointF at(view->width() * i / 14.0, view->height() * j / 10.0);
          // What a frame's hover detects there first (a hidden window draws none), then the move the placer handles.
          std::string candidate;
          TopoDS_Face face;
          opad::Vec3 hit;
          const bool planar = view->surfaceAt(at, candidate, face, hit) && !face.IsNull() && BRepAdaptor_Surface(face).GetType() == GeomAbs_Plane;
          sweep = std::max(sweep, timed("a hover while placing", QEvent::MouseMove, at));
          ++n;
          if (!st->found && planar) st->face = at, st->found = true;
        }
      trace::log(QStringLiteral("bench: primitives perf: %1 hovers while placing, the slowest %2 ms").arg(n).arg(sweep));
      if (!st->found) return finish(false, "no planar face of the model under the swept pointer");
      timed("the move onto the face", QEvent::MouseMove, st->face);
      const qint64 press = timed("the click on the face", QEvent::MouseButtonPress, st->face);
      timed("its release", QEvent::MouseButtonRelease, st->face);
      trace::log(QStringLiteral("bench: primitives perf: the click on a planar face took %1 ms (its frame resolved on a worker)").arg(press));
      st->phase = 3;
      return;
    }
    if (st->phase == 3) {  // sizing: ten moves, a preview planned between each
      if (placer->stage() != PrimitivePlacer::Stage::Size) return;
      if (st->moves < 10) {
        ++st->moves;
        const QPointF at = st->face + QPointF(6.0 * st->moves, 3.0 * st->moves);
        QElapsedTimer pick;
        pick.start();
        std::string candidate;
        TopoDS_Face face;
        opad::Vec3 hit;
        view->surfaceAt(at, candidate, face, hit);  // the frame's hover, as above (not the placer's cost)
        const qint64 picked = pick.elapsed();
        const qint64 ms = timed("a move while sizing", QEvent::MouseMove, at);
        QElapsedTimer after;  // what the move left for the event loop (the panel's refit, a preview arriving): held to the same limit
        after.start();
        QCoreApplication::processEvents();
        const qint64 left = after.elapsed();
        if (left > st->worst) st->worst = left, st->worstWhat = "the events after a sizing move";
        trace::log(QStringLiteral("bench: primitives perf: sizing move %1: %2 ms, the events after it %5 ms (the frame's pick there %4 ms), diameter %3")
                       .arg(st->moves).arg(ms).arg(design->featurePanel()->valueText("diameter")).arg(picked).arg(left));
        return;
      }
      const QPointF at = st->face + QPointF(60, 30);
      const qint64 click = timed("the click fixing the base", QEvent::MouseButtonPress, at);
      timed("its release", QEvent::MouseButtonRelease, at);
      trace::log(QStringLiteral("bench: primitives perf: the click fixing the base took %1 ms").arg(click));
      st->phase = 4;
      return;
    }
    if (st->phase == 4) {
      if (placer->stage() != PrimitivePlacer::Stage::Height || design->handleInput() != "height") return;
      for (int i = 0; i < 3; ++i) design->escape();
      st->phase = 5;
      return;
    }
    if (design->featureActive()) return;
    // Under 100 ms: a third of what the watchdog calls a stall, with room for a machine running other benches (alone the Engine
    // gives about 30 ms at most, the pick under the pointer most of it).
    finish(st->worst < 100 && st->copies < 250, QStringLiteral("the slowest event (%1) took %2 ms; the plan copies after the start %3 ms (under 250)")
                                                   .arg(st->worstWhat).arg(st->worst).arg(st->copies));
  });
  timer->start();
  return true;
}
