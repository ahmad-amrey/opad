// OPAD_BENCH_STYLES=<prefix> (UI-48): the display styles through their commands. Each switch runs as a sliced job, every
// step within 150 ms of UI-thread CPU (the Engine's wireframe froze it for 0.5 s and more: OCCT walked every edge of every
// body on the UI thread); the wireframe is drawn from the mesh worker's arrays (OCCT computes no wireframe of a meshed body)
// and asks for no zoom refinement; in Hidden line the faces take the background's colour and a curved body is outlined where
// it turns away (no edge lies there: the silhouette), where Shaded + edges fills it with its colour. Hidden edges visible
// draws every edge Hidden line draws, solid (a polyline drawn as every other segment came out dashed), outlines the part as
// Hidden line does and adds only dim dashes.
// <prefix>.<style>.png.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>

#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "Jobs.hpp"
#include "MainWindow.hpp"
#include "Viewport.hpp"

namespace {
bool waitUntil(const std::function<bool()>& done, int ms) {
  QElapsedTimer clock;
  clock.start();
  while (!done() && clock.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return done();
}

int distance(QRgb a, QRgb b) { return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b)); }
}  // namespace

bool Viewport::benchStyles(const QString& prefix, const std::function<void(const QString&)>& trigger) {
  bool all = true;
  auto require = [&all](bool ok, const QString& what) {
    trace::log(QString("bench: styles: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  };
  m_needFit = false;
  const bool one = m_items.size() == 1;  // a single part (the cylinder): seen from the front, its pixels are judged
  m_view->SetProj(one ? V3d_Yneg : V3d_XposYnegZpos);
  m_view->FitAll(fitBounds(), 0.1, Standard_False);
  m_view->Redraw();
  auto shot = [this, &prefix](const QString& name) {
    m_view->Redraw();
    QImage image = grabImage();
    image.save(prefix + "." + name + ".png");
    return image.convertToFormat(QImage::Format_RGB32);
  };
  // The middle of the view (the part's face) and the first pixel off the background along the middle row from the left.
  auto judge = [](const QImage& image, int& outline) {
    const QRgb background = image.pixel(10, image.height() - 10);
    const int y = image.height() / 2;
    outline = -1;
    for (int x = 0; x < image.width() / 2 && outline < 0; ++x)
      if (distance(image.pixel(x, y), background) > 60) outline = x;
    return distance(image.pixel(image.width() / 2, y), background);
  };
  if (one) {
    int outline = -1;
    const int filled = judge(shot("start"), outline);
    require(filled > 60 && outline > 0, QString("Shaded + edges fills the part (its middle %1 off the background, its side at x = %2)").arg(filled).arg(outline));
  }
  int hiddenOutline = -1;
  struct Step { QString command, name; Style style; };
  auto settle = [this] { return !stylePending() && !m_jobs->busy() && (m_style != Style::HiddenEdges || edgeOverlayShown()); };
  for (const Step& s : {Step{"view.wire", "wireframe", Style::Wireframe}, Step{"view.hidden", "hidden", Style::HiddenLine},
                        Step{"view.hiddenEdges", "hidden-edges", Style::HiddenEdges}, Step{"view.shaded", "shaded", Style::Shaded},
                        Step{"view.edges", "edges", Style::ShadedEdges}}) {
    waitUntil([this] { return !m_jobs->busy(); }, 30000);
    waitUntil([] { return false; }, 100);  // the watchdog reports the last frame dump's stall before the count starts
    const int stock = BodyShape::stockWireframes();
    trace::resetStalls();
    const qint64 cpu = trace::threadCpuMs();
    QElapsedTimer t;
    t.start();
    trigger(s.command);
    const qint64 call = t.elapsed(), callCpu = trace::threadCpuMs() - cpu;
    const bool settled = waitUntil(settle, 60000);
    const trace::Stalls stalls = trace::stalls();
    require(settled && m_style == s.style && std::max(callCpu, stalls.longestCpu) < 150 && std::max(call, stalls.longest) < 600,
            QString("%1: %2 ms in the command (%3 ms CPU), longest stall %4 ms (%5 ms CPU), applied to %6 bodies after %7 ms").arg(s.name).arg(call)
                .arg(callCpu).arg(stalls.longest).arg(stalls.longestCpu).arg(m_items.size()).arg(t.elapsed()));
    const QImage image = shot(s.name);
    if (s.style == Style::Wireframe) {
      require(BodyShape::stockWireframes() == stock, QString("the wireframe is the worker's (%1 computed by OCCT)").arg(BodyShape::stockWireframes() - stock));
      refineVisible();  // as the camera coming to rest would
      require(!m_refineJob, "no zoom refinement in the wireframe");
    }
    if (s.style == Style::HiddenLine && one) {
      const int middle = judge(image, hiddenOutline);
      require(middle < 30 && hiddenOutline > 0,
              QString("hidden line: the face takes the background's colour (%1 off it) and the part is outlined where it turns away (x = %2)").arg(middle).arg(hiddenOutline));
    }
    if (s.style == Style::HiddenEdges && one) {
      int outline = -1;
      const int middle = judge(image, outline);
      require(middle < 30 && outline > 0 && std::abs(outline - hiddenOutline) <= 2,
              QString("hidden edges visible: outlined as in Hidden line (x = %1 against %2)").arg(outline).arg(hiddenOutline));
    }
  }
  // Hidden edges visible against Hidden line, seen from an iso view: the edges behind the faces show, dashed and dim (the
  // back of the cylinder's bottom rim), and only those differ.
  if (one) {
    m_view->SetProj(V3d_XposYnegZpos);
    m_view->FitAll(fitBounds(), 0.1, Standard_False);
    trigger("view.hidden");
    waitUntil(settle, 60000);
    const QImage hidden = shot("hidden-iso");
    trigger("view.hiddenEdges");
    waitUntil(settle, 60000);
    const QImage dashed = shot("hidden-edges-iso");
    int added = 0, brighter = 0, seen = 0, kept = 0;
    const QRgb background = hidden.pixel(10, hidden.height() - 10);
    auto bright = [background](const QImage& image, int x, int y) {  // as bright as a seen edge: not a dim dash
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
          if (image.rect().contains(x + dx, y + dy) && distance(image.pixel(x + dx, y + dy), background) > 300) return true;
      return false;
    };
    for (int y = 0; y < hidden.height(); ++y)
      for (int x = 0; x < hidden.width(); ++x) {
        if (distance(hidden.pixel(x, y), background) > 300) {
          ++seen;
          kept += bright(dashed, x, y);
        }
        if (distance(hidden.pixel(x, y), dashed.pixel(x, y)) > 30) {
          ++added;
          brighter += distance(dashed.pixel(x, y), background) > 300 && !bright(hidden, x, y);
        }
      }
    require(seen > 500 && kept >= seen * 97 / 100,
            QString("hidden edges visible: every edge in sight is drawn solid as in Hidden line (%1 of %2 pixels)").arg(kept).arg(seen));
    require(edgeOverlayShown() && added > 50 && brighter < added / 10,
            QString("hidden edges visible: the edges behind show, dim (%1 pixels more than in Hidden line, %2 of them bright)").arg(added).arg(brighter));
    trigger("view.edges");
    waitUntil(settle, 60000);
    require(!edgeOverlayShown(), "leaving the style removes its edges");
  }
  return all;
}

OPAD_BENCH(OPAD_BENCH_STYLES, styles) {
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
    trace::log(QString("bench: styles: the model is displayed (%1 bodies) %2").arg(v->displayedCount()).arg(ready ? "PASS" : "FAIL"));
    const bool ok = ready && v->benchStyles(value, [&w](const QString& id) { w.action(id)->trigger(); });
    QCoreApplication::exit(ok ? 0 : 2);
  });
  timer->start(50);
  return true;
}
