// OPAD_BENCH_ORBITFPS=<prefix> (UI-45): adaptive quality while navigating, in Studio quality. One light casts shadows (the
// headlight's fell out of sight and cost a pass). An orbit drag through the view's mouse handlers (the preset's orbit
// gesture): on a model whose full frame takes kSmoothFrameMs or more the frames while it moves are drawn at 1.0x resolution
// without shadows, faster than the still frame; once the camera has been still the quality is full again. A light model is
// never lowered; with the setting off nothing is. Logs the frames per second still and moving.
// <prefix>.moving.png, <prefix>.still.png.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

#include <Image_PixMap.hxx>
#include <V3d_ImageDumpOptions.hxx>
#include <V3d_Light.hxx>

#include "BenchRegistry.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return done();
}
}  // namespace

bool Viewport::benchOrbitFps(const QString& prefix) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: orbit fps: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  setRenderQuality(1);
  m_needFit = false;
  m_view->SetProj(V3d_XposYnegZpos);
  m_view->FitAll(fitBounds(), 0.02, Standard_False);
  int casting = 0, directional = 0;
  for (V3d_ListOfLightIterator it = m_viewer->ActiveLightIterator(); it.More(); it.Next())
    if (it.Value()->Type() == Graphic3d_TypeOfLightSource_Directional) {
      ++directional;
      casting += it.Value()->ToCastShadows();
    }
  require(casting == 1, QString("one of the %1 directional lights casts shadows (%2 do)").arg(directional).arg(casting));
  // A frame drawn off screen with the parameters in force (a hidden window draws nothing on screen, so the view cannot time
  // its own frames here: the still one is handed to it as fullFrameMs).
  Standard_Integer w = 0, h = 0;
  m_view->Window()->Size(w, h);
  auto frame = [this, w, h] {
    Image_PixMap image;
    V3d_ImageDumpOptions options;
    options.Width = w;
    options.Height = h;
    QElapsedTimer t;
    t.start();
    m_view->ToPixMap(image, options);
    return t.elapsed();
  };
  for (int i = 0; i < 3; ++i) frame();  // shaders, shadow maps
  std::vector<qint64> still;
  for (int i = 0; i < 5; ++i) still.push_back(frame());
  std::sort(still.begin(), still.end());
  const qint64 stillMs = still[still.size() / 2];
  m_fullFrameMs = stillMs;
  const bool heavy = stillMs >= kSmoothFrameMs;
  // An orbit drag: the preset's orbit gesture (Fusion: Shift and the middle button), 30 moves of 6 px, a frame after each.
  const QPointF at(width() / 2.0, height() / 2.0);
  auto drag = [&](bool shoot) {
    auto send = [this](QEvent::Type type, const QPointF& p, Qt::MouseButton button, Qt::MouseButtons buttons) {
      QMouseEvent e(type, p, mapToGlobal(p), button, buttons, Qt::ShiftModifier);
      QCoreApplication::sendEvent(this, &e);
    };
    send(QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::MiddleButton);
    std::vector<qint64> moving;
    int lowered = 0;
    float scale = 0;
    bool shadows = true;
    for (int i = 1; i <= 30; ++i) {
      send(QEvent::MouseMove, at + QPointF(6 * i, 2 * i), Qt::NoButton, Qt::MiddleButton);
      paintEvent(nullptr);  // the controller moves the camera, the view lowers its quality (or not)
      moving.push_back(frame());
      lowered += degraded();
      if (i == 15) {
        scale = m_view->RenderingParams().RenderResolutionScale;
        shadows = m_view->RenderingParams().IsShadowEnabled;
        if (shoot) grabImage().save(prefix + ".moving.png");
      }
    }
    send(QEvent::MouseButtonRelease, at + QPointF(180, 60), Qt::MiddleButton, Qt::NoButton);
    std::sort(moving.begin() + 1, moving.end());  // the first move's frame is the one that finds the model heavy
    return std::make_tuple(moving[1 + (moving.size() - 1) / 2], lowered, scale, shadows);
  };
  const auto [movingMs, lowered, scale, shadows] = drag(true);
  trace::log(QString("bench: orbit fps: still %1 ms a frame (%2 fps), moving %3 ms (%4 fps); a full frame took %5 ms")
                 .arg(stillMs).arg(stillMs > 0 ? 1000.0 / stillMs : 1000.0, 0, 'f', 1).arg(movingMs).arg(movingMs > 0 ? 1000.0 / movingMs : 1000.0, 0, 'f', 1)
                 .arg(fullFrameMs()));
  if (heavy) {
    require(lowered >= 25 && scale == 1.0f && !shadows,
            QString("the moving frames are drawn lower (%1 of 30; resolution %2x, shadows %3)").arg(lowered).arg(scale).arg(shadows ? "on" : "off"));
    require(movingMs < stillMs || movingMs <= kSmoothFrameMs,
            QString("they are faster than the still frame or smooth (%1 ms against %2 ms)").arg(movingMs).arg(stillMs));
  } else {
    require(lowered == 0, QString("a light model is never drawn lower (%1 of 30 moving frames were)").arg(lowered));
  }
  const bool restored = waitUntil([this] { return !degraded(); }, 2000);
  require(restored && m_view->RenderingParams().RenderResolutionScale == 1.25f && m_view->RenderingParams().IsShadowEnabled,
          QString("still, it is drawn at full quality again (resolution %1x, shadows %2)").arg(m_view->RenderingParams().RenderResolutionScale)
              .arg(m_view->RenderingParams().IsShadowEnabled ? "on" : "off"));
  grabImage().save(prefix + ".still.png");
  setAdaptiveQuality(false);
  const auto [offMs, offLowered, offScale, offShadows] = drag(false);
  require(offLowered == 0 && offScale == 1.25f && offShadows, QString("with the setting off nothing is lowered (%1 frames, %2 ms a frame)").arg(offLowered).arg(offMs));
  setAdaptiveQuality(true);
  myUI.Reset();
  return all;
}

OPAD_BENCH(OPAD_BENCH_ORBITFPS, orbitfps) {
  Viewport* v = w.m_viewport;
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto shown = std::make_shared<bool>(false);
  auto* timer = new QTimer(&w);
  QObject::connect(timer, &QTimer::timeout, &w, [&w, v, timer, clock, value, shown] {
    if (!w.m_loadJob && v->displayedCount() == 0 && !std::exchange(*shown, true)) v->isolate(w.m_doc->scene.roots);  // the Engine's root is hidden
    const bool ready = !w.m_loadJob && !w.m_displayJob && w.m_meshRemaining == 0 && v->displayedCount() > 0 && !w.m_jobs->busy();
    if (!ready && clock->elapsed() < 120000) return;
    timer->stop();
    timer->deleteLater();
    trace::log(QString("bench: orbit fps: the model is displayed (%1 bodies) %2").arg(v->displayedCount()).arg(ready ? "PASS" : "FAIL"));
    const bool ok = ready && v->benchOrbitFps(value);
    QCoreApplication::exit(ok ? 0 : 2);
  });
  timer->start(50);
  return true;
}
